#include "pass/mir/MIRPseudoExpansionPass.h"

#include "mir/MIR.h"
#include "mir/MIRVerifier.h"
#include "mir/MachineInstrDesc.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pass {
namespace {

mir::Register gpr(const char *name, mir::ValueType type = mir::ValueType::I32) {
    return mir::Register::physical(name, mir::RegisterClass::GPR, type);
}

bool fits_simm12(std::int64_t value) {
    return value >= -2048 && value <= 2047;
}

std::int32_t float_bits(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "f32 must have 32-bit storage");
    std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<std::int32_t>(bits);
}

void emit_load_immediate_impl(std::vector<mir::MachineInstr> &out,
                              const mir::Register &destination, std::int64_t value) {
    if (fits_simm12(value)) {
        out.emplace_back(
            mir::Opcode::AddI,
            std::vector<mir::MachineOperand>{
                mir::MachineOperand::reg_def(destination),
                mir::MachineOperand::reg_use(gpr("zero", destination.value_type)),
                mir::MachineOperand::imm(value)});
        return;
    }

    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max()) {
        const auto low = static_cast<std::int64_t>(
            static_cast<std::int16_t>((static_cast<std::uint64_t>(value) & 0xfffU) |
                                      ((static_cast<std::uint64_t>(value) & 0x800U) != 0U
                                           ? 0xf000U
                                           : 0U)));
        const auto high = static_cast<std::int64_t>(
            (static_cast<__int128>(value) - static_cast<__int128>(low)) / 4096);
        emit_load_immediate_impl(out, destination, high);
        out.emplace_back(
            mir::Opcode::SllI,
            std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(destination),
                                             mir::MachineOperand::reg_use(destination),
                                             mir::MachineOperand::imm(12)});
        if (low != 0) {
            out.emplace_back(
                mir::Opcode::AddI,
                std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(destination),
                                                 mir::MachineOperand::reg_use(destination),
                                                 mir::MachineOperand::imm(low)});
        }
        return;
    }

    const auto high = (value + 0x800) >> 12;
    const auto low = value - (high << 12);
    out.emplace_back(
        mir::Opcode::RISCVLUI,
        std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(destination),
                                         mir::MachineOperand::imm(high & 0xfffff)});
    if (low != 0) {
        out.emplace_back(
            mir::Opcode::AddIW,
            std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(destination),
                                             mir::MachineOperand::reg_use(destination),
                                             mir::MachineOperand::imm(low)});
    }
}

void emit_load_immediate(std::vector<mir::MachineInstr> &out, mir::Register destination,
                         std::int64_t value) {
    if (destination.value_type == mir::ValueType::I1 ||
        destination.value_type == mir::ValueType::I32) {
        if (value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("32-bit LI operand exceeds its machine value type");
        }
        value = static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
    }
    emit_load_immediate_impl(out, destination, value);
}

mir::Opcode load_opcode(mir::ValueType type, bool byte_sized_i1 = true) {
    switch (type) {
    case mir::ValueType::I1:
        return byte_sized_i1 ? mir::Opcode::RISCVLBU : mir::Opcode::RISCVLW;
    case mir::ValueType::I32:
        return mir::Opcode::RISCVLW;
    case mir::ValueType::Ptr:
        return mir::Opcode::RISCVLD;
    case mir::ValueType::F32:
        return mir::Opcode::RISCVFLW;
    case mir::ValueType::Void:
    case mir::ValueType::Aggregate:
        break;
    }
    throw std::runtime_error("cannot select a concrete load for MIR type");
}

mir::Opcode store_opcode(mir::ValueType type, bool byte_sized_i1 = true) {
    switch (type) {
    case mir::ValueType::I1:
        return byte_sized_i1 ? mir::Opcode::RISCVSB : mir::Opcode::RISCVSW;
    case mir::ValueType::I32:
        return mir::Opcode::RISCVSW;
    case mir::ValueType::Ptr:
        return mir::Opcode::RISCVSD;
    case mir::ValueType::F32:
        return mir::Opcode::RISCVFSW;
    case mir::ValueType::Void:
    case mir::ValueType::Aggregate:
        break;
    }
    throw std::runtime_error("cannot select a concrete store for MIR type");
}

void emit_memory_access(std::vector<mir::MachineInstr> &out, mir::Opcode opcode,
                        mir::Register value, mir::Register base, std::int64_t offset,
                        bool is_load) {
    if (!fits_simm12(offset)) {
        auto scratch = gpr("t6", mir::ValueType::Ptr);
        emit_load_immediate(out, scratch, offset);
        out.emplace_back(
            mir::Opcode::Add,
            std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(scratch),
                                             mir::MachineOperand::reg_use(base),
                                             mir::MachineOperand::reg_use(scratch)});
        base = scratch;
        offset = 0;
    }
    out.emplace_back(
        opcode,
        std::vector<mir::MachineOperand>{
            is_load ? mir::MachineOperand::reg_def(value)
                    : mir::MachineOperand::reg_use(value),
            mir::MachineOperand::reg_use(base), mir::MachineOperand::imm(offset)});
}

