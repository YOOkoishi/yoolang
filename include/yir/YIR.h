#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace yir {

class Operation;
class Region;

class Type;
using TypePtr = std::shared_ptr<const Type>;

class Type final {
  public:
    enum class Kind {
        I1,
        I32,
        F32,
        Void,
        Ptr,
        Array,
        Vector,
        Mask,
        Func,
    };

    static TypePtr get_i1();
    static TypePtr get_i32();
    static TypePtr get_f32();
    static TypePtr get_void();
    static TypePtr get_ptr(TypePtr pointee);
    static TypePtr get_array(std::uint64_t count, TypePtr element);
    static TypePtr get_vector(std::uint64_t count, TypePtr element);
    static TypePtr get_mask(std::uint64_t count);
    static TypePtr get_func(std::vector<TypePtr> params, TypePtr result, bool is_variadic = false);

    Kind kind() const {
        return kind_;
    }
    const TypePtr &pointee() const {
        return pointee_;
    }
    const TypePtr &element() const {
        return element_;
    }
    std::uint64_t count() const {
        return count_;
    }
    const std::vector<TypePtr> &params() const {
        return params_;
    }
    const TypePtr &result() const {
        return result_;
    }
    bool is_variadic() const {
        return is_variadic_;
    }

    bool is_integer() const;
    bool is_float() const;
    bool is_scalar() const;
    bool is_vector() const;
    bool is_mask() const;
    bool is_void() const;
    bool is_ptr() const;
    bool is_array() const;
    std::string str() const;

  private:
    explicit Type(Kind kind);
    Type(Kind kind, TypePtr nested);
    Type(Kind kind, std::uint64_t count, TypePtr element);
    Type(std::vector<TypePtr> params, TypePtr result, bool is_variadic);

    Kind kind_;
    TypePtr pointee_;
    TypePtr element_;
    std::uint64_t count_ = 0;
    std::vector<TypePtr> params_;
    TypePtr result_;
    bool is_variadic_ = false;
};

class Constant;
using ConstantPtr = std::shared_ptr<const Constant>;

class Constant {
  public:
    explicit Constant(TypePtr type);
    virtual ~Constant() = default;

    const TypePtr &type() const {
        return type_;
    }
    virtual bool is_zero() const = 0;
    virtual std::string str() const = 0;

  private:
    TypePtr type_;
};

class ConstantInt final : public Constant {
  public:
    ConstantInt(TypePtr type, std::int64_t value);
    std::int64_t value() const {
        return value_;
    }
    bool is_zero() const override;
    std::string str() const override;

  private:
    std::int64_t value_;
};

class ConstantFloat final : public Constant {
  public:
    explicit ConstantFloat(float value);
    float value() const {
        return value_;
    }
    bool is_zero() const override;
    std::string str() const override;

  private:
    float value_;
};

class ConstantAggregateZero final : public Constant {
  public:
    explicit ConstantAggregateZero(TypePtr type);
    bool is_zero() const override;
    std::string str() const override;
};

class ConstantArray final : public Constant {
  public:
    ConstantArray(TypePtr type, std::vector<ConstantPtr> elements);
    const std::vector<ConstantPtr> &elements() const {
        return elements_;
    }
    bool is_zero() const override;
    std::string str() const override;

  private:
    std::vector<ConstantPtr> elements_;
};

class ConstantVector final : public Constant {
  public:
    ConstantVector(TypePtr type, std::vector<ConstantPtr> lanes);
    const std::vector<ConstantPtr> &lanes() const {
        return lanes_;
    }
    bool is_zero() const override;
    std::string str() const override;

  private:
    std::vector<ConstantPtr> lanes_;
};

class ConstantMask final : public Constant {
  public:
    ConstantMask(TypePtr type, std::vector<std::uint8_t> packed_bytes);
    const std::vector<std::uint8_t> &packed_bytes() const {
        return packed_bytes_;
    }
    bool lane(std::uint64_t index) const;
    bool is_zero() const override;
    std::string str() const override;

  private:
    std::vector<std::uint8_t> packed_bytes_;
};

