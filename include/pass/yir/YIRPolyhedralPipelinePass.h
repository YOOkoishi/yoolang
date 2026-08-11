#pragma once

#include "pass/PassManager.h"

#include <string_view>

namespace pass {

enum class YIRPolyhedralPipelineMode {
    Auto,
    Force,
};

class YIRPolyhedralPipelinePass final : public Pass {
  public:
    explicit YIRPolyhedralPipelinePass(bool run_transform = true,
                                       bool enable_rvv_preparation = false);
    YIRPolyhedralPipelinePass(YIRPolyhedralPipelineMode mode, bool run_transform = true,
                              bool enable_rvv_preparation = false);

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    YIRPolyhedralPipelineMode mode_;
    bool run_transform_;
    bool enable_rvv_preparation_;
};

} // namespace pass
