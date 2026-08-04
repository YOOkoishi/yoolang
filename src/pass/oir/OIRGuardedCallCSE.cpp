#include "oir/OIRScalarOpt.h"

#include "oir/OIRAnalysis.h"
#include "oir/OIRCFGUtils.h"
#include "pass/oir/OIRCostModel.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pass::oir_opt {
namespace {

constexpr unsigned kMaxGuardedCallTransforms = 64;
constexpr unsigned kMaxGuardedCallCandidates = 256;
constexpr std::size_t kMaxLeadersPerCallee = 16;

struct ArgumentEquality {
    oir::Value *lhs = nullptr;
    oir::Value *rhs = nullptr;
};

struct InstructionMix {
    std::int64_t instructions = 0;
    std::int64_t int_alu = 0;
    std::int64_t int_mul = 0;
    std::int64_t int_div_rem = 0;
    std::int64_t fp_alu = 0;
    std::int64_t fp_div = 0;
    std::int64_t loads = 0;
    std::int64_t stores = 0;
    std::int64_t pointer_arith = 0;
    std::int64_t branches = 0;
    std::int64_t calls = 0;
    std::int64_t phis = 0;
};

struct GuardedCallCandidate {
    oir::Function *caller = nullptr;
    oir::BasicBlock *block = nullptr;
    oir::CallInst *leader = nullptr;
    oir::CallInst *duplicate = nullptr;
    oir::Function *callee = nullptr;
    std::vector<ArgumentEquality> equalities;
    std::int64_t callee_cost = 0;
    std::int64_t module_instructions = 0;
    InstructionMix callee_mix;
    bool recursive = false;
};

struct CallPair {
    const oir::CallInst *leader = nullptr;
    const oir::CallInst *duplicate = nullptr;

