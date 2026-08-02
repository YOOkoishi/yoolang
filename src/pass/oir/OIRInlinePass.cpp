#include "pass/oir/OIRInlinePass.h"

#include "oir/OIRScalarOpt.h"

#include "oir/OIRAnalysis.h"
#include "oir/OIRCFGUtils.h"
#include "pass/oir/OIRCostModel.h"

#include <algorithm>
#include <deque>
#include <list>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

constexpr unsigned kMaxInlineSites = 128;
constexpr unsigned kMaxCalleeBlocks = 12;
constexpr unsigned kMaxCalleeReturns = 4;
constexpr unsigned kMaxCalleeParams = 16;
constexpr unsigned kMaxSpecializedInlineBlocks = 96;
constexpr unsigned kMaxSpecializedInlineReturns = 8;
constexpr unsigned kMaxSpecializationCalleeBlocks = 64;
constexpr unsigned kMaxSpecializedFunctions = 48;
constexpr unsigned kMaxSpecializedCallSites = 128;
constexpr unsigned kMaxRecursiveConstantSpecializationLayers = 10;
constexpr unsigned kMaxRecursiveSpecializationCalleeBlocks = 64;
constexpr unsigned kMaxRecursiveSpecializationStaticInstrs = 1600;
constexpr unsigned kMaxRecursiveInlineDepth = 8;
constexpr unsigned kMaxRecursiveCalleeBlocks = 16;
constexpr unsigned kMaxRecursiveCalleeReturns = 8;
constexpr unsigned kRecursiveInlineEdgeProbabilityBasisPoints = 5000;
constexpr unsigned kMinRecursiveInlineProbabilityBasisPoints = 1000;
// OIR counts control/phi instructions separately from GCC's inline size units;
// 512 OIR instructions is the corresponding bounded-growth envelope.
constexpr unsigned kMaxRecursiveInlineGrowth = 512;
constexpr unsigned kMaxAdvisedCalleeBlocks = 96;
constexpr unsigned kMaxAdvisedCalleeReturns = 32;

using ValueMap = std::unordered_map<oir::Value *, oir::Value *>;
using BlockMap = std::unordered_map<oir::BasicBlock *, oir::BasicBlock *>;
using SpecializationMask = std::vector<bool>;

struct CalleeInfo {
    unsigned blocks = 0;
    unsigned cost = 0;
    unsigned returns = 0;
    unsigned static_instrs = 0;
    unsigned int_alu = 0;
    unsigned int_mul = 0;
    unsigned int_div_rem = 0;
    unsigned fp_alu = 0;
    unsigned fp_div = 0;
    unsigned loads = 0;
    unsigned stores = 0;
    unsigned pointer_arith = 0;
    unsigned branches = 0;
    unsigned calls = 0;
    unsigned phis = 0;
};

struct InlineContext {
    std::unordered_map<oir::Function *, std::unique_ptr<oir::Function>> recursive_templates;
    std::unordered_map<const oir::CallInst *, unsigned> recursive_call_depths;
    std::unordered_map<const oir::Function *, unsigned> recursive_growth;
    std::unordered_set<const oir::Function *> defer_recursive_expansion;
};

struct CallsiteEstimate {
    unsigned constant_arguments = 0;
    unsigned constant_argument_uses = 0;
    unsigned predicted_eliminated_instrs = 0;
    unsigned predicted_eliminated_branches = 0;
    unsigned loop_depth = 0;
    unsigned execution_scale = 1;
    unsigned effective_growth = 0;
    unsigned callee_call_sites = 0;
    bool result_unused = false;
};

bool is_constprop_specialization(const oir::Function &function);
bool mask_selects_argument(const SpecializationMask &mask, std::size_t index);
bool is_specializable_constant(oir::Value *value);

std::string inline_name(const oir::Function &callee, const oir::Value &value,
                        unsigned inline_index) {
    std::string base = value.name().empty() ? "tmp" : value.name();
    return "inl." + callee.name() + "." + std::to_string(inline_index) + "." + base;
}

CalleeInfo inspect_callee(const oir::Function &function) {
    CalleeInfo info;
    for (const auto &block : function.blocks()) {
        ++info.blocks;
        for (const auto &inst : block->instructions()) {
            switch (inst->op()) {
            case oir::Instruction::OpID::Ret:
                ++info.returns;
                ++info.branches;
                continue;
            case oir::Instruction::OpID::Br:
                ++info.branches;
                continue;
            case oir::Instruction::OpID::Phi:
                ++info.phis;
                continue;
            case oir::Instruction::OpID::Mul:
                ++info.int_mul;
                break;
            case oir::Instruction::OpID::SDiv:
            case oir::Instruction::OpID::SRem:
                ++info.int_div_rem;
                break;
            case oir::Instruction::OpID::FAdd:
            case oir::Instruction::OpID::FSub:
            case oir::Instruction::OpID::FMul:
                ++info.fp_alu;
                break;
            case oir::Instruction::OpID::FDiv:
                ++info.fp_div;
                break;
            case oir::Instruction::OpID::Load:
                ++info.loads;
                break;
            case oir::Instruction::OpID::Store:
                ++info.stores;
                break;
            case oir::Instruction::OpID::GetElementPtr:
                ++info.pointer_arith;
                break;
            case oir::Instruction::OpID::Call:
                ++info.calls;
                break;
            default:
                ++info.int_alu;
                break;
            }
            ++info.static_instrs;
            ++info.cost;
            if (inst->op() == oir::Instruction::OpID::Call) {
                info.cost += 4;
            }
        }
    }
    return info;
}

unsigned module_instruction_count(const oir::Module &module) {
    unsigned count = 0;
    for (const auto &function : module.functions()) {
        if (!function->is_external()) {
            count += inspect_callee(*function).static_instrs;
        }
    }
    return std::max(1U, count);
}

unsigned direct_callsite_count(const oir::Module &module, const oir::Function &callee) {
    unsigned count = 0;
    for (const auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        for (const auto &block : function->blocks()) {
            for (const auto &inst : block->instructions()) {
                auto *call = dynamic_cast<const oir::CallInst *>(inst.get());
                if (call != nullptr && call->callee() == &callee) {
                    ++count;
                }
            }
        }
    }
    return count;
}

unsigned loop_depth_for_block(const oir::Function &function, const oir::BasicBlock &block) {
    oir::DominatorTree dom_tree(function);
    oir::LoopInfo loop_info(function, dom_tree);
    unsigned depth = 0;
    for (const auto &loop : loop_info.loops()) {
        if (std::find(loop.blocks.begin(), loop.blocks.end(), &block) != loop.blocks.end()) {
            ++depth;
        }
    }
    return depth;
}

std::pair<unsigned, unsigned>
estimate_constant_cfg_cleanup(const oir::CallInst &call, const oir::Function &callee) {
    std::unordered_map<const oir::Value *, std::int64_t> known;
    const auto actual_args = call.args();
    for (std::size_t index = 0;
         index < actual_args.size() && index < callee.args().size(); ++index) {
        if (auto value = int_constant(actual_args[index])) {
            known[callee.args()[index].get()] = *value;
        }
    }
    if (known.empty() || callee.entry_block() == nullptr) {
        return {0, 0};
    }

    auto known_int = [&](const oir::Value *value) -> std::optional<std::int64_t> {
        if (auto literal = int_constant(const_cast<oir::Value *>(value))) {
            return literal;
        }
        auto found = known.find(value);
        return found == known.end() ? std::nullopt
                                    : std::optional<std::int64_t>(found->second);
    };

    std::unordered_set<const oir::BasicBlock *> reachable{callee.entry_block()};
    std::unordered_set<const oir::BranchInst *> decided_branches;
    const unsigned max_rounds =
        std::max<unsigned>(1, static_cast<unsigned>(callee.blocks().size()) * 2);
    for (unsigned round = 0; round < max_rounds; ++round) {
        bool changed = false;
        for (const auto &block : callee.blocks()) {
            if (reachable.find(block.get()) == reachable.end()) {
                continue;
            }
            for (const auto &inst : block->instructions()) {
                std::optional<std::int64_t> result;
                if (auto *binary = dynamic_cast<const oir::BinaryInst *>(inst.get())) {
                    const auto lhs = known_int(binary->lhs());
                    const auto rhs = known_int(binary->rhs());
                    if (lhs && rhs) {
                        result = fold_int_binary(binary->op(), *lhs, *rhs);
                    }
                } else if (auto *cmp = dynamic_cast<const oir::CmpInst *>(inst.get())) {
                    const auto lhs = known_int(cmp->lhs());
                    const auto rhs = known_int(cmp->rhs());
                    if (lhs && rhs && cmp->op() == oir::Instruction::OpID::ICmp) {
                        result = eval_cmp(cmp->pred(), *lhs, *rhs) ? 1 : 0;
                    }
                } else if (auto *cast = dynamic_cast<const oir::CastInst *>(inst.get())) {
                    if (cast->op() == oir::Instruction::OpID::ZExt) {
                        result = known_int(cast->src());
                    }
                }
                if (result && known.emplace(inst.get(), *result).second) {
                    changed = true;
                }

                auto *branch = dynamic_cast<const oir::BranchInst *>(inst.get());
                if (branch == nullptr) {
                    continue;
                }
                if (!branch->is_conditional()) {
                    changed |= reachable.insert(branch->target_bb()).second;
                    continue;
                }
                if (auto condition = known_int(branch->cond())) {
                    changed |= reachable
                                   .insert(*condition != 0 ? branch->true_bb()
                                                           : branch->false_bb())
                                   .second;
                    decided_branches.insert(branch);
                } else {
                    changed |= reachable.insert(branch->true_bb()).second;
                    changed |= reachable.insert(branch->false_bb()).second;
                }
            }
        }
        if (!changed) {
            break;
        }
    }

    unsigned unreachable_instrs = 0;
    for (const auto &block : callee.blocks()) {
        if (reachable.find(block.get()) != reachable.end()) {
            continue;
        }
        unreachable_instrs += static_cast<unsigned>(block->instructions().size());
    }
    return {unreachable_instrs, static_cast<unsigned>(decided_branches.size())};
}

