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
    case mir::Opcode::Or:
    case mir::Opcode::SllW:
    case mir::Opcode::SrlW:
    case mir::Opcode::SraW:
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
            if (ops.size() >= 3 && instr.opcode() == mir::Opcode::Or && is_zero_reg(ops[2])) {
                replace_current(make_move_like(ops[0], ops[1]));
                continue;
            }
            if (ops.size() >= 3 && instr.opcode() == mir::Opcode::Or && is_zero_reg(ops[1])) {
                replace_current(make_move_like(ops[0], ops[2]));
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

std::optional<mir::Register> zero_compare_reg(const mir::MachineInstr &branch) {
    if (branch.opcode() != mir::Opcode::BranchEq && branch.opcode() != mir::Opcode::BranchNe) {
        return std::nullopt;
    }

    const auto &ops = branch.operands();
    if (ops.size() < 3 || !ops[0].is_reg() || !ops[1].is_reg()) {
        return std::nullopt;
    }

    if (is_zero_reg(ops[0])) {
        return ops[1].reg_value();
    }
    if (is_zero_reg(ops[1])) {
        return ops[0].reg_value();
    }
    return std::nullopt;
}

bool has_imm_operand(const mir::MachineInstr &instr, std::size_t index, std::int64_t value) {
    const auto &ops = instr.operands();
    return ops.size() > index && ops[index].kind() == mir::OperandKind::Imm &&
           ops[index].int_value() == value;
}

std::optional<mir::MachineOperand>
other_add_operand_matching_value(const mir::MachineInstr &instr, const mir::MachineOperand &value) {
    if (instr.opcode() != mir::Opcode::AddW) {
        return std::nullopt;
    }

    const auto &ops = instr.operands();
    if (ops.size() < 3) {
        return std::nullopt;
    }
    if (same_reg(ops[1], value)) {
        return ops[2];
    }
    if (same_reg(ops[2], value)) {
        return ops[1];
    }
    return std::nullopt;
}

struct SignedRemPow2Match {
    std::size_t rem_def_index = 0;
    mir::MachineOperand value;
    std::int64_t mask = 0;
    std::vector<std::size_t> dead_indices;
};

std::optional<SignedRemPow2Match>
match_signed_rem_pow2(const std::vector<mir::MachineInstr> &instrs, std::size_t branch_index,
                      const mir::Register &rem,
                      const std::map<VRegId, RegCounts> &counts) {
    if (!single_use_vreg(counts, rem)) {
        return std::nullopt;
    }

    auto rem_index = find_def_before(instrs, branch_index, rem);
    if (!rem_index) {
        return std::nullopt;
    }

    const auto &rem_def = instrs[*rem_index];
    const auto &rem_ops = rem_def.operands();
    if (rem_def.opcode() != mir::Opcode::SubW || rem_ops.size() < 3 ||
        !rem_ops[0].is_reg() || !same_reg(rem_ops[0].reg_value(), rem) ||
        !rem_ops[1].is_reg() || !rem_ops[2].is_reg()) {
        return std::nullopt;
    }

    const auto value = rem_ops[1];
    const auto product = rem_ops[2].reg_value();
    if (!single_use_vreg(counts, product)) {
        return std::nullopt;
    }
    auto product_index = find_def_before(instrs, *rem_index, product);
    if (!product_index) {
        return std::nullopt;
    }

    const auto &product_def = instrs[*product_index];
    const auto &product_ops = product_def.operands();
    if (product_def.opcode() != mir::Opcode::SllIW || product_ops.size() < 3 ||
        !product_ops[1].is_reg() || product_ops[2].kind() != mir::OperandKind::Imm) {
        return std::nullopt;
    }

    const auto shift = product_ops[2].int_value();
    if (shift <= 0 || shift >= 31) {
        return std::nullopt;
    }

    const auto quotient = product_ops[1].reg_value();
    if (!single_use_vreg(counts, quotient)) {
        return std::nullopt;
    }
    auto quotient_index = find_def_before(instrs, *product_index, quotient);
    if (!quotient_index) {
        return std::nullopt;
    }

    const auto &quotient_def = instrs[*quotient_index];
    const auto &quotient_ops = quotient_def.operands();
    if (quotient_def.opcode() != mir::Opcode::SraIW || quotient_ops.size() < 3 ||
        !quotient_ops[1].is_reg() || !has_imm_operand(quotient_def, 2, shift)) {
        return std::nullopt;
    }

    const auto adjusted = quotient_ops[1].reg_value();
    if (!single_use_vreg(counts, adjusted)) {
        return std::nullopt;
    }
    auto adjusted_index = find_def_before(instrs, *quotient_index, adjusted);
    if (!adjusted_index) {
        return std::nullopt;
    }

    const auto &adjusted_def = instrs[*adjusted_index];
    auto bias = other_add_operand_matching_value(adjusted_def, value);
    if (!bias || !bias->is_reg()) {
        return std::nullopt;
    }

    auto bias_index = find_def_before(instrs, *adjusted_index, bias->reg_value());
    if (!bias_index) {
        return std::nullopt;
    }

    const auto &bias_def = instrs[*bias_index];
    const auto &bias_ops = bias_def.operands();
    if (bias_def.opcode() != mir::Opcode::SrliW || bias_ops.size() < 3 ||
        !bias_ops[1].is_reg() || !has_imm_operand(bias_def, 2, 32 - shift)) {
        return std::nullopt;
    }

    const auto sign = bias_ops[1].reg_value();
    if (same_reg(sign, bias->reg_value())) {
        if (def_count(counts, sign) != 2 || use_count(counts, sign) != 2) {
            return std::nullopt;
        }
    } else if (!single_use_vreg(counts, sign) || !single_use_vreg(counts, bias->reg_value())) {
        return std::nullopt;
    }

    auto sign_index = find_def_before(instrs, *bias_index, sign);
    if (!sign_index) {
        return std::nullopt;
    }

    const auto &sign_def = instrs[*sign_index];
    const auto &sign_ops = sign_def.operands();
    if (sign_def.opcode() != mir::Opcode::SraIW || sign_ops.size() < 3 ||
        !same_reg(sign_ops[1], value) || !has_imm_operand(sign_def, 2, 31)) {
        return std::nullopt;
    }

    const auto mask = (std::int64_t{1} << static_cast<unsigned>(shift)) - 1;
    if (!fits_simm12(mask)) {
        return std::nullopt;
    }

    std::vector<std::size_t> dead_indices = {*product_index, *quotient_index, *adjusted_index,
                                             *bias_index,    *sign_index};
    return SignedRemPow2Match{*rem_index, value, mask, std::move(dead_indices)};
}

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

bool combine_rem_zero_branches(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;

    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            const auto rem = zero_compare_reg(instrs[i]);
            if (!rem) {
                continue;
            }

            auto match = match_signed_rem_pow2(instrs, i, *rem, counts);
            if (!match) {
                continue;
            }

            const auto rem_dst = instrs[match->rem_def_index].operands()[0];
            instrs[match->rem_def_index] =
                mir::MachineInstr(mir::Opcode::AndI,
                                  {rem_dst, match->value, mir::MachineOperand::imm(match->mask)});

            std::sort(match->dead_indices.begin(), match->dead_indices.end(),
                      std::greater<std::size_t>());
            match->dead_indices.erase(
                std::unique(match->dead_indices.begin(), match->dead_indices.end()),
                match->dead_indices.end());
            for (auto dead_index : match->dead_indices) {
                erase_producer(instrs, i, dead_index, stats);
            }

            ++stats.bit_idioms;
            changed = true;
        }
    }

    return changed;
}

