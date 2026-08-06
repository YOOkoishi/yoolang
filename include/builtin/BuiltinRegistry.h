#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace builtin {

// Stable semantic identity shared by the source semantic analyser, IR
// lowering, ModRef analysis and documentation/tests.  Values are intentionally
// explicit: serialized diagnostics and vectorization remarks may refer to
// them, so reordering the registry must not change an id.
enum class BuiltinID : std::uint16_t {
    Invalid = 0,

    GetInt = 1,
    GetCh = 2,
    GetFloat = 3,
    GetArray = 4,
    GetFloatArray = 5,
    PutInt = 6,
    PutCh = 7,
    PutArray = 8,
    PutFloat = 9,
    PutFloatArray = 10,
    PutFormat = 11,
    StartTime = 12,
    StopTime = 13,

    // Compiler-internal, pure runtime predicate used by overflow-safe loop
    // alias versioning.  Its stable descriptor is also the ModRef contract.
    RuntimeRangesDisjoint = 50,

    VectorSelect = 100,
    VectorAny = 101,
    VectorAll = 102,
    VectorNone = 103,
    VectorExtract = 104,
    VectorInsert = 105,
    VectorIota = 106,
    VectorReduceAdd = 107,
    VectorReduceMul = 108,
    VectorReduceMin = 109,
    VectorReduceMax = 110,
    VectorReduceAnd = 111,
    VectorReduceOr = 112,
    VectorReduceXor = 113,
    VectorMaskedLoad = 114,
    VectorMaskedStore = 115,
    VectorGather = 116,
    VectorScatter = 117,
    VectorShuffle = 118,
};

// TypePattern is a small, target-independent type scheme.  Concrete source
// types are owned by sema::SemanticTypeContext; this descriptor only states
// relationships that an overload resolver must instantiate.
enum class TypePatternKind : std::uint8_t {
    Void,
    Int,
    Float,
    PointerToInt,
    PointerToFloat,
    NumericScalar,
    IntegerScalar,
    NumericVector,
    VectorOrMask,
    IntegerVector,
    Mask,
    ScalarOfVector,
    PointerToVectorElement,
    SameAsArgument,
};

struct TypePattern final {
    TypePatternKind kind = TypePatternKind::Void;
    // Used by SameAsArgument, ScalarOfVector and PointerToVectorElement.
    std::uint8_t argument_index = 0;

    static constexpr TypePattern simple(TypePatternKind kind) {
        return {kind, 0};
    }

    static constexpr TypePattern related(TypePatternKind kind, std::uint8_t argument_index) {
        return {kind, argument_index};
    }
};

enum class MemoryEffect : std::uint8_t {
    None,
    Read,
    Write,
    ReadWrite,
    Unknown,
};

enum class LoweringKind : std::uint8_t {
    RuntimeCall,
    YIRIntrinsic,
};

struct BuiltinDescriptor final {
    BuiltinID id = BuiltinID::Invalid;
    std::string_view source_name;
    TypePattern result;
    std::vector<TypePattern> parameters;
    bool variadic = false;
    MemoryEffect memory_effect = MemoryEffect::None;
    bool has_observable_side_effect = false;
    // Parameter indices whose pointees are read/written.  Unknown memory is
    // represented by MemoryEffect::Unknown instead of an invented index.
    std::vector<std::uint8_t> read_pointer_parameters;
    std::vector<std::uint8_t> written_pointer_parameters;
    LoweringKind lowering = LoweringKind::RuntimeCall;
    std::string_view documentation_anchor;
    std::string_view test_tag;
};

class BuiltinRegistry final {
  public:
    static const BuiltinRegistry &instance();

    const BuiltinDescriptor *find(std::string_view source_name) const;
    const BuiltinDescriptor *find(BuiltinID id) const;
    const std::vector<BuiltinDescriptor> &entries() const;

  private:
    BuiltinRegistry();

    std::vector<BuiltinDescriptor> entries_;
};

std::string_view builtin_id_name(BuiltinID id);

} // namespace builtin
