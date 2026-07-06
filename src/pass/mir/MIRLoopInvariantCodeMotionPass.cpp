#include "pass/mir/MIRPeepholeCommon.h"

#include "pass/mir/MIRCostModel.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
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

struct DivLoc {
    Block *block = nullptr;
    std::size_t index = 0;
};

struct DivReciprocal {
    mir::Register divisor32;
    mir::Register abs_divisor;
    mir::Register magic;
    mir::Register zero_mask;
    mir::Register neg_one;
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
    case mir::Opcode::Sub:
    case mir::Opcode::SubW:
    case mir::Opcode::Mul:
    case mir::Opcode::MulW:
    case mir::Opcode::And:
    case mir::Opcode::AndI:
    case mir::Opcode::SllI:
    case mir::Opcode::SllIW:
    case mir::Opcode::SraI:
    case mir::Opcode::SraIW:
    case mir::Opcode::Srli:
    case mir::Opcode::SrliW:
    case mir::Opcode::Xor:
    case mir::Opcode::XorI:
    case mir::Opcode::Slt:
    case mir::Opcode::Sltu:
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

bool allows_mir_loop_transform(Stats &stats, pass::cost_model::TransformKind kind,
                               const std::string &candidate_id,
                               std::int64_t before_dynamic_instrs,
                               std::int64_t after_dynamic_instrs,
                               std::int64_t before_cycles,
                               std::int64_t after_cycles,
                               std::int64_t register_pressure_growth) {
    pass::mir_cost_model::MIRTransformCostEstimate estimate;
    estimate.kind = kind;
    estimate.stage = pass::cost_model::CostIRStage::PreRAMIR;
    estimate.pass_name = "MIRLoopInvariantCodeMotionPass";
    estimate.candidate_id = candidate_id;
    estimate.scope = "loop";
    estimate.proof_summary = "preheader availability and loop-contained use checks";
    estimate.confidence = kind == pass::cost_model::TransformKind::LoopInvariantCodeMotion
                              ? 0.68
                              : 0.62;
    estimate.before_dynamic_instrs = before_dynamic_instrs;
    estimate.after_dynamic_instrs = after_dynamic_instrs;
    estimate.before_cycles = before_cycles;
    estimate.after_cycles = after_cycles;
    estimate.risk.register_pressure_growth = register_pressure_growth;
    estimate.risk.live_range_growth = register_pressure_growth;
    return pass::mir_cost_model::allows_transform(stats.cost_model_report,
                                                  stats.cost_model_policy,
                                                  stats.cost_model_filter, estimate);
}

mir::Register create_gpr(mir::MachineFunction &function,
                         mir::ValueType type = mir::ValueType::I32) {
    return function.regs().create_virtual(mir::RegisterClass::GPR, type);
}

mir::Register create_gpr64(mir::MachineFunction &function) {
    return create_gpr(function, mir::ValueType::Ptr);
}

mir::Register zero_reg() {
    return mir::Register::physical("zero", mir::RegisterClass::GPR);
}

mir::MachineOperand def(mir::Register reg) {
    return mir::MachineOperand::reg_def(reg);
}

mir::MachineOperand use(mir::Register reg) {
    return mir::MachineOperand::reg_use(reg);
}

mir::MachineOperand imm(std::int64_t value) {
    return mir::MachineOperand::imm(value);
}

bool operand_available_in_preheader(const DefUseInfo &info,
                                    const std::map<Block *, BlockSet> &dom, const Loop &loop,
                                    Block *preheader, const mir::MachineOperand &operand);

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

std::vector<DivLoc> collect_loop_divisions(const Loop &loop, const DefUseInfo &info,
                                           const std::map<Block *, BlockSet> &dom,
                                           Block *preheader, mir::Register *divisor) {
    std::vector<DivLoc> divs;
    std::optional<mir::Register> candidate_divisor;

    for (auto *block : loop.blocks) {
        const auto &instrs = block->instructions();
        for (std::size_t index = 0; index < instrs.size(); ++index) {
            const auto &instr = instrs[index];
            if (instr.opcode() != mir::Opcode::DivW) {
                continue;
            }

            const auto &ops = instr.operands();
            if (ops.size() < 3 || !ops[0].is_reg() || !ops[1].is_reg() || !ops[2].is_reg() ||
                !ops[0].is_def() || !ops[1].is_use() || !ops[2].is_use() ||
                !ops[0].reg_value().is_virtual()) {
                return {};
            }

            const auto &rhs = ops[2].reg_value();
            if (rhs.is_physical()) {
                return {};
            }
            if (!operand_available_in_preheader(info, dom, loop, preheader, ops[2])) {
                return {};
            }
            if (candidate_divisor && !same_reg(*candidate_divisor, rhs)) {
                return {};
            }
            candidate_divisor = rhs;
            divs.push_back({block, index});
        }
    }

    if (!candidate_divisor || divs.empty()) {
        return {};
    }
    *divisor = *candidate_divisor;
    return divs;
}

DivReciprocal insert_divisor_reciprocal_setup(mir::MachineFunction &function, Block *preheader,
                                              mir::Register divisor) {
    DivReciprocal rec;
    rec.divisor32 = create_gpr(function);
    auto sign = create_gpr64(function);
    auto abs_xor = create_gpr64(function);
    rec.abs_divisor = create_gpr64(function);
    auto numerator = create_gpr64(function);
    rec.magic = create_gpr64(function);
    auto zero_flag = create_gpr(function);
    rec.zero_mask = create_gpr64(function);
    rec.neg_one = create_gpr64(function);

    std::vector<mir::MachineInstr> setup;
    setup.reserve(9);
    setup.emplace_back(mir::Opcode::AddIW, std::vector<mir::MachineOperand>{
                                             def(rec.divisor32), use(divisor), imm(0)});
    setup.emplace_back(mir::Opcode::SraI, std::vector<mir::MachineOperand>{
                                            def(sign), use(rec.divisor32), imm(31)});
    setup.emplace_back(mir::Opcode::Xor, std::vector<mir::MachineOperand>{
                                           def(abs_xor), use(rec.divisor32), use(sign)});
    setup.emplace_back(mir::Opcode::Sub, std::vector<mir::MachineOperand>{
                                           def(rec.abs_divisor), use(abs_xor), use(sign)});
    setup.emplace_back(mir::Opcode::LoadImm, std::vector<mir::MachineOperand>{
                                               def(numerator), imm(0x100000000LL)});
    setup.emplace_back(mir::Opcode::DivU, std::vector<mir::MachineOperand>{
                                            def(rec.magic), use(numerator), use(rec.abs_divisor)});
    setup.emplace_back(mir::Opcode::SeqZ, std::vector<mir::MachineOperand>{
                                            def(zero_flag), use(rec.divisor32)});
    setup.emplace_back(mir::Opcode::Sub, std::vector<mir::MachineOperand>{
                                           def(rec.zero_mask), use(zero_reg()), use(zero_flag)});
    setup.emplace_back(mir::Opcode::LoadImm, std::vector<mir::MachineOperand>{
                                               def(rec.neg_one), imm(-1)});

    auto insert = insertion_point(preheader);
    preheader->instructions().insert(insert, setup.begin(), setup.end());
    return rec;
}

std::vector<mir::MachineInstr> make_div_reciprocal_sequence(mir::MachineFunction &function,
                                                            const mir::MachineInstr &div,
                                                            const DivReciprocal &rec) {
    const auto &ops = div.operands();
    const auto dst = ops[0].reg_value();
    const auto numerator = ops[1].reg_value();

    auto numerator32 = create_gpr(function);
    auto sign_n = create_gpr64(function);
    auto abs_n_xor = create_gpr64(function);
    auto abs_n = create_gpr64(function);
    auto product = create_gpr64(function);
    auto q0 = create_gpr64(function);
    auto q_product = create_gpr64(function);
    auto rem = create_gpr64(function);
    auto rem_lt_divisor = create_gpr(function);
    auto needs_correction = create_gpr(function);
    auto q_abs = create_gpr64(function);
    auto sign_xor = create_gpr(function);
    auto sign_q = create_gpr64(function);
    auto signed_xor = create_gpr64(function);
    auto signed64 = create_gpr64(function);
    auto signed32 = create_gpr(function);
    auto zero_xor = create_gpr64(function);
    auto zero_adjust = create_gpr64(function);

    std::vector<mir::MachineInstr> seq;
    seq.reserve(19);
    seq.emplace_back(mir::Opcode::AddIW, std::vector<mir::MachineOperand>{
                                             def(numerator32), use(numerator), imm(0)});
    seq.emplace_back(mir::Opcode::SraI, std::vector<mir::MachineOperand>{
                                            def(sign_n), use(numerator32), imm(31)});
    seq.emplace_back(mir::Opcode::Xor, std::vector<mir::MachineOperand>{
                                           def(abs_n_xor), use(numerator32), use(sign_n)});
    seq.emplace_back(mir::Opcode::Sub, std::vector<mir::MachineOperand>{
                                           def(abs_n), use(abs_n_xor), use(sign_n)});
    seq.emplace_back(mir::Opcode::Mul, std::vector<mir::MachineOperand>{
                                           def(product), use(abs_n), use(rec.magic)});
    seq.emplace_back(mir::Opcode::Srli, std::vector<mir::MachineOperand>{
                                            def(q0), use(product), imm(32)});
    seq.emplace_back(mir::Opcode::Mul, std::vector<mir::MachineOperand>{
                                           def(q_product), use(q0), use(rec.abs_divisor)});
    seq.emplace_back(mir::Opcode::Sub, std::vector<mir::MachineOperand>{
                                           def(rem), use(abs_n), use(q_product)});
    seq.emplace_back(mir::Opcode::Sltu, std::vector<mir::MachineOperand>{
                                            def(rem_lt_divisor), use(rem), use(rec.abs_divisor)});
    seq.emplace_back(mir::Opcode::XorI, std::vector<mir::MachineOperand>{
                                            def(needs_correction), use(rem_lt_divisor), imm(1)});
    seq.emplace_back(mir::Opcode::Add, std::vector<mir::MachineOperand>{
                                           def(q_abs), use(q0), use(needs_correction)});
    seq.emplace_back(mir::Opcode::Xor, std::vector<mir::MachineOperand>{
                                           def(sign_xor), use(numerator32), use(rec.divisor32)});
    seq.emplace_back(mir::Opcode::SraI, std::vector<mir::MachineOperand>{
                                            def(sign_q), use(sign_xor), imm(31)});
    seq.emplace_back(mir::Opcode::Xor, std::vector<mir::MachineOperand>{
                                           def(signed_xor), use(q_abs), use(sign_q)});
    seq.emplace_back(mir::Opcode::Sub, std::vector<mir::MachineOperand>{
                                           def(signed64), use(signed_xor), use(sign_q)});
    seq.emplace_back(mir::Opcode::AddW, std::vector<mir::MachineOperand>{
                                            def(signed32), use(signed64), use(zero_reg())});
    seq.emplace_back(mir::Opcode::Xor, std::vector<mir::MachineOperand>{
                                           def(zero_xor), use(signed32), use(rec.neg_one)});
    seq.emplace_back(mir::Opcode::And, std::vector<mir::MachineOperand>{
                                           def(zero_adjust), use(zero_xor), use(rec.zero_mask)});
    seq.emplace_back(mir::Opcode::Xor, std::vector<mir::MachineOperand>{
                                           def(dst), use(signed32), use(zero_adjust)});
    return seq;
}

bool reduce_loop_divisions(mir::MachineFunction &function, const Loop &loop,
                           const std::map<Block *, BlockSet> &dom, Stats &stats) {
    if (loop_contains_call(loop)) {
        return false;
    }
    auto *preheader = find_preheader(loop);
    if (preheader == nullptr) {
        return false;
    }

    auto info = collect_def_use_info(function);
    mir::Register divisor;
    auto divs = collect_loop_divisions(loop, info, dom, preheader, &divisor);
    if (divs.empty()) {
        return false;
    }
    constexpr std::int64_t kUnknownTripCountProfitabilityScale = 4;
    const auto div_count = static_cast<std::int64_t>(divs.size());
    if (!allows_mir_loop_transform(
            stats, pass::cost_model::TransformKind::StrengthReduction,
            "div-reciprocal." + std::to_string(stats.arithmetic + 1),
            div_count * 12 * kUnknownTripCountProfitabilityScale,
            div_count * 9 * kUnknownTripCountProfitabilityScale,
            div_count * 24 * kUnknownTripCountProfitabilityScale,
            div_count * 9 * kUnknownTripCountProfitabilityScale, div_count + 2)) {
        return false;
    }

    auto reciprocal = insert_divisor_reciprocal_setup(function, preheader, divisor);
    for (auto it = divs.rbegin(); it != divs.rend(); ++it) {
        auto &instrs = it->block->instructions();
        if (it->index >= instrs.size()) {
            continue;
        }
        auto replacement = make_div_reciprocal_sequence(function, instrs[it->index], reciprocal);
        auto pos = instrs.begin() + static_cast<std::ptrdiff_t>(it->index);
        pos = instrs.erase(pos);
        instrs.insert(pos, replacement.begin(), replacement.end());
    }

    stats.arithmetic += static_cast<unsigned>(divs.size());
    return true;
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
    if (!allows_mir_loop_transform(
            stats, pass::cost_model::TransformKind::LoopInvariantCodeMotion,
            "licm." + std::to_string(stats.licm + 1),
            static_cast<std::int64_t>(to_move.size()) * 4,
            static_cast<std::int64_t>(to_move.size()),
            static_cast<std::int64_t>(to_move.size()) * 4,
            static_cast<std::int64_t>(to_move.size()),
            static_cast<std::int64_t>(to_move.size() / 4))) {
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
    auto div_loops = loops;
    std::sort(div_loops.begin(), div_loops.end(), [](const Loop &lhs, const Loop &rhs) {
        return lhs.blocks.size() > rhs.blocks.size();
    });
    for (const auto &loop : div_loops) {
        changed |= reduce_loop_divisions(function, loop, dom, stats);
    }

    for (const auto &loop : loops) {
        changed |= hoist_from_loop(function, loop, dom, stats);
    }
    return changed;
}

} // namespace pass::mir_peephole