CallsiteEstimate inspect_callsite(const oir::Module &module, const oir::Function &caller,
                                  const oir::BasicBlock &block, const oir::CallInst &call,
                                  const oir::Function &callee, const CalleeInfo &info) {
    CallsiteEstimate estimate;
    const auto args = call.args();
    for (std::size_t index = 0; index < args.size() && index < callee.args().size(); ++index) {
        if (!is_specializable_constant(args[index])) {
            continue;
        }
        ++estimate.constant_arguments;
        estimate.constant_argument_uses += static_cast<unsigned>(
            std::min<std::size_t>(callee.args()[index]->use_count(), 8));
    }

    estimate.loop_depth = loop_depth_for_block(caller, block);
    estimate.execution_scale = 1U << std::min(estimate.loop_depth, 2U);
    estimate.callee_call_sites = direct_callsite_count(module, callee);
    estimate.result_unused = !call.type()->is_void() && !call.has_uses();

    // This is deliberately a bounded, read-only forecast. Constant argument uses are a proxy for
    // the SCCP/algebraic cleanup enabled after cloning; no scratch IR or solver is involved.
    estimate.predicted_eliminated_instrs =
        std::min<unsigned>(info.static_instrs,
                           estimate.constant_argument_uses * 2 +
                               estimate.constant_arguments +
                               (estimate.result_unused ? 1U : 0U));
    estimate.predicted_eliminated_branches =
        std::min<unsigned>(info.branches,
                           estimate.constant_argument_uses / 2 +
                               (estimate.constant_arguments != 0 ? 1U : 0U));
    const auto [unreachable_instrs, decided_branches] =
        estimate_constant_cfg_cleanup(call, callee);
    estimate.predicted_eliminated_instrs =
        std::max(estimate.predicted_eliminated_instrs, unreachable_instrs);
    estimate.predicted_eliminated_branches =
        std::max(estimate.predicted_eliminated_branches, decided_branches);
    // OIR static instruction growth excludes terminators and phis; charge the same unit that the
    // shared policy's inline thresholds use, plus only the merge required for multiple returns.
    const unsigned cloned_growth = info.static_instrs + (info.returns > 1 ? 1U : 0U);
    const unsigned cleanup_credit = estimate.predicted_eliminated_instrs;
    estimate.effective_growth =
        cloned_growth > cleanup_credit ? cloned_growth - cleanup_credit : 0;
    if (estimate.callee_call_sites == 1) {
        // A sole callsite gives later whole-program cleanup more leverage, but this pass keeps the
        // original definition for stable diagnostics and downstream contracts. Credit only half
        // of the predicted growth instead of assuming the definition has already disappeared.
        estimate.effective_growth /= 2;
    }
    return estimate;
}

bool within_inline_resource_limit(const CalleeInfo &info,
                                  const pass::cost_model::CostModelPolicy &policy) {
    return info.cost <= static_cast<unsigned>(policy.max_inline_callee_cost) &&
           info.returns != 0 && info.returns <= kMaxCalleeReturns;
}

void fill_before_after_from_callee(OIRTransformCostEstimate &estimate, const CalleeInfo &info,
                                   std::int64_t before_calls, std::int64_t after_calls) {
    estimate.has_detailed_instruction_mix = true;
    estimate.before_instrs = static_cast<std::int64_t>(info.static_instrs + before_calls);
    estimate.before_code_bytes = estimate.before_instrs * 4;
    estimate.before_int_alu = static_cast<std::int64_t>(info.int_alu);
    estimate.before_int_mul = static_cast<std::int64_t>(info.int_mul);
    estimate.before_int_div_rem = static_cast<std::int64_t>(info.int_div_rem);
    estimate.before_fp_alu = static_cast<std::int64_t>(info.fp_alu);
    estimate.before_fp_div = static_cast<std::int64_t>(info.fp_div);
    estimate.before_loads = static_cast<std::int64_t>(info.loads);
    estimate.before_stores = static_cast<std::int64_t>(info.stores);
    estimate.before_pointer_arith = static_cast<std::int64_t>(info.pointer_arith);
    estimate.before_branches = static_cast<std::int64_t>(info.branches);
    estimate.before_calls = before_calls;
    estimate.before_phis = static_cast<std::int64_t>(info.phis);
    estimate.before_live_values = static_cast<std::int64_t>(info.static_instrs + info.phis);
    estimate.before_max_live_values =
        static_cast<std::int64_t>(std::min<unsigned>(info.static_instrs + info.phis, 24));

    estimate.after_instrs = static_cast<std::int64_t>(info.static_instrs + after_calls);
    estimate.after_code_bytes = estimate.before_code_bytes;
    estimate.after_int_alu = estimate.before_int_alu;
    estimate.after_int_mul = estimate.before_int_mul;
    estimate.after_int_div_rem = estimate.before_int_div_rem;
    estimate.after_fp_alu = estimate.before_fp_alu;
    estimate.after_fp_div = estimate.before_fp_div;
    estimate.after_loads = estimate.before_loads;
    estimate.after_stores = estimate.before_stores;
    estimate.after_pointer_arith = estimate.before_pointer_arith;
    estimate.after_branches = estimate.before_branches;
    estimate.after_calls = after_calls;
    estimate.after_phis = estimate.before_phis;
    estimate.after_live_values = estimate.before_live_values;
    estimate.after_max_live_values = estimate.before_max_live_values;
}

bool contains_call_to(const oir::Function &function, const oir::Function &target) {
    for (const auto &block : function.blocks()) {
        for (const auto &inst : block->instructions()) {
            auto *call = dynamic_cast<const oir::CallInst *>(inst.get());
            if (call != nullptr && call->callee() == &target) {
                return true;
            }
        }
    }
    return false;
}

std::vector<oir::Function *> direct_callees(const oir::Function &function) {
    std::vector<oir::Function *> out;
    for (const auto &block : function.blocks()) {
        for (const auto &inst : block->instructions()) {
            auto *call = dynamic_cast<const oir::CallInst *>(inst.get());
            if (call == nullptr) {
                continue;
            }
            auto *callee = dynamic_cast<oir::Function *>(call->callee());
            if (callee != nullptr && !callee->is_external()) {
                out.push_back(callee);
            }
        }
    }
    return out;
}

bool reaches_function(const oir::Function &function, const oir::Function &target,
                      std::unordered_set<const oir::Function *> &seen) {
    if (!seen.insert(&function).second) {
        return false;
    }
    for (auto *callee : direct_callees(function)) {
        if (callee == &target || reaches_function(*callee, target, seen)) {
            return true;
        }
    }
    return false;
}

bool is_recursive_in_call_graph(const oir::Function &function) {
    std::unordered_set<const oir::Function *> seen;
    return reaches_function(function, function, seen);
}

bool reaches_recursive_function(const oir::Function &function,
                                std::unordered_set<const oir::Function *> &seen) {
    if (!seen.insert(&function).second) {
        return false;
    }
    for (auto *callee : direct_callees(function)) {
        if (is_recursive_in_call_graph(*callee) || reaches_recursive_function(*callee, seen)) {
            return true;
        }
    }
    return false;
}