mir::Register emit_scalable_slot_address(
    std::vector<mir::MachineInstr> &out, const mir::MachineFunction &function,
    const mir::StackSlot &slot) {
    if (!slot.scalable_offset.has_value() ||
        slot.scalable_offset->vlenb_eighths % 8U != 0U) {
        throw std::runtime_error(
            "whole-register spill slot has a non-integral vlenb offset");
    }
    const auto address = gpr("t6", mir::ValueType::Ptr);
    const auto scalar = gpr("t5", mir::ValueType::Ptr);
    const auto sp = gpr("sp", mir::ValueType::Ptr);
    const auto vlenb_multiplier =
        slot.scalable_offset->vlenb_eighths / 8U;
    const auto fixed_offset = function.frame_size();

    if (vlenb_multiplier == 0U) {
        if (fits_simm12(fixed_offset)) {
            out.emplace_back(
                mir::Opcode::AddI,
                std::vector<mir::MachineOperand>{
                    mir::MachineOperand::reg_def(address),
                    mir::MachineOperand::reg_use(sp),
                    mir::MachineOperand::imm(fixed_offset)});
        } else {
            emit_load_immediate(out, address, fixed_offset);
            out.emplace_back(
                mir::Opcode::Add,
                std::vector<mir::MachineOperand>{
                    mir::MachineOperand::reg_def(address),
                    mir::MachineOperand::reg_use(sp),
                    mir::MachineOperand::reg_use(address)});
        }
        return address;
    }

    out.emplace_back(
        mir::Opcode::RISCVReadVLENB,
        std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(address)});
    if (vlenb_multiplier != 1U) {
        emit_load_immediate(out, scalar, vlenb_multiplier);
        out.emplace_back(
            mir::Opcode::Mul,
            std::vector<mir::MachineOperand>{
                mir::MachineOperand::reg_def(address),
                mir::MachineOperand::reg_use(address),
                mir::MachineOperand::reg_use(scalar)});
    }
    if (fixed_offset != 0) {
        if (fits_simm12(fixed_offset)) {
            out.emplace_back(
                mir::Opcode::AddI,
                std::vector<mir::MachineOperand>{
                    mir::MachineOperand::reg_def(address),
                    mir::MachineOperand::reg_use(address),
                    mir::MachineOperand::imm(fixed_offset)});
        } else {
            emit_load_immediate(out, scalar, fixed_offset);
            out.emplace_back(
                mir::Opcode::Add,
                std::vector<mir::MachineOperand>{
                    mir::MachineOperand::reg_def(address),
                    mir::MachineOperand::reg_use(address),
                    mir::MachineOperand::reg_use(scalar)});
        }
    }
    out.emplace_back(
        mir::Opcode::Add,
        std::vector<mir::MachineOperand>{
            mir::MachineOperand::reg_def(address),
            mir::MachineOperand::reg_use(sp),
            mir::MachineOperand::reg_use(address)});
    return address;
}

bool emit_aligned_scalable_frame_bytes(
    std::vector<mir::MachineInstr> &out, mir::MachineScalableSize size,
    const mir::Register &destination, const mir::Register &scratch) {
    if (!size.is_valid()) {
        return false;
    }
    if (size.vlenb_eighths % 8U != 0U) {
        throw std::runtime_error(
            "scalable stack frame size must be an integral multiple of vlenb");
    }
    const auto multiplier = size.vlenb_eighths / 8U;
    out.emplace_back(
        mir::Opcode::RISCVReadVLENB,
        std::vector<mir::MachineOperand>{
            mir::MachineOperand::reg_def(destination)});
    if (multiplier != 1U) {
        emit_load_immediate(out, scratch, multiplier);
        out.emplace_back(
            mir::Opcode::Mul,
            std::vector<mir::MachineOperand>{
                mir::MachineOperand::reg_def(destination),
                mir::MachineOperand::reg_use(destination),
                mir::MachineOperand::reg_use(scratch)});
    }
    // The psABI requires sp to stay 128-bit aligned.  Zve permits VLENB=4
    // or 8, so an integral number of vector registers is not necessarily a
    // 16-byte multiple.
    out.emplace_back(
        mir::Opcode::AddI,
        std::vector<mir::MachineOperand>{
            mir::MachineOperand::reg_def(destination),
            mir::MachineOperand::reg_use(destination), mir::MachineOperand::imm(15)});
    out.emplace_back(
        mir::Opcode::AndI,
        std::vector<mir::MachineOperand>{
            mir::MachineOperand::reg_def(destination),
            mir::MachineOperand::reg_use(destination), mir::MachineOperand::imm(-16)});
    return true;
}