class Value {
  public:
    explicit Value(TypePtr type, std::string name = "");
    virtual ~Value() = default;

    const TypePtr &type() const {
        return type_;
    }
    void set_type(TypePtr type) {
        type_ = std::move(type);
    }

    const std::string &name() const {
        return name_;
    }
    void set_name(std::string name) {
        name_ = std::move(name);
    }

    Operation *defining_op() const {
        return defining_op_;
    }
    void set_defining_op(Operation *op) {
        defining_op_ = op;
    }

  private:
    TypePtr type_;
    std::string name_;
    Operation *defining_op_ = nullptr; // 指向创造该值的 Operation, Def (定义) 引用
};

struct ArrayInitEntry {
    std::vector<std::uint64_t> indices;
    Value *value = nullptr;
    ConstantPtr constant;
    // Transitional text bridge for existing scalar lowering. New producers
    // must populate constant instead.
    std::string literal;
};

class Operation {
  public:
    Operation(std::string op_name, std::vector<Value *> operands, TypePtr result_type = nullptr,
              std::string result_name = "");
    virtual ~Operation() = default;

    const std::string &op_name() const {
        return op_name_;
    }
    const std::vector<Value *> &operands() const {
        return operands_;
    }
    std::vector<Value *> &operands() {
        return operands_;
    }

    Value *result() {
        return result_.get();
    }
    const Value *result() const {
        return result_.get();
    }

    Region *parent() const {
        return parent_;
    }
    void set_parent(Region *parent) {
        parent_ = parent;
    }

  private:
    std::string op_name_;
    std::vector<Value *> operands_;
    std::unique_ptr<Value> result_;
    Region *parent_ = nullptr;
};

class Region {
  public:
    using OpList = std::vector<std::unique_ptr<Operation>>;

    template <typename OpT, typename... Args> OpT *append(Args &&...args) {
        auto op = std::make_unique<OpT>(std::forward<Args>(args)...);
        auto *raw = op.get();
        raw->set_parent(this);
        operations_.push_back(std::move(op));
        return raw;
    }

    OpList &operations() {
        return operations_;
    }
    const OpList &operations() const {
        return operations_;
    }

  private:
    OpList operations_;
};

class Global {
  public:
    Global(std::string name, TypePtr storage_type, bool is_const);

    const std::string &name() const {
        return name_;
    }
    const TypePtr &storage_type() const {
        return storage_type_;
    }
    bool is_const() const {
        return is_const_;
    }
    Value *address() {
        return &address_;
    }
    const Value *address() const {
        return &address_;
    }

    void set_initializer(ConstantPtr initializer) {
        typed_initializer_ = std::move(initializer);
    }
    const ConstantPtr &typed_initializer() const {
        return typed_initializer_;
    }

  private:
    std::string name_;
    TypePtr storage_type_;
    bool is_const_;
    Value address_;
    ConstantPtr typed_initializer_;
};

class Function {
  public:
    Function(std::string name, TypePtr return_type, std::vector<TypePtr> param_types,
             bool is_variadic = false, bool is_external = false);

    const std::string &name() const {
        return name_;
    }
    const TypePtr &return_type() const {
        return return_type_;
    }
    const std::vector<TypePtr> &param_types() const {
        return param_types_;
    }
    TypePtr function_type() const {
        return Type::get_func(param_types_, return_type_, is_variadic_);
    }
    bool is_variadic() const {
        return is_variadic_;
    }
    bool is_external() const {
        return is_external_;
    }

    Value *add_param(TypePtr type, std::string name);
    const std::vector<std::unique_ptr<Value>> &params() const {
        return params_;
    }
    std::vector<std::unique_ptr<Value>> &params() {
        return params_;
    }

    Region &body() {
        return body_;
    }
    const Region &body() const {
        return body_;
    }

  private:
    std::string name_;
    TypePtr return_type_;
    std::vector<TypePtr> param_types_;
    bool is_variadic_ = false;
    bool is_external_ = false;
    std::vector<std::unique_ptr<Value>> params_;
    Region body_;
};

