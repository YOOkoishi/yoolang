#include "pass/mir/MIRLocalCSEPass.h"

#include "pass/mir/MIRPeepholeCommon.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace pass::mir_peephole {
namespace {

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

} // namespace

bool local_cse(mir::MachineFunction &function, bool post_ra, Stats &stats) {
    (void)post_ra;

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

} // namespace pass::mir_peephole

namespace pass {

std::string_view MIRLocalCSEPass::name() const {
    return "MIRLocalCSEPass";
}

PassKind MIRLocalCSEPass::kind() const {
    return PassKind::Transform;
}

PassResult MIRLocalCSEPass::run(PassContext &context) {
    return mir_peephole::run_transform(context, name(), false, mir_peephole::local_cse);
}

} // namespace pass
