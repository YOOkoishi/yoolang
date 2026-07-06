#pragma once

#include "pass/CostModel.h"
#include "oir/OIRScalarOpt.h"

#include <cstdint>
#include <string>

namespace pass::oir_opt {

struct OIRTransformCostEstimate {
    pass::cost_model::TransformKind kind = pass::cost_model::TransformKind::Peephole;
    std::string pass_name;
    std::string candidate_id;
    std::string scope;

    pass::cost_model::ProofKind proof_kind = pass::cost_model::ProofKind::Structural;
    pass::cost_model::ProofStatus proof_status = pass::cost_model::ProofStatus::Proven;
    std::string proof_summary = "structural legality checks";
    std::string proof_rule_id;
    std::string proof_solver_id;
    std::int64_t proof_time_us = 0;
    std::int64_t proof_obligations = 0;
    double confidence = 0.65;

    std::int64_t before_instrs = 0;
    std::int64_t after_instrs = 0;
    std::int64_t before_code_bytes = 0;
    std::int64_t after_code_bytes = 0;
    std::int64_t before_int_alu = 0;
    std::int64_t after_int_alu = 0;
    std::int64_t before_int_mul = 0;
    std::int64_t after_int_mul = 0;
    std::int64_t before_int_div_rem = 0;
    std::int64_t after_int_div_rem = 0;
    std::int64_t before_fp_alu = 0;
    std::int64_t after_fp_alu = 0;
    std::int64_t before_fp_div = 0;
    std::int64_t after_fp_div = 0;
    std::int64_t before_loads = 0;
    std::int64_t after_loads = 0;
    std::int64_t before_stores = 0;
    std::int64_t after_stores = 0;
    std::int64_t before_pointer_arith = 0;
    std::int64_t after_pointer_arith = 0;
    std::int64_t before_branches = 0;
    std::int64_t after_branches = 0;
    std::int64_t before_calls = 0;
    std::int64_t after_calls = 0;
    std::int64_t before_phis = 0;
    std::int64_t after_phis = 0;
    std::int64_t before_live_values = 0;
    std::int64_t after_live_values = 0;
    std::int64_t before_max_live_values = 0;
    std::int64_t after_max_live_values = 0;
    std::int64_t proof_time_units = 0;
    std::int64_t smt_queries = 0;
    pass::cost_model::PartialEvalCost partial_eval;
    pass::cost_model::EGraphExtractCost egraph;

    pass::cost_model::RiskVector risk;
};

bool cost_model_allows_transform(Stats &stats, const OIRTransformCostEstimate &estimate);

} // namespace pass::oir_opt