mir::Register create_gpr_vreg(mir::MachineFunction &function, mir::ValueType type) {
    return function.regs().create_virtual(mir::RegisterClass::GPR, type);
}

std::optional<std::size_t> block_index(const mir::MachineFunction &function,
                                       const mir::MachineBasicBlock *block) {
    const auto &blocks = function.blocks();
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].get() == block) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::string> successor_after_payload(const mir::MachineFunction &function,
                                                   const mir::MachineBasicBlock &block,
                                                   std::size_t payload_count) {
    const auto &instrs = block.instructions();
    if (instrs.size() == payload_count) {
        auto index = block_index(function, &block);
        if (index && *index + 1 < function.blocks().size()) {
            return function.blocks()[*index + 1]->name();
        }
        return std::nullopt;
    }
    if (instrs.size() == payload_count + 1 && instrs.back().opcode() == mir::Opcode::Jump &&
        !instrs.back().operands().empty() &&
        instrs.back().operands()[0].kind() == mir::OperandKind::Block) {
        return instrs.back().operands()[0].string_value();
    }
    return std::nullopt;
}

struct BranchTargets {
    const mir::MachineInstr *branch = nullptr;
    mir::MachineBasicBlock *taken = nullptr;
    mir::MachineBasicBlock *other = nullptr;
    std::size_t erase_from = 0;
};

