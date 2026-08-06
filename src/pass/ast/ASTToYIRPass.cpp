#include "pass/ast/ASTToYIRPass.h"

#include "builtin/BuiltinRegistry.h"
#include "pass/ast/ASTSemanticAnalysisPass.h"
#include "sema/SemanticModel.h"

#include "yir/YIR.h"
#include "yir/YIRVerifier.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace pass {
namespace {

std::string sanitize_name(const std::string &name) {
    std::string out;
    out.reserve(name.size());
    for (char ch : name) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '_' || ch == '.') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "v" : out;
}

class Lowerer final : public ASTVisitor {
    struct Symbol {
        enum class Kind {
            Variable,
            Function,
        };

        Kind kind = Kind::Variable;
        std::string name;
        yir::Value *value = nullptr;
        yir::TypePtr type;
        bool is_const = false;
        bool is_array = false;
        bool is_global = false;
        bool is_builtin = false;
        bool is_variadic = false;
        yir::Function *function = nullptr;
        yir::TypePtr return_type;
        std::vector<yir::TypePtr> param_types;
    };

    class SymbolTable {
      public:
        void enter_scope() {
            scopes_.push_back({});
        }

        void leave_scope() {
            scopes_.pop_back();
        }

        bool empty() const {
            return scopes_.empty();
        }

        bool define(Symbol symbol) {
            if (scopes_.empty()) {
                enter_scope();
            }
            auto &scope = scopes_.back();
            std::string name = symbol.name;
            return scope.emplace(std::move(name), std::move(symbol)).second;
        }

        Symbol *lookup(const std::string &name) {
            for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
                auto found = it->find(name);
                if (found != it->end()) {
                    return &found->second;
                }
            }
            return nullptr;
        }

