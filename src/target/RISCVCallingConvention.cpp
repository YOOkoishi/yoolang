#include "target/RISCVCallingConvention.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace target {
namespace {

constexpr CCArgumentRegisterNames kGPRArguments = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
constexpr CCArgumentRegisterNames kFPRArguments = {"fa0", "fa1", "fa2", "fa3",
                                                   "fa4", "fa5", "fa6", "fa7"};
constexpr CCPSABIVectorRegisterConvention kPSABIVectorRegisters = {
    0,
    {8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23},
    {0, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23},
    {1, 2, 3, 4, 5, 6, 7, 24, 25, 26, 27, 28, 29, 30, 31},
    false,
    false,
    false,
    false,
    true,
    true,
};
constexpr std::uint64_t kXLENBytes = 8;
constexpr std::uint64_t kStackAlignment = 16;
constexpr unsigned kFirstVectorArgumentRegister = 8;
constexpr unsigned kLastVectorArgumentRegister = 23;

std::optional<std::uint64_t> checked_align_to(std::uint64_t value, std::uint64_t alignment) {
    if (alignment == 0) {
        return std::nullopt;
    }
    const auto remainder = value % alignment;
    const auto increment = remainder == 0 ? 0 : alignment - remainder;
    if (increment > std::numeric_limits<std::uint64_t>::max() - value) {
        return std::nullopt;
    }
    return value + increment;
}

bool contains_scalable_vector(const oir::Type *type) {
    if (type == nullptr) {
        return false;
    }
    if (const auto *vector = dynamic_cast<const oir::VectorType *>(type)) {
        return vector->element_count().is_scalable();
    }
    if (const auto *array = dynamic_cast<const oir::ArrayType *>(type)) {
        return contains_scalable_vector(array->element_type());
    }
    return false;
}

bool is_aggregate_value(const oir::Type *type) {
    return type != nullptr && (type->is_vector() || type->is_array());
}

struct AssignmentState final {
    std::size_t next_gpr = 0;
    std::size_t next_fpr = 0;
    std::uint64_t stack_offset = 0;
};

bool assign_stack_piece(CCValueAssignment &assignment, AssignmentState &state,
                        std::uint64_t value_offset, std::uint64_t size,
                        std::uint64_t natural_alignment, std::string &error) {
    const auto argument_alignment = std::max<std::uint64_t>(
        kXLENBytes, std::min<std::uint64_t>(natural_alignment, kStackAlignment));
    auto aligned = checked_align_to(state.stack_offset, argument_alignment);
    if (!aligned) {
        error = "calling-convention stack offset overflow";
        return false;
    }
    assignment.locations.push_back({CCLocationKind::Stack, {}, *aligned, value_offset, size});
    if (*aligned > std::numeric_limits<std::uint64_t>::max() - size) {
        error = "calling-convention stack size overflow";
        return false;
    }
    auto next = checked_align_to(*aligned + size, kXLENBytes);
    if (!next) {
        error = "calling-convention stack size overflow";
        return false;
    }
    state.stack_offset = *next;
    return true;
}

bool assign_integer_piece(CCValueAssignment &assignment, AssignmentState &state,
                          std::uint64_t value_offset, std::uint64_t size, std::uint64_t alignment,
                          std::string &error) {
    if (state.next_gpr < kGPRArguments.size()) {
        assignment.locations.push_back(
            {CCLocationKind::GPR, kGPRArguments[state.next_gpr++], 0, value_offset, size});
        return true;
    }
    return assign_stack_piece(assignment, state, value_offset, size, alignment, error);
}

bool assign_parameter(CCValueAssignment &assignment, AssignmentState &state,
                      const oir::DataLayout &layout, bool variadic, std::string &error) {
    const auto *type = assignment.type;
    if (type == nullptr || type->is_void() || type->is_function() || type->is_label()) {
        error = "invalid function parameter type in RISC-V calling convention";
        return false;
    }
    if (contains_scalable_vector(type)) {
        error = "scalable vector is forbidden in the standard ABI and varargs";
        return false;
    }
    if (variadic && is_aggregate_value(type)) {
        error = "fixed vector, mask, and aggregate variadic arguments are not implemented";
        return false;
    }

    const auto size = layout.fixed_alloc_size(type);
    if (!size) {
        error = "function parameter has no fixed ABI size";
        return false;
    }
    assignment.size = *size;
    assignment.alignment = layout.abi_alignment(type);

    // Named scalar float arguments use the hardware FP convention.  Variadic
    // arguments are classified by the integer convention.  Vector<float,N>
    // is an aggregate and must never consume fa registers.
    if (type->is_scalar_float() && !variadic && state.next_fpr < kFPRArguments.size()) {
        assignment.locations.push_back(
            {CCLocationKind::FPR32, kFPRArguments[state.next_fpr++], 0, 0, 4});
        return true;
    }

    if (!is_aggregate_value(type) && *size <= kXLENBytes) {
        return assign_integer_piece(assignment, state, 0, *size, assignment.alignment, error);
    }

    // RISC-V passes aggregates larger than 2*XLEN by reference.  The pointer
    // itself follows the integer argument convention; the caller owns the
    // correctly laid-out temporary copy.
    if (*size > 2 * kXLENBytes) {
        assignment.indirect = true;
        return assign_integer_piece(assignment, state, 0, kXLENBytes, kXLENBytes, error);
    }

    std::uint64_t offset = 0;
    while (offset < *size) {
        const auto piece_size = std::min<std::uint64_t>(kXLENBytes, *size - offset);
        if (!assign_integer_piece(assignment, state, offset, piece_size, assignment.alignment,
                                  error)) {
            return false;
        }
        offset += piece_size;
    }
    return true;
}

bool assign_return(CCSignatureAssignment &result, const oir::Type *type,
                   const oir::DataLayout &layout, AssignmentState &parameter_state) {
    auto &assignment = result.return_value;
    assignment.type = type;
    if (type == nullptr || type->is_void()) {
        return true;
    }
    if (type->is_function() || type->is_label() || contains_scalable_vector(type)) {
        result.valid = false;
        result.error = contains_scalable_vector(type)
                           ? "scalable vector is forbidden in the standard ABI"
                           : "invalid function return type in RISC-V calling convention";
        return false;
    }
    const auto size = layout.fixed_alloc_size(type);
    if (!size) {
        result.valid = false;
        result.error = "function return type has no fixed ABI size";
        return false;
    }
    assignment.size = *size;
    assignment.alignment = layout.abi_alignment(type);

    if (type->is_scalar_float()) {
        assignment.locations.push_back({CCLocationKind::FPR32, "fa0", 0, 0, 4});
        return true;
    }
    if (!is_aggregate_value(type) && *size <= kXLENBytes) {
        assignment.locations.push_back({CCLocationKind::GPR, "a0", 0, 0, *size});
        return true;
    }
    if (*size > 2 * kXLENBytes) {
        result.has_sret = true;
        assignment.indirect = true;
        assignment.locations.push_back({CCLocationKind::GPR, "a0", 0, 0, kXLENBytes});
        // The hidden return pointer consumes the first integer argument slot.
        parameter_state.next_gpr = 1;
        return true;
    }
    assignment.locations.push_back(
        {CCLocationKind::GPR, "a0", 0, 0, std::min<std::uint64_t>(*size, kXLENBytes)});
    if (*size > kXLENBytes) {
        assignment.locations.push_back(
            {CCLocationKind::GPR, "a1", 0, kXLENBytes, *size - kXLENBytes});
    }
    return true;
}

bool is_power_of_two(unsigned value) {
    return value != 0 && (value & (value - 1U)) == 0;
}

std::optional<std::uint64_t> checked_multiply(std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        return std::nullopt;
    }
    return lhs * rhs;
}

