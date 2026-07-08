#include "pass/mir/MIRBlockSimplifyPass.h"

#include "pass/mir/MIRPeepholeCommon.h"

#include <utility>

namespace pass::mir_peephole {
namespace {

bool has_non_implicit_reg_use(const mir::MachineInstr &instr, const mir::Register &reg) {
    for (const auto &operand : instr.operands()) {
        if (operand.is_reg() && operand.is_use() && !operand.is_implicit() &&
            same_reg(operand.reg_value(), reg)) {
            return true;
        }
    }
    return false;
}

bool has_reg_def(const mir::MachineInstr &instr, const mir::Register &reg) {
    for (const auto &def : instr.defs()) {
        if (same_reg(def, reg)) {
            return true;
        }
    }
    return false;
}

bool reg_redefined_before_next_use(const std::vector<mir::MachineInstr> &instrs,
                                   std::size_t start, const mir::Register &reg) {
    for (std::size_t i = start; i < instrs.size(); ++i) {
        if (has_non_implicit_reg_use(instrs[i], reg)) {
            return false;
        }
        if (has_reg_def(instrs[i], reg)) {
            return true;
        }
    }
    return false;
}

bool simplify_block(mir::MachineBasicBlock &block, const mir::MachineBasicBlock *next_block,
                    bool post_ra, Stats &stats) {
    bool changed = false;
    auto &instrs = block.instructions();

    for (std::size_t i = 0; i < instrs.size();) {
        auto &instr = instrs[i];
        const auto &ops = instr.operands();

        if (is_move(instr.opcode()) && ops.size() >= 2 && same_reg(ops[0], ops[1])) {
            instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
            ++stats.copies;
            changed = true;
            continue;
        }

        if ((instr.opcode() == mir::Opcode::Add || instr.opcode() == mir::Opcode::AddW) &&
            ops.size() >= 3 && (is_zero_reg(ops[1]) || is_zero_reg(ops[2]))) {
            instr = make_move_like(ops[0], is_zero_reg(ops[1]) ? ops[2] : ops[1]);
            ++stats.arithmetic;
            changed = true;
        } else if ((instr.opcode() == mir::Opcode::AddI ||
                    instr.opcode() == mir::Opcode::AddIW) &&
                   ops.size() >= 3 && ops[2].kind() == mir::OperandKind::Imm &&
                   ops[2].int_value() == 0) {
            instr = make_move_like(ops[0], ops[1]);
            ++stats.arithmetic;
            changed = true;
        } else if ((instr.opcode() == mir::Opcode::SllI ||
                    instr.opcode() == mir::Opcode::SllIW ||
                    instr.opcode() == mir::Opcode::SraI ||
                    instr.opcode() == mir::Opcode::SraIW ||
                    instr.opcode() == mir::Opcode::SrliW) &&
                   ops.size() >= 3 && ops[2].kind() == mir::OperandKind::Imm &&
                   ops[2].int_value() == 0) {
            instr = make_move_like(ops[0], ops[1]);
            ++stats.arithmetic;
            changed = true;
        }

        if (instr.opcode() == mir::Opcode::BranchNonZero && ops.size() >= 2 &&
            is_zero_reg(ops[0])) {
            instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
            ++stats.branches;
            changed = true;
            continue;
        }
        if (instr.opcode() == mir::Opcode::BranchZero && ops.size() >= 2 && is_zero_reg(ops[0])) {
            instr = mir::MachineInstr(mir::Opcode::Jump, {ops[1]});
            ++stats.branches;
            changed = true;
        } else if ((instr.opcode() == mir::Opcode::BranchEq ||
                    instr.opcode() == mir::Opcode::BranchGE) &&
                   ops.size() >= 3 && same_reg(ops[0], ops[1])) {
            instr = mir::MachineInstr(mir::Opcode::Jump, {ops[2]});
            ++stats.branches;
            changed = true;
        } else if ((instr.opcode() == mir::Opcode::BranchNe ||
                    instr.opcode() == mir::Opcode::BranchLT) &&
                   ops.size() >= 3 && same_reg(ops[0], ops[1])) {
            instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
            ++stats.branches;
            changed = true;
            continue;
        }

        if (i + 1 < instrs.size()) {
            auto &next = instrs[i + 1];
            const auto &next_ops = next.operands();

            if (post_ra && is_move(instr.opcode()) && instr.opcode() == next.opcode() &&
                ops.size() == 2 && next_ops.size() == 2 && same_reg(ops[0], next_ops[1]) &&
                same_reg(ops[1], next_ops[0])) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i + 1));
                ++stats.copies;
                changed = true;
                continue;
            }

