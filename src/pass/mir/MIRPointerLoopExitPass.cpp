#include "pass/mir/MIRPeepholeCommon.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace pass::mir_peephole {
namespace {

struct CopyMatch {
    std::size_t index = 0;
    mir::MachineOperand src;
};

struct StepMatch {
    std::size_t index = 0;
    mir::Register current;
};

struct PointerMatch {
    std::size_t preheader_copy_index = 0;
    std::size_t backedge_copy_index = 0;
    mir::MachineOperand start;
    mir::Register current;
    mir::Register next;
    std::int64_t stride = 0;
};

using PositiveFactSet = std::set<VRegId>;

struct PositiveBoundInfo {
    std::map<VRegId, std::int64_t> constants;
    std::map<VRegId, std::set<mir::MachineBasicBlock *>> positive_edge_targets;
};

constexpr std::int64_t kSmallConstantTripCount = 8;
constexpr unsigned kMaxLoopCarriedGprs = 6;

bool is_jump_to(const mir::MachineInstr &instr, const mir::MachineBasicBlock &target) {
    const auto &ops = instr.operands();
    return instr.opcode() == mir::Opcode::Jump && !ops.empty() &&
           ops[0].kind() == mir::OperandKind::Block && ops[0].string_value() == target.name();
}

mir::MachineBasicBlock *next_block(mir::MachineFunction &function,
                                   mir::MachineBasicBlock &block) {
    auto &blocks = function.blocks();
    for (std::size_t i = 0; i + 1 < blocks.size(); ++i) {
        if (blocks[i].get() == &block) {
            return blocks[i + 1].get();
        }
    }
    return nullptr;
}

bool flows_unconditionally_to(mir::MachineFunction &function, mir::MachineBasicBlock &block,
                              mir::MachineBasicBlock &target) {
    const auto &instrs = block.instructions();
    if (!instrs.empty()) {
        const auto &last = instrs.back();
        if (is_jump_to(last, target)) {
            return true;
        }
        if (last.opcode() == mir::Opcode::Jump || is_conditional_branch(last.opcode())) {
            return false;
        }
    }
    return next_block(function, block) == &target;
}

bool is_power_of_two_stride(std::int64_t value) {
    return value > 0 && (value & (value - 1)) == 0 && value <= 8;
}

unsigned log2_stride(std::int64_t value) {
    unsigned shift = 0;
    while (value > 1) {
        value >>= 1;
        ++shift;
    }
    return shift;
}

bool is_move_to_reg(const mir::MachineInstr &instr, const mir::Register &dst) {
    const auto &ops = instr.operands();
    return instr.opcode() == mir::Opcode::Move && ops.size() >= 2 && ops[0].is_reg() &&
           ops[0].is_def() && same_reg(ops[0].reg_value(), dst);
}

bool instr_defines_reg(const mir::MachineInstr &instr, const mir::Register &reg) {
    for (const auto &def : instr.defs()) {
        if (same_reg(def, reg)) {
            return true;
        }
    }
    return false;
}

bool block_defines_reg(const mir::MachineBasicBlock &block, const mir::Register &reg) {
    for (const auto &instr : block.instructions()) {
        if (instr_defines_reg(instr, reg)) {
            return true;
        }
    }
    return false;
}

bool is_positive_constant(const PositiveBoundInfo &info, const mir::Register &reg) {
    if (!reg.is_virtual()) {
        return false;
    }
    auto found = info.constants.find(reg.id);
    return found != info.constants.end() && found->second > 0;
}

bool is_small_positive_constant(const PositiveBoundInfo &info, const mir::Register &reg) {
    if (!reg.is_virtual()) {
        return false;
    }
    auto found = info.constants.find(reg.id);
    return found != info.constants.end() && found->second > 0 &&
           found->second <= kSmallConstantTripCount;
}

std::optional<mir::Register> positive_slt_bound(const mir::MachineInstr &instr) {
    const auto &ops = instr.operands();
    if (instr.opcode() != mir::Opcode::Slt || ops.size() < 3 || !ops[0].is_reg() ||
        !ops[0].is_def() || !is_zero_reg(ops[1]) || !ops[2].is_reg() ||
        ops[2].reg_value().value_type != mir::ValueType::I32) {
        return std::nullopt;
    }
    return ops[2].reg_value();
}

std::map<VRegId, mir::Register>
collect_positive_slt_defs(const mir::MachineFunction &function,
                          const std::map<VRegId, RegCounts> &counts) {
    std::map<VRegId, mir::Register> out;
    for (const auto &block : function.blocks()) {
        for (const auto &instr : block->instructions()) {
            auto bound = positive_slt_bound(instr);
            if (!bound) {
                continue;
            }
            const auto &def = instr.operands()[0].reg_value();
            if (def.is_virtual() && def_count(counts, def) == 1 && bound->is_virtual()) {
                out[def.id] = *bound;
            }
        }
    }
    return out;
}

std::map<VRegId, std::int64_t>
collect_i32_constants(const mir::MachineFunction &function,
                      const std::map<VRegId, RegCounts> &counts) {
    std::map<VRegId, std::int64_t> out;
    for (const auto &block : function.blocks()) {
        for (const auto &instr : block->instructions()) {
            const auto &ops = instr.operands();
            if (instr.opcode() != mir::Opcode::LoadImm || ops.size() < 2 || !ops[0].is_reg() ||
                !ops[0].is_def() || ops[0].reg_value().value_type != mir::ValueType::I32 ||
                ops[1].kind() != mir::OperandKind::Imm) {
                continue;
            }
            const auto &reg = ops[0].reg_value();
            if (reg.is_virtual() && def_count(counts, reg) == 1) {
                out[reg.id] = ops[1].int_value();
            }
        }
    }
    return out;
}

mir::MachineBasicBlock *branch_target(mir::MachineFunction &function,
                                      const mir::MachineInstr &branch) {
    if (!is_conditional_branch(branch.opcode())) {
        return nullptr;
    }
    const auto &ops = branch.operands();
    const auto target = branch_target_index(branch.opcode());
    if (ops.size() <= target || ops[target].kind() != mir::OperandKind::Block) {
        return nullptr;
    }
    return function.get_block(ops[target].string_value());
}

void add_positive_fact(PositiveFactSet &facts, const mir::Register &reg) {
    if (reg.is_virtual() && reg.value_type == mir::ValueType::I32) {
        facts.insert(reg.id);
    }
}

void add_branch_positive_facts(PositiveFactSet &facts, const mir::MachineInstr &branch,
                               bool true_edge,
                               const std::map<VRegId, mir::Register> &positive_slt_defs) {
    const auto &ops = branch.operands();
    switch (branch.opcode()) {
    case mir::Opcode::BranchLT:
        if (true_edge && ops.size() >= 2 && is_zero_reg(ops[0]) && ops[1].is_reg()) {
            add_positive_fact(facts, ops[1].reg_value());
        }
        break;
    case mir::Opcode::BranchGE:
        if (!true_edge && ops.size() >= 2 && is_zero_reg(ops[0]) && ops[1].is_reg()) {
            add_positive_fact(facts, ops[1].reg_value());
        }
        break;
    case mir::Opcode::BranchNonZero:
    case mir::Opcode::BranchZero:
        if (ops.empty() || !ops[0].is_reg() || !ops[0].reg_value().is_virtual()) {
            break;
        }
        if ((branch.opcode() == mir::Opcode::BranchNonZero && true_edge) ||
            (branch.opcode() == mir::Opcode::BranchZero && !true_edge)) {
            auto found = positive_slt_defs.find(ops[0].reg_value().id);
            if (found != positive_slt_defs.end()) {
                add_positive_fact(facts, found->second);
            }
        }
        break;
    case mir::Opcode::BranchNe:
    case mir::Opcode::BranchEq: {
        if (ops.size() < 2) {
            break;
        }
        const mir::MachineOperand *cond = nullptr;
        if (is_zero_reg(ops[0]) && ops[1].is_reg()) {
            cond = &ops[1];
        } else if (ops[0].is_reg() && is_zero_reg(ops[1])) {
            cond = &ops[0];
        }
        if (cond == nullptr || !cond->reg_value().is_virtual()) {
            break;
        }
        if ((branch.opcode() == mir::Opcode::BranchNe && true_edge) ||
            (branch.opcode() == mir::Opcode::BranchEq && !true_edge)) {
            auto found = positive_slt_defs.find(cond->reg_value().id);
            if (found != positive_slt_defs.end()) {
                add_positive_fact(facts, found->second);
            }
        }
        break;
    }
    default:
        break;
    }
}

void add_edge_positive_facts(mir::MachineFunction &function, mir::MachineBasicBlock &block,
                             mir::MachineBasicBlock &succ, PositiveFactSet &facts,
                             const std::map<VRegId, mir::Register> &positive_slt_defs) {
    const auto &instrs = block.instructions();
    if (instrs.empty()) {
        return;
    }

    const mir::MachineInstr *branch = nullptr;
    mir::MachineBasicBlock *true_target = nullptr;
    mir::MachineBasicBlock *false_target = nullptr;
    const auto &last = instrs.back();

    if (is_conditional_branch(last.opcode())) {
        branch = &last;
        true_target = branch_target(function, last);
        false_target = next_block(function, block);
    } else if (last.opcode() == mir::Opcode::Jump && instrs.size() >= 2 &&
               is_conditional_branch(instrs[instrs.size() - 2].opcode())) {
        branch = &instrs[instrs.size() - 2];
        true_target = branch_target(function, *branch);
        if (!last.operands().empty() && last.operands()[0].kind() == mir::OperandKind::Block) {
            false_target = function.get_block(last.operands()[0].string_value());
        }
    } else if (instrs.size() >= 2 && is_conditional_branch(instrs[instrs.size() - 2].opcode())) {
        branch = &instrs[instrs.size() - 2];
        true_target = branch_target(function, *branch);
        false_target = next_block(function, block);
    }

    if (branch == nullptr) {
        return;
    }
    if (true_target == &succ) {
        add_branch_positive_facts(facts, *branch, true, positive_slt_defs);
    }
    if (false_target == &succ) {
        add_branch_positive_facts(facts, *branch, false, positive_slt_defs);
    }
}

PositiveBoundInfo compute_positive_bound_info(
    mir::MachineFunction &function, const std::map<VRegId, RegCounts> &counts) {
    PositiveBoundInfo info;
    info.constants = collect_i32_constants(function, counts);
    const auto positive_slt_defs = collect_positive_slt_defs(function, counts);

    for (auto &block_ptr : function.blocks()) {
        auto *block = block_ptr.get();
        for (auto *succ : block->successors()) {
            PositiveFactSet direct_facts;
            add_edge_positive_facts(function, *block, *succ, direct_facts, positive_slt_defs);
            for (auto fact : direct_facts) {
                info.positive_edge_targets[fact].insert(succ);
            }
        }
    }

    return info;
}

bool dominates_by_reachability(mir::MachineFunction &function,
                               const mir::MachineBasicBlock &dominator,
                               const mir::MachineBasicBlock &block) {
    if (&dominator == &block) {
        return true;
    }
    if (function.blocks().empty()) {
        return false;
    }

    auto *entry = function.blocks().front().get();
    if (entry == &dominator) {
        return true;
    }

    std::set<mir::MachineBasicBlock *> seen;
    std::vector<mir::MachineBasicBlock *> stack;
    seen.insert(const_cast<mir::MachineBasicBlock *>(&dominator));
    stack.push_back(entry);
    seen.insert(entry);

    while (!stack.empty()) {
        auto *current = stack.back();
        stack.pop_back();
        if (current == &block) {
            return false;
        }
        for (auto *succ : current->successors()) {
            if (seen.insert(succ).second) {
                stack.push_back(succ);
            }
        }
    }
    return true;
}

bool bound_known_positive_at(const PositiveBoundInfo &info, const mir::MachineBasicBlock &block,
                             const mir::Register &bound, mir::MachineFunction &function) {
    if (!bound.is_virtual()) {
        return false;
    }
    if (is_positive_constant(info, bound)) {
        return true;
    }
    auto targets = info.positive_edge_targets.find(bound.id);
    if (targets == info.positive_edge_targets.end()) {
        return false;
    }
    for (auto *target : targets->second) {
        if (dominates_by_reachability(function, *target, block)) {
            return true;
        }
    }
    return false;
}

unsigned loop_carried_gpr_count(const mir::MachineBasicBlock &backedge) {
    unsigned out = 0;
    for (const auto &instr : backedge.instructions()) {
        const auto &ops = instr.operands();
        if (instr.opcode() == mir::Opcode::Move && ops.size() >= 2 && ops[0].is_reg() &&
            ops[0].is_def() && ops[0].reg_value().reg_class == mir::RegisterClass::GPR) {
            ++out;
        }
    }
    return out;
}

std::optional<CopyMatch> find_copy_to(mir::MachineBasicBlock &block, const mir::Register &dst) {
    auto &instrs = block.instructions();
    for (std::size_t i = 0; i < instrs.size(); ++i) {
        if (!is_move_to_reg(instrs[i], dst)) {
            continue;
        }
        const auto &ops = instrs[i].operands();
        if (!ops[1].is_reg()) {
            continue;
        }
        return CopyMatch{i, ops[1]};
    }
    return std::nullopt;
}

std::optional<CopyMatch> find_copy_from(const mir::MachineBasicBlock &block,
                                        const mir::Register &src) {
    const auto &instrs = block.instructions();
    for (std::size_t i = 0; i < instrs.size(); ++i) {
        const auto &instr = instrs[i];
        const auto &ops = instr.operands();
        if (instr.opcode() != mir::Opcode::Move || ops.size() < 2 || !ops[0].is_reg() ||
            !ops[1].is_reg() || !same_reg(ops[1].reg_value(), src)) {
            continue;
        }
        return CopyMatch{i, ops[0]};
    }
    return std::nullopt;
}

std::optional<StepMatch> match_unit_iv_step(const mir::MachineBasicBlock &header,
                                            const mir::Register &next_iv) {
    const auto &instrs = header.instructions();
    for (std::size_t i = 0; i < instrs.size(); ++i) {
        const auto &instr = instrs[i];
        const auto &ops = instr.operands();
        if (instr.opcode() != mir::Opcode::AddIW || ops.size() < 3 || !ops[0].is_reg() ||
            !ops[1].is_reg() || ops[2].kind() != mir::OperandKind::Imm ||
            ops[2].int_value() != 1 || !same_reg(ops[0].reg_value(), next_iv)) {
            continue;
        }
        return StepMatch{i, ops[1].reg_value()};
    }
    return std::nullopt;
}

std::optional<PointerMatch> find_pointer_progression(
    mir::MachineBasicBlock &preheader, mir::MachineBasicBlock &header,
    mir::MachineBasicBlock &backedge) {
    const auto &header_instrs = header.instructions();
    for (const auto &instr : header_instrs) {
        const auto &ops = instr.operands();
        if (instr.opcode() != mir::Opcode::AddI || ops.size() < 3 || !ops[0].is_reg() ||
            !ops[1].is_reg() || ops[2].kind() != mir::OperandKind::Imm ||
            !is_power_of_two_stride(ops[2].int_value())) {
            continue;
        }

        auto raw_next_ptr = ops[0].reg_value();
        auto current_ptr = ops[1].reg_value();
        if (raw_next_ptr.value_type != mir::ValueType::Ptr ||
            current_ptr.value_type != mir::ValueType::Ptr) {
            continue;
        }

        auto next_ptr = raw_next_ptr;
        if (auto next_copy = find_copy_from(header, raw_next_ptr)) {
            if (!next_copy->src.is_reg() ||
                next_copy->src.reg_value().value_type != mir::ValueType::Ptr) {
                continue;
            }
            next_ptr = next_copy->src.reg_value();
        }

        auto preheader_copy = find_copy_to(preheader, current_ptr);
        auto backedge_copy = find_copy_to(backedge, current_ptr);
        if (!preheader_copy || !backedge_copy || !preheader_copy->src.is_reg() ||
            !backedge_copy->src.is_reg() ||
            !same_reg(backedge_copy->src.reg_value(), next_ptr)) {
            continue;
        }

        return PointerMatch{preheader_copy->index, backedge_copy->index, preheader_copy->src,
                            current_ptr, next_ptr, ops[2].int_value()};
    }
    return std::nullopt;
}

unsigned memory_access_count(const mir::MachineBasicBlock &block) {
    unsigned out = 0;
    for (const auto &instr : block.instructions()) {
        switch (instr.opcode()) {
        case mir::Opcode::LoadMem:
        case mir::Opcode::StoreMem:
        case mir::Opcode::LoadMemOffset:
        case mir::Opcode::StoreMemOffset:
            ++out;
            break;
        default:
            break;
        }
    }
    return out;
}

std::optional<std::size_t> terminator_index(const mir::MachineBasicBlock &block) {
    const auto &instrs = block.instructions();
    for (std::size_t i = instrs.size(); i > 0; --i) {
        const auto opcode = instrs[i - 1].opcode();
        if (opcode == mir::Opcode::Jump || is_conditional_branch(opcode)) {
            return i - 1;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> continue_branch_index(const mir::MachineBasicBlock &block) {
    const auto &instrs = block.instructions();
    if (instrs.empty()) {
        return std::nullopt;
    }
    if (instrs.back().opcode() == mir::Opcode::BranchLT) {
        return instrs.size() - 1;
    }
    if (instrs.size() >= 2 && instrs[instrs.size() - 2].opcode() == mir::Opcode::BranchLT &&
        instrs.back().opcode() == mir::Opcode::Jump) {
        return instrs.size() - 2;
    }
    return std::nullopt;
}

mir::MachineBasicBlock *unique_preheader(mir::MachineBasicBlock &header,
                                         mir::MachineBasicBlock &backedge) {
    mir::MachineBasicBlock *preheader = nullptr;
    for (auto *pred : header.predecessors()) {
        if (pred == &backedge) {
            continue;
        }
        if (preheader != nullptr) {
            return nullptr;
        }
        preheader = pred;
    }
    return preheader;
}

std::vector<mir::MachineInstr> materialize_end_pointer(mir::MachineFunction &function,
                                                       const mir::MachineOperand &start_ptr,
                                                       const mir::MachineOperand &bound,
                                                       std::int64_t stride,
                                                       mir::Register end_ptr) {
    auto &regs = function.regs();
    auto offset = stride == 1 ? bound.reg_value()
                              : regs.create_virtual(mir::RegisterClass::GPR, mir::ValueType::I32);

    std::vector<mir::MachineInstr> out;
    if (stride != 1) {
        out.emplace_back(mir::Opcode::SllIW,
                         std::vector<mir::MachineOperand>{
                             mir::MachineOperand::reg_def(offset), bound,
                             mir::MachineOperand::imm(static_cast<std::int64_t>(log2_stride(stride)))});
    }
    out.emplace_back(mir::Opcode::Add,
                     std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(end_ptr),
                                                      start_ptr, mir::MachineOperand::reg_use(offset)});
    return out;
}

void erase_indices(std::vector<mir::MachineInstr> &instrs, std::vector<std::size_t> indices) {
    std::sort(indices.begin(), indices.end(), std::greater<std::size_t>());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    for (auto index : indices) {
        if (index < instrs.size()) {
            instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
}

bool try_rewrite_loop(mir::MachineFunction &function, mir::MachineBasicBlock &header,
                      const std::map<VRegId, RegCounts> &counts,
                      const PositiveBoundInfo &positive_bounds, Stats &stats) {
    if (memory_access_count(header) < 2) {
        return false;
    }

    auto &header_instrs = header.instructions();
    if (header_instrs.empty()) {
        return false;
    }

    auto maybe_branch_index = continue_branch_index(header);
    if (!maybe_branch_index) {
        return false;
    }

    const std::size_t branch_index = *maybe_branch_index;
    auto &branch = header_instrs[branch_index];
    const auto &branch_ops = branch.operands();
    if (branch.opcode() != mir::Opcode::BranchLT || branch_ops.size() < 3 ||
        !branch_ops[0].is_reg() || !branch_ops[1].is_reg() ||
        branch_ops[2].kind() != mir::OperandKind::Block) {
        return false;
    }

    auto *backedge = function.get_block(branch_ops[2].string_value());
    if (backedge == nullptr || backedge == &header || backedge->instructions().empty() ||
        !flows_unconditionally_to(function, *backedge, header)) {
        return false;
    }

    auto *preheader = unique_preheader(header, *backedge);
    if (preheader == nullptr || preheader->instructions().empty() ||
        !flows_unconditionally_to(function, *preheader, header)) {
        return false;
    }

    const auto next_iv = branch_ops[0].reg_value();
    const auto bound = branch_ops[1];
    if (def_count(counts, bound.reg_value()) != 1) {
        return false;
    }
    if (block_defines_reg(header, bound.reg_value()) ||
        block_defines_reg(*backedge, bound.reg_value())) {
        return false;
    }
    if (use_count(counts, next_iv) != 2) {
        return false;
    }

    auto iv_step = match_unit_iv_step(header, next_iv);
    if (!iv_step || use_count(counts, iv_step->current) != 1) {
        return false;
    }

    auto preheader_iv = find_copy_to(*preheader, iv_step->current);
    auto backedge_iv = find_copy_to(*backedge, iv_step->current);
    if (!preheader_iv || !backedge_iv || !is_zero_reg(preheader_iv->src) ||
        !backedge_iv->src.is_reg() || !same_reg(backedge_iv->src.reg_value(), next_iv)) {
        return false;
    }

    auto pointer = find_pointer_progression(*preheader, header, *backedge);
    if (!pointer || !pointer->start.is_reg()) {
        return false;
    }
    if (is_small_positive_constant(positive_bounds, bound.reg_value())) {
        return false;
    }
    if (!bound_known_positive_at(positive_bounds, *preheader, bound.reg_value(), function)) {
        return false;
    }
    if (loop_carried_gpr_count(*backedge) > kMaxLoopCarriedGprs) {
        return false;
    }

    auto end_ptr = function.regs().create_virtual(mir::RegisterClass::GPR, mir::ValueType::Ptr);
    auto setup = materialize_end_pointer(function, pointer->start, bound, pointer->stride, end_ptr);
    auto &preheader_instrs = preheader->instructions();
    auto insert_at = terminator_index(*preheader);
    if (!insert_at) {
        return false;
    }
    preheader_instrs.insert(preheader_instrs.begin() + static_cast<std::ptrdiff_t>(*insert_at),
                            setup.begin(), setup.end());

    branch = mir::MachineInstr(
        mir::Opcode::BranchNe,
        {mir::MachineOperand::reg_use(pointer->next), mir::MachineOperand::reg_use(end_ptr),
         branch_ops[2]});

    erase_indices(header.instructions(), {iv_step->index});
    erase_indices(preheader->instructions(), {preheader_iv->index});
    erase_indices(backedge->instructions(), {backedge_iv->index});

    ++stats.branches;
    ++stats.arithmetic;
    ++stats.dead;
    return true;
}

} // namespace

bool optimize_pointer_loop_exits(mir::MachineFunction &function, bool post_ra, Stats &stats) {
    if (post_ra) {
        return false;
    }

    function.rebuild_cfg();
    auto counts = count_vregs(function);
    auto positive_bounds = compute_positive_bound_info(function, counts);
    bool changed = false;
    for (auto &block : function.blocks()) {
        if (try_rewrite_loop(function, *block, counts, positive_bounds, stats)) {
            changed = true;
            function.rebuild_cfg();
            counts = count_vregs(function);
            positive_bounds = compute_positive_bound_info(function, counts);
        }
    }
    return changed;
}

} // namespace pass::mir_peephole
