#include "pass/oir/OIRToMIRCommon.h"
#include "oir/OIRDataLayout.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace pass::oir_to_mir {
namespace {

std::size_t checked_size(std::uint64_t size) {
    if (size > std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error("OIR global initializer is too large for the host");
    }
    return static_cast<std::size_t>(size);
}

std::size_t fixed_alloc_size(const oir::DataLayout &layout, const oir::Type *type) {
    const auto size = layout.fixed_alloc_size(type);
    if (!size.has_value()) {
        throw std::runtime_error("global initializer requires a fixed-size OIR type");
    }
    return checked_size(*size);
}

std::vector<std::uint8_t> encode_constant(const oir::Constant &constant,
                                          const oir::DataLayout &layout) {
    const auto total_size = fixed_alloc_size(layout, constant.type());
    if (dynamic_cast<const oir::ConstantAggregateZero *>(&constant) != nullptr) {
        return std::vector<std::uint8_t>(total_size, 0);
    }
    if (const auto *integer = dynamic_cast<const oir::ConstantInt *>(&constant)) {
        const auto *integer_type = dynamic_cast<const oir::IntegerType *>(constant.type());
        if (integer_type == nullptr ||
            (integer_type->bit_width() != 1 && integer_type->bit_width() != 32)) {
            throw std::runtime_error("unsupported integer global initializer type");
        }
        std::vector<std::uint8_t> bytes(total_size, 0);
        const auto bits = static_cast<std::uint64_t>(integer->value());
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::uint8_t>((bits >> (index * 8U)) & 0xffU);
        }
        return bytes;
    }
    if (const auto *floating = dynamic_cast<const oir::ConstantFloat *>(&constant)) {
        if (total_size != sizeof(float)) {
            throw std::runtime_error("unsupported floating global initializer size");
        }
        std::uint32_t bits = 0;
        const float value = floating->value();
        static_assert(sizeof(bits) == sizeof(value), "OIR f32 must be 32-bit");
        std::memcpy(&bits, &value, sizeof(bits));
        std::vector<std::uint8_t> bytes(total_size, 0);
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::uint8_t>((bits >> (index * 8U)) & 0xffU);
        }
        return bytes;
    }
    if (const auto *array = dynamic_cast<const oir::ConstantArray *>(&constant)) {
        const auto stride = fixed_alloc_size(layout, array->array_type()->element_type());
        std::vector<std::uint8_t> bytes(total_size, 0);
        for (std::size_t index = 0; index < array->elements().size(); ++index) {
            auto element = encode_constant(*array->elements()[index], layout);
            if (stride == 0 || stride > bytes.size() || element.size() != stride ||
                index > (bytes.size() - stride) / stride) {
                throw std::runtime_error("OIR array initializer layout mismatch");
            }
            std::copy(element.begin(), element.end(), bytes.begin() + index * stride);
        }
        return bytes;
    }
    if (const auto *vector = dynamic_cast<const oir::ConstantVector *>(&constant)) {
        const auto stride = fixed_alloc_size(layout, vector->vector_type()->element_type());
        std::vector<std::uint8_t> bytes(total_size, 0);
        for (std::size_t index = 0; index < vector->elements().size(); ++index) {
            auto lane = encode_constant(*vector->elements()[index], layout);
            if (stride == 0 || stride > bytes.size() || lane.size() != stride ||
                index > (bytes.size() - stride) / stride) {
                throw std::runtime_error("OIR vector initializer layout mismatch");
            }
            std::copy(lane.begin(), lane.end(), bytes.begin() + index * stride);
        }
        return bytes;
    }
    if (const auto *mask = dynamic_cast<const oir::ConstantMask *>(&constant)) {
        std::vector<std::uint8_t> bytes(total_size, 0);
        if (mask->packed_bits().size() > bytes.size()) {
            throw std::runtime_error("OIR mask initializer layout mismatch");
        }
        std::copy(mask->packed_bits().begin(), mask->packed_bits().end(), bytes.begin());
        return bytes;
    }
    throw std::runtime_error("unsupported typed OIR global initializer");
}

} // namespace

bool is_power_of_two(std::uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

unsigned log2_u64(std::uint64_t value) {
    unsigned out = 0;
    while (value > 1) {
        value >>= 1;
        ++out;
    }
    return out;
}

std::string edge_key(const oir::BasicBlock *pred, const oir::BasicBlock *succ) {
    std::ostringstream oss;
    oss << reinterpret_cast<std::uintptr_t>(pred) << ":" << reinterpret_cast<std::uintptr_t>(succ);
    return oss.str();
}

std::vector<std::uint8_t> lower_global_initializer(const oir::GlobalVariable &global) {
    const auto *initializer = global.initializer();
    if (initializer == nullptr) {
        return {};
    }
    if (initializer->type() != global.value_type()) {
        throw std::runtime_error("OIR global initializer type does not match its object type");
    }

    const oir::DataLayout layout;
    auto bytes = encode_constant(*initializer, layout);
    const auto expected_size = fixed_alloc_size(layout, global.value_type());
    if (bytes.size() != expected_size) {
        throw std::runtime_error("OIR global initializer byte count does not match its type");
    }
    if (std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0; })) {
        bytes.clear();
    }
    return bytes;
}

mir::Register phys_gpr(const std::string &name) {
    return mir::Register::physical(name, mir::RegisterClass::GPR);
}

mir::Register phys_fpr(const std::string &name) {
    return mir::Register::physical(name, mir::RegisterClass::FPR32);
}

} // namespace pass::oir_to_mir
