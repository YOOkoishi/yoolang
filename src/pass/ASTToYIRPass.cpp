#include "../../include/pass/ASTToYIRPass.h"

#include "../../include/yir/YIR.h"
#include "../../include/yir/YIRVerifier.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace pass {
namespace {

bool same_type(const yir::TypePtr &lhs, const yir::TypePtr &rhs) {
    if (lhs == rhs) {
        return true;
    }
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    return lhs->str() == rhs->str();
}

std::string sanitize_name(const std::string &name) {
    std::string out;
    out.reserve(name.size());
    for (char ch : name) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '_' || ch == '.') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "v" : out;
}

class Lowerer final : public ASTVisitor {
    struct ConstantValue {
        bool valid = false;
        BuiltinType type = BuiltinType::Int;
        int int_value = 0;
        float float_value = 0.0F;
    };

    struct Symbol {
        enum class Kind {
            Variable,
            Function,
        };

        Kind kind = Kind::Variable;
        std::string name;
        yir::Value *value = nullptr;
        yir::TypePtr type;
        bool is_const = false;
        bool is_array = false;
        bool is_global = false;
        bool is_builtin = false;
        bool is_variadic = false;
        yir::Function *function = nullptr;
        yir::TypePtr return_type;
        std::vector<yir::TypePtr> param_types;
        ConstantValue constant;
    };

    class SymbolTable {
      public:
        void enter_scope() {
            scopes_.push_back({});
        }

        void leave_scope() {
            scopes_.pop_back();
        }

        bool empty() const {
            return scopes_.empty();
        }

        bool define(Symbol symbol) {
            if (scopes_.empty()) {
                enter_scope();
            }
            auto &scope = scopes_.back();
            std::string name = symbol.name;
            return scope.emplace(std::move(name), std::move(symbol)).second;
        }

        Symbol *lookup(const std::string &name) {
            for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
                auto found = it->find(name);
                if (found != it->end()) {
                    return &found->second;
                }
            }
            return nullptr;
        }

