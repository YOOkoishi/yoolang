#include "oir/OIRAnalysis.h"
#include "oir/OIRScalarOpt.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pass::oir_opt {
namespace {

constexpr std::size_t kMaxPromotedPathsPerArgument = 4;

struct IndexSpec {
    oir::Type *type = nullptr;
    std::int64_t value = 0;
};

struct GEPStep {
    oir::Type *result_type = nullptr;
    std::vector<IndexSpec> indices;
};

struct LoadPath {
    std::string key;
    oir::Type *loaded_type = nullptr;
    std::vector<GEPStep> steps;
    std::vector<oir::LoadInst *> loads;
};

struct ArgumentPlan {
    oir::Argument *argument = nullptr;
    std::size_t original_index = 0;
    std::vector<LoadPath> paths;
    std::unordered_set<oir::GetElementPtrInst *> geps;
};

struct FunctionPlan {
    oir::Function *function = nullptr;
    std::vector<oir::CallInst *> calls;
    std::vector<ArgumentPlan> arguments;
};

bool is_prefix_barrier(const oir::Instruction &inst) {
    switch (inst.op()) {
    case oir::Instruction::OpID::Ret:
    case oir::Instruction::OpID::Br:
    case oir::Instruction::OpID::Store:
    case oir::Instruction::OpID::Call:
    case oir::Instruction::OpID::MemZero:
        return true;
    default:
        return false;
    }
}

std::unordered_set<const oir::Instruction *> entry_safe_prefix(const oir::Function &function) {
    std::unordered_set<const oir::Instruction *> prefix;
    auto *entry = function.entry_block();
    if (entry == nullptr) {
        return prefix;
    }

    for (const auto &inst : entry->instructions()) {
        if (is_prefix_barrier(*inst)) {
            break;
        }
        prefix.insert(inst.get());
    }
    return prefix;
}

std::string path_key(const std::vector<GEPStep> &steps, oir::Type *loaded_type) {
    std::string key = "load:" + loaded_type->print();
    for (const auto &step : steps) {
        key += "/gep:" + step.result_type->print();
        for (const auto &index : step.indices) {
            key += ":" + index.type->print() + "=" + std::to_string(index.value);
        }
    }
    return key;
}

bool collect_pointer_loads(oir::Value *ptr, const std::vector<GEPStep> &steps,
                           const std::unordered_set<const oir::Instruction *> &safe_prefix,
                           std::unordered_map<std::string, LoadPath> &paths,
                           std::unordered_set<oir::GetElementPtrInst *> &geps,
                           std::unordered_set<oir::Value *> &active) {
    if (ptr == nullptr || ptr->type() == nullptr || !ptr->type()->is_pointer() ||
        !active.insert(ptr).second) {
        return false;
    }

    bool ok = true;
    const auto uses = ptr->uses();
    if (uses.empty()) {
        active.erase(ptr);
        return false;
    }

    for (const auto &use : uses) {
        auto *inst = dynamic_cast<oir::Instruction *>(use.user);
        if (inst == nullptr || safe_prefix.find(inst) == safe_prefix.end()) {
            ok = false;
            break;
        }

        if (auto *gep = dynamic_cast<oir::GetElementPtrInst *>(inst)) {
            if (gep->base_ptr() != ptr || use.operand_index != 0) {
                ok = false;
                break;
            }

            GEPStep step;
            step.result_type = gep->type();
            for (auto *index : gep->indices()) {
                auto constant = int_constant(index);
                if (!constant.has_value()) {
                    ok = false;
                    break;
                }
                step.indices.push_back({index->type(), *constant});
            }
            if (!ok || step.indices.empty()) {
                ok = false;
                break;
            }

            auto nested_steps = steps;
            nested_steps.push_back(std::move(step));
            geps.insert(gep);
            if (!collect_pointer_loads(gep, nested_steps, safe_prefix, paths, geps, active)) {
                ok = false;
                break;
            }
            continue;
        }

        auto *load = dynamic_cast<oir::LoadInst *>(inst);
        auto *ptr_type = dynamic_cast<oir::PointerType *>(ptr->type());
        if (load == nullptr || load->ptr() != ptr || use.operand_index != 0 ||
            ptr_type == nullptr || ptr_type->element_type() != load->type() ||
            !is_scalar_type(load->type())) {
            ok = false;
            break;
        }

        const auto key = path_key(steps, load->type());
        auto [found, inserted] = paths.emplace(key, LoadPath{key, load->type(), steps, {}});
        if (!inserted && found->second.loaded_type != load->type()) {
            ok = false;
            break;
        }
        if (std::find(found->second.loads.begin(), found->second.loads.end(), load) ==
            found->second.loads.end()) {
            found->second.loads.push_back(load);
        }
        if (paths.size() > kMaxPromotedPathsPerArgument) {
            ok = false;
            break;
        }
    }

    active.erase(ptr);
    return ok;
}

bool analyze_argument(oir::Argument &argument, std::size_t original_index,
                      const std::unordered_set<const oir::Instruction *> &safe_prefix,
                      ArgumentPlan &plan) {
    if (argument.type() == nullptr || !argument.type()->is_pointer()) {
        return false;
    }

    std::unordered_map<std::string, LoadPath> paths;
    std::unordered_set<oir::GetElementPtrInst *> geps;
    std::unordered_set<oir::Value *> active;
    if (!collect_pointer_loads(&argument, {}, safe_prefix, paths, geps, active) || paths.empty() ||
        paths.size() > kMaxPromotedPathsPerArgument) {
        return false;
    }

    plan.argument = &argument;
    plan.original_index = original_index;
    plan.geps = std::move(geps);
    plan.paths.reserve(paths.size());
    for (auto &[key, path] : paths) {
        (void)key;
        plan.paths.push_back(std::move(path));
    }
    std::sort(plan.paths.begin(), plan.paths.end(),
              [](const LoadPath &lhs, const LoadPath &rhs) { return lhs.key < rhs.key; });
    return true;
}

bool call_shape_matches(const oir::CallInst &call, const oir::Function &function) {
    if (call.callee() != &function || call.type() != function.return_type() ||
        call.parent() == nullptr) {
        return false;
    }
    const auto &instructions = call.parent()->instructions();
    if (std::none_of(instructions.begin(), instructions.end(),
                     [&call](const auto &inst) { return inst.get() == &call; })) {
        return false;
    }
    const auto args = call.args();
    if (args.size() != function.args().size()) {
        return false;
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == nullptr || args[i]->type() != function.args()[i]->type()) {
            return false;
        }
    }
    return true;
}

