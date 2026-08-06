#include "yir/YIR.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace yir {

Type::Type(Kind kind) : kind_(kind) {
}

Type::Type(Kind kind, TypePtr nested) : kind_(kind), pointee_(std::move(nested)) {
}

Type::Type(Kind kind, std::uint64_t count, TypePtr element)
    : kind_(kind), element_(std::move(element)), count_(count) {
}

Type::Type(std::vector<TypePtr> params, TypePtr result, bool is_variadic)
    : kind_(Kind::Func), params_(std::move(params)), result_(std::move(result)),
      is_variadic_(is_variadic) {
}

TypePtr Type::get_i1() {
    static TypePtr type(new Type(Kind::I1));
    return type;
}

TypePtr Type::get_i32() {
    static TypePtr type(new Type(Kind::I32));
    return type;
}

TypePtr Type::get_f32() {
    static TypePtr type(new Type(Kind::F32));
    return type;
}

TypePtr Type::get_void() {
    static TypePtr type(new Type(Kind::Void));
    return type;
}

TypePtr Type::get_ptr(TypePtr pointee) {
    if (pointee == nullptr) {
        throw std::invalid_argument("pointer pointee type must not be null");
    }
    static std::mutex mutex;
    static std::map<const Type *, TypePtr> types;
    std::lock_guard<std::mutex> lock(mutex);
    auto &slot = types[pointee.get()];
    if (slot != nullptr) {
        return slot;
    }
    auto type = TypePtr(new Type(Kind::Ptr, std::move(pointee)));
    slot = type;
    return type;
}

TypePtr Type::get_array(std::uint64_t count, TypePtr element) {
    if (count == 0) {
        throw std::invalid_argument("array element count must be positive");
    }
    if (element == nullptr || element->is_void()) {
        throw std::invalid_argument("array element type must be a non-void type");
    }
    using Key = std::pair<std::uint64_t, const Type *>;
    static std::mutex mutex;
    static std::map<Key, TypePtr> types;
    std::lock_guard<std::mutex> lock(mutex);
    Key key{count, element.get()};
    auto &slot = types[key];
    if (slot != nullptr) {
        return slot;
    }
    auto type = TypePtr(new Type(Kind::Array, count, std::move(element)));
    slot = type;
    return type;
}

TypePtr Type::get_vector(std::uint64_t count, TypePtr element) {
    if (count == 0) {
        throw std::invalid_argument("vector lane count must be positive");
    }
    if (element == nullptr || (!element->is_integer() && !element->is_float()) ||
        element->kind() == Kind::I1) {
        throw std::invalid_argument("vector element type must be i32 or f32");
    }
    using Key = std::pair<std::uint64_t, const Type *>;
    static std::mutex mutex;
    static std::map<Key, TypePtr> types;
    std::lock_guard<std::mutex> lock(mutex);
    Key key{count, element.get()};
    auto &slot = types[key];
    if (slot != nullptr) {
        return slot;
    }
    auto type = TypePtr(new Type(Kind::Vector, count, std::move(element)));
    slot = type;
    return type;
}

TypePtr Type::get_mask(std::uint64_t count) {
    if (count == 0) {
        throw std::invalid_argument("mask lane count must be positive");
    }
    static std::mutex mutex;
    static std::map<std::uint64_t, TypePtr> types;
    std::lock_guard<std::mutex> lock(mutex);
    auto &slot = types[count];
    if (slot != nullptr) {
        return slot;
    }
    auto type = TypePtr(new Type(Kind::Mask, count, Type::get_i1()));
    slot = type;
    return type;
}

TypePtr Type::get_func(std::vector<TypePtr> params, TypePtr result, bool is_variadic) {
    if (result == nullptr) {
        throw std::invalid_argument("function result type must not be null");
    }
    for (const auto &param : params) {
        if (param == nullptr || param->is_void()) {
            throw std::invalid_argument("function parameter type must be non-void");
        }
    }
    struct Key {
        std::vector<const Type *> params;
        const Type *result = nullptr;
        bool is_variadic = false;

        bool operator<(const Key &other) const {
            return std::tie(params, result, is_variadic) <
                   std::tie(other.params, other.result, other.is_variadic);
        }
    };
    Key key;
    key.result = result.get();
    key.is_variadic = is_variadic;
    key.params.reserve(params.size());
    std::transform(params.begin(), params.end(), std::back_inserter(key.params),
                   [](const TypePtr &type) { return type.get(); });
    static std::mutex mutex;
    static std::map<Key, TypePtr> types;
    std::lock_guard<std::mutex> lock(mutex);
    auto &slot = types[key];
    if (slot != nullptr) {
        return slot;
    }
    auto type = TypePtr(new Type(std::move(params), std::move(result), is_variadic));
    slot = type;
    return type;
}

