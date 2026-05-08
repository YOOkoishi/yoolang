#include "../../include/pass/YIRToOIRLowerer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pass::yir_to_oir {
namespace {

std::string sanitize_name(const std::string &name) {
    std::string out;
    out.reserve(name.size());
    for (char ch : name) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '_' || ch == '.') {
            out.push_back(ch);
        } else if (ch != '%' && ch != '@') {
            out.push_back('_');
        }
    }
    return out.empty() ? "v" : out;
}

bool is_scalar_type(const yir::TypePtr &type) {
    return type != nullptr &&
           (type->kind() == yir::Type::Kind::I1 || type->kind() == yir::Type::Kind::I32 ||
            type->kind() == yir::Type::Kind::F32);
}

yir::TypePtr scalar_element_type(yir::TypePtr type) {
    while (type != nullptr && type->is_array()) {
        type = type->element();
    }
    return type;
}

class AssignmentCollector final {
  public:
    std::unordered_set<const yir::Value *> collect(const yir::Region &region) {
        assigned_.clear();
        scan_region(region);
        return assigned_;
    }

  private:
    void scan_region(const yir::Region &region) {
        for (const auto &op : region.operations()) {
            scan_op(*op);
        }
    }

    void scan_op(const yir::Operation &op) {
        if (const auto *assign = dynamic_cast<const yir::AssignOp *>(&op)) {
            assigned_.insert(assign->target());
            return;
        }
        if (const auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
            scan_region(if_op->then_region());
            if (if_op->has_else()) {
                scan_region(if_op->else_region());
            }
            return;
        }
        if (const auto *while_op = dynamic_cast<const yir::WhileOp *>(&op)) {
            scan_region(while_op->cond_region());
            scan_region(while_op->body_region());
            return;
        }
        if (const auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
            assigned_.insert(for_op->induction_var());
            scan_region(for_op->body_region());
            return;
        }
    }

    std::unordered_set<const yir::Value *> assigned_;
};

class Lowerer final {
    using ValueMap = std::unordered_map<const yir::Value *, oir::Value *>;
    using ValueSet = std::unordered_set<const yir::Value *>;

    struct Edge {
        oir::BasicBlock *block = nullptr;
        ValueMap values;
    };

    struct LoopContext {
        oir::BasicBlock *continue_target = nullptr;
        oir::BasicBlock *break_target = nullptr;
        std::vector<Edge> continue_edges;
        std::vector<Edge> break_edges;
    };

  public:
    std::unique_ptr<oir::Module> lower(const yir::Module &module) {
        module_ = std::make_unique<oir::Module>("yoolang.oir");
        builder_ = std::make_unique<oir::IRBuilder>(module_.get());

        lower_globals(module);
        declare_functions(module);

        for (const auto &function : module.functions()) {
            lower_function(*function);
        }

        return std::move(module_);
    }

  private:
    oir::Type *lower_type(const yir::TypePtr &type) {
        if (type == nullptr) {
            return types().void_ty();
        }

        auto key = type->str();
        auto found = type_cache_.find(key);
        if (found != type_cache_.end()) {
            return found->second;
        }

        oir::Type *out = nullptr;
        switch (type->kind()) {
        case yir::Type::Kind::I1:
            out = types().int1_ty();
            break;
        case yir::Type::Kind::I32:
            out = types().int32_ty();
            break;
        case yir::Type::Kind::F32:
            out = types().float_ty();
            break;
        case yir::Type::Kind::Void:
            out = types().void_ty();
            break;
        case yir::Type::Kind::Ptr:
            out = types().ptr_ty(lower_type(type->pointee()));
            break;
        case yir::Type::Kind::Array:
            out = types().array_ty(lower_type(type->element()),
                                   static_cast<std::size_t>(type->count()));
            break;
        case yir::Type::Kind::Func: {
            std::vector<oir::Type *> params;
            params.reserve(type->params().size());
            for (const auto &param : type->params()) {
                params.push_back(lower_type(param));
            }
            out = types().func_ty(lower_type(type->result()), params);
            break;
        }
        }

        type_cache_[std::move(key)] = out;
        return out;
    }

    oir::TypeContext &types() {
        return module_->types();
    }

    void lower_globals(const yir::Module &module) {
        for (const auto &global : module.globals()) {
            auto *value_type = lower_type(global->storage_type());
            auto *out = module_->create_global(global->name(), value_type, global->is_const());
            out->set_initializer_literal(global->initializer().empty() ? "zero"
                                                                       : global->initializer());
            memory_addresses_[global->address()] = out;
        }
    }

