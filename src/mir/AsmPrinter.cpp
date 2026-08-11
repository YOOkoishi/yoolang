#include "mir/AsmPrinter.h"
#include "mir/MachineInstrDesc.h"

#include <algorithm>
#include <limits>
#include <ostream>
#include <stdexcept>

namespace mir {
namespace {

int log2_align(std::uint64_t align) {
    int out = 0;
    while (align > 1) {
        align >>= 1;
        ++out;
    }
    return out;
}

const char *rvv_integer_binary_mnemonic(RVVOperation operation) {
    switch (operation) {
    case RVVOperation::Add:
        return "vadd.vv";
    case RVVOperation::Sub:
        return "vsub.vv";
    case RVVOperation::Mul:
        return "vmul.vv";
    case RVVOperation::Div:
        return "vdiv.vv";
    case RVVOperation::Rem:
        return "vrem.vv";
    case RVVOperation::And:
        return "vand.vv";
    case RVVOperation::Or:
        return "vor.vv";
    case RVVOperation::Xor:
        return "vxor.vv";
    case RVVOperation::Min:
        return "vmin.vv";
    case RVVOperation::Max:
        return "vmax.vv";
    default:
        throw std::runtime_error("unsupported final RVV integer operation");
    }
}

const char *rvv_float_binary_mnemonic(RVVOperation operation) {
    switch (operation) {
    case RVVOperation::Add:
        return "vfadd.vv";
    case RVVOperation::Sub:
        return "vfsub.vv";
    case RVVOperation::Mul:
        return "vfmul.vv";
    case RVVOperation::Div:
        return "vfdiv.vv";
    case RVVOperation::Min:
        return "vfmin.vv";
    case RVVOperation::Max:
        return "vfmax.vv";
    default:
        throw std::runtime_error("unsupported final RVV floating operation");
    }
}

const char *rvv_mask_logical_mnemonic(RVVOperation operation) {
    switch (operation) {
    case RVVOperation::MaskAnd:
        return "vmand.mm";
    case RVVOperation::MaskOr:
        return "vmor.mm";
    case RVVOperation::MaskXor:
        return "vmxor.mm";
    case RVVOperation::MaskNot:
        return "vmnand.mm";
    default:
        throw std::runtime_error("unsupported final RVV mask logical operation");
    }
}

const char *rvv_compare_mnemonic(RVVOperation operation, bool is_float) {
    if (is_float) {
        switch (operation) {
        case RVVOperation::Eq:
            return "vmfeq";
        case RVVOperation::Ne:
            return "vmfne";
        case RVVOperation::Lt:
            return "vmflt";
        case RVVOperation::Le:
            return "vmfle";
        default:
            throw std::runtime_error("unsupported final RVV floating comparison");
        }
    }
    switch (operation) {
    case RVVOperation::Eq:
        return "vmseq";
    case RVVOperation::Ne:
        return "vmsne";
    case RVVOperation::Lt:
        return "vmslt";
    case RVVOperation::Le:
        return "vmsle";
    default:
        throw std::runtime_error("unsupported final RVV integer comparison");
    }
}

const char *rvv_integer_reduction_mnemonic(RVVOperation operation) {
    switch (operation) {
    case RVVOperation::ReduceSum:
        return "vredsum.vs";
    case RVVOperation::ReduceMin:
        return "vredmin.vs";
    case RVVOperation::ReduceMax:
        return "vredmax.vs";
    case RVVOperation::ReduceAnd:
        return "vredand.vs";
    case RVVOperation::ReduceOr:
        return "vredor.vs";
    case RVVOperation::ReduceXor:
        return "vredxor.vs";
    default:
        throw std::runtime_error("unsupported final RVV integer reduction");
    }
}

void emit_i32_constant(std::ostream &out, const std::string &reg, std::int64_t value) {
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error("assembly constant exceeds signed 32-bit materialization");
    }
    if (value >= -2048 && value <= 2047) {
        out << "\taddi " << reg << ", zero, " << value << "\n";
        return;
    }
    const auto high = (value + 0x800) >> 12;
    const auto low = value - (high << 12);
    out << "\tlui " << reg << ", " << (high & 0xfffff) << "\n";
    if (low != 0) {
        out << "\taddiw " << reg << ", " << reg << ", " << low << "\n";
    }
}

} // namespace

AsmPrinter::AsmPrinter(std::ostream &out) : out_(out) {
}

