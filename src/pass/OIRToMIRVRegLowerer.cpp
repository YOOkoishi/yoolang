#include "../../include/pass/OIRToMIRCommon.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pass::oir_to_mir {
namespace {

class VRegLowerer final {
  public:
    std::unique_ptr<mir::Module> lower(const oir::Module &module) {
        module_ = std::make_unique<mir::Module>(module.name() + ".mir");

        for (const auto &global : module.globals()) {
            mir::Global lowered;
            lowered.name = global->name();
            lowered.type = type_info(global->value_type());
            lowered.is_const = global->is_const();
            lowered.initializer = global->initializer_literal();
            module_->add_global(std::move(lowered));
        }

        for (const auto &function : module.functions()) {
            std::vector<mir::TypeInfo> params;
            params.reserve(function->function_type()->param_types().size());
            for (auto *param : function->function_type()->param_types()) {
                params.push_back(type_info(param));
            }
            auto *out =
                module_->create_function(function->name(), type_info(function->return_type()),
                                         std::move(params), function->is_external());
            functions_[function.get()] = out;
        }

        for (const auto &function : module.functions()) {
            if (!function->is_external()) {
                lower_function(*function, *functions_.at(function.get()));
            }
        }

        return std::move(module_);
    }

  private:
    mir::TypeInfo type_info(oir::Type *type) const {
        mir::TypeInfo out;
        out.ir = type == nullptr ? "void" : type->print();
        out.align = 1;

        if (type == nullptr || type->is_void()) {
            out.value_type = mir::ValueType::Void;
            out.size = 0;
            return out;
        }
        if (auto *integer = dynamic_cast<oir::IntegerType *>(type)) {
            out.value_type = integer->bit_width() == 1 ? mir::ValueType::I1 : mir::ValueType::I32;
            out.size = 4;
            out.align = 4;
            return out;
        }
        if (type->is_float()) {
            out.value_type = mir::ValueType::F32;
            out.size = 4;
            out.align = 4;
            return out;
        }
        if (type->is_pointer()) {
            out.value_type = mir::ValueType::Ptr;
            out.size = 8;
            out.align = 8;
            return out;
        }
        if (auto *array = dynamic_cast<oir::ArrayType *>(type)) {
            auto element = type_info(array->element_type());
            out.value_type = mir::ValueType::Aggregate;
            out.size = element.size * array->element_count();
            out.align = element.align;
            return out;
        }
        throw std::runtime_error("unsupported OIR type for MIR: " + type->print());
    }

    mir::RegisterClass reg_class_for(mir::ValueType type) const {
        return type == mir::ValueType::F32 ? mir::RegisterClass::FPR32 : mir::RegisterClass::GPR;
    }

    mir::Register create_vreg(mir::ValueType type) {
        return current_function_->regs().create_virtual(reg_class_for(type), type);
    }

    void lower_function(const oir::Function &function, mir::MachineFunction &out) {
        current_function_ = &out;
        value_regs_.clear();
        alloca_slots_.clear();
        blocks_.clear();
        edge_blocks_.clear();
        pending_edge_blocks_.clear();
        temp_index_ = 0;

        for (const auto &block : function.blocks()) {
            blocks_[block.get()] = out.create_block(block->name());
        }

        for (const auto &arg : function.args()) {
            auto type = type_info(arg->type()).value_type;
            value_regs_[arg.get()] = create_vreg(type);
        }

        for (const auto &block : function.blocks()) {
            for (const auto &inst : block->instructions()) {
                preallocate_result(*inst);
            }
        }

        create_phi_edge_blocks(function, out);
        emit_parameter_copies(function);

        for (const auto &block : function.blocks()) {
            current_block_ = blocks_.at(block.get());
            for (const auto &inst : block->instructions()) {
                lower_instruction(*inst);
            }
        }

        fill_phi_edge_blocks();
        out.rebuild_cfg();
        current_function_ = nullptr;
        current_block_ = nullptr;
    }

