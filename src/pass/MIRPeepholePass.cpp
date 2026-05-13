#include "../../include/pass/MIRPeepholePass.h"

#include "../../include/mir/MIR.h"
#include "../../include/mir/MIRVerifier.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pass {
namespace {

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
    unsigned dead = 0;

    bool changed() const {
        return copies != 0 || loads != 0 || stores != 0 || jumps != 0 ||
               arithmetic != 0 || branches != 0 || address_folds != 0 || cse != 0 ||
               dead != 0;
    }

    std::string message() const {
        std::ostringstream oss;
        oss << "removed copy=" << copies << " load=" << loads << " store=" << stores
            << " jump=" << jumps << " arith=" << arithmetic << " branch=" << branches
            << " addr-fold=" << address_folds << " cse=" << cse << " dead=" << dead;
        return oss.str();
    }
};

struct RegCounts {
    unsigned defs = 0;
    unsigned uses = 0;
};

bool same_reg(const mir::MachineOperand &lhs, const mir::MachineOperand &rhs) {
    return lhs.is_reg() && rhs.is_reg() && lhs.reg_value() == rhs.reg_value();
}

bool same_reg(const mir::Register &lhs, const mir::Register &rhs) {
    return lhs == rhs;
}

bool is_zero_reg(const mir::MachineOperand &operand) {
    return operand.is_reg() && operand.reg_value().is_physical() &&
           operand.reg_value().name == "zero";
}

bool is_zero_reg(const mir::Register &reg) {
    return reg.is_physical() && reg.name == "zero";
}

bool same_slot(const mir::MachineOperand &lhs, const mir::MachineOperand &rhs) {
    return lhs.kind() == mir::OperandKind::Slot && rhs.kind() == mir::OperandKind::Slot &&
           lhs.slot_id() == rhs.slot_id();
}

bool is_move(mir::Opcode opcode) {
    return opcode == mir::Opcode::Move || opcode == mir::Opcode::FMove;
}

bool is_conditional_branch(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::BranchNonZero:
    case mir::Opcode::BranchZero:
    case mir::Opcode::BranchEq:
    case mir::Opcode::BranchNe:
    case mir::Opcode::BranchLT:
    case mir::Opcode::BranchGE:
        return true;
    default:
        return false;
    }
}

std::size_t branch_target_index(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::BranchNonZero:
    case mir::Opcode::BranchZero:
        return 1;
    case mir::Opcode::BranchEq:
    case mir::Opcode::BranchNe:
    case mir::Opcode::BranchLT:
    case mir::Opcode::BranchGE:
        return 2;
    default:
        return 0;
    }
}

std::optional<mir::Opcode> inverted_branch(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::BranchNonZero:
        return mir::Opcode::BranchZero;
    case mir::Opcode::BranchZero:
        return mir::Opcode::BranchNonZero;
    case mir::Opcode::BranchEq:
        return mir::Opcode::BranchNe;
    case mir::Opcode::BranchNe:
        return mir::Opcode::BranchEq;
    case mir::Opcode::BranchLT:
        return mir::Opcode::BranchGE;
    case mir::Opcode::BranchGE:
        return mir::Opcode::BranchLT;
    default:
        return std::nullopt;
    }
}

bool fits_simm12(std::int64_t value) {
    return value >= -2048 && value <= 2047;
}

mir::MachineInstr make_move_like(const mir::MachineOperand &dst, const mir::MachineOperand &src) {
    auto opcode = dst.reg_value().reg_class == mir::RegisterClass::FPR32 ? mir::Opcode::FMove
                                                                         : mir::Opcode::Move;
    return mir::MachineInstr(opcode, {dst, src});
}

mir::MachineInstr make_move_like(mir::Register dst, mir::Register src) {
    auto opcode = dst.reg_class == mir::RegisterClass::FPR32 ? mir::Opcode::FMove
                                                             : mir::Opcode::Move;
    return mir::MachineInstr(
        opcode, {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(src)});
}

std::map<VRegId, RegCounts> count_vregs(const mir::MachineFunction &function) {
    std::map<VRegId, RegCounts> counts;
    for (const auto &block : function.blocks()) {
        for (const auto &instr : block->instructions()) {
            for (const auto &operand : instr.operands()) {
                if (!operand.is_reg() || !operand.reg_value().is_virtual()) {
                    continue;
                }
                auto &entry = counts[operand.reg_value().id];
                if (operand.is_def()) {
                    ++entry.defs;
                }
                if (operand.is_use()) {
                    ++entry.uses;
                }
            }
        }
    }
    return counts;
}

