#include "pass/CostModel.h"

#include <algorithm>
#include <ostream>
#include <string>

namespace pass::cost_model {
namespace {

void print_json_string(std::ostream &out, std::string_view value) {
    out << '"';
    for (char ch : value) {
        switch (ch) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            out << ch;
            break;
        }
    }
    out << '"';
}

void print_cost_vector_json(const CostVector &cost, std::ostream &out, const char *indent) {
    out << "{\n";
#define COST_FIELD(name)                                                                           \
    out << indent << "  \"" #name "\": " << cost.name << ",\n"
    COST_FIELD(static_instrs);
    COST_FIELD(dynamic_instrs);
    COST_FIELD(code_bytes);
    COST_FIELD(int_alu);
    COST_FIELD(int_mul);
    COST_FIELD(int_div_rem);
    COST_FIELD(fp_alu);
    COST_FIELD(fp_div);
    COST_FIELD(loads);
    COST_FIELD(stores);
    COST_FIELD(memzero_bytes);
    COST_FIELD(pointer_arith);
    COST_FIELD(branches);
    COST_FIELD(unpredictable_branches);
    COST_FIELD(jumps);
    COST_FIELD(calls);
    COST_FIELD(phis);
    COST_FIELD(moves);
    COST_FIELD(virtual_regs);
    COST_FIELD(live_values);
    COST_FIELD(max_live_values);
    COST_FIELD(estimated_spills);
    COST_FIELD(stack_slots);
    COST_FIELD(compile_time_units);
    COST_FIELD(proof_time_units);
    COST_FIELD(egraph_nodes);
    COST_FIELD(smt_queries);
#undef COST_FIELD
    out << indent << "  \"estimated_cycles\": " << cost.estimated_cycles << "\n";
    out << indent << "}";
}

void print_risk_vector_json(const RiskVector &risk, std::ostream &out, const char *indent) {
    out << "{\n";
#define RISK_FIELD(name)                                                                           \
    out << indent << "  \"" #name "\": " << risk.name << ",\n"
    RISK_FIELD(code_growth);
    RISK_FIELD(live_range_growth);
    RISK_FIELD(register_pressure_growth);
    RISK_FIELD(memory_pressure_growth);
    RISK_FIELD(branch_predictability_loss);
    RISK_FIELD(locality_loss);
    RISK_FIELD(compile_time_growth);
    RISK_FIELD(proof_timeout_risk);
#undef RISK_FIELD
    out << indent << "  \"cleanup_dependency\": " << risk.cleanup_dependency << "\n";
    out << indent << "}";
}

std::string reason_for_reject(RejectReason reason) {
    return std::string(to_string(reason));
}

} // namespace

std::string_view to_string(CostIRStage stage) {
    switch (stage) {
    case CostIRStage::YIR:
        return "YIR";
    case CostIRStage::OIR:
        return "OIR";
    case CostIRStage::PreRAMIR:
        return "PreRAMIR";
    case CostIRStage::PostRAMIR:
        return "PostRAMIR";
    case CostIRStage::FinalMIR:
        return "FinalMIR";
    case CostIRStage::ASM:
        return "ASM";
    }
    return "Unknown";
}

std::string_view to_string(TransformKind kind) {
    switch (kind) {
    case TransformKind::Inline:
        return "Inline";
    case TransformKind::ConstantArgumentSpecialization:
        return "ConstantArgumentSpecialization";
    case TransformKind::PartialEvaluation:
        return "PartialEvaluation";
    case TransformKind::EGraphRewrite:
        return "EGraphRewrite";
    case TransformKind::AlgebraicSimplify:
        return "AlgebraicSimplify";
    case TransformKind::StrengthReduction:
        return "StrengthReduction";
    case TransformKind::LoopInvariantCodeMotion:
        return "LoopInvariantCodeMotion";
    case TransformKind::LoopUnswitch:
        return "LoopUnswitch";
    case TransformKind::LoopRotate:
        return "LoopRotate";
    case TransformKind::LoopInterchange:
        return "LoopInterchange";
    case TransformKind::LoopTiling:
        return "LoopTiling";
    case TransformKind::LoopUnroll:
        return "LoopUnroll";
    case TransformKind::LoopIdiom:
        return "LoopIdiom";
    case TransformKind::MemoryForwarding:
        return "MemoryForwarding";
    case TransformKind::MemZeroLowering:
        return "MemZeroLowering";
    case TransformKind::IfConversion:
        return "IfConversion";
    case TransformKind::BranchCombine:
        return "BranchCombine";
    case TransformKind::CompareBranchCombine:
        return "CompareBranchCombine";
    case TransformKind::LocalCSE:
        return "LocalCSE";
    case TransformKind::GlobalCSE:
        return "GlobalCSE";
    case TransformKind::AddressModeFold:
        return "AddressModeFold";
    case TransformKind::InstructionScheduling:
        return "InstructionScheduling";
    case TransformKind::RegisterAllocationHint:
        return "RegisterAllocationHint";
    case TransformKind::Peephole:
        return "Peephole";
    }
    return "Unknown";
}

