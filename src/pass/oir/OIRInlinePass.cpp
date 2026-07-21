#include "pass/oir/OIRInlinePass.h"

#include "oir/OIRScalarOpt.h"

#include "oir/OIRAnalysis.h"
#include "oir/OIRCFGUtils.h"
#include "pass/oir/OIRCostModel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

constexpr unsigned kMaxInlineRounds = 3;
constexpr unsigned kMaxInlineSites = 128;
constexpr unsigned kMaxCalleeBlocks = 12;
constexpr unsigned kMaxCalleeReturns = 4;
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
constexpr unsigned kMaxAffectedCleanupRounds = 3;
constexpr unsigned kMaxScratchCleanupRounds = 8;
constexpr std::uint64_t kSpecializationAttemptBudget = 128;
constexpr std::uint64_t kRV64IntegerArgumentRegisters = 8;

bool residual_test_failure_is(const char *name) {
    const auto *configured =
        std::getenv("YOOLANG_TEST_OIR_RESIDUAL_FAILURE");
    return configured != nullptr && std::strcmp(configured, name) == 0;
}

bool consume_residual_test_failure(const char *name) {
    if (!residual_test_failure_is(name)) {
        return false;
    }
    // Each RUN line is a fresh compiler process.  Consume the selected generic
    // failpoint once so the same module can prove a subsequent control commit.
    static bool consumed = false;
    if (consumed) {
        return false;
    }
    consumed = true;
    return true;
}

void apply_canonical_container_permutation_test_once(oir::Module &module) {
    if (std::getenv("YOOLANG_TEST_OIR_CANONICAL_CONTAINER_PERMUTE") == nullptr) {
        return;
    }
    static bool applied = false;
    if (applied) {
        return;
    }
    applied = true;
    for (auto &function : module.functions()) {
        auto &blocks = function->blocks();
        auto *entry = function->entry_block();
        if (blocks.size() > 2) {
            blocks.reverse();
            auto entry_it = std::find_if(blocks.begin(), blocks.end(),
                [&](const auto &block) { return block.get() == entry; });
            blocks.splice(blocks.begin(), blocks, entry_it);
        }
        for (auto &block : blocks) {
            auto successors = block->successors();
            if (successors.size() < 2) {
                continue;
            }
            for (auto *successor : successors) {
                block->remove_successor(successor);
            }
            for (auto it = successors.rbegin(); it != successors.rend(); ++it) {
                block->add_successor(*it);
            }
        }
    }
}

using ValueMap = std::unordered_map<oir::Value *, oir::Value *>;
using BlockMap = std::unordered_map<oir::BasicBlock *, oir::BasicBlock *>;
using TypeMap = std::unordered_map<oir::Type *, oir::Type *>;
using SpecializationMask = std::vector<bool>;

enum class CallGrowthClass {
    Specialization,
    Ordinary,
    Recursive,
};

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
    std::unordered_map<const oir::CallInst *, unsigned> exposed_call_depths;
    std::unordered_set<oir::Function *> affected_functions;
    bool cleanup_budget_exhausted = false;
};

struct FixedPointSuffixSummary {
    std::uint64_t loops = 0;
    std::uint64_t before_iterations = 0;
    std::uint64_t after_iterations = 0;
    std::uint64_t eliminated_dynamic_instructions = 0;
};

std::uint64_t saturating_u64_add(std::uint64_t lhs, std::uint64_t rhs) {
    const auto limit = std::numeric_limits<std::uint64_t>::max();
    return rhs > limit - lhs ? limit : lhs + rhs;
}

bool reserve_growth_entries(
    std::unordered_map<oir::FunctionID, std::uint64_t> &growth,
    const std::vector<oir::FunctionID> &roots,
    std::vector<oir::FunctionID> &inserted) {
    inserted.clear();
    try {
        inserted.reserve(roots.size());
        for (const auto root : roots) {
            const auto [position, did_insert] = growth.emplace(root, 0);
            (void)position;
            if (did_insert) {
                inserted.push_back(root);
            }
        }
        return true;
    } catch (...) {
        for (const auto root : inserted) {
            growth.erase(root);
        }
        inserted.clear();
        return false;
    }
}

void erase_reserved_growth_entries(
    std::unordered_map<oir::FunctionID, std::uint64_t> &growth,
    const std::vector<oir::FunctionID> &inserted) {
    for (const auto root : inserted) {
        growth.erase(root);
    }
}

std::uint64_t saturating_u64_multiply(std::uint64_t lhs, std::uint64_t rhs) {
    const auto limit = std::numeric_limits<std::uint64_t>::max();
    return lhs != 0 && rhs > limit / lhs ? limit : lhs * rhs;
}

void add_call_pressure(CallPressureVector &total, const CallPressureVector &delta) {
#define ADD_PRESSURE_FIELD(name) \
    total.name = saturating_u64_add(total.name, delta.name)
    ADD_PRESSURE_FIELD(live_pointers);
    ADD_PRESSURE_FIELD(alias_uncertainty);
    ADD_PRESSURE_FIELD(loads);
    ADD_PRESSURE_FIELD(stores);
    ADD_PRESSURE_FIELD(max_live_values);
    ADD_PRESSURE_FIELD(memory_pressure);
    ADD_PRESSURE_FIELD(register_pressure);
    ADD_PRESSURE_FIELD(spill_proxy);
#undef ADD_PRESSURE_FIELD
}

bool pressure_field_fits(std::uint64_t committed, std::uint64_t added,
                         std::uint64_t budget) {
    return added <= budget - std::min(committed, budget);
}

pass::cost_model::RejectReason pressure_reject_reason(
    const CallPressureVector &committed, const CallPressureVector &added,
    const CallPressureVector &budget) {
    const bool memory_exhausted =
        !pressure_field_fits(committed.live_pointers, added.live_pointers,
                             budget.live_pointers) ||
        !pressure_field_fits(committed.alias_uncertainty, added.alias_uncertainty,
                             budget.alias_uncertainty) ||
        !pressure_field_fits(committed.loads, added.loads, budget.loads) ||
        !pressure_field_fits(committed.stores, added.stores, budget.stores) ||
        !pressure_field_fits(committed.memory_pressure, added.memory_pressure,
                             budget.memory_pressure);
    if (memory_exhausted) {
        return pass::cost_model::RejectReason::MemoryPressureTooHigh;
    }
    const bool register_exhausted =
        !pressure_field_fits(committed.max_live_values, added.max_live_values,
                             budget.max_live_values) ||
        !pressure_field_fits(committed.register_pressure, added.register_pressure,
                             budget.register_pressure) ||
        !pressure_field_fits(committed.spill_proxy, added.spill_proxy,
                             budget.spill_proxy);
    return register_exhausted ? pass::cost_model::RejectReason::RegisterPressureTooHigh
                              : pass::cost_model::RejectReason::None;
}

bool is_constprop_specialization(const oir::Function &function);
bool mask_selects_argument(const SpecializationMask &mask, std::size_t index);
unsigned count_specializable_constants(const SpecializationMask &mask);
bool is_specializable_constant(oir::Value *value);
std::string constant_key(oir::Value *value);

std::string typed_argument_bindings(
    const std::vector<oir::Value *> &arguments,
    const SpecializationMask *selected = nullptr) {
    std::ostringstream out;
    out << "arity=" << arguments.size();
    if (selected != nullptr && selected->size() != arguments.size()) {
        out << ":invalid-mask=" << selected->size();
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        auto *argument = arguments[index];
        const bool binding_selected =
            selected == nullptr ||
            (index < selected->size() && mask_selects_argument(*selected, index));
        out << ":arg" << index << '=';
        if (binding_selected && is_specializable_constant(argument)) {
            out << "const:" << constant_key(argument);
        } else {
            // A formal-position placeholder is a semantic IR fact.  It prevents
            // (const, unknown) and (unknown, const) from becoming one tie/reuse
            // key without using names, allocation order, or testcase identity.
            out << (binding_selected ? "unknown:" : "unselected:")
                << (argument == nullptr || argument->type() == nullptr
                        ? std::string("<null>")
                        : argument->type()->print());
        }
    }
    return out.str();
}

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

bool within_inline_resource_limit(const CalleeInfo &info,
                                  const pass::cost_model::CostModelPolicy &policy) {
    return info.cost <= static_cast<unsigned>(policy.max_inline_callee_cost) &&
           info.returns != 0 && info.returns <= kMaxCalleeReturns;
}

void fill_before_after_from_callee(OIRTransformCostEstimate &estimate, const CalleeInfo &info,
                                   std::int64_t before_calls, std::int64_t after_calls) {
    estimate.has_operation_breakdown = true;
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
    estimate.before_calls = static_cast<std::int64_t>(info.calls) + before_calls;
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
    estimate.after_calls = static_cast<std::int64_t>(info.calls) + after_calls;
    estimate.after_phis = estimate.before_phis;
    estimate.after_live_values = estimate.before_live_values;
    estimate.after_max_live_values = estimate.before_max_live_values;
}

void fill_after_from_residual(OIRTransformCostEstimate &estimate, const CalleeInfo &residual,
                              std::int64_t after_calls) {
    estimate.after_instrs = static_cast<std::int64_t>(residual.static_instrs) + after_calls;
    estimate.after_int_alu = residual.int_alu;
    estimate.after_int_mul = residual.int_mul;
    estimate.after_int_div_rem = residual.int_div_rem;
    estimate.after_fp_alu = residual.fp_alu;
    estimate.after_fp_div = residual.fp_div;
    estimate.after_loads = residual.loads;
    estimate.after_stores = residual.stores;
    estimate.after_pointer_arith = residual.pointer_arith;
    estimate.after_branches = residual.branches;
    estimate.after_calls = static_cast<std::int64_t>(residual.calls) + after_calls;
    estimate.after_phis = residual.phis;
    estimate.after_live_values = residual.static_instrs + residual.phis;
    estimate.after_max_live_values =
        static_cast<std::int64_t>(std::min<unsigned>(residual.static_instrs + residual.phis, 24));
}

CallPressureVector estimate_call_pressure(const CalleeInfo &body,
                                           const oir::CallInst &call,
                                           const SpecializationMask *mask = nullptr) {
    CallPressureVector out;
    const auto args = call.args();
    for (std::size_t index = 0; index < args.size(); ++index) {
        if ((mask == nullptr || !mask_selects_argument(*mask, index)) &&
            args[index]->type()->is_pointer()) {
            ++out.live_pointers;
        }
    }
    out.loads = body.loads;
    out.stores = body.stores;
    const auto memory_ops = saturating_u64_add(out.loads, out.stores);
    out.alias_uncertainty = out.live_pointers == 0
                                ? 0
                                : saturating_u64_multiply(
                                      out.live_pointers,
                                      std::max<std::uint64_t>(1, memory_ops));
    const auto body_live_values = std::min<std::uint64_t>(
        saturating_u64_add(body.static_instrs, body.phis), 24);
    // Account for the callsite's simultaneous argument materialization as a
    // structural pressure fact.  This is deliberately independent of any exact
    // arity cutoff: policy pressure limits decide whether a wide call is safe to
    // specialize or inline, including when all arguments are constants.
    out.max_live_values =
        std::max<std::uint64_t>(body_live_values, args.size());
    out.memory_pressure = saturating_u64_add(
        saturating_u64_add(out.loads, saturating_u64_multiply(out.stores, 2)),
        out.alias_uncertainty);
    out.register_pressure = saturating_u64_add(out.max_live_values,
                                               out.live_pointers);
    // RV64 exposes eight integer argument registers.  Call arguments beyond
    // that envelope and unusually broad body liveness are concrete stack/spill
    // proxies, not arity-based eligibility rules; policy makes the decision.
    constexpr std::uint64_t kBodyRegisterPressureEnvelope = 16;
    const auto argument_spill_proxy =
        args.size() > kRV64IntegerArgumentRegisters
            ? args.size() - kRV64IntegerArgumentRegisters
            : 0;
    const auto body_spill_proxy =
        body_live_values > kBodyRegisterPressureEnvelope
            ? body_live_values - kBodyRegisterPressureEnvelope
            : 0;
    out.spill_proxy = saturating_u64_add(
        std::max<std::uint64_t>(argument_spill_proxy, body_spill_proxy),
        out.live_pointers / 2);
    return out;
}

std::vector<oir::BasicBlock *>
canonical_reachable_order(const oir::Function &function);

bool contains_call_to(const oir::Function &function, const oir::Function &target) {
    for (auto *block : canonical_reachable_order(function)) {
        for (const auto &inst : block->instructions()) {
            auto *call = dynamic_cast<const oir::CallInst *>(inst.get());
            if (call != nullptr && call->callee() == &target) {
                return true;
            }
        }
    }
    return false;
}

void append_reachable_blocks(oir::BasicBlock *block,
                             std::unordered_set<oir::BasicBlock *> &seen,
                             std::vector<oir::BasicBlock *> &out) {
    if (block == nullptr || !seen.insert(block).second) {
        return;
    }
    out.push_back(block);
    for (auto *successor : block->successors()) {
        append_reachable_blocks(successor, seen, out);
    }
}

std::vector<oir::BasicBlock *>
canonical_reachable_order(const oir::Function &function) {
    std::vector<oir::BasicBlock *> out;
    std::unordered_set<oir::BasicBlock *> seen;
    append_reachable_blocks(function.entry_block(), seen, out);
    return out;
}

std::unordered_set<const oir::BasicBlock *>
reachable_block_set(const oir::Function &function) {
    const auto ordered = canonical_reachable_order(function);
    return {ordered.begin(), ordered.end()};
}

std::vector<oir::Function *> direct_callees(const oir::Function &function) {
    std::vector<oir::Function *> out;
    for (auto *block : canonical_reachable_order(function)) {
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

std::uint64_t cloned_instruction_growth(const CalleeInfo &info) {
    return saturating_u64_add(
        saturating_u64_add(info.static_instrs, info.branches),
        saturating_u64_add(info.phis, info.returns > 1 ? 1 : 0));
}

const char *call_growth_class_name(CallGrowthClass growth_class) {
    if (growth_class == CallGrowthClass::Specialization) {
        return "specialization";
    }
    if (growth_class == CallGrowthClass::Ordinary) {
        return "ordinary nonrecursive inline";
    }
    return "self-recursive inline";
}

std::uint64_t call_class_cumulative_growth(
    const Stats &stats, CallGrowthClass growth_class) {
    if (growth_class == CallGrowthClass::Specialization) {
        return stats.cumulative_call_specialization_growth;
    }
    if (growth_class == CallGrowthClass::Ordinary) {
        return stats.cumulative_call_ordinary_growth;
    }
    return stats.cumulative_call_recursive_growth;
}

std::uint64_t call_class_module_growth_budget(
    const Stats &stats, CallGrowthClass growth_class) {
    if (growth_class == CallGrowthClass::Specialization) {
        return stats.call_specialization_module_growth_budget;
    }
    if (growth_class == CallGrowthClass::Ordinary) {
        return stats.call_ordinary_module_growth_budget;
    }
    return stats.call_recursive_module_growth_budget;
}

std::uint64_t call_class_root_growth_budget(
    const Stats &stats, CallGrowthClass growth_class) {
    if (growth_class == CallGrowthClass::Specialization) {
        return stats.call_specialization_root_growth_budget;
    }
    if (growth_class == CallGrowthClass::Ordinary) {
        return stats.call_ordinary_root_growth_budget;
    }
    return stats.call_recursive_root_growth_budget;
}

const std::unordered_map<oir::FunctionID, std::uint64_t> &
call_class_root_growth(const Stats &stats, CallGrowthClass growth_class) {
    if (growth_class == CallGrowthClass::Specialization) {
        return stats.call_specialization_root_growth;
    }
    if (growth_class == CallGrowthClass::Ordinary) {
        return stats.call_ordinary_root_growth;
    }
    return stats.call_recursive_root_growth;
}

std::unordered_map<oir::FunctionID, std::uint64_t> &
call_class_root_growth(Stats &stats, CallGrowthClass growth_class) {
    return const_cast<std::unordered_map<oir::FunctionID, std::uint64_t> &>(
        call_class_root_growth(static_cast<const Stats &>(stats), growth_class));
}

struct CallGrowthBudgetFit {
    bool class_module = true;
    bool class_root = true;
    bool total_module = true;
    bool total_root = true;

    bool fits() const {
        return class_module && class_root && total_module && total_root;
    }
};

bool call_growth_amount_fits(std::uint64_t committed,
                             std::uint64_t added,
                             std::uint64_t budget) {
    return added <= budget - std::min(committed, budget);
}

CallGrowthBudgetFit call_growth_budget_fit(
    const Stats &stats, CallGrowthClass growth_class,
    std::uint64_t added_growth,
    const std::unordered_map<oir::FunctionID, std::uint64_t> &added_roots) {
    CallGrowthBudgetFit fit;
    fit.class_module = call_growth_amount_fits(
        call_class_cumulative_growth(stats, growth_class), added_growth,
        call_class_module_growth_budget(stats, growth_class));
    fit.total_module = call_growth_amount_fits(
        stats.cumulative_call_growth, added_growth,
        stats.call_module_growth_budget);
    const auto &class_roots =
        call_class_root_growth(stats, growth_class);
    for (const auto &[root, growth] : added_roots) {
        const auto class_position = class_roots.find(root);
        const auto class_committed =
            class_position == class_roots.end() ? 0 : class_position->second;
        const auto total_position = stats.call_root_growth.find(root);
        const auto total_committed =
            total_position == stats.call_root_growth.end()
                ? 0
                : total_position->second;
        fit.class_root &= call_growth_amount_fits(
            class_committed, growth,
            call_class_root_growth_budget(stats, growth_class));
        fit.total_root &= call_growth_amount_fits(
            total_committed, growth, stats.call_root_growth_budget);
    }
    return fit;
}

CallGrowthBudgetFit call_growth_budget_fit(
    const Stats &stats, CallGrowthClass growth_class,
    oir::FunctionID root, std::uint64_t growth) {
    const std::unordered_map<oir::FunctionID, std::uint64_t> roots{
        {root, growth}};
    return call_growth_budget_fit(stats, growth_class, growth, roots);
}

void append_call_growth_budget_summary(
    std::string &summary, CallGrowthClass growth_class,
    const CallGrowthBudgetFit &fit) {
    if (!fit.class_module || !fit.class_root) {
        summary += "; ";
        summary += call_growth_class_name(growth_class);
        summary += " class module/root growth quota exhausted";
    }
    if (!fit.total_module || !fit.total_root) {
        summary += "; total call module/root growth hard cap exhausted";
    }
}

struct CallGrowthReservation {
    std::vector<oir::FunctionID> total_roots;
    std::vector<oir::FunctionID> class_roots;
};

bool reserve_call_growth_entries(
    Stats &stats, CallGrowthClass growth_class,
    const std::vector<oir::FunctionID> &roots,
    CallGrowthReservation &reservation) {
    if (!reserve_growth_entries(stats.call_root_growth, roots,
                                reservation.total_roots)) {
        return false;
    }
    if (!reserve_growth_entries(
            call_class_root_growth(stats, growth_class), roots,
            reservation.class_roots)) {
        erase_reserved_growth_entries(stats.call_root_growth,
                                      reservation.total_roots);
        return false;
    }
    return true;
}

void erase_reserved_call_growth_entries(
    Stats &stats, CallGrowthClass growth_class,
    const CallGrowthReservation &reservation) {
    erase_reserved_growth_entries(stats.call_root_growth,
                                  reservation.total_roots);
    erase_reserved_growth_entries(
        call_class_root_growth(stats, growth_class),
        reservation.class_roots);
}

void commit_call_growth(
    Stats &stats, CallGrowthClass growth_class,
    std::uint64_t growth,
    const std::unordered_map<oir::FunctionID, std::uint64_t> &root_growth) {
    stats.cumulative_call_growth =
        saturating_u64_add(stats.cumulative_call_growth, growth);
    auto *class_cumulative =
        growth_class == CallGrowthClass::Specialization
            ? &stats.cumulative_call_specialization_growth
            : (growth_class == CallGrowthClass::Ordinary
                   ? &stats.cumulative_call_ordinary_growth
                   : &stats.cumulative_call_recursive_growth);
    *class_cumulative = saturating_u64_add(*class_cumulative, growth);
    auto &class_roots = call_class_root_growth(stats, growth_class);
    for (const auto &[root, added] : root_growth) {
        stats.call_root_growth.at(root) = saturating_u64_add(
            stats.call_root_growth.at(root), added);
        class_roots.at(root) =
            saturating_u64_add(class_roots.at(root), added);
    }
}

void commit_call_growth(Stats &stats, CallGrowthClass growth_class,
                        oir::FunctionID root, std::uint64_t growth) {
    const std::unordered_map<oir::FunctionID, std::uint64_t> roots{
        {root, growth}};
    commit_call_growth(stats, growth_class, growth, roots);
}

void initialize_call_growth_budget(
    const oir::Module &module, Stats &stats,
    const pass::cost_model::CostModelPolicy &policy) {
    if (stats.call_growth_budget_initialized) {
        return;
    }
    std::uint64_t initial_module_growth = 0;
    std::uint64_t initial_loads = 0;
    std::uint64_t initial_stores = 0;
    for (const auto &function : module.functions()) {
        stats.call_root_specializations.try_emplace(function->root_function_id(), 0);
        if (function->origin() == oir::FunctionOrigin::ResidualSpecialization) {
            ++stats.call_root_specializations.at(function->root_function_id());
        }
        if (!function->is_external()) {
            const auto info = inspect_callee(*function);
            initial_module_growth = saturating_u64_add(
                initial_module_growth, cloned_instruction_growth(info));
            initial_loads = saturating_u64_add(initial_loads, info.loads);
            initial_stores = saturating_u64_add(initial_stores, info.stores);
        }
    }
    const auto allowance = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, policy.small_code_growth_allowance));
    const auto growth_percent = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, policy.max_module_code_growth_percent));
    const auto function_growth = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, policy.max_function_code_growth));
    const __uint128_t percent_product =
        static_cast<__uint128_t>(initial_module_growth) * growth_percent;
    const __uint128_t rounded_percent =
        (percent_product + static_cast<__uint128_t>(99)) / 100;
    const auto percent_budget = static_cast<std::uint64_t>(
        std::min<__uint128_t>(rounded_percent,
                              std::numeric_limits<std::uint64_t>::max()));
    const auto module_growth = std::max(allowance, percent_budget);
    const auto root_growth = std::max(allowance, function_growth);
    const auto recursive_growth = static_cast<std::uint64_t>(
        recursive_inline_growth_budget(policy));
    stats.call_specialization_module_growth_budget = module_growth;
    stats.call_specialization_root_growth_budget = root_growth;
    stats.call_ordinary_module_growth_budget = module_growth;
    stats.call_ordinary_root_growth_budget = root_growth;
    stats.call_recursive_module_growth_budget = recursive_growth;
    stats.call_recursive_root_growth_budget = recursive_growth;
    stats.call_module_growth_budget = saturating_u64_add(
        saturating_u64_add(module_growth, module_growth), recursive_growth);
    stats.call_root_growth_budget = saturating_u64_add(
        saturating_u64_add(root_growth, root_growth), recursive_growth);
    if (const auto *forced_budget =
            std::getenv("YOOLANG_TEST_OIR_CALL_GROWTH_BUDGET")) {
        char *end = nullptr;
        const auto parsed = std::strtoull(forced_budget, &end, 10);
        if (end != forced_budget && end != nullptr && *end == '\0') {
            stats.call_specialization_module_growth_budget = parsed;
            stats.call_specialization_root_growth_budget = parsed;
            stats.call_ordinary_module_growth_budget = parsed;
            stats.call_ordinary_root_growth_budget = parsed;
            stats.call_recursive_module_growth_budget = parsed;
            stats.call_recursive_root_growth_budget = parsed;
            const auto total_budget = saturating_u64_add(
                saturating_u64_add(parsed, parsed), parsed);
            stats.call_module_growth_budget = total_budget;
            stats.call_root_growth_budget = total_budget;
        }
    }
    const auto pressure_scale =
        saturating_u64_multiply(function_growth, 16);
    stats.call_pressure_budget.live_pointers =
        std::max<std::uint64_t>(32, saturating_u64_multiply(function_growth, 2));
    stats.call_pressure_budget.alias_uncertainty =
        std::max<std::uint64_t>(64, pressure_scale);
    stats.call_pressure_budget.loads =
        saturating_u64_add(initial_loads, pressure_scale);
    stats.call_pressure_budget.stores =
        saturating_u64_add(initial_stores, pressure_scale);
    stats.call_pressure_budget.max_live_values =
        saturating_u64_multiply(pressure_scale, 2);
    stats.call_pressure_budget.memory_pressure =
        saturating_u64_add(saturating_u64_add(initial_loads, initial_stores),
                           saturating_u64_multiply(pressure_scale, 2));
    stats.call_pressure_budget.register_pressure =
        saturating_u64_multiply(pressure_scale, 2);
    stats.call_pressure_budget.spill_proxy =
        std::max<std::uint64_t>(32, saturating_u64_multiply(function_growth, 4));
    stats.call_growth_budget_initialized = true;
}

void initialize_call_specialization_work_budget(
    Stats &stats, const pass::cost_model::CostModelPolicy &policy) {
    if (stats.call_specialization_work_initialized) {
        return;
    }
    stats.call_specialization_attempt_budget =
        kSpecializationAttemptBudget;
    stats.call_specialization_work_budget = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, policy.max_compile_time_growth));
    if (residual_test_failure_is("work-budget")) {
        stats.call_specialization_work_budget = 1;
    }
    stats.call_specialization_work_initialized = true;
}

bool specialization_work_has_capacity(const Stats &stats,
                                      std::uint64_t attempts,
                                      std::uint64_t work_units) {
    if (stats.call_specialization_work_exhausted) {
        return false;
    }
    const auto remaining_attempts =
        stats.call_specialization_attempt_budget -
        std::min(stats.call_specialization_attempts,
                 stats.call_specialization_attempt_budget);
    const auto remaining_work =
        stats.call_specialization_work_budget -
        std::min(stats.call_specialization_work_units,
                 stats.call_specialization_work_budget);
    return attempts <= remaining_attempts && work_units <= remaining_work;
}

bool require_specialization_work_capacity(
    Stats &stats, std::uint64_t attempts, std::uint64_t work_units,
    const std::string &proof_rule_id, const std::string &summary) {
    if (specialization_work_has_capacity(stats, attempts, work_units)) {
        return true;
    }
    if (stats.call_specialization_work_exhausted) {
        return false;
    }
    stats.call_specialization_work_exhausted = true;
    OIRTransformCostEstimate rejected;
    rejected.kind =
        pass::cost_model::TransformKind::ConstantArgumentSpecialization;
    rejected.pass_name = "OIRInlinePass";
    rejected.candidate_id =
        "specialize.work." +
        std::to_string(++stats.cost_model_candidates);
    rejected.scope = "specialization-work-budget";
    rejected.proof_kind = pass::cost_model::ProofKind::Structural;
    rejected.proof_status = pass::cost_model::ProofStatus::Proven;
    rejected.proof_rule_id = proof_rule_id;
    rejected.proof_summary = summary;
    rejected.confidence = 1.0;
    rejected.risk.compile_time_growth = static_cast<std::int64_t>(
        std::min<std::uint64_t>(work_units,
                                std::numeric_limits<std::int64_t>::max()));
    rejected.forced_reject_reason =
        pass::cost_model::RejectReason::CompileTimeTooHigh;
    (void)cost_model_allows_transform(stats, rejected);
    return false;
}

void consume_specialization_work(Stats &stats, std::uint64_t attempts,
                                 std::uint64_t work_units) {
    stats.call_specialization_attempts = saturating_u64_add(
        stats.call_specialization_attempts, attempts);
    stats.call_specialization_work_units = saturating_u64_add(
        stats.call_specialization_work_units, work_units);
}

std::uint64_t specialization_scratch_base(const CalleeInfo &original) {
    return std::max<std::uint64_t>(1, original.cost);
}

std::uint64_t specialization_scratch_reservation(
    const CalleeInfo &original) {
    return saturating_u64_multiply(
        specialization_scratch_base(original),
        1 + static_cast<std::uint64_t>(kMaxScratchCleanupRounds));
}

std::uint64_t specialization_plan_work_units(const CalleeInfo &residual) {
    return std::max<std::uint64_t>(1,
                                   cloned_instruction_growth(residual));
}

bool charge_specialization_plan_work(
    Stats &stats, const CalleeInfo &residual,
    const std::string &proof_rule_id, const std::string &summary) {
    const auto work_units = specialization_plan_work_units(residual);
    if (!require_specialization_work_capacity(
            stats, 0, work_units, proof_rule_id, summary)) {
        return false;
    }
    consume_specialization_work(stats, 0, work_units);
    return true;
}

unsigned recursive_pressure_growth(const oir::CallInst &call, const CalleeInfo &info,
                                   unsigned depth) {
    const auto per_level = std::min<std::size_t>(call.args().size(), 4) +
                           (info.calls > 0 ? info.calls - 1 : 0);
    return static_cast<unsigned>(per_level * depth);
}

std::vector<oir::BasicBlock *> clone_order(const oir::Function &function) {
    auto out = canonical_reachable_order(function);
    std::unordered_set<oir::BasicBlock *> seen(out.begin(), out.end());
    for (const auto &block : function.blocks()) {
        if (seen.insert(block.get()).second) {
            out.push_back(block.get());
        }
    }
    return out;
}

