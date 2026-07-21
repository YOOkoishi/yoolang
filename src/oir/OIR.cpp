#include "oir/OIR.h"

#include "oir/OIRAnalysis.h"

#include <algorithm>
#include <functional>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace oir {

namespace {

std::string type_key(const Type *type) {
    return type == nullptr ? "<null>" : type->print();
}

std::string op_to_string(Instruction::OpID op) {
    switch (op) {
    case Instruction::OpID::Ret:
        return "ret";
    case Instruction::OpID::Br:
        return "br";
    case Instruction::OpID::Add:
        return "add";
    case Instruction::OpID::Sub:
        return "sub";
    case Instruction::OpID::Mul:
        return "mul";
    case Instruction::OpID::SDiv:
        return "sdiv";
    case Instruction::OpID::SRem:
        return "srem";
    case Instruction::OpID::FAdd:
        return "fadd";
    case Instruction::OpID::FSub:
        return "fsub";
    case Instruction::OpID::FMul:
        return "fmul";
    case Instruction::OpID::FDiv:
        return "fdiv";
    case Instruction::OpID::ICmp:
        return "icmp";
    case Instruction::OpID::FCmp:
        return "fcmp";
    case Instruction::OpID::Alloca:
        return "alloca";
    case Instruction::OpID::Load:
        return "load";
    case Instruction::OpID::Store:
        return "store";
    case Instruction::OpID::GetElementPtr:
        return "gep";
    case Instruction::OpID::Call:
        return "call";
    case Instruction::OpID::MemZero:
        return "memzero";
    case Instruction::OpID::ZExt:
        return "zext";
    case Instruction::OpID::SIToFP:
        return "sitofp";
    case Instruction::OpID::FPToSI:
        return "fptosi";
    case Instruction::OpID::Phi:
        return "phi";
    case Instruction::OpID::And:
        return "and";
    case Instruction::OpID::Xor:
        return "xor";
    }
    return "unknown";
}

std::string cmp_pred_to_string(CmpPred pred) {
    switch (pred) {
    case CmpPred::EQ:
        return "eq";
    case CmpPred::NE:
        return "ne";
    case CmpPred::LT:
        return "lt";
    case CmpPred::LE:
        return "le";
    case CmpPred::GT:
        return "gt";
    case CmpPred::GE:
        return "ge";
    }
    return "?";
}

std::string value_ref(const Value *value) {
    if (value == nullptr) {
        return "<null>";
    }

    if (dynamic_cast<const ConstantInt *>(value) != nullptr ||
        dynamic_cast<const ConstantFloat *>(value) != nullptr ||
        dynamic_cast<const ConstantZero *>(value) != nullptr ||
        dynamic_cast<const UndefValue *>(value) != nullptr) {
        return value->print();
    }

    if (dynamic_cast<const BasicBlock *>(value) != nullptr) {
        return "%" + value->name();
    }

    if (dynamic_cast<const Function *>(value) != nullptr ||
        dynamic_cast<const GlobalVariable *>(value) != nullptr) {
        return "@" + value->name();
    }

    if (!value->name().empty()) {
        return "%" + value->name();
    }

    return "<tmp>";
}

std::string prefix_name(const Value *value) {
    if (value == nullptr || value->name().empty()) {
        return "";
    }
    return "%" + value->name() + " = ";
}

std::string typed_value_ref(const Value *value) {
    if (value == nullptr || value->type() == nullptr) {
        return "<null>";
    }
    return value->type()->print() + " " + value_ref(value);
}

std::string function_ref(const Function *function) {
    return function == nullptr ? "<null function>" : "@" + function->name();
}

std::string block_ref(const BasicBlock *block) {
    return block == nullptr ? "<null block>" : "%" + block->name();
}

std::string inst_ref(const Instruction *inst) {
    if (inst == nullptr) {
        return "<null inst>";
    }
    if (!inst->name().empty()) {
        return "%" + inst->name();
    }
    return op_to_string(inst->op()) + " in " + block_ref(inst->parent());
}

struct UseKey {
    const User *user = nullptr;
    std::size_t operand_index = 0;

    bool operator==(const UseKey &other) const {
        return user == other.user && operand_index == other.operand_index;
    }
};

struct UseKeyHash {
    std::size_t operator()(const UseKey &key) const {
        return std::hash<const void *>{}(key.user) ^ (std::hash<std::size_t>{}(key.operand_index)
                                                      << 1U);
    }
};

bool contains_block_ptr(const std::vector<BasicBlock *> &blocks, const BasicBlock *needle) {
    return std::find(blocks.begin(), blocks.end(), needle) != blocks.end();
}

bool has_duplicate_block(const std::vector<BasicBlock *> &blocks) {
    std::unordered_set<const BasicBlock *> seen;
    for (auto *block : blocks) {
        if (!seen.insert(block).second) {
            return true;
        }
    }
    return false;
}

std::vector<BasicBlock *> branch_targets(const BranchInst &branch) {
    std::vector<BasicBlock *> targets;
    auto add_unique = [&targets](BasicBlock *target) {
        if (target != nullptr &&
            std::find(targets.begin(), targets.end(), target) == targets.end()) {
            targets.push_back(target);
        }
    };

    if (branch.is_conditional()) {
        add_unique(branch.true_bb());
        add_unique(branch.false_bb());
    } else {
        add_unique(branch.target_bb());
    }
    return targets;
}

} // namespace

Type::Type(TypeID id) : id_(id) {
}

Type::TypeID Type::id() const {
    return id_;
}

bool Type::is_void() const {
    return id_ == TypeID::Void;
}

bool Type::is_label() const {
    return id_ == TypeID::Label;
}

bool Type::is_integer() const {
    return id_ == TypeID::Integer;
}

bool Type::is_float() const {
    return id_ == TypeID::Float;
}

bool Type::is_pointer() const {
    return id_ == TypeID::Pointer;
}

bool Type::is_array() const {
    return id_ == TypeID::Array;
}

bool Type::is_function() const {
    return id_ == TypeID::Function;
}

VoidType::VoidType() : Type(TypeID::Void) {
}

std::string VoidType::print() const {
    return "void";
}

LabelType::LabelType() : Type(TypeID::Label) {
}

std::string LabelType::print() const {
    return "label";
}

IntegerType::IntegerType(std::size_t bit_width) : Type(TypeID::Integer), bit_width_(bit_width) {
}

std::size_t IntegerType::bit_width() const {
    return bit_width_;
}

std::string IntegerType::print() const {
    return "i" + std::to_string(bit_width_);
}

FloatType::FloatType() : Type(TypeID::Float) {
}

std::string FloatType::print() const {
    return "float";
}

PointerType::PointerType(Type *element_type) : Type(TypeID::Pointer), element_type_(element_type) {
}

Type *PointerType::element_type() const {
    return element_type_;
}

std::string PointerType::print() const {
    return element_type_->print() + "*";
}

ArrayType::ArrayType(Type *element_type, std::size_t element_count)
    : Type(TypeID::Array), element_type_(element_type), element_count_(element_count) {
}

Type *ArrayType::element_type() const {
    return element_type_;
}

std::size_t ArrayType::element_count() const {
    return element_count_;
}

std::string ArrayType::print() const {
    return "[" + std::to_string(element_count_) + " x " + element_type_->print() + "]";
}

FunctionType::FunctionType(Type *return_type, std::vector<Type *> param_types)
    : Type(TypeID::Function), return_type_(return_type), param_types_(std::move(param_types)) {
}

Type *FunctionType::return_type() const {
    return return_type_;
}

const std::vector<Type *> &FunctionType::param_types() const {
    return param_types_;
}

std::string FunctionType::print() const {
    std::ostringstream oss;
    oss << return_type_->print() << " (";
    for (std::size_t i = 0; i < param_types_.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << param_types_[i]->print();
    }
    oss << ")";
    return oss.str();
}

TypeContext::TypeContext()
    : void_ty_(std::make_unique<VoidType>()), label_ty_(std::make_unique<LabelType>()),
      int1_ty_(std::make_unique<IntegerType>(1)), int32_ty_(std::make_unique<IntegerType>(32)),
      float_ty_(std::make_unique<FloatType>()) {
}

VoidType *TypeContext::void_ty() const {
    return void_ty_.get();
}

LabelType *TypeContext::label_ty() const {
    return label_ty_.get();
}

IntegerType *TypeContext::int1_ty() const {
    return int1_ty_.get();
}

IntegerType *TypeContext::int32_ty() const {
    return int32_ty_.get();
}

FloatType *TypeContext::float_ty() const {
    return float_ty_.get();
}

PointerType *TypeContext::ptr_ty(Type *element_type) {
    std::string key = type_key(element_type) + "*";
    auto found = pointer_types_.find(key);
    if (found != pointer_types_.end()) {
        return found->second;
    }

    auto ptr = std::make_unique<PointerType>(element_type);
    auto *raw = ptr.get();
    owned_composite_types_.push_back(std::move(ptr));
    pointer_types_[std::move(key)] = raw;
    return raw;
}

