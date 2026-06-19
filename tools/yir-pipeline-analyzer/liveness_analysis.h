#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "yir/YIR.h"

namespace yir_analysis {

struct BlockLiveness {
    std::string block_label;
    std::vector<const yir::Value *> live_in;
    std::vector<const yir::Value *> live_out;
    std::vector<const yir::Value *> defs;
    std::vector<const yir::Value *> uses;
};

struct FunctionLiveness {
    std::string function_name;
    std::vector<BlockLiveness> blocks;
};

struct ModuleLiveness {
    std::vector<FunctionLiveness> functions;
};

ModuleLiveness compute_yir_liveness(const yir::Module &module);

std::string liveness_to_json(const ModuleLiveness &liveness);

} // namespace yir_analysis
