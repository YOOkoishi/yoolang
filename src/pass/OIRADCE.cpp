#include "../../include/oir/OIRCFGUtils.h"
#include "../../include/oir/OIRScalarOpt.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

bool is_live_root(const oir::Instruction &inst) {
    switch (inst.op()) {
    case oir::Instruction::OpID::Ret:
    case oir::Instruction::OpID::Br:
    case oir::Instruction::OpID::Store:
    case oir::Instruction::OpID::Call:
        return true;
    default:
        return false;
    }
}

bool run_adce_on_function(oir::Function &function, Stats &stats) {
    std::unordered_set<oir::Instruction *> live;
    std::deque<oir::Instruction *> worklist;
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            if (is_live_root(*inst)) {
                live.insert(inst.get());
                worklist.push_back(inst.get());
            }
        }
    }

    while (!worklist.empty()) {
        auto *inst = worklist.front();
        worklist.pop_front();
        for (auto *operand : inst->operands()) {
            auto *operand_inst = dynamic_cast<oir::Instruction *>(operand);
            if (operand_inst != nullptr && live.insert(operand_inst).second) {
                worklist.push_back(operand_inst);
            }
        }
    }

    std::vector<oir::Instruction *> dead;
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            if (live.find(inst.get()) == live.end() && is_pure_instruction(*inst)) {
                dead.push_back(inst.get());
            }
        }
    }
    if (dead.empty()) {
        return false;
    }

    for (auto *inst : dead) {
        inst->drop_all_operands();
    }
    for (auto &block : function.blocks()) {
        for (auto it = block->instructions().begin(); it != block->instructions().end();) {
            if (std::find(dead.begin(), dead.end(), it->get()) != dead.end()) {
                it = block->instructions().erase(it);
            } else {
                ++it;
            }
        }
    }
    stats.adce += static_cast<unsigned>(dead.size());
    return true;
}

bool can_thread_to(oir::BasicBlock *from, oir::BasicBlock *old_succ, oir::BasicBlock *new_succ) {
    if (new_succ == old_succ || new_succ == from) {
        return false;
    }
    for (const auto &inst : new_succ->instructions()) {
        auto *phi = dynamic_cast<const oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        bool has_old = false;
        for (const auto &incoming : phi->incoming()) {
            if (incoming.second == from) {
                return false;
            }
            has_old = has_old || incoming.second == old_succ;
        }
        if (!has_old) {
            return false;
        }
    }
    return true;
}

bool thread_block(oir::BasicBlock *pred, oir::BasicBlock *succ, bool pred_edge_is_true,
                  Stats &stats) {
    if (succ->predecessors().size() != 1 || succ->predecessors().front() != pred) {
        return false;
    }
    auto *pred_br = dynamic_cast<oir::BranchInst *>(pred->terminator());
    auto *succ_br = dynamic_cast<oir::BranchInst *>(succ->terminator());
    if (pred_br == nullptr || succ_br == nullptr || !pred_br->is_conditional() ||
        !succ_br->is_conditional() || pred_br->cond() != succ_br->cond()) {
        return false;
    }

    auto *taken = pred_edge_is_true ? succ_br->true_bb() : succ_br->false_bb();
    if (!can_thread_to(pred, succ, taken)) {
        return false;
    }

    for (auto &inst : taken->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        for (const auto &incoming : phi->incoming()) {
            if (incoming.second == succ) {
                phi->add_incoming(incoming.first, pred);
                break;
            }
        }
    }
    if (!oir::cfg::replace_successor(pred, succ, taken)) {
        return false;
    }
    ++stats.jump_threading;
    ++stats.cfg;
    return true;
}

bool jump_thread_function(oir::Function &function, Stats &stats) {
    for (auto &block : function.blocks()) {
        auto *br = dynamic_cast<oir::BranchInst *>(block->terminator());
        if (br == nullptr || !br->is_conditional()) {
            continue;
        }
        if (thread_block(block.get(), br->true_bb(), true, stats) ||
            thread_block(block.get(), br->false_bb(), false, stats)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool aggressive_dead_code_elimination(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        if (!function->is_external()) {
            changed |= run_adce_on_function(*function, stats);
        }
    }
    return changed;
}

bool jump_threading(oir::Module &module, Stats &stats) {
    bool changed = false;
    constexpr unsigned kMaxIterations = 32;
    for (unsigned i = 0; i < kMaxIterations; ++i) {
        bool iteration_changed = false;
        for (auto &function : module.functions()) {
            if (!function->is_external()) {
                iteration_changed |= jump_thread_function(*function, stats);
            }
        }
        changed |= iteration_changed;
        if (!iteration_changed) {
            break;
        }
    }
    return changed;
}

} // namespace pass::oir_opt
