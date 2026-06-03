#include "pass/oir/OIRToMIRCommon.h"

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

class Lowerer final {
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

    void lower_function(const oir::Function &function, mir::MachineFunction &out) {
        current_function_ = &out;
        value_slots_.clear();
        alloca_slots_.clear();
        blocks_.clear();
        edge_blocks_.clear();
        pending_edge_blocks_.clear();
        temp_index_ = 0;

        for (const auto &block : function.blocks()) {
            blocks_[block.get()] = out.create_block(block->name());
        }

        for (const auto &arg : function.args()) {
            value_slots_[arg.get()] =
                out.add_stack_slot(arg->name(), type_info(arg->type()), mir::StackSlotKind::Value);
        }

        for (const auto &block : function.blocks()) {
            for (const auto &inst : block->instructions()) {
                preallocate_result_slot(*inst);
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
        out.layout_frame();
        current_function_ = nullptr;
        current_block_ = nullptr;
    }

    void preallocate_result_slot(const oir::Instruction &inst) {
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
        value_slots_[&inst] = current_function_->add_stack_slot(
            slot_name(inst, "v"), type_info(inst.type()), mir::StackSlotKind::Value);
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
            int slot = value_slots_.at(arg.get());
            if (type == mir::ValueType::F32) {
                if (float_reg < kFArgRegs.size()) {
                    emit(mir::Opcode::StoreSlot, {mir::MachineOperand::slot(slot),
                                                  mir::MachineOperand::freg(kFArgRegs[float_reg++]),
                                                  mir::MachineOperand::type(type)});
                } else {
                    emit(mir::Opcode::LoadIncomingArg,
                         {mir::MachineOperand::freg("ft0"), mir::MachineOperand::imm(stack_offset),
                          mir::MachineOperand::type(type)});
                    emit(mir::Opcode::StoreSlot,
                         {mir::MachineOperand::slot(slot), mir::MachineOperand::freg("ft0"),
                          mir::MachineOperand::type(type)});
                    stack_offset += 8;
                }
            } else {
                if (int_reg < kArgRegs.size()) {
                    emit(mir::Opcode::StoreSlot, {mir::MachineOperand::slot(slot),
                                                  mir::MachineOperand::reg(kArgRegs[int_reg++]),
                                                  mir::MachineOperand::type(type)});
                } else {
                    emit(mir::Opcode::LoadIncomingArg,
                         {mir::MachineOperand::reg("t0"), mir::MachineOperand::imm(stack_offset),
                          mir::MachineOperand::type(type)});
                    emit(mir::Opcode::StoreSlot,
                         {mir::MachineOperand::slot(slot), mir::MachineOperand::reg("t0"),
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
        int pointer_slot = value_slots_.at(&inst);
        int object_slot = alloca_slots_.at(&inst);
        emit(mir::Opcode::LoadStackAddr,
             {mir::MachineOperand::reg("t0"), mir::MachineOperand::slot(object_slot)});
        emit(mir::Opcode::StoreSlot,
             {mir::MachineOperand::slot(pointer_slot), mir::MachineOperand::reg("t0"),
              mir::MachineOperand::type(mir::ValueType::Ptr)});
    }

    void lower_load(const oir::LoadInst &inst) {
        auto loaded = type_info(inst.type());
        int result_slot = value_slots_.at(&inst);
        load_int_value(inst.ptr(), "t0");
        if (loaded.value_type == mir::ValueType::F32) {
            emit(mir::Opcode::LoadMem,
                 {mir::MachineOperand::freg("ft0"), mir::MachineOperand::reg("t0"),
                  mir::MachineOperand::type(loaded.value_type)});
            emit(mir::Opcode::StoreSlot,
                 {mir::MachineOperand::slot(result_slot), mir::MachineOperand::freg("ft0"),
                  mir::MachineOperand::type(loaded.value_type)});
        } else {
            emit(mir::Opcode::LoadMem,
                 {mir::MachineOperand::reg("t1"), mir::MachineOperand::reg("t0"),
                  mir::MachineOperand::type(loaded.value_type)});
            emit(mir::Opcode::StoreSlot,
                 {mir::MachineOperand::slot(result_slot), mir::MachineOperand::reg("t1"),
                  mir::MachineOperand::type(loaded.value_type)});
        }
    }

    void lower_store(const oir::StoreInst &inst) {
        auto stored = type_info(inst.value()->type());
        load_int_value(inst.ptr(), "t0");
        if (stored.value_type == mir::ValueType::Aggregate) {
            if (dynamic_cast<const oir::ConstantZero *>(inst.value()) == nullptr) {
                throw std::runtime_error("only zero aggregate stores are supported in MIR v1");
            }
            emit(mir::Opcode::MemZero,
                 {mir::MachineOperand::reg("t0"),
                  mir::MachineOperand::imm(static_cast<std::int64_t>(stored.size))});
            return;
        }

        if (stored.value_type == mir::ValueType::F32) {
            load_float_value(inst.value(), "ft0");
            emit(mir::Opcode::StoreMem,
                 {mir::MachineOperand::reg("t0"), mir::MachineOperand::freg("ft0"),
                  mir::MachineOperand::type(stored.value_type)});
        } else {
            load_int_value(inst.value(), "t1");
            emit(mir::Opcode::StoreMem,
                 {mir::MachineOperand::reg("t0"), mir::MachineOperand::reg("t1"),
                  mir::MachineOperand::type(stored.value_type)});
        }
    }

    void lower_gep(const oir::GetElementPtrInst &inst) {
        int result_slot = value_slots_.at(&inst);
        load_int_value(inst.base_ptr(), "t0");

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

            load_int_value(indices[i], "t1");
            if (stride == 1) {
                emit(mir::Opcode::Add,
                     {mir::MachineOperand::reg("t0"), mir::MachineOperand::reg("t0"),
                      mir::MachineOperand::reg("t1")});
            } else if (is_power_of_two(stride)) {
                emit(mir::Opcode::SllI,
                     {mir::MachineOperand::reg("t1"), mir::MachineOperand::reg("t1"),
                      mir::MachineOperand::imm(log2_u64(stride))});
                emit(mir::Opcode::Add,
                     {mir::MachineOperand::reg("t0"), mir::MachineOperand::reg("t0"),
                      mir::MachineOperand::reg("t1")});
            } else {
                emit(mir::Opcode::LoadImm,
                     {mir::MachineOperand::reg("t2"),
                      mir::MachineOperand::imm(static_cast<std::int64_t>(stride))});
                emit(mir::Opcode::Mul,
                     {mir::MachineOperand::reg("t1"), mir::MachineOperand::reg("t1"),
                      mir::MachineOperand::reg("t2")});
                emit(mir::Opcode::Add,
                     {mir::MachineOperand::reg("t0"), mir::MachineOperand::reg("t0"),
                      mir::MachineOperand::reg("t1")});
            }
        }

        if (constant_offset != 0) {
            emit(mir::Opcode::LoadImm,
                 {mir::MachineOperand::reg("t1"), mir::MachineOperand::imm(constant_offset)});
            emit(mir::Opcode::Add, {mir::MachineOperand::reg("t0"), mir::MachineOperand::reg("t0"),
                                    mir::MachineOperand::reg("t1")});
        }

        emit(mir::Opcode::StoreSlot,
             {mir::MachineOperand::slot(result_slot), mir::MachineOperand::reg("t0"),
              mir::MachineOperand::type(mir::ValueType::Ptr)});
    }

    void lower_int_binary(const oir::BinaryInst &inst) {
        load_int_value(inst.lhs(), "t0");
        load_int_value(inst.rhs(), "t1");
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
        emit(opcode, {mir::MachineOperand::reg("t2"), mir::MachineOperand::reg("t0"),
                      mir::MachineOperand::reg("t1")});
        store_reg_to_value_slot(&inst, "t2");
    }

    void lower_float_binary(const oir::BinaryInst &inst) {
        load_float_value(inst.lhs(), "ft0");
        load_float_value(inst.rhs(), "ft1");
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
        emit(opcode, {mir::MachineOperand::freg("ft2"), mir::MachineOperand::freg("ft0"),
                      mir::MachineOperand::freg("ft1")});
        store_freg_to_value_slot(&inst, "ft2");
    }

    void lower_icmp(const oir::CmpInst &inst) {
        load_int_value(inst.lhs(), "t0");
        load_int_value(inst.rhs(), "t1");
        switch (inst.pred()) {
        case oir::CmpPred::EQ:
            emit(mir::Opcode::Xor, {mir::MachineOperand::reg("t2"), mir::MachineOperand::reg("t0"),
                                    mir::MachineOperand::reg("t1")});
            emit(mir::Opcode::SeqZ,
                 {mir::MachineOperand::reg("t2"), mir::MachineOperand::reg("t2")});
            break;
        case oir::CmpPred::NE:
            emit(mir::Opcode::Xor, {mir::MachineOperand::reg("t2"), mir::MachineOperand::reg("t0"),
                                    mir::MachineOperand::reg("t1")});
            emit(mir::Opcode::Snez,
                 {mir::MachineOperand::reg("t2"), mir::MachineOperand::reg("t2")});
            break;
        case oir::CmpPred::LT:
            emit(mir::Opcode::Slt, {mir::MachineOperand::reg("t2"), mir::MachineOperand::reg("t0"),
                                    mir::MachineOperand::reg("t1")});
            break;
        case oir::CmpPred::LE:
            emit(mir::Opcode::Slt, {mir::MachineOperand::reg("t2"), mir::MachineOperand::reg("t1"),
                                    mir::MachineOperand::reg("t0")});
            emit(mir::Opcode::XorI, {mir::MachineOperand::reg("t2"), mir::MachineOperand::reg("t2"),
                                     mir::MachineOperand::imm(1)});
            break;
        case oir::CmpPred::GT:
            emit(mir::Opcode::Slt, {mir::MachineOperand::reg("t2"), mir::MachineOperand::reg("t1"),
                                    mir::MachineOperand::reg("t0")});
            break;
        case oir::CmpPred::GE:
            emit(mir::Opcode::Slt, {mir::MachineOperand::reg("t2"), mir::MachineOperand::reg("t0"),
                                    mir::MachineOperand::reg("t1")});
            emit(mir::Opcode::XorI, {mir::MachineOperand::reg("t2"), mir::MachineOperand::reg("t2"),
                                     mir::MachineOperand::imm(1)});
            break;
        }
        store_reg_to_value_slot(&inst, "t2");
    }

    void lower_fcmp(const oir::CmpInst &inst) {
        load_float_value(inst.lhs(), "ft0");
        load_float_value(inst.rhs(), "ft1");
        switch (inst.pred()) {
        case oir::CmpPred::EQ:
            emit(mir::Opcode::FeqS,
                 {mir::MachineOperand::reg("t2"), mir::MachineOperand::freg("ft0"),
                  mir::MachineOperand::freg("ft1")});
            break;
        case oir::CmpPred::NE:
            emit(mir::Opcode::FeqS,
                 {mir::MachineOperand::reg("t2"), mir::MachineOperand::freg("ft0"),
                  mir::MachineOperand::freg("ft1")});
            emit(mir::Opcode::XorI, {mir::MachineOperand::reg("t2"), mir::MachineOperand::reg("t2"),
                                     mir::MachineOperand::imm(1)});
            break;
        case oir::CmpPred::LT:
            emit(mir::Opcode::FltS,
                 {mir::MachineOperand::reg("t2"), mir::MachineOperand::freg("ft0"),
                  mir::MachineOperand::freg("ft1")});
            break;
        case oir::CmpPred::LE:
            emit(mir::Opcode::FleS,
                 {mir::MachineOperand::reg("t2"), mir::MachineOperand::freg("ft0"),
                  mir::MachineOperand::freg("ft1")});
            break;
        case oir::CmpPred::GT:
            emit(mir::Opcode::FltS,
                 {mir::MachineOperand::reg("t2"), mir::MachineOperand::freg("ft1"),
                  mir::MachineOperand::freg("ft0")});
            break;
        case oir::CmpPred::GE:
            emit(mir::Opcode::FleS,
                 {mir::MachineOperand::reg("t2"), mir::MachineOperand::freg("ft1"),
                  mir::MachineOperand::freg("ft0")});
            break;
        }
        store_reg_to_value_slot(&inst, "t2");
    }

    void lower_zext(const oir::CastInst &inst) {
        load_int_value(inst.src(), "t0");
        store_reg_to_value_slot(&inst, "t0");
    }

    void lower_sitofp(const oir::CastInst &inst) {
        load_int_value(inst.src(), "t0");
        emit(mir::Opcode::FcvtSW,
             {mir::MachineOperand::freg("ft0"), mir::MachineOperand::reg("t0")});
        store_freg_to_value_slot(&inst, "ft0");
    }

    void lower_fptosi(const oir::CastInst &inst) {
        load_float_value(inst.src(), "ft0");
        emit(mir::Opcode::FcvtWS,
             {mir::MachineOperand::reg("t0"), mir::MachineOperand::freg("ft0")});
        store_reg_to_value_slot(&inst, "t0");
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
                 {mir::MachineOperand::reg("a0"), mir::MachineOperand::imm(0)});
            symbol = symbol == "starttime" ? "_sysy_starttime" : "_sysy_stoptime";
            int_reg = 1;
        }

        auto args = inst.args();
        for (auto *arg : args) {
            auto type = type_info(arg->type()).value_type;
            if (type == mir::ValueType::F32) {
                if (float_reg < kFArgRegs.size()) {
                    load_float_value(arg, "ft0");
                    emit(mir::Opcode::FMove, {mir::MachineOperand::freg(kFArgRegs[float_reg++]),
                                              mir::MachineOperand::freg("ft0")});
                } else {
                    load_float_value(arg, "ft0");
                    emit(mir::Opcode::StoreOutgoingArg,
                         {mir::MachineOperand::freg("ft0"), mir::MachineOperand::imm(stack_offset),
                          mir::MachineOperand::type(type)});
                    stack_offset += 8;
                }
            } else {
                if (int_reg < kArgRegs.size()) {
                    load_int_value(arg, "t0");
                    emit(mir::Opcode::Move, {mir::MachineOperand::reg(kArgRegs[int_reg++]),
                                             mir::MachineOperand::reg("t0")});
                } else {
                    load_int_value(arg, "t0");
                    emit(mir::Opcode::StoreOutgoingArg,
                         {mir::MachineOperand::reg("t0"), mir::MachineOperand::imm(stack_offset),
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
                emit(mir::Opcode::StoreSlot,
                     {mir::MachineOperand::slot(value_slots_.at(&inst)),
                      mir::MachineOperand::freg("fa0"), mir::MachineOperand::type(result_type)});
            } else {
                emit(mir::Opcode::StoreSlot,
                     {mir::MachineOperand::slot(value_slots_.at(&inst)),
                      mir::MachineOperand::reg("a0"), mir::MachineOperand::type(result_type)});
            }
        }
    }

    void lower_return(const oir::ReturnInst &inst) {
        if (inst.has_value()) {
            auto type = type_info(inst.value()->type()).value_type;
            if (type == mir::ValueType::F32) {
                load_float_value(inst.value(), "fa0");
            } else {
                load_int_value(inst.value(), "a0");
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

        load_int_value(inst.cond(), "t0");
        emit(mir::Opcode::BranchNonZero,
             {mir::MachineOperand::reg("t0"),
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
            std::vector<int> temp_slots;
            std::vector<int> phi_slots;
            for (const auto &inst : edge.target->instructions()) {
                auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
                if (phi == nullptr) {
                    break;
                }

                auto *incoming = incoming_for(*phi, edge.pred);
                auto type = type_info(phi->type());
                int temp = current_function_->add_stack_slot(
                    "phi.tmp." + std::to_string(temp_index_++), type, mir::StackSlotKind::PhiTemp);
                store_value_to_slot(incoming, temp);
                temp_slots.push_back(temp);
                phi_slots.push_back(value_slots_.at(phi));
            }

            for (std::size_t i = 0; i < temp_slots.size(); ++i) {
                auto type = current_function_->stack_slot(temp_slots[i])->type.value_type;
                if (type == mir::ValueType::F32) {
                    emit(mir::Opcode::LoadSlot, {mir::MachineOperand::freg("ft0"),
                                                 mir::MachineOperand::slot(temp_slots[i]),
                                                 mir::MachineOperand::type(type)});
                    emit(mir::Opcode::StoreSlot,
                         {mir::MachineOperand::slot(phi_slots[i]), mir::MachineOperand::freg("ft0"),
                          mir::MachineOperand::type(type)});
                } else {
                    emit(mir::Opcode::LoadSlot,
                         {mir::MachineOperand::reg("t0"), mir::MachineOperand::slot(temp_slots[i]),
                          mir::MachineOperand::type(type)});
                    emit(mir::Opcode::StoreSlot,
                         {mir::MachineOperand::slot(phi_slots[i]), mir::MachineOperand::reg("t0"),
                          mir::MachineOperand::type(type)});
                }
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

    void load_int_value(oir::Value *value, const std::string &reg) {
        if (auto *constant = dynamic_cast<oir::ConstantInt *>(value)) {
            emit(mir::Opcode::LoadImm,
                 {mir::MachineOperand::reg(reg), mir::MachineOperand::imm(constant->value())});
            return;
        }
        if (dynamic_cast<oir::ConstantZero *>(value) != nullptr ||
            dynamic_cast<oir::UndefValue *>(value) != nullptr) {
            emit(mir::Opcode::LoadImm,
                 {mir::MachineOperand::reg(reg), mir::MachineOperand::imm(0)});
            return;
        }
        if (auto *global = dynamic_cast<oir::GlobalVariable *>(value)) {
            emit(mir::Opcode::LoadGlobalAddr,
                 {mir::MachineOperand::reg(reg), mir::MachineOperand::global(global->name())});
            return;
        }

        auto found = value_slots_.find(value);
        if (found == value_slots_.end()) {
            throw std::runtime_error("MIR lowering cannot find slot for value: " + value->print());
        }
        emit(mir::Opcode::LoadSlot,
             {mir::MachineOperand::reg(reg), mir::MachineOperand::slot(found->second),
              mir::MachineOperand::type(type_info(value->type()).value_type)});
    }

    void load_float_value(oir::Value *value, const std::string &reg) {
        if (auto *constant = dynamic_cast<oir::ConstantFloat *>(value)) {
            emit(mir::Opcode::LoadFloatImm, {mir::MachineOperand::freg(reg),
                                             mir::MachineOperand::float_imm(constant->value())});
            return;
        }
        if (dynamic_cast<oir::ConstantZero *>(value) != nullptr ||
            dynamic_cast<oir::UndefValue *>(value) != nullptr) {
            emit(mir::Opcode::LoadFloatImm,
                 {mir::MachineOperand::freg(reg), mir::MachineOperand::float_imm(0.0F)});
            return;
        }

        auto found = value_slots_.find(value);
        if (found == value_slots_.end()) {
            throw std::runtime_error("MIR lowering cannot find slot for float value: " +
                                     value->print());
        }
        emit(mir::Opcode::LoadSlot,
             {mir::MachineOperand::freg(reg), mir::MachineOperand::slot(found->second),
              mir::MachineOperand::type(mir::ValueType::F32)});
    }

    void store_value_to_slot(oir::Value *value, int slot) {
        auto type = type_info(value->type()).value_type;
        if (type == mir::ValueType::F32) {
            load_float_value(value, "ft0");
            emit(mir::Opcode::StoreSlot,
                 {mir::MachineOperand::slot(slot), mir::MachineOperand::freg("ft0"),
                  mir::MachineOperand::type(type)});
        } else {
            load_int_value(value, "t0");
            emit(mir::Opcode::StoreSlot,
                 {mir::MachineOperand::slot(slot), mir::MachineOperand::reg("t0"),
                  mir::MachineOperand::type(type)});
        }
    }

    void store_reg_to_value_slot(const oir::Value *value, const std::string &reg) {
        auto type = type_info(value->type()).value_type;
        emit(mir::Opcode::StoreSlot,
             {mir::MachineOperand::slot(value_slots_.at(value)), mir::MachineOperand::reg(reg),
              mir::MachineOperand::type(type)});
    }

    void store_freg_to_value_slot(const oir::Value *value, const std::string &reg) {
        emit(mir::Opcode::StoreSlot,
             {mir::MachineOperand::slot(value_slots_.at(value)), mir::MachineOperand::freg(reg),
              mir::MachineOperand::type(mir::ValueType::F32)});
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
    std::unordered_map<const oir::Value *, int> value_slots_;
    std::unordered_map<const oir::Instruction *, int> alloca_slots_;
};

} // namespace

std::unique_ptr<mir::Module> lower_with_stack_slots(const oir::Module &module) {
    Lowerer lowerer;
    return lowerer.lower(module);
}

} // namespace pass::oir_to_mir
