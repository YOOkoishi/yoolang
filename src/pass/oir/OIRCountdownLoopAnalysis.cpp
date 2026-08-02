#include "pass/oir/OIRCountdownLoopAnalysis.h"

#include "oir/OIRScalarOpt.h"

#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

namespace pass::oir_opt {
namespace {

bool contains_block(const oir::Loop &loop, const oir::BasicBlock *block) {
    return std::find(loop.blocks.begin(), loop.blocks.end(), block) != loop.blocks.end();
}

oir::BasicBlock *mut(const oir::BasicBlock *block) {
    return const_cast<oir::BasicBlock *>(block);
}

bool is_integer_constant(oir::Value *value, std::int64_t expected) {
    auto constant = int_constant(value);
    return constant.has_value() && *constant == expected;
}

bool is_known_nonnegative_count(oir::Value *value) {
    if (auto constant = int_constant(value)) {
        return *constant >= 0;
    }

    auto *call = dynamic_cast<oir::CallInst *>(value);
    auto *callee = call == nullptr ? nullptr : dynamic_cast<oir::Function *>(call->callee());
    return callee != nullptr && (callee->name() == "getarray" || callee->name() == "getfarray");
}

bool is_nonzero_test(oir::Value *condition, oir::Value *value) {
    auto *cmp = dynamic_cast<oir::CmpInst *>(condition);
    if (cmp == nullptr || cmp->pred() != oir::CmpPred::NE) {
        return false;
    }
    return (cmp->lhs() == value && is_integer_constant(cmp->rhs(), 0)) ||
           (cmp->rhs() == value && is_integer_constant(cmp->lhs(), 0));
}

oir::BasicBlock *find_preheader(const oir::Loop &loop) {
    if (loop.header == nullptr) {
        return nullptr;
    }

    oir::BasicBlock *preheader = nullptr;
    for (auto *pred : loop.header->predecessors()) {
        if (contains_block(loop, pred)) {
            continue;
        }
        if (preheader != nullptr) {
            return nullptr;
        }
        preheader = pred;
    }
    return preheader;
}

std::pair<oir::Value *, oir::Value *>
incoming_pair(const oir::PhiInst &phi, oir::BasicBlock *preheader, oir::BasicBlock *latch) {
    oir::Value *outside = nullptr;
    oir::Value *back = nullptr;
    for (const auto &[value, pred] : phi.incoming()) {
        if (pred == preheader) {
            outside = value;
        } else if (pred == latch) {
            back = value;
        }
    }
    return {outside, back};
}

bool is_decrement_by_one(const oir::BinaryInst &next, const oir::PhiInst &phi) {
    if (next.op() == oir::Instruction::OpID::Sub && next.lhs() == &phi &&
        is_integer_constant(next.rhs(), 1)) {
        return true;
    }
    if (next.op() != oir::Instruction::OpID::Add) {
        return false;
    }
    return (next.lhs() == &phi && is_integer_constant(next.rhs(), -1)) ||
           (next.rhs() == &phi && is_integer_constant(next.lhs(), -1));
}

bool block_has_no_effects(const oir::BasicBlock &block, const oir::FunctionModRefAnalysis &modref) {
    for (const auto &inst : block.instructions()) {
        if (dynamic_cast<const oir::PhiInst *>(inst.get()) != nullptr || inst->is_terminator()) {
            continue;
        }
        if (auto *call = dynamic_cast<const oir::CallInst *>(inst.get())) {
            if (modref.call_has_side_effect(*call)) {
                return false;
            }
            continue;
        }
        if (inst->op() == oir::Instruction::OpID::Store ||
            inst->op() == oir::Instruction::OpID::Alloca || !is_pure_instruction(*inst)) {
            return false;
        }
    }
    return true;
}

bool external_uses_are_exit_phis(const oir::BasicBlock &body, const oir::BasicBlock &exit) {
    for (const auto &inst : body.instructions()) {
        if (inst->is_terminator()) {
            continue;
        }
        for (auto *user : inst->users()) {
            auto *user_inst = dynamic_cast<oir::Instruction *>(user);
            if (user_inst != nullptr && user_inst->parent() == &body) {
                continue;
            }
            auto *phi = dynamic_cast<oir::PhiInst *>(user);
            if (phi == nullptr || phi->parent() != &exit) {
                return false;
            }
        }
    }
    return true;
}

std::unordered_set<const oir::BasicBlock *>
normal_exit_reachable_blocks(const oir::BasicBlock &exit, const oir::Loop &loop) {
    std::unordered_set<const oir::BasicBlock *> reachable;
    std::deque<const oir::BasicBlock *> worklist;
    reachable.insert(&exit);
    worklist.push_back(&exit);
    while (!worklist.empty()) {
        auto *block = worklist.front();
        worklist.pop_front();
        for (auto *successor : block->successors()) {
            if (contains_block(loop, successor)) {
                continue;
            }
            if (reachable.insert(successor).second) {
                worklist.push_back(successor);
            }
        }
    }
    return reachable;
}

class GuardedFinalValueVerifier final {
  public:
    GuardedFinalValueVerifier(const oir::BasicBlock &body, const oir::PhiInst &induction,
                              std::unordered_set<const oir::Value *> forbidden_carried,
                              std::unordered_set<const oir::BasicBlock *> loop_blocks,
                              const oir::FunctionModRefAnalysis &modref)
        : body_(body), induction_(induction), forbidden_carried_(std::move(forbidden_carried)),
          loop_blocks_(std::move(loop_blocks)), modref_(modref) {
    }