bool has_recursive_call_graph_dependency(const oir::Function &function) {
    if (is_recursive_in_call_graph(function)) {
        return true;
    }
    std::unordered_set<const oir::Function *> seen;
    return reaches_recursive_function(function, seen);
}

std::string recursive_inline_marker(const oir::Function &function) {
    return "rinl." + function.name() + ".";
}

std::string recursive_inline_prefix(const oir::Function &function, unsigned depth) {
    std::string out;
    const std::string marker = recursive_inline_marker(function);
    for (unsigned i = 0; i < depth; ++i) {
        out += marker;
    }
    return out;
}

unsigned recursive_inline_depth(const InlineContext &context, const oir::CallInst &call) {
    auto found = context.recursive_call_depths.find(&call);
    return found == context.recursive_call_depths.end() ? 0 : found->second;
}

unsigned recursive_inline_probability(unsigned depth) {
    // GCC's default recursive-inline profile decays each call edge by roughly one
    // half even when sibling recursive calls execute sequentially.  Fanout is
    // bounded separately by the cumulative growth and pressure budgets.
    const unsigned edge_probability = kRecursiveInlineEdgeProbabilityBasisPoints;
    unsigned probability = edge_probability;
    for (unsigned level = 0; level < depth && probability != 0; ++level) {
        probability = probability * edge_probability / 10000;
    }
    return probability;
}

unsigned recursive_inline_growth_budget(const pass::cost_model::CostModelPolicy &policy) {
    const auto policy_growth = static_cast<unsigned>(
        std::max<std::int64_t>(1, policy.max_function_code_growth));
    const auto inline_scaled = static_cast<unsigned>(
        std::max<std::int64_t>(1, policy.max_inline_callee_cost) * 6);
    return std::min(kMaxRecursiveInlineGrowth, std::max(policy_growth, inline_scaled));
}

unsigned cloned_instruction_growth(const CalleeInfo &info) {
    return info.static_instrs + info.branches + info.phis + (info.returns > 1 ? 1 : 0);
}

unsigned recursive_pressure_growth(const oir::CallInst &call, const CalleeInfo &info,
                                   unsigned depth) {
    const auto per_level = std::min<std::size_t>(call.args().size(), 4) +
                           (info.calls > 0 ? info.calls - 1 : 0);
    return static_cast<unsigned>(per_level * depth);
}

void append_reachable_blocks(oir::BasicBlock *block, std::unordered_set<oir::BasicBlock *> &seen,
                             std::vector<oir::BasicBlock *> &out) {
    if (block == nullptr || !seen.insert(block).second) {
        return;
    }
    out.push_back(block);
    for (auto *succ : block->successors()) {
        append_reachable_blocks(succ, seen, out);
    }
}

std::vector<oir::BasicBlock *> clone_order(const oir::Function &function) {
    std::vector<oir::BasicBlock *> out;
    std::unordered_set<oir::BasicBlock *> seen;
    append_reachable_blocks(function.entry_block(), seen, out);
    for (const auto &block : function.blocks()) {
        if (seen.insert(block.get()).second) {
            out.push_back(block.get());
        }
    }
    return out;
}

bool has_compatible_call_shape(const oir::CallInst &call, const oir::Function &callee) {
    return call.type() == callee.return_type() && call.args().size() == callee.args().size() &&
           callee.args().size() <= kMaxCalleeParams;
}

bool has_scalar_recursive_signature(const oir::Function &function) {
    if (!function.return_type()->is_void() && !is_scalar_type(function.return_type())) {
        return false;
    }
    return std::all_of(function.args().begin(), function.args().end(), [](const auto &arg) {
        return is_scalar_type(arg->type());
    });
}

