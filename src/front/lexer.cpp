#include "front/parser_tokens.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace std;

FILE *lexer_input = nullptr;

TokenValue token_value;

namespace {

std::string input;
std::size_t loc = 0;
bool loaded = false;

void loadInput() {
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

bool atEnd() {
    return loc >= input.size();
}

char peek(std::size_t offset = 0) {
    if (loc + offset >= input.size()) {
        return '\0';
    }
    return input[loc + offset];
}

void advance(std::size_t count = 1) {
    loc += count;
}

bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool isIdentPart(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool startsWith(const char *text) {
    for (std::size_t i = 0; text[i] != '\0'; ++i) {
        if (peek(i) != text[i]) {
            return false;
        }
    }
    return true;
}

void skipWhitespaceAndComments() {
    while (!atEnd()) {
        char c = peek();

        if (std::isspace(static_cast<unsigned char>(c))) {
            advance();
            continue;
        }

        if (startsWith("//")) {
            advance(2);
            while (!atEnd() && peek() != '\n') {
                advance();
            }
            continue;
        }

        if (startsWith("/*")) {
            advance(2);
            while (!atEnd() && !startsWith("*/")) {
                advance();
            }
            if (startsWith("*/")) {
                advance(2);
            }
            continue;
        }

        break;
    }
}

int scanKeywordOrIdent() {
    std::size_t start = loc;
    while (!atEnd() && isIdentPart(peek())) {
        advance();
    }

    std::string name = input.substr(start, loc - start);
    if (name == "int") {
        return TOK_INT;
    }
    if (name == "float") {
        return TOK_FLOAT;
    }
    if (name == "void") {
        return TOK_VOID;
    }
    if (name == "return") {
        return TOK_RETURN;
    }
    if (name == "const") {
        return TOK_CONST;
    }
    if (name == "if") {
        return TOK_IF;
    }
    if (name == "else") {
        return TOK_ELSE;
    }
    if (name == "while") {
        return TOK_WHILE;
    }
    if (name == "break") {
        return TOK_BREAK;
    }
    if (name == "continue") {
        return TOK_CONTINUE;
    }

    token_value.str_val = name;
    return TOK_IDENT;
}

int scanNumber() {
    std::size_t start = loc;
    bool isFloat = false;

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        advance(2);
        while (std::isxdigit(static_cast<unsigned char>(peek())) || peek() == '.') {
            if (peek() == '.') {
                isFloat = true;
            }
            advance();
        }

        if (peek() == 'p' || peek() == 'P') {
            isFloat = true;
            advance();

            if (peek() == '+' || peek() == '-') {
                advance();
            }
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        }
    } else {
        while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.') {
            if (peek() == '.') {
                isFloat = true;
            }
            advance();
        }

        if (peek() == 'e' || peek() == 'E') {
            isFloat = true;
            advance();

            if (peek() == '+' || peek() == '-') {
                advance();
            }
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        }
    }

    std::string raw = input.substr(start, loc - start);
    if (isFloat) {
        token_value.float_val = strtof(raw.c_str(), nullptr);
        return TOK_FLOAT_CONST;
    }

    token_value.int_val = static_cast<int>(strtol(raw.c_str(), nullptr, 0));
    return TOK_INT_CONST;
}

int scanPunctuation() {
    char c = peek();

    if (startsWith("<=")) {
        advance(2);
        return TOK_LE;
    }
    if (startsWith(">=")) {
        advance(2);
        return TOK_GE;
    }
    if (startsWith("==")) {
        advance(2);
        return TOK_EQ;
    }
    if (startsWith("!=")) {
        advance(2);
        return TOK_NE;
    }
    if (startsWith("||")) {
        advance(2);
        return TOK_LOR;
    }
    if (startsWith("&&")) {
        advance(2);
        return TOK_LAND;
    }

    advance();
    return static_cast<unsigned char>(c);
}

} // namespace

void reset_lexer_state() {
    input.clear();
    loc = 0;
    loaded = false;
}

int nextToken() {
    loadInput();
    skipWhitespaceAndComments();

    if (atEnd()) {
        return TOK_EOF;
    }

    char c = peek();
    if (isIdentStart(c)) {
        return scanKeywordOrIdent();
    }

    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
        return scanNumber();
    }

    switch (c) {
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
    case '<':
    case '>':
    case '=':
    case '&':
    case '|':
        return scanPunctuation();
    default:
        std::cerr << "Lexical error: unexpected character '" << c << "'" << std::endl;
        advance();
        return TOK_ERROR;
    }
}
