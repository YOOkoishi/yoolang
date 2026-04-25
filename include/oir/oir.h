#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oir {

class Type {
  public:
    enum class TypeID {
        Void,
        Label,
        Integer,
        Float,
        Pointer,
        Array,
        Function,
    };

    explicit Type(TypeID id);
    virtual ~Type() = default;

    TypeID id() const;

    bool is_void() const;
    bool is_label() const;
    bool is_integer() const;
    bool is_float() const;
    bool is_pointer() const;
    bool is_array() const;
    bool is_function() const;

    virtual std::string print() const = 0;

  private:
    TypeID id_;
};

class VoidType final : public Type {
  public:
    VoidType();
    std::string print() const override;
};

class LabelType final : public Type {
  public:
    LabelType();
    std::string print() const override;
};

class IntegerType final : public Type {
  public:
    explicit IntegerType(std::size_t bit_width);

    std::size_t bit_width() const;
    std::string print() const override;

  private:
    std::size_t bit_width_;
};

class FloatType final : public Type {
  public:
    FloatType();
    std::string print() const override;
};

class PointerType final : public Type {
  public:
    explicit PointerType(Type *element_type);

    Type *element_type() const;
    std::string print() const override;

  private:
    Type *element_type_;
};

class ArrayType final : public Type {
  public:
    ArrayType(Type *element_type, std::size_t element_count);

    Type *element_type() const;
    std::size_t element_count() const;
    std::string print() const override;

  private:
    Type *element_type_;
    std::size_t element_count_;
};

class FunctionType final : public Type {
  public:
    FunctionType(Type *return_type, std::vector<Type *> param_types);

    Type *return_type() const;
    const std::vector<Type *> &param_types() const;
    std::string print() const override;

  private:
    Type *return_type_;
    std::vector<Type *> param_types_;
};

class TypeContext {
  public:
    TypeContext();

    VoidType *void_ty() const;
    LabelType *label_ty() const;
    IntegerType *int1_ty() const;
    IntegerType *int32_ty() const;
    FloatType *float_ty() const;

    PointerType *ptr_ty(Type *element_type);
    ArrayType *array_ty(Type *element_type, std::size_t element_count);
    FunctionType *func_ty(Type *return_type, const std::vector<Type *> &param_types);

  private:
    std::unique_ptr<VoidType> void_ty_;
    std::unique_ptr<LabelType> label_ty_;
    std::unique_ptr<IntegerType> int1_ty_;
    std::unique_ptr<IntegerType> int32_ty_;
    std::unique_ptr<FloatType> float_ty_;
    std::vector<std::unique_ptr<Type>> owned_composite_types_;
    std::unordered_map<std::string, PointerType *> pointer_types_;
    std::unordered_map<std::string, ArrayType *> array_types_;
    std::unordered_map<std::string, FunctionType *> function_types_;
};

class Value {
  public:
    Value(Type *type, const std::string &name = "");
    virtual ~Value() = default;

    Type *type() const;
    const std::string &name() const;
    void set_name(const std::string &name);

    virtual std::string print() const = 0;

  private:
    Type *type_;
    std::string name_;
};

class ConstantInt final : public Value {
  public:
    ConstantInt(Type *type, std::int64_t value);

    std::int64_t value() const;
    std::string print() const override;

  private:
    std::int64_t value_;
};

class ConstantFloat final : public Value {
  public:
    ConstantFloat(Type *type, float value);

    float value() const;
    std::string print() const override;

  private:
    float value_;
};

class ConstantZero final : public Value {
  public:
    explicit ConstantZero(Type *type);
    std::string print() const override;
};

class UndefValue final : public Value {
  public:
    explicit UndefValue(Type *type);
    std::string print() const override;
};

class User : public Value {
  public:
    User(Type *type, const std::string &name = "");

    void add_operand(Value *value);
    Value *operand(std::size_t index) const;
    std::size_t operand_count() const;
    const std::vector<Value *> &operands() const;

  protected:
    std::vector<Value *> operands_;
};

class BasicBlock;
class Function;
class Module;

class Instruction : public User {
  public:
    enum class OpID {
        Ret,
        Br,
        Add,
        Sub,
        Mul,
        SDiv,
        SRem,
        FAdd,
        FSub,
        FMul,
        FDiv,
        ICmp,
        FCmp,
        Alloca,
        Load,
        Store,
        GetElementPtr,
        Call,
        ZExt,
        SIToFP,
        FPToSI,
        Phi,
    };

    Instruction(Type *type, OpID op, BasicBlock *parent, const std::string &name = "");
    ~Instruction() override = default;

    OpID op() const;
    BasicBlock *parent() const;
    void set_parent(BasicBlock *parent);
    bool is_terminator() const;

  private:
    OpID op_;
    BasicBlock *parent_;
};

