#include "ast/ast_printer.h"

#include <functional>
#include <sstream>
#include <utility>

namespace {

const char *binary_op_symbol(BinaryOp op) {
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
    case BinaryOp::BitAnd:
        return "&";
    case BinaryOp::BitXor:
        return "^";
    case BinaryOp::BitOr:
        return "|";
    case BinaryOp::And:
        return "&&";
    case BinaryOp::Or:
        return "||";
    }
    return "?";
}

const char *unary_op_symbol(UnaryOp op) {
    switch (op) {
    case UnaryOp::Neg:
        return "-";
    case UnaryOp::Not:
        return "!";
    case UnaryOp::Pos:
        return "+";
    case UnaryOp::BitNot:
        return "~";
    }
    return "?";
}

} // namespace

ASTPrinter::ASTPrinter(std::ostream &out) : out_(out) {
}

void ASTPrinter::print(CompUnit &node) {
    node.accept(*this);
}

void ASTPrinter::write_indent() {
    for (int i = 0; i < indent_; ++i) {
        out_ << "  ";
    }
}

void ASTPrinter::write_line(const std::string &text) {
    write_indent();
    out_ << text << '\n';
}

void ASTPrinter::with_indent(const std::function<void()> &fn) {
    ++indent_;
    fn();
    --indent_;
}

