#include "../../include/pass/ASTSemanticAnalysisPass.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pass {
namespace {

struct SemanticType {
    BuiltinType base = BuiltinType::Void;
    unsigned rank = 0;
    bool valid = true;

    bool is_void() const {
        return base == BuiltinType::Void && rank == 0;
    }
};

struct SemanticSymbol {
    enum class Kind {
        Variable,
        Function,
    };

    Kind kind = Kind::Variable;
    BuiltinType type = BuiltinType::Void;
    unsigned rank = 0;
    bool is_const = false;
    bool variadic = false;
    std::vector<SemanticType> params;
};

class SemanticScopeStack {
  public:
    void enter() {
        scopes_.push_back({});
    }

    void leave() {
        scopes_.pop_back();
    }

    bool define(const std::string &name, SemanticSymbol symbol) {
        if (scopes_.empty()) {
            enter();
        }
        return scopes_.back().emplace(name, std::move(symbol)).second;
    }

    SemanticSymbol *lookup(const std::string &name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

  private:
    std::vector<std::unordered_map<std::string, SemanticSymbol>> scopes_;
};

class Analyzer final : public ASTVisitor {
  public:
    bool analyze(CompUnit &unit, std::string &message) {
        unit.accept(*this);
        if (errors_.empty()) {
            return true;
        }
        message = errors_.front();
        return false;
    }

    void visit(IntLiteral &) override {
        last_type_ = {BuiltinType::Int, 0, true};
    }

    void visit(FloatLiteral &) override {
        last_type_ = {BuiltinType::Float, 0, true};
    }

    void visit(LValExpr &node) override {
        auto *symbol = symbols_.lookup(node.name);
        if (symbol == nullptr || symbol->kind != SemanticSymbol::Kind::Variable) {
            error("unknown variable: " + node.name);
            last_type_ = invalid_type();
            return;
        }
        for (auto &index : node.indices) {
            auto type = expr_type(*index);
            if (type.is_void()) {
                error("array index cannot be void");
            }
        }
        if (node.indices.size() > symbol->rank) {
            error("too many indices for array: " + node.name);
            last_type_ = invalid_type();
            return;
        }
        last_type_ = {symbol->type, static_cast<unsigned>(symbol->rank - node.indices.size()),
                      true};
    }

    void visit(BinaryExpr &node) override {
        auto lhs = expr_type(*node.lhs);
        auto rhs = expr_type(*node.rhs);
        if (lhs.is_void() || rhs.is_void()) {
            error("binary expression cannot use void value");
            last_type_ = invalid_type();
            return;
        }
        switch (node.op) {
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge:
        case BinaryOp::Eq:
        case BinaryOp::Ne:
        case BinaryOp::And:
        case BinaryOp::Or:
            last_type_ = {BuiltinType::Int, 0, true};
            return;
        case BinaryOp::Mod:
            last_type_ = {BuiltinType::Int, 0, true};
            return;
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
            last_type_ = {lhs.base == BuiltinType::Float || rhs.base == BuiltinType::Float
                              ? BuiltinType::Float
                              : BuiltinType::Int,
                          0, true};
            return;
        }
    }

    void visit(UnaryExpr &node) override {
        auto value = expr_type(*node.operand);
        if (value.is_void()) {
            error("unary expression cannot use void value");
            last_type_ = invalid_type();
            return;
        }
        last_type_ = node.op == UnaryOp::Not ? SemanticType{BuiltinType::Int, 0, true} : value;
    }

    void visit(CallExpr &node) override {
        auto *symbol = symbols_.lookup(node.func_name);
        if (symbol == nullptr || symbol->kind != SemanticSymbol::Kind::Function) {
            error("unknown function: " + node.func_name);
            last_type_ = invalid_type();
            return;
        }
        if (!symbol->variadic && node.args.size() != symbol->params.size()) {
            error("argument count mismatch when calling: " + node.func_name);
        }
        for (auto &arg : node.args) {
            expr_type(*arg);
        }
        last_type_ = {symbol->type, 0, true};
    }

    void visit(InitVal &node) override {
        if (node.expr) {
            expr_type(*node.expr);
        }
        for (auto &elem : node.elems) {
            elem->accept(*this);
        }
    }

    void visit(ExprStmt &node) override {
        if (node.expr) {
            expr_type(*node.expr);
        }
    }

    void visit(AssignStmt &node) override {
        auto *symbol = symbols_.lookup(node.target->name);
        if (symbol == nullptr || symbol->kind != SemanticSymbol::Kind::Variable) {
            error("unknown variable: " + node.target->name);
        } else if (symbol->is_const) {
            error("cannot assign to const variable: " + node.target->name);
        }
        node.target->accept(*this);
        expr_type(*node.value);
    }

    void visit(BlockStmt &node) override {
        symbols_.enter();
        for (auto &stmt : node.stmts) {
            stmt->accept(*this);
        }
        symbols_.leave();
    }

    void visit(ReturnStmt &node) override {
        if (current_return_type_ == BuiltinType::Void) {
            if (node.expr) {
                error("void function cannot return a value");
                expr_type(*node.expr);
            }
            return;
        }
        if (!node.expr) {
            error("non-void function must return a value");
            return;
        }
        expr_type(*node.expr);
    }

    void visit(IfStmt &node) override {
        expr_type(*node.cond);
        node.then_stmt->accept(*this);
        if (node.else_stmt) {
            node.else_stmt->accept(*this);
        }
    }

