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

struct ElementCount final {
    std::uint64_t min_lanes;
    bool scalable;

    explicit ElementCount(std::uint64_t min_lanes, bool scalable = false);

    static ElementCount get_fixed(std::uint64_t lanes);
    static ElementCount get_scalable(std::uint64_t min_lanes);

    bool is_fixed() const;
    bool is_scalable() const;

    bool operator==(const ElementCount &other) const;
    bool operator!=(const ElementCount &other) const;
};

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
        Vector,
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
    bool is_vector() const;

    // These predicates deliberately keep scalar and vector categories
    // separate.  Existing is_integer()/is_float() callers retain their
    // scalar-only meaning.
    bool is_scalar_integer() const;
    bool is_scalar_float() const;
    bool is_scalar_numeric() const;
    bool is_scalar() const;
    bool is_fixed_vector() const;
    bool is_scalable_vector() const;
    bool is_mask() const;

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
    FunctionType(Type *return_type, std::vector<Type *> param_types, bool is_variadic = false);

    Type *return_type() const;
    const std::vector<Type *> &param_types() const;
    bool is_variadic() const;
    std::string print() const override;

  private:
    Type *return_type_;
    std::vector<Type *> param_types_;
    bool is_variadic_;
};

class VectorType final : public Type {
  public:
    VectorType(Type *element_type, ElementCount element_count);

    Type *element_type() const;
    const ElementCount &element_count() const;
    bool is_mask() const;
    bool is_integer_vector() const;
    bool is_float_vector() const;
    std::string print() const override;

  private:
    Type *element_type_;
    ElementCount element_count_;
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
    FunctionType *func_ty(Type *return_type, const std::vector<Type *> &param_types,
                          bool is_variadic = false);
    VectorType *vector_ty(Type *element_type, ElementCount element_count);
    VectorType *fixed_vector_ty(Type *element_type, std::uint64_t lanes);
    VectorType *scalable_vector_ty(Type *element_type, std::uint64_t min_lanes);

  private:
    struct ArrayTypeKey {
        Type *element_type;
        std::size_t element_count;

        bool operator==(const ArrayTypeKey &other) const;
    };

    struct ArrayTypeKeyHash {
        std::size_t operator()(const ArrayTypeKey &key) const;
    };

    struct FunctionTypeKey {
        Type *return_type;
        std::vector<Type *> param_types;
        bool is_variadic;

        bool operator==(const FunctionTypeKey &other) const;
    };

    struct FunctionTypeKeyHash {
        std::size_t operator()(const FunctionTypeKey &key) const;
    };

    struct VectorTypeKey {
        Type *element_type;
        ElementCount element_count;

        bool operator==(const VectorTypeKey &other) const;
    };

    struct VectorTypeKeyHash {
        std::size_t operator()(const VectorTypeKey &key) const;
    };

    std::unique_ptr<VoidType> void_ty_;
    std::unique_ptr<LabelType> label_ty_;
    std::unique_ptr<IntegerType> int1_ty_;
    std::unique_ptr<IntegerType> int32_ty_;
    std::unique_ptr<FloatType> float_ty_;
    std::vector<std::unique_ptr<Type>> owned_composite_types_;
    std::unordered_map<Type *, PointerType *> pointer_types_;
    std::unordered_map<ArrayTypeKey, ArrayType *, ArrayTypeKeyHash> array_types_;
    std::unordered_map<FunctionTypeKey, FunctionType *, FunctionTypeKeyHash> function_types_;
    std::unordered_map<VectorTypeKey, VectorType *, VectorTypeKeyHash> vector_types_;
};

class User;

class Value {
  public:
    struct Use {
        User *user = nullptr;
        std::size_t operand_index = 0;
    };

    Value(Type *type, const std::string &name = "");
    virtual ~Value() = default;

    Type *type() const;
    const std::string &name() const;
    void set_name(const std::string &name);

    const std::vector<Use> &uses() const;
    std::vector<User *> users() const;
    std::size_t use_count() const;
    bool has_uses() const;
    void replace_all_uses_with(Value *new_value);

    virtual std::string print() const = 0;

  private:
    void set_type(Type *type);
    void add_use(User *user, std::size_t operand_index);
    void remove_use(User *user, std::size_t operand_index);

    Type *type_;
    std::string name_;
    std::vector<Use> uses_;

