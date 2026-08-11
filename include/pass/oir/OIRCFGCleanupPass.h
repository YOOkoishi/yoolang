#pragma once

#include "pass/PassManager.h"

namespace pass {

enum class OIRCFGCleanupMode {
    Full,
    PreserveStructure,
};

class OIRCFGCleanupPass final : public Pass {
  public:
    explicit OIRCFGCleanupPass(OIRCFGCleanupMode mode = OIRCFGCleanupMode::Full)
        : mode_(mode) {}

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    OIRCFGCleanupMode mode_;
};

} // namespace pass