unsigned use_count(const std::map<VRegId, RegCounts> &counts, const mir::Register &reg) {
    if (!reg.is_virtual()) {
        return 0;
    }
    auto found = counts.find(reg.id);
    return found == counts.end() ? 0 : found->second.uses;
}

unsigned def_count(const std::map<VRegId, RegCounts> &counts, const mir::Register &reg) {
    if (!reg.is_virtual()) {
        return 0;
    }
    auto found = counts.find(reg.id);
    return found == counts.end() ? 0 : found->second.defs;
}

bool defines_reg(const mir::MachineInstr &instr, const mir::Register &reg) {
    for (const auto &operand : instr.operands()) {
        if (operand.is_reg() && operand.is_def() && same_reg(operand.reg_value(), reg)) {
            return true;
        }
    }
    return false;
}

bool uses_reg(const mir::MachineInstr &instr, const mir::Register &reg) {
    for (const auto &operand : instr.operands()) {
        if (operand.is_reg() && operand.is_use() && same_reg(operand.reg_value(), reg)) {
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

bool coalesce_copies_once(mir::MachineFunction &function, Stats &stats) {
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

            unsigned replaced = 0;
            bool blocked = false;
            for (std::size_t j = i + 1; j < instrs.size(); ++j) {
                if (defines_reg(instrs[j], src) || defines_reg(instrs[j], dst)) {
                    blocked = true;
                    break;
                }
                replaced += replace_reg_uses(instrs[j], dst, src);
                if (replaced == total_uses) {
                    break;
                }
            }

            if (!blocked && replaced == total_uses) {
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

bool coalesce_copies(mir::MachineFunction &function, Stats &stats) {
    bool changed = false;
    for (int iteration = 0; iteration < 8; ++iteration) {
        if (!coalesce_copies_once(function, stats)) {
            break;
        }
        changed = true;
    }
    return changed;
}

bool is_pure_def(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::LoadImm:
    case mir::Opcode::LoadGlobalAddr:
    case mir::Opcode::LoadStackAddr:
    case mir::Opcode::Move:
    case mir::Opcode::FMove:
    case mir::Opcode::Add:
    case mir::Opcode::AddW:
    case mir::Opcode::AddI:
    case mir::Opcode::AddIW:
    case mir::Opcode::SubW:
    case mir::Opcode::Mul:
    case mir::Opcode::MulW:
    case mir::Opcode::DivW:
    case mir::Opcode::RemW:
    case mir::Opcode::And:
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
    case mir::Opcode::FeqS:
    case mir::Opcode::FltS:
    case mir::Opcode::FleS:
    case mir::Opcode::FcvtSW:
    case mir::Opcode::FcvtWS:
    case mir::Opcode::FmvWX:
        return true;
    default:
        return false;
    }
}

bool remove_dead_defs_once(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;
    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size();) {
            const auto defs = instrs[i].defs();
            if (defs.size() == 1 && defs[0].is_virtual() && is_pure_def(instrs[i].opcode()) &&
                use_count(counts, defs[0]) == 0) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                ++stats.dead;
                changed = true;
                continue;
            }
            ++i;
        }
    }
    return changed;
}

bool remove_dead_defs(mir::MachineFunction &function, Stats &stats) {
    bool changed = false;
    for (int iteration = 0; iteration < 8; ++iteration) {
        if (!remove_dead_defs_once(function, stats)) {
            break;
        }
        changed = true;
    }
    return changed;
}

std::string reg_key(const mir::Register &reg) {
    if (reg.is_virtual()) {
        return "v" + std::to_string(reg.id);
    }
    return "p" + reg.name;
}

bool stable_operand_for_cse(const mir::MachineOperand &operand) {
    if (!operand.is_reg()) {
        return true;
    }
    return operand.reg_value().is_virtual() || is_zero_reg(operand.reg_value());
}

std::string operand_key(const mir::MachineOperand &operand) {
    switch (operand.kind()) {
    case mir::OperandKind::Reg:
    case mir::OperandKind::FReg:
        return reg_key(operand.reg_value());
    case mir::OperandKind::Imm:
        return "i" + std::to_string(operand.int_value());
    case mir::OperandKind::FloatImm:
        return "f" + std::to_string(operand.float_value());
    case mir::OperandKind::Slot:
        return "s" + std::to_string(operand.slot_id());
    case mir::OperandKind::Global:
        return "g" + operand.string_value();
    case mir::OperandKind::Block:
        return "b" + operand.string_value();
    case mir::OperandKind::Symbol:
        return "y" + operand.string_value();
    case mir::OperandKind::Type:
        return "t" + std::to_string(static_cast<int>(operand.type_value()));
    case mir::OperandKind::Text:
        return "x" + operand.string_value();
    }
    return "";
}

bool commutative_opcode(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::Add:
    case mir::Opcode::AddW:
    case mir::Opcode::Mul:
    case mir::Opcode::MulW:
    case mir::Opcode::And:
    case mir::Opcode::Xor:
        return true;
    default:
        return false;
    }
}

