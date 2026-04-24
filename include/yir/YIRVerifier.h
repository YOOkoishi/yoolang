#pragma once

#include "YIR.h"

#include <string>
#include <vector>

namespace yir {

struct VerifyResult {
    bool success = true;
    std::vector<std::string> errors;
};

VerifyResult verify_high_level_yir(const Module &module);

} // namespace yir
