#include "../../include/pass/MIRCompareBranchCombinePass.h"

#include "../../include/pass/MIRCombineCommon.h"

namespace pass::mir_combine {
namespace {

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
