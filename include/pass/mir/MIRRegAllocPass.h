#pragma once

#include "pass/PassManager.h"

namespace pass {

class MIRVectorRegAllocPass final : public Pass {
  public:
    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

class MIRScalarRegAllocPass final : public Pass {
  public:
    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

// Compatibility wrapper used by programmatic clients.  The production
// pipeline schedules the three phases separately so CFG-wide VL/VTYPE
// analysis runs after vector allocation and before GPR/FPR allocation.
class MIRRegAllocPass final : public Pass {
  public:
    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
