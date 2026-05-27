#pragma once

#include "../mir/MIR.h"
#include "PassManager.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pass::mir_combine {

using VRegId = std::uint32_t;

struct Stats {
    unsigned branches = 0;
    unsigned immediates = 0;
    unsigned address_folds = 0;
    unsigned bit_idioms = 0;
    unsigned dead = 0;

    std::string message() const;
};

struct RegCounts {
    unsigned defs = 0;
    unsigned uses = 0;
};

struct ImmUse {
    std::size_t producer = 0;
    std::int64_t value = 0;
};

using Transform = bool (*)(mir::MachineFunction &, Stats &);

bool same_reg(const mir::Register &lhs, const mir::Register &rhs);
bool same_reg(const mir::MachineOperand &lhs, const mir::MachineOperand &rhs);
bool is_zero_reg(const mir::Register &reg);
bool is_zero_reg(const mir::MachineOperand &operand);
mir::Register zero_reg();
bool fits_simm12(std::int64_t value);
bool neg_fits_simm12(std::int64_t value);
bool is_power_of_two(std::uint64_t value);
unsigned log2_u64(std::uint64_t value);
bool is_move(mir::Opcode opcode);
bool is_pure_def(mir::Opcode opcode);
mir::MachineInstr make_move_like(const mir::MachineOperand &dst,
                                 const mir::MachineOperand &src);
mir::MachineInstr make_move_like(const mir::MachineOperand &dst, const mir::Register &src);
std::map<VRegId, RegCounts> count_vregs(const mir::MachineFunction &function);
unsigned use_count(const std::map<VRegId, RegCounts> &counts, const mir::Register &reg);
unsigned def_count(const std::map<VRegId, RegCounts> &counts, const mir::Register &reg);
bool single_use_vreg(const std::map<VRegId, RegCounts> &counts, const mir::Register &reg);
std::optional<std::size_t> find_def_before(const std::vector<mir::MachineInstr> &instrs,
                                           std::size_t before,
                                           const mir::Register &reg);
std::optional<ImmUse> find_single_use_load_imm(const std::vector<mir::MachineInstr> &instrs,
                                               std::size_t user_index,
                                               const mir::MachineOperand &operand,
                                               const std::map<VRegId, RegCounts> &counts);
void erase_producer(std::vector<mir::MachineInstr> &instrs, std::size_t &user_index,
                    std::size_t producer_index, Stats &stats);
PassResult run_transform(PassContext &context, std::string_view pass_name, Transform transform);

bool combine_immediates(mir::MachineFunction &function, Stats &stats);
bool combine_address_modes(mir::MachineFunction &function, Stats &stats);
bool combine_compare_branches(mir::MachineFunction &function, Stats &stats);
bool combine_rem_zero_branches(mir::MachineFunction &function, Stats &stats);
bool combine_bit_idioms(mir::MachineFunction &function, Stats &stats);
bool remove_dead_defs(mir::MachineFunction &function, Stats &stats);

} // namespace pass::mir_combine