bool collect_direct_calls(oir::Function &function, std::vector<oir::CallInst *> &calls) {
    std::unordered_set<oir::CallInst *> seen;
    for (const auto &use : function.uses()) {
        auto *call = dynamic_cast<oir::CallInst *>(use.user);
        if (call == nullptr || use.operand_index != 0 || call->callee() != &function ||
            !call_shape_matches(*call, function)) {
            return false;
        }
        if (seen.insert(call).second) {
            calls.push_back(call);
        }
    }
    return !calls.empty();
}

bool reaches_function(const oir::Function &current, const oir::Function &target,
                      std::unordered_set<const oir::Function *> &seen) {
    if (!seen.insert(&current).second) {
        return false;
    }
    for (const auto &block : current.blocks()) {
        for (const auto &inst : block->instructions()) {
            auto *call = dynamic_cast<const oir::CallInst *>(inst.get());
            auto *callee =
                call == nullptr ? nullptr : dynamic_cast<const oir::Function *>(call->callee());
            if (callee == nullptr || callee->is_external()) {
                continue;
            }
            if (callee == &target || reaches_function(*callee, target, seen)) {
                return true;
            }
        }
    }
    return false;
}

bool is_recursive(const oir::Function &function) {
    std::unordered_set<const oir::Function *> seen;
    return reaches_function(function, function, seen);
}

bool analyze_function(oir::Function &function, FunctionPlan &plan) {
    if (function.is_external() || function.name() == "main" || function.entry_block() == nullptr ||
        is_recursive(function)) {
        return false;
    }

    std::vector<oir::CallInst *> calls;
    if (!collect_direct_calls(function, calls)) {
        return false;
    }

    const auto safe_prefix = entry_safe_prefix(function);
    std::vector<ArgumentPlan> arguments;
    for (std::size_t index = 0; index < function.args().size(); ++index) {
        ArgumentPlan argument_plan;
        if (analyze_argument(*function.args()[index], index, safe_prefix, argument_plan)) {
            arguments.push_back(std::move(argument_plan));
        }
    }
    if (arguments.empty()) {
        return false;
    }

    plan.function = &function;
    plan.calls = std::move(calls);
    plan.arguments = std::move(arguments);
    return true;
}

std::list<std::unique_ptr<oir::Instruction>>::iterator
find_instruction(oir::BasicBlock &block, const oir::Instruction *needle) {
    return std::find_if(block.instructions().begin(), block.instructions().end(),
                        [needle](const auto &inst) { return inst.get() == needle; });
}