bool has_compatible_call_shape(const oir::CallInst &call, const oir::Function &callee) {
    // Exact arity equality is a type/IR legality invariant.  The numeric arity
    // itself is deliberately not an optimization selector; profitability and
    // pressure budgets bound large calls structurally.
    return call.type() == callee.return_type() &&
           call.args().size() == callee.args().size();
}

bool block_reaches(const oir::BasicBlock *from, const oir::BasicBlock *target,
                   std::unordered_set<const oir::BasicBlock *> &seen) {
    if (from == target) {
        return true;
    }
    if (from == nullptr || !seen.insert(from).second) {
        return false;
    }
    for (auto *successor : from->successors()) {
        if (block_reaches(successor, target, seen)) {
            return true;
        }
    }
    return false;
}

bool callsite_is_in_cycle(const oir::CallInst &call) {
    auto *block = call.parent();
    if (block == nullptr) {
        return false;
    }
    for (auto *successor : block->successors()) {
        std::unordered_set<const oir::BasicBlock *> seen;
        if (block_reaches(successor, block, seen)) {
            return true;
        }
    }
    return false;
}

bool has_internal_caller(const oir::Module &module, const oir::Function &target) {
    for (const auto &function : module.functions()) {
        if (!function->is_external() && contains_call_to(*function, target)) {
            return true;
        }
    }
    return false;
}

bool same_call_graph_scc(const oir::Function &caller, const oir::Function &callee) {
    std::unordered_set<const oir::Function *> forward_seen;
    std::unordered_set<const oir::Function *> reverse_seen;
    return reaches_function(caller, callee, forward_seen) &&
           reaches_function(callee, caller, reverse_seen);
}

std::vector<oir::Function *> collect_caller_scc(oir::Module &module, oir::Function &caller) {
    std::vector<oir::Function *> members{&caller};
    for (const auto &function : module.functions()) {
        if (function.get() == &caller || function->is_external()) {
            continue;
        }
        if (same_call_graph_scc(caller, *function)) {
            members.push_back(function.get());
        }
    }
    return members;
}

void mark_affected_functions(InlineContext &context,
                             const std::vector<oir::Function *> &functions) {
    context.affected_functions.insert(functions.begin(), functions.end());
}

template <typename Fn>
bool run_on_affected_functions(oir::Module &module,
                               const std::unordered_set<oir::Function *> &affected,
                               Fn &&fn) {
    auto &functions = module.functions();
    const std::size_t original_size = functions.size();
    std::vector<std::unique_ptr<oir::Function>> storage(original_size);
    std::vector<bool> active_slots(original_size, false);
    std::size_t active_count = 0;
    for (std::size_t index = 0; index < original_size; ++index) {
        if (affected.find(functions[index].get()) == affected.end()) {
            storage[index] = std::move(functions[index]);
            continue;
        }
        active_slots[index] = true;
        functions[active_count++] = std::move(functions[index]);
    }
    functions.resize(active_count);

    auto restore = [&]() {
        std::size_t active_index = 0;
        for (std::size_t index = 0; index < original_size; ++index) {
            if (active_slots[index]) {
                storage[index] = std::move(functions[active_index++]);
            }
        }
        functions = std::move(storage);
    };

    try {
        const bool changed = fn();
        restore();
        return changed;
    } catch (...) {
        restore();
        throw;
    }
}

std::string function_body_fingerprint(const oir::Function &function) {
    std::ostringstream out;
    out << function.print() << '{';
    for (const auto &block : function.blocks()) {
        out << block->print() << ';';
        for (const auto &instruction : block->instructions()) {
            out << instruction->print() << ';';
        }
    }
    out << '}';
    return out.str();
}

unsigned call_exposure_depth(const InlineContext &context, const oir::CallInst &call) {
    auto found = context.exposed_call_depths.find(&call);
    return found == context.exposed_call_depths.end() ? 0 : found->second;
}

std::string structural_hash_text(const std::string &text);

struct CanonicalFunctionColors {
    std::unordered_map<const oir::BasicBlock *, std::string> blocks;
    std::unordered_map<const oir::Instruction *, std::string> instructions;
    std::unordered_map<const oir::CallInst *, std::size_t> call_occurrences;
};

struct StructuralKeyCache {
    std::unordered_map<const oir::Function *, CanonicalFunctionColors> colors;
    std::unordered_map<const oir::Function *, std::string> shapes;
};

std::string structural_value_leaf(const oir::Value *value) {
    if (value == nullptr) {
        return "null";
    }
    if (dynamic_cast<const oir::ConstantInt *>(value) != nullptr ||
        dynamic_cast<const oir::ConstantFloat *>(value) != nullptr ||
        dynamic_cast<const oir::ConstantZero *>(value) != nullptr) {
        return "C:" + constant_key(const_cast<oir::Value *>(value));
    }
    if (dynamic_cast<const oir::UndefValue *>(value) != nullptr) {
        return "U:" + value->type()->print();
    }
    if (auto *argument = dynamic_cast<const oir::Argument *>(value)) {
        return "A:" + std::to_string(argument->index()) + ':' +
               value->type()->print();
    }
    if (auto *callee = dynamic_cast<const oir::Function *>(value)) {
        std::ostringstream out;
        out << "F:" << (callee->is_external() ? "E:" : "I:")
            << static_cast<unsigned>(callee->origin()) << ':'
            << callee->return_type()->print() << ":args";
        for (const auto &argument : callee->args()) {
            out << ':' << argument->type()->print();
        }
        return out.str();
    }
    if (auto *global = dynamic_cast<const oir::GlobalVariable *>(value)) {
        return "G:" + std::string(global->is_const() ? "C:" : "M:") +
               global->value_type()->print();
    }
    return "V:" + value->type()->print();
}

bool has_commutative_operands(const oir::Instruction &instruction) {
    switch (instruction.op()) {
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Mul:
    case oir::Instruction::OpID::FAdd:
    case oir::Instruction::OpID::FMul:
    case oir::Instruction::OpID::And:
    case oir::Instruction::OpID::Xor:
        return true;
    case oir::Instruction::OpID::ICmp:
    case oir::Instruction::OpID::FCmp: {
        const auto pred = static_cast<const oir::CmpInst &>(instruction).pred();
        return pred == oir::CmpPred::EQ || pred == oir::CmpPred::NE;
    }
    default:
        return false;
    }
}

CanonicalFunctionColors refine_canonical_function_colors(
    const oir::Function &function) {
    const auto blocks = canonical_reachable_order(function);
    CanonicalFunctionColors colors;
    std::size_t instruction_count = 0;
    for (auto *block : blocks) {
        colors.blocks.emplace(block, block == function.entry_block() ? "entry" : "block");
        for (const auto &instruction : block->instructions()) {
            std::ostringstream seed;
            seed << "op=" << static_cast<unsigned>(instruction->op()) << ":ty="
                 << instruction->type()->print();
            if (auto *cmp = dynamic_cast<const oir::CmpInst *>(instruction.get())) {
                seed << ":pred=" << static_cast<unsigned>(cmp->pred());
            }
            if (auto *alloca = dynamic_cast<const oir::AllocaInst *>(instruction.get())) {
                seed << ":alloca=" << alloca->allocated_type()->print();
            }
            colors.instructions.emplace(instruction.get(),
                                        structural_hash_text(seed.str()));
            ++instruction_count;
        }
    }

    const auto rounds = std::min<std::size_t>(4, std::max<std::size_t>(2,
        blocks.size() + instruction_count + 1));
    for (std::size_t round = 0; round < rounds; ++round) {
        CanonicalFunctionColors next;
        auto value_color = [&](const oir::Value *value) {
            if (auto *block = dynamic_cast<const oir::BasicBlock *>(value)) {
                auto found = colors.blocks.find(block);
                return std::string("B:") +
                       (found == colors.blocks.end() ? "unreachable" : found->second);
            }
            if (auto *instruction = dynamic_cast<const oir::Instruction *>(value)) {
                auto found = colors.instructions.find(instruction);
                return std::string("I:") +
                       (found == colors.instructions.end() ? "unreachable" : found->second);
            }
            return structural_value_leaf(value);
        };

        for (auto *block : blocks) {
            for (const auto &instruction_ptr : block->instructions()) {
                const auto *instruction = instruction_ptr.get();
                std::ostringstream descriptor;
                descriptor << colors.instructions.at(instruction);
                if (auto *phi = dynamic_cast<const oir::PhiInst *>(instruction)) {
                    std::vector<std::string> incoming;
                    for (const auto &[value, from] : phi->incoming()) {
                        incoming.push_back(value_color(value) + "@" + value_color(from));
                    }
                    std::sort(incoming.begin(), incoming.end());
                    for (const auto &item : incoming) {
                        descriptor << ":phi=" << item;
                    }
                } else {
                    std::vector<std::string> operands;
                    operands.reserve(instruction->operand_count());
                    for (std::size_t index = 0; index < instruction->operand_count(); ++index) {
                        operands.push_back(value_color(instruction->operand(index)));
                    }
                    if (has_commutative_operands(*instruction)) {
                        std::sort(operands.begin(), operands.end());
                    }
                    for (std::size_t index = 0; index < operands.size(); ++index) {
                        descriptor << ":v" << (has_commutative_operands(*instruction) ? 0 : index)
                                   << '=' << operands[index];
                    }
                }
                next.instructions.emplace(instruction,
                    structural_hash_text(descriptor.str()));
            }
        }

        for (auto *block : blocks) {
            std::ostringstream descriptor;
            descriptor << (block == function.entry_block() ? "entry" : "block");
            std::vector<std::string> instruction_colors;
            for (const auto &instruction : block->instructions()) {
                instruction_colors.push_back(next.instructions.at(instruction.get()));
            }
            for (const auto &color : instruction_colors) {
                descriptor << ":i=" << color;
            }
            if (auto *branch = dynamic_cast<const oir::BranchInst *>(block->terminator())) {
                if (branch->is_conditional()) {
                    descriptor << ":true=" << colors.blocks.at(branch->true_bb())
                               << ":false=" << colors.blocks.at(branch->false_bb());
                } else {
                    descriptor << ":target=" << colors.blocks.at(branch->target_bb());
                }
            }
            std::vector<std::string> predecessors;
            for (auto *predecessor : block->predecessors()) {
                auto found = colors.blocks.find(predecessor);
                if (found != colors.blocks.end()) {
                    predecessors.push_back(found->second);
                }
            }
            std::sort(predecessors.begin(), predecessors.end());
            for (const auto &color : predecessors) {
                descriptor << ":pred=" << color;
            }
            next.blocks.emplace(block, structural_hash_text(descriptor.str()));
        }
        colors = std::move(next);
    }
    for (auto *block : blocks) {
        std::unordered_map<std::string, std::size_t> occurrences;
        // A block's instruction sequence is semantic program order, unlike module,
        // block-list, or successor-container insertion order.  Rank only calls in
        // the same refined-color class so equivalent positions in symmetric blocks
        // remain a tie while repeated calls at distinct positions do not collapse.
        for (const auto &instruction : block->instructions()) {
            auto *call = dynamic_cast<const oir::CallInst *>(instruction.get());
            if (call == nullptr) {
                continue;
            }
            const auto &color = colors.instructions.at(call);
            colors.call_occurrences.emplace(call, occurrences[color]++);
        }
    }
    return colors;
}

std::string canonical_function_summary(const oir::Function &function,
                                       const CanonicalFunctionColors &colors) {
    std::vector<std::string> block_histogram;
    block_histogram.reserve(colors.blocks.size());
    for (const auto &[block, color] : colors.blocks) {
        (void)block;
        block_histogram.push_back(color);
    }
    std::sort(block_histogram.begin(), block_histogram.end());

    std::ostringstream out;
    out << "ret=" << function.return_type()->print() << ";origin="
        << static_cast<unsigned>(function.origin()) << ";args";
    for (const auto &argument : function.args()) {
        out << ':' << argument->type()->print();
    }
    out << ";entry=";
    const auto entry = colors.blocks.find(function.entry_block());
    out << (entry == colors.blocks.end() ? "external" : entry->second)
        << ";blocks";
    for (const auto &color : block_histogram) {
        out << ':' << color;
    }
    return out.str();
}

const CanonicalFunctionColors &canonical_function_colors(
    const oir::Function &function, StructuralKeyCache &cache) {
    auto found = cache.colors.find(&function);
    if (found == cache.colors.end()) {
        found = cache.colors.emplace(
            &function, refine_canonical_function_colors(function)).first;
    }
    return found->second;
}

std::string canonical_function_shape(const oir::Function &function,
                                     StructuralKeyCache *cache = nullptr) {
    if (cache == nullptr) {
        return canonical_function_summary(
            function, refine_canonical_function_colors(function));
    }
    auto found = cache->shapes.find(&function);
    if (found == cache->shapes.end()) {
        found = cache->shapes.emplace(
            &function, canonical_function_summary(
                           function, canonical_function_colors(function, *cache))).first;
    }
    return found->second;
}

std::string canonical_callsite_position(const oir::Function &function,
                                        const oir::CallInst &call,
                                        StructuralKeyCache *cache = nullptr) {
    CanonicalFunctionColors local_colors;
    const auto *orbit_colors = &local_colors;
    if (cache == nullptr) {
        local_colors = refine_canonical_function_colors(function);
    } else {
        orbit_colors = &canonical_function_colors(function, *cache);
    }
    auto value_position = [&](const oir::Value *value) {
        if (auto *argument = dynamic_cast<const oir::Argument *>(value)) {
            return std::string("A:") + std::to_string(argument->index()) + ':' +
                   argument->type()->print();
        }
        if (auto *block = dynamic_cast<const oir::BasicBlock *>(value)) {
            return std::string("B:") + orbit_colors->blocks.at(block);
        }
        if (auto *instruction = dynamic_cast<const oir::Instruction *>(value)) {
            if (instruction->op() == oir::Instruction::OpID::Phi) {
                return std::string("I:phi:") + instruction->type()->print();
            }
            return std::string("I:") + orbit_colors->instructions.at(instruction);
        }
        return structural_value_leaf(value);
    };
    const auto &call_color = orbit_colors->instructions.at(&call);
    std::ostringstream position;
    position << "block=" << orbit_colors->blocks.at(call.parent())
             << ":instruction=" << call_color
             << ":occurrence=" << orbit_colors->call_occurrences.at(&call)
             << ":callee=" << value_position(call.callee());
    for (auto *argument : call.args()) {
        position << ":arg=" << value_position(argument);
    }
    return structural_hash_text(position.str());
}

std::uint64_t stable_structural_hash(const std::string &text) {
    // Fixed FNV-1a parameters keep SCC colors stable across processes and hosts.
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : text) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string structural_hash_text(const std::string &text) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0')
        << stable_structural_hash(text);
    return out.str();
}

std::string normalized_scc_topology(const oir::Function &root,
                                    const oir::Function *peer,
                                    StructuralKeyCache *cache = nullptr) {
    if (root.is_external()) {
        return "external:" + root.return_type()->print();
    }

    std::vector<const oir::Function *> component;
    if (auto *module = root.parent()) {
        for (const auto &function : module->functions()) {
            if (function->is_external()) {
                continue;
            }
            if (function.get() == &root ||
                same_call_graph_scc(root, *function)) {
                component.push_back(function.get());
            }
        }
    }
    if (component.empty()) {
        component.push_back(&root);
    }

    std::unordered_map<const oir::Function *, std::size_t> node_index;
    for (std::size_t index = 0; index < component.size(); ++index) {
        node_index.emplace(component[index], index);
    }
    std::vector<std::vector<std::size_t>> outgoing(component.size());
    std::vector<std::vector<std::size_t>> incoming(component.size());
    for (std::size_t source = 0; source < component.size(); ++source) {
        for (auto *target : direct_callees(*component[source])) {
            auto found = node_index.find(target);
            if (found == node_index.end()) {
                continue;
            }
            outgoing[source].push_back(found->second);
            incoming[found->second].push_back(source);
        }
    }

    std::vector<std::string> colors(component.size());
    for (std::size_t index = 0; index < component.size(); ++index) {
        std::string seed = canonical_function_shape(*component[index], cache);
        seed += component[index] == &root ? "|role=root" : "|role=node";
        if (peer != nullptr && component[index] == peer) {
            seed += "|role=peer";
        }
        colors[index] = structural_hash_text(seed);
    }
    for (std::size_t round = 0; round < component.size(); ++round) {
        std::vector<std::string> next(colors.size());
        for (std::size_t index = 0; index < component.size(); ++index) {
            std::vector<std::string> outgoing_colors;
            std::vector<std::string> incoming_colors;
            outgoing_colors.reserve(outgoing[index].size());
            incoming_colors.reserve(incoming[index].size());
            for (const auto target : outgoing[index]) {
                outgoing_colors.push_back(colors[target]);
            }
            for (const auto source : incoming[index]) {
                incoming_colors.push_back(colors[source]);
            }
            std::sort(outgoing_colors.begin(), outgoing_colors.end());
            std::sort(incoming_colors.begin(), incoming_colors.end());
            std::ostringstream descriptor;
            descriptor << colors[index] << "|out";
            for (const auto &color : outgoing_colors) {
                descriptor << ':' << color;
            }
            descriptor << "|in";
            for (const auto &color : incoming_colors) {
                descriptor << ':' << color;
            }
            next[index] = structural_hash_text(descriptor.str());
        }
        if (next == colors) {
            break;
        }
        colors = std::move(next);
    }

    const auto root_position = node_index.at(&root);
    std::vector<std::string> color_histogram = colors;
    std::sort(color_histogram.begin(), color_histogram.end());
    std::vector<std::string> edge_colors;
    for (std::size_t source = 0; source < outgoing.size(); ++source) {
        for (const auto target : outgoing[source]) {
            edge_colors.push_back(colors[source] + ">" + colors[target]);
        }
    }
    std::sort(edge_colors.begin(), edge_colors.end());

    auto append_runs = [](std::ostringstream &out, const auto &items) {
        for (std::size_t begin = 0; begin < items.size();) {
            std::size_t end = begin + 1;
            while (end < items.size() && items[end] == items[begin]) {
                ++end;
            }
            out << ':' << items[begin] << '*' << (end - begin);
            begin = end;
        }
    };
    std::ostringstream topology;
    topology << "n=" << component.size() << ":root=" << colors[root_position];
    auto peer_position = peer == nullptr ? node_index.end() : node_index.find(peer);
    if (peer_position != node_index.end()) {
        topology << ":peer=" << colors[peer_position->second];
    }
    topology << ":nodes";
    append_runs(topology, color_histogram);
    topology << ":edges";
    append_runs(topology, edge_colors);
    return topology.str();
}

std::string structural_callsite_key(const oir::Function &caller, const oir::CallInst &call,
                                    const oir::Function *callee,
                                    StructuralKeyCache *cache = nullptr) {
    std::ostringstream key;
    const bool same_component =
        callee != nullptr &&
        (callee == &caller || same_call_graph_scc(caller, *callee));
    key << "scc="
        << normalized_scc_topology(caller,
                                   same_component ? callee : nullptr, cache)
        << ":callee-scc="
        << (callee == nullptr
                ? std::string("external-or-indirect")
                : (same_component
                       ? std::string("same")
                       : normalized_scc_topology(*callee, nullptr, cache)))
        << ":edge=" << same_component
        << ":loop=" << callsite_is_in_cycle(call)
        << ":dead=" << call.users().empty() << ":caller="
        << canonical_function_shape(caller, cache) << ":callee="
        << (callee == nullptr ? std::string("external-or-indirect")
                              : canonical_function_shape(*callee, cache));
    key << ':' << typed_argument_bindings(call.args());
    key << ":position=" << canonical_callsite_position(caller, call, cache);
    return key.str();
}

std::string structural_callsite_fingerprint(const std::string &key) {
    return "callsite." + structural_hash_text(key);
}

std::int64_t callsite_score(const oir::Module &module, const InlineContext &context,
                            const oir::Function &caller, const oir::CallInst &call,
                            const oir::Function *callee) {
    const bool direct_recursive = callee == &caller;
    std::int64_t score =
        direct_recursive
            ? -static_cast<std::int64_t>(recursive_inline_depth(context, call)) * 1000
            : static_cast<std::int64_t>(call_exposure_depth(context, call)) * 1000;
    // Prefer bottom-up simplification until an accepted inline explicitly exposes a
    // new caller context.  This keeps a helper compact before the higher-context
    // worklist revisits it, without relying on module definition order.
    score += has_internal_caller(module, caller) ? 300 : 0;
    score += callsite_is_in_cycle(call) ? 160 : 0;
    score += call.users().empty() ? 80 : 0;
    for (auto *argument : call.args()) {
        if (is_specializable_constant(argument)) {
            score += 60;
        }
    }
    if (callee != nullptr) {
        const auto info = inspect_callee(*callee);
        score -= static_cast<std::int64_t>(std::min<unsigned>(info.cost, 200));
        if (same_call_graph_scc(caller, *callee)) {
            score -= 400;
        }
    }
    return score;
}

void retain_live_exposed_calls(oir::Module &module, InlineContext &context) {
    std::unordered_map<const oir::CallInst *, unsigned> live;
    for (const auto &function : module.functions()) {
        for (const auto &block : function->blocks()) {
            for (const auto &instruction : block->instructions()) {
                auto *call = dynamic_cast<oir::CallInst *>(instruction.get());
                if (call == nullptr) {
                    continue;
                }
                auto found = context.exposed_call_depths.find(call);
                if (found != context.exposed_call_depths.end()) {
                    live.emplace(call, found->second);
                }
            }
        }
    }
    context.exposed_call_depths = std::move(live);
}

enum class ResidualSymbolKind {
    Unknown,
    Constant,
    Identity,
};

bool is_residual_i32_type(const oir::Type *type) {
    auto *integer = dynamic_cast<const oir::IntegerType *>(type);
    return integer != nullptr && integer->bit_width() == 32;
}

bool is_residual_i1_type(const oir::Type *type) {
    auto *integer = dynamic_cast<const oir::IntegerType *>(type);
    return integer != nullptr && integer->bit_width() == 1;
}

std::optional<std::int64_t> exact_residual_integer_constant(oir::Value *value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    if (dynamic_cast<oir::ConstantZero *>(value) != nullptr) {
        return is_residual_i1_type(value->type()) || is_residual_i32_type(value->type())
                   ? std::optional<std::int64_t>(0)
                   : std::nullopt;
    }
    auto *constant = dynamic_cast<oir::ConstantInt *>(value);
    if (constant == nullptr) {
        return std::nullopt;
    }
    const auto raw = constant->value();
    if (is_residual_i1_type(value->type())) {
        return raw == 0 || raw == 1 ? std::optional<std::int64_t>(raw) : std::nullopt;
    }
    if (!is_residual_i32_type(value->type())) {
        return std::nullopt;
    }
    const auto canonical = static_cast<std::int64_t>(static_cast<std::int32_t>(raw));
    return raw == canonical ? std::optional<std::int64_t>(canonical) : std::nullopt;
}

struct ResidualSymbol {
    ResidualSymbolKind kind = ResidualSymbolKind::Unknown;
    std::int64_t constant = 0;
    const oir::Value *identity = nullptr;
};

ResidualSymbol residual_constant(std::int64_t value) {
    return {ResidualSymbolKind::Constant, value, nullptr};
}

ResidualSymbol residual_identity(const oir::Value *value) {
    return {ResidualSymbolKind::Identity, 0, value};
}

bool same_residual_symbol(const ResidualSymbol &lhs, const ResidualSymbol &rhs) {
    if (lhs.kind != rhs.kind) {
        return false;
    }
    if (lhs.kind == ResidualSymbolKind::Constant) {
        return lhs.constant == rhs.constant;
    }
    if (lhs.kind == ResidualSymbolKind::Identity) {
        return lhs.identity == rhs.identity;
    }
    return false;
}

ResidualSymbol evaluate_residual_binary(oir::Instruction::OpID op, ResidualSymbol lhs,
                                        ResidualSymbol rhs) {
    if (lhs.kind == ResidualSymbolKind::Constant &&
        rhs.kind == ResidualSymbolKind::Constant) {
        if (auto folded = fold_int_binary(op, lhs.constant, rhs.constant)) {
            return residual_constant(*folded);
        }
        return {};
    }

    const auto is_constant = [](const ResidualSymbol &value, std::int64_t expected) {
        return value.kind == ResidualSymbolKind::Constant && value.constant == expected;
    };
    switch (op) {
    case oir::Instruction::OpID::Add:
        if (is_constant(rhs, 0)) {
            return lhs;
        }
        if (is_constant(lhs, 0)) {
            return rhs;
        }
        break;
    case oir::Instruction::OpID::Sub:
        if (is_constant(rhs, 0)) {
            return lhs;
        }
        if (same_residual_symbol(lhs, rhs)) {
            return residual_constant(0);
        }
        break;
    case oir::Instruction::OpID::Mul:
        if (is_constant(lhs, 0) || is_constant(rhs, 0)) {
            return residual_constant(0);
        }
        if (is_constant(lhs, 1)) {
            return rhs;
        }
        if (is_constant(rhs, 1)) {
            return lhs;
        }
        break;
    case oir::Instruction::OpID::And:
        if (is_constant(lhs, 0) || is_constant(rhs, 0)) {
            return residual_constant(0);
        }
        if (same_residual_symbol(lhs, rhs)) {
            return lhs;
        }
        break;
    case oir::Instruction::OpID::Xor:
        if (is_constant(lhs, 0)) {
            return rhs;
        }
        if (is_constant(rhs, 0)) {
            return lhs;
        }
        if (same_residual_symbol(lhs, rhs)) {
            return residual_constant(0);
        }
        break;
    case oir::Instruction::OpID::SDiv:
        if (is_constant(lhs, 0) && rhs.kind == ResidualSymbolKind::Constant &&
            rhs.constant != 0) {
            return residual_constant(0);
        }
        if (is_constant(rhs, 1)) {
            return lhs;
        }
        break;
    case oir::Instruction::OpID::SRem:
        if ((is_constant(lhs, 0) && rhs.kind == ResidualSymbolKind::Constant &&
             rhs.constant != 0) ||
            is_constant(rhs, 1) || is_constant(rhs, -1)) {
            return residual_constant(0);
        }
        break;
    default:
        break;
    }
    return {};
}

struct ResidualLoopPhi {
    oir::PhiInst *phi = nullptr;
    oir::Value *backedge = nullptr;
    std::vector<std::size_t> outside_indices;
};

using ResidualEnvironment = std::unordered_map<oir::Value *, ResidualSymbol>;

ResidualSymbol residual_symbol_for(oir::Value *value, const ResidualEnvironment &environment,
                                   const oir::BasicBlock &body) {
    if (auto constant = exact_residual_integer_constant(value)) {
        return residual_constant(*constant);
    }
    if (dynamic_cast<oir::ConstantInt *>(value) != nullptr ||
        dynamic_cast<oir::ConstantZero *>(value) != nullptr) {
        return {};
    }
    if (auto found = environment.find(value); found != environment.end()) {
        return found->second;
    }
    auto *instruction = dynamic_cast<oir::Instruction *>(value);
    if (instruction != nullptr && instruction->parent() == &body) {
        return {};
    }
    return residual_identity(value);
}

ResidualEnvironment evaluate_residual_loop_body(const oir::BasicBlock &body,
                                                 const std::vector<ResidualLoopPhi> &phis,
                                                 oir::PhiInst &constant_state,
                                                 std::int64_t state_value) {
    ResidualEnvironment environment;
    for (const auto &phi : phis) {
        environment.emplace(phi.phi, phi.phi == &constant_state
                                          ? residual_constant(state_value)
                                          : residual_identity(phi.phi));
    }

    for (const auto &instruction_ptr : body.instructions()) {
        auto *instruction = instruction_ptr.get();
        if (dynamic_cast<oir::BranchInst *>(instruction) != nullptr) {
            continue;
        }
        ResidualSymbol value;
        if (auto *binary = dynamic_cast<oir::BinaryInst *>(instruction)) {
            value = evaluate_residual_binary(
                binary->op(), residual_symbol_for(binary->lhs(), environment, body),
                residual_symbol_for(binary->rhs(), environment, body));
        } else if (auto *cmp = dynamic_cast<oir::CmpInst *>(instruction)) {
            auto lhs = residual_symbol_for(cmp->lhs(), environment, body);
            auto rhs = residual_symbol_for(cmp->rhs(), environment, body);
            if (lhs.kind == ResidualSymbolKind::Constant &&
                rhs.kind == ResidualSymbolKind::Constant) {
                value = residual_constant(eval_cmp(cmp->pred(), lhs.constant, rhs.constant));
            } else if (same_residual_symbol(lhs, rhs)) {
                value = residual_constant(eval_cmp(cmp->pred(), 0, 0));
            }
        } else if (auto *cast = dynamic_cast<oir::CastInst *>(instruction)) {
            auto source = residual_symbol_for(cast->src(), environment, body);
            if (cast->op() == oir::Instruction::OpID::ZExt &&
                source.kind == ResidualSymbolKind::Constant) {
                value = residual_constant(source.constant == 0 ? 0 : 1);
            }
        }
        environment.emplace(instruction, value);
    }
    return environment;
}

bool value_has_use_outside_loop(const oir::Value &value, const oir::BasicBlock &header,
                                const oir::BasicBlock &body) {
    for (const auto &use : value.uses()) {
        auto *instruction = dynamic_cast<oir::Instruction *>(use.user);
        if (instruction == nullptr ||
            (instruction->parent() != &header && instruction->parent() != &body)) {
            return true;
        }
    }
    return false;
}

