#include "yir/YIR.h"

#include <sstream>
#include <stdexcept>

namespace yir {

Type::Type(Kind kind) : kind_(kind) {
}

Type::Type(Kind kind, TypePtr nested) : kind_(kind), pointee_(std::move(nested)) {
}

Type::Type(std::uint64_t count, TypePtr element)
    : kind_(Kind::Array), element_(std::move(element)), count_(count) {
}

Type::Type(std::vector<TypePtr> params, TypePtr result)
    : kind_(Kind::Func), params_(std::move(params)), result_(std::move(result)) {
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
    return TypePtr(new Type(Kind::Ptr, std::move(pointee)));
}

TypePtr Type::get_array(std::uint64_t count, TypePtr element) {
    return TypePtr(new Type(count, std::move(element)));
}

TypePtr Type::get_func(std::vector<TypePtr> params, TypePtr result) {
    return TypePtr(new Type(std::move(params), std::move(result)));
}

bool Type::is_integer() const {
    return kind_ == Kind::I1 || kind_ == Kind::I32;
}

bool Type::is_float() const {
    return kind_ == Kind::F32;
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
    case Kind::Func: {
        std::ostringstream oss;
        oss << "func<(";
        for (std::size_t i = 0; i < params_.size(); ++i) {
            if (i != 0) {
                oss << ", ";
            }
            oss << params_[i]->str();
        }
        oss << ") -> " << result_->str() << ">";
        return oss.str();
    }
    }
    return "<unknown>";
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

Function::Function(std::string name, TypePtr return_type, std::vector<TypePtr> param_types)
    : name_(std::move(name)), return_type_(std::move(return_type)),
      param_types_(std::move(param_types)) {
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

Function *Module::add_function(std::string name, TypePtr return_type, std::vector<TypePtr> param_types) {
    auto function =
        std::make_unique<Function>(std::move(name), std::move(return_type), std::move(param_types));
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

AddIOp::AddIOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.addi", lhs, rhs, Type::get_i32(), std::move(result_name)) {
}
SubIOp::SubIOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.subi", lhs, rhs, Type::get_i32(), std::move(result_name)) {
}
MulIOp::MulIOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.muli", lhs, rhs, Type::get_i32(), std::move(result_name)) {
}
DivSIOp::DivSIOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.divsi", lhs, rhs, Type::get_i32(), std::move(result_name)) {
}
RemSIOp::RemSIOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.remsi", lhs, rhs, Type::get_i32(), std::move(result_name)) {
}

AddFOp::AddFOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.addf", lhs, rhs, Type::get_f32(), std::move(result_name)) {
}
SubFOp::SubFOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.subf", lhs, rhs, Type::get_f32(), std::move(result_name)) {
}
MulFOp::MulFOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.mulf", lhs, rhs, Type::get_f32(), std::move(result_name)) {
}
DivFOp::DivFOp(Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.divf", lhs, rhs, Type::get_f32(), std::move(result_name)) {
}

ICmpOp::ICmpOp(Predicate pred, Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.icmp", lhs, rhs, Type::get_i1(), std::move(result_name)),
      predicate_(pred) {
}

FCmpOp::FCmpOp(Predicate pred, Value *lhs, Value *rhs, std::string result_name)
    : BinaryOpBase("yir.fcmp", lhs, rhs, Type::get_i1(), std::move(result_name)),
      predicate_(pred) {
}

ZExtI1ToI32Op::ZExtI1ToI32Op(Value *value, std::string result_name)
    : Operation("yir.zext_i1_to_i32", {value}, Type::get_i32(), std::move(result_name)) {
}

TruncI32ToI1Op::TruncI32ToI1Op(Value *value, std::string result_name)
    : Operation("yir.trunc_i32_to_i1", {value}, Type::get_i1(), std::move(result_name)) {
}

SIToFPOp::SIToFPOp(Value *value, std::string result_name)
    : Operation("yir.sitofp", {value}, Type::get_f32(), std::move(result_name)) {
}

FPToSIOp::FPToSIOp(Value *value, std::string result_name)
    : Operation("yir.fptosi", {value}, Type::get_i32(), std::move(result_name)) {
}

ToBoolOp::ToBoolOp(Value *value, std::string result_name)
    : Operation("yir.to_bool", {value}, Type::get_i1(), std::move(result_name)) {
}

NotOp::NotOp(Value *value, std::string result_name)
    : Operation("yir.not", {value}, Type::get_i1(), std::move(result_name)) {
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
