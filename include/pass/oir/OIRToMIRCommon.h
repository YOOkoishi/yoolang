#pragma once

#include "mir/MIR.h"
#include "oir/OIR.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace target {
struct TargetProfile;
}

namespace pass::oir_to_mir {

bool is_power_of_two(std::uint64_t value);
unsigned log2_u64(std::uint64_t value);
std::string edge_key(const oir::BasicBlock *pred, const oir::BasicBlock *succ);
std::vector<std::uint8_t> lower_global_initializer(const oir::GlobalVariable &global);
mir::Register phys_gpr(const std::string &name);
mir::Register phys_fpr(const std::string &name);

std::unique_ptr<mir::Module> lower_with_stack_slots(const oir::Module &module);
std::unique_ptr<mir::Module> lower_with_vregs(const oir::Module &module,
                                              const target::TargetProfile &profile);

} // namespace pass::oir_to_mir