bool has_supported_recursive_body(const oir::Function &body, const oir::Function &recursive_target) {
    for (const auto &block : body.blocks()) {
        for (const auto &inst : block->instructions()) {
            if (inst->op() == oir::Instruction::OpID::SDiv ||
                inst->op() == oir::Instruction::OpID::SRem ||
                inst->op() == oir::Instruction::OpID::FDiv) {
                return false;
            }
            if (auto *call = dynamic_cast<const oir::CallInst *>(inst.get())) {
                if (call->callee() != &recursive_target) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool is_eligible_non_recursive_call(const oir::Function &caller, const oir::CallInst &call,
                                    oir::Function *callee,
                                    const pass::cost_model::CostModelPolicy &policy) {
    if (callee == nullptr || callee == &caller || callee->is_external() ||
        callee->entry_block() == nullptr || !has_compatible_call_shape(call, *callee)) {
        return false;
    }
    if (contains_call_to(*callee, *callee) || contains_call_to(*callee, caller)) {
        return false;
    }

    auto info = inspect_callee(*callee);
    if (is_constprop_specialization(*callee)) {
        return info.blocks <= kMaxSpecializedInlineBlocks &&
               info.cost <= static_cast<unsigned>(policy.max_inline_callee_cost * 3) &&
               info.returns != 0 &&
               info.returns <= kMaxSpecializedInlineReturns;
    }
    const auto advised_cost_cap = static_cast<unsigned>(
        std::max<std::int64_t>(1, policy.max_inline_callee_cost) * 4);
    return info.blocks <= kMaxAdvisedCalleeBlocks && info.cost <= advised_cost_cap &&
           info.returns != 0 && info.returns <= kMaxAdvisedCalleeReturns;
}

bool is_eligible_recursive_call(const oir::Function &caller, const oir::CallInst &call,
                                const oir::Function &callee_template,
                                const pass::cost_model::CostModelPolicy &policy,
                                unsigned depth, unsigned current_growth) {
    if (call.callee() != &caller || caller.is_external() || caller.entry_block() == nullptr ||
        !has_compatible_call_shape(call, callee_template) ||
        !has_scalar_recursive_signature(caller) ||
        !has_supported_recursive_body(callee_template, caller) ||
        depth >= kMaxRecursiveInlineDepth) {
        return false;
    }

    auto info = inspect_callee(callee_template);
    return recursive_inline_probability(depth) >=
               kMinRecursiveInlineProbabilityBasisPoints &&
           info.blocks <= kMaxRecursiveCalleeBlocks &&
           info.cost <= static_cast<unsigned>(policy.max_inline_callee_cost * 2) &&
           info.returns != 0 && info.returns <= kMaxRecursiveCalleeReturns &&
           recursive_pressure_growth(call, info, depth) <=
               static_cast<unsigned>(std::max<std::int64_t>(
                   0, policy.max_register_pressure_growth)) &&
           current_growth + cloned_instruction_growth(info) <=
               recursive_inline_growth_budget(policy);
}

oir::Value *map_value(oir::Value *value, const ValueMap &values, const BlockMap &blocks) {
    if (value == nullptr) {
        return nullptr;
    }
    if (auto *block = dynamic_cast<oir::BasicBlock *>(value)) {
        auto found = blocks.find(block);
        if (found == blocks.end()) {
            throw std::runtime_error("inline clone cannot map callee block %" + block->name());
        }
        return found->second;
    }
    auto found = values.find(value);
    if (found != values.end()) {
        return found->second;
    }
    return value;
}

std::vector<oir::Value *> map_values(const std::vector<oir::Value *> &input,
                                     const ValueMap &values, const BlockMap &blocks) {
    std::vector<oir::Value *> out;
    out.reserve(input.size());
    for (auto *value : input) {
        out.push_back(map_value(value, values, blocks));
    }
    return out;
}

std::unique_ptr<oir::Instruction> clone_non_phi_instruction(oir::Module &module,
                                                            const oir::Function &callee,
                                                            oir::Instruction &inst,
                                                            oir::BasicBlock *parent,
                                                            const ValueMap &values,
                                                            const BlockMap &blocks,
                                                            unsigned inline_index) {
    const std::string name = inline_name(callee, inst, inline_index);
    switch (inst.op()) {
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::Mul:
    case oir::Instruction::OpID::SDiv:
    case oir::Instruction::OpID::SRem:
    case oir::Instruction::OpID::And:
    case oir::Instruction::OpID::Xor:
    case oir::Instruction::OpID::FAdd:
    case oir::Instruction::OpID::FSub:
    case oir::Instruction::OpID::FMul:
    case oir::Instruction::OpID::FDiv: {
        auto &binary = static_cast<oir::BinaryInst &>(inst);
        return std::make_unique<oir::BinaryInst>(
            inst.type(), inst.op(), map_value(binary.lhs(), values, blocks),
            map_value(binary.rhs(), values, blocks), parent, name);
    }
    case oir::Instruction::OpID::ICmp:
    case oir::Instruction::OpID::FCmp: {
        auto &cmp = static_cast<oir::CmpInst &>(inst);
        return std::make_unique<oir::CmpInst>(
            inst.type(), inst.op(), cmp.pred(), map_value(cmp.lhs(), values, blocks),
            map_value(cmp.rhs(), values, blocks), parent, name);
    }
    case oir::Instruction::OpID::ZExt:
    case oir::Instruction::OpID::SIToFP:
    case oir::Instruction::OpID::FPToSI: {
        auto &cast = static_cast<oir::CastInst &>(inst);
        return std::make_unique<oir::CastInst>(inst.type(), inst.op(),
                                               map_value(cast.src(), values, blocks), parent, name);
    }
    case oir::Instruction::OpID::Alloca: {
        auto &alloca = static_cast<oir::AllocaInst &>(inst);
        return std::make_unique<oir::AllocaInst>(inst.type(), alloca.allocated_type(), parent,
                                                 name);
    }
    case oir::Instruction::OpID::Load: {
        auto &load = static_cast<oir::LoadInst &>(inst);
        return std::make_unique<oir::LoadInst>(inst.type(), map_value(load.ptr(), values, blocks),
                                               parent, name);
    }
    case oir::Instruction::OpID::Store: {
        auto &store = static_cast<oir::StoreInst &>(inst);
        return std::make_unique<oir::StoreInst>(
            module.types().void_ty(), map_value(store.value(), values, blocks),
            map_value(store.ptr(), values, blocks), parent);
    }
    case oir::Instruction::OpID::MemZero: {
        auto &memzero = static_cast<oir::MemZeroInst &>(inst);
        return std::make_unique<oir::MemZeroInst>(
            module.types().void_ty(), map_value(memzero.ptr(), values, blocks),
            map_value(memzero.byte_value(), values, blocks),
            map_value(memzero.byte_count(), values, blocks), parent);
    }
    case oir::Instruction::OpID::GetElementPtr: {
        auto &gep = static_cast<oir::GetElementPtrInst &>(inst);
        return std::make_unique<oir::GetElementPtrInst>(
            inst.type(), map_value(gep.base_ptr(), values, blocks),
            map_values(gep.indices(), values, blocks), parent, name);
    }
    case oir::Instruction::OpID::Call: {
        auto &call = static_cast<oir::CallInst &>(inst);
        return std::make_unique<oir::CallInst>(
            inst.type(), map_value(call.callee(), values, blocks),
            map_values(call.args(), values, blocks), parent, name);
    }
    case oir::Instruction::OpID::Phi:
    case oir::Instruction::OpID::Br:
    case oir::Instruction::OpID::Ret:
        break;
    }
    throw std::runtime_error("unsupported instruction while cloning inline body");
}

std::unique_ptr<oir::Function> clone_function_template(oir::Function &source) {
    auto out = std::make_unique<oir::Function>(source.function_type(), source.name(),
                                               source.parent(), source.is_external());
    ValueMap values;
    BlockMap blocks;

    for (const auto &arg : source.args()) {
        values[arg.get()] = out->add_argument(arg->type(), arg->name());
    }
    for (const auto &block : source.blocks()) {
        blocks[block.get()] = out->create_block(block->name());
    }

    auto &module = *source.parent();
    for (const auto &block : source.blocks()) {
        auto *out_block = blocks.at(block.get());
        for (const auto &inst : block->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }
            auto out_phi = std::make_unique<oir::PhiInst>(phi->type(), out_block, phi->name());
            values[phi] = out_phi.get();
            out_block->append_instruction(std::move(out_phi));
        }
    }

    for (const auto &block : source.blocks()) {
        auto *out_block = blocks.at(block.get());
        for (const auto &inst_ptr : block->instructions()) {
            auto *inst = inst_ptr.get();
            if (inst->op() == oir::Instruction::OpID::Phi) {
                continue;
            }
            if (auto *ret = dynamic_cast<oir::ReturnInst *>(inst)) {
                out_block->append_instruction(std::make_unique<oir::ReturnInst>(
                    module.types().void_ty(),
                    ret->has_value() ? map_value(ret->value(), values, blocks) : nullptr,
                    out_block));
                continue;
            }
            if (auto *br = dynamic_cast<oir::BranchInst *>(inst)) {
                if (br->is_conditional()) {
                    auto *true_bb = static_cast<oir::BasicBlock *>(
                        map_value(br->true_bb(), values, blocks));
                    auto *false_bb = static_cast<oir::BasicBlock *>(
                        map_value(br->false_bb(), values, blocks));
                    out_block->append_instruction(std::make_unique<oir::BranchInst>(
                        module.types().void_ty(), map_value(br->cond(), values, blocks), true_bb,
                        false_bb, out_block));
                    out_block->add_successor(true_bb);
                    out_block->add_successor(false_bb);
                    true_bb->add_predecessor(out_block);
                    false_bb->add_predecessor(out_block);
                } else {
                    auto *target = static_cast<oir::BasicBlock *>(
                        map_value(br->target_bb(), values, blocks));
                    out_block->append_instruction(std::make_unique<oir::BranchInst>(
                        module.types().void_ty(), target, out_block));
                    out_block->add_successor(target);
                    target->add_predecessor(out_block);
                }
                continue;
            }

            auto cloned =
                clone_non_phi_instruction(module, source, *inst, out_block, values, blocks, 0);
            values[inst] = cloned.get();
            out_block->append_instruction(std::move(cloned));
        }
    }

    for (const auto &block : source.blocks()) {
        auto *out_block = blocks.at(block.get());
        auto out_it = out_block->instructions().begin();
        for (const auto &inst : block->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }
            auto *out_phi = static_cast<oir::PhiInst *>(out_it->get());
            for (const auto &[value, from] : phi->incoming()) {
                out_phi->add_incoming(map_value(value, values, blocks), blocks.at(from));
            }
            ++out_it;
        }
    }

    return out;
}

bool is_constprop_specialization(const oir::Function &function) {
    return function.name().rfind("__yo_constprop.", 0) == 0;
}

unsigned existing_specialization_count(const oir::Module &module, const oir::Function &callee) {
    const std::string prefix = "__yo_constprop." + callee.name() + ".";
    unsigned count = 0;
    for (const auto &function : module.functions()) {
        if (function->name().rfind(prefix, 0) == 0) {
            ++count;
        }
    }
    return count;
}

bool is_specializable_constant(oir::Value *value) {
    return int_constant(value).has_value() || float_constant(value).has_value();
}

std::string constant_key(oir::Value *value) {
    if (auto constant = int_constant(value)) {
        return "i" + std::to_string(*constant);
    }
    if (auto constant = float_constant(value)) {
        std::ostringstream oss;
        oss << "f" << *constant;
        return oss.str();
    }
    return "*";
}

std::string specialization_key(const oir::Function &callee, const oir::CallInst &call,
                               const SpecializationMask &mask) {
    std::ostringstream oss;
    oss << static_cast<const void *>(&callee);
    auto args = call.args();
    for (std::size_t i = 0; i < args.size(); ++i) {
        oss << ";";
        if (mask_selects_argument(mask, i)) {
            oss << i << "=" << constant_key(args[i]);
        } else {
            oss << i << "=*";
        }
    }
    return oss.str();
}

std::string next_specialization_name(oir::Module &module, const oir::Function &callee,
                                     unsigned &next_id) {
    while (true) {
        std::string name = "__yo_constprop." + callee.name() + "." + std::to_string(next_id++);
        if (module.get_function(name) == nullptr) {
            return name;
        }
    }
}

bool has_constant_argument(const oir::CallInst &call) {
    for (auto *arg : call.args()) {
        if (is_specializable_constant(arg)) {
            return true;
        }
    }
    return false;
}

unsigned count_specializable_constants(const SpecializationMask &mask) {
    unsigned count = 0;
    for (bool selected : mask) {
        if (selected) {
            ++count;
        }
    }
    return count;
}

bool mask_selects_argument(const SpecializationMask &mask, std::size_t index) {
    return index < mask.size() && mask[index];
}

bool has_live_pointer_argument_after_specialization(const oir::CallInst &call,
                                                    const oir::Function &callee,
                                                    const SpecializationMask &mask) {
    auto args = call.args();
    if (args.size() != callee.args().size()) {
        return true;
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (mask_selects_argument(mask, i)) {
            continue;
        }
        if (callee.args()[i]->type()->is_pointer()) {
            return true;
        }
    }
    return false;
}

bool argument_feeds_phi(const oir::Function &callee, std::size_t index) {
    if (index >= callee.args().size()) {
        return true;
    }
    for (auto *user : callee.args()[index]->users()) {
        if (dynamic_cast<oir::PhiInst *>(user) != nullptr) {
            return true;
        }
    }
    return false;
}

bool specialized_constant_argument_feeds_phi(const oir::CallInst &call,
                                             const oir::Function &callee) {
    auto args = call.args();
    if (args.size() != callee.args().size()) {
        return true;
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!is_specializable_constant(args[i])) {
            continue;
        }
        if (argument_feeds_phi(callee, i)) {
            return true;
        }
    }
    return false;
}

bool argument_has_structural_use(const oir::Function &callee, std::size_t index) {
    if (index >= callee.args().size()) {
        return false;
    }
    for (auto *user : callee.args()[index]->users()) {
        auto *inst = dynamic_cast<oir::Instruction *>(user);
        if (inst == nullptr) {
            continue;
        }
        switch (inst->op()) {
        case oir::Instruction::OpID::Add:
        case oir::Instruction::OpID::Sub:
        case oir::Instruction::OpID::Mul:
        case oir::Instruction::OpID::SDiv:
        case oir::Instruction::OpID::SRem:
        case oir::Instruction::OpID::And:
        case oir::Instruction::OpID::Xor:
        case oir::Instruction::OpID::ICmp:
        case oir::Instruction::OpID::FCmp:
        case oir::Instruction::OpID::Br:
        case oir::Instruction::OpID::Call:
            return true;
        case oir::Instruction::OpID::Ret:
        case oir::Instruction::OpID::Alloca:
        case oir::Instruction::OpID::Load:
        case oir::Instruction::OpID::Store:
        case oir::Instruction::OpID::MemZero:
        case oir::Instruction::OpID::Phi:
        case oir::Instruction::OpID::GetElementPtr:
        case oir::Instruction::OpID::ZExt:
        case oir::Instruction::OpID::SIToFP:
        case oir::Instruction::OpID::FPToSI:
        case oir::Instruction::OpID::FAdd:
        case oir::Instruction::OpID::FSub:
        case oir::Instruction::OpID::FMul:
        case oir::Instruction::OpID::FDiv:
            break;
        }
    }
    return false;
}

bool mask_has_selection(const SpecializationMask &mask) {
    return std::any_of(mask.begin(), mask.end(), [](bool selected) { return selected; });
}

SpecializationMask build_specialization_mask(const oir::CallInst &call,
                                             const oir::Function &callee,
                                             bool recursive_layer) {
    auto args = call.args();
    SpecializationMask mask(args.size(), false);
    if (args.size() != callee.args().size()) {
        return mask;
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!is_specializable_constant(args[i])) {
            continue;
        }
        if (recursive_layer &&
            (argument_feeds_phi(callee, i) || !argument_has_structural_use(callee, i))) {
            continue;
        }
        mask[i] = true;
    }
    return mask;
}