    void declare_functions(const yir::Module &module) {
        for (const auto &function : module.functions()) {
            std::vector<oir::Type *> params;
            params.reserve(function->param_types().size());
            for (const auto &param : function->param_types()) {
                params.push_back(lower_type(param));
            }
            auto *type = types().func_ty(lower_type(function->return_type()), params);
            auto *out = module_->create_function(function->name(), type);
            functions_[function.get()] = out;
        }
    }

    void lower_function(const yir::Function &function) {
        auto *out = functions_.at(&function);
        current_function_ = out;
        value_map_.clear();
        ssa_vars_.clear();
        name_counts_.clear();
        used_names_.clear();

        auto *entry = out->create_block("entry");
        builder_->set_insert_point(entry);

        for (std::size_t i = 0; i < function.params().size(); ++i) {
            auto *arg = out->args()[i].get();
            arg->set_name(unique_name(function.params()[i]->name().empty()
                                          ? "arg" + std::to_string(i)
                                          : function.params()[i]->name()));
            value_map_[function.params()[i].get()] = arg;
        }

        lower_region(function.body());
        emit_default_return_if_needed();

        current_function_ = nullptr;
        builder_->clear_insert_point();
    }

    void lower_region(const yir::Region &region) {
        for (const auto &op : region.operations()) {
            if (!can_insert()) {
                return;
            }
            lower_op(*op);
        }
    }

    void lower_op(const yir::Operation &op) {
        if (const auto *const_i32 = dynamic_cast<const yir::ConstI32Op *>(&op)) {
            bind_result(op, module_->create_i32(const_i32->value()));
            return;
        }
        if (const auto *const_f32 = dynamic_cast<const yir::ConstF32Op *>(&op)) {
            bind_result(op, module_->create_f32(const_f32->value()));
            return;
        }
        if (const auto *const_bool = dynamic_cast<const yir::ConstBoolOp *>(&op)) {
            bind_result(op, module_->create_i1(const_bool->value()));
            return;
        }
        if (dynamic_cast<const yir::ZeroOp *>(&op) != nullptr) {
            bind_result(op, zero_value(op.result()->type()));
            return;
        }
        if (const auto *var = dynamic_cast<const yir::VarOp *>(&op)) {
            lower_var(*var);
            return;
        }
        if (const auto *assign = dynamic_cast<const yir::AssignOp *>(&op)) {
            lower_assign(*assign);
            return;
        }
        if (dynamic_cast<const yir::ArrayVarOp *>(&op) != nullptr) {
            lower_array_var(op);
            return;
        }
        if (const auto *array_init = dynamic_cast<const yir::ArrayInitOp *>(&op)) {
            lower_array_init(*array_init);
            return;
        }
        if (const auto *array_load = dynamic_cast<const yir::ArrayLoadOp *>(&op)) {
            lower_array_load(*array_load);
            return;
        }
        if (const auto *array_store = dynamic_cast<const yir::ArrayStoreOp *>(&op)) {
            lower_array_store(*array_store);
            return;
        }
        if (const auto *alloca_op = dynamic_cast<const yir::AllocaOp *>(&op)) {
            auto *alloca = builder_->create_alloca(lower_type(alloca_op->storage_type()),
                                                   result_name(op.result(), "addr"));
            bind_result(op, alloca);
            return;
        }
        if (const auto *load = dynamic_cast<const yir::LoadOp *>(&op)) {
            auto *loaded =
                builder_->create_load(address_for(load->address()), lower_type(op.result()->type()),
                                      result_name(op.result(), "load"));
            bind_result(op, loaded);
            return;
        }
        if (const auto *store = dynamic_cast<const yir::StoreOp *>(&op)) {
            builder_->create_store(value_for(store->value()), address_for(store->address()));
            return;
        }
        if (const auto *elem_addr = dynamic_cast<const yir::ElemAddrOp *>(&op)) {
            std::vector<oir::Value *> indices;
            for (auto *index : elem_addr->indices()) {
                indices.push_back(value_for(index));
            }
            auto *gep =
                create_element_ptr(elem_addr->base(), indices, lower_type(op.result()->type()),
                                   result_name(op.result(), "addr"));
            bind_result(op, gep);
            return;
        }
        if (const auto *decay = dynamic_cast<const yir::DecayOp *>(&op)) {
            lower_decay(*decay);
            return;
        }
        if (const auto *icmp = dynamic_cast<const yir::ICmpOp *>(&op)) {
            lower_icmp(*icmp);
            return;
        }
        if (const auto *fcmp = dynamic_cast<const yir::FCmpOp *>(&op)) {
            lower_fcmp(*fcmp);
            return;
        }
        if (const auto *binary = dynamic_cast<const yir::BinaryOpBase *>(&op)) {
            lower_binary(op, *binary);
            return;
        }
        if (dynamic_cast<const yir::ZExtI1ToI32Op *>(&op) != nullptr) {
            bind_result(op, builder_->create_zext(value_for(op.operands()[0]), types().int32_ty(),
                                                  result_name(op.result(), "zext")));
            return;
        }
        if (dynamic_cast<const yir::TruncI32ToI1Op *>(&op) != nullptr ||
            dynamic_cast<const yir::ToBoolOp *>(&op) != nullptr) {
            bind_result(op, to_bool(value_for(op.operands()[0]), result_name(op.result(), "bool")));
            return;
        }
        if (dynamic_cast<const yir::SIToFPOp *>(&op) != nullptr) {
            bind_result(op, builder_->create_sitofp(value_for(op.operands()[0]), types().float_ty(),
                                                    result_name(op.result(), "sitofp")));
            return;
        }
        if (dynamic_cast<const yir::FPToSIOp *>(&op) != nullptr) {
            bind_result(op, builder_->create_fptosi(value_for(op.operands()[0]), types().int32_ty(),
                                                    result_name(op.result(), "fptosi")));
            return;
        }
        if (dynamic_cast<const yir::NotOp *>(&op) != nullptr) {
            bind_result(op,
                        logical_not(value_for(op.operands()[0]), result_name(op.result(), "not")));
            return;
        }
        if (const auto *call = dynamic_cast<const yir::CallOp *>(&op)) {
            lower_call(*call);
            return;
        }
        if (const auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
            lower_if(*if_op);
            return;
        }
        if (const auto *while_op = dynamic_cast<const yir::WhileOp *>(&op)) {
            lower_while(*while_op);
            return;
        }
        if (const auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
            lower_for(*for_op);
            return;
        }
        if (dynamic_cast<const yir::BreakOp *>(&op) != nullptr) {
            lower_break();
            return;
        }
        if (dynamic_cast<const yir::ContinueOp *>(&op) != nullptr) {
            lower_continue();
            return;
        }
        if (const auto *ret = dynamic_cast<const yir::ReturnOp *>(&op)) {
            builder_->create_ret(ret->has_value() ? value_for(ret->value()) : nullptr);
            return;
        }
        if (dynamic_cast<const yir::CondOp *>(&op) != nullptr) {
            throw std::runtime_error("yir.cond is only valid inside yir.while condition regions");
        }
    }