    bool operator==(const CallPair &other) const {
        return leader == other.leader && duplicate == other.duplicate;
    }
};

struct CallPairHash {
    std::size_t operator()(const CallPair &pair) const {
        const auto lhs = std::hash<const void *>{}(pair.leader);
        const auto rhs = std::hash<const void *>{}(pair.duplicate);
        return lhs ^ (rhs + 0x9e3779b9U + (lhs << 6U) + (lhs >> 2U));
    }
};

using RejectedPairs = std::unordered_set<CallPair, CallPairHash>;

bool same_integer_constant(oir::Value *lhs, oir::Value *rhs) {
    auto lhs_value = int_constant(lhs);
    auto rhs_value = int_constant(rhs);
    return lhs_value.has_value() && rhs_value.has_value() && *lhs_value == *rhs_value;
}

bool add_unique_equality(std::vector<ArgumentEquality> &equalities, oir::Value *lhs,
                         oir::Value *rhs) {
    for (const auto &equality : equalities) {
        if ((equality.lhs == lhs && equality.rhs == rhs) ||
            (equality.lhs == rhs && equality.rhs == lhs)) {
            return true;
        }
    }
    equalities.push_back({lhs, rhs});
    return true;
}

bool collect_integer_equalities(const oir::CallInst &leader, const oir::CallInst &duplicate,
                                std::vector<ArgumentEquality> &equalities) {
    const auto leader_args = leader.args();
    const auto duplicate_args = duplicate.args();
    if (leader_args.size() != duplicate_args.size()) {
        return false;
    }

    equalities.clear();
    for (std::size_t index = 0; index < leader_args.size(); ++index) {
        auto *lhs = leader_args[index];
        auto *rhs = duplicate_args[index];
        if (lhs == nullptr || rhs == nullptr || lhs->type() == nullptr ||
            lhs->type() != rhs->type()) {
            return false;
        }
        // Undef may choose a different value at each use.  Even the same UndefValue
        // object is therefore not a proof that two dynamic call arguments agree.
        if (dynamic_cast<oir::UndefValue *>(lhs) != nullptr ||
            dynamic_cast<oir::UndefValue *>(rhs) != nullptr) {
            return false;
        }
        if (lhs == rhs || same_integer_constant(lhs, rhs)) {
            continue;
        }
        if (!lhs->type()->is_integer()) {
            return false;
        }

        // Unequal integer constants can never select the reuse path, so adding a
        // permanently-false guard would only grow and slow the program.
        if (int_constant(lhs).has_value() && int_constant(rhs).has_value()) {
            return false;
        }
        add_unique_equality(equalities, lhs, rhs);
    }
    return !equalities.empty();
}

std::int64_t instruction_cost(const oir::Instruction &inst,
                              const pass::cost_model::TargetCostProfile &target) {
    switch (inst.op()) {
    case oir::Instruction::OpID::Mul:
        return target.mul_i32;
    case oir::Instruction::OpID::SDiv:
    case oir::Instruction::OpID::SRem:
        return target.div_i32;
    case oir::Instruction::OpID::FMul:
        return target.fp_mul;
    case oir::Instruction::OpID::FDiv:
        return target.fp_div;
    case oir::Instruction::OpID::FAdd:
    case oir::Instruction::OpID::FSub:
        return target.fp_add;
    case oir::Instruction::OpID::Load:
        return target.load;
    case oir::Instruction::OpID::Store:
    case oir::Instruction::OpID::MemZero:
        return target.store;
    case oir::Instruction::OpID::Call:
        return target.call;
    case oir::Instruction::OpID::Br:
        return target.branch;
    case oir::Instruction::OpID::GetElementPtr:
        return target.alu_i64;
    case oir::Instruction::OpID::Ret:
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::And:
    case oir::Instruction::OpID::Xor:
    case oir::Instruction::OpID::ICmp:
    case oir::Instruction::OpID::FCmp:
    case oir::Instruction::OpID::Alloca:
    case oir::Instruction::OpID::ZExt:
    case oir::Instruction::OpID::SIToFP:
    case oir::Instruction::OpID::FPToSI:
    case oir::Instruction::OpID::Phi:
        return target.alu_i32;
    }
    return target.alu_i32;
}

std::int64_t function_cost(const oir::Function &function,
                           const pass::cost_model::TargetCostProfile &target) {
    std::int64_t cost = 0;
    for (const auto &block : function.blocks()) {
        for (const auto &inst : block->instructions()) {
            cost += instruction_cost(*inst, target);
        }
    }
    return std::max<std::int64_t>(1, cost);
}

InstructionMix function_instruction_mix(const oir::Function &function) {
    InstructionMix mix;
    for (const auto &block : function.blocks()) {
        for (const auto &inst : block->instructions()) {
            ++mix.instructions;
            switch (inst->op()) {
            case oir::Instruction::OpID::Mul:
                ++mix.int_mul;
                break;
            case oir::Instruction::OpID::SDiv:
            case oir::Instruction::OpID::SRem:
                ++mix.int_div_rem;
                break;
            case oir::Instruction::OpID::FMul:
            case oir::Instruction::OpID::FAdd:
            case oir::Instruction::OpID::FSub:
                ++mix.fp_alu;
                break;
            case oir::Instruction::OpID::FDiv:
                ++mix.fp_div;
                break;
            case oir::Instruction::OpID::Load:
                ++mix.loads;
                break;
            case oir::Instruction::OpID::Store:
            case oir::Instruction::OpID::MemZero:
                ++mix.stores;
                break;
            case oir::Instruction::OpID::Call:
                ++mix.calls;
                break;
            case oir::Instruction::OpID::Br:
                ++mix.branches;
                break;
            case oir::Instruction::OpID::GetElementPtr:
                ++mix.pointer_arith;
                break;
            case oir::Instruction::OpID::Phi:
                ++mix.phis;
                break;
            case oir::Instruction::OpID::Ret:
            case oir::Instruction::OpID::Add:
            case oir::Instruction::OpID::Sub:
            case oir::Instruction::OpID::And:
            case oir::Instruction::OpID::Xor:
            case oir::Instruction::OpID::ICmp:
            case oir::Instruction::OpID::FCmp:
            case oir::Instruction::OpID::Alloca:
            case oir::Instruction::OpID::ZExt:
            case oir::Instruction::OpID::SIToFP:
            case oir::Instruction::OpID::FPToSI:
                ++mix.int_alu;
                break;
            }
        }
    }
    return mix;
}

std::int64_t module_instruction_count(const oir::Module &module) {
    std::int64_t count = 0;
    for (const auto &function : module.functions()) {
        for (const auto &block : function->blocks()) {
            count += static_cast<std::int64_t>(block->instructions().size());
        }
    }
    return std::max<std::int64_t>(1, count);
}

bool is_directly_recursive(const oir::Function &function) {
    for (const auto &block : function.blocks()) {
        for (const auto &inst : block->instructions()) {
            auto *call = dynamic_cast<const oir::CallInst *>(inst.get());
            if (call != nullptr && call->callee() == &function) {
                return true;
            }
        }
    }
    return false;
}

bool passes_structural_profitability(const GuardedCallCandidate &candidate) {
    const auto equality_count = static_cast<std::int64_t>(candidate.equalities.size());
    const std::int64_t guard_cost = equality_count + std::max<std::int64_t>(0, equality_count - 1) +
                                    4; // compare/and, branch, two jumps, and phi
    if (candidate.recursive) {
        return candidate.equalities.size() <= 4 && candidate.callee_cost >= guard_cost;
    }

    // Without profile data, require a substantial static body before speculating
    // that runtime argument equality will pay for the extra control flow.
    return candidate.equalities.size() <= 3 && candidate.callee_cost >= guard_cost * 4;
}

bool cost_model_allows_candidate(Stats &stats, const GuardedCallCandidate &candidate) {
    const auto equality_count = static_cast<std::int64_t>(candidate.equalities.size());
    const std::int64_t and_count = std::max<std::int64_t>(0, equality_count - 1);
    const std::int64_t guard_instrs = equality_count + and_count + 4;

    OIRTransformCostEstimate estimate;
    estimate.kind = pass::cost_model::TransformKind::GlobalCSE;
    estimate.pass_name = "OIRGuardedCallCSE";
    estimate.candidate_id = "guarded-call." + std::to_string(++stats.cost_model_candidates);
    estimate.scope = candidate.recursive ? "same-block-recursive-call" : "same-block-readonly-call";
    estimate.proof_kind = pass::cost_model::ProofKind::Composite;
    estimate.proof_status = pass::cost_model::ProofStatus::Proven;
    estimate.proof_summary =
        "same direct callee, nonwriting call with no pointer or unknown reads, "
        "identical MemorySSA snapshot, and exact integer argument equality guard";
    estimate.proof_obligations = static_cast<std::int64_t>(candidate.equalities.size()) + 3;
    estimate.confidence = candidate.recursive ? 0.74 : 0.62;
    estimate.has_detailed_instruction_mix = true;

    // With no value profile, model one successful reuse over four ordinary
    // calls (two for the recursive pattern).  The other calls occur on both
    // sides of the comparison and cancel, leaving one saved call/body against
    // the guard cost paid on every invocation in the horizon.
    const std::int64_t guard_horizon = candidate.recursive ? 2 : 4;
    estimate.before_instrs = candidate.module_instructions;
    estimate.after_instrs = candidate.module_instructions + guard_instrs;
    estimate.before_int_alu = candidate.callee_mix.int_alu;
    estimate.before_int_mul = candidate.callee_mix.int_mul;
    estimate.before_int_div_rem = candidate.callee_mix.int_div_rem;
    estimate.before_fp_alu = candidate.callee_mix.fp_alu;
    estimate.before_fp_div = candidate.callee_mix.fp_div;
    estimate.before_loads = candidate.callee_mix.loads;
    estimate.before_stores = candidate.callee_mix.stores;
    estimate.before_pointer_arith = candidate.callee_mix.pointer_arith;
    estimate.before_branches = candidate.callee_mix.branches;
    estimate.before_calls = candidate.callee_mix.calls + 1;
    estimate.before_phis = candidate.callee_mix.phis;
    estimate.after_int_alu = (equality_count + and_count) * guard_horizon;
    estimate.after_branches = 2 * guard_horizon;
    estimate.after_phis = guard_horizon;
    estimate.risk.code_growth = guard_instrs;
    estimate.risk.live_range_growth = 1;
    estimate.risk.register_pressure_growth = equality_count > 1 ? 1 : 0;
    estimate.risk.branch_predictability_loss = candidate.recursive ? 1 : 2;
    estimate.risk.cleanup_dependency = 1;
    return cost_model_allows_transform(stats, estimate);
}

bool is_candidate_call(const oir::CallInst &call, const oir::FunctionModRefAnalysis &modref) {
    auto *callee = dynamic_cast<oir::Function *>(call.callee());
    if (callee == nullptr || callee->is_external() || call.type() == nullptr ||
        call.type()->is_void() || modref.call_has_side_effect(call) ||
        modref.call_may_write_memory(call)) {
        return false;
    }

    // MemorySSA can precisely separate explicitly named global reads.  A
    // pointer-parameter summary currently records only the parameter index,
    // not the callee's reachable byte range, so it is not a sufficient proof
    // for speculative result reuse.  Fail closed until range-aware footprints
    // are available.  This still admits memory-free and tracked-global readers.
    const auto &summary = modref.summary(callee);
    return !summary.reads_all && !summary.reads_unknown && summary.read_param_indices.empty();
}

std::optional<GuardedCallCandidate>
find_candidate_in_function(oir::Function &function, const oir::FunctionModRefAnalysis &modref,
                           const pass::cost_model::TargetCostProfile &target,
                           std::int64_t module_instructions, const RejectedPairs &rejected) {
    if (function.is_external() || function.entry_block() == nullptr) {
        return std::nullopt;
    }

    oir::OIRAliasAnalysis alias_analysis;
    oir::MemorySSA memory_ssa(function, alias_analysis, modref);
    std::unordered_map<oir::Function *, std::vector<oir::CallInst *>> available;

    for (auto &block : function.blocks()) {
        available.clear();
        for (auto &inst : block->instructions()) {
            auto *call = dynamic_cast<oir::CallInst *>(inst.get());
            if (call == nullptr || !is_candidate_call(*call, modref)) {
                continue;
            }

            auto *callee = static_cast<oir::Function *>(call->callee());
            auto &leaders = available[callee];
            for (auto leader_it = leaders.rbegin(); leader_it != leaders.rend(); ++leader_it) {
                auto *leader = *leader_it;
                if (rejected.find({leader, call}) != rejected.end()) {
                    continue;
                }
                if (leader->type() != call->type() ||
                    memory_ssa.clobbering_access(*leader) != memory_ssa.clobbering_access(*call)) {
                    continue;
                }

                GuardedCallCandidate candidate;
                candidate.caller = &function;
                candidate.block = block.get();
                candidate.leader = leader;
                candidate.duplicate = call;
                candidate.callee = callee;
                if (!collect_integer_equalities(*leader, *call, candidate.equalities)) {
                    continue;
                }
                candidate.callee_cost = function_cost(*callee, target);
                candidate.module_instructions = module_instructions;
                candidate.callee_mix = function_instruction_mix(*callee);
                candidate.recursive = callee == &function || is_directly_recursive(*callee);
                if (passes_structural_profitability(candidate)) {
                    return candidate;
                }
            }
            leaders.push_back(call);
            if (leaders.size() > kMaxLeadersPerCallee) {
                leaders.erase(leaders.begin());
            }
        }
    }
    return std::nullopt;
}

std::optional<GuardedCallCandidate>
find_candidate(oir::Module &module, const oir::FunctionModRefAnalysis &modref,
               const pass::cost_model::TargetCostProfile &target, const RejectedPairs &rejected) {
    const auto module_instructions = module_instruction_count(module);
    for (auto &function : module.functions()) {
        if (auto candidate = find_candidate_in_function(*function, modref, target,
                                                        module_instructions, rejected)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::list<std::unique_ptr<oir::Instruction>>::iterator
find_instruction(oir::BasicBlock &block, const oir::Instruction *needle) {
    return std::find_if(
        block.instructions().begin(), block.instructions().end(),
        [needle](const std::unique_ptr<oir::Instruction> &inst) { return inst.get() == needle; });
}

void move_original_successors(oir::BasicBlock &from, oir::BasicBlock &continuation) {
    const auto successors = from.successors();
    for (auto *successor : successors) {
        oir::cfg::move_successor_edge(&from, &continuation, successor);
    }
}

bool rewrite_candidate(oir::Module &module, GuardedCallCandidate &candidate, Stats &stats,
                       unsigned transform_index) {
    auto &instructions = candidate.block->instructions();
    auto duplicate_it = find_instruction(*candidate.block, candidate.duplicate);
    if (duplicate_it == instructions.end()) {
        return false;
    }

    auto *slow_call =
        candidate.caller->create_block("guard.call.slow." + std::to_string(transform_index));
    auto *reuse =
        candidate.caller->create_block("guard.call.reuse." + std::to_string(transform_index));
    auto *continuation =
        candidate.caller->create_block("guard.call.cont." + std::to_string(transform_index));
    move_original_successors(*candidate.block, *continuation);

    auto tail_begin = std::next(duplicate_it);
    continuation->instructions().splice(continuation->instructions().end(), instructions,
                                        tail_begin, instructions.end());
    for (auto &inst : continuation->instructions()) {
        inst->set_parent(continuation);
    }

    slow_call->instructions().splice(slow_call->instructions().end(), instructions, duplicate_it);
    candidate.duplicate->set_parent(slow_call);

    oir::IRBuilder builder(&module);
    builder.set_insert_point(candidate.block);
    oir::Value *condition = nullptr;
    for (std::size_t index = 0; index < candidate.equalities.size(); ++index) {
        const auto &equality = candidate.equalities[index];
        auto *equal =
            builder.create_icmp(oir::CmpPred::EQ, equality.lhs, equality.rhs, "guard.call.eq");
        condition = condition == nullptr
                        ? static_cast<oir::Value *>(equal)
                        : static_cast<oir::Value *>(builder.create_binary(
                              oir::Instruction::OpID::And, condition, equal, "guard.call.all"));
    }
    builder.create_cond_br(condition, reuse, slow_call);

    // Keep the phi's fast incoming edge in a dedicated block.  Inlining the
    // leader call may split candidate.block and move its successor edges; the
    // stable reuse predecessor prevents that later CFG rewrite from having to
    // retarget an incoming value defined by the call being inlined.
    builder.set_insert_point(reuse);
    builder.create_br(continuation);

    builder.set_insert_point(slow_call);
    builder.create_br(continuation);

    const std::string result_name =
        candidate.duplicate->name().empty() ? "guard.call.result" : candidate.duplicate->name();
    candidate.duplicate->set_name(result_name + ".slow");
    auto phi =
        std::make_unique<oir::PhiInst>(candidate.duplicate->type(), continuation, result_name);
    auto *result = phi.get();
    candidate.duplicate->replace_all_uses_with(result);
    result->add_incoming(candidate.leader, reuse);
    result->add_incoming(candidate.duplicate, slow_call);
    continuation->instructions().push_front(std::move(phi));
    result->set_parent(continuation);

    ++stats.gvn;
    stats.cfg += 3;
    return true;
}

} // namespace

bool guard_duplicate_readonly_calls(oir::Module &module, Stats &stats) {
    bool changed = false;
    const auto target = stats.cost_model_report == nullptr
                            ? pass::cost_model::default_target_profile()
                            : stats.cost_model_report->target;
    oir::FunctionModRefAnalysis modref(module);
    RejectedPairs rejected;

    unsigned transformed = 0;
    for (unsigned evaluated = 0;
         evaluated < kMaxGuardedCallCandidates && transformed < kMaxGuardedCallTransforms;
         ++evaluated) {
        auto candidate = find_candidate(module, modref, target, rejected);
        if (!candidate.has_value()) {
            break;
        }
        if (!cost_model_allows_candidate(stats, *candidate)) {
            rejected.insert({candidate->leader, candidate->duplicate});
            continue;
        }
        if (!rewrite_candidate(module, *candidate, stats, transformed)) {
            break;
        }
        changed = true;
        ++transformed;
    }
    return changed;
}

} // namespace pass::oir_opt
