#include "mir/MachineInstrDesc.h"

#include <array>
#include <stdexcept>

namespace mir {
namespace {

constexpr std::uint8_t kVariableOperands = 0xffU;
constexpr std::uint32_t kPseudo = MIF_Pseudo;
constexpr std::uint32_t kBranch = MIF_Terminator | MIF_Branch | MIF_Barrier;
constexpr std::uint32_t kSideEffect = MIF_SideEffects | MIF_Barrier;
constexpr std::uint32_t kVectorPseudo = MIF_Vector | MIF_Pseudo;

constexpr MachineOperandConstraint operand(
    MachineOperandConstraintKind kind, MachineOperandRole role = MachineOperandRole::Any,
    std::uint32_t classes = MRC_None, std::int8_t tied_to = -1,
    bool carries_vector_group = false, bool requires_group_alignment = false) {
    return {kind, role, classes, tied_to, carries_vector_group, requires_group_alignment};
}

constexpr std::array<MachineOperandConstraint, 3> kSetVL = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def, MRC_GPR),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
}};

constexpr std::array<MachineOperandConstraint, 2> kSetVLI = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def, MRC_GPR),
    operand(MachineOperandConstraintKind::RegisterOrImmediate, MachineOperandRole::Use, MRC_GPR),
}};

constexpr std::array<MachineOperandConstraint, 1> kMaskSet = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def, MRC_VMASK,
            -1, true, true),
}};

constexpr std::array<MachineOperandConstraint, 2> kMaskCopy = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def, MRC_VMASK,
            -1, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_VMASK,
            -1, true, true),
}};

constexpr std::array<MachineOperandConstraint, 3> kMaskLogical = {{
    kMaskCopy[0], kMaskCopy[1], kMaskCopy[1],
}};

constexpr std::array<MachineOperandConstraint, 3> kMaskPopCount = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def, MRC_GPR),
    kMaskCopy[1], kMaskCopy[1],
}};

constexpr std::array<MachineOperandConstraint, 2> kMaskFirst = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def, MRC_GPR),
    kMaskCopy[1],
}};

constexpr std::array<MachineOperandConstraint, 2> kMaskLoad = {{
    kMaskCopy[0],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
}};

constexpr std::array<MachineOperandConstraint, 2> kMaskStore = {{
    kMaskCopy[1],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
}};

constexpr std::array<MachineOperandConstraint, 3> kSplatVX = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def,
            MRC_VR | MRC_VRNoV0, 1, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use,
            MRC_VR | MRC_VRNoV0, 0, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
}};

constexpr std::array<MachineOperandConstraint, 3> kSplatVI = {{
    kSplatVX[0], kSplatVX[1],
    operand(MachineOperandConstraintKind::Immediate, MachineOperandRole::Use),
}};

constexpr std::array<MachineOperandConstraint, 3> kSplatVF = {{
    kSplatVX[0], kSplatVX[1],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_FPR32),
}};

constexpr std::array<MachineOperandConstraint, 2> kStep = {{
    kSplatVX[0], kSplatVX[1],
}};

constexpr std::array<MachineOperandConstraint, 2> kSplatVXTA = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def,
            MRC_VR | MRC_VRNoV0, -1, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
}};

constexpr std::array<MachineOperandConstraint, 2> kSplatVITA = {{
    kSplatVXTA[0],
    operand(MachineOperandConstraintKind::Immediate, MachineOperandRole::Use),
}};

constexpr std::array<MachineOperandConstraint, 2> kSplatVFTA = {{
    kSplatVXTA[0],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_FPR32),
}};

constexpr std::array<MachineOperandConstraint, 1> kStepTA = {{kSplatVXTA[0]}};

constexpr std::uint32_t kDataVectorClasses = MRC_VR | MRC_VRNoV0;

constexpr std::array<MachineOperandConstraint, 2> kVectorCopy = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def,
            kDataVectorClasses, -1, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use,
            kDataVectorClasses, -1, true, true),
}};

constexpr std::array<MachineOperandConstraint, 2> kVectorConvert = {{
    kVectorCopy[0], kVectorCopy[1],
}};

constexpr std::array<MachineOperandConstraint, 5> kVectorVV = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def, kDataVectorClasses,
            1, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, kDataVectorClasses,
            0, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, kDataVectorClasses,
            -1, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, kDataVectorClasses,
            -1, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_VMASK, -1, true,
            true),
}};

