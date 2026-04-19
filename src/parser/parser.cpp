#include "../../include/parser/parser.h"

int yylex();
void yyerror(std::unique_ptr<CompUnit> &ast, const char *s);

namespace {

struct Token {
    int kind = 0;
    std::string text;
    int int_val = 0;
    float float_val = 0.0f;
};

class Parser {
  public:
        explicit Parser(std::unique_ptr<CompUnit> &ast) : ast_(ast) {
        }

    std::unique_ptr<CompUnit> parseCompUnit() {
        auto unit = std::make_unique<CompUnit>();
        while (!check(0)) {
            if (check(CONST)) {
                auto decl = parseDeclStmt();
                if (!decl) {
                    return nullptr;
                }
                unit->global_decls.push_back(std::move(decl));
                continue;
            }

            if (isTypeToken(current().kind)) {
                std::size_t mark = position_;
                BuiltinType return_type = parseBType();
                std::string name;
                if (!consumeIdentifier(name)) {
                    return nullptr;
                }

                if (check('(')) {
                    auto func = parseFuncDef(return_type, std::move(name));
                    if (!func) {
                        return nullptr;
                    }
                    unit->functions.push_back(std::move(func));
                } else {
                    restore(mark);
                    auto decl = parseDeclStmt();
                    if (!decl) {
                        return nullptr;
                    }
                    unit->global_decls.push_back(std::move(decl));
                }
                continue;
            }

            syntaxError("expected declaration or function definition");
            return nullptr;
        }

        if (unit->global_decls.empty() && unit->functions.empty()) {
            syntaxError("expected declaration or function definition");
            return nullptr;
        }

        return unit;
    }

  private:
    std::unique_ptr<CompUnit> &ast_;
    std::vector<Token> tokens_;
    std::size_t position_ = 0;

    static bool isTypeToken(int kind) {
        return kind == INT || kind == FLOAT || kind == VOID;
    }

    void syntaxError(const std::string &message) {
        yyerror(ast_, message.c_str());
    }

    void ensure(std::size_t index) {
        while (tokens_.size() <= index) {
            Token token;
            token.kind = yylex();
            if (token.kind == IDENT) {
                token.text = *yylval.str_val;
                delete yylval.str_val;
            } else if (token.kind == INT_CONST) {
                token.int_val = yylval.int_val;
            } else if (token.kind == FLOAT_CONST) {
                token.float_val = yylval.float_val;
            }
            tokens_.push_back(std::move(token));
            if (tokens_.back().kind == 0) {
                break;
            }
        }
    }

    const Token &current() {
        ensure(position_);
        return tokens_[position_];
    }

    const Token &lookahead(std::size_t offset) {
        ensure(position_ + offset);
        return tokens_[position_ + offset];
    }

    bool check(int kind) {
        return current().kind == kind;
    }

    bool match(int kind) {
        if (!check(kind)) {
            return false;
        }
        ++position_;
        return true;
    }

    void restore(std::size_t mark) {
        position_ = mark;
    }

    bool consumeIdentifier(std::string &name) {
        if (!check(IDENT)) {
            syntaxError("expected identifier");
            return false;
        }
        name = current().text;
        ++position_;
        return true;
    }

    bool expect(int kind, const char *what) {
        if (!check(kind)) {
            syntaxError(std::string("expected ") + what);
            return false;
        }
        ++position_;
        return true;
    }

    BuiltinType parseBType() {
        if (match(INT)) {
            return BuiltinType::Int;
        }
        if (match(FLOAT)) {
            return BuiltinType::Float;
        }
        if (match(VOID)) {
            return BuiltinType::Void;
        }
        syntaxError("expected type");
        return BuiltinType::Int;
    }

