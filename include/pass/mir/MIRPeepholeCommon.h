#pragma once

#include "mir/MIR.h"
#include "pass/PassManager.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace pass::mir_peephole {

using VRegId = std::uint32_t;

struct Stats {
    unsigned copies = 0;
    unsigned loads = 0;
    unsigned stores = 0;
    unsigned jumps = 0;
    unsigned arithmetic = 0;
    unsigned branches = 0;
    unsigned address_folds = 0;
    unsigned cse = 0;
    unsigned licm = 0;
    unsigned dead = 0;

    bool changed() const;
    std::string message() const;
};

struct RegCounts {
    unsigned defs = 0;
    unsigned uses = 0;
};

using Transform = bool (*)(mir::MachineFunction &, bool, Stats &);

bool same_reg(const mir::MachineOperand &lhs, const mir::MachineOperand &rhs);
bool same_reg(const mir::Register &lhs, const mir::Register &rhs);
bool is_zero_reg(const mir::MachineOperand &operand);
bool is_zero_reg(const mir::Register &reg);
bool same_slot(const mir::MachineOperand &lhs, const mir::MachineOperand &rhs);
bool is_move(mir::Opcode opcode);
bool is_conditional_branch(mir::Opcode opcode);
std::size_t branch_target_index(mir::Opcode opcode);
std::optional<mir::Opcode> inverted_branch(mir::Opcode opcode);
bool fits_simm12(std::int64_t value);
mir::MachineInstr make_move_like(const mir::MachineOperand &dst,
                                 const mir::MachineOperand &src);
mir::MachineInstr make_move_like(mir::Register dst, mir::Register src);
std::map<VRegId, RegCounts> count_vregs(const mir::MachineFunction &function);
unsigned use_count(const std::map<VRegId, RegCounts> &counts, const mir::Register &reg);
unsigned def_count(const std::map<VRegId, RegCounts> &counts, const mir::Register &reg);
bool defines_reg(const mir::MachineInstr &instr, const mir::Register &reg);
bool is_pure_def(mir::Opcode opcode);
PassResult run_transform(PassContext &context, std::string_view pass_name, bool post_ra,
                         Transform transform);

bool coalesce_copies(mir::MachineFunction &function, bool post_ra, Stats &stats);
bool fuse_compare_branches(mir::MachineFunction &function, bool post_ra, Stats &stats);
bool local_cse(mir::MachineFunction &function, bool post_ra, Stats &stats);
bool hoist_loop_invariants(mir::MachineFunction &function, bool post_ra, Stats &stats);
bool fold_address_offsets(mir::MachineFunction &function, bool post_ra, Stats &stats);
bool optimize_pointer_loop_exits(mir::MachineFunction &function, bool post_ra, Stats &stats);
bool cleanup_jumps(mir::MachineFunction &function, bool post_ra, Stats &stats);
bool remove_dead_defs(mir::MachineFunction &function, bool post_ra, Stats &stats);
bool simplify_blocks(mir::MachineFunction &function, bool post_ra, Stats &stats);

} // namespace pass::mir_peephole
