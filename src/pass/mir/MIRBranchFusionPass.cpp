#include "pass/mir/MIRBranchFusionPass.h"

#include "pass/mir/MIRPeepholeCommon.h"

#include <utility>
#include <vector>

namespace pass::mir_peephole {
namespace {

bool replace_branch_with(mir::MachineBasicBlock &block, std::size_t index, mir::Opcode opcode,
                         std::vector<mir::MachineOperand> operands) {
    block.instructions()[index] = mir::MachineInstr(opcode, std::move(operands));
    return true;
}

} // namespace

bool fuse_compare_branches(mir::MachineFunction &function, bool post_ra, Stats &stats) {
    (void)post_ra;

    auto counts = count_vregs(function);
    bool changed = false;

    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            if (instrs[i].opcode() != mir::Opcode::BranchNonZero ||
                instrs[i].operands().size() < 2 || i == 0) {
                continue;
            }

            const auto cond = instrs[i].operands()[0].reg_value();
            if (!cond.is_virtual() || use_count(counts, cond) != 1) {
                continue;
            }

            auto &prev = instrs[i - 1];
            const auto &prev_ops = prev.operands();
            if (prev.defs().size() == 1 && same_reg(prev.defs()[0], cond)) {
                if (prev.opcode() == mir::Opcode::Slt && prev_ops.size() >= 3) {
                    replace_branch_with(*block_ptr, i, mir::Opcode::BranchLT,
                                        {prev_ops[1], prev_ops[2], instrs[i].operands()[1]});
                    instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i - 1));
                    --i;
                    ++stats.branches;
                    changed = true;
                    continue;
                }

                if ((prev.opcode() == mir::Opcode::SeqZ ||
                     prev.opcode() == mir::Opcode::Snez) &&
                    prev_ops.size() >= 2 && i >= 2) {
                    const auto tmp = prev_ops[1].reg_value();
                    auto &cmp = instrs[i - 2];
                    const auto &cmp_ops = cmp.operands();
                    if (tmp.is_virtual() && use_count(counts, tmp) == 1 &&
                        cmp.opcode() == mir::Opcode::Xor && cmp_ops.size() >= 3 &&
                        cmp.defs().size() == 1 && same_reg(cmp.defs()[0], tmp)) {
                        auto branch_opcode = prev.opcode() == mir::Opcode::SeqZ
                                                 ? mir::Opcode::BranchEq
                                                 : mir::Opcode::BranchNe;
                        replace_branch_with(*block_ptr, i, branch_opcode,
                                            {cmp_ops[1], cmp_ops[2], instrs[i].operands()[1]});
                        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i - 1));
                        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i - 2));
                        i -= 2;
                        ++stats.branches;
                        changed = true;
                        continue;
                    }
                }

                if (prev.opcode() == mir::Opcode::XorI && prev_ops.size() >= 3 &&
                    prev_ops[2].kind() == mir::OperandKind::Imm && prev_ops[2].int_value() == 1 &&
                    i >= 2) {
                    const auto tmp = prev_ops[1].reg_value();
                    auto &cmp = instrs[i - 2];
                    const auto &cmp_ops = cmp.operands();
                    if (tmp.is_virtual() && use_count(counts, tmp) == 1 &&
                        cmp.opcode() == mir::Opcode::Slt && cmp_ops.size() >= 3 &&
                        cmp.defs().size() == 1 && same_reg(cmp.defs()[0], tmp)) {
                        replace_branch_with(*block_ptr, i, mir::Opcode::BranchGE,
                                            {cmp_ops[1], cmp_ops[2], instrs[i].operands()[1]});
                        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i - 1));
                        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i - 2));
                        i -= 2;
                        ++stats.branches;
                        changed = true;
                        continue;
                    }
                }
            }
        }
    }

    return changed;
}

} // namespace pass::mir_peephole

namespace pass {

std::string_view MIRBranchFusionPass::name() const {
    return "MIRBranchFusionPass";
}

PassKind MIRBranchFusionPass::kind() const {
    return PassKind::Transform;
}

PassResult MIRBranchFusionPass::run(PassContext &context) {
    return mir_peephole::run_transform(context, name(), false, mir_peephole::fuse_compare_branches);
}

} // namespace pass
