#pragma once

#include "pass/PassManager.h"

#include <iosfwd>

namespace pass {

class ASTDumpPass final : public Pass {
  public:
    explicit ASTDumpPass(std::ostream &out);

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    std::ostream &out_;
};

} // namespace pass
