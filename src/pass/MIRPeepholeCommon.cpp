#include "../../include/pass/MIRPeepholeCommon.h"

#include "../../include/mir/MIRVerifier.h"

#include <sstream>

namespace pass::mir_peephole {

bool Stats::changed() const {
    return copies != 0 || loads != 0 || stores != 0 || jumps != 0 || arithmetic != 0 ||
           branches != 0 || address_folds != 0 || cse != 0 || dead != 0;
}

std::string Stats::message() const {
    std::ostringstream oss;
    oss << "removed copy=" << copies << " load=" << loads << " store=" << stores
        << " jump=" << jumps << " arith=" << arithmetic << " branch=" << branches
        << " addr-fold=" << address_folds << " cse=" << cse << " dead=" << dead;
    return oss.str();
}

bool same_reg(const mir::MachineOperand &lhs, const mir::MachineOperand &rhs) {
    return lhs.is_reg() && rhs.is_reg() && lhs.reg_value() == rhs.reg_value();
}

bool same_reg(const mir::Register &lhs, const mir::Register &rhs) {
    return lhs == rhs;
}

bool is_zero_reg(const mir::MachineOperand &operand) {
    return operand.is_reg() && operand.reg_value().is_physical() &&
           operand.reg_value().name == "zero";
}

bool is_zero_reg(const mir::Register &reg) {
    return reg.is_physical() && reg.name == "zero";
}

bool same_slot(const mir::MachineOperand &lhs, const mir::MachineOperand &rhs) {
    return lhs.kind() == mir::OperandKind::Slot && rhs.kind() == mir::OperandKind::Slot &&
           lhs.slot_id() == rhs.slot_id();
}

bool is_move(mir::Opcode opcode) {
    return opcode == mir::Opcode::Move || opcode == mir::Opcode::FMove;
}

bool is_conditional_branch(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::BranchNonZero:
    case mir::Opcode::BranchZero:
    case mir::Opcode::BranchEq:
    case mir::Opcode::BranchNe:
    case mir::Opcode::BranchLT:
    case mir::Opcode::BranchGE:
        return true;
    default:
        return false;
    }
}

std::size_t branch_target_index(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::BranchNonZero:
    case mir::Opcode::BranchZero:
        return 1;
    case mir::Opcode::BranchEq:
    case mir::Opcode::BranchNe:
    case mir::Opcode::BranchLT:
    case mir::Opcode::BranchGE:
        return 2;
    default:
        return 0;
    }
}

std::optional<mir::Opcode> inverted_branch(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::BranchNonZero:
        return mir::Opcode::BranchZero;
    case mir::Opcode::BranchZero:
        return mir::Opcode::BranchNonZero;
    case mir::Opcode::BranchEq:
        return mir::Opcode::BranchNe;
    case mir::Opcode::BranchNe:
        return mir::Opcode::BranchEq;
    case mir::Opcode::BranchLT:
        return mir::Opcode::BranchGE;
    case mir::Opcode::BranchGE:
        return mir::Opcode::BranchLT;
    default:
        return std::nullopt;
    }
}

bool fits_simm12(std::int64_t value) {
    return value >= -2048 && value <= 2047;
}

mir::MachineInstr make_move_like(const mir::MachineOperand &dst,
                                 const mir::MachineOperand &src) {
    auto opcode = dst.reg_value().reg_class == mir::RegisterClass::FPR32 ? mir::Opcode::FMove
                                                                         : mir::Opcode::Move;
    return mir::MachineInstr(opcode, {dst, src});
}

mir::MachineInstr make_move_like(mir::Register dst, mir::Register src) {
    auto opcode = dst.reg_class == mir::RegisterClass::FPR32 ? mir::Opcode::FMove
                                                             : mir::Opcode::Move;
    return mir::MachineInstr(
        opcode, {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(src)});
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

bool defines_reg(const mir::MachineInstr &instr, const mir::Register &reg) {
    for (const auto &operand : instr.operands()) {
        if (operand.is_reg() && operand.is_def() && same_reg(operand.reg_value(), reg)) {
            return true;
        }
    }
    return false;
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

PassResult run_transform(PassContext &context, std::string_view pass_name, bool post_ra,
                         Transform transform) {
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
        changed |= transform(*function, post_ra, total);
        function->rebuild_cfg();
    }

    auto verify = mir::verify_module(
        *module, post_ra ? mir::MIRVerificationStage::PostRA : mir::MIRVerificationStage::PreRA);
    if (!verify.ok) {
        return PassResult::fail(verify.message);
    }

    context.set_artifact(std::string(pass_name), total.message());
    return PassResult::ok(changed, total.message());
}

} // namespace pass::mir_peephole