    void lower_var(const yir::VarOp &op) {
        auto *result = op.result();
        if (!is_scalar_type(result->type())) {
            throw std::runtime_error("non-scalar yir.var should be represented by yir.array_var");
        }

        oir::Value *initial =
            op.has_initializer() ? value_for(op.initializer()) : zero_value(result->type());
        ssa_vars_.insert(result);
        value_map_[result] = initial;
    }

    void lower_assign(const yir::AssignOp &op) {
        auto *target = op.target();
        auto *value = value_for(op.value());
        if (ssa_vars_.find(target) != ssa_vars_.end()) {
            value_map_[target] = value;
            return;
        }
        auto found = memory_addresses_.find(target);
        if (found != memory_addresses_.end()) {
            builder_->create_store(value, found->second);
            return;
        }
        throw std::runtime_error("assignment target is neither an SSA variable nor memory");
    }

    void lower_array_var(const yir::Operation &op) {
        auto *array_type = lower_type(op.result()->type());
        auto *alloca = builder_->create_alloca(array_type, result_name(op.result(), "array"));
        memory_addresses_[op.result()] = alloca;
        value_map_[op.result()] = alloca;
    }

    void lower_array_init(const yir::ArrayInitOp &op) {
        auto *element_type = lower_type(scalar_element_type(op.array_type()));

        if (op.default_zero()) {
            auto *array_type = lower_type(op.array_type());
            builder_->create_store(module_->create_zero(array_type), address_for(op.array()));
        }

        for (const auto &entry : op.entries()) {
            auto *ptr = create_element_ptr(op.array(), constant_indices(entry.indices),
                                           types().ptr_ty(element_type), unique_name("init.addr"));
            oir::Value *value =
                entry.value == nullptr
                    ? literal_value(entry.literal, scalar_element_type(op.array_type()))
                    : value_for(entry.value);
            builder_->create_store(value, ptr);
        }
    }

    void lower_array_load(const yir::ArrayLoadOp &op) {
        std::vector<oir::Value *> indices;
        for (auto *index : op.indices()) {
            indices.push_back(value_for(index));
        }
        auto *element_type = lower_type(op.result()->type());
        auto *ptr = create_element_ptr(op.array(), indices, types().ptr_ty(element_type),
                                       unique_name(var_name(op.result()) + ".addr"));
        auto *load = builder_->create_load(ptr, element_type, result_name(op.result(), "load"));
        bind_result(op, load);
    }

