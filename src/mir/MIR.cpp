#include "../../include/mir/MIR.h"

#include <algorithm>
#include <utility>

namespace mir {

MachineOperand MachineOperand::reg(std::string name) {
    MachineOperand out;
    out.kind_ = OperandKind::Reg;
    out.string_value_ = std::move(name);
    return out;
}

MachineOperand MachineOperand::freg(std::string name) {
    MachineOperand out;
    out.kind_ = OperandKind::FReg;
    out.string_value_ = std::move(name);
    return out;
}

MachineOperand MachineOperand::imm(std::int64_t value) {
    MachineOperand out;
    out.kind_ = OperandKind::Imm;
    out.int_value_ = value;
    return out;
}

MachineOperand MachineOperand::float_imm(float value) {
    MachineOperand out;
    out.kind_ = OperandKind::FloatImm;
    out.float_value_ = value;
    return out;
}

MachineOperand MachineOperand::slot(int id) {
    MachineOperand out;
    out.kind_ = OperandKind::Slot;
    out.slot_id_ = id;
    return out;
}

MachineOperand MachineOperand::global(std::string name) {
    MachineOperand out;
    out.kind_ = OperandKind::Global;
    out.string_value_ = std::move(name);
    return out;
}

MachineOperand MachineOperand::block(std::string name) {
    MachineOperand out;
    out.kind_ = OperandKind::Block;
    out.string_value_ = std::move(name);
    return out;
}

MachineOperand MachineOperand::symbol(std::string name) {
    MachineOperand out;
    out.kind_ = OperandKind::Symbol;
    out.string_value_ = std::move(name);
    return out;
}

MachineOperand MachineOperand::type(ValueType type) {
    MachineOperand out;
    out.kind_ = OperandKind::Type;
    out.type_value_ = type;
    return out;
}

MachineOperand MachineOperand::text(std::string value) {
    MachineOperand out;
    out.kind_ = OperandKind::Text;
    out.string_value_ = std::move(value);
    return out;
}

OperandKind MachineOperand::kind() const {
    return kind_;
}

const std::string &MachineOperand::string_value() const {
    return string_value_;
}

std::int64_t MachineOperand::int_value() const {
    return int_value_;
}

float MachineOperand::float_value() const {
    return float_value_;
}

int MachineOperand::slot_id() const {
    return slot_id_;
}

ValueType MachineOperand::type_value() const {
    return type_value_;
}

MachineInstr::MachineInstr(Opcode opcode, std::vector<MachineOperand> operands)
    : opcode_(opcode), operands_(std::move(operands)) {
}

Opcode MachineInstr::opcode() const {
    return opcode_;
}

const std::vector<MachineOperand> &MachineInstr::operands() const {
    return operands_;
}

std::vector<MachineOperand> &MachineInstr::operands() {
    return operands_;
}

MachineBasicBlock::MachineBasicBlock(std::string name) : name_(std::move(name)) {
}

const std::string &MachineBasicBlock::name() const {
    return name_;
}

std::vector<MachineInstr> &MachineBasicBlock::instructions() {
    return instructions_;
}

const std::vector<MachineInstr> &MachineBasicBlock::instructions() const {
    return instructions_;
}

void MachineBasicBlock::add_instr(Opcode opcode, std::vector<MachineOperand> operands) {
    instructions_.emplace_back(opcode, std::move(operands));
}

void MachineBasicBlock::add_instr(MachineInstr instr) {
    instructions_.push_back(std::move(instr));
}

MachineFunction::MachineFunction(std::string name, TypeInfo return_type,
                                 std::vector<TypeInfo> param_types, bool is_external)
    : name_(std::move(name)), return_type_(std::move(return_type)),
      param_types_(std::move(param_types)), is_external_(is_external) {
}

const std::string &MachineFunction::name() const {
    return name_;
}

const TypeInfo &MachineFunction::return_type() const {
    return return_type_;
}

const std::vector<TypeInfo> &MachineFunction::param_types() const {
    return param_types_;
}

bool MachineFunction::is_external() const {
    return is_external_;
}

MachineBasicBlock *MachineFunction::create_block(const std::string &name) {
    auto block = std::make_unique<MachineBasicBlock>(name);
    auto *raw = block.get();
    blocks_.push_back(std::move(block));
    block_table_[name] = raw;
    return raw;
}

MachineBasicBlock *MachineFunction::get_block(const std::string &name) const {
    auto found = block_table_.find(name);
    return found == block_table_.end() ? nullptr : found->second;
}

std::vector<std::unique_ptr<MachineBasicBlock>> &MachineFunction::blocks() {
    return blocks_;
}

const std::vector<std::unique_ptr<MachineBasicBlock>> &MachineFunction::blocks() const {
    return blocks_;
}

int MachineFunction::add_stack_slot(std::string name, TypeInfo type, StackSlotKind kind) {
    StackSlot slot;
    slot.id = static_cast<int>(stack_slots_.size());
    slot.name = std::move(name);
    slot.type = std::move(type);
    slot.kind = kind;
    stack_slots_.push_back(std::move(slot));
    return stack_slots_.back().id;
}

StackSlot *MachineFunction::stack_slot(int id) {
    if (id < 0 || static_cast<std::size_t>(id) >= stack_slots_.size()) {
        return nullptr;
    }
    return &stack_slots_[static_cast<std::size_t>(id)];
}

const StackSlot *MachineFunction::stack_slot(int id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= stack_slots_.size()) {
        return nullptr;
    }
    return &stack_slots_[static_cast<std::size_t>(id)];
}

