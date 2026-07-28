#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace pass::cost_model {

inline constexpr const char *kReportArtifactKey = "cost.model.report";

enum class CostIRStage {
    YIR,
    OIR,
    PreRAMIR,
    PostRAMIR,
    FinalMIR,
    ASM,
};

enum class TransformKind {
    Inline,
    ConstantArgumentSpecialization,
    PartialEvaluation,
    EGraphRewrite,
    AlgebraicSimplify,
    StrengthReduction,
    LoopInvariantCodeMotion,
    LoopUnswitch,
    LoopRotate,
    LoopInterchange,
    LoopTiling,
    LoopFusion,
    LoopUnroll,
    LoopIdiom,
    MemoryForwarding,
    MemZeroLowering,
    IfConversion,
    BranchCombine,
    CompareBranchCombine,
    LocalCSE,
    GlobalCSE,
    AddressModeFold,
    InstructionScheduling,
    RegisterAllocationHint,
    Peephole,
};

enum class FrequencySource {
    Unknown,
    StructuredYIRLoop,
    OIRLoopAnalysis,
    ConstantTripCount,
    ValueRangeTripCount,
    HeuristicLoopDepth,
    UserDisabled,
};

enum class ProofKind {
    None,
    Structural,
    DataFlow,
    Dependence,
    SMT,
    EGraphEquality,
    PartialEvaluation,
    Composite,
};

enum class ProofStatus {
    Proven,
    Refuted,
    Timeout,
    Unknown,
};

enum class DecisionAction {
    Accept,
    Reject,
    BypassProfitability,
    Defer,
    PreferAlternative,
    RequireLaterStageCheck,
};

enum class RejectReason {
    None,
    Illegal,
    ProofTimeout,
    ProofUnknown,
    NegativeGain,
    LowConfidence,
    CodeGrowthTooHigh,
    RegisterPressureTooHigh,
    MemoryPressureTooHigh,
    CompileTimeTooHigh,
    CleanupTooSpeculative,
    TargetUnsupported,
};

enum class CostModelPolicyKind {
    Conservative,
    Balanced,
    Aggressive,
};

struct TargetCostProfile {
    std::string arch = "rv64gc";
    std::string abi = "lp64d";
    int xlen_bits = 64;
    int flen_bits = 64;
    int stack_align = 16;

    int alu_i32 = 1;
    int alu_i64 = 1;
    int mul_i32 = 3;
    int div_i32 = 24;
    int rem_i32 = 24;
    int fp_add = 4;
    int fp_mul = 5;
    int fp_div = 30;
    int load = 4;
    int store = 4;
    int branch = 2;
    int unpredictable_branch = 8;
    int call = 18;
    int spill_load = 6;
    int spill_store = 6;
    int code_byte = 1;
};

struct FrequencyEstimate {
    std::int64_t scale = 1;
    int loop_depth = 0;
    bool exact_trip_count = false;
    bool bounded_trip_count = false;
    FrequencySource source = FrequencySource::Unknown;
    double confidence = 0.5;
};

struct CostVector {
    std::int64_t static_instrs = 0;
    std::int64_t dynamic_instrs = 0;
    std::int64_t code_bytes = 0;

    std::int64_t int_alu = 0;
    std::int64_t int_mul = 0;
    std::int64_t int_div_rem = 0;
    std::int64_t fp_alu = 0;
    std::int64_t fp_div = 0;

    std::int64_t loads = 0;
    std::int64_t stores = 0;
    std::int64_t memzero_bytes = 0;
    std::int64_t pointer_arith = 0;

    std::int64_t branches = 0;
    std::int64_t unpredictable_branches = 0;
    std::int64_t jumps = 0;
    std::int64_t calls = 0;

    std::int64_t phis = 0;
    std::int64_t moves = 0;
    std::int64_t virtual_regs = 0;
    std::int64_t live_values = 0;
    std::int64_t max_live_values = 0;
    std::int64_t estimated_spills = 0;
    std::int64_t stack_slots = 0;

    std::int64_t compile_time_units = 0;
    std::int64_t proof_time_units = 0;
    std::int64_t egraph_nodes = 0;
    std::int64_t smt_queries = 0;

    std::int64_t estimated_cycles = 0;
};

struct RiskVector {
    std::int64_t code_growth = 0;
    std::int64_t live_range_growth = 0;
    std::int64_t register_pressure_growth = 0;
    std::int64_t memory_pressure_growth = 0;
    std::int64_t branch_predictability_loss = 0;
    std::int64_t locality_loss = 0;
    std::int64_t compile_time_growth = 0;
    std::int64_t proof_timeout_risk = 0;
    std::int64_t cleanup_dependency = 0;
};

struct PartialEvalCost {
    std::int64_t cloned_functions = 0;
    std::int64_t cloned_blocks = 0;
    std::int64_t residual_instrs = 0;
    std::int64_t eliminated_instrs = 0;
    std::int64_t eliminated_branches = 0;
    std::int64_t eliminated_calls = 0;
    std::int64_t new_constants = 0;
    std::int64_t required_cleanup_rounds = 0;
};

