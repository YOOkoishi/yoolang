#include "pass/mir/MIRCostModel.h"

#include <utility>

namespace pass::mir_cost_model {
namespace {

bool filter_matches(const std::string &filter, pass::cost_model::TransformKind kind,
                    const std::string &pass_name) {
    if (filter.empty()) {
        return true;
    }
    return filter == pass_name || filter == std::string(pass::cost_model::to_string(kind));
}

} // namespace

bool allows_transform(pass::cost_model::CostModelReport *report,
                      pass::cost_model::CostModelPolicyKind policy,
                      const std::string &filter,
                      const MIRTransformCostEstimate &estimate) {
    if (report == nullptr) {
        return true;
    }

    pass::cost_model::TransformCandidate candidate;
    candidate.kind = estimate.kind;
    candidate.stage = estimate.stage;
    candidate.pass_name = estimate.pass_name;
    candidate.candidate_id = estimate.candidate_id;
    candidate.scope = estimate.scope;
    candidate.frequency.confidence = estimate.confidence;
    candidate.proof.kind = estimate.proof_kind;
    candidate.proof.status = pass::cost_model::ProofStatus::Proven;
    candidate.proof.summary = estimate.proof_summary;

    candidate.before.static_instrs = estimate.before_instrs;
    candidate.before.dynamic_instrs = estimate.before_dynamic_instrs;
    candidate.before.estimated_cycles = estimate.before_cycles;
    candidate.before.moves = estimate.before_moves;
    candidate.before.loads = estimate.before_loads;
    candidate.before.stores = estimate.before_stores;
    candidate.before.branches = estimate.before_branches;

    candidate.after.static_instrs = estimate.after_instrs;
    candidate.after.dynamic_instrs = estimate.after_dynamic_instrs;
    candidate.after.estimated_cycles = estimate.after_cycles;
    candidate.after.moves = estimate.after_moves;
    candidate.after.loads = estimate.after_loads;
    candidate.after.stores = estimate.after_stores;
    candidate.after.branches = estimate.after_branches;
    candidate.risk = estimate.risk;

    auto decision = pass::cost_model::decide(
        candidate, pass::cost_model::policy_for_kind(policy), report->target);
    const bool accepted =
        decision.action == pass::cost_model::DecisionAction::Accept && decision.legal &&
        decision.profitable;
    if (filter_matches(filter, estimate.kind, estimate.pass_name)) {
        report->decisions.push_back(std::move(decision));
    }
    return accepted;
}

} // namespace pass::mir_cost_model
