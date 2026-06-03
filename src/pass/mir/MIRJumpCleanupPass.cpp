#include "pass/mir/MIRJumpCleanupPass.h"

#include "pass/mir/MIRPeepholeCommon.h"

#include <utility>
#include <vector>

namespace pass::mir_peephole {
namespace {

std::optional<std::string> jump_only_target(const mir::MachineBasicBlock &block) {
    if (block.instructions().size() != 1) {
        return std::nullopt;
    }
    const auto &jump = block.instructions().front();
    if (jump.opcode() != mir::Opcode::Jump || jump.operands().empty() ||
        jump.operands()[0].kind() != mir::OperandKind::Block ||
        jump.operands()[0].string_value() == block.name() ||
        jump.operands()[0].string_value() == "epilogue") {
        return std::nullopt;
    }
    return jump.operands()[0].string_value();
}

std::string resolve_jump_only_target(const mir::MachineFunction &function, std::string target) {
    for (std::size_t depth = 0; depth < function.blocks().size(); ++depth) {
        auto *block = function.get_block(target);
        if (block == nullptr) {
            break;
        }
        auto next = jump_only_target(*block);
        if (!next || *next == target) {
            break;
        }
        target = *next;
    }
    return target;
}

bool redirect_jump_only_blocks(mir::MachineFunction &function, Stats &stats) {
    bool changed = false;
    for (auto &block_ptr : function.blocks()) {
        for (auto &instr : block_ptr->instructions()) {
            std::optional<std::size_t> target_index;
            if (instr.opcode() == mir::Opcode::Jump) {
                target_index = 0;
            } else if (is_conditional_branch(instr.opcode())) {
                target_index = branch_target_index(instr.opcode());
            }

            if (!target_index || instr.operands().size() <= *target_index ||
                instr.operands()[*target_index].kind() != mir::OperandKind::Block) {
                continue;
            }

            const auto old_target = instr.operands()[*target_index].string_value();
            auto new_target = resolve_jump_only_target(function, old_target);
            if (new_target == old_target) {
                continue;
            }

            instr.operands()[*target_index] = mir::MachineOperand::block(std::move(new_target));
            ++stats.jumps;
            changed = true;
        }
    }
    return changed;
}

bool remove_unreachable_jump_only_blocks(mir::MachineFunction &function, Stats &stats) {
    function.rebuild_cfg();

    std::vector<std::string> dead_blocks;
    for (std::size_t i = 1; i < function.blocks().size(); ++i) {
        const auto &block = *function.blocks()[i];
        if (!block.predecessors().empty() || !jump_only_target(block)) {
            continue;
        }
        dead_blocks.push_back(block.name());
    }

    bool changed = false;
    for (const auto &name : dead_blocks) {
        if (function.erase_block(name)) {
            ++stats.jumps;
            changed = true;
        }
    }
    if (changed) {
        function.rebuild_cfg();
    }
    return changed;
}

} // namespace

bool cleanup_jumps(mir::MachineFunction &function, bool post_ra, Stats &stats) {
    (void)post_ra;

    bool changed = false;
    changed |= redirect_jump_only_blocks(function, stats);
    changed |= remove_unreachable_jump_only_blocks(function, stats);
    return changed;
}

} // namespace pass::mir_peephole

namespace pass {

MIRJumpCleanupPass::MIRJumpCleanupPass(bool post_ra) : post_ra_(post_ra) {
}

std::string_view MIRJumpCleanupPass::name() const {
    return post_ra_ ? "MIRPostRAJumpCleanupPass" : "MIRPreRAJumpCleanupPass";
}

PassKind MIRJumpCleanupPass::kind() const {
    return PassKind::Transform;
}

PassResult MIRJumpCleanupPass::run(PassContext &context) {
    return mir_peephole::run_transform(context, name(), post_ra_, mir_peephole::cleanup_jumps);
}

} // namespace pass