    void preallocate_result(const oir::Instruction &inst) {
        if (auto *alloca = dynamic_cast<const oir::AllocaInst *>(&inst)) {
            std::string object_name =
                inst.name().empty() ? slot_name(inst, "alloca.obj") : inst.name() + ".obj";
            alloca_slots_[&inst] = current_function_->add_stack_slot(
                std::move(object_name), type_info(alloca->allocated_type()),
                mir::StackSlotKind::Alloca);
        }

        if (inst.type() == nullptr || inst.type()->is_void()) {
            return;
        }
        auto type = type_info(inst.type()).value_type;
        if (type != mir::ValueType::Aggregate) {
            value_regs_[&inst] = create_vreg(type);
        }
    }

    std::string slot_name(const oir::Value &value, const std::string &fallback) const {
        if (!value.name().empty()) {
            return value.name();
        }
        return fallback + "." + std::to_string(temp_index_);
    }

    void create_phi_edge_blocks(const oir::Function &function, mir::MachineFunction &out) {
        for (const auto &target_ptr : function.blocks()) {
            const auto *target = target_ptr.get();
            if (!block_has_phi(*target)) {
                continue;
            }
            for (auto *pred : target->predecessors()) {
                std::string name = "edge." + pred->name() + ".to." + target->name();
                auto *edge = out.create_block(name);
                edge_blocks_[edge_key(pred, target)] = edge;
                pending_edge_blocks_.push_back({pred, target, edge});
            }
        }
    }

    bool block_has_phi(const oir::BasicBlock &block) const {
        for (const auto &inst : block.instructions()) {
            if (inst->op() == oir::Instruction::OpID::Phi) {
                return true;
            }
            return false;
        }
        return false;
    }

    void emit_parameter_copies(const oir::Function &function) {
        current_block_ = blocks_.at(function.entry_block());
        std::size_t int_reg = 0;
        std::size_t float_reg = 0;
        std::int64_t stack_offset = 0;

        for (const auto &arg : function.args()) {
            auto type = type_info(arg->type()).value_type;
            auto dst = value_regs_.at(arg.get());
            if (type == mir::ValueType::F32) {
                if (float_reg < kFArgRegs.size()) {
                    std::vector<mir::Register> future_regs;
                    for (std::size_t i = float_reg + 1; i < kFArgRegs.size(); ++i) {
                        future_regs.push_back(phys_fpr(kFArgRegs[i]));
                    }
                    emit_entry_move(dst, phys_fpr(kFArgRegs[float_reg++]), future_regs);
                } else {
                    emit(mir::Opcode::LoadIncomingArg,
                         {mir::MachineOperand::reg_def(dst), mir::MachineOperand::imm(stack_offset),
                          mir::MachineOperand::type(type)});
                    stack_offset += 8;
                }
            } else {
                if (int_reg < kArgRegs.size()) {
                    std::vector<mir::Register> future_regs;
                    for (std::size_t i = int_reg + 1; i < kArgRegs.size(); ++i) {
                        future_regs.push_back(phys_gpr(kArgRegs[i]));
                    }
                    emit_entry_move(dst, phys_gpr(kArgRegs[int_reg++]), future_regs);
                } else {
                    emit(mir::Opcode::LoadIncomingArg,
                         {mir::MachineOperand::reg_def(dst), mir::MachineOperand::imm(stack_offset),
                          mir::MachineOperand::type(type)});
                    stack_offset += 8;
                }
            }
        }
    }