bool is_directly_recursive(const oir::Function &function) {
    return contains_call_to(function, function);
}

unsigned recursive_specialization_budget(
    const pass::cost_model::CostModelPolicy &policy) {
    return std::min<unsigned>(static_cast<unsigned>(policy.max_specializations_per_function),
                              kMaxRecursiveConstantSpecializationLayers);
}

bool within_recursive_specialization_growth_budget(
    const CalleeInfo &info, unsigned existing_for_callee,
    const pass::cost_model::CostModelPolicy &policy) {
    const auto projected_layers = existing_for_callee + 1;
    if (projected_layers > recursive_specialization_budget(policy)) {
        return false;
    }
    if (info.blocks > kMaxRecursiveSpecializationCalleeBlocks || info.returns == 0) {
        return false;
    }
    const auto projected_static_instrs = info.static_instrs * projected_layers;
    const auto policy_static_cap =
        static_cast<unsigned>(std::max<std::int64_t>(policy.max_function_code_growth, 1) * 8);
    return projected_static_instrs <=
           std::min<unsigned>(kMaxRecursiveSpecializationStaticInstrs, policy_static_cap);
}

bool is_eligible_for_constant_specialization(const oir::Function &caller, const oir::CallInst &call,
                                             oir::Function *callee,
                                             const pass::cost_model::CostModelPolicy &policy,
                                             unsigned existing_for_callee,
                                             SpecializationMask &mask) {
    mask.clear();
    if (callee == nullptr || callee == &caller || callee->is_external() ||
        callee->entry_block() == nullptr || is_constprop_specialization(*callee) ||
        !has_compatible_call_shape(call, *callee) || !has_constant_argument(call)) {
        return false;
    }

    auto info = inspect_callee(*callee);
    if (has_recursive_call_graph_dependency(*callee)) {
        mask = build_specialization_mask(call, *callee, true);
        return is_directly_recursive(*callee) &&
               mask_has_selection(mask) &&
               within_recursive_specialization_growth_budget(info, existing_for_callee, policy);
    }

    if (specialized_constant_argument_feeds_phi(call, *callee)) {
        return false;
    }
    mask = build_specialization_mask(call, *callee, false);
    if (!mask_has_selection(mask)) {
        return false;
    }

    if ((info.blocks > kMaxCalleeBlocks ||
         info.cost > static_cast<unsigned>(policy.max_inline_callee_cost)) &&
        has_live_pointer_argument_after_specialization(call, *callee, mask)) {
        return false;
    }
    return info.blocks <= kMaxSpecializationCalleeBlocks &&
           info.cost <= static_cast<unsigned>(policy.max_inline_callee_cost * 3) &&
           info.returns != 0;
}

oir::Function *clone_constant_specialization(oir::Module &module, oir::Function &callee,
                                             const oir::CallInst &call,
                                             const SpecializationMask &mask, unsigned &next_id) {
    auto args = call.args();
    std::vector<oir::Type *> param_types;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!mask_selects_argument(mask, i)) {
            param_types.push_back(callee.args()[i]->type());
        }
    }

    auto *clone = module.create_function(next_specialization_name(module, callee, next_id),
                                         module.types().func_ty(callee.return_type(), param_types),
                                         false);

    ValueMap values;
    BlockMap blocks;
    std::size_t next_arg = 0;
    for (std::size_t i = 0; i < callee.args().size(); ++i) {
        if (mask_selects_argument(mask, i)) {
            values[callee.args()[i].get()] = args[i];
            continue;
        }
        auto *arg = clone->args()[next_arg++].get();
        if (!callee.args()[i]->name().empty()) {
            arg->set_name(callee.args()[i]->name());
        }
        values[callee.args()[i].get()] = arg;
    }

    for (const auto &block : callee.blocks()) {
        blocks[block.get()] = clone->create_block("constprop." + block->name());
    }

    for (const auto &block : callee.blocks()) {
        auto *out_block = blocks.at(block.get());
        for (const auto &inst : block->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }
            auto out_phi =
                std::make_unique<oir::PhiInst>(phi->type(), out_block, phi->name());
            values[phi] = out_phi.get();
            out_block->append_instruction(std::move(out_phi));
        }
    }

    for (const auto &block : callee.blocks()) {
        auto *out_block = blocks.at(block.get());
        for (const auto &inst_ptr : block->instructions()) {
            auto *inst = inst_ptr.get();
            if (inst->op() == oir::Instruction::OpID::Phi) {
                continue;
            }
            if (auto *ret = dynamic_cast<oir::ReturnInst *>(inst)) {
                out_block->append_instruction(std::make_unique<oir::ReturnInst>(
                    module.types().void_ty(),
                    ret->has_value() ? map_value(ret->value(), values, blocks) : nullptr,
                    out_block));
                continue;
            }
            if (auto *br = dynamic_cast<oir::BranchInst *>(inst)) {
                if (br->is_conditional()) {
                    auto *true_bb = static_cast<oir::BasicBlock *>(
                        map_value(br->true_bb(), values, blocks));
                    auto *false_bb = static_cast<oir::BasicBlock *>(
                        map_value(br->false_bb(), values, blocks));
                    out_block->append_instruction(std::make_unique<oir::BranchInst>(
                        module.types().void_ty(), map_value(br->cond(), values, blocks), true_bb,
                        false_bb, out_block));
                    out_block->add_successor(true_bb);
                    out_block->add_successor(false_bb);
                    true_bb->add_predecessor(out_block);
                    false_bb->add_predecessor(out_block);
                } else {
                    auto *target = static_cast<oir::BasicBlock *>(
                        map_value(br->target_bb(), values, blocks));
                    out_block->append_instruction(std::make_unique<oir::BranchInst>(
                        module.types().void_ty(), target, out_block));
                    out_block->add_successor(target);
                    target->add_predecessor(out_block);
                }
                continue;
            }

            auto cloned =
                clone_non_phi_instruction(module, callee, *inst, out_block, values, blocks, 0);
            values[inst] = cloned.get();
            out_block->append_instruction(std::move(cloned));
        }
    }

    for (const auto &block : callee.blocks()) {
        auto *out_block = blocks.at(block.get());
        auto out_it = out_block->instructions().begin();
        for (const auto &inst : block->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }
            auto *out_phi = static_cast<oir::PhiInst *>(out_it->get());
            for (const auto &[value, from] : phi->incoming()) {
                out_phi->add_incoming(map_value(value, values, blocks), blocks.at(from));
            }
            ++out_it;
        }
    }

    return clone;
}

