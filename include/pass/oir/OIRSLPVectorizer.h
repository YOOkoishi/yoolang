#pragma once

#include "oir/OIR.h"
#include "pass/oir/OIRVectorizationRemark.h"
#include "target/TargetMachine.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pass::oir_vectorize {

// Stable, machine-readable SLP outcome identifiers.  These are deliberately
// independent of the coarser shared RemarkCode contract so SLP diagnostics can
// evolve without changing the loop-vectorizer API.
enum class SLPReasonCode : std::uint8_t {
    Vectorized,
    RejectTooFewLanes,
    RejectUnsupportedType,
    RejectDependence,
    RejectAlias,
    RejectMemoryOrder,
    RejectCall,
    RejectPotentialTrap,
    RejectMemorySemantics,
    RejectCost,
    RejectTargetFeature,
    RejectVerification,
    RejectMutation,
    Disabled,
};

std::string_view slp_reason_code_name(SLPReasonCode code);

struct SLPDiagnostic final {
    SLPReasonCode code = SLPReasonCode::RejectUnsupportedType;
    std::string function;
    std::string block;
    std::string explanation;
};

struct SLPVectorizerOptions final {
    bool enabled = true;
    // Force bypasses profitability only.  Target, dependence, memory-order,
    // trapping-operation, and verifier legality checks always remain active.
    bool force = false;
    unsigned minimum_lanes = 2;
    unsigned preferred_lanes = 4;
    unsigned maximum_lanes = 8;
    // Optional integration verifier.  It runs after the built-in OIR verifier
    // while the mutation journal is still live; failure rolls the module back
    // exactly like a built-in verifier failure.
    std::function<bool(const oir::Module &, std::string &)> post_transform_verifier;
};

struct SLPVectorizerResult final {
    bool success = true;
    bool changed = false;
    unsigned packs_vectorized = 0;
    unsigned scalar_instructions_replaced = 0;
    std::string message;
    std::vector<SLPDiagnostic> diagnostics;
};

class SLPVectorizer final {
  public:
    explicit SLPVectorizer(SLPVectorizerOptions options = {});

    SLPVectorizerResult run(oir::Module &module, const target::TargetProfile &target,
                            RemarkLog &remarks) const;

  private:
    SLPVectorizerOptions options_;
};

} // namespace pass::oir_vectorize
