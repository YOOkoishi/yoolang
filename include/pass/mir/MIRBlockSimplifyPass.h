#pragma once

#include "pass/PassManager.h"

namespace pass {

class MIRBlockSimplifyPass final : public Pass {
  public:
    explicit MIRBlockSimplifyPass(bool post_ra = false);

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    bool post_ra_ = false;
};

} // namespace pass
