#pragma once

#include "pass/CostModel.h"
#include "pass/PassManager.h"

#include <string>
#include <string_view>

namespace pass {

class CostModelDiagnosticsPass final : public Pass {
  public:
    static constexpr const char *kReportArtifactKey = cost_model::kReportArtifactKey;

    CostModelDiagnosticsPass(cost_model::CostIRStage stage, std::string label,
                             cost_model::CostModelPolicyKind policy,
                             std::string filter = "");

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    cost_model::CostIRStage stage_;
    std::string label_;
    cost_model::CostModelPolicyKind policy_;
    std::string filter_;
};

} // namespace pass