            if (post_ra && (instr.opcode() == mir::Opcode::AddI ||
                            instr.opcode() == mir::Opcode::AddIW) &&
                next.opcode() == mir::Opcode::Move && ops.size() >= 3 && next_ops.size() >= 2 &&
                ops[0].is_reg() && ops[1].is_reg() && ops[2].kind() == mir::OperandKind::Imm &&
                next_ops[0].is_reg() && next_ops[1].is_reg() &&
                same_reg(ops[0], next_ops[1]) &&
                reg_redefined_before_next_use(instrs, i + 2, ops[0].reg_value())) {
                instr = mir::MachineInstr(
                    instr.opcode(),
                    {mir::MachineOperand::reg_def(next_ops[0].reg_value()), ops[1], ops[2]});
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i + 1));
                ++stats.arithmetic;
                ++stats.copies;
                changed = true;
                continue;
            }

            if (instr.opcode() == mir::Opcode::StoreSlot &&
                next.opcode() == mir::Opcode::LoadSlot && ops.size() >= 3 &&
                next_ops.size() >= 3 && same_slot(ops[0], next_ops[1]) &&
                ops[2].type_value() == next_ops[2].type_value()) {
                next = make_move_like(next_ops[0], ops[1]);
                ++stats.loads;
                changed = true;
            } else if (instr.opcode() == mir::Opcode::LoadSlot &&
                       next.opcode() == mir::Opcode::StoreSlot && ops.size() >= 3 &&
                       next_ops.size() >= 3 && same_slot(ops[1], next_ops[0]) &&
                       same_reg(ops[0], next_ops[1])) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i + 1));
                ++stats.stores;
                changed = true;
                continue;
            } else if (instr.opcode() == mir::Opcode::StoreSlot &&
                       next.opcode() == mir::Opcode::StoreSlot && ops.size() >= 1 &&
                       next_ops.size() >= 1 && same_slot(ops[0], next_ops[0])) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                ++stats.stores;
                changed = true;
                continue;
            } else if ((instr.opcode() == mir::Opcode::LoadImm &&
                        next.opcode() == mir::Opcode::LoadImm) ||
                       (instr.opcode() == mir::Opcode::LoadFloatImm &&
                        next.opcode() == mir::Opcode::LoadFloatImm)) {
                if (ops.size() >= 1 && next_ops.size() >= 1 && same_reg(ops[0], next_ops[0])) {
                    instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                    ++stats.copies;
                    changed = true;
                    continue;
                }
            }
        }

        ++i;
    }

    if (instrs.size() >= 2 && next_block != nullptr && instrs.back().opcode() == mir::Opcode::Jump) {
        auto &branch = instrs[instrs.size() - 2];
        const auto &jump_ops = instrs.back().operands();
        if (is_conditional_branch(branch.opcode()) && !jump_ops.empty() &&
            jump_ops[0].kind() == mir::OperandKind::Block) {
            const auto target_index = branch_target_index(branch.opcode());
            if (branch.operands().size() > target_index &&
                branch.operands()[target_index].kind() == mir::OperandKind::Block &&
                branch.operands()[target_index].string_value() == next_block->name()) {
                if (auto inverted = inverted_branch(branch.opcode())) {
                    auto operands = branch.operands();
                    operands[target_index] = jump_ops[0];
                    branch = mir::MachineInstr(*inverted, std::move(operands));
                    instrs.pop_back();
                    ++stats.branches;
                    ++stats.jumps;
                    changed = true;
                }
            }
        }
    }

    if (!instrs.empty() && next_block != nullptr && instrs.back().opcode() == mir::Opcode::Jump) {
        const auto &ops = instrs.back().operands();
        if (!ops.empty() && ops[0].kind() == mir::OperandKind::Block &&
            ops[0].string_value() == next_block->name()) {
            instrs.pop_back();
            ++stats.jumps;
            changed = true;
        }
    }

    return changed;
}

} // namespace

bool simplify_blocks(mir::MachineFunction &function, bool post_ra, Stats &stats) {
    bool changed = false;
    for (std::size_t i = 0; i < function.blocks().size(); ++i) {
        auto *next = i + 1 < function.blocks().size() ? function.blocks()[i + 1].get() : nullptr;
        changed |= simplify_block(*function.blocks()[i], next, post_ra, stats);
    }
    return changed;
}

} // namespace pass::mir_peephole

namespace pass {

MIRBlockSimplifyPass::MIRBlockSimplifyPass(bool post_ra) : post_ra_(post_ra) {
}

std::string_view MIRBlockSimplifyPass::name() const {
    return post_ra_ ? "MIRPostRABlockSimplifyPass" : "MIRPreRABlockSimplifyPass";
}

PassKind MIRBlockSimplifyPass::kind() const {
    return PassKind::Transform;
}

PassResult MIRBlockSimplifyPass::run(PassContext &context) {
    return mir_peephole::run_transform(context, name(), post_ra_, mir_peephole::simplify_blocks);
}

} // namespace pass