ArrayType *TypeContext::array_ty(Type *element_type, std::size_t element_count) {
    std::string key = "[" + std::to_string(element_count) + " x " + type_key(element_type) + "]";
    auto found = array_types_.find(key);
    if (found != array_types_.end()) {
        return found->second;
    }

    auto arr = std::make_unique<ArrayType>(element_type, element_count);
    auto *raw = arr.get();
    owned_composite_types_.push_back(std::move(arr));
    array_types_[std::move(key)] = raw;
    return raw;
}

FunctionType *TypeContext::func_ty(Type *return_type, const std::vector<Type *> &param_types) {
    std::ostringstream key;
    key << type_key(return_type) << "(";
    for (std::size_t i = 0; i < param_types.size(); ++i) {
        if (i != 0) {
            key << ",";
        }
        key << type_key(param_types[i]);
    }
    key << ")";
    auto key_string = key.str();
    auto found = function_types_.find(key_string);
    if (found != function_types_.end()) {
        return found->second;
    }

    auto fn = std::make_unique<FunctionType>(return_type, param_types);
    auto *raw = fn.get();
    owned_composite_types_.push_back(std::move(fn));
    function_types_[std::move(key_string)] = raw;
    return raw;
}

Value::Value(Type *type, const std::string &name) : type_(type), name_(name) {
}

Type *Value::type() const {
    return type_;
}

const std::string &Value::name() const {
    return name_;
}

void Value::set_name(const std::string &name) {
    name_ = name;
}

void Value::set_type(Type *type) {
    type_ = type;
}

const std::vector<Value::Use> &Value::uses() const {
    return uses_;
}

std::vector<User *> Value::users() const {
    std::vector<User *> out;
    out.reserve(uses_.size());
    for (const auto &use : uses_) {
        out.push_back(use.user);
    }
    return out;
}

std::size_t Value::use_count() const {
    return uses_.size();
}

bool Value::has_uses() const {
    return !uses_.empty();
}

void Value::reserve_additional_uses(std::size_t additional) {
    uses_.reserve(uses_.size() + additional);
}

void Value::replace_all_uses_with(Value *new_value) {
    if (new_value == nullptr || new_value == this || new_value->type() != type()) {
        return;
    }

    auto worklist = uses_;
    for (const auto &use : worklist) {
        if (use.user != nullptr && use.operand_index < use.user->operand_count() &&
            use.user->operand(use.operand_index) == this) {
            use.user->set_operand(use.operand_index, new_value);
        }
    }
}

void Value::add_use(User *user, std::size_t operand_index) {
    uses_.push_back({user, operand_index});
}

void Value::remove_use(User *user, std::size_t operand_index) {
    auto found = std::find_if(uses_.begin(), uses_.end(), [user, operand_index](const Use &use) {
        return use.user == user && use.operand_index == operand_index;
    });
    if (found != uses_.end()) {
        uses_.erase(found);
    }
}

ConstantInt::ConstantInt(Type *type, std::int64_t value)
    : Value(type, std::to_string(value)), value_(value) {
}

std::int64_t ConstantInt::value() const {
    return value_;
}

std::string ConstantInt::print() const {
    return std::to_string(value_);
}

ConstantFloat::ConstantFloat(Type *type, float value)
    : Value(type, std::to_string(value)), value_(value) {
}

float ConstantFloat::value() const {
    return value_;
}

std::string ConstantFloat::print() const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << value_;
    return oss.str();
}

ConstantZero::ConstantZero(Type *type) : Value(type, "zero") {
}

std::string ConstantZero::print() const {
    return "zero";
}

UndefValue::UndefValue(Type *type) : Value(type, "undef") {
}

std::string UndefValue::print() const {
    return "undef";
}

User::User(Type *type, const std::string &name) : Value(type, name) {
}

User::~User() {
    drop_all_operands();
}

void User::add_operand(Value *value) {
    if (value != nullptr) {
        value->reserve_additional_uses(1);
    }
    operands_.push_back(value);
    if (value != nullptr) {
        value->add_use(this, operands_.size() - 1);
    }
}

Value *User::operand(std::size_t index) const {
    return operands_[index];
}

void User::set_operand(std::size_t index, Value *value) {
    auto *old_value = operands_[index];
    if (old_value == value) {
        return;
    }
    if (value != nullptr) {
        // Allocate before mutating either use-list so failure leaves the old
        // operand and both use lists unchanged.
        value->reserve_additional_uses(1);
    }
    if (old_value != nullptr) {
        old_value->remove_use(this, index);
    }
    operands_[index] = value;
    if (value != nullptr) {
        value->add_use(this, index);
    }
}

void User::replace_operand(Value *old_value, Value *new_value) {
    replace_operands(old_value, new_value);
}

std::size_t User::replace_operands(Value *old_value, Value *new_value) {
    std::size_t replaced = 0;
    for (std::size_t i = 0; i < operands_.size(); ++i) {
        if (operands_[i] == old_value) {
            set_operand(i, new_value);
            ++replaced;
        }
    }
    return replaced;
}

void User::drop_all_operands() {
    for (std::size_t i = 0; i < operands_.size(); ++i) {
        if (operands_[i] != nullptr) {
            operands_[i]->remove_use(this, i);
        }
    }
    operands_.clear();
}

void User::erase_operands(std::size_t first, std::size_t count) {
    if (first >= operands_.size() || count == 0) {
        return;
    }

    const auto last = std::min(first + count, operands_.size());
    for (std::size_t i = first; i < last; ++i) {
        if (operands_[i] != nullptr) {
            operands_[i]->remove_use(this, i);
        }
    }
    for (std::size_t i = last; i < operands_.size(); ++i) {
        if (operands_[i] != nullptr) {
            operands_[i]->remove_use(this, i);
        }
    }

    operands_.erase(operands_.begin() + static_cast<std::ptrdiff_t>(first),
                    operands_.begin() + static_cast<std::ptrdiff_t>(last));

    for (std::size_t i = first; i < operands_.size(); ++i) {
        if (operands_[i] != nullptr) {
            operands_[i]->add_use(this, i);
        }
    }
}

std::size_t User::operand_count() const {
    return operands_.size();
}

const std::vector<Value *> &User::operands() const {
    return operands_;
}

Instruction::Instruction(Type *type, OpID op, BasicBlock *parent, const std::string &name)
    : User(type, name), op_(op), parent_(parent) {
}

Instruction::OpID Instruction::op() const {
    return op_;
}

BasicBlock *Instruction::parent() const {
    return parent_;
}

void Instruction::set_parent(BasicBlock *parent) {
    parent_ = parent;
}

bool Instruction::is_terminator() const {
    return op_ == OpID::Ret || op_ == OpID::Br;
}

BinaryInst::BinaryInst(Type *type, OpID op, Value *lhs, Value *rhs, BasicBlock *parent,
                       const std::string &name)
    : Instruction(type, op, parent, name) {
    add_operand(lhs);
    add_operand(rhs);
}

Value *BinaryInst::lhs() const {
    return operand(0);
}

Value *BinaryInst::rhs() const {
    return operand(1);
}

std::string BinaryInst::print() const {
    return prefix_name(this) + op_to_string(op()) + " " + typed_value_ref(lhs()) + ", " +
           value_ref(rhs());
}

CmpInst::CmpInst(Type *result_type, OpID op, CmpPred pred, Value *lhs, Value *rhs,
                 BasicBlock *parent, const std::string &name)
    : Instruction(result_type, op, parent, name), pred_(pred) {
    add_operand(lhs);
    add_operand(rhs);
}

CmpPred CmpInst::pred() const {
    return pred_;
}

Value *CmpInst::lhs() const {
    return operand(0);
}

Value *CmpInst::rhs() const {
    return operand(1);
}

std::string CmpInst::print() const {
    return prefix_name(this) + op_to_string(op()) + " " + cmp_pred_to_string(pred_) + " " +
           typed_value_ref(lhs()) + ", " + value_ref(rhs());
}

CastInst::CastInst(Type *dst_type, OpID op, Value *src, BasicBlock *parent, const std::string &name)
    : Instruction(dst_type, op, parent, name) {
    add_operand(src);
}

Value *CastInst::src() const {
    return operand(0);
}

std::string CastInst::print() const {
    return prefix_name(this) + op_to_string(op()) + " " + typed_value_ref(src()) + " to " +
           type()->print();
}

AllocaInst::AllocaInst(Type *ptr_type, Type *allocated_type, BasicBlock *parent,
                       const std::string &name)
    : Instruction(ptr_type, OpID::Alloca, parent, name), allocated_type_(allocated_type) {
}

Type *AllocaInst::allocated_type() const {
    return allocated_type_;
}

