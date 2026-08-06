#include "sema/SemanticModel.h"

#include <stdexcept>
#include <utility>

namespace sema {
namespace {

void require_type(SemanticTypeRef type, const char *description) {
    if (type == nullptr) {
        throw std::invalid_argument(std::string(description) + " type cannot be null");
    }
}

} // namespace

SemanticConstant::SemanticConstant(
    Kind kind, SemanticTypeRef type, std::int64_t integer_value, float float_value,
    std::vector<std::shared_ptr<const SemanticConstant>> elements)
    : kind_(kind), type_(type), integer_value_(integer_value), float_value_(float_value),
      elements_(std::move(elements)) {
    require_type(type_, "semantic constant");
}

SemanticConstantRef SemanticConstant::integer(SemanticTypeRef type, std::int64_t value) {
    if (!type->is_integer()) {
        throw std::invalid_argument("integer semantic constant requires int type");
    }
    return SemanticConstantRef(new SemanticConstant(Kind::Integer, type, value, 0.0F, {}));
}

SemanticConstantRef SemanticConstant::floating(SemanticTypeRef type, float value) {
    if (!type->is_float()) {
        throw std::invalid_argument("floating semantic constant requires float type");
    }
    return SemanticConstantRef(new SemanticConstant(Kind::Float, type, 0, value, {}));
}

SemanticConstantRef SemanticConstant::aggregate_zero(SemanticTypeRef type) {
    return SemanticConstantRef(
        new SemanticConstant(Kind::AggregateZero, type, 0, 0.0F, {}));
}

SemanticConstantRef SemanticConstant::aggregate(
    SemanticTypeRef type, std::vector<SemanticConstantRef> elements) {
    return SemanticConstantRef(
        new SemanticConstant(Kind::Aggregate, type, 0, 0.0F, std::move(elements)));
}

SemanticModel::SemanticModel() : type_context_(std::make_shared<SemanticTypeContext>()) {
}

SemanticModel::SemanticModel(std::shared_ptr<SemanticTypeContext> type_context)
    : type_context_(std::move(type_context)) {
    if (type_context_ == nullptr) {
        throw std::invalid_argument("semantic model type context cannot be null");
    }
}

void SemanticModel::set_expr_type(const Expr &expr, SemanticTypeRef type) {
    require_type(type, "expression");
    expression_types_.insert_or_assign(&expr, type);
}

SemanticTypeRef SemanticModel::expr_type(const Expr &expr) const {
    auto found = expression_types_.find(&expr);
    return found == expression_types_.end() ? nullptr : found->second;
}

void SemanticModel::set_declaration_type(const VarDecl &declaration, SemanticTypeRef type) {
    require_type(type, "declaration");
    declaration_types_.insert_or_assign(&declaration, type);
}

SemanticTypeRef SemanticModel::declaration_type(const VarDecl &declaration) const {
    auto found = declaration_types_.find(&declaration);
    return found == declaration_types_.end() ? nullptr : found->second;
}

void SemanticModel::set_parameter_type(const FuncParam &parameter, SemanticTypeRef type) {
    require_type(type, "parameter");
    parameter_types_.insert_or_assign(&parameter, type);
}

SemanticTypeRef SemanticModel::parameter_type(const FuncParam &parameter) const {
    auto found = parameter_types_.find(&parameter);
    return found == parameter_types_.end() ? nullptr : found->second;
}

void SemanticModel::set_function_type(const FuncDef &function, SemanticTypeRef type) {
    require_type(type, "function");
    if (!type->is_function() && !type->is_error()) {
        throw std::invalid_argument("function semantic type must be a function or error type");
    }
    function_types_.insert_or_assign(&function, type);
}

SemanticTypeRef SemanticModel::function_type(const FuncDef &function) const {
    auto found = function_types_.find(&function);
    return found == function_types_.end() ? nullptr : found->second;
}

void SemanticModel::set_conversion(const Expr &expr, ConversionInfo conversion) {
    require_type(conversion.source_type, "conversion source");
    require_type(conversion.target_type, "conversion target");
    conversions_.insert_or_assign(&expr, std::vector<ConversionInfo>{std::move(conversion)});
}

void SemanticModel::add_conversion(const Expr &expr, ConversionInfo conversion) {
    require_type(conversion.source_type, "conversion source");
    require_type(conversion.target_type, "conversion target");
    conversions_[&expr].push_back(std::move(conversion));
}

const ConversionInfo *SemanticModel::conversion(const Expr &expr) const {
    auto found = conversions_.find(&expr);
    return found == conversions_.end() || found->second.empty() ? nullptr
                                                                : &found->second.back();
}

const std::vector<ConversionInfo> *SemanticModel::conversions(const Expr &expr) const {
    auto found = conversions_.find(&expr);
    return found == conversions_.end() ? nullptr : &found->second;
}

void SemanticModel::set_checked_constant(const Expr &expr, CheckedInteger value) {
    checked_constants_.insert_or_assign(&expr, value);
}

const CheckedInteger *SemanticModel::checked_constant(const Expr &expr) const {
    auto found = checked_constants_.find(&expr);
    return found == checked_constants_.end() ? nullptr : &found->second;
}

void SemanticModel::set_checked_extent(const Expr &expr, std::uint64_t extent) {
    if (extent == 0) {
        throw std::invalid_argument("checked extent must be positive");
    }
    checked_extents_.insert_or_assign(&expr, extent);
}

const std::uint64_t *SemanticModel::checked_extent(const Expr &expr) const {
    auto found = checked_extents_.find(&expr);
    return found == checked_extents_.end() ? nullptr : &found->second;
}

void SemanticModel::set_constant(const Expr &expr, SemanticConstantRef constant) {
    if (!constant) {
        throw std::invalid_argument("expression semantic constant cannot be null");
    }
    constants_.insert_or_assign(&expr, std::move(constant));
}

const SemanticConstantRef *SemanticModel::constant(const Expr &expr) const {
    auto found = constants_.find(&expr);
    return found == constants_.end() ? nullptr : &found->second;
}

void SemanticModel::set_initializer_constant(const InitVal &initializer,
                                             SemanticConstantRef constant) {
    if (!constant) {
        throw std::invalid_argument("initializer semantic constant cannot be null");
    }
    initializer_constants_.insert_or_assign(&initializer, std::move(constant));
}

const SemanticConstantRef *SemanticModel::initializer_constant(const InitVal &initializer) const {
    auto found = initializer_constants_.find(&initializer);
    return found == initializer_constants_.end() ? nullptr : &found->second;
}

void SemanticModel::set_builtin_binding(const CallExpr &call, BuiltinBinding binding) {
    if (binding.id == 0) {
        throw std::invalid_argument("builtin binding id zero is reserved for no binding");
    }
    require_type(binding.result_type, "builtin result");
    for (auto *argument_type : binding.argument_types) {
        require_type(argument_type, "builtin argument");
    }
    for (auto *type_argument : binding.type_arguments) {
        require_type(type_argument, "builtin type argument");
    }
    builtin_bindings_.insert_or_assign(&call, std::move(binding));
}

const BuiltinBinding *SemanticModel::builtin_binding(const CallExpr &call) const {
    auto found = builtin_bindings_.find(&call);
    return found == builtin_bindings_.end() ? nullptr : &found->second;
}

} // namespace sema
