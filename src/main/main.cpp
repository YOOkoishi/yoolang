#include "../../include/include.h"
#include "../../include/ast/ast.h"

extern FILE *lexer_input;
int parse(std::unique_ptr<CompUnit> &ast);

namespace {

std::string builtin_type_to_string(BuiltinType type) {
    switch (type) {
    case BuiltinType::Void:
        return "void";
    case BuiltinType::Int:
        return "int";
    case BuiltinType::Float:
        return "float";
    }
    return "unknown";
}

std::string binary_op_to_string(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add:
        return "+";
    case BinaryOp::Sub:
        return "-";
    case BinaryOp::Mul:
        return "*";
    case BinaryOp::Div:
        return "/";
    case BinaryOp::Mod:
        return "%";
    case BinaryOp::Lt:
        return "<";
    case BinaryOp::Le:
        return "<=";
    case BinaryOp::Gt:
        return ">";
    case BinaryOp::Ge:
        return ">=";
    case BinaryOp::Eq:
        return "==";
    case BinaryOp::Ne:
        return "!=";
    case BinaryOp::And:
        return "&&";
    case BinaryOp::Or:
        return "||";
    }
    return "?";
}

std::string unary_op_to_string(UnaryOp op) {
    switch (op) {
    case UnaryOp::Neg:
        return "-";
    case UnaryOp::Not:
        return "!";
    case UnaryOp::Pos:
        return "+";
    }
    return "?";
}

void indent(std::ostream &os, int depth) {
    for (int i = 0; i < depth; ++i) {
        os << "  ";
    }
}

void print_expr(const Expr &node, std::ostream &os, int depth);
void print_stmt(const Stmt &node, std::ostream &os, int depth);
void print_initval(const InitVal &node, std::ostream &os, int depth);

void print_expr(const Expr &node, std::ostream &os, int depth) {
    if (auto literal = dynamic_cast<const IntLiteral *>(&node)) {
        indent(os, depth);
        os << "IntLiteral(" << literal->value << ")\n";
        return;
    }
    if (auto literal = dynamic_cast<const FloatLiteral *>(&node)) {
        indent(os, depth);
        os << "FloatLiteral(" << literal->value << ")\n";
        return;
    }
    if (auto lval = dynamic_cast<const LValExpr *>(&node)) {
        indent(os, depth);
        os << "LValExpr(" << lval->name << ")\n";
        for (const auto &index : lval->indices) {
            indent(os, depth + 1);
            os << "index\n";
            if (index) {
                print_expr(*index, os, depth + 2);
            }
        }
        return;
    }
    if (auto binary = dynamic_cast<const BinaryExpr *>(&node)) {
        indent(os, depth);
        os << "BinaryExpr(" << binary_op_to_string(binary->op) << ")\n";
        indent(os, depth + 1);
        os << "lhs\n";
        print_expr(*binary->lhs, os, depth + 2);
        indent(os, depth + 1);
        os << "rhs\n";
        print_expr(*binary->rhs, os, depth + 2);
        return;
    }
    if (auto unary = dynamic_cast<const UnaryExpr *>(&node)) {
        indent(os, depth);
        os << "UnaryExpr(" << unary_op_to_string(unary->op) << ")\n";
        indent(os, depth + 1);
        os << "operand\n";
        print_expr(*unary->operand, os, depth + 2);
        return;
    }
    if (auto call = dynamic_cast<const CallExpr *>(&node)) {
        indent(os, depth);
        os << "CallExpr(" << call->func_name << ")\n";
        for (const auto &arg : call->args) {
            indent(os, depth + 1);
            os << "arg\n";
            print_expr(*arg, os, depth + 2);
        }
        return;
    }
    indent(os, depth);
    os << "<unknown expr>\n";
}

void print_initval(const InitVal &node, std::ostream &os, int depth) {
    indent(os, depth);
    os << "InitVal\n";
    if (node.expr) {
        indent(os, depth + 1);
        os << "expr\n";
        print_expr(*node.expr, os, depth + 2);
        return;
    }
    for (const auto &elem : node.elems) {
        indent(os, depth + 1);
        os << "elem\n";
        print_initval(*elem, os, depth + 2);
    }
}

void print_var_decl(const VarDecl &node, std::ostream &os, int depth) {
    indent(os, depth);
    os << (node.is_const ? "ConstDecl" : "VarDecl") << "(" << builtin_type_to_string(node.base_type)
       << ", " << node.name << ")\n";
    for (const auto &dim : node.dimensions) {
        indent(os, depth + 1);
        os << "dim\n";
        print_expr(*dim, os, depth + 2);
    }
    if (node.init) {
        indent(os, depth + 1);
        os << "init\n";
        print_initval(*node.init, os, depth + 2);
    }
}

