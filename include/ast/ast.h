#pragma once

#include "front/SourceLocation.h"

#include <cassert>
#include <memory>
#include <string>
#include <vector>

class ASTVisitor;
class TypeSyntax;
using TypeSyntaxRef = std::shared_ptr<const TypeSyntax>;

// ============================================================================
// 基类
// ============================================================================
class ASTNode {
  public:
    front::SourceRange source_range;

    virtual ~ASTNode() = default;
    virtual void accept(ASTVisitor &visitor) = 0;
};

// ============================================================================
// 类型与算子枚举
// ============================================================================
enum class BuiltinType { Void, Int, Float };

enum class BinaryOp {
    Add,
    Sub,
    Mul,
    Div,
    Mod, // 算术
    MatMul, // tensor 矩阵乘法：a @ b
    Lt,
    Le,
    Gt,
    Ge,
    Eq,
    Ne, // 比较
    BitAnd,
    BitXor,
    BitOr,
    And,
    Or // 逻辑
};

enum class UnaryOp { Neg, Not, Pos, BitNot };

// ============================================================================
// 表达式
// ============================================================================
class Expr : public ASTNode {};

class IntLiteral : public Expr {
  public:
    int value;
    explicit IntLiteral(int v) : value(v) {
    }
    void accept(ASTVisitor &visitor) override;
};

class FloatLiteral : public Expr {
  public:
    float value;
    explicit FloatLiteral(float v) : value(v) {
    }
    void accept(ASTVisitor &visitor) override;
};

// 变量引用 / 数组下标访问
class LValExpr : public Expr {
  public:
    std::string name;
    std::vector<std::unique_ptr<Expr>> indices; // 空 = 标量

    explicit LValExpr(const std::string &n) : name(n) {
    }
    void accept(ASTVisitor &visitor) override;
};

class BinaryExpr : public Expr {
  public:
    BinaryOp op;
    std::unique_ptr<Expr> lhs;
    std::unique_ptr<Expr> rhs;

    BinaryExpr(BinaryOp op, std::unique_ptr<Expr> lhs, std::unique_ptr<Expr> rhs)
        : op(op), lhs(std::move(lhs)), rhs(std::move(rhs)) {
    }
    void accept(ASTVisitor &visitor) override;
};

class UnaryExpr : public Expr {
  public:
    UnaryOp op;
    std::unique_ptr<Expr> operand;

    UnaryExpr(UnaryOp op, std::unique_ptr<Expr> operand) : op(op), operand(std::move(operand)) {
    }
    void accept(ASTVisitor &visitor) override;
};

class CallExpr : public Expr {
  public:
    std::string func_name;
    std::vector<std::unique_ptr<Expr>> args;

    explicit CallExpr(const std::string &name) : func_name(name) {
    }
    void accept(ASTVisitor &visitor) override;
};

// An explicitly typed fixed-vector or mask literal.  The parsed type syntax is
// retained verbatim so semantic analysis can resolve the lane expression once
// and attach the canonical type to this expression.  An empty lane list is the
// source spelling for an aggregate zero value.
class TypedVectorLiteralExpr final : public Expr {
  public:
    TypeSyntaxRef type_syntax;
    std::vector<std::unique_ptr<Expr>> lanes;

    TypedVectorLiteralExpr(TypeSyntaxRef type, std::vector<std::unique_ptr<Expr>> values)
        : type_syntax(std::move(type)), lanes(std::move(values)) {
    }
    void accept(ASTVisitor &visitor) override;
};

// vector<T, N>(value) is an explicit constructor: a scalar value is splatted,
// while a same-width numeric vector is converted lane-wise.
class VectorCastExpr final : public Expr {
  public:
    TypeSyntaxRef target_type_syntax;
    std::unique_ptr<Expr> operand;

    VectorCastExpr(TypeSyntaxRef target, std::unique_ptr<Expr> value)
        : target_type_syntax(std::move(target)), operand(std::move(value)) {
    }
    void accept(ASTVisitor &visitor) override;
};

// A type-level constant expression owns an ordinary expression AST, but only
// exposes it through const access. Semantic analysis decides whether the tree
// is an integer constant expression and whether its value is a legal extent.
class ConstExpr final : public ASTNode {
  public:
    explicit ConstExpr(std::unique_ptr<Expr> expression,
                       front::SourceRange range = front::SourceRange{});

    const Expr &expression() const {
        assert(expression_ != nullptr);
        return *expression_;
    }

    void accept(ASTVisitor &visitor) override;

  private:
    std::unique_ptr<Expr> expression_;
};

using ConstExprRef = std::shared_ptr<const ConstExpr>;

// Parsed source types are immutable values shared by a declaration group and
// its declarators. Arrays remain declarator dimensions; this node represents
// their scalar, fixed-vector, or mask element type.
class TypeSyntax final {
  public:
    enum class Kind { Builtin, Vector, Mask, Tensor };