    std::unique_ptr<DeclStmt> parseDeclStmt() {
        bool is_const = false;
        if (match(CONST)) {
            is_const = true;
        }

        BuiltinType base_type = parseBType();
        auto decl = std::make_unique<DeclStmt>(is_const, base_type);

        while (true) {
            std::string name;
            if (!consumeIdentifier(name)) {
                return nullptr;
            }

            auto item = std::make_unique<VarDecl>(is_const, base_type, name);
            while (match('[')) {
                auto dim = parseExpr();
                if (!dim || !expect(']', "']'")) {
                    return nullptr;
                }
                item->dimensions.push_back(std::move(dim));
            }

            if (match('=')) {
                item->init = parseInitVal();
                if (!item->init) {
                    return nullptr;
                }
            }

            decl->decls.push_back(std::move(item));
            if (!match(',')) {
                break;
            }
        }

        if (!expect(';', "';'")) {
            return nullptr;
        }
        return decl;
    }

    std::unique_ptr<FuncDef> parseFuncDef(BuiltinType return_type, std::string name) {
        auto func = std::make_unique<FuncDef>(return_type, name);
        if (!expect('(', "'('")) {
            return nullptr;
        }

        if (!check(')')) {
            auto params = parseFuncParamList();
            if (params.empty() && !check(')')) {
                return nullptr;
            }
            func->params = std::move(params);
        }

        if (!expect(')', "')'")) {
            return nullptr;
        }

        func->body = parseBlockStmt();
        if (!func->body) {
            return nullptr;
        }
        return func;
    }

    std::vector<FuncParam> parseFuncParamList() {
        std::vector<FuncParam> params;
        auto param = parseFuncParam();
        if (!param) {
            return {};
        }
        params.push_back(std::move(*param));

        while (match(',')) {
            param = parseFuncParam();
            if (!param) {
                return {};
            }
            params.push_back(std::move(*param));
        }
        return params;
    }

    std::unique_ptr<FuncParam> parseFuncParam() {
        BuiltinType type = parseBType();
        std::string name;
        if (!consumeIdentifier(name)) {
            return nullptr;
        }

        auto param = std::make_unique<FuncParam>();
        param->type = type;
        param->name = std::move(name);

        while (match('[')) {
            if (match(']')) {
                param->dimensions.push_back(nullptr);
            } else {
                auto dim = parseExpr();
                if (!dim || !expect(']', "']'")) {
                    return nullptr;
                }
                param->dimensions.push_back(std::move(dim));
            }
        }

        return param;
    }

    std::unique_ptr<BlockStmt> parseBlockStmt() {
        if (!expect('{', "'{'")) {
            return nullptr;
        }

        auto block = std::make_unique<BlockStmt>();
        while (!check('}')) {
            if (check(0)) {
                syntaxError("expected '}'");
                return nullptr;
            }
            auto stmt = parseStmt();
            if (!stmt) {
                return nullptr;
            }
            block->stmts.push_back(std::move(stmt));
        }

        if (!expect('}', "'}'")) {
            return nullptr;
        }
        return block;
    }

    std::unique_ptr<Stmt> parseStmt() {
        if (check('{')) {
            return parseBlockStmt();
        }

        if (check(CONST) || isTypeToken(current().kind)) {
            return parseDeclStmt();
        }

        if (match(IF)) {
            if (!expect('(', "'('")) {
                return nullptr;
            }
            auto cond = parseExpr();
            if (!cond || !expect(')', "')'")) {
                return nullptr;
            }
            auto then_stmt = parseStmt();
            if (!then_stmt) {
                return nullptr;
            }
            std::unique_ptr<Stmt> else_stmt;
            if (match(ELSE)) {
                else_stmt = parseStmt();
                if (!else_stmt) {
                    return nullptr;
                }
            }
            return std::make_unique<IfStmt>(std::move(cond), std::move(then_stmt), std::move(else_stmt));
        }

        if (match(WHILE)) {
            if (!expect('(', "'('")) {
                return nullptr;
            }
            auto cond = parseExpr();
            if (!cond || !expect(')', "')'")) {
                return nullptr;
            }
            auto body = parseStmt();
            if (!body) {
                return nullptr;
            }
            return std::make_unique<WhileStmt>(std::move(cond), std::move(body));
        }

        if (match(RETURN)) {
            if (match(';')) {
                return std::make_unique<ReturnStmt>();
            }
            auto expr = parseExpr();
            if (!expr || !expect(';', "';'")) {
                return nullptr;
            }
            return std::make_unique<ReturnStmt>(std::move(expr));
        }

        if (match(BREAK)) {
            if (!expect(';', "';'")) {
                return nullptr;
            }
            return std::make_unique<BreakStmt>();
        }

        if (match(CONTINUE)) {
            if (!expect(';', "';'")) {
                return nullptr;
            }
            return std::make_unique<ContinueStmt>();
        }

        if (match(';')) {
            return std::make_unique<ExprStmt>();
        }

        std::size_t mark = position_;
        if (check(IDENT)) {
            std::string name = current().text;
            ++position_;
            auto lval = parseLValTail(std::move(name));
            if (lval && match('=')) {
                auto value = parseExpr();
                if (!value || !expect(';', "';'")) {
                    return nullptr;
                }
                return std::make_unique<AssignStmt>(std::move(lval), std::move(value));
            }
            restore(mark);
        }

        auto expr = parseExpr();
        if (!expr || !expect(';', "';'")) {
            return nullptr;
        }
        return std::make_unique<ExprStmt>(std::move(expr));
    }