bool Type::is_integer() const {
    return kind_ == Kind::I1 || kind_ == Kind::I32;
}

bool Type::is_float() const {
    return kind_ == Kind::F32;
}

bool Type::is_scalar() const {
    return is_integer() || is_float();
}

bool Type::is_vector() const {
    return kind_ == Kind::Vector;
}

bool Type::is_mask() const {
    return kind_ == Kind::Mask;
}

bool Type::is_void() const {
    return kind_ == Kind::Void;
}

bool Type::is_ptr() const {
    return kind_ == Kind::Ptr;
}

bool Type::is_array() const {
    return kind_ == Kind::Array;
}

std::string Type::str() const {
    switch (kind_) {
    case Kind::I1:
        return "i1";
    case Kind::I32:
        return "i32";
    case Kind::F32:
        return "f32";
    case Kind::Void:
        return "void";
    case Kind::Ptr:
        return "ptr<" + pointee_->str() + ">";
    case Kind::Array:
        return "array<" + std::to_string(count_) + " x " + element_->str() + ">";
    case Kind::Vector:
        return "vector<" + std::to_string(count_) + " x " + element_->str() + ">";
    case Kind::Mask:
        return "mask<" + std::to_string(count_) + ">";
    case Kind::Func: {
        std::ostringstream oss;
        oss << "func<(";
        for (std::size_t i = 0; i < params_.size(); ++i) {
            if (i != 0) {
                oss << ", ";
            }
            oss << params_[i]->str();
        }
        if (is_variadic_) {
            if (!params_.empty()) {
                oss << ", ";
            }
            oss << "...";
        }
        oss << ") -> " << result_->str() << ">";
        return oss.str();
    }
    }
    return "<unknown>";
}

Constant::Constant(TypePtr type) : type_(std::move(type)) {
    if (type_ == nullptr || type_->is_void() || type_->kind() == Type::Kind::Func ||
        type_->is_ptr()) {
        throw std::invalid_argument("YIR constant requires a non-pointer object type");
    }
}

ConstantInt::ConstantInt(TypePtr type, std::int64_t value)
    : Constant(std::move(type)), value_(value) {
    if (this->type() != Type::get_i1() && this->type() != Type::get_i32()) {
        throw std::invalid_argument("YIR integer constant requires i1 or i32 type");
    }
    if (this->type() == Type::get_i1() && value_ != 0 && value_ != 1) {
        throw std::invalid_argument("YIR i1 constant must be zero or one");
    }
    if (this->type() == Type::get_i32() &&
        (value_ < std::numeric_limits<std::int32_t>::min() ||
         value_ > std::numeric_limits<std::int32_t>::max())) {
        throw std::out_of_range("YIR i32 constant is out of range");
    }
}

bool ConstantInt::is_zero() const {
    return value_ == 0;
}

std::string ConstantInt::str() const {
    return type()->str() + " " +
           (type() == Type::get_i1() ? (value_ == 0 ? "false" : "true")
                                     : std::to_string(value_));
}

ConstantFloat::ConstantFloat(float value) : Constant(Type::get_f32()), value_(value) {
}

bool ConstantFloat::is_zero() const {
    // Aggregate-zero elision is a storage decision, so signed zero must not be
    // treated as the all-zero bit pattern.  In particular, -0.0f has an
    // observable sign bit through vector/global loads and floating-point
    // operations even though it compares equal to +0.0f.
    return value_ == 0.0F && !std::signbit(value_);
}

std::string ConstantFloat::str() const {
    std::ostringstream out;
    out << "f32 " << std::setprecision(9) << value_;
    return out.str();
}

ConstantAggregateZero::ConstantAggregateZero(TypePtr type) : Constant(std::move(type)) {
}

bool ConstantAggregateZero::is_zero() const {
    return true;
}

std::string ConstantAggregateZero::str() const {
    return "zero";
}

