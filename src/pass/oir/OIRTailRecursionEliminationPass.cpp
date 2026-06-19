#include "pass/oir/OIRTailRecursionEliminationPass.h"

#include "oir/OIRScalarOpt.h"

#include "oir/OIRCFGUtils.h"

#include <iterator>
#include <memory>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

struct TailSelfCall {
    oir::CallInst *call = nullptr;
    oir::BasicBlock *return_block = nullptr;
};

bool return_block_has_only_phis_and_return(const oir::BasicBlock &block) {
    if (block.instructions().empty()) {
        return false;
    }

    auto *ret = dynamic_cast<oir::ReturnInst *>(block.terminator());
    if (ret == nullptr) {
        return false;
    }

    for (const auto &inst : block.instructions()) {
        if (inst.get() == ret) {
            return true;
        }
        if (dynamic_cast<oir::PhiInst *>(inst.get()) == nullptr) {
            return false;
        }
    }
    return false;
}

oir::Value *phi_incoming_from(const oir::PhiInst &phi, const oir::BasicBlock *pred) {
    for (const auto &[value, from] : phi.incoming()) {
        if (from == pred) {
            return value;
        }
    }
    return nullptr;
}

bool uses_only_return_edge_phis(const oir::CallInst &call, const oir::BasicBlock &pred,
                                const oir::BasicBlock &return_block) {
    for (const auto &use : call.uses()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(use.user);
        if (phi == nullptr || phi->parent() != &return_block || use.operand_index % 2 != 0) {
            return false;
        }

        const std::size_t incoming_index = use.operand_index / 2;
        if (incoming_index >= phi->incoming().size() ||
            phi->incoming()[incoming_index].first != &call ||
            phi->incoming()[incoming_index].second != &pred) {
            return false;
        }
    }
    return true;
}

TailSelfCall tail_self_call(oir::Function &function, oir::BasicBlock &block) {
    if (block.instructions().size() < 2) {
        return {};
    }

    auto term_it = std::prev(block.instructions().end());
    if (auto *ret = dynamic_cast<oir::ReturnInst *>(term_it->get())) {
        auto call_it = std::prev(term_it);
        auto *call = dynamic_cast<oir::CallInst *>(call_it->get());
        if (call == nullptr || call->callee() != &function) {
            return {};
        }

        if (ret->has_value()) {
            if (ret->value() == call && call->use_count() == 1) {
                return {call, nullptr};
            }
            return {};
        }
        return call->type()->is_void() && call->use_count() == 0 ? TailSelfCall{call, nullptr}
                                                                 : TailSelfCall{};
    }

    auto *branch = dynamic_cast<oir::BranchInst *>(term_it->get());
    if (branch == nullptr || branch->is_conditional()) {
        return {};
    }

    auto call_it = std::prev(term_it);
    auto *call = dynamic_cast<oir::CallInst *>(call_it->get());
    if (call == nullptr || call->callee() != &function) {
        return {};
    }

    auto *return_block = branch->target_bb();
    if (return_block == nullptr || return_block == &block ||
        !return_block_has_only_phis_and_return(*return_block)) {
        return {};
    }

    auto *ret = static_cast<oir::ReturnInst *>(return_block->terminator());
    if (ret->has_value()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(ret->value());
        if (ret->value() != call &&
            (phi == nullptr || phi->parent() != return_block ||
             phi_incoming_from(*phi, &block) != call)) {
            return {};
        }
        if (!uses_only_return_edge_phis(*call, block, *return_block)) {
            return {};
        }
        return {call, return_block};
    }

    return call->type()->is_void() && call->use_count() == 0 ? TailSelfCall{call, return_block}
                                                             : TailSelfCall{};
}

bool has_tail_self_call(oir::Function &function) {
    for (auto &block : function.blocks()) {
        if (tail_self_call(function, *block).call != nullptr) {
            return true;
        }
    }
    return false;
}