oir::Value *materialize_path_before_call(oir::Module &module, oir::CallInst &call,
                                         oir::Value *actual, const LoadPath &path,
                                         std::size_t promoted_index) {
    auto *block = call.parent();
    auto call_it = find_instruction(*block, &call);
    oir::Value *ptr = actual;

    for (std::size_t step_index = 0; step_index < path.steps.size(); ++step_index) {
        const auto &step = path.steps[step_index];
        std::vector<oir::Value *> indices;
        indices.reserve(step.indices.size());
        for (const auto &index : step.indices) {
            indices.push_back(make_int_constant(module, index.type, index.value));
        }

        auto gep = std::make_unique<oir::GetElementPtrInst>(
            step.result_type, ptr, indices, block,
            "argprom.gep." + std::to_string(promoted_index) + "." + std::to_string(step_index));
        auto *raw = gep.get();
        raw->set_parent(block);
        block->instructions().insert(call_it, std::move(gep));
        ptr = raw;
    }

    auto load = std::make_unique<oir::LoadInst>(path.loaded_type, ptr, block,
                                                "argprom.load." + std::to_string(promoted_index));
    auto *raw = load.get();
    raw->set_parent(block);
    block->instructions().insert(call_it, std::move(load));
    return raw;
}

void erase_promoted_memory_ops(const FunctionPlan &plan) {
    std::unordered_set<oir::Instruction *> dead;
    for (const auto &argument : plan.arguments) {
        for (auto *gep : argument.geps) {
            dead.insert(gep);
        }
        for (const auto &path : argument.paths) {
            for (auto *load : path.loads) {
                dead.insert(load);
            }
        }
    }

    for (auto *inst : dead) {
        inst->drop_all_operands();
    }
    for (auto &block : plan.function->blocks()) {
        for (auto it = block->instructions().begin(); it != block->instructions().end();) {
            if (dead.find(it->get()) != dead.end()) {
                it = block->instructions().erase(it);
            } else {
                ++it;
            }
        }
    }
}

bool apply_plan(oir::Module &module, FunctionPlan &plan, Stats &stats) {
    auto &function = *plan.function;
    const std::size_t original_arg_count = function.args().size();

    std::vector<std::vector<oir::Value *>> call_promoted_values(plan.calls.size());
    for (std::size_t call_index = 0; call_index < plan.calls.size(); ++call_index) {
        auto &call = *plan.calls[call_index];
        const auto actuals = call.args();
        std::size_t promoted_index = 0;
        for (const auto &argument : plan.arguments) {
            auto *actual = actuals[argument.original_index];
            for (const auto &path : argument.paths) {
                call_promoted_values[call_index].push_back(
                    materialize_path_before_call(module, call, actual, path, promoted_index++));
            }
        }
    }

    std::vector<oir::Argument *> promoted_arguments;
    for (const auto &argument : plan.arguments) {
        std::size_t path_index = 0;
        for (const auto &path : argument.paths) {
            std::string name =
                argument.argument->name().empty() ? "arg" : argument.argument->name();
            name += ".promoted." + std::to_string(path_index++);
            promoted_arguments.push_back(function.add_argument(path.loaded_type, name));
        }
    }

    std::size_t promoted_arg_index = 0;
    for (const auto &argument : plan.arguments) {
        for (const auto &path : argument.paths) {
            auto *replacement = promoted_arguments[promoted_arg_index++];
            for (auto *load : path.loads) {
                load->replace_all_uses_with(replacement);
            }
        }
    }
    erase_promoted_memory_ops(plan);

    std::vector<std::size_t> removed_indices;
    removed_indices.reserve(plan.arguments.size());
    for (const auto &argument : plan.arguments) {
        removed_indices.push_back(argument.original_index);
    }
    std::sort(removed_indices.begin(), removed_indices.end(), std::greater<std::size_t>());

    for (std::size_t call_index = 0; call_index < plan.calls.size(); ++call_index) {
        auto &call = *plan.calls[call_index];
        for (auto *value : call_promoted_values[call_index]) {
            call.add_operand(value);
        }
        for (auto index : removed_indices) {
            call.remove_arg(index);
        }
    }

    std::vector<bool> keep(function.args().size(), true);
    for (const auto &argument : plan.arguments) {
        keep[argument.original_index] = false;
    }

    std::vector<oir::Type *> final_types;
    final_types.reserve(function.args().size() - plan.arguments.size());
    for (std::size_t i = 0; i < original_arg_count; ++i) {
        if (keep[i]) {
            final_types.push_back(function.args()[i]->type());
        }
    }
    for (auto *argument : promoted_arguments) {
        final_types.push_back(argument->type());
    }

    function.set_function_type(module.types().func_ty(function.return_type(), final_types));
    function.keep_arguments(keep);
    stats.arg_promotion += static_cast<unsigned>(plan.arguments.size());
    return true;
}

} // namespace

bool promote_fixed_load_arguments(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        FunctionPlan plan;
        if (analyze_function(*function, plan)) {
            changed |= apply_plan(module, plan, stats);
        }
    }
    return changed;
}

} // namespace pass::oir_opt
