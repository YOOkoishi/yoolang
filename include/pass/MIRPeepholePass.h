#pragma once

#include "PassManager.h"

namespace pass {

class MIRPeepholePass final : public Pass {
  public:
    explicit MIRPeepholePass(bool post_ra = false);

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    bool post_ra_ = false;
};

} // namespace pass
