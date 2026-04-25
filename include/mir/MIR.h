#pragma once

#include <cstdint>
#include <memory>
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

enum class StackSlotKind {
    Value,
    Alloca,
    PhiTemp,
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
    MemZero,
    Move,
    FMove,
    Add,
    AddW,
    SubW,
    Mul,
    MulW,
    DivW,
    RemW,
    SllI,
    Xor,
    XorI,
    Slt,
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
    StoreOutgoingArg,
    LoadIncomingArg,
    BranchNonZero,
    Jump,
    Call,
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
};

struct TargetInfo {
    std::string arch = "riscv64";
    std::string abi = "lp64d";
    unsigned xlen_bits = 64;
    unsigned flen_bits = 64;
    unsigned stack_align = 16;
};

struct TypeInfo {
    ValueType value_type = ValueType::Void;
    std::string ir;
    std::uint64_t size = 0;
    std::uint64_t align = 1;
};

struct Global {
    std::string name;
    TypeInfo type;
    bool is_const = false;
    std::string initializer;
};

struct StackSlot {
    int id = -1;
    std::string name;
    TypeInfo type;
    StackSlotKind kind = StackSlotKind::Value;
    std::int64_t offset = 0;
};

class MachineOperand final {
  public:
    MachineOperand() = default;

    static MachineOperand reg(std::string name);
    static MachineOperand freg(std::string name);
    static MachineOperand imm(std::int64_t value);
    static MachineOperand float_imm(float value);
    static MachineOperand slot(int id);
    static MachineOperand global(std::string name);
    static MachineOperand block(std::string name);
    static MachineOperand symbol(std::string name);
    static MachineOperand type(ValueType type);
    static MachineOperand text(std::string value);

    OperandKind kind() const;
    const std::string &string_value() const;
    std::int64_t int_value() const;
    float float_value() const;
    int slot_id() const;
    ValueType type_value() const;

  private:
    OperandKind kind_ = OperandKind::Text;
    std::string string_value_;
    std::int64_t int_value_ = 0;
    float float_value_ = 0.0F;
    int slot_id_ = -1;
    ValueType type_value_ = ValueType::Void;
};

class MachineInstr final {
  public:
    MachineInstr() = default;
    MachineInstr(Opcode opcode, std::vector<MachineOperand> operands = {});

    Opcode opcode() const;
    const std::vector<MachineOperand> &operands() const;
    std::vector<MachineOperand> &operands();

  private:
    Opcode opcode_ = Opcode::Comment;
    std::vector<MachineOperand> operands_;
};

class MachineBasicBlock final {
  public:
    explicit MachineBasicBlock(std::string name);

    const std::string &name() const;
    std::vector<MachineInstr> &instructions();
    const std::vector<MachineInstr> &instructions() const;
    void add_instr(Opcode opcode, std::vector<MachineOperand> operands = {});
    void add_instr(MachineInstr instr);

  private:
    std::string name_;
    std::vector<MachineInstr> instructions_;
};

class MachineFunction final {
  public:
    MachineFunction(std::string name, TypeInfo return_type, std::vector<TypeInfo> param_types,
                    bool is_external = false);

    const std::string &name() const;
    const TypeInfo &return_type() const;
    const std::vector<TypeInfo> &param_types() const;
    bool is_external() const;

    MachineBasicBlock *create_block(const std::string &name);
    MachineBasicBlock *get_block(const std::string &name) const;
    std::vector<std::unique_ptr<MachineBasicBlock>> &blocks();
    const std::vector<std::unique_ptr<MachineBasicBlock>> &blocks() const;

    int add_stack_slot(std::string name, TypeInfo type, StackSlotKind kind);
    StackSlot *stack_slot(int id);
    const StackSlot *stack_slot(int id) const;
    std::vector<StackSlot> &stack_slots();
    const std::vector<StackSlot> &stack_slots() const;

    void note_call();
    bool has_call() const;
    void reserve_outgoing_arg_bytes(std::uint64_t bytes);
    std::uint64_t outgoing_arg_bytes() const;
    std::int64_t frame_size() const;
    std::int64_t return_address_offset() const;
    void layout_frame();

  private:
    std::string name_;
    TypeInfo return_type_;
    std::vector<TypeInfo> param_types_;
    bool is_external_ = false;
    bool has_call_ = false;
    std::uint64_t outgoing_arg_bytes_ = 0;
    std::int64_t frame_size_ = 0;
    std::int64_t return_address_offset_ = -1;
    std::vector<StackSlot> stack_slots_;
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
                                     std::vector<TypeInfo> param_types,
                                     bool is_external = false);

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
const char *opcode_name(Opcode opcode);
std::uint64_t align_to(std::uint64_t value, std::uint64_t align);

} // namespace mir
