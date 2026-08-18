#include "front/parser_tokens.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

FILE *lexer_input = nullptr;
TokenValue token_value;

namespace {

std::string input;
std::size_t offset = 0;
std::uint32_t line = 1;
std::uint32_t column = 1;
bool loaded = false;
front::SourceFileId source_file = 1;
front::DiagnosticEngine *diagnostic_engine = nullptr;

front::SourceLocation current_location() {
    return front::SourceLocation(source_file, offset, line, column);
}

front::SourceRange range_from(front::SourceLocation begin) {
    return front::SourceRange(begin, current_location());
}

void load_input() {
    if (loaded) {
        return;
    }

    loaded = true;
    FILE *stream = lexer_input != nullptr ? lexer_input : stdin;
    int ch = 0;
    while ((ch = std::fgetc(stream)) != EOF) {
        input.push_back(static_cast<char>(ch));
    }
}

bool at_end() {
    return offset >= input.size();
}

char peek(std::size_t lookahead = 0) {
    if (offset + lookahead >= input.size()) {
        return '\0';
    }
    return input[offset + lookahead];
}

void advance(std::size_t count = 1) {
    for (std::size_t i = 0; i < count && !at_end(); ++i) {
        const char ch = input[offset++];
        if (ch == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
}

bool is_ident_start(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

bool is_ident_part(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

bool starts_with(const char *text) {
    for (std::size_t i = 0; text[i] != '\0'; ++i) {
        if (peek(i) != text[i]) {
            return false;
        }
    }
    return true;
}

void report(front::DiagnosticCode code, front::SourceRange range, std::string message) {
    if (diagnostic_engine != nullptr) {
        diagnostic_engine->error(code, range, std::move(message));
    }
}

int finish_token(int kind, front::SourceLocation begin, std::size_t begin_offset) {
    token_value.range = range_from(begin);
    token_value.spelling = input.substr(begin_offset, offset - begin_offset);
    return kind;
}

void skip_whitespace_and_comments() {
    while (!at_end()) {
        if (std::isspace(static_cast<unsigned char>(peek())) != 0) {
            advance();
            continue;
        }

        if (starts_with("//")) {
            advance(2);
            while (!at_end() && peek() != '\n') {
                advance();
            }
            continue;
        }

        if (starts_with("/*")) {
            const auto begin = current_location();
            advance(2);
            while (!at_end() && !starts_with("*/")) {
                advance();
            }
            if (at_end()) {
                report(front::DiagnosticCode::LexUnterminatedComment, range_from(begin),
                       "unterminated block comment");
                return;
            }
            advance(2);
            continue;
        }
        return;
    }
}

int scan_keyword_or_identifier(front::SourceLocation begin, std::size_t begin_offset) {
    while (!at_end() && is_ident_part(peek())) {
        advance();
    }

    const std::string name = input.substr(begin_offset, offset - begin_offset);
    int kind = TOK_IDENT;
    if (name == "int") {
        kind = TOK_INT;
    } else if (name == "float") {
        kind = TOK_FLOAT;
    } else if (name == "void") {
        kind = TOK_VOID;
    } else if (name == "return") {
        kind = TOK_RETURN;
    } else if (name == "const") {
        kind = TOK_CONST;
    } else if (name == "if") {
        kind = TOK_IF;
    } else if (name == "else") {
        kind = TOK_ELSE;
    } else if (name == "while") {
        kind = TOK_WHILE;
    } else if (name == "break") {
        kind = TOK_BREAK;
    } else if (name == "continue") {
        kind = TOK_CONTINUE;
    } else if (name == "vector") {
        kind = TOK_VECTOR;
    } else if (name == "mask") {
        kind = TOK_MASK;
    } else if (name == "tensor") {
        kind = TOK_TENSOR;
    } else if (name == "extern") {
        kind = TOK_EXTERN;
    } else {
        token_value.str_val = name;
    }
    return finish_token(kind, begin, begin_offset);
}

void consume_invalid_numeric_suffix(bool &invalid) {
    if (peek() == '.') {
        invalid = true;
        do {
            advance();
        } while (std::isalnum(static_cast<unsigned char>(peek())) != 0 || peek() == '.');
    }
    if (is_ident_start(peek())) {
        invalid = true;
        while (is_ident_part(peek())) {
            advance();
        }
    }
}

int scan_number(front::SourceLocation begin, std::size_t begin_offset) {
    bool is_float = false;
    bool invalid = false;
    const bool is_hex = peek() == '0' && (peek(1) == 'x' || peek(1) == 'X');

    if (is_hex) {
        advance(2);
        std::size_t digits = 0;
        while (std::isxdigit(static_cast<unsigned char>(peek())) != 0) {
            ++digits;
            advance();
        }
        if (peek() == '.') {
            is_float = true;
            advance();
            while (std::isxdigit(static_cast<unsigned char>(peek())) != 0) {
                ++digits;
                advance();
            }
        }
        if (digits == 0) {
            invalid = true;
        }
        if (peek() == 'p' || peek() == 'P') {
            is_float = true;
            advance();
            if (peek() == '+' || peek() == '-') {
                advance();
            }
            std::size_t exponent_digits = 0;
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                ++exponent_digits;
                advance();
            }
            invalid = invalid || exponent_digits == 0;
        } else if (is_float) {
            invalid = true;
        }
    } else {
        while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
            advance();
        }
        if (peek() == '.') {
            is_float = true;
            advance();
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                advance();
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            is_float = true;
            advance();
            if (peek() == '+' || peek() == '-') {
                advance();
            }
            std::size_t exponent_digits = 0;
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                ++exponent_digits;
                advance();
            }
            invalid = exponent_digits == 0;
        }
    }

    consume_invalid_numeric_suffix(invalid);
    const auto range = range_from(begin);
    const std::string raw = input.substr(begin_offset, offset - begin_offset);
    token_value.range = range;
    token_value.spelling = raw;

    if (invalid) {
        report(is_float ? front::DiagnosticCode::LexInvalidFloatLiteral
                        : front::DiagnosticCode::LexInvalidIntegerLiteral,
               range, "invalid numeric literal '" + raw + "'");
        return TOK_ERROR;
    }

    char *end = nullptr;
    errno = 0;
    if (is_float) {
        const float value = std::strtof(raw.c_str(), &end);
        if (end != raw.c_str() + raw.size()) {
            report(front::DiagnosticCode::LexInvalidFloatLiteral, range,
                   "invalid floating literal '" + raw + "'");
            return TOK_ERROR;
        }
        if (errno == ERANGE || !std::isfinite(value)) {
            report(front::DiagnosticCode::LexFloatLiteralOutOfRange, range,
                   "floating literal is outside float range: '" + raw + "'");
            return TOK_ERROR;
        }
        token_value.float_val = value;
        return TOK_FLOAT_CONST;
    }

    const long long value = std::strtoll(raw.c_str(), &end, 0);
    if (end != raw.c_str() + raw.size()) {
        report(front::DiagnosticCode::LexInvalidIntegerLiteral, range,
               "invalid integer literal '" + raw + "'");
        return TOK_ERROR;
    }
    if (errno == ERANGE || value < std::numeric_limits<int>::min() ||
        value > std::numeric_limits<int>::max()) {
        report(front::DiagnosticCode::LexIntegerLiteralOutOfRange, range,
               "integer literal is outside signed 32-bit range: '" + raw + "'");
        return TOK_ERROR;
    }
    token_value.int_val = static_cast<int>(value);
    return TOK_INT_CONST;
}

int scan_punctuation(front::SourceLocation begin, std::size_t begin_offset) {
    int kind = static_cast<unsigned char>(peek());
    if (starts_with("...")) {
        advance(3);
        kind = TOK_ELLIPSIS;
    } else if (starts_with("<=")) {
        advance(2);
        kind = TOK_LE;
    } else if (starts_with(">=")) {
        advance(2);
        kind = TOK_GE;
    } else if (starts_with("==")) {
        advance(2);
        kind = TOK_EQ;
    } else if (starts_with("!=")) {
        advance(2);
        kind = TOK_NE;
    } else if (starts_with("||")) {
        advance(2);
        kind = TOK_LOR;
    } else if (starts_with("&&")) {
        advance(2);
        kind = TOK_LAND;
    } else {
        advance();
    }
    return finish_token(kind, begin, begin_offset);
}

} // namespace

void resetLexer(front::DiagnosticEngine *diagnostics, front::SourceFileId file) {
    input.clear();
    offset = 0;
    line = 1;
    column = 1;
    loaded = false;
    source_file = file == 0 ? 1 : file;
    diagnostic_engine = diagnostics;
    token_value = TokenValue{};
}

int nextToken() {
    load_input();
    skip_whitespace_and_comments();
    token_value = TokenValue{};

    const auto begin = current_location();
    const std::size_t begin_offset = offset;
    if (at_end()) {
        token_value.range = front::SourceRange::point(begin);
        return TOK_EOF;
    }

    const char ch = peek();
    if (is_ident_start(ch)) {
        return scan_keyword_or_identifier(begin, begin_offset);
    }
    if (starts_with("...")) {
        return scan_punctuation(begin, begin_offset);
    }
    if (std::isdigit(static_cast<unsigned char>(ch)) != 0 ||
        (ch == '.' && std::isdigit(static_cast<unsigned char>(peek(1))) != 0)) {
        return scan_number(begin, begin_offset);
    }

    switch (ch) {
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case ';':
    case ',':
    case '+':
    case '-':
    case '*':
    case '/':
    case '%':
    case '!':
    case '~':
    case '<':
    case '>':
    case '=':
    case '&':
    case '^':
    case '|':
    case '@':
        return scan_punctuation(begin, begin_offset);
    default:
        advance();
        token_value.range = range_from(begin);
        token_value.spelling = input.substr(begin_offset, offset - begin_offset);
        report(front::DiagnosticCode::LexUnexpectedCharacter, token_value.range,
               std::string("unexpected character '") + ch + "'");
        return TOK_ERROR;
    }
}
