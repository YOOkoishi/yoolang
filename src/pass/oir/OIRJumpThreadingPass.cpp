#include "pass/oir/OIRJumpThreadingPass.h"

#include "oir/OIRCFGUtils.h"
#include "oir/OIRScalarOpt.h"

#include <unordered_set>
#include <utility>
#include <vector>

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

oir::Value *incoming_value_from(oir::PhiInst &phi, oir::BasicBlock *pred) {
    for (const auto &[value, incoming_block] : phi.incoming()) {
        if (incoming_block == pred) {
            return value;
        }
    }
    return nullptr;
}

oir::Value *translate_value_through_phi(oir::Value *value, oir::BasicBlock *through,
                                        oir::BasicBlock *pred) {
    std::unordered_set<oir::Value *> seen;
    while (auto *phi = dynamic_cast<oir::PhiInst *>(value)) {
        if (phi->parent() != through) {
            break;
        }
        if (!seen.insert(phi).second) {
            return nullptr;
        }
        value = incoming_value_from(*phi, pred);
        if (value == nullptr) {
            return nullptr;
        }
    }

    auto *inst = dynamic_cast<oir::Instruction *>(value);
    return inst != nullptr && inst->parent() == through ? nullptr : value;
}

bool has_only_phis_and_branch(oir::BasicBlock &block, oir::BranchInst &branch) {
    for (const auto &inst : block.instructions()) {
        if (inst.get() == &branch) {
            continue;
        }
        if (dynamic_cast<oir::PhiInst *>(inst.get()) == nullptr) {
            return false;
        }
    }
    return true;
}

bool merge_phi_uses_are_threadable(oir::BasicBlock &merge, oir::BasicBlock *taken) {
    for (const auto &inst : merge.instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        for (const auto &use : phi->uses()) {
            auto *user_inst = dynamic_cast<oir::Instruction *>(use.user);
            if (user_inst != nullptr && user_inst->parent() == &merge) {
                continue;
            }

            auto *target_phi = dynamic_cast<oir::PhiInst *>(use.user);
            const std::size_t incoming_index = use.operand_index / 2;
            if (target_phi == nullptr || target_phi->parent() != taken ||
                use.operand_index % 2 != 0 || incoming_index >= target_phi->incoming().size() ||
                target_phi->incoming()[incoming_index].second != &merge) {
                return false;
            }
        }
    }
    return true;
}

bool thread_constant_phi_predecessor(oir::BasicBlock &merge, oir::BranchInst &branch,
                                     Stats &stats) {
    auto *condition_phi = dynamic_cast<oir::PhiInst *>(branch.cond());
    auto *condition_type =
        condition_phi == nullptr ? nullptr
                                 : dynamic_cast<oir::IntegerType *>(condition_phi->type());
    if (condition_phi == nullptr || condition_phi->parent() != &merge ||
        condition_type == nullptr || condition_type->bit_width() != 1 ||
        !has_only_phis_and_branch(merge, branch)) {
        return false;
    }

    const auto condition_incomings = condition_phi->incoming();
    for (const auto &[condition_value, pred] : condition_incomings) {
        const auto constant = int_constant(condition_value);
        if (!constant || (*constant != 0 && *constant != 1) || pred == nullptr ||
            pred == &merge) {
            continue;
        }

        auto *taken = *constant != 0 ? branch.true_bb() : branch.false_bb();
        if (!can_thread_to(pred, &merge, taken) ||
            !merge_phi_uses_are_threadable(merge, taken)) {
            continue;
        }

        std::vector<std::pair<oir::PhiInst *, oir::Value *>> target_incomings;
        bool can_translate = true;
        for (auto &inst : taken->instructions()) {
            auto *target_phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (target_phi == nullptr) {
                break;
            }
            auto *merge_value = incoming_value_from(*target_phi, &merge);
            auto *translated = translate_value_through_phi(merge_value, &merge, pred);
            if (translated == nullptr) {
                can_translate = false;
                break;
            }
            target_incomings.emplace_back(target_phi, translated);
        }
        if (!can_translate || !oir::cfg::replace_successor(pred, &merge, taken)) {
            continue;
        }
        for (const auto &[target_phi, value] : target_incomings) {
            target_phi->add_incoming(value, pred);
        }

        ++stats.jump_threading;
        ++stats.cfg;
        return true;
    }
    return false;
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
        if (thread_constant_phi_predecessor(*block, *br, stats)) {
            return true;
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
