#include "../../include/pass/MIRCombinePass.h"

#include "../../include/mir/MIR.h"
#include "../../include/mir/MIRVerifier.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <vector>

namespace pass {
namespace {

using VRegId = std::uint32_t;

struct Stats {
    unsigned branches = 0;
    unsigned immediates = 0;
    unsigned address_folds = 0;
    unsigned bit_idioms = 0;
    unsigned dead = 0;

    std::string message() const {
        std::ostringstream oss;
        oss << "combined branch=" << branches << " imm=" << immediates
            << " addr-fold=" << address_folds << " bit=" << bit_idioms
            << " dead=" << dead;
        return oss.str();
    }
};

struct RegCounts {
    unsigned defs = 0;
    unsigned uses = 0;
};

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

struct ImmUse {
    std::size_t producer = 0;
    std::int64_t value = 0;
};

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
    if (producer.opcode() != mir::Opcode::LoadImm || ops.size() < 2 ||
        !ops[0].is_reg() || !same_reg(ops[0].reg_value(), operand.reg_value()) ||
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

bool combine_immediates(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;

    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            auto &instr = instrs[i];
            auto ops = instr.operands();
            if (ops.size() < 3 || !ops[0].is_reg()) {
                continue;
            }

            auto replace_with = [&](mir::Opcode opcode, std::vector<mir::MachineOperand> operands,
                                    std::size_t producer) {
                instrs[i] = mir::MachineInstr(opcode, std::move(operands));
                erase_producer(instrs, i, producer, stats);
                ++stats.immediates;
                changed = true;
            };

            if (instr.opcode() == mir::Opcode::Add || instr.opcode() == mir::Opcode::AddW) {
                auto imm_rhs = find_single_use_load_imm(instrs, i, ops[2], counts);
                if (imm_rhs && fits_simm12(imm_rhs->value)) {
                    replace_with(instr.opcode() == mir::Opcode::Add ? mir::Opcode::AddI
                                                                    : mir::Opcode::AddIW,
                                 {ops[0], ops[1], mir::MachineOperand::imm(imm_rhs->value)},
                                 imm_rhs->producer);
                    continue;
                }

                auto imm_lhs = find_single_use_load_imm(instrs, i, ops[1], counts);
                if (imm_lhs && fits_simm12(imm_lhs->value)) {
                    replace_with(instr.opcode() == mir::Opcode::Add ? mir::Opcode::AddI
                                                                    : mir::Opcode::AddIW,
                                 {ops[0], ops[2], mir::MachineOperand::imm(imm_lhs->value)},
                                 imm_lhs->producer);
                    continue;
                }
            }

            if (instr.opcode() == mir::Opcode::SubW) {
                auto imm_rhs = find_single_use_load_imm(instrs, i, ops[2], counts);
                if (imm_rhs && neg_fits_simm12(imm_rhs->value)) {
                    replace_with(mir::Opcode::AddIW,
                                 {ops[0], ops[1], mir::MachineOperand::imm(-imm_rhs->value)},
                                 imm_rhs->producer);
                    continue;
                }
                auto imm_lhs = find_single_use_load_imm(instrs, i, ops[1], counts);
                if (imm_lhs && imm_lhs->value == 0) {
                    replace_with(mir::Opcode::SubW,
                                 {ops[0], mir::MachineOperand::reg_use(zero_reg()), ops[2]},
                                 imm_lhs->producer);
                    continue;
                }
            }

            if (instr.opcode() == mir::Opcode::And || instr.opcode() == mir::Opcode::Xor) {
                auto imm_rhs = find_single_use_load_imm(instrs, i, ops[2], counts);
                bool rhs = true;
                auto imm = imm_rhs;
                if (!imm || !fits_simm12(imm->value)) {
                    rhs = false;
                    imm = find_single_use_load_imm(instrs, i, ops[1], counts);
                }
                if (imm && fits_simm12(imm->value)) {
                    const auto value_operand = rhs ? ops[1] : ops[2];
                    auto opcode = instr.opcode() == mir::Opcode::And ? mir::Opcode::AndI
                                                                     : mir::Opcode::XorI;
                    replace_with(opcode,
                                 {ops[0], value_operand, mir::MachineOperand::imm(imm->value)},
                                 imm->producer);
                    continue;
                }
            }

            if (instr.opcode() == mir::Opcode::MulW) {
                auto imm_rhs = find_single_use_load_imm(instrs, i, ops[2], counts);
                bool rhs = true;
                auto imm = imm_rhs;
                if (!imm) {
                    rhs = false;
                    imm = find_single_use_load_imm(instrs, i, ops[1], counts);
                }
                if (!imm) {
                    continue;
                }

                const auto value_operand = rhs ? ops[1] : ops[2];
                if (imm->value == 0) {
                    replace_with(mir::Opcode::Move,
                                 {ops[0], mir::MachineOperand::reg_use(zero_reg())},
                                 imm->producer);
                    continue;
                }
                if (imm->value == 1) {
                    replace_with(mir::Opcode::Move, {ops[0], value_operand}, imm->producer);
                    continue;
                }
                if (imm->value > 0 && is_power_of_two(static_cast<std::uint64_t>(imm->value))) {
                    auto shift = log2_u64(static_cast<std::uint64_t>(imm->value));
                    if (shift < 32) {
                        replace_with(mir::Opcode::SllIW,
                                     {ops[0], value_operand, mir::MachineOperand::imm(shift)},
                                     imm->producer);
                    }
                }
            }
        }
    }