    void lower_instruction(const oir::Instruction &inst) {
        if (inst.op() == oir::Instruction::OpID::Phi) {
            return;
        }

        switch (inst.op()) {
        case oir::Instruction::OpID::Alloca:
            lower_alloca(static_cast<const oir::AllocaInst &>(inst));
            break;
        case oir::Instruction::OpID::Load:
            lower_load(static_cast<const oir::LoadInst &>(inst));
            break;
        case oir::Instruction::OpID::Store:
            lower_store(static_cast<const oir::StoreInst &>(inst));
            break;
        case oir::Instruction::OpID::GetElementPtr:
            lower_gep(static_cast<const oir::GetElementPtrInst &>(inst));
            break;
        case oir::Instruction::OpID::Add:
        case oir::Instruction::OpID::Sub:
        case oir::Instruction::OpID::Mul:
        case oir::Instruction::OpID::SDiv:
        case oir::Instruction::OpID::SRem:
            lower_int_binary(static_cast<const oir::BinaryInst &>(inst));
            break;
        case oir::Instruction::OpID::FAdd:
        case oir::Instruction::OpID::FSub:
        case oir::Instruction::OpID::FMul:
        case oir::Instruction::OpID::FDiv:
            lower_float_binary(static_cast<const oir::BinaryInst &>(inst));
            break;
        case oir::Instruction::OpID::ICmp:
            lower_icmp(static_cast<const oir::CmpInst &>(inst));
            break;
        case oir::Instruction::OpID::FCmp:
            lower_fcmp(static_cast<const oir::CmpInst &>(inst));
            break;
        case oir::Instruction::OpID::ZExt:
            lower_zext(static_cast<const oir::CastInst &>(inst));
            break;
        case oir::Instruction::OpID::SIToFP:
            lower_sitofp(static_cast<const oir::CastInst &>(inst));
            break;
        case oir::Instruction::OpID::FPToSI:
            lower_fptosi(static_cast<const oir::CastInst &>(inst));
            break;
        case oir::Instruction::OpID::Call:
            lower_call(static_cast<const oir::CallInst &>(inst));
            break;
        case oir::Instruction::OpID::Ret:
            lower_return(static_cast<const oir::ReturnInst &>(inst));
            break;
        case oir::Instruction::OpID::Br:
            lower_branch(static_cast<const oir::BranchInst &>(inst));
            break;
        case oir::Instruction::OpID::Phi:
            break;
        }
    }

    void lower_alloca(const oir::AllocaInst &inst) {
        auto dst = value_regs_.at(&inst);
        int object_slot = alloca_slots_.at(&inst);
        emit(mir::Opcode::LoadStackAddr,
             {mir::MachineOperand::reg_def(dst), mir::MachineOperand::slot(object_slot)});
    }

    void lower_load(const oir::LoadInst &inst) {
        auto loaded = type_info(inst.type());
        auto dst = value_regs_.at(&inst);
        auto addr = value_reg(inst.ptr());
        emit(mir::Opcode::LoadMem,
             {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(addr),
              mir::MachineOperand::type(loaded.value_type)});
    }

    void lower_store(const oir::StoreInst &inst) {
        auto stored = type_info(inst.value()->type());
        auto addr = value_reg(inst.ptr());
        if (stored.value_type == mir::ValueType::Aggregate) {
            if (dynamic_cast<const oir::ConstantZero *>(inst.value()) == nullptr) {
                throw std::runtime_error("only zero aggregate stores are supported in MIR vreg");
            }
            emit(mir::Opcode::MemZero,
                 {mir::MachineOperand::reg_use(addr),
                  mir::MachineOperand::imm(static_cast<std::int64_t>(stored.size))});
            return;
        }

        auto value = value_reg(inst.value());
        emit(mir::Opcode::StoreMem,
             {mir::MachineOperand::reg_use(addr), mir::MachineOperand::reg_use(value),
              mir::MachineOperand::type(stored.value_type)});
    }

