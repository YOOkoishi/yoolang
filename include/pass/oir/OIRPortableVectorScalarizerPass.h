#pragma once

#include "oir/OIR.h"
#include "pass/PassManager.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace pass::oir_portable {

enum class ScalarizationReasonCode : std::uint8_t {
    Scalarized,
    NoFixedVector,
    InputVerificationFailed,
    SnapshotFailed,
    ScalableVectorUnsupported,
    AggregateABIUnavailable,
    FixedEVLRequired,
    UnsupportedOperation,
    MutationFailed,
    OutputVerificationFailed,
    PostVerificationFailed,
};

std::string_view scalarization_reason_code_name(ScalarizationReasonCode code);

struct PortableVectorScalarizerOptions final {
    // There is currently no profitability gate.  This flag is reserved for
    // pipeline policy and can never bypass legality, ABI, EVL, or verifier
    // checks.
    bool force = false;

    // Runs after the built-in verifier while the transformed module is still
    // a private clone.  Returning false discards that clone and leaves both
    // printer text and use lists of the input module untouched.
    std::function<bool(const oir::Module &, std::string &)> post_transform_verifier;
};

struct PortableVectorScalarizerResult final {
    bool success = true;
    bool changed = false;
    ScalarizationReasonCode code = ScalarizationReasonCode::NoFixedVector;
    unsigned vector_instructions_scalarized = 0;
    unsigned scalar_instructions_created = 0;
    unsigned boundary_instructions_created = 0;
    unsigned cfg_blocks_created = 0;
    unsigned memory_operations_scalarized = 0;
    std::string message;
};

class PortableVectorScalarizer final {
  public:
    explicit PortableVectorScalarizer(PortableVectorScalarizerOptions options = {});

    PortableVectorScalarizerResult run(oir::Module &module) const;

  private:
    PortableVectorScalarizerOptions options_;
};

} // namespace pass::oir_portable

namespace pass {

class OIRPortableVectorScalarizerPass final : public Pass {
  public:
    explicit OIRPortableVectorScalarizerPass(
        oir_portable::PortableVectorScalarizerOptions options = {});

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;

  private:
    oir_portable::PortableVectorScalarizerOptions options_;
};

} // namespace pass
