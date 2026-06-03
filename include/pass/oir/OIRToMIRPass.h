#pragma once

#include "pass/PassManager.h"

namespace pass {

class OIRToMIRPass final : public Pass {
  public:
    explicit OIRToMIRPass(bool use_virtual_registers = false);

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    bool use_virtual_registers_ = false;
};

} // namespace pass