      private:
        std::vector<std::unordered_map<std::string, Symbol>> scopes_;
    };

  public:
    explicit Lowerer(const sema::SemanticModel &model) : model_(model) {
    }

    std::unique_ptr<yir::Module> lower(CompUnit &unit) {
        module_ = std::make_unique<yir::Module>();
        unit.accept(*this);
        if (!errors_.empty()) {
            throw std::runtime_error(errors_.front());
        }
        return std::move(module_);
    }

    void visit(IntLiteral &node) override {
        const auto *constant = model_.constant(node);
        if (!constant || !*constant ||
            (*constant)->kind() != sema::SemanticConstant::Kind::Integer) {
            fail("FE_SEMANTIC_MODEL_INCOMPLETE: integer literal has no checked constant");
            last_value_ = nullptr;
            return;
        }
        last_value_ =
            emit<yir::ConstI32Op>(static_cast<int>((*constant)->integer_value()), fresh_temp());
    }

    void visit(FloatLiteral &node) override {
        const auto *constant = model_.constant(node);
        if (!constant || !*constant || (*constant)->kind() != sema::SemanticConstant::Kind::Float) {
            fail("FE_SEMANTIC_MODEL_INCOMPLETE: float literal has no checked constant");
            last_value_ = nullptr;
            return;
        }
        last_value_ = emit<yir::ConstF32Op>((*constant)->float_value(), fresh_temp());
    }

    void visit(LValExpr &node) override {
        last_value_ = lower_lval_value(node);
    }

    void visit(BinaryExpr &node) override {
        if (node.op == BinaryOp::And) {
            last_value_ = lower_logical_and(*node.lhs, *node.rhs);
            if (!has_boolean_context_conversion(node))
                last_value_ = emit<yir::ZExtI1ToI32Op>(last_value_, fresh_temp());
            return;
        }
        if (node.op == BinaryOp::Or) {
            last_value_ = lower_logical_or(*node.lhs, *node.rhs);
            if (!has_boolean_context_conversion(node))
                last_value_ = emit<yir::ZExtI1ToI32Op>(last_value_, fresh_temp());
            return;
        }

        yir::Value *lhs = lower_expr(*node.lhs);
        yir::Value *rhs = lower_expr(*node.rhs);
        auto *result_type = require_expr_type(node);
        const bool float_operation =
            result_type->is_float() ||
            (result_type->is_fixed_vector() && result_type->element_type()->is_float());

        switch (node.op) {
        case BinaryOp::Add:
            last_value_ = float_operation ? emit<yir::AddFOp>(lhs, rhs, fresh_temp())
                                          : emit<yir::AddIOp>(lhs, rhs, fresh_temp());
            return;
        case BinaryOp::Sub:
            last_value_ = float_operation ? emit<yir::SubFOp>(lhs, rhs, fresh_temp())
                                          : emit<yir::SubIOp>(lhs, rhs, fresh_temp());
            return;
        case BinaryOp::Mul:
            last_value_ = float_operation ? emit<yir::MulFOp>(lhs, rhs, fresh_temp())
                                          : emit<yir::MulIOp>(lhs, rhs, fresh_temp());
            return;
        case BinaryOp::Div:
            last_value_ = float_operation ? emit<yir::DivFOp>(lhs, rhs, fresh_temp())
                                          : emit<yir::DivSIOp>(lhs, rhs, fresh_temp());
            return;
        case BinaryOp::Mod:
            last_value_ = emit<yir::RemSIOp>(lhs, rhs, fresh_temp());
            return;
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge:
        case BinaryOp::Eq:
        case BinaryOp::Ne:
            last_value_ = operand_is_float(lhs)
                              ? emit<yir::FCmpOp>(to_fcmp(node.op), lhs, rhs, fresh_temp())
                              : emit<yir::ICmpOp>(to_icmp(node.op), lhs, rhs, fresh_temp());
            if (result_type->is_integer() && !has_boolean_context_conversion(node))
                last_value_ = emit<yir::ZExtI1ToI32Op>(last_value_, fresh_temp());
            return;
        case BinaryOp::BitAnd:
            last_value_ =
                result_type->is_mask()
                    ? emit<yir::MaskBinaryOp>(yir::MaskBinaryOp::Kind::And, lhs, rhs, fresh_temp())
                    : emit<yir::AndIOp>(lhs, rhs, fresh_temp());
            return;
        case BinaryOp::BitXor:
            last_value_ =
                result_type->is_mask()
                    ? emit<yir::MaskBinaryOp>(yir::MaskBinaryOp::Kind::Xor, lhs, rhs, fresh_temp())
                    : emit<yir::XorIOp>(lhs, rhs, fresh_temp());
            return;
        case BinaryOp::BitOr:
            last_value_ =
                result_type->is_mask()
                    ? emit<yir::MaskBinaryOp>(yir::MaskBinaryOp::Kind::Or, lhs, rhs, fresh_temp())
                    : emit<yir::OrIOp>(lhs, rhs, fresh_temp());
            return;
        case BinaryOp::And:
        case BinaryOp::Or:
            break;
        }
    }

    void visit(UnaryExpr &node) override {
        yir::Value *value = lower_expr(*node.operand);
        auto *result_type = require_expr_type(node);
        switch (node.op) {
        case UnaryOp::Pos:
            last_value_ = value;
            return;
        case UnaryOp::Neg:
            if (result_type->is_float()) {
                auto *zero = emit<yir::ConstF32Op>(0.0F, fresh_temp());
                last_value_ = emit<yir::SubFOp>(zero, value, fresh_temp());
            } else if (result_type->is_fixed_vector()) {
                auto *zero = emit<yir::ZeroOp>(type_for_semantic(result_type), fresh_temp());
                last_value_ = result_type->element_type()->is_float()
                                  ? emit<yir::SubFOp>(zero, value, fresh_temp())
                                  : emit<yir::SubIOp>(zero, value, fresh_temp());
            } else {
                auto *zero = emit<yir::ConstI32Op>(0, fresh_temp());
                last_value_ = emit<yir::SubIOp>(zero, value, fresh_temp());
            }
            return;
        case UnaryOp::Not:
            last_value_ = emit<yir::NotOp>(to_bool(value), fresh_temp());
            if (!has_boolean_context_conversion(node))
                last_value_ = emit<yir::ZExtI1ToI32Op>(last_value_, fresh_temp());
            return;
        case UnaryOp::BitNot:
            last_value_ = result_type->is_mask() ? emit<yir::MaskNotOp>(value, fresh_temp())
                                                 : emit<yir::BitNotOp>(value, fresh_temp());
            return;
        }
    }

    void visit(CallExpr &node) override {
        if (const auto *binding = model_.builtin_binding(node)) {
            lower_builtin_call(node, *binding);
            return;
        }

        Symbol *callee = lookup_function(node.func_name);
        if (callee == nullptr) {
            fail("FE_SEMANTIC_MODEL_INCONSISTENT: no user function binding for '" + node.func_name +
                 "'");
            last_value_ = nullptr;
            return;
        }
        std::vector<yir::Value *> args;
        args.reserve(node.args.size());
        for (auto &argument : node.args)
            args.push_back(lower_expr(*argument));

        auto *op =
            emit<yir::CallOp>(node.func_name, std::move(args), callee->return_type, fresh_temp());
        last_value_ = op == nullptr ? nullptr : op;
    }

    void visit(TypedVectorLiteralExpr &node) override {
        auto *semantic_type = require_expr_type(node);
        auto result_type = type_for_semantic(semantic_type);
        if (node.lanes.empty()) {
            last_value_ = emit<yir::ZeroOp>(result_type, fresh_temp());
            return;
        }
        std::vector<yir::Value *> lanes;
        lanes.reserve(node.lanes.size());
        for (auto &lane : node.lanes) {
            auto *value = lower_expr(*lane);
            if (semantic_type->is_mask())
                value = to_i1(value);
            lanes.push_back(value);
        }
        last_value_ = emit<yir::VectorCreateOp>(result_type, std::move(lanes), fresh_temp());
    }

    void visit(VectorCastExpr &node) override {
        // The constructor's authoritative conversion chain is attached to its
        // operand by semantic analysis; lowering does not rediscover it.
        last_value_ = lower_expr(*node.operand);
    }

    void visit(InitVal &) override {
    }

    void visit(ExprStmt &node) override {
        if (node.expr) {
            lower_expr(*node.expr);
        }
    }

    void visit(AssignStmt &node) override {
        Symbol *target = lookup_variable(node.target->name);
        yir::Value *value = lower_expr(*node.value);
        if (target == nullptr || value == nullptr) {
            if (target == nullptr) {
                errors_.push_back("unknown variable: " + node.target->name);
            }
            return;
        }
        if (target->is_const) {
            errors_.push_back("cannot assign to const variable: " + node.target->name);
            return;
        }
        if (node.target->indices.empty()) {
            emit<yir::AssignOp>(target->value, value);
            return;
        }
        lower_indexed_assignment(*target, *node.target, value);
    }

    void visit(BlockStmt &node) override {
        enter_scope();
        for (auto &stmt : node.stmts) {
            stmt->accept(*this);
        }
        leave_scope();
    }

    void visit(ReturnStmt &node) override {
        if (!node.expr) {
            emit<yir::ReturnOp>();
            return;
        }
        yir::Value *value = lower_expr(*node.expr);
        emit<yir::ReturnOp>(value);
    }

    void visit(IfStmt &node) override {
        yir::Value *condition = to_bool(lower_expr(*node.cond));
        auto *if_op = current_region_->append<yir::IfOp>(condition);

        RegionScope then_scope(*this, if_op->then_region());
        node.then_stmt->accept(*this);

        if (node.else_stmt) {
            if_op->set_has_else(true);
            RegionScope else_scope(*this, if_op->else_region());
            node.else_stmt->accept(*this);
        }
    }

    void visit(WhileStmt &node) override {
        auto *while_op = current_region_->append<yir::WhileOp>();

        {
            RegionScope cond_scope(*this, while_op->cond_region());
            yir::Value *condition = to_bool(lower_expr(*node.cond));
            emit<yir::CondOp>(condition);
        }

        {
            RegionScope body_scope(*this, while_op->body_region());
            ++loop_depth_;
            node.body->accept(*this);
            --loop_depth_;
        }
    }

    void visit(BreakStmt &) override {
        if (loop_depth_ == 0) {
            errors_.push_back("break outside of loop");
        }
        emit<yir::BreakOp>();
    }

    void visit(ContinueStmt &) override {
        if (loop_depth_ == 0) {
            errors_.push_back("continue outside of loop");
        }
        emit<yir::ContinueOp>();
    }

    void visit(VarDecl &node) override {
        auto *semantic_type = model_.declaration_type(node);
        if (!semantic_type) {
            fail("FE_SEMANTIC_MODEL_INCOMPLETE: missing declaration type for '" + node.name + "'");
            return;
        }
        yir::TypePtr storage_type = type_for_semantic(semantic_type);
        if (storage_type->is_array()) {
            auto *array = emit<yir::ArrayVarOp>(storage_type, fresh_name(node.name));
            define_variable(node.name, array, storage_type, node.is_const, true, false);
            if (node.init) {
                emit_array_init(array, semantic_type, *node.init);
            }
            return;
        }

        yir::Value *initializer = nullptr;
        if (node.init) {
            initializer = lower_object_initializer(semantic_type, *node.init);
        } else {
            initializer = emit<yir::ZeroOp>(storage_type, fresh_temp());
        }
        auto *var = emit<yir::VarOp>(storage_type, initializer, fresh_name(node.name));
        define_variable(node.name, var, storage_type, node.is_const, false, false);
    }

    void visit(DeclStmt &node) override {
        for (auto &decl : node.decls) {
            decl->accept(*this);
        }
    }

    void visit(FuncDef &node) override {
        Symbol *function_symbol = lookup_function(node.name);
        if (function_symbol == nullptr || function_symbol->function == nullptr) {
            return;
        }

        current_function_ = function_symbol->function;
        current_region_ = &current_function_->body();
        auto *function_type = model_.function_type(node);
        if (!function_type || !function_type->is_function()) {
            fail("FE_SEMANTIC_MODEL_INCOMPLETE: missing function type for '" + node.name + "'");
            return;
        }

        if (node.is_external) {
            name_counts_.clear();
            for (std::size_t i = 0; i < node.params.size(); ++i) {
                const auto &param = node.params[i];
                auto *parameter_semantic_type = model_.parameter_type(param);
                if (!parameter_semantic_type) {
                    fail("FE_SEMANTIC_MODEL_INCOMPLETE: missing parameter type for external '" +
                         node.name + "'");
                    continue;
                }
                const std::string base =
                    param.name.empty() ? "arg" + std::to_string(i) : param.name + ".arg";
                current_function_->add_param(type_for_semantic(parameter_semantic_type),
                                             fresh_name(base));
            }
            current_function_ = nullptr;
            current_region_ = nullptr;
            return;
        }

        current_return_type_ = type_for_semantic(function_type->return_type());
        name_counts_.clear();
        temp_counter_ = 0;

        enter_scope();
        for (std::size_t i = 0; i < node.params.size(); ++i) {
            const auto &param = node.params[i];
            auto *parameter_semantic_type = model_.parameter_type(param);
            if (!parameter_semantic_type) {
                fail("FE_SEMANTIC_MODEL_INCOMPLETE: missing parameter type for '" + param.name +
                     "'");
                continue;
            }
            auto parameter_type = type_for_semantic(parameter_semantic_type);
            yir::Value *param_value =
                current_function_->add_param(parameter_type, fresh_name(param.name + ".arg"));
            if (!parameter_semantic_type->is_pointer()) {
                auto *var = emit<yir::VarOp>(parameter_type, param_value, fresh_name(param.name));
                define_variable(param.name, var, parameter_type, false, false, false);
            } else {
                define_variable(param.name, param_value, parameter_type, false, true, false);
            }
        }

        if (node.body) {
            node.body->accept(*this);
        }
        leave_scope();

        current_function_ = nullptr;
        current_region_ = nullptr;
        current_return_type_ = nullptr;
    }

    void visit(CompUnit &node) override {
        enter_scope();
        for (auto &decl : node.global_decls) {
            lower_global_decl(*decl);
        }

        // Semantic analysis has already checked compatibility. Collapse a
        // declaration family to one YIR symbol, preferring its definition.
        std::vector<FuncDef *> canonical_functions;
        std::unordered_map<std::string, std::size_t> function_positions;
        for (auto &owned : node.functions) {
            auto *func = owned.get();
            auto [position, inserted] =
                function_positions.emplace(func->name, canonical_functions.size());
            if (inserted) {
                canonical_functions.push_back(func);
            } else if (canonical_functions[position->second]->is_external && !func->is_external) {
                canonical_functions[position->second] = func;
            }
        }

        for (auto *func : canonical_functions) {
            auto *semantic_function_type = model_.function_type(*func);
            if (!semantic_function_type || !semantic_function_type->is_function()) {
                fail("FE_SEMANTIC_MODEL_INCOMPLETE: missing function type for '" + func->name +
                     "'");
                continue;
            }
            std::vector<yir::TypePtr> param_types;
            for (auto *parameter : semantic_function_type->parameter_types())
                param_types.push_back(type_for_semantic(parameter));
            auto return_type = type_for_semantic(semantic_function_type->return_type());
            auto *function =
                module_->add_function(func->name, return_type, param_types,
                                      semantic_function_type->is_variadic(), func->is_external);
            define_function(func->name, function, return_type, std::move(param_types), false,
                            semantic_function_type->is_variadic());
        }

        for (auto *func : canonical_functions) {
            func->accept(*this);
        }
        leave_scope();
    }

  private:
    class RegionScope {
      public:
        RegionScope(Lowerer &lowerer, yir::Region &region)
            : lowerer_(lowerer), previous_(lowerer.current_region_) {
            lowerer_.current_region_ = &region;
        }
        ~RegionScope() {
            lowerer_.current_region_ = previous_;
        }

      private:
        Lowerer &lowerer_;
        yir::Region *previous_;
    };

    template <typename OpT, typename... Args> yir::Value *emit(Args &&...args) {
        if (current_region_ == nullptr) {
            errors_.push_back("attempted to emit YIR outside of a region");
            return nullptr;
        }
        auto *op = current_region_->append<OpT>(std::forward<Args>(args)...);
        return op->result();
    }

    yir::Value *lower_expr(Expr &expr) {
        last_value_ = nullptr;
        expr.accept(*this);
        last_value_ = apply_semantic_conversions(expr, last_value_);
        return last_value_;
    }

    bool has_boolean_context_conversion(const Expr &expression) const {
        const auto *conversions = model_.conversions(expression);
        return conversions != nullptr &&
               std::any_of(conversions->begin(), conversions->end(), [](const auto &conversion) {
                   return conversion.kind == sema::ConversionKind::ScalarToBool;
               });
    }

    sema::SemanticTypeRef require_expr_type(const Expr &expression) {
        auto *type = model_.expr_type(expression);
        if (!type) {
            fail("FE_SEMANTIC_MODEL_INCOMPLETE: missing resolved expression type");
            return model_.types().error_type();
        }
        return type;
    }

    yir::TypePtr type_for_semantic(sema::SemanticTypeRef type) {
        if (!type || type->is_error()) {
            fail("FE_SEMANTIC_MODEL_INCOMPLETE: cannot lower missing/error semantic type");
            return yir::Type::get_i32();
        }
        switch (type->kind()) {
        case sema::SemanticType::Kind::Error:
            return yir::Type::get_i32();
        case sema::SemanticType::Kind::Void:
            return yir::Type::get_void();
        case sema::SemanticType::Kind::Int:
            return yir::Type::get_i32();
        case sema::SemanticType::Kind::Float:
            return yir::Type::get_f32();
        case sema::SemanticType::Kind::FixedVector:
            return yir::Type::get_vector(type->lane_count(),
                                         type_for_semantic(type->element_type()));
        case sema::SemanticType::Kind::Mask:
            return yir::Type::get_mask(type->lane_count());
        case sema::SemanticType::Kind::Array:
            return yir::Type::get_array(type->array_bound(),
                                        type_for_semantic(type->element_type()));
        case sema::SemanticType::Kind::Pointer:
            return yir::Type::get_ptr(type_for_semantic(type->pointee_type()));
        case sema::SemanticType::Kind::Function: {
            std::vector<yir::TypePtr> parameters;
            parameters.reserve(type->parameter_types().size());
            for (auto *parameter : type->parameter_types())
                parameters.push_back(type_for_semantic(parameter));
            return yir::Type::get_func(std::move(parameters),
                                       type_for_semantic(type->return_type()), type->is_variadic());
        }
        }
        return yir::Type::get_i32();
    }

    yir::Value *apply_semantic_conversions(Expr &expression, yir::Value *value) {
        if (!value)
            return nullptr;
        const auto *conversions = model_.conversions(expression);
        if (!conversions)
            return value;
        for (const auto &conversion : *conversions) {
            auto target = type_for_semantic(conversion.target_type);
            switch (conversion.kind) {
            case sema::ConversionKind::None:
            case sema::ConversionKind::Identity:
            case sema::ConversionKind::LValueToRValue:
                break;
            case sema::ConversionKind::ArrayToPointer:
                value = emit<yir::DecayOp>(value, target, fresh_temp());
                break;
            case sema::ConversionKind::IntToFloat:
                value = emit<yir::SIToFPOp>(value, fresh_temp());
                break;
            case sema::ConversionKind::FloatToInt:
                value = emit<yir::FPToSIOp>(value, fresh_temp());
                break;
            case sema::ConversionKind::ScalarToBool:
                value = to_bool(value);
                break;
            case sema::ConversionKind::BoolToInt:
            case sema::ConversionKind::MaskLaneToInt:
                value = emit<yir::ZExtI1ToI32Op>(value, fresh_temp());
                break;
            case sema::ConversionKind::ScalarSplat:
                value = emit<yir::SplatOp>(value, target, fresh_temp());
                break;
            case sema::ConversionKind::VectorElementCast:
                value = emit<yir::VectorCastOp>(value, target, fresh_temp());
                break;
            }
        }
        return value;
    }

    void lower_builtin_call(CallExpr &call, const sema::BuiltinBinding &binding) {
        const auto id = static_cast<builtin::BuiltinID>(binding.id);
        const auto *descriptor = builtin::BuiltinRegistry::instance().find(id);
        if (!descriptor) {
            fail("FE_SEMANTIC_MODEL_INCONSISTENT: unknown builtin binding id " +
                 std::to_string(binding.id));
            last_value_ = nullptr;
            return;
        }
        std::vector<yir::Value *> arguments;
        arguments.reserve(call.args.size());
        for (std::size_t i = 0; i < call.args.size(); ++i) {
            // Shuffle's third argument is a semantic-only structured constant;
            // it is encoded in ShuffleOp and has no runtime SSA operand.
            if (id == builtin::BuiltinID::VectorShuffle && i == 2)
                arguments.push_back(nullptr);
            else
                arguments.push_back(lower_expr(*call.args[i]));
        }
        auto result_type = type_for_semantic(binding.result_type);

        if (descriptor->lowering == builtin::LoweringKind::RuntimeCall) {
            last_value_ = emit<yir::CallOp>(std::string(descriptor->source_name),
                                            std::move(arguments), result_type, fresh_temp());
            return;
        }

        switch (id) {
        case builtin::BuiltinID::VectorSelect:
            last_value_ =
                emit<yir::SelectOp>(arguments[0], arguments[1], arguments[2], fresh_temp());
            return;
        case builtin::BuiltinID::VectorAny:
        case builtin::BuiltinID::VectorAll:
        case builtin::BuiltinID::VectorNone: {
            auto kind = yir::MaskReduceOp::Kind::Any;
            if (id == builtin::BuiltinID::VectorAll)
                kind = yir::MaskReduceOp::Kind::All;
            if (id == builtin::BuiltinID::VectorNone)
                kind = yir::MaskReduceOp::Kind::None;
            auto *predicate = emit<yir::MaskReduceOp>(kind, arguments[0], fresh_temp());
            last_value_ = emit<yir::ZExtI1ToI32Op>(predicate, fresh_temp());
            return;
        }
        case builtin::BuiltinID::VectorExtract:
            last_value_ = emit<yir::ExtractLaneOp>(arguments[0], arguments[1], fresh_temp());
            return;
        case builtin::BuiltinID::VectorInsert:
            last_value_ =
                emit<yir::InsertLaneOp>(arguments[0], arguments[1], arguments[2], fresh_temp());
            return;
        case builtin::BuiltinID::VectorIota:
            last_value_ = emit<yir::StepVectorOp>(result_type, fresh_temp());
            return;
        case builtin::BuiltinID::VectorReduceAdd:
        case builtin::BuiltinID::VectorReduceMul:
        case builtin::BuiltinID::VectorReduceMin:
        case builtin::BuiltinID::VectorReduceMax:
        case builtin::BuiltinID::VectorReduceAnd:
        case builtin::BuiltinID::VectorReduceOr:
        case builtin::BuiltinID::VectorReduceXor: {
            auto kind = yir::VectorReduceOp::Kind::Add;
            if (id == builtin::BuiltinID::VectorReduceMul)
                kind = yir::VectorReduceOp::Kind::Mul;
            else if (id == builtin::BuiltinID::VectorReduceMin)
                kind = yir::VectorReduceOp::Kind::Min;
            else if (id == builtin::BuiltinID::VectorReduceMax)
                kind = yir::VectorReduceOp::Kind::Max;
            else if (id == builtin::BuiltinID::VectorReduceAnd)
                kind = yir::VectorReduceOp::Kind::And;
            else if (id == builtin::BuiltinID::VectorReduceOr)
                kind = yir::VectorReduceOp::Kind::Or;
            else if (id == builtin::BuiltinID::VectorReduceXor)
                kind = yir::VectorReduceOp::Kind::Xor;
            const auto &operand_type = arguments[0]->type();
            const bool ordered = operand_type->is_vector() && operand_type->element()->is_float();
            last_value_ = emit<yir::VectorReduceOp>(kind, arguments[0], ordered, fresh_temp());
            return;
        }
        case builtin::BuiltinID::VectorMaskedLoad:
            last_value_ =
                emit<yir::MaskedLoadOp>(arguments[0], arguments[1], arguments[2], 4, fresh_temp());
            return;
        case builtin::BuiltinID::VectorMaskedStore:
            emit<yir::MaskedStoreOp>(arguments[2], arguments[0], arguments[1], 4);
            last_value_ = nullptr;
            return;
        case builtin::BuiltinID::VectorGather:
            last_value_ = emit<yir::GatherOp>(arguments[0], arguments[1], arguments[2],
                                              arguments[3], 4, fresh_temp());
            return;
        case builtin::BuiltinID::VectorScatter:
            emit<yir::ScatterOp>(arguments[3], arguments[0], arguments[1], arguments[2], 4);
            last_value_ = nullptr;
            return;
        case builtin::BuiltinID::VectorShuffle: {
            std::vector<std::uint64_t> indices;
            const auto *constant = model_.constant(*call.args[2]);
            if (!constant || !*constant) {
                fail("FE_SEMANTIC_MODEL_INCOMPLETE: shuffle binding has no structured indices");
                last_value_ = nullptr;
                return;
            }
            if ((*constant)->kind() == sema::SemanticConstant::Kind::AggregateZero) {
                indices.assign(static_cast<std::size_t>(binding.result_type->lane_count()), 0);
            } else {
                for (const auto &element : (*constant)->elements()) {
                    const auto index = element->integer_value();
                    indices.push_back(index == -1 ? yir::ShuffleOp::UndefLane
                                                  : static_cast<std::uint64_t>(index));
                }
            }
            last_value_ = emit<yir::ShuffleOp>(arguments[0], arguments[1], std::move(indices),
                                               result_type, fresh_temp());
            return;
        }
        case builtin::BuiltinID::Invalid:
        case builtin::BuiltinID::GetInt:
        case builtin::BuiltinID::GetCh:
        case builtin::BuiltinID::GetFloat:
        case builtin::BuiltinID::GetArray:
        case builtin::BuiltinID::GetFloatArray:
        case builtin::BuiltinID::PutInt:
        case builtin::BuiltinID::PutCh:
        case builtin::BuiltinID::PutArray:
        case builtin::BuiltinID::PutFloat:
        case builtin::BuiltinID::PutFloatArray:
        case builtin::BuiltinID::PutFormat:
        case builtin::BuiltinID::StartTime:
        case builtin::BuiltinID::StopTime:
        case builtin::BuiltinID::RuntimeRangesDisjoint:
            break;
        }
        fail("FE_SEMANTIC_MODEL_INCONSISTENT: builtin has no YIR lowering");
        last_value_ = nullptr;
    }

    yir::Value *lower_lval_value(LValExpr &lval) {
        Symbol *symbol = lookup_variable(lval.name);
        if (symbol == nullptr) {
            errors_.push_back("unknown variable: " + lval.name);
            return emit<yir::ConstI32Op>(0, fresh_temp());
        }
        if (lval.indices.empty()) {
            return symbol->value;
        }

        auto path = lower_index_path(*symbol, lval);
        yir::Value *value = symbol->value;
        if (!path.object_indices.empty()) {
            auto indexed_type =
                path.lane_index != nullptr ? path.lane_container_type : path.object_type;
            if (indexed_type->is_array()) {
                value = emit<yir::ElemAddrOp>(symbol->value, std::move(path.object_indices),
                                              yir::Type::get_ptr(indexed_type),
                                              fresh_name(lval.name + ".addr"));
            } else {
                value = emit<yir::ArrayLoadOp>(symbol->value, std::move(path.object_indices),
                                               indexed_type, fresh_name(lval.name));
            }
        }
        if (path.lane_index != nullptr) {
            value =
                emit<yir::ExtractLaneOp>(value, path.lane_index, fresh_name(lval.name + ".lane"));
        }
        return value;
    }

    struct IndexPath {
        std::vector<yir::Value *> object_indices;
        yir::TypePtr object_type;
        yir::TypePtr lane_container_type;
        yir::Value *lane_index = nullptr;
    };

    IndexPath lower_index_path(const Symbol &symbol, LValExpr &lvalue) {
        IndexPath path;
        auto current = symbol.type;
        for (auto &index_expression : lvalue.indices) {
            auto *index = lower_expr(*index_expression);
            if (current->is_vector() || current->is_mask()) {
                if (path.lane_index != nullptr) {
                    fail("FE_SEMANTIC_MODEL_INCONSISTENT: vector value has multiple lane indices");
                }
                path.lane_container_type = current;
                path.lane_index = index;
                current = current->is_mask() ? yir::Type::get_i1() : current->element();
                continue;
            }
            path.object_indices.push_back(index);
            if (current->is_ptr())
                current = current->pointee();
            else if (current->is_array())
                current = current->element();
            else
                fail("FE_SEMANTIC_MODEL_INCONSISTENT: non-indexable YIR object in lvalue");
        }
        path.object_type = current;
        return path;
    }

    void lower_indexed_assignment(Symbol &target, LValExpr &lvalue, yir::Value *value) {
        auto path = lower_index_path(target, lvalue);
        if (path.lane_index == nullptr) {
            emit<yir::ArrayStoreOp>(value, target.value, std::move(path.object_indices));
            return;
        }

        // Recompute the container type without consuming the final lane index.
        yir::TypePtr vector_type = target.type;
        std::size_t object_index = 0;
        while (object_index < path.object_indices.size()) {
            vector_type = vector_type->is_ptr() ? vector_type->pointee() : vector_type->element();
            ++object_index;
        }

        yir::Value *vector = target.value;
        if (!path.object_indices.empty())
            vector = emit<yir::ArrayLoadOp>(target.value, path.object_indices, vector_type,
                                            fresh_name(lvalue.name + ".update"));
        if (vector_type->is_mask())
            value = to_i1(value);
        auto *updated = emit<yir::InsertLaneOp>(vector, path.lane_index, value, fresh_temp());
        if (path.object_indices.empty())
            emit<yir::AssignOp>(target.value, updated);
        else
            emit<yir::ArrayStoreOp>(updated, target.value, std::move(path.object_indices));
    }

    yir::Value *lower_logical_and(Expr &lhs_expr, Expr &rhs_expr) {
        auto *false_value = emit<yir::ConstBoolOp>(false, fresh_temp());
        auto *result = emit<yir::VarOp>(yir::Type::get_i1(), false_value, fresh_name("logic"));

        yir::Value *lhs = to_bool(lower_expr(lhs_expr));
        auto *if_op = current_region_->append<yir::IfOp>(lhs);
        {
            RegionScope then_scope(*this, if_op->then_region());
            emit<yir::AssignOp>(result, to_bool(lower_expr(rhs_expr)));
        }
        return result;
    }

    yir::Value *lower_logical_or(Expr &lhs_expr, Expr &rhs_expr) {
        auto *false_value = emit<yir::ConstBoolOp>(false, fresh_temp());
        auto *result = emit<yir::VarOp>(yir::Type::get_i1(), false_value, fresh_name("logic"));

        yir::Value *lhs = to_bool(lower_expr(lhs_expr));
        auto *if_op = current_region_->append<yir::IfOp>(lhs);
        if_op->set_has_else(true);
        {
            RegionScope then_scope(*this, if_op->then_region());
            auto *true_value = emit<yir::ConstBoolOp>(true, fresh_temp());
            emit<yir::AssignOp>(result, true_value);
        }
        {
            RegionScope else_scope(*this, if_op->else_region());
            emit<yir::AssignOp>(result, to_bool(lower_expr(rhs_expr)));
        }
        return result;
    }

    yir::Value *to_bool(yir::Value *value) {
        if (value == nullptr || value->type()->kind() == yir::Type::Kind::I1) {
            return value;
        }
        return emit<yir::ToBoolOp>(value, fresh_temp());
    }

    yir::Value *to_i1(yir::Value *value) {
        if (!value || value->type() == yir::Type::get_i1())
            return value;
        return emit<yir::TruncI32ToI1Op>(value, fresh_temp());
    }

    bool operand_is_float(const yir::Value *value) const {
        if (!value || !value->type())
            return false;
        return value->type()->is_float() ||
               (value->type()->is_vector() && value->type()->element()->is_float());
    }

    void fail(std::string message) {
        if (errors_.empty())
            errors_.push_back(std::move(message));
    }

    yir::Value *lower_object_initializer(sema::SemanticTypeRef semantic_type, InitVal &init) {
        auto storage_type = type_for_semantic(semantic_type);
        if (init.expr) {
            return lower_expr(*init.expr);
        }
        if ((semantic_type->is_fixed_vector() || semantic_type->is_mask()) && !init.elems.empty()) {
            std::vector<yir::Value *> lanes;
            lanes.reserve(init.elems.size());
            for (auto &lane_initializer : init.elems) {
                if (!lane_initializer->expr) {
                    fail("FE_SEMANTIC_MODEL_INCONSISTENT: non-scalar vector lane initializer");
                    return nullptr;
                }
                auto *lane = lower_expr(*lane_initializer->expr);
                if (semantic_type->is_mask())
                    lane = to_i1(lane);
                lanes.push_back(lane);
            }
            return emit<yir::VectorCreateOp>(storage_type, std::move(lanes), fresh_temp());
        }
        if (!init.elems.empty() && init.elems.size() == 1)
            return lower_object_initializer(semantic_type, *init.elems.front());
        return emit<yir::ZeroOp>(storage_type, fresh_temp());
    }

    void emit_array_init(yir::Value *array, sema::SemanticTypeRef semantic_type, InitVal &init) {
        auto storage_type = type_for_semantic(semantic_type);
        std::vector<yir::ArrayInitEntry> entries;
        if (const auto *constant = model_.initializer_constant(init); constant && *constant) {
            std::vector<std::uint64_t> coordinates;
            flatten_constant_array(*constant, storage_type, coordinates, entries);
        } else if (array_leaf_semantic_type(semantic_type)->is_fixed_vector() ||
                   array_leaf_semantic_type(semantic_type)->is_mask()) {
            std::vector<std::uint64_t> coordinates;
            lower_vector_array_init(semantic_type, init, coordinates, entries);
        } else {
            entries = build_array_init_entries(storage_type, init);
        }
        current_region_->append<yir::ArrayInitOp>(array, storage_type, std::move(entries), true);
    }

    sema::SemanticTypeRef array_leaf_semantic_type(sema::SemanticTypeRef type) const {
        while (type && type->is_array())
            type = type->element_type();
        return type;
    }

    yir::ConstantPtr to_yir_constant(const sema::SemanticConstantRef &constant) {
        if (!constant)
            return nullptr;
        auto type = type_for_semantic(constant->type());
        switch (constant->kind()) {
        case sema::SemanticConstant::Kind::Integer:
            return std::make_shared<yir::ConstantInt>(type, constant->integer_value());
        case sema::SemanticConstant::Kind::Float:
            return std::make_shared<yir::ConstantFloat>(constant->float_value());
        case sema::SemanticConstant::Kind::AggregateZero:
            return std::make_shared<yir::ConstantAggregateZero>(type);
        case sema::SemanticConstant::Kind::Aggregate:
            break;
        }
        if (constant->type()->is_mask()) {
            std::vector<std::uint8_t> bytes(
                static_cast<std::size_t>((constant->type()->lane_count() + 7) / 8), 0);
            for (std::size_t lane = 0; lane < constant->elements().size(); ++lane) {
                const auto &element = constant->elements()[lane];
                if (element && element->kind() == sema::SemanticConstant::Kind::Integer &&
                    element->integer_value() != 0)
                    bytes[lane / 8] |= static_cast<std::uint8_t>(1U << (lane % 8));
            }
            return std::make_shared<yir::ConstantMask>(type, std::move(bytes));
        }
        std::vector<yir::ConstantPtr> elements;
        elements.reserve(constant->elements().size());
        for (const auto &element : constant->elements()) {
            auto lowered = to_yir_constant(element);
            if (!lowered)
                return nullptr;
            elements.push_back(std::move(lowered));
        }
        if (constant->type()->is_fixed_vector())
            return std::make_shared<yir::ConstantVector>(type, std::move(elements));
        if (constant->type()->is_array())
            return std::make_shared<yir::ConstantArray>(type, std::move(elements));
        return nullptr;
    }

    void flatten_constant_array(const sema::SemanticConstantRef &constant, const yir::TypePtr &type,
                                std::vector<std::uint64_t> &coordinates,
                                std::vector<yir::ArrayInitEntry> &entries) {
        if (!constant || constant->kind() == sema::SemanticConstant::Kind::AggregateZero)
            return;
        if (!type->is_array()) {
            auto value = to_yir_constant(constant);
            if (value && !value->is_zero()) {
                yir::ArrayInitEntry entry;
                entry.indices = coordinates;
                entry.constant = std::move(value);
                entries.push_back(std::move(entry));
            }
            return;
        }
        if (constant->kind() != sema::SemanticConstant::Kind::Aggregate)
            return;
        for (std::size_t i = 0; i < constant->elements().size(); ++i) {
            coordinates.push_back(static_cast<std::uint64_t>(i));
            flatten_constant_array(constant->elements()[i], type->element(), coordinates, entries);
            coordinates.pop_back();
        }
    }

    void lower_vector_array_init(sema::SemanticTypeRef type, InitVal &initializer,
                                 std::vector<std::uint64_t> &coordinates,
                                 std::vector<yir::ArrayInitEntry> &entries) {
        if (!type->is_array()) {
            auto *value = lower_object_initializer(type, initializer);
            yir::ArrayInitEntry entry;
            entry.indices = coordinates;
            entry.value = value;
            entries.push_back(std::move(entry));
            return;
        }
        for (std::size_t i = 0; i < initializer.elems.size(); ++i) {
            coordinates.push_back(static_cast<std::uint64_t>(i));
            lower_vector_array_init(type->element_type(), *initializer.elems[i], coordinates,
                                    entries);
            coordinates.pop_back();
        }
    }

    std::vector<yir::ArrayInitEntry> build_array_init_entries(yir::TypePtr storage_type,
                                                              InitVal &init) {
        if (!storage_type->is_array()) {
            return {};
        }

        std::vector<std::uint64_t> dimensions;
        yir::TypePtr element_type = collect_array_dimensions(storage_type, dimensions);
        std::uint64_t total_elements = 1;
        for (std::uint64_t dim : dimensions) {
            total_elements *= dim;
        }

        std::uint64_t cursor = 0;
        std::vector<yir::ArrayInitEntry> entries;
        lower_array_init_list(element_type, dimensions, init, 0, cursor, total_elements, entries);
        return entries;
    }

    yir::TypePtr collect_array_dimensions(yir::TypePtr type,
                                          std::vector<std::uint64_t> &dimensions) {
        while (type->is_array()) {
            dimensions.push_back(type->count());
            type = type->element();
        }
        return type;
    }

    void lower_array_init_list(yir::TypePtr element_type,
                               const std::vector<std::uint64_t> &dimensions, InitVal &init,
                               std::size_t level, std::uint64_t &cursor, std::uint64_t limit,
                               std::vector<yir::ArrayInitEntry> &entries) {
        if (cursor >= limit) {
            return;
        }
        if (init.expr) {
            append_array_init_entry(element_type, dimensions, cursor, init, entries);
            ++cursor;
            return;
        }
        for (auto &elem : init.elems) {
            if (cursor >= limit) {
                return;
            }
            if (elem->expr) {
                append_array_init_entry(element_type, dimensions, cursor, *elem, entries);
                ++cursor;
                continue;
            }

            std::uint64_t sub_size = 1;
            std::size_t sub_level =
                braced_subobject_level(dimensions, level, cursor, limit, sub_size);
            std::uint64_t sub_end = cursor + sub_size;
            lower_array_init_list(element_type, dimensions, *elem, sub_level, cursor, sub_end,
                                  entries);
            cursor = sub_end;
        }
    }

    std::size_t braced_subobject_level(const std::vector<std::uint64_t> &dimensions,
                                       std::size_t level, std::uint64_t cursor, std::uint64_t limit,
                                       std::uint64_t &sub_size) {
        for (std::size_t candidate = level + 1; candidate < dimensions.size(); ++candidate) {
            std::uint64_t size = suffix_element_count(dimensions, candidate);
            if (size != 0 && cursor % size == 0 && cursor + size <= limit) {
                sub_size = size;
                return candidate;
            }
        }
        sub_size = 1;
        return dimensions.size();
    }

    std::uint64_t suffix_element_count(const std::vector<std::uint64_t> &dimensions,
                                       std::size_t first_dimension) {
        std::uint64_t count = 1;
        for (std::size_t i = first_dimension; i < dimensions.size(); ++i) {
            count *= dimensions[i];
        }
        return count;
    }

    void append_array_init_entry(yir::TypePtr element_type,
                                 const std::vector<std::uint64_t> &dimensions,
                                 std::uint64_t flat_index, InitVal &init,
                                 std::vector<yir::ArrayInitEntry> &entries) {
        yir::ArrayInitEntry entry;
        entry.indices = coordinates_for_index(dimensions, flat_index);
        (void)element_type;
        entry.value = lower_expr(*init.expr);
        entries.push_back(std::move(entry));
    }

    std::vector<std::uint64_t> coordinates_for_index(const std::vector<std::uint64_t> &dimensions,
                                                     std::uint64_t flat_index) {
        std::vector<std::uint64_t> coordinates(dimensions.size(), 0);
        for (std::size_t i = dimensions.size(); i > 0; --i) {
            std::uint64_t dim = dimensions[i - 1];
            coordinates[i - 1] = dim == 0 ? 0 : flat_index % dim;
            flat_index = dim == 0 ? 0 : flat_index / dim;
        }
        return coordinates;
    }

    void lower_global_decl(DeclStmt &decl_stmt) {
        for (auto &decl : decl_stmt.decls) {
            auto *semantic_type = model_.declaration_type(*decl);
            if (!semantic_type) {
                fail("FE_SEMANTIC_MODEL_INCOMPLETE: missing global declaration type for '" +
                     decl->name + "'");
                continue;
            }
            yir::TypePtr storage_type = type_for_semantic(semantic_type);
            yir::Global *global = module_->add_global(decl->name, storage_type, decl->is_const);
            if (decl->init) {
                const auto *constant = model_.initializer_constant(*decl->init);
                if (!constant || !*constant) {
                    fail("FE_SEMANTIC_MODEL_INCOMPLETE: global initializer for '" + decl->name +
                         "' has no structured constant");
                } else {
                    global->set_initializer(to_yir_constant(*constant));
                }
            } else {
                global->set_initializer(std::make_shared<yir::ConstantAggregateZero>(storage_type));
            }
            define_variable(decl->name, global->address(), storage_type, decl->is_const,
                            storage_type->is_array(), true);
        }
    }

    yir::ICmpOp::Predicate to_icmp(BinaryOp op) {
        switch (op) {
        case BinaryOp::Eq:
            return yir::ICmpOp::Predicate::Eq;
        case BinaryOp::Ne:
            return yir::ICmpOp::Predicate::Ne;
        case BinaryOp::Lt:
            return yir::ICmpOp::Predicate::Lt;
        case BinaryOp::Le:
            return yir::ICmpOp::Predicate::Le;
        case BinaryOp::Gt:
            return yir::ICmpOp::Predicate::Gt;
        case BinaryOp::Ge:
            return yir::ICmpOp::Predicate::Ge;
        default:
            return yir::ICmpOp::Predicate::Eq;
        }
    }

    yir::FCmpOp::Predicate to_fcmp(BinaryOp op) {
        switch (op) {
        case BinaryOp::Eq:
            return yir::FCmpOp::Predicate::Eq;
        case BinaryOp::Ne:
            return yir::FCmpOp::Predicate::Ne;
        case BinaryOp::Lt:
            return yir::FCmpOp::Predicate::Lt;
        case BinaryOp::Le:
            return yir::FCmpOp::Predicate::Le;
        case BinaryOp::Gt:
            return yir::FCmpOp::Predicate::Gt;
        case BinaryOp::Ge:
            return yir::FCmpOp::Predicate::Ge;
        default:
            return yir::FCmpOp::Predicate::Eq;
        }
    }

    void enter_scope() {
        symbols_.enter_scope();
    }

    void leave_scope() {
        symbols_.leave_scope();
    }

    void define_variable(const std::string &name, yir::Value *value, yir::TypePtr type,
                         bool is_const, bool is_array, bool is_global) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Variable;
        symbol.name = name;
        symbol.value = value;
        symbol.type = std::move(type);
        symbol.is_const = is_const;
        symbol.is_array = is_array;
        symbol.is_global = is_global;
        if (!symbols_.define(std::move(symbol))) {
            errors_.push_back("redefinition of symbol: " + name);
        }
    }

    void define_function(const std::string &name, yir::Function *function, yir::TypePtr return_type,
                         std::vector<yir::TypePtr> param_types, bool is_builtin, bool is_variadic) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Function;
        symbol.name = name;
        symbol.type = yir::Type::get_func(param_types, return_type, is_variadic);
        symbol.function = function;
        symbol.return_type = std::move(return_type);
        symbol.param_types = std::move(param_types);
        symbol.is_builtin = is_builtin;
        symbol.is_variadic = is_variadic;
        if (!symbols_.define(std::move(symbol))) {
            errors_.push_back("redefinition of symbol: " + name);
        }
    }

    Symbol *lookup_variable(const std::string &name) {
        Symbol *symbol = symbols_.lookup(name);
        if (symbol == nullptr || symbol->kind != Symbol::Kind::Variable) {
            return nullptr;
        }
        return symbol;
    }

    Symbol *lookup_function(const std::string &name) {
        Symbol *symbol = symbols_.lookup(name);
        if (symbol == nullptr || symbol->kind != Symbol::Kind::Function) {
            return nullptr;
        }
        return symbol;
    }

    std::string fresh_temp() {
        return "v" + std::to_string(temp_counter_++);
    }

    std::string fresh_name(const std::string &base) {
        std::string name = sanitize_name(base);
        auto &count = name_counts_[name];
        if (count == 0) {
            ++count;
            return name;
        }
        return name + "." + std::to_string(count++);
    }

    const sema::SemanticModel &model_;
    std::unique_ptr<yir::Module> module_;
    yir::Function *current_function_ = nullptr;
    yir::Region *current_region_ = nullptr;
    yir::TypePtr current_return_type_;
    yir::Value *last_value_ = nullptr;
    int loop_depth_ = 0;
    unsigned temp_counter_ = 0;
    SymbolTable symbols_;
    std::unordered_map<std::string, unsigned> name_counts_;
    std::vector<std::string> errors_;
};

} // namespace