void retarget_call_to_specialization(oir::CallInst &call, oir::Function &clone,
                                     const SpecializationMask &mask) {
    auto args = call.args();
    call.set_operand(0, &clone);
    for (std::size_t i = args.size(); i > 0; --i) {
        if (mask_selects_argument(mask, i - 1)) {
            call.remove_arg(i - 1);
        }
    }
}

oir::Function &recursive_template_for(InlineContext &context, oir::Function &function) {
    auto found = context.recursive_templates.find(&function);
    if (found != context.recursive_templates.end()) {
        return *found->second;
    }
    auto inserted = context.recursive_templates.emplace(&function, clone_function_template(function));
    return *inserted.first->second;
}

void split_call_block(oir::BasicBlock *block,
                      std::list<std::unique_ptr<oir::Instruction>>::iterator call_it,
                      oir::BasicBlock *continuation) {
    auto original_successors = block->successors();
    for (auto *succ : original_successors) {
        oir::cfg::move_successor_edge(block, continuation, succ);
    }

    auto &from = block->instructions();
    auto tail_begin = std::next(call_it);
    auto &to = continuation->instructions();
    to.splice(to.end(), from, tail_begin, from.end());
    for (auto &inst : to) {
        inst->set_parent(continuation);
    }
}

oir::Value *materialize_return_value(oir::Module &module, oir::CallInst &call,
                                     oir::BasicBlock *continuation,
                                     const std::vector<std::pair<oir::BasicBlock *, oir::Value *>>
                                         &returns) {
    if (call.type()->is_void()) {
        return nullptr;
    }
    if (returns.empty()) {
        throw std::runtime_error("cannot inline non-void call whose callee has no return value");
    }
    if (returns.size() == 1) {
        return returns.front().second;
    }

    auto phi =
        std::make_unique<oir::PhiInst>(call.type(), continuation,
                                       call.name().empty() ? "inl.ret" : call.name());
    auto *raw = phi.get();
    for (const auto &[block, value] : returns) {
        phi->add_incoming(value, block);
    }
    continuation->instructions().push_front(std::move(phi));
    raw->set_parent(continuation);
    (void)module;
    return raw;
}

void clone_callee_into_caller(oir::Module &module, oir::Function &caller, oir::Function &callee,
                              oir::CallInst &call, oir::BasicBlock *continuation,
                              ValueMap &values, BlockMap &blocks,
                              std::vector<std::pair<oir::BasicBlock *, oir::Value *>> &returns,
                              unsigned inline_index) {
    auto args = call.args();
    for (std::size_t i = 0; i < callee.args().size(); ++i) {
        values[callee.args()[i].get()] = args[i];
    }

    auto ordered_blocks = clone_order(callee);
    for (auto *block : ordered_blocks) {
        auto *out_block = blocks.at(block);
        for (const auto &inst : block->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }
            auto out_phi = std::make_unique<oir::PhiInst>(
                phi->type(), out_block, inline_name(callee, *phi, inline_index));
            values[phi] = out_phi.get();
            out_block->append_instruction(std::move(out_phi));
        }
    }

    for (auto *block : ordered_blocks) {
        auto *out_block = blocks.at(block);
        for (const auto &inst_ptr : block->instructions()) {
            auto *inst = inst_ptr.get();
            if (inst->op() == oir::Instruction::OpID::Phi) {
                continue;
            }
            if (auto *ret = dynamic_cast<oir::ReturnInst *>(inst)) {
                returns.push_back(
                    {out_block, ret->has_value() ? map_value(ret->value(), values, blocks)
                                                  : nullptr});
                oir::cfg::append_unconditional_branch(module, out_block, continuation);
                continue;
            }
            if (auto *br = dynamic_cast<oir::BranchInst *>(inst)) {
                if (br->is_conditional()) {
                    oir::cfg::append_conditional_branch(
                        module, out_block, map_value(br->cond(), values, blocks),
                        static_cast<oir::BasicBlock *>(map_value(br->true_bb(), values, blocks)),
                        static_cast<oir::BasicBlock *>(map_value(br->false_bb(), values, blocks)));
                } else {
                    oir::cfg::append_unconditional_branch(
                        module, out_block,
                        static_cast<oir::BasicBlock *>(map_value(br->target_bb(), values, blocks)));
                }
                continue;
            }

            auto cloned = clone_non_phi_instruction(module, callee, *inst, out_block, values,
                                                    blocks, inline_index);
            auto *raw = cloned.get();
            out_block->append_instruction(std::move(cloned));
            values[inst] = raw;
        }
    }

    for (auto *block : ordered_blocks) {
        auto *out_block = blocks.at(block);
        auto out_it = out_block->instructions().begin();
        for (const auto &inst : block->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }
            auto *out_phi = static_cast<oir::PhiInst *>(out_it->get());
            for (const auto &[value, from] : phi->incoming()) {
                out_phi->add_incoming(map_value(value, values, blocks), blocks.at(from));
            }
            ++out_it;
        }
    }
}

