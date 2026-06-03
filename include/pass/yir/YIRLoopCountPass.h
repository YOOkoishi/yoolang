#pragma once

#include "pass/PassManager.h"
#include <iosfwd>

namespace pass {

class YIRLoopCountPass final : public Pass {
  public:
    explicit YIRLoopCountPass(std::ostream &out) : out_(out) {}

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    std::ostream &out_;
};

} // namespace pass
