#pragma once

#include "target/TargetMachine.h"

#include <cstdint>
#include <string>
#include <vector>

namespace target {

// A mask uses the SEW of the data vector it governs.  This keeps a data value
// and its mask on identical lane boundaries while the mask storage itself
// remains densely packed at one bit per logical lane.
enum class RVVFixedVectorElementKind {
    Integer,
    FloatingPoint,
    Mask,
};

// Fixed-value legalization deliberately starts at MF2.  MF4/MF8 do not reduce
// the physical register footprint below one register and are not used by the
// fixed-value backend's register-group contract.
enum class RVVFixedLMUL {
    MF2,
    M1,
    M2,
    M4,
    M8,
};

enum class RVVFixedStorageOffsetUnit {
    Bytes,
    Bits,
};

struct RVVFixedVectorShape {
    RVVFixedVectorElementKind element_kind = RVVFixedVectorElementKind::Integer;
    unsigned sew_bits = 0;
    std::uint64_t logical_lanes = 0;
};

struct RVVFixedVectorPiece {
    std::uint64_t lane_base = 0;
    std::uint64_t lane_count = 0;
    std::uint64_t storage_offset = 0;
    RVVFixedStorageOffsetUnit storage_offset_unit = RVVFixedStorageOffsetUnit::Bytes;
    RVVFixedLMUL lmul = RVVFixedLMUL::MF2;
    unsigned register_group_width = 0;
    std::uint64_t minimum_capacity_lanes = 0;
};

struct RVVFixedVectorLegalizationPlan {
    RVVFixedVectorShape shape;
    unsigned minimum_vlen_bits = 0;
    std::uint64_t storage_size_bits = 0;
    std::uint64_t storage_size_bytes = 0;
    std::vector<RVVFixedVectorPiece> pieces;

    // Masks use a stable, packed little-endian bit layout.  When the final
    // byte is partial, its unused most-significant bits must be written as
    // zero.  Data vectors leave both fields at their default values.
    unsigned final_byte_unused_high_bits = 0;
    bool zero_fill_unused_high_bits = false;
};

struct RVVFixedVectorCapacityResult {
    bool valid = false;
    std::uint64_t minimum_capacity_lanes = 0;
    unsigned register_group_width = 0;
    std::string error;
};

struct RVVFixedVectorLegalizationResult {
    bool valid = false;
    RVVFixedVectorLegalizationPlan plan;
    std::string error;
};

// Returns the lane capacity that is guaranteed at the supplied minimum VLEN.
// Invalid VLEN, SEW, LMUL, and zero-capacity combinations are diagnosed rather
// than asserted.  This helper is public so later lowering stages can verify a
// serialized or transformed plan before consuming it.
RVVFixedVectorCapacityResult
rvv_fixed_vector_minimum_capacity(unsigned minimum_vlen_bits, unsigned sew_bits,
                                  RVVFixedLMUL lmul);

// Splits a fixed logical value into the largest possible leading pieces.  Each
// piece is assigned the smallest supported LMUL that can hold it for every
// implementation whose VLEN is at least profile.minimum_vlen_bits.
RVVFixedVectorLegalizationResult
plan_rvv_fixed_vector_legalization(const RVVFixedVectorShape &shape,
                                   const TargetProfile &profile);

const char *rvv_fixed_lmul_name(RVVFixedLMUL lmul);

} // namespace target
