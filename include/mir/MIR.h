#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mir {

enum class ValueType {
    Void,
    I1,
    I32,
    F32,
    Ptr,
    Aggregate,
};

enum class VectorLMUL {
    MF8,
    MF4,
    MF2,
    M1,
    M2,
    M4,
    M8,
};

enum class VectorContainerKind {
    Scalable,
    Fixed,
};

class MachineVectorType final {
  public:
    static MachineVectorType scalable(ValueType element_type, unsigned sew_bits, VectorLMUL lmul,
                                      unsigned mask_ratio = 0);
    static MachineVectorType fixed(ValueType element_type, unsigned sew_bits, VectorLMUL lmul,
                                   unsigned fixed_lanes, unsigned mask_ratio = 0);
    static MachineVectorType mask_for(const MachineVectorType &data_type);

    ValueType element_type() const;
    unsigned sew_bits() const;
    VectorLMUL lmul() const;
    VectorContainerKind container_kind() const;
    unsigned fixed_lanes() const;
    unsigned fixed_bits() const;
    unsigned mask_ratio() const;
    bool is_scalable() const;
    bool is_fixed() const;
    bool is_mask() const;
    unsigned lmul_eighths() const;
    unsigned register_group_width() const;
    unsigned register_group_alignment() const;
    bool operator==(const MachineVectorType &other) const;
    bool operator!=(const MachineVectorType &other) const;

  private:
    MachineVectorType(ValueType element_type, unsigned sew_bits, VectorLMUL lmul,
                      VectorContainerKind container_kind, unsigned fixed_lanes,
                      unsigned mask_ratio);

    ValueType element_type_ = ValueType::I32;
    unsigned sew_bits_ = 32;
    VectorLMUL lmul_ = VectorLMUL::M1;
    VectorContainerKind container_kind_ = VectorContainerKind::Scalable;
    unsigned fixed_lanes_ = 0;
    unsigned fixed_bits_ = 0;
    unsigned mask_ratio_ = 0;
};

struct MachineScalableSize {
    // One unit is vlenb/8.  This can describe either a logical vlenb*LMUL
    // quantity or the integral physical register groups used by whole-register
    // spill slots without pretending VLEN is fixed.
    unsigned vlenb_eighths = 0;

    static MachineScalableSize from_lmul(VectorLMUL lmul);
    // Whole-register spill storage is sized by the physical register group,
    // not the logical LMUL.  Fractional LMUL values and masks still occupy
    // one complete architectural vector register when saved by vs1r.v.
    static MachineScalableSize from_register_group_width(unsigned width);
    bool is_valid() const;
    bool operator==(const MachineScalableSize &other) const;
    bool operator!=(const MachineScalableSize &other) const;
};

enum class VectorAVLKind {
    Unspecified,
    CurrentVL,
    Operand,
    WholeRegister,
};

struct MachineVectorAVL {
    VectorAVLKind kind = VectorAVLKind::Unspecified;
    std::size_t operand_index = 0;

    static MachineVectorAVL current_vl();
    static MachineVectorAVL operand(std::size_t index);
    static MachineVectorAVL whole_register();
};

enum class VectorTailPolicy {
    Unspecified,
    Agnostic,
    Undisturbed,
};

enum class VectorMaskPolicy {
    Unspecified,
    Agnostic,
    Undisturbed,
};

enum class VectorRoundingMode {
    None,
    RNE,
    RTZ,
    RDN,
    RUP,
    RMM,
    Dynamic,
};

enum class RVVOperation {
    None,
    SetVL,
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    And,
    Or,
    Xor,
    Min,
    Max,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Merge,
    Load,
    Store,
    Extract,
    Insert,
    SlideUp,
    SlideDown,
    Gather,
    ReduceSum,
    ReduceMin,
    ReduceMax,
    ReduceAnd,
    ReduceOr,
    ReduceXor,
    MaskPopulationCount,
    MaskFirst,
    Spill,
    Reload,
    MaskSet,
    MaskClear,
    MaskAnd,
    MaskOr,
    MaskXor,
    MaskNot,
    Copy,
    ConvertSIToFP,
    ConvertFPToSI,
    Splat,
    Step,
};

struct MachineVectorInfo {
    explicit MachineVectorInfo(MachineVectorType vector_type);

