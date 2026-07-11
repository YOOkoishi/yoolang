#include "pass/mir/MIRCostModel.h"
#include "pass/mir/MIRPeepholeCommon.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
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

struct PretestedLoopControl {
    mir::MachineBasicBlock *body = nullptr;
    mir::MachineBasicBlock *exit = nullptr;
    mir::Register current_iv;
    mir::MachineOperand bound;
};

struct DirectPointerStep {
    std::size_t step_index = 0;
    std::size_t copy_index = 0;
    mir::Register current;
    mir::Register next;
};

using PositiveFactSet = std::set<VRegId>;

struct PositiveBoundInfo {
    std::map<VRegId, std::int64_t> constants;
    std::map<VRegId, std::set<mir::MachineBasicBlock *>> positive_edge_targets;
    std::map<VRegId, std::map<mir::MachineBasicBlock *, std::int64_t>> lower_bound_edge_targets;
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

bool instr_uses_reg(const mir::MachineInstr &instr, const mir::Register &reg) {
    for (const auto &use : instr.uses()) {
        if (same_reg(use, reg)) {
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

struct ConditionalBranchEdges {
    const mir::MachineInstr *branch = nullptr;
    mir::MachineBasicBlock *true_target = nullptr;
    mir::MachineBasicBlock *false_target = nullptr;
};

std::optional<ConditionalBranchEdges> conditional_branch_edges(mir::MachineFunction &function,
                                                               mir::MachineBasicBlock &block) {
    const auto &instrs = block.instructions();
    if (instrs.empty()) {
        return std::nullopt;
    }

    ConditionalBranchEdges edges;
    const auto &last = instrs.back();
    if (is_conditional_branch(last.opcode())) {
        edges.branch = &last;
        edges.false_target = next_block(function, block);
    } else if (instrs.size() >= 2 && is_conditional_branch(instrs[instrs.size() - 2].opcode())) {
        edges.branch = &instrs[instrs.size() - 2];
        if (last.opcode() == mir::Opcode::Jump && !last.operands().empty() &&
            last.operands()[0].kind() == mir::OperandKind::Block) {
            edges.false_target = function.get_block(last.operands()[0].string_value());
        } else {
            edges.false_target = next_block(function, block);
        }
    } else {
        return std::nullopt;
    }
    edges.true_target = branch_target(function, *edges.branch);
    return edges;
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

std::optional<std::int64_t>
constant_operand_value(const mir::MachineOperand &operand,
                       const std::map<VRegId, std::int64_t> &constants) {
    if (!operand.is_reg()) {
        return std::nullopt;
    }
    const auto &reg = operand.reg_value();
    if (!reg.is_virtual()) {
        return is_zero_reg(reg) ? std::optional<std::int64_t>(0) : std::nullopt;
    }
    auto found = constants.find(reg.id);
    return found == constants.end() ? std::nullopt : std::optional<std::int64_t>(found->second);
}

std::optional<std::pair<VRegId, std::int64_t>>
make_lower_bound_fact(const mir::MachineOperand &operand, std::int64_t lower_bound) {
    if (!operand.is_reg() || !operand.reg_value().is_virtual() ||
        operand.reg_value().value_type != mir::ValueType::I32) {
        return std::nullopt;
    }
    return std::pair<VRegId, std::int64_t>{operand.reg_value().id, lower_bound};
}

std::optional<std::pair<VRegId, std::int64_t>>
branch_lower_bound_fact(const mir::MachineInstr &branch, bool true_edge,
                        const std::map<VRegId, std::int64_t> &constants) {
    const auto &ops = branch.operands();
    if (ops.size() < 2) {
        return std::nullopt;
    }

    const auto lhs_constant = constant_operand_value(ops[0], constants);
    const auto rhs_constant = constant_operand_value(ops[1], constants);
    if (branch.opcode() == mir::Opcode::BranchLT) {
        if (true_edge && lhs_constant && *lhs_constant < std::numeric_limits<std::int32_t>::max()) {
            return make_lower_bound_fact(ops[1], *lhs_constant + 1);
        }
        if (!true_edge && rhs_constant) {
            return make_lower_bound_fact(ops[0], *rhs_constant);
        }
    } else if (branch.opcode() == mir::Opcode::BranchGE) {
        if (true_edge && rhs_constant) {
            return make_lower_bound_fact(ops[0], *rhs_constant);
        }
        if (!true_edge && lhs_constant &&
            *lhs_constant < std::numeric_limits<std::int32_t>::max()) {
            return make_lower_bound_fact(ops[1], *lhs_constant + 1);
        }
    }
    return std::nullopt;
}

void add_edge_positive_facts(mir::MachineFunction &function, mir::MachineBasicBlock &block,
                             mir::MachineBasicBlock &succ, PositiveFactSet &facts,
                             const std::map<VRegId, mir::Register> &positive_slt_defs) {
    const auto edges = conditional_branch_edges(function, block);
    if (!edges) {
        return;
    }
    if (edges->true_target == &succ) {
        add_branch_positive_facts(facts, *edges->branch, true, positive_slt_defs);
    }
    if (edges->false_target == &succ) {
        add_branch_positive_facts(facts, *edges->branch, false, positive_slt_defs);
    }
}

PositiveBoundInfo compute_positive_bound_info(mir::MachineFunction &function,
                                              const std::map<VRegId, RegCounts> &counts) {
    PositiveBoundInfo info;
    info.constants = collect_i32_constants(function, counts);
    const auto positive_slt_defs = collect_positive_slt_defs(function, counts);
    auto *entry = function.blocks().empty() ? nullptr : function.blocks().front().get();

    for (auto &block_ptr : function.blocks()) {
        auto *block = block_ptr.get();
        for (auto *succ : block->successors()) {
            PositiveFactSet direct_facts;
            add_edge_positive_facts(function, *block, *succ, direct_facts, positive_slt_defs);
            for (auto fact : direct_facts) {
                info.positive_edge_targets[fact].insert(succ);
            }

            if (succ == entry || succ->predecessors().size() != 1) {
                continue;
            }
            const auto edges = conditional_branch_edges(function, *block);
            if (!edges) {
                continue;
            }
            const bool true_edge = edges->true_target == succ;
            const bool false_edge = edges->false_target == succ;
            if (true_edge == false_edge) {
                continue;
            }
            auto fact = branch_lower_bound_fact(*edges->branch, true_edge, info.constants);
            if (fact) {
                info.lower_bound_edge_targets[fact->first][succ] = fact->second;
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

std::optional<std::int64_t> bound_lower_bound_at(const PositiveBoundInfo &info,
                                                 const mir::MachineBasicBlock &block,
                                                 const mir::Register &bound,
                                                 mir::MachineFunction &function) {
    if (!bound.is_virtual()) {
        return std::nullopt;
    }
    auto constant = info.constants.find(bound.id);
    if (constant != info.constants.end()) {
        return constant->second;
    }
    auto facts = info.lower_bound_edge_targets.find(bound.id);
    if (facts == info.lower_bound_edge_targets.end()) {
        return std::nullopt;
    }

    std::optional<std::int64_t> lower_bound;
    for (const auto &[target, value] : facts->second) {
        if (dominates_by_reachability(function, *target, block)) {
            lower_bound = lower_bound ? std::max(*lower_bound, value) : value;
        }
    }
    return lower_bound;
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

        auto preheader_copy = find_copy_to(preheader, current_ptr);
        auto backedge_copy = find_copy_to(backedge, current_ptr);
        if (!preheader_copy || !backedge_copy || !preheader_copy->src.is_reg() ||
            !backedge_copy->src.is_reg()) {
            continue;
        }

        auto next_ptr = backedge_copy->src.reg_value();
        if (!same_reg(next_ptr, raw_next_ptr)) {
            auto intermediate_copy = find_copy_to(header, next_ptr);
            if (!intermediate_copy || !intermediate_copy->src.is_reg() ||
                !same_reg(intermediate_copy->src.reg_value(), raw_next_ptr)) {
                continue;
            }
        }

        return PointerMatch{
            preheader_copy->index, backedge_copy->index, preheader_copy->src, current_ptr, next_ptr,
            ops[2].int_value()};
    }
    return std::nullopt;
}

std::vector<DirectPointerStep>
collect_direct_pointer_steps(mir::MachineBasicBlock &block,
                             const std::map<VRegId, RegCounts> &counts) {
    std::vector<DirectPointerStep> out;
    const auto &instrs = block.instructions();
    for (std::size_t i = 0; i < instrs.size(); ++i) {
        const auto &instr = instrs[i];
        const auto &ops = instr.operands();
        if (instr.opcode() != mir::Opcode::AddI || ops.size() < 3 || !ops[0].is_reg() ||
            !ops[1].is_reg() || ops[2].kind() != mir::OperandKind::Imm) {
            continue;
        }

        const auto next = ops[0].reg_value();
        const auto current = ops[1].reg_value();
        if (next.value_type != mir::ValueType::Ptr || current.value_type != mir::ValueType::Ptr ||
            !next.is_virtual() || !current.is_virtual() || def_count(counts, next) != 1 ||
            use_count(counts, next) != 1 || def_count(counts, current) != 2) {
            continue;
        }

        auto copy = find_copy_to(block, current);
        if (!copy || copy->index <= i || !copy->src.is_reg() ||
            !same_reg(copy->src.reg_value(), next)) {
            continue;
        }

        bool changes_old_pointer_semantics = false;
        for (std::size_t scan = i + 1; scan < copy->index; ++scan) {
            if (instr_uses_reg(instrs[scan], current) ||
                instr_defines_reg(instrs[scan], current)) {
                changes_old_pointer_semantics = true;
                break;
            }
        }
        if (changes_old_pointer_semantics) {
            continue;
        }
        out.push_back(DirectPointerStep{i, copy->index, current, next});
    }
    return out;
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

std::optional<PretestedLoopControl> match_pretested_loop_control(mir::MachineFunction &function,
                                                                 mir::MachineBasicBlock &header) {
    const auto &instrs = header.instructions();
    if (instrs.empty() || instrs.size() > 2) {
        return std::nullopt;
    }

    const auto &branch = instrs.front();
    const auto &ops = branch.operands();
    if ((branch.opcode() != mir::Opcode::BranchLT && branch.opcode() != mir::Opcode::BranchGE) ||
        ops.size() < 3 || !ops[0].is_reg() || !ops[1].is_reg() ||
        ops[0].reg_value().value_type != mir::ValueType::I32 ||
        ops[1].reg_value().value_type != mir::ValueType::I32 ||
        ops[2].kind() != mir::OperandKind::Block) {
        return std::nullopt;
    }

    auto *true_target = function.get_block(ops[2].string_value());
    mir::MachineBasicBlock *false_target = nullptr;
    if (instrs.size() == 2) {
        const auto &jump = instrs.back();
        if (jump.opcode() != mir::Opcode::Jump || jump.operands().empty() ||
            jump.operands()[0].kind() != mir::OperandKind::Block) {
            return std::nullopt;
        }
        false_target = function.get_block(jump.operands()[0].string_value());
    } else {
        false_target = next_block(function, header);
    }

    auto *body = branch.opcode() == mir::Opcode::BranchLT ? true_target : false_target;
    auto *exit = branch.opcode() == mir::Opcode::BranchLT ? false_target : true_target;
    if (body == nullptr || exit == nullptr || body == &header || exit == &header || body == exit) {
        return std::nullopt;
    }

    return PretestedLoopControl{body, exit, ops[0].reg_value(), ops[1]};
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

bool is_factor_four_unroll_core(const std::vector<mir::MachineInstr> &core) {
    constexpr std::size_t kMaxUnrolledCoreInstructions = 12;
    if (core.empty() || core.size() > kMaxUnrolledCoreInstructions) {
        return false;
    }
    for (const auto &instr : core) {
        if (instr.opcode() == mir::Opcode::Call || instr.opcode() == mir::Opcode::MemZero ||
            instr.opcode() == mir::Opcode::Jump || is_conditional_branch(instr.opcode()) ||
            instr.opcode() == mir::Opcode::StoreOutgoingArg ||
            instr.opcode() == mir::Opcode::LoadIncomingArg) {
            return false;
        }
        for (const auto &reg : instr.defs()) {
            if (reg.is_physical()) {
                return false;
            }
        }
    }
    return true;
}

bool cost_model_allows_factor_four_unroll(const mir::MachineFunction &function,
                                          const mir::MachineBasicBlock &header,
                                          const std::vector<mir::MachineInstr> &core,
                                          std::optional<std::int64_t> trip_lower_bound,
                                          bool exact_trip_count, Stats &stats) {
    const bool has_useful_trip_bound =
        exact_trip_count || (trip_lower_bound && *trip_lower_bound > 1);
    constexpr std::int64_t kUnknownTripEstimate = 32;
    const auto estimated_trip_count =
        has_useful_trip_bound
            ? std::max<std::int64_t>(trip_lower_bound.value_or(1), 1)
            : kUnknownTripEstimate;
    const auto remainder = estimated_trip_count % 4;
    const auto unrolled_iterations = estimated_trip_count / 4;

    const auto before_control_instrs = estimated_trip_count + 1;
    const auto before_control_cycles = estimated_trip_count * 2 + 1;
    std::int64_t after_control_instrs = 2;
    std::int64_t after_control_cycles = 3;
    if (remainder != 0) {
        after_control_instrs += remainder * 2 + 2;
        after_control_cycles += remainder * 3 + 3;
        if (unrolled_iterations == 0) {
            ++after_control_instrs;
            ++after_control_cycles;
        }
    } else {
        ++after_control_instrs;
        ++after_control_cycles;
    }
    if (unrolled_iterations != 0) {
        after_control_instrs += unrolled_iterations + 1;
        after_control_cycles += unrolled_iterations * 2 + 1;
    }

    const auto core_instrs = estimated_trip_count * static_cast<std::int64_t>(core.size());
    const auto pre_ra_static_growth = static_cast<std::int64_t>(core.size()) * 4 + 8;
    constexpr std::int64_t kLiveRangeGrowth = 2;
    constexpr std::int64_t kRegisterPressureGrowth = 2;
    // Project the PreRA expansion through register allocation.  Each extra
    // callee-saved register needs a save/restore pair, while each extended live
    // range reserves one copy/spill instruction.  This keeps the code-growth
    // gate honest about the final code that exposed the original regression.
    constexpr std::int64_t kEstimatedPostRAGrowth =
        2 * kRegisterPressureGrowth + kLiveRangeGrowth;
    const auto estimated_static_growth = pre_ra_static_growth + kEstimatedPostRAGrowth;
    std::int64_t function_instrs = 0;
    for (const auto &block : function.blocks()) {
        function_instrs += static_cast<std::int64_t>(block->instructions().size());
    }

    pass::mir_cost_model::MIRTransformCostEstimate estimate;
    estimate.kind = pass::cost_model::TransformKind::LoopUnroll;
    estimate.stage = pass::cost_model::CostIRStage::PreRAMIR;
    estimate.pass_name = "MIRPointerLoopExitPass";
    estimate.candidate_id = "pointer-unroll4." + function.name() + "." + header.name();
    estimate.scope = function.name() + ":" + header.name();
    estimate.proof_summary = exact_trip_count
                                 ? "exact trip-count dynamic estimate and factor-four structural "
                                   "checks"
                             : has_useful_trip_bound
                                 ? "dominating minimum-trip dynamic estimate and factor-four "
                                   "structural checks"
                                 : "low-confidence 32-trip heuristic; runtime trip count unknown";
    estimate.confidence = exact_trip_count ? 0.85 : has_useful_trip_bound ? 0.72 : 0.50;
    estimate.before_instrs =
        stats.module_static_instrs > 0 ? stats.module_static_instrs : function_instrs;
    estimate.after_instrs = estimate.before_instrs + estimated_static_growth;
    estimate.before_dynamic_instrs = core_instrs + before_control_instrs;
    estimate.after_dynamic_instrs = core_instrs + after_control_instrs;
    estimate.before_cycles = core_instrs + before_control_cycles;
    estimate.after_cycles = core_instrs + after_control_cycles;
    estimate.before_branches = estimated_trip_count;
    estimate.after_branches = remainder + unrolled_iterations + (remainder != 0 ? 2 : 1);
    estimate.risk.code_growth = estimated_static_growth;
    estimate.risk.live_range_growth = kLiveRangeGrowth;
    estimate.risk.register_pressure_growth = kRegisterPressureGrowth;
    estimate.risk.cleanup_dependency = 1;
    return pass::mir_cost_model::allows_transform(stats.cost_model_report, stats.cost_model_policy,
                                                  stats.cost_model_filter, estimate);
}

bool unroll_rewritten_pointer_loop_by_four(
    mir::MachineFunction &function, mir::MachineBasicBlock &preheader,
    mir::MachineBasicBlock &header, mir::MachineBasicBlock &body, mir::MachineBasicBlock &exit,
    const mir::Register &current_iv, const mir::MachineOperand &bound, const mir::Register &pointer,
    const mir::Register &end_pointer, std::optional<std::int64_t> trip_lower_bound,
    bool exact_trip_count, Stats &stats) {
    const auto &original_body_instrs = body.instructions();
    std::optional<std::size_t> branch_index;
    if (!original_body_instrs.empty() &&
        original_body_instrs.back().opcode() == mir::Opcode::BranchNe) {
        branch_index = original_body_instrs.size() - 1;
    } else if (original_body_instrs.size() >= 2 &&
               original_body_instrs[original_body_instrs.size() - 2].opcode() ==
                   mir::Opcode::BranchNe &&
               original_body_instrs.back().opcode() == mir::Opcode::Jump) {
        branch_index = original_body_instrs.size() - 2;
    }
    if (!branch_index) {
        return false;
    }

    std::vector<mir::MachineInstr> core(original_body_instrs.begin(),
                                        original_body_instrs.begin() +
                                            static_cast<std::ptrdiff_t>(*branch_index));
    if (!is_factor_four_unroll_core(core)) {
        return false;
    }

    const std::string dispatch_name = header.name() + ".unroll4.dispatch";
    const std::string peel_name = header.name() + ".unroll4.peel";
    const std::string peel_done_name = header.name() + ".unroll4.peel.done";
    if (function.get_block(dispatch_name) != nullptr || function.get_block(peel_name) != nullptr ||
        function.get_block(peel_done_name) != nullptr) {
        return false;
    }
    if (!cost_model_allows_factor_four_unroll(function, header, core, trip_lower_bound,
                                               exact_trip_count, stats)) {
        return false;
    }

    auto remainder = function.regs().create_virtual(mir::RegisterClass::GPR, mir::ValueType::I32);
    auto &preheader_instrs = preheader.instructions();
    const auto remainder_index = terminator_index(preheader).value_or(preheader_instrs.size());
    preheader_instrs.insert(
        preheader_instrs.begin() + static_cast<std::ptrdiff_t>(remainder_index),
        mir::MachineInstr(mir::Opcode::AndI, {mir::MachineOperand::reg_def(remainder), bound,
                                              mir::MachineOperand::imm(3)}));

    auto *dispatch = function.create_block(dispatch_name);
    auto *peel = function.create_block(peel_name);
    auto *peel_done = function.create_block(peel_done_name);

    auto &header_instrs = header.instructions();
    header_instrs.clear();
    header.add_instr(mir::Opcode::BranchGE, {mir::MachineOperand::reg_use(current_iv), bound,
                                             mir::MachineOperand::block(exit.name())});
    header.add_instr(mir::Opcode::Jump, {mir::MachineOperand::block(dispatch->name())});

    dispatch->add_instr(mir::Opcode::BranchNonZero, {mir::MachineOperand::reg_use(remainder),
                                                     mir::MachineOperand::block(peel->name())});
    dispatch->add_instr(mir::Opcode::Jump, {mir::MachineOperand::block(body.name())});

    for (const auto &instr : core) {
        peel->add_instr(instr);
    }
    peel->add_instr(mir::Opcode::AddIW,
                    {mir::MachineOperand::reg_def(remainder),
                     mir::MachineOperand::reg_use(remainder), mir::MachineOperand::imm(-1)});
    peel->add_instr(mir::Opcode::BranchNonZero, {mir::MachineOperand::reg_use(remainder),
                                                 mir::MachineOperand::block(peel->name())});
    peel->add_instr(mir::Opcode::Jump, {mir::MachineOperand::block(peel_done->name())});

    peel_done->add_instr(mir::Opcode::BranchNe, {mir::MachineOperand::reg_use(pointer),
                                                 mir::MachineOperand::reg_use(end_pointer),
                                                 mir::MachineOperand::block(body.name())});
    peel_done->add_instr(mir::Opcode::Jump, {mir::MachineOperand::block(exit.name())});

    auto &body_instrs = body.instructions();
    body_instrs.clear();
    for (unsigned lane = 0; lane < 4; ++lane) {
        for (const auto &instr : core) {
            body.add_instr(instr);
        }
    }
    body.add_instr(mir::Opcode::BranchNe, {mir::MachineOperand::reg_use(pointer),
                                           mir::MachineOperand::reg_use(end_pointer),
                                           mir::MachineOperand::block(body.name())});
    body.add_instr(mir::Opcode::Jump, {mir::MachineOperand::block(exit.name())});

    ++stats.arithmetic;
    ++stats.branches;
    return true;
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

bool try_rewrite_pretested_loop(mir::MachineFunction &function, mir::MachineBasicBlock &header,
                                const std::map<VRegId, RegCounts> &counts,
                                const PositiveBoundInfo &positive_bounds, Stats &stats) {
    auto control = match_pretested_loop_control(function, header);
    if (!control || memory_access_count(*control->body) < 2 ||
        !flows_unconditionally_to(function, *control->body, header) ||
        control->body->predecessors().size() != 1 ||
        control->body->predecessors().front() != &header) {
        return false;
    }

    auto *preheader = unique_preheader(header, *control->body);
    if (preheader == nullptr || !flows_unconditionally_to(function, *preheader, header)) {
        return false;
    }

    const auto &bound = control->bound.reg_value();
    if (!bound.is_virtual() || def_count(counts, bound) != 1 || block_defines_reg(header, bound) ||
        block_defines_reg(*control->body, bound) || use_count(counts, control->current_iv) != 2) {
        return false;
    }

    auto preheader_iv = find_copy_to(*preheader, control->current_iv);
    auto backedge_iv = find_copy_to(*control->body, control->current_iv);
    if (!preheader_iv || !backedge_iv || !is_zero_reg(preheader_iv->src) ||
        !backedge_iv->src.is_reg()) {
        return false;
    }

    const auto next_iv = backedge_iv->src.reg_value();
    auto iv_step = match_unit_iv_step(*control->body, next_iv);
    if (!iv_step || !same_reg(iv_step->current, control->current_iv) ||
        def_count(counts, next_iv) != 1 || use_count(counts, next_iv) != 1) {
        return false;
    }

    auto pointer = find_pointer_progression(*preheader, *control->body, *control->body);
    if (!pointer || !pointer->start.is_reg() ||
        is_small_positive_constant(positive_bounds, bound) ||
        loop_carried_gpr_count(*control->body) > kMaxLoopCarriedGprs) {
        return false;
    }

    auto backedge_terminator = terminator_index(*control->body);
    if (!backedge_terminator || *backedge_terminator + 1 != control->body->instructions().size() ||
        !is_jump_to(control->body->instructions()[*backedge_terminator], header)) {
        return false;
    }
    for (std::size_t index = 0; index < *backedge_terminator; ++index) {
        const auto opcode = control->body->instructions()[index].opcode();
        if (opcode == mir::Opcode::Jump || is_conditional_branch(opcode)) {
            return false;
        }
    }

    auto end_ptr = function.regs().create_virtual(mir::RegisterClass::GPR, mir::ValueType::Ptr);
    auto setup =
        materialize_end_pointer(function, pointer->start, control->bound, pointer->stride, end_ptr);
    auto &preheader_instrs = preheader->instructions();
    const auto setup_index = terminator_index(*preheader).value_or(preheader_instrs.size());
    preheader_instrs.insert(preheader_instrs.begin() + static_cast<std::ptrdiff_t>(setup_index),
                            setup.begin(), setup.end());

    auto &body_instrs = control->body->instructions();
    auto direct_pointer_steps = collect_direct_pointer_steps(*control->body, counts);
    auto branch_ptr = pointer->next;
    std::vector<std::size_t> dead_body_indices{iv_step->index, backedge_iv->index};
    for (const auto &step : direct_pointer_steps) {
        auto operands = body_instrs[step.step_index].operands();
        operands[0] = mir::MachineOperand::reg_def(step.current);
        body_instrs[step.step_index] = mir::MachineInstr(mir::Opcode::AddI, std::move(operands));
        dead_body_indices.push_back(step.copy_index);
        if (same_reg(branch_ptr, step.next)) {
            branch_ptr = step.current;
        }
    }

    body_instrs[*backedge_terminator] = mir::MachineInstr(
        mir::Opcode::BranchNe,
        {mir::MachineOperand::reg_use(branch_ptr), mir::MachineOperand::reg_use(end_ptr),
         mir::MachineOperand::block(control->body->name())});
    body_instrs.insert(
        body_instrs.begin() + static_cast<std::ptrdiff_t>(*backedge_terminator + 1),
        mir::MachineInstr(mir::Opcode::Jump, {mir::MachineOperand::block(control->exit->name())}));

    erase_indices(body_instrs, std::move(dead_body_indices));

    const auto trip_lower_bound =
        bound_lower_bound_at(positive_bounds, *preheader, bound, function);
    const bool exact_trip_count =
        positive_bounds.constants.find(bound.id) != positive_bounds.constants.end();
    unroll_rewritten_pointer_loop_by_four(
        function, *preheader, header, *control->body, *control->exit, control->current_iv,
        control->bound, branch_ptr, end_ptr, trip_lower_bound, exact_trip_count, stats);

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
    const std::size_t initial_block_count = function.blocks().size();
    for (std::size_t index = 0; index < initial_block_count; ++index) {
        auto *block = function.blocks()[index].get();
        if (try_rewrite_loop(function, *block, counts, positive_bounds, stats) ||
            try_rewrite_pretested_loop(function, *block, counts, positive_bounds, stats)) {
            changed = true;
            function.rebuild_cfg();
            counts = count_vregs(function);
            positive_bounds = compute_positive_bound_info(function, counts);
        }
    }
    return changed;
}

} // namespace pass::mir_peephole