    void lower_gep(const oir::GetElementPtrInst &inst) {
        auto dst = value_regs_.at(&inst);
        auto acc = value_reg(inst.base_ptr());

        auto *ptr_type = dynamic_cast<oir::PointerType *>(inst.base_ptr()->type());
        if (ptr_type == nullptr) {
            throw std::runtime_error("gep base is not a pointer");
        }

        oir::Type *cursor = ptr_type->element_type();
        std::int64_t constant_offset = 0;
        auto indices = inst.indices();
        for (std::size_t i = 0; i < indices.size(); ++i) {
            std::uint64_t stride = 0;
            if (i == 0) {
                stride = type_info(cursor).size;
            } else if (auto *array = dynamic_cast<oir::ArrayType *>(cursor)) {
                stride = type_info(array->element_type()).size;
                cursor = array->element_type();
            } else {
                stride = type_info(cursor).size;
            }

            if (stride == 0) {
                continue;
            }

            if (auto *constant = dynamic_cast<oir::ConstantInt *>(indices[i])) {
                constant_offset += constant->value() * static_cast<std::int64_t>(stride);
                continue;
            }

            auto index = value_reg(indices[i]);
            mir::Register scaled = index;
            if (stride != 1) {
                scaled = create_vreg(mir::ValueType::I32);
                if (is_power_of_two(stride)) {
                    emit(mir::Opcode::SllI,
                         {mir::MachineOperand::reg_def(scaled), mir::MachineOperand::reg_use(index),
                          mir::MachineOperand::imm(log2_u64(stride))});
                } else {
                    auto stride_reg = create_vreg(mir::ValueType::I32);
                    emit(mir::Opcode::LoadImm,
                         {mir::MachineOperand::reg_def(stride_reg),
                          mir::MachineOperand::imm(static_cast<std::int64_t>(stride))});
                    emit(mir::Opcode::Mul,
                         {mir::MachineOperand::reg_def(scaled), mir::MachineOperand::reg_use(index),
                          mir::MachineOperand::reg_use(stride_reg)});
                }
            }

            auto next = create_vreg(mir::ValueType::Ptr);
            emit(mir::Opcode::Add,
                 {mir::MachineOperand::reg_def(next), mir::MachineOperand::reg_use(acc),
                  mir::MachineOperand::reg_use(scaled)});
            acc = next;
        }

        if (constant_offset != 0) {
            auto offset = create_vreg(mir::ValueType::I32);
            auto next = create_vreg(mir::ValueType::Ptr);
            emit(mir::Opcode::LoadImm,
                 {mir::MachineOperand::reg_def(offset), mir::MachineOperand::imm(constant_offset)});
            emit(mir::Opcode::Add,
                 {mir::MachineOperand::reg_def(next), mir::MachineOperand::reg_use(acc),
                  mir::MachineOperand::reg_use(offset)});
            acc = next;
        }

        emit_move(dst, acc);
    }

    void lower_int_binary(const oir::BinaryInst &inst) {
        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        auto dst = value_regs_.at(&inst);
        mir::Opcode opcode = mir::Opcode::AddW;
        switch (inst.op()) {
        case oir::Instruction::OpID::Add:
            opcode = mir::Opcode::AddW;
            break;
        case oir::Instruction::OpID::Sub:
            opcode = mir::Opcode::SubW;
            break;
        case oir::Instruction::OpID::Mul:
            opcode = mir::Opcode::MulW;
            break;
        case oir::Instruction::OpID::SDiv:
            opcode = mir::Opcode::DivW;
            break;
        case oir::Instruction::OpID::SRem:
            opcode = mir::Opcode::RemW;
            break;
        default:
            break;
        }
        emit(opcode, {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(lhs),
                      mir::MachineOperand::reg_use(rhs)});
    }

    void lower_float_binary(const oir::BinaryInst &inst) {
        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        auto dst = value_regs_.at(&inst);
        mir::Opcode opcode = mir::Opcode::FAddS;
        switch (inst.op()) {
        case oir::Instruction::OpID::FAdd:
            opcode = mir::Opcode::FAddS;
            break;
        case oir::Instruction::OpID::FSub:
            opcode = mir::Opcode::FSubS;
            break;
        case oir::Instruction::OpID::FMul:
            opcode = mir::Opcode::FMulS;
            break;
        case oir::Instruction::OpID::FDiv:
            opcode = mir::Opcode::FDivS;
            break;
        default:
            break;
        }
        emit(opcode, {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(lhs),
                      mir::MachineOperand::reg_use(rhs)});
    }