class Module {
  public:
    Global *add_global(std::string name, TypePtr storage_type, bool is_const);
    Function *add_function(std::string name, TypePtr return_type, std::vector<TypePtr> param_types,
                           bool is_variadic = false, bool is_external = false);

    const std::vector<std::unique_ptr<Global>> &globals() const {
        return globals_;
    }
    std::vector<std::unique_ptr<Global>> &globals() {
        return globals_;
    }
    const std::vector<std::unique_ptr<Function>> &functions() const {
        return functions_;
    }
    std::vector<std::unique_ptr<Function>> &functions() {
        return functions_;
    }

  private:
    std::vector<std::unique_ptr<Global>> globals_;
    std::vector<std::unique_ptr<Function>> functions_;
};

class ConstI32Op final : public Operation {
  public:
    explicit ConstI32Op(int value, std::string result_name = "");
    int value() const {
        return value_;
    }

  private:
    int value_;
};

class ConstF32Op final : public Operation {
  public:
    explicit ConstF32Op(float value, std::string result_name = "");
    float value() const {
        return value_;
    }

  private:
    float value_;
};

class ConstBoolOp final : public Operation {
  public:
    explicit ConstBoolOp(bool value, std::string result_name = "");
    bool value() const {
        return value_;
    }

  private:
    bool value_;
};

class ZeroOp final : public Operation {
  public:
    explicit ZeroOp(TypePtr type, std::string result_name = "");
};

class VarOp final : public Operation {
  public:
    VarOp(TypePtr type, Value *initializer = nullptr, std::string result_name = "");
    bool has_initializer() const {
        return !operands().empty();
    }
    Value *initializer() const {
        return has_initializer() ? operands()[0] : nullptr;
    }
};

class AssignOp final : public Operation {
  public:
    AssignOp(Value *target, Value *value);
    Value *target() const {
        return operands()[0];
    }
    Value *value() const {
        return operands()[1];
    }
};

class ArrayVarOp final : public Operation {
  public:
    explicit ArrayVarOp(TypePtr array_type, std::string result_name = "");
};

class ArrayInitOp final : public Operation {
  public:
    ArrayInitOp(Value *array, TypePtr array_type, std::vector<ArrayInitEntry> entries,
                bool default_zero = true);
    Value *array() const {
        return operands()[0];
    }
    const TypePtr &array_type() const {
        return array_type_;
    }
    const std::vector<ArrayInitEntry> &entries() const {
        return entries_;
    }
    bool default_zero() const {
        return default_zero_;
    }

  private:
    TypePtr array_type_;
    std::vector<ArrayInitEntry> entries_;
    bool default_zero_;
};

class ArrayLoadOp final : public Operation {
  public:
    ArrayLoadOp(Value *array, std::vector<Value *> indices, TypePtr element_type,
                std::string result_name = "");
    Value *array() const {
        return operands()[0];
    }
    std::vector<Value *> indices() const;
};

class ArrayStoreOp final : public Operation {
  public:
    ArrayStoreOp(Value *value, Value *array, std::vector<Value *> indices);
    Value *value() const {
        return operands()[0];
    }
    Value *array() const {
        return operands()[1];
    }
    std::vector<Value *> indices() const;
};

class AllocaOp final : public Operation {
  public:
    explicit AllocaOp(TypePtr storage_type, std::string result_name = "");
    const TypePtr &storage_type() const {
        return storage_type_;
    }

  private:
    TypePtr storage_type_;
};

class LoadOp final : public Operation {
  public:
    explicit LoadOp(Value *address, TypePtr result_type, std::string result_name = "");
    Value *address() const {
        return operands()[0];
    }
};

class StoreOp final : public Operation {
  public:
    StoreOp(Value *value, Value *address);
    Value *value() const {
        return operands()[0];
    }
    Value *address() const {
        return operands()[1];
    }
};

class ElemAddrOp final : public Operation {
  public:
    ElemAddrOp(Value *base, std::vector<Value *> indices, TypePtr result_type,
               std::string result_name = "");
    Value *base() const {
        return operands()[0];
    }
    std::vector<Value *> indices() const;
};