mir::Register emit_entry_stack_address(
    std::vector<mir::MachineInstr> &out, const mir::MachineFunction &function,
    std::int64_t incoming_offset) {
    const auto address = gpr("t6", mir::ValueType::Ptr);
    const auto scratch = gpr("t5", mir::ValueType::Ptr);
    const auto sp = gpr("sp", mir::ValueType::Ptr);
    const auto fixed_and_incoming = function.frame_size() + incoming_offset;
    const bool has_scalable_frame = emit_aligned_scalable_frame_bytes(
        out, function.scalable_frame_size(), address, scratch);

    if (!has_scalable_frame) {
        if (fits_simm12(fixed_and_incoming)) {
            out.emplace_back(
                mir::Opcode::AddI,
                std::vector<mir::MachineOperand>{
                    mir::MachineOperand::reg_def(address),
                    mir::MachineOperand::reg_use(sp),
                    mir::MachineOperand::imm(fixed_and_incoming)});
        } else {
            emit_load_immediate(out, address, fixed_and_incoming);
            out.emplace_back(
                mir::Opcode::Add,
                std::vector<mir::MachineOperand>{
                    mir::MachineOperand::reg_def(address),
                    mir::MachineOperand::reg_use(sp),
                    mir::MachineOperand::reg_use(address)});
        }
        return address;
    }

    if (fixed_and_incoming != 0) {
        if (fits_simm12(fixed_and_incoming)) {
            out.emplace_back(
                mir::Opcode::AddI,
                std::vector<mir::MachineOperand>{
                    mir::MachineOperand::reg_def(address),
                    mir::MachineOperand::reg_use(address),
                    mir::MachineOperand::imm(fixed_and_incoming)});
        } else {
            emit_load_immediate(out, scratch, fixed_and_incoming);
            out.emplace_back(
                mir::Opcode::Add,
                std::vector<mir::MachineOperand>{
                    mir::MachineOperand::reg_def(address),
                    mir::MachineOperand::reg_use(address),
                    mir::MachineOperand::reg_use(scratch)});
        }
    }
    out.emplace_back(
        mir::Opcode::Add,
        std::vector<mir::MachineOperand>{
            mir::MachineOperand::reg_def(address),
            mir::MachineOperand::reg_use(sp),
            mir::MachineOperand::reg_use(address)});
    return address;
}

std::string relocation_label(const mir::MachineFunction &function, unsigned &next_label,
                             const char *kind) {
    return ".L" + function.name() + "." + kind + "." +
           std::to_string(next_label++);
}

void emit_symbol_address(std::vector<mir::MachineInstr> &out,
                         const mir::MachineFunction &function, unsigned &next_label,
                         mir::Register destination, const std::string &symbol) {
    const auto anchor = relocation_label(function, next_label, "pcrel");
    out.emplace_back(mir::Opcode::RISCVLocalLabel,
                     std::vector<mir::MachineOperand>{mir::MachineOperand::symbol(anchor)});
    out.emplace_back(
        mir::Opcode::RISCVAUIPC,
        std::vector<mir::MachineOperand>{
            mir::MachineOperand::reg_def(destination),
            mir::MachineOperand::reloc(mir::RelocationKind::PCRelHi, symbol)});
    out.emplace_back(
        mir::Opcode::RISCVAddiReloc,
        std::vector<mir::MachineOperand>{
            mir::MachineOperand::reg_def(destination),
            mir::MachineOperand::reg_use(destination),
            mir::MachineOperand::reloc(mir::RelocationKind::PCRelLo, anchor)});
}

void emit_direct_call(std::vector<mir::MachineInstr> &out,
                      const mir::MachineFunction &function, unsigned &next_label,
                      const std::string &symbol) {
    auto ra = gpr("ra", mir::ValueType::Ptr);
    if (function.has_standard_aggregate_call()) {
        // Preserve the assembler's canonical CALL_PLT relocation for calls
        // whose fixed-vector aggregate callee may be a weak, interposable ABI
        // stub.  Scalar-only code retains the explicit medany sequence below.
        out.emplace_back(
            mir::Opcode::RISCVJAL,
            std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(ra),
                                             mir::MachineOperand::symbol(symbol)});
        return;
    }
    const auto anchor = relocation_label(function, next_label, "call");
    out.emplace_back(mir::Opcode::RISCVLocalLabel,
                     std::vector<mir::MachineOperand>{mir::MachineOperand::symbol(anchor)});
    out.emplace_back(
        mir::Opcode::RISCVAUIPC,
        std::vector<mir::MachineOperand>{
            mir::MachineOperand::reg_def(ra),
            mir::MachineOperand::reloc(mir::RelocationKind::PCRelHi, symbol)});
    out.emplace_back(
        mir::Opcode::RISCVJALR,
        std::vector<mir::MachineOperand>{
            mir::MachineOperand::reg_def(ra), mir::MachineOperand::reg_use(ra),
            mir::MachineOperand::reloc(mir::RelocationKind::PCRelLo, anchor)});
}

