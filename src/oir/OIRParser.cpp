#include "oir/OIRParser.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace oir {
namespace {

enum class TokenKind {
    Identifier,
    AtName,
    PercentName,
    Integer,
    Floating,
    FloatBits,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    LAngle,
    RAngle,
    Comma,
    Colon,
    Equal,
    Star,
    Newline,
    End,
};

struct Token final {
    TokenKind kind = TokenKind::End;
    std::string_view text;
    OIRSourceRange range;
};

struct ParseFailure final {
    OIRParseError error;
};

bool is_name_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '.' ||
           ch == '$';
}

bool is_name_continue(char ch) {
    return is_name_start(ch) || (ch >= '0' && ch <= '9') || ch == '-';
}

bool is_hex_digit(char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

class Lexer final {
  public:
    Lexer(std::string_view source, std::string source_name)
        : source_(source), source_name_(std::move(source_name)) {
    }

    std::vector<Token> lex() {
        std::vector<Token> tokens;
        while (!at_end()) {
            const char ch = peek();
            if (ch == ' ' || ch == '\t' || ch == '\f' || ch == '\v') {
                advance();
                continue;
            }
            if (ch == ';') {
                while (!at_end() && peek() != '\n' && peek() != '\r') {
                    advance();
                }
                continue;
            }
            if (ch == '\n' || ch == '\r') {
                const auto begin = position();
                if (ch == '\r') {
                    advance_raw();
                    if (!at_end() && peek() == '\n') {
                        advance_raw();
                    }
                    ++line_;
                    column_ = 1;
                } else {
                    advance();
                }
                tokens.push_back(make_token(TokenKind::Newline, begin, position()));
                continue;
            }
            if (ch == '@' || ch == '%') {
                const auto begin = position();
                const auto start = offset_;
                advance();
                if (at_end() || !is_name_continue(peek())) {
                    fail(begin, position(), "expected a name after sigil");
                }
                while (!at_end() && is_name_continue(peek())) {
                    advance();
                }
                tokens.push_back(make_token(ch == '@' ? TokenKind::AtName : TokenKind::PercentName,
                                            begin, position(), start));
                continue;
            }
            if (ch == '0' && offset_ + 1 < source_.size() &&
                (source_[offset_ + 1] == 'x' || source_[offset_ + 1] == 'X')) {
                const auto begin = position();
                const auto start = offset_;
                advance();
                advance();
                std::size_t digits = 0;
                while (!at_end() && is_hex_digit(peek())) {
                    advance();
                    ++digits;
                }
                if (digits != 8 || (!at_end() && is_name_continue(peek()))) {
                    fail(begin, position(),
                         "binary32 bit literal must contain exactly eight hexadecimal digits");
                }
                tokens.push_back(make_token(TokenKind::FloatBits, begin, position(), start));
                continue;
            }
            if ((ch >= '0' && ch <= '9') ||
                (ch == '-' && offset_ + 1 < source_.size() && source_[offset_ + 1] >= '0' &&
                 source_[offset_ + 1] <= '9')) {
                const auto begin = position();
                const auto start = offset_;
                if (ch == '-') {
                    advance();
                }
                while (!at_end() && peek() >= '0' && peek() <= '9') {
                    advance();
                }
                bool floating = false;
                if (!at_end() && peek() == '.') {
                    floating = true;
                    advance();
                    while (!at_end() && peek() >= '0' && peek() <= '9') {
                        advance();
                    }
                }
                if (!at_end() && (peek() == 'e' || peek() == 'E')) {
                    floating = true;
                    advance();
                    if (!at_end() && (peek() == '+' || peek() == '-')) {
                        advance();
                    }
                    if (at_end() || peek() < '0' || peek() > '9') {
                        fail(begin, position(), "malformed floating-point exponent");
                    }
                    while (!at_end() && peek() >= '0' && peek() <= '9') {
                        advance();
                    }
                }
                tokens.push_back(make_token(floating ? TokenKind::Floating : TokenKind::Integer,
                                            begin, position(), start));
                continue;
            }
            if (is_name_start(ch)) {
                const auto begin = position();
                const auto start = offset_;
                while (!at_end() && is_name_continue(peek())) {
                    advance();
                }
                tokens.push_back(make_token(TokenKind::Identifier, begin, position(), start));
                continue;
            }

            const auto begin = position();
            advance();
            TokenKind kind;
            switch (ch) {
            case '(':
                kind = TokenKind::LParen;
                break;
            case ')':
                kind = TokenKind::RParen;
                break;
            case '{':
                kind = TokenKind::LBrace;
                break;
            case '}':
                kind = TokenKind::RBrace;
                break;
            case '[':
                kind = TokenKind::LBracket;
                break;
            case ']':
                kind = TokenKind::RBracket;
                break;
            case '<':
                kind = TokenKind::LAngle;
                break;
            case '>':
                kind = TokenKind::RAngle;
                break;
            case ',':
                kind = TokenKind::Comma;
                break;
            case ':':
                kind = TokenKind::Colon;
                break;
            case '=':
                kind = TokenKind::Equal;
                break;
            case '*':
                kind = TokenKind::Star;
                break;
            default:
                fail(begin, position(), std::string("unexpected character '") + ch + "'");
            }
            tokens.push_back(make_token(kind, begin, position(), begin.offset));
        }
        const auto end = position();
        tokens.push_back(Token{TokenKind::End, {}, {end, end}});
        return tokens;
    }

  private:
    bool at_end() const {
        return offset_ >= source_.size();
    }
    char peek() const {
        return source_[offset_];
    }

    OIRSourcePosition position() const {
        return {offset_, line_, column_};
    }

    void advance_raw() {
        ++offset_;
        ++column_;
    }

    void advance() {
        if (peek() == '\n') {
            ++offset_;
            ++line_;
            column_ = 1;
        } else {
            advance_raw();
        }
    }

    Token make_token(TokenKind kind, OIRSourcePosition begin, OIRSourcePosition end,
                     std::size_t start = std::numeric_limits<std::size_t>::max()) const {
        if (start == std::numeric_limits<std::size_t>::max()) {
            start = begin.offset;
        }
        return {kind, source_.substr(start, end.offset - start), {begin, end}};
    }

    [[noreturn]] void fail(OIRSourcePosition begin, OIRSourcePosition end,
                           std::string message) const {
        throw ParseFailure{{{begin, end}, source_name_ + ": " + std::move(message)}};
    }

    std::string_view source_;
    std::string source_name_;
    std::size_t offset_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
};

std::string trim_copy(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ||
                             text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                             text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

std::string module_name_from_source(std::string_view source) {
    const auto line_end = source.find_first_of("\r\n");
    auto first_line = source.substr(0, line_end);
    constexpr std::string_view prefix = "; module:";
    if (first_line.substr(0, prefix.size()) == prefix) {
        auto name = trim_copy(first_line.substr(prefix.size()));
        if (!name.empty()) {
            return name;
        }
    }
    return "parsed.oir";
}

class ParserImpl final {
  public:
    ParserImpl(std::string_view source, std::string source_name)
        : source_(source), source_name_(std::move(source_name)),
          tokens_(Lexer(source, source_name_).lex()),
          module_(std::make_unique<Module>(module_name_from_source(source))) {
    }

    std::unique_ptr<Module> parse() {
        try {
            parse_skeleton();
            pos_ = 0;
            parse_full_module();
            std::string verification_error;
            if (!module_->verify(&verification_error)) {
                fail(current(), "parsed module failed verification: " + verification_error);
            }
            return std::move(module_);
        } catch (const ParseFailure &) {
            throw;
        } catch (const std::exception &exception) {
            fail(current(), "IR construction failed: " + std::string(exception.what()));
        }
    }

  private:
    struct LocalValue final {
        Value *value = nullptr;
        bool placeholder = false;
        OIRSourceRange first_use;
    };

    const Token &current() const {
        return tokens_[pos_];
    }
    const Token &peek(std::size_t lookahead) const {
        return tokens_[std::min(pos_ + lookahead, tokens_.size() - 1)];
    }
    bool at(TokenKind kind) const {
        return current().kind == kind;
    }
    bool at_identifier(std::string_view text) const {
        return at(TokenKind::Identifier) && current().text == text;
    }

    const Token &consume() {
        return tokens_[pos_++];
    }

    const Token &expect(TokenKind kind, std::string message) {
        if (!at(kind)) {
            fail(current(), std::move(message));
        }
        return consume();
    }

    const Token &expect_identifier(std::string_view text) {
        if (!at_identifier(text)) {
            fail(current(), "expected '" + std::string(text) + "'");
        }
        return consume();
    }

    [[noreturn]] void fail(const Token &token, std::string message) const {
        throw ParseFailure{{token.range, source_name_ + ": " + std::move(message)}};
    }

    void skip_newlines() {
        while (at(TokenKind::Newline)) {
            consume();
        }
    }

    void expect_line_end() {
        if (at(TokenKind::Newline)) {
            skip_newlines();
            return;
        }
        if (!at(TokenKind::End) && !at(TokenKind::RBrace)) {
            fail(current(), "expected end of line");
        }
    }

    void skip_to_line_end() {
        while (!at(TokenKind::Newline) && !at(TokenKind::End)) {
            consume();
        }
        skip_newlines();
    }

    std::uint64_t parse_u64(const Token &token, std::string_view what) {
        if (token.kind != TokenKind::Integer ||
            (!token.text.empty() && token.text.front() == '-')) {
            fail(token, std::string(what) + " must be a nonnegative integer");
        }
        errno = 0;
        char *end = nullptr;
        const std::string spelling(token.text);
        const auto value = std::strtoull(spelling.c_str(), &end, 10);
        if (errno == ERANGE || end != spelling.c_str() + spelling.size()) {
            fail(token, std::string(what) + " is out of range");
        }
        return static_cast<std::uint64_t>(value);
    }

    std::int64_t parse_i64(const Token &token, std::string_view what) {
        if (token.kind != TokenKind::Integer) {
            fail(token, std::string(what) + " must be an integer");
        }
        errno = 0;
        char *end = nullptr;
        const std::string spelling(token.text);
        const auto value = std::strtoll(spelling.c_str(), &end, 10);
        if (errno == ERANGE || end != spelling.c_str() + spelling.size()) {
            fail(token, std::string(what) + " is out of range");
        }
        return static_cast<std::int64_t>(value);
    }

    Type *parse_type() {
        Type *type = nullptr;
        if (at_identifier("void")) {
            consume();
            type = module_->types().void_ty();
        } else if (at_identifier("label")) {
            consume();
            type = module_->types().label_ty();
        } else if (at_identifier("i1")) {
            consume();
            type = module_->types().int1_ty();
        } else if (at_identifier("i32")) {
            consume();
            type = module_->types().int32_ty();
        } else if (at_identifier("float")) {
            consume();
            type = module_->types().float_ty();
        } else if (at(TokenKind::LBracket)) {
            consume();
            const auto count =
                parse_u64(expect(TokenKind::Integer, "expected array bound"), "array bound");
            expect_identifier("x");
            auto *element = parse_type();
            expect(TokenKind::RBracket, "expected ']' after array type");
            if (count > std::numeric_limits<std::size_t>::max()) {
                fail(current(), "array bound is too large for this target");
            }
            type = module_->types().array_ty(element, static_cast<std::size_t>(count));
        } else if (at(TokenKind::LAngle)) {
            consume();
            bool scalable = false;
            if (at_identifier("vscale")) {
                consume();
                expect_identifier("x");
                scalable = true;
            }
            const auto lanes = parse_u64(expect(TokenKind::Integer, "expected vector lane count"),
                                         "vector lane count");
            expect_identifier("x");
            auto *element = parse_type();
            expect(TokenKind::RAngle, "expected '>' after vector type");
            type = module_->types().vector_ty(element, ElementCount(lanes, scalable));
        } else {
            fail(current(), "expected OIR type");
        }

        if (at(TokenKind::LParen)) {
            consume();
            std::vector<Type *> params;
            bool is_variadic = false;
            if (!at(TokenKind::RParen)) {
                while (true) {
                    if (at_identifier("...")) {
                        consume();
                        is_variadic = true;
                        break;
                    }
                    params.push_back(parse_type());
                    if (!at(TokenKind::Comma)) {
                        break;
                    }
                    consume();
                    if (at(TokenKind::RParen)) {
                        fail(current(), "expected function parameter type or '...' after ','");
                    }
                }
            }
            expect(TokenKind::RParen, "expected ')' after function type");
            type = module_->types().func_ty(type, params, is_variadic);
        }

        while (at(TokenKind::Star)) {
            consume();
            type = module_->types().ptr_ty(type);
        }
        return type;
    }

    struct Signature final {
        std::string name;
        Type *return_type = nullptr;
        std::vector<Type *> param_types;
        std::vector<std::string> param_names;
        bool is_variadic = false;
    };

    Signature parse_signature() {
        Signature signature;
        signature.return_type = parse_type();
        const auto &name = expect(TokenKind::AtName, "expected function name");
        signature.name = std::string(name.text.substr(1));
        expect(TokenKind::LParen, "expected '(' after function name");
        if (!at(TokenKind::RParen)) {
            while (true) {
                if (at_identifier("...")) {
                    consume();
                    signature.is_variadic = true;
                    break;
                }
                signature.param_types.push_back(parse_type());
                const auto &param = expect(TokenKind::PercentName, "expected parameter name");
                signature.param_names.emplace_back(param.text.substr(1));
                if (!at(TokenKind::Comma)) {
                    break;
                }
                consume();
                if (at(TokenKind::RParen)) {
                    fail(current(), "expected parameter or '...' after ','");
                }
            }
        }
        expect(TokenKind::RParen, "expected ')' after parameters");
        return signature;
    }

    Function *materialize_signature(const Signature &signature, bool external) {
        auto *function_type = module_->types().func_ty(signature.return_type, signature.param_types,
                                                       signature.is_variadic);
        auto *existing = module_->get_function(signature.name);
        auto *function = module_->create_function(signature.name, function_type, external);
        if (existing != nullptr && existing->function_type() != function_type) {
            fail(current(), "conflicting declaration of @" + signature.name);
        }
        if (!external) {
            function->set_external(false);
        }
        if (function->args().size() != signature.param_names.size()) {
            fail(current(), "function parameter count is inconsistent");
        }
        for (std::size_t i = 0; i < signature.param_names.size(); ++i) {
            function->args()[i]->set_name(signature.param_names[i]);
        }
        return function;
    }

    void parse_skeleton() {
        skip_newlines();
        while (!at(TokenKind::End)) {
            if (at(TokenKind::AtName)) {
                const auto name = std::string(consume().text.substr(1));
                expect(TokenKind::Equal, "expected '=' after global name");
                const bool is_const = at_identifier("constant");
                if (!is_const && !at_identifier("global")) {
                    fail(current(), "expected 'global' or 'constant'");
                }
                consume();
                auto *type = parse_type();
                if (module_->get_global(name) != nullptr) {
                    fail(current(), "duplicate global @" + name);
                }
                module_->create_global(name, type, is_const);
                skip_to_line_end();
                continue;
            }
            const bool declaration = at_identifier("declare");
            const bool definition = at_identifier("define");
            if (!declaration && !definition) {
                fail(current(), "expected global, declaration, or function definition");
            }
            consume();
            auto signature = parse_signature();
            if (module_->get_function(signature.name) != nullptr) {
                fail(current(), "duplicate function @" + signature.name);
            }
            materialize_signature(signature, declaration);
            if (declaration) {
                expect_line_end();
                continue;
            }
            expect(TokenKind::LBrace, "expected '{' before function body");
            unsigned depth = 1;
            while (depth != 0) {
                if (at(TokenKind::End)) {
                    fail(current(), "unterminated function body");
                }
                if (at(TokenKind::LBrace)) {
                    ++depth;
                } else if (at(TokenKind::RBrace)) {
                    --depth;
                }
                consume();
            }
            expect_line_end();
        }
    }

    Constant *parse_constant(Type *expected_type) {
        if (at_identifier("zero")) {
            consume();
            return module_->create_zero(expected_type);
        }
        if (auto *integer_type = dynamic_cast<IntegerType *>(expected_type)) {
            const auto &literal = expect(TokenKind::Integer, "expected integer constant");
            const auto value = parse_i64(literal, "integer constant");
            if (integer_type->bit_width() == 1) {
                if (value != 0 && value != 1) {
                    fail(literal, "i1 constant must be zero or one");
                }
                return module_->create_i1(value != 0);
            }
            if (integer_type->bit_width() == 32 &&
                (value < std::numeric_limits<std::int32_t>::min() ||
                 value > std::numeric_limits<std::int32_t>::max())) {
                fail(literal, "i32 constant is out of range");
            }
            return module_->create_i32(value);
        }
        if (expected_type->is_scalar_float()) {
            if (at(TokenKind::FloatBits)) {
                const auto literal = consume();
                const std::string spelling(literal.text.substr(2));
                errno = 0;
                char *end = nullptr;
                const auto raw = std::strtoul(spelling.c_str(), &end, 16);
                if (errno == ERANGE || end != spelling.c_str() + spelling.size() ||
                    raw > std::numeric_limits<std::uint32_t>::max()) {
                    fail(literal, "binary32 bit literal is out of range");
                }
                const auto bits = static_cast<std::uint32_t>(raw);
                float value = 0.0F;
                static_assert(sizeof(value) == sizeof(bits),
                              "OIR requires an IEEE-754 binary32 host float");
                std::memcpy(&value, &bits, sizeof(value));
                return module_->create_f32(value);
            }
            if (!at(TokenKind::Floating) && !at(TokenKind::Integer)) {
                fail(current(), "expected floating-point constant");
            }
            const auto literal = consume();
            errno = 0;
            char *end = nullptr;
            const std::string spelling(literal.text);
            const auto value = std::strtof(spelling.c_str(), &end);
            // C libraries also report ERANGE for a correctly rounded binary32
            // subnormal.  Accept underflow (including round-to-zero) so legacy
            // decimal OIR remains readable, but reject overflow to infinity.
            // The canonical printer uses an exact 0xXXXXXXXX bit literal.
            if (end != spelling.c_str() + spelling.size() ||
                (errno == ERANGE && std::isinf(value))) {
                fail(literal, "floating-point constant is out of range");
            }
            return module_->create_f32(value);
        }
        if (auto *array_type = dynamic_cast<ArrayType *>(expected_type)) {
            expect(TokenKind::LBracket, "expected '[' for typed array constant");
            std::vector<Constant *> elements;
            if (!at(TokenKind::RBracket)) {
                while (true) {
                    auto *element_type = parse_type();
                    if (element_type != array_type->element_type()) {
                        fail(current(), "array constant element type mismatch");
                    }
                    elements.push_back(parse_constant(element_type));
                    if (!at(TokenKind::Comma)) {
                        break;
                    }
                    consume();
                }
            }
            expect(TokenKind::RBracket, "expected ']' after array constant");
            return module_->create_constant_array(array_type, elements);
        }
        if (auto *vector_type = dynamic_cast<VectorType *>(expected_type)) {
            if (vector_type->element_count().is_scalable()) {
                fail(current(), "explicit scalable vector constants are not representable");
            }
            expect(TokenKind::LAngle, "expected '<' for typed vector constant");
            std::vector<Constant *> elements;
            std::vector<std::uint8_t> packed(
                static_cast<std::size_t>(vector_type->element_count().min_lanes / 8U +
                                         (vector_type->element_count().min_lanes % 8U != 0)),
                0);
            std::uint64_t lane = 0;
            if (!at(TokenKind::RAngle)) {
                while (true) {
                    auto *element_type = parse_type();
                    if (element_type != vector_type->element_type()) {
                        fail(current(), "vector constant element type mismatch");
                    }
                    auto *element = parse_constant(element_type);
                    elements.push_back(element);
                    if (vector_type->is_mask()) {
                        const auto *bit = dynamic_cast<const ConstantInt *>(element);
                        if (bit == nullptr) {
                            fail(current(), "mask constant lanes must be explicit i1 values");
                        }
                        if (bit->value() != 0 && lane < vector_type->element_count().min_lanes) {
                            packed[static_cast<std::size_t>(lane / 8U)] |=
                                static_cast<std::uint8_t>(1U << (lane % 8U));
                        }
                    }
                    ++lane;
                    if (!at(TokenKind::Comma)) {
                        break;
                    }
                    consume();
                }
            }
            expect(TokenKind::RAngle, "expected '>' after vector constant");
            if (lane != vector_type->element_count().min_lanes) {
                fail(current(), "vector constant lane count mismatch");
            }
            if (vector_type->is_mask()) {
                return module_->create_constant_mask(vector_type, packed);
            }
            return module_->create_constant_vector(vector_type, elements);
        }
        fail(current(), "type does not support a structured constant");
    }

    void parse_full_module();
    void parse_global_definition();
    void parse_function(bool declaration);
    void parse_function_body(Function *function);
    Instruction *parse_instruction(Function *function, BasicBlock *block, IRBuilder &builder,
                                   std::unordered_map<std::string, LocalValue> &values,
                                   const std::unordered_map<std::string, BasicBlock *> &blocks);

    Value *parse_value(Type *expected_type, Function *function,
                       std::unordered_map<std::string, LocalValue> &values);
    std::pair<Type *, Value *>
    parse_typed_value(Function *function, std::unordered_map<std::string, LocalValue> &values);
    void define_local(const Token &name_token, Instruction *value,
                      std::unordered_map<std::string, LocalValue> &values);
    BasicBlock *parse_block_ref(const std::unordered_map<std::string, BasicBlock *> &blocks);

    std::string_view source_;
    std::string source_name_;
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    std::unique_ptr<Module> module_;
};

void ParserImpl::parse_full_module() {
    skip_newlines();
    while (!at(TokenKind::End)) {
        if (at(TokenKind::AtName)) {
            parse_global_definition();
            continue;
        }
        if (at_identifier("declare")) {
            consume();
            parse_function(true);
            continue;
        }
        if (at_identifier("define")) {
            consume();
            parse_function(false);
            continue;
        }
        fail(current(), "expected global, declaration, or function definition");
    }
}

void ParserImpl::parse_global_definition() {
    const auto &name_token = expect(TokenKind::AtName, "expected global name");
    const std::string name(name_token.text.substr(1));
    expect(TokenKind::Equal, "expected '=' after global name");
    const bool is_const = at_identifier("constant");
    if (!is_const && !at_identifier("global")) {
        fail(current(), "expected 'global' or 'constant'");
    }
    consume();
    auto *type = parse_type();
    auto *global = module_->get_global(name);
    if (global == nullptr || global->value_type() != type || global->is_const() != is_const) {
        fail(name_token, "global definition disagrees with its declaration skeleton");
    }

    if (at(TokenKind::Newline) || at(TokenKind::End)) {
        expect_line_end();
        return;
    }
    if (at(TokenKind::LBrace)) {
        fail(current(),
             "legacy textual global initializers are not supported; use a typed constant");
    } else {
        global->set_initializer(parse_constant(type));
    }
    expect_line_end();
}

void ParserImpl::parse_function(bool declaration) {
    auto signature = parse_signature();
    auto *function = module_->get_function(signature.name);
    auto *expected_type = module_->types().func_ty(signature.return_type, signature.param_types,
                                                   signature.is_variadic);
    if (function == nullptr || function->function_type() != expected_type ||
        function->is_external() != declaration) {
        fail(current(), "function definition disagrees with its declaration skeleton");
    }
    for (std::size_t i = 0; i < signature.param_names.size(); ++i) {
        function->args()[i]->set_name(signature.param_names[i]);
    }

    if (declaration) {
        expect_line_end();
        return;
    }
    expect(TokenKind::LBrace, "expected '{' before function body");
    parse_function_body(function);
    expect(TokenKind::RBrace, "expected '}' after function body");
    expect_line_end();
}

void ParserImpl::parse_function_body(Function *function) {
    const auto body_begin = pos_;
    std::unordered_map<std::string, BasicBlock *> blocks;
    bool at_line_start = true;
    std::size_t scan = pos_;
    for (; scan < tokens_.size() && tokens_[scan].kind != TokenKind::RBrace; ++scan) {
        if (tokens_[scan].kind == TokenKind::End) {
            fail(tokens_[scan], "unterminated function body");
        }
        if (tokens_[scan].kind == TokenKind::Newline) {
            at_line_start = true;
            continue;
        }
        if (at_line_start && tokens_[scan].kind == TokenKind::Identifier &&
            scan + 1 < tokens_.size() && tokens_[scan + 1].kind == TokenKind::Colon) {
            const std::string name(tokens_[scan].text);
            if (blocks.find(name) != blocks.end()) {
                fail(tokens_[scan], "duplicate basic block '" + name + "'");
            }
            blocks.emplace(name, function->create_block_exact(name));
        }
        at_line_start = false;
    }
    if (scan >= tokens_.size() || tokens_[scan].kind != TokenKind::RBrace) {
        fail(current(), "unterminated function body");
    }
    pos_ = body_begin;

    std::unordered_map<std::string, LocalValue> values;
    for (const auto &argument : function->args()) {
        const auto [it, inserted] =
            values.emplace(argument->name(), LocalValue{argument.get(), false, current().range});
        if (!inserted) {
            fail(current(), "duplicate function argument %" + argument->name());
        }
        (void)it;
    }

    IRBuilder builder(module_.get());
    BasicBlock *current_block = nullptr;
    std::unordered_set<std::string> parsed_blocks;
    skip_newlines();
    while (!at(TokenKind::RBrace)) {
        if (at(TokenKind::End)) {
            fail(current(), "unterminated function body");
        }
        if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon) {
            const auto label = std::string(consume().text);
            expect(TokenKind::Colon, "expected ':' after basic block label");
            if (!parsed_blocks.insert(label).second) {
                fail(current(), "duplicate basic block '" + label + "'");
            }
            auto found = blocks.find(label);
            if (found == blocks.end()) {
                fail(current(), "unknown basic block '" + label + "'");
            }
            current_block = found->second;
            builder.set_insert_point(current_block);
            expect_line_end();
            continue;
        }
        if (current_block == nullptr) {
            fail(current(), "instruction appears before the first basic block label");
        }
        parse_instruction(function, current_block, builder, values, blocks);
        expect_line_end();
    }

    for (const auto &[name, local] : values) {
        if (local.placeholder) {
            throw ParseFailure{{local.first_use, source_name_ + ": unresolved SSA value %" + name}};
        }
    }
}

Value *ParserImpl::parse_value(Type *expected_type, Function *,
                               std::unordered_map<std::string, LocalValue> &values) {
    Value *value = nullptr;
    const auto token = current();
    if (at(TokenKind::PercentName)) {
        const std::string name(consume().text.substr(1));
        auto found = values.find(name);
        if (found == values.end()) {
            auto *placeholder = module_->create_undef(expected_type);
            found = values.emplace(name, LocalValue{placeholder, true, token.range}).first;
        }
        value = found->second.value;
    } else if (at(TokenKind::AtName)) {
        const std::string name(consume().text.substr(1));
        value = module_->get_global(name);
        if (value == nullptr) {
            value = module_->get_function(name);
        }
        if (value == nullptr) {
            fail(token, "unknown global value @" + name);
        }
    } else if (at_identifier("undef")) {
        consume();
        value = module_->create_undef(expected_type);
    } else {
        value = parse_constant(expected_type);
    }
    if (value == nullptr || value->type() != expected_type) {
        fail(token, "value type does not match the preceding type");
    }
    return value;
}

std::pair<Type *, Value *>
ParserImpl::parse_typed_value(Function *function,
                              std::unordered_map<std::string, LocalValue> &values) {
    auto *type = parse_type();
    return {type, parse_value(type, function, values)};
}

void ParserImpl::define_local(const Token &name_token, Instruction *value,
                              std::unordered_map<std::string, LocalValue> &values) {
    if (value == nullptr || value->type() == nullptr || value->type()->is_void()) {
        fail(name_token, "a void instruction cannot define an SSA value");
    }
    const std::string name(name_token.text.substr(1));
    auto found = values.find(name);
    if (found == values.end()) {
        values.emplace(name, LocalValue{value, false, name_token.range});
        return;
    }
    if (!found->second.placeholder) {
        fail(name_token, "duplicate SSA definition %" + name);
    }
    if (found->second.value->type() != value->type()) {
        fail(name_token, "SSA definition type disagrees with an earlier forward reference");
    }
    found->second.value->replace_all_uses_with(value);
    found->second = LocalValue{value, false, name_token.range};
}

BasicBlock *
ParserImpl::parse_block_ref(const std::unordered_map<std::string, BasicBlock *> &blocks) {
    const auto &token = expect(TokenKind::PercentName, "expected basic block reference");
    const std::string name(token.text.substr(1));
    auto found = blocks.find(name);
    if (found == blocks.end()) {
        fail(token, "unknown basic block %" + name);
    }
    return found->second;
}

Instruction *
ParserImpl::parse_instruction(Function *function, BasicBlock *, IRBuilder &builder,
                              std::unordered_map<std::string, LocalValue> &values,
                              const std::unordered_map<std::string, BasicBlock *> &blocks) {
    bool has_name = false;
    Token name_token;
    std::string result_name;
    if (at(TokenKind::PercentName) && peek(1).kind == TokenKind::Equal) {
        has_name = true;
        name_token = consume();
        result_name = std::string(name_token.text.substr(1));
        consume();
    }
    const auto &op_token = expect(TokenKind::Identifier, "expected instruction opcode");
    const std::string op(op_token.text);

    auto comma = [&]() { expect(TokenKind::Comma, "expected ',' between operands"); };
    auto parse_predicate = [&]() {
        const auto &token = expect(TokenKind::Identifier, "expected comparison predicate");
        if (token.text == "eq")
            return CmpPred::EQ;
        if (token.text == "ne")
            return CmpPred::NE;
        if (token.text == "lt")
            return CmpPred::LT;
        if (token.text == "le")
            return CmpPred::LE;
        if (token.text == "gt")
            return CmpPred::GT;
        if (token.text == "ge")
            return CmpPred::GE;
        fail(token, "unknown comparison predicate '" + std::string(token.text) + "'");
    };
    auto parse_alignment = [&]() -> std::size_t {
        expect_identifier("align");
        const auto &token = expect(TokenKind::Integer, "expected alignment value");
        const auto value = parse_u64(token, "alignment");
        if (value > std::numeric_limits<std::size_t>::max()) {
            fail(token, "alignment is too large for this target");
        }
        return static_cast<std::size_t>(value);
    };
    auto parse_lane_index = [&]() -> std::uint64_t {
        comma();
        expect_identifier("lane");
        const auto &token = expect(TokenKind::Integer, "expected fixed ABI lane index");
        return parse_u64(token, "fixed ABI lane index");
    };
    auto parse_tail_policy = [&]() {
        const auto &token = expect(TokenKind::Identifier, "expected tail policy");
        if (token.text == "agnostic")
            return TailPolicy::Agnostic;
        if (token.text == "undisturbed")
            return TailPolicy::Undisturbed;
        fail(token, "unknown tail policy '" + std::string(token.text) + "'");
    };
    auto parse_mask_policy = [&]() {
        const auto &token = expect(TokenKind::Identifier, "expected mask policy");
        if (token.text == "agnostic")
            return MaskPolicy::Agnostic;
        if (token.text == "undisturbed")
            return MaskPolicy::Undisturbed;
        fail(token, "unknown mask policy '" + std::string(token.text) + "'");
    };
    auto parse_vp_metadata = [&](bool has_passthrough) {
        VPMetadata metadata;
        comma();
        expect_identifier("mask");
        metadata.active_mask = parse_typed_value(function, values).second;
        comma();
        expect_identifier("evl");
        metadata.evl = parse_typed_value(function, values).second;
        if (has_passthrough) {
            comma();
            expect_identifier("passthrough");
            metadata.passthrough = parse_typed_value(function, values).second;
        }
        comma();
        expect_identifier("tail");
        expect(TokenKind::Equal, "expected '=' after tail policy key");
        metadata.tail_policy = parse_tail_policy();
        comma();
        expect_identifier("mask-policy");
        expect(TokenKind::Equal, "expected '=' after mask policy key");
        metadata.mask_policy = parse_mask_policy();
        return metadata;
    };
    auto binary_opcode = [&](std::string_view spelling, bool vp) {
        if (spelling == "add")
            return Instruction::OpID::Add;
        if (spelling == "sub")
            return Instruction::OpID::Sub;
        if (spelling == "mul")
            return Instruction::OpID::Mul;
        if (spelling == "sdiv")
            return Instruction::OpID::SDiv;
        if (spelling == "srem")
            return Instruction::OpID::SRem;
        if (spelling == "fadd")
            return Instruction::OpID::FAdd;
        if (spelling == "fsub")
            return Instruction::OpID::FSub;
        if (spelling == "fmul")
            return Instruction::OpID::FMul;
        if (spelling == "fdiv")
            return Instruction::OpID::FDiv;
        if (spelling == "and")
            return Instruction::OpID::And;
        if (spelling == "or")
            return Instruction::OpID::Or;
        if (spelling == "xor")
            return Instruction::OpID::Xor;
        fail(op_token, std::string(vp ? "unknown VP binary opcode '" : "unknown binary opcode '") +
                           std::string(spelling) + "'");
    };

    Instruction *instruction = nullptr;
    const bool plain_binary = op == "add" || op == "sub" || op == "mul" || op == "sdiv" ||
                              op == "srem" || op == "fadd" || op == "fsub" || op == "fmul" ||
                              op == "fdiv" || op == "and" || op == "or" || op == "xor";
    if (plain_binary) {
        auto [type, lhs] = parse_typed_value(function, values);
        comma();
        auto *rhs = parse_value(type, function, values);
        instruction = builder.create_binary(binary_opcode(op, false), lhs, rhs, result_name);
    } else if (op == "icmp" || op == "fcmp") {
        const auto predicate = parse_predicate();
        auto [type, lhs] = parse_typed_value(function, values);
        comma();
        auto *rhs = parse_value(type, function, values);
        instruction =
            op == "icmp"
                ? static_cast<Instruction *>(builder.create_icmp(predicate, lhs, rhs, result_name))
                : static_cast<Instruction *>(builder.create_fcmp(predicate, lhs, rhs, result_name));
    } else if (op == "zext" || op == "sitofp" || op == "fptosi") {
        auto [source_type, source] = parse_typed_value(function, values);
        (void)source_type;
        expect_identifier("to");
        auto *destination = parse_type();
        if (op == "zext") {
            instruction = builder.create_zext(source, destination, result_name);
        } else if (op == "sitofp") {
            instruction = builder.create_sitofp(source, destination, result_name);
        } else {
            instruction = builder.create_fptosi(source, destination, result_name);
        }
    } else if (op == "setvl") {
        auto *configuration = dynamic_cast<VectorType *>(parse_type());
        if (configuration == nullptr) {
            fail(current(), "setvl configuration must be a vector type");
        }
        comma();
        auto [avl_type, avl] = parse_typed_value(function, values);
        (void)avl_type;
        instruction = builder.create_set_vl(configuration, avl, result_name);
    } else if (op == "alloca") {
        instruction = builder.create_alloca(parse_type(), result_name);
    } else if (op == "load") {
        auto *result_type = parse_type();
        comma();
        auto [pointer_type, pointer] = parse_typed_value(function, values);
        (void)pointer_type;
        instruction = builder.create_load(pointer, result_type, result_name);
    } else if (op == "store") {
        auto [value_type, value] = parse_typed_value(function, values);
        (void)value_type;
        comma();
        auto [pointer_type, pointer] = parse_typed_value(function, values);
        (void)pointer_type;
        instruction = builder.create_store(value, pointer);
    } else if (op == "memzero") {
        auto [pointer_type, pointer] = parse_typed_value(function, values);
        (void)pointer_type;
        comma();
        auto [count_type, count] = parse_typed_value(function, values);
        (void)count_type;
        instruction = builder.create_memzero(pointer, count);
    } else if (op == "memset") {
        auto [pointer_type, pointer] = parse_typed_value(function, values);
        (void)pointer_type;
        comma();
        auto [byte_type, byte] = parse_typed_value(function, values);
        (void)byte_type;
        comma();
        auto [count_type, count] = parse_typed_value(function, values);
        (void)count_type;
        instruction = builder.create_memset(pointer, byte, count);
    } else if (op == "gep") {
        auto [base_type, base] = parse_typed_value(function, values);
        (void)base_type;
        std::vector<Value *> indices;
        while (at(TokenKind::Comma)) {
            consume();
            indices.push_back(parse_typed_value(function, values).second);
        }
        expect_identifier("to");
        auto *result_type = parse_type();
        instruction = builder.create_gep(base, result_type, indices, result_name);
    } else if (op == "call") {
        auto *return_type = parse_type();
        Value *callee = nullptr;
        const auto callee_token = current();
        if (at(TokenKind::AtName)) {
            const std::string name(consume().text.substr(1));
            callee = module_->get_function(name);
            if (callee == nullptr) {
                callee = module_->get_global(name);
            }
        } else if (at(TokenKind::PercentName)) {
            const std::string name(consume().text.substr(1));
            auto found = values.find(name);
            if (found != values.end() && !found->second.placeholder) {
                callee = found->second.value;
            }
        } else {
            fail(current(), "expected call target");
        }
        if (callee == nullptr) {
            fail(callee_token, "unknown or forward-referenced call target");
        }
        expect(TokenKind::LParen, "expected '(' after call target");
        std::vector<Value *> arguments;
        if (!at(TokenKind::RParen)) {
            while (true) {
                arguments.push_back(parse_typed_value(function, values).second);
                if (!at(TokenKind::Comma)) {
                    break;
                }
                consume();
            }
        }
        expect(TokenKind::RParen, "expected ')' after call arguments");
        instruction = builder.create_call(callee, return_type, arguments, result_name);
    } else if (op == "ret") {
        if (at_identifier("void")) {
            consume();
            instruction = builder.create_ret();
        } else {
            instruction = builder.create_ret(parse_typed_value(function, values).second);
        }
    } else if (op == "br") {
        if (at(TokenKind::PercentName)) {
            instruction = builder.create_br(parse_block_ref(blocks));
        } else {
            auto [condition_type, condition] = parse_typed_value(function, values);
            (void)condition_type;
            comma();
            auto *true_block = parse_block_ref(blocks);
            comma();
            auto *false_block = parse_block_ref(blocks);
            instruction = builder.create_cond_br(condition, true_block, false_block);
        }
    } else if (op == "phi") {
        const auto incoming_begin = pos_;
        auto colon = pos_;
        while (colon < tokens_.size() && tokens_[colon].kind != TokenKind::Colon &&
               tokens_[colon].kind != TokenKind::Newline && tokens_[colon].kind != TokenKind::End) {
            ++colon;
        }
        if (colon >= tokens_.size() || tokens_[colon].kind != TokenKind::Colon) {
            fail(current(), "phi instruction is missing its result type");
        }
        pos_ = colon + 1;
        auto *phi_type = parse_type();
        const auto after_type = pos_;
        pos_ = incoming_begin;
        auto *phi = builder.create_phi(phi_type, result_name);
        if (!at(TokenKind::Colon)) {
            while (true) {
                expect(TokenKind::LBracket, "expected '[' before phi incoming value");
                auto *incoming = parse_value(phi_type, function, values);
                comma();
                auto *from = parse_block_ref(blocks);
                expect(TokenKind::RBracket, "expected ']' after phi incoming pair");
                phi->add_incoming(incoming, from);
                if (!at(TokenKind::Comma)) {
                    break;
                }
                consume();
            }
        }
        expect(TokenKind::Colon, "expected ':' before phi result type");
        if (parse_type() != phi_type || pos_ != after_type) {
            fail(current(), "phi result type changed while parsing incoming values");
        }
        instruction = phi;
    } else if (op == "splat") {
        auto [scalar_type, scalar] = parse_typed_value(function, values);
        (void)scalar_type;
        expect_identifier("to");
        auto *result_type = dynamic_cast<VectorType *>(parse_type());
        if (result_type == nullptr) {
            fail(current(), "splat result must be a vector type");
        }
        instruction = builder.create_splat(result_type, scalar, result_name);
    } else if (op == "stepvector") {
        auto *result_type = dynamic_cast<VectorType *>(parse_type());
        if (result_type == nullptr) {
            fail(current(), "stepvector result must be a vector type");
        }
        instruction = builder.create_step_vector(result_type, result_name);
    } else if (op == "extractelement") {
        auto [vector_type, vector] = parse_typed_value(function, values);
        (void)vector_type;
        comma();
        auto [index_type, index] = parse_typed_value(function, values);
        (void)index_type;
        instruction = builder.create_extract_element(vector, index, result_name);
    } else if (op == "insertelement") {
        auto [vector_type, vector] = parse_typed_value(function, values);
        (void)vector_type;
        comma();
        auto [element_type, element] = parse_typed_value(function, values);
        (void)element_type;
        comma();
        auto [index_type, index] = parse_typed_value(function, values);
        (void)index_type;
        instruction = builder.create_insert_element(vector, element, index, result_name);
    } else if (op == "shufflevector") {
        auto [lhs_type, lhs] = parse_typed_value(function, values);
        (void)lhs_type;
        comma();
        auto [rhs_type, rhs] = parse_typed_value(function, values);
        (void)rhs_type;
        comma();
        expect(TokenKind::LBracket, "expected '[' before shuffle mask");
        std::vector<std::int64_t> mask;
        if (!at(TokenKind::RBracket)) {
            while (true) {
                const auto &entry = expect(TokenKind::Integer, "expected shuffle mask entry");
                mask.push_back(parse_i64(entry, "shuffle mask entry"));
                if (!at(TokenKind::Comma)) {
                    break;
                }
                consume();
            }
        }
        expect(TokenKind::RBracket, "expected ']' after shuffle mask");
        expect_identifier("to");
        auto *result_type = dynamic_cast<VectorType *>(parse_type());
        if (result_type == nullptr) {
            fail(current(), "shufflevector result must be a vector type");
        }
        instruction = builder.create_shuffle_vector(result_type, lhs, rhs, mask, result_name);
    } else if (op == "select") {
        auto [condition_type, condition] = parse_typed_value(function, values);
        (void)condition_type;
        comma();
        auto [true_type, true_value] = parse_typed_value(function, values);
        (void)true_type;
        comma();
        auto [false_type, false_value] = parse_typed_value(function, values);
        (void)false_type;
        instruction = builder.create_vector_select(condition, true_value, false_value, result_name);
    } else if (op == "vector.zext" || op == "vector.sitofp" || op == "vector.fptosi" ||
               op == "vector.bitcast") {
        auto [source_type, source] = parse_typed_value(function, values);
        (void)source_type;
        expect_identifier("to");
        auto *result_type = dynamic_cast<VectorType *>(parse_type());
        if (result_type == nullptr) {
            fail(current(), "vector cast result must be a vector type");
        }
        VectorCastKind kind = VectorCastKind::Bitcast;
        if (op == "vector.zext")
            kind = VectorCastKind::ZExt;
        if (op == "vector.sitofp")
            kind = VectorCastKind::SIToFP;
        if (op == "vector.fptosi")
            kind = VectorCastKind::FPToSI;
        instruction = builder.create_vector_cast(kind, result_type, source, result_name);
    } else if (op == "abi.fixed.extract") {
        auto [aggregate_type, aggregate] = parse_typed_value(function, values);
        auto *vector_type = dynamic_cast<VectorType *>(aggregate_type);
        const auto lane = parse_lane_index();
        if (vector_type == nullptr || vector_type->element_count().is_scalable()) {
            fail(op_token, "abi.fixed.extract requires a fixed-vector aggregate");
        }
        if (lane >= vector_type->element_count().min_lanes) {
            fail(op_token, "abi.fixed.extract lane index is out of range");
        }
        instruction = builder.create_fixed_abi_extract_lane(aggregate, lane, result_name);
    } else if (op == "abi.fixed.pack") {
        auto *result_type = dynamic_cast<VectorType *>(parse_type());
        if (result_type == nullptr || result_type->element_count().is_scalable()) {
            fail(op_token, "abi.fixed.pack result must be a fixed-vector type");
        }
        expect(TokenKind::LBracket, "expected '[' before fixed ABI pack lanes");
        std::vector<Value *> lane_values;
        if (!at(TokenKind::RBracket)) {
            while (true) {
                auto [lane_type, lane_value] = parse_typed_value(function, values);
                if (lane_type != result_type->element_type()) {
                    fail(current(), "abi.fixed.pack lane type does not match vector element type");
                }
                lane_values.push_back(lane_value);
                if (!at(TokenKind::Comma))
                    break;
                consume();
            }
        }
        expect(TokenKind::RBracket, "expected ']' after fixed ABI pack lanes");
        if (lane_values.size() != result_type->element_count().min_lanes) {
            fail(op_token, "abi.fixed.pack requires exactly one value per fixed lane");
        }
        instruction = builder.create_fixed_abi_pack(result_type, lane_values, result_name);
    } else if (op == "abi.fixed.load_lane") {
        auto [pointer_type, object_ptr] = parse_typed_value(function, values);
        auto *pointer = dynamic_cast<PointerType *>(pointer_type);
        auto *object_type =
            pointer == nullptr ? nullptr : dynamic_cast<VectorType *>(pointer->element_type());
        const auto lane = parse_lane_index();
        if (object_type == nullptr || object_type->element_count().is_scalable()) {
            fail(op_token, "abi.fixed.load_lane requires ptr<fixed-vector>");
        }
        if (lane >= object_type->element_count().min_lanes) {
            fail(op_token, "abi.fixed.load_lane index is out of range");
        }
        instruction = builder.create_fixed_abi_object_load_lane(object_ptr, lane, result_name);
    } else if (op == "abi.fixed.store_lane") {
        auto [lane_type, lane_value] = parse_typed_value(function, values);
        comma();
        auto [pointer_type, object_ptr] = parse_typed_value(function, values);
        auto *pointer = dynamic_cast<PointerType *>(pointer_type);
        auto *object_type =
            pointer == nullptr ? nullptr : dynamic_cast<VectorType *>(pointer->element_type());
        const auto lane = parse_lane_index();
        if (object_type == nullptr || object_type->element_count().is_scalable()) {
            fail(op_token, "abi.fixed.store_lane requires ptr<fixed-vector>");
        }
        if (lane_type != object_type->element_type()) {
            fail(op_token, "abi.fixed.store_lane value type does not match vector element type");
        }
        if (lane >= object_type->element_count().min_lanes) {
            fail(op_token, "abi.fixed.store_lane index is out of range");
        }
        instruction = builder.create_fixed_abi_object_store_lane(lane_value, object_ptr, lane);
    } else {
        const bool vp_prefix = op.size() > 3 && op.compare(0, 3, "vp.") == 0;
        const std::string vp_op = vp_prefix ? op.substr(3) : std::string();
        const bool vp_binary =
            vp_prefix &&
            (vp_op == "add" || vp_op == "sub" || vp_op == "mul" || vp_op == "sdiv" ||
             vp_op == "srem" || vp_op == "fadd" || vp_op == "fsub" || vp_op == "fmul" ||
             vp_op == "fdiv" || vp_op == "and" || vp_op == "or" || vp_op == "xor");
        if (vp_binary) {
            auto [type, lhs] = parse_typed_value(function, values);
            comma();
            auto *rhs = parse_value(type, function, values);
            auto metadata = parse_vp_metadata(true);
            instruction = builder.create_vp_binary(
                binary_opcode(vp_op, true), lhs, rhs, metadata.active_mask, metadata.evl,
                metadata.passthrough, metadata.tail_policy, metadata.mask_policy, result_name);
        } else if (op == "vp.icmp" || op == "vp.fcmp") {
            const auto predicate = parse_predicate();
            auto [type, lhs] = parse_typed_value(function, values);
            comma();
            auto *rhs = parse_value(type, function, values);
            auto metadata = parse_vp_metadata(true);
            instruction = op == "vp.icmp"
                              ? static_cast<Instruction *>(builder.create_vp_icmp(
                                    predicate, lhs, rhs, metadata.active_mask, metadata.evl,
                                    metadata.passthrough, metadata.tail_policy,
                                    metadata.mask_policy, result_name))
                              : static_cast<Instruction *>(builder.create_vp_fcmp(
                                    predicate, lhs, rhs, metadata.active_mask, metadata.evl,
                                    metadata.passthrough, metadata.tail_policy,
                                    metadata.mask_policy, result_name));
        } else if (op == "vp.load" || op == "masked.load") {
            auto *result_type = dynamic_cast<VectorType *>(parse_type());
            if (result_type == nullptr) {
                fail(current(), "VP load result must be a vector type");
            }
            comma();
            auto [pointer_type, pointer] = parse_typed_value(function, values);
            (void)pointer_type;
            comma();
            const auto alignment = parse_alignment();
            auto metadata = parse_vp_metadata(true);
            instruction = op == "vp.load"
                              ? static_cast<Instruction *>(builder.create_vp_load(
                                    result_type, pointer, metadata.active_mask, metadata.evl,
                                    metadata.passthrough, metadata.tail_policy,
                                    metadata.mask_policy, alignment, result_name))
                              : static_cast<Instruction *>(builder.create_masked_load(
                                    result_type, pointer, metadata.active_mask, metadata.evl,
                                    metadata.passthrough, metadata.tail_policy,
                                    metadata.mask_policy, alignment, result_name));
        } else if (op == "vp.store" || op == "masked.store") {
            auto [value_type, value] = parse_typed_value(function, values);
            (void)value_type;
            comma();
            auto [pointer_type, pointer] = parse_typed_value(function, values);
            (void)pointer_type;
            comma();
            const auto alignment = parse_alignment();
            auto metadata = parse_vp_metadata(false);
            instruction = op == "vp.store"
                              ? static_cast<Instruction *>(builder.create_vp_store(
                                    value, pointer, metadata.active_mask, metadata.evl,
                                    metadata.tail_policy, metadata.mask_policy, alignment))
                              : static_cast<Instruction *>(builder.create_masked_store(
                                    value, pointer, metadata.active_mask, metadata.evl,
                                    metadata.tail_policy, metadata.mask_policy, alignment));
        } else if (op == "vp.gather") {
            auto *result_type = dynamic_cast<VectorType *>(parse_type());
            if (result_type == nullptr) {
                fail(current(), "VP gather result must be a vector type");
            }
            comma();
            expect_identifier("base");
            auto *base = parse_typed_value(function, values).second;
            comma();
            expect_identifier("indices");
            auto *indices = parse_typed_value(function, values).second;
            comma();
            const auto alignment = parse_alignment();
            auto metadata = parse_vp_metadata(true);
            instruction =
                builder.create_vp_gather(result_type, base, indices, metadata.active_mask,
                                         metadata.evl, metadata.passthrough, metadata.tail_policy,
                                         metadata.mask_policy, alignment, result_name);
        } else if (op == "vp.scatter") {
            auto *value = parse_typed_value(function, values).second;
            comma();
            expect_identifier("base");
            auto *base = parse_typed_value(function, values).second;
            comma();
            expect_identifier("indices");
            auto *indices = parse_typed_value(function, values).second;
            comma();
            const auto alignment = parse_alignment();
            auto metadata = parse_vp_metadata(false);
            instruction =
                builder.create_vp_scatter(value, base, indices, metadata.active_mask, metadata.evl,
                                          metadata.tail_policy, metadata.mask_policy, alignment);
        } else if (op.size() > 10 && op.compare(0, 10, "vp.reduce.") == 0) {
            auto reduction_name = op.substr(10);
            bool ordered = false;
            constexpr std::string_view ordered_prefix = "ordered.";
            if (reduction_name.size() >= ordered_prefix.size() &&
                reduction_name.compare(0, ordered_prefix.size(), ordered_prefix) == 0) {
                ordered = true;
                reduction_name.erase(0, ordered_prefix.size());
            }
            ReductionKind kind;
            if (reduction_name == "add" || reduction_name == "fadd")
                kind = ReductionKind::Add;
            else if (reduction_name == "mul" || reduction_name == "fmul")
                kind = ReductionKind::Mul;
            else if (reduction_name == "min" || reduction_name == "fmin")
                kind = ReductionKind::Min;
            else if (reduction_name == "max" || reduction_name == "fmax")
                kind = ReductionKind::Max;
            else if (reduction_name == "and")
                kind = ReductionKind::And;
            else if (reduction_name == "or")
                kind = ReductionKind::Or;
            else if (reduction_name == "xor")
                kind = ReductionKind::Xor;
            else
                fail(op_token, "unknown VP reduction kind '" + reduction_name + "'");
            auto *vector = parse_typed_value(function, values).second;
            auto metadata = parse_vp_metadata(true);
            instruction = builder.create_vp_reduction(
                kind, ordered, vector, metadata.active_mask, metadata.evl, metadata.passthrough,
                metadata.tail_policy, metadata.mask_policy, result_name);
        } else {
            fail(op_token, "unknown instruction opcode '" + op + "'");
        }
    }

    if (has_name) {
        define_local(name_token, instruction, values);
    }
    return instruction;
}

} // namespace

bool OIRParseResult::ok() const {
    return module != nullptr && errors.empty();
}

OIRParseResult OIRParser::parse(std::string_view source, std::string source_name) noexcept {
    try {
        ParserImpl parser(source, std::move(source_name));
        return {parser.parse(), {}};
    } catch (const ParseFailure &failure) {
        return {nullptr, {failure.error}};
    } catch (const std::exception &exception) {
        OIRParseError error;
        error.range = {{0, 1, 1}, {0, 1, 1}};
        error.message =
            "OIR parser internal construction failure: " + std::string(exception.what());
        return {nullptr, {std::move(error)}};
    } catch (...) {
        OIRParseError error;
        error.range = {{0, 1, 1}, {0, 1, 1}};
        error.message = "OIR parser internal unknown failure";
        return {nullptr, {std::move(error)}};
    }
}

} // namespace oir