std::string AllocaInst::print() const {
    return prefix_name(this) + "alloca " + allocated_type_->print();
}

GetElementPtrInst::GetElementPtrInst(Type *ptr_type, Value *base_ptr,
                                     const std::vector<Value *> &indices, BasicBlock *parent,
                                     const std::string &name)
    : Instruction(ptr_type, OpID::GetElementPtr, parent, name) {
    add_operand(base_ptr);
    for (auto *idx : indices) {
        add_operand(idx);
    }
}

Value *GetElementPtrInst::base_ptr() const {
    return operand(0);
}

std::vector<Value *> GetElementPtrInst::indices() const {
    std::vector<Value *> out;
    for (std::size_t i = 1; i < operand_count(); ++i) {
        out.push_back(operand(i));
    }
    return out;
}

std::string GetElementPtrInst::print() const {
    std::ostringstream oss;
    oss << prefix_name(this) << "gep " << typed_value_ref(base_ptr());
    for (std::size_t i = 1; i < operand_count(); ++i) {
        oss << ", " << typed_value_ref(operand(i));
    }
    return oss.str();
}

LoadInst::LoadInst(Type *loaded_type, Value *ptr, BasicBlock *parent, const std::string &name)
    : Instruction(loaded_type, OpID::Load, parent, name) {
    add_operand(ptr);
}

Value *LoadInst::ptr() const {
    return operand(0);
}

std::string LoadInst::print() const {
    return prefix_name(this) + "load " + type()->print() + ", " + typed_value_ref(ptr());
}

StoreInst::StoreInst(Type *void_type, Value *value, Value *ptr, BasicBlock *parent)
    : Instruction(void_type, OpID::Store, parent, "") {
    add_operand(value);
    add_operand(ptr);
}

Value *StoreInst::value() const {
    return operand(0);
}

Value *StoreInst::ptr() const {
    return operand(1);
}

std::string StoreInst::print() const {
    return "store " + typed_value_ref(value()) + ", " + typed_value_ref(ptr());
}

MemZeroInst::MemZeroInst(Type *void_type, Value *ptr, Value *byte_value, Value *byte_count,
                         BasicBlock *parent)
    : Instruction(void_type, OpID::MemZero, parent, "") {
    add_operand(ptr);
    add_operand(byte_value);
    add_operand(byte_count);
}

Value *MemZeroInst::ptr() const {
    return operand(0);
}

Value *MemZeroInst::byte_value() const {
    return operand(1);
}

Value *MemZeroInst::byte_count() const {
    return operand(2);
}

std::string MemZeroInst::print() const {
    if (auto *byte = dynamic_cast<ConstantInt *>(byte_value())) {
        if (byte->value() == 0) {
            return "memzero " + typed_value_ref(ptr()) + ", " + typed_value_ref(byte_count());
        }
    }
    return "memset " + typed_value_ref(ptr()) + ", " + typed_value_ref(byte_value()) + ", " +
           typed_value_ref(byte_count());
}

CallInst::CallInst(Type *return_type, Value *callee, const std::vector<Value *> &args,
                   BasicBlock *parent, const std::string &name)
    : Instruction(return_type, OpID::Call, parent, name) {
    add_operand(callee);
    for (auto *arg : args) {
        add_operand(arg);
    }
}

Value *CallInst::callee() const {
    return operand(0);
}

std::vector<Value *> CallInst::args() const {
    std::vector<Value *> out;
    for (std::size_t i = 1; i < operand_count(); ++i) {
        out.push_back(operand(i));
    }
    return out;
}

void CallInst::remove_arg(std::size_t arg_index) {
    erase_operands(arg_index + 1, 1);
}

void CallInst::reset_callee_and_args(Value *callee, const std::vector<Value *> &args) {
    operands_.reserve(args.size() + 1);
    drop_all_operands();
    add_operand(callee);
    for (auto *arg : args) {
        add_operand(arg);
    }
}

std::string CallInst::print() const {
    std::ostringstream oss;
    if (!name().empty()) {
        oss << "%" << name() << " = ";
    }
    oss << "call " << type()->print() << " " << value_ref(callee()) << "(";
    for (std::size_t i = 1; i < operand_count(); ++i) {
        if (i != 1) {
            oss << ", ";
        }
        oss << typed_value_ref(operand(i));
    }
    oss << ")";
    return oss.str();
}

ReturnInst::ReturnInst(Type *void_type, Value *value, BasicBlock *parent)
    : Instruction(void_type, OpID::Ret, parent, "") {
    if (value != nullptr) {
        add_operand(value);
    }
}

bool ReturnInst::has_value() const {
    return operand_count() == 1;
}

Value *ReturnInst::value() const {
    if (!has_value()) {
        return nullptr;
    }
    return operand(0);
}

std::string ReturnInst::print() const {
    if (!has_value()) {
        return "ret void";
    }
    return "ret " + typed_value_ref(value());
}

BranchInst::BranchInst(Type *void_type, BasicBlock *target, BasicBlock *parent)
    : Instruction(void_type, OpID::Br, parent, "") {
    add_operand(target);
}

BranchInst::BranchInst(Type *void_type, Value *cond, BasicBlock *true_bb, BasicBlock *false_bb,
                       BasicBlock *parent)
    : Instruction(void_type, OpID::Br, parent, "") {
    add_operand(cond);
    add_operand(true_bb);
    add_operand(false_bb);
}

bool BranchInst::is_conditional() const {
    return operand_count() == 3;
}

Value *BranchInst::cond() const {
    if (!is_conditional()) {
        return nullptr;
    }
    return operand(0);
}

BasicBlock *BranchInst::true_bb() const {
    if (!is_conditional()) {
        return nullptr;
    }
    return static_cast<BasicBlock *>(operand(1));
}

BasicBlock *BranchInst::false_bb() const {
    if (!is_conditional()) {
        return nullptr;
    }
    return static_cast<BasicBlock *>(operand(2));
}

BasicBlock *BranchInst::target_bb() const {
    if (is_conditional()) {
        return nullptr;
    }
    return static_cast<BasicBlock *>(operand(0));
}

std::string BranchInst::print() const {
    if (is_conditional()) {
        return "br " + typed_value_ref(cond()) + ", " + value_ref(true_bb()) + ", " +
               value_ref(false_bb());
    }
    return "br " + value_ref(target_bb());
}

PhiInst::PhiInst(Type *type, BasicBlock *parent, const std::string &name)
    : Instruction(type, OpID::Phi, parent, name) {
}

void PhiInst::add_incoming(Value *value, BasicBlock *from) {
    incoming_.push_back({value, from});
    add_operand(value);
    add_operand(from);
}

void PhiInst::remove_incoming_from(BasicBlock *from) {
    for (std::size_t i = 0; i < incoming_.size();) {
        if (incoming_[i].second != from) {
            ++i;
            continue;
        }
        incoming_.erase(incoming_.begin() + static_cast<std::ptrdiff_t>(i));
        erase_operands(i * 2, 2);
    }
}

const std::vector<std::pair<Value *, BasicBlock *>> &PhiInst::incoming() const {
    return incoming_;
}

void PhiInst::set_operand(std::size_t index, Value *value) {
    User::set_operand(index, value);
    const std::size_t incoming_index = index / 2;
    if (incoming_index >= incoming_.size()) {
        return;
    }
    if (index % 2 == 0) {
        incoming_[incoming_index].first = value;
    } else {
        incoming_[incoming_index].second = static_cast<BasicBlock *>(value);
    }
}

std::string PhiInst::print() const {
    std::ostringstream oss;
    oss << prefix_name(this) << "phi ";
    for (std::size_t i = 0; i < incoming_.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << "[" << value_ref(incoming_[i].first) << ", " << value_ref(incoming_[i].second)
            << "]";
    }
    return oss.str();
}

Argument::Argument(Type *type, const std::string &name, Function *parent, std::size_t index)
    : Value(type, name), parent_(parent), index_(index) {
}

Function *Argument::parent() const {
    return parent_;
}

std::size_t Argument::index() const {
    return index_;
}

void Argument::set_parent(Function *parent) {
    parent_ = parent;
}

void Argument::set_index(std::size_t index) {
    index_ = index;
}

std::string Argument::print() const {
    return type()->print() + " %" + name();
}

BasicBlock::BasicBlock(Type *label_type, const std::string &name, Function *parent)
    : Value(label_type, name), parent_(parent) {
}

Instruction *BasicBlock::append_instruction(std::unique_ptr<Instruction> inst) {
    auto *raw = inst.get();
    raw->set_parent(this);
    instructions_.push_back(std::move(inst));
    return raw;
}

Instruction *BasicBlock::insert_before_terminator(std::unique_ptr<Instruction> inst) {
    auto *raw = inst.get();
    raw->set_parent(this);
    if (has_terminator()) {
        instructions_.insert(std::prev(instructions_.end()), std::move(inst));
    } else {
        instructions_.push_back(std::move(inst));
    }
    return raw;
}