std::string_view to_string(ProofKind kind) {
    switch (kind) {
    case ProofKind::None:
        return "None";
    case ProofKind::Structural:
        return "Structural";
    case ProofKind::DataFlow:
        return "DataFlow";
    case ProofKind::Dependence:
        return "Dependence";
    case ProofKind::SMT:
        return "SMT";
    case ProofKind::EGraphEquality:
        return "EGraphEquality";
    case ProofKind::PartialEvaluation:
        return "PartialEvaluation";
    case ProofKind::Composite:
        return "Composite";
    }
    return "Unknown";
}

std::string_view to_string(ProofStatus status) {
    switch (status) {
    case ProofStatus::Proven:
        return "Proven";
    case ProofStatus::Refuted:
        return "Refuted";
    case ProofStatus::Timeout:
        return "Timeout";
    case ProofStatus::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

std::string_view to_string(DecisionAction action) {
    switch (action) {
    case DecisionAction::Accept:
        return "Accept";
    case DecisionAction::Reject:
        return "Reject";
    case DecisionAction::BypassProfitability:
        return "BypassProfitability";
    case DecisionAction::Defer:
        return "Defer";
    case DecisionAction::PreferAlternative:
        return "PreferAlternative";
    case DecisionAction::RequireLaterStageCheck:
        return "RequireLaterStageCheck";
    }
    return "Unknown";
}

std::string_view to_string(RejectReason reason) {
    switch (reason) {
    case RejectReason::None:
        return "None";
    case RejectReason::Illegal:
        return "Illegal";
    case RejectReason::ProofTimeout:
        return "ProofTimeout";
    case RejectReason::ProofUnknown:
        return "ProofUnknown";
    case RejectReason::NegativeGain:
        return "NegativeGain";
    case RejectReason::LowConfidence:
        return "LowConfidence";
    case RejectReason::CodeGrowthTooHigh:
        return "CodeGrowthTooHigh";
    case RejectReason::RegisterPressureTooHigh:
        return "RegisterPressureTooHigh";
    case RejectReason::MemoryPressureTooHigh:
        return "MemoryPressureTooHigh";
    case RejectReason::CompileTimeTooHigh:
        return "CompileTimeTooHigh";
    case RejectReason::CleanupTooSpeculative:
        return "CleanupTooSpeculative";
    case RejectReason::TargetUnsupported:
        return "TargetUnsupported";
    }
    return "Unknown";
}

std::string_view to_string(CostModelPolicyKind policy) {
    switch (policy) {
    case CostModelPolicyKind::Conservative:
        return "conservative";
    case CostModelPolicyKind::Balanced:
        return "balanced";
    case CostModelPolicyKind::Aggressive:
        return "aggressive";
    }
    return "unknown";
}

bool parse_policy_kind(std::string_view text, CostModelPolicyKind &out) {
    if (text == "conservative") {
        out = CostModelPolicyKind::Conservative;
        return true;
    }
    if (text == "balanced") {
        out = CostModelPolicyKind::Balanced;
        return true;
    }
    if (text == "aggressive") {
        out = CostModelPolicyKind::Aggressive;
        return true;
    }
    return false;
}

TargetCostProfile default_target_profile() {
    return TargetCostProfile{};
}

CostModelPolicy policy_for_kind(CostModelPolicyKind kind) {
    CostModelPolicy policy;
    policy.kind = kind;
    switch (kind) {
    case CostModelPolicyKind::Conservative:
        policy.min_final_score = 8;
        policy.min_confidence = 0.70;
        policy.max_function_code_growth = 80;
        policy.small_code_growth_allowance = 0;
        policy.max_module_code_growth_percent = 8;
        policy.max_register_pressure_growth = 4;
        policy.max_live_range_growth = 8;
        policy.max_memory_pressure_growth = 8;
        policy.max_inline_callee_cost = 45;
        policy.max_specializations_per_function = 2;
        policy.max_egraph_nodes = 1500;
        policy.max_smt_time_us = 2500;
        policy.allow_loop_unswitch = false;
        policy.allow_partial_eval = true;
        policy.allow_egraph = true;
        policy.allow_smt = true;
        break;
    case CostModelPolicyKind::Balanced:
        break;
    case CostModelPolicyKind::Aggressive:
        policy.min_final_score = 0;
        policy.min_confidence = 0.45;
        policy.max_function_code_growth = 420;
        policy.small_code_growth_allowance = 48;
        policy.max_module_code_growth_percent = 25;
        policy.max_register_pressure_growth = 14;
        policy.max_live_range_growth = 28;
        policy.max_memory_pressure_growth = 28;
        policy.max_compile_time_growth = 25000;
        policy.max_inline_callee_cost = 140;
        policy.max_specializations_per_function = 8;
        policy.max_egraph_nodes = 12000;
        policy.max_smt_time_us = 12000;
        break;
    }
    return policy;
}

CostVector subtract_cost(const CostVector &lhs, const CostVector &rhs) {
    CostVector out;
#define COST_DIFF(name) out.name = lhs.name - rhs.name
    COST_DIFF(static_instrs);
    COST_DIFF(dynamic_instrs);
    COST_DIFF(code_bytes);
    COST_DIFF(int_alu);
    COST_DIFF(int_mul);
    COST_DIFF(int_div_rem);
    COST_DIFF(fp_alu);
    COST_DIFF(fp_div);
    COST_DIFF(loads);
    COST_DIFF(stores);
    COST_DIFF(memzero_bytes);
    COST_DIFF(pointer_arith);
    COST_DIFF(branches);
    COST_DIFF(unpredictable_branches);
    COST_DIFF(jumps);
    COST_DIFF(calls);
    COST_DIFF(phis);
    COST_DIFF(moves);
    COST_DIFF(virtual_regs);
    COST_DIFF(live_values);
    COST_DIFF(max_live_values);
    COST_DIFF(estimated_spills);
    COST_DIFF(stack_slots);
    COST_DIFF(compile_time_units);
    COST_DIFF(proof_time_units);
    COST_DIFF(egraph_nodes);
    COST_DIFF(smt_queries);
    COST_DIFF(estimated_cycles);
#undef COST_DIFF
    return out;
}

std::int64_t weighted_cost(const CostVector &cost, const TargetCostProfile &target) {
    if (cost.estimated_cycles != 0) {
        return cost.estimated_cycles;
    }

    std::int64_t total = 0;
    total += cost.int_alu * target.alu_i32;
    total += cost.int_mul * target.mul_i32;
    total += cost.int_div_rem * target.div_i32;
    total += cost.fp_alu * target.fp_add;
    total += cost.fp_div * target.fp_div;
    total += cost.loads * target.load;
    total += cost.stores * target.store;
    total += cost.branches * target.branch;
    total += cost.unpredictable_branches * target.unpredictable_branch;
    total += cost.jumps;
    total += cost.calls * target.call;
    total += cost.moves;
    total += cost.phis;
    total += cost.pointer_arith * target.alu_i64;
    total += cost.estimated_spills * (target.spill_load + target.spill_store);
    total += cost.code_bytes * target.code_byte;
    total += cost.compile_time_units;
    total += cost.proof_time_units;
    total += cost.smt_queries * 3;
    total += cost.egraph_nodes / 8;
    return total;
}

std::int64_t weighted_risk(const RiskVector &risk, const CostModelPolicy &policy) {
    std::int64_t total = 0;
    total += std::max<std::int64_t>(0, risk.code_growth - policy.small_code_growth_allowance);
    total += risk.live_range_growth * 2;
    total += risk.register_pressure_growth * 6;
    total += risk.memory_pressure_growth * 4;
    total += risk.branch_predictability_loss * 4;
    total += risk.locality_loss * 3;
    total += risk.compile_time_growth;
    total += risk.proof_timeout_risk * 8;
    total += risk.cleanup_dependency * 5;
    return total;
}

TransformDecision decide(const TransformCandidate &candidate, const CostModelPolicy &policy,
                         const TargetCostProfile &target) {
    TransformDecision decision;
    decision.before = candidate.before;
    decision.after = candidate.after;
    decision.delta = subtract_cost(candidate.after, candidate.before);
    decision.risk = candidate.risk;
    decision.proof = candidate.proof;
    decision.transform = std::string(to_string(candidate.kind));
    decision.pass_name = candidate.pass_name;
    decision.candidate_id = candidate.candidate_id;
    decision.scope = candidate.scope;
    decision.confidence = candidate.frequency.confidence;

    decision.estimated_gain =
        weighted_cost(candidate.before, target) - weighted_cost(candidate.after, target);
    decision.setup_cost = weighted_cost(candidate.setup, target);
    decision.risk_penalty = weighted_risk(candidate.risk, policy);
    decision.final_score =
        decision.estimated_gain - decision.setup_cost - decision.risk_penalty;

    if (candidate.proof.status == ProofStatus::Proven) {
        decision.legal = true;
    } else if (candidate.proof.status == ProofStatus::Timeout) {
        decision.reject_reason = RejectReason::ProofTimeout;
        decision.reason = reason_for_reject(decision.reject_reason);
        return decision;
    } else if (candidate.proof.status == ProofStatus::Unknown ||
               candidate.proof.status == ProofStatus::Refuted) {
        decision.reject_reason = candidate.proof.status == ProofStatus::Refuted
                                     ? RejectReason::Illegal
                                     : RejectReason::ProofUnknown;
        decision.reason = reason_for_reject(decision.reject_reason);
        return decision;
    }

    if (candidate.bypass_profitability) {
        decision.action = DecisionAction::BypassProfitability;
        decision.reject_reason = RejectReason::None;
        decision.profitable = true;
        decision.reason = candidate.bypass_reason.empty() ? "ProfitabilityBypassed"
                                                          : candidate.bypass_reason;
        return decision;
    }

    const bool small_inline_growth =
        candidate.kind == TransformKind::Inline &&
        candidate.risk.code_growth <= policy.small_code_growth_allowance;
    const bool exceeds_growth_percent =
        !small_inline_growth && candidate.before.static_instrs > 0 &&
        candidate.risk.code_growth * 100 >
            candidate.before.static_instrs * policy.max_module_code_growth_percent;

    if (candidate.risk.code_growth > policy.max_function_code_growth || exceeds_growth_percent) {
        decision.reject_reason = RejectReason::CodeGrowthTooHigh;
    } else if (candidate.risk.register_pressure_growth >
               policy.max_register_pressure_growth) {
        decision.reject_reason = RejectReason::RegisterPressureTooHigh;
    } else if (candidate.risk.live_range_growth > policy.max_live_range_growth) {
        decision.reject_reason = RejectReason::RegisterPressureTooHigh;
    } else if (candidate.risk.memory_pressure_growth > policy.max_memory_pressure_growth) {
        decision.reject_reason = RejectReason::MemoryPressureTooHigh;
    } else if (candidate.risk.compile_time_growth > policy.max_compile_time_growth) {
        decision.reject_reason = RejectReason::CompileTimeTooHigh;
    } else if (candidate.kind == TransformKind::LoopUnswitch && !policy.allow_loop_unswitch) {
        decision.reject_reason = RejectReason::TargetUnsupported;
    } else if ((candidate.kind == TransformKind::PartialEvaluation ||
                candidate.kind == TransformKind::ConstantArgumentSpecialization) &&
               !policy.allow_partial_eval) {
        decision.reject_reason = RejectReason::TargetUnsupported;
    } else if (candidate.kind == TransformKind::EGraphRewrite && !policy.allow_egraph) {
        decision.reject_reason = RejectReason::TargetUnsupported;
    } else if (candidate.proof.kind == ProofKind::SMT && !policy.allow_smt) {
        decision.reject_reason = RejectReason::TargetUnsupported;
    } else if (decision.confidence < policy.min_confidence) {
        decision.reject_reason = RejectReason::LowConfidence;
    } else if (decision.final_score < policy.min_final_score) {
        decision.reject_reason = RejectReason::NegativeGain;
    } else {
        decision.action = DecisionAction::Accept;
        decision.reject_reason = RejectReason::None;
        decision.profitable = true;
        decision.reason = "Profitable";
        return decision;
    }

    decision.reason = reason_for_reject(decision.reject_reason);
    return decision;
}

void merge_report(CostModelReport &report, const CostModelReport &other) {
    if (report.target.arch.empty()) {
        report.target = other.target;
    }
    for (const auto &summary : other.summaries) {
        report.summaries.push_back(summary);
    }
    for (const auto &decision : other.decisions) {
        report.decisions.push_back(decision);
    }
}

void print_report_json(const CostModelReport &report, std::ostream &out) {
    out << "{\n";
    out << "  \"target\": ";
    print_json_string(out, report.target.arch + "/" + report.target.abi);
    out << ",\n";
    out << "  \"policy\": ";
    print_json_string(out, to_string(report.policy));
    out << ",\n";
    out << "  \"filter\": ";
    print_json_string(out, report.filter);
    out << ",\n";
    out << "  \"summaries\": [\n";
    for (std::size_t i = 0; i < report.summaries.size(); ++i) {
        const auto &summary = report.summaries[i];
        out << "    {\n";
        out << "      \"stage\": ";
        print_json_string(out, to_string(summary.stage));
        out << ",\n";
        out << "      \"label\": ";
        print_json_string(out, summary.label);
        out << ",\n";
        out << "      \"functions\": " << summary.functions << ",\n";
        out << "      \"external_functions\": " << summary.external_functions << ",\n";
        out << "      \"basic_blocks\": " << summary.basic_blocks << ",\n";
        out << "      \"globals\": " << summary.globals << ",\n";
        out << "      \"cost\": ";
        print_cost_vector_json(summary.cost, out, "      ");
        out << "\n";
        out << "    }";
        if (i + 1 != report.summaries.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"decisions\": [\n";
    for (std::size_t i = 0; i < report.decisions.size(); ++i) {
        const auto &decision = report.decisions[i];
        out << "    {\n";
        out << "      \"transform\": ";
        print_json_string(out, decision.transform);
        out << ",\n";
        out << "      \"pass\": ";
        print_json_string(out, decision.pass_name);
        out << ",\n";
        out << "      \"candidate_id\": ";
        print_json_string(out, decision.candidate_id);
        out << ",\n";
        out << "      \"scope\": ";
        print_json_string(out, decision.scope);
        out << ",\n";
        out << "      \"action\": ";
        print_json_string(out, to_string(decision.action));
        out << ",\n";
        out << "      \"reject_reason\": ";
        print_json_string(out, to_string(decision.reject_reason));
        out << ",\n";
        out << "      \"legal\": " << (decision.legal ? "true" : "false") << ",\n";
        out << "      \"profitable\": " << (decision.profitable ? "true" : "false") << ",\n";
        out << "      \"confidence\": " << decision.confidence << ",\n";
        out << "      \"estimated_gain\": " << decision.estimated_gain << ",\n";
        out << "      \"setup_cost\": " << decision.setup_cost << ",\n";
        out << "      \"risk_penalty\": " << decision.risk_penalty << ",\n";
        out << "      \"final_score\": " << decision.final_score << ",\n";
        out << "      \"proof\": {\n";
        out << "        \"kind\": ";
        print_json_string(out, to_string(decision.proof.kind));
        out << ",\n";
        out << "        \"status\": ";
        print_json_string(out, to_string(decision.proof.status));
        out << ",\n";
        out << "        \"summary\": ";
        print_json_string(out, decision.proof.summary);
        out << ",\n";
        out << "        \"rule_id\": ";
        print_json_string(out, decision.proof.rule_id);
        out << ",\n";
        out << "        \"solver_id\": ";
        print_json_string(out, decision.proof.solver_id);
        out << ",\n";
        out << "        \"time_us\": " << decision.proof.time_us << ",\n";
        out << "        \"obligations\": " << decision.proof.obligations << "\n";
        out << "      },\n";
        out << "      \"before\": ";
        print_cost_vector_json(decision.before, out, "      ");
        out << ",\n";
        out << "      \"after\": ";
        print_cost_vector_json(decision.after, out, "      ");
        out << ",\n";
        out << "      \"delta\": ";
        print_cost_vector_json(decision.delta, out, "      ");
        out << ",\n";
        out << "      \"risk\": ";
        print_risk_vector_json(decision.risk, out, "      ");
        out << ",\n";
        out << "      \"reason\": ";
        print_json_string(out, decision.reason);
        out << "\n";
        out << "    }";
        if (i + 1 != report.decisions.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

void print_report_text(const CostModelReport &report, std::ostream &out) {
    out << "[cost-model] target=" << report.target.arch << "/" << report.target.abi
        << " policy=" << to_string(report.policy);
    if (!report.filter.empty()) {
        out << " filter=" << report.filter;
    }
    out << "\n";
    for (const auto &summary : report.summaries) {
        out << "[cost-model] summary stage=" << to_string(summary.stage)
            << " label=" << summary.label << " functions=" << summary.functions
            << " blocks=" << summary.basic_blocks
            << " instrs=" << summary.cost.static_instrs
            << " loads=" << summary.cost.loads
            << " stores=" << summary.cost.stores
            << " branches=" << summary.cost.branches
            << " calls=" << summary.cost.calls
            << " spills=" << summary.cost.estimated_spills
            << " score=" << weighted_cost(summary.cost, report.target) << "\n";
    }
    for (const auto &decision : report.decisions) {
        out << "[cost-model] " << decision.pass_name << " "
            << to_string(decision.action) << " transform=" << decision.transform
            << " scope=" << decision.scope << " gain=" << decision.estimated_gain
            << " risk=" << decision.risk_penalty
            << " score=" << decision.final_score
            << " reason=" << decision.reason << "\n";
    }
}

} // namespace pass::cost_model