    friend class User;
    friend class Function;
};

class Constant : public Value {
  public:
    ~Constant() override = default;

    std::string print() const override = 0;

  protected:
    Constant(Type *type, const std::string &name);
};

class ConstantInt final : public Constant {
  public:
    ConstantInt(Type *type, std::int64_t value);

    std::int64_t value() const;
    std::string print() const override;

  private:
    std::int64_t value_;
};

class ConstantFloat final : public Constant {
  public:
    ConstantFloat(Type *type, float value);

    float value() const;
    std::string print() const override;

  private:
    float value_;
};

class ConstantAggregateZero final : public Constant {
  public:
    explicit ConstantAggregateZero(Type *type);
    std::string print() const override;
};

// Compatibility name retained for existing scalar optimization and lowering
// code.  The structured constant is valid for scalar and aggregate zero values.
using ConstantZero = ConstantAggregateZero;

class ConstantArray final : public Constant {
  public:
    ConstantArray(ArrayType *type, std::vector<Constant *> elements);

    ArrayType *array_type() const;
    const std::vector<Constant *> &elements() const;
    std::string print() const override;

  private:
    std::vector<Constant *> elements_;
};

class ConstantVector final : public Constant {
  public:
    ConstantVector(VectorType *type, std::vector<Constant *> elements);

    VectorType *vector_type() const;
    const std::vector<Constant *> &elements() const;
    std::string print() const override;

  private:
    std::vector<Constant *> elements_;
};

class ConstantMask final : public Constant {
  public:
    // Lane zero is the least-significant bit of packed_bits[0].  Bytes must
    // exactly cover the fixed lane count, and unused high bits must be zero.
    ConstantMask(VectorType *type, std::vector<std::uint8_t> packed_bits);

    VectorType *mask_type() const;
    std::uint64_t lane_count() const;
    bool lane(std::uint64_t index) const;
    const std::vector<std::uint8_t> &packed_bits() const;
    std::string print() const override;

  private:
    std::vector<std::uint8_t> packed_bits_;
};

class UndefValue final : public Value {
  public:
    explicit UndefValue(Type *type);
    std::string print() const override;
};

class User : public Value {
  public:
    User(Type *type, const std::string &name = "");
    ~User() override;

    void add_operand(Value *value);
    Value *operand(std::size_t index) const;
    virtual void set_operand(std::size_t index, Value *value);
    void replace_operand(Value *old_value, Value *new_value);
    std::size_t replace_operands(Value *old_value, Value *new_value);
    void drop_all_operands();
    std::size_t operand_count() const;
    const std::vector<Value *> &operands() const;

  protected:
    void erase_operands(std::size_t first, std::size_t count);

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
        MemZero,
        ZExt,
        SIToFP,
        FPToSI,
        Phi,
        And,
        Or,
        Xor,
        SetVL,
        Splat,
        StepVector,
        ExtractElement,
        InsertElement,
        ShuffleVector,
        VectorSelect,
        VectorCast,
        FixedABIExtractLane,
        FixedABIPack,
        FixedABIObjectLoadLane,
        FixedABIObjectStoreLane,
        VPBinary,
        VPCmp,
        VPLoad,
        VPStore,
        MaskedLoad,
        MaskedStore,
        VPGather,
        VPScatter,
        VPReduction,
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

enum class TailPolicy { Agnostic, Undisturbed };
enum class MaskPolicy { Agnostic, Undisturbed };

struct VPMetadata final {
    Value *active_mask = nullptr;
    Value *evl = nullptr;
    Value *passthrough = nullptr;
    TailPolicy tail_policy = TailPolicy::Agnostic;
    MaskPolicy mask_policy = MaskPolicy::Agnostic;
};

enum class VectorCastKind { ZExt, SIToFP, FPToSI, Bitcast };
enum class ReductionKind { Add, Mul, Min, Max, And, Or, Xor };

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

// Target-independent VLA length selection.  The scalable vector type carries
// the lane shape and element/SEW semantics; the single i32 operand is the
// requested application vector length (AVL), and the i32 result is the actual
// runtime vector length selected for that configuration.
class SetVLInst final : public Instruction {
  public:
    SetVLInst(Type *result_type, VectorType *vector_type, Value *avl, BasicBlock *parent,
              const std::string &name = "");