void AsmPrinter::print(const Module &module) {
    out_ << "\t.attribute arch, \"" << module.target().arch << "\"\n";
    out_ << "\t.attribute stack_align, " << module.target().stack_align << "\n";
    out_ << "\t.option nopic\n";
    for (const auto &function : module.functions()) {
        if (function->is_variant_cc()) {
            // GNU as maps this directive to STO_RISCV_VARIANT_CC for both a
            // definition and an undefined reference.  Keep it module-scoped
            // so external variant callees are not silently left unmarked.
            out_ << "\t.variant_cc " << symbol_name(function->name()) << "\n";
        }
    }
    print_global_sections(module);
    out_ << "\t.text\n";
    for (const auto &function : module.functions()) {
        if (!function->is_external()) {
            print_function(*function);
        }
    }
}

void AsmPrinter::print_global_sections(const Module &module) {
    enum class Section { None, Data, Bss };
    Section current_section = Section::None;

    for (const auto &global : module.globals()) {
        const Section target_section = is_zero_initializer(global) ? Section::Bss : Section::Data;
        if (target_section != current_section) {
            if (target_section == Section::Bss) {
                out_ << "\t.bss\n";
            } else {
                out_ << "\t.data\n";
            }
            current_section = target_section;
        }
        print_global(global);
    }
}

void AsmPrinter::print_global(const Global &global) {
    out_ << "\t.globl " << symbol_name(global.name) << "\n";
    out_ << "\t.align " << log2_align(global.type.align) << "\n";
    out_ << "\t.type " << symbol_name(global.name) << ", @object\n";
    out_ << "\t.size " << symbol_name(global.name) << ", " << global.type.size << "\n";
    out_ << symbol_name(global.name) << ":\n";
    if (is_zero_initializer(global)) {
        out_ << "\t.zero " << global.type.size << "\n";
        return;
    }

    print_initializer_bytes(global);
}

void AsmPrinter::print_function(const MachineFunction &function) {
    out_ << "\t.align 1\n";
    out_ << "\t.globl " << symbol_name(function.name()) << "\n";
    out_ << "\t.type " << symbol_name(function.name()) << ", @function\n";
    out_ << symbol_name(function.name()) << ":\n";

    emit_adjust_scalable_sp(function.scalable_frame_size(), true);
    emit_adjust_sp(-function.frame_size());
    if (function.has_call()) {
        emit_int_slot_access("sd", "ra", "sp", function.return_address_offset());
    }
    for (const auto &saved : function.saved_registers()) {
        if (saved.reg.reg_class == RegisterClass::FPR32) {
            emit_float_slot_access("fsd", saved.reg.name, "sp", saved.offset);
        } else {
            emit_int_slot_access("sd", saved.reg.name, "sp", saved.offset);
        }
    }
    for (const auto &saved : function.saved_vector_registers()) {
        const auto *slot = function.stack_slot(saved.stack_slot);
        if (slot == nullptr) {
            throw std::runtime_error(
                "vector callee-save references a missing stack slot");
        }
        emit_vector_callee_saved_address(function, *slot);
        out_ << "\tvs1r.v " << saved.reg.name << ", (t6)\n";
    }

    for (const auto &block : function.blocks()) {
        out_ << label_for(function.name(), block->name()) << ":\n";
        for (const auto &instr : block->instructions()) {
            print_instr(function, instr);
        }
    }

    print_epilogue(function);
    out_ << "\t.size " << symbol_name(function.name()) << ", .-" << symbol_name(function.name())
         << "\n";
}

