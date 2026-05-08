#include "../../include/oir/OIRScalarOpt.h"

#include <deque>
#include <memory>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

void remove_phi_incoming_from(oir::BasicBlock *block, oir::BasicBlock *pred) {
    for (auto &inst : block->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        phi->remove_incoming_from(pred);
    }
}

void replace_phi_incoming_block(oir::BasicBlock *block, oir::BasicBlock *old_pred,
                                oir::BasicBlock *new_pred) {
    for (auto &inst : block->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        for (std::size_t i = 0; i < phi->incoming().size(); ++i) {
            if (phi->incoming()[i].second == old_pred) {
                phi->set_operand(i * 2 + 1, new_pred);
            }
        }
    }
}

void replace_branch_target(oir::BranchInst &branch, oir::BasicBlock *old_target,
                           oir::BasicBlock *new_target) {
    if (branch.is_conditional()) {
        if (branch.true_bb() == old_target) {
            branch.set_operand(1, new_target);
        }
        if (branch.false_bb() == old_target) {
            branch.set_operand(2, new_target);
        }
        return;
    }
    if (branch.target_bb() == old_target) {
        branch.set_operand(0, new_target);
    }
}

std::unordered_set<oir::BasicBlock *> reachable_blocks(oir::Function &function) {
    std::unordered_set<oir::BasicBlock *> reachable;
    auto *entry = function.entry_block();
    if (entry == nullptr) {
        return reachable;
    }

    std::deque<oir::BasicBlock *> worklist;
    reachable.insert(entry);
    worklist.push_back(entry);
    while (!worklist.empty()) {
        auto *block = worklist.front();
        worklist.pop_front();
        for (auto *succ : block->successors()) {
            if (reachable.insert(succ).second) {
                worklist.push_back(succ);
            }
        }
    }
    return reachable;
}

bool remove_unreachable_blocks(oir::Function &function, Stats &stats) {
    auto reachable = reachable_blocks(function);
    std::vector<oir::BasicBlock *> unreachable;
    for (auto &block : function.blocks()) {
        if (reachable.find(block.get()) == reachable.end()) {
            unreachable.push_back(block.get());
        }
    }

    for (auto *block : unreachable) {
        auto predecessors = block->predecessors();
        auto successors = block->successors();
        for (auto *pred : predecessors) {
            pred->remove_successor(block);
            block->remove_predecessor(pred);
        }
        for (auto *succ : successors) {
            succ->remove_predecessor(block);
            block->remove_successor(succ);
            remove_phi_incoming_from(succ, block);
        }
        function.erase_block(block);
        ++stats.cfg;
    }
    return !unreachable.empty();
}

bool simplify_single_predecessor_phis(oir::Module &module, oir::Function &function,
                                      Stats &stats) {
    ReplacementMap replacements;
    std::vector<oir::PhiInst *> dead_phis;

    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }
            if (phi->incoming().size() == 1 && phi->incoming().front().first != phi &&
                phi->incoming().front().second != phi->parent()) {
                replacements[phi] = phi->incoming().front().first;
                dead_phis.push_back(phi);
            }
        }
    }

    if (dead_phis.empty()) {
        return false;
    }

    apply_replacements(module, replacements);
    for (auto *phi : dead_phis) {
        auto &insts = phi->parent()->instructions();
        for (auto it = insts.begin(); it != insts.end(); ++it) {
            if (it->get() == phi) {
                insts.erase(it);
                ++stats.cfg;
                break;
            }
        }
    }
    return true;
}

bool is_empty_jump_block(const oir::BasicBlock &block) {
    if (block.instructions().size() != 1) {
        return false;
    }
    auto *branch = dynamic_cast<oir::BranchInst *>(block.terminator());
    return branch != nullptr && !branch->is_conditional();
}