bool BasicBlock::has_terminator() const {
    if (instructions_.empty()) {
        return false;
    }
    return instructions_.back()->is_terminator();
}

Instruction *BasicBlock::terminator() const {
    if (!has_terminator()) {
        return nullptr;
    }
    return instructions_.back().get();
}

void BasicBlock::add_predecessor(BasicBlock *pred) {
    if (std::find(predecessors_.begin(), predecessors_.end(), pred) == predecessors_.end()) {
        predecessors_.push_back(pred);
    }
}

void BasicBlock::add_successor(BasicBlock *succ) {
    if (std::find(successors_.begin(), successors_.end(), succ) == successors_.end()) {
        successors_.push_back(succ);
    }
}

void BasicBlock::remove_predecessor(BasicBlock *pred) {
    predecessors_.erase(std::remove(predecessors_.begin(), predecessors_.end(), pred),
                        predecessors_.end());
}

void BasicBlock::remove_successor(BasicBlock *succ) {
    successors_.erase(std::remove(successors_.begin(), successors_.end(), succ),
                      successors_.end());
}

const std::vector<BasicBlock *> &BasicBlock::predecessors() const {
    return predecessors_;
}

const std::vector<BasicBlock *> &BasicBlock::successors() const {
    return successors_;
}

std::list<std::unique_ptr<Instruction>> &BasicBlock::instructions() {
    return instructions_;
}

const std::list<std::unique_ptr<Instruction>> &BasicBlock::instructions() const {
    return instructions_;
}

Function *BasicBlock::parent() const {
    return parent_;
}

void BasicBlock::set_parent(Function *parent) {
    parent_ = parent;
}

std::string BasicBlock::print() const {
    return name() + ":";
}

Function::Function(FunctionType *type, const std::string &name, Module *parent, bool is_external,
                   FunctionID function_id, FunctionOrigin origin, FunctionID root_function_id)
    : Value(type, name), parent_(parent), is_external_(is_external), function_id_(function_id),
      root_function_id_(root_function_id == kInvalidFunctionID ? function_id : root_function_id),
      origin_(origin), next_block_id_(0) {
}

Function::~Function() {
    for (auto &block : blocks_) {
        for (auto &inst : block->instructions()) {
            inst->drop_all_operands();
        }
    }
}

ModuleFunctionSet::ModuleFunctionSet(ModuleFunctionSet &&other) noexcept
    : functions(std::move(other.functions)),
      function_table(std::move(other.function_table)) {
}

ModuleFunctionSet &ModuleFunctionSet::operator=(ModuleFunctionSet &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    drop_all_references();
    functions = std::move(other.functions);
    function_table = std::move(other.function_table);
    return *this;
}

ModuleFunctionSet::~ModuleFunctionSet() {
    drop_all_references();
}

void ModuleFunctionSet::drop_all_references() noexcept {
    for (auto &function : functions) {
        if (function == nullptr) {
            continue;
        }
        for (auto &block : function->blocks()) {
            for (auto &instruction : block->instructions()) {
                instruction->drop_all_operands();
            }
        }
    }
}

FunctionType *Function::function_type() const {
    return static_cast<FunctionType *>(type());
}

Type *Function::return_type() const {
    return function_type()->return_type();
}

Module *Function::parent() const {
    return parent_;
}

bool Function::is_external() const {
    return is_external_;
}

void Function::set_external(bool is_external) {
    is_external_ = is_external;
}

void Function::set_function_type(FunctionType *type) {
    set_type(type);
}

FunctionID Function::function_id() const {
    return function_id_;
}

FunctionID Function::root_function_id() const {
    return root_function_id_;
}

FunctionOrigin Function::origin() const {
    return origin_;
}

Argument *Function::add_argument(Type *type, const std::string &name) {
    auto arg = std::make_unique<Argument>(type, name, this, args_.size());
    auto *raw = arg.get();
    args_.push_back(std::move(arg));
    return raw;
}

void Function::keep_arguments(const std::vector<bool> &keep) {
    if (keep.size() != args_.size()) {
        return;
    }
    std::vector<std::unique_ptr<Argument>> kept;
    kept.reserve(args_.size());
    for (std::size_t i = 0; i < args_.size(); ++i) {
        if (!keep[i]) {
            continue;
        }
        args_[i]->set_index(kept.size());
        kept.push_back(std::move(args_[i]));
    }
    args_ = std::move(kept);
}

BasicBlock *Function::create_block(const std::string &name) {
    // 始终生成唯一名：原名 + 计数器
    std::string block_name = name.empty() ? "bb" : name;
    block_name += "." + std::to_string(next_block_id_++);
    auto block = std::make_unique<BasicBlock>(parent_->types().label_ty(), block_name, this);
    auto *raw = block.get();
    blocks_.push_back(std::move(block));
    return raw;
}

void Function::erase_block(BasicBlock *block) {
    auto it = std::find_if(
        blocks_.begin(), blocks_.end(),
        [block](const std::unique_ptr<BasicBlock> &candidate) { return candidate.get() == block; });
    if (it != blocks_.end()) {
        for (auto &inst : (*it)->instructions()) {
            inst->drop_all_operands();
        }
        blocks_.erase(it);
    }
}

BasicBlock *Function::entry_block() const {
    if (blocks_.empty()) {
        return nullptr;
    }
    return blocks_.front().get();
}

std::vector<std::unique_ptr<Argument>> &Function::args() {
    return args_;
}

const std::vector<std::unique_ptr<Argument>> &Function::args() const {
    return args_;
}

std::list<std::unique_ptr<BasicBlock>> &Function::blocks() {
    return blocks_;
}

const std::list<std::unique_ptr<BasicBlock>> &Function::blocks() const {
    return blocks_;
}

std::size_t Function::block_allocator_state() const {
    return next_block_id_;
}

void Function::set_block_allocator_state(std::size_t state) {
    next_block_id_ = state;
}

void Function::swap_body(Function &other) noexcept {
    auto *this_type = function_type();
    set_function_type(other.function_type());
    other.set_function_type(this_type);
    args_.swap(other.args_);
    blocks_.swap(other.blocks_);
    std::swap(next_block_id_, other.next_block_id_);
    auto reparent = [](Function &function) noexcept {
        for (std::size_t index = 0; index < function.args_.size(); ++index) {
            function.args_[index]->set_parent(&function);
            function.args_[index]->set_index(index);
        }
        for (auto &block : function.blocks_) {
            block->set_parent(&function);
            for (auto &instruction : block->instructions()) {
                instruction->set_parent(block.get());
            }
        }
    };
    reparent(*this);
    reparent(other);
}

std::string Function::print() const {
    std::ostringstream oss;
    oss << return_type()->print() << " @" << name() << "(";
    for (std::size_t i = 0; i < args_.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << args_[i]->print();
    }
    oss << ")";
    return oss.str();
}

GlobalVariable::GlobalVariable(Type *ptr_type, Type *value_type, const std::string &name,
                               bool is_const, Value *init_value)
    : Value(ptr_type, name), value_type_(value_type), is_const_(is_const), init_value_(init_value) {
}

Type *GlobalVariable::value_type() const {
    return value_type_;
}

bool GlobalVariable::is_const() const {
    return is_const_;
}

Value *GlobalVariable::init_value() const {
    return init_value_;
}

void GlobalVariable::set_initializer_literal(std::string literal) {
    initializer_literal_ = std::move(literal);
}

const std::string &GlobalVariable::initializer_literal() const {
    return initializer_literal_;
}

std::string GlobalVariable::print() const {
    std::ostringstream oss;
    oss << "@" << name() << " = " << (is_const_ ? "constant " : "global ") << value_type_->print();
    if (!initializer_literal_.empty()) {
        oss << " " << initializer_literal_;
    } else if (init_value_ != nullptr) {
        oss << " " << value_ref(init_value_);
    }
    return oss.str();
}

Module::Module(const std::string &name) : name_(name), next_function_id_(1) {
}

Module::~Module() {
    // Functions can reference other functions owned by this module.  Clear every
    // instruction operand while all Value use-lists are still alive; individual
    // Function destructors may then run in any container destruction order.
    for (auto &function : functions_) {
        for (auto &block : function->blocks()) {
            for (auto &instruction : block->instructions()) {
                instruction->drop_all_operands();
            }
        }
    }
}

const std::string &Module::name() const {
    return name_;
}

TypeContext &Module::types() {
    return types_;
}

const TypeContext &Module::types() const {
    return types_;
}