void AsmPrinter::print_instr(const MachineFunction &function, const MachineInstr &instr) {
    if (instruction_desc(instr.opcode()).has_flag(MIF_Pseudo)) {
        throw std::runtime_error(std::string("unexpanded pseudo reached AsmPrinter: ") +
                                 opcode_name(instr.opcode()));
    }
    const auto &ops = instr.operands();
    switch (instr.opcode()) {
    case Opcode::Add:
        out_ << "\tadd " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::AddW:
        out_ << "\taddw " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::AddI:
        out_ << "\taddi " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].int_value() << "\n";
        break;
    case Opcode::AddIW:
        out_ << "\taddiw " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].int_value() << "\n";
        break;
    case Opcode::Sub:
        out_ << "\tsub " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::SubW:
        out_ << "\tsubw " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::Mul:
        out_ << "\tmul " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::MulW:
        out_ << "\tmulw " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::DivU:
        out_ << "\tdivu " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::DivW:
        out_ << "\tdivw " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::RemW:
        out_ << "\tremw " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::And:
        out_ << "\tand " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::AndI:
        out_ << "\tandi " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].int_value() << "\n";
        break;
    case Opcode::SllI:
        out_ << "\tslli " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].int_value() << "\n";
        break;
    case Opcode::SllIW:
        out_ << "\tslliw " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].int_value() << "\n";
        break;
    case Opcode::SraI:
        out_ << "\tsrai " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].int_value() << "\n";
        break;
    case Opcode::SraIW:
        out_ << "\tsraiw " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].int_value() << "\n";
        break;
    case Opcode::Srli:
        out_ << "\tsrli " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].int_value() << "\n";
        break;
    case Opcode::SrliW:
        out_ << "\tsrliw " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].int_value() << "\n";
        break;
    case Opcode::Xor:
        out_ << "\txor " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::XorI:
        out_ << "\txori " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].int_value() << "\n";
        break;
    case Opcode::Slt:
        out_ << "\tslt " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::Sltu:
        out_ << "\tsltu " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::FAddS:
        out_ << "\tfadd.s " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::FSubS:
        out_ << "\tfsub.s " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::FMulS:
        out_ << "\tfmul.s " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::FDivS:
        out_ << "\tfdiv.s " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::FeqS:
        out_ << "\tfeq.s " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::FltS:
        out_ << "\tflt.s " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::FleS:
        out_ << "\tfle.s " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::FcvtSW:
        out_ << "\tfcvt.s.w " << ops[0].string_value() << ", " << ops[1].string_value() << "\n";
        break;
    case Opcode::FcvtWS:
        out_ << "\tfcvt.w.s " << ops[0].string_value() << ", " << ops[1].string_value()
             << ", rtz\n";
        break;
    case Opcode::FmvWX:
        out_ << "\tfmv.w.x " << ops[0].string_value() << ", " << ops[1].string_value() << "\n";
        break;
    case Opcode::FmvXW:
        out_ << "\tfmv.x.w " << ops[0].string_value() << ", " << ops[1].string_value() << "\n";
        break;
    case Opcode::BranchEq:
        out_ << "\tbeq " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << label_for(function.name(), ops[2].string_value()) << "\n";
        break;
    case Opcode::BranchNe:
        out_ << "\tbne " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << label_for(function.name(), ops[2].string_value()) << "\n";
        break;
    case Opcode::BranchLT:
        out_ << "\tblt " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << label_for(function.name(), ops[2].string_value()) << "\n";
        break;
    case Opcode::BranchGE:
        out_ << "\tbge " << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << label_for(function.name(), ops[2].string_value()) << "\n";
        break;
    case Opcode::RISCVLocalLabel:
        out_ << ops[0].string_value() << ":\n";
        break;
    case Opcode::RISCVLUI:
        out_ << "\tlui " << ops[0].string_value() << ", " << ops[1].int_value() << "\n";
        break;
    case Opcode::RISCVAUIPC:
        out_ << "\tauipc " << ops[0].string_value() << ", %pcrel_hi("
             << symbol_name(ops[1].string_value()) << ")\n";
        break;
    case Opcode::RISCVAddiReloc:
        out_ << "\taddi " << ops[0].string_value() << ", " << ops[1].string_value()
             << ", %pcrel_lo(" << ops[2].string_value() << ")\n";
        break;
    case Opcode::RISCVJAL:
        if (ops[1].kind() == OperandKind::Symbol &&
            ops[0].string_value() == "ra") {
            out_ << "\tcall " << symbol_name(ops[1].string_value()) << "\n";
            break;
        }
        out_ << "\tjal " << ops[0].string_value() << ", ";
        if (ops[1].kind() == OperandKind::Block) {
            out_ << label_for(function.name(), ops[1].string_value());
        } else {
            out_ << symbol_name(ops[1].string_value());
        }
        out_ << "\n";
        break;
    case Opcode::RISCVJALR:
        out_ << "\tjalr " << ops[0].string_value() << ", %pcrel_lo("
             << ops[2].string_value() << ")(" << ops[1].string_value() << ")\n";
        break;
    case Opcode::RISCVSLTIU:
        out_ << "\tsltiu " << ops[0].string_value() << ", " << ops[1].string_value()
             << ", " << ops[2].int_value() << "\n";
        break;
    case Opcode::RISCVFSGNJS:
        out_ << "\tfsgnj.s " << ops[0].string_value() << ", " << ops[1].string_value()
             << ", " << ops[2].string_value() << "\n";
        break;
    case Opcode::RISCVLBU:
    case Opcode::RISCVLW:
    case Opcode::RISCVLD:
    case Opcode::RISCVFLW: {
        const char *mnemonic = instr.opcode() == Opcode::RISCVLBU ? "lbu"
                               : instr.opcode() == Opcode::RISCVLW ? "lw"
                               : instr.opcode() == Opcode::RISCVLD ? "ld"
                                                                   : "flw";
        out_ << "\t" << mnemonic << " " << ops[0].string_value() << ", "
             << ops[2].int_value() << "(" << ops[1].string_value() << ")\n";
        break;
    }
    case Opcode::RISCVSB:
    case Opcode::RISCVSW:
    case Opcode::RISCVSD:
    case Opcode::RISCVFSW: {
        const char *mnemonic = instr.opcode() == Opcode::RISCVSB ? "sb"
                               : instr.opcode() == Opcode::RISCVSW ? "sw"
                               : instr.opcode() == Opcode::RISCVSD ? "sd"
                                                                   : "fsw";
        out_ << "\t" << mnemonic << " " << ops[0].string_value() << ", "
             << ops[2].int_value() << "(" << ops[1].string_value() << ")\n";
        break;
    }
    case Opcode::RISCVVSetVLI:
    case Opcode::RISCVVSetIVLI: {
        const auto &info = instr.vector_info();
        out_ << "\t"
             << (instr.opcode() == Opcode::RISCVVSetIVLI ? "vsetivli " : "vsetvli ")
             << ops[0].string_value() << ", ";
        if (ops[1].kind() == OperandKind::Imm) {
            out_ << ops[1].int_value();
        } else {
            out_ << ops[1].string_value();
        }
        out_ << ", e" << info.vector_type.sew_bits() << ", "
             << vector_lmul_name(info.vector_type.lmul()) << ", "
             << vector_tail_policy_name(info.tail_policy) << ", "
             << vector_mask_policy_name(info.mask_policy) << "\n";
        break;
    }
    case Opcode::RISCVVMaskSet:
        out_ << "\tvmset.m " << ops[0].string_value() << "\n";
        break;
    case Opcode::RISCVVMaskClear:
        out_ << "\tvmclr.m " << ops[0].string_value() << "\n";
        break;
    case Opcode::RISCVVMaskCopy:
        out_ << "\tvmand.mm " << ops[0].string_value() << ", "
             << ops[1].string_value() << ", " << ops[1].string_value() << "\n";
        break;
    case Opcode::RISCVVMaskLogical:
        out_ << "\t" << rvv_mask_logical_mnemonic(instr.vector_info().operation) << " "
             << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value() << "\n";
        break;
    case Opcode::RISCVVMaskPopCount:
        out_ << "\tvcpop.m " << ops[0].string_value() << ", "
             << ops[1].string_value() << ", " << ops[2].string_value() << ".t\n";
        break;
    case Opcode::RISCVVMaskFirst:
        out_ << "\tvfirst.m " << ops[0].string_value() << ", "
             << ops[1].string_value() << "\n";
        break;
    case Opcode::RISCVVMaskLoad:
        out_ << "\tvlm.v " << ops[0].string_value() << ", ("
             << ops[1].string_value() << ")\n";
        break;
    case Opcode::RISCVVMaskStore:
        out_ << "\tvsm.v " << ops[0].string_value() << ", ("
             << ops[1].string_value() << ")\n";
        break;
    case Opcode::RISCVVVectorCopy:
        out_ << "\tvmv.v.v " << ops[0].string_value() << ", "
             << ops[1].string_value() << "\n";
        break;
    case Opcode::RISCVVSplatVX:
        out_ << "\tvmv.v.x " << ops[0].string_value() << ", " << ops[1].string_value()
             << "\n";
        break;
    case Opcode::RISCVVSplatVI:
        out_ << "\tvmv.v.i " << ops[0].string_value() << ", " << ops[1].int_value()
             << "\n";
        break;
    case Opcode::RISCVVSplatVF:
        out_ << "\tvfmv.v.f " << ops[0].string_value() << ", " << ops[1].string_value()
             << "\n";
        break;
    case Opcode::RISCVVStep:
        out_ << "\tvid.v " << ops[0].string_value() << "\n";
        break;
    case Opcode::RISCVVIntBinaryVV:
    case Opcode::RISCVVFloatBinaryVV: {
        const auto &info = instr.vector_info();
        const auto *mnemonic = instr.opcode() == Opcode::RISCVVIntBinaryVV
                                   ? rvv_integer_binary_mnemonic(info.operation)
                                   : rvv_float_binary_mnemonic(info.operation);
        out_ << "\t" << mnemonic << " " << ops[0].string_value() << ", "
             << ops[1].string_value() << ", " << ops[2].string_value();
        if (info.mask_operand.has_value()) {
            out_ << ", " << ops[*info.mask_operand].string_value() << ".t";
        }
        out_ << "\n";
        break;
    }
    case Opcode::RISCVVLoadUnit: {
        const auto &info = instr.vector_info();
        out_ << "\tvle" << info.vector_type.sew_bits() << ".v " << ops[0].string_value()
             << ", (" << ops[1].string_value() << ")";
        if (info.mask_operand.has_value()) {
            out_ << ", " << ops[*info.mask_operand].string_value() << ".t";
        }
        out_ << "\n";
        break;
    }
    case Opcode::RISCVVStoreUnit: {
        const auto &info = instr.vector_info();
        out_ << "\tvse" << info.vector_type.sew_bits() << ".v " << ops[0].string_value()
             << ", (" << ops[1].string_value() << ")";
        if (info.mask_operand.has_value()) {
            out_ << ", " << ops[*info.mask_operand].string_value() << ".t";
        }
        out_ << "\n";
        break;
    }
    case Opcode::RISCVVLoadStrided: {
        const auto &info = instr.vector_info();
        out_ << "\tvlse" << info.vector_type.sew_bits() << ".v "
             << ops[0].string_value() << ", (" << ops[1].string_value() << "), "
             << ops[2].string_value();
        if (info.mask_operand.has_value()) {
            out_ << ", " << ops[*info.mask_operand].string_value() << ".t";
        }
        out_ << "\n";
        break;
    }
    case Opcode::RISCVVStoreStrided: {
        const auto &info = instr.vector_info();
        out_ << "\tvsse" << info.vector_type.sew_bits() << ".v "
             << ops[0].string_value() << ", (" << ops[1].string_value() << "), "
             << ops[2].string_value();
        if (info.mask_operand.has_value()) {
            out_ << ", " << ops[*info.mask_operand].string_value() << ".t";
        }
        out_ << "\n";
        break;
    }
    case Opcode::RISCVVLoadSegment2: {
        const auto &info = instr.vector_info();
        out_ << "\tvlseg2e" << info.vector_type.sew_bits() << ".v "
             << ops[0].string_value() << ", (" << ops[2].string_value() << ")";
        if (info.mask_operand.has_value()) {
            out_ << ", " << ops[*info.mask_operand].string_value() << ".t";
        }
        out_ << "\n";
        break;
    }
    case Opcode::RISCVVStoreSegment2: {
        const auto &info = instr.vector_info();
        out_ << "\tvsseg2e" << info.vector_type.sew_bits() << ".v "
             << ops[0].string_value() << ", (" << ops[2].string_value() << ")";
        if (info.mask_operand.has_value()) {
            out_ << ", " << ops[*info.mask_operand].string_value() << ".t";
        }
        out_ << "\n";
        break;
    }
    case Opcode::RISCVVLoadIndexedOrdered: {
        const auto &info = instr.vector_info();
        out_ << "\tvloxei32.v " << ops[0].string_value() << ", ("
             << ops[1].string_value() << "), " << ops[2].string_value();
        if (info.mask_operand.has_value()) {
            out_ << ", " << ops[*info.mask_operand].string_value() << ".t";
        }
        out_ << "\n";
        break;
    }
    case Opcode::RISCVVStoreIndexedOrdered: {
        const auto &info = instr.vector_info();
        out_ << "\tvsoxei32.v " << ops[0].string_value() << ", ("
             << ops[1].string_value() << "), " << ops[2].string_value();
        if (info.mask_operand.has_value()) {
            out_ << ", " << ops[*info.mask_operand].string_value() << ".t";
        }
        out_ << "\n";
        break;
    }
    case Opcode::RISCVVCompareVV:
    case Opcode::RISCVVCompareVX: {
        const auto &info = instr.vector_info();
        const bool is_float = info.vector_type.element_type() == ValueType::F32;
        out_ << "\t" << rvv_compare_mnemonic(info.operation, is_float)
             << (instr.opcode() == Opcode::RISCVVCompareVV ? ".vv " : ".vx ")
             << ops[0].string_value() << ", " << ops[1].string_value() << ", "
             << ops[2].string_value();
        if (info.mask_operand.has_value()) {
            out_ << ", " << ops[*info.mask_operand].string_value() << ".t";
        }
        out_ << "\n";
        break;
    }
    case Opcode::RISCVVMergeVVM:
        out_ << "\tvmerge.vvm " << ops[0].string_value() << ", "
             << ops[1].string_value() << ", " << ops[2].string_value() << ", "
             << ops[3].string_value() << "\n";
        break;
    case Opcode::RISCVVSlideDownVX:
    case Opcode::RISCVVSlideDownVI:
        out_ << "\tvslidedown."
             << (instr.opcode() == Opcode::RISCVVSlideDownVX ? "vx " : "vi ")
             << ops[0].string_value() << ", " << ops[1].string_value() << ", ";
        if (instr.opcode() == Opcode::RISCVVSlideDownVX) {
            out_ << ops[2].string_value();
        } else {
            out_ << ops[2].int_value();
        }
        if (instr.vector_info().mask_operand.has_value()) {
            out_ << ", " << ops[*instr.vector_info().mask_operand].string_value() << ".t";
        }
        out_ << "\n";
        break;
    case Opcode::RISCVVExtractElement:
        out_ << "\t"
             << (instr.vector_info().vector_type.element_type() == ValueType::F32
                     ? "vfmv.f.s "
                     : "vmv.x.s ")
             << ops[0].string_value() << ", " << ops[1].string_value() << "\n";
        break;
    case Opcode::RISCVVSIToFP:
        out_ << "\tvfcvt.f.x.v " << ops[0].string_value() << ", "
             << ops[1].string_value() << "\n";
        break;
    case Opcode::RISCVVFPToSI:
        out_ << "\tvfcvt.rtz.x.f.v " << ops[0].string_value() << ", "
             << ops[1].string_value() << "\n";
        break;
    case Opcode::RISCVVReductionInt:
    case Opcode::RISCVVReductionFloatOrdered: {
        const auto &info = instr.vector_info();
        const auto *mnemonic =
            instr.opcode() == Opcode::RISCVVReductionInt
                ? rvv_integer_reduction_mnemonic(info.operation)
                : "vfredosum.vs";
        out_ << "\t" << mnemonic << " " << ops[0].string_value() << ", "
             << ops[1].string_value() << ", " << ops[2].string_value();
        if (info.mask_operand.has_value()) {
            out_ << ", " << ops[*info.mask_operand].string_value() << ".t";
        }
        out_ << "\n";
        break;
    }
    case Opcode::RISCVVWholeRegSpill: {
        const auto width = instr.vector_info().vector_type.register_group_width();
        if (width != 1 && width != 2 && width != 4 && width != 8) {
            throw std::runtime_error(
                "whole-register spill has an unsupported register group width");
        }
        out_ << "\tvs" << width << "r.v " << ops[0].string_value() << ", ("
             << ops[1].string_value() << ")\n";
        break;
    }
    case Opcode::RISCVVWholeRegReload: {
        const auto width = instr.vector_info().vector_type.register_group_width();
        if (width != 1 && width != 2 && width != 4 && width != 8) {
            throw std::runtime_error(
                "whole-register reload has an unsupported register group width");
        }
        out_ << "\tvl" << width << "re32.v " << ops[0].string_value() << ", ("
             << ops[1].string_value() << ")\n";
        break;
    }
    case Opcode::RISCVReadVLENB:
        out_ << "\tcsrr " << ops[0].string_value() << ", vlenb\n";
        break;
    case Opcode::Comment:
    case Opcode::LoadImm:
    case Opcode::LoadFloatImm:
    case Opcode::LoadGlobalAddr:
    case Opcode::LoadStackAddr:
    case Opcode::LoadSlot:
    case Opcode::StoreSlot:
    case Opcode::LoadMem:
    case Opcode::StoreMem:
    case Opcode::LoadMemOffset:
    case Opcode::StoreMemOffset:
    case Opcode::MemZero:
    case Opcode::Move:
    case Opcode::FMove:
    case Opcode::SeqZ:
    case Opcode::Snez:
    case Opcode::StoreOutgoingArg:
    case Opcode::LoadIncomingArg:
    case Opcode::BranchNonZero:
    case Opcode::BranchZero:
    case Opcode::Jump:
    case Opcode::Call:
    case Opcode::RVVSetVL:
    case Opcode::RVVSetVLI:
    case Opcode::RVVMaskSet:
    case Opcode::RVVMaskClear:
    case Opcode::RVVMaskCopy:
    case Opcode::RVVMaskLogical:
    case Opcode::RVVMaskPopCount:
    case Opcode::RVVMaskFirst:
    case Opcode::RVVMaskLoad:
    case Opcode::RVVMaskStore:
    case Opcode::RVVVectorCopy:
    case Opcode::RVVSIToFP:
    case Opcode::RVVFPToSI:
    case Opcode::RVVSplatVX:
    case Opcode::RVVSplatVI:
    case Opcode::RVVSplatVF:
    case Opcode::RVVStep:
    case Opcode::RVVSplatVXTA:
    case Opcode::RVVSplatVITA:
    case Opcode::RVVSplatVFTA:
    case Opcode::RVVStepTA:
    case Opcode::RVVIntBinaryVVTA:
    case Opcode::RVVFloatBinaryVVTA:
    case Opcode::RVVLoadUnitTA:
    case Opcode::RVVLoadStridedTA:
    case Opcode::RVVCompareVVTA:
    case Opcode::RVVIntBinaryVV:
    case Opcode::RVVIntBinaryVX:
    case Opcode::RVVIntBinaryVI:
    case Opcode::RVVFloatBinaryVV:
    case Opcode::RVVFloatBinaryVF:
    case Opcode::RVVCompareVV:
    case Opcode::RVVCompareVX:
    case Opcode::RVVCompareVI:
    case Opcode::RVVCompareVF:
    case Opcode::RVVMergeVVM:
    case Opcode::RVVMergeVXM:
    case Opcode::RVVMergeVIM:
    case Opcode::RVVMergeVFM:
    case Opcode::RVVLoadUnit:
    case Opcode::RVVStoreUnit:
    case Opcode::RVVLoadStrided:
    case Opcode::RVVStoreStrided:
    case Opcode::RVVLoadSegment2:
    case Opcode::RVVStoreSegment2:
    case Opcode::RVVLoadIndexed:
    case Opcode::RVVStoreIndexed:
    case Opcode::RVVExtractElement:
    case Opcode::RVVInsertElement:
    case Opcode::RVVSlideUpVX:
    case Opcode::RVVSlideUpVI:
    case Opcode::RVVSlideDownVX:
    case Opcode::RVVSlideDownVI:
    case Opcode::RVVGatherVV:
    case Opcode::RVVGatherVX:
    case Opcode::RVVGatherVI:
    case Opcode::RVVReductionInt:
    case Opcode::RVVReductionFloat:
    case Opcode::RVVWholeRegSpill:
    case Opcode::RVVWholeRegReload:
        throw std::runtime_error(std::string("unexpanded pseudo reached AsmPrinter: ") +
                                 opcode_name(instr.opcode()));
    case Opcode::Count:
        throw std::runtime_error("invalid MIR opcode reached AsmPrinter");
    }
}