class DecayOp final : public Operation {
  public:
    DecayOp(Value *array_address, TypePtr result_type, std::string result_name = "");
    Value *array_address() const {
        return operands()[0];
    }
};

class BinaryOpBase : public Operation {
  public:
    BinaryOpBase(std::string op_name, Value *lhs, Value *rhs, TypePtr result_type,
                 std::string result_name = "");
    Value *lhs() const {
        return operands()[0];
    }
    Value *rhs() const {
        return operands()[1];
    }
};

class AddIOp final : public BinaryOpBase {
  public:
    AddIOp(Value *lhs, Value *rhs, std::string result_name = "");
};
class SubIOp final : public BinaryOpBase {
  public:
    SubIOp(Value *lhs, Value *rhs, std::string result_name = "");
};
class MulIOp final : public BinaryOpBase {
  public:
    MulIOp(Value *lhs, Value *rhs, std::string result_name = "");
};
class DivSIOp final : public BinaryOpBase {
  public:
    DivSIOp(Value *lhs, Value *rhs, std::string result_name = "");
};
class RemSIOp final : public BinaryOpBase {
  public:
    RemSIOp(Value *lhs, Value *rhs, std::string result_name = "");
};
class AndIOp final : public BinaryOpBase {
  public:
    AndIOp(Value *lhs, Value *rhs, std::string result_name = "");
};
class OrIOp final : public BinaryOpBase {
  public:
    OrIOp(Value *lhs, Value *rhs, std::string result_name = "");
};
class XorIOp final : public BinaryOpBase {
  public:
    XorIOp(Value *lhs, Value *rhs, std::string result_name = "");
};

class AddFOp final : public BinaryOpBase {
  public:
    AddFOp(Value *lhs, Value *rhs, std::string result_name = "");
};
class SubFOp final : public BinaryOpBase {
  public:
    SubFOp(Value *lhs, Value *rhs, std::string result_name = "");
};
class MulFOp final : public BinaryOpBase {
  public:
    MulFOp(Value *lhs, Value *rhs, std::string result_name = "");
};
class DivFOp final : public BinaryOpBase {
  public:
    DivFOp(Value *lhs, Value *rhs, std::string result_name = "");
};

class ICmpOp final : public BinaryOpBase {
  public:
    enum class Predicate { Eq, Ne, Lt, Le, Gt, Ge };
    ICmpOp(Predicate pred, Value *lhs, Value *rhs, std::string result_name = "");
    Predicate predicate() const {
        return predicate_;
    }

  private:
    Predicate predicate_;
};

class FCmpOp final : public BinaryOpBase {
  public:
    enum class Predicate { Eq, Ne, Lt, Le, Gt, Ge };
    FCmpOp(Predicate pred, Value *lhs, Value *rhs, std::string result_name = "");
    Predicate predicate() const {
        return predicate_;
    }

  private:
    Predicate predicate_;
};

class ZExtI1ToI32Op final : public Operation {
  public:
    explicit ZExtI1ToI32Op(Value *value, std::string result_name = "");
};

class TruncI32ToI1Op final : public Operation {
  public:
    explicit TruncI32ToI1Op(Value *value, std::string result_name = "");
};

class SIToFPOp final : public Operation {
  public:
    explicit SIToFPOp(Value *value, std::string result_name = "");
};

class FPToSIOp final : public Operation {
  public:
    explicit FPToSIOp(Value *value, std::string result_name = "");
};

class ToBoolOp final : public Operation {
  public:
    explicit ToBoolOp(Value *value, std::string result_name = "");
};

class NotOp final : public Operation {
  public:
    explicit NotOp(Value *value, std::string result_name = "");
};

class BitNotOp final : public Operation {
  public:
    explicit BitNotOp(Value *value, std::string result_name = "");
    Value *value() const {
        return operands()[0];
    }
};

class MaskBinaryOp final : public BinaryOpBase {
  public:
    enum class Kind { And, Or, Xor };