std::vector<StackSlot> &MachineFunction::stack_slots() {
    return stack_slots_;
}

const std::vector<StackSlot> &MachineFunction::stack_slots() const {
    return stack_slots_;
}

void MachineFunction::note_call() {
    has_call_ = true;
}

bool MachineFunction::has_call() const {
    return has_call_;
}

void MachineFunction::reserve_outgoing_arg_bytes(std::uint64_t bytes) {
    outgoing_arg_bytes_ = std::max(outgoing_arg_bytes_, bytes);
}

std::uint64_t MachineFunction::outgoing_arg_bytes() const {
    return outgoing_arg_bytes_;
}

std::int64_t MachineFunction::frame_size() const {
    return frame_size_;
}

std::int64_t MachineFunction::return_address_offset() const {
    return return_address_offset_;
}

void MachineFunction::layout_frame() {
    std::uint64_t offset = align_to(outgoing_arg_bytes_, 16);
    for (auto &slot : stack_slots_) {
        offset = align_to(offset, slot.type.align);
        slot.offset = static_cast<std::int64_t>(offset);
        offset += slot.type.size;
    }

    if (has_call_) {
        offset = align_to(offset, 8);
        return_address_offset_ = static_cast<std::int64_t>(offset);
        offset += 8;
    } else {
        return_address_offset_ = -1;
    }

    frame_size_ = static_cast<std::int64_t>(align_to(offset, 16));
}

Module::Module(std::string name) : name_(std::move(name)) {
}

const std::string &Module::name() const {
    return name_;
}

const TargetInfo &Module::target() const {
    return target_;
}

TargetInfo &Module::target() {
    return target_;
}

void Module::add_global(Global global) {
    globals_.push_back(std::move(global));
}

MachineFunction *Module::create_function(std::string name, TypeInfo return_type,
                                         std::vector<TypeInfo> param_types, bool is_external) {
    auto function = std::make_unique<MachineFunction>(std::move(name), std::move(return_type),
                                                      std::move(param_types), is_external);
    auto *raw = function.get();
    functions_.push_back(std::move(function));
    return raw;
}

std::vector<Global> &Module::globals() {
    return globals_;
}

const std::vector<Global> &Module::globals() const {
    return globals_;
}

std::vector<std::unique_ptr<MachineFunction>> &Module::functions() {
    return functions_;
}

const std::vector<std::unique_ptr<MachineFunction>> &Module::functions() const {
    return functions_;
}

const char *value_type_name(ValueType type) {
    switch (type) {
    case ValueType::Void:
        return "void";
    case ValueType::I1:
        return "i1";
    case ValueType::I32:
        return "i32";
    case ValueType::F32:
        return "f32";
    case ValueType::Ptr:
        return "ptr";
    case ValueType::Aggregate:
        return "aggregate";
    }
    return "unknown";
}

const char *opcode_name(Opcode opcode) {
    switch (opcode) {
    case Opcode::Comment:
        return "COMMENT";
    case Opcode::LoadImm:
        return "LI";
    case Opcode::LoadFloatImm:
        return "FLI.S";
    case Opcode::LoadGlobalAddr:
        return "LA";
    case Opcode::LoadStackAddr:
        return "FI_ADDR";
    case Opcode::LoadSlot:
        return "LOAD_SLOT";
    case Opcode::StoreSlot:
        return "STORE_SLOT";
    case Opcode::LoadMem:
        return "LOAD_MEM";
    case Opcode::StoreMem:
        return "STORE_MEM";
    case Opcode::MemZero:
        return "MEMZERO";
    case Opcode::Move:
        return "MV";
    case Opcode::FMove:
        return "FMV.S";
    case Opcode::Add:
        return "ADD";
    case Opcode::AddW:
        return "ADDW";
    case Opcode::SubW:
        return "SUBW";
    case Opcode::Mul:
        return "MUL";
    case Opcode::MulW:
        return "MULW";
    case Opcode::DivW:
        return "DIVW";
    case Opcode::RemW:
        return "REMW";
    case Opcode::SllI:
        return "SLLI";
    case Opcode::Xor:
        return "XOR";
    case Opcode::XorI:
        return "XORI";
    case Opcode::Slt:
        return "SLT";
    case Opcode::SeqZ:
        return "SEQZ";
    case Opcode::Snez:
        return "SNEZ";
    case Opcode::FAddS:
        return "FADD.S";
    case Opcode::FSubS:
        return "FSUB.S";
    case Opcode::FMulS:
        return "FMUL.S";
    case Opcode::FDivS:
        return "FDIV.S";
    case Opcode::FeqS:
        return "FEQ.S";
    case Opcode::FltS:
        return "FLT.S";
    case Opcode::FleS:
        return "FLE.S";
    case Opcode::FcvtSW:
        return "FCVT.S.W";
    case Opcode::FcvtWS:
        return "FCVT.W.S";
    case Opcode::FmvWX:
        return "FMV.W.X";
    case Opcode::StoreOutgoingArg:
        return "STORE_OUT_ARG";
    case Opcode::LoadIncomingArg:
        return "LOAD_IN_ARG";
    case Opcode::BranchNonZero:
        return "BNEZ";
    case Opcode::Jump:
        return "J";
    case Opcode::Call:
        return "CALL";
    }
    return "UNKNOWN";
}

std::uint64_t align_to(std::uint64_t value, std::uint64_t align) {
    if (align == 0 || align == 1) {
        return value;
    }
    return ((value + align - 1) / align) * align;
}

} // namespace mir