    MachineVectorType vector_type;
    RVVOperation operation = RVVOperation::None;
    MachineVectorAVL avl;
    VectorTailPolicy tail_policy = VectorTailPolicy::Unspecified;
    VectorMaskPolicy mask_policy = VectorMaskPolicy::Unspecified;
    VectorRoundingMode rounding = VectorRoundingMode::None;
    std::optional<MachineVectorType> index_vector_type;
    std::optional<std::size_t> passthrough_operand;
    std::optional<std::size_t> mask_operand;
    // Stable identity of the actual VL established by a vset* instruction.
    // The CFG-wide vector-state pass assigns this before scalar register
    // allocation so later verification never mistakes two unrelated AVL
    // values that happened to receive the same physical GPR for one value.
    // Policy-only vsetvli x0, x0 instructions inherit the current identity.
    std::uint64_t vl_identity = 0;
};

enum class StackSlotKind {
    Value,
    Alloca,
    PhiTemp,
    Spill,
    CalleeSaved,
};

enum class RegisterKind {
    Virtual,
    Physical,
};

enum class RegisterClass {
    GPR,
    FPR32,
    VR,
    VMASK,
    VRNoV0,
    VSTATE,
};

enum class Opcode {
    Comment,
    LoadImm,
    LoadFloatImm,
    LoadGlobalAddr,
    LoadStackAddr,
    LoadSlot,
    StoreSlot,
    LoadMem,
    StoreMem,
    LoadMemOffset,
    StoreMemOffset,
    MemZero,
    Move,
    FMove,
    Add,
    AddW,
    AddI,
    AddIW,
    Sub,
    SubW,
    Mul,
    MulW,
    DivU,
    DivW,
    RemW,
    And,
    AndI,
    SllI,
    SllIW,
    SraI,
    SraIW,
    Srli,
    SrliW,
    Xor,
    XorI,
    Slt,
    Sltu,
    SeqZ,
    Snez,
    FAddS,
    FSubS,
    FMulS,
    FDivS,
    FeqS,
    FltS,
    FleS,
    FcvtSW,
    FcvtWS,
    FmvWX,
    FmvXW,
    StoreOutgoingArg,
    LoadIncomingArg,
    BranchNonZero,
    BranchZero,
    BranchEq,
    BranchNe,
    BranchLT,
    BranchGE,
    Jump,
    Call,
    RVVSetVL,
    RVVSetVLI,
    RVVMaskSet,
    RVVMaskClear,
    RVVMaskCopy,
    RVVMaskLogical,
    RVVMaskPopCount,
    RVVMaskFirst,
    RVVMaskLoad,
    RVVMaskStore,
    RVVVectorCopy,
    RVVSIToFP,
    RVVFPToSI,
    RVVSplatVX,
    RVVSplatVI,
    RVVSplatVF,
    RVVStep,
    RVVSplatVXTA,
    RVVSplatVITA,
    RVVSplatVFTA,
    RVVStepTA,
    RVVIntBinaryVVTA,
    RVVFloatBinaryVVTA,
    RVVLoadUnitTA,
    RVVCompareVVTA,
    RVVIntBinaryVV,
    RVVIntBinaryVX,
    RVVIntBinaryVI,
    RVVFloatBinaryVV,
    RVVFloatBinaryVF,
    RVVCompareVV,
    RVVCompareVX,
    RVVCompareVI,
    RVVCompareVF,
    RVVMergeVVM,
    RVVMergeVXM,
    RVVMergeVIM,
    RVVMergeVFM,
    RVVLoadUnit,
    RVVStoreUnit,
    RVVLoadStridedTA,
    RVVLoadStrided,
    RVVStoreStrided,
    RVVLoadSegment2,
    RVVStoreSegment2,
    RVVLoadIndexed,
    RVVStoreIndexed,
    RVVExtractElement,
    RVVInsertElement,
    RVVSlideUpVX,
    RVVSlideUpVI,
    RVVSlideDownVX,
    RVVSlideDownVI,
    RVVGatherVV,
    RVVGatherVX,
    RVVGatherVI,
    RVVReductionInt,
    RVVReductionFloat,
    RVVWholeRegSpill,
    RVVWholeRegReload,
    RISCVVSetVLI,
    RISCVVSetIVLI,
    RISCVVMaskSet,
    RISCVVMaskClear,
    RISCVVSplatVX,
    RISCVVSplatVI,
    RISCVVSplatVF,
    RISCVVStep,
    RISCVVIntBinaryVV,
    RISCVVFloatBinaryVV,
    RISCVVLoadUnit,
    RISCVVStoreUnit,
    RISCVVLoadStrided,
    RISCVVStoreStrided,
    RISCVVLoadSegment2,
    RISCVVStoreSegment2,
    RISCVVLoadIndexedOrdered,
    RISCVVStoreIndexedOrdered,
    RISCVVMaskCopy,
    RISCVVMaskLogical,
    RISCVVMaskPopCount,
    RISCVVMaskFirst,
    RISCVVMaskLoad,
    RISCVVMaskStore,
    RISCVVVectorCopy,
    RISCVVCompareVV,
    RISCVVCompareVX,
    RISCVVMergeVVM,
    RISCVVSlideDownVX,
    RISCVVSlideDownVI,
    RISCVVExtractElement,
    RISCVVSIToFP,
    RISCVVFPToSI,
    RISCVVReductionInt,
    RISCVVReductionFloatOrdered,
    RISCVVWholeRegSpill,
    RISCVVWholeRegReload,
    RISCVReadVLENB,
    RISCVLocalLabel,
    RISCVLUI,
    RISCVAUIPC,
    RISCVAddiReloc,
    RISCVJAL,
    RISCVJALR,
    RISCVSLTIU,
    RISCVFSGNJS,
    RISCVLBU,
    RISCVLW,
    RISCVLD,
    RISCVFLW,
    RISCVSB,
    RISCVSW,
    RISCVSD,
    RISCVFSW,
    Count,
};