    void lower_array_store(const yir::ArrayStoreOp &op) {
        auto *value = value_for(op.value());
        std::vector<oir::Value *> indices;
        for (auto *index : op.indices()) {
            indices.push_back(value_for(index));
        }
        auto *ptr = create_element_ptr(op.array(), indices, types().ptr_ty(value->type()),
                                       unique_name("elem.addr"));
        builder_->create_store(value, ptr);
    }

    void lower_decay(const yir::DecayOp &op) {
        auto *source = address_for(op.array_address());
        auto *result_type = lower_type(op.result()->type());
        if (source->type() == result_type) {
            bind_result(op, source);
            return;
        }
        std::vector<oir::Value *> indices;
        indices.push_back(module_->create_i32(0));
        indices.push_back(module_->create_i32(0));
        auto *gep =
            builder_->create_gep(source, result_type, indices, result_name(op.result(), "decay"));
        bind_result(op, gep);
    }

    void lower_binary(const yir::Operation &op, const yir::BinaryOpBase &binary) {
        oir::Instruction::OpID id;
        if (op.op_name() == "yir.addi") {
            id = oir::Instruction::OpID::Add;
        } else if (op.op_name() == "yir.subi") {
            id = oir::Instruction::OpID::Sub;
        } else if (op.op_name() == "yir.muli") {
            id = oir::Instruction::OpID::Mul;
        } else if (op.op_name() == "yir.divsi") {
            id = oir::Instruction::OpID::SDiv;
        } else if (op.op_name() == "yir.remsi") {
            id = oir::Instruction::OpID::SRem;
        } else if (op.op_name() == "yir.addf") {
            id = oir::Instruction::OpID::FAdd;
        } else if (op.op_name() == "yir.subf") {
            id = oir::Instruction::OpID::FSub;
        } else if (op.op_name() == "yir.mulf") {
            id = oir::Instruction::OpID::FMul;
        } else if (op.op_name() == "yir.divf") {
            id = oir::Instruction::OpID::FDiv;
        } else {
            throw std::runtime_error("unknown YIR binary op: " + op.op_name());
        }
        bind_result(op,
                    builder_->create_binary(id, value_for(binary.lhs()), value_for(binary.rhs()),
                                            result_name(op.result(), "bin")));
    }

    void lower_icmp(const yir::ICmpOp &op) {
        bind_result(op,
                    builder_->create_icmp(lower_pred(op.predicate()), value_for(op.lhs()),
                                          value_for(op.rhs()), result_name(op.result(), "icmp")));
    }

    void lower_fcmp(const yir::FCmpOp &op) {
        bind_result(op,
                    builder_->create_fcmp(lower_pred(op.predicate()), value_for(op.lhs()),
                                          value_for(op.rhs()), result_name(op.result(), "fcmp")));
    }

    void lower_call(const yir::CallOp &op) {
        std::vector<oir::Value *> args;
        args.reserve(op.args().size());
        std::vector<oir::Type *> arg_types;
        arg_types.reserve(op.args().size());
        for (auto *arg : op.args()) {
            auto *value = value_for(arg);
            args.push_back(value);
            arg_types.push_back(value->type());
        }

        auto *return_type =
            op.result() == nullptr ? types().void_ty() : lower_type(op.result()->type());
        auto *callee = ensure_function(op.callee(), return_type, arg_types);
        auto *call =
            builder_->create_call(callee, return_type, args,
                                  op.result() == nullptr ? "" : result_name(op.result(), "call"));
        if (op.result() != nullptr) {
            bind_result(op, call);
        }
    }

    void lower_if(const yir::IfOp &op) {
        auto *cond = value_for(op.condition());
        auto before_values = value_map_;
        auto before_ssa = ssa_vars_;
        auto before_env = snapshot_existing_vars();
        auto *cond_block = builder_->insert_block();

        auto *then_block = current_function_->create_block("if.then");
        auto *else_block = current_function_->create_block(op.has_else() ? "if.else" : "if.empty");
        auto *merge_block = current_function_->create_block("if.end");

        builder_->create_cond_br(cond, then_block, else_block);

        value_map_ = before_values;
        ssa_vars_ = before_ssa;
        builder_->set_insert_point(then_block);
        lower_region(op.then_region());
        oir::BasicBlock *then_end = reachable_block();
        ValueMap then_values = value_map_;

        value_map_ = before_values;
        ssa_vars_ = before_ssa;
        builder_->set_insert_point(else_block);
        if (op.has_else()) {
            lower_region(op.else_region());
        }
        oir::BasicBlock *else_end = reachable_block();
        ValueMap else_values = value_map_;

        std::vector<Edge> incoming;
        if (then_end != nullptr) {
            builder_->set_insert_point(then_end);
            builder_->create_br(merge_block);
            incoming.push_back({then_end, std::move(then_values)});
        }
        if (else_end != nullptr) {
            builder_->set_insert_point(else_end);
            builder_->create_br(merge_block);
            incoming.push_back({else_end, std::move(else_values)});
        }

        ssa_vars_ = before_ssa;
        value_map_ = before_values;

        if (incoming.empty()) {
            (void)cond_block;
            current_function_->erase_block(merge_block);
            builder_->clear_insert_point();
            return;
        }

        builder_->set_insert_point(merge_block);
        merge_values(before_env, incoming, merge_block);
    }

