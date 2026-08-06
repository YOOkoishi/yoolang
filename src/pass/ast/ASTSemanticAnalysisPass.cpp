#include "pass/ast/ASTSemanticAnalysisPass.h"

#include "builtin/BuiltinRegistry.h"
#include "front/Diagnostic.h"
#include "sema/ConstantEvaluator.h"
#include "sema/SemanticModel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pass {
namespace {

using sema::SemanticTypeRef;

struct SemanticSymbol final {
    enum class Kind { Variable, Function, Builtin };

    Kind kind = Kind::Variable;
    SemanticTypeRef type = nullptr;
    bool is_const = false;
    std::optional<sema::CheckedInteger> integer_constant;
    sema::SemanticConstantRef constant;
    const FuncDef *function = nullptr;
    const builtin::BuiltinDescriptor *builtin = nullptr;
    bool has_definition = false;
};

class SemanticScopeStack final {
  public:
    void enter() { scopes_.emplace_back(); }
    void leave() {
        if (!scopes_.empty()) scopes_.pop_back();
    }

    SemanticSymbol *define(std::string name, SemanticSymbol symbol) {
        if (scopes_.empty()) enter();
        auto [it, inserted] = scopes_.back().emplace(std::move(name), std::move(symbol));
        return inserted ? &it->second : nullptr;
    }

    SemanticSymbol *lookup(std::string_view name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(std::string(name));
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }

    SemanticSymbol *lookup_current(std::string_view name) {
        if (scopes_.empty()) return nullptr;
        auto found = scopes_.back().find(std::string(name));
        return found == scopes_.back().end() ? nullptr : &found->second;
    }

  private:
    std::vector<std::unordered_map<std::string, SemanticSymbol>> scopes_;
};

std::string binary_operator_name(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Sub: return "-";
    case BinaryOp::Mul: return "*";
    case BinaryOp::Div: return "/";
    case BinaryOp::Mod: return "%";
    case BinaryOp::Lt: return "<";
    case BinaryOp::Le: return "<=";
    case BinaryOp::Gt: return ">";
    case BinaryOp::Ge: return ">=";
    case BinaryOp::Eq: return "==";
    case BinaryOp::Ne: return "!=";
    case BinaryOp::BitAnd: return "&";
    case BinaryOp::BitXor: return "^";
    case BinaryOp::BitOr: return "|";
    case BinaryOp::And: return "&&";
    case BinaryOp::Or: return "||";
    }
    return "?";
}

bool is_comparison(BinaryOp op) {
    return op == BinaryOp::Lt || op == BinaryOp::Le || op == BinaryOp::Gt ||
           op == BinaryOp::Ge || op == BinaryOp::Eq || op == BinaryOp::Ne;
}

bool is_bitwise(BinaryOp op) {
    return op == BinaryOp::BitAnd || op == BinaryOp::BitXor || op == BinaryOp::BitOr;
}

class Analyzer final : public ASTVisitor {
  public:
    explicit Analyzer(std::shared_ptr<sema::SemanticModel> model) : model_(std::move(model)) {}

    bool analyze(CompUnit &unit) {
        unit.accept(*this);
        return !model_->diagnostics().has_error();
    }

    void visit(IntLiteral &node) override {
        set_expr_type(node, types().int_type());
        model_->set_checked_constant(node, sema::CheckedInteger::i32(node.value));
        model_->set_constant(node, sema::SemanticConstant::integer(types().int_type(), node.value));
    }

    void visit(FloatLiteral &node) override {
        set_expr_type(node, types().float_type());
        model_->set_constant(node,
                             sema::SemanticConstant::floating(types().float_type(), node.value));
    }

    void visit(LValExpr &node) override {
        auto *symbol = symbols_.lookup(node.name);
        if (symbol == nullptr || symbol->kind != SemanticSymbol::Kind::Variable) {
            diagnose(front::DiagnosticCode::SemaUnknownSymbol, node.source_range,
                     "unknown variable '" + node.name + "'");
            set_expr_type(node, types().error_type());
            for (auto &index : node.indices) expr_type(*index);
            return;
        }

        SemanticTypeRef current = symbol->type;
        for (auto &index : node.indices) {
            auto *index_type = as_value(*index);
            if (!index_type->is_error() && !index_type->is_integer()) {
                diagnose(front::DiagnosticCode::SemaInvalidSubscript, index->source_range,
                         "subscript must have type int, not " + index_type->str());
            }
            if (current->is_array()) current = current->element_type();
            else if (current->is_pointer()) current = current->pointee_type();
            else if (current->is_fixed_vector()) {
                check_constant_lane_index(*index, current->lane_count(), node.name);
                current = current->element_type();
            }
            else if (current->is_mask()) {
                auto *mask = current;
                check_constant_lane_index(*index, mask->lane_count(), node.name);
                current = types().int_type();
                add_conversion(node, sema::ConversionKind::MaskLaneToInt, mask, current);
            } else if (!current->is_error()) {
                diagnose(front::DiagnosticCode::SemaInvalidSubscript, node.source_range,
                         "cannot subscript value of type " + current->str());
                current = types().error_type();
            }
        }
        set_expr_type(node, current);
        if (node.indices.empty() && symbol->integer_constant)
            model_->set_checked_constant(node, *symbol->integer_constant);
    }

    void visit(BinaryExpr &node) override {
        auto *lhs = as_value(*node.lhs);
        auto *rhs = as_value(*node.rhs);
        if (lhs->is_error() || rhs->is_error()) {
            set_expr_type(node, types().error_type());
            return;
        }

        if (node.op == BinaryOp::And || node.op == BinaryOp::Or) {
            if (!is_condition_type(lhs) || !is_condition_type(rhs)) {
                invalid_operands(node, lhs, rhs,
                                 "logical operators require scalar int or float operands");
                return;
            }
            add_conversion(*node.lhs, sema::ConversionKind::ScalarToBool, lhs, types().int_type());
            add_conversion(*node.rhs, sema::ConversionKind::ScalarToBool, rhs, types().int_type());
            set_expr_type(node, types().int_type());
            record_checked_constant(node);
            return;
        }

        if (is_bitwise(node.op)) {
            if (lhs->is_mask() || rhs->is_mask()) {
                if (lhs->is_mask() && rhs == lhs) set_expr_type(node, lhs);
                else invalid_operands(node, lhs, rhs,
                                      "mask bitwise operands must have the same lane count");
                return;
            }
            SemanticTypeRef vector_type = nullptr;
            if (lhs->is_fixed_vector() || rhs->is_fixed_vector()) {
                if (!coerce_vector_operands(node, lhs, rhs, true, vector_type)) return;
                set_expr_type(node, vector_type);
                return;
            }
            if (!lhs->is_integer() || !rhs->is_integer()) {
                invalid_operands(node, lhs, rhs,
                                 "bitwise operators require int, integer-vector, or mask operands");
                return;
            }
            set_expr_type(node, types().int_type());
            record_checked_constant(node);
            return;
        }

        if (is_comparison(node.op)) {
            SemanticTypeRef vector_type = nullptr;
            if (lhs->is_fixed_vector() || rhs->is_fixed_vector()) {
                if (!coerce_vector_operands(node, lhs, rhs, false, vector_type)) return;
                set_expr_type(node, types().mask_type(vector_type->lane_count()));
                return;
            }
            if (!lhs->is_numeric_scalar() || !rhs->is_numeric_scalar()) {
                invalid_operands(node, lhs, rhs,
                                 "comparison requires scalar numeric operands or compatible vectors");
                return;
            }
            promote_scalar_numeric(*node.lhs, lhs, *node.rhs, rhs);
            set_expr_type(node, types().int_type());
            record_checked_constant(node);
            return;
        }

        SemanticTypeRef vector_type = nullptr;
        if (lhs->is_fixed_vector() || rhs->is_fixed_vector()) {
            if (!coerce_vector_operands(node, lhs, rhs, node.op == BinaryOp::Mod, vector_type))
                return;
            set_expr_type(node, vector_type);
            return;
        }
        if (!lhs->is_numeric_scalar() || !rhs->is_numeric_scalar()) {
            invalid_operands(node, lhs, rhs,
                             "arithmetic operators require scalar numeric operands or compatible vectors");
            return;
        }
        if (node.op == BinaryOp::Mod && (!lhs->is_integer() || !rhs->is_integer())) {
            invalid_operands(node, lhs, rhs, "operator % requires integer operands");
            return;
        }
        auto *result = promote_scalar_numeric(*node.lhs, lhs, *node.rhs, rhs);
        set_expr_type(node, result);
        if (result->is_integer()) record_checked_constant(node);
    }

