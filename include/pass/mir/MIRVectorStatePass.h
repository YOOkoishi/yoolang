#pragma once

#include "pass/PassManager.h"

#include <cstddef>
#include <string_view>

namespace pass {

struct MIRVectorStateMetrics final {
    std::size_t inserted_vset = 0;
    std::size_t removed_vset = 0;
};

class MIRVectorStatePass final : public Pass {
  public:
    static constexpr const char *kMetricsArtifactKey = "mir.vector-state.metrics";

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
