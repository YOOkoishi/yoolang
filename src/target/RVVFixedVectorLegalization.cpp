#include "target/RVVFixedVectorLegalization.h"

#include <array>
#include <cstddef>
#include <limits>

namespace target {
namespace {

constexpr std::array<RVVFixedLMUL, 5> kSupportedLMULs = {
    RVVFixedLMUL::MF2,
    RVVFixedLMUL::M1,
    RVVFixedLMUL::M2,
    RVVFixedLMUL::M4,
    RVVFixedLMUL::M8,
};

// A plan is an in-memory lowering artifact, so bound its number of records.
// This still represents at least 33,554,432 e32 lanes at minimum VLEN=128.
constexpr std::uint64_t kMaximumPieceCount = 1U << 20U;

bool is_power_of_two(unsigned value) {
    return value != 0 && (value & (value - 1U)) == 0;
}

bool is_legal_vlen(unsigned bits) {
    return bits >= 32U && bits <= 65536U && is_power_of_two(bits);
}

bool is_legal_sew(unsigned bits) {
    return bits == 8U || bits == 16U || bits == 32U || bits == 64U;
}

bool checked_multiply(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t &result) {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        return false;
    }
    result = lhs * rhs;
    return true;
}

bool decode_lmul(RVVFixedLMUL lmul, unsigned &numerator, unsigned &denominator,
                 unsigned &register_group_width) {
    switch (lmul) {
    case RVVFixedLMUL::MF2:
        numerator = 1;
        denominator = 2;
        register_group_width = 1;
        return true;
    case RVVFixedLMUL::M1:
        numerator = 1;
        denominator = 1;
        register_group_width = 1;
        return true;
    case RVVFixedLMUL::M2:
        numerator = 2;
        denominator = 1;
        register_group_width = 2;
        return true;
    case RVVFixedLMUL::M4:
        numerator = 4;
        denominator = 1;
        register_group_width = 4;
        return true;
    case RVVFixedLMUL::M8:
        numerator = 8;
        denominator = 1;
        register_group_width = 8;
        return true;
    }
    return false;
}

bool supports_integer_sew(const TargetProfile &profile, unsigned sew_bits) {
    if (sew_bits <= 32U) {
        return profile.features.has_any_vector();
    }
    return profile.features.v || profile.features.zve64x || profile.features.zve64f ||
           profile.features.zve64d;
}

bool supports_float_sew(const TargetProfile &profile, unsigned sew_bits) {
    if (sew_bits == 32U) {
        return (profile.features.v && profile.features.f) || profile.features.zve32f ||
               profile.features.zve64f || profile.features.zve64d;
    }
    if (sew_bits == 64U) {
        return (profile.features.v && profile.features.d) || profile.features.zve64d;
    }
    return false;
}

bool minimum_vlen_matches_features(const TargetProfile &profile) {
    if (profile.features.v) {
        return profile.minimum_vlen_bits >= 128U;
    }
    if (profile.features.zve64x || profile.features.zve64f || profile.features.zve64d) {
        return profile.minimum_vlen_bits >= 64U;
    }
    return profile.minimum_vlen_bits >= 32U;
}

} // namespace

RVVFixedVectorCapacityResult
rvv_fixed_vector_minimum_capacity(unsigned minimum_vlen_bits, unsigned sew_bits,
                                  RVVFixedLMUL lmul) {
    RVVFixedVectorCapacityResult result;
    if (!is_legal_vlen(minimum_vlen_bits)) {
        result.error =
            "fixed RVV legalization requires minimum VLEN to be a power of two in [32, 65536]";
        return result;
    }
    if (!is_legal_sew(sew_bits)) {
        result.error = "fixed RVV legalization requires SEW 8, 16, 32, or 64";
        return result;
    }

    unsigned numerator = 0;
    unsigned denominator = 0;
    if (!decode_lmul(lmul, numerator, denominator, result.register_group_width)) {
        result.error = "fixed RVV legalization received an unsupported LMUL";
        return result;
    }

    const auto scaled_vlen = static_cast<std::uint64_t>(minimum_vlen_bits) * numerator;
    const auto capacity_denominator = static_cast<std::uint64_t>(sew_bits) * denominator;
    result.minimum_capacity_lanes = scaled_vlen / capacity_denominator;
    if (result.minimum_capacity_lanes == 0) {
        result.register_group_width = 0;
        result.error = "fixed RVV legalization LMUL has zero lane capacity at minimum VLEN";
        return result;
    }
    result.valid = true;
    return result;
}