std::optional<std::int64_t> identical_outside_integer(const ResidualLoopPhi &phi) {
    std::optional<std::int64_t> result;
    for (auto index : phi.outside_indices) {
        const auto &incoming = phi.phi->incoming();
        if (index >= incoming.size()) {
            return std::nullopt;
        }
        auto constant = exact_residual_integer_constant(incoming[index].first);
        if (!constant || (result && *result != *constant)) {
            return std::nullopt;
        }
        result = *constant;
    }
    return result;
}

bool is_safe_dead_countdown_instruction(oir::Instruction &instruction) {
    switch (instruction.op()) {
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::Mul: {
        auto &binary = static_cast<oir::BinaryInst &>(instruction);
        return is_residual_i32_type(binary.type()) &&
               is_residual_i32_type(binary.lhs()->type()) &&
               is_residual_i32_type(binary.rhs()->type());
    }
    case oir::Instruction::OpID::And:
    case oir::Instruction::OpID::Xor: {
        auto &binary = static_cast<oir::BinaryInst &>(instruction);
        const bool all_i32 = is_residual_i32_type(binary.type()) &&
                             is_residual_i32_type(binary.lhs()->type()) &&
                             is_residual_i32_type(binary.rhs()->type());
        const bool all_i1 = is_residual_i1_type(binary.type()) &&
                            is_residual_i1_type(binary.lhs()->type()) &&
                            is_residual_i1_type(binary.rhs()->type());
        return all_i32 || all_i1;
    }
    case oir::Instruction::OpID::SDiv:
    case oir::Instruction::OpID::SRem: {
        auto &binary = static_cast<oir::BinaryInst &>(instruction);
        auto divisor = exact_residual_integer_constant(binary.rhs());
        return is_residual_i32_type(binary.type()) &&
               is_residual_i32_type(binary.lhs()->type()) &&
               is_residual_i32_type(binary.rhs()->type()) && divisor && *divisor != 0 &&
               *divisor != -1;
    }
    case oir::Instruction::OpID::ICmp: {
        auto &cmp = static_cast<oir::CmpInst &>(instruction);
        const bool operands_i32 = is_residual_i32_type(cmp.lhs()->type()) &&
                                  is_residual_i32_type(cmp.rhs()->type());
        const bool operands_i1 = is_residual_i1_type(cmp.lhs()->type()) &&
                                 is_residual_i1_type(cmp.rhs()->type());
        return is_residual_i1_type(cmp.type()) && (operands_i32 || operands_i1);
    }
    case oir::Instruction::OpID::ZExt: {
        auto &cast = static_cast<oir::CastInst &>(instruction);
        return is_residual_i32_type(cast.type()) && is_residual_i1_type(cast.src()->type());
    }
    case oir::Instruction::OpID::Phi:
    case oir::Instruction::OpID::Br:
        return true;
    case oir::Instruction::OpID::Ret:
    case oir::Instruction::OpID::FAdd:
    case oir::Instruction::OpID::FSub:
    case oir::Instruction::OpID::FMul:
    case oir::Instruction::OpID::FDiv:
    case oir::Instruction::OpID::FCmp:
    case oir::Instruction::OpID::Alloca:
    case oir::Instruction::OpID::Load:
    case oir::Instruction::OpID::Store:
    case oir::Instruction::OpID::GetElementPtr:
    case oir::Instruction::OpID::Call:
    case oir::Instruction::OpID::MemZero:
    case oir::Instruction::OpID::SIToFP:
    case oir::Instruction::OpID::FPToSI:
        return false;
    }
    return false;
}

bool try_eliminate_dead_pure_countdown_loop(oir::Module &module, oir::BasicBlock &header,
                                            FixedPointSuffixSummary &summary) {
    auto *function = header.parent();
    auto *header_branch = dynamic_cast<oir::BranchInst *>(header.terminator());
    if (function == nullptr || header_branch == nullptr || !header_branch->is_conditional()) {
        return false;
    }

    oir::DominatorTree dom_tree(*function);
    oir::LoopInfo loop_info(*function, dom_tree);
    const oir::Loop *loop = nullptr;
    for (const auto &candidate : loop_info.loops()) {
        if (candidate.header == &header) {
            loop = &candidate;
            break;
        }
    }
    if (loop == nullptr || loop->latches.empty() || loop->blocks.size() < 2) {
        return false;
    }

    std::unordered_set<const oir::BasicBlock *> region(loop->blocks.begin(), loop->blocks.end());
    region.erase(&header);
    const bool true_inside = region.find(header_branch->true_bb()) != region.end();
    const bool false_inside = region.find(header_branch->false_bb()) != region.end();
    if (true_inside == false_inside) {
        return false;
    }

    auto *condition = dynamic_cast<oir::CmpInst *>(header_branch->cond());
    if (condition == nullptr || condition->op() != oir::Instruction::OpID::ICmp) {
        return false;
    }
    for (const auto &instruction : header.instructions()) {
        auto *raw = instruction.get();
        if (dynamic_cast<oir::PhiInst *>(raw) == nullptr && raw != condition &&
            raw != header_branch) {
            return false;
        }
    }

    oir::PhiInst *countdown = nullptr;
    bool countdown_is_lhs = false;
    if (auto *phi = dynamic_cast<oir::PhiInst *>(condition->lhs());
        phi != nullptr && phi->parent() == &header && is_int_value(condition->rhs(), 0)) {
        countdown = phi;
        countdown_is_lhs = true;
    } else if (auto *phi = dynamic_cast<oir::PhiInst *>(condition->rhs());
               phi != nullptr && phi->parent() == &header && is_int_value(condition->lhs(), 0)) {
        countdown = phi;
    }
    if (countdown == nullptr || !is_residual_i32_type(countdown->type())) {
        return false;
    }

    std::optional<std::int64_t> trip_count;
    std::vector<std::size_t> outside_indices;
    std::unordered_set<const oir::BasicBlock *> incoming_latches;
    const auto &incoming = countdown->incoming();
    for (std::size_t index = 0; index < incoming.size(); ++index) {
        auto *value = incoming[index].first;
        auto *from = incoming[index].second;
        if (region.find(from) == region.end()) {
            auto constant = exact_residual_integer_constant(value);
            if (!constant || (trip_count && *trip_count != *constant)) {
                return false;
            }
            trip_count = *constant;
            outside_indices.push_back(index);
            continue;
        }

        auto *update = dynamic_cast<oir::BinaryInst *>(value);
        const bool subtracts_one =
            update != nullptr && update->parent() == from &&
            ((update->op() == oir::Instruction::OpID::Sub && update->lhs() == countdown &&
              is_int_value(update->rhs(), 1)) ||
             (update->op() == oir::Instruction::OpID::Add &&
              ((update->lhs() == countdown && is_int_value(update->rhs(), -1)) ||
               (update->rhs() == countdown && is_int_value(update->lhs(), -1)))));
        if (!subtracts_one) {
            return false;
        }
        incoming_latches.insert(from);
    }
    if (!trip_count || *trip_count <= 0 || outside_indices.empty() ||
        incoming_latches.size() != loop->latches.size()) {
        return false;
    }
    for (auto *latch : loop->latches) {
        if (incoming_latches.find(latch) == incoming_latches.end()) {
            return false;
        }
    }

    auto condition_for = [&](std::int64_t value) {
        return countdown_is_lhs ? eval_cmp(condition->pred(), value, 0)
                                : eval_cmp(condition->pred(), 0, value);
    };
    const bool body_on_true = true_inside;
    // The countdown proof requires every value in [1, trip_count] to enter the
    // body and zero to leave it.  ICmp against zero is monotone over that
    // positive interval, so checking both endpoints rejects single-value
    // predicates such as `remaining == 1` without iterating a huge constant
    // trip count.
    if (condition_for(*trip_count) != body_on_true ||
        condition_for(1) != body_on_true || condition_for(0) == body_on_true) {
        return false;
    }

    std::unordered_map<const oir::BasicBlock *, unsigned> visit_state;
    std::function<bool(const oir::BasicBlock *)> region_is_acyclic =
        [&](const oir::BasicBlock *block) {
            if (block == &header) {
                return true;
            }
            if (region.find(block) == region.end()) {
                return false;
            }
            auto &state = visit_state[block];
            if (state == 1) {
                return false;
            }
            if (state == 2) {
                return true;
            }
            state = 1;
            if (block->successors().empty()) {
                return false;
            }
            for (auto *successor : block->successors()) {
                if (!region_is_acyclic(successor)) {
                    return false;
                }
            }
            state = 2;
            return true;
        };
    auto *body_entry = true_inside ? header_branch->true_bb() : header_branch->false_bb();
    if (!region_is_acyclic(body_entry) || visit_state.size() != region.size()) {
        return false;
    }

    auto use_is_inside = [&](const auto &use) {
        auto *instruction = dynamic_cast<oir::Instruction *>(use.user);
        if (instruction == nullptr || instruction->parent() == nullptr) {
            return false;
        }
        return instruction->parent() == &header ||
               region.find(instruction->parent()) != region.end();
    };
    for (const auto &instruction : header.instructions()) {
        if (dynamic_cast<oir::PhiInst *>(instruction.get()) == nullptr) {
            continue;
        }
        if (std::any_of(instruction->uses().begin(), instruction->uses().end(),
                        [&](const auto &use) { return !use_is_inside(use); })) {
            return false;
        }
    }
    std::uint64_t loop_instructions = header.instructions().size();
    for (auto *block : region) {
        loop_instructions += block->instructions().size();
        for (const auto &instruction : block->instructions()) {
            if (!is_safe_dead_countdown_instruction(*instruction) ||
                std::any_of(instruction->uses().begin(), instruction->uses().end(),
                            [&](const auto &use) { return !use_is_inside(use); })) {
                return false;
            }
        }
    }

    auto *zero = make_int_constant(module, countdown->type(), 0);
    for (auto index : outside_indices) {
        countdown->set_operand(index * 2, zero);
    }
    summary.loops = saturating_u64_add(summary.loops, 1);
    summary.before_iterations = saturating_u64_add(
        summary.before_iterations, static_cast<std::uint64_t>(*trip_count));
    summary.eliminated_dynamic_instructions = saturating_u64_add(
        summary.eliminated_dynamic_instructions,
        saturating_u64_multiply(static_cast<std::uint64_t>(*trip_count), loop_instructions));
    return true;
}

bool is_safe_residual_suffix_body(const oir::BasicBlock &body) {
    for (const auto &instruction_ptr : body.instructions()) {
        auto *instruction = instruction_ptr.get();
        switch (instruction->op()) {
        case oir::Instruction::OpID::Add:
        case oir::Instruction::OpID::Sub:
        case oir::Instruction::OpID::Mul: {
            auto *binary = static_cast<oir::BinaryInst *>(instruction);
            if (!is_residual_i32_type(binary->type()) ||
                !is_residual_i32_type(binary->lhs()->type()) ||
                !is_residual_i32_type(binary->rhs()->type())) {
                return false;
            }
            break;
        }
        case oir::Instruction::OpID::And:
        case oir::Instruction::OpID::Xor: {
            auto *binary = static_cast<oir::BinaryInst *>(instruction);
            const bool all_i32 = is_residual_i32_type(binary->type()) &&
                                 is_residual_i32_type(binary->lhs()->type()) &&
                                 is_residual_i32_type(binary->rhs()->type());
            const bool all_i1 = is_residual_i1_type(binary->type()) &&
                                is_residual_i1_type(binary->lhs()->type()) &&
                                is_residual_i1_type(binary->rhs()->type());
            if (!all_i32 && !all_i1) {
                return false;
            }
            break;
        }
        case oir::Instruction::OpID::ICmp: {
            auto *cmp = static_cast<oir::CmpInst *>(instruction);
            const bool operands_i32 = is_residual_i32_type(cmp->lhs()->type()) &&
                                      is_residual_i32_type(cmp->rhs()->type());
            const bool operands_i1 = is_residual_i1_type(cmp->lhs()->type()) &&
                                     is_residual_i1_type(cmp->rhs()->type());
            if (!is_residual_i1_type(cmp->type()) ||
                (!operands_i32 && !operands_i1)) {
                return false;
            }
            break;
        }
        case oir::Instruction::OpID::ZExt: {
            auto *cast = static_cast<oir::CastInst *>(instruction);
            if (!is_residual_i32_type(cast->type()) ||
                !is_residual_i1_type(cast->src()->type())) {
                return false;
            }
            break;
        }
        case oir::Instruction::OpID::SDiv:
        case oir::Instruction::OpID::SRem: {
            auto *binary = static_cast<oir::BinaryInst *>(instruction);
            if (!is_residual_i32_type(binary->type()) ||
                !is_residual_i32_type(binary->lhs()->type()) ||
                !is_residual_i32_type(binary->rhs()->type())) {
                return false;
            }
            auto divisor = exact_residual_integer_constant(binary->rhs());
            if (!divisor || *divisor == 0 || *divisor == -1) {
                return false;
            }
            break;
        }
        case oir::Instruction::OpID::Br: {
            auto *branch = static_cast<oir::BranchInst *>(instruction);
            if (branch != body.terminator() || branch->is_conditional()) {
                return false;
            }
            break;
        }
        case oir::Instruction::OpID::Ret:
        case oir::Instruction::OpID::FAdd:
        case oir::Instruction::OpID::FSub:
        case oir::Instruction::OpID::FMul:
        case oir::Instruction::OpID::FDiv:
        case oir::Instruction::OpID::FCmp:
        case oir::Instruction::OpID::Alloca:
        case oir::Instruction::OpID::Load:
        case oir::Instruction::OpID::Store:
        case oir::Instruction::OpID::GetElementPtr:
        case oir::Instruction::OpID::Call:
        case oir::Instruction::OpID::MemZero:
        case oir::Instruction::OpID::SIToFP:
        case oir::Instruction::OpID::FPToSI:
        case oir::Instruction::OpID::Phi:
            return false;
        }
    }
    return true;
}

bool try_truncate_constant_fixed_point_suffix(oir::Module &module, oir::BasicBlock &header,
                                              FixedPointSuffixSummary &summary) {
    if (try_eliminate_dead_pure_countdown_loop(module, header, summary)) {
        return true;
    }
    auto *header_branch = dynamic_cast<oir::BranchInst *>(header.terminator());
    if (header_branch == nullptr || !header_branch->is_conditional()) {
        return false;
    }

    auto is_latch_to_header = [&](oir::BasicBlock *candidate) {
        if (candidate == nullptr || candidate == &header || candidate->predecessors().size() != 1 ||
            candidate->predecessors().front() != &header) {
            return false;
        }
        auto *branch = dynamic_cast<oir::BranchInst *>(candidate->terminator());
        return branch != nullptr && !branch->is_conditional() && branch->target_bb() == &header;
    };
    oir::BasicBlock *body = nullptr;
    if (is_latch_to_header(header_branch->true_bb())) {
        body = header_branch->true_bb();
    }
    if (is_latch_to_header(header_branch->false_bb())) {
        if (body != nullptr) {
            return false;
        }
        body = header_branch->false_bb();
    }
    if (body == nullptr || !is_safe_residual_suffix_body(*body)) {
        return false;
    }

    auto *condition = dynamic_cast<oir::CmpInst *>(header_branch->cond());
    if (condition == nullptr || condition->op() != oir::Instruction::OpID::ICmp) {
        return false;
    }
    for (const auto &instruction : header.instructions()) {
        auto *raw = instruction.get();
        if (dynamic_cast<oir::PhiInst *>(raw) == nullptr && raw != condition &&
            raw != header_branch) {
            return false;
        }
    }

    std::vector<ResidualLoopPhi> phis;
    for (const auto &instruction : header.instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(instruction.get());
        if (phi == nullptr) {
            break;
        }
        ResidualLoopPhi info;
        info.phi = phi;
        for (std::size_t index = 0; index < phi->incoming().size(); ++index) {
            const auto &[value, from] = phi->incoming()[index];
            if (from == body) {
                if (info.backedge != nullptr) {
                    return false;
                }
                info.backedge = value;
            } else {
                info.outside_indices.push_back(index);
            }
        }
        if (info.backedge == nullptr || info.outside_indices.empty()) {
            return false;
        }
        phis.push_back(std::move(info));
    }
    if (phis.empty()) {
        return false;
    }

    oir::PhiInst *countdown_phi = nullptr;
    bool countdown_is_lhs = false;
    if (auto *phi = dynamic_cast<oir::PhiInst *>(condition->lhs());
        phi != nullptr && phi->parent() == &header && is_int_value(condition->rhs(), 0)) {
        countdown_phi = phi;
        countdown_is_lhs = true;
    } else if (auto *phi = dynamic_cast<oir::PhiInst *>(condition->rhs());
               phi != nullptr && phi->parent() == &header && is_int_value(condition->lhs(), 0)) {
        countdown_phi = phi;
    }
    if (countdown_phi == nullptr) {
        return false;
    }
    if (!is_residual_i32_type(countdown_phi->type())) {
        return false;
    }
    auto countdown_it = std::find_if(phis.begin(), phis.end(), [&](const auto &phi) {
        return phi.phi == countdown_phi;
    });
    if (countdown_it == phis.end()) {
        return false;
    }
    auto trip_count = identical_outside_integer(*countdown_it);
    if (!trip_count || *trip_count <= 0) {
        return false;
    }
    auto condition_for = [&](std::int64_t countdown) {
        return countdown_is_lhs ? eval_cmp(condition->pred(), countdown, 0)
                                : eval_cmp(condition->pred(), 0, countdown);
    };
    const bool body_on_true = header_branch->true_bb() == body;
    if (condition_for(*trip_count) != body_on_true ||
        condition_for(1) != body_on_true || condition_for(0) == body_on_true) {
        return false;
    }
    auto *countdown_update = dynamic_cast<oir::BinaryInst *>(countdown_it->backedge);
    const bool subtracts_one =
        countdown_update != nullptr &&
        ((countdown_update->op() == oir::Instruction::OpID::Sub &&
          countdown_update->lhs() == countdown_phi && is_int_value(countdown_update->rhs(), 1)) ||
         (countdown_update->op() == oir::Instruction::OpID::Add &&
          ((countdown_update->lhs() == countdown_phi &&
            is_int_value(countdown_update->rhs(), -1)) ||
           (countdown_update->rhs() == countdown_phi &&
            is_int_value(countdown_update->lhs(), -1)))));
    if (!subtracts_one) {
        return false;
    }

    constexpr std::uint64_t kMaxFixedPointIterations = 64;
    struct Candidate {
        ResidualLoopPhi *state = nullptr;
        std::int64_t fixed_value = 0;
        std::uint64_t required_iterations = 0;
    };
    std::optional<Candidate> best;
    const auto simulation_limit = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(*trip_count), kMaxFixedPointIterations);
    for (auto &state : phis) {
        if (state.phi == countdown_phi || !is_residual_i32_type(state.phi->type())) {
            continue;
        }
        auto initial = identical_outside_integer(state);
        if (!initial) {
            continue;
        }
        std::int64_t current = *initial;
        std::optional<Candidate> candidate;
        for (std::uint64_t executed = 0; executed <= simulation_limit; ++executed) {
            auto environment = evaluate_residual_loop_body(*body, phis, *state.phi, current);
            auto next = residual_symbol_for(state.backedge, environment, *body);
            if (next.kind != ResidualSymbolKind::Constant) {
                break;
            }
            if (next.constant == current) {
                if (executed < static_cast<std::uint64_t>(*trip_count)) {
                    candidate = Candidate{&state, current, executed};
                }
                break;
            }
            if (executed == simulation_limit) {
                break;
            }
            current = next.constant;
        }
        if (!candidate) {
            continue;
        }

        auto environment = evaluate_residual_loop_body(
            *body, phis, *candidate->state->phi, candidate->fixed_value);
        bool observable_suffix_is_identity = true;
        for (const auto &phi : phis) {
            if (!value_has_use_outside_loop(*phi.phi, header, *body)) {
                continue;
            }
            auto actual = residual_symbol_for(phi.backedge, environment, *body);
            auto expected = phi.phi == candidate->state->phi
                                ? residual_constant(candidate->fixed_value)
                                : residual_identity(phi.phi);
            if (!same_residual_symbol(actual, expected)) {
                observable_suffix_is_identity = false;
                break;
            }
        }
        if (!observable_suffix_is_identity) {
            continue;
        }
        for (const auto &instruction : body->instructions()) {
            if (dynamic_cast<oir::BranchInst *>(instruction.get()) == nullptr &&
                value_has_use_outside_loop(*instruction, header, *body)) {
                observable_suffix_is_identity = false;
                break;
            }
        }
        if (!observable_suffix_is_identity) {
            continue;
        }
        if (!best || candidate->required_iterations < best->required_iterations) {
            best = candidate;
        }
    }
    if (!best) {
        return false;
    }

    auto *shortened = make_int_constant(module, countdown_phi->type(),
                                        static_cast<std::int64_t>(best->required_iterations));
    for (auto index : countdown_it->outside_indices) {
        countdown_phi->set_operand(index * 2, shortened);
    }
    std::uint64_t loop_instructions = header.instructions().size() + body->instructions().size();
    summary.loops = saturating_u64_add(summary.loops, 1);
    summary.before_iterations = saturating_u64_add(
        summary.before_iterations, static_cast<std::uint64_t>(*trip_count));
    summary.after_iterations =
        saturating_u64_add(summary.after_iterations, best->required_iterations);
    summary.eliminated_dynamic_instructions = saturating_u64_add(
        summary.eliminated_dynamic_instructions,
        saturating_u64_multiply(
            static_cast<std::uint64_t>(*trip_count) - best->required_iterations,
            loop_instructions));
    return true;
}

bool truncate_constant_fixed_point_suffixes(oir::Module &module, Stats &stats,
                                            FixedPointSuffixSummary *summary_out = nullptr) {
    FixedPointSuffixSummary summary;
    bool changed = false;
    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        for (auto &block : function->blocks()) {
            changed |= try_truncate_constant_fixed_point_suffix(module, *block, summary);
        }
    }
    stats.loop_bound_tighten += static_cast<unsigned>(summary.loops);
    if (summary_out != nullptr) {
        summary_out->loops = saturating_u64_add(summary_out->loops, summary.loops);
        summary_out->before_iterations = saturating_u64_add(
            summary_out->before_iterations, summary.before_iterations);
        summary_out->after_iterations = saturating_u64_add(
            summary_out->after_iterations, summary.after_iterations);
        summary_out->eliminated_dynamic_instructions = saturating_u64_add(
            summary_out->eliminated_dynamic_instructions,
            summary.eliminated_dynamic_instructions);
    }
    return changed;
}

void run_bounded_affected_cleanup(oir::Module &module, InlineContext &context, Stats &stats) {
    if (context.affected_functions.empty()) {
        return;
    }
    bool converged = false;
    for (unsigned round = 0; round < kMaxAffectedCleanupRounds; ++round) {
        bool changed = run_on_affected_functions(module, context.affected_functions, [&]() {
            bool local_changed = false;
            local_changed |= local_simplify(module, stats, SimplifyMode::ConstantFold);
            local_changed |= local_simplify(module, stats, SimplifyMode::Algebraic);
            local_changed |= run_sccp(module, stats);
            local_changed |= simplify_branches(module, stats);
            local_changed |= cleanup_cfg(module, stats);
            local_changed |= eliminate_dead_code(module, stats);
            local_changed |= aggressive_dead_code_elimination(module, stats);
            local_changed |= value_range_propagation(module, stats);
            local_changed |= local_simplify(module, stats, SimplifyMode::Algebraic);
            local_changed |= if_convert_conditional_adds(module, stats);
            local_changed |= truncate_constant_fixed_point_suffixes(module, stats);
            local_changed |= cleanup_cfg(module, stats);
            local_changed |= global_value_numbering(module, stats);
            local_changed |= scalar_replacement_of_aggregates(module, stats);
            local_changed |= promote_memory_to_registers(module, stats);
            return local_changed;
        });

        std::unordered_map<oir::Function *, std::string> before_global_promotion;
        for (const auto &function : module.functions()) {
            before_global_promotion.emplace(function.get(),
                                            function_body_fingerprint(*function));
        }
        const bool globals_changed = promote_global_loads(module, stats);
        changed |= globals_changed;
        if (globals_changed) {
            for (const auto &function : module.functions()) {
                auto found = before_global_promotion.find(function.get());
                if (found == before_global_promotion.end() ||
                    found->second != function_body_fingerprint(*function)) {
                    context.affected_functions.insert(function.get());
                }
            }
        }
        std::string verify_message;
        const char *forced_verify_failure =
            std::getenv("YOOLANG_TEST_FORCE_OIR_CLEANUP_VERIFY_FAILURE");
        if ((forced_verify_failure != nullptr &&
             std::strcmp(forced_verify_failure, "1") == 0) ||
            !module.verify(&verify_message)) {
            if (verify_message.empty() && forced_verify_failure != nullptr) {
                verify_message = "test-injected staged cleanup verifier failure";
            }
            throw std::runtime_error("OIR inline affected cleanup verification failed: " +
                                     verify_message);
        }
        retain_live_exposed_calls(module, context);
        if (!changed) {
            converged = true;
            break;
        }
    }
    context.cleanup_budget_exhausted = !converged;
    stats.call_cleanup_budget_exhausted |= context.cleanup_budget_exhausted;
    if (context.cleanup_budget_exhausted) {
        OIRTransformCostEstimate rejected;
        rejected.kind = pass::cost_model::TransformKind::Inline;
        rejected.pass_name = "OIRInlinePass";
        rejected.candidate_id =
            "cleanup." + std::to_string(++stats.cost_model_candidates);
        rejected.scope = "affected-cleanup";
        rejected.proof_summary =
            "affected cleanup reached its fixed-point round cap; dependent decisions stopped";
        rejected.forced_reject_reason =
            pass::cost_model::RejectReason::CleanupBudgetExhausted;
        (void)cost_model_allows_transform(stats, rejected);
    }
    context.affected_functions.clear();
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
                                    const pass::cost_model::CostModelPolicy &policy,
                                    bool contextual_large) {
    if (callee == nullptr || callee == &caller || callee->is_external() ||
        callee->entry_block() == nullptr || !has_compatible_call_shape(call, *callee)) {
        return false;
    }
    // Mutual recursion must not enter the ordinary-inline path: that path has no
    // SCC-depth template or recursive pressure accounting.  Direct self recursion
    // is handled separately by the bounded recursive legality checks.
    if (same_call_graph_scc(caller, *callee)) {
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
    if (info.blocks <= kMaxCalleeBlocks && within_inline_resource_limit(info, policy)) {
        return true;
    }
    if (!contextual_large || info.blocks > kMaxSpecializedInlineBlocks ||
        info.cost > static_cast<unsigned>(policy.max_inline_callee_cost * 3) ||
        info.returns == 0 || info.returns > kMaxSpecializedInlineReturns) {
        return false;
    }
    return std::none_of(callee->args().begin(), callee->args().end(),
                        [](const auto &argument) { return argument->type()->is_pointer(); });
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

oir::Value *map_value(oir::Value *value, const ValueMap &values, const BlockMap &blocks,
                      bool strict = false) {
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
    if (strict) {
        throw std::runtime_error("detached residual clone cannot map value " + value->print());
    }
    return value;
}

std::vector<oir::Value *> map_values(const std::vector<oir::Value *> &input,
                                     const ValueMap &values, const BlockMap &blocks,
                                     bool strict = false) {
    std::vector<oir::Value *> out;
    out.reserve(input.size());
    for (auto *value : input) {
        out.push_back(map_value(value, values, blocks, strict));
    }
    return out;
}

oir::Type *map_clone_type(oir::Type *type, const TypeMap *types) {
    if (types == nullptr) {
        return type;
    }
    auto found = types->find(type);
    if (found == types->end()) {
        throw std::runtime_error("detached residual clone cannot map type " + type->print());
    }
    return found->second;
}

std::unique_ptr<oir::Instruction> clone_non_phi_instruction(oir::Module &module,
                                                            const oir::Function &callee,
                                                            oir::Instruction &inst,
                                                            oir::BasicBlock *parent,
                                                            const ValueMap &values,
                                                            const BlockMap &blocks,
                                                            unsigned inline_index,
                                                            const TypeMap *types = nullptr) {
    const std::string name = inline_name(callee, inst, inline_index);
    const bool strict = types != nullptr;
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
            map_clone_type(inst.type(), types), inst.op(),
            map_value(binary.lhs(), values, blocks, strict),
            map_value(binary.rhs(), values, blocks, strict), parent, name);
    }
    case oir::Instruction::OpID::ICmp:
    case oir::Instruction::OpID::FCmp: {
        auto &cmp = static_cast<oir::CmpInst &>(inst);
        return std::make_unique<oir::CmpInst>(
            map_clone_type(inst.type(), types), inst.op(), cmp.pred(),
            map_value(cmp.lhs(), values, blocks, strict),
            map_value(cmp.rhs(), values, blocks, strict), parent, name);
    }
    case oir::Instruction::OpID::ZExt:
    case oir::Instruction::OpID::SIToFP:
    case oir::Instruction::OpID::FPToSI: {
        auto &cast = static_cast<oir::CastInst &>(inst);
        return std::make_unique<oir::CastInst>(map_clone_type(inst.type(), types), inst.op(),
                                               map_value(cast.src(), values, blocks, strict), parent,
                                               name);
    }
    case oir::Instruction::OpID::Alloca: {
        auto &alloca = static_cast<oir::AllocaInst &>(inst);
        return std::make_unique<oir::AllocaInst>(map_clone_type(inst.type(), types),
                                                 map_clone_type(alloca.allocated_type(), types),
                                                 parent, name);
    }
    case oir::Instruction::OpID::Load: {
        auto &load = static_cast<oir::LoadInst &>(inst);
        return std::make_unique<oir::LoadInst>(map_clone_type(inst.type(), types),
                                               map_value(load.ptr(), values, blocks, strict), parent,
                                               name);
    }
    case oir::Instruction::OpID::Store: {
        auto &store = static_cast<oir::StoreInst &>(inst);
        return std::make_unique<oir::StoreInst>(
            module.types().void_ty(), map_value(store.value(), values, blocks, strict),
            map_value(store.ptr(), values, blocks, strict), parent);
    }
    case oir::Instruction::OpID::MemZero: {
        auto &memzero = static_cast<oir::MemZeroInst &>(inst);
        return std::make_unique<oir::MemZeroInst>(
            module.types().void_ty(), map_value(memzero.ptr(), values, blocks, strict),
            map_value(memzero.byte_value(), values, blocks, strict),
            map_value(memzero.byte_count(), values, blocks, strict), parent);
    }
    case oir::Instruction::OpID::GetElementPtr: {
        auto &gep = static_cast<oir::GetElementPtrInst &>(inst);
        return std::make_unique<oir::GetElementPtrInst>(
            map_clone_type(inst.type(), types),
            map_value(gep.base_ptr(), values, blocks, strict),
            map_values(gep.indices(), values, blocks, strict), parent, name);
    }
    case oir::Instruction::OpID::Call: {
        auto &call = static_cast<oir::CallInst &>(inst);
        return std::make_unique<oir::CallInst>(
            map_clone_type(inst.type(), types),
            map_value(call.callee(), values, blocks, strict),
            map_values(call.args(), values, blocks, strict), parent, name);
    }
    case oir::Instruction::OpID::Phi:
    case oir::Instruction::OpID::Br:
    case oir::Instruction::OpID::Ret:
        break;
    }
    throw std::runtime_error("unsupported instruction while cloning inline body");
}

