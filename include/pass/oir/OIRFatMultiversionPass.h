#pragma once

#include "pass/PassManager.h"

namespace pass {

struct OIRFatMultiversionOptions final {
    bool loop_vectorize = false;
    bool slp_vectorize = false;
    bool explore_interleave = false;
    bool optimize_mir = false;
};

// Compiles two private variants from a verified OIR snapshot and publishes a
// single baseline-ISA assembly artifact containing standard-LP64D dispatchers.
// Fixed vectors and masks retain their ordinary aggregate FunctionType and do
// not cross dispatchers in vector registers.
// The input OIR module is never mutated.
class OIRFatMultiversionPass final : public Pass {
  public:
    explicit OIRFatMultiversionPass(OIRFatMultiversionOptions options = {});

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    OIRFatMultiversionOptions options_;
};

} // namespace pass
