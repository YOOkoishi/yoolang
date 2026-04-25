#include "../../include/oir/oir.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

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
    case Instruction::OpID::ZExt:
        return "zext";
    case Instruction::OpID::SIToFP:
        return "sitofp";
    case Instruction::OpID::FPToSI:
        return "fptosi";
    case Instruction::OpID::Phi:
        return "phi";
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

void User::add_operand(Value *value) {
    operands_.push_back(value);
}

Value *User::operand(std::size_t index) const {
    return operands_[index];
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

const std::vector<std::pair<Value *, BasicBlock *>> &PhiInst::incoming() const {
    return incoming_;
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

std::string BasicBlock::print() const {
    return name() + ":";
}

Function::Function(FunctionType *type, const std::string &name, Module *parent, bool is_external)
    : Value(type, name), parent_(parent), is_external_(is_external), next_block_id_(0) {
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

Argument *Function::add_argument(Type *type, const std::string &name) {
    auto arg = std::make_unique<Argument>(type, name, this, args_.size());
    auto *raw = arg.get();
    args_.push_back(std::move(arg));
    return raw;
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

Module::Module(const std::string &name) : name_(name) {
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

Function *Module::create_function(const std::string &name, FunctionType *type, bool is_external) {
    auto found = function_table_.find(name);
    if (found != function_table_.end()) {
        return found->second;
    }

    auto fn = std::make_unique<Function>(type, name, this, is_external);
    auto *raw = fn.get();

    const auto &param_types = type->param_types();
    for (std::size_t i = 0; i < param_types.size(); ++i) {
        raw->add_argument(param_types[i], "arg" + std::to_string(i));
    }

    functions_.push_back(std::move(fn));
    function_table_[name] = raw;
    return raw;
}

Function *Module::get_function(const std::string &name) const {
    auto found = function_table_.find(name);
    if (found == function_table_.end()) {
        return nullptr;
    }
    return found->second;
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

std::vector<std::unique_ptr<GlobalVariable>> &Module::globals() {
    return globals_;
}

const std::vector<std::unique_ptr<GlobalVariable>> &Module::globals() const {
    return globals_;
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
    for (const auto &function_ptr : module.functions()) {
        const auto *function = function_ptr.get();
        if (function->is_external()) {
            continue;
        }

        if (function->blocks().empty()) {
            return {false, "function @" + function->name() + " has no basic blocks"};
        }

        for (const auto &block_ptr : function->blocks()) {
            const auto *block = block_ptr.get();
            const auto &insts = block->instructions();

            if (insts.empty()) {
                return {false, "block %" + block->name() + " is empty"};
            }

            bool saw_non_phi = false;
            for (auto it = insts.begin(); it != insts.end(); ++it) {
                const auto *inst = it->get();
                const bool is_last = std::next(it) == insts.end();

                if (inst->is_terminator() && !is_last) {
                    return {false, "terminator in block %" + block->name() +
                                       " is not the last instruction"};
                }
                if (inst->op() == Instruction::OpID::Phi) {
                    if (saw_non_phi) {
                        return {false, "phi instruction in block %" + block->name() +
                                           " appears after a non-phi instruction"};
                    }
                } else {
                    saw_non_phi = true;
                }

                switch (inst->op()) {
                case Instruction::OpID::Br: {
                    const auto *br = dynamic_cast<const BranchInst *>(inst);
                    if (br == nullptr) {
                        return {false, "branch instruction type mismatch"};
                    }
                    if (!br->is_conditional() && br->target_bb() == nullptr) {
                        return {false, "unconditional branch missing target"};
                    }
                    if (br->is_conditional()) {
                        if (br->cond() == nullptr || br->true_bb() == nullptr ||
                            br->false_bb() == nullptr) {
                            return {false, "conditional branch is incomplete"};
                        }
                        auto *cond_ty = dynamic_cast<IntegerType *>(br->cond()->type());
                        if (cond_ty == nullptr || cond_ty->bit_width() != 1) {
                            return {false, "conditional branch expects i1 condition"};
                        }
                        if (br->true_bb()->parent() != function ||
                            br->false_bb()->parent() != function) {
                            return {false, "conditional branch target is outside the function"};
                        }
                    } else if (br->target_bb()->parent() != function) {
                        return {false, "branch target is outside the function"};
                    }
                    break;
                }
                case Instruction::OpID::Ret: {
                    const auto *ret = dynamic_cast<const ReturnInst *>(inst);
                    if (ret == nullptr) {
                        return {false, "return instruction type mismatch"};
                    }
                    if (function->return_type()->is_void()) {
                        if (ret->has_value()) {
                            return {false, "void function @" + function->name() +
                                               " cannot return a value"};
                        }
                    } else {
                        if (!ret->has_value()) {
                            return {false, "non-void function @" + function->name() +
                                               " must return a value"};
                        }
                        if (ret->value()->type() != function->return_type()) {
                            return {false, "return type mismatch in function @" + function->name()};
                        }
                    }
                    break;
                }
                case Instruction::OpID::Load: {
                    const auto *load = dynamic_cast<const LoadInst *>(inst);
                    auto *ptr_ty = dynamic_cast<PointerType *>(load->ptr()->type());
                    if (ptr_ty == nullptr) {
                        return {false, "load expects pointer operand"};
                    }
                    if (ptr_ty->element_type() != load->type()) {
                        return {false, "load result type does not match pointer element type"};
                    }
                    break;
                }
                case Instruction::OpID::Store: {
                    const auto *store = dynamic_cast<const StoreInst *>(inst);
                    auto *ptr_ty = dynamic_cast<PointerType *>(store->ptr()->type());
                    if (ptr_ty == nullptr) {
                        return {false, "store expects pointer operand"};
                    }
                    if (ptr_ty->element_type() != store->value()->type()) {
                        return {false, "store value type mismatch"};
                    }
                    break;
                }
                case Instruction::OpID::Add:
                case Instruction::OpID::Sub:
                case Instruction::OpID::Mul:
                case Instruction::OpID::SDiv:
                case Instruction::OpID::SRem:
                case Instruction::OpID::FAdd:
                case Instruction::OpID::FSub:
                case Instruction::OpID::FMul:
                case Instruction::OpID::FDiv: {
                    const auto *bin = dynamic_cast<const BinaryInst *>(inst);
                    if (bin == nullptr) {
                        return {false, "binary instruction type mismatch"};
                    }
                    if (bin->lhs()->type() != bin->rhs()->type() ||
                        bin->lhs()->type() != bin->type()) {
                        return {false,
                                "binary instruction type mismatch between operands and result"};
                    }
                    break;
                }
                case Instruction::OpID::ICmp:
                case Instruction::OpID::FCmp: {
                    const auto *cmp = dynamic_cast<const CmpInst *>(inst);
                    if (cmp == nullptr) {
                        return {false, "compare instruction type mismatch"};
                    }
                    if (cmp->lhs()->type() != cmp->rhs()->type()) {
                        return {false, "compare operands must have same type"};
                    }
                    auto *result_ty = dynamic_cast<IntegerType *>(cmp->type());
                    if (result_ty == nullptr || result_ty->bit_width() != 1) {
                        return {false, "compare result must be i1"};
                    }
                    if (inst->op() == Instruction::OpID::ICmp &&
                        !cmp->lhs()->type()->is_integer()) {
                        return {false, "icmp operands must be integer"};
                    }
                    if (inst->op() == Instruction::OpID::FCmp && !cmp->lhs()->type()->is_float()) {
                        return {false, "fcmp operands must be float"};
                    }
                    break;
                }
                case Instruction::OpID::GetElementPtr: {
                    const auto *gep = dynamic_cast<const GetElementPtrInst *>(inst);
                    if (gep == nullptr) {
                        return {false, "gep instruction type mismatch"};
                    }
                    if (!gep->base_ptr()->type()->is_pointer() || !gep->type()->is_pointer()) {
                        return {false, "gep expects pointer base and pointer result"};
                    }
                    for (auto *index : gep->indices()) {
                        if (!index->type()->is_integer()) {
                            return {false, "gep index must be integer"};
                        }
                    }
                    break;
                }
                case Instruction::OpID::Phi: {
                    const auto *phi = dynamic_cast<const PhiInst *>(inst);
                    if (phi == nullptr) {
                        return {false, "phi instruction type mismatch"};
                    }
                    if (phi->incoming().size() != block->predecessors().size()) {
                        return {false, "phi incoming count does not match predecessor count"};
                    }
                    for (const auto &item : phi->incoming()) {
                        if (item.first->type() != phi->type()) {
                            return {false, "phi incoming value type mismatch"};
                        }
                        const auto &preds = block->predecessors();
                        if (std::find(preds.begin(), preds.end(), item.second) == preds.end()) {
                            return {false, "phi incoming predecessor is not a CFG predecessor"};
                        }
                    }
                    break;
                }
                default:
                    break;
                }
            }

            if (!block->has_terminator()) {
                return {false, "block %" + block->name() + " has no terminator"};
            }
        }
    }

    return {true, "ok"};
}

} // namespace oir