    void lower_while(const yir::WhileOp &op) {
        auto before_values = value_map_;
        auto before_ssa = ssa_vars_;
        auto before_env = snapshot_existing_vars();
        auto assigned = assigned_existing_vars(op.body_region(), before_env);

        auto *preheader = builder_->insert_block();
        auto *header = current_function_->create_block("while.cond");
        auto *body = current_function_->create_block("while.body");
        auto *exit = current_function_->create_block("while.end");

        builder_->create_br(header);
        builder_->set_insert_point(header);

        std::unordered_map<const yir::Value *, oir::PhiInst *> loop_phis;
        value_map_ = before_values;
        ssa_vars_ = before_ssa;
        for (auto *var : assigned) {
            auto *phi = builder_->create_phi(before_env.at(var)->type(),
                                             unique_name(var_name(var) + ".loop"));
            phi->add_incoming(before_env.at(var), preheader);
            value_map_[var] = phi;
            loop_phis[var] = phi;
        }

        auto loop_entry_values = value_map_;
        auto loop_entry_ssa = ssa_vars_;
        auto *cond = lower_condition_region(op.cond_region());
        auto *cond_end = reachable_block();
        if (cond_end == nullptr) {
            builder_->clear_insert_point();
            return;
        }
        builder_->set_insert_point(cond_end);
        builder_->create_cond_br(cond, body, exit);

        LoopContext context;
        context.continue_target = header;
        context.break_target = exit;
        loop_stack_.push_back(std::move(context));

        value_map_ = loop_entry_values;
        ssa_vars_ = loop_entry_ssa;
        builder_->set_insert_point(body);
        lower_region(op.body_region());
        oir::BasicBlock *body_end = reachable_block();
        if (body_end != nullptr) {
            loop_stack_.back().continue_edges.push_back({body_end, value_map_});
            builder_->set_insert_point(body_end);
            builder_->create_br(header);
        }

        auto loop_context = std::move(loop_stack_.back());
        loop_stack_.pop_back();

        for (const auto &edge : loop_context.continue_edges) {
            for (auto &[var, phi] : loop_phis) {
                phi->add_incoming(value_from(edge.values, var, before_env.at(var)), edge.block);
            }
        }

        std::vector<Edge> exit_edges;
        exit_edges.push_back({cond_end, loop_entry_values});
        for (auto &edge : loop_context.break_edges) {
            exit_edges.push_back(std::move(edge));
        }

        value_map_ = before_values;
        ssa_vars_ = before_ssa;
        for (auto &[var, phi] : loop_phis) {
            value_map_[var] = phi;
        }

        builder_->set_insert_point(exit);
        merge_values(before_env, exit_edges, exit);
    }