      private:
        std::vector<std::unordered_map<std::string, Symbol>> scopes_;
    };

  public:
    std::unique_ptr<yir::Module> lower(CompUnit &unit) {
        module_ = std::make_unique<yir::Module>();
        unit.accept(*this);
        if (!errors_.empty()) {
            throw std::runtime_error(errors_.front());
        }
        return std::move(module_);
    }

    void visit(IntLiteral &node) override {
        last_value_ = emit<yir::ConstI32Op>(node.value, fresh_temp());
    }

    void visit(FloatLiteral &node) override {
        last_value_ = emit<yir::ConstF32Op>(node.value, fresh_temp());
    }

    void visit(LValExpr &node) override {
        last_value_ = lower_lval_value(node);
    }

    void visit(BinaryExpr &node) override {
        if (node.op == BinaryOp::And) {
            last_value_ = lower_logical_and(*node.lhs, *node.rhs);
            return;
        }
        if (node.op == BinaryOp::Or) {
            last_value_ = lower_logical_or(*node.lhs, *node.rhs);
            return;
        }

        yir::Value *lhs = lower_expr(*node.lhs);
        yir::Value *rhs = lower_expr(*node.rhs);

        switch (node.op) {
        case BinaryOp::Add:
            last_value_ = lower_add(lhs, rhs);
            return;
        case BinaryOp::Sub:
            last_value_ = lower_sub(lhs, rhs);
            return;
        case BinaryOp::Mul:
            last_value_ = lower_mul(lhs, rhs);
            return;
        case BinaryOp::Div:
            last_value_ = lower_div(lhs, rhs);
            return;
        case BinaryOp::Mod:
            last_value_ = emit<yir::RemSIOp>(cast_to(lhs, yir::Type::get_i32()),
                                             cast_to(rhs, yir::Type::get_i32()), fresh_temp());
            return;
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge:
        case BinaryOp::Eq:
        case BinaryOp::Ne:
            last_value_ = lower_compare(node.op, lhs, rhs);
            return;
        case BinaryOp::And:
        case BinaryOp::Or:
            break;
        }
    }

    void visit(UnaryExpr &node) override {
        yir::Value *value = lower_expr(*node.operand);
        switch (node.op) {
        case UnaryOp::Pos:
            last_value_ = value;
            return;
        case UnaryOp::Neg:
            if (value->type()->is_float()) {
                auto *zero = emit<yir::ConstF32Op>(0.0F, fresh_temp());
                last_value_ = emit<yir::SubFOp>(zero, value, fresh_temp());
            } else {
                auto *zero = emit<yir::ConstI32Op>(0, fresh_temp());
                last_value_ =
                    emit<yir::SubIOp>(zero, cast_to(value, yir::Type::get_i32()), fresh_temp());
            }
            return;
        case UnaryOp::Not:
            last_value_ = emit<yir::NotOp>(to_bool(value), fresh_temp());
            return;
        }
    }

    void visit(CallExpr &node) override {
        Symbol *callee = lookup_function(node.func_name);
        std::vector<yir::Value *> args;
        args.reserve(node.args.size());
        for (std::size_t i = 0; i < node.args.size(); ++i) {
            yir::TypePtr expected_type;
            if (callee != nullptr && !callee->is_variadic && i < callee->param_types.size()) {
                expected_type = callee->param_types[i];
            }
            args.push_back(lower_call_arg(*node.args[i], expected_type));
        }

        yir::TypePtr result_type = callee == nullptr ? yir::Type::get_i32() : callee->return_type;
        auto *op = emit<yir::CallOp>(node.func_name, std::move(args), result_type, fresh_temp());
        last_value_ = op == nullptr ? nullptr : op;
    }

    void visit(InitVal &) override {
    }

    void visit(ExprStmt &node) override {
        if (node.expr) {
            lower_expr(*node.expr);
        }
    }

    void visit(AssignStmt &node) override {
        Symbol *target = lookup_variable(node.target->name);
        yir::Value *value = lower_expr(*node.value);
        if (target == nullptr || value == nullptr) {
            if (target == nullptr) {
                errors_.push_back("unknown variable: " + node.target->name);
            }
            return;
        }
        if (target->is_const) {
            errors_.push_back("cannot assign to const variable: " + node.target->name);
            return;
        }
        if (node.target->indices.empty()) {
            emit<yir::AssignOp>(target->value, cast_to(value, target->type));
            return;
        }

        auto indices = lower_indices(node.target->indices);
        yir::TypePtr element_type = type_after_indices(target->type, indices.size());
        emit<yir::ArrayStoreOp>(cast_to(value, element_type), target->value, std::move(indices));
    }

    void visit(BlockStmt &node) override {
        enter_scope();
        for (auto &stmt : node.stmts) {
            stmt->accept(*this);
        }
        leave_scope();
    }

    void visit(ReturnStmt &node) override {
        if (!node.expr) {
            emit<yir::ReturnOp>();
            return;
        }
        yir::Value *value = lower_expr(*node.expr);
        if (current_return_type_ != nullptr && !current_return_type_->is_void()) {
            value = cast_to(value, current_return_type_);
        }
        emit<yir::ReturnOp>(value);
    }

    void visit(IfStmt &node) override {
        yir::Value *condition = to_bool(lower_expr(*node.cond));
        auto *if_op = current_region_->append<yir::IfOp>(condition);

        RegionScope then_scope(*this, if_op->then_region());
        node.then_stmt->accept(*this);

        if (node.else_stmt) {
            if_op->set_has_else(true);
            RegionScope else_scope(*this, if_op->else_region());
            node.else_stmt->accept(*this);
        }
    }

    void visit(WhileStmt &node) override {
        auto *while_op = current_region_->append<yir::WhileOp>();

        {
            RegionScope cond_scope(*this, while_op->cond_region());
            yir::Value *condition = to_bool(lower_expr(*node.cond));
            emit<yir::CondOp>(condition);
        }

        {
            RegionScope body_scope(*this, while_op->body_region());
            ++loop_depth_;
            node.body->accept(*this);
            --loop_depth_;
        }
    }

    void visit(BreakStmt &) override {
        if (loop_depth_ == 0) {
            errors_.push_back("break outside of loop");
        }
        emit<yir::BreakOp>();
    }

    void visit(ContinueStmt &) override {
        if (loop_depth_ == 0) {
            errors_.push_back("continue outside of loop");
        }
        emit<yir::ContinueOp>();
    }

    void visit(VarDecl &node) override {
        yir::TypePtr storage_type = type_for_var(node.base_type, node.dimensions);
        ConstantValue constant = constant_value_for_decl(node);
        if (storage_type->is_array()) {
            auto *array = emit<yir::ArrayVarOp>(storage_type, fresh_name(node.name));
            define_variable(node.name, array, storage_type, node.is_const, true, false, constant);
            if (node.init) {
                emit_array_init(array, storage_type, *node.init);
            }
            return;
        }

        yir::Value *initializer = nullptr;
        if (node.init) {
            initializer = lower_scalar_init(storage_type, *node.init);
        } else {
            initializer = emit<yir::ZeroOp>(storage_type, fresh_temp());
        }
        auto *var = emit<yir::VarOp>(storage_type, cast_to(initializer, storage_type),
                                     fresh_name(node.name));
        define_variable(node.name, var, storage_type, node.is_const, false, false, constant);
    }

    void visit(DeclStmt &node) override {
        for (auto &decl : node.decls) {
            decl->accept(*this);
        }
    }

    void visit(FuncDef &node) override {
        Symbol *function_symbol = lookup_function(node.name);
        if (function_symbol == nullptr || function_symbol->function == nullptr) {
            return;
        }

        current_function_ = function_symbol->function;
        current_region_ = &current_function_->body();
        current_return_type_ = type_for_builtin(node.return_type);
        name_counts_.clear();
        temp_counter_ = 0;

        enter_scope();
        for (std::size_t i = 0; i < node.params.size(); ++i) {
            const auto &param = node.params[i];
            yir::Value *param_value =
                current_function_->add_param(param_type(param), fresh_name(param.name + ".arg"));
            if (param.dimensions.empty()) {
                auto type = type_for_builtin(param.type);
                auto *var = emit<yir::VarOp>(type, param_value, fresh_name(param.name));
                define_variable(param.name, var, type, false, false, false, {});
            } else {
                define_variable(param.name, param_value, param_type(param), false, true, false, {});
            }
        }

        if (node.body) {
            node.body->accept(*this);
        }
        leave_scope();

        current_function_ = nullptr;
        current_region_ = nullptr;
        current_return_type_ = nullptr;
    }

    void visit(CompUnit &node) override {
        enter_scope();
        install_sysy_builtins();
        for (auto &decl : node.global_decls) {
            lower_global_decl(*decl);
        }

        for (auto &func : node.functions) {
            std::vector<yir::TypePtr> param_types;
            for (const auto &param : func->params) {
                param_types.push_back(param_type(param));
            }
            auto return_type = type_for_builtin(func->return_type);
            auto *function = module_->add_function(func->name, return_type, param_types);
            define_function(func->name, function, return_type, std::move(param_types), false,
                            false);
        }

        for (auto &func : node.functions) {
            func->accept(*this);
        }
        leave_scope();
    }

  private:
    class RegionScope {
      public:
        RegionScope(Lowerer &lowerer, yir::Region &region)
            : lowerer_(lowerer), previous_(lowerer.current_region_) {
            lowerer_.current_region_ = &region;
        }
        ~RegionScope() {
            lowerer_.current_region_ = previous_;
        }

      private:
        Lowerer &lowerer_;
        yir::Region *previous_;
    };

    template <typename OpT, typename... Args> yir::Value *emit(Args &&...args) {
        if (current_region_ == nullptr) {
            errors_.push_back("attempted to emit YIR outside of a region");
            return nullptr;
        }
        auto *op = current_region_->append<OpT>(std::forward<Args>(args)...);
        return op->result();
    }

    yir::Value *lower_expr(Expr &expr) {
        last_value_ = nullptr;
        expr.accept(*this);
        return last_value_;
    }

    yir::Value *lower_lval_value(LValExpr &lval) {
        Symbol *symbol = lookup_variable(lval.name);
        if (symbol == nullptr) {
            errors_.push_back("unknown variable: " + lval.name);
            return emit<yir::ConstI32Op>(0, fresh_temp());
        }
        if (lval.indices.empty()) {
            return symbol->value;
        }

        auto indices = lower_indices(lval.indices);
        yir::TypePtr indexed_type = type_after_indices(symbol->type, indices.size());
        if (indexed_type != nullptr && indexed_type->is_array()) {
            return emit<yir::ElemAddrOp>(symbol->value, std::move(indices),
                                         yir::Type::get_ptr(indexed_type),
                                         fresh_name(lval.name + ".addr"));
        }
        return emit<yir::ArrayLoadOp>(symbol->value, std::move(indices), indexed_type,
                                      fresh_name(lval.name));
    }

    yir::Value *lower_lval_pointer(LValExpr &lval, yir::TypePtr expected_type) {
        Symbol *symbol = lookup_variable(lval.name);
        if (symbol == nullptr) {
            errors_.push_back("unknown variable: " + lval.name);
            return emit<yir::ConstI32Op>(0, fresh_temp());
        }
        if (lval.indices.empty()) {
            return expected_type == nullptr ? decay(symbol->value)
                                            : cast_to(symbol->value, expected_type);
        }

        auto indices = lower_indices(lval.indices);
        yir::TypePtr indexed_type = type_after_indices(symbol->type, indices.size());
        auto *address = emit<yir::ElemAddrOp>(symbol->value, std::move(indices),
                                              yir::Type::get_ptr(indexed_type),
                                              fresh_name(lval.name + ".addr"));
        return expected_type == nullptr ? decay(address) : cast_to(address, expected_type);
    }

    yir::Value *lower_call_arg(Expr &expr, yir::TypePtr expected_type) {
        if (auto *lval = dynamic_cast<LValExpr *>(&expr)) {
            if (expected_type != nullptr && expected_type->is_ptr()) {
                return lower_lval_pointer(*lval, expected_type);
            }
            return expected_type == nullptr ? decay(lower_lval_value(*lval))
                                            : cast_to(lower_lval_value(*lval), expected_type);
        }
        return expected_type == nullptr ? lower_expr(expr)
                                        : cast_to(lower_expr(expr), expected_type);
    }

    std::vector<yir::Value *> lower_indices(const std::vector<std::unique_ptr<Expr>> &exprs) {
        std::vector<yir::Value *> indices;
        indices.reserve(exprs.size());
        for (auto &index : exprs) {
            indices.push_back(cast_to(lower_expr(*index), yir::Type::get_i32()));
        }
        return indices;
    }

    yir::Value *decay(yir::Value *value) {
        if (value == nullptr || value->type() == nullptr) {
            return value;
        }
        if (value->type()->is_array()) {
            return emit<yir::DecayOp>(value, yir::Type::get_ptr(value->type()->element()),
                                      fresh_temp());
        }
        if (value->type()->is_ptr()) {
            auto pointee = value->type()->pointee();
            if (pointee != nullptr && pointee->is_array()) {
                return emit<yir::DecayOp>(value, yir::Type::get_ptr(pointee->element()),
                                          fresh_temp());
            }
        }
        return value;
    }

    yir::TypePtr type_after_indices(yir::TypePtr base_type, std::size_t index_count) {
        yir::TypePtr current = base_type;
        if (current != nullptr && current->is_ptr() && index_count > 0) {
            current = current->pointee();
            --index_count;
        }
        for (std::size_t i = 0; i < index_count; ++i) {
            if (current != nullptr && current->is_array()) {
                current = current->element();
            }
        }
        return current == nullptr ? yir::Type::get_i32() : current;
    }

    yir::Value *lower_add(yir::Value *lhs, yir::Value *rhs) {
        if (lhs->type()->is_float() || rhs->type()->is_float()) {
            return emit<yir::AddFOp>(cast_to(lhs, yir::Type::get_f32()),
                                     cast_to(rhs, yir::Type::get_f32()), fresh_temp());
        }
        return emit<yir::AddIOp>(cast_to(lhs, yir::Type::get_i32()),
                                 cast_to(rhs, yir::Type::get_i32()), fresh_temp());
    }

    yir::Value *lower_sub(yir::Value *lhs, yir::Value *rhs) {
        if (lhs->type()->is_float() || rhs->type()->is_float()) {
            return emit<yir::SubFOp>(cast_to(lhs, yir::Type::get_f32()),
                                     cast_to(rhs, yir::Type::get_f32()), fresh_temp());
        }
        return emit<yir::SubIOp>(cast_to(lhs, yir::Type::get_i32()),
                                 cast_to(rhs, yir::Type::get_i32()), fresh_temp());
    }

    yir::Value *lower_mul(yir::Value *lhs, yir::Value *rhs) {
        if (lhs->type()->is_float() || rhs->type()->is_float()) {
            return emit<yir::MulFOp>(cast_to(lhs, yir::Type::get_f32()),
                                     cast_to(rhs, yir::Type::get_f32()), fresh_temp());
        }
        return emit<yir::MulIOp>(cast_to(lhs, yir::Type::get_i32()),
                                 cast_to(rhs, yir::Type::get_i32()), fresh_temp());
    }

    yir::Value *lower_div(yir::Value *lhs, yir::Value *rhs) {
        if (lhs->type()->is_float() || rhs->type()->is_float()) {
            return emit<yir::DivFOp>(cast_to(lhs, yir::Type::get_f32()),
                                     cast_to(rhs, yir::Type::get_f32()), fresh_temp());
        }
        return emit<yir::DivSIOp>(cast_to(lhs, yir::Type::get_i32()),
                                  cast_to(rhs, yir::Type::get_i32()), fresh_temp());
    }

    yir::Value *lower_compare(BinaryOp op, yir::Value *lhs, yir::Value *rhs) {
        if (lhs->type()->is_float() || rhs->type()->is_float()) {
            return emit<yir::FCmpOp>(to_fcmp(op), cast_to(lhs, yir::Type::get_f32()),
                                     cast_to(rhs, yir::Type::get_f32()), fresh_temp());
        }
        return emit<yir::ICmpOp>(to_icmp(op), cast_to(lhs, yir::Type::get_i32()),
                                 cast_to(rhs, yir::Type::get_i32()), fresh_temp());
    }

    yir::Value *lower_logical_and(Expr &lhs_expr, Expr &rhs_expr) {
        auto *false_value = emit<yir::ConstBoolOp>(false, fresh_temp());
        auto *result = emit<yir::VarOp>(yir::Type::get_i1(), false_value, fresh_name("logic"));

        yir::Value *lhs = to_bool(lower_expr(lhs_expr));
        auto *if_op = current_region_->append<yir::IfOp>(lhs);
        {
            RegionScope then_scope(*this, if_op->then_region());
            emit<yir::AssignOp>(result, to_bool(lower_expr(rhs_expr)));
        }
        return result;
    }

    yir::Value *lower_logical_or(Expr &lhs_expr, Expr &rhs_expr) {
        auto *false_value = emit<yir::ConstBoolOp>(false, fresh_temp());
        auto *result = emit<yir::VarOp>(yir::Type::get_i1(), false_value, fresh_name("logic"));

        yir::Value *lhs = to_bool(lower_expr(lhs_expr));
        auto *if_op = current_region_->append<yir::IfOp>(lhs);
        if_op->set_has_else(true);
        {
            RegionScope then_scope(*this, if_op->then_region());
            auto *true_value = emit<yir::ConstBoolOp>(true, fresh_temp());
            emit<yir::AssignOp>(result, true_value);
        }
        {
            RegionScope else_scope(*this, if_op->else_region());
            emit<yir::AssignOp>(result, to_bool(lower_expr(rhs_expr)));
        }
        return result;
    }

    yir::Value *cast_to(yir::Value *value, yir::TypePtr target) {
        if (value == nullptr || target == nullptr || same_type(value->type(), target)) {
            return value;
        }
        if (target->kind() == yir::Type::Kind::I1) {
            return to_bool(value);
        }
        if (target->kind() == yir::Type::Kind::I32) {
            if (value->type()->kind() == yir::Type::Kind::I1) {
                return emit<yir::ZExtI1ToI32Op>(value, fresh_temp());
            }
            if (value->type()->kind() == yir::Type::Kind::F32) {
                return emit<yir::FPToSIOp>(value, fresh_temp());
            }
        }
        if (target->kind() == yir::Type::Kind::F32) {
            if (value->type()->kind() == yir::Type::Kind::I1) {
                value = emit<yir::ZExtI1ToI32Op>(value, fresh_temp());
            }
            if (value->type()->kind() == yir::Type::Kind::I32) {
                return emit<yir::SIToFPOp>(value, fresh_temp());
            }
        }
        if (target->kind() == yir::Type::Kind::Ptr) {
            if (value->type()->is_array()) {
                return emit<yir::DecayOp>(value, target, fresh_temp());
            }
            if (value->type()->is_ptr() && value->type()->pointee() != nullptr &&
                value->type()->pointee()->is_array()) {
                return emit<yir::DecayOp>(value, target, fresh_temp());
            }
        }
        return value;
    }

    yir::Value *to_bool(yir::Value *value) {
        if (value == nullptr || value->type()->kind() == yir::Type::Kind::I1) {
            return value;
        }
        return emit<yir::ToBoolOp>(value, fresh_temp());
    }

    yir::Value *lower_scalar_init(yir::TypePtr storage_type, InitVal &init) {
        if (init.expr) {
            return lower_expr(*init.expr);
        }
        return emit<yir::ZeroOp>(storage_type, fresh_temp());
    }

    ConstantValue constant_value_for_decl(const VarDecl &decl) {
        ConstantValue value;
        if (!decl.is_const || !decl.dimensions.empty() || decl.init == nullptr) {
            return value;
        }
        value.valid = true;
        value.type = decl.base_type;
        if (decl.base_type == BuiltinType::Float) {
            value.float_value = eval_const_float(decl.init->expr.get());
            value.int_value = static_cast<int>(value.float_value);
            return value;
        }
        value.int_value = eval_const_int(decl.init->expr.get());
        value.float_value = static_cast<float>(value.int_value);
        return value;
    }

    void emit_array_init(yir::Value *array, yir::TypePtr storage_type, InitVal &init) {
        auto entries = build_array_init_entries(storage_type, init);
        current_region_->append<yir::ArrayInitOp>(array, storage_type, std::move(entries), true);
    }

    std::vector<yir::ArrayInitEntry> build_array_init_entries(yir::TypePtr storage_type,
                                                              InitVal &init) {
        if (!storage_type->is_array()) {
            return {};
        }

        std::vector<std::uint64_t> dimensions;
        yir::TypePtr element_type = collect_array_dimensions(storage_type, dimensions);
        std::uint64_t total_elements = 1;
        for (std::uint64_t dim : dimensions) {
            total_elements *= dim;
        }

        std::uint64_t cursor = 0;
        std::vector<yir::ArrayInitEntry> entries;
        lower_array_init_list(element_type, dimensions, init, 0, cursor, total_elements, entries);
        return entries;
    }

    yir::TypePtr collect_array_dimensions(yir::TypePtr type,
                                          std::vector<std::uint64_t> &dimensions) {
        while (type->is_array()) {
            dimensions.push_back(type->count());
            type = type->element();
        }
        return type;
    }

    void lower_array_init_list(yir::TypePtr element_type,
                               const std::vector<std::uint64_t> &dimensions, InitVal &init,
                               std::size_t level, std::uint64_t &cursor, std::uint64_t limit,
                               std::vector<yir::ArrayInitEntry> &entries) {
        if (cursor >= limit) {
            return;
        }
        if (init.expr) {
            append_array_init_entry(element_type, dimensions, cursor, init, entries);
            ++cursor;
            return;
        }
        for (auto &elem : init.elems) {
            if (cursor >= limit) {
                return;
            }
            if (elem->expr) {
                append_array_init_entry(element_type, dimensions, cursor, *elem, entries);
                ++cursor;
                continue;
            }

            std::uint64_t sub_size = 1;
            std::size_t sub_level =
                braced_subobject_level(dimensions, level, cursor, limit, sub_size);
            std::uint64_t sub_end = cursor + sub_size;
            lower_array_init_list(element_type, dimensions, *elem, sub_level, cursor, sub_end,
                                  entries);
            cursor = sub_end;
        }
    }

    std::size_t braced_subobject_level(const std::vector<std::uint64_t> &dimensions,
                                       std::size_t level, std::uint64_t cursor, std::uint64_t limit,
                                       std::uint64_t &sub_size) {
        for (std::size_t candidate = level + 1; candidate < dimensions.size(); ++candidate) {
            std::uint64_t size = suffix_element_count(dimensions, candidate);
            if (size != 0 && cursor % size == 0 && cursor + size <= limit) {
                sub_size = size;
                return candidate;
            }
        }
        sub_size = 1;
        return dimensions.size();
    }

    std::uint64_t suffix_element_count(const std::vector<std::uint64_t> &dimensions,
                                       std::size_t first_dimension) {
        std::uint64_t count = 1;
        for (std::size_t i = first_dimension; i < dimensions.size(); ++i) {
            count *= dimensions[i];
        }
        return count;
    }

    void append_array_init_entry(yir::TypePtr element_type,
                                 const std::vector<std::uint64_t> &dimensions,
                                 std::uint64_t flat_index, InitVal &init,
                                 std::vector<yir::ArrayInitEntry> &entries) {
        yir::ArrayInitEntry entry;
        entry.indices = coordinates_for_index(dimensions, flat_index);
        if (!try_const_expr_to_string(init.expr.get(), element_type, entry.literal)) {
            entry.value = cast_to(lower_expr(*init.expr), element_type);
        }
        entries.push_back(std::move(entry));
    }

    std::vector<std::uint64_t> coordinates_for_index(const std::vector<std::uint64_t> &dimensions,
                                                     std::uint64_t flat_index) {
        std::vector<std::uint64_t> coordinates(dimensions.size(), 0);
        for (std::size_t i = dimensions.size(); i > 0; --i) {
            std::uint64_t dim = dimensions[i - 1];
            coordinates[i - 1] = dim == 0 ? 0 : flat_index % dim;
            flat_index = dim == 0 ? 0 : flat_index / dim;
        }
        return coordinates;
    }

    void lower_global_decl(DeclStmt &decl_stmt) {
        for (auto &decl : decl_stmt.decls) {
            yir::TypePtr storage_type = type_for_var(decl->base_type, decl->dimensions);
            ConstantValue constant = constant_value_for_decl(*decl);
            yir::Global *global = module_->add_global(decl->name, storage_type, decl->is_const);
            if (decl->init) {
                global->set_initializer(global_initializer(storage_type, *decl->init));
            } else {
                global->set_initializer("zero");
            }
            define_variable(decl->name, global->address(), storage_type, decl->is_const,
                            storage_type->is_array(), true, constant);
        }
    }

    std::string global_initializer(yir::TypePtr storage_type, InitVal &init) {
        if (!storage_type->is_array()) {
            return init.expr ? const_expr_to_string(init.expr.get(), storage_type)
                             : zero_literal(storage_type);
        }

        std::vector<std::uint64_t> dimensions;
        yir::TypePtr element_type = collect_array_dimensions(storage_type, dimensions);
        std::uint64_t total_elements = 1;
        for (std::uint64_t dim : dimensions) {
            total_elements *= dim;
        }

        std::vector<std::string> values(total_elements, zero_literal(element_type));
        std::uint64_t cursor = 0;
        flatten_global_init_list(element_type, dimensions, init, 0, cursor, total_elements, values);

        std::uint64_t offset = 0;
        return format_global_initializer(values, dimensions, 0, offset);
    }

    void flatten_global_init_list(yir::TypePtr element_type,
                                  const std::vector<std::uint64_t> &dimensions, InitVal &init,
                                  std::size_t level, std::uint64_t &cursor, std::uint64_t limit,
                                  std::vector<std::string> &values) {
        if (cursor >= limit) {
            return;
        }
        if (init.expr) {
            values[cursor++] = const_expr_to_string(init.expr.get(), element_type);
            return;
        }
        for (auto &elem : init.elems) {
            if (cursor >= limit) {
                return;
            }
            if (elem->expr) {
                values[cursor++] = const_expr_to_string(elem->expr.get(), element_type);
                continue;
            }

            std::uint64_t sub_size = 1;
            std::size_t sub_level =
                braced_subobject_level(dimensions, level, cursor, limit, sub_size);
            std::uint64_t sub_end = cursor + sub_size;
            flatten_global_init_list(element_type, dimensions, *elem, sub_level, cursor, sub_end,
                                     values);
            cursor = sub_end;
        }
    }

    std::string format_global_initializer(const std::vector<std::string> &values,
                                          const std::vector<std::uint64_t> &dimensions,
                                          std::size_t level, std::uint64_t &offset) {
        if (level == dimensions.size()) {
            return offset < values.size() ? values[offset++] : "0";
        }

        std::ostringstream oss;
        oss << "{";
        for (std::uint64_t i = 0; i < dimensions[level]; ++i) {
            if (i != 0) {
                oss << ", ";
            }
            oss << format_global_initializer(values, dimensions, level + 1, offset);
        }
        oss << "}";
        return oss.str();
    }

    std::string const_expr_to_string(Expr *expr, yir::TypePtr type) {
        if (type->kind() == yir::Type::Kind::F32) {
            std::ostringstream oss;
            oss << std::setprecision(9) << eval_const_float(expr);
            return oss.str();
        }
        return std::to_string(eval_const_int(expr));
    }

    bool try_const_expr_to_string(Expr *expr, yir::TypePtr type, std::string &out) {
        if (!is_const_expr(expr)) {
            return false;
        }
        out = const_expr_to_string(expr, type);
        return true;
    }

    bool is_const_expr(Expr *expr) {
        if (expr == nullptr) {
            return true;
        }
        if (dynamic_cast<IntLiteral *>(expr) != nullptr ||
            dynamic_cast<FloatLiteral *>(expr) != nullptr) {
            return true;
        }
        if (auto *lval = dynamic_cast<LValExpr *>(expr)) {
            Symbol *symbol = lookup_variable(lval->name);
            return lval->indices.empty() && symbol != nullptr && symbol->constant.valid;
        }
        if (auto *unary = dynamic_cast<UnaryExpr *>(expr)) {
            return is_const_expr(unary->operand.get());
        }
        if (auto *binary = dynamic_cast<BinaryExpr *>(expr)) {
            switch (binary->op) {
            case BinaryOp::Add:
            case BinaryOp::Sub:
            case BinaryOp::Mul:
            case BinaryOp::Div:
            case BinaryOp::Mod:
                return is_const_expr(binary->lhs.get()) && is_const_expr(binary->rhs.get());
            default:
                return false;
            }
        }
        return false;
    }

    std::string zero_literal(yir::TypePtr type) {
        return type->kind() == yir::Type::Kind::F32 ? "0.0" : "0";
    }

    yir::TypePtr type_for_builtin(BuiltinType type) {
        switch (type) {
        case BuiltinType::Void:
            return yir::Type::get_void();
        case BuiltinType::Int:
            return yir::Type::get_i32();
        case BuiltinType::Float:
            return yir::Type::get_f32();
        }
        return yir::Type::get_i32();
    }

    yir::TypePtr type_for_var(BuiltinType base_type,
                              const std::vector<std::unique_ptr<Expr>> &dims) {
        yir::TypePtr type = type_for_builtin(base_type);
        for (auto it = dims.rbegin(); it != dims.rend(); ++it) {
            type =
                yir::Type::get_array(static_cast<std::uint64_t>(eval_const_int(it->get())), type);
        }
        return type;
    }

    yir::TypePtr param_type(const FuncParam &param) {
        yir::TypePtr type = type_for_builtin(param.type);
        if (param.dimensions.empty()) {
            return type;
        }
        for (auto it = param.dimensions.rbegin(); it != param.dimensions.rend(); ++it) {
            if (it->get() == nullptr) {
                continue;
            }
            type =
                yir::Type::get_array(static_cast<std::uint64_t>(eval_const_int(it->get())), type);
        }
        return yir::Type::get_ptr(type);
    }

    int eval_const_int(Expr *expr) {
        if (expr == nullptr) {
            return 0;
        }
        if (auto *int_lit = dynamic_cast<IntLiteral *>(expr)) {
            return int_lit->value;
        }
        if (auto *float_lit = dynamic_cast<FloatLiteral *>(expr)) {
            return static_cast<int>(float_lit->value);
        }
        if (auto *lval = dynamic_cast<LValExpr *>(expr)) {
            Symbol *symbol = lookup_variable(lval->name);
            if (lval->indices.empty() && symbol != nullptr && symbol->constant.valid) {
                return symbol->constant.type == BuiltinType::Float
                           ? static_cast<int>(symbol->constant.float_value)
                           : symbol->constant.int_value;
            }
            errors_.push_back("constant expression uses non-constant variable: " + lval->name);
            return 0;
        }
        if (auto *unary = dynamic_cast<UnaryExpr *>(expr)) {
            int value = eval_const_int(unary->operand.get());
            if (unary->op == UnaryOp::Neg) {
                return -value;
            }
            return value;
        }
        if (auto *binary = dynamic_cast<BinaryExpr *>(expr)) {
            int lhs = eval_const_int(binary->lhs.get());
            int rhs = eval_const_int(binary->rhs.get());
            switch (binary->op) {
            case BinaryOp::Add:
                return lhs + rhs;
            case BinaryOp::Sub:
                return lhs - rhs;
            case BinaryOp::Mul:
                return lhs * rhs;
            case BinaryOp::Div:
                return rhs == 0 ? 0 : lhs / rhs;
            case BinaryOp::Mod:
                return rhs == 0 ? 0 : lhs % rhs;
            default:
                return 0;
            }
        }
        return 0;
    }

    float eval_const_float(Expr *expr) {
        if (expr == nullptr) {
            return 0.0F;
        }
        if (auto *float_lit = dynamic_cast<FloatLiteral *>(expr)) {
            return float_lit->value;
        }
        if (auto *int_lit = dynamic_cast<IntLiteral *>(expr)) {
            return static_cast<float>(int_lit->value);
        }
        if (auto *lval = dynamic_cast<LValExpr *>(expr)) {
            Symbol *symbol = lookup_variable(lval->name);
            if (lval->indices.empty() && symbol != nullptr && symbol->constant.valid) {
                return symbol->constant.type == BuiltinType::Float
                           ? symbol->constant.float_value
                           : static_cast<float>(symbol->constant.int_value);
            }
            errors_.push_back("constant expression uses non-constant variable: " + lval->name);
            return 0.0F;
        }
        if (auto *unary = dynamic_cast<UnaryExpr *>(expr)) {
            float value = eval_const_float(unary->operand.get());
            if (unary->op == UnaryOp::Neg) {
                return -value;
            }
            return value;
        }
        if (auto *binary = dynamic_cast<BinaryExpr *>(expr)) {
            float lhs = eval_const_float(binary->lhs.get());
            float rhs = eval_const_float(binary->rhs.get());
            switch (binary->op) {
            case BinaryOp::Add:
                return lhs + rhs;
            case BinaryOp::Sub:
                return lhs - rhs;
            case BinaryOp::Mul:
                return lhs * rhs;
            case BinaryOp::Div:
                return rhs == 0.0F ? 0.0F : lhs / rhs;
            default:
                return static_cast<float>(eval_const_int(expr));
            }
        }
        return static_cast<float>(eval_const_int(expr));
    }

    yir::ICmpOp::Predicate to_icmp(BinaryOp op) {
        switch (op) {
        case BinaryOp::Eq:
            return yir::ICmpOp::Predicate::Eq;
        case BinaryOp::Ne:
            return yir::ICmpOp::Predicate::Ne;
        case BinaryOp::Lt:
            return yir::ICmpOp::Predicate::Lt;
        case BinaryOp::Le:
            return yir::ICmpOp::Predicate::Le;
        case BinaryOp::Gt:
            return yir::ICmpOp::Predicate::Gt;
        case BinaryOp::Ge:
            return yir::ICmpOp::Predicate::Ge;
        default:
            return yir::ICmpOp::Predicate::Eq;
        }
    }

    yir::FCmpOp::Predicate to_fcmp(BinaryOp op) {
        switch (op) {
        case BinaryOp::Eq:
            return yir::FCmpOp::Predicate::Eq;
        case BinaryOp::Ne:
            return yir::FCmpOp::Predicate::Ne;
        case BinaryOp::Lt:
            return yir::FCmpOp::Predicate::Lt;
        case BinaryOp::Le:
            return yir::FCmpOp::Predicate::Le;
        case BinaryOp::Gt:
            return yir::FCmpOp::Predicate::Gt;
        case BinaryOp::Ge:
            return yir::FCmpOp::Predicate::Ge;
        default:
            return yir::FCmpOp::Predicate::Eq;
        }
    }

    void enter_scope() {
        symbols_.enter_scope();
    }

    void leave_scope() {
        symbols_.leave_scope();
    }

    void define_variable(const std::string &name, yir::Value *value, yir::TypePtr type,
                         bool is_const, bool is_array, bool is_global, ConstantValue constant) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Variable;
        symbol.name = name;
        symbol.value = value;
        symbol.type = std::move(type);
        symbol.is_const = is_const;
        symbol.is_array = is_array;
        symbol.is_global = is_global;
        symbol.constant = constant;
        if (!symbols_.define(std::move(symbol))) {
            errors_.push_back("redefinition of symbol: " + name);
        }
    }

    void define_function(const std::string &name, yir::Function *function, yir::TypePtr return_type,
                         std::vector<yir::TypePtr> param_types, bool is_builtin, bool is_variadic) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Function;
        symbol.name = name;
        symbol.type = yir::Type::get_func(param_types, return_type);
        symbol.function = function;
        symbol.return_type = std::move(return_type);
        symbol.param_types = std::move(param_types);
        symbol.is_builtin = is_builtin;
        symbol.is_variadic = is_variadic;
        if (!symbols_.define(std::move(symbol))) {
            errors_.push_back("redefinition of symbol: " + name);
        }
    }

    Symbol *lookup_variable(const std::string &name) {
        Symbol *symbol = symbols_.lookup(name);
        if (symbol == nullptr || symbol->kind != Symbol::Kind::Variable) {
            return nullptr;
        }
        return symbol;
    }

    Symbol *lookup_function(const std::string &name) {
        Symbol *symbol = symbols_.lookup(name);
        if (symbol == nullptr || symbol->kind != Symbol::Kind::Function) {
            return nullptr;
        }
        return symbol;
    }

    void install_sysy_builtins() {
        auto i32 = yir::Type::get_i32();
        auto f32 = yir::Type::get_f32();
        auto void_ty = yir::Type::get_void();
        auto i32_ptr = yir::Type::get_ptr(i32);
        auto f32_ptr = yir::Type::get_ptr(f32);

        define_function("getint", nullptr, i32, {}, true, false);
        define_function("getch", nullptr, i32, {}, true, false);
        define_function("getfloat", nullptr, f32, {}, true, false);
        define_function("getarray", nullptr, i32, {i32_ptr}, true, false);
        define_function("getfarray", nullptr, i32, {f32_ptr}, true, false);

        define_function("putint", nullptr, void_ty, {i32}, true, false);
        define_function("putch", nullptr, void_ty, {i32}, true, false);
        define_function("putarray", nullptr, void_ty, {i32, i32_ptr}, true, false);
        define_function("putfloat", nullptr, void_ty, {f32}, true, false);
        define_function("putfarray", nullptr, void_ty, {i32, f32_ptr}, true, false);
        define_function("putf", nullptr, void_ty, {}, true, true);
        define_function("starttime", nullptr, void_ty, {}, true, false);
        define_function("stoptime", nullptr, void_ty, {}, true, false);
    }

    std::string fresh_temp() {
        return "v" + std::to_string(temp_counter_++);
    }

    std::string fresh_name(const std::string &base) {
        std::string name = sanitize_name(base);
        auto &count = name_counts_[name];
        if (count == 0) {
            ++count;
            return name;
        }
        return name + "." + std::to_string(count++);
    }

    std::unique_ptr<yir::Module> module_;
    yir::Function *current_function_ = nullptr;
    yir::Region *current_region_ = nullptr;
    yir::TypePtr current_return_type_;
    yir::Value *last_value_ = nullptr;
    int loop_depth_ = 0;
    unsigned temp_counter_ = 0;
    SymbolTable symbols_;
    std::unordered_map<std::string, unsigned> name_counts_;
    std::vector<std::string> errors_;
};

} // namespace

std::string_view ASTToYIRPass::name() const {
    return "ASTToYIRPass";
}

PassKind ASTToYIRPass::kind() const {
    return PassKind::Lowering;
}

PassResult ASTToYIRPass::run(PassContext &context) {
    if (!context.has_ast()) {
        return PassResult::fail("ASTToYIRPass requires AST in pass context");
    }

    try {
        Lowerer lowerer;
        auto module = lowerer.lower(*context.ast());
        auto verify = yir::verify_high_level_yir(*module);
        if (!verify.success) {
            return PassResult::fail(verify.errors.empty() ? "YIR verification failed"
                                                          : verify.errors.front());
        }
        context.set_artifact<std::unique_ptr<yir::Module>>(std::string(kArtifactKey), std::move(module));
        return PassResult::ok(true);
    } catch (const std::exception &ex) {
        return PassResult::fail(ex.what());
    }
}

} // namespace pass
