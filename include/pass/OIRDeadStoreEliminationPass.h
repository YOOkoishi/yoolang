#pragma once

#include "PassManager.h"

namespace pass {

class OIRDeadStoreEliminationPass final : public Pass {
  public:
    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
