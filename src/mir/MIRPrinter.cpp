#include "../../include/mir/MIRPrinter.h"

#include <iomanip>
#include <ostream>
#include <sstream>

namespace mir {
namespace {

const char *stack_slot_kind_name(StackSlotKind kind) {
    switch (kind) {
    case StackSlotKind::Value:
        return "value";
    case StackSlotKind::Alloca:
        return "alloca";
    case StackSlotKind::PhiTemp:
        return "phi-temp";
    case StackSlotKind::Spill:
        return "spill";
    case StackSlotKind::CalleeSaved:
        return "callee-saved";
    }
    return "unknown";
}

std::string reg_string(const Register &reg) {
    if (reg.is_virtual()) {
        return "%v" + std::to_string(reg.id) + ":" + register_class_name(reg.reg_class);
    }
    return reg.name;
}

void print_reg_list(std::ostream &out, const char *label, const std::vector<Register> &regs) {
    if (regs.empty()) {
        return;
    }
    out << "  " << label << ": ";
    for (std::size_t i = 0; i < regs.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << reg_string(regs[i]);
    }
    out << "\n";
}

} // namespace

MIRPrinter::MIRPrinter(std::ostream &out) : out_(out) {
}

void MIRPrinter::print(const Module &module) {
    out_ << "; target: " << module.target().arch << ", abi=" << module.target().abi
         << ", features=+m,+a,+f,+d,+c\n";
    out_ << "; module: " << module.name() << "\n\n";

    for (const auto &global : module.globals()) {
        print_global(global);
    }
    if (!module.globals().empty()) {
        out_ << "\n";
    }

    for (const auto &function : module.functions()) {
        print_function(*function);
        out_ << "\n";
    }
}

void MIRPrinter::print_global(const Global &global) {
    out_ << "global @" << global.name << " : " << global.type.ir << " size=" << global.type.size
         << " align=" << global.type.align;
    if (global.is_const) {
        out_ << " const";
    }
    if (!global.initializer.empty()) {
        out_ << " = " << global.initializer;
    }
    out_ << "\n";
}

void MIRPrinter::print_function(const MachineFunction &function) {
    if (function.is_external()) {
        out_ << "declare @" << function.name() << "\n";
        return;
    }

    out_ << "func @" << function.name() << "(";
    for (std::size_t i = 0; i < function.param_types().size(); ++i) {
        if (i != 0) {
            out_ << ", ";
        }
        out_ << value_type_name(function.param_types()[i].value_type);
    }
    out_ << ") -> " << value_type_name(function.return_type().value_type) << " {\n";
    out_ << "  frame align=16 size=" << function.frame_size()
         << " outgoing=" << function.outgoing_arg_bytes();
    if (function.has_call() && function.return_address_offset() >= 0) {
        out_ << " ra=sp+" << function.return_address_offset();
    }
    out_ << "\n";

    if (!function.stack_slots().empty()) {
        out_ << "  frame objects:\n";
        for (const auto &slot : function.stack_slots()) {
            out_ << "    fi#" << slot.id << " " << slot.name << " " << slot.type.ir
                 << " size=" << slot.type.size << " align=" << slot.type.align
                 << " offset=sp+" << slot.offset << " kind=" << stack_slot_kind_name(slot.kind)
                 << "\n";
        }
    }

    if (!function.regs().virtual_registers().empty()) {
        out_ << "  virtual registers:\n";
        for (const auto &reg : function.regs().virtual_registers()) {
            out_ << "    " << reg_string(reg) << " " << value_type_name(reg.value_type);
            if (auto phys = function.regs().allocation(reg)) {
                out_ << " -> " << phys->name;
            }
            out_ << "\n";
        }
    }

    if (!function.saved_registers().empty()) {
        out_ << "  saved registers:\n";
        for (const auto &saved : function.saved_registers()) {
            out_ << "    " << saved.reg.name << " offset=sp+" << saved.offset << "\n";
        }
    }

    for (const auto &block : function.blocks()) {
        print_block(*block);
    }
    out_ << "}\n";
}

void MIRPrinter::print_block(const MachineBasicBlock &block) {
    out_ << "\n" << block.name() << ":\n";
    print_reg_list(out_, "live-in", block.live_in());
    print_reg_list(out_, "live-out", block.live_out());
    for (const auto &instr : block.instructions()) {
        out_ << "  " << opcode_name(instr.opcode());
        const auto &operands = instr.operands();
        if (!operands.empty()) {
            out_ << " ";
        }
        for (std::size_t i = 0; i < operands.size(); ++i) {
            if (i != 0) {
                out_ << ", ";
            }
            auto text = operand_string(nullptr, operands[i]);
            if (operands[i].is_implicit()) {
                out_ << "(implicit";
                if (operands[i].is_def()) {
                    out_ << "-def";
                } else if (operands[i].is_use()) {
                    out_ << "-use";
                }
                out_ << " " << text << ")";
            } else {
                out_ << text;
            }
        }
        out_ << "\n";
    }
}

std::string MIRPrinter::operand_string(const MachineFunction *, const MachineOperand &operand) {
    std::ostringstream oss;
    switch (operand.kind()) {
    case OperandKind::Reg:
    case OperandKind::FReg:
        if (operand.is_reg()) {
            oss << reg_string(operand.reg_value());
        } else {
            oss << operand.string_value();
        }
        break;
    case OperandKind::Imm:
        oss << operand.int_value();
        break;
    case OperandKind::FloatImm:
        oss << std::fixed << std::setprecision(6) << operand.float_value();
        break;
    case OperandKind::Slot:
        oss << "fi#" << operand.slot_id();
        break;
    case OperandKind::Global:
        oss << "@" << operand.string_value();
        break;
    case OperandKind::Block:
        oss << "%" << operand.string_value();
        break;
    case OperandKind::Symbol:
        oss << operand.string_value();
        break;
    case OperandKind::Type:
        oss << value_type_name(operand.type_value());
        break;
    case OperandKind::Text:
        oss << operand.string_value();
        break;
    }
    return oss.str();
}

} // namespace mir