std::optional<std::string> cse_key(const mir::MachineInstr &instr) {
    if (!is_pure_def(instr.opcode()) || is_move(instr.opcode())) {
        return std::nullopt;
    }
    const auto defs = instr.defs();
    if (defs.size() != 1 || !defs[0].is_virtual()) {
        return std::nullopt;
    }

    std::vector<std::string> pieces;
    const auto &ops = instr.operands();
    for (std::size_t i = 1; i < ops.size(); ++i) {
        if (!stable_operand_for_cse(ops[i])) {
            return std::nullopt;
        }
        pieces.push_back(operand_key(ops[i]));
    }
    if (commutative_opcode(instr.opcode()) && pieces.size() == 2) {
        std::sort(pieces.begin(), pieces.end());
    }

    std::ostringstream oss;
    oss << static_cast<int>(instr.opcode());
    for (const auto &piece : pieces) {
        oss << "|" << piece;
    }
    return oss.str();
}

bool local_cse(mir::MachineFunction &function, Stats &stats) {
    bool changed = false;
    for (auto &block_ptr : function.blocks()) {
        std::map<std::string, mir::Register> available;
        for (auto &instr : block_ptr->instructions()) {
            if (instr.opcode() == mir::Opcode::Call || instr.opcode() == mir::Opcode::StoreMem ||
                instr.opcode() == mir::Opcode::StoreMemOffset ||
                instr.opcode() == mir::Opcode::StoreSlot || instr.opcode() == mir::Opcode::MemZero) {
                available.clear();
            }

            auto key = cse_key(instr);
            const auto defs = instr.defs();
            if (key && defs.size() == 1) {
                auto found = available.find(*key);
                if (found != available.end()) {
                    instr = make_move_like(defs[0], found->second);
                    ++stats.cse;
                    changed = true;
                    continue;
                }
                available[*key] = defs[0];
            }

            for (const auto &def : defs) {
                if (def.is_physical() && !is_zero_reg(def)) {
                    available.clear();
                    break;
                }
            }
        }
    }
    return changed;
}

bool fold_address_offsets(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;
    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 1; i < instrs.size();) {
            auto &instr = instrs[i];
            if (instr.opcode() != mir::Opcode::LoadMem &&
                instr.opcode() != mir::Opcode::StoreMem) {
                ++i;
                continue;
            }
            auto &addr = instr.operands()[instr.opcode() == mir::Opcode::LoadMem ? 1 : 0];
            if (!addr.is_reg() || !addr.reg_value().is_virtual() ||
                use_count(counts, addr.reg_value()) != 1) {
                ++i;
                continue;
            }

            auto &producer = instrs[i - 1];
            const auto &prod_ops = producer.operands();
            if (producer.opcode() != mir::Opcode::AddI || prod_ops.size() < 3 ||
                !same_reg(prod_ops[0].reg_value(), addr.reg_value()) ||
                prod_ops[2].kind() != mir::OperandKind::Imm ||
                !fits_simm12(prod_ops[2].int_value())) {
                ++i;
                continue;
            }

            if (instr.opcode() == mir::Opcode::LoadMem) {
                instr = mir::MachineInstr(
                    mir::Opcode::LoadMemOffset,
                    {instr.operands()[0], prod_ops[1], prod_ops[2], instr.operands()[2]});
                ++stats.loads;
            } else {
                instr = mir::MachineInstr(
                    mir::Opcode::StoreMemOffset,
                    {prod_ops[1], instr.operands()[1], prod_ops[2], instr.operands()[2]});
                ++stats.stores;
            }
            instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i - 1));
            --i;
            ++stats.address_folds;
            changed = true;
        }
    }
    return changed;
}

bool replace_branch_with(mir::MachineBasicBlock &block, std::size_t index, mir::Opcode opcode,
                         std::vector<mir::MachineOperand> operands) {
    block.instructions()[index] = mir::MachineInstr(opcode, std::move(operands));
    return true;
}