enum class OperandKind {
    Reg,
    FReg,
    Imm,
    FloatImm,
    Slot,
    Global,
    Block,
    Symbol,
    Type,
    Text,
    Reloc,
};

enum class RelocationKind {
    PCRelHi,
    PCRelLo,
};

struct TargetInfo {
    std::string triple = "riscv64-unknown-linux-gnu";
    std::string arch = "rv64gc";
    std::string abi = "lp64d";
    std::string cpu = "generic-rv64";
    std::string tune = "generic-rv64";
    unsigned xlen_bits = 64;
    unsigned flen_bits = 64;
    unsigned minimum_vlen_bits = 0;
    unsigned abi_vlen_bits = 0;
    unsigned stack_align = 16;
    bool has_vector = false;
    bool psabi_vector = false;
};

inline constexpr std::int64_t kMemZeroMemsetThresholdBytes = 256;

struct TypeInfo {
    ValueType value_type = ValueType::Void;
    std::string ir;
    std::uint64_t size = 0;
    std::uint64_t align = 1;
    std::optional<MachineVectorType> vector_type;
};

struct Global {
    std::string name;
    TypeInfo type;
    bool is_const = false;
    // Exact target-memory representation in little-endian byte order.  An
    // empty buffer denotes an all-zero initializer of `type.size` bytes.
    // Textual IR initializers must be decoded before entering MIR.
    std::vector<std::uint8_t> initializer_bytes;
};

struct Register {
    RegisterKind kind = RegisterKind::Physical;
    RegisterClass reg_class = RegisterClass::GPR;
    ValueType value_type = ValueType::Void;
    std::uint32_t id = 0;
    std::string name;
    std::optional<MachineVectorType> vector_type;
    unsigned vector_group_width = 1;

    static Register virtual_reg(std::uint32_t id, RegisterClass reg_class, ValueType value_type);
    static Register virtual_vector(std::uint32_t id, RegisterClass reg_class,
                                   MachineVectorType vector_type);
    static Register physical(std::string name, RegisterClass reg_class,
                             ValueType value_type = ValueType::Void);
    static Register physical_vector(std::string name, RegisterClass reg_class,
                                    MachineVectorType vector_type);

    bool is_virtual() const;
    bool is_physical() const;
    bool is_vector() const;
    bool operator==(const Register &other) const;
    bool operator!=(const Register &other) const;
};

struct StackSlot {
    int id = -1;
    std::string name;
    TypeInfo type;
    StackSlotKind kind = StackSlotKind::Value;
    std::int64_t offset = 0;
    bool has_fixed_offset = true;
    std::optional<MachineScalableSize> scalable_size;
    std::optional<MachineScalableSize> scalable_align;
    std::optional<MachineScalableSize> scalable_offset;
};

struct SavedRegister {
    Register reg;
    std::int64_t offset = 0;
};

struct SavedVectorRegister {
    Register reg;
    int stack_slot = -1;
};

class MachineOperand final {
  public:
    MachineOperand() = default;