std::string_view ASTToYIRPass::name() const {
    return "ASTToYIRPass";
}

PassKind ASTToYIRPass::kind() const {
    return PassKind::Lowering;
}

PassResult ASTToYIRPass::run(PassContext &context) {
    if (!context.has_ast()) {
        return PassResult::fail("ASTToYIRPass requires AST in pass context");
    }

    auto *model_artifact = context.get_artifact<std::shared_ptr<sema::SemanticModel>>(
        ASTSemanticAnalysisPass::kArtifactKey);
    if (model_artifact == nullptr || *model_artifact == nullptr) {
        return PassResult::fail(
            "FE_SEMANTIC_MODEL_REQUIRED: ASTToYIRPass requires authoritative semantic model");
    }
    try {
        Lowerer lowerer(**model_artifact);
        auto module = lowerer.lower(*context.ast());
        auto verify = yir::verify_high_level_yir(*module);
        if (!verify.success) {
            return PassResult::fail(verify.errors.empty() ? "YIR verification failed"
                                                          : verify.errors.front());
        }
        context.set_artifact<std::unique_ptr<yir::Module>>(std::string(kArtifactKey),
                                                           std::move(module));
        return PassResult::ok(true);
    } catch (const std::exception &ex) {
        return PassResult::fail(ex.what());
    }
}

} // namespace pass