    void visit(UnaryExpr &node) override {
        auto *operand = as_value(*node.operand);
        if (operand->is_error()) {
            set_expr_type(node, operand);
            return;
        }
        switch (node.op) {
        case UnaryOp::Pos:
        case UnaryOp::Neg:
            if (!operand->is_numeric_scalar() && !operand->is_fixed_vector()) {
                diagnose(front::DiagnosticCode::SemaInvalidOperand, node.source_range,
                         "unary arithmetic operator cannot be applied to " + operand->str());
                set_expr_type(node, types().error_type());
                return;
            }
            set_expr_type(node, operand);
            if (operand->is_integer()) record_checked_constant(node);
            return;
        case UnaryOp::Not:
            if (!is_condition_type(operand)) {
                diagnose(front::DiagnosticCode::SemaInvalidOperand, node.source_range,
                         "operator ! requires a scalar int or float operand, not " + operand->str());
                set_expr_type(node, types().error_type());
                return;
            }
            add_conversion(*node.operand, sema::ConversionKind::ScalarToBool, operand,
                           types().int_type());
            set_expr_type(node, types().int_type());
            record_checked_constant(node);
            return;
        case UnaryOp::BitNot:
            if (operand->is_mask() || operand->is_integer() ||
                (operand->is_fixed_vector() && operand->element_type()->is_integer())) {
                set_expr_type(node, operand);
                if (operand->is_integer()) record_checked_constant(node);
                return;
            }
            diagnose(front::DiagnosticCode::SemaInvalidOperand, node.source_range,
                     "operator ~ requires int, integer-vector, or mask operand, not " +
                         operand->str());
            set_expr_type(node, types().error_type());
            return;
        }
    }

    void visit(CallExpr &node) override {
        auto *symbol = symbols_.lookup(node.func_name);
        if (symbol == nullptr || (symbol->kind != SemanticSymbol::Kind::Function &&
                                  symbol->kind != SemanticSymbol::Kind::Builtin)) {
            diagnose(front::DiagnosticCode::SemaUnknownSymbol, node.source_range,
                     "unknown function '" + node.func_name + "'");
            for (auto &argument : node.args) as_value(*argument);
            set_expr_type(node, types().error_type());
            return;
        }
        if (symbol->kind == SemanticSymbol::Kind::Builtin) {
            resolve_builtin_call(node, *symbol->builtin);
            return;
        }

        auto *function_type = symbol->type;
        const auto &parameters = function_type->parameter_types();
        bool valid = true;
        if ((!function_type->is_variadic() && node.args.size() != parameters.size()) ||
            (function_type->is_variadic() && node.args.size() < parameters.size())) {
            diagnose(front::DiagnosticCode::SemaArgumentMismatch, node.source_range,
                     "function '" + node.func_name + "' expects " +
                         std::to_string(parameters.size()) +
                         (function_type->is_variadic() ? " or more" : "") + " arguments, got " +
                         std::to_string(node.args.size()));
            valid = false;
        }
        for (std::size_t i = 0; i < node.args.size(); ++i) {
            auto *argument_type = expr_type(*node.args[i]);
            if (i < parameters.size()) {
                valid = convert_context(*node.args[i], argument_type, parameters[i],
                                        "argument " + std::to_string(i + 1) + " of '" +
                                            node.func_name + "'") && valid;
            } else {
                argument_type = as_value(*node.args[i]);
                if (argument_type->is_void()) {
                    diagnose(front::DiagnosticCode::SemaInvalidConversion,
                             node.args[i]->source_range, "variadic argument cannot have type void");
                    valid = false;
                }
                if (contains_vector_or_mask_aggregate(argument_type)) {
                    diagnose(front::DiagnosticCode::SemaVariadicAggregate,
                             node.args[i]->source_range,
                             "variadic argument " + std::to_string(i + 1) + " of '" +
                                 node.func_name +
                                 "' cannot contain a fixed vector or mask value");
                    valid = false;
                }
            }
        }
        set_expr_type(node, valid ? function_type->return_type() : types().error_type());
    }

    void visit(TypedVectorLiteralExpr &node) override {
        auto *target = resolve_type_syntax(node.type_syntax, false, "typed literal");
        bool valid = target->is_fixed_vector() || target->is_mask();
        if (!valid && !target->is_error()) {
            diagnose(front::DiagnosticCode::SemaInvalidVectorLiteral, node.source_range,
                     "typed vector literal requires vector or mask type, not " + target->str());
        }

        if (target->is_error()) {
            for (auto &lane : node.lanes) expr_type(*lane);
            set_expr_type(node, target);
            return;
        }
        if (!node.lanes.empty() && node.lanes.size() != target->lane_count()) {
            diagnose(front::DiagnosticCode::SemaInvalidVectorLiteral, node.source_range,
                     "typed literal requires exactly " + std::to_string(target->lane_count()) +
                         " lanes, got " + std::to_string(node.lanes.size()));
            valid = false;
        }

        std::vector<sema::SemanticConstantRef> constants;
        constants.reserve(node.lanes.size());
        for (std::size_t i = 0; i < node.lanes.size(); ++i) {
            auto &lane = *node.lanes[i];
            auto *source = expr_type(lane);
            if (target->is_mask()) {
                if (!source->is_error() && !source->is_integer()) {
                    diagnose(front::DiagnosticCode::SemaInvalidVectorLiteral, lane.source_range,
                             "mask lane " + std::to_string(i) +
                                 " must be an integer constant 0 or 1");
                    valid = false;
                    continue;
                }
                auto value = evaluate_integer(lane);
                if (!value.ok() || (value.value->value() != 0 && value.value->value() != 1)) {
                    diagnose(front::DiagnosticCode::SemaInvalidVectorLiteral, lane.source_range,
                             "mask lane " + std::to_string(i) +
                                 " must be the compile-time integer 0 or 1");
                    valid = false;
                    continue;
                }
                model_->set_checked_constant(lane, *value.value);
                auto constant =
                    sema::SemanticConstant::integer(types().int_type(), value.value->value());
                model_->set_constant(lane, constant);
                constants.push_back(std::move(constant));
                continue;
            }

            if (!convert_context(lane, source, target->element_type(),
                                 "lane " + std::to_string(i) + " of typed literal")) {
                valid = false;
                continue;
            }
            if (auto constant = constant_as(lane, target->element_type())) {
                constants.push_back(std::move(constant));
            }
        }

        if (valid) {
            if (node.lanes.empty()) {
                model_->set_constant(node, sema::SemanticConstant::aggregate_zero(target));
            } else if (constants.size() == node.lanes.size()) {
                model_->set_constant(node,
                                     sema::SemanticConstant::aggregate(target, std::move(constants)));
            }
        }
        set_expr_type(node, valid ? target : types().error_type());
    }

    void visit(VectorCastExpr &node) override {
        auto *target = resolve_type_syntax(node.target_type_syntax, false, "vector constructor");
        auto *source = as_value(*node.operand);
        bool valid = target->is_fixed_vector();
        if (!valid && !target->is_error()) {
            diagnose(front::DiagnosticCode::SemaInvalidType, node.source_range,
                     "vector constructor target must be a fixed numeric vector");
        }
        if (valid && source->is_numeric_scalar()) {
            if (source != target->element_type()) {
                add_conversion(*node.operand,
                               source->is_integer() ? sema::ConversionKind::IntToFloat
                                                    : sema::ConversionKind::FloatToInt,
                               source, target->element_type());
            }
            add_conversion(*node.operand, sema::ConversionKind::ScalarSplat,
                           target->element_type(), target);
        } else if (valid && source->is_fixed_vector() &&
                   source->lane_count() == target->lane_count()) {
            if (source == target) {
                add_conversion(*node.operand, sema::ConversionKind::Identity, source, target);
            } else {
                add_conversion(*node.operand, sema::ConversionKind::VectorElementCast, source,
                               target);
            }
        } else if (valid && !source->is_error()) {
            diagnose(front::DiagnosticCode::SemaInvalidConversion, node.source_range,
                     "vector constructor cannot convert " + source->str() + " to " +
                         target->str() +
                         "; expected a numeric scalar or same-width numeric vector");
            valid = false;
        }

        if (valid) {
            if (auto constant = constant_as(*node.operand, target)) {
                model_->set_constant(node, std::move(constant));
            }
        }
        set_expr_type(node, valid ? target : types().error_type());
    }

    void visit(ConstExpr &node) override { expr_type(const_cast<Expr &>(node.expression())); }

