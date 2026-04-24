#pragma once

#include "PassManager.h"

#include <string_view>

namespace pass {

class ASTToYIRPass final : public Pass {
  public:
    static constexpr const char *kArtifactKey = "yir.module";

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
