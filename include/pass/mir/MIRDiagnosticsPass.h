#pragma once

#include "mir/MIRVerifier.h"
#include "pass/PassManager.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace pass {

struct MIRStageMetrics {
    std::string stage;
    std::int64_t functions = 0;
    std::int64_t basic_blocks = 0;
    std::int64_t instructions = 0;
    std::int64_t moves = 0;
    std::int64_t jumps = 0;
    std::int64_t branches = 0;
    std::int64_t loads = 0;
    std::int64_t stores = 0;
    std::int64_t load_slots = 0;
    std::int64_t store_slots = 0;
    std::int64_t spills = 0;
    std::int64_t stack_slots = 0;
    std::int64_t calls = 0;
};

class MIRDiagnosticsPass final : public Pass {
  public:
    static constexpr const char *kMetricsArtifactKey = "mir.stage.metrics";

    MIRDiagnosticsPass(std::string stage, mir::MIRVerificationStage verification_stage);

    static std::string dump_artifact_key(std::string_view stage);

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    std::string stage_;
    mir::MIRVerificationStage verification_stage_;
};

} // namespace pass
