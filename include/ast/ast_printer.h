#pragma once

#include "ast.h"

#include <functional>
#include <iosfwd>
#include <string>

class ASTPrinter : public ASTVisitor {
  public:
    explicit ASTPrinter(std::ostream &out);

    void print(CompUnit &node);

    void visit(IntLiteral &node) override;
    void visit(FloatLiteral &node) override;
    void visit(LValExpr &node) override;
    void visit(BinaryExpr &node) override;
    void visit(UnaryExpr &node) override;
    void visit(CallExpr &node) override;

    void visit(InitVal &node) override;

    void visit(ExprStmt &node) override;
    void visit(AssignStmt &node) override;
    void visit(BlockStmt &node) override;
    void visit(ReturnStmt &node) override;
    void visit(IfStmt &node) override;
    void visit(WhileStmt &node) override;
    void visit(BreakStmt &node) override;
    void visit(ContinueStmt &node) override;

    void visit(VarDecl &node) override;
    void visit(DeclStmt &node) override;
    void visit(FuncDef &node) override;
    void visit(CompUnit &node) override;

  private:
    std::ostream &out_;
    int indent_ = 0;

    void write_indent();
    void write_line(const std::string &text);
    void with_indent(const std::function<void()> &fn);

    std::string type_name(BuiltinType type) const;
    std::string binary_op_name(BinaryOp op) const;
    std::string unary_op_name(UnaryOp op) const;

    void print_expr(const std::string &label, std::unique_ptr<Expr> &expr);
    void print_stmt(const std::string &label, std::unique_ptr<Stmt> &stmt);
    void print_init(const std::string &label, std::unique_ptr<InitVal> &init);
    void print_dimensions(const std::vector<std::unique_ptr<Expr>> &dimensions);
};

std::string print_ast_to_string(CompUnit &node);
