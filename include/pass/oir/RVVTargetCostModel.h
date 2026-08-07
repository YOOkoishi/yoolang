#pragma once

#include "target/TargetMachine.h"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace pass::oir_vectorize {

enum class RVVMemoryAccessKind : std::uint8_t {
    UnitStride,
    Strided,
    Indexed,
    Segment,
};

struct RVVMemoryCostOp final {
    RVVMemoryAccessKind kind = RVVMemoryAccessKind::UnitStride;
    bool is_store = false;
    // Segment operations are costed as one instruction plus a per-field data
    // cost.  Non-segment operations must leave this at one.
    unsigned segment_fields = 1;
};

// Target-independent summary produced by Loop/SLP analysis.  Keeping OIR
// objects out of this structure makes the target model independently testable
// and prevents profitability from accidentally becoming a legality oracle.
struct RVVLoopCostInput final {
    unsigned element_bits = 32;
    bool element_is_float = false;
    bool trip_count_known = false;
    std::uint64_t trip_count = 0;
    // Ordinary lane ALU operations.  Memory, mask formation and reductions
    // are represented separately and must not be included here.
    unsigned scalar_alu_operations = 0;
    unsigned vector_alu_operations = 0;
    unsigned mask_operations = 0;
    unsigned reduction_operations = 0;
    unsigned live_vector_values = 0;
    unsigned distinct_index_vectors = 0;
    std::vector<RVVMemoryCostOp> memory_operations;
    bool needs_tail_mask = true;
    unsigned runtime_alias_checks = 0;
    unsigned scalar_loop_code_bytes = 0;
    // Set only after the OIR planner has constructed a factor-one plan and
    // proved that its scalar lane recipe can be emitted as two independent
    // VLA chunks.  Profitability and force must never manufacture legality.
    bool interleave_factor_two_legal = false;
    std::string interleave_factor_two_rejection;
};

struct RVVTargetCostModelOptions final {
    bool force = false;
    std::uint64_t expected_trip_count = 64;
    bool explore_interleave = false;
    unsigned requested_max_interleave = 4;
};

enum class RVVCostRejectReason : std::uint8_t {
    None,
    TargetUnsupported,
    InvalidInput,
    NoLegalLMUL,
    ShortTrip,
    NotProfitable,
};

std::string_view rvv_cost_reject_reason_name(RVVCostRejectReason reason);
std::string_view rvv_memory_access_kind_name(RVVMemoryAccessKind kind);

struct RVVCostCandidate final {
    std::string lmul = "m1";
    unsigned lmul_numerator = 1;
    unsigned lmul_denominator = 1;
    unsigned minimum_lanes = 0;
    unsigned interleave = 1;
    bool legal = false;
    std::string rejection;

    std::uint64_t vector_iterations = 0;
    unsigned estimated_vector_registers = 0;
    unsigned predicted_spill_registers = 0;
    // Per outer iteration latency/throughput credit from overlapping the two
    // independent chunks.  All instructions, vsetvl and code bytes are still
    // charged before this target-derived credit is applied.
    std::uint64_t interleave_overlap_credit = 0;
    std::uint64_t dynamic_cost = 0;
    std::uint64_t code_size_cost = 0;
    std::uint64_t total_cost = 0;
    std::uint64_t estimated_code_bytes = 0;
    std::uint64_t break_even_trip_count = 0;
};

struct RVVCostDecision final {
    bool profitable = false;
    bool profitability_bypassed = false;
    RVVCostRejectReason reject_reason = RVVCostRejectReason::None;
    std::string explanation;
    // Non-empty when O3 requested factor two but either target tuning or the
    // concrete OIR recipe rejected it.  In that state all legal candidates
    // remain factor one.
    std::string interleave_capability_gate;
    std::uint64_t evaluated_trip_count = 0;
    std::uint64_t estimated_scalar_cost = 0;
    RVVCostCandidate selected;
    std::vector<RVVCostCandidate> candidates;
};

class RVVTargetCostModel final {
  public:
    explicit RVVTargetCostModel(target::TargetProfile target,
                                RVVTargetCostModelOptions options = {});

    RVVCostDecision choose(const RVVLoopCostInput &input) const;

  private:
    target::TargetProfile target_;
    RVVTargetCostModelOptions options_;
};

void print_rvv_cost_decision_json(const RVVCostDecision &decision, std::ostream &out);

} // namespace pass::oir_vectorize