bool redirect_empty_jump_block(oir::Function &function, oir::BasicBlock *block, Stats &stats) {
    if (block == function.entry_block() || !is_empty_jump_block(*block)) {
        return false;
    }

    auto *branch = static_cast<oir::BranchInst *>(block->terminator());
    auto *target = branch->target_bb();
    if (target == block) {
        return false;
    }

    auto predecessors = block->predecessors();
    if (predecessors.empty()) {
        return false;
    }
    for (auto *pred : predecessors) {
        for (auto *succ : pred->successors()) {
            if (succ == target) {
                return false;
            }
        }
    }

    for (auto &inst : target->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }

        oir::Value *incoming_value = nullptr;
        for (const auto &incoming : phi->incoming()) {
            if (incoming.second == block) {
                incoming_value = incoming.first;
                break;
            }
        }
        if (incoming_value == nullptr) {
            continue;
        }

        phi->remove_incoming_from(block);
        for (auto *pred : predecessors) {
            phi->add_incoming(incoming_value, pred);
        }
    }

    for (auto *pred : predecessors) {
        auto *pred_branch = dynamic_cast<oir::BranchInst *>(pred->terminator());
        if (pred_branch != nullptr) {
            replace_branch_target(*pred_branch, block, target);
        }
        pred->remove_successor(block);
        pred->add_successor(target);
        target->add_predecessor(pred);
        block->remove_predecessor(pred);
    }

    target->remove_predecessor(block);
    block->remove_successor(target);
    function.erase_block(block);
    ++stats.cfg;
    return true;
}

bool try_redirect_empty_jump_blocks(oir::Function &function, Stats &stats) {
    for (auto &block : function.blocks()) {
        if (redirect_empty_jump_block(function, block.get(), stats)) {
            return true;
        }
    }
    return false;
}

bool merge_with_single_predecessor_successor(oir::Module &module, oir::Function &function,
                                             oir::BasicBlock *block, Stats &stats) {
    auto *branch = dynamic_cast<oir::BranchInst *>(block->terminator());
    if (branch == nullptr || branch->is_conditional()) {
        return false;
    }

    auto *succ = branch->target_bb();
    if (succ == block || succ->predecessors().size() != 1 ||
        succ->predecessors().front() != block) {
        return false;
    }

    ReplacementMap phi_replacements;
    std::vector<oir::PhiInst *> phis;
    for (auto &inst : succ->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        if (phi->incoming().size() != 1 || phi->incoming().front().first == phi ||
            phi->incoming().front().second == succ) {
            return false;
        }
        phi_replacements[phi] = phi->incoming().front().first;
        phis.push_back(phi);
    }
    apply_replacements(module, phi_replacements);

    for (auto *phi : phis) {
        auto &insts = succ->instructions();
        for (auto it = insts.begin(); it != insts.end(); ++it) {
            if (it->get() == phi) {
                insts.erase(it);
                break;
            }
        }
    }

    block->instructions().pop_back();
    block->remove_successor(succ);
    succ->remove_predecessor(block);

    auto succ_successors = succ->successors();
    for (auto *succ_succ : succ_successors) {
        succ_succ->remove_predecessor(succ);
        succ_succ->add_predecessor(block);
        block->add_successor(succ_succ);
        replace_phi_incoming_block(succ_succ, succ, block);
        succ->remove_successor(succ_succ);
    }

    auto &succ_insts = succ->instructions();
    while (!succ_insts.empty()) {
        auto inst = std::move(succ_insts.front());
        succ_insts.pop_front();
        inst->set_parent(block);
        block->instructions().push_back(std::move(inst));
    }

    function.erase_block(succ);
    ++stats.cfg;
    return true;
}

bool try_merge_blocks(oir::Module &module, oir::Function &function, Stats &stats) {
    for (auto &block : function.blocks()) {
        if (merge_with_single_predecessor_successor(module, function, block.get(), stats)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool cleanup_cfg(oir::Module &module, Stats &stats) {
    bool changed = false;
    bool keep_going = true;

    while (keep_going) {
        keep_going = false;
        for (auto &function : module.functions()) {
            if (function->is_external()) {
                continue;
            }

            std::size_t block_count = 0;
            std::size_t instruction_count = 0;
            for (const auto &block : function->blocks()) {
                ++block_count;
                instruction_count += block->instructions().size();
            }
            const bool large_cfg = block_count > 1000 || instruction_count > 8000;

            if (remove_unreachable_blocks(*function, stats)) {
                changed = true;
                keep_going = true;
                continue;
            }
            if (large_cfg) {
                continue;
            }
            if (simplify_single_predecessor_phis(module, *function, stats)) {
                changed = true;
                keep_going = true;
                continue;
            }
            if (try_redirect_empty_jump_blocks(*function, stats)) {
                changed = true;
                keep_going = true;
                continue;
            }
            if (try_merge_blocks(module, *function, stats)) {
                changed = true;
                keep_going = true;
                continue;
            }
        }
    }

    return changed;
}

} // namespace pass::oir_opt