class BinaryInst final : public Instruction {
  public:
    BinaryInst(Type *type, OpID op, Value *lhs, Value *rhs, BasicBlock *parent,
               const std::string &name = "");

    Value *lhs() const;
    Value *rhs() const;
    std::string print() const override;
};

enum class CmpPred { EQ, NE, LT, LE, GT, GE };

class CmpInst final : public Instruction {
  public:
    CmpInst(Type *result_type, OpID op, CmpPred pred, Value *lhs, Value *rhs, BasicBlock *parent,
            const std::string &name = "");

    CmpPred pred() const;
    Value *lhs() const;
    Value *rhs() const;
    std::string print() const override;

  private:
    CmpPred pred_;
};

class CastInst final : public Instruction {
  public:
    CastInst(Type *dst_type, OpID op, Value *src, BasicBlock *parent, const std::string &name = "");

    Value *src() const;
    std::string print() const override;
};

class AllocaInst final : public Instruction {
  public:
    AllocaInst(Type *ptr_type, Type *allocated_type, BasicBlock *parent,
               const std::string &name = "");

    Type *allocated_type() const;
    std::string print() const override;

  private:
    Type *allocated_type_;
};

class GetElementPtrInst final : public Instruction {
  public:
    GetElementPtrInst(Type *ptr_type, Value *base_ptr, const std::vector<Value *> &indices,
                      BasicBlock *parent, const std::string &name = "");

    Value *base_ptr() const;
    std::vector<Value *> indices() const;
    std::string print() const override;
};

class LoadInst final : public Instruction {
  public:
    LoadInst(Type *loaded_type, Value *ptr, BasicBlock *parent, const std::string &name = "");

    Value *ptr() const;
    std::string print() const override;
};

class StoreInst final : public Instruction {
  public:
    StoreInst(Type *void_type, Value *value, Value *ptr, BasicBlock *parent);

    Value *value() const;
    Value *ptr() const;
    std::string print() const override;
};

class CallInst final : public Instruction {
  public:
    CallInst(Type *return_type, Value *callee, const std::vector<Value *> &args, BasicBlock *parent,
             const std::string &name = "");

    Value *callee() const;
    std::vector<Value *> args() const;
    std::string print() const override;
};

class ReturnInst final : public Instruction {
  public:
    ReturnInst(Type *void_type, Value *value, BasicBlock *parent);

    bool has_value() const;
    Value *value() const;
    std::string print() const override;
};

class BranchInst final : public Instruction {
  public:
    BranchInst(Type *void_type, BasicBlock *target, BasicBlock *parent);
    BranchInst(Type *void_type, Value *cond, BasicBlock *true_bb, BasicBlock *false_bb,
               BasicBlock *parent);

    bool is_conditional() const;
    Value *cond() const;
    BasicBlock *true_bb() const;
    BasicBlock *false_bb() const;
    BasicBlock *target_bb() const;
    std::string print() const override;
};

class PhiInst final : public Instruction {
  public:
    PhiInst(Type *type, BasicBlock *parent, const std::string &name = "");

    void add_incoming(Value *value, BasicBlock *from);
    const std::vector<std::pair<Value *, BasicBlock *>> &incoming() const;
    std::string print() const override;

  private:
    std::vector<std::pair<Value *, BasicBlock *>> incoming_;
};

class Argument final : public Value {
  public:
    Argument(Type *type, const std::string &name, Function *parent, std::size_t index);

    Function *parent() const;
    std::size_t index() const;
    std::string print() const override;

  private:
    Function *parent_;
    std::size_t index_;
};

class BasicBlock final : public Value {
  public:
    BasicBlock(Type *label_type, const std::string &name, Function *parent);

    Instruction *append_instruction(std::unique_ptr<Instruction> inst);
    Instruction *insert_before_terminator(std::unique_ptr<Instruction> inst);
    bool has_terminator() const;
    Instruction *terminator() const;

    void add_predecessor(BasicBlock *pred);
    void add_successor(BasicBlock *succ);
    const std::vector<BasicBlock *> &predecessors() const;
    const std::vector<BasicBlock *> &successors() const;

    std::list<std::unique_ptr<Instruction>> &instructions();
    const std::list<std::unique_ptr<Instruction>> &instructions() const;

    Function *parent() const;
    std::string print() const override;

  private:
    Function *parent_;
    std::list<std::unique_ptr<Instruction>> instructions_;
    std::vector<BasicBlock *> predecessors_;
    std::vector<BasicBlock *> successors_;
};

class Function final : public Value {
  public:
    Function(FunctionType *type, const std::string &name, Module *parent, bool is_external = false);

    FunctionType *function_type() const;
    Type *return_type() const;
    Module *parent() const;
    bool is_external() const;
    void set_external(bool is_external);

    Argument *add_argument(Type *type, const std::string &name);
    BasicBlock *create_block(const std::string &name = "");
    void erase_block(BasicBlock *block);
    BasicBlock *entry_block() const;