    return changed;
}

bool combine_address_modes(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;

    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            auto &instr = instrs[i];
            if (instr.opcode() != mir::Opcode::LoadMem &&
                instr.opcode() != mir::Opcode::StoreMem) {
                continue;
            }

            const std::size_t addr_index = instr.opcode() == mir::Opcode::LoadMem ? 1 : 0;
            auto &addr = instr.operands()[addr_index];
            if (!addr.is_reg() || !addr.reg_value().is_virtual()) {
                continue;
            }

            mir::Register addi_result = addr.reg_value();
            auto producer_index = find_def_before(instrs, i, addr.reg_value());
            if (!producer_index) {
                continue;
            }
            std::optional<std::size_t> copy_index;
            const auto &addr_producer = instrs[*producer_index];
            const auto &addr_prod_ops = addr_producer.operands();
            if (addr_producer.opcode() == mir::Opcode::Move && addr_prod_ops.size() >= 2 &&
                addr_prod_ops[1].is_reg() && addr_prod_ops[1].reg_value().is_virtual()) {
                copy_index = producer_index;
                addi_result = addr_prod_ops[1].reg_value();
                producer_index = find_def_before(instrs, *copy_index, addi_result);
                if (!producer_index) {
                    continue;
                }
            }

            const auto &producer = instrs[*producer_index];
            const auto &prod_ops = producer.operands();
            if (producer.opcode() != mir::Opcode::AddI || prod_ops.size() < 3 ||
                !prod_ops[0].is_reg() || !same_reg(prod_ops[0].reg_value(), addi_result) ||
                prod_ops[2].kind() != mir::OperandKind::Imm ||
                !fits_simm12(prod_ops[2].int_value())) {
                continue;
            }

            if (instr.opcode() == mir::Opcode::LoadMem) {
                instrs[i] = mir::MachineInstr(
                    mir::Opcode::LoadMemOffset,
                    {instr.operands()[0], prod_ops[1], prod_ops[2], instr.operands()[2]});
            } else {
                instrs[i] = mir::MachineInstr(
                    mir::Opcode::StoreMemOffset,
                    {prod_ops[1], instr.operands()[1], prod_ops[2], instr.operands()[2]});
            }

            std::vector<std::size_t> erase_indices;
            const bool erase_copy = copy_index && use_count(counts, addr.reg_value()) == 1;
            if (erase_copy) {
                erase_indices.push_back(*copy_index);
            }
            if ((!copy_index || erase_copy) && use_count(counts, addi_result) == 1) {
                erase_indices.push_back(*producer_index);
            }
            std::sort(erase_indices.begin(), erase_indices.end(), std::greater<std::size_t>());
            erase_indices.erase(std::unique(erase_indices.begin(), erase_indices.end()),
                                erase_indices.end());
            for (auto erase_index : erase_indices) {
                erase_producer(instrs, i, erase_index, stats);
            }
            ++stats.address_folds;
            changed = true;
        }
    }

    return changed;
}