mir::Opcode expanded_rvv_opcode(const mir::MachineInstr &instr) {
    switch (instr.opcode()) {
    case mir::Opcode::RVVSetVLI:
        if (instr.operands().size() < 2) {
            throw std::runtime_error("malformed PseudoVSETVLI during expansion");
        }
        return instr.operands()[1].kind() == mir::OperandKind::Imm
                   ? mir::Opcode::RISCVVSetIVLI
                   : mir::Opcode::RISCVVSetVLI;
    case mir::Opcode::RVVMaskSet:
        return mir::Opcode::RISCVVMaskSet;
    case mir::Opcode::RVVMaskClear:
        return mir::Opcode::RISCVVMaskClear;
    case mir::Opcode::RVVMaskCopy:
        return mir::Opcode::RISCVVMaskCopy;
    case mir::Opcode::RVVMaskLogical:
        return mir::Opcode::RISCVVMaskLogical;
    case mir::Opcode::RVVMaskPopCount:
        return mir::Opcode::RISCVVMaskPopCount;
    case mir::Opcode::RVVMaskFirst:
        return mir::Opcode::RISCVVMaskFirst;
    case mir::Opcode::RVVMaskLoad:
        return mir::Opcode::RISCVVMaskLoad;
    case mir::Opcode::RVVVectorCopy:
        return mir::Opcode::RISCVVVectorCopy;
    case mir::Opcode::RVVSIToFP:
        return mir::Opcode::RISCVVSIToFP;
    case mir::Opcode::RVVFPToSI:
        return mir::Opcode::RISCVVFPToSI;
    case mir::Opcode::RVVSplatVXTA:
        return mir::Opcode::RISCVVSplatVX;
    case mir::Opcode::RVVSplatVITA:
        return mir::Opcode::RISCVVSplatVI;
    case mir::Opcode::RVVSplatVFTA:
        return mir::Opcode::RISCVVSplatVF;
    case mir::Opcode::RVVStepTA:
        return mir::Opcode::RISCVVStep;
    case mir::Opcode::RVVIntBinaryVVTA:
    case mir::Opcode::RVVIntBinaryVV:
        return mir::Opcode::RISCVVIntBinaryVV;
    case mir::Opcode::RVVFloatBinaryVVTA:
    case mir::Opcode::RVVFloatBinaryVV:
        return mir::Opcode::RISCVVFloatBinaryVV;
    case mir::Opcode::RVVLoadUnitTA:
    case mir::Opcode::RVVLoadUnit:
        return mir::Opcode::RISCVVLoadUnit;
    case mir::Opcode::RVVCompareVVTA:
        return mir::Opcode::RISCVVCompareVV;
    case mir::Opcode::RVVStoreUnit:
        return mir::Opcode::RISCVVStoreUnit;
    case mir::Opcode::RVVLoadStridedTA:
    case mir::Opcode::RVVLoadStrided:
        return mir::Opcode::RISCVVLoadStrided;
    case mir::Opcode::RVVStoreStrided:
        return mir::Opcode::RISCVVStoreStrided;
    case mir::Opcode::RVVLoadSegment2:
        return mir::Opcode::RISCVVLoadSegment2;
    case mir::Opcode::RVVStoreSegment2:
        return mir::Opcode::RISCVVStoreSegment2;
    case mir::Opcode::RVVLoadIndexed:
        return mir::Opcode::RISCVVLoadIndexedOrdered;
    case mir::Opcode::RVVStoreIndexed:
        return mir::Opcode::RISCVVStoreIndexedOrdered;
    case mir::Opcode::RVVCompareVV:
        return mir::Opcode::RISCVVCompareVV;
    case mir::Opcode::RVVCompareVX:
        return mir::Opcode::RISCVVCompareVX;
    case mir::Opcode::RVVMergeVVM:
        return mir::Opcode::RISCVVMergeVVM;
    case mir::Opcode::RVVSlideDownVX:
        return mir::Opcode::RISCVVSlideDownVX;
    case mir::Opcode::RVVSlideDownVI:
        return mir::Opcode::RISCVVSlideDownVI;
    case mir::Opcode::RVVExtractElement:
        return mir::Opcode::RISCVVExtractElement;
    case mir::Opcode::RVVReductionInt:
        return mir::Opcode::RISCVVReductionInt;
    case mir::Opcode::RVVReductionFloat:
        return mir::Opcode::RISCVVReductionFloatOrdered;
    default:
        break;
    }
    throw std::runtime_error(std::string("unsupported RVV pseudo expansion: ") +
                             mir::opcode_name(instr.opcode()));
}

std::int32_t repeated_byte_word(std::int64_t byte) {
    const auto value = static_cast<std::uint32_t>(byte) & 0xffU;
    return static_cast<std::int32_t>(value | (value << 8U) | (value << 16U) |
                                     (value << 24U));
}

void expand_memzero(const mir::MachineInstr &instr, mir::MachineFunction &function,
                    unsigned &next_label, bool prefer_wide_zero_stores,
                    std::vector<mir::MachineInstr> &out) {
    const auto &ops = instr.operands();
    const auto byte = ops[1].int_value();
    if (ops[2].kind() != mir::OperandKind::Imm) {
        throw std::runtime_error(
            "dynamic MEMZERO must be control-flow lowered before pseudo expansion");
    }
    if (ops[2].int_value() == 0) {
        return;
    }

    const auto count = ops[2].int_value();
    if (count <= 64 && count % 4 == 0) {
        if (byte == 0 && prefer_wide_zero_stores && count % 8 == 0) {
            for (std::int64_t offset = 0; offset < count; offset += 8) {
                emit_memory_access(out, mir::Opcode::RISCVSD,
                                   gpr("zero", mir::ValueType::Ptr),
                                   ops[0].reg_value(), offset, false);
            }
            return;
        }
        auto source = gpr("zero");
        if (byte != 0) {
            source = gpr(ops[0].reg_value().name == "t3" ? "t4" : "t3");
            emit_load_immediate(out, source, repeated_byte_word(byte));
        }
        for (std::int64_t offset = 0; offset < count; offset += 4) {
            emit_memory_access(out, mir::Opcode::RISCVSW, source,
                               ops[0].reg_value(), offset, false);
        }
        return;
    }

    if (count < mir::kMemZeroMemsetThresholdBytes) {
        auto source = gpr("zero");
        if (byte != 0) {
            source = gpr("t4");
            emit_load_immediate(out, source, byte);
        }
        for (std::int64_t offset = 0; offset < count; ++offset) {
            emit_memory_access(out, mir::Opcode::RISCVSB, source,
                               ops[0].reg_value(), offset, false);
        }
        return;
    }

    const auto staged_address = gpr("t4", mir::ValueType::Ptr);
    out.emplace_back(
        mir::Opcode::AddI,
        std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(staged_address),
                                         mir::MachineOperand::reg_use(ops[0].reg_value()),
                                         mir::MachineOperand::imm(0)});
    out.emplace_back(
        mir::Opcode::AddI,
        std::vector<mir::MachineOperand>{
            mir::MachineOperand::reg_def(gpr("a0", mir::ValueType::Ptr)),
            mir::MachineOperand::reg_use(staged_address), mir::MachineOperand::imm(0)});
    emit_load_immediate(out, gpr("a1"), byte);
    emit_load_immediate(out, gpr("a2"), count);
    emit_direct_call(out, function, next_label, "memset");
}