    static MachineOperand reg(std::string name);
    static MachineOperand freg(std::string name);
    static MachineOperand reg(Register reg);
    static MachineOperand reg_def(Register reg);
    static MachineOperand reg_use(Register reg);
    static MachineOperand implicit_reg_def(Register reg);
    static MachineOperand implicit_reg_use(Register reg);
    static MachineOperand imm(std::int64_t value);
    static MachineOperand float_imm(float value);
    static MachineOperand slot(int id);
    static MachineOperand global(std::string name);
    static MachineOperand block(std::string name);
    static MachineOperand symbol(std::string name);
    static MachineOperand type(ValueType type);
    static MachineOperand text(std::string value);
    static MachineOperand reloc(RelocationKind kind, std::string symbol);

    OperandKind kind() const;
    const std::string &string_value() const;
    std::int64_t int_value() const;
    float float_value() const;
    int slot_id() const;
    ValueType type_value() const;
    RelocationKind relocation_kind() const;
    const Register &reg_value() const;
    bool is_reg() const;
    bool is_def() const;
    bool is_use() const;
    bool is_implicit() const;
    bool is_kill() const;
    bool is_dead() const;
    void set_reg(Register reg);
    void set_is_def(bool value);
    void set_is_use(bool value);
    void set_is_implicit(bool value);
    void set_is_kill(bool value);
    void set_is_dead(bool value);

  private:
    static MachineOperand make_reg(Register reg, bool is_def, bool is_use);
    void sync_reg_string();

    OperandKind kind_ = OperandKind::Text;
    Register reg_value_;
    std::string string_value_;
    std::int64_t int_value_ = 0;
    float float_value_ = 0.0F;
    int slot_id_ = -1;
    ValueType type_value_ = ValueType::Void;
    RelocationKind relocation_kind_ = RelocationKind::PCRelHi;
    bool is_def_ = false;
    bool is_use_ = false;
    bool is_implicit_ = false;
    bool is_kill_ = false;
    bool is_dead_ = false;
};

class MachineInstr final {
  public:
    MachineInstr() = default;
    MachineInstr(Opcode opcode, std::vector<MachineOperand> operands = {},
                 std::optional<MachineVectorInfo> vector_info = std::nullopt);

    Opcode opcode() const;
    const std::vector<MachineOperand> &operands() const;
    std::vector<MachineOperand> &operands();
    std::vector<Register> defs() const;
    std::vector<Register> uses() const;
    bool has_vector_info() const;
    const MachineVectorInfo &vector_info() const;
    MachineVectorInfo &vector_info();
    void set_vector_info(MachineVectorInfo vector_info);
    void clear_vector_info();
    void set_variant_cc_call(bool value);
    bool is_variant_cc_call() const;

  private:
    Opcode opcode_ = Opcode::Comment;
    std::vector<MachineOperand> operands_;
    std::optional<MachineVectorInfo> vector_info_;
    bool variant_cc_call_ = false;
};

class MachineBasicBlock final {
  public:
    explicit MachineBasicBlock(std::string name);

    const std::string &name() const;
    std::vector<MachineInstr> &instructions();
    const std::vector<MachineInstr> &instructions() const;
    void add_instr(Opcode opcode, std::vector<MachineOperand> operands = {});
    void add_instr(MachineInstr instr);
    void add_successor(MachineBasicBlock *successor);
    void add_predecessor(MachineBasicBlock *predecessor);
    void clear_cfg_edges();
    const std::vector<MachineBasicBlock *> &successors() const;
    const std::vector<MachineBasicBlock *> &predecessors() const;
    void set_live_in(std::vector<Register> regs);
    void set_live_out(std::vector<Register> regs);
    const std::vector<Register> &live_in() const;
    const std::vector<Register> &live_out() const;

  private:
    std::string name_;
    std::vector<MachineInstr> instructions_;
    std::vector<MachineBasicBlock *> predecessors_;
    std::vector<MachineBasicBlock *> successors_;
    std::vector<Register> live_in_;
    std::vector<Register> live_out_;
};

class MachineRegisterInfo final {
  public:
    Register create_virtual(RegisterClass reg_class, ValueType value_type);
    Register create_virtual_vector(RegisterClass reg_class, MachineVectorType vector_type);
    const Register *virtual_register(std::uint32_t id) const;
    const std::vector<Register> &virtual_registers() const;
    void set_allocation(Register vreg, Register physical);
    std::optional<Register> allocation(Register vreg) const;
    void clear_allocations();

  private:
    std::uint32_t next_vreg_id_ = 0;
    std::vector<Register> virtual_registers_;
    std::unordered_map<std::uint32_t, Register> allocations_;
};