std::optional<std::string> jump_only_target(const mir::MachineBasicBlock &block) {
    if (block.instructions().size() != 1) {
        return std::nullopt;
    }
    const auto &jump = block.instructions().front();
    if (jump.opcode() != mir::Opcode::Jump || jump.operands().empty() ||
        jump.operands()[0].kind() != mir::OperandKind::Block ||
        jump.operands()[0].string_value() == block.name() ||
        jump.operands()[0].string_value() == "epilogue") {
        return std::nullopt;
    }
    return jump.operands()[0].string_value();
}

std::string resolve_jump_only_target(const mir::MachineFunction &function, std::string target) {
    for (std::size_t depth = 0; depth < function.blocks().size(); ++depth) {
        auto *block = function.get_block(target);
        if (block == nullptr) {
            break;
        }
        auto next = jump_only_target(*block);
        if (!next || *next == target) {
            break;
        }
        target = *next;
    }
    return target;
}

bool redirect_jump_only_blocks(mir::MachineFunction &function, Stats &stats) {
    bool changed = false;
    for (auto &block_ptr : function.blocks()) {
        for (auto &instr : block_ptr->instructions()) {
            std::optional<std::size_t> target_index;
            if (instr.opcode() == mir::Opcode::Jump) {
                target_index = 0;
            } else if (is_conditional_branch(instr.opcode())) {
                target_index = branch_target_index(instr.opcode());
            }

            if (!target_index || instr.operands().size() <= *target_index ||
                instr.operands()[*target_index].kind() != mir::OperandKind::Block) {
                continue;
            }

            const auto old_target = instr.operands()[*target_index].string_value();
            auto new_target = resolve_jump_only_target(function, old_target);
            if (new_target == old_target) {
                continue;
            }

            instr.operands()[*target_index] = mir::MachineOperand::block(std::move(new_target));
            ++stats.jumps;
            changed = true;
        }
    }
    return changed;
}

bool fuse_compare_branches(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;

    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            if (instrs[i].opcode() != mir::Opcode::BranchNonZero ||
                instrs[i].operands().size() < 2 || i == 0) {
                continue;
            }

            const auto cond = instrs[i].operands()[0].reg_value();
            if (!cond.is_virtual() || use_count(counts, cond) != 1) {
                continue;
            }

            auto &prev = instrs[i - 1];
            const auto &prev_ops = prev.operands();
            if (prev.defs().size() == 1 && same_reg(prev.defs()[0], cond)) {
                if (prev.opcode() == mir::Opcode::Slt && prev_ops.size() >= 3) {
                    replace_branch_with(*block_ptr, i, mir::Opcode::BranchLT,
                                        {prev_ops[1], prev_ops[2], instrs[i].operands()[1]});
                    instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i - 1));
                    --i;
                    ++stats.branches;
                    changed = true;
                    continue;
                }

                if ((prev.opcode() == mir::Opcode::SeqZ ||
                     prev.opcode() == mir::Opcode::Snez) &&
                    prev_ops.size() >= 2 && i >= 2) {
                    const auto tmp = prev_ops[1].reg_value();
                    auto &cmp = instrs[i - 2];
                    const auto &cmp_ops = cmp.operands();
                    if (tmp.is_virtual() && use_count(counts, tmp) == 1 &&
                        cmp.opcode() == mir::Opcode::Xor && cmp_ops.size() >= 3 &&
                        cmp.defs().size() == 1 && same_reg(cmp.defs()[0], tmp)) {
                        auto branch_opcode = prev.opcode() == mir::Opcode::SeqZ
                                                 ? mir::Opcode::BranchEq
                                                 : mir::Opcode::BranchNe;
                        replace_branch_with(*block_ptr, i, branch_opcode,
                                            {cmp_ops[1], cmp_ops[2], instrs[i].operands()[1]});
                        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i - 1));
                        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i - 2));
                        i -= 2;
                        ++stats.branches;
                        changed = true;
                        continue;
                    }
                }

                if (prev.opcode() == mir::Opcode::XorI && prev_ops.size() >= 3 &&
                    prev_ops[2].kind() == mir::OperandKind::Imm && prev_ops[2].int_value() == 1 &&
                    i >= 2) {
                    const auto tmp = prev_ops[1].reg_value();
                    auto &cmp = instrs[i - 2];
                    const auto &cmp_ops = cmp.operands();
                    if (tmp.is_virtual() && use_count(counts, tmp) == 1 &&
                        cmp.opcode() == mir::Opcode::Slt && cmp_ops.size() >= 3 &&
                        cmp.defs().size() == 1 && same_reg(cmp.defs()[0], tmp)) {
                        replace_branch_with(*block_ptr, i, mir::Opcode::BranchGE,
                                            {cmp_ops[1], cmp_ops[2], instrs[i].operands()[1]});
                        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i - 1));
                        instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i - 2));
                        i -= 2;
                        ++stats.branches;
                        changed = true;
                        continue;
                    }
                }
            }
        }
    }

    return changed;
}

