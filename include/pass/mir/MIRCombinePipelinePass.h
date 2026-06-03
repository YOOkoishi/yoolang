#pragma once

#include "pass/PassManager.h"

namespace pass {

class MIRCombinePipelinePass final : public Pass {
  public:
    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