enum class ScratchResidualStatus {
    Success,
    Unsupported,
    CleanupBudgetExhausted,
    VerifyFailed,
};

enum class ReturnDemandMask {
    Dead,
    Scalar,
};

struct ScratchResidual {
    ScratchResidualStatus status = ScratchResidualStatus::Unsupported;
    std::unique_ptr<oir::Module> module;
    oir::Function *function = nullptr;
    TypeMap live_types;
    ValueMap live_values;
    unsigned cleanup_rounds = 0;
    unsigned new_constants = 0;
    FixedPointSuffixSummary fixed_point_suffix;
    std::string detail;
};

OIRTransformCostEstimate measured_residual_estimate(
    const oir::Function &callee, const oir::CallInst &call,
    const SpecializationMask &mask, ReturnDemandMask return_demand,
    const ScratchResidual &scratch, const CalleeInfo &original,
    const CalleeInfo &residual, bool recursive_layer, bool direct_residual,
    const pass::cost_model::CostModelPolicy &policy, const Stats &stats,
    std::string candidate_id) {
    const auto constant_arg_count = count_specializable_constants(mask);
    OIRTransformCostEstimate estimate;
    estimate.kind =
        pass::cost_model::TransformKind::ConstantArgumentSpecialization;
    estimate.pass_name = "OIRInlinePass";
    estimate.candidate_id = std::move(candidate_id);
    estimate.scope = direct_residual
                         ? (callsite_is_in_cycle(call) ? "loop-direct-residual"
                                                       : "direct-residual")
                         : "call";
    estimate.proof_kind = pass::cost_model::ProofKind::PartialEvaluation;
    estimate.proof_summary =
        direct_residual
            ? "verified detached residual with direct inline preflight"
            : (recursive_layer
                   ? "verified detached residual; bounded direct-recursive layer"
                   : (return_demand == ReturnDemandMask::Dead
                          ? "verified detached constant residual with dead-return slicing"
                          : "verified detached constant residual with demanded scalar return"));
    estimate.confidence =
        constant_arg_count == 0 ? 0.55 : (recursive_layer ? 0.70 : 0.74);
    fill_before_after_from_callee(estimate, original, 1, 1);
    estimate.dynamic_multiplier = callsite_is_in_cycle(call) ? 16 : 1;
    fill_after_from_residual(estimate, residual, direct_residual ? 0 : 1);
    estimate.after_code_bytes =
        estimate.before_code_bytes +
        (direct_residual ? static_cast<std::int64_t>(residual.static_instrs * 4)
                         : 0);

    const auto eliminated_instrs = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(original.static_instrs) -
               residual.static_instrs);
    const auto eliminated_branches = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(original.branches) - residual.branches);
    const auto eliminated_calls = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(original.calls) - residual.calls);
    estimate.risk.code_growth = direct_residual
                                    ? static_cast<std::int64_t>(
                                          std::min<std::uint64_t>(
                                              cloned_instruction_growth(residual),
                                              std::numeric_limits<std::int64_t>::max()))
                                    : 0;
    estimate.risk.register_pressure_growth =
        direct_residual
            ? static_cast<std::int64_t>((residual.loads + residual.stores) / 6)
            : 0;
    estimate.risk.live_range_growth = 0;
    estimate.risk.memory_pressure_growth = 0;
    estimate.risk.cleanup_dependency = 0;
    estimate.partial_eval.cloned_functions = 0;
    estimate.partial_eval.cloned_blocks = residual.blocks;
    estimate.partial_eval.residual_instrs = estimate.after_instrs;
    estimate.partial_eval.eliminated_instrs = eliminated_instrs;
    estimate.partial_eval.eliminated_branches = eliminated_branches;
    estimate.partial_eval.eliminated_calls = eliminated_calls;
    estimate.partial_eval.new_constants = scratch.new_constants;
    estimate.partial_eval.required_cleanup_rounds = scratch.cleanup_rounds;

    const auto caller_multiplier = static_cast<std::uint64_t>(
        std::max<std::int64_t>(1, estimate.dynamic_multiplier));
    const auto eliminated_phis =
        original.phis > residual.phis ? original.phis - residual.phis : 0;
    const auto statically_accounted = saturating_u64_add(
        static_cast<std::uint64_t>(eliminated_instrs),
        saturating_u64_add(static_cast<std::uint64_t>(eliminated_branches),
                           eliminated_phis));
    const auto proved_dynamic =
        scratch.fixed_point_suffix.eliminated_dynamic_instructions;
    const auto dynamic_savings =
        proved_dynamic > statically_accounted ? proved_dynamic - statically_accounted : 0;
    const auto max_savings =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    estimate.proven_dynamic_instruction_savings = static_cast<std::int64_t>(
        dynamic_savings == 0 || caller_multiplier <= max_savings / dynamic_savings
            ? dynamic_savings * caller_multiplier
            : max_savings);

    const auto committed_growth = cloned_instruction_growth(residual);
    const auto growth_fit = call_growth_budget_fit(
        stats, CallGrowthClass::Specialization,
        callee.root_function_id(), committed_growth);
    const auto committed_pressure =
        estimate_call_pressure(residual, call, &mask);
    estimate.risk.live_range_growth = static_cast<std::int64_t>(
        committed_pressure.max_live_values / 8);
    estimate.risk.memory_pressure_growth = static_cast<std::int64_t>(
        committed_pressure.memory_pressure / 4);
    estimate.risk.register_pressure_growth = static_cast<std::int64_t>(
        committed_pressure.spill_proxy);
    const auto pressure_reject = pressure_reject_reason(
        stats.cumulative_call_pressure, committed_pressure,
        stats.call_pressure_budget);
    if (!growth_fit.fits()) {
        estimate.forced_reject_reason =
            pass::cost_model::RejectReason::CumulativeBudgetExhausted;
        append_call_growth_budget_summary(
            estimate.proof_summary, CallGrowthClass::Specialization,
            growth_fit);
    }
    if (pressure_reject != pass::cost_model::RejectReason::None) {
        estimate.forced_reject_reason = pressure_reject;
        estimate.proof_summary += "; cumulative call pressure budget exhausted";
    }
    const auto specialization_count = stats.call_root_specializations.find(
        callee.root_function_id());
    const auto committed_specializations =
        specialization_count == stats.call_root_specializations.end()
            ? 0U
            : specialization_count->second;
    if (committed_specializations >= static_cast<unsigned>(
            std::max<std::int64_t>(0,
                                   policy.max_specializations_per_function))) {
        estimate.forced_reject_reason =
            pass::cost_model::RejectReason::CodeGrowthTooHigh;
        estimate.proof_summary += "; specialization budget exceeded";
    }
    return estimate;
}

bool is_safely_discardable_scratch_value(const oir::Instruction &instruction) {
    // Residual PE has a stricter dead-return contract than the general OIR DCE:
    // an unused result does not prove that a memory access or a trapping integer
    // operation is unobservable.  Keep those instructions (and every call) unless
    // another cleanup pass independently proves them away.
    switch (instruction.op()) {
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::Mul:
    case oir::Instruction::OpID::And:
    case oir::Instruction::OpID::Xor:
    case oir::Instruction::OpID::FAdd:
    case oir::Instruction::OpID::FSub:
    case oir::Instruction::OpID::FMul:
    case oir::Instruction::OpID::FDiv:
    case oir::Instruction::OpID::ICmp:
    case oir::Instruction::OpID::FCmp:
    case oir::Instruction::OpID::Alloca:
    case oir::Instruction::OpID::GetElementPtr:
    case oir::Instruction::OpID::ZExt:
    case oir::Instruction::OpID::SIToFP:
    case oir::Instruction::OpID::FPToSI:
    case oir::Instruction::OpID::Phi:
        return true;
    case oir::Instruction::OpID::SDiv:
    case oir::Instruction::OpID::SRem:
    case oir::Instruction::OpID::Load:
    case oir::Instruction::OpID::Call:
    case oir::Instruction::OpID::Ret:
    case oir::Instruction::OpID::Br:
    case oir::Instruction::OpID::Store:
    case oir::Instruction::OpID::MemZero:
        return false;
    }
    return false;
}

bool eliminate_safe_dead_scratch_values(oir::Module &module, Stats &stats) {
    bool changed = false;
    bool removed = true;
    while (removed) {
        removed = false;
        for (auto &function : module.functions()) {
            if (function->is_external()) {
                continue;
            }
            for (auto &block : function->blocks()) {
                for (auto it = block->instructions().begin();
                     it != block->instructions().end();) {
                    if (!(*it)->has_uses() &&
                        is_safely_discardable_scratch_value(**it)) {
                        (*it)->drop_all_operands();
                        it = block->instructions().erase(it);
                        ++stats.dce;
                        changed = true;
                        removed = true;
                        continue;
                    }
                    ++it;
                }
            }
        }
    }
    return changed;
}

ScratchResidual build_detached_constant_residual(oir::Module &live, oir::Function &callee,
                                                 const oir::CallInst &call,
                                                 const SpecializationMask &mask,
                                                 ReturnDemandMask return_demand) {
    ScratchResidual result;
    if (consume_residual_test_failure("unsupported")) {
        result.status = ScratchResidualStatus::Unsupported;
        result.detail =
            "test failpoint: unsupported detached instruction; live snapshot including function table unchanged";
        return result;
    }
    result.module = std::make_unique<oir::Module>("oir.detached.residual");
    auto &scratch = *result.module;
    TypeMap types;
    ValueMap values;
    BlockMap blocks;

    std::function<oir::Type *(oir::Type *)> map_type = [&](oir::Type *type) -> oir::Type * {
        if (type == nullptr) {
            return nullptr;
        }
        if (auto found = types.find(type); found != types.end()) {
            return found->second;
        }
        oir::Type *mapped = nullptr;
        switch (type->id()) {
        case oir::Type::TypeID::Void:
            mapped = scratch.types().void_ty();
            break;
        case oir::Type::TypeID::Label:
            mapped = scratch.types().label_ty();
            break;
        case oir::Type::TypeID::Integer: {
            auto width = static_cast<oir::IntegerType *>(type)->bit_width();
            mapped = width == 1 ? static_cast<oir::Type *>(scratch.types().int1_ty())
                                : static_cast<oir::Type *>(scratch.types().int32_ty());
            if (width != 1 && width != 32) {
                return nullptr;
            }
            break;
        }
        case oir::Type::TypeID::Float:
            mapped = scratch.types().float_ty();
            break;
        case oir::Type::TypeID::Pointer: {
            auto *element = map_type(static_cast<oir::PointerType *>(type)->element_type());
            mapped = element == nullptr ? nullptr : scratch.types().ptr_ty(element);
            break;
        }
        case oir::Type::TypeID::Array: {
            auto *array = static_cast<oir::ArrayType *>(type);
            auto *element = map_type(array->element_type());
            mapped = element == nullptr
                         ? nullptr
                         : scratch.types().array_ty(element, array->element_count());
            break;
        }
        case oir::Type::TypeID::Function: {
            auto *function = static_cast<oir::FunctionType *>(type);
            auto *return_type = map_type(function->return_type());
            std::vector<oir::Type *> params;
            for (auto *param : function->param_types()) {
                params.push_back(map_type(param));
            }
            if (return_type == nullptr ||
                std::any_of(params.begin(), params.end(), [](auto *param) {
                    return param == nullptr;
                })) {
                return nullptr;
            }
            mapped = scratch.types().func_ty(return_type, params);
            break;
        }
        }
        if (mapped != nullptr) {
            types.emplace(type, mapped);
            result.live_types.emplace(mapped, type);
        }
        return mapped;
    };

    auto map_constant = [&](oir::Value *value) -> oir::Value * {
        if (value == nullptr) {
            return nullptr;
        }
        if (auto found = values.find(value); found != values.end()) {
            return found->second;
        }
        auto *mapped_type = map_type(value->type());
        oir::Value *mapped = nullptr;
        if (auto *integer = dynamic_cast<oir::ConstantInt *>(value)) {
            mapped = mapped_type == scratch.types().int1_ty()
                         ? static_cast<oir::Value *>(scratch.create_i1(integer->value() != 0))
                         : static_cast<oir::Value *>(scratch.create_i32(integer->value()));
        } else if (auto *floating = dynamic_cast<oir::ConstantFloat *>(value)) {
            mapped = scratch.create_f32(floating->value());
        } else if (dynamic_cast<oir::ConstantZero *>(value) != nullptr) {
            mapped = scratch.create_zero(mapped_type);
        } else if (dynamic_cast<oir::UndefValue *>(value) != nullptr) {
            mapped = scratch.create_undef(mapped_type);
        }
        if (mapped != nullptr) {
            values.emplace(value, mapped);
            result.live_values.emplace(mapped, value);
        }
        return mapped;
    };

    for (const auto &constant : live.owned_constants()) {
        if (map_constant(constant.get()) == nullptr) {
            result.detail = "unsupported detached constant";
            return result;
        }
    }
    for (const auto &global : live.globals()) {
        auto *value_type = map_type(global->value_type());
        auto *initializer = map_constant(global->init_value());
        if (value_type == nullptr || (global->init_value() != nullptr && initializer == nullptr)) {
            result.detail = "unsupported detached global";
            return result;
        }
        auto *shadow = scratch.create_global(global->name(), value_type, global->is_const(),
                                             initializer);
        shadow->set_initializer_literal(global->initializer_literal());
        values.emplace(global.get(), shadow);
        result.live_values.emplace(shadow, global.get());
    }
    unsigned shadow_index = 0;
    for (const auto &function : live.functions()) {
        auto *function_type = dynamic_cast<oir::FunctionType *>(map_type(function->function_type()));
        if (function_type == nullptr) {
            result.detail = "unsupported detached function type";
            return result;
        }
        auto *shadow = scratch.create_function("__scratch.shadow." +
                                                   std::to_string(shadow_index++),
                                               function_type, true);
        values.emplace(function.get(), shadow);
        result.live_values.emplace(shadow, function.get());
    }

    auto call_args = call.args();
    if (call_args.size() != callee.args().size() || mask.size() != call_args.size()) {
        result.detail = "incompatible detached call shape";
        return result;
    }
    std::vector<oir::Type *> residual_params;
    for (std::size_t i = 0; i < callee.args().size(); ++i) {
        if (!mask_selects_argument(mask, i)) {
            residual_params.push_back(map_type(callee.args()[i]->type()));
        }
    }
    auto *residual_return_type =
        return_demand == ReturnDemandMask::Dead ? static_cast<oir::Type *>(scratch.types().void_ty())
                                                : map_type(callee.return_type());
    auto *residual_type = scratch.types().func_ty(residual_return_type, residual_params);
    auto *residual = scratch.create_function(
        "__scratch.residual", residual_type, false,
        oir::FunctionOrigin::ResidualSpecialization, callee.root_function_id());
    result.function = residual;

    std::size_t next_arg = 0;
    for (std::size_t i = 0; i < callee.args().size(); ++i) {
        if (mask_selects_argument(mask, i)) {
            auto *constant = map_constant(call_args[i]);
            if (constant == nullptr) {
                result.detail = "selected argument is not an exact typed constant";
                return result;
            }
            values.emplace(callee.args()[i].get(), constant);
            ++result.new_constants;
        } else {
            auto *argument = residual->args()[next_arg++].get();
            argument->set_name(callee.args()[i]->name());
            values.emplace(callee.args()[i].get(), argument);
        }
    }

    for (const auto &block : callee.blocks()) {
        blocks.emplace(block.get(), residual->create_block(block->name()));
        map_type(block->type());
        for (const auto &instruction : block->instructions()) {
            map_type(instruction->type());
            for (auto *operand : instruction->operands()) {
                map_type(operand->type());
            }
            if (auto *alloca = dynamic_cast<oir::AllocaInst *>(instruction.get())) {
                map_type(alloca->allocated_type());
            }
        }
    }
    for (const auto &block : callee.blocks()) {
        auto *out_block = blocks.at(block.get());
        for (const auto &instruction : block->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(instruction.get());
            if (phi == nullptr) {
                break;
            }
            auto out_phi = std::make_unique<oir::PhiInst>(map_clone_type(phi->type(), &types),
                                                          out_block, phi->name());
            values.emplace(phi, out_phi.get());
            out_block->append_instruction(std::move(out_phi));
        }
    }
    try {
        for (const auto &block : callee.blocks()) {
            auto *out_block = blocks.at(block.get());
            for (const auto &instruction_ptr : block->instructions()) {
                auto *instruction = instruction_ptr.get();
                if (instruction->op() == oir::Instruction::OpID::Phi) {
                    continue;
                }
                if (auto *ret = dynamic_cast<oir::ReturnInst *>(instruction)) {
                    out_block->append_instruction(std::make_unique<oir::ReturnInst>(
                        scratch.types().void_ty(),
                        ret->has_value() && return_demand == ReturnDemandMask::Scalar
                            ? map_value(ret->value(), values, blocks, true)
                            : nullptr,
                        out_block));
                    continue;
                }
                if (auto *branch = dynamic_cast<oir::BranchInst *>(instruction)) {
                    if (branch->is_conditional()) {
                        auto *true_block = blocks.at(branch->true_bb());
                        auto *false_block = blocks.at(branch->false_bb());
                        out_block->append_instruction(std::make_unique<oir::BranchInst>(
                            scratch.types().void_ty(),
                            map_value(branch->cond(), values, blocks, true), true_block, false_block,
                            out_block));
                        out_block->add_successor(true_block);
                        out_block->add_successor(false_block);
                        true_block->add_predecessor(out_block);
                        false_block->add_predecessor(out_block);
                    } else {
                        auto *target = blocks.at(branch->target_bb());
                        out_block->append_instruction(std::make_unique<oir::BranchInst>(
                            scratch.types().void_ty(), target, out_block));
                        out_block->add_successor(target);
                        target->add_predecessor(out_block);
                    }
                    continue;
                }
                auto cloned = clone_non_phi_instruction(scratch, callee, *instruction, out_block,
                                                        values, blocks, 0, &types);
                values.emplace(instruction, cloned.get());
                out_block->append_instruction(std::move(cloned));
            }
        }
        for (const auto &block : callee.blocks()) {
            auto *out_block = blocks.at(block.get());
            auto out_it = out_block->instructions().begin();
            for (const auto &instruction : block->instructions()) {
                auto *phi = dynamic_cast<oir::PhiInst *>(instruction.get());
                if (phi == nullptr) {
                    break;
                }
                auto *out_phi = static_cast<oir::PhiInst *>(out_it->get());
                for (const auto &[value, from] : phi->incoming()) {
                    out_phi->add_incoming(map_value(value, values, blocks, true), blocks.at(from));
                }
                ++out_it;
            }
        }
    } catch (const std::exception &error) {
        result.detail = error.what();
        return result;
    }

    if (consume_residual_test_failure("verify")) {
        result.status = ScratchResidualStatus::VerifyFailed;
        result.detail =
            "test failpoint: detached verifier failure; live snapshot including function table unchanged";
        return result;
    }
    std::string verify_message;
    if (!scratch.verify(&verify_message)) {
        result.status = ScratchResidualStatus::VerifyFailed;
        result.detail = verify_message;
        return result;
    }
    Stats scratch_stats;
    bool converged = false;
    for (unsigned round = 0; round < kMaxScratchCleanupRounds; ++round) {
        bool changed = false;
        changed |= local_simplify(scratch, scratch_stats, SimplifyMode::ConstantFold);
        changed |= local_simplify(scratch, scratch_stats, SimplifyMode::Algebraic);
        changed |= run_sccp(scratch, scratch_stats);
        changed |= simplify_branches(scratch, scratch_stats);
        changed |= cleanup_cfg(scratch, scratch_stats);
        changed |= eliminate_safe_dead_scratch_values(scratch, scratch_stats);
        changed |= value_range_propagation(scratch, scratch_stats);
        changed |= if_convert_conditional_adds(scratch, scratch_stats);
        changed |= truncate_constant_fixed_point_suffixes(scratch, scratch_stats,
                                                          &result.fixed_point_suffix);
        changed |= global_value_numbering(scratch, scratch_stats);
        changed |= scalar_replacement_of_aggregates(scratch, scratch_stats);
        changed |= promote_memory_to_registers(scratch, scratch_stats);
        result.cleanup_rounds = round + 1;
        if (!scratch.verify(&verify_message)) {
            result.status = ScratchResidualStatus::VerifyFailed;
            result.detail = verify_message;
            return result;
        }
        if (!changed) {
            converged = true;
            break;
        }
    }
    if (consume_residual_test_failure("cleanup-budget")) {
        result.status = ScratchResidualStatus::CleanupBudgetExhausted;
        result.detail =
            "test failpoint: detached cleanup budget exhausted; live snapshot including function table unchanged";
        return result;
    }
    if (!converged) {
        result.status = ScratchResidualStatus::CleanupBudgetExhausted;
        result.detail = "detached cleanup did not reach a fixed point";
        return result;
    }
    result.new_constants = 0;
    for (const auto &constant : scratch.owned_constants()) {
        if (result.live_values.find(constant.get()) == result.live_values.end()) {
            ++result.new_constants;
        }
    }
    result.status = ScratchResidualStatus::Success;
    return result;
}

ScratchResidual build_charged_detached_constant_residual(
    oir::Module &live, oir::Function &callee, const oir::CallInst &call,
    const SpecializationMask &mask, ReturnDemandMask return_demand,
    const CalleeInfo &original, Stats &stats,
    const std::string &proof_rule_id) {
    const auto reserved_work =
        specialization_scratch_reservation(original);
    if (!require_specialization_work_capacity(
            stats, 1, reserved_work, proof_rule_id,
            "detached residual scratch build would exceed the persistent specialization work budget")) {
        ScratchResidual rejected;
        rejected.detail = "specialization work budget exhausted before scratch build";
        return rejected;
    }
    consume_specialization_work(stats, 1, reserved_work);
    auto result = build_detached_constant_residual(
        live, callee, call, mask, return_demand);
    const auto actual_work = saturating_u64_multiply(
        specialization_scratch_base(original),
        1 + static_cast<std::uint64_t>(result.cleanup_rounds));
    const auto refund = reserved_work - std::min(reserved_work, actual_work);
    stats.call_specialization_work_units -=
        std::min(stats.call_specialization_work_units, refund);
    return result;
}

