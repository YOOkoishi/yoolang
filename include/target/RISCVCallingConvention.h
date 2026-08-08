#pragma once

#include "oir/OIR.h"
#include "oir/OIRDataLayout.h"
#include "target/TargetMachine.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace target {

using CCArgumentRegisterNames = std::array<const char *, 8>;

const CCArgumentRegisterNames &riscv_gpr_argument_registers();
const CCArgumentRegisterNames &riscv_fpr_argument_registers();

enum class CCLocationKind : std::uint8_t {
    GPR,
    FPR32,
    Stack,
};

struct CCLocation final {
    CCLocationKind kind = CCLocationKind::Stack;
    std::string register_name;
    std::uint64_t stack_offset = 0;
    std::uint64_t value_offset = 0;
    std::uint64_t size = 0;
};

struct CCValueAssignment final {
    const oir::Type *type = nullptr;
    std::size_t source_index = 0;
    bool indirect = false;
    std::uint64_t size = 0;
    std::uint64_t alignment = 1;
    std::vector<CCLocation> locations;
};

struct CCSignatureAssignment final {
    bool valid = true;
    std::string error;
    bool has_sret = false;
    CCValueAssignment return_value;
    std::vector<CCValueAssignment> parameters;
    std::uint64_t stack_argument_size = 0;
};

// Shared LP64D standard-ABI classifier for function entry, calls and returns.
// Source fixed vectors and masks are aggregates in this convention; scalable
// vectors are never legal in a public interface.  The opt-in vector convention
// is classified separately by RISCVPSABIVectorCallingConvention below.
class RISCVCallingConvention final {
  public:
    explicit RISCVCallingConvention(TargetProfile profile,
                                    oir::DataLayout data_layout = oir::DataLayout{});

    const TargetProfile &profile() const;
    const oir::DataLayout &data_layout() const;

    // Classify a function entry/return. Only the declared parameter prefix is
    // materialized at an entry point, so every parameter here uses the named
    // argument rules even when the function itself is variadic.
    CCSignatureAssignment assign(const oir::FunctionType &function_type) const;

    // Classify one concrete call site. Fixed parameters use the ordinary
    // LP64D named-argument rules; only actual arguments beyond the declared
    // prefix use the integer variadic convention.
    CCSignatureAssignment assign_call(const oir::FunctionType &function_type,
                                      const std::vector<oir::Type *> &actual_types) const;

  private:
    TargetProfile profile_;
    oir::DataLayout data_layout_;
};

// Register convention metadata for the opt-in RISC-V vector calling-
// convention variant.  This remains independent from the standard aggregate
// classifier so the two public ABIs cannot be mixed accidentally.
struct CCPSABIVectorRegisterConvention final {
    std::uint8_t first_mask_argument_register = 0;
    std::array<std::uint8_t, 16> data_argument_registers{};
    std::array<std::uint8_t, 17> call_clobbered_vector_registers{};
    std::array<std::uint8_t, 15> callee_saved_vector_registers{};

    bool preserves_vl = false;
    bool preserves_vtype = false;
    bool preserves_vxrm = false;
    bool preserves_vxsat = false;
    bool requires_vstart_zero_on_entry = true;
    bool requires_vstart_zero_after_call = true;
};

const CCPSABIVectorRegisterConvention &riscv_psabi_vector_register_convention();

enum class CCPSABIVectorValueKind : std::uint8_t {
    NonVector,
    Mask,
    Data,
    Tuple,
};

// A tuple value consists of tuple_fields consecutive register groups, all
// carrying the same fixed vector type.  tuple_fields == 1 denotes an ordinary
// scalar, aggregate, vector or mask value.  This explicit descriptor is used
// because OIR intentionally has no public source-level scalable/tuple type.
struct CCPSABIVectorValue final {
    const oir::Type *type = nullptr;
    unsigned tuple_fields = 1;
};

struct CCPSABIVectorFunctionType final {
    CCPSABIVectorValue return_value;
    std::vector<CCPSABIVectorValue> parameters;
    bool variadic = false;
    bool prototyped = true;
};

struct CCPSABIVectorRegisterGroup final {
    std::uint8_t first_register = 0;
    std::uint8_t register_count = 0;
    std::uint8_t lmul = 1;
    std::uint8_t tuple_fields = 1;
};

struct CCPSABIVectorValueAssignment final {
    // Scalar locations, or the pointer location when indirect is true.  A
    // direct vector-register assignment leaves value.locations empty.
    CCValueAssignment value;
    CCPSABIVectorValueKind kind = CCPSABIVectorValueKind::NonVector;
    unsigned lmul = 1;
    unsigned tuple_fields = 1;
    std::optional<CCPSABIVectorRegisterGroup> vector_group;
};

struct CCPSABIVectorSignatureAssignment final {
    bool valid = true;
    std::string error;
    bool has_sret = false;
    CCPSABIVectorValueAssignment return_value;
    std::vector<CCPSABIVectorValueAssignment> parameters;
    std::uint64_t stack_argument_size = 0;
};

// Classifier for the standard fixed-length vector calling-convention variant.
// ABI_VLEN is explicit and must be guaranteed by the target profile.  Compiler
// integration consumes the result for entry/call/return lowering, while this
// class remains independently testable for tuple and variadic fail-closed rules
// that source OIR does not expose.
class RISCVPSABIVectorCallingConvention final {
  public:
    RISCVPSABIVectorCallingConvention(TargetProfile profile, unsigned abi_vlen_bits,
                                      oir::DataLayout data_layout = oir::DataLayout{});

    const TargetProfile &profile() const;
    unsigned abi_vlen_bits() const;
    const oir::DataLayout &data_layout() const;
    const CCPSABIVectorRegisterConvention &register_convention() const;

    CCPSABIVectorSignatureAssignment
    assign(const CCPSABIVectorFunctionType &function_type) const;

    CCPSABIVectorSignatureAssignment
    assign_call(const CCPSABIVectorFunctionType &function_type,
                const std::vector<CCPSABIVectorValue> &actual_values) const;

  private:
    TargetProfile profile_;
    unsigned abi_vlen_bits_ = 0;
    oir::DataLayout data_layout_;
};

} // namespace target
