#include "../../include/IRGen/IRGen.h"
#include "../../include/include.h"

#include <iostream>

int main(int argc, char **argv) {
    // 简单 smoke test: 手工构造 AST 做 lowering 验证
    // int add(int a, int b) { return a + b; }
    // int main() { return add(3, 4); }
    using namespace irgen;

    CompUnit unit;

    {
        auto func = std::make_unique<FuncDef>(BuiltinType::Int, "add");
        func->params.push_back({BuiltinType::Int, "a", {}});
        func->params.push_back({BuiltinType::Int, "b", {}});

        auto lhs = std::make_unique<LValExpr>("a");
        auto rhs = std::make_unique<LValExpr>("b");
        auto add_expr = std::make_unique<BinaryExpr>(BinaryOp::Add, std::move(lhs), std::move(rhs));
        auto ret_stmt = std::make_unique<ReturnStmt>(std::move(add_expr));

        auto body = std::make_unique<BlockStmt>();
        body->stmts.push_back(std::move(ret_stmt));
        func->body = std::move(body);

        unit.functions.push_back(std::move(func));
    }

    {
        auto func = std::make_unique<FuncDef>(BuiltinType::Int, "main");

        std::vector<std::unique_ptr<Expr>> call_args;
        call_args.push_back(std::make_unique<IntLiteral>(3));
        call_args.push_back(std::make_unique<IntLiteral>(4));
        auto call_expr = std::make_unique<CallExpr>("add");
        call_expr->args = std::move(call_args);

        auto ret_stmt = std::make_unique<ReturnStmt>(std::move(call_expr));
        auto body = std::make_unique<BlockStmt>();
        body->stmts.push_back(std::move(ret_stmt));
        func->body = std::move(body);

        unit.functions.push_back(std::move(func));
    }

    ASTToIRLowering lowering;
    auto module = lowering.lower(unit, "smoke_test");

    std::string verify_msg;
    if (!module->verify(&verify_msg)) {
        std::cerr << "SSA verify FAILED: " << verify_msg << std::endl;
        return 1;
    }

    std::cout << "SSA verify OK" << std::endl;
    std::cout << module->print() << std::endl;

    return 0;
}
