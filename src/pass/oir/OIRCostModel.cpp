#include "pass/oir/OIRCostModel.h"

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

} // namespace

bool cost_model_allows_transform(Stats &stats, const OIRTransformCostEstimate &estimate) {
    if (stats.cost_model_report == nullptr ||
        !filter_matches(stats, estimate.kind, estimate.pass_name)) {
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
    candidate.before.dynamic_instrs = estimate.before_instrs;
    candidate.before.int_alu = estimate.before_instrs;
    candidate.before.branches = estimate.before_branches;
    candidate.before.calls = estimate.before_calls;

    candidate.after.static_instrs = estimate.after_instrs;
    candidate.after.dynamic_instrs = estimate.after_instrs;
    candidate.after.int_alu = estimate.after_instrs;
    candidate.after.branches = estimate.after_branches;
    candidate.after.calls = estimate.after_calls;
    candidate.setup.compile_time_units += estimate.partial_eval.cloned_functions;
    candidate.setup.compile_time_units += estimate.partial_eval.cloned_blocks / 4;
    candidate.setup.compile_time_units += estimate.partial_eval.required_cleanup_rounds;
    candidate.setup.static_instrs += estimate.partial_eval.residual_instrs;
    candidate.setup.compile_time_units += estimate.egraph.saturation_rounds;
    candidate.setup.compile_time_units += estimate.egraph.extraction_time_us / 1000;
    candidate.setup.egraph_nodes += estimate.egraph.enode_count / 1000;
    candidate.setup.proof_time_units = estimate.proof_time_units;
    candidate.setup.smt_queries = estimate.smt_queries;
    candidate.risk = estimate.risk;
    candidate.risk.code_growth += estimate.egraph.risk.code_growth;
    candidate.risk.live_range_growth += estimate.egraph.risk.live_range_growth;
    candidate.risk.register_pressure_growth += estimate.egraph.risk.register_pressure_growth;
    candidate.risk.compile_time_growth += estimate.egraph.risk.compile_time_growth;
    candidate.risk.cleanup_dependency += estimate.egraph.risk.cleanup_dependency;

    auto decision = pass::cost_model::decide(
        candidate, pass::cost_model::policy_for_kind(stats.cost_model_policy),
        stats.cost_model_report->target);
    const bool accepted =
        decision.action == pass::cost_model::DecisionAction::Accept && decision.legal &&
        decision.profitable;
    stats.cost_model_report->decisions.push_back(std::move(decision));
    return accepted;
}

} // namespace pass::oir_opt