void AsmPrinter::print_epilogue(const MachineFunction &function) {
    out_ << label_for(function.name(), "epilogue") << ":\n";
    for (auto it = function.saved_vector_registers().rbegin();
         it != function.saved_vector_registers().rend(); ++it) {
        const auto *slot = function.stack_slot(it->stack_slot);
        if (slot == nullptr) {
            throw std::runtime_error(
                "vector callee-restore references a missing stack slot");
        }
        emit_vector_callee_saved_address(function, *slot);
        out_ << "\tvl1re8.v " << it->reg.name << ", (t6)\n";
    }
    for (auto it = function.saved_registers().rbegin(); it != function.saved_registers().rend();
         ++it) {
        if (it->reg.reg_class == RegisterClass::FPR32) {
            emit_float_slot_access("fld", it->reg.name, "sp", it->offset);
        } else {
            emit_int_slot_access("ld", it->reg.name, "sp", it->offset);
        }
    }
    if (function.has_call()) {
        emit_int_slot_access("ld", "ra", "sp", function.return_address_offset());
    }
    emit_adjust_sp(function.frame_size());
    emit_adjust_scalable_sp(function.scalable_frame_size(), false);
    out_ << "\tjalr zero, 0(ra)\n";
}

void AsmPrinter::emit_adjust_sp(std::int64_t amount) {
    if (amount == 0) {
        return;
    }
    if (fits_simm12(amount)) {
        out_ << "\taddi sp, sp, " << amount << "\n";
        return;
    }
    emit_i32_constant(out_, "t6", amount);
    out_ << "\tadd sp, sp, t6\n";
}