ConstantArray::ConstantArray(TypePtr type, std::vector<ConstantPtr> elements)
    : Constant(std::move(type)), elements_(std::move(elements)) {
    if (!this->type()->is_array() || elements_.size() != this->type()->count()) {
        throw std::invalid_argument("YIR array constant element count does not match type");
    }
    for (const auto &element : elements_) {
        if (element == nullptr || element->type() != this->type()->element()) {
            throw std::invalid_argument("YIR array constant element type does not match type");
        }
    }
}

bool ConstantArray::is_zero() const {
    return std::all_of(elements_.begin(), elements_.end(),
                       [](const ConstantPtr &element) { return element->is_zero(); });
}

std::string ConstantArray::str() const {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < elements_.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << elements_[i]->str();
    }
    out << ']';
    return out.str();
}

ConstantVector::ConstantVector(TypePtr type, std::vector<ConstantPtr> lanes)
    : Constant(std::move(type)), lanes_(std::move(lanes)) {
    if (!this->type()->is_vector() || lanes_.size() != this->type()->count()) {
        throw std::invalid_argument("YIR vector constant lane count does not match type");
    }
    for (const auto &lane : lanes_) {
        if (lane == nullptr || lane->type() != this->type()->element()) {
            throw std::invalid_argument("YIR vector constant lane type does not match type");
        }
    }
}

bool ConstantVector::is_zero() const {
    return std::all_of(lanes_.begin(), lanes_.end(),
                       [](const ConstantPtr &lane) { return lane->is_zero(); });
}

std::string ConstantVector::str() const {
    std::ostringstream out;
    out << '<';
    for (std::size_t i = 0; i < lanes_.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << lanes_[i]->str();
    }
    out << '>';
    return out.str();
}

ConstantMask::ConstantMask(TypePtr type, std::vector<std::uint8_t> packed_bytes)
    : Constant(std::move(type)), packed_bytes_(std::move(packed_bytes)) {
    if (!this->type()->is_mask()) {
        throw std::invalid_argument("YIR mask constant requires mask type");
    }
    const auto expected_bytes = static_cast<std::size_t>((this->type()->count() + 7) / 8);
    if (packed_bytes_.size() != expected_bytes) {
        throw std::invalid_argument("YIR mask constant packed byte count does not match type");
    }
    const auto used_bits = static_cast<unsigned>(this->type()->count() % 8);
    if (used_bits != 0) {
        const auto unused_mask = static_cast<std::uint8_t>(0xffU << used_bits);
        if ((packed_bytes_.back() & unused_mask) != 0) {
            throw std::invalid_argument("YIR mask constant unused high bits must be zero");
        }
    }
}

bool ConstantMask::lane(std::uint64_t index) const {
    if (index >= type()->count()) {
        throw std::out_of_range("YIR mask lane index is out of range");
    }
    return (packed_bytes_[index / 8] & static_cast<std::uint8_t>(1U << (index % 8))) != 0;
}

bool ConstantMask::is_zero() const {
    return std::all_of(packed_bytes_.begin(), packed_bytes_.end(),
                       [](std::uint8_t byte) { return byte == 0; });
}

