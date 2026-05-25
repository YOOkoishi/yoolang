#include "../../include/mir/MIR.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace mir {
namespace {

bool starts_with(const std::string &value, const std::string &prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool contains_block(const std::vector<MachineBasicBlock *> &blocks, MachineBasicBlock *block) {
    return std::find(blocks.begin(), blocks.end(), block) != blocks.end();
}

bool contains_reg(const std::vector<Register> &regs, const Register &reg) {
    return std::find(regs.begin(), regs.end(), reg) != regs.end();
}

bool is_conditional_branch(Opcode opcode) {
    switch (opcode) {
    case Opcode::BranchNonZero:
    case Opcode::BranchZero:
    case Opcode::BranchEq:
    case Opcode::BranchNe:
    case Opcode::BranchLT:
    case Opcode::BranchGE:
        return true;
    default:
        return false;
    }
}

std::size_t branch_target_operand_index(Opcode opcode) {
    switch (opcode) {
    case Opcode::BranchNonZero:
    case Opcode::BranchZero:
        return 1;
    case Opcode::BranchEq:
    case Opcode::BranchNe:
    case Opcode::BranchLT:
    case Opcode::BranchGE:
        return 2;
    default:
        return 0;
    }
}

} // namespace

Register Register::virtual_reg(std::uint32_t id, RegisterClass reg_class,
                               ValueType value_type) {
    Register out;
    out.kind = RegisterKind::Virtual;
    out.reg_class = reg_class;
    out.value_type = value_type;
    out.id = id;
    return out;
}

Register Register::physical(std::string name, RegisterClass reg_class, ValueType value_type) {
    Register out;
    out.kind = RegisterKind::Physical;
    out.reg_class = reg_class;
    out.value_type = value_type;
    out.name = std::move(name);
    return out;
}

bool Register::is_virtual() const {
    return kind == RegisterKind::Virtual;
}

bool Register::is_physical() const {
    return kind == RegisterKind::Physical;
}

bool Register::operator==(const Register &other) const {
    if (kind != other.kind || reg_class != other.reg_class) {
        return false;
    }
    if (is_virtual()) {
        return id == other.id;
    }
    return name == other.name;
}

bool Register::operator!=(const Register &other) const {
    return !(*this == other);
}

MachineOperand MachineOperand::reg(std::string name) {
    return make_reg(Register::physical(std::move(name), RegisterClass::GPR), false, true);
}

MachineOperand MachineOperand::freg(std::string name) {
    return make_reg(Register::physical(std::move(name), RegisterClass::FPR32), false, true);
}

MachineOperand MachineOperand::reg(Register reg) {
    return make_reg(std::move(reg), false, true);
}

MachineOperand MachineOperand::reg_def(Register reg) {
    return make_reg(std::move(reg), true, false);
}

MachineOperand MachineOperand::reg_use(Register reg) {
    return make_reg(std::move(reg), false, true);
}

MachineOperand MachineOperand::make_reg(Register reg, bool is_def, bool is_use) {
    MachineOperand out;
    out.kind_ = reg.reg_class == RegisterClass::FPR32 ? OperandKind::FReg : OperandKind::Reg;
    out.reg_value_ = std::move(reg);
    out.is_def_ = is_def;
    out.is_use_ = is_use;
    out.sync_reg_string();
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

const Register &MachineOperand::reg_value() const {
    return reg_value_;
}

bool MachineOperand::is_reg() const {
    return kind_ == OperandKind::Reg || kind_ == OperandKind::FReg;
}

bool MachineOperand::is_def() const {
    return is_def_;
}

bool MachineOperand::is_use() const {
    return is_use_;
}

bool MachineOperand::is_implicit() const {
    return is_implicit_;
}

bool MachineOperand::is_kill() const {
    return is_kill_;
}

bool MachineOperand::is_dead() const {
    return is_dead_;
}

void MachineOperand::set_reg(Register reg) {
    reg_value_ = std::move(reg);
    kind_ = reg_value_.reg_class == RegisterClass::FPR32 ? OperandKind::FReg : OperandKind::Reg;
    sync_reg_string();
}

void MachineOperand::set_is_def(bool value) {
    is_def_ = value;
}

void MachineOperand::set_is_use(bool value) {
    is_use_ = value;
}

void MachineOperand::set_is_implicit(bool value) {
    is_implicit_ = value;
}

void MachineOperand::set_is_kill(bool value) {
    is_kill_ = value;
}

void MachineOperand::set_is_dead(bool value) {
    is_dead_ = value;
}

void MachineOperand::sync_reg_string() {
    string_value_ = register_name(reg_value_);
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

std::vector<Register> MachineInstr::defs() const {
    std::vector<Register> out;
    for (const auto &operand : operands_) {
        if (operand.is_reg() && operand.is_def()) {
            out.push_back(operand.reg_value());
        }
    }
    return out;
}

std::vector<Register> MachineInstr::uses() const {
    std::vector<Register> out;
    for (const auto &operand : operands_) {
        if (operand.is_reg() && operand.is_use()) {
            out.push_back(operand.reg_value());
        }
    }
    return out;
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

void MachineBasicBlock::add_successor(MachineBasicBlock *successor) {
    if (successor != nullptr && !contains_block(successors_, successor)) {
        successors_.push_back(successor);
    }
}

void MachineBasicBlock::add_predecessor(MachineBasicBlock *predecessor) {
    if (predecessor != nullptr && !contains_block(predecessors_, predecessor)) {
        predecessors_.push_back(predecessor);
    }
}

void MachineBasicBlock::clear_cfg_edges() {
    predecessors_.clear();
    successors_.clear();
}

const std::vector<MachineBasicBlock *> &MachineBasicBlock::successors() const {
    return successors_;
}

const std::vector<MachineBasicBlock *> &MachineBasicBlock::predecessors() const {
    return predecessors_;
}

void MachineBasicBlock::set_live_in(std::vector<Register> regs) {
    live_in_ = std::move(regs);
}

void MachineBasicBlock::set_live_out(std::vector<Register> regs) {
    live_out_ = std::move(regs);
}

const std::vector<Register> &MachineBasicBlock::live_in() const {
    return live_in_;
}

const std::vector<Register> &MachineBasicBlock::live_out() const {
    return live_out_;
}

Register MachineRegisterInfo::create_virtual(RegisterClass reg_class, ValueType value_type) {
    Register reg = Register::virtual_reg(next_vreg_id_++, reg_class, value_type);
    virtual_registers_.push_back(reg);
    return reg;
}

const Register *MachineRegisterInfo::virtual_register(std::uint32_t id) const {
    for (const auto &reg : virtual_registers_) {
        if (reg.id == id) {
            return &reg;
        }
    }
    return nullptr;
}

const std::vector<Register> &MachineRegisterInfo::virtual_registers() const {
    return virtual_registers_;
}

void MachineRegisterInfo::set_allocation(Register vreg, Register physical) {
    if (vreg.is_virtual()) {
        allocations_[vreg.id] = std::move(physical);
    }
}

std::optional<Register> MachineRegisterInfo::allocation(Register vreg) const {
    auto found = allocations_.find(vreg.id);
    if (found == allocations_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void MachineRegisterInfo::clear_allocations() {
    allocations_.clear();
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

bool MachineFunction::erase_block(const std::string &name) {
    auto found = block_table_.find(name);
    if (found == block_table_.end()) {
        return false;
    }

    auto block_it = std::find_if(blocks_.begin(), blocks_.end(), [&](const auto &block) {
        return block.get() == found->second;
    });
    if (block_it == blocks_.end() || block_it == blocks_.begin()) {
        return false;
    }

    block_table_.erase(found);
    blocks_.erase(block_it);
    return true;
}

std::vector<std::unique_ptr<MachineBasicBlock>> &MachineFunction::blocks() {
    return blocks_;
}

const std::vector<std::unique_ptr<MachineBasicBlock>> &MachineFunction::blocks() const {
    return blocks_;
}

MachineRegisterInfo &MachineFunction::regs() {
    return regs_;
}

const MachineRegisterInfo &MachineFunction::regs() const {
    return regs_;
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

void MachineFunction::note_used_callee_saved(Register reg) {
    if (!reg.is_physical() || !is_callee_saved_register(reg)) {
        return;
    }
    if (!contains_reg(used_callee_saved_regs_, reg)) {
        used_callee_saved_regs_.push_back(std::move(reg));
    }
}

const std::vector<SavedRegister> &MachineFunction::saved_registers() const {
    return saved_registers_;
}

void MachineFunction::rebuild_cfg() {
    for (auto &block : blocks_) {
        block->clear_cfg_edges();
    }

    for (std::size_t index = 0; index < blocks_.size(); ++index) {
        auto &block = blocks_[index];
        auto &instructions = block->instructions();
        auto add_successor = [&](MachineBasicBlock *succ) {
            block->add_successor(succ);
            if (succ != nullptr) {
                succ->add_predecessor(block.get());
            }
        };
        auto add_block_successor = [&](const MachineOperand &operand) {
            if (operand.kind() == OperandKind::Block) {
                add_successor(get_block(operand.string_value()));
            }
        };
        auto add_fallthrough_successor = [&]() {
            if (index + 1 < blocks_.size()) {
                add_successor(blocks_[index + 1].get());
            }
        };

        if (instructions.empty()) {
            add_fallthrough_successor();
            continue;
        }
        const auto &last = instructions.back();

        if (is_conditional_branch(last.opcode())) {
            const auto target_index = branch_target_operand_index(last.opcode());
            if (last.operands().size() > target_index &&
                last.operands()[target_index].kind() == OperandKind::Block) {
                add_block_successor(last.operands()[target_index]);
            }
            add_fallthrough_successor();
            continue;
        }
        if (last.opcode() == Opcode::Jump) {
            if (!last.operands().empty() && last.operands()[0].kind() == OperandKind::Block) {
                add_block_successor(last.operands()[0]);
            }
            if (instructions.size() >= 2) {
                const auto &prev = instructions[instructions.size() - 2];
                const auto target_index = branch_target_operand_index(prev.opcode());
                if (is_conditional_branch(prev.opcode()) &&
                    prev.operands().size() > target_index) {
                    add_block_successor(prev.operands()[target_index]);
                }
            }
            continue;
        }

        if (instructions.size() >= 2) {
            const auto &prev = instructions[instructions.size() - 2];
            const auto target_index = branch_target_operand_index(prev.opcode());
            if (is_conditional_branch(prev.opcode()) &&
                prev.operands().size() > target_index &&
                prev.operands()[target_index].kind() == OperandKind::Block) {
                add_block_successor(prev.operands()[target_index]);
            }
        }
        add_fallthrough_successor();
    }
}

void MachineFunction::layout_frame() {
    std::uint64_t offset = align_to(outgoing_arg_bytes_, 16);
    for (auto &slot : stack_slots_) {
        offset = align_to(offset, slot.type.align);
        slot.offset = static_cast<std::int64_t>(offset);
        offset += slot.type.size;
    }

    saved_registers_.clear();
    std::sort(used_callee_saved_regs_.begin(), used_callee_saved_regs_.end(),
              [](const Register &lhs, const Register &rhs) {
                  if (lhs.reg_class != rhs.reg_class) {
                      return lhs.reg_class < rhs.reg_class;
                  }
                  return lhs.name < rhs.name;
              });
    for (const auto &reg : used_callee_saved_regs_) {
        offset = align_to(offset, 8);
        SavedRegister saved;
        saved.reg = reg;
        saved.offset = static_cast<std::int64_t>(offset);
        saved_registers_.push_back(saved);
        offset += 8;
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

const char *register_class_name(RegisterClass reg_class) {
    switch (reg_class) {
    case RegisterClass::GPR:
        return "gpr";
    case RegisterClass::FPR32:
        return "fpr32";
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
    case Opcode::LoadMemOffset:
        return "LOAD_MEM_OFF";
    case Opcode::StoreMemOffset:
        return "STORE_MEM_OFF";
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
    case Opcode::AddI:
        return "ADDI";
    case Opcode::AddIW:
        return "ADDIW";
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
    case Opcode::And:
        return "AND";
    case Opcode::AndI:
        return "ANDI";
    case Opcode::Or:
        return "OR";
    case Opcode::SllW:
        return "SLLW";
    case Opcode::SrlW:
        return "SRLW";
    case Opcode::SraW:
        return "SRAW";
    case Opcode::SllI:
        return "SLLI";
    case Opcode::SllIW:
        return "SLLIW";
    case Opcode::SraI:
        return "SRAI";
    case Opcode::SraIW:
        return "SRAIW";
    case Opcode::SrliW:
        return "SRLIW";
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
    case Opcode::BranchZero:
        return "BEQZ";
    case Opcode::BranchEq:
        return "BEQ";
    case Opcode::BranchNe:
        return "BNE";
    case Opcode::BranchLT:
        return "BLT";
    case Opcode::BranchGE:
        return "BGE";
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

RegisterClass register_class_for_physical(const std::string &name) {
    if (!name.empty() && name[0] == 'f') {
        return RegisterClass::FPR32;
    }
    return RegisterClass::GPR;
}

std::string register_name(const Register &reg) {
    if (reg.is_virtual()) {
        return "%v" + std::to_string(reg.id);
    }
    return reg.name;
}

bool is_callee_saved_register(const Register &reg) {
    if (!reg.is_physical()) {
        return false;
    }
    const auto &name = reg.name;
    if (name.size() >= 2 && name[0] == 's' && std::isdigit(static_cast<unsigned char>(name[1]))) {
        return name != "s0";
    }
    if (starts_with(name, "fs")) {
        return true;
    }
    return false;
}

bool is_caller_saved_register(const Register &reg) {
    if (!reg.is_physical()) {
        return false;
    }
    const auto &name = reg.name;
    if (name == "ra") {
        return true;
    }
    if (!name.empty() && name[0] == 't') {
        return true;
    }
    if (!name.empty() && name[0] == 'a') {
        return true;
    }
    if (starts_with(name, "ft") || starts_with(name, "fa")) {
        return true;
    }
    return false;
}

} // namespace mir
