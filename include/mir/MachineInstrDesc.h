#pragma once

#include "mir/MIR.h"

#include <cstddef>
#include <cstdint>

namespace mir {

enum class MachineMemoryEffect {
    None,
    Read,
    Write,
    ReadWrite,
    Unknown,
};

enum MachineInstrFlag : std::uint32_t {
    MIF_None = 0,
    MIF_Pseudo = 1U << 0U,
    MIF_Terminator = 1U << 1U,
    MIF_Branch = 1U << 2U,
    MIF_Call = 1U << 3U,
    MIF_Barrier = 1U << 4U,
    MIF_SideEffects = 1U << 5U,
    MIF_ReadsVL = 1U << 6U,
    MIF_WritesVL = 1U << 7U,
    MIF_ReadsVType = 1U << 8U,
    MIF_WritesVType = 1U << 9U,
    MIF_UsesMask = 1U << 10U,
    MIF_Vector = 1U << 11U,
    MIF_WholeRegister = 1U << 12U,
    MIF_HasRoundingMode = 1U << 13U,
};

enum class MachineOperandConstraintKind : std::uint8_t {
    Any,
    Register,
    Immediate,
    RegisterOrImmediate,
    StackSlot,
};

enum class MachineOperandRole : std::uint8_t {
    Any,
    Def,
    Use,
    DefUse,
};

enum MachineRegisterClassMask : std::uint32_t {
    MRC_None = 0,
    MRC_GPR = 1U << 0U,
    MRC_FPR32 = 1U << 1U,
    MRC_VR = 1U << 2U,
    MRC_VMASK = 1U << 3U,
    MRC_VRNoV0 = 1U << 4U,
    MRC_VSTATE = 1U << 5U,
    MRC_AnyVector = MRC_VR | MRC_VMASK | MRC_VRNoV0,
};

enum MachineVectorState : std::uint32_t {
    MVS_None = 0,
    MVS_VL = 1U << 0U,
    MVS_VTYPE = 1U << 1U,
    MVS_VLENB = 1U << 2U,
    MVS_VXRM = 1U << 3U,
    MVS_FRM = 1U << 4U,
    MVS_VXSAT = 1U << 5U,
    MVS_VSTART = 1U << 6U,
};

struct MachineOperandConstraint {
    MachineOperandConstraintKind kind = MachineOperandConstraintKind::Any;
    MachineOperandRole role = MachineOperandRole::Any;
    std::uint32_t register_classes = MRC_None;
    std::int8_t tied_to = -1;
    bool carries_vector_group = false;
    bool requires_group_alignment = false;
};

struct MachineInstrDesc {
    const char *name = "UNKNOWN";
    std::uint8_t min_operands = 0;
    std::uint8_t max_operands = 0;
    std::uint32_t flags = MIF_None;
    MachineMemoryEffect memory = MachineMemoryEffect::None;
    unsigned latency = 1;
    std::uint8_t min_explicit_operands = 0;
    std::uint8_t max_explicit_operands = 0;
    const MachineOperandConstraint *operand_constraints = nullptr;
    std::uint8_t operand_constraint_count = 0;
    std::uint32_t implicit_uses = MVS_None;
    std::uint32_t implicit_defs = MVS_None;

    bool has_flag(MachineInstrFlag flag) const;
    bool has_exact_operand_count() const;
    bool accepts_operand_count(std::size_t count) const;
    bool accepts_explicit_operand_count(std::size_t count) const;
    bool implicitly_uses(MachineVectorState state) const;
    bool implicitly_defines(MachineVectorState state) const;
    bool may_load() const;
    bool may_store() const;
};

std::uint32_t register_class_mask(RegisterClass reg_class);
const MachineInstrDesc &instruction_desc(Opcode opcode);
// True for explicit call opcodes and for MEMZERO forms whose canonical
// expansion invokes memset.  All liveness, frame, and verifier clients must
// use this predicate so the call-clobber contract cannot drift from pseudo
// expansion.
bool machine_instr_may_call(const MachineInstr &instr);

} // namespace mir
