#pragma once

#include <iostream>
#include <list>
#include <memory>
#include <string>
#include <vector>

namespace ir {

class Type;
class Value;
class User;
class Instruction;
class BasicBlock;
class Function;
class Module;

// ============================================================================
// 1. 类型系统 (Type System) - 解决之前弱类型、无法校验的问题
// ============================================================================
class Type {
  public:
    enum TypeID {
        VoidTyID,    // void
        LabelTyID,   // Basic Block
        IntegerTyID, // i1, i32
        FloatTyID,   // float
        PointerTyID, // ptr
        ArrayTyID,   // array
        FunctionTyID // function
    };
    explicit Type(TypeID id) : id_(id) {
    }
    virtual ~Type() = default;
    TypeID get_id() const {
        return id_;
    }

    virtual std::string print() const = 0;

  protected:
    TypeID id_;
};

// ============================================================================
// 2. 基石：Value 与 User (彻底抛弃基于 std::string 的名字查找)
// ============================================================================

// Value 是所有可以作为操作数的东西（变量、常量、指令的结果）
class Value {
  public:
    explicit Value(Type *type, const std::string &name = "") : type_(type), name_(name) {
    }
    virtual ~Value() = default;

    Type *get_type() const {
        return type_;
    }
    std::string get_name() const {
        return name_;
    }
    void set_name(const std::string &name) {
        name_ = name;
    }

    // 未来优化：这里可以记录谁使用了我（Use-Def 链），做 DCE 和常量传播非常简单
    // std::list<Use*> use_list_;

    virtual std::string print() const = 0;

  protected:
    Type *type_;
    std::string name_;
};

// User 是会使用 Value 的东西（也就是 Instruction）
class User : public Value {
  public:
    User(Type *type, const std::string &name = "") : Value(type, name) {
    }

    void add_operand(Value *val) {
        operands_.push_back(val);
    }
    Value *get_operand(size_t i) const {
        return operands_[i];
    }
    size_t get_num_operands() const {
        return operands_.size();
    }

  protected:
    std::vector<Value *> operands_; // 核心：这里存的是真实的内存指针，而不是名字字符串！
};

// ============================================================================
// 3. 常量（Constant），它们本身也是一种 Value
// ============================================================================
class ConstantInt : public Value {
  public:
    int value_;
    ConstantInt(Type *ty, int val) : Value(ty, std::to_string(val)), value_(val) {
    }
    std::string print() const override {
        return std::to_string(value_);
    }
};

class ConstantFloat : public Value {
  public:
    float value_;
    ConstantFloat(Type *ty, float val) : Value(ty, std::to_string(val)), value_(val) {
    }
    std::string print() const override {
        return std::to_string(value_);
    }
};

// ============================================================================
// 4. 指令集 (Instructions)
// ============================================================================
class Instruction : public User {
  public:
    enum OpID {
        Ret,
        Br, // Terminator
        Add,
        Sub,
        Mul,
        Div,
        Mod, // Binary
        FAdd,
        FSub,
        FMul,
        FDiv, // Float Binary
        Alloca,
        Load,
        Store,
        GetElementPtr, // Memory
        Call,
        Zext,
        Phi // Other & SSA
    };

    Instruction(Type *ty, OpID op, BasicBlock *parent, const std::string &name = "")
        : User(ty, name), op_id_(op), parent_(parent) {
    }

    OpID get_op() const {
        return op_id_;
    }
    BasicBlock *get_parent() const {
        return parent_;
    }

