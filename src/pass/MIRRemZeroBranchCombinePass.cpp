#include "../../include/pass/MIRRemZeroBranchCombinePass.h"

#include "../../include/pass/MIRCombineCommon.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace pass::mir_combine {
namespace {

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

} // namespace

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

} // namespace pass::mir_combine

namespace pass {

std::string_view MIRRemZeroBranchCombinePass::name() const {
    return "MIRRemZeroBranchCombinePass";
}

PassKind MIRRemZeroBranchCombinePass::kind() const {
    return PassKind::Transform;
}

PassResult MIRRemZeroBranchCombinePass::run(PassContext &context) {
    return mir_combine::run_transform(context, name(), mir_combine::combine_rem_zero_branches);
}

} // namespace pass
