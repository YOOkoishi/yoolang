#include "front/parser.h"

#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Token {
    int kind = TOK_EOF;
    std::string text;
    std::string spelling;
    int int_value = 0;
    float float_value = 0.0F;
    front::SourceRange range;
};

front::SourceRange span(front::SourceRange begin, front::SourceRange end) {
    if (!begin.valid()) {
        return end;
    }
    if (!end.valid() || begin.begin.file_id != end.end.file_id) {
        return begin;
    }
    return front::SourceRange(begin.begin, end.end);
}

class Parser final {
  public:
    explicit Parser(front::DiagnosticEngine &diagnostics) : diagnostics_(diagnostics) {
    }

    std::unique_ptr<CompUnit> parse_compilation_unit() {
        auto unit = std::make_unique<CompUnit>();
        const auto begin = current().range;

        while (!check(TOK_EOF)) {
            const std::size_t before = position_;
            if (check(TOK_EXTERN)) {
                auto function = parse_external_function();
                if (function != nullptr) {
                    unit->functions.push_back(std::move(function));
                } else {
                    synchronize_declaration();
                }
            } else if (check(TOK_CONST)) {
                auto declaration = parse_declaration();
                if (declaration != nullptr) {
                    unit->global_decls.push_back(std::move(declaration));
                } else {
                    synchronize_declaration();
                }
            } else if (check(TOK_VOID)) {
                auto return_type = parse_function_type();
                std::string name;
                front::SourceRange name_range;
                if (return_type != nullptr && consume_identifier(name, &name_range) && check('(')) {
                    auto function = parse_function(std::move(return_type), std::move(name), false);
                    if (function != nullptr) {
                        unit->functions.push_back(std::move(function));
                    } else {
                        synchronize_declaration();
                    }
                } else {
                    syntax_error(front::DiagnosticCode::ParseUnexpectedToken,
                                 "void is only permitted as a function return type");
                    synchronize_declaration();
                }
            } else if (is_basic_type_start(current().kind)) {
                const std::size_t mark = position_;
                auto possible_return_type = parse_basic_type();
                std::string name;
                front::SourceRange name_range;
                if (possible_return_type == nullptr || !consume_identifier(name, &name_range)) {
                    synchronize_declaration();
                } else if (check('(')) {
                    auto function = parse_function(std::move(possible_return_type),
                                                   std::move(name), false);
                    if (function != nullptr) {
                        unit->functions.push_back(std::move(function));
                    } else {
                        synchronize_declaration();
                    }
                } else {
                    restore(mark);
                    auto declaration = parse_declaration();
                    if (declaration != nullptr) {
                        unit->global_decls.push_back(std::move(declaration));
                    } else {
                        synchronize_declaration();
                    }
                }
            } else {
                syntax_error(front::DiagnosticCode::ParseUnexpectedToken,
                             "expected declaration or function definition");
                advance_token();
                synchronize_declaration();
            }

            if (position_ == before && !check(TOK_EOF)) {
                advance_token();
            }
        }

        if (unit->global_decls.empty() && unit->functions.empty() && !has_error_) {
            syntax_error(front::DiagnosticCode::ParseUnexpectedToken,
                         "expected declaration or function definition");
        }
        unit->source_range = span(begin, current().range);
        return unit;
    }

    bool has_error() const {
        return has_error_;
    }

  private:
    front::DiagnosticEngine &diagnostics_;
    std::vector<Token> tokens_;
    std::size_t position_ = 0;
    bool has_error_ = false;
    bool stop_at_type_greater_ = false;

    static bool is_basic_type_start(int kind) {
        return kind == TOK_INT || kind == TOK_FLOAT || kind == TOK_VECTOR || kind == TOK_MASK ||
               kind == TOK_TENSOR;
    }

    static bool is_type_start(int kind) {
        return kind == TOK_VOID || is_basic_type_start(kind);
    }

    void syntax_error(front::DiagnosticCode code, const std::string &message) {
        diagnostics_.error(code, current().range, message);
        has_error_ = true;
    }

