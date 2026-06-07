#include "pass/mir/MIRCompareBranchCombinePass.h"

#include "pass/mir/MIRCombineCommon.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace pass::mir_combine {
namespace {

struct BranchReplacement {
    mir::Opcode opcode = mir::Opcode::BranchNonZero;
    mir::MachineOperand lhs;
    std::optional<mir::MachineOperand> rhs;
    std::vector<std::size_t> dead_indices;
};

bool known_boolean_value(const mir::MachineOperand &operand) {
    return operand.is_reg() && operand.reg_value().value_type == mir::ValueType::I1;
}

std::optional<mir::Opcode> zero_branch_for_bool(bool value_is_true, bool branch_if_true) {
    const bool branch_on_input_true = value_is_true == branch_if_true;
    return branch_on_input_true ? mir::Opcode::BranchNonZero : mir::Opcode::BranchZero;
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
                                 ops[1], ops[2], {*producer_index}};
    }

    if ((producer.opcode() == mir::Opcode::SeqZ || producer.opcode() == mir::Opcode::Snez) &&
        ops.size() >= 2 && ops[1].is_reg()) {
        if (single_use_vreg(counts, ops[1].reg_value())) {
            auto xor_index = find_def_before(instrs, *producer_index, ops[1].reg_value());
            if (xor_index) {
                const auto &xor_instr = instrs[*xor_index];
                const auto &xor_ops = xor_instr.operands();
                if (xor_instr.opcode() == mir::Opcode::Xor && xor_ops.size() >= 3) {
                    bool eq_test = producer.opcode() == mir::Opcode::SeqZ;
                    if (!branch_if_true) {
                        eq_test = !eq_test;
                    }
                    return BranchReplacement{
                        eq_test ? mir::Opcode::BranchEq : mir::Opcode::BranchNe, xor_ops[1],
                        xor_ops[2], {*producer_index, *xor_index}};
                }
            }
        }

        const bool value_is_true = producer.opcode() == mir::Opcode::Snez;
        auto opcode = zero_branch_for_bool(value_is_true, branch_if_true);
        return BranchReplacement{*opcode, ops[1], std::nullopt, {*producer_index}};
    }

    if (producer.opcode() == mir::Opcode::XorI && ops.size() >= 3 &&
        ops[2].kind() == mir::OperandKind::Imm && ops[2].int_value() == 1 && ops[1].is_reg()) {
        if (single_use_vreg(counts, ops[1].reg_value())) {
            auto slt_index = find_def_before(instrs, *producer_index, ops[1].reg_value());
            if (slt_index) {
                const auto &slt_instr = instrs[*slt_index];
                const auto &slt_ops = slt_instr.operands();
                if (slt_instr.opcode() == mir::Opcode::Slt && slt_ops.size() >= 3) {
                    return BranchReplacement{
                        branch_if_true ? mir::Opcode::BranchGE : mir::Opcode::BranchLT,
                        slt_ops[1], slt_ops[2], {*producer_index, *slt_index}};
                }
            }
        }

        if (!known_boolean_value(ops[1])) {
            return std::nullopt;
        }
        auto opcode = zero_branch_for_bool(false, branch_if_true);
        return BranchReplacement{*opcode, ops[1], std::nullopt, {*producer_index}};
    }

    return std::nullopt;
}

void erase_dead_indices(std::vector<mir::MachineInstr> &instrs, std::size_t &anchor,
                        std::vector<std::size_t> dead_indices, Stats &stats) {
    std::sort(dead_indices.begin(), dead_indices.end(), std::greater<std::size_t>());
    dead_indices.erase(std::unique(dead_indices.begin(), dead_indices.end()), dead_indices.end());
    for (auto index : dead_indices) {
        if (index >= instrs.size() || !is_pure_def(instrs[index].opcode())) {
            continue;
        }
        erase_producer(instrs, anchor, index, stats);
    }
}

} // namespace

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
            std::vector<mir::MachineOperand> branch_ops;
            branch_ops.push_back(replacement->lhs);
            if (replacement->rhs) {
                branch_ops.push_back(*replacement->rhs);
            }
            branch_ops.push_back(target);
            instrs[i] = mir::MachineInstr(replacement->opcode, std::move(branch_ops));

            std::size_t anchor = i;
            erase_dead_indices(instrs, anchor, std::move(replacement->dead_indices), stats);
            i = anchor;
            ++stats.branches;
            changed = true;
        }
    }

    return changed;
}

} // namespace pass::mir_combine

namespace pass {

std::string_view MIRCompareBranchCombinePass::name() const {
    return "MIRCompareBranchCombinePass";
}

PassKind MIRCompareBranchCombinePass::kind() const {
    return PassKind::Transform;
}

PassResult MIRCompareBranchCombinePass::run(PassContext &context) {
    return mir_combine::run_transform(context, name(), mir_combine::combine_compare_branches);
}

} // namespace pass
