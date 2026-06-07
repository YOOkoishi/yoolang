#include "pass/mir/MIRJumpCleanupPass.h"

#include "pass/mir/MIRPeepholeCommon.h"

#include <algorithm>
#include <optional>
#include <string>
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

std::optional<std::size_t> find_block_index(const mir::MachineFunction &function,
                                            const mir::MachineBasicBlock *block) {
    for (std::size_t index = 0; index < function.blocks().size(); ++index) {
        if (function.blocks()[index].get() == block) {
            return index;
        }
    }
    return std::nullopt;
}

bool targets_block(const mir::MachineInstr &instr, std::size_t target_index,
                   const std::string &block_name) {
    return instr.operands().size() > target_index &&
           instr.operands()[target_index].kind() == mir::OperandKind::Block &&
           instr.operands()[target_index].string_value() == block_name;
}

bool targets_block(const mir::MachineInstr &instr, const std::string &block_name) {
    if (instr.opcode() == mir::Opcode::Jump) {
        return targets_block(instr, 0, block_name);
    }
    if (is_conditional_branch(instr.opcode())) {
        return targets_block(instr, branch_target_index(instr.opcode()), block_name);
    }
    return false;
}

bool is_edge_payload_opcode(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::LoadImm:
    case mir::Opcode::LoadFloatImm:
    case mir::Opcode::LoadGlobalAddr:
    case mir::Opcode::LoadStackAddr:
    case mir::Opcode::Move:
    case mir::Opcode::FMove:
        return true;
    default:
        return false;
    }
}

std::optional<std::string> copy_edge_target(const mir::MachineBasicBlock &block) {
    const auto &instrs = block.instructions();
    if (instrs.empty() || instrs.back().opcode() != mir::Opcode::Jump ||
        instrs.back().operands().empty() ||
        instrs.back().operands()[0].kind() != mir::OperandKind::Block) {
        return std::nullopt;
    }

    for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
        if (!is_edge_payload_opcode(instrs[i].opcode())) {
            return std::nullopt;
        }
    }

    const auto &target = instrs.back().operands()[0].string_value();
    if (target == block.name()) {
        return std::nullopt;
    }
    return target;
}

bool has_successor(const mir::MachineBasicBlock &block, const mir::MachineBasicBlock *succ) {
    const auto &successors = block.successors();
    return std::find(successors.begin(), successors.end(), succ) != successors.end();
}

bool protected_layout_successor(const mir::MachineFunction &function,
                                const mir::MachineBasicBlock &pred,
                                const mir::MachineBasicBlock &candidate) {
    auto pred_index = find_block_index(function, &pred);
    if (!pred_index || *pred_index + 1 >= function.blocks().size()) {
        return false;
    }
    auto *next = function.blocks()[*pred_index + 1].get();
    return next != &candidate && has_successor(pred, next);
}

bool move_block_after(mir::MachineFunction &function, const mir::MachineBasicBlock *block,
                      const mir::MachineBasicBlock *after) {
    auto block_index = find_block_index(function, block);
    auto after_index = find_block_index(function, after);
    if (!block_index || !after_index || *block_index == *after_index ||
        *block_index == *after_index + 1) {
        return false;
    }

    auto &blocks = function.blocks();
    auto moved = std::move(blocks[*block_index]);
    blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(*block_index));
    if (*block_index < *after_index) {
        --*after_index;
    }
    blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(*after_index + 1),
                  std::move(moved));
    return true;
}

bool block_stops_fallthrough(const mir::MachineBasicBlock &block) {
    return !block.instructions().empty() &&
           block.instructions().back().opcode() == mir::Opcode::Jump;
}

bool can_remove_from_layout_position(const mir::MachineFunction &function,
                                     const mir::MachineBasicBlock &block) {
    auto block_index = find_block_index(function, &block);
    if (!block_index || *block_index == 0) {
        return true;
    }
    return block_stops_fallthrough(*function.blocks()[*block_index - 1]);
}

bool can_insert_before_layout_target(const mir::MachineFunction &function,
                                     const mir::MachineBasicBlock &target,
                                     const mir::MachineBasicBlock &inserted) {
    auto target_index = find_block_index(function, &target);
    if (!target_index || *target_index == 0) {
        return false;
    }
    auto *previous = function.blocks()[*target_index - 1].get();
    return previous == &inserted || block_stops_fallthrough(*previous);
}

bool move_block_before(mir::MachineFunction &function, const mir::MachineBasicBlock *block,
                       const mir::MachineBasicBlock *before) {
    auto block_index = find_block_index(function, block);
    auto before_index = find_block_index(function, before);
    if (!block_index || !before_index || *block_index == *before_index ||
        *block_index + 1 == *before_index) {
        return false;
    }

    auto &blocks = function.blocks();
    auto moved = std::move(blocks[*block_index]);
    blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(*block_index));
    if (*block_index < *before_index) {
        --*before_index;
    }
    blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(*before_index), std::move(moved));
    return true;
}