std::unique_ptr<oir::Function> clone_function_template(
    oir::Function &source, ValueMap *source_to_clone = nullptr) {
    auto out = std::make_unique<oir::Function>(source.function_type(), source.name(),
                                               source.parent(), source.is_external(),
                                               source.function_id(), source.origin(),
                                               source.root_function_id());
    ValueMap values;
    BlockMap blocks;

    for (const auto &arg : source.args()) {
        values[arg.get()] = out->add_argument(arg->type(), arg->name());
    }
    for (const auto &block : source.blocks()) {
        auto *cloned_block = out->create_block();
        cloned_block->set_name(block->name());
        blocks[block.get()] = cloned_block;
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
            // This is a transaction snapshot, not an inline expansion.  Keep
            // observable instruction identities stable instead of accumulating
            // the inline clone prefix on every staged transaction.
            cloned->set_name(inst->name());
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

    // Block storage order is not a dominance/topological guarantee.  A value
    // defined in a later stored block may therefore have been used as the
    // temporary fallback while its clone did not yet exist.  Close every such
    // edge now that the complete source->clone map is available; otherwise a
    // published staged body could retain an operand owned by the retired body.
    for (auto &block : out->blocks()) {
        for (auto &instruction : block->instructions()) {
            for (std::size_t operand_index = 0;
                 operand_index < instruction->operand_count(); ++operand_index) {
                auto found = values.find(instruction->operand(operand_index));
                if (found != values.end()) {
                    instruction->set_operand(operand_index, found->second);
                }
            }
        }
    }

    out->set_block_allocator_state(source.block_allocator_state());
    if (source_to_clone != nullptr) {
        source_to_clone->reserve(source_to_clone->size() + values.size() +
                                 blocks.size() + 1);
        source_to_clone->insert(values.begin(), values.end());
        for (const auto &[source_block, cloned_block] : blocks) {
            source_to_clone->emplace(source_block, cloned_block);
        }
        source_to_clone->emplace(&source, out.get());
    }

    return out;
}

struct StagedInlineModule {
    oir::ModuleFunctionSet originals;
    ValueMap original_to_staged_values;
    std::unordered_map<oir::Function *, oir::Function *>
        original_to_staged_functions;
    std::unordered_map<oir::Function *, oir::Function *>
        staged_to_original_functions;
    std::size_t original_constant_count = 0;
    oir::FunctionID original_function_allocator = oir::kInvalidFunctionID;
};

void destroy_function_set(oir::ModuleFunctionSet &set) noexcept {
    set.drop_all_references();
    set.function_table.clear();
    set.functions.clear();
}

void install_staged_inline_module(oir::Module &module,
                                  StagedInlineModule &state) {
    state.original_constant_count = module.owned_constants().size();
    state.original_function_allocator = module.function_allocator_state();

    oir::ModuleFunctionSet staged;
    staged.functions.reserve(module.functions().size());
    state.original_to_staged_functions.reserve(module.functions().size());
    state.staged_to_original_functions.reserve(module.functions().size());
    for (auto &function : module.functions()) {
        auto clone = clone_function_template(
            *function, &state.original_to_staged_values);
        auto *staged_function = clone.get();
        state.original_to_staged_functions.emplace(function.get(),
                                                    staged_function);
        state.staged_to_original_functions.emplace(staged_function,
                                                    function.get());
        staged.functions.push_back(std::move(clone));
    }

    // clone_function_template deliberately leaves module-level Function operands
    // pointing at the source graph.  Retarget only after every clone exists so
    // direct recursion and mutual SCC edges become a closed staged call graph.
    for (auto &function : staged.functions) {
        for (auto &block : function->blocks()) {
            for (auto &instruction : block->instructions()) {
                for (std::size_t operand_index = 0;
                     operand_index < instruction->operand_count();
                     ++operand_index) {
                    auto *source_function = dynamic_cast<oir::Function *>(
                        instruction->operand(operand_index));
                    auto found =
                        state.original_to_staged_functions.find(source_function);
                    if (found != state.original_to_staged_functions.end()) {
                        instruction->set_operand(operand_index, found->second);
                    }
                }
            }
        }
    }
    module.prepare_function_set(staged);
    module.exchange_function_set(staged);
    state.originals = std::move(staged);
}

bool restore_original_inline_module(oir::Module &module,
                                    StagedInlineModule &state) noexcept {
    module.exchange_function_set(state.originals);
    module.set_function_allocator_state(state.original_function_allocator);
    destroy_function_set(state.originals);
    const bool constants_restored =
        module.discard_constants_from(state.original_constant_count);
    std::string verify_message;
    return constants_restored && module.verify(&verify_message);
}

void remap_recursive_inline_stats_to_staged(
    Stats &stats, const StagedInlineModule &state) {
    std::unordered_set<const oir::Function *> mapped;
    mapped.reserve(stats.recursively_inlined_functions.size());
    for (auto *function : stats.recursively_inlined_functions) {
        auto found = state.original_to_staged_functions.find(
            const_cast<oir::Function *>(function));
        mapped.insert(found == state.original_to_staged_functions.end()
                          ? function
                          : found->second);
    }
    stats.recursively_inlined_functions = std::move(mapped);
}

void remap_recursive_inline_stats_to_published(
    Stats &stats, const StagedInlineModule &state) {
    std::unordered_set<const oir::Function *> mapped;
    mapped.reserve(stats.recursively_inlined_functions.size());
    for (auto *function : stats.recursively_inlined_functions) {
        auto found = state.staged_to_original_functions.find(
            const_cast<oir::Function *>(function));
        mapped.insert(found == state.staged_to_original_functions.end()
                          ? function
                          : found->second);
    }
    stats.recursively_inlined_functions = std::move(mapped);
}

void verify_staged_body_ownership(const oir::Module &module,
                                  const StagedInlineModule &state) {
    std::unordered_set<const oir::Value *> retired_body_values;
    for (const auto &function : state.originals.functions) {
        if (function == nullptr) {
            continue;
        }
        for (const auto &argument : function->args()) {
            retired_body_values.insert(argument.get());
        }
        for (const auto &block : function->blocks()) {
            retired_body_values.insert(block.get());
            for (const auto &instruction : block->instructions()) {
                retired_body_values.insert(instruction.get());
            }
        }
    }
    for (const auto &function : module.functions()) {
        for (const auto &block : function->blocks()) {
            for (const auto &instruction : block->instructions()) {
                for (auto *operand : instruction->operands()) {
                    if (retired_body_values.find(operand) !=
                        retired_body_values.end()) {
                        throw std::runtime_error(
                            "staged OIR inline body references a retired source-body value");
                    }
                }
            }
        }
    }
}

bool publish_staged_inline_module(oir::Module &module,
                                  StagedInlineModule &state) {
    const std::size_t existing_count =
        state.staged_to_original_functions.size();
    const std::size_t published_count = module.functions().size();

    std::unordered_map<oir::Function *, std::size_t> original_indices;
    original_indices.reserve(state.originals.functions.size());
    for (std::size_t index = 0; index < state.originals.functions.size();
         ++index) {
        original_indices.emplace(state.originals.functions[index].get(), index);
    }

    // Preflight every use-list growth before the no-throw body publication.
    std::unordered_map<oir::Function *, std::size_t> added_uses;
    added_uses.reserve(existing_count);
    for (const auto &function : module.functions()) {
        for (const auto &block : function->blocks()) {
            for (const auto &instruction : block->instructions()) {
                for (std::size_t operand_index = 0;
                     operand_index < instruction->operand_count();
                     ++operand_index) {
                    auto *staged_function = dynamic_cast<oir::Function *>(
                        instruction->operand(operand_index));
                    auto found =
                        state.staged_to_original_functions.find(staged_function);
                    if (found != state.staged_to_original_functions.end()) {
                        ++added_uses[found->second];
                    }
                }
            }
        }
    }
    for (const auto &[function, count] : added_uses) {
        function->reserve_additional_uses(count);
    }
    for (auto &function : module.functions()) {
        for (auto &block : function->blocks()) {
            for (auto &instruction : block->instructions()) {
                for (std::size_t operand_index = 0;
                     operand_index < instruction->operand_count();
                     ++operand_index) {
                    auto *staged_function = dynamic_cast<oir::Function *>(
                        instruction->operand(operand_index));
                    auto found =
                        state.staged_to_original_functions.find(staged_function);
                    if (found != state.staged_to_original_functions.end()) {
                        instruction->set_operand(operand_index, found->second);
                    }
                }
            }
        }
    }

    oir::ModuleFunctionSet published;
    oir::ModuleFunctionSet retired;
    published.functions.reserve(published_count);
    published.function_table.reserve(published_count);
    retired.functions.reserve(existing_count);
    for (const auto &staged_owner : module.functions()) {
        auto *staged_function = staged_owner.get();
        auto found = state.staged_to_original_functions.find(staged_function);
        auto *published_function =
            found == state.staged_to_original_functions.end()
                ? staged_function
                : found->second;
        if (!published.function_table
                 .emplace(published_function->name(), published_function)
                 .second) {
            throw std::runtime_error(
                "duplicate function while publishing staged OIR inline transaction");
        }
    }
    oir::ModuleFunctionSet transformed;
    module.exchange_function_set(transformed);
    for (auto &staged_owner : transformed.functions) {
        auto *staged_function = staged_owner.get();
        auto found = state.staged_to_original_functions.find(staged_function);
        if (found == state.staged_to_original_functions.end()) {
            published.functions.push_back(std::move(staged_owner));
            continue;
        }
        auto *original_function = found->second;
        const auto original_index = original_indices.at(original_function);
        auto original_owner =
            std::move(state.originals.functions[original_index]);
        original_function->swap_body(*staged_function);
        published.functions.push_back(std::move(original_owner));
        retired.functions.push_back(std::move(staged_owner));
    }
    module.exchange_function_set(published);

    std::string verify_message;
    if (module.verify(&verify_message)) {
        destroy_function_set(retired);
        transformed.function_table.clear();
        state.originals.function_table.clear();
        state.originals.functions.clear();
        return true;
    }

    // The old bodies are still owned by retired, so even a publication verifier
    // failure can restore every original Function/Argument/Block/Instruction.
    oir::ModuleFunctionSet failed_published;
    module.exchange_function_set(failed_published);
    for (const auto &[staged_function, original_function] :
         state.staged_to_original_functions) {
        original_function->swap_body(*staged_function);
    }
    for (auto &owner : failed_published.functions) {
        auto found = original_indices.find(owner.get());
        if (found != original_indices.end()) {
            state.originals.functions[found->second] = std::move(owner);
        }
    }
    module.exchange_function_set(state.originals);
    module.set_function_allocator_state(state.original_function_allocator);
    retired.drop_all_references();
    failed_published.drop_all_references();
    retired.function_table.clear();
    failed_published.function_table.clear();
    retired.functions.clear();
    failed_published.functions.clear();
    transformed.function_table.clear();
    const bool constants_restored =
        module.discard_constants_from(state.original_constant_count);
    std::string restored_verify_message;
    if (!constants_restored || !module.verify(&restored_verify_message)) {
        throw std::runtime_error(
            "failed to restore OIR inline transaction after publication verification: " +
            verify_message);
    }
    return false;
}

void record_inline_transaction_rejection(Stats &stats,
                                         const std::string &scope,
                                         const std::string &detail) {
    OIRTransformCostEstimate rejected;
    rejected.kind = pass::cost_model::TransformKind::Inline;
    rejected.pass_name = "OIRInlinePass";
    rejected.candidate_id = "inline.transaction.rollback";
    rejected.scope = scope;
    rejected.proof_kind = pass::cost_model::ProofKind::Structural;
    rejected.proof_status = pass::cost_model::ProofStatus::Proven;
    rejected.proof_summary = detail;
    rejected.confidence = 1.0;
    rejected.forced_reject_reason =
        pass::cost_model::RejectReason::CommitPreflightFailed;
    (void)cost_model_allows_transform(stats, rejected);
}

void merge_call_specialization_work_ledger(Stats &live,
                                           const Stats &staged) noexcept {
    if (!staged.call_specialization_work_initialized) {
        return;
    }
    if (!live.call_specialization_work_initialized) {
        live.call_specialization_attempt_budget =
            staged.call_specialization_attempt_budget;
        live.call_specialization_work_budget =
            staged.call_specialization_work_budget;
    }
    live.call_specialization_work_initialized = true;
    live.call_specialization_work_exhausted |=
        staged.call_specialization_work_exhausted;
    live.call_specialization_attempts = std::max(
        live.call_specialization_attempts,
        staged.call_specialization_attempts);
    live.call_specialization_work_units = std::max(
        live.call_specialization_work_units,
        staged.call_specialization_work_units);
}

template <typename Transform>
bool run_transactional_inline_transform(oir::Module &module, Stats &stats,
                                        const std::string &scope,
                                        Transform &&transform) {
    static_assert(std::is_nothrow_move_assignable_v<Stats>);
    static_assert(std::is_nothrow_move_assignable_v<
                  pass::cost_model::CostModelReport>);

    auto *live_report = stats.cost_model_report;
    pass::cost_model::CostModelReport staged_report;
    Stats staged_stats = stats;
    if (live_report != nullptr) {
        staged_report = *live_report;
        staged_stats.cost_model_report = &staged_report;
    }

    StagedInlineModule state;
    bool installed = false;
    try {
        install_staged_inline_module(module, state);
        installed = true;
        remap_recursive_inline_stats_to_staged(staged_stats, state);
        const bool changed = transform(module, staged_stats);
        std::string verify_message;
        if (!module.verify(&verify_message)) {
            throw std::runtime_error(
                "OIR inline staged transaction verification failed: " +
                verify_message);
        }
        verify_staged_body_ownership(module, state);
        remap_recursive_inline_stats_to_published(staged_stats, state);

        if (!changed &&
            module.functions().size() ==
                state.staged_to_original_functions.size()) {
            if (!restore_original_inline_module(module, state)) {
                throw std::runtime_error(
                    "failed to restore unchanged staged OIR inline transaction");
            }
        } else if (!publish_staged_inline_module(module, state)) {
            merge_call_specialization_work_ledger(stats, staged_stats);
            record_inline_transaction_rejection(
                stats, scope,
                "live publication verification failed; complete inline snapshot restored");
            return false;
        }
        if (live_report != nullptr) {
            *live_report = std::move(staged_report);
        }
        staged_stats.cost_model_report = live_report;
        stats = std::move(staged_stats);
        return changed;
    } catch (const std::exception &error) {
        merge_call_specialization_work_ledger(stats, staged_stats);
        if (installed && !state.originals.functions.empty()) {
            if (!restore_original_inline_module(module, state)) {
                throw;
            }
        }
        record_inline_transaction_rejection(stats, scope, error.what());
        return false;
    } catch (...) {
        merge_call_specialization_work_ledger(stats, staged_stats);
        if (installed && !state.originals.functions.empty()) {
            if (!restore_original_inline_module(module, state)) {
                throw;
            }
        }
        record_inline_transaction_rejection(
            stats, scope,
            "unknown staged inline transaction failure; complete snapshot restored");
        return false;
    }
}

bool is_constprop_specialization(const oir::Function &function) {
    return function.origin() == oir::FunctionOrigin::ResidualSpecialization;
}

unsigned existing_specialization_count(const oir::Module &module, const oir::Function &callee) {
    unsigned count = 0;
    for (const auto &function : module.functions()) {
        if (is_constprop_specialization(*function) &&
            function->root_function_id() == callee.root_function_id()) {
            ++count;
        }
    }
    return count;
}

bool is_specializable_constant(oir::Value *value) {
    return int_constant(value).has_value() || float_constant(value).has_value();
}

std::string constant_key(oir::Value *value) {
    if (dynamic_cast<oir::ConstantZero *>(value) != nullptr) {
        if (auto *integer = dynamic_cast<oir::IntegerType *>(value->type())) {
            return "i" + std::to_string(integer->bit_width()) + ":0";
        }
        if (value->type()->is_float()) {
            return "f32:00000000";
        }
        return "zero:" + value->type()->print();
    }
    if (auto *constant = dynamic_cast<oir::ConstantInt *>(value)) {
        auto *integer = dynamic_cast<oir::IntegerType *>(value->type());
        return "i" + std::to_string(integer == nullptr ? 0 : integer->bit_width()) + ":" +
               std::to_string(constant->value());
    }
    if (auto *constant = dynamic_cast<oir::ConstantFloat *>(value)) {
        std::uint32_t bits = 0;
        const float exact = constant->value();
        static_assert(sizeof(bits) == sizeof(exact));
        std::memcpy(&bits, &exact, sizeof(bits));
        std::ostringstream oss;
        oss << "f32:" << std::hex << std::setw(8) << std::setfill('0') << bits;
        return oss.str();
    }
    return "*";
}

std::string specialization_key(const oir::CallInst &call, const SpecializationMask &mask) {
    return typed_argument_bindings(call.args(), &mask);
}

bool same_specialization_binding(const oir::CallInst &lhs,
                                 const SpecializationMask &lhs_mask,
                                 const oir::CallInst &rhs,
                                 const SpecializationMask &rhs_mask) {
    if (lhs_mask != rhs_mask || lhs.args().size() != rhs.args().size() ||
        lhs.args().size() != lhs_mask.size()) {
        return false;
    }
    const auto lhs_args = lhs.args();
    const auto rhs_args = rhs.args();
    for (std::size_t index = 0; index < lhs_mask.size(); ++index) {
        if (mask_selects_argument(lhs_mask, index) &&
            constant_key(lhs_args[index]) != constant_key(rhs_args[index])) {
            return false;
        }
    }
    return true;
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

    auto *clone = module.create_function(
        next_specialization_name(module, callee, next_id),
        module.types().func_ty(callee.return_type(), param_types), false,
        oir::FunctionOrigin::ResidualSpecialization, callee.root_function_id());

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

bool is_materializable_detached_constant(oir::Value *value, const TypeMap &live_types) {
    if (value == nullptr || live_types.find(value->type()) == live_types.end()) {
        return false;
    }
    if (auto *integer = dynamic_cast<oir::ConstantInt *>(value)) {
        if (is_residual_i1_type(value->type())) {
            return integer->value() == 0 || integer->value() == 1;
        }
        return is_residual_i32_type(value->type());
    }
    return dynamic_cast<oir::ConstantFloat *>(value) != nullptr ||
           dynamic_cast<oir::ConstantZero *>(value) != nullptr ||
           dynamic_cast<oir::UndefValue *>(value) != nullptr;
}

std::unique_ptr<oir::Value> make_detached_live_constant(oir::Value *value,
                                                        const TypeMap &live_types) {
    auto type_found = live_types.find(value->type());
    if (type_found == live_types.end()) {
        return nullptr;
    }
    auto *live_type = type_found->second;
    if (auto *integer = dynamic_cast<oir::ConstantInt *>(value)) {
        if (is_residual_i1_type(value->type())) {
            return integer->value() == 0 || integer->value() == 1
                       ? std::make_unique<oir::ConstantInt>(live_type, integer->value())
                       : nullptr;
        }
        return is_residual_i32_type(value->type())
                   ? std::make_unique<oir::ConstantInt>(live_type, integer->value())
                   : nullptr;
    }
    if (auto *floating = dynamic_cast<oir::ConstantFloat *>(value)) {
        return std::make_unique<oir::ConstantFloat>(live_type, floating->value());
    }
    if (dynamic_cast<oir::ConstantZero *>(value) != nullptr) {
        return std::make_unique<oir::ConstantZero>(live_type);
    }
    if (dynamic_cast<oir::UndefValue *>(value) != nullptr) {
        return std::make_unique<oir::UndefValue>(live_type);
    }
    return nullptr;
}

bool detached_residual_commit_is_supported(const ScratchResidual &scratch,
                                           const oir::CallInst &call,
                                           const SpecializationMask &mask,
                                           ReturnDemandMask return_demand) {
    auto *residual = scratch.function;
    if (scratch.status != ScratchResidualStatus::Success || scratch.module == nullptr ||
        residual == nullptr || residual->entry_block() == nullptr ||
        call.args().size() != mask.size()) {
        return false;
    }
    if (!residual->entry_block()->instructions().empty() &&
        dynamic_cast<oir::PhiInst *>(
            residual->entry_block()->instructions().front().get()) != nullptr) {
        // The direct inline adds a new caller predecessor.  Entry phis would need a
        // separately proved incoming value for that edge, so reject this shape in preflight.
        return false;
    }
    if (return_demand == ReturnDemandMask::Dead) {
        if (!call.uses().empty() || !residual->return_type()->is_void()) {
            return false;
        }
    } else {
        auto type_found = scratch.live_types.find(residual->return_type());
        if (type_found == scratch.live_types.end() || type_found->second != call.type()) {
            return false;
        }
    }

    const auto unselected = static_cast<std::size_t>(
        std::count(mask.begin(), mask.end(), false));
    if (residual->args().size() != unselected) {
        return false;
    }
    auto call_args = call.args();
    std::size_t residual_arg = 0;
    for (std::size_t index = 0; index < mask.size(); ++index) {
        if (mask_selects_argument(mask, index)) {
            continue;
        }
        auto type_found = scratch.live_types.find(residual->args()[residual_arg]->type());
        if (type_found == scratch.live_types.end() ||
            type_found->second != call_args[index]->type()) {
            return false;
        }
        ++residual_arg;
    }
    for (const auto &constant : scratch.module->owned_constants()) {
        if (scratch.live_values.find(constant.get()) == scratch.live_values.end() &&
            !is_materializable_detached_constant(constant.get(), scratch.live_types)) {
            return false;
        }
    }

    std::unordered_set<const oir::BasicBlock *> blocks;
    std::unordered_set<const oir::Instruction *> instructions;
    for (const auto &block : residual->blocks()) {
        blocks.insert(block.get());
        for (const auto &instruction : block->instructions()) {
            instructions.insert(instruction.get());
            if (scratch.live_types.find(instruction->type()) == scratch.live_types.end()) {
                return false;
            }
            if (auto *alloca = dynamic_cast<oir::AllocaInst *>(instruction.get());
                alloca != nullptr &&
                scratch.live_types.find(alloca->allocated_type()) == scratch.live_types.end()) {
                return false;
            }
        }
    }
    for (const auto &argument : residual->args()) {
        if (scratch.live_types.find(argument->type()) == scratch.live_types.end()) {
            return false;
        }
    }
    for (const auto &block : residual->blocks()) {
        for (const auto &instruction : block->instructions()) {
            for (auto *operand : instruction->operands()) {
                auto live_value = scratch.live_values.find(operand);
                if (live_value != scratch.live_values.end()) {
                    auto live_type = scratch.live_types.find(operand->type());
                    if (live_type == scratch.live_types.end() || live_value->second == nullptr ||
                        live_value->second->type() != live_type->second) {
                        return false;
                    }
                    continue;
                }
                if (is_materializable_detached_constant(operand, scratch.live_types)) {
                    continue;
                }
                if (auto *argument = dynamic_cast<oir::Argument *>(operand);
                    argument != nullptr && argument->parent() == residual) {
                    continue;
                }
                if (auto *operand_block = dynamic_cast<oir::BasicBlock *>(operand);
                    operand_block != nullptr && blocks.find(operand_block) != blocks.end()) {
                    continue;
                }
                if (auto *operand_instruction = dynamic_cast<oir::Instruction *>(operand);
                    operand_instruction != nullptr &&
                    instructions.find(operand_instruction) != instructions.end()) {
                    continue;
                }
                return false;
            }
        }
    }
    return true;
}

struct PersistentResidualClone {
    oir::Function *function = nullptr;
    std::size_t original_constant_count = 0;
};

std::optional<PersistentResidualClone> import_persistent_residual_clone(
    oir::Module &module, const ScratchResidual &scratch, const oir::CallInst &call,
    const SpecializationMask &mask, const oir::Function &root) {
    if (!detached_residual_commit_is_supported(
            scratch, call, mask, ReturnDemandMask::Scalar)) {
        return std::nullopt;
    }

    auto *residual = scratch.function;
    std::vector<oir::Type *> parameter_types;
    parameter_types.reserve(residual->args().size());
    for (const auto &argument : residual->args()) {
        auto type = scratch.live_types.find(argument->type());
        if (type == scratch.live_types.end()) {
            return std::nullopt;
        }
        parameter_types.push_back(type->second);
    }
    auto return_type = scratch.live_types.find(residual->return_type());
    if (return_type == scratch.live_types.end()) {
        return std::nullopt;
    }

    unsigned display_id = 0;
    const auto name = next_specialization_name(module, root, display_id);
    const auto original_constant_count = module.owned_constants().size();
    oir::Function *clone = nullptr;
    std::vector<std::unique_ptr<oir::Value>> staged_constants;
    auto rollback = [&]() {
        if (clone != nullptr && !module.erase_function(clone)) {
            throw std::runtime_error(
                "failed to erase unreferenced persistent residual clone during rollback");
        }
        clone = nullptr;
        if (!module.discard_constants_from(original_constant_count)) {
            throw std::runtime_error(
                "failed to discard unreferenced persistent residual constants during rollback");
        }
    };

    try {
        clone = module.create_function(
            name, module.types().func_ty(return_type->second, parameter_types), false,
            oir::FunctionOrigin::ResidualSpecialization,
            root.root_function_id());
        ValueMap values = scratch.live_values;
        BlockMap blocks;
        for (const auto &constant : scratch.module->owned_constants()) {
            if (values.find(constant.get()) != values.end()) {
                continue;
            }
            auto staged = make_detached_live_constant(constant.get(), scratch.live_types);
            if (staged == nullptr) {
                rollback();
                return std::nullopt;
            }
            values.emplace(constant.get(), staged.get());
            staged_constants.push_back(std::move(staged));
        }
        for (std::size_t index = 0; index < residual->args().size(); ++index) {
            clone->args()[index]->set_name(residual->args()[index]->name());
            values.emplace(residual->args()[index].get(), clone->args()[index].get());
        }

        auto ordered_blocks = clone_order(*residual);
        for (auto *source_block : ordered_blocks) {
            blocks.emplace(source_block,
                           clone->create_block("residual." + source_block->name()));
        }
        for (auto *source_block : ordered_blocks) {
            auto *out_block = blocks.at(source_block);
            for (const auto &instruction : source_block->instructions()) {
                auto *phi = dynamic_cast<oir::PhiInst *>(instruction.get());
                if (phi == nullptr) {
                    break;
                }
                auto out_phi = std::make_unique<oir::PhiInst>(
                    map_clone_type(phi->type(), &scratch.live_types), out_block,
                    phi->name());
                values.emplace(phi, out_phi.get());
                out_block->append_instruction(std::move(out_phi));
            }
        }
        for (auto *source_block : ordered_blocks) {
            auto *out_block = blocks.at(source_block);
            for (const auto &instruction_ptr : source_block->instructions()) {
                auto *instruction = instruction_ptr.get();
                if (instruction->op() == oir::Instruction::OpID::Phi) {
                    continue;
                }
                if (auto *ret = dynamic_cast<oir::ReturnInst *>(instruction)) {
                    out_block->append_instruction(std::make_unique<oir::ReturnInst>(
                        module.types().void_ty(),
                        ret->has_value()
                            ? map_value(ret->value(), values, blocks, true)
                            : nullptr,
                        out_block));
                    continue;
                }
                if (auto *branch = dynamic_cast<oir::BranchInst *>(instruction)) {
                    if (branch->is_conditional()) {
                        oir::cfg::append_conditional_branch(
                            module, out_block,
                            map_value(branch->cond(), values, blocks, true),
                            blocks.at(branch->true_bb()), blocks.at(branch->false_bb()));
                    } else {
                        oir::cfg::append_unconditional_branch(
                            module, out_block, blocks.at(branch->target_bb()));
                    }
                    continue;
                }
                auto cloned = clone_non_phi_instruction(
                    module, *residual, *instruction, out_block, values, blocks, 0,
                    &scratch.live_types);
                values.emplace(instruction, cloned.get());
                out_block->append_instruction(std::move(cloned));
            }
        }
        for (auto *source_block : ordered_blocks) {
            auto *out_block = blocks.at(source_block);
            auto out_it = out_block->instructions().begin();
            for (const auto &instruction : source_block->instructions()) {
                auto *phi = dynamic_cast<oir::PhiInst *>(instruction.get());
                if (phi == nullptr) {
                    break;
                }
                auto *out_phi = static_cast<oir::PhiInst *>(out_it->get());
                for (const auto &[value, from] : phi->incoming()) {
                    out_phi->add_incoming(map_value(value, values, blocks, true),
                                          blocks.at(from));
                }
                ++out_it;
            }
        }
        module.adopt_constants(staged_constants);
        std::string verify_message;
        if (!module.verify(&verify_message)) {
            rollback();
            return std::nullopt;
        }
        return PersistentResidualClone{clone, original_constant_count};
    } catch (const std::bad_alloc &) {
        rollback();
        throw;
    } catch (...) {
        rollback();
        return std::nullopt;
    }
}

bool retarget_calls_to_persistent_residual(
    oir::Module &module, PersistentResidualClone &persistent,
    const std::vector<oir::CallInst *> &calls, const SpecializationMask &mask) {
    struct Snapshot {
        oir::CallInst *call = nullptr;
        oir::Value *callee = nullptr;
        std::vector<oir::Value *> args;
        std::vector<oir::Value *> residual_args;
    };
    std::vector<Snapshot> snapshots;
    snapshots.reserve(calls.size());
    auto rollback_clone = [&]() {
        if (persistent.function != nullptr &&
            !module.erase_function(persistent.function)) {
            throw std::runtime_error(
                "failed to erase persistent residual clone after retarget rollback");
        }
        if (!module.discard_constants_from(
                persistent.original_constant_count)) {
            throw std::runtime_error(
                "failed to discard persistent residual constants after retarget rollback");
        }
        persistent.function = nullptr;
    };
    try {
        std::unordered_map<oir::Value *, std::size_t> additional_uses;
        for (auto *call : calls) {
            if (call == nullptr || call->args().size() != mask.size() ||
                call->type() != persistent.function->return_type()) {
                rollback_clone();
                return false;
            }
            Snapshot snapshot{call, call->callee(), call->args(), {}};
            snapshot.residual_args.reserve(persistent.function->args().size());
            for (std::size_t index = 0; index < snapshot.args.size(); ++index) {
                if (!mask_selects_argument(mask, index)) {
                    snapshot.residual_args.push_back(snapshot.args[index]);
                }
            }
            if (snapshot.residual_args.size() != persistent.function->args().size()) {
                rollback_clone();
                return false;
            }
            ++additional_uses[persistent.function];
            for (auto *argument : snapshot.residual_args) {
                ++additional_uses[argument];
            }
            snapshots.push_back(std::move(snapshot));
        }
        for (const auto &[value, additional] : additional_uses) {
            value->reserve_additional_uses(additional);
        }
    } catch (...) {
        rollback_clone();
        return false;
    }

    bool verified = false;
    try {
        for (auto &snapshot : snapshots) {
            snapshot.call->reset_callee_and_args(persistent.function,
                                                 snapshot.residual_args);
        }
        std::string verify_message;
        verified = module.verify(&verify_message);
    } catch (...) {
        verified = false;
    }
    if (verified) {
        return true;
    }
    for (auto &snapshot : snapshots) {
        try {
            snapshot.call->reset_callee_and_args(snapshot.callee, snapshot.args);
        } catch (...) {
            // All operand/use capacity was reserved before live mutation.  This is
            // defensive only; restoration is expected to be allocation-free.
            rollback_clone();
            return false;
        }
    }
    rollback_clone();
    return false;
}

struct DetachedInlinePlan {
    // Constants outlive the staged blocks during rollback; declaration order matters.
    std::vector<std::unique_ptr<oir::Value>> constants;
    std::list<std::unique_ptr<oir::BasicBlock>> blocks;
    std::list<std::unique_ptr<oir::Instruction>> entry_branch;
    oir::BasicBlock *entry = nullptr;
    oir::BasicBlock *continuation = nullptr;
    oir::Value *replacement = nullptr;
    oir::Instruction *entry_branch_instruction = nullptr;
    std::vector<oir::Value::Use> call_uses;
    std::vector<oir::CallInst *> exposed_calls;

    DetachedInlinePlan() = default;
    DetachedInlinePlan(const DetachedInlinePlan &) = delete;
    DetachedInlinePlan &operator=(const DetachedInlinePlan &) = delete;
    DetachedInlinePlan(DetachedInlinePlan &&) noexcept = default;
    DetachedInlinePlan &operator=(DetachedInlinePlan &&) noexcept = delete;
    void drop_staged_references() {
        for (auto &instruction : entry_branch) {
            instruction->drop_all_operands();
        }
        for (auto &block : blocks) {
            for (auto &instruction : block->instructions()) {
                instruction->drop_all_operands();
            }
        }
    }
    ~DetachedInlinePlan() { drop_staged_references(); }
};

std::optional<DetachedInlinePlan> build_detached_inline_plan(
    oir::Module &module, oir::Function &caller, const oir::CallInst &call,
    const ScratchResidual &scratch, const SpecializationMask &mask,
    ReturnDemandMask return_demand, unsigned inline_index) {
    if (consume_residual_test_failure("commit-preflight")) {
        return std::nullopt;
    }
    if (!detached_residual_commit_is_supported(scratch, call, mask, return_demand)) {
        return std::nullopt;
    }

    try {
        DetachedInlinePlan plan;
        auto *residual = scratch.function;
        ValueMap values = scratch.live_values;
        BlockMap blocks;

        for (const auto &constant : scratch.module->owned_constants()) {
            if (values.find(constant.get()) != values.end()) {
                continue;
            }
            auto staged = make_detached_live_constant(constant.get(), scratch.live_types);
            if (staged == nullptr) {
                return std::nullopt;
            }
            values.emplace(constant.get(), staged.get());
            plan.constants.push_back(std::move(staged));
        }

        auto call_args = call.args();
        std::size_t residual_arg = 0;
        for (std::size_t index = 0; index < mask.size(); ++index) {
            if (!mask_selects_argument(mask, index)) {
                values.emplace(residual->args()[residual_arg++].get(), call_args[index]);
            }
        }

        const std::string prefix = "inl.residual." + std::to_string(inline_index) + ".";
        auto ordered_blocks = clone_order(*residual);
        for (auto *source_block : ordered_blocks) {
            auto staged = std::make_unique<oir::BasicBlock>(
                module.types().label_ty(), prefix + source_block->name(), &caller);
            blocks.emplace(source_block, staged.get());
            plan.blocks.push_back(std::move(staged));
        }
        auto continuation = std::make_unique<oir::BasicBlock>(
            module.types().label_ty(), prefix + "cont", &caller);
        plan.continuation = continuation.get();
        plan.blocks.push_back(std::move(continuation));
        plan.entry = blocks.at(residual->entry_block());

        for (auto *source_block : ordered_blocks) {
            auto *out_block = blocks.at(source_block);
            for (const auto &instruction : source_block->instructions()) {
                auto *phi = dynamic_cast<oir::PhiInst *>(instruction.get());
                if (phi == nullptr) {
                    break;
                }
                auto out_phi = std::make_unique<oir::PhiInst>(
                    map_clone_type(phi->type(), &scratch.live_types), out_block,
                    inline_name(*residual, *phi, inline_index));
                values.emplace(phi, out_phi.get());
                out_block->append_instruction(std::move(out_phi));
            }
        }

        std::vector<std::pair<oir::BasicBlock *, oir::Value *>> returns;
        for (auto *source_block : ordered_blocks) {
            auto *out_block = blocks.at(source_block);
            for (const auto &instruction_ptr : source_block->instructions()) {
                auto *instruction = instruction_ptr.get();
                if (instruction->op() == oir::Instruction::OpID::Phi) {
                    continue;
                }
                if (auto *ret = dynamic_cast<oir::ReturnInst *>(instruction)) {
                    returns.push_back(
                        {out_block, ret->has_value()
                                        ? map_value(ret->value(), values, blocks, true)
                                        : nullptr});
                    oir::cfg::append_unconditional_branch(module, out_block,
                                                          plan.continuation);
                    continue;
                }
                if (auto *branch = dynamic_cast<oir::BranchInst *>(instruction)) {
                    if (branch->is_conditional()) {
                        oir::cfg::append_conditional_branch(
                            module, out_block,
                            map_value(branch->cond(), values, blocks, true),
                            blocks.at(branch->true_bb()), blocks.at(branch->false_bb()));
                    } else {
                        oir::cfg::append_unconditional_branch(
                            module, out_block, blocks.at(branch->target_bb()));
                    }
                    continue;
                }
                auto cloned = clone_non_phi_instruction(
                    module, *residual, *instruction, out_block, values, blocks, inline_index,
                    &scratch.live_types);
                values.emplace(instruction, cloned.get());
                out_block->append_instruction(std::move(cloned));
            }
        }
        for (auto *source_block : ordered_blocks) {
            auto *out_block = blocks.at(source_block);
            auto out_it = out_block->instructions().begin();
            for (const auto &instruction : source_block->instructions()) {
                auto *phi = dynamic_cast<oir::PhiInst *>(instruction.get());
                if (phi == nullptr) {
                    break;
                }
                auto *out_phi = static_cast<oir::PhiInst *>(out_it->get());
                for (const auto &[value, from] : phi->incoming()) {
                    out_phi->add_incoming(map_value(value, values, blocks, true),
                                          blocks.at(from));
                }
                ++out_it;
            }
        }

        if (!call.uses().empty()) {
            if (returns.empty()) {
                return std::nullopt;
            }
            if (returns.size() == 1) {
                plan.replacement = returns.front().second;
            } else {
                auto phi = std::make_unique<oir::PhiInst>(
                    call.type(), plan.continuation,
                    call.name().empty() ? "inl.ret" : call.name());
                plan.replacement = phi.get();
                for (const auto &[return_block, value] : returns) {
                    phi->add_incoming(value, return_block);
                }
                plan.continuation->instructions().push_front(std::move(phi));
            }
            if (plan.replacement == nullptr || plan.replacement->type() != call.type()) {
                return std::nullopt;
            }
        }

        for (const auto &[source_block, cloned_block] : blocks) {
            (void)source_block;
            for (const auto &instruction : cloned_block->instructions()) {
                if (auto *cloned_call = dynamic_cast<oir::CallInst *>(instruction.get())) {
                    plan.exposed_calls.push_back(cloned_call);
                }
            }
        }
        plan.call_uses = call.uses();
        auto entry_branch = std::make_unique<oir::BranchInst>(
            module.types().void_ty(), plan.entry,
            const_cast<oir::BasicBlock *>(call.parent()));
        plan.entry_branch_instruction = entry_branch.get();
        plan.entry_branch.push_back(std::move(entry_branch));
        return plan;
    } catch (const std::bad_alloc &) {
        throw;
    } catch (...) {
        return std::nullopt;
    }
}

struct DetachedInlineRequest {
    oir::Function *caller = nullptr;
    oir::BasicBlock *block = nullptr;
    oir::CallInst *call = nullptr;
    const ScratchResidual *scratch = nullptr;
    const SpecializationMask *mask = nullptr;
    ReturnDemandMask return_demand = ReturnDemandMask::Scalar;
    unsigned inline_index = 0;
};

struct AppliedDetachedInline {
    DetachedInlineRequest request;
    std::unique_ptr<DetachedInlinePlan> plan;
    std::list<std::unique_ptr<oir::Instruction>>::iterator call_it;
    std::vector<oir::BasicBlock *> original_successors;
    std::vector<oir::Function *> affected_scc;
    oir::Instruction *first_tail = nullptr;
    oir::BasicBlock *first_staged_block = nullptr;
    std::size_t original_constant_count = 0;
    unsigned inherited_exposure_depth = 0;
    bool live_applied = false;
};

void rollback_detached_inline(oir::Module &module, AppliedDetachedInline &applied) {
    auto &request = applied.request;
    auto &plan = *applied.plan;
    for (const auto &use : plan.call_uses) {
        if (use.user != nullptr && use.operand_index < use.user->operand_count() &&
            use.user->operand(use.operand_index) == plan.replacement) {
            use.user->set_operand(use.operand_index, request.call);
        }
    }
    auto entry_branch = std::find_if(
        request.block->instructions().begin(), request.block->instructions().end(),
        [&](const auto &instruction) {
            return instruction.get() == plan.entry_branch_instruction;
        });
    if (entry_branch != request.block->instructions().end()) {
        oir::cfg::remove_edge_no_phi_update(request.block, plan.entry);
        plan.entry_branch.splice(plan.entry_branch.end(),
                                 request.block->instructions(), entry_branch);
    }
    for (auto *successor : applied.original_successors) {
        oir::cfg::move_successor_edge(plan.continuation, request.block, successor);
    }
    if (applied.first_tail != nullptr) {
        auto restore_begin = std::find_if(
            plan.continuation->instructions().begin(),
            plan.continuation->instructions().end(),
            [&](const auto &instruction) {
                return instruction.get() == applied.first_tail;
            });
        if (restore_begin != plan.continuation->instructions().end()) {
            auto restore_at = std::next(applied.call_it);
            request.block->instructions().splice(
                restore_at, plan.continuation->instructions(), restore_begin,
                plan.continuation->instructions().end());
            for (auto it = std::next(applied.call_it);
                 it != request.block->instructions().end(); ++it) {
                (*it)->set_parent(request.block);
            }
        }
    }
    auto staged_begin = std::find_if(
        request.caller->blocks().begin(), request.caller->blocks().end(),
        [&](const auto &candidate) {
            return candidate.get() == applied.first_staged_block;
        });
    if (staged_begin != request.caller->blocks().end()) {
        plan.blocks.splice(plan.blocks.end(), request.caller->blocks(), staged_begin,
                           request.caller->blocks().end());
    }
    plan.drop_staged_references();
    if (!module.discard_constants_from(applied.original_constant_count)) {
        throw std::runtime_error(
            "failed to discard detached inline constants during rollback");
    }
    applied.live_applied = false;
}

struct InlineLiveModuleSnapshot {
    struct UseEntry {
        const oir::User *user = nullptr;
        std::size_t operand_index = 0;
    };
    std::string printed;
    oir::FunctionID function_allocator = oir::kInvalidFunctionID;
    std::size_t constant_count = 0;
    std::unordered_map<std::string, oir::Function *> function_table;
    std::vector<const oir::Value *> objects;
    std::vector<std::vector<UseEntry>> uses;
    std::vector<std::size_t> block_allocators;
};

InlineLiveModuleSnapshot capture_inline_live_module_snapshot(
    const oir::Module &module) {
    InlineLiveModuleSnapshot snapshot;
    snapshot.printed = module.print();
    snapshot.function_allocator = module.function_allocator_state();
    snapshot.constant_count = module.owned_constants().size();
    snapshot.function_table = module.function_table_mappings();
    auto append_value = [&](const oir::Value *value) {
        snapshot.objects.push_back(value);
        std::vector<InlineLiveModuleSnapshot::UseEntry> value_uses;
        value_uses.reserve(value->uses().size());
        for (const auto &use : value->uses()) {
            value_uses.push_back({use.user, use.operand_index});
        }
        snapshot.uses.push_back(std::move(value_uses));
    };
    for (const auto &constant : module.owned_constants()) {
        append_value(constant.get());
    }
    for (const auto &global : module.globals()) {
        append_value(global.get());
    }
    for (const auto &function : module.functions()) {
        append_value(function.get());
        snapshot.block_allocators.push_back(function->block_allocator_state());
        for (const auto &argument : function->args()) {
            append_value(argument.get());
        }
        for (const auto &block : function->blocks()) {
            append_value(block.get());
            for (const auto &instruction : block->instructions()) {
                append_value(instruction.get());
            }
        }
    }
    return snapshot;
}

bool inline_live_module_snapshot_matches(
    const oir::Module &module, const InlineLiveModuleSnapshot &snapshot) {
    std::string verify_message;
    if (!module.verify(&verify_message) || module.print() != snapshot.printed ||
        module.function_allocator_state() != snapshot.function_allocator ||
        module.owned_constants().size() != snapshot.constant_count ||
        module.function_table_mappings() != snapshot.function_table) {
        return false;
    }
    const auto current = capture_inline_live_module_snapshot(module);
    if (current.objects != snapshot.objects ||
        current.block_allocators != snapshot.block_allocators ||
        current.uses.size() != snapshot.uses.size()) {
        return false;
    }
    for (std::size_t value_index = 0; value_index < current.uses.size();
         ++value_index) {
        const auto &lhs = current.uses[value_index];
        const auto &rhs = snapshot.uses[value_index];
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (std::size_t use_index = 0; use_index < lhs.size(); ++use_index) {
            if (lhs[use_index].user != rhs[use_index].user ||
                lhs[use_index].operand_index != rhs[use_index].operand_index) {
                return false;
            }
        }
    }
    return true;
}

struct InlineCommitStatsSnapshot {
    unsigned inlined = 0;
    unsigned specialized = 0;
    bool cleanup_budget_exhausted = false;
    std::uint64_t cumulative_growth = 0;
    std::unordered_map<oir::FunctionID, std::uint64_t> root_growth;
    std::uint64_t specialization_growth = 0;
    std::unordered_map<oir::FunctionID, std::uint64_t>
        specialization_root_growth;
    std::uint64_t ordinary_growth = 0;
    std::unordered_map<oir::FunctionID, std::uint64_t> ordinary_root_growth;
    std::uint64_t recursive_growth = 0;
    std::unordered_map<oir::FunctionID, std::uint64_t>
        recursive_root_growth;
    std::unordered_map<oir::FunctionID, unsigned> root_specializations;
    CallPressureVector pressure;
};

InlineCommitStatsSnapshot capture_inline_commit_stats(const Stats &stats) {
    return {stats.inlined,
            stats.specialized,
            stats.call_cleanup_budget_exhausted,
            stats.cumulative_call_growth,
            stats.call_root_growth,
            stats.cumulative_call_specialization_growth,
            stats.call_specialization_root_growth,
            stats.cumulative_call_ordinary_growth,
            stats.call_ordinary_root_growth,
            stats.cumulative_call_recursive_growth,
            stats.call_recursive_root_growth,
            stats.call_root_specializations,
            stats.cumulative_call_pressure};
}

bool call_pressure_equal(const CallPressureVector &lhs,
                         const CallPressureVector &rhs) {
    return lhs.live_pointers == rhs.live_pointers &&
           lhs.alias_uncertainty == rhs.alias_uncertainty &&
           lhs.loads == rhs.loads && lhs.stores == rhs.stores &&
           lhs.max_live_values == rhs.max_live_values &&
           lhs.memory_pressure == rhs.memory_pressure &&
           lhs.register_pressure == rhs.register_pressure &&
           lhs.spill_proxy == rhs.spill_proxy;
}

bool inline_commit_stats_match(const Stats &stats,
                               const InlineCommitStatsSnapshot &snapshot) {
    return stats.inlined == snapshot.inlined &&
           stats.specialized == snapshot.specialized &&
           stats.call_cleanup_budget_exhausted ==
               snapshot.cleanup_budget_exhausted &&
           stats.cumulative_call_growth == snapshot.cumulative_growth &&
           stats.call_root_growth == snapshot.root_growth &&
           stats.cumulative_call_specialization_growth ==
               snapshot.specialization_growth &&
           stats.call_specialization_root_growth ==
               snapshot.specialization_root_growth &&
           stats.cumulative_call_ordinary_growth ==
               snapshot.ordinary_growth &&
           stats.call_ordinary_root_growth ==
               snapshot.ordinary_root_growth &&
           stats.cumulative_call_recursive_growth ==
               snapshot.recursive_growth &&
           stats.call_recursive_root_growth ==
               snapshot.recursive_root_growth &&
           stats.call_root_specializations == snapshot.root_specializations &&
           call_pressure_equal(stats.cumulative_call_pressure,
                               snapshot.pressure);
}

bool inline_detached_residual_batch(oir::Module &module, InlineContext &context,
                                    const std::vector<DetachedInlineRequest> &requests,
                                    Stats *specialization_work_stats = nullptr) {
    if (requests.empty()) {
        return false;
    }
    std::vector<AppliedDetachedInline> applied;
    applied.reserve(requests.size());
    const bool test_commit_rollback =
        residual_test_failure_is("commit-after-first");
    std::optional<InlineLiveModuleSnapshot> test_snapshot;
    if (test_commit_rollback) {
        test_snapshot = capture_inline_live_module_snapshot(module);
    }
    bool injected_commit_failure = false;
    try {
        // Split later calls first within one block.  This keeps every not-yet-applied
        // call in its original block and also makes a dependent earlier call see the
        // staged uses introduced for a later call before its replacement snapshot is
        // captured.  Block groups retain their input order; only members of the same
        // block are reversed by their original program position.
        std::vector<oir::BasicBlock *> block_order;
        std::unordered_map<oir::BasicBlock *,
                           std::vector<const DetachedInlineRequest *>> block_requests;
        std::unordered_set<oir::CallInst *> unique_calls;
        block_order.reserve(requests.size());
        block_requests.reserve(requests.size());
        unique_calls.reserve(requests.size());
        for (const auto &request : requests) {
            if (request.caller == nullptr || request.block == nullptr ||
                request.call == nullptr || request.scratch == nullptr ||
                request.mask == nullptr || request.call->parent() != request.block ||
                !unique_calls.insert(request.call).second) {
                return false;
            }
            auto [group, inserted] = block_requests.emplace(
                request.block,
                std::vector<const DetachedInlineRequest *>{});
            if (inserted) {
                block_order.push_back(request.block);
            }
            group->second.push_back(&request);
        }

        std::vector<const DetachedInlineRequest *> ordered_requests;
        ordered_requests.reserve(requests.size());
        for (auto *block : block_order) {
            std::unordered_map<const oir::CallInst *, std::size_t> positions;
            positions.reserve(block_requests.at(block).size());
            std::size_t position = 0;
            for (const auto &instruction : block->instructions()) {
                if (auto *call = dynamic_cast<oir::CallInst *>(instruction.get())) {
                    positions.emplace(call, position);
                }
                ++position;
            }
            auto &group = block_requests.at(block);
            for (const auto *request : group) {
                if (positions.find(request->call) == positions.end()) {
                    return false;
                }
            }
            std::sort(group.begin(), group.end(), [&](const auto *lhs, const auto *rhs) {
                return positions.at(lhs->call) > positions.at(rhs->call);
            });
            ordered_requests.insert(ordered_requests.end(), group.begin(), group.end());
        }

        // Build every plan and all rollback metadata before the first live mutation.
        for (const auto *request_ptr : ordered_requests) {
            const auto &request = *request_ptr;
            auto call_it = std::find_if(
                request.block->instructions().begin(),
                request.block->instructions().end(), [&](const auto &instruction) {
                    return instruction.get() == request.call;
                });
            if (call_it == request.block->instructions().end()) {
                return false;
            }
            if (specialization_work_stats != nullptr &&
                !charge_specialization_plan_work(
                    *specialization_work_stats,
                    inspect_callee(*request.scratch->function),
                    "specialization.detached-inline-plan",
                    "detached inline plan would exceed the persistent specialization work budget")) {
                return false;
            }
            auto plan = build_detached_inline_plan(
                module, *request.caller, *request.call, *request.scratch,
                *request.mask, request.return_demand, request.inline_index);
            if (!plan) {
                return false;
            }
            AppliedDetachedInline item;
            item.request = request;
            item.plan = std::make_unique<DetachedInlinePlan>(std::move(*plan));
            item.call_it = call_it;
            item.original_successors = request.block->successors();
            item.affected_scc = collect_caller_scc(module, *request.caller);
            auto tail_begin = std::next(call_it);
            item.first_tail = tail_begin == request.block->instructions().end()
                                  ? nullptr
                                  : tail_begin->get();
            item.first_staged_block = item.plan->blocks.front().get();
            item.inherited_exposure_depth =
                call_exposure_depth(context, *request.call);
            applied.push_back(std::move(item));
        }

        for (auto &item : applied) {
            auto &request = item.request;
            auto &plan = *item.plan;
            if (request.call->parent() != request.block) {
                throw std::runtime_error(
                    "detached inline batch lost reverse-split call location");
            }
            item.call_it = std::find_if(
                request.block->instructions().begin(),
                request.block->instructions().end(), [&](const auto &instruction) {
                    return instruction.get() == request.call;
                });
            if (item.call_it == request.block->instructions().end()) {
                throw std::runtime_error(
                    "detached inline batch call disappeared before commit");
            }
            item.original_successors = request.block->successors();
            auto tail_begin = std::next(item.call_it);
            item.first_tail = tail_begin == request.block->instructions().end()
                                  ? nullptr
                                  : tail_begin->get();
            item.original_constant_count = module.owned_constants().size();
            item.live_applied = true;
            module.adopt_constants(plan.constants);
            request.caller->blocks().splice(request.caller->blocks().end(),
                                            plan.blocks);
            split_call_block(request.block, item.call_it, plan.continuation);
            oir::cfg::add_edge(request.block, plan.entry);
            request.block->instructions().splice(
                request.block->instructions().end(), plan.entry_branch);
            for (const auto &use : plan.call_uses) {
                if (use.user != nullptr &&
                    use.operand_index < use.user->operand_count() &&
                    use.user->operand(use.operand_index) == request.call) {
                    use.user->set_operand(use.operand_index, plan.replacement);
                }
            }
            if (!request.call->uses().empty()) {
                for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
                    if (it->live_applied) {
                        rollback_detached_inline(module, *it);
                    }
                }
                return false;
            }
            if (requests.size() > 1 &&
                consume_residual_test_failure("commit-after-first")) {
                injected_commit_failure = true;
                throw std::runtime_error(
                    "test failpoint: detached batch commit failed after first live split");
            }
        }

        std::string verify_message;
        if (!module.verify(&verify_message)) {
            for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
                rollback_detached_inline(module, *it);
            }
            return false;
        }

        std::vector<oir::CallInst *> inserted_exposed;
        std::vector<oir::Function *> inserted_affected;
        try {
            std::size_t exposed_count = 0;
            std::size_t affected_count = 0;
            for (const auto &item : applied) {
                exposed_count += item.plan->exposed_calls.size();
                affected_count += item.affected_scc.size();
            }
            inserted_exposed.reserve(exposed_count);
            inserted_affected.reserve(affected_count);
            for (auto &item : applied) {
                for (auto *cloned_call : item.plan->exposed_calls) {
                    const auto [inserted, did_insert] =
                        context.exposed_call_depths.emplace(
                            cloned_call, item.inherited_exposure_depth + 1);
                    (void)inserted;
                    if (did_insert) {
                        // Capacity was reserved before the first context mutation,
                        // so recording the rollback key cannot now throw.
                        inserted_exposed.push_back(cloned_call);
                    }
                }
                for (auto *function : item.affected_scc) {
                    if (context.affected_functions.insert(function).second) {
                        inserted_affected.push_back(function);
                    }
                }
            }
        } catch (...) {
            for (auto *call : inserted_exposed) {
                context.exposed_call_depths.erase(call);
            }
            for (auto *function : inserted_affected) {
                context.affected_functions.erase(function);
            }
            for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
                rollback_detached_inline(module, *it);
            }
            return false;
        }

        for (auto &item : applied) {
            context.exposed_call_depths.erase(item.request.call);
            (*item.call_it)->drop_all_operands();
            auto *live_parent = item.request.call->parent();
            if (live_parent == nullptr) {
                throw std::runtime_error(
                    "committed detached inline call lost its live parent");
            }
            live_parent->instructions().erase(item.call_it);
        }
        return true;
    } catch (...) {
        for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
            if (it->live_applied) {
                rollback_detached_inline(module, *it);
            }
        }
        if (injected_commit_failure) {
            for (auto &item : applied) {
                if (item.plan != nullptr) {
                    item.plan->drop_staged_references();
                }
            }
            if (!test_snapshot ||
                !inline_live_module_snapshot_matches(module, *test_snapshot)) {
                throw std::runtime_error(
                    "detached batch rollback changed the exact live module/use snapshot");
            }
        }
        return false;
    }
}

