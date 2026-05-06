#include "../../include/mir/MIRVerifier.h"

#include <sstream>
#include <stdexcept>

namespace mir {
namespace {

class Verifier {
  public:
    MIRVerifyResult verify(const Module &module, MIRVerificationStage stage) {
        stage_ = stage;
        for (const auto &function : module.functions()) {
            if (!function->is_external()) {
                verify_function(*function);
            }
        }
        return {true, ""};
    }

  private:
    [[noreturn]] void fail(const std::string &message) const {
        throw std::runtime_error(message);
    }

    void verify_function(const MachineFunction &function) const {
        for (const auto &block : function.blocks()) {
            verify_block(function, *block);
        }
    }

    void verify_block(const MachineFunction &function, const MachineBasicBlock &block) const {
        for (const auto &instr : block.instructions()) {
            verify_instr(function, block, instr);
        }
    }

    void verify_instr(const MachineFunction &function, const MachineBasicBlock &block,
                      const MachineInstr &instr) const {
        const auto &ops = instr.operands();
        for (const auto &operand : ops) {
            if (operand.is_reg()) {
                verify_reg(function, block, instr, operand.reg_value());
            }
            if (operand.kind() == OperandKind::Slot &&
                function.stack_slot(operand.slot_id()) == nullptr) {
                fail(where(function, block, instr) + " references missing fi#" +
                     std::to_string(operand.slot_id()));
            }
            if (operand.kind() == OperandKind::Block &&
                function.get_block(operand.string_value()) == nullptr &&
                operand.string_value() != "epilogue") {
                fail(where(function, block, instr) + " references missing block %" +
                     operand.string_value());
            }
        }

        switch (instr.opcode()) {
        case Opcode::LoadSlot:
            require(ops.size() >= 3 && ops[0].is_reg() && ops[1].kind() == OperandKind::Slot,
                    function, block, instr, "malformed LOAD_SLOT");
            break;
        case Opcode::StoreSlot:
            require(ops.size() >= 3 && ops[0].kind() == OperandKind::Slot && ops[1].is_reg(),
                    function, block, instr, "malformed STORE_SLOT");
            break;
        case Opcode::LoadMem:
        case Opcode::StoreMem:
            require(ops.size() >= 3 && ops[0].is_reg() && ops[1].is_reg(), function, block,
                    instr, "malformed memory instruction");
            break;
        case Opcode::BranchNonZero:
            require(ops.size() >= 2 && ops[0].is_reg() && ops[1].kind() == OperandKind::Block,
                    function, block, instr, "malformed BNEZ");
            break;
        case Opcode::Jump:
            require(!ops.empty(), function, block, instr, "malformed J");
            break;
        default:
            break;
        }
    }

    void verify_reg(const MachineFunction &function, const MachineBasicBlock &block,
                    const MachineInstr &instr, const Register &reg) const {
        if (reg.is_virtual()) {
            if (stage_ == MIRVerificationStage::PostRA) {
                fail(where(function, block, instr) + " has virtual register after RA");
            }
            if (function.regs().virtual_register(reg.id) == nullptr) {
                fail(where(function, block, instr) + " references unknown virtual register");
            }
            return;
        }
        if (reg.name.empty()) {
            fail(where(function, block, instr) + " has unnamed physical register");
        }
        if (register_class_for_physical(reg.name) != reg.reg_class) {
            fail(where(function, block, instr) + " has mismatched physical register class");
        }
    }

    void require(bool condition, const MachineFunction &function, const MachineBasicBlock &block,
                 const MachineInstr &instr, const std::string &message) const {
        if (!condition) {
            fail(where(function, block, instr) + ": " + message);
        }
    }

    std::string where(const MachineFunction &function, const MachineBasicBlock &block,
                      const MachineInstr &instr) const {
        std::ostringstream oss;
        oss << "MIR verifier @" << function.name() << " %" << block.name() << " "
            << opcode_name(instr.opcode());
        return oss.str();
    }

    MIRVerificationStage stage_ = MIRVerificationStage::PreRA;
};

} // namespace

MIRVerifyResult verify_module(const Module &module, MIRVerificationStage stage) {
    try {
        Verifier verifier;
        return verifier.verify(module, stage);
    } catch (const std::exception &ex) {
        return {false, ex.what()};
    }
}

} // namespace mir