RVVFixedVectorLegalizationResult
plan_rvv_fixed_vector_legalization(const RVVFixedVectorShape &shape,
                                   const TargetProfile &profile) {
    RVVFixedVectorLegalizationResult result;
    result.plan.shape = shape;
    result.plan.minimum_vlen_bits = profile.minimum_vlen_bits;

    switch (shape.element_kind) {
    case RVVFixedVectorElementKind::Integer:
    case RVVFixedVectorElementKind::FloatingPoint:
    case RVVFixedVectorElementKind::Mask:
        break;
    default:
        result.error = "fixed RVV legalization received an unsupported element kind";
        return result;
    }
    if (shape.logical_lanes == 0) {
        result.error = "fixed RVV legalization requires at least one logical lane";
        return result;
    }
    if (!is_legal_sew(shape.sew_bits)) {
        result.error = "fixed RVV legalization requires SEW 8, 16, 32, or 64";
        return result;
    }
    if (!profile.has_vector()) {
        result.error = "fixed RVV legalization requires a vector-enabled target";
        return result;
    }
    if (!is_legal_vlen(profile.minimum_vlen_bits)) {
        result.error =
            "fixed RVV legalization requires minimum VLEN to be a power of two in [32, 65536]";
        return result;
    }
    if (!minimum_vlen_matches_features(profile)) {
        result.error = "fixed RVV legalization minimum VLEN is incompatible with target features";
        return result;
    }

    const bool is_float =
        shape.element_kind == RVVFixedVectorElementKind::FloatingPoint;
    if (is_float ? !supports_float_sew(profile, shape.sew_bits)
                 : !supports_integer_sew(profile, shape.sew_bits)) {
        result.error = "fixed RVV legalization element SEW is unsupported by the target";
        return result;
    }

    const bool is_mask = shape.element_kind == RVVFixedVectorElementKind::Mask;
    if (is_mask) {
        result.plan.storage_size_bits = shape.logical_lanes;
        result.plan.storage_size_bytes = shape.logical_lanes / 8U;
        if (shape.logical_lanes % 8U != 0) {
            ++result.plan.storage_size_bytes;
        }
        result.plan.final_byte_unused_high_bits =
            static_cast<unsigned>((8U - shape.logical_lanes % 8U) % 8U);
        result.plan.zero_fill_unused_high_bits = true;
    } else {
        if (!checked_multiply(shape.logical_lanes, shape.sew_bits,
                              result.plan.storage_size_bits)) {
            result.error = "fixed RVV legalization storage size overflow";
            return result;
        }
        result.plan.storage_size_bytes = result.plan.storage_size_bits / 8U;
    }

    std::array<RVVFixedVectorCapacityResult, kSupportedLMULs.size()> capacities;
    for (std::size_t index = 0; index < kSupportedLMULs.size(); ++index) {
        capacities[index] = rvv_fixed_vector_minimum_capacity(
            profile.minimum_vlen_bits, shape.sew_bits, kSupportedLMULs[index]);
    }
    const auto &maximum = capacities.back();
    if (!maximum.valid || maximum.minimum_capacity_lanes == 0) {
        result.error = "fixed RVV legalization has no supported LMUL with nonzero capacity";
        return result;
    }

    const auto piece_count =
        shape.logical_lanes / maximum.minimum_capacity_lanes +
        (shape.logical_lanes % maximum.minimum_capacity_lanes != 0 ? 1U : 0U);
    if (piece_count > kMaximumPieceCount ||
        piece_count > static_cast<std::uint64_t>(result.plan.pieces.max_size())) {
        result.error = "fixed RVV legalization piece count exceeds planner capacity";
        return result;
    }
    result.plan.pieces.reserve(static_cast<std::size_t>(piece_count));

    std::uint64_t lane_base = 0;
    while (lane_base < shape.logical_lanes) {
        const auto remaining = shape.logical_lanes - lane_base;
        const auto lane_count =
            remaining < maximum.minimum_capacity_lanes ? remaining
                                                       : maximum.minimum_capacity_lanes;

        std::size_t selected = kSupportedLMULs.size();
        for (std::size_t index = 0; index < capacities.size(); ++index) {
            if (capacities[index].valid &&
                capacities[index].minimum_capacity_lanes >= lane_count) {
                selected = index;
                break;
            }
        }
        if (selected == kSupportedLMULs.size()) {
            result.plan.pieces.clear();
            result.error = "fixed RVV legalization cannot assign a legal LMUL to a piece";
            return result;
        }

        RVVFixedVectorPiece piece;
        piece.lane_base = lane_base;
        piece.lane_count = lane_count;
        piece.storage_offset_unit = is_mask ? RVVFixedStorageOffsetUnit::Bits
                                            : RVVFixedStorageOffsetUnit::Bytes;
        piece.storage_offset = is_mask ? lane_base : lane_base * (shape.sew_bits / 8U);
        piece.lmul = kSupportedLMULs[selected];
        piece.register_group_width = capacities[selected].register_group_width;
        piece.minimum_capacity_lanes = capacities[selected].minimum_capacity_lanes;
        result.plan.pieces.push_back(piece);
        lane_base += lane_count;
    }

    result.valid = true;
    return result;
}

const char *rvv_fixed_lmul_name(RVVFixedLMUL lmul) {
    switch (lmul) {
    case RVVFixedLMUL::MF2:
        return "mf2";
    case RVVFixedLMUL::M1:
        return "m1";
    case RVVFixedLMUL::M2:
        return "m2";
    case RVVFixedLMUL::M4:
        return "m4";
    case RVVFixedLMUL::M8:
        return "m8";
    }
    return "unknown";
}

} // namespace target
