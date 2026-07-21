#include "pass/oir/OIRCostModel.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace pass::oir_opt {
namespace {

bool filter_matches(const Stats &stats, pass::cost_model::TransformKind kind,
                    const std::string &pass_name) {
    if (stats.cost_model_report == nullptr || stats.cost_model_filter.empty()) {
        return true;
    }
    return stats.cost_model_filter == pass_name ||
           stats.cost_model_filter == std::string(pass::cost_model::to_string(kind));
}

std::int64_t saturating_dynamic_cost(std::int64_t value, std::int64_t multiplier) {
    value = std::max<std::int64_t>(0, value);
    multiplier = std::max<std::int64_t>(1, multiplier);
    const auto limit = std::numeric_limits<std::int64_t>::max();
    return value != 0 && multiplier > limit / value ? limit : value * multiplier;
}

std::int64_t saturating_nonnegative_add(std::int64_t lhs, std::int64_t rhs) {
    lhs = std::max<std::int64_t>(0, lhs);
    rhs = std::max<std::int64_t>(0, rhs);
    const auto limit = std::numeric_limits<std::int64_t>::max();
    return rhs > limit - lhs ? limit : lhs + rhs;
}

} // namespace

bool cost_model_allows_transform(Stats &stats, const OIRTransformCostEstimate &estimate) {
    if (stats.cost_model_report == nullptr) {
        return true;
    }

    pass::cost_model::TransformCandidate candidate;
    candidate.kind = estimate.kind;
    candidate.stage = pass::cost_model::CostIRStage::OIR;
    candidate.pass_name = estimate.pass_name;
    candidate.candidate_id = estimate.candidate_id;
    candidate.scope = estimate.scope;
    candidate.frequency.confidence = estimate.confidence;
    candidate.proof.kind = estimate.proof_kind;
    candidate.proof.status = estimate.proof_status;
    candidate.proof.summary = estimate.proof_summary;
    candidate.proof.rule_id = estimate.proof_rule_id;
    candidate.proof.solver_id = estimate.proof_solver_id;
    candidate.proof.time_us = estimate.proof_time_us;
    candidate.proof.obligations = estimate.proof_obligations;

    candidate.before.static_instrs = estimate.before_instrs;
    candidate.before.dynamic_instrs =
        saturating_dynamic_cost(estimate.before_instrs, estimate.dynamic_multiplier);
    candidate.before.code_bytes = estimate.before_code_bytes;
    candidate.before.int_alu = saturating_dynamic_cost(
        estimate.has_operation_breakdown ? estimate.before_int_alu : estimate.before_instrs,
        estimate.dynamic_multiplier);
    candidate.before.int_alu = saturating_nonnegative_add(
        candidate.before.int_alu, estimate.proven_dynamic_instruction_savings);
    candidate.before.int_mul =
        saturating_dynamic_cost(estimate.before_int_mul, estimate.dynamic_multiplier);
    candidate.before.int_div_rem =
        saturating_dynamic_cost(estimate.before_int_div_rem, estimate.dynamic_multiplier);
    candidate.before.fp_alu =
        saturating_dynamic_cost(estimate.before_fp_alu, estimate.dynamic_multiplier);
    candidate.before.fp_div =
        saturating_dynamic_cost(estimate.before_fp_div, estimate.dynamic_multiplier);
    candidate.before.loads =
        saturating_dynamic_cost(estimate.before_loads, estimate.dynamic_multiplier);
    candidate.before.stores =
        saturating_dynamic_cost(estimate.before_stores, estimate.dynamic_multiplier);
    candidate.before.pointer_arith =
        saturating_dynamic_cost(estimate.before_pointer_arith, estimate.dynamic_multiplier);
    candidate.before.branches =
        saturating_dynamic_cost(estimate.before_branches, estimate.dynamic_multiplier);
    candidate.before.calls =
        saturating_dynamic_cost(estimate.before_calls, estimate.dynamic_multiplier);
    candidate.before.phis =
        saturating_dynamic_cost(estimate.before_phis, estimate.dynamic_multiplier);
    candidate.before.live_values = estimate.before_live_values;
    candidate.before.max_live_values = estimate.before_max_live_values;

    candidate.after.static_instrs = estimate.after_instrs;
    candidate.after.dynamic_instrs =
        saturating_dynamic_cost(estimate.after_instrs, estimate.dynamic_multiplier);
    candidate.after.code_bytes = estimate.after_code_bytes;
    candidate.after.int_alu = saturating_dynamic_cost(
        estimate.has_operation_breakdown ? estimate.after_int_alu : estimate.after_instrs,
        estimate.dynamic_multiplier);
    candidate.after.int_mul =
        saturating_dynamic_cost(estimate.after_int_mul, estimate.dynamic_multiplier);
    candidate.after.int_div_rem =
        saturating_dynamic_cost(estimate.after_int_div_rem, estimate.dynamic_multiplier);
    candidate.after.fp_alu =
        saturating_dynamic_cost(estimate.after_fp_alu, estimate.dynamic_multiplier);
    candidate.after.fp_div =
        saturating_dynamic_cost(estimate.after_fp_div, estimate.dynamic_multiplier);
    candidate.after.loads =
        saturating_dynamic_cost(estimate.after_loads, estimate.dynamic_multiplier);
    candidate.after.stores =
        saturating_dynamic_cost(estimate.after_stores, estimate.dynamic_multiplier);
    candidate.after.pointer_arith =
        saturating_dynamic_cost(estimate.after_pointer_arith, estimate.dynamic_multiplier);
    candidate.after.branches =
        saturating_dynamic_cost(estimate.after_branches, estimate.dynamic_multiplier);
    candidate.after.calls =
        saturating_dynamic_cost(estimate.after_calls, estimate.dynamic_multiplier);
    candidate.after.phis =
        saturating_dynamic_cost(estimate.after_phis, estimate.dynamic_multiplier);
    candidate.after.live_values = estimate.after_live_values;
    candidate.after.max_live_values = estimate.after_max_live_values;
    candidate.setup.compile_time_units = saturating_nonnegative_add(
        candidate.setup.compile_time_units, estimate.partial_eval.cloned_functions);
    candidate.setup.compile_time_units = saturating_nonnegative_add(
        candidate.setup.compile_time_units, estimate.partial_eval.cloned_blocks / 4);
    candidate.setup.compile_time_units = saturating_nonnegative_add(
        candidate.setup.compile_time_units, estimate.partial_eval.required_cleanup_rounds);
    candidate.setup.static_instrs = saturating_nonnegative_add(
        candidate.setup.static_instrs, estimate.partial_eval.residual_instrs);
    candidate.setup.compile_time_units = saturating_nonnegative_add(
        candidate.setup.compile_time_units, estimate.egraph.saturation_rounds);
    candidate.setup.compile_time_units = saturating_nonnegative_add(
        candidate.setup.compile_time_units, estimate.egraph.extraction_time_us / 1000);
    candidate.setup.egraph_nodes = saturating_nonnegative_add(
        candidate.setup.egraph_nodes, estimate.egraph.enode_count / 1000);
    candidate.setup.proof_time_units = estimate.proof_time_units;
    candidate.setup.smt_queries = estimate.smt_queries;
    candidate.risk = estimate.risk;
    candidate.risk.code_growth = saturating_nonnegative_add(
        candidate.risk.code_growth, estimate.egraph.risk.code_growth);
    candidate.risk.live_range_growth = saturating_nonnegative_add(
        candidate.risk.live_range_growth, estimate.egraph.risk.live_range_growth);
    candidate.risk.register_pressure_growth = saturating_nonnegative_add(
        candidate.risk.register_pressure_growth,
        estimate.egraph.risk.register_pressure_growth);
    candidate.risk.compile_time_growth = saturating_nonnegative_add(
        candidate.risk.compile_time_growth, estimate.egraph.risk.compile_time_growth);
    candidate.risk.cleanup_dependency = saturating_nonnegative_add(
        candidate.risk.cleanup_dependency, estimate.egraph.risk.cleanup_dependency);
    candidate.bypass_profitability = estimate.bypass_profitability;
    candidate.bypass_reason = estimate.bypass_reason;
    candidate.forced_reject_reason = estimate.forced_reject_reason;

    auto decision = pass::cost_model::decide(
        candidate, pass::cost_model::policy_for_kind(stats.cost_model_policy),
        stats.cost_model_report->target);
    const bool accepted =
        (decision.action == pass::cost_model::DecisionAction::Accept ||
         decision.action == pass::cost_model::DecisionAction::BypassProfitability) &&
        decision.legal &&
        decision.profitable;
    if (filter_matches(stats, estimate.kind, estimate.pass_name)) {
        stats.cost_model_report->decisions.push_back(std::move(decision));
    }
    return accepted;
}

} // namespace pass::oir_opt
