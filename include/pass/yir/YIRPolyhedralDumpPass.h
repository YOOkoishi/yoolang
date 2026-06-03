#pragma once

#include "pass/PassManager.h"

#include <iosfwd>
#include <string_view>

namespace pass {

class YIRPolyhedralDumpPass final : public Pass {
  public:
    explicit YIRPolyhedralDumpPass(std::ostream &out);

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    std::ostream &out_;
};

} // namespace pass
