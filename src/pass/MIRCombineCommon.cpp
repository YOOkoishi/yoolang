#include "../../include/pass/MIRCombineCommon.h"

#include "../../include/mir/MIRVerifier.h"

#include <sstream>

namespace pass::mir_combine {

std::string Stats::message() const {
    std::ostringstream oss;
    oss << "combined branch=" << branches << " imm=" << immediates
        << " addr-fold=" << address_folds << " bit=" << bit_idioms << " dead=" << dead;
    return oss.str();
}

bool same_reg(const mir::Register &lhs, const mir::Register &rhs) {
    return lhs == rhs;
}

bool same_reg(const mir::MachineOperand &lhs, const mir::MachineOperand &rhs) {
    return lhs.is_reg() && rhs.is_reg() && same_reg(lhs.reg_value(), rhs.reg_value());
}

bool is_zero_reg(const mir::Register &reg) {
    return reg.is_physical() && reg.reg_class == mir::RegisterClass::GPR && reg.name == "zero";
}

bool is_zero_reg(const mir::MachineOperand &operand) {
    return operand.is_reg() && is_zero_reg(operand.reg_value());
}

mir::Register zero_reg() {
    return mir::Register::physical("zero", mir::RegisterClass::GPR);
}

bool fits_simm12(std::int64_t value) {
    return value >= -2048 && value <= 2047;
}

bool neg_fits_simm12(std::int64_t value) {
    return value >= -2047 && value <= 2048;
}

bool is_power_of_two(std::uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

unsigned log2_u64(std::uint64_t value) {
    unsigned out = 0;
    while (value > 1) {
        value >>= 1U;
        ++out;
    }
    return out;
}

bool is_move(mir::Opcode opcode) {
    return opcode == mir::Opcode::Move || opcode == mir::Opcode::FMove;
}

bool is_pure_def(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::LoadImm:
    case mir::Opcode::LoadGlobalAddr:
    case mir::Opcode::LoadStackAddr:
    case mir::Opcode::Move:
    case mir::Opcode::FMove:
    case mir::Opcode::Add:
    case mir::Opcode::AddW:
    case mir::Opcode::AddI:
    case mir::Opcode::AddIW:
    case mir::Opcode::SubW:
    case mir::Opcode::Mul:
    case mir::Opcode::MulW:
    case mir::Opcode::DivW:
    case mir::Opcode::RemW:
    case mir::Opcode::And:
    case mir::Opcode::AndI:
    case mir::Opcode::SllI:
    case mir::Opcode::SllIW:
    case mir::Opcode::SraI:
    case mir::Opcode::SraIW:
    case mir::Opcode::SrliW:
    case mir::Opcode::Xor:
    case mir::Opcode::XorI:
    case mir::Opcode::Slt:
    case mir::Opcode::SeqZ:
    case mir::Opcode::Snez:
    case mir::Opcode::FeqS:
    case mir::Opcode::FltS:
    case mir::Opcode::FleS:
    case mir::Opcode::FcvtSW:
    case mir::Opcode::FcvtWS:
    case mir::Opcode::FmvWX:
        return true;
    default:
        return false;
    }
}

mir::MachineInstr make_move_like(const mir::MachineOperand &dst,
                                 const mir::MachineOperand &src) {
    auto opcode = dst.reg_value().reg_class == mir::RegisterClass::FPR32 ? mir::Opcode::FMove
                                                                         : mir::Opcode::Move;
    return mir::MachineInstr(opcode, {dst, src});
}

mir::MachineInstr make_move_like(const mir::MachineOperand &dst, const mir::Register &src) {
    return make_move_like(dst, mir::MachineOperand::reg_use(src));
}

std::map<VRegId, RegCounts> count_vregs(const mir::MachineFunction &function) {
    std::map<VRegId, RegCounts> counts;
    for (const auto &block : function.blocks()) {
        for (const auto &instr : block->instructions()) {
            for (const auto &operand : instr.operands()) {
                if (!operand.is_reg() || !operand.reg_value().is_virtual()) {
                    continue;
                }
                auto &entry = counts[operand.reg_value().id];
                if (operand.is_def()) {
                    ++entry.defs;
                }
                if (operand.is_use()) {
                    ++entry.uses;
                }
            }
        }
    }
    return counts;
}

unsigned use_count(const std::map<VRegId, RegCounts> &counts, const mir::Register &reg) {
    if (!reg.is_virtual()) {
        return 0;
    }
    auto found = counts.find(reg.id);
    return found == counts.end() ? 0 : found->second.uses;
}

unsigned def_count(const std::map<VRegId, RegCounts> &counts, const mir::Register &reg) {
    if (!reg.is_virtual()) {
        return 0;
    }
    auto found = counts.find(reg.id);
    return found == counts.end() ? 0 : found->second.defs;
}

bool single_use_vreg(const std::map<VRegId, RegCounts> &counts, const mir::Register &reg) {
    return reg.is_virtual() && use_count(counts, reg) == 1 && def_count(counts, reg) == 1;
}

std::optional<std::size_t> find_def_before(const std::vector<mir::MachineInstr> &instrs,
                                           std::size_t before,
                                           const mir::Register &reg) {
    if (!reg.is_virtual() || before == 0) {
        return std::nullopt;
    }
    for (std::size_t cursor = before; cursor > 0; --cursor) {
        const std::size_t index = cursor - 1;
        for (const auto &def : instrs[index].defs()) {
            if (same_reg(def, reg)) {
                return index;
            }
        }
    }
    return std::nullopt;
}

std::optional<ImmUse> find_single_use_load_imm(const std::vector<mir::MachineInstr> &instrs,
                                               std::size_t user_index,
                                               const mir::MachineOperand &operand,
                                               const std::map<VRegId, RegCounts> &counts) {
    if (!operand.is_reg() || !single_use_vreg(counts, operand.reg_value())) {
        return std::nullopt;
    }
    auto producer_index = find_def_before(instrs, user_index, operand.reg_value());
    if (!producer_index) {
        return std::nullopt;
    }
    const auto &producer = instrs[*producer_index];
    const auto &ops = producer.operands();
    if (producer.opcode() != mir::Opcode::LoadImm || ops.size() < 2 || !ops[0].is_reg() ||
        !same_reg(ops[0].reg_value(), operand.reg_value()) ||
        ops[1].kind() != mir::OperandKind::Imm) {
        return std::nullopt;
    }
    return ImmUse{*producer_index, ops[1].int_value()};
}

void erase_producer(std::vector<mir::MachineInstr> &instrs, std::size_t &user_index,
                    std::size_t producer_index, Stats &stats) {
    instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(producer_index));
    if (producer_index < user_index) {
        --user_index;
    }
    ++stats.dead;
}

PassResult run_transform(PassContext &context, std::string_view pass_name, Transform transform) {
    auto *module = context.machine_module();
    if (module == nullptr) {
        return PassResult::fail(std::string(pass_name) + " requires MIR module in pass context");
    }

    Stats total;
    bool changed = false;
    for (auto &function : module->functions()) {
        if (function->is_external()) {
            continue;
        }
        function->rebuild_cfg();
        changed |= transform(*function, total);
        function->rebuild_cfg();
    }

    auto verify = mir::verify_module(*module, mir::MIRVerificationStage::PreRA);
    if (!verify.ok) {
        return PassResult::fail(verify.message);
    }

    context.set_artifact(std::string(pass_name), total.message());
    return PassResult::ok(changed, total.message());
}

} // namespace pass::mir_combine