std::string ASTPrinter::type_name(BuiltinType type) const {
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

std::string ASTPrinter::inline_expr_name(const Expr &expression) const {
    if (auto *literal = dynamic_cast<const IntLiteral *>(&expression)) {
        return std::to_string(literal->value);
    }
    if (auto *literal = dynamic_cast<const FloatLiteral *>(&expression)) {
        std::ostringstream out;
        out << literal->value;
        return out.str();
    }
    if (auto *lval = dynamic_cast<const LValExpr *>(&expression)) {
        std::string text = lval->name;
        for (const auto &index : lval->indices) {
            text += "[" + inline_expr_name(*index) + "]";
        }
        return text;
    }
    if (auto *binary = dynamic_cast<const BinaryExpr *>(&expression)) {
        return "(" + inline_expr_name(*binary->lhs) + " " + binary_op_symbol(binary->op) + " " +
               inline_expr_name(*binary->rhs) + ")";
    }
    if (auto *unary = dynamic_cast<const UnaryExpr *>(&expression)) {
        return std::string("(") + unary_op_symbol(unary->op) + inline_expr_name(*unary->operand) +
               ")";
    }
    if (auto *call = dynamic_cast<const CallExpr *>(&expression)) {
        std::string text = call->func_name + "(";
        for (std::size_t i = 0; i < call->args.size(); ++i) {
            if (i != 0) {
                text += ", ";
            }
            text += inline_expr_name(*call->args[i]);
        }
        return text + ")";
    }
    if (auto *literal = dynamic_cast<const TypedVectorLiteralExpr *>(&expression)) {
        std::string text = type_name(literal->type_syntax) + "{";
        for (std::size_t i = 0; i < literal->lanes.size(); ++i) {
            if (i != 0) {
                text += ", ";
            }
            text += inline_expr_name(*literal->lanes[i]);
        }
        return text + "}";
    }
    if (auto *cast = dynamic_cast<const VectorCastExpr *>(&expression)) {
        return type_name(cast->target_type_syntax) + "(" + inline_expr_name(*cast->operand) + ")";
    }
    return "<expr>";
}

std::string ASTPrinter::const_expr_name(const ConstExpr &expression) const {
    return inline_expr_name(expression.expression());
}

std::string ASTPrinter::type_name(const TypeSyntaxRef &type) const {
    if (type == nullptr) {
        return "<missing-type>";
    }
    switch (type->kind()) {
    case TypeSyntax::Kind::Builtin:
        return type_name(type->builtin_type());
    case TypeSyntax::Kind::Vector:
        return "vector<" + type_name(type->vector_element_type()) + ", " +
               const_expr_name(*type->lane_expression()) + ">";
    case TypeSyntax::Kind::Mask:
        return "mask<" + const_expr_name(*type->lane_expression()) + ">";
    }
    return "<missing-type>";
}

std::string ASTPrinter::binary_op_name(BinaryOp op) const {
    switch (op) {
    case BinaryOp::Add:
        return "Add";
    case BinaryOp::Sub:
        return "Sub";
    case BinaryOp::Mul:
        return "Mul";
    case BinaryOp::Div:
        return "Div";
    case BinaryOp::Mod:
        return "Mod";
    case BinaryOp::Lt:
        return "Lt";
    case BinaryOp::Le:
        return "Le";
    case BinaryOp::Gt:
        return "Gt";
    case BinaryOp::Ge:
        return "Ge";
    case BinaryOp::Eq:
        return "Eq";
    case BinaryOp::Ne:
        return "Ne";
    case BinaryOp::BitAnd:
        return "BitAnd";
    case BinaryOp::BitXor:
        return "BitXor";
    case BinaryOp::BitOr:
        return "BitOr";
    case BinaryOp::And:
        return "And";
    case BinaryOp::Or:
        return "Or";
    }
    return "Unknown";
}

std::string ASTPrinter::unary_op_name(UnaryOp op) const {
    switch (op) {
    case UnaryOp::Neg:
        return "Neg";
    case UnaryOp::Not:
        return "Not";
    case UnaryOp::Pos:
        return "Pos";
    case UnaryOp::BitNot:
        return "BitNot";
    }
    return "Unknown";
}

void ASTPrinter::print_expr(const std::string &label, std::unique_ptr<Expr> &expr) {
    if (!expr) {
        write_line(label + ": <null>");
        return;
    }
    write_line(label + ":");
    with_indent([&] { expr->accept(*this); });
}

void ASTPrinter::print_stmt(const std::string &label, std::unique_ptr<Stmt> &stmt) {
    if (!stmt) {
        write_line(label + ": <null>");
        return;
    }
    write_line(label + ":");
    with_indent([&] { stmt->accept(*this); });
}

void ASTPrinter::print_init(const std::string &label, std::unique_ptr<InitVal> &init) {
    if (!init) {
        write_line(label + ": <null>");
        return;
    }
    write_line(label + ":");
    with_indent([&] { init->accept(*this); });
}

void ASTPrinter::print_dimensions(const std::vector<std::unique_ptr<Expr>> &dimensions) {
    write_line("Dimensions:");
    with_indent([&] {
        if (dimensions.empty()) {
            write_line("<scalar>");
            return;
        }
        for (std::size_t i = 0; i < dimensions.size(); ++i) {
            if (!dimensions[i]) {
                write_line("[" + std::to_string(i) + "]: <unsized>");
                continue;
            }
            write_line("[" + std::to_string(i) + "]:");
            with_indent([&] { dimensions[i]->accept(*this); });
        }
    });
}

void ASTPrinter::visit(IntLiteral &node) {
    write_line("IntLiteral value=" + std::to_string(node.value));
}

void ASTPrinter::visit(FloatLiteral &node) {
    std::ostringstream oss;
    oss << "FloatLiteral value=" << node.value;
    write_line(oss.str());
}

void ASTPrinter::visit(LValExpr &node) {
    write_line("LValExpr name=" + node.name);
    with_indent([&] { print_dimensions(node.indices); });
}

void ASTPrinter::visit(BinaryExpr &node) {
    write_line("BinaryExpr op=" + binary_op_name(node.op));
    with_indent([&] {
        print_expr("LHS", node.lhs);
        print_expr("RHS", node.rhs);
    });
}

void ASTPrinter::visit(UnaryExpr &node) {
    write_line("UnaryExpr op=" + unary_op_name(node.op));
    with_indent([&] { print_expr("Operand", node.operand); });
}

void ASTPrinter::visit(CallExpr &node) {
    write_line("CallExpr name=" + node.func_name);
    with_indent([&] {
        write_line("Args:");
        with_indent([&] {
            if (node.args.empty()) {
                write_line("<none>");
                return;
            }
            for (std::size_t i = 0; i < node.args.size(); ++i) {
                write_line("[" + std::to_string(i) + "]:");
                with_indent([&] { node.args[i]->accept(*this); });
            }
        });
    });
}

void ASTPrinter::visit(TypedVectorLiteralExpr &node) {
    write_line("TypedVectorLiteralExpr type=" + type_name(node.type_syntax));
    with_indent([&] {
        write_line("Lanes:");
        with_indent([&] {
            if (node.lanes.empty()) {
                write_line("<zero>");
                return;
            }
            for (std::size_t i = 0; i < node.lanes.size(); ++i) {
                write_line("[" + std::to_string(i) + "]:");
                with_indent([&] { node.lanes[i]->accept(*this); });
            }
        });
    });
}

void ASTPrinter::visit(VectorCastExpr &node) {
    write_line("VectorCastExpr type=" + type_name(node.target_type_syntax));
    with_indent([&] { print_expr("Operand", node.operand); });
}

void ASTPrinter::visit(ConstExpr &node) {
    write_line("ConstExpr value=" + const_expr_name(node));
}

void ASTPrinter::visit(InitVal &node) {
    if (node.expr) {
        write_line("InitVal kind=expr");
        with_indent([&] { print_expr("Expr", node.expr); });
        return;
    }
    write_line("InitVal kind=list");
    with_indent([&] {
        if (node.elems.empty()) {
            write_line("<empty>");
            return;
        }
        for (std::size_t i = 0; i < node.elems.size(); ++i) {
            write_line("[" + std::to_string(i) + "]:");
            with_indent([&] { node.elems[i]->accept(*this); });
        }
    });
}

void ASTPrinter::visit(ExprStmt &node) {
    write_line("ExprStmt");
    with_indent([&] { print_expr("Expr", node.expr); });
}

void ASTPrinter::visit(AssignStmt &node) {
    write_line("AssignStmt");
    with_indent([&] {
        write_line("Target:");
        with_indent([&] { node.target->accept(*this); });
        print_expr("Value", node.value);
    });
}

void ASTPrinter::visit(BlockStmt &node) {
    write_line("BlockStmt");
    with_indent([&] {
        if (node.stmts.empty()) {
            write_line("<empty>");
            return;
        }
        for (std::size_t i = 0; i < node.stmts.size(); ++i) {
            write_line("Stmt[" + std::to_string(i) + "]:");
            with_indent([&] { node.stmts[i]->accept(*this); });
        }
    });
}

void ASTPrinter::visit(ReturnStmt &node) {
    write_line("ReturnStmt");
    with_indent([&] { print_expr("Expr", node.expr); });
}

void ASTPrinter::visit(IfStmt &node) {
    write_line("IfStmt");
    with_indent([&] {
        print_expr("Cond", node.cond);
        print_stmt("Then", node.then_stmt);
        print_stmt("Else", node.else_stmt);
    });
}

void ASTPrinter::visit(WhileStmt &node) {
    write_line("WhileStmt");
    with_indent([&] {
        print_expr("Cond", node.cond);
        print_stmt("Body", node.body);
    });
}

void ASTPrinter::visit(BreakStmt &) {
    write_line("BreakStmt");
}

void ASTPrinter::visit(ContinueStmt &) {
    write_line("ContinueStmt");
}

void ASTPrinter::visit(VarDecl &node) {
    write_line("VarDecl name=" + node.name + " type=" + type_name(node.type_syntax) +
               " const=" + (node.is_const ? "true" : "false"));
    with_indent([&] {
        print_dimensions(node.dimensions);
        print_init("Init", node.init);
    });
}

void ASTPrinter::visit(DeclStmt &node) {
    write_line("DeclStmt type=" + type_name(node.type_syntax) +
               " const=" + (node.is_const ? "true" : "false"));
    with_indent([&] {
        if (node.decls.empty()) {
            write_line("<empty>");
            return;
        }
        for (std::size_t i = 0; i < node.decls.size(); ++i) {
            write_line("Decl[" + std::to_string(i) + "]:");
            with_indent([&] { node.decls[i]->accept(*this); });
        }
    });
}

void ASTPrinter::visit(FuncDef &node) {
    if (node.is_external) {
        write_line("FuncDecl name=" + node.name + " return=" +
                   type_name(node.return_type_syntax) + " extern=true variadic=" +
                   (node.is_variadic ? "true" : "false"));
    } else {
        write_line("FuncDef name=" + node.name + " return=" +
                   type_name(node.return_type_syntax));
    }
    with_indent([&] {
        write_line("Params:");
        with_indent([&] {
            if (node.params.empty()) {
                write_line("<none>");
                return;
            }
            for (std::size_t i = 0; i < node.params.size(); ++i) {
                auto &param = node.params[i];
                write_line("Param[" + std::to_string(i) + "] name=" +
                           (param.name.empty() ? "<unnamed>" : param.name) +
                           " type=" + type_name(param.type_syntax));
                with_indent([&] { print_dimensions(param.dimensions); });
            }
        });
        if (node.body) {
            write_line("Body:");
            with_indent([&] { node.body->accept(*this); });
        } else {
            write_line(node.is_external ? "Body: <external>" : "Body: <null>");
        }
    });
}

void ASTPrinter::visit(CompUnit &node) {
    write_line("CompUnit");
    with_indent([&] {
        write_line("GlobalDecls:");
        with_indent([&] {
            if (node.global_decls.empty()) {
                write_line("<none>");
            } else {
                for (std::size_t i = 0; i < node.global_decls.size(); ++i) {
                    write_line("[" + std::to_string(i) + "]:");
                    with_indent([&] { node.global_decls[i]->accept(*this); });
                }
            }
        });
        write_line("Functions:");
        with_indent([&] {
            if (node.functions.empty()) {
                write_line("<none>");
            } else {
                for (std::size_t i = 0; i < node.functions.size(); ++i) {
                    write_line("[" + std::to_string(i) + "]:");
                    with_indent([&] { node.functions[i]->accept(*this); });
                }
            }
        });
    });
}

std::string print_ast_to_string(CompUnit &node) {
    std::ostringstream oss;
    ASTPrinter printer(oss);
    printer.print(node);
    return oss.str();
}