std::optional<BranchTargets> branch_targets(mir::MachineFunction &function,
                                            mir::MachineBasicBlock &pred,
                                            std::size_t pred_index) {
    auto &instrs = pred.instructions();
    if (instrs.empty()) {
        return std::nullopt;
    }

    if (instrs.size() >= 2 && instrs.back().opcode() == mir::Opcode::Jump &&
        is_conditional_branch(instrs[instrs.size() - 2].opcode())) {
        const auto &branch = instrs[instrs.size() - 2];
        const auto &jump = instrs.back();
        const auto target_index = branch_target_index(branch.opcode());
        if (branch.operands().size() <= target_index ||
            branch.operands()[target_index].kind() != mir::OperandKind::Block ||
            jump.operands().empty() || jump.operands()[0].kind() != mir::OperandKind::Block) {
            return std::nullopt;
        }
        auto *taken = function.get_block(branch.operands()[target_index].string_value());
        auto *other = function.get_block(jump.operands()[0].string_value());
        if (taken == nullptr || other == nullptr) {
            return std::nullopt;
        }
        return BranchTargets{&branch, taken, other, instrs.size() - 2};
    }

    if (is_conditional_branch(instrs.back().opcode())) {
        const auto &branch = instrs.back();
        const auto target_index = branch_target_index(branch.opcode());
        if (branch.operands().size() <= target_index ||
            branch.operands()[target_index].kind() != mir::OperandKind::Block ||
            pred_index + 1 >= function.blocks().size()) {
            return std::nullopt;
        }
        auto *taken = function.get_block(branch.operands()[target_index].string_value());
        auto *other = function.blocks()[pred_index + 1].get();
        if (taken == nullptr || other == nullptr) {
            return std::nullopt;
        }
        return BranchTargets{&branch, taken, other, instrs.size() - 1};
    }

    return std::nullopt;
}

