#include "builtin/BuiltinRegistry.h"

#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace builtin {
namespace {

using K = TypePatternKind;

constexpr TypePattern t(K kind) {
    return TypePattern::simple(kind);
}

constexpr TypePattern rel(K kind, std::uint8_t argument) {
    return TypePattern::related(kind, argument);
}

BuiltinDescriptor runtime(BuiltinID id, std::string_view name, TypePattern result,
                          std::vector<TypePattern> params, MemoryEffect memory_effect,
                          bool side_effect, std::vector<std::uint8_t> reads = {},
                          std::vector<std::uint8_t> writes = {}, bool variadic = false) {
    return {id,
            name,
            result,
            std::move(params),
            variadic,
            memory_effect,
            side_effect,
            std::move(reads),
            std::move(writes),
            LoweringKind::RuntimeCall,
            "sysy-runtime",
            "builtin_scalar"};
}

BuiltinDescriptor intrinsic(BuiltinID id, std::string_view name, TypePattern result,
                            std::vector<TypePattern> params, MemoryEffect memory_effect,
                            std::vector<std::uint8_t> reads = {},
                            std::vector<std::uint8_t> writes = {}) {
    return {id,
            name,
            result,
            std::move(params),
            false,
            memory_effect,
            memory_effect != MemoryEffect::None,
            std::move(reads),
            std::move(writes),
            LoweringKind::YIRIntrinsic,
            "vector-builtins",
            "builtin_vector"};
}

} // namespace