Function *Module::create_function(const std::string &name, FunctionType *type, bool is_external,
                                  FunctionOrigin origin, FunctionID root_function_id) {
    auto found = function_table_.find(name);
    if (found != function_table_.end()) {
        return found->second;
    }

    const FunctionID function_id = next_function_id_;
    auto fn = std::make_unique<Function>(type, name, this, is_external, function_id, origin,
                                         root_function_id);
    auto *raw = fn.get();

    const auto &param_types = type->param_types();
    for (std::size_t i = 0; i < param_types.size(); ++i) {
        raw->add_argument(param_types[i], "arg" + std::to_string(i));
    }

    // Complete every potentially-throwing preparation before consuming the
    // stable ID.  If table-node allocation fails after the reserved vector
    // insertion, pop the unpublished function and leave both public containers
    // semantically unchanged.
    functions_.reserve(functions_.size() + 1);
    functions_.push_back(std::move(fn));
    decltype(function_table_)::iterator table_entry;
    bool inserted = false;
    try {
        auto result = function_table_.emplace(name, raw);
        table_entry = result.first;
        inserted = result.second;
    } catch (...) {
        functions_.pop_back();
        throw;
    }
    if (!inserted) {
        functions_.pop_back();
        return table_entry->second;
    }
    ++next_function_id_;
    return raw;
}

Function *Module::get_function(const std::string &name) const {
    auto found = function_table_.find(name);
    if (found == function_table_.end()) {
        return nullptr;
    }
    return found->second;
}

const std::unordered_map<std::string, Function *> &
Module::function_table_mappings() const {
    return function_table_;
}

bool Module::erase_function(Function *function) {
    if (function == nullptr || !function->uses().empty()) {
        return false;
    }
    auto found = std::find_if(functions_.begin(), functions_.end(),
                              [&](const auto &candidate) {
                                  return candidate.get() == function;
                              });
    if (found == functions_.end()) {
        return false;
    }
    const auto function_id = function->function_id();
    for (auto &block : function->blocks()) {
        for (auto &instruction : block->instructions()) {
            instruction->drop_all_operands();
        }
    }
    auto table_entry = function_table_.find(function->name());
    if (table_entry != function_table_.end() && table_entry->second == function) {
        function_table_.erase(table_entry);
    }
    functions_.erase(found);
    if (function_id != kInvalidFunctionID && next_function_id_ == function_id + 1) {
        next_function_id_ = function_id;
    }
    return true;
}

void Module::prepare_function_set(ModuleFunctionSet &set) const {
    std::unordered_map<std::string, Function *> prepared;
    prepared.reserve(set.functions.size());
    for (const auto &function : set.functions) {
        if (function == nullptr || function->parent() != this ||
            !prepared.emplace(function->name(), function.get()).second) {
            throw std::runtime_error("invalid staged OIR function set");
        }
    }
    set.function_table = std::move(prepared);
}

void Module::exchange_function_set(ModuleFunctionSet &set) noexcept {
    functions_.swap(set.functions);
    function_table_.swap(set.function_table);
}

FunctionID Module::function_allocator_state() const {
    return next_function_id_;
}

void Module::set_function_allocator_state(FunctionID state) {
    next_function_id_ = state;
}

GlobalVariable *Module::create_global(const std::string &name, Type *value_type, bool is_const,
                                      Value *init_value) {
    auto found = global_table_.find(name);
    if (found != global_table_.end()) {
        return found->second;
    }

    auto *ptr_type = types_.ptr_ty(value_type);
    auto global =
        std::make_unique<GlobalVariable>(ptr_type, value_type, name, is_const, init_value);
    auto *raw = global.get();
    globals_.push_back(std::move(global));
    global_table_[name] = raw;
    return raw;
}

GlobalVariable *Module::get_global(const std::string &name) const {
    auto found = global_table_.find(name);
    if (found == global_table_.end()) {
        return nullptr;
    }
    return found->second;
}

ConstantInt *Module::create_i1(bool value) {
    auto c = std::make_unique<ConstantInt>(types_.int1_ty(), value ? 1 : 0);
    auto *raw = c.get();
    owned_constants_.push_back(std::move(c));
    return raw;
}

ConstantInt *Module::create_i32(std::int64_t value) {
    auto c = std::make_unique<ConstantInt>(types_.int32_ty(), value);
    auto *raw = c.get();
    owned_constants_.push_back(std::move(c));
    return raw;
}

ConstantFloat *Module::create_f32(float value) {
    auto c = std::make_unique<ConstantFloat>(types_.float_ty(), value);
    auto *raw = c.get();
    owned_constants_.push_back(std::move(c));
    return raw;
}

ConstantZero *Module::create_zero(Type *type) {
    auto zero = std::make_unique<ConstantZero>(type);
    auto *raw = zero.get();
    owned_constants_.push_back(std::move(zero));
    return raw;
}

UndefValue *Module::create_undef(Type *type) {
    auto undef = std::make_unique<UndefValue>(type);
    auto *raw = undef.get();
    owned_constants_.push_back(std::move(undef));
    return raw;
}

void Module::adopt_constants(std::vector<std::unique_ptr<Value>> &constants) {
    owned_constants_.reserve(owned_constants_.size() + constants.size());
    for (auto &constant : constants) {
        owned_constants_.push_back(std::move(constant));
    }
    constants.clear();
}

bool Module::discard_constants_from(std::size_t first) {
    if (first > owned_constants_.size()) {
        return false;
    }
    for (std::size_t index = first; index < owned_constants_.size(); ++index) {
        if (owned_constants_[index] != nullptr &&
            owned_constants_[index]->has_uses()) {
            return false;
        }
    }
    owned_constants_.resize(first);
    return true;
}

std::vector<std::unique_ptr<GlobalVariable>> &Module::globals() {
    return globals_;
}

const std::vector<std::unique_ptr<GlobalVariable>> &Module::globals() const {
    return globals_;
}

const std::vector<std::unique_ptr<Value>> &Module::owned_constants() const {
    return owned_constants_;
}

std::vector<std::unique_ptr<Function>> &Module::functions() {
    return functions_;
}

const std::vector<std::unique_ptr<Function>> &Module::functions() const {
    return functions_;
}

std::string Module::print() const {
    std::ostringstream oss;
    oss << "; module: " << name_ << "\n\n";

    for (const auto &global : globals_) {
        oss << global->print() << "\n";
    }

    if (!globals_.empty()) {
        oss << "\n";
    }

    for (const auto &function : functions_) {
        if (function->is_external()) {
            oss << "declare " << function->print() << "\n\n";
            continue;
        }

        oss << "define " << function->print() << " {\n";
        for (const auto &block : function->blocks()) {
            oss << block->print() << "\n";
            for (const auto &inst : block->instructions()) {
                oss << "  " << inst->print() << "\n";
            }
        }
        oss << "}\n\n";
    }

    return oss.str();
}

bool Module::verify(std::string *message) const {
    auto result = Verifier::verify_module(*this);
    if (message != nullptr) {
        *message = result.message;
    }
    return result.ok;
}

IRBuilder::IRBuilder(Module *module) : module_(module), insert_block_(nullptr) {
}

Module *IRBuilder::module() const {
    return module_;
}

BasicBlock *IRBuilder::insert_block() const {
    return insert_block_;
}

void IRBuilder::set_insert_point(BasicBlock *block) {
    insert_block_ = block;
}

void IRBuilder::clear_insert_point() {
    insert_block_ = nullptr;
}

ConstantInt *IRBuilder::i1(bool value) const {
    return module_->create_i1(value);
}

ConstantInt *IRBuilder::i32(std::int64_t value) const {
    return module_->create_i32(value);
}

ConstantFloat *IRBuilder::f32(float value) const {
    return module_->create_f32(value);
}

ConstantZero *IRBuilder::zero(Type *type) const {
    return module_->create_zero(type);
}

UndefValue *IRBuilder::undef(Type *type) const {
    return module_->create_undef(type);
}

BinaryInst *IRBuilder::create_binary(Instruction::OpID op, Value *lhs, Value *rhs,
                                     const std::string &name) {
    return append(std::make_unique<BinaryInst>(lhs->type(), op, lhs, rhs, insert_block_, name));
}

CmpInst *IRBuilder::create_icmp(CmpPred pred, Value *lhs, Value *rhs, const std::string &name) {
    return append(std::make_unique<CmpInst>(module_->types().int1_ty(), Instruction::OpID::ICmp,
                                            pred, lhs, rhs, insert_block_, name));
}

CmpInst *IRBuilder::create_fcmp(CmpPred pred, Value *lhs, Value *rhs, const std::string &name) {
    return append(std::make_unique<CmpInst>(module_->types().int1_ty(), Instruction::OpID::FCmp,
                                            pred, lhs, rhs, insert_block_, name));
}

CastInst *IRBuilder::create_zext(Value *src, Type *dst_type, const std::string &name) {
    return append(
        std::make_unique<CastInst>(dst_type, Instruction::OpID::ZExt, src, insert_block_, name));
}

CastInst *IRBuilder::create_sitofp(Value *src, Type *dst_type, const std::string &name) {
    return append(
        std::make_unique<CastInst>(dst_type, Instruction::OpID::SIToFP, src, insert_block_, name));
}

