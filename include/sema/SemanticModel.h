#pragma once

#include "sema/ConstantEvaluator.h"
#include "sema/SemanticType.h"
#include "front/Diagnostic.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

class CallExpr;
class Expr;
class FuncDef;
struct FuncParam;
class InitVal;
class VarDecl;

namespace sema {

enum class ConversionKind {
    None,
    Identity,
    LValueToRValue,
    ArrayToPointer,
    IntToFloat,
    FloatToInt,
    ScalarToBool,
    BoolToInt,
    ScalarSplat,
    VectorElementCast,
    MaskLaneToInt,
};

struct ConversionInfo {
    ConversionKind kind = ConversionKind::None;
    SemanticTypeRef source_type = nullptr;
    SemanticTypeRef target_type = nullptr;
};

// Builtin ids are deliberately opaque here. The registry owns their stable
// enumeration; the semantic model records the chosen entry and its concrete
// type/integer substitutions without depending on a particular registry file.
using BuiltinBindingId = std::uint32_t;

struct BuiltinBinding {
    BuiltinBindingId id = 0;
    SemanticTypeRef result_type = nullptr;
    std::vector<SemanticTypeRef> argument_types;
    std::vector<SemanticTypeRef> type_arguments;
    std::vector<std::uint64_t> integer_arguments;
};

// Canonical, structured compile-time value.  Aggregate children are kept as
// values rather than formatted text so shuffle masks and typed global
// initializers cannot be reinterpreted by lowering.
class SemanticConstant final {
  public:
    enum class Kind { Integer, Float, AggregateZero, Aggregate };

    static std::shared_ptr<const SemanticConstant> integer(SemanticTypeRef type,
                                                            std::int64_t value);
    static std::shared_ptr<const SemanticConstant> floating(SemanticTypeRef type, float value);
    static std::shared_ptr<const SemanticConstant> aggregate_zero(SemanticTypeRef type);
    static std::shared_ptr<const SemanticConstant>
    aggregate(SemanticTypeRef type,
              std::vector<std::shared_ptr<const SemanticConstant>> elements);

    Kind kind() const { return kind_; }
    SemanticTypeRef type() const { return type_; }
    std::int64_t integer_value() const { return integer_value_; }
    float float_value() const { return float_value_; }
    const std::vector<std::shared_ptr<const SemanticConstant>> &elements() const {
        return elements_;
    }

  private:
    SemanticConstant(Kind kind, SemanticTypeRef type, std::int64_t integer_value,
                     float float_value,
                     std::vector<std::shared_ptr<const SemanticConstant>> elements);

    Kind kind_;
    SemanticTypeRef type_;
    std::int64_t integer_value_ = 0;
    float float_value_ = 0.0F;
    std::vector<std::shared_ptr<const SemanticConstant>> elements_;
};

using SemanticConstantRef = std::shared_ptr<const SemanticConstant>;

class SemanticModel final {
  public:
    SemanticModel();
    explicit SemanticModel(std::shared_ptr<SemanticTypeContext> type_context);

    SemanticTypeContext &types() {
        return *type_context_;
    }

    const SemanticTypeContext &types() const {
        return *type_context_;
    }

    const std::shared_ptr<SemanticTypeContext> &type_context() const {
        return type_context_;
    }

    void set_expr_type(const Expr &expr, SemanticTypeRef type);
    SemanticTypeRef expr_type(const Expr &expr) const;

    void set_declaration_type(const VarDecl &declaration, SemanticTypeRef type);
    SemanticTypeRef declaration_type(const VarDecl &declaration) const;

    void set_parameter_type(const FuncParam &parameter, SemanticTypeRef type);
    SemanticTypeRef parameter_type(const FuncParam &parameter) const;

    void set_function_type(const FuncDef &function, SemanticTypeRef type);
    SemanticTypeRef function_type(const FuncDef &function) const;

    void set_conversion(const Expr &expr, ConversionInfo conversion);
    void add_conversion(const Expr &expr, ConversionInfo conversion);
    const ConversionInfo *conversion(const Expr &expr) const;
    const std::vector<ConversionInfo> *conversions(const Expr &expr) const;

    void set_checked_constant(const Expr &expr, CheckedInteger value);
    const CheckedInteger *checked_constant(const Expr &expr) const;

    void set_checked_extent(const Expr &expr, std::uint64_t extent);
    const std::uint64_t *checked_extent(const Expr &expr) const;

    void set_constant(const Expr &expr, SemanticConstantRef constant);
    const SemanticConstantRef *constant(const Expr &expr) const;

    void set_initializer_constant(const InitVal &initializer, SemanticConstantRef constant);
    const SemanticConstantRef *initializer_constant(const InitVal &initializer) const;

    void set_builtin_binding(const CallExpr &call, BuiltinBinding binding);
    const BuiltinBinding *builtin_binding(const CallExpr &call) const;

    front::DiagnosticEngine &diagnostics() {
        return diagnostics_;
    }

    const front::DiagnosticEngine &diagnostics() const {
        return diagnostics_;
    }

  private:
    std::shared_ptr<SemanticTypeContext> type_context_;
    std::unordered_map<const Expr *, SemanticTypeRef> expression_types_;
    std::unordered_map<const VarDecl *, SemanticTypeRef> declaration_types_;
    std::unordered_map<const FuncParam *, SemanticTypeRef> parameter_types_;
    std::unordered_map<const FuncDef *, SemanticTypeRef> function_types_;
    std::unordered_map<const Expr *, std::vector<ConversionInfo>> conversions_;
    std::unordered_map<const Expr *, CheckedInteger> checked_constants_;
    std::unordered_map<const Expr *, std::uint64_t> checked_extents_;
    std::unordered_map<const Expr *, SemanticConstantRef> constants_;
    std::unordered_map<const InitVal *, SemanticConstantRef> initializer_constants_;
    std::unordered_map<const CallExpr *, BuiltinBinding> builtin_bindings_;
    front::DiagnosticEngine diagnostics_;
};

} // namespace sema
