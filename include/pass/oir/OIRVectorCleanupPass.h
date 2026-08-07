#pragma once

#include "pass/PassManager.h"

namespace pass {

// A deliberately small post-vectorization cleanup.  It removes only dead,
// side-effect-free vector computations whose semantics have been audited for
// VP/fixed/scalable OIR.  The scalar fixed-point pipeline must not be rerun on
// vector IR merely to obtain this cleanup.
class OIRVectorCleanupPass final : public Pass {
  public:
    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