mir::Register materialize_branch_bool(mir::MachineFunction &function,
                                      const mir::MachineInstr &branch,
                                      bool branch_taken_is_true,
                                      bool force_new,
                                      std::vector<mir::MachineInstr> &out) {
    const auto &ops = branch.operands();
    auto make_bool = [&]() { return create_gpr_vreg(function, mir::ValueType::I1); };
    auto make_i32 = [&]() { return create_gpr_vreg(function, mir::ValueType::I32); };

    auto copy_bool_if_needed = [&](mir::Register cond) {
        if (!force_new) {
            return cond;
        }
        auto copy = make_bool();
        out.emplace_back(mir::Opcode::Move,
                         std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(copy),
                                                          mir::MachineOperand::reg_use(cond)});
        return copy;
    };

    if ((branch.opcode() == mir::Opcode::BranchNonZero ||
         branch.opcode() == mir::Opcode::BranchZero) &&
        ops.size() >= 1 && ops[0].is_reg()) {
        const bool want_nonzero =
            (branch.opcode() == mir::Opcode::BranchNonZero) == branch_taken_is_true;
        auto cond = ops[0].reg_value();
        if (cond.value_type == mir::ValueType::I1) {
            if (want_nonzero) {
                return copy_bool_if_needed(cond);
            }
            auto inverted = make_bool();
            out.emplace_back(mir::Opcode::XorI,
                             std::vector<mir::MachineOperand>{
                                 mir::MachineOperand::reg_def(inverted),
                                 mir::MachineOperand::reg_use(cond),
                                 mir::MachineOperand::imm(1)});
            return inverted;
        }

        auto result = make_bool();
        out.emplace_back(want_nonzero ? mir::Opcode::Snez : mir::Opcode::SeqZ,
                         std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(result),
                                                          mir::MachineOperand::reg_use(cond)});
        return result;
    }

    if ((branch.opcode() == mir::Opcode::BranchEq || branch.opcode() == mir::Opcode::BranchNe) &&
        ops.size() >= 2 && ops[0].is_reg() && ops[1].is_reg()) {
        auto diff = make_i32();
        out.emplace_back(mir::Opcode::Xor,
                         std::vector<mir::MachineOperand>{
                             mir::MachineOperand::reg_def(diff), ops[0], ops[1]});
        auto result = make_bool();
        const bool want_equal =
            (branch.opcode() == mir::Opcode::BranchEq) == branch_taken_is_true;
        out.emplace_back(want_equal ? mir::Opcode::SeqZ : mir::Opcode::Snez,
                         std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(result),
                                                          mir::MachineOperand::reg_use(diff)});
        return result;
    }

    if ((branch.opcode() == mir::Opcode::BranchLT || branch.opcode() == mir::Opcode::BranchGE) &&
        ops.size() >= 2 && ops[0].is_reg() && ops[1].is_reg()) {
        auto lt = make_bool();
        out.emplace_back(mir::Opcode::Slt,
                         std::vector<mir::MachineOperand>{
                             mir::MachineOperand::reg_def(lt), ops[0], ops[1]});
        const bool want_lt = (branch.opcode() == mir::Opcode::BranchLT) == branch_taken_is_true;
        if (want_lt) {
            return lt;
        }
        auto inverted = make_bool();
        out.emplace_back(mir::Opcode::XorI,
                         std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(inverted),
                                                          mir::MachineOperand::reg_use(lt),
                                                          mir::MachineOperand::imm(1)});
        return inverted;
    }

    return {};
}

struct ConditionalAddMatch {
    mir::Register dst;
    mir::Register base;
    mir::MachineOperand addend;
    std::string merge;
    bool add_on_branch_taken = false;
};

