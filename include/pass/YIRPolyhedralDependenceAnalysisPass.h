#pragma once

#include "PassManager.h"
#include <cstdint>
#include <string_view>
#include <vector>

namespace yir {
class Value;
class Operation;
} // namespace yir

namespace pass {

struct PolyDependence {
    enum class Kind { RAW, WAR, WAW, RAR };
    enum class DistanceKind { SameIteration, LoopCarriedConstant, Unknown };

    Kind kind;
    std::size_t source_stmt_id;
    std::size_t target_stmt_id;
    const yir::Value *memory;
    DistanceKind distance_kind = DistanceKind::Unknown;
    std::vector<std::int64_t> distance;
    // For affine/GCD test, we can store whether it's conservatively dependent
    bool is_dependent = true;
};

struct PolyDependenceInfo {
    std::vector<PolyDependence> dependences;
};

class YIRPolyhedralDependenceAnalysisPass final : public Pass {
  public:
    static constexpr std::string_view kArtifactKey = "PolyDependenceInfo";

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