std::string ConstantMask::str() const {
    std::ostringstream out;
    out << "maskbits<";
    for (std::uint64_t index = 0; index < type()->count(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << (lane(index) ? '1' : '0');
    }
    out << '>';
    return out.str();
}

Value::Value(TypePtr type, std::string name) : type_(std::move(type)), name_(std::move(name)) {
}

Operation::Operation(std::string op_name, std::vector<Value *> operands, TypePtr result_type,
                     std::string result_name)
    : op_name_(std::move(op_name)), operands_(std::move(operands)) {
    if (result_type != nullptr && !result_type->is_void()) {
        result_ = std::make_unique<Value>(std::move(result_type), std::move(result_name));
        result_->set_defining_op(this);
    }
}

Global::Global(std::string name, TypePtr storage_type, bool is_const)
    : name_(std::move(name)), storage_type_(std::move(storage_type)), is_const_(is_const),
      address_(storage_type_, "@" + name_) {
}

Function::Function(std::string name, TypePtr return_type, std::vector<TypePtr> param_types,
                   bool is_variadic, bool is_external)
    : name_(std::move(name)), return_type_(std::move(return_type)),
      param_types_(std::move(param_types)), is_variadic_(is_variadic),
      is_external_(is_external) {
}

Value *Function::add_param(TypePtr type, std::string name) {
    auto param = std::make_unique<Value>(std::move(type), std::move(name));
    auto *raw = param.get();
    params_.push_back(std::move(param));
    return raw;
}

Global *Module::add_global(std::string name, TypePtr storage_type, bool is_const) {
    auto global = std::make_unique<Global>(std::move(name), std::move(storage_type), is_const);
    auto *raw = global.get();
    globals_.push_back(std::move(global));
    return raw;
}

Function *Module::add_function(std::string name, TypePtr return_type,
                               std::vector<TypePtr> param_types, bool is_variadic,
                               bool is_external) {
    auto function = std::make_unique<Function>(std::move(name), std::move(return_type),
                                               std::move(param_types), is_variadic, is_external);
    auto *raw = function.get();
    functions_.push_back(std::move(function));
    return raw;
}

ConstI32Op::ConstI32Op(int value, std::string result_name)
    : Operation("yir.const.i32", {}, Type::get_i32(), std::move(result_name)), value_(value) {
}

ConstF32Op::ConstF32Op(float value, std::string result_name)
    : Operation("yir.const.f32", {}, Type::get_f32(), std::move(result_name)), value_(value) {
}

ConstBoolOp::ConstBoolOp(bool value, std::string result_name)
    : Operation("yir.const.bool", {}, Type::get_i1(), std::move(result_name)), value_(value) {
}

ZeroOp::ZeroOp(TypePtr type, std::string result_name)
    : Operation("yir.zero", {}, std::move(type), std::move(result_name)) {
}

VarOp::VarOp(TypePtr type, Value *initializer, std::string result_name)
    : Operation("yir.var", initializer == nullptr ? std::vector<Value *>{}
                                                  : std::vector<Value *>{initializer},
                std::move(type), std::move(result_name)) {
}

AssignOp::AssignOp(Value *target, Value *value) : Operation("yir.assign", {target, value}) {
}

ArrayVarOp::ArrayVarOp(TypePtr array_type, std::string result_name)
    : Operation("yir.array_var", {}, std::move(array_type), std::move(result_name)) {
}

ArrayInitOp::ArrayInitOp(Value *array, TypePtr array_type, std::vector<ArrayInitEntry> entries,
                         bool default_zero)
    : Operation("yir.array_init", {array}), array_type_(std::move(array_type)),
      entries_(std::move(entries)), default_zero_(default_zero) {
    for (const auto &entry : entries_) {
        if (entry.value != nullptr) {
            operands().push_back(entry.value);
        }
    }
}

ArrayLoadOp::ArrayLoadOp(Value *array, std::vector<Value *> indices, TypePtr element_type,
                         std::string result_name)
    : Operation("yir.array_load", {}, std::move(element_type), std::move(result_name)) {
    operands().push_back(array);
    operands().insert(operands().end(), indices.begin(), indices.end());
}

std::vector<Value *> ArrayLoadOp::indices() const {
    auto ops = operands();
    if (!ops.empty()) {
        ops.erase(ops.begin());
    }
    return ops;
}

ArrayStoreOp::ArrayStoreOp(Value *value, Value *array, std::vector<Value *> indices)
    : Operation("yir.array_store", {}) {
    operands().push_back(value);
    operands().push_back(array);
    operands().insert(operands().end(), indices.begin(), indices.end());
}

std::vector<Value *> ArrayStoreOp::indices() const {
    auto ops = operands();
    if (ops.size() >= 2) {
        ops.erase(ops.begin(), ops.begin() + 2);
    }
    return ops;
}

AllocaOp::AllocaOp(TypePtr storage_type, std::string result_name)
    : Operation("yir.alloca", {}, Type::get_ptr(storage_type), std::move(result_name)),
      storage_type_(std::move(storage_type)) {
}

LoadOp::LoadOp(Value *address, TypePtr result_type, std::string result_name)
    : Operation("yir.load", {address}, std::move(result_type), std::move(result_name)) {
}

StoreOp::StoreOp(Value *value, Value *address) : Operation("yir.store", {value, address}) {
}

ElemAddrOp::ElemAddrOp(Value *base, std::vector<Value *> indices, TypePtr result_type,
                       std::string result_name)
    : Operation("yir.elem_addr", {}, std::move(result_type), std::move(result_name)) {
    operands().push_back(base);
    operands().insert(operands().end(), indices.begin(), indices.end());
}

std::vector<Value *> ElemAddrOp::indices() const {
    auto ops = operands();
    if (!ops.empty()) {
        ops.erase(ops.begin());
    }
    return ops;
}

DecayOp::DecayOp(Value *array_address, TypePtr result_type, std::string result_name)
    : Operation("yir.decay", {array_address}, std::move(result_type), std::move(result_name)) {
}

BinaryOpBase::BinaryOpBase(std::string op_name, Value *lhs, Value *rhs, TypePtr result_type,
                           std::string result_name)
    : Operation(std::move(op_name), {lhs, rhs}, std::move(result_type), std::move(result_name)) {
}

namespace {

TypePtr integer_binary_result(Value *lhs) {
    return lhs != nullptr && lhs->type() != nullptr && lhs->type()->is_vector()
               ? lhs->type()
               : Type::get_i32();
}

TypePtr float_binary_result(Value *lhs) {
    return lhs != nullptr && lhs->type() != nullptr && lhs->type()->is_vector()
               ? lhs->type()
               : Type::get_f32();
}

TypePtr comparison_result(Value *lhs) {
    if (lhs != nullptr && lhs->type() != nullptr && lhs->type()->is_vector()) {
        return Type::get_mask(lhs->type()->count());
    }
    return Type::get_i1();
}

TypePtr lane_result(Value *vector) {
    if (vector == nullptr || vector->type() == nullptr) {
        return Type::get_i32();
    }
    if (vector->type()->is_mask()) {
        return Type::get_i1();
    }
    if (vector->type()->is_vector()) {
        return vector->type()->element();
    }
    return Type::get_i32();
}

TypePtr reduction_result(Value *vector) {
    return lane_result(vector);
}

const char *mask_binary_name(MaskBinaryOp::Kind kind) {
    switch (kind) {
    case MaskBinaryOp::Kind::And:
        return "yir.mask.and";
    case MaskBinaryOp::Kind::Or:
        return "yir.mask.or";
    case MaskBinaryOp::Kind::Xor:
        return "yir.mask.xor";
    }
    return "yir.mask.invalid";
}

const char *mask_reduce_name(MaskReduceOp::Kind kind) {
    switch (kind) {
    case MaskReduceOp::Kind::Any:
        return "yir.mask.any";
    case MaskReduceOp::Kind::All:
        return "yir.mask.all";
    case MaskReduceOp::Kind::None:
        return "yir.mask.none";
    }
    return "yir.mask.invalid_reduce";
}

const char *vector_reduce_name(VectorReduceOp::Kind kind) {
    switch (kind) {
    case VectorReduceOp::Kind::Add:
        return "yir.vector.reduce_add";
    case VectorReduceOp::Kind::Mul:
        return "yir.vector.reduce_mul";
    case VectorReduceOp::Kind::Min:
        return "yir.vector.reduce_min";
    case VectorReduceOp::Kind::Max:
        return "yir.vector.reduce_max";
    case VectorReduceOp::Kind::And:
        return "yir.vector.reduce_and";
    case VectorReduceOp::Kind::Or:
        return "yir.vector.reduce_or";
    case VectorReduceOp::Kind::Xor:
        return "yir.vector.reduce_xor";
    }
    return "yir.vector.invalid_reduce";
}

} // namespace

AddIOp::AddIOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.addi", lhs, rhs, integer_binary_result(lhs), std::move(result_name)) {
}
SubIOp::SubIOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.subi", lhs, rhs, integer_binary_result(lhs), std::move(result_name)) {
}
MulIOp::MulIOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.muli", lhs, rhs, integer_binary_result(lhs), std::move(result_name)) {
}
DivSIOp::DivSIOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.divsi", lhs, rhs, integer_binary_result(lhs), std::move(result_name)) {
}
RemSIOp::RemSIOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.remsi", lhs, rhs, integer_binary_result(lhs), std::move(result_name)) {
}
AndIOp::AndIOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.andi", lhs, rhs, integer_binary_result(lhs), std::move(result_name)) {
}
OrIOp::OrIOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.ori", lhs, rhs, integer_binary_result(lhs), std::move(result_name)) {
}
XorIOp::XorIOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.xori", lhs, rhs, integer_binary_result(lhs), std::move(result_name)) {
}