    MaskBinaryOp(Kind kind, Value *lhs, Value *rhs, std::string result_name = "");
    Kind kind() const {
        return kind_;
    }

  private:
    Kind kind_;
};

class MaskNotOp final : public Operation {
  public:
    explicit MaskNotOp(Value *mask, std::string result_name = "");
    Value *mask() const {
        return operands()[0];
    }
};

// Fixed source-vector construction and lane operations.  These operations are
// deliberately distinct from array operations: vectors are SSA values and do
// not decay to pointers.
class VectorCreateOp final : public Operation {
  public:
    VectorCreateOp(TypePtr result_type, std::vector<Value *> lanes, std::string result_name = "");
    const std::vector<Value *> &lanes() const {
        return operands();
    }
};

class SplatOp final : public Operation {
  public:
    SplatOp(Value *scalar, TypePtr result_type, std::string result_name = "");
    Value *scalar() const {
        return operands()[0];
    }
};

class StepVectorOp final : public Operation {
  public:
    explicit StepVectorOp(TypePtr result_type, std::string result_name = "");
};

class ExtractLaneOp final : public Operation {
  public:
    ExtractLaneOp(Value *vector, Value *index, std::string result_name = "");
    Value *vector() const {
        return operands()[0];
    }
    Value *index() const {
        return operands()[1];
    }
};

class InsertLaneOp final : public Operation {
  public:
    InsertLaneOp(Value *vector, Value *index, Value *lane, std::string result_name = "");
    Value *vector() const {
        return operands()[0];
    }
    Value *index() const {
        return operands()[1];
    }
    Value *lane() const {
        return operands()[2];
    }
};

class ShuffleOp final : public Operation {
  public:
    static constexpr std::uint64_t UndefLane = ~std::uint64_t{0};
    ShuffleOp(Value *lhs, Value *rhs, std::vector<std::uint64_t> indices, TypePtr result_type,
              std::string result_name = "");
    Value *lhs() const {
        return operands()[0];
    }
    Value *rhs() const {
        return operands()[1];
    }
    const std::vector<std::uint64_t> &indices() const {
        return indices_;
    }

  private:
    std::vector<std::uint64_t> indices_;
};

class SelectOp final : public Operation {
  public:
    SelectOp(Value *mask, Value *true_value, Value *false_value, std::string result_name = "");
    Value *mask() const {
        return operands()[0];
    }
    Value *true_value() const {
        return operands()[1];
    }
    Value *false_value() const {
        return operands()[2];
    }
};

class VectorCastOp final : public Operation {
  public:
    VectorCastOp(Value *value, TypePtr result_type, std::string result_name = "");
    Value *value() const {
        return operands()[0];
    }
};

class MaskReduceOp final : public Operation {
  public:
    enum class Kind { Any, All, None };

    MaskReduceOp(Kind kind, Value *mask, std::string result_name = "");
    Kind kind() const {
        return kind_;
    }
    Value *mask() const {
        return operands()[0];
    }

  private:
    Kind kind_;
};

class VectorReduceOp final : public Operation {
  public:
    enum class Kind { Add, Mul, Min, Max, And, Or, Xor };

    VectorReduceOp(Kind kind, Value *vector, bool ordered, std::string result_name = "");
    Kind kind() const {
        return kind_;
    }
    Value *vector() const {
        return operands()[0];
    }
    bool ordered() const {
        return ordered_;
    }

  private:
    Kind kind_;
    bool ordered_;
};

class MaskedLoadOp final : public Operation {
  public:
    MaskedLoadOp(Value *address, Value *mask, Value *passthrough, std::uint64_t alignment,
                 std::string result_name = "");
    Value *address() const {
        return operands()[0];
    }
    Value *mask() const {
        return operands()[1];
    }
    Value *passthrough() const {
        return operands()[2];
    }
    std::uint64_t alignment() const {
        return alignment_;
    }

  private:
    std::uint64_t alignment_;
};

