#pragma once

#include "pass/PassManager.h"

namespace pass {

struct OIROptimizationPipelineOptions final {
    // Fat deployment exposes every source definition through a dispatcher.
    // Its private variants must therefore retain the exact public scalar ABI
    // even when every currently visible direct call omits a dead parameter.
    bool preserve_function_signatures = false;
};

class OIROptimizationPipelinePass final : public Pass {
  public:
    explicit OIROptimizationPipelinePass(
        OIROptimizationPipelineOptions options = {});

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    OIROptimizationPipelineOptions options_;
};

} // namespace pass
