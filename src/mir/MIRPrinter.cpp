#include "mir/MIRPrinter.h"

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
    std::string out;
    if (reg.is_virtual()) {
        out = "%v" + std::to_string(reg.id) + ":" + register_class_name(reg.reg_class);
    } else {
        out = reg.name;
    }
    if (reg.vector_type.has_value()) {
        out += "[" + machine_vector_type_name(*reg.vector_type) + ";g" +
               std::to_string(reg.vector_group_width) + "]";
    }
    return out;
}

std::string avl_string(const MachineVectorAVL &avl) {
    switch (avl.kind) {
    case VectorAVLKind::Unspecified:
        return "unspecified";
    case VectorAVLKind::CurrentVL:
        return "current-vl";
    case VectorAVLKind::Operand:
        return "#" + std::to_string(avl.operand_index);
    case VectorAVLKind::WholeRegister:
        return "whole-register";
    }
    return "unknown";
}

std::string optional_operand_index(const std::optional<std::size_t> &index) {
    return index.has_value() ? "#" + std::to_string(*index) : "none";
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
    const auto &target = module.target();
    out_ << "; target: " << target.arch << ", triple=" << target.triple << ", abi=" << target.abi
         << ", cpu=" << target.cpu << ", tune=" << target.tune
         << ", min-vlen=" << target.minimum_vlen_bits
         << ", abi-vlen=" << target.abi_vlen_bits
         << ", vector=" << (target.has_vector ? "yes" : "no")
         << ", vector-abi=" << (target.psabi_vector ? "psabi-vector" : "standard") << "\n";
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
    if (global.initializer_bytes.empty()) {
        out_ << " = zeroinit";
    } else {
        out_ << " = bytes[";
        for (std::size_t index = 0; index < global.initializer_bytes.size(); ++index) {
            if (index != 0) {
                out_ << ' ';
            }
            out_ << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<unsigned>(global.initializer_bytes[index]);
        }
        out_ << std::dec << std::setfill(' ') << ']';
    }
    out_ << "\n";
}

void MIRPrinter::print_function(const MachineFunction &function) {
    if (function.is_external()) {
        out_ << "declare @" << function.name();
        if (function.is_variant_cc()) {
            out_ << " variant_cc";
        }
        out_ << "\n";
        return;
    }

    out_ << "func @" << function.name();
    if (function.is_variant_cc()) {
        out_ << " variant_cc";
    }
    out_ << "(";
    for (std::size_t i = 0; i < function.param_types().size(); ++i) {
        if (i != 0) {
            out_ << ", ";
        }
        out_ << value_type_name(function.param_types()[i].value_type);
    }
    out_ << ") -> " << value_type_name(function.return_type().value_type) << " {\n";
    out_ << "  frame align=16 size=" << function.frame_size()
         << " outgoing=" << function.outgoing_arg_bytes();
    if (function.scalable_frame_size().vlenb_eighths != 0) {
        out_ << " scalable=" << machine_scalable_size_name(function.scalable_frame_size());
    }
    if (function.has_call() && function.return_address_offset() >= 0) {
        out_ << " ra=sp+" << function.return_address_offset();
    }
    out_ << "\n";

    if (!function.stack_slots().empty()) {
        out_ << "  frame objects:\n";
        for (const auto &slot : function.stack_slots()) {
            out_ << "    fi#" << slot.id << " " << slot.name << " " << slot.type.ir;
            if (slot.scalable_size.has_value()) {
                out_ << " size=" << machine_scalable_size_name(*slot.scalable_size) << " align="
                     << machine_scalable_size_name(
                            slot.scalable_align.value_or(MachineScalableSize{}))
                     << " offset=scalable+"
                     << machine_scalable_size_name(
                            slot.scalable_offset.value_or(MachineScalableSize{}));
            } else {
                out_ << " size=" << slot.type.size << " align=" << slot.type.align << " offset=sp+"
                     << slot.offset;
            }
            out_ << " kind=" << stack_slot_kind_name(slot.kind) << "\n";
        }
    }

    if (!function.regs().virtual_registers().empty()) {
        out_ << "  virtual registers:\n";
        for (const auto &reg : function.regs().virtual_registers()) {
            out_ << "    " << reg_string(reg) << " ";
            if (reg.vector_type.has_value()) {
                out_ << machine_vector_type_name(*reg.vector_type);
            } else {
                out_ << value_type_name(reg.value_type);
            }
            if (auto phys = function.regs().allocation(reg)) {
                out_ << " -> " << reg_string(*phys);
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
    if (!function.saved_vector_registers().empty()) {
        out_ << "  saved vector registers:\n";
        for (const auto &saved : function.saved_vector_registers()) {
            out_ << "    " << saved.reg.name << " fi#" << saved.stack_slot
                 << "\n";
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
        if (instr.has_vector_info()) {
            const auto &info = instr.vector_info();
            out_ << " :: rvv{op=" << rvv_operation_name(info.operation)
                 << ",type=" << machine_vector_type_name(info.vector_type)
                 << ",avl=" << avl_string(info.avl)
                 << ",tail=" << vector_tail_policy_name(info.tail_policy)
                 << ",mask-policy=" << vector_mask_policy_name(info.mask_policy)
                 << ",vl-id=" << info.vl_identity
                 << ",round=" << vector_rounding_mode_name(info.rounding) << ",index-type="
                 << (info.index_vector_type.has_value()
                         ? machine_vector_type_name(*info.index_vector_type)
                         : "none")
                 << ",passthrough=" << optional_operand_index(info.passthrough_operand)
                 << ",mask=" << optional_operand_index(info.mask_operand) << "}";
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
    case OperandKind::Reloc:
        oss << (operand.relocation_kind() == RelocationKind::PCRelHi ? "%pcrel_hi("
                                                                     : "%pcrel_lo(")
            << operand.string_value() << ")";
        break;
    }
    return oss.str();
}

} // namespace mir