bool simplify_block(mir::MachineBasicBlock &block, const mir::MachineBasicBlock *next_block,
                    bool post_ra, Stats &stats) {
    bool changed = false;
    auto &instrs = block.instructions();

    for (std::size_t i = 0; i < instrs.size();) {
        auto &instr = instrs[i];
        const auto &ops = instr.operands();

        if (is_move(instr.opcode()) && ops.size() >= 2 && same_reg(ops[0], ops[1])) {
            instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
            ++stats.copies;
            changed = true;
            continue;
        }

        if ((instr.opcode() == mir::Opcode::Add || instr.opcode() == mir::Opcode::AddW) &&
            ops.size() >= 3 && (is_zero_reg(ops[1]) || is_zero_reg(ops[2]))) {
            instr = make_move_like(ops[0], is_zero_reg(ops[1]) ? ops[2] : ops[1]);
            ++stats.arithmetic;
            changed = true;
        } else if ((instr.opcode() == mir::Opcode::AddI ||
                    instr.opcode() == mir::Opcode::AddIW) &&
                   ops.size() >= 3 && ops[2].kind() == mir::OperandKind::Imm &&
                   ops[2].int_value() == 0) {
            instr = make_move_like(ops[0], ops[1]);
            ++stats.arithmetic;
            changed = true;
        } else if ((instr.opcode() == mir::Opcode::SllI ||
                    instr.opcode() == mir::Opcode::SllIW ||
                    instr.opcode() == mir::Opcode::SraI ||
                    instr.opcode() == mir::Opcode::SraIW ||
                    instr.opcode() == mir::Opcode::SrliW) &&
                   ops.size() >= 3 && ops[2].kind() == mir::OperandKind::Imm &&
                   ops[2].int_value() == 0) {
            instr = make_move_like(ops[0], ops[1]);
            ++stats.arithmetic;
            changed = true;
        }

        if (instr.opcode() == mir::Opcode::BranchNonZero && ops.size() >= 2 &&
            is_zero_reg(ops[0])) {
            instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
            ++stats.branches;
            changed = true;
            continue;
        }
        if (instr.opcode() == mir::Opcode::BranchZero && ops.size() >= 2 && is_zero_reg(ops[0])) {
            instr = mir::MachineInstr(mir::Opcode::Jump, {ops[1]});
            ++stats.branches;
            changed = true;
        } else if ((instr.opcode() == mir::Opcode::BranchEq ||
                    instr.opcode() == mir::Opcode::BranchGE) &&
                   ops.size() >= 3 && same_reg(ops[0], ops[1])) {
            instr = mir::MachineInstr(mir::Opcode::Jump, {ops[2]});
            ++stats.branches;
            changed = true;
        } else if ((instr.opcode() == mir::Opcode::BranchNe ||
                    instr.opcode() == mir::Opcode::BranchLT) &&
                   ops.size() >= 3 && same_reg(ops[0], ops[1])) {
            instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
            ++stats.branches;
            changed = true;
            continue;
        }

        if (i + 1 < instrs.size()) {
            auto &next = instrs[i + 1];
            const auto &next_ops = next.operands();

            if (instr.opcode() == mir::Opcode::StoreSlot &&
                next.opcode() == mir::Opcode::LoadSlot && ops.size() >= 3 &&
                next_ops.size() >= 3 && same_slot(ops[0], next_ops[1]) &&
                ops[2].type_value() == next_ops[2].type_value()) {
                next = make_move_like(next_ops[0], ops[1]);
                ++stats.loads;
                changed = true;
            } else if (instr.opcode() == mir::Opcode::LoadSlot &&
                       next.opcode() == mir::Opcode::StoreSlot && ops.size() >= 3 &&
                       next_ops.size() >= 3 && same_slot(ops[1], next_ops[0]) &&
                       same_reg(ops[0], next_ops[1])) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i + 1));
                ++stats.stores;
                changed = true;
                continue;
            } else if (instr.opcode() == mir::Opcode::StoreSlot &&
                       next.opcode() == mir::Opcode::StoreSlot && ops.size() >= 1 &&
                       next_ops.size() >= 1 && same_slot(ops[0], next_ops[0])) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                ++stats.stores;
                changed = true;
                continue;
            } else if ((instr.opcode() == mir::Opcode::LoadImm &&
                        next.opcode() == mir::Opcode::LoadImm) ||
                       (instr.opcode() == mir::Opcode::LoadFloatImm &&
                        next.opcode() == mir::Opcode::LoadFloatImm)) {
                if (ops.size() >= 1 && next_ops.size() >= 1 && same_reg(ops[0], next_ops[0])) {
                    instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                    ++stats.copies;
                    changed = true;
                    continue;
                }
            }
        }

        ++i;
    }

    if (instrs.size() >= 2 && next_block != nullptr && instrs.back().opcode() == mir::Opcode::Jump) {
        auto &branch = instrs[instrs.size() - 2];
        const auto &jump_ops = instrs.back().operands();
        if (is_conditional_branch(branch.opcode()) && !jump_ops.empty() &&
            jump_ops[0].kind() == mir::OperandKind::Block) {
            const auto target_index = branch_target_index(branch.opcode());
            if (branch.operands().size() > target_index &&
                branch.operands()[target_index].kind() == mir::OperandKind::Block &&
                branch.operands()[target_index].string_value() == next_block->name()) {
                if (auto inverted = inverted_branch(branch.opcode())) {
                    auto operands = branch.operands();
                    operands[target_index] = jump_ops[0];
                    branch = mir::MachineInstr(*inverted, std::move(operands));
                    instrs.pop_back();
                    ++stats.branches;
                    ++stats.jumps;
                    changed = true;
                }
            }
        }
    }

    if (!instrs.empty() && next_block != nullptr && instrs.back().opcode() == mir::Opcode::Jump) {
        const auto &ops = instrs.back().operands();
        if (!ops.empty() && ops[0].kind() == mir::OperandKind::Block &&
            ops[0].string_value() == next_block->name()) {
            instrs.pop_back();
            ++stats.jumps;
            changed = true;
        }
    }

    (void)post_ra;
    return changed;
}