std::optional<ConditionalAddMatch>
match_conditional_add_blocks(const mir::MachineFunction &function,
                             const mir::MachineBasicBlock &add_block,
                             const mir::MachineBasicBlock &skip_block,
                             bool add_on_branch_taken) {
    if (add_block.predecessors().size() != 1 || skip_block.predecessors().size() != 1) {
        return std::nullopt;
    }

    const auto &add_instrs = add_block.instructions();
    const auto &skip_instrs = skip_block.instructions();
    if (add_instrs.empty() || skip_instrs.empty()) {
        return std::nullopt;
    }

    const auto &add = add_instrs.front();
    const auto &skip_move = skip_instrs.front();
    const auto &add_ops = add.operands();
    const auto &skip_ops = skip_move.operands();
    if (add.opcode() != mir::Opcode::AddW || skip_move.opcode() != mir::Opcode::Move ||
        add_ops.size() < 3 || skip_ops.size() < 2 || !add_ops[0].is_reg() ||
        !add_ops[1].is_reg() || !add_ops[2].is_reg() || !skip_ops[0].is_reg() ||
        !skip_ops[1].is_reg()) {
        return std::nullopt;
    }

    std::size_t add_payload_count = 1;
    auto dst = add_ops[0].reg_value();
    if (!same_reg(add_ops[0], skip_ops[0])) {
        if (add_instrs.size() < 2 || add_instrs[1].opcode() != mir::Opcode::Move) {
            return std::nullopt;
        }
        const auto &add_move_ops = add_instrs[1].operands();
        if (add_move_ops.size() < 2 || !add_move_ops[0].is_reg() || !add_move_ops[1].is_reg() ||
            !same_reg(add_move_ops[0], skip_ops[0]) || !same_reg(add_move_ops[1], add_ops[0])) {
            return std::nullopt;
        }
        dst = add_move_ops[0].reg_value();
        add_payload_count = 2;
    }

    auto base = skip_ops[1].reg_value();
    std::optional<mir::MachineOperand> addend;
    if (same_reg(add_ops[1], skip_ops[1])) {
        addend = add_ops[2];
    } else if (same_reg(add_ops[2], skip_ops[1])) {
        addend = add_ops[1];
    }
    if (!addend) {
        return std::nullopt;
    }

    auto add_succ = successor_after_payload(function, add_block, add_payload_count);
    auto skip_succ = successor_after_payload(function, skip_block, 1);
    if (!add_succ || !skip_succ || *add_succ != *skip_succ) {
        return std::nullopt;
    }

    return ConditionalAddMatch{dst, base, *addend, *add_succ, add_on_branch_taken};
}

std::optional<ConditionalAddMatch>
match_conditional_add(const mir::MachineFunction &function, const BranchTargets &targets) {
    if (targets.taken == nullptr || targets.other == nullptr || targets.taken == targets.other) {
        return std::nullopt;
    }
    if (auto match = match_conditional_add_blocks(function, *targets.taken, *targets.other, true)) {
        return match;
    }
    return match_conditional_add_blocks(function, *targets.other, *targets.taken, false);
}

bool if_convert_conditional_add_once(mir::MachineFunction &function, Stats &stats) {
    function.rebuild_cfg();
    auto &blocks = function.blocks();

    for (std::size_t i = 0; i < blocks.size(); ++i) {
        auto &pred = *blocks[i];
        auto targets = branch_targets(function, pred, i);
        if (!targets) {
            continue;
        }

        auto match = match_conditional_add(function, *targets);
        if (!match || targets->taken == &pred || targets->other == &pred) {
            continue;
        }

        std::vector<mir::MachineInstr> replacement;
        auto cond = materialize_branch_bool(function, *targets->branch, match->add_on_branch_taken,
                                            false, replacement);
        if (cond.value_type == mir::ValueType::Void) {
            continue;
        }

        auto mask = create_gpr_vreg(function, mir::ValueType::I32);
        replacement.emplace_back(
            mir::Opcode::SubW,
            std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(mask),
                                             mir::MachineOperand::reg_use(zero_reg()),
                                             mir::MachineOperand::reg_use(cond)});
        auto masked_addend = create_gpr_vreg(function, mir::ValueType::I32);
        replacement.emplace_back(
            mir::Opcode::And,
            std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(masked_addend),
                                             match->addend,
                                             mir::MachineOperand::reg_use(mask)});
        replacement.emplace_back(
            mir::Opcode::AddW,
            std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(match->dst),
                                             mir::MachineOperand::reg_use(match->base),
                                             mir::MachineOperand::reg_use(masked_addend)});
        replacement.emplace_back(
            mir::Opcode::Jump,
            std::vector<mir::MachineOperand>{mir::MachineOperand::block(match->merge)});

        auto &instrs = pred.instructions();
        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(targets->erase_from),
                     instrs.end());
        instrs.insert(instrs.end(), replacement.begin(), replacement.end());

        const auto taken_name = targets->taken->name();
        const auto other_name = targets->other->name();
        function.erase_block(taken_name);
        function.erase_block(other_name);
        function.rebuild_cfg();
        ++stats.bit_idioms;
        return true;
    }

    return false;
}

