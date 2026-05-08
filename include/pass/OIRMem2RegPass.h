#pragma once

#include "PassManager.h"

namespace pass {

class OIRMem2RegPass final : public Pass {
  public:
    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
