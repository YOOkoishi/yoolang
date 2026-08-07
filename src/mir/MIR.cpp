#include "mir/MIR.h"
#include "mir/MachineInstrDesc.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
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

bool is_power_of_two(unsigned value) {
    return value != 0 && (value & (value - 1U)) == 0;
}

unsigned lmul_eighths(VectorLMUL lmul) {
    switch (lmul) {
    case VectorLMUL::MF8:
        return 1;
    case VectorLMUL::MF4:
        return 2;
    case VectorLMUL::MF2:
        return 4;
    case VectorLMUL::M1:
        return 8;
    case VectorLMUL::M2:
        return 16;
    case VectorLMUL::M4:
        return 32;
    case VectorLMUL::M8:
        return 64;
    }
    return 0;
}

bool is_vector_register_class(RegisterClass reg_class) {
    return reg_class == RegisterClass::VR || reg_class == RegisterClass::VMASK ||
           reg_class == RegisterClass::VRNoV0;
}

void validate_vector_register_class(RegisterClass reg_class, const MachineVectorType &type) {
    if (!is_vector_register_class(reg_class)) {
        throw std::invalid_argument("machine vector register requires a vector register class");
    }
    if (reg_class == RegisterClass::VMASK && !type.is_mask()) {
        throw std::invalid_argument("VMASK register requires an i1 machine vector type");
    }
    if (reg_class != RegisterClass::VMASK && type.is_mask()) {
        throw std::invalid_argument("data vector register cannot carry a mask machine type");
    }
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

MachineVectorType::MachineVectorType(ValueType element_type, unsigned sew_bits, VectorLMUL lmul,
                                     VectorContainerKind container_kind, unsigned fixed_lanes,
                                     unsigned mask_ratio)
    : element_type_(element_type), sew_bits_(sew_bits), lmul_(lmul),
      container_kind_(container_kind), fixed_lanes_(fixed_lanes), mask_ratio_(mask_ratio) {
    if (element_type != ValueType::I1 && element_type != ValueType::I32 &&
        element_type != ValueType::F32) {
        throw std::invalid_argument("machine vector element type must be i1, i32, or f32");
    }
    if (sew_bits != 8 && sew_bits != 16 && sew_bits != 32 && sew_bits != 64) {
        throw std::invalid_argument("RVV SEW must be one of 8, 16, 32, or 64");
    }
    if ((element_type == ValueType::I32 || element_type == ValueType::F32) && sew_bits != 32) {
        throw std::invalid_argument("i32/f32 machine vectors require SEW=32");
    }
    if (container_kind == VectorContainerKind::Scalable && fixed_lanes != 0) {
        throw std::invalid_argument("scalable machine vector cannot have fixed logical lanes");
    }
    if (container_kind == VectorContainerKind::Fixed) {
        if (fixed_lanes == 0) {
            throw std::invalid_argument("fixed machine vector requires at least one logical lane");
        }
        const auto logical_element_bits = element_type == ValueType::I1 ? 1U : 32U;
        if (fixed_lanes > std::numeric_limits<unsigned>::max() / logical_element_bits) {
            throw std::invalid_argument("fixed machine vector logical bit count overflows");
        }
        fixed_bits_ = fixed_lanes * logical_element_bits;
    }

    if (element_type != ValueType::I1) {
        if (mask_ratio != 0) {
            throw std::invalid_argument("data machine vector cannot have a mask ratio");
        }
        return;
    }

    const auto scaled_sew = sew_bits * 8U;
    const auto lmul_units = ::mir::lmul_eighths(lmul);
    if (lmul_units == 0 || scaled_sew % lmul_units != 0) {
        throw std::invalid_argument("mask ratio is not integral for SEW/LMUL");
    }
    const auto expected_ratio = scaled_sew / lmul_units;
    if (!is_power_of_two(expected_ratio) || expected_ratio > 64 || mask_ratio != expected_ratio) {
        throw std::invalid_argument("mask ratio must match a legal RVV SEW/LMUL vbool ratio");
    }
}

MachineVectorType MachineVectorType::scalable(ValueType element_type, unsigned sew_bits,
                                              VectorLMUL lmul, unsigned mask_ratio) {
    return MachineVectorType(element_type, sew_bits, lmul, VectorContainerKind::Scalable, 0,
                             mask_ratio);
}

MachineVectorType MachineVectorType::fixed(ValueType element_type, unsigned sew_bits,
                                           VectorLMUL lmul, unsigned fixed_lanes,
                                           unsigned mask_ratio) {
    return MachineVectorType(element_type, sew_bits, lmul, VectorContainerKind::Fixed,
                             fixed_lanes, mask_ratio);
}

MachineVectorType MachineVectorType::mask_for(const MachineVectorType &data_type) {
    if (data_type.is_mask()) {
        throw std::invalid_argument("cannot derive a mask type from another mask type");
    }
    const auto ratio = data_type.sew_bits() * 8U / data_type.lmul_eighths();
    if (data_type.is_scalable()) {
        return scalable(ValueType::I1, data_type.sew_bits(), data_type.lmul(), ratio);
    }
    return fixed(ValueType::I1, data_type.sew_bits(), data_type.lmul(), data_type.fixed_lanes(),
                 ratio);
}

ValueType MachineVectorType::element_type() const {
    return element_type_;
}

unsigned MachineVectorType::sew_bits() const {
    return sew_bits_;
}

VectorLMUL MachineVectorType::lmul() const {
    return lmul_;
}

VectorContainerKind MachineVectorType::container_kind() const {
    return container_kind_;
}

unsigned MachineVectorType::fixed_lanes() const {
    return fixed_lanes_;
}

unsigned MachineVectorType::fixed_bits() const {
    return fixed_bits_;
}

unsigned MachineVectorType::mask_ratio() const {
    return mask_ratio_;
}

bool MachineVectorType::is_scalable() const {
    return container_kind_ == VectorContainerKind::Scalable;
}

bool MachineVectorType::is_fixed() const {
    return container_kind_ == VectorContainerKind::Fixed;
}

bool MachineVectorType::is_mask() const {
    return element_type_ == ValueType::I1;
}

unsigned MachineVectorType::lmul_eighths() const {
    return ::mir::lmul_eighths(lmul_);
}

unsigned MachineVectorType::register_group_width() const {
    if (is_mask()) {
        return 1;
    }
    return std::max(1U, lmul_eighths() / 8U);
}

unsigned MachineVectorType::register_group_alignment() const {
    return register_group_width();
}

bool MachineVectorType::operator==(const MachineVectorType &other) const {
    return element_type_ == other.element_type_ && sew_bits_ == other.sew_bits_ &&
           lmul_ == other.lmul_ && container_kind_ == other.container_kind_ &&
           fixed_lanes_ == other.fixed_lanes_ && fixed_bits_ == other.fixed_bits_ &&
           mask_ratio_ == other.mask_ratio_;
}

bool MachineVectorType::operator!=(const MachineVectorType &other) const {
    return !(*this == other);
}

MachineScalableSize MachineScalableSize::from_lmul(VectorLMUL lmul) {
    return {::mir::lmul_eighths(lmul)};
}

MachineScalableSize MachineScalableSize::from_register_group_width(unsigned width) {
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        throw std::invalid_argument("scalable spill register-group width must be 1, 2, 4, or 8");
    }
    return {width * 8U};
}