    VectorType *vector_type() const;
    Value *avl() const;
    std::string print() const override;

  private:
    VectorType *vector_type_;
};

class SplatInst final : public Instruction {
  public:
    SplatInst(VectorType *result_type, Value *scalar, BasicBlock *parent,
              const std::string &name = "");

    Value *scalar() const;
    std::string print() const override;
};

class StepVectorInst final : public Instruction {
  public:
    StepVectorInst(VectorType *result_type, BasicBlock *parent, const std::string &name = "");

    std::string print() const override;
};

class ExtractElementInst final : public Instruction {
  public:
    ExtractElementInst(Type *result_type, Value *vector, Value *index, BasicBlock *parent,
                       const std::string &name = "");

    Value *vector() const;
    Value *index() const;
    std::string print() const override;
};

class InsertElementInst final : public Instruction {
  public:
    InsertElementInst(VectorType *result_type, Value *vector, Value *element, Value *index,
                      BasicBlock *parent, const std::string &name = "");

    Value *vector() const;
    Value *element() const;
    Value *index() const;
    std::string print() const override;
};

class ShuffleVectorInst final : public Instruction {
  public:
    ShuffleVectorInst(VectorType *result_type, Value *lhs, Value *rhs,
                      std::vector<std::int64_t> shuffle_mask, BasicBlock *parent,
                      const std::string &name = "");

    Value *lhs() const;
    Value *rhs() const;
    const std::vector<std::int64_t> &shuffle_mask() const;
    std::string print() const override;

  private:
    std::vector<std::int64_t> shuffle_mask_;
};

class VectorSelectInst final : public Instruction {
  public:
    VectorSelectInst(VectorType *result_type, Value *condition, Value *true_value,
                     Value *false_value, BasicBlock *parent, const std::string &name = "");

    Value *condition() const;
    Value *true_value() const;
    Value *false_value() const;
    std::string print() const override;
};

class VectorCastInst final : public Instruction {
  public:
    VectorCastInst(VectorType *result_type, VectorCastKind kind, Value *source, BasicBlock *parent,
                   const std::string &name = "");

    VectorCastKind kind() const;
    Value *source() const;
    std::string print() const override;

  private:
    VectorCastKind kind_;
};

// Fixed-vector standard-ABI boundary dialect.  These operations preserve the
// public aggregate value and object types while allowing all computation in a
// function body to use scalar lanes.  They are deliberately fixed-only and
// carry a constant lane attribute; scalable vectors and dynamic boundary lane
// selection are not representable.
class FixedABIExtractLaneInst final : public Instruction {
  public:
    FixedABIExtractLaneInst(Type *result_type, Value *aggregate, std::uint64_t lane_index,
                            BasicBlock *parent, const std::string &name = "");

    Value *aggregate() const;
    std::uint64_t lane_index() const;
    std::string print() const override;

  private:
    std::uint64_t lane_index_;
};

class FixedABIPackInst final : public Instruction {
  public:
    FixedABIPackInst(VectorType *result_type, const std::vector<Value *> &lane_values,
                     BasicBlock *parent, const std::string &name = "");

    const std::vector<Value *> &lane_values() const;
    std::string print() const override;
};

class FixedABIObjectLoadLaneInst final : public Instruction {
  public:
    FixedABIObjectLoadLaneInst(Type *result_type, Value *object_ptr, std::uint64_t lane_index,
                               BasicBlock *parent, const std::string &name = "");

    Value *object_ptr() const;
    std::uint64_t lane_index() const;
    std::string print() const override;

  private:
    std::uint64_t lane_index_;
};

// For mask<N> objects lane i denotes packed bit i.  A store is a bit-level
// read/modify/write: it preserves every other valid lane bit in the containing
// byte and clears bits [N, round_up(N, 8)) in the final byte.  Likewise, a mask
// FixedABIPack produces canonical packed storage with all unused high bits zero.
class FixedABIObjectStoreLaneInst final : public Instruction {
  public:
    FixedABIObjectStoreLaneInst(Type *void_type, Value *lane_value, Value *object_ptr,
                                std::uint64_t lane_index, BasicBlock *parent);

    Value *lane_value() const;
    Value *object_ptr() const;
    std::uint64_t lane_index() const;
    std::string print() const override;