    bool can_materialize(oir::Value *value) {
        // This is the read-only counterpart of FinalValueMaterializer in the countdown DCE pass;
        // keep the accepted instruction kinds in lockstep with that cloner.
        if (value == &induction_) {
            return true;
        }
        if (forbidden_carried_.find(value) != forbidden_carried_.end()) {
            return false;
        }

        auto *inst = dynamic_cast<oir::Instruction *>(value);
        if (inst == nullptr) {
            return true;
        }
        if (inst->parent() != &body_) {
            return loop_blocks_.find(inst->parent()) == loop_blocks_.end();
        }
        if (verified_.find(inst) != verified_.end()) {
            return true;
        }
        if (!active_.insert(inst).second) {
            return false;
        }

        bool valid = false;
        if (auto *binary = dynamic_cast<oir::BinaryInst *>(inst)) {
            valid = can_materialize(binary->lhs()) && can_materialize(binary->rhs());
        } else if (auto *cmp = dynamic_cast<oir::CmpInst *>(inst)) {
            valid = can_materialize(cmp->lhs()) && can_materialize(cmp->rhs());
        } else if (auto *cast = dynamic_cast<oir::CastInst *>(inst)) {
            valid = can_materialize(cast->src());
        } else if (auto *gep = dynamic_cast<oir::GetElementPtrInst *>(inst)) {
            valid = can_materialize(gep->base_ptr());
            for (auto *index : gep->indices()) {
                valid = valid && can_materialize(index);
            }
        } else if (auto *load = dynamic_cast<oir::LoadInst *>(inst)) {
            valid = can_materialize(load->ptr());
        } else if (auto *call = dynamic_cast<oir::CallInst *>(inst)) {
            valid = !modref_.call_has_side_effect(*call) && can_materialize(call->callee());
            for (auto *arg : call->args()) {
                valid = valid && can_materialize(arg);
            }
        }

        active_.erase(inst);
        if (valid) {
            verified_.insert(inst);
        }
        return valid;
    }