struct VectorAssignmentState final {
    std::array<bool, 16> allocated{};
    bool first_mask_allocated = false;
};

struct FixedVectorShape final {
    const oir::VectorType *type = nullptr;
    CCPSABIVectorValueKind kind = CCPSABIVectorValueKind::NonVector;
    std::uint64_t total_size = 0;
    std::uint64_t alignment = 1;
    unsigned lmul = 1;
    unsigned tuple_fields = 1;
    bool requires_indirect = false;
};

bool validate_psabi_vector_profile(const TargetProfile &profile, unsigned abi_vlen_bits,
                                   std::string &error) {
    if (profile.xlen_bits != 64 || profile.mabi != "lp64d") {
        error = "fixed-length psABI vector classifier currently requires RV64 LP64D";
        return false;
    }
    if (!profile.has_vector() || profile.minimum_vlen_bits == 0) {
        error = "fixed-length psABI vector classifier requires a finalized V or Zve target";
        return false;
    }
    if (!is_power_of_two(abi_vlen_bits) || abi_vlen_bits < 32U ||
        abi_vlen_bits > 65536U) {
        error = "ABI_VLEN must be a power of two in [32, 65536]";
        return false;
    }
    if (abi_vlen_bits > profile.minimum_vlen_bits) {
        error = "ABI_VLEN exceeds the VLEN guaranteed by the target profile";
        return false;
    }
    return true;
}

