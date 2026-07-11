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

std::int64_t repeated_byte_word(std::int64_t byte_value) {
    auto byte = static_cast<std::uint32_t>(byte_value) & 0xffU;
    std::uint32_t word = byte | (byte << 8U) | (byte << 16U) | (byte << 24U);
    return static_cast<std::int32_t>(word);
}

} // namespace

AsmPrinter::AsmPrinter(std::ostream &out) : out_(out) {
}

void AsmPrinter::print(const Module &module) {
    out_ << "\t.attribute stack_align, 16\n";
    out_ << "\t.option nopic\n";
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
    compute_stack_addr_facts(function);
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
        known_stack_addr_offsets_ = stack_addr_block_in_[block.get()];
        for (const auto &instr : block->instructions()) {
            print_instr(function, instr);
        }
    }

    print_epilogue(function);
    out_ << "\t.size " << symbol_name(function.name()) << ", .-" << symbol_name(function.name())
         << "\n";
    current_function_ = nullptr;
    stack_addr_block_in_.clear();
    known_stack_addr_offsets_.clear();
}

void AsmPrinter::update_stack_addr_facts(
    const MachineFunction &function, const MachineInstr &instr,
    std::unordered_map<std::string, std::int64_t> &facts) const {
    if (instr.opcode() == Opcode::Call) {
        facts.clear();
        return;
    }

    if (instr.opcode() == Opcode::MemZero) {
        invalidate_memzero_stack_addr_facts(instr, facts);
        return;
    }

    if (instr.opcode() != Opcode::LoadStackAddr) {
        for (const auto &reg : instr.defs()) {
            if (reg.is_physical()) {
                facts.erase(reg.name);
            }
        }
        return;
    }

    const auto &ops = instr.operands();
    if (ops.size() >= 2) {
        facts[ops[0].string_value()] = function.stack_slot(ops[1].slot_id())->offset;
    }
}

void AsmPrinter::invalidate_memzero_stack_addr_facts(
    const MachineInstr &instr, std::unordered_map<std::string, std::int64_t> &facts) const {
    const auto &ops = instr.operands();
    if (ops.size() < 3) {
        return;
    }

    const auto &byte_value = ops[1];
    const auto &byte_count = ops[2];
    if (memzero_uses_memset(byte_count)) {
        facts.clear();
        return;
    }

    if (byte_value.kind() == OperandKind::Imm && byte_value.int_value() != 0) {
        facts.erase("t3");
    }
    facts.erase("t4");
    facts.erase("t5");
    facts.erase("t6");
}

void AsmPrinter::compute_stack_addr_facts(const MachineFunction &function) {
    stack_addr_block_in_.clear();
    std::unordered_map<const MachineBasicBlock *,
                       std::unordered_map<std::string, std::int64_t>>
        block_out;

    for (const auto &block : function.blocks()) {
        stack_addr_block_in_[block.get()] = {};
        block_out[block.get()] = {};
    }

    auto intersect_facts =
        [](const std::unordered_map<std::string, std::int64_t> &lhs,
           const std::unordered_map<std::string, std::int64_t> &rhs) {
            std::unordered_map<std::string, std::int64_t> out;
            for (const auto &[reg, offset] : lhs) {
                auto found = rhs.find(reg);
                if (found != rhs.end() && found->second == offset) {
                    out[reg] = offset;
                }
            }
            return out;
        };

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &block_ptr : function.blocks()) {
            auto *block = block_ptr.get();
            std::unordered_map<std::string, std::int64_t> in;
            const auto &preds = block->predecessors();
            if (!preds.empty()) {
                in = block_out[preds.front()];
                for (std::size_t i = 1; i < preds.size(); ++i) {
                    in = intersect_facts(in, block_out[preds[i]]);
                }
            }

            if (stack_addr_block_in_[block] != in) {
                stack_addr_block_in_[block] = in;
                changed = true;
            }

            auto out = in;
            for (const auto &instr : block->instructions()) {
                update_stack_addr_facts(function, instr, out);
            }
            if (block_out[block] != out) {
                block_out[block] = std::move(out);
                changed = true;
            }
        }
    }
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
        emit_memzero(ops[0], ops[1], ops[2]);
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
    update_stack_addr_facts(function, instr, known_stack_addr_offsets_);
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