  private:
    const oir::BasicBlock &body_;
    const oir::PhiInst &induction_;
    std::unordered_set<const oir::Value *> forbidden_carried_;
    std::unordered_set<const oir::BasicBlock *> loop_blocks_;
    const oir::FunctionModRefAnalysis &modref_;
    std::unordered_set<const oir::Instruction *> active_;
    std::unordered_set<const oir::Instruction *> verified_;
};

std::optional<const oir::BasicBlock *>
match_guarded_overwritten_countdown_loop(const oir::Loop &loop,
                                         const oir::FunctionModRefAnalysis &modref) {
    if (loop.header == nullptr || loop.blocks.size() != 2 || loop.latches.size() != 1) {
        return std::nullopt;
    }

    auto *header = mut(loop.header);
    auto *body = mut(loop.latches.front());
    auto *preheader = find_preheader(loop);
    if (body == header || preheader == nullptr || !contains_block(loop, body) ||
        header->predecessors().size() != 2 || body->predecessors().size() != 1 ||
        body->predecessors().front() != header) {
        return std::nullopt;
    }
    for (const auto &inst : body->instructions()) {
        if (dynamic_cast<oir::PhiInst *>(inst.get()) != nullptr) {
            return std::nullopt;
        }
    }

    auto *preheader_branch = dynamic_cast<oir::BranchInst *>(preheader->terminator());
    auto *header_branch = dynamic_cast<oir::BranchInst *>(header->terminator());
    auto *body_branch = dynamic_cast<oir::BranchInst *>(body->terminator());
    if (preheader_branch == nullptr || preheader_branch->is_conditional() ||
        preheader_branch->target_bb() != header || header_branch == nullptr ||
        !header_branch->is_conditional() || body_branch == nullptr ||
        body_branch->is_conditional() || body_branch->target_bb() != header ||
        header_branch->true_bb() != body || header_branch->false_bb() == body) {
        return std::nullopt;
    }
    auto *exit = header_branch->false_bb();
    if (exit == nullptr || contains_block(loop, exit) || exit->predecessors().size() != 1 ||
        exit->predecessors().front() != header) {
        return std::nullopt;
    }

    auto *header_cmp = dynamic_cast<oir::CmpInst *>(header_branch->cond());
    if (header_cmp == nullptr || header_cmp->op() != oir::Instruction::OpID::ICmp ||
        header_cmp->parent() != header) {
        return std::nullopt;
    }
    for (auto *user : header_cmp->users()) {
        auto *user_inst = dynamic_cast<oir::Instruction *>(user);
        if (user_inst == nullptr || user_inst->parent() != header) {
            return std::nullopt;
        }
    }

    std::vector<oir::PhiInst *> header_phis;
    for (const auto &inst : header->instructions()) {
        if (auto *phi = dynamic_cast<oir::PhiInst *>(inst.get())) {
            if (phi->incoming().size() != 2) {
                return std::nullopt;
            }
            auto [outside, back] = incoming_pair(*phi, preheader, body);
            if (outside == nullptr || back == nullptr) {
                return std::nullopt;
            }
            header_phis.push_back(phi);
            continue;
        }
        if (!inst->is_terminator() && inst.get() != header_branch->cond()) {
            return std::nullopt;
        }
    }

    oir::PhiInst *induction = nullptr;
    for (auto *phi : header_phis) {
        auto [outside, back] = incoming_pair(*phi, preheader, body);
        auto *next = dynamic_cast<oir::BinaryInst *>(back);
        if (next != nullptr && is_known_nonnegative_count(outside) && next->parent() == body &&
            is_decrement_by_one(*next, *phi) && is_nonzero_test(header_branch->cond(), phi)) {
            induction = phi;
            break;
        }
    }
    if (induction == nullptr || !block_has_no_effects(*body, modref)) {
        return std::nullopt;
    }

    const std::unordered_set<const oir::BasicBlock *> loop_blocks{header, body};
    for (const auto &inst : body->instructions()) {
        if (inst->is_terminator()) {
            continue;
        }
        for (auto *user : inst->users()) {
            auto *user_inst = dynamic_cast<oir::Instruction *>(user);
            if (user_inst == nullptr ||
                loop_blocks.find(user_inst->parent()) == loop_blocks.end()) {
                return std::nullopt;
            }
        }
    }

    std::unordered_set<const oir::Value *> forbidden_carried;
    for (auto *phi : header_phis) {
        if (phi != induction) {
            forbidden_carried.insert(phi);
        }
    }
    GuardedFinalValueVerifier verifier(*body, *induction, std::move(forbidden_carried), loop_blocks,
                                       modref);
    const auto exit_reachable = normal_exit_reachable_blocks(*exit, loop);

    bool has_observable_output = false;
    for (auto *phi : header_phis) {
        bool observable = false;
        for (auto *user : phi->users()) {
            auto *user_inst = dynamic_cast<oir::Instruction *>(user);
            if (user_inst != nullptr &&
                loop_blocks.find(user_inst->parent()) != loop_blocks.end()) {
                continue;
            }
            if (user_inst == nullptr || dynamic_cast<oir::PhiInst *>(user_inst) != nullptr ||
                exit_reachable.find(user_inst->parent()) == exit_reachable.end()) {
                return std::nullopt;
            }
            auto *call = dynamic_cast<oir::CallInst *>(user_inst);
            auto *callee =
                call == nullptr ? nullptr : dynamic_cast<oir::Function *>(call->callee());
            if (callee != nullptr && !callee->is_external()) {
                // Inlining a direct downstream consumer can introduce an external PHI and make
                // the later rotation ineligible, so do not promise to preserve this candidate.
                return std::nullopt;
            }
            observable = true;
        }
        if (!observable) {
            continue;
        }

        auto [outside, back] = incoming_pair(*phi, preheader, body);
        (void)outside;
        if (!verifier.can_materialize(back)) {
            return std::nullopt;
        }
        has_observable_output = true;
    }

    if (!has_observable_output) {
        return std::nullopt;
    }
    // Keep the immediate exit stable until rotation; calls elsewhere are harmless unless they
    // directly consume a loop-carried value, which is rejected above.
    for (const auto &inst : exit->instructions()) {
        auto *call = dynamic_cast<oir::CallInst *>(inst.get());
        auto *callee = call == nullptr ? nullptr : dynamic_cast<oir::Function *>(call->callee());
        if (callee != nullptr && !callee->is_external()) {
            return std::nullopt;
        }
    }
    return body;
}

} // namespace

std::optional<RotatedOverwrittenCountdownLoop>
match_rotated_overwritten_countdown_loop(const oir::Loop &loop,
                                         const oir::FunctionModRefAnalysis &modref) {
    if (loop.blocks.size() != 1 || loop.header == nullptr) {
        return std::nullopt;
    }

    auto *body = mut(loop.header);
    auto *preheader = find_preheader(loop);
    auto *preheader_branch =
        preheader == nullptr ? nullptr : dynamic_cast<oir::BranchInst *>(preheader->terminator());
    auto *loop_branch = dynamic_cast<oir::BranchInst *>(body->terminator());
    if (preheader_branch == nullptr || loop_branch == nullptr ||
        !preheader_branch->is_conditional() || !loop_branch->is_conditional() ||
        loop_branch->true_bb() != body || loop_branch->false_bb() == body) {
        return std::nullopt;
    }

    auto *exit = loop_branch->false_bb();
    if (preheader_branch->true_bb() != body || preheader_branch->false_bb() != exit ||
        !block_has_no_effects(*body, modref) || !external_uses_are_exit_phis(*body, *exit)) {
        return std::nullopt;
    }

    for (const auto &inst : body->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }

        auto [start, back] = incoming_pair(*phi, preheader, body);
        auto *next = dynamic_cast<oir::BinaryInst *>(back);
        if (start == nullptr || next == nullptr || next->parent() != body ||
            !is_decrement_by_one(*next, *phi) || !is_known_nonnegative_count(start) ||
            !is_nonzero_test(preheader_branch->cond(), start) ||
            !is_nonzero_test(loop_branch->cond(), next)) {
            continue;
        }

        return RotatedOverwrittenCountdownLoop{preheader, body, exit, phi, start, next};
    }
    return std::nullopt;
}