bool MachineScalableSize::is_valid() const {
    return vlenb_eighths != 0;
}

bool MachineScalableSize::operator==(const MachineScalableSize &other) const {
    return vlenb_eighths == other.vlenb_eighths;
}

bool MachineScalableSize::operator!=(const MachineScalableSize &other) const {
    return !(*this == other);
}

MachineVectorAVL MachineVectorAVL::current_vl() {
    return {VectorAVLKind::CurrentVL, 0};
}

MachineVectorAVL MachineVectorAVL::operand(std::size_t index) {
    return {VectorAVLKind::Operand, index};
}

MachineVectorAVL MachineVectorAVL::whole_register() {
    return {VectorAVLKind::WholeRegister, 0};
}

MachineVectorInfo::MachineVectorInfo(MachineVectorType vector_type)
    : vector_type(std::move(vector_type)) {
}

Register Register::virtual_reg(std::uint32_t id, RegisterClass reg_class,
                               ValueType value_type) {
    Register out;
    out.kind = RegisterKind::Virtual;
    out.reg_class = reg_class;
    out.value_type = value_type;
    out.id = id;
    return out;
}

Register Register::virtual_vector(std::uint32_t id, RegisterClass reg_class,
                                  MachineVectorType vector_type) {
    validate_vector_register_class(reg_class, vector_type);
    Register out;
    out.kind = RegisterKind::Virtual;
    out.reg_class = reg_class;
    out.value_type = vector_type.element_type();
    out.id = id;
    out.vector_group_width = vector_type.register_group_width();
    out.vector_type = std::move(vector_type);
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

Register Register::physical_vector(std::string name, RegisterClass reg_class,
                                   MachineVectorType vector_type) {
    validate_vector_register_class(reg_class, vector_type);
    Register out;
    out.kind = RegisterKind::Physical;
    out.reg_class = reg_class;
    out.value_type = vector_type.element_type();
    out.name = std::move(name);
    out.vector_group_width = vector_type.register_group_width();
    out.vector_type = std::move(vector_type);
    return out;
}

bool Register::is_virtual() const {
    return kind == RegisterKind::Virtual;
}

bool Register::is_physical() const {
    return kind == RegisterKind::Physical;
}

bool Register::is_vector() const {
    return is_vector_register_class(reg_class);
}

bool Register::operator==(const Register &other) const {
    if (kind != other.kind) {
        return false;
    }
    if (is_virtual()) {
        // A virtual register's identity is its namespace id and allocation bank.
        // Type/group metadata are verifier constraints, not a second identity.
        return id == other.id && reg_class == other.reg_class;
    }
    // Physical identity is the architectural location.  a0(Void) and a0(I32),
    // or v8 carrying different LMUL views, still name the same storage and must
    // alias in liveness/overlap checks.  The verifier diagnoses invalid views.
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

MachineOperand MachineOperand::implicit_reg_def(Register reg) {
    auto out = make_reg(std::move(reg), true, false);
    out.is_implicit_ = true;
    return out;
}

MachineOperand MachineOperand::implicit_reg_use(Register reg) {
    auto out = make_reg(std::move(reg), false, true);
    out.is_implicit_ = true;
    return out;
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

MachineOperand MachineOperand::reloc(RelocationKind kind, std::string symbol) {
    MachineOperand out;
    out.kind_ = OperandKind::Reloc;
    out.relocation_kind_ = kind;
    out.string_value_ = std::move(symbol);
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

RelocationKind MachineOperand::relocation_kind() const {
    return relocation_kind_;
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

MachineInstr::MachineInstr(Opcode opcode, std::vector<MachineOperand> operands,
                           std::optional<MachineVectorInfo> vector_info)
    : opcode_(opcode), operands_(std::move(operands)), vector_info_(std::move(vector_info)) {
    if (opcode_ == Opcode::Call) {
        // Calls are a scheduling and verification barrier for all mutable RVV
        // execution state.  Centralizing the implicit defs here keeps every
        // producer (including programmatic MIR fixtures and memzero expansion)
        // on the same descriptor contract.
        for (const char *name : {"vl", "vtype", "vxrm", "vxsat", "vstart"}) {
            const bool already_present =
                std::any_of(operands_.begin(), operands_.end(), [&](const MachineOperand &op) {
                    return op.is_implicit() && op.is_reg() && op.is_def() &&
                           op.reg_value().reg_class == RegisterClass::VSTATE &&
                           op.reg_value().name == name;
                });
            if (!already_present) {
                operands_.push_back(MachineOperand::implicit_reg_def(
                    Register::physical(name, RegisterClass::VSTATE,
                                       ValueType::Void)));
            }
        }
    }
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

bool MachineInstr::has_vector_info() const {
    return vector_info_.has_value();
}

const MachineVectorInfo &MachineInstr::vector_info() const {
    return vector_info_.value();
}

MachineVectorInfo &MachineInstr::vector_info() {
    return vector_info_.value();
}

void MachineInstr::set_vector_info(MachineVectorInfo vector_info) {
    vector_info_ = std::move(vector_info);
}

void MachineInstr::clear_vector_info() {
    vector_info_.reset();
}

void MachineInstr::set_variant_cc_call(bool value) {
    variant_cc_call_ = value;
}

bool MachineInstr::is_variant_cc_call() const {
    return variant_cc_call_;
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

Register MachineRegisterInfo::create_virtual_vector(RegisterClass reg_class,
                                                     MachineVectorType vector_type) {
    Register reg = Register::virtual_vector(next_vreg_id_++, reg_class, std::move(vector_type));
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

void MachineFunction::set_variant_cc(bool value) {
    is_variant_cc_ = value;
}

bool MachineFunction::is_variant_cc() const {
    return is_variant_cc_;
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

int MachineFunction::add_scalable_stack_slot(std::string name, MachineVectorType vector_type,
                                             StackSlotKind kind) {
    StackSlot slot;
    slot.id = static_cast<int>(stack_slots_.size());
    slot.name = std::move(name);
    slot.type.value_type = vector_type.element_type();
    slot.type.ir = machine_vector_type_name(vector_type);
    slot.type.size = 0;
    slot.type.align = 1;
    slot.type.vector_type = vector_type;
    slot.kind = kind;
    slot.offset = 0;
    slot.has_fixed_offset = false;
    const auto physical_group_size = MachineScalableSize::from_register_group_width(
        vector_type.register_group_width());
    slot.scalable_size = physical_group_size;
    slot.scalable_align = physical_group_size;
    slot.scalable_offset = MachineScalableSize{0};
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

void MachineFunction::note_standard_aggregate_call() {
    has_standard_aggregate_call_ = true;
}

bool MachineFunction::has_standard_aggregate_call() const {
    return has_standard_aggregate_call_;
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

MachineScalableSize MachineFunction::scalable_frame_size() const {
    return scalable_frame_size_;
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

void MachineFunction::note_used_vector_callee_saved(Register reg) {
    if (!is_variant_cc_ || !reg.is_physical() || !reg.is_vector()) {
        return;
    }
    if (std::any_of(saved_vector_registers_.begin(),
                    saved_vector_registers_.end(),
                    [&](const SavedVectorRegister &saved) {
                        return saved.reg.name == reg.name;
                    })) {
        return;
    }
    const auto raw_type = MachineVectorType::scalable(
        ValueType::I32, 32, VectorLMUL::M1);
    auto raw_reg = Register::physical_vector(reg.name, RegisterClass::VR,
                                             raw_type);
    const auto slot = add_scalable_stack_slot(
        "rvv.callee-save." + reg.name, raw_type,
        StackSlotKind::CalleeSaved);
    saved_vector_registers_.push_back({std::move(raw_reg), slot});
}

const std::vector<SavedVectorRegister> &
MachineFunction::saved_vector_registers() const {
    return saved_vector_registers_;
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
        if (last.opcode() == Opcode::Jump || last.opcode() == Opcode::RISCVJAL) {
            const auto jump_target = last.opcode() == Opcode::Jump ? std::size_t{0}
                                                                   : std::size_t{1};
            if (last.operands().size() > jump_target &&
                last.operands()[jump_target].kind() == OperandKind::Block) {
                add_block_successor(last.operands()[jump_target]);
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
    unsigned scalable_offset_eighths = 0;
    for (auto &slot : stack_slots_) {
        if (slot.scalable_size.has_value()) {
            const auto alignment = slot.scalable_align.value_or(*slot.scalable_size);
            if (alignment.is_valid()) {
                scalable_offset_eighths = static_cast<unsigned>(
                    align_to(scalable_offset_eighths, alignment.vlenb_eighths));
            }
            slot.scalable_offset = MachineScalableSize{scalable_offset_eighths};
            scalable_offset_eighths += slot.scalable_size->vlenb_eighths;
            slot.offset = 0;
            slot.has_fixed_offset = false;
            continue;
        }
        slot.has_fixed_offset = true;
        offset = align_to(offset, slot.type.align);
        slot.offset = static_cast<std::int64_t>(offset);
        offset += slot.type.size;
    }
    scalable_frame_size_ = MachineScalableSize{scalable_offset_eighths};

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
    case RegisterClass::VR:
        return "vr";
    case RegisterClass::VMASK:
        return "vmask";
    case RegisterClass::VRNoV0:
        return "vr-nov0";
    case RegisterClass::VSTATE:
        return "vstate";
    }
    return "unknown";
}

const char *vector_lmul_name(VectorLMUL lmul) {
    switch (lmul) {
    case VectorLMUL::MF8:
        return "mf8";
    case VectorLMUL::MF4:
        return "mf4";
    case VectorLMUL::MF2:
        return "mf2";
    case VectorLMUL::M1:
        return "m1";
    case VectorLMUL::M2:
        return "m2";
    case VectorLMUL::M4:
        return "m4";
    case VectorLMUL::M8:
        return "m8";
    }
    return "unknown";
}

const char *vector_tail_policy_name(VectorTailPolicy policy) {
    switch (policy) {
    case VectorTailPolicy::Unspecified:
        return "unspecified";
    case VectorTailPolicy::Agnostic:
        return "ta";
    case VectorTailPolicy::Undisturbed:
        return "tu";
    }
    return "unknown";
}

const char *vector_mask_policy_name(VectorMaskPolicy policy) {
    switch (policy) {
    case VectorMaskPolicy::Unspecified:
        return "unspecified";
    case VectorMaskPolicy::Agnostic:
        return "ma";
    case VectorMaskPolicy::Undisturbed:
        return "mu";
    }
    return "unknown";
}

const char *vector_rounding_mode_name(VectorRoundingMode mode) {
    switch (mode) {
    case VectorRoundingMode::None:
        return "none";
    case VectorRoundingMode::RNE:
        return "rne";
    case VectorRoundingMode::RTZ:
        return "rtz";
    case VectorRoundingMode::RDN:
        return "rdn";
    case VectorRoundingMode::RUP:
        return "rup";
    case VectorRoundingMode::RMM:
        return "rmm";
    case VectorRoundingMode::Dynamic:
        return "dyn";
    }
    return "unknown";
}

const char *rvv_operation_name(RVVOperation operation) {
    switch (operation) {
    case RVVOperation::None:
        return "none";
    case RVVOperation::SetVL:
        return "setvl";
    case RVVOperation::Add:
        return "add";
    case RVVOperation::Sub:
        return "sub";
    case RVVOperation::Mul:
        return "mul";
    case RVVOperation::Div:
        return "div";
    case RVVOperation::Rem:
        return "rem";
    case RVVOperation::And:
        return "and";
    case RVVOperation::Or:
        return "or";
    case RVVOperation::Xor:
        return "xor";
    case RVVOperation::Min:
        return "min";
    case RVVOperation::Max:
        return "max";
    case RVVOperation::Eq:
        return "eq";
    case RVVOperation::Ne:
        return "ne";
    case RVVOperation::Lt:
        return "lt";
    case RVVOperation::Le:
        return "le";
    case RVVOperation::Gt:
        return "gt";
    case RVVOperation::Ge:
        return "ge";
    case RVVOperation::Merge:
        return "merge";
    case RVVOperation::Load:
        return "load";
    case RVVOperation::Store:
        return "store";
    case RVVOperation::Extract:
        return "extract";
    case RVVOperation::Insert:
        return "insert";
    case RVVOperation::SlideUp:
        return "slide-up";
    case RVVOperation::SlideDown:
        return "slide-down";
    case RVVOperation::Gather:
        return "gather";
    case RVVOperation::ReduceSum:
        return "reduce-sum";
    case RVVOperation::ReduceMin:
        return "reduce-min";
    case RVVOperation::ReduceMax:
        return "reduce-max";
    case RVVOperation::ReduceAnd:
        return "reduce-and";
    case RVVOperation::ReduceOr:
        return "reduce-or";
    case RVVOperation::ReduceXor:
        return "reduce-xor";
    case RVVOperation::MaskPopulationCount:
        return "mask-popcount";
    case RVVOperation::MaskFirst:
        return "mask-first";
    case RVVOperation::Spill:
        return "spill";
    case RVVOperation::Reload:
        return "reload";
    case RVVOperation::MaskSet:
        return "mask-set";
    case RVVOperation::MaskClear:
        return "mask-clear";
    case RVVOperation::MaskAnd:
        return "mask-and";
    case RVVOperation::MaskOr:
        return "mask-or";
    case RVVOperation::MaskXor:
        return "mask-xor";
    case RVVOperation::MaskNot:
        return "mask-not";
    case RVVOperation::Copy:
        return "copy";
    case RVVOperation::ConvertSIToFP:
        return "sitofp";
    case RVVOperation::ConvertFPToSI:
        return "fptosi";
    case RVVOperation::Splat:
        return "splat";
    case RVVOperation::Step:
        return "step";
    }
    return "unknown";
}

std::string machine_vector_type_name(const MachineVectorType &type) {
    std::ostringstream oss;
    oss << (type.is_scalable() ? "sv" : "fv") << "<";
    if (type.is_fixed()) {
        oss << type.fixed_lanes() << "x" << value_type_name(type.element_type()) << ","
            << type.fixed_bits() << "b,";
    } else {
        oss << value_type_name(type.element_type()) << ",";
    }
    oss << "e" << type.sew_bits() << "," << vector_lmul_name(type.lmul());
    if (type.is_mask()) {
        oss << ",vbool" << type.mask_ratio();
    }
    oss << ">";
    return oss.str();
}

std::string machine_scalable_size_name(MachineScalableSize size) {
    if (size.vlenb_eighths == 0) {
        return "0";
    }
    std::ostringstream oss;
    oss << "vlenb";
    if (size.vlenb_eighths == 8) {
        return oss.str();
    }
    if (size.vlenb_eighths > 8 && size.vlenb_eighths % 8 == 0) {
        oss << "*" << size.vlenb_eighths / 8;
    } else {
        oss << "*" << size.vlenb_eighths << "/8";
    }
    return oss.str();
}

const char *opcode_name(Opcode opcode) {
    return instruction_desc(opcode).name;
}

std::uint64_t align_to(std::uint64_t value, std::uint64_t align) {
    if (align == 0 || align == 1) {
        return value;
    }
    return ((value + align - 1) / align) * align;
}

RegisterClass register_class_for_physical(const std::string &name) {
    if (name == "vl" || name == "vtype" || name == "vlenb" || name == "vxrm" ||
        name == "vxsat" || name == "vstart" || name == "frm") {
        return RegisterClass::VSTATE;
    }
    if (name.size() >= 2 && name[0] == 'v' &&
        std::all_of(name.begin() + 1, name.end(),
                    [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); })) {
        return RegisterClass::VR;
    }
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
        return true;
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
