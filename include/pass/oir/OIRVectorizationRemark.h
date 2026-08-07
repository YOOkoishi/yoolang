#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace pass::oir_vectorize {

inline constexpr const char *kRemarkArtifactKey = "oir.vectorization.remarks";

// These identifiers are part of the machine-readable diagnostics contract.
// Keep spelling stable even if the human explanation evolves.
enum class RemarkCode : std::uint8_t {
    Vectorized,
    Candidate,
    RejectDependence,
    RejectAlias,
    RejectFPOrder,
    RejectCall,
    RejectCost,
    RejectRegisterPressure,
    RejectUnsupportedType,
    RejectNonCanonicalLoop,
    RejectEarlyExit,
    RejectPotentialTrap,
    RejectReduction,
    RejectStride,
    RejectVolatileOrAtomic,
    RejectScalableStorage,
    RejectTargetFeature,
    Disabled,
};

std::string_view remark_code_name(RemarkCode code);

enum class VectorizerKind : std::uint8_t {
    Loop,
    SLP,
};

std::string_view vectorizer_kind_name(VectorizerKind kind);

struct PlanChoice final {
    bool scalable = true;
    unsigned minimum_lanes = 0;
    std::string lmul = "m1";
    unsigned interleave = 1;
    std::uint64_t estimated_scalar_cost = 0;
    std::uint64_t estimated_vector_cost = 0;
    unsigned estimated_vector_registers = 0;
    unsigned predicted_spill_registers = 0;
    std::uint64_t interleave_overlap_credit = 0;
    std::uint64_t estimated_code_bytes = 0;
    std::uint64_t break_even_trip_count = 0;
    std::string tuning = "generic-rv64";
    std::string interleave_capability_gate;
    bool requires_runtime_alias_check = false;
    bool uses_mask = false;
};

struct Remark final {
    VectorizerKind vectorizer = VectorizerKind::Loop;
    RemarkCode code = RemarkCode::RejectUnsupportedType;
    std::string function;
    std::string region;
    std::string explanation;
    PlanChoice plan;

    bool succeeded() const;
};

class RemarkLog final {
  public:
    void add(Remark remark);
    const std::vector<Remark> &remarks() const;
    bool empty() const;
    void clear();

  private:
    std::vector<Remark> remarks_;
};

void print_remarks_text(const RemarkLog &log, std::ostream &out, bool include_successes = true,
                        bool include_misses = true, std::string_view function_filter = {});
void print_vector_plan_json(const RemarkLog &log, std::ostream &out);

} // namespace pass::oir_vectorize