bool may_have_guarded_overwritten_countdown_inline_barrier(const oir::Module &module) {
    for (const auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        for (const auto &body_owner : function->blocks()) {
            auto *body = body_owner.get();
            auto *body_branch = dynamic_cast<oir::BranchInst *>(body->terminator());
            if (body_branch == nullptr || body_branch->is_conditional()) {
                continue;
            }
            auto *header = body_branch->target_bb();
            auto *header_branch =
                header == nullptr ? nullptr : dynamic_cast<oir::BranchInst *>(header->terminator());
            if (header == body || header_branch == nullptr || !header_branch->is_conditional() ||
                header_branch->true_bb() != body) {
                continue;
            }

            bool has_internal_call = false;
            for (const auto &inst : body->instructions()) {
                auto *call = dynamic_cast<oir::CallInst *>(inst.get());
                auto *callee =
                    call == nullptr ? nullptr : dynamic_cast<oir::Function *>(call->callee());
                has_internal_call |= callee != nullptr && !callee->is_external();
            }
            if (!has_internal_call) {
                continue;
            }

            for (const auto &inst : header->instructions()) {
                auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
                if (phi == nullptr) {
                    break;
                }
                for (const auto &[back, pred] : phi->incoming()) {
                    auto *next = dynamic_cast<oir::BinaryInst *>(back);
                    if (pred == body && next != nullptr && next->parent() == body &&
                        is_decrement_by_one(*next, *phi) &&
                        is_nonzero_test(header_branch->cond(), phi)) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

std::unordered_set<const oir::BasicBlock *>
find_guarded_overwritten_countdown_inline_barriers(const oir::Function &function,
                                                   const oir::FunctionModRefAnalysis &modref) {
    std::unordered_set<const oir::BasicBlock *> barriers;
    if (function.is_external() || function.entry_block() == nullptr) {
        return barriers;
    }

    oir::DominatorTree dom_tree(function);
    oir::LoopInfo loop_info(function, dom_tree);
    for (const auto &loop : loop_info.loops()) {
        auto body = match_guarded_overwritten_countdown_loop(loop, modref);
        if (body.has_value()) {
            barriers.insert(*body);
        }
    }
    return barriers;
}

} // namespace pass::oir_opt