constexpr std::array<MachineOperandConstraint, 4> kVectorVVTA = {{
    kSplatVXTA[0], kVectorVV[2], kVectorVV[3], kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 5> kVectorVX = {{
    kVectorVV[0], kVectorVV[1], kVectorVV[2],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 5> kVectorVI = {{
    kVectorVV[0], kVectorVV[1], kVectorVV[2],
    operand(MachineOperandConstraintKind::Immediate, MachineOperandRole::Use), kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 5> kVectorVF = {{
    kVectorVV[0], kVectorVV[1], kVectorVV[2],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_FPR32),
    kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 5> kCompareVV = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def, MRC_VMASK, 1, true,
            true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_VMASK, 0, true,
            true),
    kVectorVV[2], kVectorVV[3], kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 5> kCompareVX = {{
    kCompareVV[0], kCompareVV[1], kCompareVV[2],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kCompareVV[4],
}};

constexpr std::array<MachineOperandConstraint, 5> kCompareVI = {{
    kCompareVV[0], kCompareVV[1], kCompareVV[2],
    operand(MachineOperandConstraintKind::Immediate, MachineOperandRole::Use), kCompareVV[4],
}};

constexpr std::array<MachineOperandConstraint, 5> kCompareVF = {{
    kCompareVV[0], kCompareVV[1], kCompareVV[2],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_FPR32),
    kCompareVV[4],
}};

constexpr std::array<MachineOperandConstraint, 4> kFinalCompareVV = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def, MRC_VMASK,
            -1, true, true),
    kVectorVV[2], kVectorVV[3], kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 4> kFinalCompareVX = {{
    kFinalCompareVV[0], kFinalCompareVV[1],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kFinalCompareVV[3],
}};

constexpr std::array<MachineOperandConstraint, 4> kFinalSlideVX = {{
    kSplatVXTA[0], kVectorVV[2],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 4> kFinalSlideVI = {{
    kSplatVXTA[0], kVectorVV[2],
    operand(MachineOperandConstraintKind::Immediate, MachineOperandRole::Use),
    kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 4> kMergeVV = {{
    kVectorCopy[0], kVectorCopy[1], kVectorCopy[1], kMaskCopy[1],
}};

constexpr std::array<MachineOperandConstraint, 4> kMergeVX = {{
    kVectorCopy[0], kVectorCopy[1],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kMaskCopy[1],
}};

constexpr std::array<MachineOperandConstraint, 4> kMergeVI = {{
    kVectorCopy[0], kVectorCopy[1],
    operand(MachineOperandConstraintKind::Immediate, MachineOperandRole::Use),
    kMaskCopy[1],
}};

constexpr std::array<MachineOperandConstraint, 4> kMergeVF = {{
    kVectorCopy[0], kVectorCopy[1],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_FPR32),
    kMaskCopy[1],
}};

constexpr std::array<MachineOperandConstraint, 4> kLoadUnit = {{
    kVectorVV[0], kVectorVV[1],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 3> kLoadUnitTA = {{
    kSplatVXTA[0],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 3> kStoreUnit = {{
    kVectorVV[2],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 5> kLoadStrided = {{
    kVectorVV[0], kVectorVV[1],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 4> kStoreStrided = {{
    kVectorVV[2],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 4> kLoadSegment2 = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def,
            kDataVectorClasses, -1, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def,
            kDataVectorClasses, -1, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 4> kStoreSegment2 = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use,
            kDataVectorClasses, -1, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use,
            kDataVectorClasses, -1, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 5> kLoadIndexed = {{
    kVectorVV[0], kVectorVV[1],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kVectorVV[2], kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 4> kStoreIndexed = {{
    kVectorVV[2],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kVectorVV[2], kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 4> kFinalLoadIndexed = {{
    kSplatVXTA[0],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kVectorVV[2], kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 4> kFinalLoadStrided = {{
    kSplatVXTA[0],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
    kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 3> kExtract = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def, MRC_GPR | MRC_FPR32),
    kVectorVV[2],
    operand(MachineOperandConstraintKind::RegisterOrImmediate, MachineOperandRole::Use, MRC_GPR),
}};

constexpr std::array<MachineOperandConstraint, 5> kInsert = {{
    kVectorVV[0], kVectorVV[1],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR | MRC_FPR32),
    operand(MachineOperandConstraintKind::RegisterOrImmediate, MachineOperandRole::Use, MRC_GPR),
    kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 4> kReduction = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def,
            kDataVectorClasses, 2, true, true),
    kVectorVV[2],
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use,
            kDataVectorClasses, 0, true, true),
    kVectorVV[4],
}};

constexpr std::array<MachineOperandConstraint, 2> kWholeSpill = {{
    operand(MachineOperandConstraintKind::StackSlot, MachineOperandRole::Def),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_AnyVector,
            -1, true, true),
}};

constexpr std::array<MachineOperandConstraint, 2> kWholeReload = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def, MRC_AnyVector,
            -1, true, true),
    operand(MachineOperandConstraintKind::StackSlot, MachineOperandRole::Use),
}};

constexpr std::array<MachineOperandConstraint, 2> kFinalWholeSpill = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_AnyVector,
            -1, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
}};

constexpr std::array<MachineOperandConstraint, 2> kFinalWholeReload = {{
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Def,
            MRC_AnyVector, -1, true, true),
    operand(MachineOperandConstraintKind::Register, MachineOperandRole::Use, MRC_GPR),
}};

constexpr std::uint32_t state_flags(std::uint32_t uses, std::uint32_t defs) {
    std::uint32_t flags = MIF_None;
    if ((uses & MVS_VL) != 0) {
        flags |= MIF_ReadsVL;
    }
    if ((defs & MVS_VL) != 0) {
        flags |= MIF_WritesVL;
    }
    if ((uses & MVS_VTYPE) != 0) {
        flags |= MIF_ReadsVType;
    }
    if ((defs & MVS_VTYPE) != 0) {
        flags |= MIF_WritesVType;
    }
    return flags;
}

constexpr MachineInstrDesc desc(const char *name, std::uint8_t operands,
                                std::uint32_t flags = MIF_None,
                                MachineMemoryEffect memory = MachineMemoryEffect::None,
                                unsigned latency = 1) {
    return {name, operands, operands, flags, memory, latency, operands, operands, nullptr, 0,
            MVS_None, MVS_None};
}

constexpr MachineInstrDesc variable_desc(const char *name, std::uint8_t minimum,
                                         std::uint32_t flags = MIF_None,
                                         MachineMemoryEffect memory = MachineMemoryEffect::None,
                                         unsigned latency = 1,
                                         std::uint8_t min_explicit = kVariableOperands,
                                         std::uint8_t max_explicit = kVariableOperands,
                                         std::uint32_t implicit_uses = MVS_None,
                                         std::uint32_t implicit_defs = MVS_None) {
    if (min_explicit == kVariableOperands) {
        min_explicit = minimum;
    }
    return {name, minimum, kVariableOperands,
            flags | state_flags(implicit_uses, implicit_defs), memory, latency,
            min_explicit, max_explicit, nullptr, 0, implicit_uses,
            implicit_defs};
}

template <std::size_t N>
constexpr MachineInstrDesc vector_desc(
    const char *name, std::uint8_t min_operands, std::uint8_t max_operands,
    std::uint8_t min_explicit, std::uint8_t max_explicit,
    const std::array<MachineOperandConstraint, N> &constraints,
    std::uint32_t extra_flags = MIF_None,
    MachineMemoryEffect memory = MachineMemoryEffect::None, unsigned latency = 1,
    std::uint32_t implicit_uses = MVS_VL | MVS_VTYPE,
    std::uint32_t implicit_defs = MVS_None) {
    return {name,
            min_operands,
            max_operands,
            kVectorPseudo | extra_flags | state_flags(implicit_uses, implicit_defs),
            memory,
            latency,
            min_explicit,
            max_explicit,
            constraints.data(),
            static_cast<std::uint8_t>(N),
            implicit_uses,
            implicit_defs};
}

template <std::size_t N>
constexpr MachineInstrDesc vector_final_desc(
    const char *name, std::uint8_t min_operands, std::uint8_t max_operands,
    std::uint8_t min_explicit, std::uint8_t max_explicit,
    const std::array<MachineOperandConstraint, N> &constraints,
    std::uint32_t extra_flags = MIF_None,
    MachineMemoryEffect memory = MachineMemoryEffect::None, unsigned latency = 1,
    std::uint32_t implicit_uses = MVS_VL | MVS_VTYPE,
    std::uint32_t implicit_defs = MVS_None) {
    return {name,
            min_operands,
            max_operands,
            MIF_Vector | extra_flags | state_flags(implicit_uses, implicit_defs),
            memory,
            latency,
            min_explicit,
            max_explicit,
            constraints.data(),
            static_cast<std::uint8_t>(N),
            implicit_uses,
            implicit_defs};
}

constexpr std::array<MachineInstrDesc, static_cast<std::size_t>(Opcode::Count)> kDescriptors = {{
    variable_desc("COMMENT", 0, kPseudo),
    desc("LI", 2, kPseudo),
    desc("FLI.S", 2, kPseudo),
    desc("LA", 2, kPseudo),
    desc("FI_ADDR", 2, kPseudo),
    desc("LOAD_SLOT", 3, kPseudo, MachineMemoryEffect::Read, 4),
    desc("STORE_SLOT", 3, kPseudo | MIF_SideEffects, MachineMemoryEffect::Write),
    desc("LOAD_MEM", 3, kPseudo, MachineMemoryEffect::Read, 4),
    desc("STORE_MEM", 3, kPseudo | MIF_SideEffects, MachineMemoryEffect::Write),
    desc("LOAD_MEM_OFF", 4, kPseudo, MachineMemoryEffect::Read, 4),
    desc("STORE_MEM_OFF", 4, kPseudo | MIF_SideEffects, MachineMemoryEffect::Write),
    desc("MEMZERO", 3, kPseudo | kSideEffect, MachineMemoryEffect::Write),
    variable_desc("MV", 2, kPseudo, MachineMemoryEffect::None, 1, 2, 2),
    variable_desc("FMV.S", 2, kPseudo, MachineMemoryEffect::None, 1, 2, 2),
    desc("ADD", 3),
    desc("ADDW", 3),
    desc("ADDI", 3),
    desc("ADDIW", 3),
    desc("SUB", 3),
    desc("SUBW", 3),
    desc("MUL", 3, MIF_None, MachineMemoryEffect::None, 4),
    desc("MULW", 3, MIF_None, MachineMemoryEffect::None, 4),
    desc("DIVU", 3, MIF_None, MachineMemoryEffect::None, 12),
    desc("DIVW", 3, MIF_None, MachineMemoryEffect::None, 12),
    desc("REMW", 3, MIF_None, MachineMemoryEffect::None, 12),
    desc("AND", 3),
    desc("ANDI", 3),
    desc("SLLI", 3),
    desc("SLLIW", 3),
    desc("SRAI", 3),
    desc("SRAIW", 3),
    desc("SRLI", 3),
    desc("SRLIW", 3),
    desc("XOR", 3),
    desc("XORI", 3),
    desc("SLT", 3),
    desc("SLTU", 3),
    desc("SEQZ", 2, kPseudo),
    desc("SNEZ", 2, kPseudo),
    desc("FADD.S", 3, MIF_None, MachineMemoryEffect::None, 3),
    desc("FSUB.S", 3, MIF_None, MachineMemoryEffect::None, 3),
    desc("FMUL.S", 3, MIF_None, MachineMemoryEffect::None, 4),
    desc("FDIV.S", 3, MIF_None, MachineMemoryEffect::None, 16),
    desc("FEQ.S", 3, MIF_None, MachineMemoryEffect::None, 3),
    desc("FLT.S", 3, MIF_None, MachineMemoryEffect::None, 3),
    desc("FLE.S", 3, MIF_None, MachineMemoryEffect::None, 3),
    desc("FCVT.S.W", 2, MIF_None, MachineMemoryEffect::None, 3),
    desc("FCVT.W.S", 2, MIF_None, MachineMemoryEffect::None, 3),
    variable_desc("FMV.W.X", 2, MIF_None, MachineMemoryEffect::None, 3, 2, 2),
    variable_desc("FMV.X.W", 2, MIF_None, MachineMemoryEffect::None, 3, 2, 2),
    desc("STORE_OUT_ARG", 3, kPseudo | MIF_SideEffects, MachineMemoryEffect::Write),
    desc("LOAD_IN_ARG", 3, kPseudo, MachineMemoryEffect::Read, 4),
    desc("BNEZ", 2, kBranch | kPseudo),
    desc("BEQZ", 2, kBranch | kPseudo),
    desc("BEQ", 3, kBranch),
    desc("BNE", 3, kBranch),
    desc("BLT", 3, kBranch),
    desc("BGE", 3, kBranch),
    desc("J", 1, kBranch | kPseudo),
    // Calls may carry implicit argument/result registers plus explicit vector
    // CSR clobbers. Pseudo expansion consumes only the one symbol operand.
    variable_desc("CALL", 1, MIF_Call | kSideEffect | kPseudo,
                  MachineMemoryEffect::Unknown, 18, 1, 1, MVS_None,
                  MVS_VL | MVS_VTYPE | MVS_VXRM | MVS_VXSAT | MVS_VSTART),
    vector_desc("PseudoVSETVL", 5, 5, 3, 3, kSetVL, MIF_SideEffects,
                MachineMemoryEffect::None, 1, MVS_None, MVS_VL | MVS_VTYPE),
    vector_desc("PseudoVSETVLI", 4, 4, 2, 2, kSetVLI, MIF_SideEffects,
                MachineMemoryEffect::None, 1, MVS_None, MVS_VL | MVS_VTYPE),
    vector_desc("PseudoVMSET", 3, 3, 1, 1, kMaskSet),
    vector_desc("PseudoVMCLR", 3, 3, 1, 1, kMaskSet),
    // Entry copies may additionally carry implicit uses of not-yet-consumed
    // physical ABI registers. Those uses are allocation barriers, not
    // encoded operands.
    vector_desc("PseudoVMMV", 4, kVariableOperands, 2, 2, kMaskCopy),
    vector_desc("PseudoVMLOGIC", 5, 5, 3, 3, kMaskLogical),
    vector_desc("PseudoVCPOP", 5, 5, 3, 3, kMaskPopCount, MIF_UsesMask),
    vector_desc("PseudoVFIRST", 4, 4, 2, 2, kMaskFirst),
    vector_desc("PseudoVLM", 4, 4, 2, 2, kMaskLoad, MIF_None,
                MachineMemoryEffect::Read, 5),
    vector_desc("PseudoVSM", 4, 4, 2, 2, kMaskStore, MIF_SideEffects,
                MachineMemoryEffect::Write, 1),
    vector_desc("PseudoVMV_V_V", 4, kVariableOperands, 2, 2, kVectorCopy),
    vector_desc("PseudoVFCVT_F_X", 5, 5, 2, 2, kVectorConvert,
                MIF_HasRoundingMode, MachineMemoryEffect::None, 4,
                MVS_VL | MVS_VTYPE | MVS_FRM),
    vector_desc("PseudoVFCVT_RTZ_X_F", 4, 4, 2, 2, kVectorConvert,
                MIF_HasRoundingMode, MachineMemoryEffect::None, 4),
    vector_desc("PseudoVMV_V_X", 5, 5, 3, 3, kSplatVX),
    vector_desc("PseudoVMV_V_I", 5, 5, 3, 3, kSplatVI),
    vector_desc("PseudoVFMV_V_F", 5, 5, 3, 3, kSplatVF),
    vector_desc("PseudoVID", 4, 4, 2, 2, kStep),
    vector_desc("PseudoVMV_V_X_TA", 4, 4, 2, 2, kSplatVXTA),
    vector_desc("PseudoVMV_V_I_TA", 4, 4, 2, 2, kSplatVITA),
    vector_desc("PseudoVFMV_V_F_TA", 4, 4, 2, 2, kSplatVFTA),
    vector_desc("PseudoVID_TA", 3, 3, 1, 1, kStepTA),
    vector_desc("PseudoVINT_VV_TA", 5, 6, 3, 4, kVectorVVTA),
    vector_desc("PseudoVFLOAT_VV_TA", 6, 7, 3, 4, kVectorVVTA,
                MIF_HasRoundingMode, MachineMemoryEffect::None, 4,
                MVS_VL | MVS_VTYPE | MVS_FRM),
    vector_desc("PseudoVLE_TA", 4, 5, 2, 3, kLoadUnitTA, MIF_None,
                MachineMemoryEffect::Read, 5),
    vector_desc("PseudoVCMP_VV_TA", 5, 6, 3, 4, kFinalCompareVV),
    vector_desc("PseudoVINT_VV", 6, 7, 4, 5, kVectorVV),
    vector_desc("PseudoVINT_VX", 6, 7, 4, 5, kVectorVX),
    vector_desc("PseudoVINT_VI", 6, 7, 4, 5, kVectorVI),
    vector_desc("PseudoVFLOAT_VV", 7, 8, 4, 5, kVectorVV, MIF_HasRoundingMode,
                MachineMemoryEffect::None, 4, MVS_VL | MVS_VTYPE | MVS_FRM),
    vector_desc("PseudoVFLOAT_VF", 7, 8, 4, 5, kVectorVF, MIF_HasRoundingMode,
                MachineMemoryEffect::None, 4, MVS_VL | MVS_VTYPE | MVS_FRM),
    vector_desc("PseudoVCMP_VV", 6, 7, 4, 5, kCompareVV),
    vector_desc("PseudoVCMP_VX", 6, 7, 4, 5, kCompareVX),
    vector_desc("PseudoVCMP_VI", 6, 7, 4, 5, kCompareVI),
    vector_desc("PseudoVCMP_VF", 6, 7, 4, 5, kCompareVF),
    vector_desc("PseudoVMERGE_VVM", 6, 6, 4, 4, kMergeVV, MIF_UsesMask),
    vector_desc("PseudoVMERGE_VXM", 6, 6, 4, 4, kMergeVX, MIF_UsesMask),
    vector_desc("PseudoVMERGE_VIM", 6, 6, 4, 4, kMergeVI, MIF_UsesMask),
    vector_desc("PseudoVMERGE_VFM", 6, 6, 4, 4, kMergeVF, MIF_UsesMask),
    vector_desc("PseudoVLE", 5, 6, 3, 4, kLoadUnit, MIF_None, MachineMemoryEffect::Read, 5),
    vector_desc("PseudoVSE", 4, 5, 2, 3, kStoreUnit, MIF_SideEffects,
                MachineMemoryEffect::Write, 1),
    vector_desc("PseudoVLSE", 6, 7, 4, 5, kLoadStrided, MIF_None,
                MachineMemoryEffect::Read, 6),
    vector_desc("PseudoVSSE", 5, 6, 3, 4, kStoreStrided, MIF_SideEffects,
                MachineMemoryEffect::Write, 1),
    vector_desc("PseudoVLSEG2", 6, 6, 4, 4, kLoadSegment2, MIF_UsesMask,
                MachineMemoryEffect::Read, 6),
    vector_desc("PseudoVSSEG2", 6, 6, 4, 4, kStoreSegment2,
                MIF_SideEffects | MIF_UsesMask, MachineMemoryEffect::Write, 1),
    vector_desc("PseudoVLXEI", 6, 7, 4, 5, kLoadIndexed, MIF_None,
                MachineMemoryEffect::Read, 7),
    vector_desc("PseudoVSXEI", 5, 6, 3, 4, kStoreIndexed, MIF_SideEffects,
                MachineMemoryEffect::Write, 1),
    vector_desc("PseudoVEXTRACT", 5, 5, 3, 3, kExtract),
    vector_desc("PseudoVINSERT", 6, 7, 4, 5, kInsert),
    vector_desc("PseudoVSLIDEUP_VX", 6, 7, 4, 5, kVectorVX),
    vector_desc("PseudoVSLIDEUP_VI", 6, 7, 4, 5, kVectorVI),
    vector_desc("PseudoVSLIDEDOWN_VX", 6, 7, 4, 5, kVectorVX),
    vector_desc("PseudoVSLIDEDOWN_VI", 6, 7, 4, 5, kVectorVI),
    vector_desc("PseudoVRGATHER_VV", 6, 7, 4, 5, kVectorVV),
    vector_desc("PseudoVRGATHER_VX", 6, 7, 4, 5, kVectorVX),
    vector_desc("PseudoVRGATHER_VI", 6, 7, 4, 5, kVectorVI),
    vector_desc("PseudoVREDUCE_INT", 5, 6, 3, 4, kReduction),
    vector_desc("PseudoVREDUCE_FLOAT", 6, 7, 3, 4, kReduction,
                MIF_HasRoundingMode, MachineMemoryEffect::None, 6,
                MVS_VL | MVS_VTYPE | MVS_FRM),
    vector_desc("PseudoVSPILL_WHOLE", 5, 5, 2, 2, kWholeSpill,
                MIF_WholeRegister | MIF_SideEffects, MachineMemoryEffect::Write, 8, MVS_VLENB),
    vector_desc("PseudoVRELOAD_WHOLE", 5, 5, 2, 2, kWholeReload,
                MIF_WholeRegister, MachineMemoryEffect::Read, 8, MVS_VLENB),
    vector_final_desc("VSETVLI", 4, 4, 2, 2, kSetVLI, MIF_SideEffects,
                      MachineMemoryEffect::None, 1, MVS_None, MVS_VL | MVS_VTYPE),
    vector_final_desc("VSETIVLI", 4, 4, 2, 2, kSetVLI, MIF_SideEffects,
                      MachineMemoryEffect::None, 1, MVS_None, MVS_VL | MVS_VTYPE),
    vector_final_desc("VMSET.M", 3, 3, 1, 1, kMaskSet),
    vector_final_desc("VMCLR.M", 3, 3, 1, 1, kMaskSet),
    vector_final_desc("VMV.V.X", 4, 4, 2, 2, kSplatVXTA),
    vector_final_desc("VMV.V.I", 4, 4, 2, 2, kSplatVITA),
    vector_final_desc("VFMV.V.F", 4, 4, 2, 2, kSplatVFTA),
    vector_final_desc("VID.V", 3, 3, 1, 1, kStepTA),
    vector_final_desc("VINT.VV", 5, 6, 3, 4, kVectorVVTA),
    vector_final_desc("VFLOAT.VV", 6, 7, 3, 4, kVectorVVTA,
                      MIF_HasRoundingMode, MachineMemoryEffect::None, 4,
                      MVS_VL | MVS_VTYPE | MVS_FRM),
    vector_final_desc("VLE", 4, 5, 2, 3, kLoadUnitTA, MIF_None,
                      MachineMemoryEffect::Read, 5),
    vector_final_desc("VSE", 4, 5, 2, 3, kStoreUnit, MIF_SideEffects,
                      MachineMemoryEffect::Write, 1),
    vector_final_desc("VLSE", 5, 6, 3, 4, kFinalLoadStrided, MIF_None,
                      MachineMemoryEffect::Read, 6),
    vector_final_desc("VSSE", 5, 6, 3, 4, kStoreStrided, MIF_SideEffects,
                      MachineMemoryEffect::Write, 1),
    vector_final_desc("VLSEG2", 6, 6, 4, 4, kLoadSegment2, MIF_UsesMask,
                      MachineMemoryEffect::Read, 6),
    vector_final_desc("VSSEG2", 6, 6, 4, 4, kStoreSegment2,
                      MIF_SideEffects | MIF_UsesMask, MachineMemoryEffect::Write, 1),
    vector_final_desc("VLOXEI32", 5, 6, 3, 4, kFinalLoadIndexed, MIF_None,
                      MachineMemoryEffect::Read, 7),
    vector_final_desc("VSOXEI32", 5, 6, 3, 4, kStoreIndexed, MIF_SideEffects,
                      MachineMemoryEffect::Write, 1),
    vector_final_desc("VMMV.M", 4, 4, 2, 2, kMaskCopy),
    vector_final_desc("VMLOGIC.MM", 5, 5, 3, 3, kMaskLogical),
    vector_final_desc("VCPOP.M", 5, 5, 3, 3, kMaskPopCount, MIF_UsesMask),
    vector_final_desc("VFIRST.M", 4, 4, 2, 2, kMaskFirst),
    vector_final_desc("VLM.V", 4, 4, 2, 2, kMaskLoad, MIF_None,
                      MachineMemoryEffect::Read, 5),
    vector_final_desc("VSM.V", 4, 4, 2, 2, kMaskStore, MIF_SideEffects,
                      MachineMemoryEffect::Write, 1),
    vector_final_desc("VMV.V.V", 4, 4, 2, 2, kVectorCopy),
    vector_final_desc("VCMP.VV", 5, 6, 3, 4, kFinalCompareVV),
    vector_final_desc("VCMP.VX", 5, 6, 3, 4, kFinalCompareVX),
    vector_final_desc("VMERGE.VVM", 6, 6, 4, 4, kMergeVV, MIF_UsesMask),
    vector_final_desc("VSLIDEDOWN.VX", 5, 6, 3, 4, kFinalSlideVX),
    vector_final_desc("VSLIDEDOWN.VI", 5, 6, 3, 4, kFinalSlideVI),
    vector_final_desc("VEXTRACT", 5, 5, 3, 3, kExtract),
    vector_final_desc("VFCVT.F.X", 5, 5, 2, 2, kVectorConvert,
                      MIF_HasRoundingMode, MachineMemoryEffect::None, 4,
                      MVS_VL | MVS_VTYPE | MVS_FRM),
    vector_final_desc("VFCVT.RTZ.X.F", 4, 4, 2, 2, kVectorConvert,
                      MIF_HasRoundingMode, MachineMemoryEffect::None, 4),
    vector_final_desc("VREDUCE.INT", 5, 6, 3, 4, kReduction),
    vector_final_desc("VFREDOSUM", 6, 7, 3, 4, kReduction,
                      MIF_HasRoundingMode, MachineMemoryEffect::None, 6,
                      MVS_VL | MVS_VTYPE | MVS_FRM),
    vector_final_desc("VSNR.V", 3, 3, 2, 2, kFinalWholeSpill,
                      MIF_WholeRegister | MIF_SideEffects,
                      MachineMemoryEffect::Write, 8, MVS_VLENB),
    vector_final_desc("VLNRE32.V", 3, 3, 2, 2, kFinalWholeReload,
                      MIF_WholeRegister, MachineMemoryEffect::Read, 8, MVS_VLENB),
    desc("READ_VLENB", 1),
    desc("LOCAL_LABEL", 1),
    desc("LUI", 2),
    desc("AUIPC", 2),
    desc("ADDI_RELOC", 3),
    desc("JAL", 2, kBranch),
    desc("JALR", 3, MIF_Call | kSideEffect, MachineMemoryEffect::Unknown, 18),
    desc("SLTIU", 3),
    desc("FSGNJ.S", 3),
    desc("LBU", 3, MIF_None, MachineMemoryEffect::Read, 4),
    desc("LW", 3, MIF_None, MachineMemoryEffect::Read, 4),
    desc("LD", 3, MIF_None, MachineMemoryEffect::Read, 4),
    desc("FLW", 3, MIF_None, MachineMemoryEffect::Read, 4),
    desc("SB", 3, MIF_SideEffects, MachineMemoryEffect::Write),
    desc("SW", 3, MIF_SideEffects, MachineMemoryEffect::Write),
    desc("SD", 3, MIF_SideEffects, MachineMemoryEffect::Write),
    desc("FSW", 3, MIF_SideEffects, MachineMemoryEffect::Write),
}};

static_assert(kDescriptors.size() == static_cast<std::size_t>(Opcode::Count),
              "every opcode must have a machine descriptor");

} // namespace

bool MachineInstrDesc::has_flag(MachineInstrFlag flag) const {
    return (flags & static_cast<std::uint32_t>(flag)) != 0;
}

bool MachineInstrDesc::has_exact_operand_count() const {
    return min_operands == max_operands;
}

bool MachineInstrDesc::accepts_operand_count(std::size_t count) const {
    return count >= min_operands && (max_operands == kVariableOperands || count <= max_operands);
}

bool MachineInstrDesc::accepts_explicit_operand_count(std::size_t count) const {
    return count >= min_explicit_operands &&
           (max_explicit_operands == kVariableOperands || count <= max_explicit_operands);
}

bool MachineInstrDesc::implicitly_uses(MachineVectorState state) const {
    return (implicit_uses & static_cast<std::uint32_t>(state)) != 0;
}

bool MachineInstrDesc::implicitly_defines(MachineVectorState state) const {
    return (implicit_defs & static_cast<std::uint32_t>(state)) != 0;
}

bool MachineInstrDesc::may_load() const {
    return memory == MachineMemoryEffect::Read || memory == MachineMemoryEffect::ReadWrite ||
           memory == MachineMemoryEffect::Unknown;
}

bool MachineInstrDesc::may_store() const {
    return memory == MachineMemoryEffect::Write || memory == MachineMemoryEffect::ReadWrite ||
           memory == MachineMemoryEffect::Unknown;
}

std::uint32_t register_class_mask(RegisterClass reg_class) {
    switch (reg_class) {
    case RegisterClass::GPR:
        return MRC_GPR;
    case RegisterClass::FPR32:
        return MRC_FPR32;
    case RegisterClass::VR:
        return MRC_VR;
    case RegisterClass::VMASK:
        return MRC_VMASK;
    case RegisterClass::VRNoV0:
        return MRC_VRNoV0;
    case RegisterClass::VSTATE:
        return MRC_VSTATE;
    }
    return MRC_None;
}

const MachineInstrDesc &instruction_desc(Opcode opcode) {
    const auto index = static_cast<std::size_t>(opcode);
    if (index >= kDescriptors.size()) {
        throw std::out_of_range("invalid MIR opcode");
    }
    return kDescriptors[index];
}

bool machine_instr_may_call(const MachineInstr &instr) {
    if (instruction_desc(instr.opcode()).has_flag(MIF_Call)) {
        return true;
    }
    if (instr.opcode() == Opcode::RISCVJAL && instr.operands().size() == 2 &&
        instr.operands()[0].is_reg() && instr.operands()[0].is_def() &&
        instr.operands()[0].reg_value().name == "ra" &&
        instr.operands()[1].kind() == OperandKind::Symbol) {
        return true;
    }
    if (instr.opcode() != Opcode::MemZero || instr.operands().size() < 3) {
        return false;
    }
    const auto &byte_count = instr.operands()[2];
    return byte_count.kind() == OperandKind::Reg ||
           (byte_count.kind() == OperandKind::Imm &&
            byte_count.int_value() >= kMemZeroMemsetThresholdBytes);
}

} // namespace mir
