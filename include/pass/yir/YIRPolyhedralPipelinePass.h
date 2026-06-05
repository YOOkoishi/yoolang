#pragma once

#include "pass/PassManager.h"

#include <string_view>

namespace pass {

class YIRPolyhedralPipelinePass final : public Pass {
  public:
    explicit YIRPolyhedralPipelinePass(bool run_transform = true);

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    bool run_transform_;
};

} // namespace pass