    void lower_icmp(const oir::CmpInst &inst) {
        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        auto dst = value_regs_.at(&inst);
        switch (inst.pred()) {
        case oir::CmpPred::EQ: {
            auto tmp = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::Xor,
                 {mir::MachineOperand::reg_def(tmp), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            emit(mir::Opcode::SeqZ,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(tmp)});
            break;
        }
        case oir::CmpPred::NE: {
            auto tmp = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::Xor,
                 {mir::MachineOperand::reg_def(tmp), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            emit(mir::Opcode::Snez,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(tmp)});
            break;
        }
        case oir::CmpPred::LT:
            emit(mir::Opcode::Slt,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            break;
        case oir::CmpPred::LE: {
            auto tmp = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::Slt,
                 {mir::MachineOperand::reg_def(tmp), mir::MachineOperand::reg_use(rhs),
                  mir::MachineOperand::reg_use(lhs)});
            emit(mir::Opcode::XorI,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(tmp),
                  mir::MachineOperand::imm(1)});
            break;
        }
        case oir::CmpPred::GT:
            emit(mir::Opcode::Slt,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(rhs),
                  mir::MachineOperand::reg_use(lhs)});
            break;
        case oir::CmpPred::GE: {
            auto tmp = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::Slt,
                 {mir::MachineOperand::reg_def(tmp), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            emit(mir::Opcode::XorI,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(tmp),
                  mir::MachineOperand::imm(1)});
            break;
        }
        }
    }

    void lower_fcmp(const oir::CmpInst &inst) {
        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        auto dst = value_regs_.at(&inst);
        switch (inst.pred()) {
        case oir::CmpPred::EQ:
            emit(mir::Opcode::FeqS,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            break;
        case oir::CmpPred::NE: {
            auto tmp = create_vreg(mir::ValueType::I1);
            emit(mir::Opcode::FeqS,
                 {mir::MachineOperand::reg_def(tmp), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            emit(mir::Opcode::XorI,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(tmp),
                  mir::MachineOperand::imm(1)});
            break;
        }
        case oir::CmpPred::LT:
            emit(mir::Opcode::FltS,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            break;
        case oir::CmpPred::LE:
            emit(mir::Opcode::FleS,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            break;
        case oir::CmpPred::GT:
            emit(mir::Opcode::FltS,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(rhs),
                  mir::MachineOperand::reg_use(lhs)});
            break;
        case oir::CmpPred::GE:
            emit(mir::Opcode::FleS,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(rhs),
                  mir::MachineOperand::reg_use(lhs)});
            break;
        }
    }

    void lower_zext(const oir::CastInst &inst) {
        emit_move(value_regs_.at(&inst), value_reg(inst.src()));
    }

    void lower_sitofp(const oir::CastInst &inst) {
        auto src = value_reg(inst.src());
        auto dst = value_regs_.at(&inst);
        emit(mir::Opcode::FcvtSW,
             {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(src)});
    }

    void lower_fptosi(const oir::CastInst &inst) {
        auto src = value_reg(inst.src());
        auto dst = value_regs_.at(&inst);
        emit(mir::Opcode::FcvtWS,
             {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(src)});
    }

    void lower_call(const oir::CallInst &inst) {
        auto *callee = dynamic_cast<oir::Function *>(inst.callee());
        if (callee == nullptr) {
            throw std::runtime_error("MIR lowering only supports direct calls");
        }

        std::string symbol = callee->name();
        current_function_->note_call();

        std::size_t int_reg = 0;
        std::size_t float_reg = 0;
        std::int64_t stack_offset = 0;

        if (symbol == "starttime" || symbol == "stoptime") {
            emit(mir::Opcode::LoadImm,
                 {mir::MachineOperand::reg_def(phys_gpr("a0")), mir::MachineOperand::imm(0)});
            symbol = symbol == "starttime" ? "_sysy_starttime" : "_sysy_stoptime";
            int_reg = 1;
        }

        auto args = inst.args();
        for (auto *arg : args) {
            auto type = type_info(arg->type()).value_type;
            auto src = value_reg(arg);
            if (type == mir::ValueType::F32) {
                if (float_reg < kFArgRegs.size()) {
                    emit_move(phys_fpr(kFArgRegs[float_reg++]), src);
                } else {
                    emit(mir::Opcode::StoreOutgoingArg,
                         {mir::MachineOperand::reg_use(src), mir::MachineOperand::imm(stack_offset),
                          mir::MachineOperand::type(type)});
                    stack_offset += 8;
                }
            } else {
                if (int_reg < kArgRegs.size()) {
                    emit_move(phys_gpr(kArgRegs[int_reg++]), src);
                } else {
                    emit(mir::Opcode::StoreOutgoingArg,
                         {mir::MachineOperand::reg_use(src), mir::MachineOperand::imm(stack_offset),
                          mir::MachineOperand::type(type)});
                    stack_offset += 8;
                }
            }
        }

        current_function_->reserve_outgoing_arg_bytes(static_cast<std::uint64_t>(stack_offset));
        emit(mir::Opcode::Call, {mir::MachineOperand::symbol(symbol)});

        if (!inst.type()->is_void()) {
            auto result_type = type_info(inst.type()).value_type;
            if (result_type == mir::ValueType::F32) {
                emit_move(value_regs_.at(&inst), phys_fpr("fa0"));
            } else {
                emit_move(value_regs_.at(&inst), phys_gpr("a0"));
            }
        }
    }

    void lower_return(const oir::ReturnInst &inst) {
        if (inst.has_value()) {
            auto type = type_info(inst.value()->type()).value_type;
            if (type == mir::ValueType::F32) {
                emit_move(phys_fpr("fa0"), value_reg(inst.value()));
            } else {
                emit_move(phys_gpr("a0"), value_reg(inst.value()));
            }
        }
        emit(mir::Opcode::Jump, {mir::MachineOperand::block("epilogue")});
    }

    void lower_branch(const oir::BranchInst &inst) {
        if (!inst.is_conditional()) {
            emit(mir::Opcode::Jump,
                 {mir::MachineOperand::block(branch_target(inst.parent(), inst.target_bb()))});
            return;
        }

        auto cond = value_reg(inst.cond());
        emit(mir::Opcode::BranchNonZero,
             {mir::MachineOperand::reg_use(cond),
              mir::MachineOperand::block(branch_target(inst.parent(), inst.true_bb()))});
        emit(mir::Opcode::Jump,
             {mir::MachineOperand::block(branch_target(inst.parent(), inst.false_bb()))});
    }

    std::string branch_target(const oir::BasicBlock *pred, const oir::BasicBlock *succ) const {
        auto found = edge_blocks_.find(edge_key(pred, succ));
        if (found != edge_blocks_.end()) {
            return found->second->name();
        }
        return succ->name();
    }

    void fill_phi_edge_blocks() {
        for (const auto &edge : pending_edge_blocks_) {
            current_block_ = edge.block;
            std::vector<mir::Register> temps;
            std::vector<mir::Register> phis;
            for (const auto &inst : edge.target->instructions()) {
                auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
                if (phi == nullptr) {
                    break;
                }

                auto *incoming = incoming_for(*phi, edge.pred);
                auto type = type_info(phi->type()).value_type;
                auto temp = create_vreg(type);
                emit_move(temp, value_reg(incoming));
                temps.push_back(temp);
                phis.push_back(value_regs_.at(phi));
            }

            for (std::size_t i = 0; i < temps.size(); ++i) {
                emit_move(phis[i], temps[i]);
            }
            emit(mir::Opcode::Jump, {mir::MachineOperand::block(edge.target->name())});
        }
    }

    oir::Value *incoming_for(const oir::PhiInst &phi, const oir::BasicBlock *pred) const {
        for (const auto &incoming : phi.incoming()) {
            if (incoming.second == pred) {
                return incoming.first;
            }
        }
        throw std::runtime_error("phi missing incoming edge");
    }

    mir::Register value_reg(oir::Value *value) {
        if (auto *constant = dynamic_cast<oir::ConstantInt *>(value)) {
            auto reg = create_vreg(type_info(value->type()).value_type);
            emit(mir::Opcode::LoadImm,
                 {mir::MachineOperand::reg_def(reg), mir::MachineOperand::imm(constant->value())});
            return reg;
        }
        if (auto *constant = dynamic_cast<oir::ConstantFloat *>(value)) {
            auto reg = create_vreg(mir::ValueType::F32);
            emit(mir::Opcode::LoadFloatImm, {mir::MachineOperand::reg_def(reg),
                                             mir::MachineOperand::float_imm(constant->value())});
            return reg;
        }
        if (dynamic_cast<oir::ConstantZero *>(value) != nullptr ||
            dynamic_cast<oir::UndefValue *>(value) != nullptr) {
            auto type = type_info(value->type()).value_type;
            auto reg = create_vreg(type);
            if (type == mir::ValueType::F32) {
                emit(mir::Opcode::LoadFloatImm,
                     {mir::MachineOperand::reg_def(reg), mir::MachineOperand::float_imm(0.0F)});
            } else {
                emit(mir::Opcode::LoadImm,
                     {mir::MachineOperand::reg_def(reg), mir::MachineOperand::imm(0)});
            }
            return reg;
        }
        if (auto *global = dynamic_cast<oir::GlobalVariable *>(value)) {
            auto reg = create_vreg(mir::ValueType::Ptr);
            emit(mir::Opcode::LoadGlobalAddr,
                 {mir::MachineOperand::reg_def(reg), mir::MachineOperand::global(global->name())});
            return reg;
        }

        auto found = value_regs_.find(value);
        if (found == value_regs_.end()) {
            throw std::runtime_error("MIR lowering cannot find vreg for value: " + value->print());
        }
        return found->second;
    }

    void emit_move(mir::Register dst, mir::Register src) {
        mir::Opcode opcode =
            dst.reg_class == mir::RegisterClass::FPR32 ? mir::Opcode::FMove : mir::Opcode::Move;
        emit(opcode,
             {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(std::move(src))});
    }

    void emit_entry_move(mir::Register dst, mir::Register src,
                         const std::vector<mir::Register> &future_arg_regs) {
        mir::Opcode opcode =
            dst.reg_class == mir::RegisterClass::FPR32 ? mir::Opcode::FMove : mir::Opcode::Move;
        std::vector<mir::MachineOperand> operands;
        operands.push_back(mir::MachineOperand::reg_def(dst));
        operands.push_back(mir::MachineOperand::reg_use(std::move(src)));
        for (const auto &reg : future_arg_regs) {
            auto implicit_use = mir::MachineOperand::reg_use(reg);
            implicit_use.set_is_implicit(true);
            operands.push_back(implicit_use);
        }
        emit(opcode, std::move(operands));
    }

    void emit(mir::Opcode opcode, std::vector<mir::MachineOperand> operands) {
        current_block_->add_instr(opcode, std::move(operands));
    }

    struct PendingEdgeBlock {
        const oir::BasicBlock *pred = nullptr;
        const oir::BasicBlock *target = nullptr;
        mir::MachineBasicBlock *block = nullptr;
    };

    std::unique_ptr<mir::Module> module_;
    mir::MachineFunction *current_function_ = nullptr;
    mir::MachineBasicBlock *current_block_ = nullptr;
    unsigned temp_index_ = 0;
    std::unordered_map<const oir::Function *, mir::MachineFunction *> functions_;
    std::unordered_map<const oir::BasicBlock *, mir::MachineBasicBlock *> blocks_;
    std::unordered_map<std::string, mir::MachineBasicBlock *> edge_blocks_;
    std::vector<PendingEdgeBlock> pending_edge_blocks_;
    std::unordered_map<const oir::Value *, mir::Register> value_regs_;
    std::unordered_map<const oir::Instruction *, int> alloca_slots_;
};

} // namespace

std::unique_ptr<mir::Module> lower_with_vregs(const oir::Module &module) {
    VRegLowerer lowerer;
    return lowerer.lower(module);
}

} // namespace pass::oir_to_mir
