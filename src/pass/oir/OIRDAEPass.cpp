#include "pass/oir/OIRDAEPass.h"

#include "oir/OIRScalarOpt.h"

#include <algorithm>
#include <vector>

namespace pass::oir_opt {
namespace {

bool is_direct_call_user(oir::User *user, oir::Function &function) {
    auto *call = dynamic_cast<oir::CallInst *>(user);
    return call != nullptr && call->callee() == &function;
}

bool is_recursive(oir::Function &function) {
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            auto *call = dynamic_cast<oir::CallInst *>(inst.get());
            if (call != nullptr && call->callee() == &function) {
                return true;
            }
        }
    }
    return false;
}

std::vector<oir::CallInst *> direct_call_sites(oir::Function &function) {
    std::vector<oir::CallInst *> calls;
    for (auto *user : function.users()) {
        if (!is_direct_call_user(user, function)) {
            calls.clear();
            return calls;
        }
        calls.push_back(static_cast<oir::CallInst *>(user));
    }
    return calls;
}

bool run_dae_on_function(oir::Function &function, Stats &stats) {
    if (function.is_external() || function.name() == "main" || is_recursive(function)) {
        return false;
    }

    const auto arg_count = function.args().size();
    if (arg_count == 0) {
        return false;
    }

    std::vector<bool> keep(arg_count, true);
    bool any_removed = false;
    for (std::size_t i = 0; i < arg_count; ++i) {
        if (!function.args()[i]->has_uses()) {
            keep[i] = false;
            any_removed = true;
        }
    }
    if (!any_removed) {
        return false;
    }

    auto calls = direct_call_sites(function);
    if (calls.empty() && function.has_uses()) {
        return false;
    }
    for (auto *call : calls) {
        if (call->args().size() != arg_count) {
            return false;
        }
    }

    for (auto *call : calls) {
        for (std::size_t i = arg_count; i > 0; --i) {
            if (!keep[i - 1]) {
                call->remove_arg(i - 1);
            }
        }
    }

    std::vector<oir::Type *> params;
    for (std::size_t i = 0; i < arg_count; ++i) {
        if (keep[i]) {
            params.push_back(function.args()[i]->type());
        }
    }
    function.set_function_type(function.parent()->types().func_ty(function.return_type(), params));
    function.keep_arguments(keep);
    ++stats.dae;
    return true;
}

} // namespace

bool eliminate_dead_arguments(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        changed |= run_dae_on_function(*function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt

namespace pass {

std::string_view OIRDAEPass::name() const {
    return "OIRDAEPass";
}

PassKind OIRDAEPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRDAEPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(context, "OIRDAEPass requires OIR module in pass context",
                                      [](oir::Module &module, oir_opt::Stats &stats) {
                                          return oir_opt::eliminate_dead_arguments(module, stats);
                                      });
}

} // namespace pass