bool classify_fixed_vector_shape(const CCPSABIVectorValue &input,
                                 const TargetProfile &profile,
                                 const oir::DataLayout &layout, unsigned abi_vlen_bits,
                                 FixedVectorShape &shape, std::string &error) {
    if (input.type == nullptr) {
        error = "null value type in psABI vector calling convention";
        return false;
    }
    if (contains_scalable_vector(input.type)) {
        error = "scalable vector is compiler-internal and forbidden in the fixed-length psABI "
                "vector calling convention";
        return false;
    }
    const auto *vector = dynamic_cast<const oir::VectorType *>(input.type);
    if (vector == nullptr) {
        if (input.tuple_fields != 1) {
            error = "vector tuple metadata requires a fixed vector element type";
            return false;
        }
        return true;
    }
    if (!vector->element_count().is_fixed()) {
        error = "scalable vector is compiler-internal and forbidden in the fixed-length psABI "
                "vector calling convention";
        return false;
    }
    if (vector->is_integer_vector() &&
        !profile.supports_vector_element(false, 32)) {
        error = "fixed integer vector signature requires target i32 vector support";
        return false;
    }
    if (vector->is_float_vector() &&
        !profile.supports_vector_element(true, 32)) {
        error = "fixed float vector signature requires target f32 vector support";
        return false;
    }
    if (vector->is_mask() && !profile.supports_vector_element(false, 32)) {
        error = "fixed mask signature requires target integer vector support";
        return false;
    }
    if (input.tuple_fields == 0 || input.tuple_fields > 8) {
        error = "vector tuple NFIELDS must be in [1, 8]";
        return false;
    }
    if (vector->is_mask() && input.tuple_fields != 1) {
        error = "vector mask tuple is not a legal psABI vector tuple type";
        return false;
    }

    const auto base_size = layout.fixed_alloc_size(vector);
    if (!base_size) {
        error = "fixed vector has no fixed ABI size";
        return false;
    }
    const auto total_size = checked_multiply(*base_size, input.tuple_fields);
    if (!total_size) {
        error = "vector tuple ABI size overflow";
        return false;
    }

    shape.type = vector;
    shape.kind = vector->is_mask() ? CCPSABIVectorValueKind::Mask
                                   : (input.tuple_fields == 1
                                          ? CCPSABIVectorValueKind::Data
                                          : CCPSABIVectorValueKind::Tuple);
    shape.total_size = *total_size;
    shape.alignment = layout.abi_alignment(vector);
    shape.tuple_fields = input.tuple_fields;

    if (vector->is_mask()) {
        // v0 and every later mask argument occupy one complete vector
        // register.  A fixed mask wider than ABI_VLEN cannot be represented by
        // that register and therefore follows the by-reference fallback.
        shape.lmul = 1;
        shape.requires_indirect = vector->element_count().min_lanes > abi_vlen_bits;
        return true;
    }

    const auto abi_vlen_bytes = static_cast<std::uint64_t>(abi_vlen_bits / 8U);
    unsigned lmul = 1;
    while (*base_size > abi_vlen_bytes * lmul && lmul < 8U) {
        lmul *= 2U;
    }
    if (*base_size > abi_vlen_bytes * 8U) {
        shape.lmul = 8;
        shape.requires_indirect = true;
        return true;
    }
    shape.lmul = lmul;

    const auto group_width = checked_multiply(lmul, input.tuple_fields);
    if (!group_width || *group_width > 8U) {
        error = "illegal fixed vector tuple: LMUL times NFIELDS exceeds eight registers";
        return false;
    }
    return true;
}