    std::vector<std::unique_ptr<Argument>> &args();
    const std::vector<std::unique_ptr<Argument>> &args() const;
    std::list<std::unique_ptr<BasicBlock>> &blocks();
    const std::list<std::unique_ptr<BasicBlock>> &blocks() const;

    std::string print() const override;

  private:
    Module *parent_;
    bool is_external_;
    std::size_t next_block_id_;
    std::vector<std::unique_ptr<Argument>> args_;
    std::list<std::unique_ptr<BasicBlock>> blocks_;
};

class GlobalVariable final : public Value {
  public:
    GlobalVariable(Type *ptr_type, Type *value_type, const std::string &name, bool is_const,
                   Value *init_value);

    Type *value_type() const;
    bool is_const() const;
    Value *init_value() const;
    void set_initializer_literal(std::string literal);
    const std::string &initializer_literal() const;
    std::string print() const override;

  private:
    Type *value_type_;
    bool is_const_;
    Value *init_value_;
    std::string initializer_literal_;
};

class Module final {
  public:
    explicit Module(const std::string &name);

    const std::string &name() const;
    TypeContext &types();
    const TypeContext &types() const;

    Function *create_function(const std::string &name, FunctionType *type,
                              bool is_external = false);
    Function *get_function(const std::string &name) const;

    GlobalVariable *create_global(const std::string &name, Type *value_type, bool is_const,
                                  Value *init_value = nullptr);
    GlobalVariable *get_global(const std::string &name) const;

    ConstantInt *create_i1(bool value);
    ConstantInt *create_i32(std::int64_t value);
    ConstantFloat *create_f32(float value);
    ConstantZero *create_zero(Type *type);
    UndefValue *create_undef(Type *type);

    std::vector<std::unique_ptr<GlobalVariable>> &globals();
    const std::vector<std::unique_ptr<GlobalVariable>> &globals() const;
    std::vector<std::unique_ptr<Function>> &functions();
    const std::vector<std::unique_ptr<Function>> &functions() const;

    std::string print() const;
    bool verify(std::string *message = nullptr) const;

  private:
    std::string name_;
    TypeContext types_;
    std::vector<std::unique_ptr<GlobalVariable>> globals_;
    std::vector<std::unique_ptr<Function>> functions_;
    std::vector<std::unique_ptr<Value>> owned_constants_;
    std::unordered_map<std::string, Function *> function_table_;
    std::unordered_map<std::string, GlobalVariable *> global_table_;
};

class IRBuilder final {
  public:
    explicit IRBuilder(Module *module);

    Module *module() const;
    BasicBlock *insert_block() const;
    void set_insert_point(BasicBlock *block);
    void clear_insert_point();

    ConstantInt *i1(bool value) const;
    ConstantInt *i32(std::int64_t value) const;
    ConstantFloat *f32(float value) const;
    ConstantZero *zero(Type *type) const;
    UndefValue *undef(Type *type) const;

    BinaryInst *create_binary(Instruction::OpID op, Value *lhs, Value *rhs,
                              const std::string &name = "");
    CmpInst *create_icmp(CmpPred pred, Value *lhs, Value *rhs, const std::string &name = "");
    CmpInst *create_fcmp(CmpPred pred, Value *lhs, Value *rhs, const std::string &name = "");
    CastInst *create_zext(Value *src, Type *dst_type, const std::string &name = "");
    CastInst *create_sitofp(Value *src, Type *dst_type, const std::string &name = "");
    CastInst *create_fptosi(Value *src, Type *dst_type, const std::string &name = "");

    AllocaInst *create_alloca(Type *allocated_type, const std::string &name = "");
    LoadInst *create_load(Value *ptr, Type *loaded_type, const std::string &name = "");
    StoreInst *create_store(Value *value, Value *ptr);
    GetElementPtrInst *create_gep(Value *base_ptr, Type *result_ptr_type,
                                  const std::vector<Value *> &indices,
                                  const std::string &name = "");

    CallInst *create_call(Value *callee, Type *return_type, const std::vector<Value *> &args,
                          const std::string &name = "");
    ReturnInst *create_ret(Value *value = nullptr);
    BranchInst *create_br(BasicBlock *target);
    BranchInst *create_cond_br(Value *cond, BasicBlock *true_bb, BasicBlock *false_bb);
    PhiInst *create_phi(Type *type, const std::string &name = "");

  private:
    template <typename T> T *append(std::unique_ptr<T> inst) {
        assert(insert_block_ != nullptr && "insert point is not set");
        return static_cast<T *>(insert_block_->append_instruction(std::move(inst)));
    }

    Module *module_;
    BasicBlock *insert_block_;
};

struct VerifyResult {
    bool ok;
    std::string message;
};

class Verifier final {
  public:
    static VerifyResult verify_module(const Module &module);
};

} // namespace oir