void print_stmt(const Stmt &node, std::ostream &os, int depth) {
    if (auto decl = dynamic_cast<const DeclStmt *>(&node)) {
        indent(os, depth);
        os << (decl->is_const ? "DeclStmt(const " : "DeclStmt(") << builtin_type_to_string(decl->base_type)
           << ")\n";
        for (const auto &item : decl->decls) {
            print_var_decl(*item, os, depth + 1);
        }
        return;
    }
    if (auto assign = dynamic_cast<const AssignStmt *>(&node)) {
        indent(os, depth);
        os << "AssignStmt\n";
        indent(os, depth + 1);
        os << "target\n";
        print_expr(*assign->target, os, depth + 2);
        indent(os, depth + 1);
        os << "value\n";
        print_expr(*assign->value, os, depth + 2);
        return;
    }
    if (auto expr_stmt = dynamic_cast<const ExprStmt *>(&node)) {
        indent(os, depth);
        os << "ExprStmt\n";
        if (expr_stmt->expr) {
            print_expr(*expr_stmt->expr, os, depth + 1);
        }
        return;
    }
    if (auto block = dynamic_cast<const BlockStmt *>(&node)) {
        indent(os, depth);
        os << "BlockStmt\n";
        for (const auto &stmt : block->stmts) {
            print_stmt(*stmt, os, depth + 1);
        }
        return;
    }
    if (auto ret = dynamic_cast<const ReturnStmt *>(&node)) {
        indent(os, depth);
        os << "ReturnStmt\n";
        if (ret->expr) {
            print_expr(*ret->expr, os, depth + 1);
        }
        return;
    }
    if (auto if_stmt = dynamic_cast<const IfStmt *>(&node)) {
        indent(os, depth);
        os << "IfStmt\n";
        indent(os, depth + 1);
        os << "cond\n";
        print_expr(*if_stmt->cond, os, depth + 2);
        indent(os, depth + 1);
        os << "then\n";
        print_stmt(*if_stmt->then_stmt, os, depth + 2);
        if (if_stmt->else_stmt) {
            indent(os, depth + 1);
            os << "else\n";
            print_stmt(*if_stmt->else_stmt, os, depth + 2);
        }
        return;
    }
    if (auto while_stmt = dynamic_cast<const WhileStmt *>(&node)) {
        indent(os, depth);
        os << "WhileStmt\n";
        indent(os, depth + 1);
        os << "cond\n";
        print_expr(*while_stmt->cond, os, depth + 2);
        indent(os, depth + 1);
        os << "body\n";
        print_stmt(*while_stmt->body, os, depth + 2);
        return;
    }
    if (dynamic_cast<const BreakStmt *>(&node)) {
        indent(os, depth);
        os << "BreakStmt\n";
        return;
    }
    if (dynamic_cast<const ContinueStmt *>(&node)) {
        indent(os, depth);
        os << "ContinueStmt\n";
        return;
    }

    indent(os, depth);
    os << "<unknown stmt>\n";
}

void print_comp_unit(const CompUnit &node, std::ostream &os) {
    os << "CompUnit\n";
    for (const auto &decl : node.global_decls) {
        print_stmt(*decl, os, 1);
    }
    for (const auto &func : node.functions) {
        indent(os, 1);
        os << "FuncDef(" << builtin_type_to_string(func->return_type) << ", " << func->name << ")\n";
        for (const auto &param : func->params) {
            indent(os, 2);
            os << "Param(" << builtin_type_to_string(param.type) << ", " << param.name << ")\n";
            for (const auto &dim : param.dimensions) {
                if (dim) {
                    print_expr(*dim, os, 3);
                } else {
                    indent(os, 3);
                    os << "ParamDim([])\n";
                }
            }
        }
        if (func->body) {
            print_stmt(*func->body, os, 2);
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <source.sy>\n";
        return 1;
    }

    FILE *input = fopen(argv[1], "r");
    if (input == nullptr) {
        std::perror("fopen");
        return 1;
    }

    lexer_input = input;
    std::unique_ptr<CompUnit> ast;
    if (parse(ast) != 0 || !ast) {
        std::cerr << "parse failed\n";
        fclose(input);
        return 1;
    }

    print_comp_unit(*ast, std::cout);
    fclose(input);
    return 0;
}