bool inline_call(oir::Module &module, InlineContext &context, oir::Function &caller,
                 oir::BasicBlock *block,
                 std::list<std::unique_ptr<oir::Instruction>>::iterator call_it,
                 unsigned inline_index, Stats &stats) {
    auto *call = static_cast<oir::CallInst *>(call_it->get());
    auto *callee = dynamic_cast<oir::Function *>(call->callee());
    bool self_recursive = callee == &caller;
    oir::Function *clone_source = callee;
    unsigned self_recursive_depth = 0;
    const auto policy = pass::cost_model::policy_for_kind(stats.cost_model_policy);
    if (self_recursive) {
        if (stats.recursively_inlined_functions.find(&caller) !=
            stats.recursively_inlined_functions.end()) {
            return false;
        }
        clone_source = &recursive_template_for(context, caller);
        self_recursive_depth = recursive_inline_depth(context, *call);
        if (!is_eligible_recursive_call(caller, *call, *clone_source, policy,
                                        self_recursive_depth,
                                        context.recursive_growth[&caller])) {
            return false;
        }
    } else if (!is_eligible_non_recursive_call(caller, *call, callee, policy)) {
        return false;
    }

    const auto info = inspect_callee(*clone_source);
    const auto callsite =
        inspect_callsite(module, caller, *block, *call, *clone_source, info);
    OIRTransformCostEstimate estimate;
    estimate.kind = pass::cost_model::TransformKind::Inline;
    estimate.pass_name = "OIRInlinePass";
    estimate.candidate_id =
        "inline." + std::to_string(++stats.cost_model_candidates) + ".l" +
        std::to_string(callsite.loop_depth) + ".c" +
        std::to_string(callsite.constant_argument_uses) + ".g" +
        std::to_string(callsite.effective_growth);
    estimate.scope = self_recursive
                         ? "direct-recursive-depth-" +
                               std::to_string(self_recursive_depth)
                         : "call";
    estimate.proof_kind = pass::cost_model::ProofKind::Structural;
    estimate.proof_summary =
        "direct call, compatible signature and structurally cloneable CFG";
    estimate.proof_rule_id = "oir.inline.direct-call.structural";
    estimate.confidence = self_recursive
                              ? 0.60
                              : (callsite.constant_arguments != 0 || callsite.loop_depth != 0
                                     ? 0.78
                                     : (callsite.callee_call_sites == 1 ? 0.72 : 0.65));
    if (!self_recursive && callsite.loop_depth != 0) {
        estimate.frequency_scale = callsite.execution_scale;
        estimate.loop_depth = static_cast<int>(callsite.loop_depth);
        estimate.frequency_source = pass::cost_model::FrequencySource::OIRLoopAnalysis;
    }
    fill_before_after_from_callee(
        estimate, info,
        self_recursive ? 1 : static_cast<std::int64_t>(info.calls + callsite.execution_scale),
        self_recursive ? 0 : static_cast<std::int64_t>(info.calls));
    if (!self_recursive) {
        const auto module_instrs = static_cast<std::int64_t>(module_instruction_count(module));
        estimate.before_instrs = module_instrs;
        estimate.after_instrs = module_instrs;
        estimate.before_code_bytes = module_instrs * 4;
        estimate.after_code_bytes = estimate.before_code_bytes;
        const auto eliminated_instrs = static_cast<std::int64_t>(
            callsite.predicted_eliminated_instrs);
        const auto eliminated_branches = static_cast<std::int64_t>(
            callsite.predicted_eliminated_branches);
        estimate.after_int_alu =
            std::max<std::int64_t>(0, estimate.after_int_alu - eliminated_instrs);
        estimate.after_branches =
            std::max<std::int64_t>(0, estimate.after_branches - eliminated_branches);
    }
    if (info.returns <= 1 && estimate.after_branches > 0) {
        --estimate.after_branches;
    }
    estimate.after_live_values += static_cast<std::int64_t>(call->args().size());
    estimate.after_max_live_values +=
        static_cast<std::int64_t>(std::min<std::size_t>(call->args().size(), 4));
    if (self_recursive) {
        estimate.after_max_live_values +=
            recursive_pressure_growth(*call, info, self_recursive_depth);
    }
    const auto projected_module_growth =
        stats.inline_code_growth + static_cast<std::int64_t>(callsite.effective_growth);
    estimate.risk.code_growth =
        self_recursive ? std::max<std::int64_t>(1, info.static_instrs)
                       : static_cast<std::int64_t>(callsite.effective_growth);
    if (!self_recursive && projected_module_growth > policy.max_function_code_growth) {
        // Keep cumulative growth as a separate hard budget. Folding it into each site's risk
        // would make a previously accepted call poison otherwise profitable tiny helpers.
        estimate.risk.code_growth = policy.max_function_code_growth + 1;
    }
    estimate.risk.register_pressure_growth =
        static_cast<std::int64_t>((info.loads + info.stores + call->args().size()) / 6);
    estimate.risk.live_range_growth =
        static_cast<std::int64_t>(std::min<std::size_t>(call->args().size(), 4)) +
        (info.returns > 1 ? 1 : 0);
    estimate.risk.memory_pressure_growth =
        static_cast<std::int64_t>((info.loads + info.stores) / 6);
    estimate.risk.cleanup_dependency = 1;
    if (!cost_model_allows_transform(stats, estimate)) {
        return false;
    }

    if (!self_recursive) {
        stats.inline_code_growth += static_cast<std::int64_t>(callsite.effective_growth);
    }

    ValueMap values;
    BlockMap blocks;
    std::vector<std::pair<oir::BasicBlock *, oir::Value *>> returns;

    const std::string block_prefix =
        self_recursive
            ? recursive_inline_prefix(caller, self_recursive_depth + 1)
            : "inl." + callee->name() + ".";
    for (const auto &callee_block : clone_source->blocks()) {
        blocks[callee_block.get()] = caller.create_block(block_prefix + callee_block->name());
    }
    auto *continuation =
        caller.create_block(block_prefix + "cont." + std::to_string(inline_index));

    split_call_block(block, call_it, continuation);
    clone_callee_into_caller(module, caller, *clone_source, *call, continuation, values, blocks,
                             returns, inline_index);
    if (self_recursive) {
        for (const auto &[source_block, cloned_block] : blocks) {
            (void)source_block;
            for (const auto &inst : cloned_block->instructions()) {
                auto *cloned_call = dynamic_cast<oir::CallInst *>(inst.get());
                if (cloned_call != nullptr && cloned_call->callee() == &caller) {
                    context.recursive_call_depths[cloned_call] = self_recursive_depth + 1;
                }
            }
        }
        context.recursive_growth[&caller] += cloned_instruction_growth(info);
        context.recursive_call_depths.erase(call);
    }
    oir::cfg::append_unconditional_branch(module, block, blocks.at(clone_source->entry_block()));

    if (!call->type()->is_void()) {
        ReplacementMap replacements;
        replacements[call] = materialize_return_value(module, *call, continuation, returns);
        apply_replacements(module, replacements);
    }
    (*call_it)->drop_all_operands();
    block->instructions().erase(call_it);
    return true;
}

bool inline_one_call(oir::Module &module, InlineContext &context, oir::Function &function,
                     unsigned inline_index, Stats &stats) {
    struct OrdinarySite {
        oir::BasicBlock *block = nullptr;
        oir::CallInst *call = nullptr;
        CallsiteEstimate estimate;
        unsigned ordinal = 0;
    };
    struct RecursiveSite {
        unsigned depth = 0;
        oir::BasicBlock *block = nullptr;
        oir::CallInst *call = nullptr;
    };

    // Rank ordinary sites from bounded read-only facts before mutating IR. Constant-rich and hot
    // sites expose the most cleanup; smaller growth breaks ties. Recursive templates must still
    // be captured only after helper calls have had a chance to disappear.
    std::vector<OrdinarySite> ordinary_sites;
    unsigned ordinal = 0;
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            auto *call = dynamic_cast<oir::CallInst *>(inst.get());
            if (call == nullptr || call->callee() == &function) {
                continue;
            }
            auto *callee = dynamic_cast<oir::Function *>(call->callee());
            if (callee == nullptr || callee->is_external() || callee->entry_block() == nullptr) {
                continue;
            }
            const auto info = inspect_callee(*callee);
            ordinary_sites.push_back(
                {block.get(), call,
                 inspect_callsite(module, function, *block, *call, *callee, info), ordinal++});
        }
    }
    std::stable_sort(ordinary_sites.begin(), ordinary_sites.end(),
                     [](const OrdinarySite &lhs, const OrdinarySite &rhs) {
                         if (lhs.estimate.loop_depth != rhs.estimate.loop_depth) {
                             return lhs.estimate.loop_depth > rhs.estimate.loop_depth;
                         }
                         if ((lhs.estimate.callee_call_sites == 1) !=
                             (rhs.estimate.callee_call_sites == 1)) {
                             return lhs.estimate.callee_call_sites == 1;
                         }
                         if ((lhs.estimate.constant_arguments != 0) !=
                             (rhs.estimate.constant_arguments != 0)) {
                             return lhs.estimate.constant_arguments != 0;
                         }
                         if (lhs.estimate.constant_argument_uses !=
                             rhs.estimate.constant_argument_uses) {
                             return lhs.estimate.constant_argument_uses >
                                    rhs.estimate.constant_argument_uses;
                         }
                         if (lhs.estimate.effective_growth != rhs.estimate.effective_growth) {
                             return lhs.estimate.effective_growth < rhs.estimate.effective_growth;
                         }
                         return lhs.ordinal < rhs.ordinal;
                     });
    for (const auto &site : ordinary_sites) {
        auto &instructions = site.block->instructions();
        for (auto it = instructions.begin(); it != instructions.end(); ++it) {
            if (it->get() != site.call) {
                continue;
            }
            if (inline_call(module, context, function, site.block, it, inline_index, stats)) {
                // Let cleanup and tail-recursion elimination see the helper-free body
                // before deciding whether recursive expansion is still profitable.
                context.defer_recursive_expansion.insert(&function);
                return true;
            }
            break;
        }
    }

    if (context.defer_recursive_expansion.find(&function) !=
        context.defer_recursive_expansion.end()) {
        return false;
    }

    std::vector<RecursiveSite> recursive_sites;
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            auto *call = dynamic_cast<oir::CallInst *>(inst.get());
            if (call == nullptr || call->callee() != &function) {
                continue;
            }
            recursive_sites.push_back(
                {recursive_inline_depth(context, *call), block.get(), call});
        }
    }
    std::stable_sort(recursive_sites.begin(), recursive_sites.end(),
                     [](const RecursiveSite &lhs, const RecursiveSite &rhs) {
                         return lhs.depth < rhs.depth;
                     });
    for (const auto &site : recursive_sites) {
        auto &instructions = site.block->instructions();
        for (auto it = instructions.begin(); it != instructions.end(); ++it) {
            if (it->get() != site.call) {
                continue;
            }
            if (inline_call(module, context, function, site.block, it, inline_index, stats)) {
                return true;
            }
            break;
        }
    }

    return false;
}

} // namespace