  protected:
    OpID op_id_;
    BasicBlock *parent_;
};

// 二元运算
class BinaryInst : public Instruction {
  public:
    BinaryInst(Type *ty, OpID op, Value *lhs, Value *rhs, BasicBlock *parent,
               const std::string &name = "")
        : Instruction(ty, op, parent, name) {
        add_operand(lhs);
        add_operand(rhs);
    }
    std::string print() const override {
        return name_ + " = binary_op " + operands_[0]->get_name() + ", " + operands_[1]->get_name();
    }
};

// 内存分配 (Alloca 应该返回 PointerType)
class AllocaInst : public Instruction {
  public:
    Type *allocated_type_;
    AllocaInst(Type *ptr_ty, Type *alloc_ty, BasicBlock *parent, const std::string &name = "")
        : Instruction(ptr_ty, Alloca, parent, name), allocated_type_(alloc_ty) {
    }
    std::string print() const override {
        return name_ + " = alloca " + allocated_type_->print();
    }
};

// 寻址 (GEP)
class GetElementPtrInst : public Instruction {
  public:
    GetElementPtrInst(Type *ptr_ty, Value *ptr, Value *idx, BasicBlock *parent,
                      const std::string &name = "")
        : Instruction(ptr_ty, GetElementPtr, parent, name) {
        add_operand(ptr);
        add_operand(idx);
    }
    std::string print() const override {
        return name_ + " = gep " + operands_[0]->get_name() + "[" + operands_[1]->get_name() + "]";
    }
};

// 内存读写
class LoadInst : public Instruction {
  public:
    LoadInst(Type *ty, Value *ptr, BasicBlock *parent, const std::string &name = "")
        : Instruction(ty, Load, parent, name) {
        add_operand(ptr);
    }
    std::string print() const override {
        return name_ + " = load " + operands_[0]->get_name();
    }
};

class StoreInst : public Instruction {
  public:
    StoreInst(Value *val, Value *ptr, BasicBlock *parent)
        : Instruction(nullptr /* void */, Store, parent, "") {
        add_operand(val);
        add_operand(ptr);
    }
    std::string print() const override {
        return "store " + operands_[0]->get_name() + " -> " + operands_[1]->get_name();
    }
};

// 函数调用
class CallInst : public Instruction {
  public:
    CallInst(Type *ret_ty, Function *func, std::vector<Value *> args, BasicBlock *parent,
             const std::string &name = "")
        : Instruction(ret_ty, Call, parent, name) {
        add_operand((Value *)func); // operand[0] 是函数本身
        for (auto arg : args)
            add_operand(arg);
    }
    std::string print() const override {
        return name_ + " = call " + operands_[0]->get_name();
    }
};

// 返回指令
class ReturnInst : public Instruction {
  public:
    ReturnInst(Value *val, BasicBlock *parent) : Instruction(nullptr, Ret, parent, "") {
        if (val)
            add_operand(val);
    }
    std::string print() const override {
        return "ret " + (operands_.empty() ? "void" : operands_[0]->get_name());
    }
};

// 分支跳转指令
class BranchInst : public Instruction {
  public:
    // 条件分支
    BranchInst(Value *cond, BasicBlock *true_bb, BasicBlock *false_bb, BasicBlock *parent)
        : Instruction(nullptr, Br, parent, "") {
        add_operand(cond);
        add_operand((Value *)true_bb);
        add_operand((Value *)false_bb);
    }
    // 无条件跳转
    BranchInst(BasicBlock *bb, BasicBlock *parent) : Instruction(nullptr, Br, parent, "") {
        add_operand((Value *)bb);
    }
    std::string print() const override {
        return "br ...";
    }
};

// SSA 极度关键指令：Phi 节点 (用于合并不同控制流分支的值)
class PhiInst : public Instruction {
  public:
    PhiInst(Type *ty, BasicBlock *parent, const std::string &name = "")
        : Instruction(ty, Phi, parent, name) {
    }

    void add_incoming(Value *val, BasicBlock *bb) {
        add_operand(val);
        add_operand((Value *)bb);
    }
    std::string print() const override {
        return name_ + " = phi ...";
    }
};

// ============================================================================
// 5. 结构化容器: BasicBlock, Function, Module
// ============================================================================
class BasicBlock : public Value {
  public:
    BasicBlock(const std::string &name, Function *parent)
        : Value(nullptr /* LabelTy */, name), parent_(parent) {
    }

    // 基本块拥有（独占管理）指令的生命周期，但外界引用指令用普通指针
    std::list<std::unique_ptr<Instruction>> insts_;
    Function *parent_;

    std::string print() const override {
        return name_ + ":";
    }
};

class Argument : public Value {
  public:
    Function *parent_;
    Argument(Type *ty, const std::string &name, Function *parent)
        : Value(ty, name), parent_(parent) {
    }
    std::string print() const override {
        return type_->print() + " " + name_;
    }
};

class Function : public Value {
  public:
    Function(Type *func_ty, const std::string &name, Module *parent)
        : Value(func_ty, name), parent_(parent) {
    }

    std::vector<std::unique_ptr<Argument>> args_;
    std::list<std::unique_ptr<BasicBlock>> blocks_;
    Module *parent_;

    std::string print() const override {
        return "define " + name_ + "()";
    }
};

// 全局变量也是一种用裸指针寻址的 Value
class GlobalVariable : public Value {
  public:
    bool is_const_;
    Value *init_val_;
    GlobalVariable(Type *ty, const std::string &name, bool is_const, Value *init_val)
        : Value(ty /* 其实应该是 PointerTy */, name), is_const_(is_const), init_val_(init_val) {
    }
    std::string print() const override {
        return name_ + " = global ...";
    }
};

class Module {
  public:
    std::string module_name_;
    std::vector<std::unique_ptr<GlobalVariable>> global_vars_;
    std::vector<std::unique_ptr<Function>> functions_;

    explicit Module(const std::string &name) : module_name_(name) {
    }
};

} // namespace ir