  private:
    std::uint64_t lane_index_;
};

class VPInstruction : public Instruction {
  public:
    Value *active_mask() const;
    Value *evl() const;
    bool has_passthrough() const;
    Value *passthrough() const;
    TailPolicy tail_policy() const;
    MaskPolicy mask_policy() const;
    VPMetadata metadata() const;

  protected:
    VPInstruction(Type *type, OpID op, const VPMetadata &metadata, bool has_passthrough,
                  BasicBlock *parent, const std::string &name = "");
    std::size_t data_operand_offset() const;

  private:
    bool has_passthrough_;
    TailPolicy tail_policy_;
    MaskPolicy mask_policy_;
};

class VPBinaryInst final : public VPInstruction {
  public:
    VPBinaryInst(VectorType *result_type, OpID binary_op, Value *lhs, Value *rhs,
                 const VPMetadata &metadata, BasicBlock *parent, const std::string &name = "");

    OpID binary_op() const;
    Value *lhs() const;
    Value *rhs() const;
    std::string print() const override;

  private:
    OpID binary_op_;
};

class VPCmpInst final : public VPInstruction {
  public:
    VPCmpInst(VectorType *result_type, OpID comparison_op, CmpPred pred, Value *lhs, Value *rhs,
              const VPMetadata &metadata, BasicBlock *parent, const std::string &name = "");

    OpID comparison_op() const;
    CmpPred pred() const;
    Value *lhs() const;
    Value *rhs() const;
    std::string print() const override;

  private:
    OpID comparison_op_;
    CmpPred pred_;
};

// Normative VP/masked memory contract (load/store/gather/scatter): a lane with
// active_mask[lane] == false or lane >= EVL performs no memory access and must
// not touch, fault on, or otherwise observe its lane address. For VPLoad and
// VPGather, an inactive result lane takes the corresponding passthrough lane
// when its governing mask/tail policy is Undisturbed; Agnostic leaves it
// unconstrained.
class VPLoadInst final : public VPInstruction {
  public:
    VPLoadInst(OpID op, VectorType *result_type, Value *ptr, std::size_t alignment,
               const VPMetadata &metadata, BasicBlock *parent, const std::string &name = "");

    Value *ptr() const;
    std::size_t alignment() const;
    bool is_masked_form() const;
    std::string print() const override;

  private:
    std::size_t alignment_;
};

class VPStoreInst final : public VPInstruction {
  public:
    VPStoreInst(OpID op, Type *void_type, Value *value, Value *ptr, std::size_t alignment,
                const VPMetadata &metadata, BasicBlock *parent);

    Value *value() const;
    Value *ptr() const;
    std::size_t alignment() const;
    bool is_masked_form() const;
    std::string print() const override;

  private:
    std::size_t alignment_;
};

class VPGatherInst final : public VPInstruction {
  public:
    VPGatherInst(VectorType *result_type, Value *base_ptr, Value *indices, std::size_t alignment,
                 const VPMetadata &metadata, BasicBlock *parent, const std::string &name = "");

    Value *base_ptr() const;
    Value *indices() const;
    std::size_t alignment() const;
    std::string print() const override;

  private:
    std::size_t alignment_;
};

class VPScatterInst final : public VPInstruction {
  public:
    VPScatterInst(Type *void_type, Value *value, Value *base_ptr, Value *indices,
                  std::size_t alignment, const VPMetadata &metadata, BasicBlock *parent);

    Value *value() const;
    Value *base_ptr() const;
    Value *indices() const;
    std::size_t alignment() const;
    std::string print() const override;

  private:
    std::size_t alignment_;
};

class VPReductionInst final : public VPInstruction {
  public:
    VPReductionInst(Type *result_type, ReductionKind kind, bool ordered, Value *vector,
                    const VPMetadata &metadata, BasicBlock *parent, const std::string &name = "");

    ReductionKind kind() const;
    bool ordered() const;
    Value *vector() const;
    std::string print() const override;

  private:
    ReductionKind kind_;
    bool ordered_;
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

class MemZeroInst final : public Instruction {
  public:
    MemZeroInst(Type *void_type, Value *ptr, Value *byte_value, Value *byte_count,
                BasicBlock *parent);

    Value *ptr() const;
    Value *byte_value() const;
    Value *byte_count() const;
    std::string print() const override;
};

class CallInst final : public Instruction {
  public:
    CallInst(Type *return_type, Value *callee, const std::vector<Value *> &args, BasicBlock *parent,
             const std::string &name = "");

