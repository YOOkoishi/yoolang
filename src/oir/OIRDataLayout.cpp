#include "oir/OIRDataLayout.h"

#include <limits>
#include <stdexcept>

namespace oir {
namespace {

std::uint64_t checked_add(std::uint64_t lhs, std::uint64_t rhs) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw std::overflow_error("OIR type layout size overflow");
    }
    return lhs + rhs;
}

std::uint64_t checked_multiply(std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        throw std::overflow_error("OIR type layout size overflow");
    }
    return lhs * rhs;
}

std::uint64_t divide_round_up(std::uint64_t value, std::uint64_t divisor) {
    if (divisor == 0) {
        throw std::invalid_argument("OIR type layout divisor must be nonzero");
    }
    return value / divisor + (value % divisor != 0 ? 1U : 0U);
}

std::uint64_t align_to(std::uint64_t value, std::uint64_t alignment) {
    if (alignment == 0) {
        throw std::invalid_argument("OIR type alignment must be nonzero");
    }
    const auto remainder = value % alignment;
    return remainder == 0 ? value : checked_add(value, alignment - remainder);
}

TypeSize scaled_by(TypeSize size, std::uint64_t count) {
    const auto minimum = checked_multiply(size.minimum_bytes(), count);
    return size.is_scalable() ? TypeSize::scalable(minimum) : TypeSize::fixed(minimum);
}

TypeSize aligned_to(TypeSize size, std::uint64_t alignment) {
    const auto minimum = align_to(size.minimum_bytes(), alignment);
    return size.is_scalable() ? TypeSize::scalable(minimum) : TypeSize::fixed(minimum);
}

} // namespace

TypeSize::TypeSize(std::uint64_t minimum_bytes, bool scalable)
    : minimum_bytes_(minimum_bytes), scalable_(scalable) {
}

TypeSize TypeSize::fixed(std::uint64_t bytes) {
    return TypeSize(bytes, false);
}

TypeSize TypeSize::scalable(std::uint64_t minimum_bytes) {
    return TypeSize(minimum_bytes, true);
}

std::uint64_t TypeSize::minimum_bytes() const {
    return minimum_bytes_;
}

bool TypeSize::is_fixed() const {
    return !scalable_;
}

bool TypeSize::is_scalable() const {
    return scalable_;
}

std::optional<std::uint64_t> TypeSize::fixed_bytes() const {
    return scalable_ ? std::nullopt : std::optional<std::uint64_t>(minimum_bytes_);
}

bool TypeSize::operator==(const TypeSize &other) const {
    return minimum_bytes_ == other.minimum_bytes_ && scalable_ == other.scalable_;
}

bool TypeSize::operator!=(const TypeSize &other) const {
    return !(*this == other);
}

DataLayout::DataLayout(std::uint64_t pointer_size_bytes, std::uint64_t pointer_abi_alignment)
    : pointer_size_bytes_(pointer_size_bytes), pointer_abi_alignment_(pointer_abi_alignment) {
    if (pointer_size_bytes_ == 0 || pointer_abi_alignment_ == 0) {
        throw std::invalid_argument("OIR pointer size and alignment must be nonzero");
    }
}

std::uint64_t DataLayout::pointer_size_bytes() const {
    return pointer_size_bytes_;
}

std::uint64_t DataLayout::pointer_abi_alignment() const {
    return pointer_abi_alignment_;
}

bool DataLayout::is_sized(const Type *type) const {
    if (type == nullptr || type->is_void() || type->is_label() || type->is_function()) {
        return false;
    }
    if (const auto *array = dynamic_cast<const ArrayType *>(type)) {
        return is_sized(array->element_type());
    }
    return type->is_integer() || type->is_float() || type->is_pointer() || type->is_vector();
}

TypeSize DataLayout::store_size(const Type *type) const {
    if (!is_sized(type)) {
        throw std::invalid_argument("cannot query storage size of unsized OIR type");
    }

    if (const auto *integer = dynamic_cast<const IntegerType *>(type)) {
        // OIR booleans have an addressable one-byte memory representation.
        return TypeSize::fixed(divide_round_up(integer->bit_width(), 8));
    }
    if (type->is_float()) {
        return TypeSize::fixed(4);
    }
    if (type->is_pointer()) {
        return TypeSize::fixed(pointer_size_bytes_);
    }
    if (const auto *array = dynamic_cast<const ArrayType *>(type)) {
        return scaled_by(alloc_size(array->element_type()), array->element_count());
    }
    if (const auto *vector = dynamic_cast<const VectorType *>(type)) {
        const auto lanes = vector->element_count().min_lanes;
        std::uint64_t minimum_bytes = 0;
        if (vector->is_mask()) {
            minimum_bytes = divide_round_up(lanes, 8);
        } else {
            const auto element_size = fixed_store_size(vector->element_type());
            if (!element_size.has_value()) {
                throw std::invalid_argument("OIR vector element type has no fixed storage size");
            }
            minimum_bytes = checked_multiply(lanes, *element_size);
        }
        return vector->element_count().is_scalable() ? TypeSize::scalable(minimum_bytes)
                                                     : TypeSize::fixed(minimum_bytes);
    }

    throw std::invalid_argument("unsupported sized OIR type");
}