    void visit(InitVal &node) override {
        if (node.expr) expr_type(*node.expr);
        for (auto &element : node.elems) element->accept(*this);
    }

    void visit(ExprStmt &node) override {
        if (node.expr) as_value(*node.expr);
    }

    void visit(AssignStmt &node) override {
        auto *target_symbol = symbols_.lookup(node.target->name);
        auto *target_type = expr_type(*node.target);
        if (target_symbol && target_symbol->kind == SemanticSymbol::Kind::Variable &&
            target_symbol->is_const) {
            diagnose(front::DiagnosticCode::SemaInvalidConversion, node.target->source_range,
                     "cannot assign to const variable '" + node.target->name + "'");
        }
        if (target_type->is_array()) {
            diagnose(front::DiagnosticCode::SemaInvalidConversion, node.target->source_range,
                     "arrays are not assignable");
        }
        auto *value_type = expr_type(*node.value);
        convert_context(*node.value, value_type, target_type, "assignment");
    }

    void visit(BlockStmt &node) override {
        symbols_.enter();
        for (auto &statement : node.stmts) statement->accept(*this);
        symbols_.leave();
    }

    void visit(ReturnStmt &node) override {
        if (current_return_type_ == nullptr) {
            if (node.expr) as_value(*node.expr);
            return;
        }
        if (current_return_type_->is_void()) {
            if (node.expr) {
                as_value(*node.expr);
                diagnose(front::DiagnosticCode::SemaInvalidConversion, node.source_range,
                         "void function cannot return a value");
            }
            return;
        }
        if (!node.expr) {
            diagnose(front::DiagnosticCode::SemaInvalidConversion, node.source_range,
                     "non-void function must return a value of type " + current_return_type_->str());
            return;
        }
        auto *value_type = expr_type(*node.expr);
        convert_context(*node.expr, value_type, current_return_type_, "return value");
    }

    void visit(IfStmt &node) override {
        check_condition(*node.cond, "if");
        node.then_stmt->accept(*this);
        if (node.else_stmt) node.else_stmt->accept(*this);
    }

    void visit(WhileStmt &node) override {
        check_condition(*node.cond, "while");
        ++loop_depth_;
        node.body->accept(*this);
        --loop_depth_;
    }

    void visit(BreakStmt &node) override {
        if (loop_depth_ == 0)
            diagnose(front::DiagnosticCode::SemaControlFlow, node.source_range,
                     "break statement is outside a loop");
    }

    void visit(ContinueStmt &node) override {
        if (loop_depth_ == 0)
            diagnose(front::DiagnosticCode::SemaControlFlow, node.source_range,
                     "continue statement is outside a loop");
    }

    void visit(VarDecl &node) override {
        auto *base = resolve_type_syntax(node.type_syntax, false, "variable");
        auto *declaration_type =
            resolve_declarator_type(base, node.dimensions, false, node.source_range);
        model_->set_declaration_type(node, declaration_type);

        SemanticSymbol symbol;
        symbol.kind = SemanticSymbol::Kind::Variable;
        symbol.type = declaration_type;
        symbol.is_const = node.is_const;
        auto *defined = symbols_.define(node.name, std::move(symbol));
        if (!defined)
            diagnose(front::DiagnosticCode::SemaRedefinition, node.source_range,
                     "redefinition of symbol '" + node.name + "'");

        if (node.is_const && !node.init)
            diagnose(front::DiagnosticCode::SemaInvalidInitializer, node.source_range,
                     "const variable '" + node.name + "' requires an initializer");
        bool initializer_valid = true;
        if (node.init)
            initializer_valid = validate_initializer(*node.init, declaration_type,
                                                     "initializer for '" + node.name + "'");

        sema::SemanticConstantRef initializer_constant;
        if (node.init && initializer_valid) {
            initializer_constant = build_initializer_constant(*node.init, declaration_type);
            if (initializer_constant) {
                model_->set_initializer_constant(*node.init, initializer_constant);
                if (defined && node.is_const && node.dimensions.empty())
                    defined->constant = initializer_constant;
            } else if (in_global_declarations_ || node.is_const) {
                diagnose(front::DiagnosticCode::SemaInvalidInitializer, node.init->source_range,
                         "initializer for '" + node.name +
                             "' must be a compile-time value");
            }
        }

        if (defined && node.is_const && declaration_type->is_integer() && node.init &&
            node.init->expr) {
            auto result = evaluate_integer(*node.init->expr);
            if (result.ok()) {
                defined->integer_constant = *result.value;
                model_->set_checked_constant(*node.init->expr, *result.value);
                if (!defined->constant)
                    defined->constant = sema::SemanticConstant::integer(
                        types().int_type(), result.value->value());
            } else if (const auto *literal =
                           dynamic_cast<const FloatLiteral *>(node.init->expr.get());
                       literal != nullptr && std::isfinite(literal->value) &&
                       static_cast<double>(literal->value) >=
                           static_cast<double>(std::numeric_limits<std::int32_t>::min()) &&
                       static_cast<double>(literal->value) <=
                           static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
                // SysY permits scalar float-to-int initialization. Preserve the
                // checked integer value after the contextual truncation so a
                // later integer constant expression can name this declaration.
                auto converted =
                    sema::CheckedInteger::i32(static_cast<std::int32_t>(literal->value));
                defined->integer_constant = converted;
                model_->set_checked_constant(*node.init->expr, converted);
                if (!defined->constant)
                    defined->constant = sema::SemanticConstant::integer(
                        types().int_type(), converted.value());
            } else {
                diagnose_constant_failure(*node.init->expr, result,
                                          "const int initializer is not a checked constant: ");
            }
        }
    }

    void visit(DeclStmt &node) override {
        for (auto &declaration : node.decls) {
            declaration->is_const = node.is_const;
            declaration->type_syntax = node.type_syntax;
            declaration->base_type = node.base_type;
            declaration->accept(*this);
        }
    }

    void visit(FuncDef &node) override {
        auto *function_type = model_->function_type(node);
        if (!function_type) function_type = types().error_type();
        auto *previous_return_type = current_return_type_;
        current_return_type_ = function_type->is_function() ? function_type->return_type()
                                                            : types().error_type();

        symbols_.enter();
        for (auto &parameter : node.params) {
            auto *parameter_type = model_->parameter_type(parameter);
            if (!parameter_type) parameter_type = types().error_type();
            SemanticSymbol symbol;
            symbol.kind = SemanticSymbol::Kind::Variable;
            symbol.type = parameter_type;
            if (!parameter.name.empty() &&
                !symbols_.define(parameter.name, std::move(symbol)))
                diagnose(front::DiagnosticCode::SemaRedefinition, parameter.source_range,
                         "redefinition of parameter '" + parameter.name + "'");
        }
        if (node.body) node.body->accept(*this);
        symbols_.leave();
        current_return_type_ = previous_return_type;
    }

    void visit(CompUnit &node) override {
        symbols_.enter();
        install_builtins();
        in_global_declarations_ = true;
        for (auto &declaration : node.global_decls) declaration->accept(*this);
        in_global_declarations_ = false;
        for (auto &function : node.functions) declare_function(*function);
        for (auto &function : node.functions) function->accept(*this);
        symbols_.leave();
    }

  private:
    sema::SemanticTypeContext &types() { return model_->types(); }

    void diagnose(front::DiagnosticCode code, front::SourceRange range, std::string message) {
        model_->diagnostics().error(code, range, std::move(message));
    }

    void set_expr_type(Expr &expression, SemanticTypeRef type) {
        model_->set_expr_type(expression, type);
    }

    bool contains_vector_or_mask_aggregate(SemanticTypeRef type) const {
        if (!type) return false;
        if (type->is_fixed_vector() || type->is_mask()) return true;
        if (type->is_array()) return contains_vector_or_mask_aggregate(type->element_type());
        if (type->is_pointer()) return contains_vector_or_mask_aggregate(type->pointee_type());
        return false;
    }

