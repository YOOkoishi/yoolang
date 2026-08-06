#pragma once

#include "pass/PassManager.h"

namespace pass {

class ASTSemanticAnalysisPass final : public Pass {
  public:
    static constexpr const char *kArtifactKey = "ast.semantic-model";

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
