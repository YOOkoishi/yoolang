#include "oir/OIRCFGUtils.h"

#include <memory>

namespace oir::cfg {

void remove_phi_incoming_from(BasicBlock *block, BasicBlock *pred) {
    if (block == nullptr || pred == nullptr) {
        return;
    }
    for (auto &inst : block->instructions()) {
        auto *phi = dynamic_cast<PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        phi->remove_incoming_from(pred);
    }
}

void replace_phi_incoming_block(BasicBlock *block, BasicBlock *old_pred, BasicBlock *new_pred) {
    if (block == nullptr || old_pred == nullptr || new_pred == nullptr || old_pred == new_pred) {
        return;
    }
    for (auto &inst : block->instructions()) {
        auto *phi = dynamic_cast<PhiInst *>(inst.get());
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

bool replace_branch_target(BranchInst &branch, BasicBlock *old_target, BasicBlock *new_target) {
    if (old_target == nullptr || new_target == nullptr || old_target == new_target) {
        return false;
    }

    bool changed = false;
    if (branch.is_conditional()) {
        if (branch.true_bb() == old_target) {
            branch.set_operand(1, new_target);
            changed = true;
        }
        if (branch.false_bb() == old_target) {
            branch.set_operand(2, new_target);
            changed = true;
        }
        return changed;
    }

    if (branch.target_bb() == old_target) {
        branch.set_operand(0, new_target);
        changed = true;
    }
    return changed;
}

void add_edge(BasicBlock *pred, BasicBlock *succ) {
    if (pred == nullptr || succ == nullptr) {
        return;
    }
    pred->add_successor(succ);
    succ->add_predecessor(pred);
}

void remove_edge_no_phi_update(BasicBlock *pred, BasicBlock *succ) {
    if (pred == nullptr || succ == nullptr) {
        return;
    }
    pred->remove_successor(succ);
    succ->remove_predecessor(pred);
}

void remove_edge(BasicBlock *pred, BasicBlock *succ) {
    remove_edge_no_phi_update(pred, succ);
    remove_phi_incoming_from(succ, pred);
}

bool replace_successor(BasicBlock *pred, BasicBlock *old_succ, BasicBlock *new_succ) {
    if (pred == nullptr || old_succ == nullptr || new_succ == nullptr || old_succ == new_succ) {
        return false;
    }

    auto *branch = dynamic_cast<BranchInst *>(pred->terminator());
    if (branch == nullptr || !replace_branch_target(*branch, old_succ, new_succ)) {
        return false;
    }

    remove_edge(pred, old_succ);
    add_edge(pred, new_succ);
    return true;
}

void move_successor_edge(BasicBlock *old_pred, BasicBlock *new_pred, BasicBlock *succ) {
    if (old_pred == nullptr || new_pred == nullptr || succ == nullptr || old_pred == new_pred) {
        return;
    }
    remove_edge_no_phi_update(old_pred, succ);
    add_edge(new_pred, succ);
    replace_phi_incoming_block(succ, old_pred, new_pred);
}

BranchInst *append_unconditional_branch(Module &module, BasicBlock *from, BasicBlock *to) {
    if (from == nullptr || to == nullptr) {
        return nullptr;
    }
    add_edge(from, to);
    return static_cast<BranchInst *>(
        from->append_instruction(std::make_unique<BranchInst>(module.types().void_ty(), to, from)));
}

BranchInst *append_conditional_branch(Module &module, BasicBlock *from, Value *cond,
                                      BasicBlock *true_bb, BasicBlock *false_bb) {
    if (from == nullptr || cond == nullptr || true_bb == nullptr || false_bb == nullptr) {
        return nullptr;
    }
    add_edge(from, true_bb);
    add_edge(from, false_bb);
    return static_cast<BranchInst *>(from->append_instruction(
        std::make_unique<BranchInst>(module.types().void_ty(), cond, true_bb, false_bb, from)));
}

void drop_all_references(BasicBlock &block) {
    for (auto &inst : block.instructions()) {
        inst->drop_all_operands();
    }
}

} // namespace oir::cfg