bool inline_detached_residual(
    oir::Module &module, InlineContext &context, oir::Function &caller,
    oir::BasicBlock *block,
    std::list<std::unique_ptr<oir::Instruction>>::iterator call_it,
    const ScratchResidual &scratch, const SpecializationMask &mask,
    ReturnDemandMask return_demand, unsigned inline_index,
    Stats *specialization_work_stats = nullptr) {
    auto *call = dynamic_cast<oir::CallInst *>(call_it->get());
    if (call == nullptr) {
        return false;
    }
    return inline_detached_residual_batch(
        module, context,
        {{&caller, block, call, &scratch, &mask, return_demand, inline_index}},
        specialization_work_stats);
}

bool inline_call(oir::Module &module, InlineContext &context, oir::Function &caller,
                 oir::BasicBlock *block,
                 std::list<std::unique_ptr<oir::Instruction>>::iterator call_it,
                 unsigned inline_index, Stats &stats, bool dry_run = false,
                 bool cost_preapproved = false) {
    auto *call = static_cast<oir::CallInst *>(call_it->get());
    auto *callee = dynamic_cast<oir::Function *>(call->callee());
    bool self_recursive = callee == &caller;
    oir::Function *clone_source = callee;
    unsigned self_recursive_depth = 0;
    const auto policy = pass::cost_model::policy_for_kind(stats.cost_model_policy);
    const bool contextual_large =
        callee != nullptr && callee != &caller && call->users().empty() &&
        callsite_is_in_cycle(*call) &&
        !same_call_graph_scc(caller, *callee) &&
        (call_exposure_depth(context, *call) != 0 || !has_internal_caller(module, caller));
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
    } else if (!is_eligible_non_recursive_call(caller, *call, callee, policy,
                                                contextual_large)) {
        return false;
    }

    const auto info = inspect_callee(*clone_source);
    const bool used_contextual_envelope =
        contextual_large &&
        (info.blocks > kMaxCalleeBlocks || !within_inline_resource_limit(info, policy));
    OIRTransformCostEstimate estimate;
    estimate.kind = pass::cost_model::TransformKind::Inline;
    estimate.pass_name = "OIRInlinePass";
    estimate.candidate_id = "inline." + std::to_string(inline_index);
    estimate.scope = self_recursive
                         ? "direct-recursive-depth-" +
                               std::to_string(self_recursive_depth)
                         : (used_contextual_envelope ? "loop-context-call" : "call");
    estimate.proof_kind = pass::cost_model::ProofKind::Structural;
    estimate.proof_summary = "existing inline legality checks";
    estimate.proof_rule_id = structural_callsite_fingerprint(
        structural_callsite_key(caller, *call, callee));
    if (dry_run) {
        estimate.scope = "structural-tie-batch";
        estimate.proof_summary =
            "all-member structural tie legality and cost preflight";
    }
    estimate.confidence = self_recursive ? 0.60 : (used_contextual_envelope ? 0.76 : 0.65);
    estimate.dynamic_multiplier = used_contextual_envelope ? 16 : 1;
    fill_before_after_from_callee(estimate, info, 1, 0);
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
    estimate.risk.code_growth = std::max<std::int64_t>(1, info.static_instrs);
    const auto argument_count =
        static_cast<std::uint64_t>(call->args().size());
    const auto argument_spill_proxy =
        argument_count > kRV64IntegerArgumentRegisters
            ? argument_count - kRV64IntegerArgumentRegisters
            : 0;
    estimate.risk.register_pressure_growth =
        static_cast<std::int64_t>(argument_spill_proxy);
    estimate.risk.live_range_growth =
        static_cast<std::int64_t>(std::min<std::size_t>(call->args().size(), 4)) +
        (info.returns > 1 ? 1 : 0);
    estimate.risk.memory_pressure_growth =
        static_cast<std::int64_t>((info.loads + info.stores) / 6);
    estimate.risk.cleanup_dependency = 1;
    const auto committed_growth = static_cast<std::uint64_t>(cloned_instruction_growth(info));
    const auto root_id = clone_source->root_function_id();
    const auto growth_class = self_recursive ? CallGrowthClass::Recursive
                                             : CallGrowthClass::Ordinary;
    const auto growth_fit = call_growth_budget_fit(
        stats, growth_class, root_id, committed_growth);
    const auto committed_pressure = estimate_call_pressure(info, *call);
    const auto pressure_reject = pressure_reject_reason(
        stats.cumulative_call_pressure, committed_pressure,
        stats.call_pressure_budget);
    const bool cumulative_budget_exhausted = !growth_fit.fits();
    if (cumulative_budget_exhausted) {
        estimate.forced_reject_reason =
            pass::cost_model::RejectReason::CumulativeBudgetExhausted;
        append_call_growth_budget_summary(estimate.proof_summary, growth_class,
                                          growth_fit);
    }
    if (pressure_reject != pass::cost_model::RejectReason::None) {
        estimate.forced_reject_reason = pressure_reject;
        estimate.proof_summary += "; cumulative call pressure budget exhausted";
    }
    if ((!cost_preapproved && !cost_model_allows_transform(stats, estimate)) ||
        cumulative_budget_exhausted ||
        pressure_reject != pass::cost_model::RejectReason::None) {
        return false;
    }
    if (dry_run) {
        return true;
    }
    CallGrowthReservation growth_reservation;
    const std::vector<oir::FunctionID> roots{root_id};
    if (!reserve_call_growth_entries(stats, growth_class, roots,
                                     growth_reservation)) {
        return false;
    }
    auto affected_scc = collect_caller_scc(module, caller);

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
    const unsigned inherited_exposure_depth = call_exposure_depth(context, *call);
    for (const auto &[source_block, cloned_block] : blocks) {
        (void)source_block;
        for (const auto &inst : cloned_block->instructions()) {
            auto *cloned_call = dynamic_cast<oir::CallInst *>(inst.get());
            if (cloned_call != nullptr) {
                context.exposed_call_depths[cloned_call] = inherited_exposure_depth + 1;
            }
        }
    }
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
    context.exposed_call_depths.erase(call);
    oir::cfg::append_unconditional_branch(module, block, blocks.at(clone_source->entry_block()));

    if (!call->type()->is_void()) {
        ReplacementMap replacements;
        replacements[call] = materialize_return_value(module, *call, continuation, returns);
        apply_replacements(module, replacements);
    }
    (*call_it)->drop_all_operands();
    block->instructions().erase(call_it);
    commit_call_growth(stats, growth_class, root_id, committed_growth);
    add_call_pressure(stats.cumulative_call_pressure, committed_pressure);
    mark_affected_functions(context, affected_scc);
    return true;
}

