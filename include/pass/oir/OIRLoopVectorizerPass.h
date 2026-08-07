#pragma once

#include "pass/PassManager.h"
#include "pass/oir/OIRVectorization.h"

namespace pass {

class OIRLoopVectorizerPass final : public Pass {
  public:
    explicit OIRLoopVectorizerPass(
        oir_vectorize::LoopVectorizerOptions options = {});

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    oir_vectorize::LoopVectorizerOptions options_;
};

} // namespace pass