    std::unique_ptr<InitVal> parseInitVal() {
        if (match('{')) {
            auto init = std::make_unique<InitVal>();
            if (!check('}')) {
                init->elems = parseInitValList();
                if (init->elems.empty() && !check('}')) {
                    return nullptr;
                }
            }
            if (!expect('}', "'}'")) {
                return nullptr;
            }
            return init;
        }

        auto init = std::make_unique<InitVal>();
        init->expr = parseExpr();
        if (!init->expr) {
            return nullptr;
        }
        return init;
    }

    std::vector<std::unique_ptr<InitVal>> parseInitValList() {
        std::vector<std::unique_ptr<InitVal>> values;
        auto value = parseInitVal();
        if (!value) {
            return {};
        }
        values.push_back(std::move(value));
        while (match(',')) {
            value = parseInitVal();
            if (!value) {
                return {};
            }
            values.push_back(std::move(value));
        }
        return values;
    }

    std::vector<std::unique_ptr<Expr>> parseExprList() {
        std::vector<std::unique_ptr<Expr>> args;
        auto expr = parseExpr();
        if (!expr) {
            return {};
        }
        args.push_back(std::move(expr));
        while (match(',')) {
            expr = parseExpr();
            if (!expr) {
                return {};
            }
            args.push_back(std::move(expr));
        }
        return args;
    }

    std::unique_ptr<LValExpr> parseLValTail(std::string name) {
        auto lval = std::make_unique<LValExpr>(name);
        while (match('[')) {
            auto index = parseExpr();
            if (!index || !expect(']', "']'")) {
                return nullptr;
            }
            lval->indices.push_back(std::move(index));
        }
        return lval;
    }

    std::unique_ptr<Expr> parseExpr() {
        return parseLOrExp();
    }

    std::unique_ptr<Expr> parseLOrExp() {
        auto expr = parseLAndExp();
        if (!expr) {
            return nullptr;
        }
        while (match(LOR)) {
            auto rhs = parseLAndExp();
            if (!rhs) {
                return nullptr;
            }
            expr = std::make_unique<BinaryExpr>(BinaryOp::Or, std::move(expr), std::move(rhs));
        }
        return expr;
    }

    std::unique_ptr<Expr> parseLAndExp() {
        auto expr = parseEqExp();
        if (!expr) {
            return nullptr;
        }
        while (match(LAND)) {
            auto rhs = parseEqExp();
            if (!rhs) {
                return nullptr;
            }
            expr = std::make_unique<BinaryExpr>(BinaryOp::And, std::move(expr), std::move(rhs));
        }
        return expr;
    }

    std::unique_ptr<Expr> parseEqExp() {
        auto expr = parseRelExp();
        if (!expr) {
            return nullptr;
        }
        while (check(EQ) || check(NE)) {
            int op = current().kind;
            ++position_;
            auto rhs = parseRelExp();
            if (!rhs) {
                return nullptr;
            }
            expr = std::make_unique<BinaryExpr>(op == EQ ? BinaryOp::Eq : BinaryOp::Ne,
                                                std::move(expr), std::move(rhs));
        }
        return expr;
    }