std::optional<CCPSABIVectorRegisterGroup>
allocate_vector_group(VectorAssignmentState &state, unsigned lmul, unsigned tuple_fields) {
    const unsigned register_count = lmul * tuple_fields;
    for (unsigned first = kFirstVectorArgumentRegister;
         first + register_count - 1U <= kLastVectorArgumentRegister; ++first) {
        if (first % lmul != 0) {
            continue;
        }
        bool available = true;
        for (unsigned reg = first; reg < first + register_count; ++reg) {
            available &= !state.allocated[reg - kFirstVectorArgumentRegister];
        }
        if (!available) {
            continue;
        }
        for (unsigned reg = first; reg < first + register_count; ++reg) {
            state.allocated[reg - kFirstVectorArgumentRegister] = true;
        }
        return CCPSABIVectorRegisterGroup{
            static_cast<std::uint8_t>(first), static_cast<std::uint8_t>(register_count),
            static_cast<std::uint8_t>(lmul), static_cast<std::uint8_t>(tuple_fields)};
    }
    return std::nullopt;
}

bool assign_indirect_vector_parameter(CCPSABIVectorValueAssignment &assignment,
                                      AssignmentState &state, std::string &error) {
    assignment.value.indirect = true;
    return assign_integer_piece(assignment.value, state, 0, kXLENBytes, kXLENBytes, error);
}

bool assign_psabi_vector_parameter(CCPSABIVectorValueAssignment &assignment,
                                   const CCPSABIVectorValue &input,
                                   const TargetProfile &profile,
                                   const oir::DataLayout &layout, unsigned abi_vlen_bits,
                                   AssignmentState &scalar_state,
                                   VectorAssignmentState &vector_state, bool variadic,
                                   std::string &error) {
    assignment.value.type = input.type;
    assignment.tuple_fields = input.tuple_fields;

    FixedVectorShape shape;
    if (!classify_fixed_vector_shape(input, profile, layout, abi_vlen_bits, shape,
                                     error)) {
        return false;
    }
    if (shape.type == nullptr) {
        assignment.kind = CCPSABIVectorValueKind::NonVector;
        return assign_parameter(assignment.value, scalar_state, layout, variadic, error);
    }

    assignment.kind = shape.kind;
    assignment.lmul = shape.lmul;
    assignment.value.size = shape.total_size;
    assignment.value.alignment = shape.alignment;
    const bool is_first_mask = shape.kind == CCPSABIVectorValueKind::Mask &&
                               !vector_state.first_mask_allocated;
    if (shape.kind == CCPSABIVectorValueKind::Mask) {
        // "First mask" is a property of argument order, even when an
        // oversized fixed mask has to fall back to an indirect pointer.
        vector_state.first_mask_allocated = true;
    }
    if (variadic) {
        error = "variadic vector, mask, and tuple arguments are rejected until their required "
                "by-reference call lowering is implemented";
        return false;
    }
    if (shape.requires_indirect) {
        return assign_indirect_vector_parameter(assignment, scalar_state, error);
    }

    if (is_first_mask) {
        assignment.vector_group = CCPSABIVectorRegisterGroup{0, 1, 1, 1};
        return true;
    }

    assignment.vector_group =
        allocate_vector_group(vector_state, shape.lmul, shape.tuple_fields);
    if (assignment.vector_group) {
        return true;
    }
    return assign_indirect_vector_parameter(assignment, scalar_state, error);
}

