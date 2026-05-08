#include "../../include/pass/OIRToMIRCommon.h"

#include <cstdint>
#include <sstream>

namespace pass::oir_to_mir {

const std::vector<std::string> kArgRegs = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
const std::vector<std::string> kFArgRegs = {"fa0", "fa1", "fa2", "fa3",
                                            "fa4", "fa5", "fa6", "fa7"};

bool is_power_of_two(std::uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

unsigned log2_u64(std::uint64_t value) {
    unsigned out = 0;
    while (value > 1) {
        value >>= 1;
        ++out;
    }
    return out;
}

std::string edge_key(const oir::BasicBlock *pred, const oir::BasicBlock *succ) {
    std::ostringstream oss;
    oss << reinterpret_cast<std::uintptr_t>(pred) << ":" << reinterpret_cast<std::uintptr_t>(succ);
    return oss.str();
}

mir::Register phys_gpr(const std::string &name) {
    return mir::Register::physical(name, mir::RegisterClass::GPR);
}

mir::Register phys_fpr(const std::string &name) {
    return mir::Register::physical(name, mir::RegisterClass::FPR32);
}

bool should_use_conservative_lowering_for_size(const oir::Module &module) {
    constexpr std::size_t kMaxVRegBlocks = 4096;
    constexpr std::size_t kMaxVRegInstructions = 12000;

    std::size_t blocks = 0;
    std::size_t instructions = 0;
    for (const auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        blocks += function->blocks().size();
        for (const auto &block : function->blocks()) {
            instructions += block->instructions().size();
        }
    }
    return blocks > kMaxVRegBlocks || instructions > kMaxVRegInstructions;
}

} // namespace pass::oir_to_mir
