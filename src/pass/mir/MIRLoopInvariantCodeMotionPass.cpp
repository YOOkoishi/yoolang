#include "pass/mir/MIRPeepholeCommon.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <vector>

namespace pass::mir_peephole {
namespace {

using Block = mir::MachineBasicBlock;
using BlockSet = std::set<Block *>;

constexpr std::size_t kMaxLICMBlocks = 512;
constexpr std::size_t kMaxLICMInstructions = 12000;
constexpr std::size_t kMaxExtendedLICMLoopBlocks = 8;

struct Loop {
    Block *header = nullptr;
    BlockSet blocks;
    std::vector<Block *> latches;
};

struct DefUseInfo {
    std::map<std::uint32_t, unsigned> def_count;
    std::map<std::uint32_t, Block *> def_block;
    std::map<std::uint32_t, std::vector<Block *>> use_blocks;
};

struct MoveLoc {
    Block *block = nullptr;
    std::size_t index = 0;
};

bool contains(const BlockSet &blocks, Block *block) {
    return blocks.find(block) != blocks.end();
}

bool function_too_large_for_licm(const mir::MachineFunction &function) {
    if (function.blocks().size() > kMaxLICMBlocks) {
        return true;
    }
    std::size_t instructions = 0;
    for (const auto &block : function.blocks()) {
        instructions += block->instructions().size();
        if (instructions > kMaxLICMInstructions) {
            return true;
        }
    }
    return false;
}

BlockSet all_blocks(mir::MachineFunction &function) {
    BlockSet out;
    for (auto &block : function.blocks()) {
        out.insert(block.get());
    }
    return out;
}

BlockSet intersect_sets(const BlockSet &lhs, const BlockSet &rhs) {
    BlockSet out;
    std::set_intersection(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                          std::inserter(out, out.begin()));
    return out;
}

std::map<Block *, BlockSet> compute_dominators(mir::MachineFunction &function) {
    std::map<Block *, BlockSet> dom;
    if (function.blocks().empty()) {
        return dom;
    }

    auto universe = all_blocks(function);
    auto *entry = function.blocks().front().get();
    for (auto &block : function.blocks()) {
        dom[block.get()] = block.get() == entry ? BlockSet{entry} : universe;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto &block_ptr : function.blocks()) {
            auto *block = block_ptr.get();
            if (block == entry) {
                continue;
            }

            BlockSet next = universe;
            if (block->predecessors().empty()) {
                next.clear();
            } else {
                bool first = true;
                for (auto *pred : block->predecessors()) {
                    if (first) {
                        next = dom[pred];
                        first = false;
                    } else {
                        next = intersect_sets(next, dom[pred]);
                    }
                }
            }
            next.insert(block);
            if (next != dom[block]) {
                dom[block] = std::move(next);
                changed = true;
            }
        }
    }