bool prepare_conditional_fallthrough_edge(mir::MachineFunction &function,
                                          mir::MachineBasicBlock &pred,
                                          const mir::MachineBasicBlock &edge,
                                          Stats &stats) {
    auto pred_index = find_block_index(function, &pred);
    if (!pred_index || *pred_index + 1 >= function.blocks().size()) {
        return false;
    }
    auto *old_fallthrough = function.blocks()[*pred_index + 1].get();
    if (old_fallthrough == &edge) {
        return true;
    }

    auto &instrs = pred.instructions();
    if (instrs.empty() || !is_conditional_branch(instrs.back().opcode()) ||
        !targets_block(instrs.back(), edge.name())) {
        return true;
    }

    auto inverted = inverted_branch(instrs.back().opcode());
    if (!inverted) {
        return false;
    }
    const auto target_index = branch_target_index(instrs.back().opcode());
    auto operands = instrs.back().operands();
    operands[target_index] = mir::MachineOperand::block(old_fallthrough->name());
    instrs.back() = mir::MachineInstr(*inverted, std::move(operands));
    ++stats.branches;
    return true;
}

bool place_copy_edge_blocks_for_fallthrough(mir::MachineFunction &function, Stats &stats) {
    function.rebuild_cfg();

    for (std::size_t index = 1; index < function.blocks().size(); ++index) {
        auto *edge = function.blocks()[index].get();
        auto target = copy_edge_target(*edge);
        if (!target || edge->predecessors().size() != 1) {
            continue;
        }

        auto *pred = edge->predecessors().front();
        if (pred == nullptr || pred == edge || protected_layout_successor(function, *pred, *edge)) {
            continue;
        }

        const auto &pred_instrs = pred->instructions();
        if (pred_instrs.empty()) {
            continue;
        }
        const auto &last = pred_instrs.back();
        bool references_edge = targets_block(last, edge->name());
        if (!references_edge && pred_instrs.size() >= 2 &&
            is_conditional_branch(pred_instrs[pred_instrs.size() - 2].opcode())) {
            references_edge = targets_block(pred_instrs[pred_instrs.size() - 2], edge->name());
        }
        if (!references_edge) {
            continue;
        }

        if (!prepare_conditional_fallthrough_edge(function, *pred, *edge, stats)) {
            continue;
        }
        if (move_block_after(function, edge, pred)) {
            ++stats.jumps;
            function.rebuild_cfg();
            return true;
        }
    }

    return false;
}

bool place_copy_edge_blocks_before_targets(mir::MachineFunction &function, Stats &stats) {
    function.rebuild_cfg();

    for (std::size_t index = 1; index < function.blocks().size(); ++index) {
        auto *edge = function.blocks()[index].get();
        auto target_name = copy_edge_target(*edge);
        if (!target_name || edge->predecessors().size() != 1) {
            continue;
        }

        auto *target = function.get_block(*target_name);
        if (target == nullptr || target == edge ||
            !can_remove_from_layout_position(function, *edge) ||
            !can_insert_before_layout_target(function, *target, *edge)) {
            continue;
        }

        auto target_index = find_block_index(function, target);
        auto candidate_pred_index = find_block_index(function, edge->predecessors().front());
        if (target_index && *target_index > 0 && candidate_pred_index) {
            auto *previous = function.blocks()[*target_index - 1].get();
            auto previous_target = copy_edge_target(*previous);
            if (previous != edge && previous_target && *previous_target == target->name() &&
                previous->predecessors().size() == 1) {
                auto previous_pred_index =
                    find_block_index(function, previous->predecessors().front());
                if (previous_pred_index && *previous_pred_index >= *candidate_pred_index) {
                    continue;
                }
            }
        }

        if (move_block_before(function, edge, target)) {
            ++stats.jumps;
            function.rebuild_cfg();
            return true;
        }
    }

    return false;
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
    bool changed = false;
    changed |= redirect_jump_only_blocks(function, stats);
    if (!post_ra) {
        for (std::size_t iteration = 0; iteration < function.blocks().size(); ++iteration) {
            if (!place_copy_edge_blocks_for_fallthrough(function, stats)) {
                break;
            }
            changed = true;
        }
        for (std::size_t iteration = 0; iteration < function.blocks().size(); ++iteration) {
            if (!place_copy_edge_blocks_before_targets(function, stats)) {
                break;
            }
            changed = true;
        }
    }
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
