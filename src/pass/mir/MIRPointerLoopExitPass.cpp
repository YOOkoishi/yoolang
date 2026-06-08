#include "pass/mir/MIRPeepholeCommon.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    auto one = regs.create_virtual(mir::RegisterClass::GPR, mir::ValueType::I32);
    auto less_than_one = regs.create_virtual(mir::RegisterClass::GPR, mir::ValueType::I1);
    auto delta = regs.create_virtual(mir::RegisterClass::GPR, mir::ValueType::I32);
    auto mask = regs.create_virtual(mir::RegisterClass::GPR, mir::ValueType::I32);
    auto extra = regs.create_virtual(mir::RegisterClass::GPR, mir::ValueType::I32);
    auto count = regs.create_virtual(mir::RegisterClass::GPR, mir::ValueType::I32);
    auto offset = stride == 1 ? count
                              : regs.create_virtual(mir::RegisterClass::GPR, mir::ValueType::I32);

    std::vector<mir::MachineInstr> out;
    out.emplace_back(mir::Opcode::LoadImm,
                     std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(one),
                                                      mir::MachineOperand::imm(1)});
    out.emplace_back(mir::Opcode::Slt,
                     std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(less_than_one),
                                                      bound, mir::MachineOperand::reg_use(one)});
    out.emplace_back(mir::Opcode::SubW,
                     std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(delta),
                                                      mir::MachineOperand::reg_use(one), bound});
    out.emplace_back(mir::Opcode::SubW,
                     std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(mask),
                                                      mir::MachineOperand::reg("zero"),
                                                      mir::MachineOperand::reg_use(less_than_one)});
    out.emplace_back(mir::Opcode::And,
                     std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(extra),
                                                      mir::MachineOperand::reg_use(delta),
                                                      mir::MachineOperand::reg_use(mask)});
    out.emplace_back(mir::Opcode::AddW,
                     std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(count), bound,
                                                      mir::MachineOperand::reg_use(extra)});
    if (stride != 1) {
        out.emplace_back(mir::Opcode::SllIW,
                         std::vector<mir::MachineOperand>{
                             mir::MachineOperand::reg_def(offset), mir::MachineOperand::reg_use(count),
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
                      const std::map<VRegId, RegCounts> &counts, Stats &stats) {
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
    bool changed = false;
    for (auto &block : function.blocks()) {
        if (try_rewrite_loop(function, *block, counts, stats)) {
            changed = true;
            function.rebuild_cfg();
            counts = count_vregs(function);
        }
    }
    return changed;
}

} // namespace pass::mir_peephole