    void ensure(std::size_t index) {
        while (tokens_.size() <= index) {
            if (!tokens_.empty() && tokens_.back().kind == TOK_EOF) {
                tokens_.push_back(tokens_.back());
                continue;
            }
            Token token;
            token.kind = nextToken();
            token.text = token_value.str_val;
            token.spelling = token_value.spelling;
            token.int_value = token_value.int_val;
            token.float_value = token_value.float_val;
            token.range = token_value.range;
            tokens_.push_back(std::move(token));
        }
    }

    const Token &current() {
        ensure(position_);
        return tokens_[position_];
    }

    const Token &previous() {
        ensure(position_ == 0 ? 0 : position_ - 1);
        return tokens_[position_ == 0 ? 0 : position_ - 1];
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

    void advance_token() {
        if (!check(TOK_EOF)) {
            ++position_;
        }
    }

    void restore(std::size_t mark) {
        position_ = mark;
    }

    bool consume_identifier(std::string &name, front::SourceRange *range = nullptr) {
        if (!check(TOK_IDENT)) {
            syntax_error(front::DiagnosticCode::ParseExpectedToken, "expected identifier");
            return false;
        }
        name = current().text;
        if (range != nullptr) {
            *range = current().range;
        }
        ++position_;
        return true;
    }

    bool expect(int kind, const char *what) {
        if (!check(kind)) {
            syntax_error(front::DiagnosticCode::ParseExpectedToken,
                         std::string("expected ") + what);
            return false;
        }
        ++position_;
        return true;
    }

    void synchronize_declaration() {
        while (!check(TOK_EOF) && !check('}')) {
            if (match(';')) {
                return;
            }
            if (check(TOK_EXTERN) || check(TOK_CONST) || is_type_start(current().kind)) {
                return;
            }
            advance_token();
        }
    }

    void synchronize_statement() {
        while (!check(TOK_EOF) && !check('}')) {
            if (match(';')) {
                return;
            }
            if (check('{') || check(TOK_IF) || check(TOK_WHILE) || check(TOK_RETURN) ||
                check(TOK_BREAK) || check(TOK_CONTINUE) || check(TOK_CONST) ||
                is_type_start(current().kind)) {
                return;
            }
            advance_token();
        }
    }

    TypeSyntaxRef parse_basic_type() {
        const auto begin = current().range;
        if (match(TOK_INT)) {
            return TypeSyntax::make_builtin(BuiltinType::Int, begin);
        }
        if (match(TOK_FLOAT)) {
            return TypeSyntax::make_builtin(BuiltinType::Float, begin);
        }
        if (match(TOK_VECTOR)) {
            if (!expect('<', "'<' after vector")) {
                return nullptr;
            }
            BuiltinType element_type = BuiltinType::Int;
            if (match(TOK_INT)) {
                element_type = BuiltinType::Int;
            } else if (match(TOK_FLOAT)) {
                element_type = BuiltinType::Float;
            } else {
                syntax_error(front::DiagnosticCode::ParseExpectedToken,
                             "expected int or float vector element type");
                return nullptr;
            }
            if (!expect(',', "',' after vector element type")) {
                return nullptr;
            }
            auto lane_expression = parse_type_lane_expression();
            if (lane_expression == nullptr || !expect('>', "'>' after vector lane expression")) {
                return nullptr;
            }
            return TypeSyntax::make_vector(element_type, std::move(lane_expression),
                                           span(begin, previous().range));
        }
        if (match(TOK_MASK)) {
            if (!expect('<', "'<' after mask")) {
                return nullptr;
            }
            auto lane_expression = parse_type_lane_expression();
            if (lane_expression == nullptr || !expect('>', "'>' after mask lane expression")) {
                return nullptr;
            }
            return TypeSyntax::make_mask(std::move(lane_expression), span(begin, previous().range));
        }
        if (match(TOK_TENSOR)) {
            BuiltinType element_type = BuiltinType::Int;
            if (match(TOK_INT)) {
                element_type = BuiltinType::Int;
            } else if (match(TOK_FLOAT)) {
                element_type = BuiltinType::Float;
            } else {
                syntax_error(front::DiagnosticCode::ParseExpectedToken,
                             "expected int or float after tensor");
                return nullptr;
            }
            return TypeSyntax::make_tensor(element_type, span(begin, previous().range));
        }
        syntax_error(front::DiagnosticCode::ParseExpectedToken,
                     "expected int, float, vector, mask, or tensor type");
        return nullptr;
    }

    TypeSyntaxRef parse_function_type() {
        if (check(TOK_VOID)) {
            const auto range = current().range;
            ++position_;
            return TypeSyntax::make_builtin(BuiltinType::Void, range);
        }
        return parse_basic_type();
    }

    ConstExprRef parse_type_lane_expression() {
        const bool saved_stop = stop_at_type_greater_;
        stop_at_type_greater_ = true;
        auto expression = parse_expression();
        stop_at_type_greater_ = saved_stop;
        if (expression == nullptr) {
            return nullptr;
        }
        const auto range = expression->source_range;
        return std::make_shared<ConstExpr>(std::move(expression), range);
    }

    std::unique_ptr<DeclStmt> parse_declaration() {
        const auto begin = current().range;
        const bool is_const = match(TOK_CONST);
        auto type = parse_basic_type();
        if (type == nullptr) {
            return nullptr;
        }
        auto declaration = std::make_unique<DeclStmt>(is_const, type);

        while (true) {
            std::string name;
            front::SourceRange name_range;
            if (!consume_identifier(name, &name_range)) {
                return nullptr;
            }

            auto item = std::make_unique<VarDecl>(is_const, type, name);
            front::SourceRange item_end = name_range;
            while (match('[')) {
                auto dimension = parse_expression();
                if (dimension == nullptr || !expect(']', "']'")) {
                    return nullptr;
                }
                item_end = previous().range;
                item->dimensions.push_back(std::move(dimension));
            }

            if (match('=')) {
                item->init = parse_initializer();
                if (item->init == nullptr) {
                    return nullptr;
                }
                item_end = item->init->source_range;
            }
            item->source_range = span(name_range, item_end);
            declaration->decls.push_back(std::move(item));
            if (!match(',')) {
                break;
            }
        }

        if (!expect(';', "';'")) {
            return nullptr;
        }
        declaration->source_range = span(begin, previous().range);
        return declaration;
    }

    std::unique_ptr<FuncDef> parse_external_function() {
        const auto begin = current().range;
        if (!expect(TOK_EXTERN, "'extern'")) {
            return nullptr;
        }
        if (!is_type_start(current().kind)) {
            syntax_error(front::DiagnosticCode::ParseExpectedToken,
                         "expected function return type after extern");
            return nullptr;
        }
        auto return_type = parse_function_type();
        std::string name;
        front::SourceRange name_range;
        if (return_type == nullptr || !consume_identifier(name, &name_range)) {
            return nullptr;
        }
        if (!check('(')) {
            syntax_error(front::DiagnosticCode::ParseUnexpectedToken,
                         "extern is only permitted on a function declaration");
            return nullptr;
        }
        auto function = parse_function(std::move(return_type), std::move(name), true);
        if (function != nullptr) {
            function->source_range = span(begin, function->source_range);
        }
        return function;
    }

    std::unique_ptr<FuncDef> parse_function(TypeSyntaxRef return_type, std::string name,
                                            bool is_external) {
        auto function = std::make_unique<FuncDef>(return_type, name);
        function->is_external = is_external;
        if (!expect('(', "'('")) {
            return nullptr;
        }

        if (!check(')')) {
            if (match(TOK_VOID)) {
                if (!check(')')) {
                    syntax_error(front::DiagnosticCode::ParseUnexpectedToken,
                                 "void must be the only item in a parameter list");
                    return nullptr;
                }
            } else {
                bool is_variadic = false;
                auto parameters = parse_parameter_list(is_external, is_variadic);
                if (parameters.empty() && !is_variadic && !check(')')) {
                    return nullptr;
                }
                function->params = std::move(parameters);
                function->is_variadic = is_variadic;
            }
        }
        if (!expect(')', "')'")) {
            return nullptr;
        }

        if (function->is_variadic && !is_external) {
            syntax_error(front::DiagnosticCode::ParseUnexpectedToken,
                         "variadic functions require an extern declaration");
            return nullptr;
        }
        if (is_external) {
            if (!expect(';', "';' after external function declaration")) {
                return nullptr;
            }
            function->source_range = span(return_type->source_range(), previous().range);
            return function;
        }

        function->body = parse_block_statement();
        if (function->body == nullptr) {
            return nullptr;
        }
        function->source_range = span(return_type->source_range(), function->body->source_range);
        return function;
    }

    std::vector<FuncParam> parse_parameter_list(bool allow_unnamed, bool &is_variadic) {
        std::vector<FuncParam> parameters;
        if (match(TOK_ELLIPSIS)) {
            is_variadic = true;
            return parameters;
        }

        auto parameter = parse_parameter(allow_unnamed);
        if (parameter == nullptr) {
            return {};
        }
        parameters.push_back(std::move(*parameter));

        while (match(',')) {
            if (match(TOK_ELLIPSIS)) {
                is_variadic = true;
                if (!check(')')) {
                    syntax_error(front::DiagnosticCode::ParseUnexpectedToken,
                                 "ellipsis must be the final item in a parameter list");
                }
                break;
            }
            parameter = parse_parameter(allow_unnamed);
            if (parameter == nullptr) {
                return {};
            }
            parameters.push_back(std::move(*parameter));
        }
        return parameters;
    }

    std::unique_ptr<FuncParam> parse_parameter(bool allow_unnamed) {
        auto type = parse_basic_type();
        if (type == nullptr) {
            return nullptr;
        }
        std::string name;
        front::SourceRange name_range = type->source_range();
        if (check(TOK_IDENT)) {
            if (!consume_identifier(name, &name_range)) {
                return nullptr;
            }
        } else if (!allow_unnamed) {
            syntax_error(front::DiagnosticCode::ParseExpectedToken,
                         "function definitions require named parameters");
            return nullptr;
        }

        auto parameter = std::make_unique<FuncParam>(type, std::move(name));
        front::SourceRange end = name_range;
        while (match('[')) {
            if (match(']')) {
                parameter->dimensions.push_back(nullptr);
                end = previous().range;
                continue;
            }
            auto dimension = parse_expression();
            if (dimension == nullptr || !expect(']', "']'")) {
                return nullptr;
            }
            parameter->dimensions.push_back(std::move(dimension));
            end = previous().range;
        }
        parameter->source_range = span(type->source_range(), end);
        return parameter;
    }

    std::unique_ptr<BlockStmt> parse_block_statement() {
        const auto begin = current().range;
        if (!expect('{', "'{'")) {
            return nullptr;
        }

        auto block = std::make_unique<BlockStmt>();
        while (!check('}') && !check(TOK_EOF)) {
            const std::size_t before = position_;
            auto statement = parse_statement();
            if (statement != nullptr) {
                block->stmts.push_back(std::move(statement));
            } else {
                synchronize_statement();
            }
            if (position_ == before && !check('}') && !check(TOK_EOF)) {
                advance_token();
            }
        }

        if (!expect('}', "'}'")) {
            block->source_range = span(begin, current().range);
            return block;
        }
        block->source_range = span(begin, previous().range);
        return block;
    }

    std::unique_ptr<Stmt> parse_statement() {
        if (check('{')) {
            return parse_block_statement();
        }
        if (check(TOK_CONST) || check(TOK_INT) || check(TOK_FLOAT) || check(TOK_VOID)) {
            return parse_declaration();
        }
        if (check(TOK_VECTOR) || check(TOK_MASK)) {
            // A fixed type followed by an identifier starts a declaration;
            // followed by '{' or '(' it is a typed expression constructor.
            const std::size_t mark = position_;
            auto type = parse_basic_type();
            const bool declaration = type != nullptr && check(TOK_IDENT);
            restore(mark);
            if (declaration) {
                return parse_declaration();
            }
        }
        if (check(TOK_TENSOR)) {
            return parse_declaration();
        }

        if (check(TOK_IF)) {
            const auto begin = current().range;
            ++position_;
            if (!expect('(', "'('")) {
                return nullptr;
            }
            auto condition = parse_expression();
            if (condition == nullptr || !expect(')', "')'")) {
                return nullptr;
            }
            auto then_statement = parse_statement();
            if (then_statement == nullptr) {
                return nullptr;
            }
            std::unique_ptr<Stmt> else_statement;
            if (match(TOK_ELSE)) {
                else_statement = parse_statement();
                if (else_statement == nullptr) {
                    return nullptr;
                }
            }
            auto statement = std::make_unique<IfStmt>(
                std::move(condition), std::move(then_statement), std::move(else_statement));
            statement->source_range =
                span(begin, statement->else_stmt != nullptr ? statement->else_stmt->source_range
                                                            : statement->then_stmt->source_range);
            return statement;
        }

        if (check(TOK_WHILE)) {
            const auto begin = current().range;
            ++position_;
            if (!expect('(', "'('")) {
                return nullptr;
            }
            auto condition = parse_expression();
            if (condition == nullptr || !expect(')', "')'")) {
                return nullptr;
            }
            auto body = parse_statement();
            if (body == nullptr) {
                return nullptr;
            }
            auto statement = std::make_unique<WhileStmt>(std::move(condition), std::move(body));
            statement->source_range = span(begin, statement->body->source_range);
            return statement;
        }

        if (check(TOK_RETURN)) {
            const auto begin = current().range;
            ++position_;
            if (match(';')) {
                auto statement = std::make_unique<ReturnStmt>();
                statement->source_range = span(begin, previous().range);
                return statement;
            }
            auto expression = parse_expression();
            if (expression == nullptr || !expect(';', "';'")) {
                return nullptr;
            }
            auto statement = std::make_unique<ReturnStmt>(std::move(expression));
            statement->source_range = span(begin, previous().range);
            return statement;
        }

        if (check(TOK_BREAK) || check(TOK_CONTINUE)) {
            const bool is_break = check(TOK_BREAK);
            const auto begin = current().range;
            ++position_;
            if (!expect(';', "';'")) {
                return nullptr;
            }
            std::unique_ptr<Stmt> statement = is_break ? std::unique_ptr<Stmt>(new BreakStmt())
                                                       : std::unique_ptr<Stmt>(new ContinueStmt());
            statement->source_range = span(begin, previous().range);
            return statement;
        }

        if (match(';')) {
            auto statement = std::make_unique<ExprStmt>();
            statement->source_range = previous().range;
            return statement;
        }

        const std::size_t mark = position_;
        if (check(TOK_IDENT)) {
            std::string name = current().text;
            const auto name_range = current().range;
            ++position_;
            auto lvalue = parse_lvalue_tail(std::move(name), name_range);
            if (lvalue != nullptr && match('=')) {
                const auto begin = lvalue->source_range;
                auto value = parse_expression();
                if (value == nullptr || !expect(';', "';'")) {
                    return nullptr;
                }
                auto statement = std::make_unique<AssignStmt>(std::move(lvalue), std::move(value));
                statement->source_range = span(begin, previous().range);
                return statement;
            }
            restore(mark);
        }

        auto expression = parse_expression();
        if (expression == nullptr || !expect(';', "';'")) {
            return nullptr;
        }
        const auto begin = expression->source_range;
        auto statement = std::make_unique<ExprStmt>(std::move(expression));
        statement->source_range = span(begin, previous().range);
        return statement;
    }

    std::unique_ptr<InitVal> parse_initializer() {
        if (check('{')) {
            const auto begin = current().range;
            ++position_;
            auto initializer = std::make_unique<InitVal>();
            if (!check('}')) {
                initializer->elems = parse_initializer_list();
                if (initializer->elems.empty() && !check('}')) {
                    return nullptr;
                }
            }
            if (!expect('}', "'}'")) {
                return nullptr;
            }
            initializer->source_range = span(begin, previous().range);
            return initializer;
        }

        auto initializer = std::make_unique<InitVal>();
        initializer->expr = parse_expression();
        if (initializer->expr == nullptr) {
            return nullptr;
        }
        initializer->source_range = initializer->expr->source_range;
        return initializer;
    }

    std::vector<std::unique_ptr<InitVal>> parse_initializer_list() {
        std::vector<std::unique_ptr<InitVal>> values;
        auto value = parse_initializer();
        if (value == nullptr) {
            return {};
        }
        values.push_back(std::move(value));
        while (match(',')) {
            value = parse_initializer();
            if (value == nullptr) {
                return {};
            }
            values.push_back(std::move(value));
        }
        return values;
    }

    std::vector<std::unique_ptr<Expr>> parse_expression_list() {
        std::vector<std::unique_ptr<Expr>> arguments;
        auto expression = parse_expression();
        if (expression == nullptr) {
            return {};
        }
        arguments.push_back(std::move(expression));
        while (match(',')) {
            expression = parse_expression();
            if (expression == nullptr) {
                return {};
            }
            arguments.push_back(std::move(expression));
        }
        return arguments;
    }

    std::unique_ptr<LValExpr> parse_lvalue_tail(std::string name, front::SourceRange name_range) {
        auto lvalue = std::make_unique<LValExpr>(name);
        lvalue->source_range = name_range;
        while (match('[')) {
            const bool saved_stop = stop_at_type_greater_;
            stop_at_type_greater_ = false;
            auto index = parse_expression();
            stop_at_type_greater_ = saved_stop;
            if (index == nullptr || !expect(']', "']'")) {
                return nullptr;
            }
            lvalue->indices.push_back(std::move(index));
            lvalue->source_range = span(name_range, previous().range);
        }
        return lvalue;
    }

    std::unique_ptr<Expr> make_binary(BinaryOp op, std::unique_ptr<Expr> lhs,
                                      std::unique_ptr<Expr> rhs) {
        const auto range = span(lhs->source_range, rhs->source_range);
        auto expression = std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs));
        expression->source_range = range;
        return expression;
    }

    std::unique_ptr<Expr> parse_expression() {
        return parse_logical_or_expression();
    }

    std::unique_ptr<Expr> parse_logical_or_expression() {
        auto expression = parse_logical_and_expression();
        if (expression == nullptr) {
            return nullptr;
        }
        while (match(TOK_LOR)) {
            auto rhs = parse_logical_and_expression();
            if (rhs == nullptr) {
                return nullptr;
            }
            expression = make_binary(BinaryOp::Or, std::move(expression), std::move(rhs));
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_logical_and_expression() {
        auto expression = parse_bitwise_or_expression();
        if (expression == nullptr) {
            return nullptr;
        }
        while (match(TOK_LAND)) {
            auto rhs = parse_bitwise_or_expression();
            if (rhs == nullptr) {
                return nullptr;
            }
            expression = make_binary(BinaryOp::And, std::move(expression), std::move(rhs));
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_bitwise_or_expression() {
        auto expression = parse_bitwise_xor_expression();
        if (expression == nullptr) {
            return nullptr;
        }
        while (match('|')) {
            auto rhs = parse_bitwise_xor_expression();
            if (rhs == nullptr) {
                return nullptr;
            }
            expression = make_binary(BinaryOp::BitOr, std::move(expression), std::move(rhs));
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_bitwise_xor_expression() {
        auto expression = parse_bitwise_and_expression();
        if (expression == nullptr) {
            return nullptr;
        }
        while (match('^')) {
            auto rhs = parse_bitwise_and_expression();
            if (rhs == nullptr) {
                return nullptr;
            }
            expression = make_binary(BinaryOp::BitXor, std::move(expression), std::move(rhs));
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_bitwise_and_expression() {
        auto expression = parse_equality_expression();
        if (expression == nullptr) {
            return nullptr;
        }
        while (match('&')) {
            auto rhs = parse_equality_expression();
            if (rhs == nullptr) {
                return nullptr;
            }
            expression = make_binary(BinaryOp::BitAnd, std::move(expression), std::move(rhs));
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_equality_expression() {
        auto expression = parse_relational_expression();
        if (expression == nullptr) {
            return nullptr;
        }
        while (check(TOK_EQ) || check(TOK_NE)) {
            const int op = current().kind;
            ++position_;
            auto rhs = parse_relational_expression();
            if (rhs == nullptr) {
                return nullptr;
            }
            expression = make_binary(op == TOK_EQ ? BinaryOp::Eq : BinaryOp::Ne,
                                     std::move(expression), std::move(rhs));
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_relational_expression() {
        auto expression = parse_additive_expression();
        if (expression == nullptr) {
            return nullptr;
        }
        while (check('<') || check('>') || check(TOK_LE) || check(TOK_GE)) {
            if (check('>') && stop_at_type_greater_) {
                break;
            }
            const int op = current().kind;
            ++position_;
            auto rhs = parse_additive_expression();
            if (rhs == nullptr) {
                return nullptr;
            }
            BinaryOp binary_op = BinaryOp::Lt;
            if (op == '>') {
                binary_op = BinaryOp::Gt;
            } else if (op == TOK_LE) {
                binary_op = BinaryOp::Le;
            } else if (op == TOK_GE) {
                binary_op = BinaryOp::Ge;
            }
            expression = make_binary(binary_op, std::move(expression), std::move(rhs));
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_additive_expression() {
        auto expression = parse_multiplicative_expression();
        if (expression == nullptr) {
            return nullptr;
        }
        while (check('+') || check('-')) {
            const int op = current().kind;
            ++position_;
            auto rhs = parse_multiplicative_expression();
            if (rhs == nullptr) {
                return nullptr;
            }
            expression = make_binary(op == '+' ? BinaryOp::Add : BinaryOp::Sub,
                                     std::move(expression), std::move(rhs));
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_multiplicative_expression() {
        auto expression = parse_unary_expression();
        if (expression == nullptr) {
            return nullptr;
        }
        while (check('*') || check('/') || check('%') || check('@')) {
            const int op = current().kind;
            ++position_;
            auto rhs = parse_unary_expression();
            if (rhs == nullptr) {
                return nullptr;
            }
            BinaryOp binary_op = BinaryOp::Mul;
            if (op == '/') {
                binary_op = BinaryOp::Div;
            } else if (op == '%') {
                binary_op = BinaryOp::Mod;
            } else if (op == '@') {
                binary_op = BinaryOp::MatMul;
            }
            expression = make_binary(binary_op, std::move(expression), std::move(rhs));
        }
        return expression;
    }

    std::unique_ptr<Expr> parse_unary_expression() {
        if (check('+') || check('-') || check('!') || check('~')) {
            const int op = current().kind;
            const auto begin = current().range;
            ++position_;
            auto operand = parse_unary_expression();
            if (operand == nullptr) {
                return nullptr;
            }
            UnaryOp unary_op = UnaryOp::Pos;
            if (op == '-') {
                unary_op = UnaryOp::Neg;
            } else if (op == '!') {
                unary_op = UnaryOp::Not;
            } else if (op == '~') {
                unary_op = UnaryOp::BitNot;
            }
            const auto range = span(begin, operand->source_range);
            auto expression = std::make_unique<UnaryExpr>(unary_op, std::move(operand));
            expression->source_range = range;
            return expression;
        }

        if (check(TOK_VECTOR) || check(TOK_MASK)) {
            return parse_typed_vector_expression();
        }

        if (check('(')) {
            const auto begin = current().range;
            ++position_;
            const bool saved_stop = stop_at_type_greater_;
            stop_at_type_greater_ = false;
            auto expression = parse_expression();
            stop_at_type_greater_ = saved_stop;
            if (expression == nullptr || !expect(')', "')'")) {
                return nullptr;
            }
            expression->source_range = span(begin, previous().range);
            return expression;
        }

        if (check(TOK_INT_CONST)) {
            auto expression = std::make_unique<IntLiteral>(current().int_value);
            expression->source_range = current().range;
            ++position_;
            return expression;
        }
        if (check(TOK_FLOAT_CONST)) {
            auto expression = std::make_unique<FloatLiteral>(current().float_value);
            expression->source_range = current().range;
            ++position_;
            return expression;
        }

        if (check(TOK_IDENT)) {
            std::string name = current().text;
            const auto name_range = current().range;
            ++position_;
            if (match('(')) {
                auto call = std::make_unique<CallExpr>(name);
                const bool saved_stop = stop_at_type_greater_;
                stop_at_type_greater_ = false;
                if (!check(')')) {
                    call->args = parse_expression_list();
                    if (call->args.empty() && !check(')')) {
                        stop_at_type_greater_ = saved_stop;
                        return nullptr;
                    }
                }
                const bool closed = expect(')', "')'");
                stop_at_type_greater_ = saved_stop;
                if (!closed) {
                    return nullptr;
                }
                call->source_range = span(name_range, previous().range);
                return call;
            }
            return parse_lvalue_tail(std::move(name), name_range);
        }

        syntax_error(front::DiagnosticCode::ParseExpectedToken, "expected expression");
        return nullptr;
    }

    std::unique_ptr<Expr> parse_typed_vector_expression() {
        const auto begin = current().range;
        auto type = parse_basic_type();
        if (type == nullptr) {
            return nullptr;
        }

        if (match('{')) {
            std::vector<std::unique_ptr<Expr>> lanes;
            if (!check('}')) {
                while (true) {
                    auto lane = parse_expression();
                    if (lane == nullptr) {
                        while (!check(TOK_EOF) && !check('}') && !check(';')) {
                            advance_token();
                        }
                        if (check('}')) {
                            advance_token();
                        }
                        return nullptr;
                    }
                    lanes.push_back(std::move(lane));
                    if (!match(',')) {
                        break;
                    }
                    if (check('}')) {
                        syntax_error(front::DiagnosticCode::ParseExpectedToken,
                                     "expected expression after ',' in typed literal");
                        advance_token();
                        return nullptr;
                    }
                }
            }
            if (!expect('}', "'}' after typed literal")) {
                return nullptr;
            }
            auto expression =
                std::make_unique<TypedVectorLiteralExpr>(std::move(type), std::move(lanes));
            expression->source_range = span(begin, previous().range);
            return expression;
        }

        if (type->kind() == TypeSyntax::Kind::Vector && match('(')) {
            auto operand = parse_expression();
            if (operand == nullptr || !expect(')', "')' after vector constructor operand")) {
                return nullptr;
            }
            auto expression =
                std::make_unique<VectorCastExpr>(std::move(type), std::move(operand));
            expression->source_range = span(begin, previous().range);
            return expression;
        }

        syntax_error(front::DiagnosticCode::ParseExpectedToken,
                     type->kind() == TypeSyntax::Kind::Mask
                         ? "expected '{' after mask type in expression"
                         : "expected '{' or '(' after vector type in expression");
        return nullptr;
    }
};

void print_diagnostics(const front::DiagnosticEngine &diagnostics) {
    for (const auto &diagnostic : diagnostics.diagnostics()) {
        if (diagnostic.range.valid()) {
            std::cerr << diagnostic.range.begin.line << ':' << diagnostic.range.begin.column
                      << ": ";
        }
        std::cerr << front::diagnostic_severity_name(diagnostic.severity) << '['
                  << front::diagnostic_code_name(diagnostic.code) << "]: " << diagnostic.message
                  << '\n';
    }
}

} // namespace

int parse(std::unique_ptr<CompUnit> &ast, front::DiagnosticEngine &diagnostics,
          front::SourceFileId source_file) {
    const std::size_t initial_errors = diagnostics.error_count();
    resetLexer(&diagnostics, source_file);
    Parser parser(diagnostics);
    ast = parser.parse_compilation_unit();
    return parser.has_error() || diagnostics.error_count() != initial_errors ? 1 : 0;
}

int parse(std::unique_ptr<CompUnit> &ast) {
    front::DiagnosticEngine diagnostics;
    const int result = parse(ast, diagnostics, 1);
    if (!diagnostics.diagnostics().empty()) {
        print_diagnostics(diagnostics);
    }
    return result;
}