TypeSize DataLayout::alloc_size(const Type *type) const {
    return aligned_to(store_size(type), abi_alignment(type));
}

std::uint64_t DataLayout::abi_alignment(const Type *type) const {
    if (!is_sized(type)) {
        throw std::invalid_argument("cannot query alignment of unsized OIR type");
    }
    if (const auto *integer = dynamic_cast<const IntegerType *>(type)) {
        return integer->bit_width() == 1 ? 1 : 4;
    }
    if (type->is_float()) {
        return 4;
    }
    if (type->is_pointer()) {
        return pointer_abi_alignment_;
    }
    if (const auto *array = dynamic_cast<const ArrayType *>(type)) {
        return abi_alignment(array->element_type());
    }
    if (const auto *vector = dynamic_cast<const VectorType *>(type)) {
        // Masks use their packed byte layout; numeric vectors retain natural
        // element alignment until a target ABI deliberately overrides it.
        return vector->is_mask() ? 1 : abi_alignment(vector->element_type());
    }
    throw std::invalid_argument("unsupported sized OIR type");
}

std::uint64_t DataLayout::preferred_alignment(const Type *type) const {
    // Preferred alignment is deliberately a separate query even though the
    // current target-independent OIR policy does not over-align objects.
    // Backend-local stack placement may raise it later without changing ABI
    // array stride or cross-module object layout.
    return abi_alignment(type);
}

TypeSize DataLayout::array_stride(const ArrayType *type) const {
    if (type == nullptr) {
        throw std::invalid_argument("cannot query stride of a null OIR array type");
    }
    return alloc_size(type->element_type());
}

std::optional<std::uint64_t> DataLayout::fixed_store_size(const Type *type) const {
    if (!is_sized(type)) {
        return std::nullopt;
    }
    return store_size(type).fixed_bytes();
}

std::optional<std::uint64_t> DataLayout::fixed_alloc_size(const Type *type) const {
    if (!is_sized(type)) {
        return std::nullopt;
    }
    return alloc_size(type).fixed_bytes();
}

std::optional<std::uint64_t> DataLayout::fixed_array_stride(const ArrayType *type) const {
    if (type == nullptr || !is_sized(type->element_type())) {
        return std::nullopt;
    }
    return array_stride(type).fixed_bytes();
}

std::optional<std::vector<std::uint64_t>>
DataLayout::fixed_gep_index_strides(const PointerType *base_pointer,
                                    std::size_t index_count) const {
    if (base_pointer == nullptr) {
        return std::nullopt;
    }
    const Type *cursor = base_pointer->element_type();
    std::vector<std::uint64_t> strides;
    strides.reserve(index_count);
    for (std::size_t index = 0; index < index_count; ++index) {
        std::optional<std::uint64_t> stride;
        if (index == 0) {
            stride = fixed_alloc_size(cursor);
        } else if (const auto *array = dynamic_cast<const ArrayType *>(cursor)) {
            stride = fixed_array_stride(array);
            cursor = array->element_type();
        } else {
            stride = fixed_alloc_size(cursor);
        }
        if (!stride.has_value() || *stride == 0) {
            return std::nullopt;
        }
        strides.push_back(*stride);
    }
    return strides;
}

std::optional<std::int64_t>
DataLayout::fixed_gep_offset(const PointerType *base_pointer,
                             const std::vector<std::int64_t> &indices) const {
    const auto strides = fixed_gep_index_strides(base_pointer, indices.size());
    if (!strides.has_value()) {
        return std::nullopt;
    }

    std::int64_t offset = 0;
    for (std::size_t index = 0; index < indices.size(); ++index) {
        const auto subscript = indices[index];
        const auto stride = (*strides)[index];
        std::int64_t term = 0;
        if (subscript != 0) {
            const auto max = std::numeric_limits<std::int64_t>::max();
            const auto min = std::numeric_limits<std::int64_t>::min();
            if (stride > static_cast<std::uint64_t>(max)) {
                if (subscript == -1 && stride == static_cast<std::uint64_t>(max) + 1U) {
                    term = min;
                } else {
                    return std::nullopt;
                }
            } else {
                const auto signed_stride = static_cast<std::int64_t>(stride);
                if ((subscript > 0 && subscript > max / signed_stride) ||
                    (subscript < 0 && subscript < min / signed_stride)) {
                    return std::nullopt;
                }
                term = subscript * signed_stride;
            }
        }
        if ((term > 0 && offset > std::numeric_limits<std::int64_t>::max() - term) ||
            (term < 0 && offset < std::numeric_limits<std::int64_t>::min() - term)) {
            return std::nullopt;
        }
        offset += term;
    }
    return offset;
}

} // namespace oir