bool inline_one_call(oir::Module &module, InlineContext &context, oir::Function &function,
                     unsigned inline_index, Stats &stats) {
    struct RecursiveSite {
        unsigned depth = 0;
        oir::BasicBlock *block = nullptr;
        oir::CallInst *call = nullptr;
    };

    // Inline ordinary helpers first.  Recursive templates must be captured only after
    // helper calls have had a chance to disappear, otherwise an obsolete template can
    // make an otherwise supported recursive body look permanently ineligible.
    for (auto &block : function.blocks()) {
        for (auto it = block->instructions().begin(); it != block->instructions().end(); ++it) {
            auto *call = dynamic_cast<oir::CallInst *>(it->get());
            if (call == nullptr || call->callee() == &function) {
                continue;
            }
            if (inline_call(module, context, function, block.get(), it, inline_index, stats)) {
                // Let cleanup and tail-recursion elimination see the helper-free body
                // before deciding whether recursive expansion is still profitable.
                context.defer_recursive_expansion.insert(&function);
                return true;
            }
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

bool specialize_constant_argument_calls_impl(oir::Module &module, Stats &stats) {
    apply_canonical_container_permutation_test_once(module);
    struct Site {
        oir::Function *caller = nullptr;
        oir::Function *callee = nullptr;
        oir::CallInst *call = nullptr;
        // The decision key includes canonical intra-function position.  It is
        // the only key allowed to define an atomic direct-inline tie.
        std::string decision_key;
        // The reuse-group key deliberately omits callsite position.  It is only
        // a deterministic coarse bucket; actual callee/root identity is checked
        // again before members may share a persistent residual.
        std::string reuse_group_key;
        std::string binding_key;
        SpecializationMask mask;
        ReturnDemandMask return_demand = ReturnDemandMask::Scalar;
    };

    std::vector<Site> sites;
    StructuralKeyCache structural_keys;
    const auto policy = pass::cost_model::policy_for_kind(stats.cost_model_policy);
    initialize_call_growth_budget(module, stats, policy);
    initialize_call_specialization_work_budget(stats, policy);
    if (stats.call_specialization_work_exhausted) {
        return false;
    }
    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        const auto reachable = reachable_block_set(*function);
        for (auto &block : function->blocks()) {
            if (reachable.find(block.get()) == reachable.end()) {
                continue;
            }
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
                auto binding_key = specialization_key(*call, mask);
                const auto return_demand =
                    call->type()->is_void() || call->uses().empty()
                        ? ReturnDemandMask::Dead
                        : ReturnDemandMask::Scalar;
                std::ostringstream reuse_group_key;
                reuse_group_key
                    << "callee="
                    << canonical_function_shape(*callee, &structural_keys)
                    << "|mask=";
                for (const bool selected : mask) {
                    reuse_group_key << (selected ? '1' : '0');
                }
                reuse_group_key
                    << "|binding=" << binding_key << "|demand="
                    << (return_demand == ReturnDemandMask::Dead ? "dead"
                                                                : "scalar")
                    << "|return=" << call->type()->print();
                sites.push_back(
                    {function.get(), callee, call,
                     binding_key + "|" + structural_callsite_key(
                                              *function, *call, callee,
                                              &structural_keys),
                     reuse_group_key.str(), std::move(binding_key),
                     std::move(mask), return_demand});
            }
        }
    }

    if (sites.empty()) {
        return false;
    }
    std::sort(sites.begin(), sites.end(), [](const Site &lhs, const Site &rhs) {
        return lhs.decision_key < rhs.decision_key;
    });

    // Apply the compile-time cap only after canonical ordering, and never split an
    // identical structural tie batch.  An oversized first batch is deferred as a
    // whole instead of letting module insertion order choose an arbitrary subset.
    std::size_t capped_sites = 0;
    while (capped_sites < sites.size()) {
        std::size_t batch_end = capped_sites + 1;
        while (batch_end < sites.size() &&
               sites[batch_end].decision_key ==
                   sites[capped_sites].decision_key) {
            ++batch_end;
        }
        if (batch_end > kMaxSpecializedCallSites) {
            break;
        }
        capped_sites = batch_end;
        if (capped_sites == kMaxSpecializedCallSites) {
            break;
        }
    }
    sites.resize(capped_sites);
    if (sites.empty()) {
        return false;
    }

    unsigned direct_inline_index = stats.inlined;
    InlineContext direct_inline_context;
    std::unordered_set<std::string> rejected_pe_ties;
    std::unordered_set<oir::CallInst *> attempted_persistent_reuse_calls;
    auto current_return_demand = [](const oir::CallInst &call) {
        return call.type()->is_void() || call.uses().empty()
                   ? ReturnDemandMask::Dead
                   : ReturnDemandMask::Scalar;
    };
    auto live_residual_signature = [](const ScratchResidual &scratch)
        -> std::optional<std::string> {
        if (scratch.function == nullptr) {
            return std::nullopt;
        }
        const auto return_type =
            scratch.live_types.find(scratch.function->return_type());
        if (return_type == scratch.live_types.end() ||
            return_type->second == nullptr) {
            return std::nullopt;
        }
        std::ostringstream signature;
        signature << return_type->second->print() << '(';
        for (const auto &argument : scratch.function->args()) {
            const auto live_type = scratch.live_types.find(argument->type());
            if (live_type == scratch.live_types.end() ||
                live_type->second == nullptr) {
                return std::nullopt;
            }
            signature << live_type->second->print() << ';';
        }
        signature << ')';
        return signature.str();
    };
    for (auto &site : sites) {
        if (rejected_pe_ties.find(site.decision_key) !=
            rejected_pe_ties.end()) {
            continue;
        }
        if (site.call == nullptr || site.callee == nullptr ||
            dynamic_cast<oir::Function *>(site.call->callee()) != site.callee) {
            continue;
        }

        const bool test_snapshot_enabled =
            std::getenv("YOOLANG_TEST_OIR_RESIDUAL_FAILURE") != nullptr;
        std::optional<InlineLiveModuleSnapshot> test_live_snapshot;
        std::optional<InlineCommitStatsSnapshot> test_stats_snapshot;
        if (test_snapshot_enabled) {
            test_live_snapshot = capture_inline_live_module_snapshot(module);
            test_stats_snapshot = capture_inline_commit_stats(stats);
        }
        auto assert_test_rejection_snapshot = [&]() {
            if (test_snapshot_enabled &&
                (!test_live_snapshot || !test_stats_snapshot ||
                 !inline_live_module_snapshot_matches(module,
                                                     *test_live_snapshot) ||
                 !inline_commit_stats_match(stats, *test_stats_snapshot))) {
                throw std::runtime_error(
                    "residual rejection changed live objects/use lists/function table/name allocator/commit stats");
            }
        };

        // Persistent residual reuse is deliberately considered before the
        // position-sensitive direct-tie path.  The string key is only a stable
        // coarse bucket: pointer/root identity and every semantic condition are
        // revalidated below.  Calls from different canonical positions may share
        // one clone, but they can never become one atomic direct-inline batch.
        if (attempted_persistent_reuse_calls.find(site.call) ==
                attempted_persistent_reuse_calls.end() &&
            site.return_demand == ReturnDemandMask::Scalar) {
            std::vector<Site *> reuse_sites;
            for (auto &candidate : sites) {
                if (candidate.call == nullptr || candidate.callee == nullptr ||
                    candidate.reuse_group_key != site.reuse_group_key ||
                    dynamic_cast<oir::Function *>(candidate.call->callee()) !=
                        candidate.callee ||
                    candidate.callee != site.callee ||
                    candidate.callee->root_function_id() !=
                        site.callee->root_function_id() ||
                    candidate.mask != site.mask ||
                    candidate.return_demand != site.return_demand ||
                    current_return_demand(*candidate.call) !=
                        site.return_demand ||
                    candidate.call->type() != site.call->type() ||
                    !same_specialization_binding(
                        *site.call, site.mask, *candidate.call,
                        candidate.mask)) {
                    continue;
                }
                reuse_sites.push_back(&candidate);
            }
            for (auto *candidate : reuse_sites) {
                attempted_persistent_reuse_calls.insert(candidate->call);
            }

            if (reuse_sites.size() >= 2) {
                std::uint64_t reuse_scratch_work = 0;
                for (const auto *candidate : reuse_sites) {
                    reuse_scratch_work = saturating_u64_add(
                        reuse_scratch_work,
                        specialization_scratch_reservation(
                            inspect_callee(*candidate->callee)));
                }
                if (!require_specialization_work_capacity(
                        stats,
                        static_cast<std::uint64_t>(reuse_sites.size()),
                        reuse_scratch_work,
                        "reuse." +
                            structural_hash_text(site.reuse_group_key),
                        "persistent residual reuse group scratch builds would exceed the persistent specialization work budget")) {
                    return false;
                }
                struct PersistentReuseMember {
                    Site *site = nullptr;
                    ScratchResidual scratch;
                    CalleeInfo original_info;
                    CalleeInfo residual_info;
                    OIRTransformCostEstimate estimate;
                    std::string residual_signature;
                    std::uint64_t growth = 0;
                    std::uint64_t savings = 0;
                    CallPressureVector pressure;
                    bool direct_candidate = false;
                    bool direct_ready = false;
                };
                std::vector<PersistentReuseMember> reuse_members;
                reuse_members.reserve(reuse_sites.size());
                std::uint64_t aggregate_reuse_savings = 0;
                CallPressureVector aggregate_reuse_pressure;
                std::optional<std::string> expected_residual_signature;
                bool persistent_preflight_ok = true;

                for (auto *candidate : reuse_sites) {
                    SpecializationMask revalidated_mask;
                    const auto existing_for_candidate =
                        existing_specialization_count(module,
                                                      *candidate->callee);
                    const auto candidate_demand =
                        current_return_demand(*candidate->call);
                    if (candidate->call->parent() == nullptr ||
                        dynamic_cast<oir::Function *>(
                            candidate->call->callee()) != candidate->callee ||
                        candidate->callee != site.callee ||
                        candidate->callee->root_function_id() !=
                            site.callee->root_function_id() ||
                        !is_eligible_for_constant_specialization(
                            *candidate->caller, *candidate->call,
                            candidate->callee, policy,
                            existing_for_candidate, revalidated_mask) ||
                        revalidated_mask != candidate->mask ||
                        specialization_key(*candidate->call,
                                           revalidated_mask) !=
                            candidate->binding_key ||
                        candidate->binding_key != site.binding_key ||
                        candidate_demand != candidate->return_demand ||
                        candidate_demand != ReturnDemandMask::Scalar ||
                        candidate->call->type() != site.call->type() ||
                        !same_specialization_binding(
                            *site.call, site.mask, *candidate->call,
                            revalidated_mask)) {
                        persistent_preflight_ok = false;
                        break;
                    }

                    const auto candidate_original =
                        inspect_callee(*candidate->callee);
                    auto candidate_scratch =
                        build_charged_detached_constant_residual(
                            module, *candidate->callee, *candidate->call,
                            revalidated_mask, candidate_demand,
                            candidate_original, stats,
                            structural_callsite_fingerprint(
                                candidate->decision_key));
                    if (candidate_scratch.status !=
                            ScratchResidualStatus::Success ||
                        candidate_scratch.function == nullptr) {
                        persistent_preflight_ok = false;
                        break;
                    }
                    if (consume_residual_test_failure("typed-import")) {
                        candidate_scratch.live_types.erase(
                            candidate_scratch.function->return_type());
                        candidate_scratch.detail =
                            "test failpoint: missing detached TypeContext import mapping; live snapshot including function table unchanged";
                    }
                    const auto signature =
                        live_residual_signature(candidate_scratch);
                    if (!signature ||
                        (expected_residual_signature &&
                         *signature != *expected_residual_signature)) {
                        persistent_preflight_ok = false;
                        break;
                    }
                    if (!expected_residual_signature) {
                        expected_residual_signature = *signature;
                    }

                    const auto candidate_residual =
                        inspect_callee(*candidate_scratch.function);
                    const bool candidate_reduced =
                        candidate_residual.static_instrs <
                            candidate_original.static_instrs ||
                        candidate_residual.branches <
                            candidate_original.branches ||
                        candidate_residual.calls < candidate_original.calls ||
                        candidate_scratch.fixed_point_suffix
                                .eliminated_dynamic_instructions != 0;
                    const bool candidate_commit_supported =
                        detached_residual_commit_is_supported(
                            candidate_scratch, *candidate->call,
                            revalidated_mask, candidate_demand);
                    const bool candidate_direct =
                        !same_call_graph_scc(*candidate->caller,
                                             *candidate->callee) &&
                        candidate_reduced && candidate_commit_supported &&
                        candidate_residual.blocks <=
                            kMaxSpecializedInlineBlocks &&
                        candidate_residual.cost <=
                            static_cast<unsigned>(
                                policy.max_inline_callee_cost * 3) &&
                        candidate_residual.returns != 0 &&
                        candidate_residual.returns <=
                            kMaxSpecializedInlineReturns;
                    if (!candidate_reduced || !candidate_commit_supported ||
                        candidate->call->uses().empty()) {
                        persistent_preflight_ok = false;
                        break;
                    }

                    const auto growth =
                        cloned_instruction_growth(candidate_residual);
                    const auto eliminated_instrs =
                        candidate_original.static_instrs >
                                candidate_residual.static_instrs
                            ? candidate_original.static_instrs -
                                  candidate_residual.static_instrs
                            : 0;
                    const auto eliminated_branches =
                        candidate_original.branches >
                                candidate_residual.branches
                            ? candidate_original.branches -
                                  candidate_residual.branches
                            : 0;
                    const auto eliminated_phis =
                        candidate_original.phis > candidate_residual.phis
                            ? candidate_original.phis -
                                  candidate_residual.phis
                            : 0;
                    const auto savings = saturating_u64_add(
                        saturating_u64_add(eliminated_instrs,
                                           eliminated_branches),
                        saturating_u64_add(
                            eliminated_phis,
                            candidate_scratch.fixed_point_suffix
                                .eliminated_dynamic_instructions));
                    const auto pressure = estimate_call_pressure(
                        candidate_residual, *candidate->call,
                        &revalidated_mask);
                    aggregate_reuse_savings = saturating_u64_add(
                        aggregate_reuse_savings, savings);
                    add_call_pressure(aggregate_reuse_pressure, pressure);
                    reuse_members.push_back(
                        {candidate, std::move(candidate_scratch),
                         candidate_original, candidate_residual, {},
                         *signature, growth, savings, pressure,
                         candidate_direct, false});
                }

                if (persistent_preflight_ok &&
                    reuse_members.size() == reuse_sites.size()) {
                    std::uint64_t reuse_plan_work = 0;
                    for (const auto &member : reuse_members) {
                        reuse_plan_work = saturating_u64_add(
                            reuse_plan_work,
                            specialization_plan_work_units(
                                member.residual_info));
                    }
                    if (!require_specialization_work_capacity(
                            stats, 0, reuse_plan_work,
                            "reuse." + structural_hash_text(
                                           site.reuse_group_key),
                            "persistent residual member plans would exceed the persistent specialization work budget")) {
                        return false;
                    }
                    for (std::size_t index = 0;
                         index < reuse_members.size(); ++index) {
                        auto &member = reuse_members[index];
                        if (!charge_specialization_plan_work(
                                stats, member.residual_info,
                                structural_callsite_fingerprint(
                                    member.site->decision_key),
                                "persistent residual member plan would exceed the persistent specialization work budget")) {
                            return false;
                        }
                        const auto staged_direct =
                            build_detached_inline_plan(
                                module, *member.site->caller,
                                *member.site->call, member.scratch,
                                member.site->mask,
                                member.site->return_demand,
                                direct_inline_index +
                                    static_cast<unsigned>(index) + 1);
                        member.direct_ready =
                            member.direct_candidate &&
                            staged_direct.has_value();
                    }
                    for (auto &member : reuse_members) {
                        member.estimate = measured_residual_estimate(
                            *member.site->callee, *member.site->call,
                            member.site->mask,
                            member.site->return_demand, member.scratch,
                            member.original_info, member.residual_info,
                            is_directly_recursive(*member.site->callee),
                            member.direct_ready, policy, stats,
                            "specialize." + std::to_string(
                                                ++stats.cost_model_candidates));
                        member.estimate.scope =
                            "measured-persistent-reuse-member";
                        member.estimate.proof_rule_id =
                            structural_callsite_fingerprint(
                                member.site->decision_key);
                        member.estimate.proof_summary =
                            "independently revalidated and measured persistent-reuse member with fresh residual signature and charged direct-plan preflight";
                        if (consume_residual_test_failure("cost-reject")) {
                            member.estimate.forced_reject_reason =
                                pass::cost_model::RejectReason::CodeGrowthTooHigh;
                            member.estimate.proof_summary +=
                                "; test failpoint cost rejection; live snapshot including function table unchanged";
                        }
                    }
                }

                const auto specialization_limit = static_cast<unsigned>(
                    std::max<std::int64_t>(
                        0, policy.max_specializations_per_function));
                const auto persistent_growth =
                    reuse_members.empty() ? 0 : reuse_members.front().growth;
                const auto persistent_root =
                    reuse_members.empty()
                        ? oir::kInvalidFunctionID
                        : reuse_members.front().site->callee->root_function_id();
                const auto persistent_growth_fit = call_growth_budget_fit(
                    stats, CallGrowthClass::Specialization,
                    persistent_root, persistent_growth);
                const auto persistent_specialization_it =
                    stats.call_root_specializations.find(persistent_root);
                const auto persistent_specializations =
                    persistent_specialization_it ==
                            stats.call_root_specializations.end()
                        ? 0U
                        : persistent_specialization_it->second;
                const auto persistent_pressure_reject =
                    pressure_reject_reason(
                        stats.cumulative_call_pressure,
                        aggregate_reuse_pressure,
                        stats.call_pressure_budget);
                const bool persistent_growth_fits =
                    !residual_test_failure_is("tie-budget") &&
                    persistent_growth_fit.fits();
                const bool measured_persistent_reuse =
                    persistent_preflight_ok &&
                    reuse_members.size() == reuse_sites.size() &&
                    reuse_members.size() >= 2 &&
                    aggregate_reuse_savings >= persistent_growth &&
                    persistent_growth_fits &&
                    persistent_pressure_reject ==
                        pass::cost_model::RejectReason::None &&
                    persistent_specializations < specialization_limit &&
                    existing_specialization_count(
                        module, *reuse_members.front().site->callee) <
                        std::min<unsigned>(kMaxSpecializedFunctions,
                                           specialization_limit);

                if (measured_persistent_reuse) {
                    if (!require_specialization_work_capacity(
                            stats, 0,
                            specialization_plan_work_units(
                                reuse_members.front().residual_info),
                            "reuse." +
                                structural_hash_text(site.reuse_group_key),
                            "persistent residual import would exceed the persistent specialization work budget")) {
                        return false;
                    }
                    const auto report_size_before_persistent =
                        stats.cost_model_report == nullptr
                            ? 0
                            : stats.cost_model_report->decisions.size();
                    bool every_member_profitable = true;
                    auto member_reject_reason =
                        pass::cost_model::RejectReason::None;
                    for (auto &member : reuse_members) {
                        if (!cost_model_allows_transform(stats,
                                                         member.estimate)) {
                            every_member_profitable = false;
                            if (stats.cost_model_report != nullptr &&
                                stats.cost_model_report->decisions.size() >
                                    report_size_before_persistent) {
                                member_reject_reason =
                                    stats.cost_model_report->decisions.back()
                                        .reject_reason;
                            } else {
                                member_reject_reason =
                                    member.estimate.forced_reject_reason ==
                                            pass::cost_model::RejectReason::None
                                        ? pass::cost_model::RejectReason::NegativeGain
                                        : member.estimate.forced_reject_reason;
                            }
                            break;
                        }
                    }

                    auto persistent_estimate = reuse_members.front().estimate;
                    persistent_estimate.candidate_id += ".reuse";
                    persistent_estimate.scope =
                        "measured-persistent-reuse";
                    persistent_estimate.proof_rule_id =
                        "reuse." + structural_hash_text(
                                       site.reuse_group_key);
                    persistent_estimate.proof_summary =
                        "position-independent group independently revalidated; multi-call savings cover one persistent clone";
                    persistent_estimate.dynamic_multiplier =
                        std::max<std::int64_t>(
                            persistent_estimate.dynamic_multiplier,
                            static_cast<std::int64_t>(
                                reuse_members.size()));
                    fill_after_from_residual(
                        persistent_estimate,
                        reuse_members.front().residual_info, 1);
                    persistent_estimate.after_code_bytes =
                        persistent_estimate.before_code_bytes +
                        static_cast<std::int64_t>(
                            reuse_members.front().residual_info.static_instrs *
                            4);
                    persistent_estimate.risk.code_growth =
                        static_cast<std::int64_t>(persistent_growth);
                    persistent_estimate.partial_eval.cloned_functions = 1;
                    persistent_estimate.partial_eval.cloned_blocks =
                        reuse_members.front().residual_info.blocks;
                    persistent_estimate.partial_eval.residual_instrs =
                        reuse_members.front().residual_info.static_instrs;

                    bool aggregate_profitable = false;
                    auto aggregate_reject_reason =
                        pass::cost_model::RejectReason::NegativeGain;
                    if (every_member_profitable) {
                        aggregate_profitable = cost_model_allows_transform(
                            stats, persistent_estimate);
                        if (!aggregate_profitable &&
                            stats.cost_model_report != nullptr &&
                            stats.cost_model_report->decisions.size() >
                                report_size_before_persistent) {
                            aggregate_reject_reason =
                                stats.cost_model_report->decisions.back()
                                    .reject_reason;
                        }
                    }
                    if (every_member_profitable && aggregate_profitable) {
                        const std::vector<oir::FunctionID> roots{
                            persistent_root};
                        CallGrowthReservation growth_reservation;
                        const bool growth_slot_ready =
                            reserve_call_growth_entries(
                                stats, CallGrowthClass::Specialization,
                                roots, growth_reservation);
                        const bool import_work_ready =
                            growth_slot_ready &&
                            charge_specialization_plan_work(
                                stats,
                                reuse_members.front().residual_info,
                                "reuse." + structural_hash_text(
                                               site.reuse_group_key),
                                "persistent residual import would exceed the persistent specialization work budget");
                        if (growth_slot_ready && !import_work_ready) {
                            erase_reserved_call_growth_entries(
                                stats, CallGrowthClass::Specialization,
                                growth_reservation);
                            return false;
                        }
                        auto persistent =
                            import_work_ready
                                ? import_persistent_residual_clone(
                                      module,
                                      reuse_members.front().scratch,
                                      *reuse_members.front().site->call,
                                      reuse_members.front().site->mask,
                                      *reuse_members.front().site->callee)
                                : std::nullopt;
                        std::vector<oir::CallInst *> reuse_calls;
                        reuse_calls.reserve(reuse_members.size());
                        for (const auto &member : reuse_members) {
                            reuse_calls.push_back(member.site->call);
                        }
                        if (persistent &&
                            retarget_calls_to_persistent_residual(
                                module, *persistent, reuse_calls,
                                reuse_members.front().site->mask)) {
                            commit_call_growth(
                                stats, CallGrowthClass::Specialization,
                                persistent_root, persistent_growth);
                            ++stats.call_root_specializations.at(
                                persistent_root);
                            add_call_pressure(
                                stats.cumulative_call_pressure,
                                aggregate_reuse_pressure);
                            stats.specialized += static_cast<unsigned>(
                                reuse_members.size());
                            return true;
                        }
                        erase_reserved_call_growth_entries(
                            stats, CallGrowthClass::Specialization,
                            growth_reservation);
                        if (stats.cost_model_report != nullptr &&
                            stats.cost_model_report->decisions.size() >
                                report_size_before_persistent) {
                            stats.cost_model_report->decisions.resize(
                                report_size_before_persistent);
                        }
                        persistent_estimate.forced_reject_reason =
                            pass::cost_model::RejectReason::CommitPreflightFailed;
                        persistent_estimate.proof_summary =
                            "persistent residual commit failed; group snapshots restored before per-position fallback";
                        (void)cost_model_allows_transform(
                            stats, persistent_estimate);
                        assert_test_rejection_snapshot();
                    } else {
                        if (stats.cost_model_report != nullptr &&
                            stats.cost_model_report->decisions.size() >
                                report_size_before_persistent) {
                            stats.cost_model_report->decisions.resize(
                                report_size_before_persistent);
                        }
                        persistent_estimate.forced_reject_reason =
                            every_member_profitable
                                ? aggregate_reject_reason
                                : member_reject_reason;
                        persistent_estimate.proof_summary =
                            every_member_profitable
                                ? "persistent reuse aggregate rejected before per-position fallback"
                                : "persistent reuse member rejected before per-position fallback";
                        (void)cost_model_allows_transform(
                            stats, persistent_estimate);
                        assert_test_rejection_snapshot();
                    }
                }
            }
        }

        const bool recursive_layer = is_directly_recursive(*site.callee);
        const auto constant_arg_count = count_specializable_constants(site.mask);
        const auto info = inspect_callee(*site.callee);
        const auto return_demand =
            site.call->type()->is_void() || site.call->uses().empty()
                ? ReturnDemandMask::Dead
                : ReturnDemandMask::Scalar;
        std::unordered_set<oir::CallInst *> exact_tie_calls;
        std::uint64_t exact_tie_scratch_work = 0;
        for (auto &candidate : sites) {
            if (candidate.decision_key == site.decision_key &&
                candidate.call != nullptr &&
                dynamic_cast<oir::Function *>(candidate.call->callee()) ==
                    candidate.callee &&
                exact_tie_calls.insert(candidate.call).second) {
                exact_tie_scratch_work = saturating_u64_add(
                    exact_tie_scratch_work,
                    specialization_scratch_reservation(
                        inspect_callee(*candidate.callee)));
            }
        }
        const auto exact_tie_size = exact_tie_calls.size();
        if (!require_specialization_work_capacity(
                stats, static_cast<std::uint64_t>(exact_tie_size),
                exact_tie_scratch_work,
                structural_callsite_fingerprint(site.decision_key),
                "direct residual tie scratch builds would exceed the persistent specialization work budget")) {
            return false;
        }
        auto scratch = build_charged_detached_constant_residual(
            module, *site.callee, *site.call, site.mask, return_demand,
            info, stats,
            structural_callsite_fingerprint(site.decision_key));
        OIRTransformCostEstimate estimate;
        estimate.kind = pass::cost_model::TransformKind::ConstantArgumentSpecialization;
        estimate.pass_name = "OIRInlinePass";
        estimate.candidate_id = "specialize." + std::to_string(++stats.cost_model_candidates);
        estimate.scope = "call";
        estimate.proof_kind = pass::cost_model::ProofKind::PartialEvaluation;
        estimate.proof_rule_id =
            structural_callsite_fingerprint(site.decision_key);
        estimate.proof_summary =
            recursive_layer
                ? "verified detached residual; bounded direct-recursive layer"
                : (return_demand == ReturnDemandMask::Dead
                       ? "verified detached constant residual with dead-return slicing"
                       : "verified detached constant residual with demanded scalar return");
        estimate.confidence = constant_arg_count == 0 ? 0.55 : (recursive_layer ? 0.70 : 0.74);
        fill_before_after_from_callee(estimate, info, 1, 1);
        estimate.dynamic_multiplier = callsite_is_in_cycle(*site.call) ? 16 : 1;
        if (scratch.status != ScratchResidualStatus::Success || scratch.function == nullptr) {
            estimate.proof_status = pass::cost_model::ProofStatus::Unknown;
            estimate.proof_summary = "detached residual rejected: " + scratch.detail;
            (void)cost_model_allows_transform(stats, estimate);
            assert_test_rejection_snapshot();
            continue;
        }
        if (consume_residual_test_failure("typed-import")) {
            scratch.live_types.erase(scratch.function->return_type());
            scratch.detail =
                "test failpoint: missing detached TypeContext import mapping; live snapshot including function table unchanged";
        }
        const auto residual_info = inspect_callee(*scratch.function);
        const bool measured_residual_reduction =
            residual_info.static_instrs < info.static_instrs ||
            residual_info.branches < info.branches || residual_info.calls < info.calls ||
            scratch.fixed_point_suffix.eliminated_dynamic_instructions != 0;
        const bool direct_commit_supported = detached_residual_commit_is_supported(
            scratch, *site.call, site.mask, return_demand);
        const bool direct_residual =
            !same_call_graph_scc(*site.caller, *site.callee) &&
            measured_residual_reduction && direct_commit_supported &&
            residual_info.blocks <= kMaxSpecializedInlineBlocks &&
            residual_info.cost <= static_cast<unsigned>(policy.max_inline_callee_cost * 3) &&
            residual_info.returns != 0 &&
            residual_info.returns <= kMaxSpecializedInlineReturns;
        estimate = measured_residual_estimate(
            *site.callee, *site.call, site.mask, return_demand, scratch,
            info, residual_info, recursive_layer, direct_residual, policy, stats,
            std::move(estimate.candidate_id));
        estimate.proof_rule_id =
            structural_callsite_fingerprint(site.decision_key);
        if (consume_residual_test_failure("cost-reject")) {
            estimate.forced_reject_reason =
                pass::cost_model::RejectReason::CodeGrowthTooHigh;
            estimate.proof_summary +=
                "; test failpoint cost rejection; live snapshot including function table unchanged";
        }
        const auto committed_growth = cloned_instruction_growth(residual_info);
        const auto root_id = site.callee->root_function_id();
        const auto committed_pressure =
            estimate_call_pressure(residual_info, *site.call, &site.mask);

        if (exact_tie_size > 1) {
            struct DirectTieMember {
                Site *site = nullptr;
                ScratchResidual scratch;
                CalleeInfo residual_info;
                OIRTransformCostEstimate estimate;
                std::uint64_t growth = 0;
                CallPressureVector pressure;
            };
            std::vector<DirectTieMember> members;
            members.reserve(exact_tie_size);
            std::unordered_map<oir::FunctionID, std::uint64_t> root_growth;
            std::unordered_map<oir::FunctionID, unsigned> root_specializations;
            std::uint64_t batch_growth = 0;
            CallPressureVector batch_pressure;
            bool batch_preflight_ok = true;
            bool direct_batch_preflight_ok = true;
            for (auto &candidate : sites) {
                if (candidate.decision_key != site.decision_key ||
                    candidate.call == nullptr ||
                    dynamic_cast<oir::Function *>(candidate.call->callee()) !=
                        candidate.callee) {
                    continue;
                }
                auto *candidate_block = candidate.call->parent();
                SpecializationMask revalidated_mask;
                const auto existing_for_candidate =
                    existing_specialization_count(module, *candidate.callee);
                if (candidate_block == nullptr ||
                    !is_eligible_for_constant_specialization(
                        *candidate.caller, *candidate.call, candidate.callee,
                        policy, existing_for_candidate, revalidated_mask) ||
                    revalidated_mask != candidate.mask ||
                    specialization_key(*candidate.call, revalidated_mask) !=
                        candidate.binding_key) {
                    batch_preflight_ok = false;
                    break;
                }
                const auto candidate_demand =
                    candidate.call->type()->is_void() ||
                            candidate.call->uses().empty()
                        ? ReturnDemandMask::Dead
                        : ReturnDemandMask::Scalar;
                const auto candidate_original =
                    inspect_callee(*candidate.callee);
                auto candidate_scratch =
                    &candidate == &site
                        ? std::move(scratch)
                        : build_charged_detached_constant_residual(
                              module, *candidate.callee, *candidate.call,
                              candidate.mask, candidate_demand,
                              candidate_original, stats,
                              structural_callsite_fingerprint(
                                  candidate.decision_key));
                if (candidate_scratch.status != ScratchResidualStatus::Success ||
                    candidate_scratch.function == nullptr) {
                    batch_preflight_ok = false;
                    break;
                }
                const auto candidate_residual =
                    inspect_callee(*candidate_scratch.function);
                const bool candidate_reduced =
                    candidate_residual.static_instrs <
                        candidate_original.static_instrs ||
                    candidate_residual.branches < candidate_original.branches ||
                    candidate_residual.calls < candidate_original.calls ||
                    candidate_scratch.fixed_point_suffix
                            .eliminated_dynamic_instructions != 0;
                const bool candidate_direct =
                    !same_call_graph_scc(*candidate.caller,
                                         *candidate.callee) &&
                    candidate_reduced &&
                    detached_residual_commit_is_supported(
                        candidate_scratch, *candidate.call, candidate.mask,
                        candidate_demand) &&
                    candidate_residual.blocks <= kMaxSpecializedInlineBlocks &&
                    candidate_residual.cost <=
                        static_cast<unsigned>(policy.max_inline_callee_cost * 3) &&
                    candidate_residual.returns != 0 &&
                    candidate_residual.returns <= kMaxSpecializedInlineReturns;
                direct_batch_preflight_ok &= candidate_direct;
                const auto growth =
                    cloned_instruction_growth(candidate_residual);
                auto member_estimate =
                    &candidate == &site
                        ? estimate
                        : measured_residual_estimate(
                              *candidate.callee, *candidate.call,
                              candidate.mask, candidate_demand,
                              candidate_scratch, candidate_original,
                              candidate_residual,
                              is_directly_recursive(*candidate.callee),
                              candidate_direct, policy, stats,
                              "specialize." + std::to_string(
                                                   ++stats.cost_model_candidates));
                member_estimate.scope = "direct-residual-tie-batch";
                member_estimate.proof_rule_id =
                    structural_callsite_fingerprint(candidate.decision_key);
                member_estimate.proof_summary =
                    "independently verified detached residual and all-member tie preflight";
                const auto member_pressure = estimate_call_pressure(
                    candidate_residual, *candidate.call, &candidate.mask);
                batch_growth = saturating_u64_add(batch_growth, growth);
                auto &per_root = root_growth[candidate.callee->root_function_id()];
                per_root = saturating_u64_add(per_root, growth);
                ++root_specializations[candidate.callee->root_function_id()];
                add_call_pressure(batch_pressure, member_pressure);
                members.push_back({&candidate, std::move(candidate_scratch),
                                   candidate_residual, std::move(member_estimate),
                                   growth, member_pressure});
            }
            if (batch_preflight_ok && direct_batch_preflight_ok &&
                members.size() == exact_tie_size) {
                std::uint64_t preflight_plan_work = 0;
                for (const auto &member : members) {
                    preflight_plan_work = saturating_u64_add(
                        preflight_plan_work,
                        specialization_plan_work_units(
                            member.residual_info));
                }
                if (!require_specialization_work_capacity(
                        stats, 0, preflight_plan_work,
                        structural_callsite_fingerprint(
                            site.decision_key),
                        "direct residual tie preflight plans would exceed the persistent specialization work budget")) {
                    return false;
                }
                for (std::size_t index = 0; index < members.size();
                     ++index) {
                    auto &member = members[index];
                    const auto member_demand =
                        member.site->call->type()->is_void() ||
                                member.site->call->uses().empty()
                            ? ReturnDemandMask::Dead
                            : ReturnDemandMask::Scalar;
                    if (!charge_specialization_plan_work(
                            stats, member.residual_info,
                            structural_callsite_fingerprint(
                                member.site->decision_key),
                            "direct residual tie preflight plan would exceed the persistent specialization work budget")) {
                        return false;
                    }
                    const auto staged_plan = build_detached_inline_plan(
                        module, *member.site->caller,
                        *member.site->call, member.scratch,
                        member.site->mask, member_demand,
                        direct_inline_index +
                            static_cast<unsigned>(index) + 1);
                    if (!staged_plan) {
                        direct_batch_preflight_ok = false;
                        break;
                    }
                }
            }
            const auto specialization_limit = static_cast<unsigned>(
                std::max<std::int64_t>(
                    0, policy.max_specializations_per_function));
            const bool forced_tie_budget_reject =
                residual_test_failure_is("tie-budget");
            // Every detached plan is built from the same pre-commit snapshot.  A
            // caller may therefore also be another member's callee without making
            // module insertion order choose a prefix of the tie.
            auto batch_reject = pressure_reject_reason(
                stats.cumulative_call_pressure, batch_pressure,
                stats.call_pressure_budget);
            const auto batch_growth_fit = call_growth_budget_fit(
                stats, CallGrowthClass::Specialization, batch_growth,
                root_growth);
            bool batch_cumulative_fits = batch_growth_fit.fits();
            if (forced_tie_budget_reject) {
                // Keep this failpoint persistent across fixed-point pipeline
                // revisits.  The focused fixture uses it to prove that a
                // same-block structural tie is never split when the combined
                // cumulative budget cannot cover every member.
                batch_cumulative_fits = false;
            }
            bool batch_specialization_fits = true;
            for (const auto &[member_root, count] : root_specializations) {
                const auto found =
                    stats.call_root_specializations.find(member_root);
                const auto committed =
                    found == stats.call_root_specializations.end()
                        ? 0U
                        : found->second;
                batch_specialization_fits &=
                    count <= specialization_limit -
                                 std::min(committed, specialization_limit);
            }
            batch_preflight_ok &=
                members.size() == exact_tie_size &&
                direct_batch_preflight_ok && batch_cumulative_fits &&
                batch_specialization_fits &&
                batch_reject == pass::cost_model::RejectReason::None;
            const auto report_size_before_tie =
                stats.cost_model_report == nullptr
                    ? 0
                    : stats.cost_model_report->decisions.size();
            if (batch_preflight_ok) {
                for (auto &member : members) {
                    if (!cost_model_allows_transform(stats, member.estimate)) {
                        batch_preflight_ok = false;
                        break;
                    }
                }
            }
            if (batch_preflight_ok) {
                std::uint64_t commit_plan_work = 0;
                for (const auto &member : members) {
                    commit_plan_work = saturating_u64_add(
                        commit_plan_work,
                        specialization_plan_work_units(
                            member.residual_info));
                }
                if (!specialization_work_has_capacity(
                        stats, 0, commit_plan_work)) {
                    if (stats.cost_model_report != nullptr &&
                        stats.cost_model_report->decisions.size() >
                            report_size_before_tie) {
                        stats.cost_model_report->decisions.resize(
                            report_size_before_tie);
                    }
                    (void)require_specialization_work_capacity(
                        stats, 0, commit_plan_work,
                        structural_callsite_fingerprint(
                            site.decision_key),
                        "direct residual tie commit plans would exceed the persistent specialization work budget");
                    return false;
                }
            }
            if (!batch_preflight_ok) {
                if (stats.cost_model_report != nullptr &&
                    stats.cost_model_report->decisions.size() >
                        report_size_before_tie) {
                    stats.cost_model_report->decisions.resize(
                        report_size_before_tie);
                }
                auto rejected = estimate;
                rejected.scope = "direct-residual-tie-batch";
                rejected.proof_summary =
                    "residual tie batch rejected atomically by all-member preflight";
                if (!batch_growth_fit.fits()) {
                    append_call_growth_budget_summary(
                        rejected.proof_summary,
                        CallGrowthClass::Specialization,
                        batch_growth_fit);
                }
                rejected.forced_reject_reason =
                    batch_reject != pass::cost_model::RejectReason::None
                        ? batch_reject
                        : (!batch_specialization_fits
                               ? pass::cost_model::RejectReason::CodeGrowthTooHigh
                               : (!batch_cumulative_fits
                               ? pass::cost_model::RejectReason::CumulativeBudgetExhausted
                               : pass::cost_model::RejectReason::CommitPreflightFailed));
                (void)cost_model_allows_transform(stats, rejected);
                rejected_pe_ties.insert(site.decision_key);
                assert_test_rejection_snapshot();
                continue;
            }
            std::vector<DetachedInlineRequest> tie_requests;
            tie_requests.reserve(members.size());
            for (std::size_t index = 0; index < members.size(); ++index) {
                auto &member = members[index];
                const auto member_demand =
                    member.site->call->type()->is_void() ||
                            member.site->call->uses().empty()
                        ? ReturnDemandMask::Dead
                        : ReturnDemandMask::Scalar;
                tie_requests.push_back(
                    {member.site->caller, member.site->call->parent(),
                     member.site->call, &member.scratch, &member.site->mask,
                     member_demand,
                     direct_inline_index + static_cast<unsigned>(index) + 1});
            }
            std::vector<oir::FunctionID> tie_roots;
            tie_roots.reserve(root_growth.size());
            for (const auto &[member_root, growth] : root_growth) {
                (void)growth;
                tie_roots.push_back(member_root);
            }
            CallGrowthReservation growth_reservation;
            const bool growth_slots_ready = reserve_call_growth_entries(
                stats, CallGrowthClass::Specialization, tie_roots,
                growth_reservation);
            if (!growth_slots_ready ||
                !inline_detached_residual_batch(module, direct_inline_context,
                                                tie_requests, &stats)) {
                erase_reserved_call_growth_entries(
                    stats, CallGrowthClass::Specialization,
                    growth_reservation);
                if (stats.cost_model_report != nullptr &&
                    stats.cost_model_report->decisions.size() >
                        report_size_before_tie) {
                    stats.cost_model_report->decisions.resize(
                        report_size_before_tie);
                }
                auto rejected = estimate;
                rejected.scope = "direct-residual-tie-batch";
                rejected.proof_summary =
                    "residual tie batch commit failed; all live snapshots including function table restored";
                rejected.forced_reject_reason =
                    pass::cost_model::RejectReason::CommitPreflightFailed;
                (void)cost_model_allows_transform(stats, rejected);
                rejected_pe_ties.insert(site.decision_key);
                assert_test_rejection_snapshot();
                continue;
            }
            direct_inline_index += static_cast<unsigned>(members.size());
            commit_call_growth(
                stats, CallGrowthClass::Specialization, batch_growth,
                root_growth);
            for (const auto &[member_root, count] : root_specializations) {
                stats.call_root_specializations.at(member_root) += count;
            }
            add_call_pressure(stats.cumulative_call_pressure, batch_pressure);
            stats.inlined += static_cast<unsigned>(members.size());
            stats.specialized += static_cast<unsigned>(members.size());
            run_bounded_affected_cleanup(module, direct_inline_context, stats);
            return true;
        }
        if (!direct_residual) {
            estimate.proof_status = pass::cost_model::ProofStatus::Unknown;
            estimate.proof_summary =
                "detached residual has no legal direct commit; persistent clone requires "
                "measured batch reuse";
            if (!scratch.detail.empty()) {
                estimate.proof_summary += "; " + scratch.detail;
            }
            (void)cost_model_allows_transform(stats, estimate);
            assert_test_rejection_snapshot();
            continue;
        }
        const auto report_size_before_commit =
            stats.cost_model_report == nullptr
                ? 0
                : stats.cost_model_report->decisions.size();
        if (!cost_model_allows_transform(stats, estimate)) {
            assert_test_rejection_snapshot();
            continue;
        }
        const auto direct_plan_work =
            specialization_plan_work_units(residual_info);
        if (!specialization_work_has_capacity(stats, 0,
                                               direct_plan_work)) {
            if (stats.cost_model_report != nullptr &&
                stats.cost_model_report->decisions.size() >
                    report_size_before_commit) {
                stats.cost_model_report->decisions.resize(
                    report_size_before_commit);
            }
            (void)require_specialization_work_capacity(
                stats, 0, direct_plan_work,
                structural_callsite_fingerprint(site.decision_key),
                "direct residual commit plan would exceed the persistent specialization work budget");
            return false;
        }
        CallGrowthReservation growth_reservation;
        const std::vector<oir::FunctionID> roots{root_id};
        if (!reserve_call_growth_entries(
                stats, CallGrowthClass::Specialization, roots,
                growth_reservation)) {
            if (stats.cost_model_report != nullptr &&
                stats.cost_model_report->decisions.size() >
                    report_size_before_commit) {
                stats.cost_model_report->decisions.resize(
                    report_size_before_commit);
            }
            estimate.forced_reject_reason =
                pass::cost_model::RejectReason::CommitPreflightFailed;
            estimate.proof_summary =
                "direct residual growth-accounting preflight failed";
            (void)cost_model_allows_transform(stats, estimate);
            continue;
        }
        bool committed = false;
        for (auto &block : site.caller->blocks()) {
            for (auto it = block->instructions().begin(); it != block->instructions().end();
                 ++it) {
                if (it->get() != site.call) {
                    continue;
                }
                committed = inline_detached_residual(
                    module, direct_inline_context, *site.caller, block.get(), it, scratch,
                    site.mask, return_demand, direct_inline_index + 1,
                    &stats);
                break;
            }
            if (committed) {
                break;
            }
        }
        if (!committed) {
            erase_reserved_call_growth_entries(
                stats, CallGrowthClass::Specialization,
                growth_reservation);
            if (stats.cost_model_report != nullptr &&
                stats.cost_model_report->decisions.size() > report_size_before_commit) {
                stats.cost_model_report->decisions.resize(report_size_before_commit);
            }
            estimate.forced_reject_reason =
                pass::cost_model::RejectReason::CommitPreflightFailed;
            estimate.proof_summary =
                "direct residual commit verification failed; live snapshot including function table restored";
            (void)cost_model_allows_transform(stats, estimate);
            assert_test_rejection_snapshot();
            continue;
        }
        ++direct_inline_index;
        commit_call_growth(stats, CallGrowthClass::Specialization,
                           root_id, committed_growth);
        ++stats.call_root_specializations.at(root_id);
        add_call_pressure(stats.cumulative_call_pressure, committed_pressure);
        ++stats.inlined;
        ++stats.specialized;
        run_bounded_affected_cleanup(module, direct_inline_context, stats);
        return true;
    }
    return false;
}

bool specialize_constant_argument_calls(oir::Module &module, Stats &stats) {
    return run_transactional_inline_transform(
        module, stats, "constant-residual-affected-cleanup",
        [](oir::Module &staged_module, Stats &staged_stats) {
            return specialize_constant_argument_calls_impl(staged_module,
                                                             staged_stats);
        });
}

bool inline_functions_impl(oir::Module &module, Stats &stats) {
    bool changed = truncate_constant_fixed_point_suffixes(module, stats);
    if (stats.call_cleanup_budget_exhausted) {
        return changed;
    }
    unsigned inline_index = stats.inlined;
    InlineContext context;
    const auto policy = pass::cost_model::policy_for_kind(stats.cost_model_policy);
    initialize_call_growth_budget(module, stats, policy);

    struct Candidate {
        oir::Function *caller = nullptr;
        oir::CallInst *call = nullptr;
        std::int64_t score = 0;
        std::string structural_key;
        bool recursive = false;
    };

    auto locate_call = [](oir::Function &caller, oir::CallInst *target,
                          oir::BasicBlock *&block,
                          std::list<std::unique_ptr<oir::Instruction>>::iterator &position) {
        for (auto &candidate_block : caller.blocks()) {
            for (auto it = candidate_block->instructions().begin();
                 it != candidate_block->instructions().end(); ++it) {
                if (it->get() == target) {
                    block = candidate_block.get();
                    position = it;
                    return true;
                }
            }
        }
        return false;
    };

    for (unsigned window = 0; window < kMaxInlineRounds * kMaxInlineSites &&
                              inline_index < kMaxInlineSites;
         ++window) {
        std::vector<Candidate> candidates;
        StructuralKeyCache structural_keys;
        for (auto &function : module.functions()) {
            if (function->is_external()) {
                continue;
            }
            const auto reachable = reachable_block_set(*function);
            for (auto &block : function->blocks()) {
                if (reachable.find(block.get()) == reachable.end()) {
                    continue;
                }
                for (auto &instruction : block->instructions()) {
                    auto *call = dynamic_cast<oir::CallInst *>(instruction.get());
                    if (call == nullptr) {
                        continue;
                    }
                    auto *callee = dynamic_cast<oir::Function *>(call->callee());
                    const bool recursive = callee == function.get();
                    if (recursive && context.defer_recursive_expansion.find(function.get()) !=
                                         context.defer_recursive_expansion.end()) {
                        continue;
                    }
                    candidates.push_back(
                        {function.get(), call,
                         callsite_score(module, context, *function, *call, callee),
                         structural_callsite_key(*function, *call, callee,
                                                 &structural_keys), recursive});
                }
            }
        }
        if (candidates.empty()) {
            break;
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate &lhs, const Candidate &rhs) {
                      if (lhs.score != rhs.score) {
                          return lhs.score > rhs.score;
                      }
                      return lhs.structural_key < rhs.structural_key;
                  });

        bool window_changed = false;
        for (std::size_t begin = 0; begin < candidates.size() && !window_changed;) {
            std::size_t end = begin + 1;
            // Direct recursion has its own depth, frequency, pressure and growth
            // accounting.  It must advance one layer at a time instead of being
            // coalesced into the ordinary detached-residual tie transaction.
            while (end < candidates.size() && !candidates[begin].recursive &&
                   !candidates[end].recursive &&
                   candidates[end].score == candidates[begin].score &&
                   candidates[end].structural_key == candidates[begin].structural_key) {
                ++end;
            }
            if (candidates[begin].recursive) {
                auto &candidate = candidates[begin];
                oir::BasicBlock *block = nullptr;
                std::list<std::unique_ptr<oir::Instruction>>::iterator position;
                if (locate_call(*candidate.caller, candidate.call, block,
                                position) &&
                    inline_call(module, context, *candidate.caller, block,
                                position, inline_index + 1, stats, false,
                                false)) {
                    ++inline_index;
                    ++stats.inlined;
                    changed = true;
                    window_changed = true;
                }
                begin = end;
                continue;
            }
            std::uint64_t batch_growth = 0;
            std::unordered_map<oir::FunctionID, std::uint64_t> batch_root_growth;
            CallPressureVector batch_pressure;
            for (std::size_t index = begin; index < end; ++index) {
                auto *callee = dynamic_cast<oir::Function *>(candidates[index].call->callee());
                if (callee == nullptr || callee->is_external()) {
                    continue;
                }
                const auto growth = static_cast<std::uint64_t>(
                    cloned_instruction_growth(inspect_callee(*callee)));
                batch_growth = saturating_u64_add(batch_growth, growth);
                auto &root_growth = batch_root_growth[callee->root_function_id()];
                root_growth = saturating_u64_add(root_growth, growth);
                add_call_pressure(
                    batch_pressure,
                    estimate_call_pressure(inspect_callee(*callee),
                                           *candidates[index].call));
            }
            const auto batch_pressure_reject = pressure_reject_reason(
                stats.cumulative_call_pressure, batch_pressure,
                stats.call_pressure_budget);
            const auto batch_growth_fit = call_growth_budget_fit(
                stats, CallGrowthClass::Ordinary, batch_growth,
                batch_root_growth);
            bool batch_fits =
                batch_growth_fit.fits() &&
                batch_pressure_reject == pass::cost_model::RejectReason::None &&
                end - begin <= kMaxInlineSites - inline_index;
            if (!batch_fits) {
                for (std::size_t index = begin; index < end; ++index) {
                    auto *callee =
                        dynamic_cast<oir::Function *>(candidates[index].call->callee());
                    if (callee == nullptr || callee->is_external()) {
                        continue;
                    }
                    OIRTransformCostEstimate rejected;
                    rejected.kind = pass::cost_model::TransformKind::Inline;
                    rejected.pass_name = "OIRInlinePass";
                    rejected.candidate_id =
                        "inline." + std::to_string(inline_index + index - begin + 1);
                    rejected.scope = "structural-tie-batch";
                    rejected.proof_kind = pass::cost_model::ProofKind::Structural;
                    rejected.proof_rule_id = structural_callsite_fingerprint(
                        candidates[index].structural_key);
                    rejected.proof_summary =
                        "structural tie batch rejected atomically";
                    if (!batch_growth_fit.fits()) {
                        append_call_growth_budget_summary(
                            rejected.proof_summary,
                            CallGrowthClass::Ordinary,
                            batch_growth_fit);
                    } else if (batch_pressure_reject !=
                               pass::cost_model::RejectReason::None) {
                        rejected.proof_summary +=
                            "; cumulative call pressure budget exhausted";
                    } else {
                        rejected.proof_summary +=
                            "; inline site cap exhausted";
                    }
                    rejected.confidence = 0.65;
                    fill_before_after_from_callee(rejected, inspect_callee(*callee), 1, 0);
                    rejected.forced_reject_reason =
                        batch_pressure_reject != pass::cost_model::RejectReason::None
                            ? batch_pressure_reject
                            : (end - begin > kMaxInlineSites - inline_index
                                   ? pass::cost_model::RejectReason::CompileTimeTooHigh
                                   : pass::cost_model::RejectReason::CumulativeBudgetExhausted);
                    (void)cost_model_allows_transform(stats, rejected);
                }
                begin = end;
                continue;
            }
            if (end - begin > 1) {
                struct OrdinaryTieMember {
                    Candidate *candidate = nullptr;
                    SpecializationMask mask;
                    ReturnDemandMask return_demand = ReturnDemandMask::Scalar;
                    ScratchResidual scratch;
                };

                const auto report_size_before_batch =
                    stats.cost_model_report == nullptr
                        ? 0
                        : stats.cost_model_report->decisions.size();
                auto reject_batch = [&](pass::cost_model::RejectReason reason,
                                        const std::string &summary) {
                    if (stats.cost_model_report != nullptr &&
                        stats.cost_model_report->decisions.size() >
                            report_size_before_batch) {
                        stats.cost_model_report->decisions.resize(
                            report_size_before_batch);
                    }
                    for (std::size_t index = begin; index < end; ++index) {
                        auto *callee = dynamic_cast<oir::Function *>(
                            candidates[index].call->callee());
                        if (callee == nullptr || callee->is_external()) {
                            continue;
                        }
                        OIRTransformCostEstimate rejected;
                        rejected.kind = pass::cost_model::TransformKind::Inline;
                        rejected.pass_name = "OIRInlinePass";
                        rejected.candidate_id =
                            "inline." +
                            std::to_string(inline_index + index - begin + 1);
                        rejected.scope = "structural-tie-batch";
                        rejected.proof_kind =
                            pass::cost_model::ProofKind::Structural;
                        rejected.proof_rule_id = structural_callsite_fingerprint(
                            candidates[index].structural_key);
                        rejected.proof_summary = summary;
                        rejected.forced_reject_reason = reason;
                        fill_before_after_from_callee(
                            rejected, inspect_callee(*callee), 1, 0);
                        (void)cost_model_allows_transform(stats, rejected);
                    }
                };

                bool batch_preflight_ok = true;
                for (std::size_t index = begin; index < end; ++index) {
                    auto &candidate = candidates[index];
                    auto *callee = dynamic_cast<oir::Function *>(
                        candidate.call->callee());
                    auto *call_block = candidate.call->parent();
                    if (candidate.recursive || callee == nullptr ||
                        callee->is_external() || call_block == nullptr) {
                        batch_preflight_ok = false;
                        break;
                    }
                }

                if (batch_preflight_ok) {
                    for (std::size_t index = begin; index < end; ++index) {
                        oir::BasicBlock *block = nullptr;
                        std::list<std::unique_ptr<oir::Instruction>>::iterator
                            position;
                        auto &candidate = candidates[index];
                        if (!locate_call(*candidate.caller, candidate.call,
                                         block, position) ||
                            !inline_call(module, context, *candidate.caller,
                                         block, position,
                                         inline_index + index - begin + 1,
                                         stats, true, false)) {
                            batch_preflight_ok = false;
                            break;
                        }
                    }
                }

                std::vector<OrdinaryTieMember> members;
                members.reserve(end - begin);
                if (batch_preflight_ok) {
                    for (std::size_t index = begin; index < end; ++index) {
                        auto &candidate = candidates[index];
                        auto *callee = dynamic_cast<oir::Function *>(
                            candidate.call->callee());
                        SpecializationMask mask(candidate.call->args().size(),
                                                false);
                        const auto return_demand =
                            candidate.call->type()->is_void() ||
                                    candidate.call->uses().empty()
                                ? ReturnDemandMask::Dead
                                : ReturnDemandMask::Scalar;
                        auto scratch = build_detached_constant_residual(
                            module, *callee, *candidate.call, mask,
                            return_demand);
                        if (scratch.status != ScratchResidualStatus::Success ||
                            scratch.function == nullptr ||
                            !detached_residual_commit_is_supported(
                                scratch, *candidate.call, mask,
                                return_demand)) {
                            batch_preflight_ok = false;
                            break;
                        }
                        auto staged_plan = build_detached_inline_plan(
                            module, *candidate.caller, *candidate.call, scratch,
                            mask, return_demand,
                            inline_index + static_cast<unsigned>(members.size()) +
                                1);
                        if (!staged_plan) {
                            batch_preflight_ok = false;
                            break;
                        }
                        members.push_back({&candidate, std::move(mask),
                                           return_demand,
                                           std::move(scratch)});
                    }
                }
                if (!batch_preflight_ok || members.size() != end - begin) {
                    reject_batch(
                        pass::cost_model::RejectReason::CommitPreflightFailed,
                        "structural tie batch failed detached all-member commit preflight");
                    begin = end;
                    continue;
                }

                std::vector<DetachedInlineRequest> requests;
                requests.reserve(members.size());
                for (std::size_t index = 0; index < members.size(); ++index) {
                    auto &member = members[index];
                    requests.push_back(
                        {member.candidate->caller,
                         member.candidate->call->parent(),
                         member.candidate->call, &member.scratch, &member.mask,
                         member.return_demand,
                         inline_index + static_cast<unsigned>(index) + 1});
                }

                std::vector<oir::FunctionID> roots;
                roots.reserve(batch_root_growth.size());
                for (const auto &[root, growth] : batch_root_growth) {
                    (void)growth;
                    roots.push_back(root);
                }
                CallGrowthReservation growth_reservation;
                std::vector<const oir::Function *> inserted_deferred;
                bool metadata_ready = reserve_call_growth_entries(
                    stats, CallGrowthClass::Ordinary, roots,
                    growth_reservation);
                if (metadata_ready) {
                    try {
                        inserted_deferred.reserve(members.size());
                        for (const auto &member : members) {
                            if (context.defer_recursive_expansion
                                    .insert(member.candidate->caller)
                                    .second) {
                                inserted_deferred.push_back(
                                    member.candidate->caller);
                            }
                        }
                    } catch (...) {
                        metadata_ready = false;
                    }
                }
                if (!metadata_ready) {
                    for (auto *function : inserted_deferred) {
                        context.defer_recursive_expansion.erase(function);
                    }
                    erase_reserved_call_growth_entries(
                        stats, CallGrowthClass::Ordinary,
                        growth_reservation);
                    reject_batch(
                        pass::cost_model::RejectReason::CommitPreflightFailed,
                        "structural tie batch accounting preflight failed");
                    begin = end;
                    continue;
                }

                if (!inline_detached_residual_batch(module, context, requests)) {
                    for (auto *function : inserted_deferred) {
                        context.defer_recursive_expansion.erase(function);
                    }
                    erase_reserved_call_growth_entries(
                        stats, CallGrowthClass::Ordinary,
                        growth_reservation);
                    reject_batch(
                        pass::cost_model::RejectReason::CommitPreflightFailed,
                        "structural tie batch commit failed; every live snapshot was restored");
                    begin = end;
                    continue;
                }

                commit_call_growth(stats, CallGrowthClass::Ordinary,
                                   batch_growth, batch_root_growth);
                add_call_pressure(stats.cumulative_call_pressure,
                                  batch_pressure);
                inline_index += static_cast<unsigned>(members.size());
                stats.inlined += static_cast<unsigned>(members.size());
                changed = true;
                window_changed = true;
                begin = end;
                continue;
            }
            for (std::size_t index = begin;
                 index < end && inline_index < kMaxInlineSites; ++index) {
                auto &candidate = candidates[index];
                oir::BasicBlock *block = nullptr;
                std::list<std::unique_ptr<oir::Instruction>>::iterator position;
                if (!locate_call(*candidate.caller, candidate.call, block, position)) {
                    continue;
                }
                if (!inline_call(module, context, *candidate.caller, block, position,
                                 inline_index + 1, stats, false,
                                 false)) {
                    continue;
                }
                if (!candidate.recursive) {
                    context.defer_recursive_expansion.insert(candidate.caller);
                }
                ++inline_index;
                ++stats.inlined;
                changed = true;
                window_changed = true;
            }
            begin = end;
        }
        if (!window_changed) {
            break;
        }
        run_bounded_affected_cleanup(module, context, stats);
        if (context.cleanup_budget_exhausted) {
            break;
        }
    }

    for (const auto &[function, growth] : context.recursive_growth) {
        if (growth != 0) {
            stats.recursively_inlined_functions.insert(function);
        }
    }

    return changed;
}

bool inline_functions(oir::Module &module, Stats &stats) {
    return run_transactional_inline_transform(
        module, stats, "ordinary-inline-affected-cleanup",
        [](oir::Module &staged_module, Stats &staged_stats) {
            return inline_functions_impl(staged_module, staged_stats);
        });
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