bool lower_dynamic_memzero_control_flow(mir::MachineFunction &function,
                                        unsigned &next_block_id) {
    bool changed = false;
    for (std::size_t block_index = 0; block_index < function.blocks().size(); ++block_index) {
        auto *block = function.blocks()[block_index].get();
        auto &instructions = block->instructions();
        const auto found = std::find_if(
            instructions.begin(), instructions.end(), [](const mir::MachineInstr &instr) {
                return instr.opcode() == mir::Opcode::MemZero &&
                       instr.operands().size() >= 3 && instr.operands()[2].is_reg();
            });
        if (found == instructions.end()) {
            continue;
        }

        const auto operands = found->operands();
        std::vector<mir::MachineInstr> continuation(
            std::make_move_iterator(found + 1),
            std::make_move_iterator(instructions.end()));
        instructions.erase(found, instructions.end());

        const auto suffix = std::to_string(next_block_id++);
        const auto call_name = "memzero.call." + suffix;
        const auto done_name = "memzero.done." + suffix;
        auto *call_block = function.create_block(call_name);
        auto *done_block = function.create_block(done_name);
        done_block->instructions() = std::move(continuation);

        const auto staged_address = gpr("t4", mir::ValueType::Ptr);
        const auto staged_count = gpr("t5");
        block->add_instr(
            mir::Opcode::Move,
            std::vector<mir::MachineOperand>{
                mir::MachineOperand::reg_def(staged_address),
                mir::MachineOperand::reg_use(operands[0].reg_value())});
        block->add_instr(
            mir::Opcode::Move,
            std::vector<mir::MachineOperand>{
                mir::MachineOperand::reg_def(staged_count),
                mir::MachineOperand::reg_use(operands[2].reg_value())});
        block->add_instr(
            mir::Opcode::BranchGE,
            std::vector<mir::MachineOperand>{
                mir::MachineOperand::reg_use(gpr("zero")),
                mir::MachineOperand::reg_use(staged_count),
                mir::MachineOperand::block(done_name)});
        block->add_instr(mir::Opcode::Jump,
                         std::vector<mir::MachineOperand>{
                             mir::MachineOperand::block(call_name)});

        call_block->add_instr(
            mir::Opcode::Move,
            std::vector<mir::MachineOperand>{
                mir::MachineOperand::reg_def(gpr("a0", mir::ValueType::Ptr)),
                mir::MachineOperand::reg_use(staged_address)});
        call_block->add_instr(
            mir::Opcode::LoadImm,
            std::vector<mir::MachineOperand>{
                mir::MachineOperand::reg_def(gpr("a1")), operands[1]});
        call_block->add_instr(
            mir::Opcode::Move,
            std::vector<mir::MachineOperand>{
                mir::MachineOperand::reg_def(gpr("a2")),
                mir::MachineOperand::reg_use(staged_count)});
        call_block->add_instr(mir::Opcode::Call,
                              std::vector<mir::MachineOperand>{
                                  mir::MachineOperand::symbol("memset")});
        call_block->add_instr(mir::Opcode::Jump,
                              std::vector<mir::MachineOperand>{
                                  mir::MachineOperand::block(done_name)});
        changed = true;
    }
    if (changed) {
        function.rebuild_cfg();
    }
    return changed;
}