    static std::shared_ptr<const TypeSyntax>
    make_builtin(BuiltinType type, front::SourceRange range = front::SourceRange{});
    static std::shared_ptr<const TypeSyntax>
    make_vector(BuiltinType element_type, ConstExprRef lane_expression,
                front::SourceRange range = front::SourceRange{});
    static std::shared_ptr<const TypeSyntax>
    make_mask(ConstExprRef lane_expression, front::SourceRange range = front::SourceRange{});
    static std::shared_ptr<const TypeSyntax>
    make_tensor(BuiltinType element_type, front::SourceRange range = front::SourceRange{});

    Kind kind() const {
        return kind_;
    }

    BuiltinType builtin_type() const {
        assert(kind_ == Kind::Builtin);
        return scalar_type_;
    }

    BuiltinType vector_element_type() const {
        assert(kind_ == Kind::Vector);
        return scalar_type_;
    }

    // tensor 和 vector 一样只允许 int/float 元素；tensor 的形状在声明符 [] 中。
    BuiltinType tensor_element_type() const {
        assert(kind_ == Kind::Tensor);
        return scalar_type_;
    }

    const ConstExprRef &lane_expression() const {
        assert(kind_ == Kind::Vector || kind_ == Kind::Mask);
        return lane_expression_;
    }

    const front::SourceRange &source_range() const {
        return source_range_;
    }

    // Compatibility projection for scalar-only passes. Vector operations are
    // not lowered through this field; it only keeps legacy visitors buildable
    // until they consume SemanticModel.
    BuiltinType legacy_builtin_type() const;

  private:
    TypeSyntax(Kind kind, BuiltinType scalar_type, ConstExprRef lane_expression,
               front::SourceRange range);

    const Kind kind_;
    const BuiltinType scalar_type_;
    const ConstExprRef lane_expression_;
    const front::SourceRange source_range_;
};

// ============================================================================
// 初始化值 (对应 SysY 的 InitVal / ConstInitVal，可递归嵌套)
// 放在 Expr 之后，因为持有 unique_ptr<Expr>
// ============================================================================
class InitVal : public ASTNode {
  public:
    std::unique_ptr<Expr> expr;                  // 标量初始化
    std::vector<std::unique_ptr<InitVal>> elems; // 聚合初始化
    // expr==nullptr && elems.empty() → 零初始化

    void accept(ASTVisitor &visitor) override;
};

// ============================================================================
// 语句
// ============================================================================
class Stmt : public ASTNode {};

// 纯表达式语句 (expr;) 或空语句 (;)
class ExprStmt : public Stmt {
  public:
    std::unique_ptr<Expr> expr; // 可为 nullptr（空语句）
    explicit ExprStmt(std::unique_ptr<Expr> e = nullptr) : expr(std::move(e)) {
    }
    void accept(ASTVisitor &visitor) override;
};

// 赋值语句 (lval = expr;)
class AssignStmt : public Stmt {
  public:
    std::unique_ptr<LValExpr> target;
    std::unique_ptr<Expr> value;

    AssignStmt(std::unique_ptr<LValExpr> t, std::unique_ptr<Expr> v)
        : target(std::move(t)), value(std::move(v)) {
    }
    void accept(ASTVisitor &visitor) override;
};

// 块 { ... }
class BlockStmt : public Stmt {
  public:
    std::vector<std::unique_ptr<Stmt>> stmts; // 含 DeclStmt
    void accept(ASTVisitor &visitor) override;
};

class ReturnStmt : public Stmt {
  public:
    std::unique_ptr<Expr> expr; // nullptr = void return
    explicit ReturnStmt(std::unique_ptr<Expr> e = nullptr) : expr(std::move(e)) {
    }
    void accept(ASTVisitor &visitor) override;
};

class IfStmt : public Stmt {
  public:
    std::unique_ptr<Expr> cond;
    std::unique_ptr<Stmt> then_stmt;
    std::unique_ptr<Stmt> else_stmt; // 可为 nullptr

    IfStmt(std::unique_ptr<Expr> cond, std::unique_ptr<Stmt> then_s,
           std::unique_ptr<Stmt> else_s = nullptr)
        : cond(std::move(cond)), then_stmt(std::move(then_s)), else_stmt(std::move(else_s)) {
    }
    void accept(ASTVisitor &visitor) override;
};

class WhileStmt : public Stmt {
  public:
    std::unique_ptr<Expr> cond;
    std::unique_ptr<Stmt> body;

    WhileStmt(std::unique_ptr<Expr> cond, std::unique_ptr<Stmt> body)
        : cond(std::move(cond)), body(std::move(body)) {
    }
    void accept(ASTVisitor &visitor) override;
};

class BreakStmt : public Stmt {
  public:
    void accept(ASTVisitor &visitor) override;
};

class ContinueStmt : public Stmt {
  public:
    void accept(ASTVisitor &visitor) override;
};

// ============================================================================
// 声明
// ============================================================================

