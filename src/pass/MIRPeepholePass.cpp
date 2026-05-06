#include "../../include/pass/MIRPeepholePass.h"

#include "../../include/mir/MIR.h"
#include "../../include/mir/MIRVerifier.h"

#include <sstream>
#include <string>
#include <utility>

namespace pass {
namespace {

struct Stats {
    unsigned copies = 0;
    unsigned loads = 0;
    unsigned stores = 0;
    unsigned jumps = 0;
    unsigned arithmetic = 0;

    bool changed() const {
        return copies != 0 || loads != 0 || stores != 0 || jumps != 0 || arithmetic != 0;
    }

    std::string message() const {
        std::ostringstream oss;
        oss << "removed copy=" << copies << " load=" << loads << " store=" << stores
            << " jump=" << jumps << " arith=" << arithmetic;
        return oss.str();
    }
};

bool same_reg(const mir::MachineOperand &lhs, const mir::MachineOperand &rhs) {
    return lhs.is_reg() && rhs.is_reg() && lhs.reg_value() == rhs.reg_value();
}

bool is_zero_reg(const mir::MachineOperand &operand) {
    return operand.is_reg() && operand.reg_value().is_physical() &&
           operand.reg_value().name == "zero";
}

bool same_slot(const mir::MachineOperand &lhs, const mir::MachineOperand &rhs) {
    return lhs.kind() == mir::OperandKind::Slot && rhs.kind() == mir::OperandKind::Slot &&
           lhs.slot_id() == rhs.slot_id();
}

mir::MachineInstr make_move_like(const mir::MachineOperand &dst, const mir::MachineOperand &src) {
    auto opcode = dst.reg_value().reg_class == mir::RegisterClass::FPR32 ? mir::Opcode::FMove
                                                                         : mir::Opcode::Move;
    return mir::MachineInstr(opcode, {dst, src});
}

bool simplify_block(mir::MachineBasicBlock &block, const mir::MachineBasicBlock *next_block,
                    bool post_ra, Stats &stats) {
    bool changed = false;
    auto &instrs = block.instructions();

    for (std::size_t i = 0; i < instrs.size();) {
        auto &instr = instrs[i];
        const auto &ops = instr.operands();

        if ((instr.opcode() == mir::Opcode::Move || instr.opcode() == mir::Opcode::FMove) &&
            ops.size() >= 2 && same_reg(ops[0], ops[1])) {
            instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
            ++stats.copies;
            changed = true;
            continue;
        }

        if (post_ra && (instr.opcode() == mir::Opcode::Add || instr.opcode() == mir::Opcode::AddW) &&
            ops.size() >= 3 && is_zero_reg(ops[2])) {
            instr = make_move_like(ops[0], ops[1]);
            ++stats.arithmetic;
            changed = true;
        } else if (post_ra && instr.opcode() == mir::Opcode::SllI && ops.size() >= 3 &&
                   ops[2].kind() == mir::OperandKind::Imm && ops[2].int_value() == 0) {
            instr = make_move_like(ops[0], ops[1]);
            ++stats.arithmetic;
            changed = true;
        } else if (post_ra && instr.opcode() == mir::Opcode::MulW && ops.size() >= 3 &&
                   ops[2].kind() == mir::OperandKind::Imm && ops[2].int_value() == 1) {
            instr = make_move_like(ops[0], ops[1]);
            ++stats.arithmetic;
            changed = true;
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

    if (!instrs.empty() && next_block != nullptr && instrs.back().opcode() == mir::Opcode::Jump) {
        const auto &ops = instrs.back().operands();
        if (!ops.empty() && ops[0].kind() == mir::OperandKind::Block &&
            ops[0].string_value() == next_block->name()) {
            instrs.pop_back();
            ++stats.jumps;
            changed = true;
        }
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
        for (std::size_t i = 0; i < function->blocks().size(); ++i) {
            auto *next = i + 1 < function->blocks().size() ? function->blocks()[i + 1].get()
                                                           : nullptr;
            changed |= simplify_block(*function->blocks()[i], next, post_ra_, total);
        }
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
