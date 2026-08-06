#include "sema/SemanticType.h"

#include <functional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sema {
namespace {

std::size_t combine_hash(std::size_t seed, std::size_t value) {
    return seed ^ (value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U));
}

} // namespace

SemanticType::SemanticType(Kind kind) : kind_(kind) {
}

SemanticType::SemanticType(Kind kind, SemanticTypeRef contained_type, std::uint64_t count)
    : kind_(kind), contained_type_(contained_type), count_(count) {
}

SemanticType::SemanticType(SemanticTypeRef return_type,
                           std::vector<SemanticTypeRef> parameter_types, bool is_variadic)
    : kind_(Kind::Function), parameter_types_(std::move(parameter_types)),
      return_type_(return_type), is_variadic_(is_variadic) {
}

SemanticTypeRef SemanticType::element_type() const {
    return is_fixed_vector() || is_array() ? contained_type_ : nullptr;
}

SemanticTypeRef SemanticType::pointee_type() const {
    return is_pointer() ? contained_type_ : nullptr;
}

std::uint64_t SemanticType::lane_count() const {
    return is_fixed_vector() || is_mask() ? count_ : 0;
}

std::uint64_t SemanticType::array_bound() const {
    return is_array() ? count_ : 0;
}

std::string SemanticType::str() const {
    switch (kind_) {
    case Kind::Error:
        return "<error>";
    case Kind::Void:
        return "void";
    case Kind::Int:
        return "int";
    case Kind::Float:
        return "float";
    case Kind::FixedVector:
        return "vector<" + contained_type_->str() + ", " + std::to_string(count_) + ">";
    case Kind::Mask:
        return "mask<" + std::to_string(count_) + ">";
    case Kind::Array:
        return "array<" + std::to_string(count_) + " x " + contained_type_->str() + ">";
    case Kind::Pointer:
        return "ptr<" + contained_type_->str() + ">";
    case Kind::Function: {
        std::ostringstream out;
        out << "func<(";
        for (std::size_t i = 0; i < parameter_types_.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << parameter_types_[i]->str();
        }
        if (is_variadic_) {
            if (!parameter_types_.empty()) {
                out << ", ";
            }
            out << "...";
        }
        out << ") -> " << return_type_->str() << ">";
        return out.str();
    }
    }
    return "<error>";
}

SemanticTypeContext::SemanticTypeContext() {
    error_type_ = own(std::unique_ptr<SemanticType>(new SemanticType(SemanticType::Kind::Error)));
    void_type_ = own(std::unique_ptr<SemanticType>(new SemanticType(SemanticType::Kind::Void)));
    int_type_ = own(std::unique_ptr<SemanticType>(new SemanticType(SemanticType::Kind::Int)));
    float_type_ = own(std::unique_ptr<SemanticType>(new SemanticType(SemanticType::Kind::Float)));
}

std::size_t SemanticTypeContext::TypeAndCountKeyHash::operator()(const TypeAndCountKey &key) const {
    std::size_t seed = std::hash<SemanticTypeRef>{}(key.type);
    return combine_hash(seed, std::hash<std::uint64_t>{}(key.count));
}

std::size_t SemanticTypeContext::FunctionKeyHash::operator()(const FunctionKey &key) const {
    std::size_t seed = std::hash<SemanticTypeRef>{}(key.return_type);
    for (auto *parameter_type : key.parameter_types) {
        seed = combine_hash(seed, std::hash<SemanticTypeRef>{}(parameter_type));
    }
    return combine_hash(seed, std::hash<bool>{}(key.is_variadic));
}

SemanticTypeRef SemanticTypeContext::own(std::unique_ptr<SemanticType> type) {
    auto *raw = type.get();
    owned_types_.push_back(std::move(type));
    return raw;
}

SemanticTypeRef SemanticTypeContext::fixed_vector_type(SemanticTypeRef element_type,
                                                       std::uint64_t lane_count) {
    if (element_type == nullptr || !element_type->is_numeric_scalar()) {
        throw std::invalid_argument("fixed vector element type must be int or float");
    }
    if (lane_count == 0) {
        throw std::invalid_argument("fixed vector lane count must be positive");
    }

    TypeAndCountKey key{element_type, lane_count};
    auto found = vector_types_.find(key);
    if (found != vector_types_.end()) {
        return found->second;
    }
    auto *type = own(std::unique_ptr<SemanticType>(
        new SemanticType(SemanticType::Kind::FixedVector, element_type, lane_count)));
    vector_types_.emplace(key, type);
    return type;
}

SemanticTypeRef SemanticTypeContext::mask_type(std::uint64_t lane_count) {
    if (lane_count == 0) {
        throw std::invalid_argument("mask lane count must be positive");
    }
    auto found = mask_types_.find(lane_count);
    if (found != mask_types_.end()) {
        return found->second;
    }
    auto *type = own(std::unique_ptr<SemanticType>(
        new SemanticType(SemanticType::Kind::Mask, nullptr, lane_count)));
    mask_types_.emplace(lane_count, type);
    return type;
}

SemanticTypeRef SemanticTypeContext::array_type(SemanticTypeRef element_type, std::uint64_t bound) {
    if (element_type == nullptr || element_type->is_void() || element_type->is_function()) {
        throw std::invalid_argument("array element type must be an object type");
    }
    if (bound == 0) {
        throw std::invalid_argument("array bound must be positive");
    }

    TypeAndCountKey key{element_type, bound};
    auto found = array_types_.find(key);
    if (found != array_types_.end()) {
        return found->second;
    }
    auto *type = own(std::unique_ptr<SemanticType>(
        new SemanticType(SemanticType::Kind::Array, element_type, bound)));
    array_types_.emplace(key, type);
    return type;
}

SemanticTypeRef SemanticTypeContext::pointer_type(SemanticTypeRef pointee_type) {
    if (pointee_type == nullptr) {
        throw std::invalid_argument("pointer pointee type cannot be null");
    }
    auto found = pointer_types_.find(pointee_type);
    if (found != pointer_types_.end()) {
        return found->second;
    }
    auto *type = own(
        std::unique_ptr<SemanticType>(new SemanticType(SemanticType::Kind::Pointer, pointee_type)));
    pointer_types_.emplace(pointee_type, type);
    return type;
}

SemanticTypeRef
SemanticTypeContext::function_type(SemanticTypeRef return_type,
                                   const std::vector<SemanticTypeRef> &parameter_types,
                                   bool is_variadic) {
    if (return_type == nullptr) {
        throw std::invalid_argument("function return type cannot be null");
    }
    for (auto *parameter_type : parameter_types) {
        if (parameter_type == nullptr || parameter_type->is_void() ||
            parameter_type->is_function()) {
            throw std::invalid_argument("function parameter type is invalid");
        }
    }

    FunctionKey key{return_type, parameter_types, is_variadic};
    auto found = function_types_.find(key);
    if (found != function_types_.end()) {
        return found->second;
    }
    auto *type = own(std::unique_ptr<SemanticType>(
        new SemanticType(return_type, parameter_types, is_variadic)));
    function_types_.emplace(std::move(key), type);
    return type;
}

} // namespace sema