BuiltinRegistry::BuiltinRegistry() {
    entries_ = {
        runtime(BuiltinID::GetInt, "getint", t(K::Int), {}, MemoryEffect::None, true),
        runtime(BuiltinID::GetCh, "getch", t(K::Int), {}, MemoryEffect::None, true),
        runtime(BuiltinID::GetFloat, "getfloat", t(K::Float), {}, MemoryEffect::None, true),
        runtime(BuiltinID::GetArray, "getarray", t(K::Int), {t(K::PointerToInt)},
                MemoryEffect::Write, true, {}, {0}),
        runtime(BuiltinID::GetFloatArray, "getfarray", t(K::Int), {t(K::PointerToFloat)},
                MemoryEffect::Write, true, {}, {0}),
        runtime(BuiltinID::PutInt, "putint", t(K::Void), {t(K::Int)}, MemoryEffect::None, true),
        runtime(BuiltinID::PutCh, "putch", t(K::Void), {t(K::Int)}, MemoryEffect::None, true),
        runtime(BuiltinID::PutArray, "putarray", t(K::Void), {t(K::Int), t(K::PointerToInt)},
                MemoryEffect::Read, true, {1}),
        runtime(BuiltinID::PutFloat, "putfloat", t(K::Void), {t(K::Float)}, MemoryEffect::None,
                true),
        runtime(BuiltinID::PutFloatArray, "putfarray", t(K::Void),
                {t(K::Int), t(K::PointerToFloat)}, MemoryEffect::Read, true, {1}),
        runtime(BuiltinID::PutFormat, "putf", t(K::Void), {}, MemoryEffect::Unknown, true, {}, {},
                true),
        runtime(BuiltinID::StartTime, "starttime", t(K::Void), {}, MemoryEffect::None, true),
        runtime(BuiltinID::StopTime, "stoptime", t(K::Void), {}, MemoryEffect::None, true),
        runtime(BuiltinID::RuntimeRangesDisjoint, "__yoolang_ranges_disjoint", t(K::Int),
                {t(K::PointerToInt), t(K::Int), t(K::Int), t(K::Int), t(K::PointerToInt), t(K::Int),
                 t(K::Int), t(K::Int)},
                MemoryEffect::None, false),

        intrinsic(BuiltinID::VectorSelect, "select", rel(K::SameAsArgument, 1),
                  {t(K::Mask), t(K::VectorOrMask), rel(K::SameAsArgument, 1)}, MemoryEffect::None),
        intrinsic(BuiltinID::VectorAny, "any", t(K::Int), {t(K::Mask)}, MemoryEffect::None),
        intrinsic(BuiltinID::VectorAll, "all", t(K::Int), {t(K::Mask)}, MemoryEffect::None),
        intrinsic(BuiltinID::VectorNone, "none", t(K::Int), {t(K::Mask)}, MemoryEffect::None),
        intrinsic(BuiltinID::VectorExtract, "extract_lane", rel(K::ScalarOfVector, 0),
                  {t(K::NumericVector), t(K::Int)}, MemoryEffect::None),
        intrinsic(BuiltinID::VectorInsert, "insert_lane", rel(K::SameAsArgument, 0),
                  {t(K::NumericVector), t(K::Int), rel(K::ScalarOfVector, 0)}, MemoryEffect::None),
        intrinsic(BuiltinID::VectorIota, "iota", rel(K::SameAsArgument, 0), {t(K::IntegerVector)},
                  MemoryEffect::None),
        intrinsic(BuiltinID::VectorReduceAdd, "reduce_add", rel(K::ScalarOfVector, 0),
                  {t(K::NumericVector)}, MemoryEffect::None),
        intrinsic(BuiltinID::VectorReduceMul, "reduce_mul", rel(K::ScalarOfVector, 0),
                  {t(K::NumericVector)}, MemoryEffect::None),
        intrinsic(BuiltinID::VectorReduceMin, "reduce_min", rel(K::ScalarOfVector, 0),
                  {t(K::NumericVector)}, MemoryEffect::None),
        intrinsic(BuiltinID::VectorReduceMax, "reduce_max", rel(K::ScalarOfVector, 0),
                  {t(K::NumericVector)}, MemoryEffect::None),
        intrinsic(BuiltinID::VectorReduceAnd, "reduce_and", t(K::Int), {t(K::IntegerVector)},
                  MemoryEffect::None),
        intrinsic(BuiltinID::VectorReduceOr, "reduce_or", t(K::Int), {t(K::IntegerVector)},
                  MemoryEffect::None),
        intrinsic(BuiltinID::VectorReduceXor, "reduce_xor", t(K::Int), {t(K::IntegerVector)},
                  MemoryEffect::None),
        intrinsic(BuiltinID::VectorMaskedLoad, "masked_load", rel(K::SameAsArgument, 2),
                  {rel(K::PointerToVectorElement, 2), t(K::Mask), t(K::NumericVector)},
                  MemoryEffect::Read, {0}),
        intrinsic(BuiltinID::VectorMaskedStore, "masked_store", t(K::Void),
                  {rel(K::PointerToVectorElement, 2), t(K::Mask), t(K::NumericVector)},
                  MemoryEffect::Write, {}, {0}),
        intrinsic(BuiltinID::VectorGather, "gather", rel(K::SameAsArgument, 3),
                  {rel(K::PointerToVectorElement, 3), t(K::IntegerVector), t(K::Mask),
                   t(K::NumericVector)},
                  MemoryEffect::Read, {0}),
        intrinsic(BuiltinID::VectorScatter, "scatter", t(K::Void),
                  {rel(K::PointerToVectorElement, 3), t(K::IntegerVector), t(K::Mask),
                   t(K::NumericVector)},
                  MemoryEffect::Write, {}, {0}),
        intrinsic(BuiltinID::VectorShuffle, "shuffle", rel(K::SameAsArgument, 0),
                  {t(K::NumericVector), rel(K::SameAsArgument, 0), t(K::IntegerVector)},
                  MemoryEffect::None),
    };

    std::unordered_set<std::string_view> names;
    std::unordered_set<std::uint16_t> ids;
    for (const auto &entry : entries_) {
        if (entry.id == BuiltinID::Invalid || entry.source_name.empty() ||
            !names.insert(entry.source_name).second ||
            !ids.insert(static_cast<std::uint16_t>(entry.id)).second) {
            throw std::logic_error("duplicate or invalid builtin registry entry");
        }
    }
}

const BuiltinRegistry &BuiltinRegistry::instance() {
    static const BuiltinRegistry registry;
    return registry;
}

const BuiltinDescriptor *BuiltinRegistry::find(std::string_view source_name) const {
    for (const auto &entry : entries_) {
        if (entry.source_name == source_name) {
            return &entry;
        }
    }
    return nullptr;
}

const BuiltinDescriptor *BuiltinRegistry::find(BuiltinID id) const {
    for (const auto &entry : entries_) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

const std::vector<BuiltinDescriptor> &BuiltinRegistry::entries() const {
    return entries_;
}

std::string_view builtin_id_name(BuiltinID id) {
    if (const auto *entry = BuiltinRegistry::instance().find(id)) {
        return entry->source_name;
    }
    return "invalid";
}

} // namespace builtin