bool combine_bit_idioms(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;

    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            auto &instr = instrs[i];
            const auto ops = instr.operands();
            if (ops.empty()) {
                continue;
            }

            auto replace_current = [&](mir::MachineInstr replacement) {
                instrs[i] = std::move(replacement);
                ++stats.bit_idioms;
                changed = true;
            };

            if (is_move(instr.opcode()) && ops.size() >= 2 && same_reg(ops[0], ops[1])) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                --i;
                ++stats.bit_idioms;
                changed = true;
                continue;
            }

            if (ops.size() >= 3 &&
                (instr.opcode() == mir::Opcode::Xor || instr.opcode() == mir::Opcode::SubW) &&
                same_reg(ops[1], ops[2])) {
                replace_current(make_move_like(ops[0], zero_reg()));
                continue;
            }

            if (ops.size() >= 3 && instr.opcode() == mir::Opcode::And && is_zero_reg(ops[2])) {
                replace_current(make_move_like(ops[0], zero_reg()));
                continue;
            }
            if (ops.size() >= 3 && instr.opcode() == mir::Opcode::And && is_zero_reg(ops[1])) {
                replace_current(make_move_like(ops[0], zero_reg()));
                continue;
            }

            if ((instr.opcode() == mir::Opcode::XorI ||
                 instr.opcode() == mir::Opcode::AddI ||
                 instr.opcode() == mir::Opcode::AddIW ||
                 instr.opcode() == mir::Opcode::SllI ||
                 instr.opcode() == mir::Opcode::SllIW ||
                 instr.opcode() == mir::Opcode::SraI ||
                 instr.opcode() == mir::Opcode::SraIW ||
                 instr.opcode() == mir::Opcode::SrliW) &&
                ops.size() >= 3 && ops[2].kind() == mir::OperandKind::Imm &&
                ops[2].int_value() == 0) {
                replace_current(make_move_like(ops[0], ops[1]));
                continue;
            }

            if (instr.opcode() == mir::Opcode::AndI && ops.size() >= 3 &&
                ops[2].kind() == mir::OperandKind::Imm) {
                if (ops[2].int_value() == 0) {
                    replace_current(make_move_like(ops[0], zero_reg()));
                    continue;
                }
                if (ops[2].int_value() == -1) {
                    replace_current(make_move_like(ops[0], ops[1]));
                    continue;
                }

                if (ops[1].is_reg() && single_use_vreg(counts, ops[1].reg_value())) {
                    auto producer_index = find_def_before(instrs, i, ops[1].reg_value());
                    if (producer_index) {
                        const auto &producer = instrs[*producer_index];
                        const auto &prod_ops = producer.operands();
                        if (producer.opcode() == mir::Opcode::AndI && prod_ops.size() >= 3 &&
                            prod_ops[2].kind() == mir::OperandKind::Imm) {
                            const auto combined = prod_ops[2].int_value() & ops[2].int_value();
                            if (fits_simm12(combined)) {
                                instrs[i] = mir::MachineInstr(
                                    mir::Opcode::AndI,
                                    {ops[0], prod_ops[1], mir::MachineOperand::imm(combined)});
                                erase_producer(instrs, i, *producer_index, stats);
                                ++stats.bit_idioms;
                                changed = true;
                            }
                        }
                    }
                }
            }
        }
    }

    return changed;
}

struct BranchReplacement {
    mir::Opcode opcode = mir::Opcode::BranchNonZero;
    mir::MachineOperand lhs;
    mir::MachineOperand rhs;
};

std::optional<BranchReplacement>
match_compare_branch(const std::vector<mir::MachineInstr> &instrs, std::size_t branch_index,
                     const mir::Register &cond, bool branch_if_true,
                     const std::map<VRegId, RegCounts> &counts) {
    if (!single_use_vreg(counts, cond)) {
        return std::nullopt;
    }

    auto producer_index = find_def_before(instrs, branch_index, cond);
    if (!producer_index) {
        return std::nullopt;
    }

    const auto &producer = instrs[*producer_index];
    const auto &ops = producer.operands();
    if (producer.opcode() == mir::Opcode::Slt && ops.size() >= 3) {
        return BranchReplacement{branch_if_true ? mir::Opcode::BranchLT : mir::Opcode::BranchGE,
                                 ops[1], ops[2]};
    }

    if ((producer.opcode() == mir::Opcode::SeqZ || producer.opcode() == mir::Opcode::Snez) &&
        ops.size() >= 2 && ops[1].is_reg() && single_use_vreg(counts, ops[1].reg_value())) {
        auto xor_index = find_def_before(instrs, *producer_index, ops[1].reg_value());
        if (xor_index) {
            const auto &xor_instr = instrs[*xor_index];
            const auto &xor_ops = xor_instr.operands();
            if (xor_instr.opcode() == mir::Opcode::Xor && xor_ops.size() >= 3) {
                bool eq_test = producer.opcode() == mir::Opcode::SeqZ;
                if (!branch_if_true) {
                    eq_test = !eq_test;
                }
                return BranchReplacement{eq_test ? mir::Opcode::BranchEq : mir::Opcode::BranchNe,
                                         xor_ops[1], xor_ops[2]};
            }
        }
    }

    if (producer.opcode() == mir::Opcode::XorI && ops.size() >= 3 &&
        ops[2].kind() == mir::OperandKind::Imm && ops[2].int_value() == 1 && ops[1].is_reg() &&
        single_use_vreg(counts, ops[1].reg_value())) {
        auto slt_index = find_def_before(instrs, *producer_index, ops[1].reg_value());
        if (slt_index) {
            const auto &slt_instr = instrs[*slt_index];
            const auto &slt_ops = slt_instr.operands();
            if (slt_instr.opcode() == mir::Opcode::Slt && slt_ops.size() >= 3) {
                return BranchReplacement{
                    branch_if_true ? mir::Opcode::BranchGE : mir::Opcode::BranchLT,
                    slt_ops[1], slt_ops[2]};
            }
        }
    }

    return std::nullopt;
}

