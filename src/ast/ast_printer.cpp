#include "../../include/ast/ast_printer.h"

#include <functional>
#include <sstream>
#include <utility>

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
    write_line("VarDecl name=" + node.name + " type=" + type_name(node.base_type) +
               " const=" + (node.is_const ? "true" : "false"));
    with_indent([&] {
        print_dimensions(node.dimensions);
        print_init("Init", node.init);
    });
}

void ASTPrinter::visit(DeclStmt &node) {
    write_line("DeclStmt type=" + type_name(node.base_type) +
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
    write_line("FuncDef name=" + node.name + " return=" + type_name(node.return_type));
    with_indent([&] {
        write_line("Params:");
        with_indent([&] {
            if (node.params.empty()) {
                write_line("<none>");
                return;
            }
            for (std::size_t i = 0; i < node.params.size(); ++i) {
                auto &param = node.params[i];
                write_line("Param[" + std::to_string(i) + "] name=" + param.name +
                           " type=" + type_name(param.type));
                with_indent([&] { print_dimensions(param.dimensions); });
            }
        });
        if (node.body) {
            write_line("Body:");
            with_indent([&] { node.body->accept(*this); });
        } else {
            write_line("Body: <null>");
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