// 单个变量声明（标量或数组，有无初始化）
class VarDecl : public ASTNode {
  public:
    bool is_const;
    TypeSyntaxRef type_syntax;
    // Scalar compatibility for legacy semantic/lowering passes.
    BuiltinType base_type;
    std::string name;
    std::vector<std::unique_ptr<Expr>> dimensions; // 空 = 标量
    std::unique_ptr<InitVal> init;                 // 可为 nullptr

    VarDecl(bool is_const, BuiltinType ty, const std::string &n)
        : VarDecl(is_const, TypeSyntax::make_builtin(ty), n) {
    }
    VarDecl(bool is_const, TypeSyntaxRef type, const std::string &n)
        : is_const(is_const), type_syntax(std::move(type)),
          base_type(type_syntax->legacy_builtin_type()), name(n) {
    }
    void accept(ASTVisitor &visitor) override;
};

class DeclStmt : public Stmt {
  public:
    bool is_const;
    TypeSyntaxRef type_syntax;
    // Scalar compatibility for legacy semantic/lowering passes.
    BuiltinType base_type;
    std::vector<std::unique_ptr<VarDecl>> decls;

    DeclStmt(bool is_const, BuiltinType ty) : DeclStmt(is_const, TypeSyntax::make_builtin(ty)) {
    }
    DeclStmt(bool is_const, TypeSyntaxRef type)
        : is_const(is_const), type_syntax(std::move(type)),
          base_type(type_syntax->legacy_builtin_type()) {
    }
    void accept(ASTVisitor &visitor) override;
};

// ============================================================================
// 函数
// ============================================================================
struct FuncParam {
    TypeSyntaxRef type_syntax = TypeSyntax::make_builtin(BuiltinType::Int);
    // Scalar compatibility for legacy semantic/lowering passes.
    BuiltinType type = BuiltinType::Int;
    std::string name;
    std::vector<std::unique_ptr<Expr>> dimensions; // 数组参数有维度；首维隐式为 0
    front::SourceRange source_range;

    FuncParam() = default;
    FuncParam(TypeSyntaxRef syntax, std::string parameter_name)
        : type_syntax(std::move(syntax)), type(type_syntax->legacy_builtin_type()),
          name(std::move(parameter_name)) {
    }
};

class FuncDef : public ASTNode {
  public:
    TypeSyntaxRef return_type_syntax;
    // tensor int[2][3] f() 中的 [2][3]。普通返回类型保持为空。
    std::vector<std::unique_ptr<Expr>> return_dimensions;
    // Scalar compatibility for legacy semantic/lowering passes.
    BuiltinType return_type;
    std::string name;
    std::vector<FuncParam> params;
    std::unique_ptr<BlockStmt> body;
    // An external declaration has a signature but deliberately owns no body.
    // Variadic source functions are declaration-only; ordinary definitions
    // remain non-variadic in the current language.
    bool is_external = false;
    bool is_variadic = false;

    FuncDef(BuiltinType ret_ty, const std::string &n)
        : FuncDef(TypeSyntax::make_builtin(ret_ty), n) {
    }
    FuncDef(TypeSyntaxRef ret_ty, const std::string &n)
        : return_type_syntax(std::move(ret_ty)),
          return_type(return_type_syntax->legacy_builtin_type()), name(n) {
    }
    void accept(ASTVisitor &visitor) override;
};

// ============================================================================
// 编译单元 (根节点)
// ============================================================================
class CompUnit : public ASTNode {
  public:
    std::vector<std::unique_ptr<DeclStmt>> global_decls;
    std::vector<std::unique_ptr<FuncDef>> functions;
    void accept(ASTVisitor &visitor) override;
};

// ============================================================================
// Visitor 接口
// ============================================================================
class ASTVisitor {
  public:
    virtual ~ASTVisitor() = default;

    // Expr
    virtual void visit(IntLiteral &node) = 0;
    virtual void visit(FloatLiteral &node) = 0;
    virtual void visit(LValExpr &node) = 0;
    virtual void visit(BinaryExpr &node) = 0;
    virtual void visit(UnaryExpr &node) = 0;
    virtual void visit(CallExpr &node) = 0;
    virtual void visit(TypedVectorLiteralExpr &) {
    }
    virtual void visit(VectorCastExpr &) {
    }

    // New source-type-only node. Defaulting this hook keeps scalar-only
    // visitors source-compatible until they need to traverse type syntax.
    virtual void visit(ConstExpr &) {
    }

    // InitVal
    virtual void visit(InitVal &node) = 0;

    // Stmt
    virtual void visit(ExprStmt &node) = 0;
    virtual void visit(AssignStmt &node) = 0;
    virtual void visit(BlockStmt &node) = 0;
    virtual void visit(ReturnStmt &node) = 0;
    virtual void visit(IfStmt &node) = 0;
    virtual void visit(WhileStmt &node) = 0;
    virtual void visit(BreakStmt &node) = 0;
    virtual void visit(ContinueStmt &node) = 0;

    // Decl
    virtual void visit(VarDecl &node) = 0;
    virtual void visit(DeclStmt &node) = 0;
    virtual void visit(FuncDef &node) = 0;

    // Root
    virtual void visit(CompUnit &node) = 0;
};