struct TailRecLoop {
    oir::BasicBlock *entry = nullptr;
    oir::BasicBlock *header = nullptr;
    std::vector<oir::PhiInst *> arg_phis;
};

TailRecLoop create_argument_loop_header(oir::Function &function) {
    TailRecLoop loop;
    loop.entry = function.entry_block();
    loop.header = function.create_block("tailrec.header");

    auto old_successors = loop.entry->successors();
    for (auto *succ : old_successors) {
        oir::cfg::move_successor_edge(loop.entry, loop.header, succ);
    }

    for (auto &arg : function.args()) {
        auto phi = std::make_unique<oir::PhiInst>(
            arg->type(), loop.header,
            arg->name().empty() ? "tailrec.arg" : arg->name() + ".tail");
        auto *raw = phi.get();
        raw->set_parent(loop.header);
        raw->add_incoming(arg.get(), loop.entry);
        loop.arg_phis.push_back(raw);
        loop.header->instructions().push_back(std::move(phi));
    }

    while (!loop.entry->instructions().empty()) {
        auto inst = std::move(loop.entry->instructions().front());
        loop.entry->instructions().pop_front();
        inst->set_parent(loop.header);
        loop.header->instructions().push_back(std::move(inst));
    }

    oir::cfg::append_unconditional_branch(*function.parent(), loop.entry, loop.header);
    return loop;
}

void replace_argument_uses(oir::Function &function, const TailRecLoop &loop) {
    std::unordered_set<oir::Instruction *> skip;
    for (auto *phi : loop.arg_phis) {
        skip.insert(phi);
    }

    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            if (skip.find(inst.get()) != skip.end()) {
                continue;
            }
            for (std::size_t arg_index = 0; arg_index < function.args().size(); ++arg_index) {
                inst->replace_operands(function.args()[arg_index].get(),
                                       loop.arg_phis[arg_index]);
            }
        }
    }
}

bool rewrite_tail_calls(oir::Function &function, const TailRecLoop &loop, Stats &stats) {
    bool changed = false;
    std::vector<oir::BasicBlock *> blocks;
    for (auto &block : function.blocks()) {
        blocks.push_back(block.get());
    }

    for (auto *block : blocks) {
        auto tail_call = tail_self_call(function, *block);
        if (tail_call.call == nullptr) {
            continue;
        }

        auto *call = tail_call.call;
        auto args = call->args();
        if (args.size() != loop.arg_phis.size()) {
            continue;
        }

        for (std::size_t i = 0; i < args.size(); ++i) {
            loop.arg_phis[i]->add_incoming(args[i], block);
        }

        auto &insts = block->instructions();
        if (tail_call.return_block != nullptr) {
            oir::cfg::remove_edge(block, tail_call.return_block);
        }
        insts.back()->drop_all_operands();
        insts.pop_back();
        insts.back()->drop_all_operands();
        insts.pop_back();
        oir::cfg::append_unconditional_branch(*function.parent(), block, loop.header);

        ++stats.tail_recursion;
        changed = true;
    }

    return changed;
}

bool run_on_function(oir::Function &function, Stats &stats) {
    if (function.is_external() || function.entry_block() == nullptr || !has_tail_self_call(function)) {
        return false;
    }

    auto loop = create_argument_loop_header(function);
    replace_argument_uses(function, loop);
    return rewrite_tail_calls(function, loop, stats);
}

} // namespace

bool eliminate_tail_recursion(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        changed |= run_on_function(*function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt

namespace pass {

std::string_view OIRTailRecursionEliminationPass::name() const {
    return "OIRTailRecursionEliminationPass";
}

PassKind OIRTailRecursionEliminationPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRTailRecursionEliminationPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRTailRecursionEliminationPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::eliminate_tail_recursion(module, stats);
            changed |= oir_opt::simplify_branches(module, stats);
            changed |= oir_opt::cleanup_cfg(module, stats);
            changed |= oir_opt::eliminate_dead_code(module, stats);
            return changed;
        });
}

} // namespace pass