CastInst *IRBuilder::create_fptosi(Value *src, Type *dst_type, const std::string &name) {
    return append(
        std::make_unique<CastInst>(dst_type, Instruction::OpID::FPToSI, src, insert_block_, name));
}

AllocaInst *IRBuilder::create_alloca(Type *allocated_type, const std::string &name) {
    auto *ptr_type = module_->types().ptr_ty(allocated_type);
    return append(std::make_unique<AllocaInst>(ptr_type, allocated_type, insert_block_, name));
}

LoadInst *IRBuilder::create_load(Value *ptr, Type *loaded_type, const std::string &name) {
    return append(std::make_unique<LoadInst>(loaded_type, ptr, insert_block_, name));
}

StoreInst *IRBuilder::create_store(Value *value, Value *ptr) {
    return append(
        std::make_unique<StoreInst>(module_->types().void_ty(), value, ptr, insert_block_));
}

MemZeroInst *IRBuilder::create_memzero(Value *ptr, Value *byte_count) {
    return create_memset(ptr, module_->create_i32(0), byte_count);
}

MemZeroInst *IRBuilder::create_memset(Value *ptr, Value *byte_value, Value *byte_count) {
    return append(
        std::make_unique<MemZeroInst>(module_->types().void_ty(), ptr, byte_value, byte_count,
                                      insert_block_));
}

GetElementPtrInst *IRBuilder::create_gep(Value *base_ptr, Type *result_ptr_type,
                                         const std::vector<Value *> &indices,
                                         const std::string &name) {
    return append(std::make_unique<GetElementPtrInst>(result_ptr_type, base_ptr, indices,
                                                      insert_block_, name));
}

CallInst *IRBuilder::create_call(Value *callee, Type *return_type, const std::vector<Value *> &args,
                                 const std::string &name) {
    return append(std::make_unique<CallInst>(return_type, callee, args, insert_block_, name));
}

ReturnInst *IRBuilder::create_ret(Value *value) {
    return append(std::make_unique<ReturnInst>(module_->types().void_ty(), value, insert_block_));
}

BranchInst *IRBuilder::create_br(BasicBlock *target) {
    insert_block_->add_successor(target);
    target->add_predecessor(insert_block_);
    return append(std::make_unique<BranchInst>(module_->types().void_ty(), target, insert_block_));
}

BranchInst *IRBuilder::create_cond_br(Value *cond, BasicBlock *true_bb, BasicBlock *false_bb) {
    insert_block_->add_successor(true_bb);
    insert_block_->add_successor(false_bb);
    true_bb->add_predecessor(insert_block_);
    false_bb->add_predecessor(insert_block_);
    return append(std::make_unique<BranchInst>(module_->types().void_ty(), cond, true_bb, false_bb,
                                               insert_block_));
}

PhiInst *IRBuilder::create_phi(Type *type, const std::string &name) {
    return append(std::make_unique<PhiInst>(type, insert_block_, name));
}

