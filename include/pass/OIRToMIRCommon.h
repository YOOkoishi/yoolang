#pragma once

#include "../../include/mir/MIR.h"
#include "../../include/oir/oir.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pass::oir_to_mir {

extern const std::vector<std::string> kArgRegs;
extern const std::vector<std::string> kFArgRegs;

bool is_power_of_two(std::uint64_t value);
unsigned log2_u64(std::uint64_t value);
std::string edge_key(const oir::BasicBlock *pred, const oir::BasicBlock *succ);
mir::Register phys_gpr(const std::string &name);
mir::Register phys_fpr(const std::string &name);

std::unique_ptr<mir::Module> lower_with_stack_slots(const oir::Module &module);
std::unique_ptr<mir::Module> lower_with_vregs(const oir::Module &module);
bool should_use_conservative_lowering_for_size(const oir::Module &module);

} // namespace pass::oir_to_mir