    std::unique_ptr<Expr> parseRelExp() {
        auto expr = parseAddExp();
        if (!expr) {
            return nullptr;
        }
        while (check('<') || check('>') || check(LE) || check(GE)) {
            int op = current().kind;
            ++position_;
            auto rhs = parseAddExp();
            if (!rhs) {
                return nullptr;
            }
            BinaryOp binary_op = BinaryOp::Lt;
            if (op == '<') {
                binary_op = BinaryOp::Lt;
            } else if (op == '>') {
                binary_op = BinaryOp::Gt;
            } else if (op == LE) {
                binary_op = BinaryOp::Le;
            } else {
                binary_op = BinaryOp::Ge;
            }
            expr = std::make_unique<BinaryExpr>(binary_op, std::move(expr), std::move(rhs));
        }
        return expr;
    }

    std::unique_ptr<Expr> parseAddExp() {
        auto expr = parseMulExp();
        if (!expr) {
            return nullptr;
        }
        while (check('+') || check('-')) {
            int op = current().kind;
            ++position_;
            auto rhs = parseMulExp();
            if (!rhs) {
                return nullptr;
            }
            expr = std::make_unique<BinaryExpr>(op == '+' ? BinaryOp::Add : BinaryOp::Sub,
                                                std::move(expr), std::move(rhs));
        }
        return expr;
    }

    std::unique_ptr<Expr> parseMulExp() {
        auto expr = parseUnaryExp();
        if (!expr) {
            return nullptr;
        }
        while (check('*') || check('/') || check('%')) {
            int op = current().kind;
            ++position_;
            auto rhs = parseUnaryExp();
            if (!rhs) {
                return nullptr;
            }
            BinaryOp binary_op = BinaryOp::Mul;
            if (op == '*') {
                binary_op = BinaryOp::Mul;
            } else if (op == '/') {
                binary_op = BinaryOp::Div;
            } else {
                binary_op = BinaryOp::Mod;
            }
            expr = std::make_unique<BinaryExpr>(binary_op, std::move(expr), std::move(rhs));
        }
        return expr;
    }

    std::unique_ptr<Expr> parseUnaryExp() {
        if (match('+')) {
            auto operand = parseUnaryExp();
            if (!operand) {
                return nullptr;
            }
            return std::make_unique<UnaryExpr>(UnaryOp::Pos, std::move(operand));
        }
        if (match('-')) {
            auto operand = parseUnaryExp();
            if (!operand) {
                return nullptr;
            }
            return std::make_unique<UnaryExpr>(UnaryOp::Neg, std::move(operand));
        }
        if (match('!')) {
            auto operand = parseUnaryExp();
            if (!operand) {
                return nullptr;
            }
            return std::make_unique<UnaryExpr>(UnaryOp::Not, std::move(operand));
        }

        if (match('(')) {
            auto expr = parseExpr();
            if (!expr || !expect(')', "')'")) {
                return nullptr;
            }
            return expr;
        }

        if (check(INT_CONST)) {
            int value = current().int_val;
            ++position_;
            return std::make_unique<IntLiteral>(value);
        }

        if (check(FLOAT_CONST)) {
            float value = current().float_val;
            ++position_;
            return std::make_unique<FloatLiteral>(value);
        }

        if (check(IDENT)) {
            std::string name = current().text;
            ++position_;
            if (match('(')) {
                auto call = std::make_unique<CallExpr>(name);
                if (!check(')')) {
                    auto args = parseExprList();
                    if (args.empty() && !check(')')) {
                        return nullptr;
                    }
                    call->args = std::move(args);
                }
                if (!expect(')', "')'")) {
                    return nullptr;
                }
                return call;
            }
            return parseLValTail(std::move(name));
        }

        syntaxError("expected expression");
        return nullptr;
    }
};

} // namespace

YYSTYPE yylval;

int yyparse(std::unique_ptr<CompUnit> &ast) {
    Parser parser(ast);
    auto result = parser.parseCompUnit();
    if (!result) {
        return 1;
    }
    ast = std::move(result);
    return 0;
}

void yyerror(std::unique_ptr<CompUnit> &ast, const char *s) {
    (void)ast;
    std::cerr << "error: " << s << std::endl;
}