    return dom;
}

bool dominates(const std::map<Block *, BlockSet> &dom, Block *dominator, Block *block) {
    auto found = dom.find(block);
    return found != dom.end() && contains(found->second, dominator);
}

BlockSet collect_natural_loop(Block *header, Block *latch) {
    BlockSet loop;
    std::vector<Block *> stack;
    loop.insert(header);
    if (loop.insert(latch).second) {
        stack.push_back(latch);
    }

    while (!stack.empty()) {
        auto *block = stack.back();
        stack.pop_back();
        for (auto *pred : block->predecessors()) {
            if (loop.insert(pred).second && pred != header) {
                stack.push_back(pred);
            }
        }
    }
    return loop;
}

std::vector<Loop> collect_loops(mir::MachineFunction &function,
                                const std::map<Block *, BlockSet> &dom) {
    std::vector<Loop> loops;
    std::map<Block *, std::size_t> by_header;

    for (auto &block_ptr : function.blocks()) {
        auto *block = block_ptr.get();
        for (auto *succ : block->successors()) {
            if (!dominates(dom, succ, block)) {
                continue;
            }
            auto natural = collect_natural_loop(succ, block);
            auto found = by_header.find(succ);
            if (found == by_header.end()) {
                by_header[succ] = loops.size();
                Loop loop;
                loop.header = succ;
                loop.blocks = std::move(natural);
                loop.latches.push_back(block);
                loops.push_back(std::move(loop));
                continue;
            }

            auto &loop = loops[found->second];
            loop.blocks.insert(natural.begin(), natural.end());
            if (std::find(loop.latches.begin(), loop.latches.end(), block) ==
                loop.latches.end()) {
                loop.latches.push_back(block);
            }
        }
    }

    std::sort(loops.begin(), loops.end(), [](const Loop &lhs, const Loop &rhs) {
        return lhs.blocks.size() < rhs.blocks.size();
    });
    return loops;
}

Block *find_preheader(const Loop &loop) {
    Block *preheader = nullptr;
    for (auto *pred : loop.header->predecessors()) {
        if (contains(loop.blocks, pred)) {
            continue;
        }
        if (preheader != nullptr) {
            return nullptr;
        }
        preheader = pred;
    }
    if (preheader == nullptr || preheader->successors().size() != 1 ||
        preheader->successors().front() != loop.header) {
        return nullptr;
    }
    return preheader;
}

bool is_terminator(mir::Opcode opcode) {
    return opcode == mir::Opcode::Jump || is_conditional_branch(opcode);
}

bool legacy_licm_candidate(mir::Opcode opcode) {
    return opcode == mir::Opcode::SllI || opcode == mir::Opcode::SllIW;
}

std::vector<mir::MachineInstr>::iterator insertion_point(Block *block) {
    auto &instrs = block->instructions();
    auto it = instrs.end();
    while (it != instrs.begin()) {
        auto prev = std::prev(it);
        if (!is_terminator(prev->opcode())) {
            break;
        }
        it = prev;
    }
    return it;
}

bool is_licm_candidate(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::LoadImm:
    case mir::Opcode::LoadFloatImm:
    case mir::Opcode::LoadGlobalAddr:
    case mir::Opcode::LoadStackAddr:
    case mir::Opcode::Add:
    case mir::Opcode::AddW:
    case mir::Opcode::AddI:
    case mir::Opcode::AddIW:
    case mir::Opcode::SubW:
    case mir::Opcode::Mul:
    case mir::Opcode::MulW:
    case mir::Opcode::And:
    case mir::Opcode::AndI:
    case mir::Opcode::SllI:
    case mir::Opcode::SllIW:
    case mir::Opcode::SraI:
    case mir::Opcode::SraIW:
    case mir::Opcode::SrliW:
    case mir::Opcode::Xor:
    case mir::Opcode::XorI:
    case mir::Opcode::Slt:
    case mir::Opcode::SeqZ:
    case mir::Opcode::Snez:
        return true;
    default:
        return false;
    }
}

bool loop_contains_call(const Loop &loop) {
    for (auto *block : loop.blocks) {
        for (const auto &instr : block->instructions()) {
            if (instr.opcode() == mir::Opcode::Call) {
                return true;
            }
        }
    }
    return false;
}

DefUseInfo collect_def_use_info(mir::MachineFunction &function) {
    DefUseInfo info;
    for (auto &block_ptr : function.blocks()) {
        auto *block = block_ptr.get();
        for (const auto &instr : block->instructions()) {
            for (const auto &def : instr.defs()) {
                if (!def.is_virtual()) {
                    continue;
                }
                ++info.def_count[def.id];
                info.def_block[def.id] = block;
            }
            for (const auto &use : instr.uses()) {
                if (use.is_virtual()) {
                    info.use_blocks[use.id].push_back(block);
                }
            }
        }
    }
    return info;
}

bool all_uses_inside_loop(const DefUseInfo &info, const mir::Register &reg, const Loop &loop) {
    auto found = info.use_blocks.find(reg.id);
    if (found == info.use_blocks.end() || found->second.empty()) {
        return false;
    }
    for (auto *block : found->second) {
        if (!contains(loop.blocks, block)) {
            return false;
        }
    }
    return true;
}