bool specialize_constant_argument_calls(oir::Module &module, Stats &stats) {
    struct Site {
        oir::Function *caller = nullptr;
        oir::Function *callee = nullptr;
        oir::CallInst *call = nullptr;
        std::string key;
        SpecializationMask mask;
    };

    std::vector<Site> sites;
    const auto policy = pass::cost_model::policy_for_kind(stats.cost_model_policy);
    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        for (auto &block : function->blocks()) {
            for (auto &inst : block->instructions()) {
                auto *call = dynamic_cast<oir::CallInst *>(inst.get());
                if (call == nullptr) {
                    continue;
                }
                auto *callee = dynamic_cast<oir::Function *>(call->callee());
                const unsigned existing_for_callee =
                    callee == nullptr
                        ? 0
                        : existing_specialization_count(module, *callee);
                SpecializationMask mask;
                if (!is_eligible_for_constant_specialization(*function, *call, callee, policy,
                                                             existing_for_callee, mask)) {
                    continue;
                }
                sites.push_back({function.get(), callee, call,
                                 specialization_key(*callee, *call, mask), std::move(mask)});
                if (sites.size() >= kMaxSpecializedCallSites) {
                    break;
                }
            }
            if (sites.size() >= kMaxSpecializedCallSites) {
                break;
            }
        }
        if (sites.size() >= kMaxSpecializedCallSites) {
            break;
        }
    }

    if (sites.empty()) {
        return false;
    }

    bool changed = false;
    unsigned next_id = 0;
    std::unordered_map<std::string, oir::Function *> clones;
    std::unordered_map<oir::Function *, unsigned> specializations_by_callee;
    for (auto &site : sites) {
        if (site.call == nullptr || site.callee == nullptr ||
            dynamic_cast<oir::Function *>(site.call->callee()) != site.callee) {
            continue;
        }

        auto found = clones.find(site.key);
        oir::Function *clone = nullptr;
        const bool needs_new_clone = found == clones.end();
        const unsigned existing_for_callee =
            existing_specialization_count(module, *site.callee) +
            specializations_by_callee[site.callee];
        const bool recursive_layer = is_directly_recursive(*site.callee);
        const unsigned specialization_budget =
            recursive_layer ? recursive_specialization_budget(policy)
                            : static_cast<unsigned>(policy.max_specializations_per_function);
        const bool exceeds_policy_specialization_budget =
            stats.cost_model_report != nullptr && needs_new_clone &&
            existing_for_callee >= specialization_budget;
        const auto constant_arg_count = count_specializable_constants(site.mask);
        const auto info = inspect_callee(*site.callee);
        OIRTransformCostEstimate estimate;
        estimate.kind = pass::cost_model::TransformKind::ConstantArgumentSpecialization;
        estimate.pass_name = "OIRInlinePass";
        estimate.candidate_id = "specialize." + std::to_string(++stats.cost_model_candidates);
        estimate.scope = "call";
        estimate.proof_kind = pass::cost_model::ProofKind::PartialEvaluation;
        estimate.proof_summary =
            recursive_layer
                ? "constant arguments proven at callsite; bounded direct-recursive layer"
                : "constant arguments proven at callsite";
        estimate.confidence = constant_arg_count == 0 ? 0.55 : (recursive_layer ? 0.70 : 0.74);
        fill_before_after_from_callee(estimate, info, 1, 0);
        const std::int64_t eliminated_alu =
            std::min<std::int64_t>(estimate.after_int_alu, constant_arg_count * 2);
        const std::int64_t eliminated_branches =
            std::min<std::int64_t>(estimate.after_branches, constant_arg_count);
        estimate.after_instrs =
            std::max<std::int64_t>(1, estimate.after_instrs - eliminated_alu - eliminated_branches);
        estimate.after_int_alu -= eliminated_alu;
        estimate.after_branches -= eliminated_branches;
        estimate.after_code_bytes += needs_new_clone
                                         ? static_cast<std::int64_t>(estimate.after_instrs * 4)
                                         : 0;
        estimate.after_live_values =
            std::max<std::int64_t>(1, estimate.after_live_values - constant_arg_count);
        estimate.after_max_live_values =
            std::max<std::int64_t>(1, estimate.after_max_live_values - constant_arg_count);
        estimate.risk.code_growth =
            needs_new_clone
                ? std::max<std::int64_t>(0,
                                         (estimate.after_code_bytes - estimate.before_code_bytes) / 16)
                : 0;
        estimate.risk.register_pressure_growth =
            needs_new_clone ? static_cast<std::int64_t>((info.loads + info.stores) / 4) : 0;
        estimate.risk.live_range_growth = 0;
        estimate.risk.memory_pressure_growth =
            needs_new_clone ? static_cast<std::int64_t>((info.loads + info.stores) / 3) : 0;
        estimate.risk.cleanup_dependency = 1;
        estimate.partial_eval.cloned_functions = needs_new_clone ? 1 : 0;
        estimate.partial_eval.cloned_blocks = needs_new_clone ? info.blocks : 0;
        estimate.partial_eval.residual_instrs = estimate.after_instrs;
        estimate.partial_eval.eliminated_instrs =
            eliminated_alu + eliminated_branches;
        estimate.partial_eval.eliminated_branches = eliminated_branches;
        estimate.partial_eval.eliminated_calls = 0;
        estimate.partial_eval.new_constants = constant_arg_count;
        estimate.partial_eval.required_cleanup_rounds = 1;
        if (exceeds_policy_specialization_budget) {
            estimate.proof_summary =
                "constant arguments proven at callsite; specialization budget exceeded";
            estimate.risk.code_growth = policy.max_function_code_growth + 1;
        }
        if (recursive_layer && !exceeds_policy_specialization_budget) {
            estimate.bypass_profitability = true;
            estimate.bypass_reason = "BoundedRecursiveConstantLayer";
        }
        if (!cost_model_allows_transform(stats, estimate)) {
            continue;
        }
        if (found != clones.end()) {
            clone = found->second;
        } else {
            if (clones.size() >= kMaxSpecializedFunctions) {
                break;
            }
            clone = clone_constant_specialization(module, *site.callee, *site.call, site.mask,
                                                  next_id);
            clones.emplace(site.key, clone);
            ++specializations_by_callee[site.callee];
        }
        retarget_call_to_specialization(*site.call, *clone, site.mask);
        ++stats.specialized;
        changed = true;
        (void)site.caller;
    }

    return changed;
}

bool inline_functions(oir::Module &module, Stats &stats) {
    bool changed = false;
    unsigned inline_index = 0;
    InlineContext context;
    const auto current_module_instrs =
        static_cast<std::int64_t>(module_instruction_count(module));
    if (stats.inline_baseline_instrs == 0) {
        stats.inline_baseline_instrs = current_module_instrs;
    }
    stats.inline_code_growth =
        std::max<std::int64_t>(0, current_module_instrs - stats.inline_baseline_instrs);

    // Start from callers at the end of the stable module order. This prevents early helper
    // expansion from making their callers artificially too large, while the bounded requeue
    // revisits only functions whose IR actually changed.
    std::deque<oir::Function *> worklist;
    std::unordered_map<const oir::Function *, unsigned> visits;
    for (auto it = module.functions().rbegin(); it != module.functions().rend(); ++it) {
        if (!(*it)->is_external()) {
            worklist.push_back(it->get());
        }
    }

    while (!worklist.empty() && inline_index < kMaxInlineSites) {
        auto *function = worklist.front();
        worklist.pop_front();
        auto &visit_count = visits[function];
        if (visit_count >= kMaxInlineSites) {
            continue;
        }
        ++visit_count;
        if (!inline_one_call(module, context, *function, inline_index + 1, stats)) {
            continue;
        }
        ++inline_index;
        ++stats.inlined;
        changed = true;
        worklist.push_front(function);
    }

    for (const auto &[function, growth] : context.recursive_growth) {
        if (growth != 0) {
            stats.recursively_inlined_functions.insert(function);
        }
    }

    return changed;
}

} // namespace pass::oir_opt

namespace pass {

std::string_view OIRInlinePass::name() const {
    return "OIRInlinePass";
}

PassKind OIRInlinePass::kind() const {
    return PassKind::Transform;
}

PassResult OIRInlinePass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRInlinePass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::inline_functions(module, stats);
            if (changed) {
                changed |= oir_opt::run_sccp(module, stats);
                changed |= oir_opt::global_value_numbering(module, stats);
            }
            changed |= oir_opt::simplify_branches(module, stats);
            changed |= oir_opt::cleanup_cfg(module, stats);
            changed |= oir_opt::eliminate_dead_code(module, stats);
            return changed;
        });
}

} // namespace pass
