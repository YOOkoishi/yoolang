#include "mir/AsmPrinter.h"

#include <cctype>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <sstream>

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

std::uint32_t float_bits(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

} // namespace

AsmPrinter::AsmPrinter(std::ostream &out) : out_(out) {
}

void AsmPrinter::print(const Module &module) {
    out_ << "\t.attribute stack_align, 16\n";
    out_ << "\t.option pic\n";
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
        const Section target_section =
            is_zero_initializer(global) ? Section::Bss : Section::Data;
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

    auto words = initializer_words(global);
    print_initializer_words(global, words);
}

void AsmPrinter::print_function(const MachineFunction &function) {
    current_function_ = &function;
    out_ << "\t.align 1\n";
    out_ << "\t.globl " << symbol_name(function.name()) << "\n";
    out_ << "\t.type " << symbol_name(function.name()) << ", @function\n";
    out_ << symbol_name(function.name()) << ":\n";

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

    for (const auto &block : function.blocks()) {
        out_ << label_for(function.name(), block->name()) << ":\n";
        for (const auto &instr : block->instructions()) {
            print_instr(function, instr);
        }
    }

    print_epilogue(function);
    out_ << "\t.size " << symbol_name(function.name()) << ", .-" << symbol_name(function.name())
         << "\n";
    current_function_ = nullptr;
}

void AsmPrinter::print_instr(const MachineFunction &function, const MachineInstr &instr) {
    const auto &ops = instr.operands();
    switch (instr.opcode()) {
    case Opcode::Comment:
        if (!ops.empty()) {
            out_ << "\t# " << ops[0].string_value() << "\n";
        }
        break;
    case Opcode::LoadImm:
        out_ << "\tli " << ops[0].string_value() << ", " << ops[1].int_value() << "\n";
        break;
    case Opcode::LoadFloatImm: {
        out_ << "\tli t6, " << float_bits(ops[1].float_value()) << "\n";
        out_ << "\tfmv.w.x " << ops[0].string_value() << ", t6\n";
        break;
    }
    case Opcode::LoadGlobalAddr:
        out_ << "\tla " << ops[0].string_value() << ", " << symbol_name(ops[1].string_value())
             << "\n";
        break;
    case Opcode::LoadStackAddr:
        emit_add_sp_offset(ops[0].string_value(),
                           function.stack_slot(ops[1].slot_id())->offset);
        break;
    case Opcode::LoadSlot:
        emit_load_slot(function, ops[0].string_value(), ops[1].slot_id(), ops[2].type_value());
        break;
    case Opcode::StoreSlot:
        emit_store_slot(function, ops[1].string_value(), ops[0].slot_id(), ops[2].type_value());
        break;
    case Opcode::LoadMem:
        emit_load_mem(ops[0].string_value(), ops[1].string_value(), 0, ops[2].type_value());
        break;
    case Opcode::StoreMem:
        emit_store_mem(ops[1].string_value(), ops[0].string_value(), 0, ops[2].type_value());
        break;
    case Opcode::LoadMemOffset:
        emit_load_mem(ops[0].string_value(), ops[1].string_value(), ops[2].int_value(),
                      ops[3].type_value());
        break;
    case Opcode::StoreMemOffset:
        emit_store_mem(ops[1].string_value(), ops[0].string_value(), ops[2].int_value(),
                       ops[3].type_value());
        break;
    case Opcode::MemZero:
        emit_memzero(ops[0], ops[1]);
        break;
    case Opcode::Move:
        out_ << "\tmv " << ops[0].string_value() << ", " << ops[1].string_value() << "\n";
        break;
    case Opcode::FMove:
        out_ << "\tfmv.s " << ops[0].string_value() << ", " << ops[1].string_value() << "\n";
        break;
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
    case Opcode::SeqZ:
        out_ << "\tseqz " << ops[0].string_value() << ", " << ops[1].string_value() << "\n";
        break;
    case Opcode::Snez:
        out_ << "\tsnez " << ops[0].string_value() << ", " << ops[1].string_value() << "\n";
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
        out_ << "\tfcvt.s.w " << ops[0].string_value() << ", " << ops[1].string_value()
             << "\n";
        break;
    case Opcode::FcvtWS:
        out_ << "\tfcvt.w.s " << ops[0].string_value() << ", " << ops[1].string_value()
             << ", rtz\n";
        break;
    case Opcode::FmvWX:
        out_ << "\tfmv.w.x " << ops[0].string_value() << ", " << ops[1].string_value() << "\n";
        break;
    case Opcode::StoreOutgoingArg:
        emit_store_outgoing_arg(ops[0].string_value(), ops[1].int_value(), ops[2].type_value());
        break;
    case Opcode::LoadIncomingArg:
        emit_load_incoming_arg(function, ops[0].string_value(), ops[1].int_value(),
                               ops[2].type_value());
        break;
    case Opcode::BranchNonZero:
        out_ << "\tbnez " << ops[0].string_value() << ", "
             << label_for(function.name(), ops[1].string_value()) << "\n";
        break;
    case Opcode::BranchZero:
        out_ << "\tbeqz " << ops[0].string_value() << ", "
             << label_for(function.name(), ops[1].string_value()) << "\n";
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
    case Opcode::Jump:
        if (ops[0].kind() == OperandKind::Block) {
            out_ << "\tj " << label_for(function.name(), ops[0].string_value()) << "\n";
        } else {
            out_ << "\tj " << ops[0].string_value() << "\n";
        }
        break;
    case Opcode::Call:
        out_ << "\tcall " << symbol_name(ops[0].string_value()) << "\n";
        break;
    }
}

