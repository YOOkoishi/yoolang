#include "mir/MIRVerifier.h"
#include "mir/MachineInstrDesc.h"
#include "mir/MIRVectorState.h"
#include "builtin/BuiltinRegistry.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace mir {
namespace {

bool is_post_ra(MIRVerificationStage stage) {
    return stage == MIRVerificationStage::PostRA || stage == MIRVerificationStage::Final;
}

std::optional<unsigned> vector_register_index(const std::string &name) {
    if (name.size() < 2 || name[0] != 'v' ||
        !std::all_of(name.begin() + 1, name.end(),
                     [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); })) {
        return std::nullopt;
    }
    try {
        const auto index = std::stoul(name.substr(1));
        if (index > 31) {
            return std::nullopt;
        }
        return static_cast<unsigned>(index);
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

bool is_standard_runtime_symbol(const std::string &name) {
    const auto *descriptor = builtin::BuiltinRegistry::instance().find(name);
    return descriptor != nullptr &&
           descriptor->lowering == builtin::LoweringKind::RuntimeCall;
}

std::optional<MachineVectorState> vector_state_for_name(const std::string &name) {
    if (name == "vl") {
        return MVS_VL;
    }
    if (name == "vtype") {
        return MVS_VTYPE;
    }
    if (name == "vlenb") {
        return MVS_VLENB;
    }
    if (name == "vxrm") {
        return MVS_VXRM;
    }
    if (name == "vxsat") {
        return MVS_VXSAT;
    }
    if (name == "vstart") {
        return MVS_VSTART;
    }
    if (name == "frm") {
        return MVS_FRM;
    }
    return std::nullopt;
}

const char *vector_state_name(MachineVectorState state) {
    switch (state) {
    case MVS_VL:
        return "vl";
    case MVS_VTYPE:
        return "vtype";
    case MVS_VLENB:
        return "vlenb";
    case MVS_VXRM:
        return "vxrm";
    case MVS_VXSAT:
        return "vxsat";
    case MVS_VSTART:
        return "vstart";
    case MVS_FRM:
        return "frm";
    case MVS_None:
        break;
    }
    return "unknown";
}

bool is_integer_binary_operation(RVVOperation operation) {
    switch (operation) {
    case RVVOperation::Add:
    case RVVOperation::Sub:
    case RVVOperation::Mul:
    case RVVOperation::Div:
    case RVVOperation::Rem:
    case RVVOperation::And:
    case RVVOperation::Or:
    case RVVOperation::Xor:
    case RVVOperation::Min:
    case RVVOperation::Max:
        return true;
    default:
        return false;
    }
}

bool is_float_binary_operation(RVVOperation operation) {
    switch (operation) {
    case RVVOperation::Add:
    case RVVOperation::Sub:
    case RVVOperation::Mul:
    case RVVOperation::Div:
    case RVVOperation::Min:
    case RVVOperation::Max:
        return true;
    default:
        return false;
    }
}

bool is_compare_operation(RVVOperation operation) {
    switch (operation) {
    case RVVOperation::Eq:
    case RVVOperation::Ne:
    case RVVOperation::Lt:
    case RVVOperation::Le:
        return true;
    default:
        return false;
    }
}

bool is_reduction_operation(RVVOperation operation) {
    return operation == RVVOperation::ReduceSum || operation == RVVOperation::ReduceMin ||
           operation == RVVOperation::ReduceMax || operation == RVVOperation::ReduceAnd ||
           operation == RVVOperation::ReduceOr || operation == RVVOperation::ReduceXor;
}

bool is_mask_logical_operation(RVVOperation operation) {
    return operation == RVVOperation::MaskAnd || operation == RVVOperation::MaskOr ||
           operation == RVVOperation::MaskXor || operation == RVVOperation::MaskNot;
}

bool is_segment2_opcode(Opcode opcode) {
    return opcode == Opcode::RVVLoadSegment2 ||
           opcode == Opcode::RVVStoreSegment2 ||
           opcode == Opcode::RISCVVLoadSegment2 ||
           opcode == Opcode::RISCVVStoreSegment2;
}

bool opcode_has_vector_destination(Opcode opcode) {
    switch (opcode) {
    case Opcode::RVVSplatVX:
    case Opcode::RVVSplatVI:
    case Opcode::RVVSplatVF:
    case Opcode::RVVStep:
    case Opcode::RVVIntBinaryVV:
    case Opcode::RVVIntBinaryVX:
    case Opcode::RVVIntBinaryVI:
    case Opcode::RVVFloatBinaryVV:
    case Opcode::RVVFloatBinaryVF:
    case Opcode::RVVCompareVV:
    case Opcode::RVVCompareVX:
    case Opcode::RVVCompareVI:
    case Opcode::RVVCompareVF:
    case Opcode::RVVLoadUnit:
    case Opcode::RVVLoadStrided:
    case Opcode::RVVLoadIndexed:
    case Opcode::RVVInsertElement:
    case Opcode::RVVSlideUpVX:
    case Opcode::RVVSlideUpVI:
    case Opcode::RVVSlideDownVX:
    case Opcode::RVVSlideDownVI:
    case Opcode::RVVGatherVV:
    case Opcode::RVVGatherVX:
    case Opcode::RVVGatherVI:
        return true;
    default:
        return false;
    }
}

std::optional<std::size_t> index_vector_operand_ordinal(Opcode opcode) {
    switch (opcode) {
    case Opcode::RVVLoadIndexed:
    case Opcode::RVVGatherVV:
        return 3;
    case Opcode::RISCVVLoadIndexedOrdered:
        return 2;
    case Opcode::RVVStoreIndexed:
    case Opcode::RISCVVStoreIndexedOrdered:
        return 2;
    default:
        return std::nullopt;
    }
}

bool ranges_overlap(unsigned lhs_begin, unsigned lhs_width, unsigned rhs_begin,
                    unsigned rhs_width) {
    return lhs_begin < rhs_begin + rhs_width && rhs_begin < lhs_begin + lhs_width;
}

class Verifier {
  public:
    MIRVerifyResult verify(const Module &module, MIRVerificationStage stage) {
        module_ = &module;
        stage_ = stage;
        verify_module_abi_contract(module);
        for (const auto &function : module.functions()) {
            if (!function->is_external()) {
                verify_function(*function);
            }
        }
        return {true, ""};
    }

  private:
    struct PhysicalVectorOperand {
        std::size_t operand_index = 0;
        unsigned base = 0;
        unsigned width = 1;
        bool is_def = false;
        bool is_use = false;
        bool is_mask = false;
    };

    [[noreturn]] void fail(const std::string &message) const {
        throw std::runtime_error(message);
    }

    void verify_module_abi_contract(const Module &module) const {
        const auto &target = module.target();
        if (target.psabi_vector) {
            if (!target.has_vector || target.abi_vlen_bits == 0 ||
                target.abi_vlen_bits != target.minimum_vlen_bits) {
                fail("MIR verifier: psabi-vector target requires a nonzero ABI_VLEN "
                     "equal to minimum VLEN");
            }
        } else if (target.abi_vlen_bits != 0) {
            fail("MIR verifier: standard ABI target cannot carry vector ABI_VLEN metadata");
        }
        for (const auto &function : module.functions()) {
            const bool expected_variant =
                target.psabi_vector &&
                !is_standard_runtime_symbol(function->name());
            if (function->is_variant_cc() != expected_variant) {
                fail("MIR verifier @" + function->name() +
                     " variant-CC metadata disagrees with the module vector ABI");
            }
        }
    }

    void verify_function(const MachineFunction &function) const {
        verify_stack_slots(function);
        verify_saved_vector_registers(function);
        bool contains_call = false;
        for (const auto &block : function.blocks()) {
            contains_call = contains_call ||
                            std::any_of(block->instructions().begin(),
                                        block->instructions().end(),
                                        [](const MachineInstr &instr) {
                                            return machine_instr_may_call(instr);
                                        });
            verify_block(function, *block);
        }
        if (contains_call != function.has_call()) {
            fail("MIR verifier @" + function.name() +
                 (contains_call ? " contains a call but has_call is false"
                                : " has_call is true but contains no call"));
        }
        const auto vector_state = analyze_mir_vector_state(function, true);
        if (!vector_state.ok) {
            fail(vector_state.message);
        }
    }

    void verify_saved_vector_registers(const MachineFunction &function) const {
        std::vector<unsigned> saved_indices;
        for (const auto &saved : function.saved_vector_registers()) {
            const auto index = vector_register_index(saved.reg.name);
            if (!function.is_variant_cc() || !saved.reg.is_physical() ||
                !saved.reg.is_vector() || !index.has_value() ||
                !((*index >= 1U && *index <= 7U) ||
                  (*index >= 24U && *index <= 31U))) {
                fail("MIR verifier @" + function.name() +
                     " has an invalid vector callee-save register");
            }
            if (std::find(saved_indices.begin(), saved_indices.end(), *index) !=
                saved_indices.end()) {
                fail("MIR verifier @" + function.name() +
                     " saves a vector register more than once");
            }
            saved_indices.push_back(*index);
            const auto *slot = function.stack_slot(saved.stack_slot);
            if (slot == nullptr || slot->kind != StackSlotKind::CalleeSaved ||
                !slot->scalable_size.has_value() ||
                *slot->scalable_size !=
                    MachineScalableSize::from_register_group_width(1)) {
                fail("MIR verifier @" + function.name() +
                     " vector callee-save lacks one whole-register stack slot");
            }
        }

        if (!is_post_ra(stage_) || !function.is_variant_cc()) {
            return;
        }
        for (const auto &block : function.blocks()) {
            for (const auto &instr : block->instructions()) {
                if (instr.is_variant_cc_call() && instr.opcode() != Opcode::Call) {
                    fail("MIR verifier @" + function.name() +
                         " marks a non-call instruction as variant CC");
                }
                for (const auto &operand : instr.operands()) {
                    if (!operand.is_reg() || !operand.reg_value().is_physical() ||
                        !operand.reg_value().is_vector()) {
                        continue;
                    }
                    const auto base =
                        vector_register_index(operand.reg_value().name);
                    if (!base.has_value()) {
                        continue;
                    }
                    for (unsigned offset = 0;
                         offset < operand.reg_value().vector_group_width &&
                         *base + offset <= 31U;
                         ++offset) {
                        const auto index = *base + offset;
                        const bool callee_saved =
                            (index >= 1U && index <= 7U) ||
                            (index >= 24U && index <= 31U);
                        if (callee_saved &&
                            std::find(saved_indices.begin(), saved_indices.end(), index) ==
                                saved_indices.end()) {
                            fail("MIR verifier @" + function.name() +
                                 " uses an unsaved variant-CC vector register v" +
                                 std::to_string(index));
                        }
                    }
                }
            }
        }
    }

    void verify_stack_slots(const MachineFunction &function) const {
        unsigned expected_scalable_offset = 0;
        for (const auto &slot : function.stack_slots()) {
            const bool is_scalable = slot.scalable_size.has_value();
            if (!is_scalable) {
                if (!slot.has_fixed_offset || slot.scalable_align.has_value() ||
                    slot.scalable_offset.has_value()) {
                    fail("MIR verifier @" + function.name() + " fi#" +
                         std::to_string(slot.id) + " has inconsistent fixed stack metadata");
                }
                continue;
            }

            if (!module_->target().has_vector) {
                fail("MIR verifier @" + function.name() + " fi#" +
                     std::to_string(slot.id) + " uses a scalable slot on a non-vector target");
            }
            if (slot.has_fixed_offset) {
                fail("MIR verifier @" + function.name() + " fi#" +
                     std::to_string(slot.id) + " gives a scalable slot a fixed offset");
            }
            if (!slot.type.vector_type.has_value()) {
                fail("MIR verifier @" + function.name() + " fi#" +
                     std::to_string(slot.id) + " lacks a machine vector type");
            }
            if (!slot.scalable_align.has_value() || !slot.scalable_offset.has_value()) {
                fail("MIR verifier @" + function.name() + " fi#" +
                     std::to_string(slot.id) + " lacks scalable alignment/offset metadata");
            }
            const auto expected_size = MachineScalableSize::from_register_group_width(
                slot.type.vector_type->register_group_width());
            if (*slot.scalable_size != expected_size || *slot.scalable_align != expected_size) {
                fail("MIR verifier @" + function.name() + " fi#" +
                     std::to_string(slot.id) +
                     " scalable size/alignment does not match the physical register group");
            }
            expected_scalable_offset = static_cast<unsigned>(
                align_to(expected_scalable_offset, slot.scalable_align->vlenb_eighths));
            if (slot.scalable_offset->vlenb_eighths != expected_scalable_offset) {
                fail("MIR verifier @" + function.name() + " fi#" +
                     std::to_string(slot.id) + " has an invalid scalable frame offset");
            }
            expected_scalable_offset += slot.scalable_size->vlenb_eighths;
        }
        if (function.scalable_frame_size().vlenb_eighths != expected_scalable_offset) {
            fail("MIR verifier @" + function.name() + " has inconsistent scalable frame size");
        }
    }

    void verify_block(const MachineFunction &function, const MachineBasicBlock &block) const {
        for (const auto &instr : block.instructions()) {
            verify_instr(function, block, instr);
        }
    }

    void verify_instr(const MachineFunction &function, const MachineBasicBlock &block,
                      const MachineInstr &instr) const {
        const auto &ops = instr.operands();
        const auto &desc = instruction_desc(instr.opcode());
        require(desc.accepts_operand_count(ops.size()), function, block, instr,
                "operand count does not match machine instruction descriptor");

        std::vector<std::size_t> explicit_indices;
        bool saw_implicit = false;
        for (std::size_t index = 0; index < ops.size(); ++index) {
            const auto &operand = ops[index];
            if (operand.is_implicit()) {
                saw_implicit = true;
            } else {
                require(!saw_implicit || !desc.has_flag(MIF_Vector), function, block, instr,
                        "RVV explicit operands must precede implicit state operands");
                explicit_indices.push_back(index);
            }
            if (operand.is_reg()) {
                verify_reg(function, block, instr, operand.reg_value());
            }
            if (operand.kind() == OperandKind::Slot) {
                const auto *slot = function.stack_slot(operand.slot_id());
                if (slot == nullptr) {
                    fail(where(function, block, instr) + " references missing fi#" +
                         std::to_string(operand.slot_id()));
                }
                if (slot->scalable_size.has_value() && !desc.has_flag(MIF_WholeRegister)) {
                    fail(where(function, block, instr) +
                         ": scalable stack slot requires a dedicated scalable spill/reload "
                         "pseudo");
                }
            }
            if (operand.kind() == OperandKind::Block &&
                function.get_block(operand.string_value()) == nullptr &&
                operand.string_value() != "epilogue") {
                fail(where(function, block, instr) + " references missing block %" +
                     operand.string_value());
            }
        }

        require(desc.accepts_explicit_operand_count(explicit_indices.size()), function, block,
                instr, "explicit operand count does not match descriptor");
        verify_operand_constraints(function, block, instr, explicit_indices, desc);
        verify_implicit_state(function, block, instr, desc);

        if (desc.has_flag(MIF_Vector)) {
            verify_vector_instr(function, block, instr, explicit_indices, desc);
        } else {
            require(!instr.has_vector_info(), function, block, instr,
                    "non-vector instruction carries RVV metadata");
            verify_scalar_instr(function, block, instr);
        }

        if (stage_ == MIRVerificationStage::Final && desc.has_flag(MIF_Pseudo)) {
            fail(where(function, block, instr) + ": pseudo instruction remains at Final stage");
        }
    }

    void verify_operand_constraints(const MachineFunction &function,
                                    const MachineBasicBlock &block,
                                    const MachineInstr &instr,
                                    const std::vector<std::size_t> &explicit_indices,
                                    const MachineInstrDesc &desc) const {
        if (desc.operand_constraints == nullptr) {
            return;
        }
        require(explicit_indices.size() <= desc.operand_constraint_count, function, block, instr,
                "descriptor lacks an explicit operand constraint");
        const auto &ops = instr.operands();
        for (std::size_t ordinal = 0; ordinal < explicit_indices.size(); ++ordinal) {
            const auto operand_index = explicit_indices[ordinal];
            const auto &op = ops[operand_index];
            const auto &constraint = desc.operand_constraints[ordinal];

            bool kind_ok = false;
            switch (constraint.kind) {
            case MachineOperandConstraintKind::Any:
                kind_ok = true;
                break;
            case MachineOperandConstraintKind::Register:
                kind_ok = op.is_reg();
                break;
            case MachineOperandConstraintKind::Immediate:
                kind_ok = op.kind() == OperandKind::Imm;
                break;
            case MachineOperandConstraintKind::RegisterOrImmediate:
                kind_ok = op.is_reg() || op.kind() == OperandKind::Imm;
                break;
            case MachineOperandConstraintKind::StackSlot:
                kind_ok = op.kind() == OperandKind::Slot;
                break;
            }
            require(kind_ok, function, block, instr,
                    "operand #" + std::to_string(ordinal) + " has the wrong kind");

            if (!op.is_reg()) {
                continue;
            }
            if (constraint.role == MachineOperandRole::Def ||
                constraint.role == MachineOperandRole::DefUse) {
                require(op.is_def(), function, block, instr,
                        "operand #" + std::to_string(ordinal) + " must define a register");
            }
            if (constraint.role == MachineOperandRole::Use ||
                constraint.role == MachineOperandRole::DefUse) {
                require(op.is_use(), function, block, instr,
                        "operand #" + std::to_string(ordinal) + " must use a register");
            }
            if (constraint.register_classes != MRC_None) {
                require((constraint.register_classes & register_class_mask(op.reg_value().reg_class)) !=
                            0,
                        function, block, instr,
                        "operand #" + std::to_string(ordinal) + " has the wrong register class");
            }
            if (constraint.carries_vector_group) {
                require(op.reg_value().vector_type.has_value(), function, block, instr,
                        "vector operand #" + std::to_string(ordinal) +
                            " lacks machine vector type metadata");
                require(op.reg_value().vector_group_width ==
                            op.reg_value().vector_type->register_group_width(),
                        function, block, instr,
                        "vector operand #" + std::to_string(ordinal) +
                            " has an invalid register group width");
            }

            if (constraint.tied_to < 0) {
                continue;
            }
            const auto tied_ordinal = static_cast<std::size_t>(constraint.tied_to);
            require(tied_ordinal < explicit_indices.size(), function, block, instr,
                    "tied operand is missing");
            const auto &tied = ops[explicit_indices[tied_ordinal]];
            require(tied.is_reg(), function, block, instr, "tied operand is not a register");
            require(op.reg_value().reg_class == tied.reg_value().reg_class &&
                        op.reg_value().vector_type == tied.reg_value().vector_type &&
                        op.reg_value().vector_group_width == tied.reg_value().vector_group_width,
                    function, block, instr, "tied operands have incompatible register views");
            if (is_post_ra(stage_)) {
                require(op.reg_value() == tied.reg_value(), function, block, instr,
                        "tied operands do not use the same physical register");
            }
        }
    }

    void verify_implicit_state(const MachineFunction &function, const MachineBasicBlock &block,
                               const MachineInstr &instr,
                               const MachineInstrDesc &desc) const {
        for (auto state : {MVS_VL, MVS_VTYPE, MVS_VLENB, MVS_VXRM,
                           MVS_VXSAT, MVS_VSTART, MVS_FRM}) {
            unsigned use_count = 0;
            unsigned def_count = 0;
            for (const auto &op : instr.operands()) {
                if (!op.is_implicit() || !op.is_reg() || !op.reg_value().is_physical() ||
                    op.reg_value().name != vector_state_name(state)) {
                    continue;
                }
                use_count += op.is_use() ? 1U : 0U;
                def_count += op.is_def() ? 1U : 0U;
            }
            const auto expected_use = desc.implicitly_uses(state);
            const auto expected_def = desc.implicitly_defines(state);
            require(use_count == (expected_use ? 1U : 0U), function, block, instr,
                    std::string(expected_use ? "missing or duplicate implicit use of "
                                             : "undeclared implicit use of ") +
                        vector_state_name(state));
            require(def_count == (expected_def ? 1U : 0U), function, block, instr,
                    std::string(expected_def ? "missing or duplicate implicit def of "
                                             : "undeclared implicit def of ") +
                        vector_state_name(state));
        }
        for (const auto &op : instr.operands()) {
            if (!op.is_implicit() || !op.is_reg() ||
                op.reg_value().reg_class != RegisterClass::VSTATE) {
                continue;
            }
            require(vector_state_for_name(op.reg_value().name).has_value(), function, block,
                    instr, "unknown implicit vector state register");
        }
    }

    void verify_vector_instr(const MachineFunction &function, const MachineBasicBlock &block,
                             const MachineInstr &instr,
                             const std::vector<std::size_t> &explicit_indices,
                             const MachineInstrDesc &desc) const {
        require(module_->target().has_vector, function, block, instr,
                "RVV instruction requires a vector target");
        require(instr.has_vector_info(), function, block, instr,
                "RVV instruction lacks structured vector metadata");
        const auto &info = instr.vector_info();
        require(!info.vector_type.is_mask() || desc.has_flag(MIF_WholeRegister),
                function, block, instr,
                "RVV configuration type must describe data elements, not a mask register");
        require(info.tail_policy != VectorTailPolicy::Unspecified, function, block, instr,
                "RVV tail policy is unspecified");
        require(info.mask_policy != VectorMaskPolicy::Unspecified, function, block, instr,
                "RVV mask policy is unspecified");

        verify_vector_operation(function, block, instr, info, desc);
        verify_physical_vector_groups(function, block, instr, explicit_indices, info, desc);
        verify_vector_metadata_operands(function, block, instr, explicit_indices, info, desc);
        if (desc.has_flag(MIF_WholeRegister)) {
            verify_whole_register_slot(function, block, instr, info);
        }
    }

    void verify_vector_operation(const MachineFunction &function, const MachineBasicBlock &block,
                                 const MachineInstr &instr, const MachineVectorInfo &info,
                                 const MachineInstrDesc &desc) const {
        bool operation_ok = false;
        bool element_ok = true;
        switch (instr.opcode()) {
        case Opcode::RVVSetVL:
        case Opcode::RVVSetVLI:
        case Opcode::RISCVVSetVLI:
        case Opcode::RISCVVSetIVLI:
            operation_ok = info.operation == RVVOperation::SetVL;
            break;
        case Opcode::RVVMaskSet:
        case Opcode::RISCVVMaskSet:
            operation_ok = info.operation == RVVOperation::MaskSet;
            break;
        case Opcode::RVVMaskClear:
        case Opcode::RISCVVMaskClear:
            operation_ok = info.operation == RVVOperation::MaskClear;
            break;
        case Opcode::RVVMaskCopy:
        case Opcode::RISCVVMaskCopy:
        case Opcode::RVVVectorCopy:
        case Opcode::RISCVVVectorCopy:
            operation_ok = info.operation == RVVOperation::Copy;
            break;
        case Opcode::RVVMaskLogical:
        case Opcode::RISCVVMaskLogical:
            operation_ok = is_mask_logical_operation(info.operation);
            break;
        case Opcode::RVVMaskPopCount:
        case Opcode::RISCVVMaskPopCount:
            operation_ok = info.operation == RVVOperation::MaskPopulationCount;
            // Mask population/first operate on the mask layout selected by
            // SEW/LMUL.  The associated data lanes may be either i32 or f32;
            // VTYPE does not encode that semantic distinction.
            element_ok = info.vector_type.element_type() == ValueType::I32 ||
                         info.vector_type.element_type() == ValueType::F32;
            break;
        case Opcode::RVVMaskFirst:
        case Opcode::RISCVVMaskFirst:
            operation_ok = info.operation == RVVOperation::MaskFirst;
            element_ok = info.vector_type.element_type() == ValueType::I32 ||
                         info.vector_type.element_type() == ValueType::F32;
            break;
        case Opcode::RVVMaskLoad:
        case Opcode::RISCVVMaskLoad:
            operation_ok = info.operation == RVVOperation::Load;
            break;
        case Opcode::RVVMaskStore:
        case Opcode::RISCVVMaskStore:
            operation_ok = info.operation == RVVOperation::Store;
            break;
        case Opcode::RVVSIToFP:
        case Opcode::RISCVVSIToFP:
            operation_ok = info.operation == RVVOperation::ConvertSIToFP;
            element_ok = info.vector_type.element_type() == ValueType::F32;
            break;
        case Opcode::RVVFPToSI:
        case Opcode::RISCVVFPToSI:
            operation_ok = info.operation == RVVOperation::ConvertFPToSI;
            element_ok = info.vector_type.element_type() == ValueType::I32;
            break;
        case Opcode::RVVSplatVX:
        case Opcode::RVVSplatVI:
        case Opcode::RVVSplatVXTA:
        case Opcode::RVVSplatVITA:
        case Opcode::RISCVVSplatVX:
        case Opcode::RISCVVSplatVI:
            operation_ok = info.operation == RVVOperation::Splat;
            element_ok = info.vector_type.element_type() == ValueType::I32;
            break;
        case Opcode::RVVSplatVF:
        case Opcode::RVVSplatVFTA:
        case Opcode::RISCVVSplatVF:
            operation_ok = info.operation == RVVOperation::Splat;
            element_ok = info.vector_type.element_type() == ValueType::F32;
            break;
        case Opcode::RVVStep:
        case Opcode::RVVStepTA:
        case Opcode::RISCVVStep:
            operation_ok = info.operation == RVVOperation::Step;
            element_ok = info.vector_type.element_type() == ValueType::I32;
            break;
        case Opcode::RVVIntBinaryVVTA:
        case Opcode::RISCVVIntBinaryVV:
        case Opcode::RVVIntBinaryVV:
        case Opcode::RVVIntBinaryVX:
        case Opcode::RVVIntBinaryVI:
            operation_ok = is_integer_binary_operation(info.operation);
            element_ok = info.vector_type.element_type() == ValueType::I32;
            break;
        case Opcode::RVVFloatBinaryVVTA:
        case Opcode::RISCVVFloatBinaryVV:
        case Opcode::RVVFloatBinaryVV:
        case Opcode::RVVFloatBinaryVF:
            operation_ok = is_float_binary_operation(info.operation);
            element_ok = info.vector_type.element_type() == ValueType::F32;
            break;
        case Opcode::RVVCompareVV:
        case Opcode::RVVCompareVVTA:
        case Opcode::RISCVVCompareVV:
            operation_ok = is_compare_operation(info.operation);
            break;
        case Opcode::RVVCompareVX:
        case Opcode::RISCVVCompareVX:
        case Opcode::RVVCompareVI:
            operation_ok = is_compare_operation(info.operation);
            element_ok = info.vector_type.element_type() == ValueType::I32;
            break;
        case Opcode::RVVCompareVF:
            operation_ok = is_compare_operation(info.operation);
            element_ok = info.vector_type.element_type() == ValueType::F32;
            break;
        case Opcode::RVVMergeVVM:
        case Opcode::RISCVVMergeVVM:
            operation_ok = info.operation == RVVOperation::Merge;
            break;
        case Opcode::RVVMergeVXM:
        case Opcode::RVVMergeVIM:
            operation_ok = info.operation == RVVOperation::Merge;
            element_ok = info.vector_type.element_type() == ValueType::I32;
            break;
        case Opcode::RVVMergeVFM:
            operation_ok = info.operation == RVVOperation::Merge;
            element_ok = info.vector_type.element_type() == ValueType::F32;
            break;
        case Opcode::RVVLoadUnitTA:
        case Opcode::RISCVVLoadUnit:
        case Opcode::RVVLoadUnit:
        case Opcode::RVVLoadStrided:
        case Opcode::RISCVVLoadStrided:
        case Opcode::RVVLoadSegment2:
        case Opcode::RISCVVLoadSegment2:
        case Opcode::RVVLoadIndexed:
        case Opcode::RISCVVLoadIndexedOrdered:
            operation_ok = info.operation == RVVOperation::Load;
            break;
        case Opcode::RVVStoreUnit:
        case Opcode::RISCVVStoreUnit:
        case Opcode::RVVStoreStrided:
        case Opcode::RISCVVStoreStrided:
        case Opcode::RVVStoreSegment2:
        case Opcode::RISCVVStoreSegment2:
        case Opcode::RVVStoreIndexed:
        case Opcode::RISCVVStoreIndexedOrdered:
            operation_ok = info.operation == RVVOperation::Store;
            break;
        case Opcode::RVVExtractElement:
        case Opcode::RISCVVExtractElement:
            operation_ok = info.operation == RVVOperation::Extract;
            break;
        case Opcode::RVVInsertElement:
            operation_ok = info.operation == RVVOperation::Insert;
            break;
        case Opcode::RVVSlideUpVX:
        case Opcode::RVVSlideUpVI:
            operation_ok = info.operation == RVVOperation::SlideUp;
            break;
        case Opcode::RVVSlideDownVX:
        case Opcode::RVVSlideDownVI:
        case Opcode::RISCVVSlideDownVX:
        case Opcode::RISCVVSlideDownVI:
            operation_ok = info.operation == RVVOperation::SlideDown;
            break;
        case Opcode::RVVGatherVV:
        case Opcode::RVVGatherVX:
        case Opcode::RVVGatherVI:
            operation_ok = info.operation == RVVOperation::Gather;
            break;
        case Opcode::RVVReductionInt:
        case Opcode::RISCVVReductionInt:
            operation_ok = is_reduction_operation(info.operation);
            element_ok = info.vector_type.element_type() == ValueType::I32;
            break;
        case Opcode::RVVReductionFloat:
        case Opcode::RISCVVReductionFloatOrdered:
            operation_ok = info.operation == RVVOperation::ReduceSum;
            element_ok = info.vector_type.element_type() == ValueType::F32;
            break;
        case Opcode::RVVWholeRegSpill:
        case Opcode::RISCVVWholeRegSpill:
            operation_ok = info.operation == RVVOperation::Spill;
            break;
        case Opcode::RVVWholeRegReload:
        case Opcode::RISCVVWholeRegReload:
            operation_ok = info.operation == RVVOperation::Reload;
            break;
        default:
            break;
        }
        require(operation_ok, function, block, instr,
                "RVV operation is incompatible with the pseudo opcode");
        require(element_ok, function, block, instr,
                "RVV element type is incompatible with the pseudo opcode");
        if (desc.has_flag(MIF_HasRoundingMode)) {
            require(info.rounding != VectorRoundingMode::None, function, block, instr,
                    "RVV floating operation lacks a rounding mode");
        } else {
            require(info.rounding == VectorRoundingMode::None, function, block, instr,
                    "RVV opcode carries an unsupported rounding mode");
        }
    }

    void verify_vector_metadata_operands(
        const MachineFunction &function, const MachineBasicBlock &block,
        const MachineInstr &instr, const std::vector<std::size_t> &explicit_indices,
        const MachineVectorInfo &info, const MachineInstrDesc &desc) const {
        const auto &ops = instr.operands();
        if (instr.opcode() == Opcode::RVVSetVL || instr.opcode() == Opcode::RVVSetVLI ||
            instr.opcode() == Opcode::RISCVVSetVLI ||
            instr.opcode() == Opcode::RISCVVSetIVLI) {
            require(info.avl.kind == VectorAVLKind::Operand && info.avl.operand_index == 1,
                    function, block, instr, "setvl/setvli must name AVL operand #1");
            require(!info.index_vector_type.has_value() &&
                        !info.passthrough_operand.has_value() && !info.mask_operand.has_value(),
                    function, block, instr, "setvl/setvli cannot carry mask/passthrough operands");
            if (instr.opcode() == Opcode::RISCVVSetVLI) {
                require(ops[1].is_reg(), function, block, instr,
                        "final vsetvli requires a GPR AVL operand");
            } else if (instr.opcode() == Opcode::RISCVVSetIVLI) {
                require(ops[1].kind() == OperandKind::Imm && ops[1].int_value() >= 0 &&
                            ops[1].int_value() <= 31,
                        function, block, instr,
                        "final vsetivli requires an unsigned 5-bit AVL immediate");
            }
            return;
        }
        if (desc.has_flag(MIF_WholeRegister)) {
            require(info.avl.kind == VectorAVLKind::WholeRegister, function, block, instr,
                    "whole-register pseudo requires whole-register AVL semantics");
            require(!info.index_vector_type.has_value() &&
                        !info.passthrough_operand.has_value() && !info.mask_operand.has_value(),
                    function, block, instr,
                    "whole-register pseudo cannot carry mask/passthrough operands");
            return;
        }
        require(info.avl.kind == VectorAVLKind::CurrentVL, function, block, instr,
                "RVV pseudo must consume the current VL");

        const bool has_vector_dest = opcode_has_vector_destination(instr.opcode());
        if (has_vector_dest) {
            require(info.passthrough_operand.has_value() && *info.passthrough_operand == 1,
                    function, block, instr,
                    "vector-producing pseudo must name passthrough operand #1");
        } else {
            require(!info.passthrough_operand.has_value(), function, block, instr,
                    "non-producing RVV pseudo cannot carry a passthrough operand");
        }

        std::optional<std::size_t> mask_explicit_ordinal;
        if (info.mask_operand.has_value()) {
            require(*info.mask_operand < ops.size() && !ops[*info.mask_operand].is_implicit() &&
                        ops[*info.mask_operand].is_reg(),
                    function, block, instr, "RVV mask operand index is invalid");
            const auto found = std::find(explicit_indices.begin(), explicit_indices.end(),
                                         *info.mask_operand);
            require(found != explicit_indices.end(), function, block, instr,
                    "RVV mask does not name an explicit operand");
            mask_explicit_ordinal =
                static_cast<std::size_t>(std::distance(explicit_indices.begin(), found));
            require(*mask_explicit_ordinal + 1 == explicit_indices.size(), function, block, instr,
                    "RVV mask must be the final explicit operand");
            require(ops[*info.mask_operand].reg_value().reg_class == RegisterClass::VMASK,
                    function, block, instr, "RVV mask operand must use VMASK class");
            require(ops[*info.mask_operand].reg_value().is_physical() &&
                        ops[*info.mask_operand].reg_value().name == "v0",
                    function, block, instr,
                    "RVV execution mask operand must be explicit physical v0");
        }
        if (desc.has_flag(MIF_UsesMask)) {
            require(info.mask_operand.has_value(), function, block, instr,
                    "masked-only RVV pseudo lacks a mask operand");
        }

        const auto expected_mask_type = MachineVectorType::mask_for(info.vector_type);
        const auto index_ordinal = index_vector_operand_ordinal(instr.opcode());
        if (index_ordinal.has_value()) {
            require(info.index_vector_type.has_value() &&
                        !info.index_vector_type->is_mask() &&
                        info.index_vector_type->element_type() == ValueType::I32 &&
                        info.index_vector_type->sew_bits() == 32 &&
                        info.index_vector_type->lmul() == info.vector_type.lmul(),
                    function, block, instr,
                    "indexed RVV instruction requires an e32 index vector with data LMUL");
            require(info.index_vector_type->container_kind() ==
                            info.vector_type.container_kind() &&
                        (!info.index_vector_type->is_fixed() ||
                         info.index_vector_type->fixed_lanes() ==
                             info.vector_type.fixed_lanes()),
                    function, block, instr,
                    "RVV index and data vectors require compatible logical lane counts");
        } else {
            require(!info.index_vector_type.has_value(), function, block, instr,
                    "non-indexed RVV pseudo carries an index vector type");
        }
        for (std::size_t ordinal = 0; ordinal < explicit_indices.size(); ++ordinal) {
            const auto operand_index = explicit_indices[ordinal];
            const auto &op = ops[operand_index];
            if (!op.is_reg() || !op.reg_value().is_vector()) {
                continue;
            }
            const bool is_mask_reg = op.reg_value().reg_class == RegisterClass::VMASK;
            auto expected_type =
                is_mask_reg
                    ? expected_mask_type
                    : (index_ordinal.has_value() && ordinal == *index_ordinal
                           ? *info.index_vector_type
                           : info.vector_type);
            const bool is_conversion_source =
                ordinal == 1 &&
                (instr.opcode() == Opcode::RVVSIToFP ||
                 instr.opcode() == Opcode::RISCVVSIToFP ||
                 instr.opcode() == Opcode::RVVFPToSI ||
                 instr.opcode() == Opcode::RISCVVFPToSI);
            if (is_conversion_source) {
                const auto source_element =
                    info.operation == RVVOperation::ConvertSIToFP ? ValueType::I32
                                                                  : ValueType::F32;
                expected_type = info.vector_type.is_fixed()
                                    ? MachineVectorType::fixed(
                                          source_element, info.vector_type.sew_bits(),
                                          info.vector_type.lmul(),
                                          info.vector_type.fixed_lanes())
                                    : MachineVectorType::scalable(
                                          source_element, info.vector_type.sew_bits(),
                                          info.vector_type.lmul());
            }
            require(op.reg_value().vector_type.has_value() &&
                        *op.reg_value().vector_type == expected_type,
                    function, block, instr,
                    "RVV operand vector type does not match instruction metadata");
        }

        if (instr.opcode() == Opcode::RVVExtractElement ||
            instr.opcode() == Opcode::RISCVVExtractElement ||
            instr.opcode() == Opcode::RVVInsertElement) {
            const auto scalar_ordinal =
                (instr.opcode() == Opcode::RVVExtractElement ||
                 instr.opcode() == Opcode::RISCVVExtractElement)
                    ? std::size_t{0}
                    : std::size_t{2};
            const auto &scalar = ops[explicit_indices[scalar_ordinal]].reg_value();
            const auto expected_class = info.vector_type.element_type() == ValueType::F32
                                            ? RegisterClass::FPR32
                                            : RegisterClass::GPR;
            require(scalar.reg_class == expected_class, function, block, instr,
                    "extract/insert scalar register class does not match vector element type");
        }
        if (instr.opcode() == Opcode::RVVExtractElement ||
            instr.opcode() == Opcode::RISCVVExtractElement) {
            const auto &index = ops[explicit_indices[2]];
            require(index.kind() == OperandKind::Imm && index.int_value() == 0,
                    function, block, instr,
                    "RVV extract final family only supports lane zero");
        }
        if (instr.opcode() == Opcode::RVVSlideDownVI ||
            instr.opcode() == Opcode::RISCVVSlideDownVI) {
            const auto index_ordinal =
                instr.opcode() == Opcode::RVVSlideDownVI ? 3U : 2U;
            const auto &index = ops[explicit_indices[index_ordinal]];
            require(index.kind() == OperandKind::Imm && index.int_value() >= 0 &&
                        index.int_value() <= 31,
                    function, block, instr,
                    "vslidedown.vi requires an unsigned 5-bit immediate");
        }
    }

    void verify_physical_vector_groups(
        const MachineFunction &function, const MachineBasicBlock &block,
        const MachineInstr &instr, const std::vector<std::size_t> &explicit_indices,
        const MachineVectorInfo &info, const MachineInstrDesc &desc) const {
        if (!is_post_ra(stage_)) {
            return;
        }
        const auto &ops = instr.operands();
        std::vector<PhysicalVectorOperand> groups;
        for (std::size_t ordinal = 0; ordinal < explicit_indices.size(); ++ordinal) {
            const auto operand_index = explicit_indices[ordinal];
            const auto &op = ops[operand_index];
            if (!op.is_reg() || !op.reg_value().is_vector()) {
                continue;
            }
            const auto index = vector_register_index(op.reg_value().name);
            require(index.has_value(), function, block, instr,
                    "physical vector operand has an invalid register name");
            groups.push_back({operand_index, *index, op.reg_value().vector_group_width, op.is_def(),
                              op.is_use(), op.reg_value().reg_class == RegisterClass::VMASK});
        }

        if (is_segment2_opcode(instr.opcode())) {
            require(explicit_indices.size() == 4, function, block, instr,
                    "2-field segment instruction requires four explicit operands");
            const auto &field0 = ops[explicit_indices[0]];
            const auto &field1 = ops[explicit_indices[1]];
            require(field0.is_reg() && field1.is_reg() &&
                        field0.reg_value().is_physical() &&
                        field1.reg_value().is_physical(),
                    function, block, instr,
                    "post-RA segment fields must name physical vector groups");
            const auto base0 = vector_register_index(field0.reg_value().name);
            const auto base1 = vector_register_index(field1.reg_value().name);
            const auto width = info.vector_type.register_group_width();
            require(base0.has_value() && base1.has_value() &&
                        field0.reg_value().vector_group_width == width &&
                        field1.reg_value().vector_group_width == width,
                    function, block, instr,
                    "segment field register width does not match LMUL");
            require(info.vector_type.lmul_eighths() * 2U <= 64U &&
                        *base0 % width == 0U && *base1 == *base0 + width &&
                        *base0 + 2U * width <= 32U,
                    function, block, instr,
                    "segment fields must be consecutive LMUL-aligned groups with NFIELDS*LMUL <= 8");
        }

        if (info.mask_operand.has_value()) {
            const auto &mask = ops[*info.mask_operand].reg_value();
            require(mask.name == "v0", function, block, instr,
                    "post-RA mask operand must be physical v0");
            for (const auto &group : groups) {
                if (group.operand_index == *info.mask_operand) {
                    continue;
                }
                require(!ranges_overlap(group.base, group.width, 0, 1), function, block, instr,
                        "masked RVV operand group conflicts with v0");
            }
        }

        for (const auto &group : groups) {
            if (!group.is_mask || group.base != 0) {
                continue;
            }
            const bool is_execution_mask =
                info.mask_operand.has_value() && group.operand_index == *info.mask_operand;
            // A mask copy is the only legal bridge between the ordinary mask
            // value bank and architectural v0.  Besides staging execution
            // masks into v0, the fixed-length vector ABI receives its first
            // mask argument from v0 and returns a mask through v0.  Keep every
            // arithmetic/query live range subject to the ordinary-v0 ban.
            const bool is_mask_copy_operand =
                instr.opcode() == Opcode::RVVMaskCopy ||
                instr.opcode() == Opcode::RISCVVMaskCopy;
            require(is_execution_mask || is_mask_copy_operand, function, block, instr,
                    "ordinary mask value cannot occupy physical v0");
        }

        for (std::size_t lhs_index = 0; lhs_index < groups.size(); ++lhs_index) {
            for (std::size_t rhs_index = lhs_index + 1; rhs_index < groups.size(); ++rhs_index) {
                const auto &lhs = groups[lhs_index];
                const auto &rhs = groups[rhs_index];
                if (!ranges_overlap(lhs.base, lhs.width, rhs.base, rhs.width) ||
                    (!lhs.is_def && !rhs.is_def)) {
                    continue;
                }
                bool tied = false;
                for (std::size_t ordinal = 0; ordinal < explicit_indices.size(); ++ordinal) {
                    if (explicit_indices[ordinal] != lhs.operand_index ||
                        ordinal >= desc.operand_constraint_count) {
                        continue;
                    }
                    const auto tied_to = desc.operand_constraints[ordinal].tied_to;
                    tied = tied_to >= 0 && static_cast<std::size_t>(tied_to) < explicit_indices.size() &&
                           explicit_indices[static_cast<std::size_t>(tied_to)] == rhs.operand_index;
                }
                require(tied && lhs.base == rhs.base && lhs.width == rhs.width, function, block,
                        instr, "physical vector register groups overlap without a legal tie");
            }
        }
    }

    void verify_whole_register_slot(const MachineFunction &function,
                                    const MachineBasicBlock &block,
                                    const MachineInstr &instr,
                                    const MachineVectorInfo &info) const {
        const auto vector_operand =
            instr.opcode() == Opcode::RVVWholeRegSpill ? 1U : 0U;
        require(vector_operand < instr.operands().size() &&
                    instr.operands()[vector_operand].is_reg() &&
                    instr.operands()[vector_operand].reg_value().is_vector() &&
                    instr.operands()[vector_operand].reg_value().vector_type.has_value() &&
                    *instr.operands()[vector_operand].reg_value().vector_type ==
                        info.vector_type &&
                    instr.operands()[vector_operand].reg_value().vector_group_width ==
                        info.vector_type.register_group_width(),
                function, block, instr,
                "whole-register operand type/group does not match spill metadata");
        if (instr.opcode() == Opcode::RISCVVWholeRegSpill ||
            instr.opcode() == Opcode::RISCVVWholeRegReload) {
            require(instr.operands().size() == 3 && instr.operands()[1].is_reg() &&
                        instr.operands()[1].reg_value().reg_class == RegisterClass::GPR,
                    function, block, instr,
                    "final whole-register spill/reload requires a concrete GPR address");
            return;
        }
        require(instr.operands().size() == 5, function, block, instr,
                "whole-register pseudo must expose vlenb and t5/t6 scratch state");
        const auto require_scratch_def = [&](std::size_t index, const char *name) {
            const auto &scratch = instr.operands()[index];
            require(scratch.is_implicit() && scratch.is_reg() && scratch.is_def() &&
                        !scratch.is_use() && scratch.reg_value().is_physical() &&
                        scratch.reg_value().reg_class == RegisterClass::GPR &&
                        scratch.reg_value().name == name,
                    function, block, instr,
                    std::string("whole-register pseudo requires implicit def of ") + name);
        };
        require_scratch_def(3, "t5");
        require_scratch_def(4, "t6");
        const auto slot_operand = instr.opcode() == Opcode::RVVWholeRegSpill ? 0U : 1U;
        const auto &op = instr.operands()[slot_operand];
        require(op.kind() == OperandKind::Slot, function, block, instr,
                "whole-register pseudo lacks a stack slot");
        const auto *slot = function.stack_slot(op.slot_id());
        require(slot != nullptr && slot->scalable_size.has_value() &&
                    slot->type.vector_type.has_value(),
                function, block, instr,
                "whole-register pseudo requires a scalable vector stack slot");
        require(!slot->has_fixed_offset && *slot->type.vector_type == info.vector_type &&
                    *slot->scalable_size ==
                        MachineScalableSize::from_register_group_width(
                            info.vector_type.register_group_width()),
                function, block, instr,
                "whole-register slot size/type does not match the RVV register group");
        require(slot->kind == StackSlotKind::Spill, function, block, instr,
                "whole-register pseudo requires a spill stack slot");
    }

    void verify_scalar_instr(const MachineFunction &function, const MachineBasicBlock &block,
                             const MachineInstr &instr) const {
        const auto &ops = instr.operands();
        switch (instr.opcode()) {
        case Opcode::LoadSlot:
            require(ops.size() >= 3 && ops[0].is_reg() && ops[1].kind() == OperandKind::Slot,
                    function, block, instr, "malformed LOAD_SLOT");
            require(ops[2].kind() == OperandKind::Type, function, block, instr,
                    "LOAD_SLOT missing type operand");
            verify_reg_matches_type(function, block, instr, ops[0].reg_value(),
                                    ops[2].type_value(), "LOAD_SLOT destination");
            break;
        case Opcode::StoreSlot:
            require(ops.size() >= 3 && ops[0].kind() == OperandKind::Slot && ops[1].is_reg(),
                    function, block, instr, "malformed STORE_SLOT");
            require(ops[2].kind() == OperandKind::Type, function, block, instr,
                    "STORE_SLOT missing type operand");
            verify_reg_matches_type(function, block, instr, ops[1].reg_value(),
                                    ops[2].type_value(), "STORE_SLOT source");
            break;
        case Opcode::LoadMem:
        case Opcode::StoreMem:
            require(ops.size() >= 3 && ops[0].is_reg() && ops[1].is_reg(), function, block,
                    instr, "malformed memory instruction");
            require(ops[2].kind() == OperandKind::Type, function, block, instr,
                    "memory instruction missing type operand");
            break;
        case Opcode::LoadMemOffset:
        case Opcode::StoreMemOffset:
            require(ops.size() >= 4 && ops[0].is_reg() && ops[1].is_reg() &&
                        ops[2].kind() == OperandKind::Imm,
                    function, block, instr, "malformed offset memory instruction");
            require(ops[3].kind() == OperandKind::Type, function, block, instr,
                    "offset memory instruction missing type operand");
            break;
        case Opcode::MemZero:
            require(ops.size() >= 3 && ops[0].is_reg(), function, block, instr,
                    "malformed MEMZERO");
            require(ops[1].kind() == OperandKind::Imm && ops[1].int_value() >= 0 &&
                        ops[1].int_value() <= 255,
                    function, block, instr, "MEMZERO byte value must be an immediate in [0, 255]");
            require(ops[2].kind() == OperandKind::Imm || ops[2].is_reg(), function, block, instr,
                    "MEMZERO byte count must be immediate or register");
            if (ops[2].kind() == OperandKind::Imm) {
                require(ops[2].int_value() >= 0, function, block, instr,
                        "MEMZERO byte count must be non-negative");
            } else {
                require(ops[2].reg_value().reg_class == RegisterClass::GPR, function, block, instr,
                        "MEMZERO byte count register must be a GPR");
            }
            break;
        case Opcode::FmvWX:
            require(ops.size() >= 2 && ops[0].is_reg() && ops[1].is_reg(), function, block,
                    instr, "malformed FMV.W.X");
            require(ops[0].reg_value().reg_class == RegisterClass::FPR32 &&
                        ops[1].reg_value().reg_class == RegisterClass::GPR,
                    function, block, instr, "FMV.W.X requires an FPR destination and GPR source");
            break;
        case Opcode::FmvXW:
            require(ops.size() >= 2 && ops[0].is_reg() && ops[1].is_reg(), function, block,
                    instr, "malformed FMV.X.W");
            require(ops[0].reg_value().reg_class == RegisterClass::GPR &&
                        ops[1].reg_value().reg_class == RegisterClass::FPR32,
                    function, block, instr, "FMV.X.W requires a GPR destination and FPR source");
            break;
        case Opcode::BranchNonZero:
            require(ops.size() >= 2 && ops[0].is_reg() && ops[1].kind() == OperandKind::Block,
                    function, block, instr, "malformed BNEZ");
            break;
        case Opcode::BranchZero:
            require(ops.size() >= 2 && ops[0].is_reg() && ops[1].kind() == OperandKind::Block,
                    function, block, instr, "malformed BEQZ");
            break;
        case Opcode::BranchEq:
        case Opcode::BranchNe:
        case Opcode::BranchLT:
        case Opcode::BranchGE:
            require(ops.size() >= 3 && ops[0].is_reg() && ops[1].is_reg() &&
                        ops[2].kind() == OperandKind::Block,
                    function, block, instr, "malformed binary branch");
            break;
        case Opcode::Jump:
            require(!ops.empty(), function, block, instr, "malformed J");
            break;
        case Opcode::RISCVLocalLabel:
            require(ops.size() == 1 && ops[0].kind() == OperandKind::Symbol &&
                        !ops[0].string_value().empty(),
                    function, block, instr, "malformed final local label");
            break;
        case Opcode::RISCVLUI:
            require(ops.size() == 2 && ops[0].is_reg() && ops[0].is_def() &&
                        ops[0].reg_value().reg_class == RegisterClass::GPR &&
                        ops[1].kind() == OperandKind::Imm,
                    function, block, instr, "malformed final LUI");
            break;
        case Opcode::RISCVAUIPC:
            require(ops.size() == 2 && ops[0].is_reg() && ops[0].is_def() &&
                        ops[0].reg_value().reg_class == RegisterClass::GPR &&
                        ops[1].kind() == OperandKind::Reloc &&
                        ops[1].relocation_kind() == RelocationKind::PCRelHi &&
                        !ops[1].string_value().empty(),
                    function, block, instr, "malformed final AUIPC relocation");
            break;
        case Opcode::RISCVAddiReloc:
            require(ops.size() == 3 && ops[0].is_reg() && ops[0].is_def() &&
                        ops[1].is_reg() && ops[1].is_use() &&
                        ops[0].reg_value().reg_class == RegisterClass::GPR &&
                        ops[1].reg_value().reg_class == RegisterClass::GPR &&
                        ops[2].kind() == OperandKind::Reloc &&
                        ops[2].relocation_kind() == RelocationKind::PCRelLo &&
                        !ops[2].string_value().empty(),
                    function, block, instr, "malformed final ADDI relocation");
            break;
        case Opcode::RISCVJAL:
            require(ops.size() == 2 && ops[0].is_reg() && ops[0].is_def() &&
                        ops[0].reg_value().reg_class == RegisterClass::GPR &&
                        (ops[1].kind() == OperandKind::Block ||
                         ops[1].kind() == OperandKind::Symbol),
                    function, block, instr, "malformed final JAL");
            break;
        case Opcode::RISCVJALR:
            require(ops.size() == 3 && ops[0].is_reg() && ops[0].is_def() &&
                        ops[1].is_reg() && ops[1].is_use() &&
                        ops[0].reg_value().reg_class == RegisterClass::GPR &&
                        ops[1].reg_value().reg_class == RegisterClass::GPR &&
                        ops[2].kind() == OperandKind::Reloc &&
                        ops[2].relocation_kind() == RelocationKind::PCRelLo &&
                        !ops[2].string_value().empty(),
                    function, block, instr, "malformed final JALR relocation");
            break;
        case Opcode::RISCVSLTIU:
            require(ops.size() == 3 && ops[0].is_reg() && ops[0].is_def() &&
                        ops[1].is_reg() && ops[1].is_use() &&
                        ops[0].reg_value().reg_class == RegisterClass::GPR &&
                        ops[1].reg_value().reg_class == RegisterClass::GPR &&
                        ops[2].kind() == OperandKind::Imm,
                    function, block, instr, "malformed final SLTIU");
            break;
        case Opcode::RISCVFSGNJS:
            require(ops.size() == 3 && ops[0].is_reg() && ops[0].is_def() &&
                        ops[1].is_reg() && ops[1].is_use() && ops[2].is_reg() &&
                        ops[2].is_use() &&
                        ops[0].reg_value().reg_class == RegisterClass::FPR32 &&
                        ops[1].reg_value().reg_class == RegisterClass::FPR32 &&
                        ops[2].reg_value().reg_class == RegisterClass::FPR32,
                    function, block, instr, "malformed final FSGNJ.S");
            break;
        case Opcode::RISCVLBU:
        case Opcode::RISCVLW:
        case Opcode::RISCVLD:
        case Opcode::RISCVFLW: {
            const auto expected = instr.opcode() == Opcode::RISCVFLW
                                      ? RegisterClass::FPR32
                                      : RegisterClass::GPR;
            require(ops.size() == 3 && ops[0].is_reg() && ops[0].is_def() &&
                        ops[0].reg_value().reg_class == expected && ops[1].is_reg() &&
                        ops[1].is_use() &&
                        ops[1].reg_value().reg_class == RegisterClass::GPR &&
                        ops[2].kind() == OperandKind::Imm,
                    function, block, instr, "malformed final load");
            break;
        }
        case Opcode::RISCVSB:
        case Opcode::RISCVSW:
        case Opcode::RISCVSD:
        case Opcode::RISCVFSW: {
            const auto expected = instr.opcode() == Opcode::RISCVFSW
                                      ? RegisterClass::FPR32
                                      : RegisterClass::GPR;
            require(ops.size() == 3 && ops[0].is_reg() && ops[0].is_use() &&
                        ops[0].reg_value().reg_class == expected && ops[1].is_reg() &&
                        ops[1].is_use() &&
                        ops[1].reg_value().reg_class == RegisterClass::GPR &&
                        ops[2].kind() == OperandKind::Imm,
                    function, block, instr, "malformed final store");
            break;
        }
        default:
            break;
        }
    }

    void verify_reg_matches_type(const MachineFunction &function, const MachineBasicBlock &block,
                                 const MachineInstr &instr, const Register &reg, ValueType type,
                                 const std::string &what) const {
        if (type == ValueType::F32) {
            require(reg.reg_class == RegisterClass::FPR32, function, block, instr,
                    what + " must be an FPR for f32");
            return;
        }
        if (type == ValueType::I1 || type == ValueType::I32 || type == ValueType::Ptr) {
            require(reg.reg_class == RegisterClass::GPR, function, block, instr,
                    what + " must be a GPR for integer/pointer type");
        }
    }

    void verify_reg(const MachineFunction &function, const MachineBasicBlock &block,
                    const MachineInstr &instr, const Register &reg) const {
        if (reg.is_virtual()) {
            if (is_post_ra(stage_)) {
                fail(where(function, block, instr) + " has virtual register after RA");
            }
            const auto *canonical = function.regs().virtual_register(reg.id);
            if (canonical == nullptr) {
                fail(where(function, block, instr) + " references unknown virtual register");
            }
            if (canonical->reg_class != reg.reg_class || canonical->value_type != reg.value_type ||
                canonical->vector_type != reg.vector_type ||
                canonical->vector_group_width != reg.vector_group_width) {
                fail(where(function, block, instr) +
                     " references a virtual register with inconsistent type/group metadata");
            }
            return;
        }
        if (reg.name.empty()) {
            fail(where(function, block, instr) + " has unnamed physical register");
        }

        if (reg.is_vector()) {
            const auto index = vector_register_index(reg.name);
            require(index.has_value(), function, block, instr,
                    "vector register must name physical v0-v31");
            require(reg.vector_type.has_value(), function, block, instr,
                    "physical vector register lacks machine vector type metadata");
            require(reg.value_type == reg.vector_type->element_type(), function, block, instr,
                    "physical vector register element type is inconsistent");
            require(reg.vector_group_width == reg.vector_type->register_group_width(), function,
                    block, instr, "physical vector register group width does not match LMUL");
            require(*index % reg.vector_type->register_group_alignment() == 0, function, block,
                    instr, "physical vector register group is misaligned for LMUL");
            require(*index + reg.vector_group_width <= 32, function, block, instr,
                    "physical vector register group extends past v31");
            if (reg.reg_class == RegisterClass::VMASK) {
                require(reg.vector_type->is_mask(), function, block, instr,
                        "VMASK physical register must carry a mask type");
            } else {
                require(!reg.vector_type->is_mask(), function, block, instr,
                        "data vector register carries a mask type");
                if (reg.reg_class == RegisterClass::VRNoV0) {
                    require(*index != 0, function, block, instr,
                            "VRNoV0 physical register cannot use v0");
                }
            }
            return;
        }

        require(register_class_for_physical(reg.name) == reg.reg_class, function, block, instr,
                "mismatched physical register class");
    }

    void require(bool condition, const MachineFunction &function, const MachineBasicBlock &block,
                 const MachineInstr &instr, const std::string &message) const {
        if (!condition) {
            fail(where(function, block, instr) + ": " + message);
        }
    }

    std::string where(const MachineFunction &function, const MachineBasicBlock &block,
                      const MachineInstr &instr) const {
        std::ostringstream oss;
        oss << "MIR verifier @" << function.name() << " %" << block.name() << " "
            << opcode_name(instr.opcode());
        return oss.str();
    }

    const Module *module_ = nullptr;
    MIRVerificationStage stage_ = MIRVerificationStage::PreRA;
};

} // namespace

MIRVerifyResult verify_module(const Module &module, MIRVerificationStage stage) {
    try {
        Verifier verifier;
        return verifier.verify(module, stage);
    } catch (const std::exception &ex) {
        return {false, ex.what()};
    }
}

} // namespace mir
