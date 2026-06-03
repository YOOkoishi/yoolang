#include "pass/oir/OIRJumpThreadingPass.h"

#include "oir/OIRCFGUtils.h"
#include "oir/OIRScalarOpt.h"

namespace pass::oir_opt {
namespace {

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

namespace pass {

std::string_view OIRJumpThreadingPass::name() const {
    return "OIRJumpThreadingPass";
}

PassKind OIRJumpThreadingPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRJumpThreadingPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRJumpThreadingPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::jump_threading(module, stats);
            changed |= oir_opt::simplify_branches(module, stats);
            changed |= oir_opt::cleanup_cfg(module, stats);
            changed |= oir_opt::aggressive_dead_code_elimination(module, stats);
            return changed;
        });
}

} // namespace pass