void AsmPrinter::print_epilogue(const MachineFunction &function) {
    out_ << label_for(function.name(), "epilogue") << ":\n";
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
    out_ << "\tret\n";
}

void AsmPrinter::emit_load_slot(const MachineFunction &function, const std::string &reg, int slot,
                                ValueType type) {
    const auto *stack_slot = function.stack_slot(slot);
    if (is_float_type(type)) {
        emit_float_slot_access("flw", reg, "sp", stack_slot->offset);
    } else if (type == ValueType::Ptr) {
        emit_int_slot_access("ld", reg, "sp", stack_slot->offset);
    } else {
        emit_int_slot_access("lw", reg, "sp", stack_slot->offset);
    }
}

void AsmPrinter::emit_store_slot(const MachineFunction &function, const std::string &reg, int slot,
                                 ValueType type) {
    const auto *stack_slot = function.stack_slot(slot);
    if (is_float_type(type)) {
        emit_float_slot_access("fsw", reg, "sp", stack_slot->offset);
    } else if (type == ValueType::Ptr) {
        emit_int_slot_access("sd", reg, "sp", stack_slot->offset);
    } else {
        emit_int_slot_access("sw", reg, "sp", stack_slot->offset);
    }
}

void AsmPrinter::emit_load_mem(const std::string &reg, const std::string &addr_reg,
                               std::int64_t offset, ValueType type) {
    if (is_float_type(type)) {
        emit_float_slot_access("flw", reg, addr_reg, offset);
    } else if (type == ValueType::Ptr) {
        emit_int_slot_access("ld", reg, addr_reg, offset);
    } else {
        emit_int_slot_access("lw", reg, addr_reg, offset);
    }
}

void AsmPrinter::emit_store_mem(const std::string &reg, const std::string &addr_reg,
                                std::int64_t offset, ValueType type) {
    if (is_float_type(type)) {
        emit_float_slot_access("fsw", reg, addr_reg, offset);
    } else if (type == ValueType::Ptr) {
        emit_int_slot_access("sd", reg, addr_reg, offset);
    } else {
        emit_int_slot_access("sw", reg, addr_reg, offset);
    }
}

void AsmPrinter::emit_store_outgoing_arg(const std::string &reg, std::int64_t offset,
                                         ValueType type) {
    if (is_float_type(type)) {
        emit_float_slot_access("fsw", reg, "sp", offset);
    } else {
        emit_int_slot_access("sd", reg, "sp", offset);
    }
}

void AsmPrinter::emit_load_incoming_arg(const MachineFunction &function, const std::string &reg,
                                        std::int64_t offset, ValueType type) {
    std::int64_t incoming_offset = function.frame_size() + offset;
    if (is_float_type(type)) {
        emit_float_slot_access("flw", reg, "sp", incoming_offset);
    } else if (type == ValueType::Ptr) {
        emit_int_slot_access("ld", reg, "sp", incoming_offset);
    } else {
        emit_int_slot_access("lw", reg, "sp", incoming_offset);
    }
}

void AsmPrinter::emit_add_sp_offset(const std::string &dst, std::int64_t offset) {
    if (fits_simm12(offset)) {
        out_ << "\taddi " << dst << ", sp, " << offset << "\n";
        return;
    }
    out_ << "\tli t6, " << offset << "\n";
    out_ << "\tadd " << dst << ", sp, t6\n";
}

void AsmPrinter::emit_adjust_sp(std::int64_t amount) {
    if (amount == 0) {
        return;
    }
    if (fits_simm12(amount)) {
        out_ << "\taddi sp, sp, " << amount << "\n";
        return;
    }
    out_ << "\tli t6, " << amount << "\n";
    out_ << "\tadd sp, sp, t6\n";
}

void AsmPrinter::emit_memzero(const MachineOperand &addr, const MachineOperand &byte_count) {
    if (byte_count.kind() == OperandKind::Imm) {
        auto size = static_cast<std::uint64_t>(byte_count.int_value());
        if (size == 0) {
            return;
        }
        if (byte_count.int_value() >= kMemZeroMemsetThresholdBytes) {
            emit_memset_call(addr.string_value(), size);
            return;
        }
    }
    emit_memzero_loop(addr.string_value(), byte_count);
}