bool optimize_function(mir::MachineFunction &function, bool post_ra, Stats &stats) {
    bool changed = false;

    if (!post_ra) {
        for (int iteration = 0; iteration < 6; ++iteration) {
            bool iteration_changed = false;
            iteration_changed |= coalesce_copies(function, stats);
            iteration_changed |= fuse_compare_branches(function, stats);
            iteration_changed |= local_cse(function, stats);
            iteration_changed |= fold_address_offsets(function, stats);
            iteration_changed |= redirect_jump_only_blocks(function, stats);
            iteration_changed |= remove_dead_defs(function, stats);
            if (!iteration_changed) {
                break;
            }
            changed = true;
        }
    }

    changed |= redirect_jump_only_blocks(function, stats);
    for (std::size_t i = 0; i < function.blocks().size(); ++i) {
        auto *next = i + 1 < function.blocks().size() ? function.blocks()[i + 1].get() : nullptr;
        changed |= simplify_block(*function.blocks()[i], next, post_ra, stats);
    }
    return changed;
}

} // namespace

MIRPeepholePass::MIRPeepholePass(bool post_ra) : post_ra_(post_ra) {
}

std::string_view MIRPeepholePass::name() const {
    return post_ra_ ? "MIRPostRAPeepholePass" : "MIRPreRAPeepholePass";
}

PassKind MIRPeepholePass::kind() const {
    return PassKind::Transform;
}

PassResult MIRPeepholePass::run(PassContext &context) {
    auto *module = context.machine_module();
    if (module == nullptr) {
        return PassResult::fail("MIRPeepholePass requires MIR module in pass context");
    }

    Stats total;
    bool changed = false;
    for (auto &function : module->functions()) {
        if (function->is_external()) {
            continue;
        }
        function->rebuild_cfg();
        changed |= optimize_function(*function, post_ra_, total);
        function->rebuild_cfg();
    }

    auto verify = mir::verify_module(
        *module, post_ra_ ? mir::MIRVerificationStage::PostRA : mir::MIRVerificationStage::PreRA);
    if (!verify.ok) {
        return PassResult::fail(verify.message);
    }

    context.set_artifact(std::string(name()), total.message());
    return PassResult::ok(changed, total.message());
}

} // namespace pass
