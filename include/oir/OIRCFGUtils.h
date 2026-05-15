#pragma once

#include "OIR.h"

namespace oir::cfg {

void remove_phi_incoming_from(BasicBlock *block, BasicBlock *pred);
void replace_phi_incoming_block(BasicBlock *block, BasicBlock *old_pred, BasicBlock *new_pred);

bool replace_branch_target(BranchInst &branch, BasicBlock *old_target, BasicBlock *new_target);

void add_edge(BasicBlock *pred, BasicBlock *succ);
void remove_edge(BasicBlock *pred, BasicBlock *succ);
void remove_edge_no_phi_update(BasicBlock *pred, BasicBlock *succ);
bool replace_successor(BasicBlock *pred, BasicBlock *old_succ, BasicBlock *new_succ);
void move_successor_edge(BasicBlock *old_pred, BasicBlock *new_pred, BasicBlock *succ);

BranchInst *append_unconditional_branch(Module &module, BasicBlock *from, BasicBlock *to);
BranchInst *append_conditional_branch(Module &module, BasicBlock *from, Value *cond,
                                      BasicBlock *true_bb, BasicBlock *false_bb);

void drop_all_references(BasicBlock &block);

} // namespace oir::cfg