    Value *callee() const;
    std::vector<Value *> args() const;
    void remove_arg(std::size_t arg_index);
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
    void remove_incoming_from(BasicBlock *from);
    const std::vector<std::pair<Value *, BasicBlock *>> &incoming() const;
    void set_operand(std::size_t index, Value *value) override;
    std::string print() const override;

  private:
    std::vector<std::pair<Value *, BasicBlock *>> incoming_;
};

class Argument final : public Value {
  public:
    Argument(Type *type, const std::string &name, Function *parent, std::size_t index);

    Function *parent() const;
    std::size_t index() const;
    void set_index(std::size_t index);
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
    void remove_predecessor(BasicBlock *pred);
    void remove_successor(BasicBlock *succ);
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
    ~Function() override;

    FunctionType *function_type() const;
    Type *return_type() const;
    Module *parent() const;
    bool is_external() const;
    void set_external(bool is_external);
    void set_function_type(FunctionType *type);

    Argument *add_argument(Type *type, const std::string &name);
    void keep_arguments(const std::vector<bool> &keep);
    BasicBlock *create_block(const std::string &name = "");
    // Parser-only style construction hook: preserves an already-canonical
    // textual block name instead of appending the normal uniqueness suffix.
    BasicBlock *create_block_exact(const std::string &name);
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

    friend class Module;
};

class GlobalVariable final : public Value {
  public:
    GlobalVariable(Type *ptr_type, Type *value_type, const std::string &name, bool is_const,
                   Constant *initializer);

    Type *value_type() const;
    bool is_const() const;
    Constant *initializer() const;
    void set_initializer(Constant *initializer);

    // Deprecated compatibility spelling.  The initializer is now always a
    // typed Constant; this accessor remains while downstream users migrate.
    Constant *init_value() const;

    std::string print() const override;

  private:
    Type *value_type_;
    bool is_const_;
    Constant *initializer_;
};

class Module final {
  public:
    explicit Module(const std::string &name);

    // Replaces all owned IR while rebasing Function::parent() to this Module.
    // This is used by transactional transforms to restore a verified snapshot.
    void replace_with(Module &&other);

    const std::string &name() const;
    TypeContext &types();
    const TypeContext &types() const;

    Function *create_function(const std::string &name, FunctionType *type,
                              bool is_external = false);
    Function *get_function(const std::string &name) const;

    GlobalVariable *create_global(const std::string &name, Type *value_type, bool is_const,
                                  Constant *initializer = nullptr);
    GlobalVariable *get_global(const std::string &name) const;

    ConstantInt *create_i1(bool value);
    ConstantInt *create_i32(std::int64_t value);
    ConstantFloat *create_f32(float value);
    ConstantZero *create_zero(Type *type);
    ConstantArray *create_constant_array(ArrayType *type, const std::vector<Constant *> &elements);
    ConstantVector *create_constant_vector(VectorType *type,
                                           const std::vector<Constant *> &elements);
    ConstantMask *create_constant_mask(VectorType *type,
                                       const std::vector<std::uint8_t> &packed_bits);
    UndefValue *create_undef(Type *type);

    std::vector<std::unique_ptr<GlobalVariable>> &globals();
    const std::vector<std::unique_ptr<GlobalVariable>> &globals() const;
    const std::vector<std::unique_ptr<Value>> &owned_constants() const;
    std::vector<std::unique_ptr<Function>> &functions();
    const std::vector<std::unique_ptr<Function>> &functions() const;

    std::string print() const;
    bool verify(std::string *message = nullptr) const;

  private:
    std::string name_;
    TypeContext types_;
    std::vector<std::unique_ptr<Value>> owned_constants_;
    std::vector<std::unique_ptr<GlobalVariable>> globals_;
    std::vector<std::unique_ptr<Function>> functions_;
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
    SetVLInst *create_set_vl(VectorType *vector_type, Value *avl, const std::string &name = "");