void AsmPrinter::emit_memset_call(const std::string &addr_reg, std::uint64_t size) {
    if (addr_reg != "a0") {
        out_ << "\tmv a0, " << addr_reg << "\n";
    }
    out_ << "\tli a1, 0\n";
    out_ << "\tli a2, " << size << "\n";
    out_ << "\tcall memset\n";
}

void AsmPrinter::emit_memzero_loop(const std::string &addr_reg,
                                   const MachineOperand &byte_count) {
    if (byte_count.kind() == OperandKind::Imm && byte_count.int_value() == 0) {
        return;
    }
    std::string loop = ".Lmemzero_" + std::to_string(unique_label_id_++);
    std::string done = loop + "_done";
    const std::string cursor = addr_reg == "t5" ? "t4" : "t5";
    const std::string counter = cursor == "t5" ? "t4" : "t5";
    out_ << "\tmv " << cursor << ", " << addr_reg << "\n";
    if (byte_count.kind() == OperandKind::Imm) {
        out_ << "\tli " << counter << ", " << byte_count.int_value() << "\n";
    } else {
        out_ << "\tmv " << counter << ", " << byte_count.string_value() << "\n";
    }
    out_ << loop << ":\n";
    out_ << "\tbge zero, " << counter << ", " << done << "\n";
    out_ << "\tsw zero, 0(" << cursor << ")\n";
    out_ << "\taddi " << cursor << ", " << cursor << ", 4\n";
    out_ << "\taddi " << counter << ", " << counter << ", -4\n";
    out_ << "\tj " << loop << "\n";
    out_ << done << ":\n";
}

void AsmPrinter::emit_int_slot_access(const std::string &mnemonic, const std::string &reg,
                                      const std::string &base, std::int64_t offset) {
    if (fits_simm12(offset)) {
        out_ << "\t" << mnemonic << " " << reg << ", " << offset << "(" << base << ")\n";
        return;
    }
    out_ << "\tli t6, " << offset << "\n";
    out_ << "\tadd t6, " << base << ", t6\n";
    out_ << "\t" << mnemonic << " " << reg << ", 0(t6)\n";
}

void AsmPrinter::emit_float_slot_access(const std::string &mnemonic, const std::string &reg,
                                        const std::string &base, std::int64_t offset) {
    if (fits_simm12(offset)) {
        out_ << "\t" << mnemonic << " " << reg << ", " << offset << "(" << base << ")\n";
        return;
    }
    out_ << "\tli t6, " << offset << "\n";
    out_ << "\tadd t6, " << base << ", t6\n";
    out_ << "\t" << mnemonic << " " << reg << ", 0(t6)\n";
}

bool AsmPrinter::is_int_type(ValueType type) const {
    return type == ValueType::I1 || type == ValueType::I32 || type == ValueType::Ptr;
}

bool AsmPrinter::is_float_type(ValueType type) const {
    return type == ValueType::F32;
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
    if (global.initializer == "zero" || global.initializer.empty()) {
        return true;
    }

    auto words = initializer_words(global);
    for (std::uint32_t word : words) {
        if (word != 0) {
            return false;
        }
    }
    return true;
}

void AsmPrinter::print_initializer_words(const Global &global,
                                         const std::vector<std::uint32_t> &words) const {
    std::uint64_t emitted = 0;
    std::size_t index = 0;
    while (index < words.size()) {
        if (words[index] == 0) {
            std::size_t end = index + 1;
            while (end < words.size() && words[end] == 0) {
                ++end;
            }
            std::uint64_t bytes = (end - index) * 4ULL;
            out_ << "\t.zero " << bytes << "\n";
            emitted += bytes;
            index = end;
            continue;
        }

        out_ << "\t.word " << words[index] << "\n";
        emitted += 4;
        ++index;
    }

    if (emitted < global.type.size) {
        out_ << "\t.zero " << (global.type.size - emitted) << "\n";
    }
}

std::vector<std::uint32_t> AsmPrinter::initializer_words(const Global &global) const {
    std::vector<std::uint32_t> words;
    const bool is_float = global.type.ir.find("float") != std::string::npos;
    const std::string &text = global.initializer;
    std::size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && !(std::isdigit(static_cast<unsigned char>(text[pos])) ||
                                      text[pos] == '-' || text[pos] == '+')) {
            ++pos;
        }
        if (pos >= text.size()) {
            break;
        }
        std::size_t start = pos;
        while (pos < text.size()) {
            char ch = text[pos];
            if (!(std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' || ch == '+' ||
                  ch == '.' || ch == 'e' || ch == 'E')) {
                break;
            }
            ++pos;
        }
        std::string token = text.substr(start, pos - start);
        if (is_float || token.find('.') != std::string::npos || token.find('e') != std::string::npos ||
            token.find('E') != std::string::npos) {
            words.push_back(float_bits(std::stof(token)));
        } else {
            words.push_back(static_cast<std::uint32_t>(std::stoll(token)));
        }
    }
    return words;
}

} // namespace mir