    void visit(WhileStmt &node) override {
        expr_type(*node.cond);
        ++loop_depth_;
        node.body->accept(*this);
        --loop_depth_;
    }

    void visit(BreakStmt &) override {
        if (loop_depth_ == 0) {
            error("break outside of loop");
        }
    }

    void visit(ContinueStmt &) override {
        if (loop_depth_ == 0) {
            error("continue outside of loop");
        }
    }

    void visit(VarDecl &node) override {
        for (auto &dimension : node.dimensions) {
            auto type = expr_type(*dimension);
            if (type.is_void()) {
                error("array dimension cannot be void");
            }
        }
        SemanticSymbol symbol;
        symbol.kind = SemanticSymbol::Kind::Variable;
        symbol.type = node.base_type;
        symbol.rank = static_cast<unsigned>(node.dimensions.size());
        symbol.is_const = node.is_const;
        if (!symbols_.define(node.name, symbol)) {
            error("redefinition of symbol: " + node.name);
        }
        if (node.init) {
            node.init->accept(*this);
        }
    }

    void visit(DeclStmt &node) override {
        for (auto &decl : node.decls) {
            decl->is_const = node.is_const;
            decl->base_type = node.base_type;
            decl->accept(*this);
        }
    }

    void visit(FuncDef &node) override {
        auto previous_return = current_return_type_;
        current_return_type_ = node.return_type;
        symbols_.enter();
        for (const auto &param : node.params) {
            SemanticSymbol symbol;
            symbol.kind = SemanticSymbol::Kind::Variable;
            symbol.type = param.type;
            symbol.rank = static_cast<unsigned>(param.dimensions.size());
            if (!symbols_.define(param.name, symbol)) {
                error("redefinition of symbol: " + param.name);
            }
            for (const auto &dimension : param.dimensions) {
                if (dimension) {
                    expr_type(*dimension);
                }
            }
        }
        node.body->accept(*this);
        symbols_.leave();
        current_return_type_ = previous_return;
    }

    void visit(CompUnit &node) override {
        symbols_.enter();
        install_builtins();
        for (auto &func : node.functions) {
            declare_function(*func);
        }
        for (auto &decl : node.global_decls) {
            decl->accept(*this);
        }
        for (auto &func : node.functions) {
            func->accept(*this);
        }
        symbols_.leave();
    }

  private:
    static SemanticType invalid_type() {
        return {BuiltinType::Void, 0, false};
    }

    SemanticType expr_type(Expr &expr) {
        expr.accept(*this);
        return last_type_;
    }

    void declare_function(const FuncDef &func) {
        SemanticSymbol symbol;
        symbol.kind = SemanticSymbol::Kind::Function;
        symbol.type = func.return_type;
        for (const auto &param : func.params) {
            symbol.params.push_back(
                {param.type, static_cast<unsigned>(param.dimensions.size()), true});
        }
        if (!symbols_.define(func.name, std::move(symbol))) {
            error("redefinition of symbol: " + func.name);
        }
    }

    void define_builtin(std::string name, BuiltinType result, std::vector<SemanticType> params,
                        bool variadic = false) {
        SemanticSymbol symbol;
        symbol.kind = SemanticSymbol::Kind::Function;
        symbol.type = result;
        symbol.params = std::move(params);
        symbol.variadic = variadic;
        symbols_.define(name, std::move(symbol));
    }

    void install_builtins() {
        auto i32 = SemanticType{BuiltinType::Int, 0, true};
        auto f32 = SemanticType{BuiltinType::Float, 0, true};
        auto i32_ptr = SemanticType{BuiltinType::Int, 1, true};
        auto f32_ptr = SemanticType{BuiltinType::Float, 1, true};
        define_builtin("getint", BuiltinType::Int, {});
        define_builtin("getch", BuiltinType::Int, {});
        define_builtin("getfloat", BuiltinType::Float, {});
        define_builtin("getarray", BuiltinType::Int, {i32_ptr});
        define_builtin("getfarray", BuiltinType::Int, {f32_ptr});
        define_builtin("putint", BuiltinType::Void, {i32});
        define_builtin("putch", BuiltinType::Void, {i32});
        define_builtin("putarray", BuiltinType::Void, {i32, i32_ptr});
        define_builtin("putfloat", BuiltinType::Void, {f32});
        define_builtin("putfarray", BuiltinType::Void, {i32, f32_ptr});
        define_builtin("putf", BuiltinType::Void, {}, true);
        define_builtin("starttime", BuiltinType::Void, {});
        define_builtin("stoptime", BuiltinType::Void, {});
    }

    void error(std::string message) {
        if (errors_.empty()) {
            errors_.push_back(std::move(message));
        }
    }

    SemanticScopeStack symbols_;
    SemanticType last_type_;
    BuiltinType current_return_type_ = BuiltinType::Void;
    unsigned loop_depth_ = 0;
    std::vector<std::string> errors_;
};

} // namespace

std::string_view ASTSemanticAnalysisPass::name() const {
    return "ASTSemanticAnalysisPass";
}

PassKind ASTSemanticAnalysisPass::kind() const {
    return PassKind::Verification;
}

PassResult ASTSemanticAnalysisPass::run(PassContext &context) {
    if (!context.has_ast()) {
        return PassResult::fail("ASTSemanticAnalysisPass requires AST in pass context");
    }

    Analyzer analyzer;
    std::string message;
    if (!analyzer.analyze(*context.ast(), message)) {
        return PassResult::fail(message.empty() ? "semantic analysis failed" : message);
    }
    return PassResult::ok(false);
}

} // namespace pass