bool assign_psabi_vector_return(CCPSABIVectorSignatureAssignment &result,
                                const CCPSABIVectorValue &input,
                                const TargetProfile &profile,
                                const oir::DataLayout &layout, unsigned abi_vlen_bits,
                                AssignmentState &parameter_state) {
    auto &assignment = result.return_value;
    assignment.value.type = input.type;
    assignment.tuple_fields = input.tuple_fields;
    if (input.type == nullptr || input.type->is_void()) {
        if (input.tuple_fields != 1) {
            result.valid = false;
            result.error = "void return cannot carry vector tuple metadata";
            return false;
        }
        return true;
    }

    FixedVectorShape shape;
    if (!classify_fixed_vector_shape(input, profile, layout, abi_vlen_bits, shape,
                                     result.error)) {
        result.valid = false;
        return false;
    }
    if (shape.type == nullptr) {
        CCSignatureAssignment standard_result;
        if (!assign_return(standard_result, input.type, layout, parameter_state)) {
            result.valid = false;
            result.error = std::move(standard_result.error);
            return false;
        }
        result.has_sret = standard_result.has_sret;
        assignment.value = std::move(standard_result.return_value);
        return true;
    }

    assignment.kind = shape.kind;
    assignment.lmul = shape.lmul;
    assignment.value.size = shape.total_size;
    assignment.value.alignment = shape.alignment;
    if (shape.requires_indirect) {
        result.has_sret = true;
        assignment.value.indirect = true;
        assignment.value.locations.push_back(
            {CCLocationKind::GPR, "a0", 0, 0, kXLENBytes});
        parameter_state.next_gpr = 1;
        return true;
    }

    if (shape.kind == CCPSABIVectorValueKind::Mask) {
        assignment.vector_group = CCPSABIVectorRegisterGroup{0, 1, 1, 1};
        return true;
    }
    VectorAssignmentState return_state;
    assignment.vector_group =
        allocate_vector_group(return_state, shape.lmul, shape.tuple_fields);
    if (!assignment.vector_group) {
        result.valid = false;
        result.error = "legal vector return type did not fit v8-v23";
        return false;
    }
    return true;
}

bool contains_vector_cc_value(const CCPSABIVectorValue &value) {
    return value.type != nullptr && value.type->is_vector();
}

bool validate_prototype(const CCPSABIVectorFunctionType &function_type, std::string &error) {
    if (function_type.prototyped) {
        return true;
    }
    if (contains_vector_cc_value(function_type.return_value) ||
        std::any_of(function_type.parameters.begin(), function_type.parameters.end(),
                    contains_vector_cc_value)) {
        error = "vector arguments and return values are forbidden for unprototyped functions";
        return false;
    }
    return true;
}

} // namespace

const CCArgumentRegisterNames &riscv_gpr_argument_registers() {
    return kGPRArguments;
}

const CCArgumentRegisterNames &riscv_fpr_argument_registers() {
    return kFPRArguments;
}

const CCPSABIVectorRegisterConvention &riscv_psabi_vector_register_convention() {
    return kPSABIVectorRegisters;
}