    SplatInst *create_splat(VectorType *result_type, Value *scalar, const std::string &name = "");
    StepVectorInst *create_step_vector(VectorType *result_type, const std::string &name = "");
    ExtractElementInst *create_extract_element(Value *vector, Value *index,
                                               const std::string &name = "");
    InsertElementInst *create_insert_element(Value *vector, Value *element, Value *index,
                                             const std::string &name = "");
    ShuffleVectorInst *create_shuffle_vector(VectorType *result_type, Value *lhs, Value *rhs,
                                             const std::vector<std::int64_t> &shuffle_mask,
                                             const std::string &name = "");
    VectorSelectInst *create_vector_select(Value *condition, Value *true_value, Value *false_value,
                                           const std::string &name = "");
    VectorCastInst *create_vector_cast(VectorCastKind kind, VectorType *result_type, Value *source,
                                       const std::string &name = "");

    FixedABIExtractLaneInst *create_fixed_abi_extract_lane(Value *aggregate,
                                                           std::uint64_t lane_index,
                                                           const std::string &name = "");
    FixedABIPackInst *create_fixed_abi_pack(VectorType *result_type,
                                            const std::vector<Value *> &lane_values,
                                            const std::string &name = "");
    FixedABIObjectLoadLaneInst *create_fixed_abi_object_load_lane(Value *object_ptr,
                                                                  std::uint64_t lane_index,
                                                                  const std::string &name = "");
    FixedABIObjectStoreLaneInst *create_fixed_abi_object_store_lane(Value *lane_value,
                                                                    Value *object_ptr,
                                                                    std::uint64_t lane_index);

    VPBinaryInst *create_vp_binary(Instruction::OpID binary_op, Value *lhs, Value *rhs,
                                   Value *active_mask, Value *evl, Value *passthrough,
                                   TailPolicy tail_policy, MaskPolicy mask_policy,
                                   const std::string &name = "");
    VPCmpInst *create_vp_icmp(CmpPred pred, Value *lhs, Value *rhs, Value *active_mask, Value *evl,
                              Value *passthrough, TailPolicy tail_policy, MaskPolicy mask_policy,
                              const std::string &name = "");
    VPCmpInst *create_vp_fcmp(CmpPred pred, Value *lhs, Value *rhs, Value *active_mask, Value *evl,
                              Value *passthrough, TailPolicy tail_policy, MaskPolicy mask_policy,
                              const std::string &name = "");

    VPLoadInst *create_vp_load(VectorType *result_type, Value *ptr, Value *active_mask, Value *evl,
                               Value *passthrough, TailPolicy tail_policy, MaskPolicy mask_policy,
                               std::size_t alignment, const std::string &name = "");
    VPStoreInst *create_vp_store(Value *value, Value *ptr, Value *active_mask, Value *evl,
                                 TailPolicy tail_policy, MaskPolicy mask_policy,
                                 std::size_t alignment);
    VPLoadInst *create_masked_load(VectorType *result_type, Value *ptr, Value *active_mask,
                                   Value *evl, Value *passthrough, TailPolicy tail_policy,
                                   MaskPolicy mask_policy, std::size_t alignment,
                                   const std::string &name = "");
    VPStoreInst *create_masked_store(Value *value, Value *ptr, Value *active_mask, Value *evl,
                                     TailPolicy tail_policy, MaskPolicy mask_policy,
                                     std::size_t alignment);
    VPGatherInst *create_vp_gather(VectorType *result_type, Value *base_ptr, Value *indices,
                                   Value *active_mask, Value *evl, Value *passthrough,
                                   TailPolicy tail_policy, MaskPolicy mask_policy,
                                   std::size_t alignment, const std::string &name = "");
    VPScatterInst *create_vp_scatter(Value *value, Value *base_ptr, Value *indices,
                                     Value *active_mask, Value *evl, TailPolicy tail_policy,
                                     MaskPolicy mask_policy, std::size_t alignment);
    VPReductionInst *create_vp_reduction(ReductionKind kind, bool ordered, Value *vector,
                                         Value *active_mask, Value *evl, Value *passthrough,
                                         TailPolicy tail_policy, MaskPolicy mask_policy,
                                         const std::string &name = "");

    AllocaInst *create_alloca(Type *allocated_type, const std::string &name = "");
    LoadInst *create_load(Value *ptr, Type *loaded_type, const std::string &name = "");
    StoreInst *create_store(Value *value, Value *ptr);
    MemZeroInst *create_memzero(Value *ptr, Value *byte_count);
    MemZeroInst *create_memset(Value *ptr, Value *byte_value, Value *byte_count);
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
