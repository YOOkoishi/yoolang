#pragma once

#include "pass/PassManager.h"
#include "pass/oir/OIRSLPVectorizer.h"

namespace pass {

class OIRSLPVectorizerPass final : public Pass {
  public:
    explicit OIRSLPVectorizerPass(oir_vectorize::SLPVectorizerOptions options = {});

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    oir_vectorize::SLPVectorizerOptions options_;
};

} // namespace pass