    void lower_for(const yir::ForOp &op) {
        lower_assign_to_value(op.induction_var(), value_for(op.lower_bound()));

        auto before_values = value_map_;
        auto before_ssa = ssa_vars_;
        auto before_env = snapshot_existing_vars();
        auto assigned = assigned_existing_vars(op.body_region(), before_env);
        assigned.insert(op.induction_var());

        auto *preheader = builder_->insert_block();
        auto *header = current_function_->create_block("for.cond");
        auto *body = current_function_->create_block("for.body");
        auto *step = current_function_->create_block("for.step");
        auto *exit = current_function_->create_block("for.end");

        builder_->create_br(header);
        builder_->set_insert_point(header);

        std::unordered_map<const yir::Value *, oir::PhiInst *> loop_phis;
        value_map_ = before_values;
        ssa_vars_ = before_ssa;
        for (auto *var : assigned) {
            if (before_env.find(var) == before_env.end()) {
                continue;
            }
            auto *phi = builder_->create_phi(before_env.at(var)->type(),
                                             unique_name(var_name(var) + ".for"));
            phi->add_incoming(before_env.at(var), preheader);
            value_map_[var] = phi;
            loop_phis[var] = phi;
        }

        auto loop_entry_values = value_map_;
        auto loop_entry_ssa = ssa_vars_;
        auto *cond = builder_->create_icmp(oir::CmpPred::LT, value_for(op.induction_var()),
                                           value_for(op.upper_bound()),
                                           unique_name(var_name(op.induction_var()) + ".for.cond"));
        builder_->create_cond_br(cond, body, exit);

        LoopContext context;
        context.continue_target = step;
        context.break_target = exit;
        loop_stack_.push_back(std::move(context));

        value_map_ = loop_entry_values;
        ssa_vars_ = loop_entry_ssa;
        builder_->set_insert_point(body);
        lower_region(op.body_region());
        oir::BasicBlock *body_end = reachable_block();
        std::vector<Edge> step_edges;
        if (body_end != nullptr) {
            step_edges.push_back({body_end, value_map_});
            builder_->set_insert_point(body_end);
            builder_->create_br(step);
        }

        auto loop_context = std::move(loop_stack_.back());
        loop_stack_.pop_back();
        for (auto &edge : loop_context.continue_edges) {
            step_edges.push_back(std::move(edge));
        }

        if (!step_edges.empty()) {
            value_map_ = loop_entry_values;
            ssa_vars_ = loop_entry_ssa;
            builder_->set_insert_point(step);
            for (auto *var : assigned) {
                if (before_env.find(var) == before_env.end()) {
                    continue;
                }
                oir::Value *first = value_from(step_edges.front().values, var, before_env.at(var));
                bool all_same = true;
                for (std::size_t i = 1; i < step_edges.size(); ++i) {
                    if (value_from(step_edges[i].values, var, before_env.at(var)) != first) {
                        all_same = false;
                        break;
                    }
                }
                if (step_edges.size() == 1 || all_same) {
                    value_map_[var] = first;
                    continue;
                }
                auto *phi =
                    builder_->create_phi(first->type(), unique_name(var_name(var) + ".step"));
                for (const auto &edge : step_edges) {
                    phi->add_incoming(value_from(edge.values, var, before_env.at(var)), edge.block);
                }
                value_map_[var] = phi;
            }

            auto *next = builder_->create_binary(
                oir::Instruction::OpID::Add, value_for(op.induction_var()), value_for(op.step()),
                unique_name(var_name(op.induction_var()) + ".next"));
            value_map_[op.induction_var()] = next;
            builder_->create_br(header);
            for (auto &[var, phi] : loop_phis) {
                phi->add_incoming(value_from(value_map_, var, before_env.at(var)), step);
            }
        } else {
            current_function_->erase_block(step);
        }

        std::vector<Edge> exit_edges;
        exit_edges.push_back({header, loop_entry_values});
        for (auto &edge : loop_context.break_edges) {
            exit_edges.push_back(std::move(edge));
        }
        value_map_ = before_values;
        ssa_vars_ = before_ssa;
        for (auto &[var, phi] : loop_phis) {
            value_map_[var] = phi;
        }

        builder_->set_insert_point(exit);
        merge_values(before_env, exit_edges, exit);
    }

    void lower_break() {
        if (loop_stack_.empty()) {
            throw std::runtime_error("yir.break outside of loop");
        }
        auto *block = reachable_block();
        if (block == nullptr) {
            return;
        }
        loop_stack_.back().break_edges.push_back({block, value_map_});
        builder_->create_br(loop_stack_.back().break_target);
    }

    void lower_continue() {
        if (loop_stack_.empty()) {
            throw std::runtime_error("yir.continue outside of loop");
        }
        auto *block = reachable_block();
        if (block == nullptr) {
            return;
        }
        loop_stack_.back().continue_edges.push_back({block, value_map_});
        builder_->create_br(loop_stack_.back().continue_target);
    }

    oir::Value *lower_condition_region(const yir::Region &region) {
        oir::Value *condition = module_->create_i1(true);
        for (const auto &op : region.operations()) {
            if (!can_insert()) {
                return condition;
            }
            if (const auto *cond = dynamic_cast<const yir::CondOp *>(op.get())) {
                condition = value_for(cond->condition());
                continue;
            }
            lower_op(*op);
        }
        return condition;
    }

    ValueSet assigned_existing_vars(const yir::Region &region, const ValueMap &before_env) {
        AssignmentCollector collector;
        auto assigned = collector.collect(region);
        ValueSet out;
        for (auto *var : assigned) {
            if (before_env.find(var) != before_env.end()) {
                out.insert(var);
            }
        }
        return out;
    }

