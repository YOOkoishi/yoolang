#pragma once

#include <cassert>
#include <memory>
#include <string>
#include <vector>

class ASTVisitor;

// ============================================================================
// 基类
// ============================================================================
class ASTNode {
  public:
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
    Lt,
    Le,
    Gt,
    Ge,
    Eq,
    Ne, // 比较
    And,
    Or // 逻辑
};

enum class UnaryOp { Neg, Not, Pos };

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
    BuiltinType base_type;
    std::string name;
    std::vector<std::unique_ptr<Expr>> dimensions; // 空 = 标量
    std::unique_ptr<InitVal> init;                 // 可为 nullptr

    VarDecl(bool is_const, BuiltinType ty, const std::string &n)
        : is_const(is_const), base_type(ty), name(n) {
    }
    void accept(ASTVisitor &visitor) override;
};

class DeclStmt : public Stmt {
  public:
    bool is_const;
    BuiltinType base_type;
    std::vector<std::unique_ptr<VarDecl>> decls;

    DeclStmt(bool is_const, BuiltinType ty) : is_const(is_const), base_type(ty) {
    }
    void accept(ASTVisitor &visitor) override;
};

// ============================================================================
// 函数
// ============================================================================
struct FuncParam {
    BuiltinType type;
    std::string name;
    std::vector<std::unique_ptr<Expr>> dimensions; // 数组参数有维度；首维隐式为 0
};

class FuncDef : public ASTNode {
  public:
    BuiltinType return_type;
    std::string name;
    std::vector<FuncParam> params;
    std::unique_ptr<BlockStmt> body;

    FuncDef(BuiltinType ret_ty, const std::string &n) : return_type(ret_ty), name(n) {
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