bool operand_available_in_preheader(const DefUseInfo &info,
                                    const std::map<Block *, BlockSet> &dom, const Loop &loop,
                                    Block *preheader, const mir::MachineOperand &operand) {
    if (!operand.is_reg() || !operand.is_use()) {
        return true;
    }

    const auto &reg = operand.reg_value();
    if (reg.is_physical()) {
        return is_zero_reg(reg);
    }

    auto def_count = info.def_count.find(reg.id);
    if (def_count == info.def_count.end() || def_count->second != 1) {
        return false;
    }
    auto found = info.def_block.find(reg.id);
    if (found == info.def_block.end()) {
        return false;
    }
    auto *def_block = found->second;
    if (contains(loop.blocks, def_block)) {
        return false;
    }
    return def_block == preheader || dominates(dom, def_block, preheader);
}

bool can_hoist(const DefUseInfo &info, const std::map<Block *, BlockSet> &dom, const Loop &loop,
               Block *preheader, bool extended_allowed, const mir::MachineInstr &instr) {
    if (!is_licm_candidate(instr.opcode())) {
        return false;
    }
    if (!extended_allowed && !legacy_licm_candidate(instr.opcode())) {
        return false;
    }
    for (const auto &operand : instr.operands()) {
        if (operand.is_implicit()) {
            return false;
        }
    }

    const auto defs = instr.defs();
    if (defs.size() != 1 || !defs.front().is_virtual()) {
        return false;
    }
    auto def_count = info.def_count.find(defs.front().id);
    if (def_count == info.def_count.end() || def_count->second != 1) {
        return false;
    }
    if (!all_uses_inside_loop(info, defs.front(), loop)) {
        return false;
    }

    for (const auto &operand : instr.operands()) {
        if (!operand_available_in_preheader(info, dom, loop, preheader, operand)) {
            return false;
        }
    }
    return true;
}

bool hoist_from_loop(mir::MachineFunction &function, const Loop &loop,
                     const std::map<Block *, BlockSet> &dom, Stats &stats) {
    auto *preheader = find_preheader(loop);
    if (preheader == nullptr) {
        return false;
    }

    auto info = collect_def_use_info(function);
    const bool has_call = loop_contains_call(loop);
    const bool extended_allowed = !has_call && loop.blocks.size() <= kMaxExtendedLICMLoopBlocks;
    std::vector<MoveLoc> to_move;
    for (auto &block_ptr : function.blocks()) {
        auto *block = block_ptr.get();
        if (block == preheader || !contains(loop.blocks, block)) {
            continue;
        }
        auto &instrs = block->instructions();
        for (std::size_t index = 0; index < instrs.size(); ++index) {
            if (can_hoist(info, dom, loop, preheader, extended_allowed, instrs[index])) {
                to_move.push_back({block, index});
            }
        }
    }

    if (to_move.empty()) {
        return false;
    }

    std::vector<mir::MachineInstr> moved;
    moved.reserve(to_move.size());
    for (const auto &loc : to_move) {
        moved.push_back(loc.block->instructions()[loc.index]);
    }

    for (auto it = to_move.rbegin(); it != to_move.rend(); ++it) {
        auto &instrs = it->block->instructions();
        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(it->index));
    }

    auto insert = insertion_point(preheader);
    preheader->instructions().insert(insert, moved.begin(), moved.end());
    stats.licm += static_cast<unsigned>(moved.size());
    return true;
}

} // namespace

bool hoist_loop_invariants(mir::MachineFunction &function, bool post_ra, Stats &stats) {
    if (post_ra) {
        return false;
    }
    if (function_too_large_for_licm(function)) {
        return false;
    }

    function.rebuild_cfg();
    auto dom = compute_dominators(function);
    auto loops = collect_loops(function, dom);

    bool changed = false;
    for (const auto &loop : loops) {
        changed |= hoist_from_loop(function, loop, dom, stats);
    }
    return changed;
}

} // namespace pass::mir_peephole