struct EGraphExtractCost {
    CostVector cost;
    RiskVector risk;
    std::int64_t eclass_count = 0;
    std::int64_t enode_count = 0;
    std::int64_t saturation_rounds = 0;
    std::int64_t extraction_time_us = 0;
};

struct EquivalenceProof {
    ProofKind kind = ProofKind::None;
    ProofStatus status = ProofStatus::Unknown;
    std::string summary;
    std::string rule_id;
    std::string solver_id;
    std::int64_t time_us = 0;
    std::int64_t obligations = 0;
};

struct AlternativeCost {
    std::string alternative_id;
    CostVector cost;
    RiskVector risk;
    EquivalenceProof proof;
};

struct TransformCandidate {
    TransformKind kind = TransformKind::Peephole;
    CostIRStage stage = CostIRStage::OIR;
    std::string pass_name;
    std::string candidate_id;
    std::string scope;

    FrequencyEstimate frequency;
    EquivalenceProof proof;

    CostVector before;
    CostVector after;
    CostVector setup;
    RiskVector risk;

    bool bypass_profitability = false;
    std::string bypass_reason;

    std::vector<std::string> required_cleanup_passes;
    std::vector<std::string> reason_hints;
    std::vector<AlternativeCost> alternatives;
};

struct CandidateProviderBudget {
    std::int64_t max_candidates = 128;
    std::int64_t max_compile_time_us = 10000;
    std::int64_t max_smt_time_us = 5000;
    std::int64_t max_egraph_nodes = 5000;
    std::int64_t max_specializations_per_function = 4;
};

struct CandidateProviderRequest {
    TransformKind kind = TransformKind::Peephole;
    CostIRStage stage = CostIRStage::OIR;
    std::string provider_name;
    std::string scope;
    TargetCostProfile target;
    CostModelPolicyKind policy = CostModelPolicyKind::Balanced;
    CandidateProviderBudget budget;
};

struct CandidateProviderResult {
    std::vector<TransformCandidate> candidates;
    bool budget_exhausted = false;
    std::string note;
};

struct TransformDecision {
    DecisionAction action = DecisionAction::Reject;
    RejectReason reject_reason = RejectReason::None;

    bool legal = false;
    bool profitable = false;
    double confidence = 0.0;

    std::int64_t estimated_gain = 0;
    std::int64_t setup_cost = 0;
    std::int64_t risk_penalty = 0;
    std::int64_t final_score = 0;

    CostVector before;
    CostVector after;
    CostVector delta;
    RiskVector risk;
    EquivalenceProof proof;

    std::string transform;
    std::string pass_name;
    std::string candidate_id;
    std::string scope;
    std::string reason;
};

struct CostModelPolicy {
    CostModelPolicyKind kind = CostModelPolicyKind::Balanced;

    std::int64_t min_final_score = 1;
    double min_confidence = 0.55;

    std::int64_t max_function_code_growth = 200;
    std::int64_t small_code_growth_allowance = 24;
    std::int64_t max_module_code_growth_percent = 15;
    std::int64_t max_register_pressure_growth = 8;
    std::int64_t max_live_range_growth = 16;
    std::int64_t max_memory_pressure_growth = 16;
    std::int64_t max_compile_time_growth = 10000;

    std::int64_t max_inline_callee_cost = 80;
    std::int64_t max_specializations_per_function = 4;
    std::int64_t max_egraph_nodes = 5000;
    std::int64_t max_smt_time_us = 5000;

    bool allow_loop_unswitch = true;
    bool allow_partial_eval = true;
    bool allow_egraph = true;
    bool allow_smt = true;
};

struct ModuleCostSummary {
    CostIRStage stage = CostIRStage::OIR;
    std::string label;
    std::int64_t functions = 0;
    std::int64_t external_functions = 0;
    std::int64_t basic_blocks = 0;
    std::int64_t globals = 0;
    CostVector cost;
};

struct CostModelReport {
    TargetCostProfile target;
    CostModelPolicyKind policy = CostModelPolicyKind::Balanced;
    std::string filter;
    std::vector<ModuleCostSummary> summaries;
    std::vector<TransformDecision> decisions;
};

std::string_view to_string(CostIRStage stage);
std::string_view to_string(TransformKind kind);
std::string_view to_string(ProofKind kind);
std::string_view to_string(ProofStatus status);
std::string_view to_string(DecisionAction action);
std::string_view to_string(RejectReason reason);
std::string_view to_string(CostModelPolicyKind policy);

bool parse_policy_kind(std::string_view text, CostModelPolicyKind &out);

TargetCostProfile default_target_profile();
CostModelPolicy policy_for_kind(CostModelPolicyKind kind);

CostVector subtract_cost(const CostVector &lhs, const CostVector &rhs);
std::int64_t weighted_cost(const CostVector &cost, const TargetCostProfile &target);
std::int64_t weighted_risk(const RiskVector &risk, const CostModelPolicy &policy);
TransformDecision decide(const TransformCandidate &candidate, const CostModelPolicy &policy,
                         const TargetCostProfile &target);

void merge_report(CostModelReport &report, const CostModelReport &other);
void print_report_json(const CostModelReport &report, std::ostream &out);
void print_report_text(const CostModelReport &report, std::ostream &out);

} // namespace pass::cost_model