bool erase_if_dead_def(std::vector<mir::MachineInstr> &instrs, std::size_t &anchor,
                       const mir::Register &reg, Stats &stats) {
    if (!reg.is_virtual()) {
        return false;
    }
    auto producer = find_def_before(instrs, anchor, reg);
    if (!producer || !is_pure_def(instrs[*producer].opcode())) {
        return false;
    }
    instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(*producer));
    if (*producer < anchor) {
        --anchor;
    }
    ++stats.dead;
    return true;
}

bool combine_compare_branches(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;

    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            auto &branch = instrs[i];
            if ((branch.opcode() != mir::Opcode::BranchNonZero &&
                 branch.opcode() != mir::Opcode::BranchZero) ||
                branch.operands().size() < 2 || !branch.operands()[0].is_reg() ||
                branch.operands()[1].kind() != mir::OperandKind::Block) {
                continue;
            }

            const auto cond = branch.operands()[0].reg_value();
            auto replacement =
                match_compare_branch(instrs, i, cond,
                                     branch.opcode() == mir::Opcode::BranchNonZero, counts);
            if (!replacement) {
                continue;
            }

            auto target = branch.operands()[1];
            instrs[i] = mir::MachineInstr(replacement->opcode,
                                          {replacement->lhs, replacement->rhs, target});

            std::size_t anchor = i;
            while (erase_if_dead_def(instrs, anchor, cond, stats)) {
            }
            i = anchor;
            ++stats.branches;
            changed = true;
        }
    }

    return changed;
}

bool remove_dead_defs_once(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;
    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size();) {
            const auto defs = instrs[i].defs();
            if (defs.size() == 1 && defs[0].is_virtual() && is_pure_def(instrs[i].opcode()) &&
                use_count(counts, defs[0]) == 0) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                ++stats.dead;
                changed = true;
                continue;
            }
            ++i;
        }
    }
    return changed;
}

bool remove_dead_defs(mir::MachineFunction &function, Stats &stats) {
    bool changed = false;
    for (int iteration = 0; iteration < 8; ++iteration) {
        if (!remove_dead_defs_once(function, stats)) {
            break;
        }
        changed = true;
    }
    return changed;
}

bool optimize_function(mir::MachineFunction &function, Stats &stats) {
    bool changed = false;
    for (int iteration = 0; iteration < 8; ++iteration) {
        bool iteration_changed = false;
        iteration_changed |= combine_immediates(function, stats);
        iteration_changed |= combine_address_modes(function, stats);
        iteration_changed |= combine_compare_branches(function, stats);
        iteration_changed |= combine_bit_idioms(function, stats);
        iteration_changed |= remove_dead_defs(function, stats);
        if (!iteration_changed) {
            break;
        }
        changed = true;
    }
    return changed;
}

} // namespace

std::string_view MIRCombinePass::name() const {
    return "MIRCombinePass";
}

PassKind MIRCombinePass::kind() const {
    return PassKind::Transform;
}

PassResult MIRCombinePass::run(PassContext &context) {
    auto *module = context.machine_module();
    if (module == nullptr) {
        return PassResult::fail("MIRCombinePass requires MIR module in pass context");
    }
    if (auto *conservative = context.get_artifact<bool>("MIRConservativeLowering");
        conservative != nullptr && *conservative) {
        return PassResult::ok(false, "skipped conservative MIR");
    }

    Stats total;
    bool changed = false;
    for (auto &function : module->functions()) {
        if (function->is_external()) {
            continue;
        }
        function->rebuild_cfg();
        changed |= optimize_function(*function, total);
        function->rebuild_cfg();
    }

    auto verify = mir::verify_module(*module, mir::MIRVerificationStage::PreRA);
    if (!verify.ok) {
        return PassResult::fail(verify.message);
    }

    context.set_artifact(std::string(name()), total.message());
    return PassResult::ok(changed, total.message());
}

} // namespace pass
