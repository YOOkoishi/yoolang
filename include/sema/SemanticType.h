#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sema {

class SemanticType;
using SemanticTypeRef = const SemanticType *;

class SemanticType final {
  public:
    enum class Kind {
        Error,
        Void,
        Int,
        Float,
        FixedVector,
        Mask,
        Array,
        Pointer,
        Function,
    };

    Kind kind() const {
        return kind_;
    }

    bool is_error() const {
        return kind_ == Kind::Error;
    }
    bool is_void() const {
        return kind_ == Kind::Void;
    }
    bool is_integer() const {
        return kind_ == Kind::Int;
    }
    bool is_float() const {
        return kind_ == Kind::Float;
    }
    bool is_numeric_scalar() const {
        return is_integer() || is_float();
    }
    bool is_fixed_vector() const {
        return kind_ == Kind::FixedVector;
    }
    bool is_vector() const {
        return is_fixed_vector();
    }
    bool is_mask() const {
        return kind_ == Kind::Mask;
    }
    bool is_array() const {
        return kind_ == Kind::Array;
    }
    bool is_pointer() const {
        return kind_ == Kind::Pointer;
    }
    bool is_function() const {
        return kind_ == Kind::Function;
    }

    // Fixed vectors and arrays carry an element type. Other kinds return null.
    SemanticTypeRef element_type() const;

    // Pointers carry a pointee type. Other kinds return null.
    SemanticTypeRef pointee_type() const;

    // The count is non-zero for fixed vectors and masks. Other kinds return 0.
    std::uint64_t lane_count() const;

    // The bound is non-zero for arrays. Other kinds return 0.
    std::uint64_t array_bound() const;

    const std::vector<SemanticTypeRef> &parameter_types() const {
        return parameter_types_;
    }

    SemanticTypeRef return_type() const {
        return return_type_;
    }

    bool is_variadic() const {
        return is_variadic_;
    }

    std::string str() const;

  private:
    friend class SemanticTypeContext;

    explicit SemanticType(Kind kind);
    SemanticType(Kind kind, SemanticTypeRef contained_type, std::uint64_t count = 0);
    SemanticType(SemanticTypeRef return_type, std::vector<SemanticTypeRef> parameter_types,
                 bool is_variadic);

    Kind kind_ = Kind::Error;
    SemanticTypeRef contained_type_ = nullptr;
    std::uint64_t count_ = 0;
    std::vector<SemanticTypeRef> parameter_types_;
    SemanticTypeRef return_type_ = nullptr;
    bool is_variadic_ = false;
};

class SemanticTypeContext final {
  public:
    SemanticTypeContext();
    ~SemanticTypeContext() = default;

    SemanticTypeContext(const SemanticTypeContext &) = delete;
    SemanticTypeContext &operator=(const SemanticTypeContext &) = delete;

    SemanticTypeRef error_type() const {
        return error_type_;
    }
    SemanticTypeRef void_type() const {
        return void_type_;
    }
    SemanticTypeRef int_type() const {
        return int_type_;
    }
    SemanticTypeRef float_type() const {
        return float_type_;
    }

    // Only int and float are legal source vector element types in the current
    // language. A zero lane count is rejected.
    SemanticTypeRef fixed_vector_type(SemanticTypeRef element_type, std::uint64_t lane_count);
    SemanticTypeRef mask_type(std::uint64_t lane_count);
    SemanticTypeRef array_type(SemanticTypeRef element_type, std::uint64_t bound);
    SemanticTypeRef pointer_type(SemanticTypeRef pointee_type);
    SemanticTypeRef function_type(SemanticTypeRef return_type,
                                  const std::vector<SemanticTypeRef> &parameter_types,
                                  bool is_variadic = false);

  private:
    struct TypeAndCountKey {
        SemanticTypeRef type = nullptr;
        std::uint64_t count = 0;

        bool operator==(const TypeAndCountKey &other) const {
            return type == other.type && count == other.count;
        }
    };

    struct TypeAndCountKeyHash {
        std::size_t operator()(const TypeAndCountKey &key) const;
    };

    struct FunctionKey {
        SemanticTypeRef return_type = nullptr;
        std::vector<SemanticTypeRef> parameter_types;
        bool is_variadic = false;

        bool operator==(const FunctionKey &other) const {
            return return_type == other.return_type && parameter_types == other.parameter_types &&
                   is_variadic == other.is_variadic;
        }
    };

    struct FunctionKeyHash {
        std::size_t operator()(const FunctionKey &key) const;
    };

    SemanticTypeRef own(std::unique_ptr<SemanticType> type);

    std::vector<std::unique_ptr<SemanticType>> owned_types_;
    SemanticTypeRef error_type_ = nullptr;
    SemanticTypeRef void_type_ = nullptr;
    SemanticTypeRef int_type_ = nullptr;
    SemanticTypeRef float_type_ = nullptr;

    std::unordered_map<TypeAndCountKey, SemanticTypeRef, TypeAndCountKeyHash> vector_types_;
    std::unordered_map<std::uint64_t, SemanticTypeRef> mask_types_;
    std::unordered_map<TypeAndCountKey, SemanticTypeRef, TypeAndCountKeyHash> array_types_;
    std::unordered_map<SemanticTypeRef, SemanticTypeRef> pointer_types_;
    std::unordered_map<FunctionKey, SemanticTypeRef, FunctionKeyHash> function_types_;
};

} // namespace sema