bool expand_function(mir::MachineFunction &function) {
    unsigned next_block_id = 0;
    bool changed = lower_dynamic_memzero_control_flow(function, next_block_id);
    unsigned next_label = 0;
    for (auto &block : function.blocks()) {
        std::vector<mir::MachineInstr> rewritten;
        rewritten.reserve(block->instructions().size());
        std::unordered_map<std::string, std::int64_t> known_stack_addresses;
        for (auto &instr : block->instructions()) {
            const auto &ops = instr.operands();
            const auto opcode = instr.opcode();
            const auto &desc = mir::instruction_desc(opcode);

            if (opcode == mir::Opcode::RVVWholeRegSpill ||
                opcode == mir::Opcode::RVVWholeRegReload) {
                const auto slot_operand =
                    opcode == mir::Opcode::RVVWholeRegSpill ? 0U : 1U;
                const auto *slot = function.stack_slot(
                    ops[slot_operand].slot_id());
                if (slot == nullptr) {
                    throw std::runtime_error(
                        "whole-register spill/reload references a missing stack slot");
                }
                const auto address =
                    emit_scalable_slot_address(rewritten, function, *slot);
                const auto info = instr.vector_info();
                if (opcode == mir::Opcode::RVVWholeRegSpill) {
                    rewritten.emplace_back(
                        mir::Opcode::RISCVVWholeRegSpill,
                        std::vector<mir::MachineOperand>{
                            mir::MachineOperand::reg_use(ops[1].reg_value()),
                            mir::MachineOperand::reg_use(address),
                            mir::MachineOperand::implicit_reg_use(
                                mir::Register::physical(
                                    "vlenb", mir::RegisterClass::VSTATE))},
                        info);
                } else {
                    rewritten.emplace_back(
                        mir::Opcode::RISCVVWholeRegReload,
                        std::vector<mir::MachineOperand>{
                            mir::MachineOperand::reg_def(ops[0].reg_value()),
                            mir::MachineOperand::reg_use(address),
                            mir::MachineOperand::implicit_reg_use(
                                mir::Register::physical(
                                    "vlenb", mir::RegisterClass::VSTATE))},
                        info);
                }
                changed = true;
                continue;
            }

            if (opcode == mir::Opcode::RVVMaskStore) {
                const auto info = instr.vector_info();
                const auto address = ops[1].reg_value();
                rewritten.emplace_back(mir::Opcode::RISCVVMaskStore,
                                       std::move(instr.operands()), info);
                if (!info.vector_type.is_fixed()) {
                    throw std::runtime_error(
                        "scalable VSM requires an explicit packed-tail cleanup strategy");
                }
                const auto remainder = info.vector_type.fixed_lanes() % 8U;
                if (remainder != 0U) {
                    const auto last_byte =
                        static_cast<std::int64_t>(info.vector_type.fixed_lanes() / 8U);
                    const auto byte_scratch = gpr("t5", mir::ValueType::I32);
                    auto cleanup_address = address;
                    auto cleanup_offset = last_byte;
                    if (!fits_simm12(last_byte)) {
                        const auto address_scratch = gpr("t6", mir::ValueType::Ptr);
                        emit_load_immediate(rewritten, address_scratch, last_byte);
                        rewritten.emplace_back(
                            mir::Opcode::Add,
                            std::vector<mir::MachineOperand>{
                                mir::MachineOperand::reg_def(address_scratch),
                                mir::MachineOperand::reg_use(address),
                                mir::MachineOperand::reg_use(address_scratch)});
                        cleanup_address = address_scratch;
                        cleanup_offset = 0;
                    }
                    emit_memory_access(rewritten, mir::Opcode::RISCVLBU,
                                       byte_scratch, cleanup_address,
                                       cleanup_offset, true);
                    rewritten.emplace_back(
                        mir::Opcode::AndI,
                        std::vector<mir::MachineOperand>{
                            mir::MachineOperand::reg_def(byte_scratch),
                            mir::MachineOperand::reg_use(byte_scratch),
                            mir::MachineOperand::imm((1U << remainder) - 1U)});
                    emit_memory_access(rewritten, mir::Opcode::RISCVSB,
                                       byte_scratch, cleanup_address,
                                       cleanup_offset, false);
                }
                changed = true;
                continue;
            }

            if (desc.has_flag(mir::MIF_Vector) && desc.has_flag(mir::MIF_Pseudo)) {
                const auto final_opcode = expanded_rvv_opcode(instr);
                auto operands = std::move(instr.operands());
                auto info = instr.vector_info();
                switch (opcode) {
                case mir::Opcode::RVVMaskCopy:
                case mir::Opcode::RVVVectorCopy:
                    // Direct ABI entry copies carry pre-RA-only implicit uses
                    // of physical vector arguments that have not yet been
                    // consumed. They are allocation barriers and have no
                    // encoded final operand.
                    operands.erase(
                        std::remove_if(
                            operands.begin(), operands.end(),
                            [](const mir::MachineOperand &operand) {
                                return operand.is_implicit() && operand.is_reg() &&
                                       operand.reg_value().is_vector();
                            }),
                        operands.end());
                    break;
                case mir::Opcode::RVVIntBinaryVV:
                case mir::Opcode::RVVFloatBinaryVV:
                case mir::Opcode::RVVLoadUnit:
                case mir::Opcode::RVVLoadStrided:
                case mir::Opcode::RVVLoadIndexed:
                case mir::Opcode::RVVCompareVV:
                case mir::Opcode::RVVCompareVX:
                case mir::Opcode::RVVSlideDownVX:
                case mir::Opcode::RVVSlideDownVI: {
                    // The pre-RA pseudo carries an explicit, tied passthrough
                    // so allocation can preserve TU/MU semantics.  RVV encodes
                    // that old value implicitly in vd; Final MIR therefore
                    // removes the redundant tied operand and marks vd as a use
                    // whenever inactive lanes must be preserved.
                    if (!info.passthrough_operand.has_value() ||
                        *info.passthrough_operand != 1 || operands.size() < 2) {
                        throw std::runtime_error(
                            "tied RVV pseudo lacks passthrough operand #1");
                    }
                    operands.erase(operands.begin() + 1);
                    if (info.mask_operand.has_value()) {
                        --*info.mask_operand;
                    }
                    if (info.tail_policy == mir::VectorTailPolicy::Undisturbed ||
                        (info.mask_operand.has_value() &&
                         info.mask_policy == mir::VectorMaskPolicy::Undisturbed)) {
                        operands.front().set_is_use(true);
                    }
                    info.passthrough_operand.reset();
                    break;
                }
                default:
                    break;
                }
                rewritten.emplace_back(final_opcode, std::move(operands), std::move(info));
                changed = true;
                continue;
            }

            switch (opcode) {
            case mir::Opcode::Comment:
                changed = true;
                break;
            case mir::Opcode::LoadImm:
                emit_load_immediate(rewritten, ops[0].reg_value(), ops[1].int_value());
                changed = true;
                break;
            case mir::Opcode::LoadFloatImm:
                emit_load_immediate(rewritten, gpr("t6"), float_bits(ops[1].float_value()));
                rewritten.emplace_back(
                    mir::Opcode::FmvWX,
                    std::vector<mir::MachineOperand>{
                        mir::MachineOperand::reg_def(ops[0].reg_value()),
                        mir::MachineOperand::reg_use(gpr("t6"))});
                changed = true;
                break;
            case mir::Opcode::LoadGlobalAddr:
                emit_symbol_address(rewritten, function, next_label, ops[0].reg_value(),
                                    ops[1].string_value());
                changed = true;
                break;
            case mir::Opcode::LoadStackAddr: {
                const auto *slot = function.stack_slot(ops[1].slot_id());
                const auto offset = slot->offset;
                if (fits_simm12(offset)) {
                    rewritten.emplace_back(
                        mir::Opcode::AddI,
                        std::vector<mir::MachineOperand>{
                            mir::MachineOperand::reg_def(ops[0].reg_value()),
                            mir::MachineOperand::reg_use(gpr("sp", mir::ValueType::Ptr)),
                            mir::MachineOperand::imm(offset)});
                } else {
                    emit_load_immediate(rewritten, gpr("t6", mir::ValueType::Ptr), offset);
                    rewritten.emplace_back(
                        mir::Opcode::Add,
                        std::vector<mir::MachineOperand>{
                            mir::MachineOperand::reg_def(ops[0].reg_value()),
                            mir::MachineOperand::reg_use(gpr("sp", mir::ValueType::Ptr)),
                            mir::MachineOperand::reg_use(gpr("t6", mir::ValueType::Ptr))});
                }
                known_stack_addresses[ops[0].reg_value().name] = offset;
                changed = true;
                break;
            }
            case mir::Opcode::LoadSlot: {
                const auto *slot = function.stack_slot(ops[1].slot_id());
                const bool byte_i1 = ops[2].type_value() != mir::ValueType::I1 ||
                                     slot->type.size == 1;
                emit_memory_access(rewritten, load_opcode(ops[2].type_value(), byte_i1),
                                   ops[0].reg_value(), gpr("sp", mir::ValueType::Ptr),
                                   slot->offset, true);
                changed = true;
                break;
            }
            case mir::Opcode::StoreSlot: {
                const auto *slot = function.stack_slot(ops[0].slot_id());
                const bool byte_i1 = ops[2].type_value() != mir::ValueType::I1 ||
                                     slot->type.size == 1;
                emit_memory_access(rewritten, store_opcode(ops[2].type_value(), byte_i1),
                                   ops[1].reg_value(), gpr("sp", mir::ValueType::Ptr),
                                   slot->offset, false);
                changed = true;
                break;
            }
            case mir::Opcode::LoadMem:
                emit_memory_access(rewritten, load_opcode(ops[2].type_value()),
                                   ops[0].reg_value(), ops[1].reg_value(), 0, true);
                changed = true;
                break;
            case mir::Opcode::StoreMem:
                emit_memory_access(rewritten, store_opcode(ops[2].type_value()),
                                   ops[1].reg_value(), ops[0].reg_value(), 0, false);
                changed = true;
                break;
            case mir::Opcode::LoadMemOffset:
                emit_memory_access(rewritten, load_opcode(ops[3].type_value()),
                                   ops[0].reg_value(), ops[1].reg_value(),
                                   ops[2].int_value(), true);
                changed = true;
                break;
            case mir::Opcode::StoreMemOffset:
                emit_memory_access(rewritten, store_opcode(ops[3].type_value()),
                                   ops[1].reg_value(), ops[0].reg_value(),
                                   ops[2].int_value(), false);
                changed = true;
                break;
            case mir::Opcode::MemZero:
                expand_memzero(
                    instr, function, next_label,
                    known_stack_addresses.find(ops[0].reg_value().name) !=
                            known_stack_addresses.end() &&
                        known_stack_addresses.at(ops[0].reg_value().name) % 8 == 0,
                    rewritten);
                changed = true;
                break;
            case mir::Opcode::Move:
                if (const auto found =
                        known_stack_addresses.find(ops[1].reg_value().name);
                    found != known_stack_addresses.end()) {
                    known_stack_addresses[ops[0].reg_value().name] = found->second;
                } else {
                    known_stack_addresses.erase(ops[0].reg_value().name);
                }
                rewritten.emplace_back(
                    mir::Opcode::AddI,
                    std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(ops[0].reg_value()),
                                                     mir::MachineOperand::reg_use(ops[1].reg_value()),
                                                     mir::MachineOperand::imm(0)});
                changed = true;
                break;
            case mir::Opcode::FMove:
                rewritten.emplace_back(
                    mir::Opcode::RISCVFSGNJS,
                    std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(ops[0].reg_value()),
                                                     mir::MachineOperand::reg_use(ops[1].reg_value()),
                                                     mir::MachineOperand::reg_use(ops[1].reg_value())});
                changed = true;
                break;
            case mir::Opcode::SeqZ:
                rewritten.emplace_back(
                    mir::Opcode::RISCVSLTIU,
                    std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(ops[0].reg_value()),
                                                     mir::MachineOperand::reg_use(ops[1].reg_value()),
                                                     mir::MachineOperand::imm(1)});
                changed = true;
                break;
            case mir::Opcode::Snez:
                rewritten.emplace_back(
                    mir::Opcode::Sltu,
                    std::vector<mir::MachineOperand>{mir::MachineOperand::reg_def(ops[0].reg_value()),
                                                     mir::MachineOperand::reg_use(gpr("zero")),
                                                     mir::MachineOperand::reg_use(ops[1].reg_value())});
                changed = true;
                break;
            case mir::Opcode::StoreOutgoingArg: {
                const auto selected = ops[2].type_value() == mir::ValueType::F32
                                          ? mir::Opcode::RISCVFSW
                                          : mir::Opcode::RISCVSD;
                emit_memory_access(rewritten, selected, ops[0].reg_value(),
                                   gpr("sp", mir::ValueType::Ptr), ops[1].int_value(), false);
                changed = true;
                break;
            }
            case mir::Opcode::LoadIncomingArg: {
                if (!function.scalable_frame_size().is_valid()) {
                    emit_memory_access(
                        rewritten, load_opcode(ops[2].type_value()),
                        ops[0].reg_value(), gpr("sp", mir::ValueType::Ptr),
                        function.frame_size() + ops[1].int_value(), true);
                    changed = true;
                    break;
                }
                const auto address = emit_entry_stack_address(
                    rewritten, function, ops[1].int_value());
                emit_memory_access(rewritten, load_opcode(ops[2].type_value()),
                                   ops[0].reg_value(), address, 0, true);
                changed = true;
                break;
            }
            case mir::Opcode::BranchNonZero:
                rewritten.emplace_back(
                    mir::Opcode::BranchNe,
                    std::vector<mir::MachineOperand>{mir::MachineOperand::reg_use(ops[0].reg_value()),
                                                     mir::MachineOperand::reg_use(gpr("zero")),
                                                     ops[1]});
                changed = true;
                break;
            case mir::Opcode::BranchZero:
                rewritten.emplace_back(
                    mir::Opcode::BranchEq,
                    std::vector<mir::MachineOperand>{mir::MachineOperand::reg_use(ops[0].reg_value()),
                                                     mir::MachineOperand::reg_use(gpr("zero")),
                                                     ops[1]});
                changed = true;
                break;
            case mir::Opcode::Jump:
                rewritten.emplace_back(
                    mir::Opcode::RISCVJAL,
                    std::vector<mir::MachineOperand>{
                        mir::MachineOperand::reg_def(gpr("zero", mir::ValueType::Ptr)), ops[0]});
                changed = true;
                break;
            case mir::Opcode::Call:
                emit_direct_call(rewritten, function, next_label, ops[0].string_value());
                changed = true;
                break;
            default:
                if (desc.has_flag(mir::MIF_Pseudo)) {
                    throw std::runtime_error(std::string("unsupported scalar pseudo expansion: ") +
                                             mir::opcode_name(opcode));
                }
                rewritten.push_back(std::move(instr));
                break;
            }
        }
        block->instructions() = std::move(rewritten);
    }
    function.rebuild_cfg();
    return changed;
}

} // namespace

bool expand_machine_pseudos(mir::Module &module, std::string &error) {
    try {
        const auto before = mir::verify_module(module, mir::MIRVerificationStage::PostRA);
        if (!before.ok) {
            error = before.message;
            return false;
        }
        for (auto &function : module.functions()) {
            if (!function->is_external()) {
                (void)expand_function(*function);
            }
        }
        const auto after = mir::verify_module(module, mir::MIRVerificationStage::Final);
        if (!after.ok) {
            error = after.message;
            return false;
        }
        error.clear();
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

std::string_view MIRPseudoExpansionPass::name() const {
    return "MIRPseudoExpansionPass";
}

PassKind MIRPseudoExpansionPass::kind() const {
    return PassKind::Lowering;
}

PassResult MIRPseudoExpansionPass::run(PassContext &context) {
    auto *module = context.machine_module();
    if (module == nullptr) {
        return PassResult::fail("MIRPseudoExpansionPass requires MIR module in pass context");
    }
    std::string error;
    if (!expand_machine_pseudos(*module, error)) {
        return PassResult::fail(std::move(error));
    }
    return PassResult::ok(true);
}

} // namespace pass
