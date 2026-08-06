#include "ast/ast.h"

#include <stdexcept>
#include <utility>

ConstExpr::ConstExpr(std::unique_ptr<Expr> expression, front::SourceRange range)
    : expression_(std::move(expression)) {
    if (expression_ == nullptr) {
        throw std::invalid_argument("constant expression AST cannot be null");
    }
    source_range = range;
}

TypeSyntax::TypeSyntax(Kind kind, BuiltinType scalar_type, ConstExprRef lane_expression,
                       front::SourceRange range)
    : kind_(kind), scalar_type_(scalar_type), lane_expression_(std::move(lane_expression)),
      source_range_(range) {
}

TypeSyntaxRef TypeSyntax::make_builtin(BuiltinType type, front::SourceRange range) {
    return TypeSyntaxRef(new TypeSyntax(Kind::Builtin, type, nullptr, range));
}

TypeSyntaxRef TypeSyntax::make_vector(BuiltinType element_type, ConstExprRef lane_expression,
                                      front::SourceRange range) {
    if (element_type != BuiltinType::Int && element_type != BuiltinType::Float) {
        throw std::invalid_argument("vector type syntax element must be int or float");
    }
    if (lane_expression == nullptr) {
        throw std::invalid_argument("vector type syntax requires a lane expression");
    }
    return TypeSyntaxRef(
        new TypeSyntax(Kind::Vector, element_type, std::move(lane_expression), range));
}

TypeSyntaxRef TypeSyntax::make_mask(ConstExprRef lane_expression, front::SourceRange range) {
    if (lane_expression == nullptr) {
        throw std::invalid_argument("mask type syntax requires a lane expression");
    }
    return TypeSyntaxRef(
        new TypeSyntax(Kind::Mask, BuiltinType::Int, std::move(lane_expression), range));
}

BuiltinType TypeSyntax::legacy_builtin_type() const {
    if (kind_ == Kind::Mask) {
        return BuiltinType::Int;
    }
    return scalar_type_;
}

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

void TypedVectorLiteralExpr::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void VectorCastExpr::accept(ASTVisitor &visitor) {
    visitor.visit(*this);
}

void ConstExpr::accept(ASTVisitor &visitor) {
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