bool if_convert_conditional_adds(mir::MachineFunction &function, Stats &stats) {
    bool changed = false;
    for (int iteration = 0; iteration < 32; ++iteration) {
        if (!if_convert_conditional_add_once(function, stats)) {
            break;
        }
        changed = true;
    }
    return changed;
}

struct BoolConstBlock {
    mir::Register dst;
    int value = 0;
    std::string merge;
};

std::optional<BoolConstBlock>
match_bool_const_block(const mir::MachineFunction &function, const mir::MachineBasicBlock &block) {
    if (block.predecessors().size() != 1 || block.instructions().empty()) {
        return std::nullopt;
    }

    const auto &instr = block.instructions().front();
    const auto &ops = instr.operands();
    mir::Register dst;
    std::optional<int> value;
    std::size_t payload_count = 1;
    if (instr.opcode() == mir::Opcode::Move && ops.size() >= 2 && ops[0].is_reg()) {
        if (is_zero_reg(ops[1])) {
            dst = ops[0].reg_value();
            value = 0;
        }
    } else if (instr.opcode() == mir::Opcode::LoadImm && ops.size() >= 2 && ops[0].is_reg() &&
               ops[1].kind() == mir::OperandKind::Imm &&
               (ops[1].int_value() == 0 || ops[1].int_value() == 1)) {
        dst = ops[0].reg_value();
        value = static_cast<int>(ops[1].int_value());
    } else if (instr.opcode() == mir::Opcode::LoadImm && ops.size() >= 2 && ops[0].is_reg() &&
               ops[1].kind() == mir::OperandKind::Imm &&
               (ops[1].int_value() == 0 || ops[1].int_value() == 1) &&
               block.instructions().size() >= 2 &&
               block.instructions()[1].opcode() == mir::Opcode::Move) {
        const auto &move_ops = block.instructions()[1].operands();
        if (move_ops.size() >= 2 && move_ops[0].is_reg() && move_ops[1].is_reg() &&
            same_reg(move_ops[1], ops[0])) {
            dst = move_ops[0].reg_value();
            value = static_cast<int>(ops[1].int_value());
            payload_count = 2;
        }
    }
    if (!value) {
        return std::nullopt;
    }

    auto succ = successor_after_payload(function, block, payload_count);
    if (!succ) {
        return std::nullopt;
    }
    return BoolConstBlock{dst, *value, *succ};
}

struct BoolComputeBlock {
    mir::Register dst;
    std::vector<mir::MachineInstr> payload;
    std::string merge;
};

std::optional<BoolComputeBlock>
match_bool_compute_block(const mir::MachineFunction &function,
                         const mir::MachineBasicBlock &block) {
    if (block.predecessors().size() != 1) {
        return std::nullopt;
    }

    if (block.instructions().empty()) {
        return std::nullopt;
    }

    std::size_t payload_count = block.instructions().size();
    std::optional<std::string> succ;
    if (!block.instructions().empty() && block.instructions().back().opcode() == mir::Opcode::Jump) {
        payload_count = block.instructions().size() - 1;
        succ = successor_after_payload(function, block, payload_count);
    } else {
        succ = successor_after_payload(function, block, block.instructions().size());
    }
    if (!succ || payload_count == 0 || payload_count > 3) {
        return std::nullopt;
    }

    std::vector<mir::MachineInstr> payload;
    payload.reserve(payload_count);
    for (std::size_t i = 0; i < payload_count; ++i) {
        const auto &instr = block.instructions()[i];
        if (!is_pure_def(instr.opcode())) {
            return std::nullopt;
        }
        payload.push_back(instr);
    }

    const auto defs = payload.back().defs();
    if (defs.size() != 1) {
        return std::nullopt;
    }
    if (defs[0].value_type != mir::ValueType::I1) {
        return std::nullopt;
    }
    return BoolComputeBlock{defs[0], std::move(payload), *succ};
}

