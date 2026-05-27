#pragma once

#include "PassManager.h"

namespace pass {

class MIRJumpCleanupPass final : public Pass {
  public:
    explicit MIRJumpCleanupPass(bool post_ra = false);

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    bool post_ra_ = false;
};

} // namespace pass
