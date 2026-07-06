#pragma once

#include "pass/CostModel.h"

#include <cstdint>
#include <string>

namespace pass::mir_cost_model {

struct MIRTransformCostEstimate {
    pass::cost_model::TransformKind kind = pass::cost_model::TransformKind::Peephole;
    pass::cost_model::CostIRStage stage = pass::cost_model::CostIRStage::PreRAMIR;
    std::string pass_name;
    std::string candidate_id;
    std::string scope;

    pass::cost_model::ProofKind proof_kind = pass::cost_model::ProofKind::Structural;
    std::string proof_summary = "MIR structural checks";
    double confidence = 0.65;

    std::int64_t before_instrs = 0;
    std::int64_t after_instrs = 0;
    std::int64_t before_dynamic_instrs = 0;
    std::int64_t after_dynamic_instrs = 0;
    std::int64_t before_cycles = 0;
    std::int64_t after_cycles = 0;
    std::int64_t before_moves = 0;
    std::int64_t after_moves = 0;
    std::int64_t before_loads = 0;
    std::int64_t after_loads = 0;
    std::int64_t before_stores = 0;
    std::int64_t after_stores = 0;
    std::int64_t before_branches = 0;
    std::int64_t after_branches = 0;

    pass::cost_model::RiskVector risk;
};

bool allows_transform(pass::cost_model::CostModelReport *report,
                      pass::cost_model::CostModelPolicyKind policy,
                      const std::string &filter,
                      const MIRTransformCostEstimate &estimate);

} // namespace pass::mir_cost_model