class MaskedStoreOp final : public Operation {
  public:
    MaskedStoreOp(Value *value, Value *address, Value *mask, std::uint64_t alignment);
    Value *value() const {
        return operands()[0];
    }
    Value *address() const {
        return operands()[1];
    }
    Value *mask() const {
        return operands()[2];
    }
    std::uint64_t alignment() const {
        return alignment_;
    }

  private:
    std::uint64_t alignment_;
};

class GatherOp final : public Operation {
  public:
    GatherOp(Value *base, Value *indices, Value *mask, Value *passthrough, std::uint64_t alignment,
             std::string result_name = "");
    Value *base() const {
        return operands()[0];
    }
    Value *indices() const {
        return operands()[1];
    }
    Value *mask() const {
        return operands()[2];
    }
    Value *passthrough() const {
        return operands()[3];
    }
    std::uint64_t alignment() const {
        return alignment_;
    }

  private:
    std::uint64_t alignment_;
};

class ScatterOp final : public Operation {
  public:
    ScatterOp(Value *value, Value *base, Value *indices, Value *mask, std::uint64_t alignment);
    Value *value() const {
        return operands()[0];
    }
    Value *base() const {
        return operands()[1];
    }
    Value *indices() const {
        return operands()[2];
    }
    Value *mask() const {
        return operands()[3];
    }
    std::uint64_t alignment() const {
        return alignment_;
    }

  private:
    std::uint64_t alignment_;
};

class CallOp final : public Operation {
  public:
    CallOp(std::string callee, std::vector<Value *> args, TypePtr result_type,
           std::string result_name = "");
    const std::string &callee() const {
        return callee_;
    }
    const std::vector<Value *> &args() const {
        return operands();
    }

  private:
    std::string callee_;
};

class IfOp final : public Operation {
  public:
    explicit IfOp(Value *condition);
    Value *condition() const {
        return operands()[0];
    }
    Region &then_region() {
        return then_region_;
    }
    const Region &then_region() const {
        return then_region_;
    }
    Region &else_region() {
        return else_region_;
    }
    const Region &else_region() const {
        return else_region_;
    }
    void set_has_else(bool has_else) {
        has_else_ = has_else;
    }
    bool has_else() const {
        return has_else_;
    }

  private:
    Region then_region_;
    Region else_region_;
    bool has_else_ = false;
};

class WhileOp final : public Operation {
  public:
    WhileOp();
    Region &cond_region() {
        return cond_region_;
    }
    const Region &cond_region() const {
        return cond_region_;
    }
    Region &body_region() {
        return body_region_;
    }
    const Region &body_region() const {
        return body_region_;
    }

  private:
    Region cond_region_;
    Region body_region_;
};

class ForOp final : public Operation {
  public:
    ForOp(Value *induction_var, Value *lower_bound, Value *upper_bound, Value *step);
    Value *induction_var() const {
        return operands()[0];
    }
    Value *lower_bound() const {
        return operands()[1];
    }
    Value *upper_bound() const {
        return operands()[2];
    }
    Value *step() const {
        return operands()[3];
    }
    Region &body_region() {
        return body_region_;
    }
    const Region &body_region() const {
        return body_region_;
    }
    bool is_parallel() const {
        return is_parallel_;
    }
    void set_parallel(bool is_parallel) {
        is_parallel_ = is_parallel;
    }

  private:
    Region body_region_;
    bool is_parallel_ = false;
};

class CondOp final : public Operation {
  public:
    explicit CondOp(Value *condition);
    Value *condition() const {
        return operands()[0];
    }
};

class BreakOp final : public Operation {
  public:
    BreakOp();
};

class ContinueOp final : public Operation {
  public:
    ContinueOp();
};

class ReturnOp final : public Operation {
  public:
    explicit ReturnOp(Value *value = nullptr);
    bool has_value() const {
        return !operands().empty();
    }
    Value *value() const {
        return has_value() ? operands()[0] : nullptr;
    }
};

TypePtr element_type_after_indices(TypePtr address_type, std::size_t index_count);
TypePtr array_element_type_after_indices(TypePtr array_type, std::size_t index_count);
TypePtr pointee_type(TypePtr address_type);
std::size_t array_rank(TypePtr type);

} // namespace yir
