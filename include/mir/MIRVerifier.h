#pragma once

#include "MIR.h"

#include <string>

namespace mir {

enum class MIRVerificationStage {
    PreRA,
    PostRA,
};

struct MIRVerifyResult {
    bool ok = true;
    std::string message;
};

MIRVerifyResult verify_module(const Module &module, MIRVerificationStage stage);

} // namespace mir