RISCVCallingConvention::RISCVCallingConvention(TargetProfile profile, oir::DataLayout data_layout)
    : profile_(std::move(profile)), data_layout_(std::move(data_layout)) {
}

const TargetProfile &RISCVCallingConvention::profile() const {
    return profile_;
}

const oir::DataLayout &RISCVCallingConvention::data_layout() const {
    return data_layout_;
}

CCSignatureAssignment RISCVCallingConvention::assign(const oir::FunctionType &function_type) const {
    CCSignatureAssignment result;
    if (profile_.vector_abi != VectorABI::Standard) {
        result.valid = false;
        result.error = "RISCVCallingConvention implements only the standard aggregate ABI; "
                       "use RISCVPSABIVectorCallingConvention for psabi-vector";
        return result;
    }

    AssignmentState state;
    if (!assign_return(result, function_type.return_type(), data_layout_, state)) {
        return result;
    }

    const auto &parameters = function_type.param_types();
    result.parameters.reserve(parameters.size());
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        CCValueAssignment assignment;
        assignment.type = parameters[index];
        assignment.source_index = index;
        if (!assign_parameter(assignment, state, data_layout_, false, result.error)) {
            result.valid = false;
            return result;
        }
        result.parameters.push_back(std::move(assignment));
    }
    const auto stack_size = checked_align_to(state.stack_offset, kStackAlignment);
    if (!stack_size) {
        result.valid = false;
        result.error = "calling-convention stack size overflow";
        return result;
    }
    result.stack_argument_size = *stack_size;
    return result;
}

CCSignatureAssignment
RISCVCallingConvention::assign_call(const oir::FunctionType &function_type,
                                    const std::vector<oir::Type *> &actual_types) const {
    CCSignatureAssignment result;
    if (profile_.vector_abi != VectorABI::Standard) {
        result.valid = false;
        result.error = "RISCVCallingConvention implements only the standard aggregate ABI; "
                       "use RISCVPSABIVectorCallingConvention for psabi-vector";
        return result;
    }

    const auto &fixed_types = function_type.param_types();
    if ((!function_type.is_variadic() && actual_types.size() != fixed_types.size()) ||
        (function_type.is_variadic() && actual_types.size() < fixed_types.size())) {
        result.valid = false;
        result.error = "call argument count does not match the function type";
        return result;
    }

    AssignmentState state;
    if (!assign_return(result, function_type.return_type(), data_layout_, state)) {
        return result;
    }

    result.parameters.reserve(actual_types.size());
    for (std::size_t index = 0; index < actual_types.size(); ++index) {
        if (index < fixed_types.size() && actual_types[index] != fixed_types[index]) {
            result.valid = false;
            result.error = "call argument type does not match the fixed parameter type";
            return result;
        }
        CCValueAssignment assignment;
        assignment.type = actual_types[index];
        assignment.source_index = index;
        const bool variadic_tail = index >= fixed_types.size();
        if (!assign_parameter(assignment, state, data_layout_, variadic_tail, result.error)) {
            result.valid = false;
            return result;
        }
        result.parameters.push_back(std::move(assignment));
    }

    const auto stack_size = checked_align_to(state.stack_offset, kStackAlignment);
    if (!stack_size) {
        result.valid = false;
        result.error = "calling-convention stack size overflow";
        return result;
    }
    result.stack_argument_size = *stack_size;
    return result;
}

RISCVPSABIVectorCallingConvention::RISCVPSABIVectorCallingConvention(
    TargetProfile profile, unsigned abi_vlen_bits, oir::DataLayout data_layout)
    : profile_(std::move(profile)), abi_vlen_bits_(abi_vlen_bits),
      data_layout_(std::move(data_layout)) {
}

const TargetProfile &RISCVPSABIVectorCallingConvention::profile() const {
    return profile_;
}

unsigned RISCVPSABIVectorCallingConvention::abi_vlen_bits() const {
    return abi_vlen_bits_;
}