AddFOp::AddFOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.addf", lhs, rhs, float_binary_result(lhs), std::move(result_name)) {
}
SubFOp::SubFOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.subf", lhs, rhs, float_binary_result(lhs), std::move(result_name)) {
}
MulFOp::MulFOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.mulf", lhs, rhs, float_binary_result(lhs), std::move(result_name)) {
}
DivFOp::DivFOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.divf", lhs, rhs, float_binary_result(lhs), std::move(result_name)) {
}

ICmpOp::ICmpOp(Predicate pred, Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.icmp", lhs, rhs, comparison_result(lhs), std::move(result_name)),
      predicate_(pred) {
}

FCmpOp::FCmpOp(Predicate pred, Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.fcmp", lhs, rhs, comparison_result(lhs), std::move(result_name)),
      predicate_(pred) {
}

ZExtI1ToI32Op::ZExtI1ToI32Op(Value *value, std::string result_name)
    : Operation("yir.zext_i1_to_i32", {value},
                value != nullptr && value->type() != nullptr && value->type()->is_mask()
                    ? Type::get_vector(value->type()->count(), Type::get_i32())
                    : Type::get_i32(),
                std::move(result_name)) {
}

TruncI32ToI1Op::TruncI32ToI1Op(Value *value, std::string result_name)
    : Operation("yir.trunc_i32_to_i1", {value},
                value != nullptr && value->type() != nullptr && value->type()->is_vector()
                    ? Type::get_mask(value->type()->count())
                    : Type::get_i1(),
                std::move(result_name)) {
}

SIToFPOp::SIToFPOp(Value *value, std::string result_name)
    : Operation("yir.sitofp", {value},
                value != nullptr && value->type() != nullptr && value->type()->is_vector()
                    ? Type::get_vector(value->type()->count(), Type::get_f32())
                    : Type::get_f32(),
                std::move(result_name)) {
}

FPToSIOp::FPToSIOp(Value *value, std::string result_name)
    : Operation("yir.fptosi", {value},
                value != nullptr && value->type() != nullptr && value->type()->is_vector()
                    ? Type::get_vector(value->type()->count(), Type::get_i32())
                    : Type::get_i32(),
                std::move(result_name)) {
}

ToBoolOp::ToBoolOp(Value *value, std::string result_name)
    : Operation("yir.to_bool", {value},
                value != nullptr && value->type() != nullptr && value->type()->is_vector()
                    ? Type::get_mask(value->type()->count())
                    : Type::get_i1(),
                std::move(result_name)) {
}

NotOp::NotOp(Value *value, std::string result_name)
    : Operation("yir.not", {value}, Type::get_i1(), std::move(result_name)) {
}

BitNotOp::BitNotOp(Value *value, std::string result_name)
    : Operation("yir.noti", {value}, value == nullptr ? Type::get_i32() : value->type(),
                std::move(result_name)) {
}

MaskBinaryOp::MaskBinaryOp(Kind kind, Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase(mask_binary_name(kind), lhs, rhs,
                   lhs == nullptr ? Type::get_mask(1) : lhs->type(), std::move(result_name)),
      kind_(kind) {
}

MaskNotOp::MaskNotOp(Value *mask, std::string result_name)
    : Operation("yir.mask.not", {mask}, mask == nullptr ? Type::get_mask(1) : mask->type(),
                std::move(result_name)) {
}

VectorCreateOp::VectorCreateOp(TypePtr result_type, std::vector<Value *> lanes,
                               std::string result_name)
    : Operation("yir.vector.create", std::move(lanes), std::move(result_type),
                std::move(result_name)) {
}

SplatOp::SplatOp(Value *scalar, TypePtr result_type, std::string result_name)
    : Operation("yir.vector.splat", {scalar}, std::move(result_type), std::move(result_name)) {
}

StepVectorOp::StepVectorOp(TypePtr result_type, std::string result_name)
    : Operation("yir.vector.step", {}, std::move(result_type), std::move(result_name)) {
}

ExtractLaneOp::ExtractLaneOp(Value *vector, Value *index, std::string result_name)
    : Operation("yir.vector.extract", {vector, index}, lane_result(vector),
                std::move(result_name)) {
}

InsertLaneOp::InsertLaneOp(Value *vector, Value *index, Value *lane, std::string result_name)
    : Operation("yir.vector.insert", {vector, index, lane},
                vector == nullptr ? Type::get_vector(1, Type::get_i32()) : vector->type(),
                std::move(result_name)) {
}

ShuffleOp::ShuffleOp(Value *lhs, Value *rhs, std::vector<std::uint64_t> indices,
                     TypePtr result_type, std::string result_name)
    : Operation("yir.vector.shuffle", {lhs, rhs}, std::move(result_type),
                std::move(result_name)),
      indices_(std::move(indices)) {
}

SelectOp::SelectOp(Value *mask, Value *true_value, Value *false_value,
                   std::string result_name)
    : Operation("yir.vector.select", {mask, true_value, false_value},
                true_value == nullptr ? Type::get_vector(1, Type::get_i32()) : true_value->type(),
                std::move(result_name)) {
}

VectorCastOp::VectorCastOp(Value *value, TypePtr result_type, std::string result_name)
    : Operation("yir.vector.cast", {value}, std::move(result_type), std::move(result_name)) {
}

MaskReduceOp::MaskReduceOp(Kind kind, Value *mask, std::string result_name)
    : Operation(mask_reduce_name(kind), {mask}, Type::get_i1(), std::move(result_name)),
      kind_(kind) {
}

VectorReduceOp::VectorReduceOp(Kind kind, Value *vector, bool ordered,
                               std::string result_name)
    : Operation(vector_reduce_name(kind), {vector}, reduction_result(vector),
                std::move(result_name)),
      kind_(kind), ordered_(ordered) {
}

MaskedLoadOp::MaskedLoadOp(Value *address, Value *mask, Value *passthrough,
                           std::uint64_t alignment, std::string result_name)
    : Operation("yir.vector.masked_load", {address, mask, passthrough},
                passthrough == nullptr ? Type::get_vector(1, Type::get_i32())
                                       : passthrough->type(),
                std::move(result_name)),
      alignment_(alignment) {
}

MaskedStoreOp::MaskedStoreOp(Value *value, Value *address, Value *mask,
                             std::uint64_t alignment)
    : Operation("yir.vector.masked_store", {value, address, mask}), alignment_(alignment) {
}

GatherOp::GatherOp(Value *base, Value *indices, Value *mask, Value *passthrough,
                   std::uint64_t alignment, std::string result_name)
    : Operation("yir.vector.gather", {base, indices, mask, passthrough},
                passthrough == nullptr ? Type::get_vector(1, Type::get_i32())
                                       : passthrough->type(),
                std::move(result_name)),
      alignment_(alignment) {
}

ScatterOp::ScatterOp(Value *value, Value *base, Value *indices, Value *mask,
                     std::uint64_t alignment)
    : Operation("yir.vector.scatter", {value, base, indices, mask}), alignment_(alignment) {
}

CallOp::CallOp(std::string callee, std::vector<Value *> args, TypePtr result_type,
               std::string result_name)
    : Operation("yir.call", std::move(args), std::move(result_type), std::move(result_name)),
      callee_(std::move(callee)) {
}

IfOp::IfOp(Value *condition) : Operation("yir.if", {condition}) {
}

WhileOp::WhileOp() : Operation("yir.while", {}) {
}

ForOp::ForOp(Value *induction_var, Value *lower_bound, Value *upper_bound, Value *step)
    : Operation("yir.for", {induction_var, lower_bound, upper_bound, step}) {
}

CondOp::CondOp(Value *condition) : Operation("yir.cond", {condition}) {
}

BreakOp::BreakOp() : Operation("yir.break", {}) {
}

ContinueOp::ContinueOp() : Operation("yir.continue", {}) {
}

ReturnOp::ReturnOp(Value *value) : Operation("yir.return", value == nullptr ? std::vector<Value *>{}
                                                                            : std::vector<Value *>{value}) {
}

TypePtr pointee_type(TypePtr address_type) {
    if (address_type == nullptr || !address_type->is_ptr()) {
        throw std::runtime_error("expected pointer type");
    }
    return address_type->pointee();
}

TypePtr element_type_after_indices(TypePtr address_type, std::size_t index_count) {
    TypePtr current = pointee_type(std::move(address_type));
    for (std::size_t i = 0; i < index_count; ++i) {
        if (current->is_array()) {
            current = current->element();
        }
    }
    return current;
}

TypePtr array_element_type_after_indices(TypePtr array_type, std::size_t index_count) {
    TypePtr current = std::move(array_type);
    for (std::size_t i = 0; i < index_count; ++i) {
        if (current->is_array()) {
            current = current->element();
        }
    }
    return current;
}

std::size_t array_rank(TypePtr type) {
    std::size_t rank = 0;
    while (type != nullptr && type->is_array()) {
        ++rank;
        type = type->element();
    }
    return rank;
}

} // namespace yir
