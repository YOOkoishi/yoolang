#include "pass/mir/MIRCopyCoalescingPass.h"

#include "pass/mir/MIRPeepholeCommon.h"

#include <map>
#include <set>
#include <vector>

namespace pass::mir_peephole {
namespace {

struct UseSite {
    mir::MachineInstr *instr = nullptr;
};

bool has_reg_use(const mir::MachineInstr &instr, const mir::Register &reg) {
    for (const auto &operand : instr.operands()) {
        if (operand.is_reg() && operand.is_use() && !operand.is_implicit() &&
            same_reg(operand.reg_value(), reg)) {
            return true;
        }
    }
    return false;
}

unsigned replace_reg_uses(mir::MachineInstr &instr, const mir::Register &old_reg,
                          const mir::Register &new_reg) {
    unsigned replaced = 0;
    for (auto &operand : instr.operands()) {
        if (!operand.is_reg() || !operand.is_use() || operand.is_implicit() ||
            !same_reg(operand.reg_value(), old_reg)) {
            continue;
        }
        operand.set_reg(new_reg);
        ++replaced;
    }
    return replaced;
}

bool can_copy_propagate_source(const mir::Register &src) {
    return src.is_virtual() || is_zero_reg(src);
}

std::map<const mir::MachineBasicBlock *, std::size_t>
block_indices(const mir::MachineFunction &function) {
    std::map<const mir::MachineBasicBlock *, std::size_t> out;
    for (std::size_t index = 0; index < function.blocks().size(); ++index) {
        out[function.blocks()[index].get()] = index;
    }
    return out;
}

bool find_all_replaceable_uses(
    mir::MachineFunction &function,
    const std::map<const mir::MachineBasicBlock *, std::size_t> &indices,
    std::size_t block_index, std::size_t instr_index, const mir::Register &dst,
    const mir::Register &src, unsigned total_uses, std::vector<UseSite> &use_sites) {
    unsigned found_uses = 0;
    std::set<const mir::MachineBasicBlock *> visited;
    auto *block = function.blocks()[block_index].get();
    std::size_t scan_index = instr_index + 1;

    while (block != nullptr) {
        if (!visited.insert(block).second) {
            return false;
        }

        auto &instrs = block->instructions();
        for (std::size_t i = scan_index; i < instrs.size(); ++i) {
            auto &instr = instrs[i];
            if (instr.opcode() == mir::Opcode::Call || defines_reg(instr, src) ||
                defines_reg(instr, dst)) {
                return false;
            }
            if (has_reg_use(instr, dst)) {
                use_sites.push_back({&instr});
                for (const auto &operand : instr.operands()) {
                    if (operand.is_reg() && operand.is_use() && !operand.is_implicit() &&
                        same_reg(operand.reg_value(), dst)) {
                        ++found_uses;
                    }
                }
                if (found_uses == total_uses) {
                    return true;
                }
            }
        }

        if (block->successors().size() != 1) {
            return false;
        }
        auto *succ = block->successors().front();
        if (succ == nullptr || succ->predecessors().size() != 1) {
            return false;
        }
        auto found_index = indices.find(succ);
        if (found_index == indices.end()) {
            return false;
        }
        block = succ;
        scan_index = 0;
    }

    return false;
}

bool has_side_effects(const mir::MachineInstr &instr) {
    switch (instr.opcode()) {
    case mir::Opcode::Call:
    case mir::Opcode::LoadMem:
    case mir::Opcode::StoreMem:
    case mir::Opcode::LoadMemOffset:
    case mir::Opcode::StoreMemOffset:
    case mir::Opcode::MemZero:
    case mir::Opcode::LoadSlot:
    case mir::Opcode::StoreSlot:
    case mir::Opcode::StoreOutgoingArg:
    case mir::Opcode::LoadIncomingArg:
    case mir::Opcode::BranchNonZero:
    case mir::Opcode::BranchZero:
    case mir::Opcode::BranchEq:
    case mir::Opcode::BranchNe:
    case mir::Opcode::BranchLT:
    case mir::Opcode::BranchGE:
    case mir::Opcode::Jump:
        return true;
    default:
        return false;
    }
}

// Retarget the def of an MV-source producer. For `MV vdst, vsrc` where:
//   - both regs are virtual,
//   - vsrc has exactly one def in this block, exactly one use (the MV),
//   - vdst has exactly one def (the MV),
//   - the producer of vsrc is a non-side-effecting instr defining only vsrc and
//     not using vdst,
//   - no instruction between the producer and the MV reads vdst,
// rewrite the producer to define vdst directly and erase the MV.
bool elide_move_via_def_retarget(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;

    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size();) {
            auto &instr = instrs[i];
            const auto &ops = instr.operands();
            if (!is_move(instr.opcode()) || ops.size() < 2 || !ops[0].is_reg() ||
                !ops[1].is_reg()) {
                ++i;
                continue;
            }
            auto dst = ops[0].reg_value();
            auto src = ops[1].reg_value();
            if (!dst.is_virtual() || !src.is_virtual() || same_reg(dst, src)) {
                ++i;
                continue;
            }
            if (def_count(counts, dst) != 1 || def_count(counts, src) != 1 ||
                use_count(counts, src) != 1) {
                ++i;
                continue;
            }

            // Find the unique def of src in this block, scanning back.
            std::size_t producer_index = 0;
            bool found_producer = false;
            for (std::size_t k = i; k > 0; --k) {
                auto &candidate = instrs[k - 1];
                if (defines_reg(candidate, src)) {
                    producer_index = k - 1;
                    found_producer = true;
                    break;
                }
            }
            if (!found_producer) {
                ++i;
                continue;
            }
            auto &producer = instrs[producer_index];
            if (has_side_effects(producer)) {
                ++i;
                continue;
            }
            // The producer must define src and only src among virtual defs.
            std::size_t src_def_slot = ops.size();
            bool only_src_virtual_def = true;
            const auto &producer_ops = producer.operands();
            for (std::size_t op_idx = 0; op_idx < producer_ops.size(); ++op_idx) {
                const auto &op = producer_ops[op_idx];
                if (!op.is_reg() || !op.is_def()) {
                    continue;
                }
                if (op.is_implicit() && op.reg_value().is_physical()) {
                    continue;
                }
                if (op.reg_value().is_virtual() && same_reg(op.reg_value(), src)) {
                    src_def_slot = op_idx;
                    continue;
                }
                only_src_virtual_def = false;
                break;
            }
            if (src_def_slot >= producer_ops.size() || !only_src_virtual_def) {
                ++i;
                continue;
            }

            // Producer must not read dst (would create a self-use cycle).
            bool producer_reads_dst = false;
            for (const auto &op : producer_ops) {
                if (op.is_reg() && op.is_use() && !op.is_implicit() &&
                    same_reg(op.reg_value(), dst)) {
                    producer_reads_dst = true;
                    break;
                }
            }
            if (producer_reads_dst) {
                ++i;
                continue;
            }

            // No instruction between producer and the MV may read dst.
            bool intervening_reads_dst = false;
            for (std::size_t k = producer_index + 1; k < i; ++k) {
                if (has_reg_use(instrs[k], dst) || defines_reg(instrs[k], dst)) {
                    intervening_reads_dst = true;
                    break;
                }
            }
            if (intervening_reads_dst) {
                ++i;
                continue;
            }

            // Rewrite producer's def from src to dst.
            auto new_operands = producer.operands();
            new_operands[src_def_slot].set_reg(dst);
            producer = mir::MachineInstr(producer.opcode(), std::move(new_operands));

            // Erase the MV.
            instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
            ++stats.copies;
            changed = true;
            counts = count_vregs(function);
            // Restart from producer position to allow chained eliminations.
            i = producer_index;
        }
    }

