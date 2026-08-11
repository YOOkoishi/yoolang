#pragma once

#include "pass/PassManager.h"
#include <cstddef>
#include <string_view>

namespace pass {

enum class YIRPolyhedralTransformMode {
    Full,
    Structural,
    Local,
};

enum class YIRPolyhedralStructuralChange {
    None,
    Fusion,
    Schedule,
};

struct YIRPolyhedralTransformSummary {
    static constexpr std::string_view kArtifactKey =
        "YIRPolyhedralTransformSummary";

    YIRPolyhedralStructuralChange structural_change =
        YIRPolyhedralStructuralChange::None;
    std::size_t interchanges = 0;
    std::size_t tilings = 0;
    std::size_t fusions = 0;
    // Number of independent-output lane packs intentionally left in scalar
    // form for the OIR SLP vectorizer.  Keeping this hand-off explicit lets
    // the ordinary RVV backend own vector types, masks, VL and VTYPE.
    std::size_t rvv_preparations = 0;
};

class YIRPolyhedralTransformPass final : public Pass {
  public:
    explicit YIRPolyhedralTransformPass(
        YIRPolyhedralTransformMode mode = YIRPolyhedralTransformMode::Full,
        bool enable_rvv_preparation = false)
        : mode_(mode), enable_rvv_preparation_(enable_rvv_preparation) {}

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    YIRPolyhedralTransformMode mode_;
    bool enable_rvv_preparation_;
};

} // namespace pass