    void merge_values(const ValueMap &before_env, const std::vector<Edge> &incoming,
                      oir::BasicBlock *merge_block) {
        if (incoming.empty()) {
            builder_->clear_insert_point();
            return;
        }

        for (const auto &[var, before_value] : before_env) {
            oir::Value *first = value_from(incoming.front().values, var, before_value);
            bool all_same = true;
            for (std::size_t i = 1; i < incoming.size(); ++i) {
                if (value_from(incoming[i].values, var, before_value) != first) {
                    all_same = false;
                    break;
                }
            }
            if (incoming.size() == 1 || all_same) {
                value_map_[var] = first;
                continue;
            }

            auto *phi = builder_->create_phi(first->type(), unique_name(var_name(var) + ".phi"));
            for (const auto &edge : incoming) {
                phi->add_incoming(value_from(edge.values, var, before_value), edge.block);
            }
            value_map_[var] = phi;
        }
        builder_->set_insert_point(merge_block);
    }

    void lower_assign_to_value(const yir::Value *target, oir::Value *value) {
        if (ssa_vars_.find(target) != ssa_vars_.end()) {
            value_map_[target] = value;
            return;
        }
        auto found = memory_addresses_.find(target);
        if (found != memory_addresses_.end()) {
            builder_->create_store(value, found->second);
            return;
        }
        throw std::runtime_error("for induction variable is not assignable");
    }

    oir::Value *value_for(const yir::Value *value) {
        if (value == nullptr) {
            return nullptr;
        }
        if (ssa_vars_.find(value) != ssa_vars_.end()) {
            auto found = value_map_.find(value);
            if (found == value_map_.end()) {
                throw std::runtime_error("SSA value is not available: " + var_name(value));
            }
            return found->second;
        }

        auto memory = memory_addresses_.find(value);
        if (memory != memory_addresses_.end()) {
            if (is_scalar_type(value->type())) {
                return builder_->create_load(memory->second, lower_type(value->type()),
                                             unique_name(var_name(value) + ".load"));
            }
            return memory->second;
        }

        auto found = value_map_.find(value);
        if (found != value_map_.end()) {
            return found->second;
        }

        throw std::runtime_error("unmapped YIR value: " + var_name(value));
    }

    oir::Value *address_for(const yir::Value *value) {
        auto memory = memory_addresses_.find(value);
        if (memory != memory_addresses_.end()) {
            return memory->second;
        }
        auto found = value_map_.find(value);
        if (found != value_map_.end() && found->second->type()->is_pointer()) {
            return found->second;
        }
        throw std::runtime_error("YIR value has no address: " + var_name(value));
    }

    oir::Value *create_element_ptr(const yir::Value *base,
                                   const std::vector<oir::Value *> &lowered_indices,
                                   oir::Type *result_ptr_type, const std::string &name) {
        std::vector<oir::Value *> indices;
        if (base->type()->is_array()) {
            indices.push_back(module_->create_i32(0));
        }
        indices.insert(indices.end(), lowered_indices.begin(), lowered_indices.end());
        return builder_->create_gep(address_for(base), result_ptr_type, indices, name);
    }

    std::vector<oir::Value *> constant_indices(const std::vector<std::uint64_t> &indices) {
        std::vector<oir::Value *> out;
        out.reserve(indices.size());
        for (std::uint64_t index : indices) {
            out.push_back(module_->create_i32(static_cast<std::int64_t>(index)));
        }
        return out;
    }

    oir::Value *zero_value(const yir::TypePtr &type) {
        return zero_value(lower_type(type));
    }

    oir::Value *zero_value(oir::Type *type) {
        if (auto *integer = dynamic_cast<oir::IntegerType *>(type)) {
            return integer->bit_width() == 1 ? static_cast<oir::Value *>(module_->create_i1(false))
                                             : static_cast<oir::Value *>(module_->create_i32(0));
        }
        if (type->is_float()) {
            return module_->create_f32(0.0F);
        }
        return module_->create_zero(type);
    }

    oir::Value *literal_value(const std::string &literal, const yir::TypePtr &type) {
        if (literal.empty() || literal == "zero") {
            return zero_value(type);
        }
        if (type->kind() == yir::Type::Kind::F32) {
            return module_->create_f32(std::strtof(literal.c_str(), nullptr));
        }
        if (type->kind() == yir::Type::Kind::I1) {
            return module_->create_i1(literal == "true" || literal == "1");
        }
        return module_->create_i32(std::strtoll(literal.c_str(), nullptr, 10));
    }

    oir::Value *to_bool(oir::Value *value, const std::string &name) {
        if (auto *integer = dynamic_cast<oir::IntegerType *>(value->type())) {
            return builder_->create_icmp(oir::CmpPred::NE, value,
                                         integer->bit_width() == 1
                                             ? static_cast<oir::Value *>(module_->create_i1(false))
                                             : static_cast<oir::Value *>(module_->create_i32(0)),
                                         name);
        }
        if (value->type()->is_float()) {
            return builder_->create_fcmp(oir::CmpPred::NE, value, module_->create_f32(0.0F), name);
        }
        return builder_->create_icmp(oir::CmpPred::NE, value, module_->create_undef(value->type()),
                                     name);
    }