    sema::SemanticConstantRef convert_constant(sema::SemanticConstantRef constant,
                                               SemanticTypeRef target) {
        if (!constant || !target || target->is_error()) return nullptr;
        if (constant->type() == target) return constant;
        if (target->is_integer()) {
            if (constant->kind() == sema::SemanticConstant::Kind::Integer)
                return sema::SemanticConstant::integer(target, constant->integer_value());
            if (constant->kind() == sema::SemanticConstant::Kind::Float) {
                const double value = constant->float_value();
                if (!std::isfinite(value) ||
                    value < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
                    value > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
                    return nullptr;
                return sema::SemanticConstant::integer(target,
                                                       static_cast<std::int32_t>(value));
            }
            return nullptr;
        }
        if (target->is_float()) {
            if (constant->kind() == sema::SemanticConstant::Kind::Float)
                return sema::SemanticConstant::floating(target, constant->float_value());
            if (constant->kind() == sema::SemanticConstant::Kind::Integer)
                return sema::SemanticConstant::floating(
                    target, static_cast<float>(constant->integer_value()));
            return nullptr;
        }
        if (target->is_fixed_vector()) {
            if (constant->kind() == sema::SemanticConstant::Kind::AggregateZero)
                return sema::SemanticConstant::aggregate_zero(target);
            std::vector<sema::SemanticConstantRef> lanes;
            lanes.reserve(static_cast<std::size_t>(target->lane_count()));
            if (constant->type()->is_numeric_scalar()) {
                auto lane = convert_constant(constant, target->element_type());
                if (!lane) return nullptr;
                lanes.assign(static_cast<std::size_t>(target->lane_count()), lane);
            } else if (constant->type()->is_fixed_vector() &&
                       constant->type()->lane_count() == target->lane_count() &&
                       constant->kind() == sema::SemanticConstant::Kind::Aggregate) {
                for (const auto &source_lane : constant->elements()) {
                    auto lane = convert_constant(source_lane, target->element_type());
                    if (!lane) return nullptr;
                    lanes.push_back(std::move(lane));
                }
            } else {
                return nullptr;
            }
            return sema::SemanticConstant::aggregate(target, std::move(lanes));
        }
        return nullptr;
    }

    sema::SemanticConstantRef constant_as(Expr &expression, SemanticTypeRef target) {
        if (const auto *constant = model_->constant(expression)) {
            return convert_constant(*constant, target);
        }
        if (auto *lvalue = dynamic_cast<LValExpr *>(&expression);
            lvalue != nullptr && lvalue->indices.empty()) {
            if (auto *symbol = symbols_.lookup(lvalue->name); symbol && symbol->constant) {
                auto constant = convert_constant(symbol->constant, target);
                if (constant) model_->set_constant(expression, constant);
                return constant;
            }
        }
        if (target->is_integer()) {
            auto integer = evaluate_integer(expression);
            if (integer.ok()) {
                model_->set_checked_constant(expression, *integer.value);
                auto constant =
                    sema::SemanticConstant::integer(types().int_type(), integer.value->value());
                model_->set_constant(expression, constant);
                return convert_constant(std::move(constant), target);
            }
        }
        if (auto *unary = dynamic_cast<UnaryExpr *>(&expression)) {
            auto operand = constant_as(*unary->operand, target);
            if (!operand) return nullptr;
            if (target->is_float() && operand->kind() == sema::SemanticConstant::Kind::Float) {
                float value = operand->float_value();
                if (unary->op == UnaryOp::Neg) value = -value;
                else if (unary->op != UnaryOp::Pos) return nullptr;
                auto result = sema::SemanticConstant::floating(target, value);
                model_->set_constant(expression, result);
                return result;
            }
        }
        if (auto *binary = dynamic_cast<BinaryExpr *>(&expression);
            binary != nullptr && target->is_float()) {
            auto lhs = constant_as(*binary->lhs, target);
            auto rhs = constant_as(*binary->rhs, target);
            if (!lhs || !rhs || lhs->kind() != sema::SemanticConstant::Kind::Float ||
                rhs->kind() != sema::SemanticConstant::Kind::Float)
                return nullptr;
            const float left = lhs->float_value();
            const float right = rhs->float_value();
            float value = 0.0F;
            switch (binary->op) {
            case BinaryOp::Add: value = left + right; break;
            case BinaryOp::Sub: value = left - right; break;
            case BinaryOp::Mul: value = left * right; break;
            case BinaryOp::Div:
                if (right == 0.0F) return nullptr;
                value = left / right;
                break;
            default: return nullptr;
            }
            if (!std::isfinite(value)) return nullptr;
            auto result = sema::SemanticConstant::floating(target, value);
            model_->set_constant(expression, result);
            return result;
        }
        return nullptr;
    }

    sema::SemanticConstantRef zero_constant(SemanticTypeRef type) {
        if (type->is_integer()) return sema::SemanticConstant::integer(type, 0);
        if (type->is_float()) return sema::SemanticConstant::floating(type, 0.0F);
        if (type->is_array() || type->is_fixed_vector() || type->is_mask())
            return sema::SemanticConstant::aggregate_zero(type);
        return nullptr;
    }

    void collect_array_shape(SemanticTypeRef type, std::vector<std::uint64_t> &dimensions,
                             SemanticTypeRef &leaf) {
        while (type->is_array()) {
            dimensions.push_back(type->array_bound());
            type = type->element_type();
        }
        leaf = type;
    }

    std::uint64_t suffix_element_count(const std::vector<std::uint64_t> &dimensions,
                                       std::size_t first) const {
        std::uint64_t count = 1;
        for (std::size_t i = first; i < dimensions.size(); ++i) count *= dimensions[i];
        return count;
    }

    std::size_t braced_subobject_level(const std::vector<std::uint64_t> &dimensions,
                                       std::size_t level, std::uint64_t cursor,
                                       std::uint64_t limit, std::uint64_t &sub_size) const {
        for (std::size_t candidate = level + 1; candidate < dimensions.size(); ++candidate) {
            const auto size = suffix_element_count(dimensions, candidate);
            if (size != 0 && cursor % size == 0 && cursor + size <= limit) {
                sub_size = size;
                return candidate;
            }
        }
        sub_size = 1;
        return dimensions.size();
    }

    bool flatten_scalar_initializer(InitVal &initializer, SemanticTypeRef leaf,
                                    const std::vector<std::uint64_t> &dimensions,
                                    std::size_t level, std::uint64_t &cursor,
                                    std::uint64_t limit,
                                    std::vector<sema::SemanticConstantRef> &values) {
        if (cursor >= limit) return true;
        if (initializer.expr) {
            auto constant = constant_as(*initializer.expr, leaf);
            if (!constant) return false;
            values[static_cast<std::size_t>(cursor++)] = std::move(constant);
            return true;
        }
        for (auto &element : initializer.elems) {
            if (cursor >= limit) break;
            if (element->expr) {
                auto constant = constant_as(*element->expr, leaf);
                if (!constant) return false;
                values[static_cast<std::size_t>(cursor++)] = std::move(constant);
                continue;
            }
            std::uint64_t sub_size = 1;
            const auto sub_level = braced_subobject_level(dimensions, level, cursor, limit,
                                                          sub_size);
            const auto sub_end = cursor + sub_size;
            if (!flatten_scalar_initializer(*element, leaf, dimensions, sub_level, cursor,
                                            sub_end, values))
                return false;
            cursor = sub_end;
        }
        return true;
    }

    sema::SemanticConstantRef assemble_array_constant(
        SemanticTypeRef type, const std::vector<sema::SemanticConstantRef> &flat,
        std::size_t &cursor) {
        if (!type->is_array()) {
            return cursor < flat.size() ? flat[cursor++] : zero_constant(type);
        }
        std::vector<sema::SemanticConstantRef> elements;
        elements.reserve(static_cast<std::size_t>(type->array_bound()));
        for (std::uint64_t i = 0; i < type->array_bound(); ++i) {
            auto element = assemble_array_constant(type->element_type(), flat, cursor);
            if (!element) return nullptr;
            elements.push_back(std::move(element));
        }
        return sema::SemanticConstant::aggregate(type, std::move(elements));
    }

    sema::SemanticConstantRef build_initializer_constant(InitVal &initializer,
                                                         SemanticTypeRef target) {
        if (target->is_integer() || target->is_float()) {
            if (initializer.expr) return constant_as(*initializer.expr, target);
            if (initializer.elems.empty()) return zero_constant(target);
            if (initializer.elems.size() == 1)
                return build_initializer_constant(*initializer.elems.front(), target);
            return nullptr;
        }
        if (target->is_fixed_vector() || target->is_mask()) {
            if (initializer.expr) return constant_as(*initializer.expr, target);
            if (initializer.elems.empty()) return zero_constant(target);
            if (initializer.elems.size() != target->lane_count()) return nullptr;
            std::vector<sema::SemanticConstantRef> lanes;
            lanes.reserve(initializer.elems.size());
            auto *lane_type = target->is_mask() ? types().int_type() : target->element_type();
            for (auto &lane_initializer : initializer.elems) {
                if (!lane_initializer->expr || !lane_initializer->elems.empty()) return nullptr;
                auto lane = constant_as(*lane_initializer->expr, lane_type);
                if (!lane) return nullptr;
                if (target->is_mask() &&
                    (lane->kind() != sema::SemanticConstant::Kind::Integer ||
                     (lane->integer_value() != 0 && lane->integer_value() != 1)))
                    return nullptr;
                lanes.push_back(std::move(lane));
            }
            return sema::SemanticConstant::aggregate(target, std::move(lanes));
        }
        if (!target->is_array() || initializer.expr) return nullptr;
        if (initializer.elems.empty()) return zero_constant(target);

        SemanticTypeRef leaf = nullptr;
        std::vector<std::uint64_t> dimensions;
        collect_array_shape(target, dimensions, leaf);
        if (leaf->is_fixed_vector() || leaf->is_mask()) {
            std::vector<sema::SemanticConstantRef> elements;
            elements.reserve(static_cast<std::size_t>(target->array_bound()));
            for (std::uint64_t i = 0; i < target->array_bound(); ++i) {
                if (i < initializer.elems.size()) {
                    auto element =
                        build_initializer_constant(*initializer.elems[static_cast<std::size_t>(i)],
                                                   target->element_type());
                    if (!element) return nullptr;
                    elements.push_back(std::move(element));
                } else {
                    elements.push_back(zero_constant(target->element_type()));
                }
            }
            return sema::SemanticConstant::aggregate(target, std::move(elements));
        }

        const auto total = suffix_element_count(dimensions, 0);
        std::vector<sema::SemanticConstantRef> flat(static_cast<std::size_t>(total),
                                                    zero_constant(leaf));
        std::uint64_t flat_cursor = 0;
        if (!flatten_scalar_initializer(initializer, leaf, dimensions, 0, flat_cursor, total,
                                        flat))
            return nullptr;
        std::size_t assemble_cursor = 0;
        return assemble_array_constant(target, flat, assemble_cursor);
    }

    void check_constant_lane_index(Expr &index, std::uint64_t lanes,
                                   const std::string &object_name) {
        auto value = evaluate_integer(index);
        if (!value.ok()) return;
        model_->set_checked_constant(index, *value.value);
        const auto lane = value.value->value();
        if (lane < 0 || static_cast<std::uint64_t>(lane) >= lanes) {
            diagnose(front::DiagnosticCode::SemaLaneOutOfRange, index.source_range,
                     "constant lane index " + std::to_string(lane) + " for '" + object_name +
                         "' is outside [0, " + std::to_string(lanes) + ")");
        }
    }

    SemanticTypeRef expr_type(Expr &expression) {
        if (auto *type = model_->expr_type(expression)) return type;
        expression.accept(*this);
        if (auto *type = model_->expr_type(expression)) return type;
        model_->set_expr_type(expression, types().error_type());
        return types().error_type();
    }

    void add_conversion(Expr &expression, sema::ConversionKind kind, SemanticTypeRef source,
                        SemanticTypeRef target) {
        if (!source || !target) return;
        if (const auto *chain = model_->conversions(expression); chain && !chain->empty()) {
            const auto &last = chain->back();
            if (last.kind == kind && last.source_type == source && last.target_type == target) return;
        }
        model_->add_conversion(expression, sema::ConversionInfo{kind, source, target});
    }

    SemanticTypeRef as_value(Expr &expression) {
        auto *type = expr_type(expression);
        if (type->is_array()) {
            auto *pointer = types().pointer_type(type->element_type());
            add_conversion(expression, sema::ConversionKind::ArrayToPointer, type, pointer);
            return pointer;
        }
        if (dynamic_cast<LValExpr *>(&expression) && !type->is_error() && !type->is_void())
            add_conversion(expression, sema::ConversionKind::LValueToRValue, type, type);
        return type;
    }

    bool convert_context(Expr &expression, SemanticTypeRef source, SemanticTypeRef target,
                         const std::string &context) {
        if (source->is_error() || target->is_error()) return false;
        if (source->is_array()) {
            if (target->is_pointer()) {
                auto *immediate_element = source->element_type();
                auto *scalar_element = immediate_element;
                while (scalar_element->is_array()) {
                    scalar_element = scalar_element->element_type();
                }
                if (immediate_element == target->pointee_type() ||
                    scalar_element == target->pointee_type()) {
                    // Existing SysY runtime calls treat a multidimensional
                    // scalar array as contiguous storage. This remains an
                    // array-only conversion: vectors and masks never decay.
                    add_conversion(expression, sema::ConversionKind::ArrayToPointer, source,
                                   target);
                    return true;
                }
            }
            diagnose(front::DiagnosticCode::SemaInvalidConversion, expression.source_range,
                     context + " cannot convert " + source->str() + " to " + target->str() +
                         "; only arrays decay to pointers");
            return false;
        }
        if (dynamic_cast<LValExpr *>(&expression) && !source->is_void())
            add_conversion(expression, sema::ConversionKind::LValueToRValue, source, source);
        if (source == target) {
            add_conversion(expression, sema::ConversionKind::Identity, source, target);
            return true;
        }
        if (source->is_integer() && target->is_float()) {
            add_conversion(expression, sema::ConversionKind::IntToFloat, source, target);
            return true;
        }
        if (source->is_float() && target->is_integer()) {
            add_conversion(expression, sema::ConversionKind::FloatToInt, source, target);
            return true;
        }
        if (target->is_fixed_vector() && source->is_numeric_scalar()) {
            auto *element = target->element_type();
            if (source == element) {
                add_conversion(expression, sema::ConversionKind::ScalarSplat, source, target);
                return true;
            }
            if (source->is_integer() && element->is_float()) {
                add_conversion(expression, sema::ConversionKind::IntToFloat, source, element);
                add_conversion(expression, sema::ConversionKind::ScalarSplat, element, target);
                return true;
            }
        }
        diagnose(front::DiagnosticCode::SemaInvalidConversion, expression.source_range,
                 context + " cannot convert " + source->str() + " to " + target->str());
        return false;
    }

    bool is_condition_type(SemanticTypeRef type) const { return type->is_numeric_scalar(); }

    void check_condition(Expr &condition, const char *statement) {
        auto *type = as_value(condition);
        if (!type->is_error() && !is_condition_type(type)) {
            diagnose(front::DiagnosticCode::SemaInvalidCondition, condition.source_range,
                     std::string(statement) + " condition must be scalar int or float, not " +
                         type->str());
            return;
        }
        if (!type->is_error())
            add_conversion(condition, sema::ConversionKind::ScalarToBool, type, types().int_type());
    }

    void invalid_operands(BinaryExpr &node, SemanticTypeRef lhs, SemanticTypeRef rhs,
                          const std::string &reason) {
        diagnose(front::DiagnosticCode::SemaInvalidOperand, node.source_range,
                 "invalid operands to '" + binary_operator_name(node.op) + "': " + lhs->str() +
                     " and " + rhs->str() + "; " + reason);
        set_expr_type(node, types().error_type());
    }

    SemanticTypeRef promote_scalar_numeric(Expr &lhs_expression, SemanticTypeRef lhs,
                                           Expr &rhs_expression, SemanticTypeRef rhs) {
        if (lhs->is_float() || rhs->is_float()) {
            if (lhs->is_integer())
                add_conversion(lhs_expression, sema::ConversionKind::IntToFloat, lhs,
                               types().float_type());
            if (rhs->is_integer())
                add_conversion(rhs_expression, sema::ConversionKind::IntToFloat, rhs,
                               types().float_type());
            return types().float_type();
        }
        return types().int_type();
    }

    bool coerce_vector_operands(BinaryExpr &node, SemanticTypeRef lhs, SemanticTypeRef rhs,
                                bool integer_only, SemanticTypeRef &result) {
        auto *lhs_vector = lhs->is_fixed_vector() ? lhs : nullptr;
        auto *rhs_vector = rhs->is_fixed_vector() ? rhs : nullptr;
        if (lhs_vector && rhs_vector) {
            if (lhs_vector != rhs_vector) {
                invalid_operands(node, lhs, rhs,
                                 "vector operands must have identical element type and lane count");
                return false;
            }
            result = lhs_vector;
        } else {
            auto *vector = lhs_vector ? lhs_vector : rhs_vector;
            auto *scalar = lhs_vector ? rhs : lhs;
            auto &scalar_expression = lhs_vector ? *node.rhs : *node.lhs;
            if (!scalar->is_numeric_scalar()) {
                invalid_operands(node, lhs, rhs,
                                 "vector/scalar operation requires a compatible numeric scalar");
                return false;
            }
            if (!convert_context(scalar_expression, scalar, vector, "vector scalar splat")) {
                set_expr_type(node, types().error_type());
                return false;
            }
            result = vector;
        }
        if (integer_only && !result->element_type()->is_integer()) {
            invalid_operands(node, lhs, rhs, "operator requires integer vector elements");
            return false;
        }
        return true;
    }

    sema::CheckedIntegerResult evaluate_integer(const Expr &expression) {
        return sema::evaluate_integer_constant(
            expression, [this](std::string_view name) -> std::optional<sema::CheckedInteger> {
                auto *symbol = symbols_.lookup(name);
                if (!symbol || symbol->kind != SemanticSymbol::Kind::Variable || !symbol->is_const)
                    return std::nullopt;
                return symbol->integer_constant;
            });
    }

    void record_checked_constant(Expr &expression) {
        auto result = evaluate_integer(expression);
        if (result.ok()) {
            model_->set_checked_constant(expression, *result.value);
            model_->set_constant(
                expression,
                sema::SemanticConstant::integer(types().int_type(), result.value->value()));
        }
    }

    void diagnose_constant_failure(const Expr &expression,
                                   const sema::CheckedIntegerResult &result,
                                   const std::string &prefix) {
        front::DiagnosticCode code = front::DiagnosticCode::SemaInvalidConstantExpression;
        if (result.error == sema::ConstantEvalError::Overflow)
            code = front::DiagnosticCode::ConstOverflow;
        else if (result.error == sema::ConstantEvalError::DivisionByZero)
            code = front::DiagnosticCode::ConstDivisionByZero;
        else if (result.error == sema::ConstantEvalError::InvalidLiteral)
            code = front::DiagnosticCode::ConstInvalidLiteral;
        else if (result.error == sema::ConstantEvalError::WidthMismatch)
            code = front::DiagnosticCode::ConstWidthMismatch;
        diagnose(code, expression.source_range, prefix + result.message);
    }

    std::optional<std::uint64_t> resolve_extent(Expr &expression, sema::ExtentKind kind) {
        auto *expression_type = expr_type(expression);
        if (!expression_type->is_error() && !expression_type->is_integer()) {
            diagnose(front::DiagnosticCode::SemaInvalidConstantExpression, expression.source_range,
                     std::string(kind == sema::ExtentKind::LaneCount ? "lane count" :
                                                                       "array bound") +
                         " must be an integer constant expression, not " + expression_type->str());
            return std::nullopt;
        }
        auto constant = evaluate_integer(expression);
        if (!constant.ok()) {
            diagnose_constant_failure(expression, constant,
                                      kind == sema::ExtentKind::LaneCount ?
                                          "invalid lane count: " : "invalid array bound: ");
            return std::nullopt;
        }
        model_->set_checked_constant(expression, *constant.value);
        auto extent = sema::checked_positive_extent(*constant.value, kind);
        if (!extent.ok()) {
            diagnose(front::DiagnosticCode::SemaInvalidExtent, expression.source_range,
                     extent.message);
            return std::nullopt;
        }
        model_->set_checked_extent(expression, *extent.value);
        return extent.value;
    }

    SemanticTypeRef resolve_type_syntax(const TypeSyntaxRef &syntax, bool permit_void,
                                        const char *use) {
        if (!syntax) {
            diagnose(front::DiagnosticCode::SemaInvalidType, {},
                     std::string(use) + " has no parsed type syntax");
            return types().error_type();
        }

        auto found = source_type_cache_.find(syntax.get());
        SemanticTypeRef result = nullptr;
        if (found != source_type_cache_.end()) result = found->second;
        else if (syntax->kind() == TypeSyntax::Kind::Builtin) {
            switch (syntax->builtin_type()) {
            case BuiltinType::Void: result = types().void_type(); break;
            case BuiltinType::Int: result = types().int_type(); break;
            case BuiltinType::Float: result = types().float_type(); break;
            }
            source_type_cache_.emplace(syntax.get(), result);
        } else {
            auto &lane_expression = const_cast<Expr &>(syntax->lane_expression()->expression());
            auto lane_count = resolve_extent(lane_expression, sema::ExtentKind::LaneCount);
            if (!lane_count) result = types().error_type();
            else if (syntax->kind() == TypeSyntax::Kind::Mask)
                result = types().mask_type(*lane_count);
            else {
                SemanticTypeRef element = nullptr;
                switch (syntax->vector_element_type()) {
                case BuiltinType::Int: element = types().int_type(); break;
                case BuiltinType::Float: element = types().float_type(); break;
                case BuiltinType::Void:
                    diagnose(front::DiagnosticCode::SemaInvalidType, syntax->source_range(),
                             "vector element type must be int or float");
                    result = types().error_type();
                    break;
                }
                if (!result) result = types().fixed_vector_type(element, *lane_count);
            }
            source_type_cache_.emplace(syntax.get(), result);
        }

        if (!permit_void && result->is_void()) {
            diagnose(front::DiagnosticCode::SemaInvalidType, syntax->source_range(),
                     std::string(use) + " cannot have type void");
            return types().error_type();
        }
        return result;
    }

    SemanticTypeRef resolve_declarator_type(
        SemanticTypeRef base, const std::vector<std::unique_ptr<Expr>> &dimensions,
        bool parameter, front::SourceRange declaration_range) {
        if (dimensions.empty()) return base;
        std::vector<std::optional<std::uint64_t>> bounds;
        bounds.reserve(dimensions.size());
        for (std::size_t i = 0; i < dimensions.size(); ++i) {
            if (!dimensions[i]) {
                if (!parameter || i != 0)
                    diagnose(front::DiagnosticCode::SemaInvalidExtent, declaration_range,
                             "only the first array-parameter dimension may be unsized");
                bounds.push_back(std::nullopt);
            } else {
                bounds.push_back(resolve_extent(*dimensions[i], sema::ExtentKind::ArrayBound));
            }
        }

        SemanticTypeRef result = base;
        const std::size_t first_nested = parameter ? 1 : 0;
        for (std::size_t i = bounds.size(); i-- > first_nested;) {
            if (!bounds[i] || result->is_error()) result = types().error_type();
            else result = types().array_type(result, *bounds[i]);
        }
        if (parameter) return result->is_error() ? result : types().pointer_type(result);
        return result;
    }

    void declare_function(FuncDef &function) {
        auto *return_type =
            resolve_type_syntax(function.return_type_syntax, true, "function return type");
        std::vector<SemanticTypeRef> parameter_types;
        for (auto &parameter : function.params) {
            auto *base = resolve_type_syntax(parameter.type_syntax, false, "parameter");
            auto *parameter_type =
                resolve_declarator_type(base, parameter.dimensions, true, parameter.source_range);
            model_->set_parameter_type(parameter, parameter_type);
            parameter_types.push_back(parameter_type);
        }
        auto *function_type =
            types().function_type(return_type, parameter_types, function.is_variadic);
        model_->set_function_type(function, function_type);

        auto *existing = symbols_.lookup_current(function.name);
        if (existing != nullptr) {
            if (existing->kind != SemanticSymbol::Kind::Function) {
                diagnose(front::DiagnosticCode::SemaRedefinition, function.source_range,
                         "declaration of function '" + function.name +
                             "' conflicts with an existing symbol");
                return;
            }
            if (existing->type != function_type) {
                diagnose(front::DiagnosticCode::SemaConflictingDeclaration,
                         function.source_range,
                         "conflicting declaration of function '" + function.name +
                             "': previously " + existing->type->str() + ", now " +
                             function_type->str());
                return;
            }
            if (!function.is_external) {
                if (existing->has_definition) {
                    diagnose(front::DiagnosticCode::SemaRedefinition, function.source_range,
                             "redefinition of function '" + function.name + "'");
                    return;
                }
                existing->has_definition = true;
                existing->function = &function;
            }
            return;
        }

        SemanticSymbol symbol;
        symbol.kind = SemanticSymbol::Kind::Function;
        symbol.type = function_type;
        symbol.function = &function;
        symbol.has_definition = !function.is_external;
        (void)symbols_.define(function.name, std::move(symbol));
    }

    void install_builtins() {
        for (const auto &descriptor : builtin::BuiltinRegistry::instance().entries()) {
            SemanticSymbol symbol;
            symbol.kind = SemanticSymbol::Kind::Builtin;
            symbol.type = types().error_type();
            symbol.builtin = &descriptor;
            if (!symbols_.define(std::string(descriptor.source_name), std::move(symbol)))
                diagnose(front::DiagnosticCode::SemaRedefinition, {},
                         "duplicate builtin registry entry '" +
                             std::string(descriptor.source_name) + "'");
        }
    }

    std::uint64_t scalar_slot_count(SemanticTypeRef type) const {
        std::uint64_t count = 1;
        while (type->is_array()) {
            if (count > std::numeric_limits<std::uint64_t>::max() / type->array_bound())
                return std::numeric_limits<std::uint64_t>::max();
            count *= type->array_bound();
            type = type->element_type();
        }
        return count;
    }

    SemanticTypeRef array_leaf_type(SemanticTypeRef type) const {
        while (type->is_array()) type = type->element_type();
        return type;
    }

    void collect_initializer_expressions(InitVal &initializer, std::vector<Expr *> &expressions) {
        if (initializer.expr) expressions.push_back(initializer.expr.get());
        for (auto &element : initializer.elems)
            collect_initializer_expressions(*element, expressions);
    }

    bool validate_initializer(InitVal &initializer, SemanticTypeRef target,
                              const std::string &context) {
        if (target->is_error()) {
            initializer.accept(*this);
            return false;
        }
        if (target->is_void() || target->is_function()) {
            diagnose(front::DiagnosticCode::SemaInvalidInitializer, initializer.source_range,
                     context + " targets non-object type " + target->str());
            initializer.accept(*this);
            return false;
        }
        if (target->is_array()) {
            if (initializer.expr) {
                expr_type(*initializer.expr);
                diagnose(front::DiagnosticCode::SemaInvalidInitializer, initializer.source_range,
                         context + " cannot initialize array from a single expression");
                return false;
            }
            auto *leaf = array_leaf_type(target);
            if (leaf->is_fixed_vector() || leaf->is_mask()) {
                bool valid = true;
                if (initializer.elems.size() > target->array_bound()) {
                    diagnose(front::DiagnosticCode::SemaInvalidInitializer,
                             initializer.source_range,
                             context + " has more elements than array bound " +
                                 std::to_string(target->array_bound()));
                    valid = false;
                }
                for (auto &element : initializer.elems)
                    valid = validate_initializer(*element, target->element_type(), context) && valid;
                return valid;
            }
            std::vector<Expr *> expressions;
            collect_initializer_expressions(initializer, expressions);
            bool valid = true;
            if (expressions.size() > scalar_slot_count(target)) {
                diagnose(front::DiagnosticCode::SemaInvalidInitializer, initializer.source_range,
                         context + " has too many scalar initializers for " + target->str());
                valid = false;
            }
            for (auto *expression : expressions) {
                auto *source = expr_type(*expression);
                valid = convert_context(*expression, source, leaf, context) && valid;
            }
            return valid;
        }
        if (target->is_fixed_vector() || target->is_mask()) {
            if (initializer.expr) {
                auto *source = expr_type(*initializer.expr);
                return convert_context(*initializer.expr, source, target, context);
            }
            if (initializer.elems.empty()) return true;
            bool valid = true;
            if (initializer.elems.size() != target->lane_count()) {
                diagnose(front::DiagnosticCode::SemaInvalidInitializer, initializer.source_range,
                         context + " requires exactly " + std::to_string(target->lane_count()) +
                             " lane initializers, got " +
                             std::to_string(initializer.elems.size()));
                valid = false;
            }
            auto *lane_type = target->is_mask() ? types().int_type() : target->element_type();
            for (auto &element : initializer.elems) {
                if (!element->expr || !element->elems.empty()) {
                    element->accept(*this);
                    diagnose(front::DiagnosticCode::SemaInvalidInitializer, element->source_range,
                             context + " lane initializer must be a scalar expression");
                    valid = false;
                } else {
                    auto *source = expr_type(*element->expr);
                    if (target->is_mask()) {
                        auto value = evaluate_integer(*element->expr);
                        if (!source->is_integer() || !value.ok() ||
                            (value.value->value() != 0 && value.value->value() != 1)) {
                            diagnose(front::DiagnosticCode::SemaInvalidVectorLiteral,
                                     element->source_range,
                                     context +
                                         " mask lane must be the compile-time integer 0 or 1");
                            valid = false;
                        } else {
                            model_->set_checked_constant(*element->expr, *value.value);
                            model_->set_constant(
                                *element->expr,
                                sema::SemanticConstant::integer(types().int_type(),
                                                                value.value->value()));
                        }
                    } else {
                        valid = convert_context(*element->expr, source, lane_type,
                                                context + " lane") && valid;
                    }
                }
            }
            return valid;
        }
        if (initializer.expr) {
            auto *source = expr_type(*initializer.expr);
            return convert_context(*initializer.expr, source, target, context);
        }
        if (initializer.elems.empty()) return true;
        if (initializer.elems.size() != 1) {
            initializer.accept(*this);
            diagnose(front::DiagnosticCode::SemaInvalidInitializer, initializer.source_range,
                     context + " has multiple initializers for scalar type " + target->str());
            return false;
        }
        return validate_initializer(*initializer.elems.front(), target, context);
    }

    SemanticTypeRef primitive_pattern_type(builtin::TypePatternKind kind) {
        using K = builtin::TypePatternKind;
        switch (kind) {
        case K::Void: return types().void_type();
        case K::Int:
        case K::IntegerScalar: return types().int_type();
        case K::Float: return types().float_type();
        case K::PointerToInt: return types().pointer_type(types().int_type());
        case K::PointerToFloat: return types().pointer_type(types().float_type());
        case K::NumericScalar:
        case K::NumericVector:
        case K::VectorOrMask:
        case K::IntegerVector:
        case K::Mask:
        case K::ScalarOfVector:
        case K::PointerToVectorElement:
        case K::SameAsArgument: return nullptr;
        }
        return nullptr;
    }

    SemanticTypeRef related_pattern_type(const builtin::TypePattern &pattern,
                                         const std::vector<SemanticTypeRef> &arguments,
                                         std::optional<std::uint64_t> lanes) {
        using K = builtin::TypePatternKind;
        if (auto *primitive = primitive_pattern_type(pattern.kind)) return primitive;
        if (pattern.kind == K::NumericScalar) return nullptr;
        if (pattern.kind == K::Mask)
            return lanes ? types().mask_type(*lanes) : nullptr;
        if (pattern.argument_index >= arguments.size()) return nullptr;
        auto *related = arguments[pattern.argument_index];
        if (pattern.kind == K::SameAsArgument) return related;
        if (pattern.kind == K::ScalarOfVector)
            return related->is_fixed_vector() ? related->element_type() : nullptr;
        if (pattern.kind == K::PointerToVectorElement)
            return related->is_fixed_vector() ? types().pointer_type(related->element_type()) :
                                                nullptr;
        if (pattern.kind == K::VectorOrMask)
            return related->is_fixed_vector() || related->is_mask() ? related : nullptr;
        if (pattern.kind == K::NumericVector || pattern.kind == K::IntegerVector)
            return related->is_fixed_vector() ? related : nullptr;
        return nullptr;
    }

    bool match_builtin_argument(CallExpr &call, std::size_t index,
                                const builtin::TypePattern &pattern,
                                std::vector<SemanticTypeRef> &arguments) {
        using K = builtin::TypePatternKind;
        auto &expression = *call.args[index];
        auto *source = arguments[index];
        if (auto *target = primitive_pattern_type(pattern.kind)) {
            bool valid = convert_context(expression, source, target,
                                         "argument " + std::to_string(index + 1) +
                                             " of builtin '" + call.func_name + "'");
            if (valid) arguments[index] = target;
            return valid;
        }
        auto *value = as_value(expression);
        arguments[index] = value;
        bool matches = false;
        switch (pattern.kind) {
        case K::NumericScalar: matches = value->is_numeric_scalar(); break;
        case K::IntegerScalar: matches = value->is_integer(); break;
        case K::NumericVector: matches = value->is_fixed_vector(); break;
        case K::VectorOrMask: matches = value->is_fixed_vector() || value->is_mask(); break;
        case K::IntegerVector:
            matches = value->is_fixed_vector() && value->element_type()->is_integer();
            break;
        case K::Mask: matches = value->is_mask(); break;
        case K::SameAsArgument:
        case K::ScalarOfVector:
        case K::PointerToVectorElement: {
            auto *target = related_pattern_type(pattern, arguments, std::nullopt);
            if (target) {
                matches = convert_context(expression, source, target,
                                          "argument " + std::to_string(index + 1) +
                                              " of builtin '" + call.func_name + "'");
                if (matches) arguments[index] = target;
            }
            break;
        }
        case K::Void:
        case K::Int:
        case K::Float:
        case K::PointerToInt:
        case K::PointerToFloat: break;
        }
        if (!matches)
            diagnose(front::DiagnosticCode::SemaArgumentMismatch, expression.source_range,
                     "argument " + std::to_string(index + 1) + " of builtin '" +
                         call.func_name + "' does not match its type pattern (got " +
                         value->str() + ")");
        return matches;
    }

    void resolve_builtin_call(CallExpr &call, const builtin::BuiltinDescriptor &descriptor) {
        bool valid = true;
        if ((!descriptor.variadic && call.args.size() != descriptor.parameters.size()) ||
            (descriptor.variadic && call.args.size() < descriptor.parameters.size())) {
            diagnose(front::DiagnosticCode::SemaArgumentMismatch, call.source_range,
                     "builtin '" + call.func_name + "' expects " +
                         std::to_string(descriptor.parameters.size()) +
                         (descriptor.variadic ? " or more" : "") + " arguments, got " +
                         std::to_string(call.args.size()));
            valid = false;
        }
        std::vector<SemanticTypeRef> arguments;
        for (auto &argument : call.args) arguments.push_back(expr_type(*argument));
        const auto fixed_count = std::min(arguments.size(), descriptor.parameters.size());
        for (std::size_t i = 0; i < fixed_count; ++i)
            valid = match_builtin_argument(call, i, descriptor.parameters[i], arguments) && valid;
        for (std::size_t i = descriptor.parameters.size(); i < arguments.size(); ++i) {
            arguments[i] = as_value(*call.args[i]);
            if (arguments[i]->is_void()) {
                diagnose(front::DiagnosticCode::SemaArgumentMismatch,
                         call.args[i]->source_range,
                         "variadic builtin argument cannot have type void");
                valid = false;
            }
            if (contains_vector_or_mask_aggregate(arguments[i])) {
                diagnose(front::DiagnosticCode::SemaVariadicAggregate,
                         call.args[i]->source_range,
                         "variadic builtin argument " + std::to_string(i + 1) + " of '" +
                             call.func_name +
                             "' cannot contain a fixed vector or mask value");
                valid = false;
            }
        }

        std::optional<std::uint64_t> lanes;
        for (auto *argument : arguments) {
            if (!argument->is_fixed_vector() && !argument->is_mask()) continue;
            if (!lanes) lanes = argument->lane_count();
            else if (*lanes != argument->lane_count()) {
                diagnose(front::DiagnosticCode::SemaArgumentMismatch, call.source_range,
                         "builtin '" + call.func_name +
                             "' requires all vector and mask arguments to have the same lane count");
                valid = false;
            }
        }

        const auto builtin_id = descriptor.id;
        if ((builtin_id == builtin::BuiltinID::VectorExtract ||
             builtin_id == builtin::BuiltinID::VectorInsert) &&
            call.args.size() >= 2 && !arguments.empty() && arguments[0]->is_fixed_vector()) {
            auto lane = evaluate_integer(*call.args[1]);
            if (lane.ok()) {
                model_->set_checked_constant(*call.args[1], *lane.value);
                if (lane.value->value() < 0 ||
                    static_cast<std::uint64_t>(lane.value->value()) >=
                        arguments[0]->lane_count()) {
                    diagnose(front::DiagnosticCode::SemaLaneOutOfRange,
                             call.args[1]->source_range,
                             "constant lane index " + std::to_string(lane.value->value()) +
                                 " is outside [0, " +
                                 std::to_string(arguments[0]->lane_count()) + ")");
                    valid = false;
                }
            }
        }

        if (builtin_id == builtin::BuiltinID::VectorShuffle && call.args.size() == 3 &&
            arguments.size() == 3 && arguments[0]->is_fixed_vector()) {
            const auto *literal = dynamic_cast<TypedVectorLiteralExpr *>(call.args[2].get());
            const auto *constant_slot = model_->constant(*call.args[2]);
            if (literal == nullptr || constant_slot == nullptr || !*constant_slot ||
                (!(*constant_slot)->type()->is_fixed_vector()) ||
                !(*constant_slot)->type()->element_type()->is_integer()) {
                diagnose(front::DiagnosticCode::SemaInvalidShuffleIndex,
                         call.args[2]->source_range,
                         "shuffle indices must be a compile-time integer vector literal");
                valid = false;
            } else {
                std::vector<std::int64_t> indices;
                if ((*constant_slot)->kind() == sema::SemanticConstant::Kind::AggregateZero) {
                    indices.assign(static_cast<std::size_t>(arguments[0]->lane_count()), 0);
                } else if ((*constant_slot)->kind() ==
                           sema::SemanticConstant::Kind::Aggregate) {
                    for (const auto &element : (*constant_slot)->elements()) {
                        if (!element ||
                            element->kind() != sema::SemanticConstant::Kind::Integer) {
                            indices.clear();
                            break;
                        }
                        indices.push_back(element->integer_value());
                    }
                }
                if (indices.size() != arguments[0]->lane_count()) {
                    diagnose(front::DiagnosticCode::SemaInvalidShuffleIndex,
                             call.args[2]->source_range,
                             "shuffle index literal must have the same lane count as its inputs");
                    valid = false;
                } else {
                    const auto upper = static_cast<std::int64_t>(arguments[0]->lane_count() * 2);
                    for (std::size_t i = 0; i < indices.size(); ++i) {
                        if (indices[i] < -1 || indices[i] >= upper) {
                            diagnose(front::DiagnosticCode::SemaInvalidShuffleIndex,
                                     literal->lanes.empty() ? literal->source_range
                                                            : literal->lanes[i]->source_range,
                                     "shuffle index " + std::to_string(indices[i]) +
                                         " is outside [-1, " + std::to_string(upper) + ")");
                            valid = false;
                        }
                    }
                }
            }
        }

        auto *result = related_pattern_type(descriptor.result, arguments, lanes);
        if (!result) {
            diagnose(front::DiagnosticCode::SemaArgumentMismatch, call.source_range,
                     "cannot instantiate result type of builtin '" + call.func_name + "'");
            valid = false;
            result = types().error_type();
        }
        if (valid) {
            sema::BuiltinBinding binding;
            binding.id = static_cast<sema::BuiltinBindingId>(descriptor.id);
            binding.result_type = result;
            binding.argument_types = arguments;
            for (auto *argument : arguments) {
                if (argument->is_fixed_vector() || argument->is_mask()) {
                    if (std::find(binding.type_arguments.begin(), binding.type_arguments.end(),
                                  argument) == binding.type_arguments.end()) {
                        binding.type_arguments.push_back(argument);
                    }
                    if (std::find(binding.integer_arguments.begin(),
                                  binding.integer_arguments.end(), argument->lane_count()) ==
                        binding.integer_arguments.end()) {
                        binding.integer_arguments.push_back(argument->lane_count());
                    }
                }
            }
            model_->set_builtin_binding(call, std::move(binding));
        }
        set_expr_type(call, valid ? result : types().error_type());
    }

    std::shared_ptr<sema::SemanticModel> model_;
    SemanticScopeStack symbols_;
    std::unordered_map<const TypeSyntax *, SemanticTypeRef> source_type_cache_;
    SemanticTypeRef current_return_type_ = nullptr;
    unsigned loop_depth_ = 0;
    bool in_global_declarations_ = false;
};

std::string format_first_diagnostic(const sema::SemanticModel &model) {
    const auto &diagnostics = model.diagnostics().diagnostics();
    if (diagnostics.empty()) return "semantic analysis failed";
    const auto &diagnostic = diagnostics.front();
    std::ostringstream out;
    out << front::diagnostic_code_name(diagnostic.code) << ": " << diagnostic.message;
    if (diagnostic.range.begin.valid())
        out << " at " << diagnostic.range.begin.line << ':' << diagnostic.range.begin.column;
    return out.str();
}

} // namespace

std::string_view ASTSemanticAnalysisPass::name() const { return "ASTSemanticAnalysisPass"; }

PassKind ASTSemanticAnalysisPass::kind() const { return PassKind::Verification; }

PassResult ASTSemanticAnalysisPass::run(PassContext &context) {
    if (!context.has_ast())
        return PassResult::fail("ASTSemanticAnalysisPass requires AST in pass context");
    auto model = std::make_shared<sema::SemanticModel>();
    Analyzer analyzer(model);
    const bool valid = analyzer.analyze(*context.ast());
    context.set_artifact<std::shared_ptr<sema::SemanticModel>>(std::string(kArtifactKey), model);
    if (!valid) return PassResult::fail(format_first_diagnostic(*model));
    return PassResult::ok(false);
}

} // namespace pass
