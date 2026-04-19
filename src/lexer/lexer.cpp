#include "../../include/parser/parser_tokens.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace std;

FILE *yyin = nullptr;
FILE *yyout = nullptr;

namespace {

std::string input;
std::size_t loc = 0;
bool loaded = false;

void loadInput() {
    if (loaded) {
        return;
    }

    loaded = true;
    FILE *stream = yyin != nullptr ? yyin : stdin;
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
        return INT;
    }
    if (name == "float") {
        return FLOAT;
    }
    if (name == "void") {
        return VOID;
    }
    if (name == "return") {
        return RETURN;
    }
    if (name == "const") {
        return CONST;
    }
    if (name == "if") {
        return IF;
    }
    if (name == "else") {
        return ELSE;
    }
    if (name == "while") {
        return WHILE;
    }
    if (name == "break") {
        return BREAK;
    }
    if (name == "continue") {
        return CONTINUE;
    }

    yylval.str_val = new std::string(name);
    return IDENT;
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
        yylval.float_val = strtof(raw.c_str(), nullptr);
        return FLOAT_CONST;
    }

    yylval.int_val = static_cast<int>(strtol(raw.c_str(), nullptr, 0));
    return INT_CONST;
}

int scanPunctuation() {
    char c = peek();

    if (startsWith("<=")) {
        advance(2);
        return LE;
    }
    if (startsWith(">=")) {
        advance(2);
        return GE;
    }
    if (startsWith("==")) {
        advance(2);
        return EQ;
    }
    if (startsWith("!=")) {
        advance(2);
        return NE;
    }
    if (startsWith("||")) {
        advance(2);
        return LOR;
    }
    if (startsWith("&&")) {
        advance(2);
        return LAND;
    }

    advance();
    return static_cast<unsigned char>(c);
}

} // namespace

int yylex() {
    loadInput();
    skipWhitespaceAndComments();

    if (atEnd()) {
        return 0;
    }

    char c = peek();
    if (isIdentStart(c)) {
        return scanKeywordOrIdent();
    }

    if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
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
        std::cerr << "Lexical error: " << c << std::endl;
        advance();
        return static_cast<unsigned char>(c);
    }
}