const oir::DataLayout &RISCVPSABIVectorCallingConvention::data_layout() const {
    return data_layout_;
}

const CCPSABIVectorRegisterConvention &
RISCVPSABIVectorCallingConvention::register_convention() const {
    return riscv_psabi_vector_register_convention();
}

CCPSABIVectorSignatureAssignment RISCVPSABIVectorCallingConvention::assign(
    const CCPSABIVectorFunctionType &function_type) const {
    CCPSABIVectorSignatureAssignment result;
    if (!validate_psabi_vector_profile(profile_, abi_vlen_bits_, result.error) ||
        !validate_prototype(function_type, result.error)) {
        result.valid = false;
        return result;
    }

    AssignmentState scalar_state;
    if (!assign_psabi_vector_return(result, function_type.return_value, profile_, data_layout_,
                                    abi_vlen_bits_, scalar_state)) {
        return result;
    }

    VectorAssignmentState vector_state;
    result.parameters.reserve(function_type.parameters.size());
    for (std::size_t index = 0; index < function_type.parameters.size(); ++index) {
        CCPSABIVectorValueAssignment assignment;
        assignment.value.source_index = index;
        if (!assign_psabi_vector_parameter(assignment, function_type.parameters[index], profile_,
                                           data_layout_, abi_vlen_bits_, scalar_state,
                                           vector_state, false, result.error)) {
            result.valid = false;
            return result;
        }
        result.parameters.push_back(std::move(assignment));
    }

    const auto stack_size = checked_align_to(scalar_state.stack_offset, kStackAlignment);
    if (!stack_size) {
        result.valid = false;
        result.error = "calling-convention stack size overflow";
        return result;
    }
    result.stack_argument_size = *stack_size;
    return result;
}

CCPSABIVectorSignatureAssignment RISCVPSABIVectorCallingConvention::assign_call(
    const CCPSABIVectorFunctionType &function_type,
    const std::vector<CCPSABIVectorValue> &actual_values) const {
    CCPSABIVectorSignatureAssignment result;
    if (!validate_psabi_vector_profile(profile_, abi_vlen_bits_, result.error) ||
        !validate_prototype(function_type, result.error)) {
        result.valid = false;
        return result;
    }
    if ((!function_type.variadic && actual_values.size() != function_type.parameters.size()) ||
        (function_type.variadic && actual_values.size() < function_type.parameters.size())) {
        result.valid = false;
        result.error = "call argument count does not match the vector function type";
        return result;
    }

    AssignmentState scalar_state;
    if (!assign_psabi_vector_return(result, function_type.return_value, profile_, data_layout_,
                                    abi_vlen_bits_, scalar_state)) {
        return result;
    }

    VectorAssignmentState vector_state;
    result.parameters.reserve(actual_values.size());
    for (std::size_t index = 0; index < actual_values.size(); ++index) {
        if (index < function_type.parameters.size() &&
            (actual_values[index].type != function_type.parameters[index].type ||
             actual_values[index].tuple_fields !=
                 function_type.parameters[index].tuple_fields)) {
            result.valid = false;
            result.error = "call argument type or tuple shape does not match the fixed parameter";
            return result;
        }
        CCPSABIVectorValueAssignment assignment;
        assignment.value.source_index = index;
        const bool variadic_tail = index >= function_type.parameters.size();
        if (!assign_psabi_vector_parameter(assignment, actual_values[index], profile_,
                                           data_layout_, abi_vlen_bits_, scalar_state, vector_state,
                                           variadic_tail, result.error)) {
            result.valid = false;
            return result;
        }
        result.parameters.push_back(std::move(assignment));
    }

    const auto stack_size = checked_align_to(scalar_state.stack_offset, kStackAlignment);
    if (!stack_size) {
        result.valid = false;
        result.error = "calling-convention stack size overflow";
        return result;
    }
    result.stack_argument_size = *stack_size;
    return result;
}

} // namespace target