    oir::Value *logical_not(oir::Value *value, const std::string &name) {
        if (auto *integer = dynamic_cast<oir::IntegerType *>(value->type())) {
            return builder_->create_icmp(oir::CmpPred::EQ, value,
                                         integer->bit_width() == 1
                                             ? static_cast<oir::Value *>(module_->create_i1(false))
                                             : static_cast<oir::Value *>(module_->create_i32(0)),
                                         name);
        }
        if (value->type()->is_float()) {
            return builder_->create_fcmp(oir::CmpPred::EQ, value, module_->create_f32(0.0F), name);
        }
        return builder_->create_icmp(oir::CmpPred::EQ, value, module_->create_undef(value->type()),
                                     name);
    }

    template <typename PredT> oir::CmpPred lower_pred(PredT pred) {
        switch (pred) {
        case PredT::Eq:
            return oir::CmpPred::EQ;
        case PredT::Ne:
            return oir::CmpPred::NE;
        case PredT::Lt:
            return oir::CmpPred::LT;
        case PredT::Le:
            return oir::CmpPred::LE;
        case PredT::Gt:
            return oir::CmpPred::GT;
        case PredT::Ge:
            return oir::CmpPred::GE;
        }
        return oir::CmpPred::EQ;
    }

    oir::Function *ensure_function(const std::string &name, oir::Type *return_type,
                                   const std::vector<oir::Type *> &arg_types) {
        if (auto *existing = module_->get_function(name)) {
            return existing;
        }
        auto *type = types().func_ty(return_type, arg_types);
        return module_->create_function(name, type, true);
    }

    void bind_result(const yir::Operation &op, oir::Value *value) {
        if (op.result() != nullptr) {
            value_map_[op.result()] = value;
        }
    }

    ValueMap snapshot_existing_vars() const {
        ValueMap env;
        for (auto *var : ssa_vars_) {
            auto found = value_map_.find(var);
            if (found != value_map_.end()) {
                env[var] = found->second;
            }
        }
        return env;
    }

    static oir::Value *value_from(const ValueMap &values, const yir::Value *var,
                                  oir::Value *fallback) {
        auto found = values.find(var);
        return found == values.end() ? fallback : found->second;
    }

    bool can_insert() const {
        auto *block = builder_->insert_block();
        return block != nullptr && !block->has_terminator();
    }

    oir::BasicBlock *reachable_block() const {
        auto *block = builder_->insert_block();
        if (block == nullptr || block->has_terminator()) {
            return nullptr;
        }
        return block;
    }

    void emit_default_return_if_needed() {
        if (!can_insert()) {
            return;
        }
        auto *return_type = current_function_->return_type();
        if (return_type->is_void()) {
            builder_->create_ret();
            return;
        }
        builder_->create_ret(zero_value(return_type));
    }

    std::string result_name(const yir::Value *value, const std::string &fallback) {
        return unique_name(value == nullptr || value->name().empty() ? fallback : value->name());
    }

    std::string var_name(const yir::Value *value) const {
        if (value == nullptr || value->name().empty()) {
            return "v";
        }
        std::string name = value->name();
        if (!name.empty() && (name[0] == '%' || name[0] == '@')) {
            name.erase(name.begin());
        }
        return sanitize_name(name);
    }

    std::string unique_name(const std::string &base) {
        std::string clean = sanitize_name(base);
        auto &count = name_counts_[clean];
        std::string candidate;
        do {
            if (count == 0) {
                candidate = clean;
            } else {
                candidate = clean + "." + std::to_string(count);
            }
            ++count;
        } while (used_names_.find(candidate) != used_names_.end());
        used_names_.insert(candidate);
        return candidate;
    }

    std::unique_ptr<oir::Module> module_;
    std::unique_ptr<oir::IRBuilder> builder_;
    oir::Function *current_function_ = nullptr;
    std::unordered_map<std::string, oir::Type *> type_cache_;
    std::unordered_map<const yir::Function *, oir::Function *> functions_;
    std::unordered_map<const yir::Value *, oir::Value *> value_map_;
    std::unordered_map<const yir::Value *, oir::Value *> memory_addresses_;
    std::unordered_set<const yir::Value *> ssa_vars_;
    std::unordered_map<std::string, unsigned> name_counts_;
    std::unordered_set<std::string> used_names_;
    std::vector<LoopContext> loop_stack_;
};

} // namespace

std::unique_ptr<oir::Module> lower_yir_to_oir(const yir::Module &module) {
    Lowerer lowerer;
    return lowerer.lower(module);
}

} // namespace pass::yir_to_oir
