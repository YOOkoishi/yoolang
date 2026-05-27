#include "../../include/pass/OIRTailRecursionEliminationPass.h"

#include "../../include/oir/OIRScalarOpt.h"

#include "../../include/oir/OIRCFGUtils.h"

#include <iterator>
#include <memory>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

oir::CallInst *tail_self_call(oir::Function &function, oir::BasicBlock &block) {
    if (block.instructions().size() < 2) {
        return nullptr;
    }

    auto ret_it = std::prev(block.instructions().end());
    auto *ret = dynamic_cast<oir::ReturnInst *>(ret_it->get());
    if (ret == nullptr) {
        return nullptr;
    }

    auto call_it = std::prev(ret_it);
    auto *call = dynamic_cast<oir::CallInst *>(call_it->get());
    if (call == nullptr || call->callee() != &function) {
        return nullptr;
    }

    if (ret->has_value()) {
        return ret->value() == call ? call : nullptr;
    }
    return call->type()->is_void() ? call : nullptr;
}

bool has_tail_self_call(oir::Function &function) {
    for (auto &block : function.blocks()) {
        if (tail_self_call(function, *block) != nullptr) {
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
        auto *call = tail_self_call(function, *block);
        if (call == nullptr) {
            continue;
        }

        auto args = call->args();
        if (args.size() != loop.arg_phis.size()) {
            continue;
        }

        for (std::size_t i = 0; i < args.size(); ++i) {
            loop.arg_phis[i]->add_incoming(args[i], block);
        }

        auto &insts = block->instructions();
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
    if (function.is_external() || function.entry_block() == nullptr || function.args().empty() ||
        !has_tail_self_call(function)) {
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
