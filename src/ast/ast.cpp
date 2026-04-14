#include "../../include/ast/ast.h"

void IntLiteral::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void FloatLiteral::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void LValExpr::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void BinaryExpr::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void UnaryExpr::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void CallExpr::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void InitVal::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void ExprStmt::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void AssignStmt::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void BlockStmt::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void ReturnStmt::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void IfStmt::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void WhileStmt::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void BreakStmt::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void ContinueStmt::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void VarDecl::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void DeclStmt::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void FuncDef::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void CompUnit::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}