struct BoolSelectMatch {
    BoolComputeBlock compute;
    mir::Register result_dst;
    int const_value = 0;
    std::string merge;
    bool compute_on_branch_taken = false;
};

std::optional<BoolSelectMatch>
match_bool_select(const mir::MachineFunction &function, const BranchTargets &targets) {
    auto taken_const = match_bool_const_block(function, *targets.taken);
    if (taken_const) {
        auto compute = match_bool_compute_block(function, *targets.other);
        if (compute && compute->merge == taken_const->merge) {
            return BoolSelectMatch{std::move(*compute), taken_const->dst, taken_const->value,
                                   taken_const->merge, false};
        }
    }

    auto other_const = match_bool_const_block(function, *targets.other);
    if (other_const) {
        auto compute = match_bool_compute_block(function, *targets.taken);
        if (compute && compute->merge == other_const->merge) {
            return BoolSelectMatch{std::move(*compute), other_const->dst, other_const->value,
                                   other_const->merge, true};
        }
    }

    return std::nullopt;
}

bool if_convert_bool_select_once(mir::MachineFunction &function, Stats &stats) {
    function.rebuild_cfg();
    auto &blocks = function.blocks();

    for (std::size_t i = 0; i < blocks.size(); ++i) {
        auto &pred = *blocks[i];
        auto targets = branch_targets(function, pred, i);
        if (!targets || targets->taken == &pred || targets->other == &pred ||
            targets->taken == targets->other) {
            continue;
        }

        auto match = match_bool_select(function, *targets);
        if (!match) {
            continue;
        }

        std::vector<mir::MachineInstr> replacement;
        auto compute_cond = materialize_branch_bool(function, *targets->branch,
                                                    match->compute_on_branch_taken, true,
                                                    replacement);
        if (compute_cond.value_type == mir::ValueType::Void) {
            continue;
        }

        replacement.insert(replacement.end(), match->compute.payload.begin(),
                           match->compute.payload.end());
        if (match->const_value == 0) {
            replacement.emplace_back(
                mir::Opcode::And,
                std::vector<mir::MachineOperand>{
                    mir::MachineOperand::reg_def(match->result_dst),
                    mir::MachineOperand::reg_use(match->compute.dst),
                    mir::MachineOperand::reg_use(compute_cond)});
        } else {
            auto not_compute_cond = create_gpr_vreg(function, mir::ValueType::I1);
            replacement.emplace_back(
                mir::Opcode::XorI,
                std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(not_compute_cond),
                                                 mir::MachineOperand::reg_use(compute_cond),
                                                 mir::MachineOperand::imm(1)});
            replacement.emplace_back(
                mir::Opcode::Or,
                std::vector<mir::MachineOperand>{
                    mir::MachineOperand::reg_def(match->result_dst),
                    mir::MachineOperand::reg_use(match->compute.dst),
                    mir::MachineOperand::reg_use(not_compute_cond)});
        }
        replacement.emplace_back(mir::Opcode::Jump,
                                 std::vector<mir::MachineOperand>{
                                     mir::MachineOperand::block(match->merge)});

        auto &instrs = pred.instructions();
        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(targets->erase_from),
                     instrs.end());
        instrs.insert(instrs.end(), replacement.begin(), replacement.end());

        const auto taken_name = targets->taken->name();
        const auto other_name = targets->other->name();
        function.erase_block(taken_name);
        function.erase_block(other_name);
        function.rebuild_cfg();
        ++stats.bit_idioms;
        return true;
    }

    return false;
}

bool if_convert_bool_selects(mir::MachineFunction &function, Stats &stats) {
    bool changed = false;
    for (int iteration = 0; iteration < 32; ++iteration) {
        if (!if_convert_bool_select_once(function, stats)) {
            break;
        }
        changed = true;
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
        iteration_changed |= combine_rem_zero_branches(function, stats);
        iteration_changed |= if_convert_bool_selects(function, stats);
        iteration_changed |= if_convert_conditional_adds(function, stats);
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