void AsmPrinter::emit_adjust_scalable_sp(MachineScalableSize size, bool allocate) {
    if (!size.is_valid()) {
        return;
    }
    if (size.vlenb_eighths % 8U != 0U) {
        throw std::runtime_error(
            "scalable stack frame size must be an integral multiple of vlenb");
    }
    const auto multiplier = size.vlenb_eighths / 8U;
    out_ << "\tcsrr t6, vlenb\n";
    if (multiplier != 1U) {
        emit_i32_constant(out_, "t5", multiplier);
        out_ << "\tmul t6, t6, t5\n";
    }
    out_ << "\taddi t6, t6, 15\n";
    out_ << "\tandi t6, t6, -16\n";
    out_ << (allocate ? "\tsub sp, sp, t6\n" : "\tadd sp, sp, t6\n");
}

void AsmPrinter::emit_vector_callee_saved_address(
    const MachineFunction &function, const StackSlot &slot) {
    if (!slot.scalable_offset.has_value() ||
        slot.scalable_offset->vlenb_eighths % 8U != 0U) {
        throw std::runtime_error(
            "vector callee-save slot has a non-integral vlenb offset");
    }
    const auto multiplier = slot.scalable_offset->vlenb_eighths / 8U;
    const auto fixed_offset = function.frame_size();
    if (multiplier == 0U) {
        if (fits_simm12(fixed_offset)) {
            out_ << "\taddi t6, sp, " << fixed_offset << "\n";
        } else {
            emit_i32_constant(out_, "t6", fixed_offset);
            out_ << "\tadd t6, sp, t6\n";
        }
        return;
    }

    out_ << "\tcsrr t6, vlenb\n";
    if (multiplier != 1U) {
        emit_i32_constant(out_, "t5", multiplier);
        out_ << "\tmul t6, t6, t5\n";
    }
    if (fixed_offset != 0) {
        if (fits_simm12(fixed_offset)) {
            out_ << "\taddi t6, t6, " << fixed_offset << "\n";
        } else {
            emit_i32_constant(out_, "t5", fixed_offset);
            out_ << "\tadd t6, t6, t5\n";
        }
    }
    out_ << "\tadd t6, sp, t6\n";
}