VerifyResult Verifier::verify_module(const Module &module) {
    auto fail = [](std::string message) { return VerifyResult{false, std::move(message)}; };

    // Reject dangling/cross-module operands by pointer identity before any
    // verifier path performs RTTI or otherwise dereferences an operand.  This
    // also makes detached/live ownership mistakes fail at their commit boundary
    // instead of surfacing as allocator corruption in a later pass.
    std::unordered_set<const Value *> owned_values;
    std::unordered_map<const Value *, const Function *> body_owners;
    owned_values.reserve(module.owned_constants().size() + module.globals().size() +
                         module.functions().size());
    for (const auto &constant : module.owned_constants()) {
        owned_values.insert(constant.get());
    }
    for (const auto &global : module.globals()) {
        owned_values.insert(global.get());
    }
    for (const auto &function : module.functions()) {
        owned_values.insert(function.get());
        for (const auto &argument : function->args()) {
            owned_values.insert(argument.get());
            body_owners.emplace(argument.get(), function.get());
        }
        for (const auto &block : function->blocks()) {
            owned_values.insert(block.get());
            body_owners.emplace(block.get(), function.get());
            for (const auto &instruction : block->instructions()) {
                owned_values.insert(instruction.get());
                body_owners.emplace(instruction.get(), function.get());
            }
        }
    }
    for (const auto &function : module.functions()) {
        for (const auto &block : function->blocks()) {
            for (const auto &instruction : block->instructions()) {
                std::size_t operand_index = 0;
                for (auto *operand : instruction->operands()) {
                    if (operand != nullptr &&
                        owned_values.find(operand) == owned_values.end()) {
                        return fail(
                            "instruction operand references a value outside the module: @" +
                            function->name() + " %" + block->name() + " op=" +
                            std::to_string(static_cast<unsigned>(instruction->op())) +
                            " operand=" + std::to_string(operand_index));
                    }
                    auto owner = body_owners.find(operand);
                    if (owner != body_owners.end() &&
                        owner->second != function.get()) {
                        return fail(
                            "instruction operand references a value owned by another function: @" +
                            function->name() + " %" + block->name() + " op=" +
                            std::to_string(static_cast<unsigned>(instruction->op())) +
                            " operand=" + std::to_string(operand_index) +
                            " owner=@" + owner->second->name());
                    }
                    ++operand_index;
                }
            }
        }
    }

    auto use_list_error = [](const Value *value) -> std::string {
        if (value == nullptr) {
            return "";
        }
        for (const auto &use : value->uses()) {
            if (use.user == nullptr) {
                return "value " + value_ref(value) + " has a null use-list user";
            }
            if (use.operand_index >= use.user->operand_count()) {
                return "value " + value_ref(value) + " has stale use-list index in user " +
                       value_ref(use.user);
            }
            if (use.user->operand(use.operand_index) != value) {
                return "value " + value_ref(value) + " has stale use-list entry in user " +
                       value_ref(use.user);
            }
        }
        return "";
    };

    for (const auto &function_ptr : module.functions()) {
        const auto *function = function_ptr.get();
        if (function->parent() != &module) {
            return fail("function " + function_ref(function) + " has wrong module parent");
        }
        const auto &param_types = function->function_type()->param_types();
        if (function->args().size() != param_types.size()) {
            return fail("function " + function_ref(function) +
                        " argument count does not match its function type");
        }
        for (std::size_t i = 0; i < function->args().size(); ++i) {
            const auto *arg = function->args()[i].get();
            if (arg->parent() != function || arg->index() != i) {
                return fail("argument %" + arg->name() + " in " + function_ref(function) +
                            " has inconsistent parent or index");
            }
            if (arg->type() != param_types[i]) {
                return fail("argument %" + arg->name() + " in " + function_ref(function) +
                            " does not match its function type");
            }
            if (auto error = use_list_error(arg); !error.empty()) {
                return fail(error);
            }
        }
        if (auto error = use_list_error(function); !error.empty()) {
            return fail(error);
        }
        if (function->is_external()) {
            continue;
        }

        if (function->blocks().empty()) {
            return fail("function " + function_ref(function) + " has no basic blocks");
        }

        std::unordered_set<const BasicBlock *> block_set;
        std::unordered_map<const Instruction *, const BasicBlock *> inst_blocks;
        std::unordered_map<const Instruction *, std::size_t> inst_indices;
        for (const auto &block_ptr : function->blocks()) {
            const auto *block = block_ptr.get();
            block_set.insert(block);
            if (block->parent() != function) {
                return fail("block " + block_ref(block) + " has wrong function parent in " +
                            function_ref(function));
            }
            const auto &insts = block->instructions();

            if (insts.empty()) {
                return fail("block " + block_ref(block) + " in " + function_ref(function) +
                            " is empty");
            }

            bool saw_non_phi = false;
            std::size_t inst_index = 0;
            for (auto it = insts.begin(); it != insts.end(); ++it) {
                const auto *inst = it->get();
                const bool is_last = std::next(it) == insts.end();
                inst_blocks[inst] = block;
                inst_indices[inst] = inst_index++;

                if (inst->parent() != block) {
                    return fail("instruction " + inst_ref(inst) + " has wrong parent; expected " +
                                block_ref(block));
                }

                if (inst->is_terminator() && !is_last) {
                    return fail("terminator " + inst_ref(inst) + " in block " +
                                block_ref(block) + " is not the last instruction");
                }
                if (inst->op() == Instruction::OpID::Phi) {
                    if (saw_non_phi) {
                        return fail("phi instruction " + inst_ref(inst) + " in block " +
                                    block_ref(block) + " appears after a non-phi instruction");
                    }
                } else {
                    saw_non_phi = true;
                }
            }

            if (!block->has_terminator()) {
                return fail("block " + block_ref(block) + " in " + function_ref(function) +
                            " has no terminator");
            }
            if (has_duplicate_block(block->predecessors())) {
                return fail("block " + block_ref(block) + " has duplicate predecessors");
            }
            if (has_duplicate_block(block->successors())) {
                return fail("block " + block_ref(block) + " has duplicate successors");
            }

            for (auto *pred : block->predecessors()) {
                if (pred == nullptr || pred->parent() != function) {
                    return fail("block " + block_ref(block) +
                                " has predecessor outside its function");
                }
                if (!contains_block_ptr(pred->successors(), block)) {
                    return fail("CFG mismatch: predecessor " + block_ref(pred) +
                                " does not list successor " + block_ref(block));
                }
            }
            for (auto *succ : block->successors()) {
                if (succ == nullptr || succ->parent() != function) {
                    return fail("block " + block_ref(block) +
                                " has successor outside its function");
                }
                if (!contains_block_ptr(succ->predecessors(), block)) {
                    return fail("CFG mismatch: successor " + block_ref(succ) +
                                " does not list predecessor " + block_ref(block));
                }
            }

            const auto *terminator = block->terminator();
            if (const auto *br = dynamic_cast<const BranchInst *>(terminator)) {
                if (br->operand_count() != 1 && br->operand_count() != 3) {
                    return fail("branch in " + block_ref(block) + " has invalid operand count");
                }
                auto targets = branch_targets(*br);
                for (auto *target : targets) {
                    if (target->parent() != function) {
                        return fail("branch in " + block_ref(block) +
                                    " targets block outside " + function_ref(function));
                    }
                    if (!contains_block_ptr(block->successors(), target)) {
                        return fail("CFG mismatch: branch in " + block_ref(block) +
                                    " targets " + block_ref(target) +
                                    " but successor list is missing it");
                    }
                }
                for (auto *succ : block->successors()) {
                    if (!contains_block_ptr(targets, succ)) {
                        return fail("CFG mismatch: block " + block_ref(block) +
                                    " lists successor " + block_ref(succ) +
                                    " not named by its terminator");
                    }
                }
            } else if (dynamic_cast<const ReturnInst *>(terminator) != nullptr) {
                if (!block->successors().empty()) {
                    return fail("return block " + block_ref(block) +
                                " must not have CFG successors");
                }
            } else {
                return fail("block " + block_ref(block) + " has unknown terminator kind");
            }
        }

        DominatorTree dom_tree(*function);
        auto normal_def_error = [&](const Instruction *def,
                                    const Instruction *use) -> std::string {
            if (def == use) {
                return "instruction " + inst_ref(use) + " uses itself";
            }
            auto def_block_found = inst_blocks.find(def);
            auto use_block_found = inst_blocks.find(use);
            if (def_block_found == inst_blocks.end() || use_block_found == inst_blocks.end()) {
                return "instruction operand in " + inst_ref(use) +
                       " is not defined in the same function";
            }
            auto *def_block = def_block_found->second;
            auto *use_block = use_block_found->second;
            if (def_block == use_block) {
                if (inst_indices.at(def) >= inst_indices.at(use)) {
                    return "definition " + inst_ref(def) + " does not precede use in " +
                           inst_ref(use);
                }
                return "";
            }
            if (dom_tree.is_reachable(use_block) && !dom_tree.dominates(def_block, use_block)) {
                return "definition " + inst_ref(def) + " in " + block_ref(def_block) +
                       " does not dominate use in " + inst_ref(use);
            }
            return "";
        };

        auto phi_def_error = [&](const Instruction *def, const PhiInst *phi,
                                 const BasicBlock *pred) -> std::string {
            auto def_block_found = inst_blocks.find(def);
            if (def_block_found == inst_blocks.end()) {
                return "phi " + inst_ref(phi) + " uses instruction from another function";
            }
            auto *def_block = def_block_found->second;
            if (def_block == pred) {
                return "";
            }
            if (dom_tree.is_reachable(pred) && !dom_tree.dominates(def_block, pred)) {
                return "phi " + inst_ref(phi) + " incoming definition " + inst_ref(def) +
                       " does not dominate predecessor " + block_ref(pred);
            }
            return "";
        };

        auto check_operand_parent = [&](const Value *operand, const Instruction *inst,
                                        std::size_t index) -> VerifyResult {
            if (operand == nullptr) {
                return fail("instruction " + inst_ref(inst) + " has null operand " +
                            std::to_string(index));
            }
            if (auto *arg = dynamic_cast<const Argument *>(operand)) {
                if (arg->parent() != function) {
                    return fail("instruction " + inst_ref(inst) +
                                " uses argument from another function");
                }
            } else if (auto *def = dynamic_cast<const Instruction *>(operand)) {
                if (def->parent() == nullptr || def->parent()->parent() != function) {
                    return fail("instruction " + inst_ref(inst) +
                                " uses instruction from another function");
                }
                if (inst->op() != Instruction::OpID::Phi) {
                    if (auto error = normal_def_error(def, inst); !error.empty()) {
                        return fail(error);
                    }
                }
            } else if (auto *bb = dynamic_cast<const BasicBlock *>(operand)) {
                if (bb->parent() != function) {
                    return fail("instruction " + inst_ref(inst) +
                                " uses block from another function");
                }
            } else if (auto *callee = dynamic_cast<const Function *>(operand)) {
                if (callee->parent() != &module) {
                    return fail("instruction " + inst_ref(inst) +
                                " uses function from another module");
                }
            }
            return {true, "ok"};
        };

        std::unordered_map<const Value *, std::vector<UseKey>> actual_uses;
        for (const auto &block_ptr : function->blocks()) {
            for (const auto &inst_ptr : block_ptr->instructions()) {
                const auto *inst = inst_ptr.get();
                for (std::size_t i = 0; i < inst->operand_count(); ++i) {
                    auto *operand = inst->operand(i);
                    if (operand != nullptr) {
                        actual_uses[operand].push_back({inst, i});
                    }
                }
            }
        }
        for (const auto &[value, uses] : actual_uses) {
            if (auto error = use_list_error(value); !error.empty()) {
                return fail(error);
            }

            std::unordered_set<UseKey, UseKeyHash> recorded;
            for (const auto &use : value->uses()) {
                recorded.insert({use.user, use.operand_index});
            }
            for (const auto &use : uses) {
                if (recorded.find(use) == recorded.end()) {
                    return fail("missing use-list entry for operand " +
                                std::to_string(use.operand_index) + " of " +
                                value_ref(use.user) + " using " + value_ref(value));
                }
            }
        }

        for (const auto &block_ptr : function->blocks()) {
            const auto *block = block_ptr.get();
            if (auto error = use_list_error(block); !error.empty()) {
                return fail(error);
            }
            for (const auto &inst_ptr : block->instructions()) {
                const auto *inst = inst_ptr.get();
                if (auto error = use_list_error(inst); !error.empty()) {
                    return fail(error);
                }
                for (std::size_t i = 0; i < inst->operand_count(); ++i) {
                    auto result = check_operand_parent(inst->operand(i), inst, i);
                    if (!result.ok) {
                        return result;
                    }
                }

                switch (inst->op()) {
                case Instruction::OpID::Br: {
                    const auto *br = dynamic_cast<const BranchInst *>(inst);
                    if (br == nullptr) {
                        return fail("branch instruction type mismatch in " + block_ref(block));
                    }
                    if (!br->is_conditional() && br->target_bb() == nullptr) {
                        return fail("unconditional branch in " + block_ref(block) +
                                    " is missing target");
                    }
                    if (br->is_conditional()) {
                        if (br->cond() == nullptr || br->true_bb() == nullptr ||
                            br->false_bb() == nullptr) {
                            return fail("conditional branch in " + block_ref(block) +
                                        " is incomplete");
                        }
                        auto *cond_ty = dynamic_cast<IntegerType *>(br->cond()->type());
                        if (cond_ty == nullptr || cond_ty->bit_width() != 1) {
                            return fail("conditional branch in " + block_ref(block) +
                                        " expects i1 condition");
                        }
                    }
                    break;
                }
                case Instruction::OpID::Ret: {
                    const auto *ret = dynamic_cast<const ReturnInst *>(inst);
                    if (ret == nullptr) {
                        return fail("return instruction type mismatch in " + block_ref(block));
                    }
                    if (ret->operand_count() > 1) {
                        return fail("return in " + block_ref(block) + " has too many operands");
                    }
                    if (function->return_type()->is_void()) {
                        if (ret->has_value()) {
                            return fail("void function " + function_ref(function) +
                                        " cannot return a value");
                        }
                    } else {
                        if (!ret->has_value()) {
                            return fail("non-void function " + function_ref(function) +
                                        " must return a value");
                        }
                        if (ret->value()->type() != function->return_type()) {
                            return fail("return type mismatch in function " +
                                        function_ref(function));
                        }
                    }
                    break;
                }
                case Instruction::OpID::Load: {
                    const auto *load = dynamic_cast<const LoadInst *>(inst);
                    if (load == nullptr) {
                        return fail("load instruction type mismatch in " + block_ref(block));
                    }
                    auto *ptr_ty = dynamic_cast<PointerType *>(load->ptr()->type());
                    if (ptr_ty == nullptr) {
                        return fail("load " + inst_ref(load) + " expects pointer operand");
                    }
                    if (ptr_ty->element_type() != load->type()) {
                        return fail("load " + inst_ref(load) +
                                    " result type does not match pointer element type");
                    }
                    break;
                }
                case Instruction::OpID::Store: {
                    const auto *store = dynamic_cast<const StoreInst *>(inst);
                    if (store == nullptr) {
                        return fail("store instruction type mismatch in " + block_ref(block));
                    }
                    auto *ptr_ty = dynamic_cast<PointerType *>(store->ptr()->type());
                    if (ptr_ty == nullptr) {
                        return fail("store in " + block_ref(block) + " expects pointer operand");
                    }
                    if (ptr_ty->element_type() != store->value()->type()) {
                        return fail("store in " + block_ref(block) + " value type mismatch");
                    }
                    break;
                }
                case Instruction::OpID::MemZero: {
                    const auto *memzero = dynamic_cast<const MemZeroInst *>(inst);
                    if (memzero == nullptr) {
                        return fail("memzero instruction type mismatch in " + block_ref(block));
                    }
                    if (!memzero->ptr()->type()->is_pointer()) {
                        return fail("memzero in " + block_ref(block) + " expects pointer operand");
                    }
                    auto *value_ty = dynamic_cast<IntegerType *>(memzero->byte_value()->type());
                    if (value_ty == nullptr || value_ty->bit_width() != 32) {
                        return fail("memzero in " + block_ref(block) +
                                    " expects i32 byte value");
                    }
                    auto *byte_value = dynamic_cast<ConstantInt *>(memzero->byte_value());
                    if (byte_value == nullptr || byte_value->value() < 0 ||
                        byte_value->value() > 255) {
                        return fail("memzero in " + block_ref(block) +
                                    " expects constant byte value in [0, 255]");
                    }
                    auto *count_ty = dynamic_cast<IntegerType *>(memzero->byte_count()->type());
                    if (count_ty == nullptr || count_ty->bit_width() != 32) {
                        return fail("memzero in " + block_ref(block) +
                                    " expects i32 byte count");
                    }
                    break;
                }
                case Instruction::OpID::Call: {
                    const auto *call = dynamic_cast<const CallInst *>(inst);
                    if (call == nullptr) {
                        return fail("call instruction type mismatch in " + block_ref(block));
                    }
                    const auto *callee = dynamic_cast<const Function *>(call->callee());
                    if (callee != nullptr) {
                        const auto &callee_params = callee->function_type()->param_types();
                        const auto call_args = call->args();
                        if (call_args.size() != callee_params.size()) {
                            return fail("call " + inst_ref(call) + " to " +
                                        function_ref(callee) +
                                        " has an argument-count mismatch");
                        }
                        for (std::size_t i = 0; i < call_args.size(); ++i) {
                            if (call_args[i]->type() != callee_params[i]) {
                                return fail("call " + inst_ref(call) + " to " +
                                            function_ref(callee) + " has argument " +
                                            std::to_string(i) + " type mismatch");
                            }
                        }
                        if (call->type() != callee->return_type()) {
                            return fail("call " + inst_ref(call) + " to " +
                                        function_ref(callee) +
                                        " has a return-type mismatch");
                        }
                    }
                    break;
                }
                case Instruction::OpID::Add:
                case Instruction::OpID::Sub:
                case Instruction::OpID::Mul:
                case Instruction::OpID::And:
                case Instruction::OpID::Xor:
                case Instruction::OpID::SDiv:
                case Instruction::OpID::SRem:
                case Instruction::OpID::FAdd:
                case Instruction::OpID::FSub:
                case Instruction::OpID::FMul:
                case Instruction::OpID::FDiv: {
                    const auto *bin = dynamic_cast<const BinaryInst *>(inst);
                    if (bin == nullptr) {
                        return fail("binary instruction type mismatch in " + block_ref(block));
                    }
                    if (bin->lhs()->type() != bin->rhs()->type() ||
                        bin->lhs()->type() != bin->type()) {
                        return fail("binary instruction " + inst_ref(bin) +
                                    " type mismatch between operands and result");
                    }
                    if ((inst->op() == Instruction::OpID::And ||
                         inst->op() == Instruction::OpID::Xor) &&
                        !bin->type()->is_integer()) {
                        return fail(op_to_string(inst->op()) + " instruction " + inst_ref(bin) +
                                    " operands must be integer");
                    }
                    break;
                }
                case Instruction::OpID::ICmp:
                case Instruction::OpID::FCmp: {
                    const auto *cmp = dynamic_cast<const CmpInst *>(inst);
                    if (cmp == nullptr) {
                        return fail("compare instruction type mismatch in " + block_ref(block));
                    }
                    if (cmp->lhs()->type() != cmp->rhs()->type()) {
                        return fail("compare " + inst_ref(cmp) +
                                    " operands must have same type");
                    }
                    auto *result_ty = dynamic_cast<IntegerType *>(cmp->type());
                    if (result_ty == nullptr || result_ty->bit_width() != 1) {
                        return fail("compare " + inst_ref(cmp) + " result must be i1");
                    }
                    if (inst->op() == Instruction::OpID::ICmp &&
                        !cmp->lhs()->type()->is_integer()) {
                        return fail("icmp " + inst_ref(cmp) + " operands must be integer");
                    }
                    if (inst->op() == Instruction::OpID::FCmp && !cmp->lhs()->type()->is_float()) {
                        return fail("fcmp " + inst_ref(cmp) + " operands must be float");
                    }
                    break;
                }
                case Instruction::OpID::GetElementPtr: {
                    const auto *gep = dynamic_cast<const GetElementPtrInst *>(inst);
                    if (gep == nullptr) {
                        return fail("gep instruction type mismatch in " + block_ref(block));
                    }
                    if (!gep->base_ptr()->type()->is_pointer() || !gep->type()->is_pointer()) {
                        return fail("gep " + inst_ref(gep) +
                                    " expects pointer base and pointer result");
                    }
                    for (auto *index : gep->indices()) {
                        if (!index->type()->is_integer()) {
                            return fail("gep " + inst_ref(gep) + " index must be integer");
                        }
                    }
                    break;
                }
                case Instruction::OpID::Phi: {
                    const auto *phi = dynamic_cast<const PhiInst *>(inst);
                    if (phi == nullptr) {
                        return fail("phi instruction type mismatch in " + block_ref(block));
                    }
                    if (phi->operand_count() != phi->incoming().size() * 2) {
                        return fail("phi " + inst_ref(phi) +
                                    " operand count does not match incoming list");
                    }
                    if (phi->incoming().size() != block->predecessors().size()) {
                        return fail("phi " + inst_ref(phi) +
                                    " incoming count does not match predecessor count in " +
                                    block_ref(block));
                    }
                    std::unordered_set<const BasicBlock *> incoming_preds;
                    for (std::size_t i = 0; i < phi->incoming().size(); ++i) {
                        const auto &item = phi->incoming()[i];
                        if (item.first == nullptr || item.second == nullptr) {
                            return fail("phi " + inst_ref(phi) + " has null incoming");
                        }
                        if (phi->operand(i * 2) != item.first ||
                            phi->operand(i * 2 + 1) != item.second) {
                            return fail("phi " + inst_ref(phi) +
                                        " operands and incoming list are out of sync");
                        }
                        if (!incoming_preds.insert(item.second).second) {
                            return fail("phi " + inst_ref(phi) +
                                        " has duplicate incoming predecessor " +
                                        block_ref(item.second));
                        }
                        if (item.first->type() != phi->type()) {
                            return fail("phi " + inst_ref(phi) +
                                        " incoming value type mismatch");
                        }
                        if (!contains_block_ptr(block->predecessors(), item.second)) {
                            return fail("phi " + inst_ref(phi) + " incoming predecessor " +
                                        block_ref(item.second) +
                                        " is not a CFG predecessor of " + block_ref(block));
                        }
                        if (auto *arg = dynamic_cast<const Argument *>(item.first)) {
                            if (arg->parent() != function) {
                                return fail("phi " + inst_ref(phi) +
                                            " uses argument from another function");
                            }
                        } else if (auto *def = dynamic_cast<const Instruction *>(item.first)) {
                            if (auto error = phi_def_error(def, phi, item.second);
                                !error.empty()) {
                                return fail(error);
                            }
                        }
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }
    }

    for (const auto &global : module.globals()) {
        if (auto error = use_list_error(global.get()); !error.empty()) {
            return fail(error);
        }
    }
    for (const auto &constant : module.owned_constants()) {
        if (auto error = use_list_error(constant.get()); !error.empty()) {
            return fail(error);
        }
    }

    return {true, "ok"};
}

} // namespace oir
