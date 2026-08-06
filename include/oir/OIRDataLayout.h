#pragma once

#include "oir/OIR.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace oir {

// A fixed TypeSize is exact.  A scalable TypeSize records the known minimum
// for vscale >= 1; its runtime byte size must never be treated as a compile-
// time constant.
class TypeSize final {
  public:
    static TypeSize fixed(std::uint64_t bytes);
    static TypeSize scalable(std::uint64_t minimum_bytes);

    std::uint64_t minimum_bytes() const;
    bool is_fixed() const;
    bool is_scalable() const;
    std::optional<std::uint64_t> fixed_bytes() const;

    bool operator==(const TypeSize &other) const;
    bool operator!=(const TypeSize &other) const;

  private:
    TypeSize(std::uint64_t minimum_bytes, bool scalable);

    std::uint64_t minimum_bytes_ = 0;
    bool scalable_ = false;

    friend class DataLayout;
};

// Target-independent OIR storage layout for the currently supported RV64
// scalar and vector types.  Register legalization is intentionally outside
// this API: in particular, an i1 occupies one byte in memory even when a
// later lowering chooses a wider register or spill slot.
class DataLayout final {
  public:
    explicit DataLayout(std::uint64_t pointer_size_bytes = 8,
                        std::uint64_t pointer_abi_alignment = 8);

    std::uint64_t pointer_size_bytes() const;
    std::uint64_t pointer_abi_alignment() const;

    bool is_sized(const Type *type) const;
    TypeSize store_size(const Type *type) const;
    TypeSize alloc_size(const Type *type) const;
    std::uint64_t abi_alignment(const Type *type) const;
    std::uint64_t preferred_alignment(const Type *type) const;
    TypeSize array_stride(const ArrayType *type) const;

    // These helpers are the safe interface for consumers that require a
    // compile-time byte count (for example constant GEP offset analysis).
    // They return nullopt for unsized and scalable types.
    std::optional<std::uint64_t> fixed_store_size(const Type *type) const;
    std::optional<std::uint64_t> fixed_alloc_size(const Type *type) const;
    std::optional<std::uint64_t> fixed_array_stride(const ArrayType *type) const;

    // Returns one byte stride per GEP index using the same first-index and
    // nested-array rules as OIR GetElementPtrInst.  Scalable/unsized paths
    // fail closed with nullopt.  The offset helper additionally rejects all
    // signed multiplication or addition overflow.
    std::optional<std::vector<std::uint64_t>>
    fixed_gep_index_strides(const PointerType *base_pointer, std::size_t index_count) const;
    std::optional<std::int64_t> fixed_gep_offset(const PointerType *base_pointer,
                                                 const std::vector<std::int64_t> &indices) const;

  private:
    std::uint64_t pointer_size_bytes_;
    std::uint64_t pointer_abi_alignment_;
};

} // namespace oir
