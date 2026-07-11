#include "oir/OIRScalarOpt.h"

#include "oir/OIRAnalysis.h"

#include <optional>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

bool is_scalar_constant(oir::Value *value) {
    if (value == nullptr || !is_scalar_type(value->type())) {
        return false;
    }
    return dynamic_cast<oir::ConstantInt *>(value) != nullptr ||
           dynamic_cast<oir::ConstantFloat *>(value) != nullptr ||
           dynamic_cast<oir::ConstantZero *>(value) != nullptr;
}

bool has_matching_call_shape(const oir::CallInst &call, const oir::Function &function) {
    if (call.callee() != &function || call.type() != function.return_type()) {
        return false;
    }

    auto actuals = call.args();
    if (actuals.size() != function.args().size()) {
        return false;
    }
    for (std::size_t index = 0; index < actuals.size(); ++index) {
        if (actuals[index] == nullptr || actuals[index]->type() != function.args()[index]->type()) {
            return false;
        }
    }
    return true;
}

std::optional<std::vector<oir::CallInst *>> closed_world_call_sites(oir::Function &function) {
    if (function.is_external() || function.name() == "main") {
        return std::nullopt;
    }

    std::vector<oir::CallInst *> calls;
    std::unordered_set<oir::CallInst *> seen;
    for (const auto &use : function.uses()) {
        auto *call = dynamic_cast<oir::CallInst *>(use.user);
        if (use.operand_index != 0 || call == nullptr ||
            !has_matching_call_shape(*call, function)) {
            return std::nullopt;
        }
        if (seen.insert(call).second) {
            calls.push_back(call);
        }
    }
    return calls;
}

bool is_recursive_call_site(const oir::CallInst &call, const oir::Function &function) {
    auto *block = call.parent();
    return block != nullptr && block->parent() == &function;
}

bool propagate_constant_arguments(oir::Function &function,
                                  const std::vector<oir::CallInst *> &calls, Stats &stats) {
    // A single constant call is already handled well by yoolang's specialization/inliner.
    // Reserve signature-wide propagation for facts shared by multiple call sites so the
    // generic body is not needlessly narrowed when there is no cross-call payoff.
    if (calls.size() < 2) {
        return false;
    }
    for (auto *call : calls) {
        if (call != nullptr && is_recursive_call_site(*call, function)) {
            return false;
        }
    }

    bool changed = false;
    for (std::size_t index = 0; index < function.args().size(); ++index) {
        auto *formal = function.args()[index].get();
        if (formal == nullptr || !is_scalar_type(formal->type()) || !formal->has_uses()) {
            continue;
        }

        oir::Value *common = nullptr;
        bool consistent = true;
        for (auto *call : calls) {
            auto actuals = call->args();
            auto *actual = actuals[index];
            if (actual->type() != formal->type() || !is_scalar_constant(actual)) {
                consistent = false;
                break;
            }
            if (common == nullptr) {
                common = actual;
            } else if (!same_constant_value(common, actual)) {
                consistent = false;
                break;
            }
        }
        if (!consistent || common == nullptr || common->type() != formal->type()) {
            continue;
        }

        const auto before = formal->use_count();
        formal->replace_all_uses_with(common);
        if (formal->use_count() != before) {
            ++stats.ipsccp;
            changed = true;
        }
    }
    return changed;
}

enum class UniformReturnKind { Constant, Argument };

struct UniformReturn {
    UniformReturnKind kind = UniformReturnKind::Constant;
    oir::Value *value = nullptr;
    std::size_t argument_index = 0;
};

std::optional<UniformReturn> uniform_return_value(oir::Function &function) {
    if (!is_scalar_type(function.return_type()) || function.entry_block() == nullptr) {
        return std::nullopt;
    }

    // Replacing a call result can make a readnone call dead.  Only infer returned-value
    // attributes for leaf, acyclic CFGs so that doing so cannot erase a recursive/non-returning
    // computation.
    oir::DominatorTree dom_tree(function);
    for (const auto &block : function.blocks()) {
        if (!dom_tree.is_reachable(block.get())) {
            return std::nullopt;
        }
        for (const auto &inst : block->instructions()) {
            if (dynamic_cast<const oir::CallInst *>(inst.get()) != nullptr) {
                return std::nullopt;
            }
        }
        for (auto *successor : block->successors()) {
            if (dom_tree.dominates(successor, block.get())) {
                return std::nullopt;
            }
        }
    }

    std::optional<UniformReturn> result;
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            auto *ret = dynamic_cast<oir::ReturnInst *>(inst.get());
            if (ret == nullptr) {
                continue;
            }
            if (!ret->has_value() || ret->value() == nullptr ||
                ret->value()->type() != function.return_type()) {
                return std::nullopt;
            }

            UniformReturn candidate;
            if (is_scalar_constant(ret->value())) {
                candidate.kind = UniformReturnKind::Constant;
                candidate.value = ret->value();
            } else if (auto *argument = dynamic_cast<oir::Argument *>(ret->value());
                       argument != nullptr && argument->parent() == &function &&
                       argument->index() < function.args().size() &&
                       function.args()[argument->index()].get() == argument) {
                candidate.kind = UniformReturnKind::Argument;
                candidate.value = argument;
                candidate.argument_index = argument->index();
            } else {
                return std::nullopt;
            }

            if (!result.has_value()) {
                result = candidate;
                continue;
            }
            if (result->kind != candidate.kind) {
                return std::nullopt;
            }
            if (candidate.kind == UniformReturnKind::Constant) {
                if (!same_constant_value(result->value, candidate.value)) {
                    return std::nullopt;
                }
            } else if (result->argument_index != candidate.argument_index) {
                return std::nullopt;
            }
        }
    }
    return result;
}

bool propagate_uniform_returns(oir::Function &function, const std::vector<oir::CallInst *> &calls,
                               Stats &stats) {
    if (calls.empty()) {
        return false;
    }
    auto uniform = uniform_return_value(function);
    if (!uniform.has_value()) {
        return false;
    }

    bool changed = false;
    for (auto *call : calls) {
        if (call == nullptr || !call->has_uses()) {
            continue;
        }

        oir::Value *replacement = uniform->value;
        if (uniform->kind == UniformReturnKind::Argument) {
            auto actuals = call->args();
            if (uniform->argument_index >= actuals.size()) {
                continue;
            }
            replacement = actuals[uniform->argument_index];
        }
        if (replacement == nullptr || replacement->type() != call->type() ||
            !is_scalar_type(replacement->type())) {
            continue;
        }

        const auto before = call->use_count();
        call->replace_all_uses_with(replacement);
        if (call->use_count() != before) {
            ++stats.ipsccp;
            changed = true;
        }
    }
    return changed;
}

} // namespace

bool propagate_interprocedural_constants(oir::Module &module, Stats &stats) {
    bool changed = false;
    constexpr unsigned kMaxRounds = 8;
    for (unsigned round = 0; round < kMaxRounds; ++round) {
        bool round_changed = false;

        for (auto &function : module.functions()) {
            auto calls = closed_world_call_sites(*function);
            if (!calls.has_value()) {
                continue;
            }
            round_changed |= propagate_constant_arguments(*function, *calls, stats);
        }

        round_changed |= run_sccp(module, stats);

        for (auto &function : module.functions()) {
            auto calls = closed_world_call_sites(*function);
            if (!calls.has_value()) {
                continue;
            }
            round_changed |= propagate_uniform_returns(*function, *calls, stats);
        }

        if (!round_changed) {
            break;
        }
        changed = true;
    }
    return changed;
}

} // namespace pass::oir_opt