class MachineFunction final {
  public:
    MachineFunction(std::string name, TypeInfo return_type, std::vector<TypeInfo> param_types,
                    bool is_external = false);

    const std::string &name() const;
    const TypeInfo &return_type() const;
    const std::vector<TypeInfo> &param_types() const;
    bool is_external() const;
    void set_variant_cc(bool value);
    bool is_variant_cc() const;

    MachineBasicBlock *create_block(const std::string &name);
    MachineBasicBlock *get_block(const std::string &name) const;
    bool erase_block(const std::string &name);
    std::vector<std::unique_ptr<MachineBasicBlock>> &blocks();
    const std::vector<std::unique_ptr<MachineBasicBlock>> &blocks() const;
    MachineRegisterInfo &regs();
    const MachineRegisterInfo &regs() const;

    int add_stack_slot(std::string name, TypeInfo type, StackSlotKind kind);
    int add_scalable_stack_slot(std::string name, MachineVectorType vector_type,
                                StackSlotKind kind);
    StackSlot *stack_slot(int id);
    const StackSlot *stack_slot(int id) const;
    std::vector<StackSlot> &stack_slots();
    const std::vector<StackSlot> &stack_slots() const;

    void note_call();
    bool has_call() const;
    void note_standard_aggregate_call();
    bool has_standard_aggregate_call() const;
    void reserve_outgoing_arg_bytes(std::uint64_t bytes);
    std::uint64_t outgoing_arg_bytes() const;
    std::int64_t frame_size() const;
    MachineScalableSize scalable_frame_size() const;
    std::int64_t return_address_offset() const;
    void note_used_callee_saved(Register reg);
    const std::vector<SavedRegister> &saved_registers() const;
    void note_used_vector_callee_saved(Register reg);
    const std::vector<SavedVectorRegister> &saved_vector_registers() const;
    void rebuild_cfg();
    void layout_frame();

  private:
    std::string name_;
    TypeInfo return_type_;
    std::vector<TypeInfo> param_types_;
    bool is_external_ = false;
    bool is_variant_cc_ = false;
    bool has_call_ = false;
    bool has_standard_aggregate_call_ = false;
    std::uint64_t outgoing_arg_bytes_ = 0;
    std::int64_t frame_size_ = 0;
    MachineScalableSize scalable_frame_size_;
    std::int64_t return_address_offset_ = -1;
    MachineRegisterInfo regs_;
    std::vector<StackSlot> stack_slots_;
    std::vector<Register> used_callee_saved_regs_;
    std::vector<SavedRegister> saved_registers_;
    std::vector<SavedVectorRegister> saved_vector_registers_;
    std::vector<std::unique_ptr<MachineBasicBlock>> blocks_;
    std::unordered_map<std::string, MachineBasicBlock *> block_table_;
};

class Module final {
  public:
    explicit Module(std::string name);

    const std::string &name() const;
    const TargetInfo &target() const;
    TargetInfo &target();

    void add_global(Global global);
    MachineFunction *create_function(std::string name, TypeInfo return_type,
                                     std::vector<TypeInfo> param_types, bool is_external = false);

    std::vector<Global> &globals();
    const std::vector<Global> &globals() const;
    std::vector<std::unique_ptr<MachineFunction>> &functions();
    const std::vector<std::unique_ptr<MachineFunction>> &functions() const;

  private:
    std::string name_;
    TargetInfo target_;
    std::vector<Global> globals_;
    std::vector<std::unique_ptr<MachineFunction>> functions_;
};

const char *value_type_name(ValueType type);
const char *register_class_name(RegisterClass reg_class);
const char *vector_lmul_name(VectorLMUL lmul);
const char *vector_tail_policy_name(VectorTailPolicy policy);
const char *vector_mask_policy_name(VectorMaskPolicy policy);
const char *vector_rounding_mode_name(VectorRoundingMode mode);
const char *rvv_operation_name(RVVOperation operation);
std::string machine_vector_type_name(const MachineVectorType &type);
std::string machine_scalable_size_name(MachineScalableSize size);
const char *opcode_name(Opcode opcode);
std::uint64_t align_to(std::uint64_t value, std::uint64_t align);
RegisterClass register_class_for_physical(const std::string &name);
std::string register_name(const Register &reg);
bool is_callee_saved_register(const Register &reg);
bool is_caller_saved_register(const Register &reg);

} // namespace mir