void AsmPrinter::emit_int_slot_access(const std::string &mnemonic, const std::string &reg,
                                      const std::string &base, std::int64_t offset) {
    if (fits_simm12(offset)) {
        out_ << "\t" << mnemonic << " " << reg << ", " << offset << "(" << base << ")\n";
        return;
    }
    emit_i32_constant(out_, "t6", offset);
    out_ << "\tadd t6, " << base << ", t6\n";
    out_ << "\t" << mnemonic << " " << reg << ", 0(t6)\n";
}

void AsmPrinter::emit_float_slot_access(const std::string &mnemonic, const std::string &reg,
                                        const std::string &base, std::int64_t offset) {
    if (fits_simm12(offset)) {
        out_ << "\t" << mnemonic << " " << reg << ", " << offset << "(" << base << ")\n";
        return;
    }
    emit_i32_constant(out_, "t6", offset);
    out_ << "\tadd t6, " << base << ", t6\n";
    out_ << "\t" << mnemonic << " " << reg << ", 0(t6)\n";
}

bool AsmPrinter::fits_simm12(std::int64_t value) const {
    return value >= -2048 && value <= 2047;
}

std::string AsmPrinter::label_for(const std::string &function, const std::string &block) const {
    if (block == "entry.0") {
        return ".L" + function + "_entry";
    }
    return ".L" + function + "_" + block;
}

std::string AsmPrinter::symbol_name(const std::string &name) const {
    return name;
}

bool AsmPrinter::is_zero_initializer(const Global &global) const {
    if (global.initializer_bytes.empty()) {
        return true;
    }
    if (global.initializer_bytes.size() != global.type.size) {
        throw std::runtime_error("MIR global initializer byte count does not match object size");
    }
    return std::all_of(global.initializer_bytes.begin(), global.initializer_bytes.end(),
                       [](std::uint8_t byte) { return byte == 0; });
}

void AsmPrinter::print_initializer_bytes(const Global &global) const {
    if (global.initializer_bytes.size() != global.type.size) {
        throw std::runtime_error("MIR global initializer byte count does not match object size");
    }

    std::size_t index = 0;
    while (index < global.initializer_bytes.size()) {
        if (global.initializer_bytes[index] == 0) {
            std::size_t end = index + 1;
            while (end < global.initializer_bytes.size() && global.initializer_bytes[end] == 0) {
                ++end;
            }
            out_ << "\t.zero " << (end - index) << "\n";
            index = end;
            continue;
        }

        out_ << "\t.byte " << static_cast<unsigned>(global.initializer_bytes[index]) << "\n";
        ++index;
    }
}

} // namespace mir