    return changed;
}

bool coalesce_copies_once(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    auto indices = block_indices(function);
    bool changed = false;

    for (std::size_t block_index = 0; block_index < function.blocks().size(); ++block_index) {
        auto &block_ptr = function.blocks()[block_index];
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size();) {
            auto &instr = instrs[i];
            const auto &ops = instr.operands();
            if (!is_move(instr.opcode()) || ops.size() < 2 || !ops[0].is_reg() ||
                !ops[1].is_reg()) {
                ++i;
                continue;
            }

            auto dst = ops[0].reg_value();
            auto src = ops[1].reg_value();
            if (same_reg(dst, src)) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                ++stats.copies;
                changed = true;
                continue;
            }
            if (!dst.is_virtual() || !can_copy_propagate_source(src) ||
                def_count(counts, dst) != 1) {
                ++i;
                continue;
            }

            const unsigned total_uses = use_count(counts, dst);
            if (total_uses == 0) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                ++stats.copies;
                changed = true;
                continue;
            }

            std::vector<UseSite> use_sites;
            if (find_all_replaceable_uses(function, indices, block_index, i, dst, src,
                                          total_uses, use_sites)) {
                for (auto site : use_sites) {
                    replace_reg_uses(*site.instr, dst, src);
                }
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                ++stats.copies;
                changed = true;
                continue;
            }
            ++i;
        }
    }

    return changed;
}

} // namespace

bool coalesce_copies(mir::MachineFunction &function, bool post_ra, Stats &stats) {
    (void)post_ra;

    bool changed = false;
    for (int iteration = 0; iteration < 8; ++iteration) {
        bool iter_changed = coalesce_copies_once(function, stats);
        iter_changed |= elide_move_via_def_retarget(function, stats);
        if (!iter_changed) {
            break;
        }
        changed = true;
    }
    return changed;
}

} // namespace pass::mir_peephole

namespace pass {

std::string_view MIRCopyCoalescingPass::name() const {
    return "MIRCopyCoalescingPass";
}

PassKind MIRCopyCoalescingPass::kind() const {
    return PassKind::Transform;
}

PassResult MIRCopyCoalescingPass::run(PassContext &context) {
    return mir_peephole::run_transform(context, name(), false, mir_peephole::coalesce_copies);
}

} // namespace pass