void AsmPrinter::emit_memzero(const MachineOperand &addr, const MachineOperand &byte_value,
                              const MachineOperand &byte_count) {
    if (byte_count.kind() == OperandKind::Imm) {
        auto size = static_cast<std::uint64_t>(byte_count.int_value());
        if (size == 0) {
            return;
        }
        const auto known_stack_addr = known_stack_addr_offsets_.find(addr.string_value());
        const bool prefer_wide_zero_stores =
            known_stack_addr != known_stack_addr_offsets_.end() && known_stack_addr->second % 8 == 0;
        if (emit_inline_memzero_stores(addr.string_value(), byte_value, size,
                                       prefer_wide_zero_stores)) {
            return;
        }
        if (memzero_uses_memset(byte_count)) {
            emit_memset_call(addr.string_value(), byte_value, size);
            return;
        }
    }
    emit_memzero_loop(addr.string_value(), byte_value, byte_count);
}

bool AsmPrinter::emit_inline_memzero_stores(const std::string &addr_reg,
                                            const MachineOperand &byte_value,
                                            std::uint64_t size,
                                            bool prefer_wide_zero_stores) {
    constexpr std::uint64_t kMaxInlineStoreBytes = 64;
    if (size > kMaxInlineStoreBytes || size % 4 != 0) {
        return false;
    }
    if (byte_value.kind() != OperandKind::Imm) {
        return false;
    }

    const bool stores_zero = byte_value.int_value() == 0;
    if (stores_zero && prefer_wide_zero_stores && size % 8 == 0) {
        for (std::uint64_t offset = 0; offset < size; offset += 8) {
            out_ << "\tsd zero, " << offset << "(" << addr_reg << ")\n";
        }
        return true;
    }

    const std::string value_reg = stores_zero ? "zero" : (addr_reg == "t3" ? "t4" : "t3");
    if (!stores_zero) {
        out_ << "\tli " << value_reg << ", " << repeated_byte_word(byte_value.int_value()) << "\n";
    }
    for (std::uint64_t offset = 0; offset < size; offset += 4) {
        out_ << "\tsw " << value_reg << ", " << offset << "(" << addr_reg << ")\n";
    }
    return true;
}

void AsmPrinter::emit_memset_call(const std::string &addr_reg, const MachineOperand &byte_value,
                                  std::uint64_t size) {
    if (addr_reg != "a0") {
        out_ << "\tmv a0, " << addr_reg << "\n";
    }
    out_ << "\tli a1, " << byte_value.int_value() << "\n";
    out_ << "\tli a2, " << size << "\n";
    out_ << "\tcall memset\n";
}

void AsmPrinter::emit_memzero_loop(const std::string &addr_reg,
                                   const MachineOperand &byte_value,
                                   const MachineOperand &byte_count) {
    if (byte_count.kind() == OperandKind::Imm && byte_count.int_value() == 0) {
        return;
    }
    std::string loop = ".Lmemzero_" + std::to_string(unique_label_id_++);
    std::string done = loop + "_done";
    const std::string cursor = addr_reg == "t5" ? "t4" : "t5";
    const std::string counter = cursor == "t5" ? "t4" : "t5";
    if (byte_count.kind() == OperandKind::Imm) {
        out_ << "\tmv " << cursor << ", " << addr_reg << "\n";
        out_ << "\tli " << counter << ", " << byte_count.int_value() << "\n";
    } else {
        const auto &count_reg = byte_count.string_value();
        if (count_reg == cursor && addr_reg == counter) {
            // Spill rewriting may assign the two inputs to t4/t5. Preserve the
            // parallel-copy semantics when the requested scratch assignment is a swap.
            out_ << "\tmv t6, " << addr_reg << "\n";
            out_ << "\tmv " << counter << ", " << count_reg << "\n";
            out_ << "\tmv " << cursor << ", t6\n";
        } else if (count_reg == cursor) {
            // Save the count before installing the cursor over its source register.
            out_ << "\tmv " << counter << ", " << count_reg << "\n";
            if (addr_reg != cursor) {
                out_ << "\tmv " << cursor << ", " << addr_reg << "\n";
            }
        } else {
            if (addr_reg != cursor) {
                out_ << "\tmv " << cursor << ", " << addr_reg << "\n";
            }
            if (count_reg != counter) {
                out_ << "\tmv " << counter << ", " << count_reg << "\n";
            }
        }
    }
    const bool stores_zero = byte_value.kind() == OperandKind::Imm && byte_value.int_value() == 0;
    if (!stores_zero) {
        out_ << "\tli t3, " << repeated_byte_word(byte_value.int_value()) << "\n";
    }
    out_ << loop << ":\n";
    out_ << "\tbge zero, " << counter << ", " << done << "\n";
    out_ << "\tsw " << (stores_zero ? "zero" : "t3") << ", 0(" << cursor << ")\n";
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
