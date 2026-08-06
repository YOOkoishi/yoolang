#pragma once

#include "front/Diagnostic.h"

#include <cstdio>
#include <string>

enum TokenKind {
    TOK_EOF = 0,

    // 关键字
    TOK_INT = 258,
    TOK_FLOAT = 259,
    TOK_VOID = 260,
    TOK_RETURN = 261,
    TOK_CONST = 262,
    TOK_IF = 263,
    TOK_ELSE = 264,
    TOK_WHILE = 265,
    TOK_BREAK = 266,
    TOK_CONTINUE = 267,

    // 运算符
    TOK_LE = 268,
    TOK_GE = 269,
    TOK_EQ = 270,
    TOK_NE = 271,
    TOK_LOR = 272,
    TOK_LAND = 273,

    // 字面量 / 标识符
    TOK_IDENT = 274,
    TOK_INT_CONST = 275,
    TOK_FLOAT_CONST = 276,

    // 错误
    TOK_ERROR = 277,

    // 一等源类型
    TOK_VECTOR = 278,
    TOK_MASK = 279,

    // 外部函数原型
    TOK_EXTERN = 280,
    TOK_ELLIPSIS = 281,
};

// Lexer 产生的语义值
struct TokenValue {
    std::string str_val;
    std::string spelling;
    int int_val = 0;
    float float_val = 0.0f;
    front::SourceRange range;
};

extern TokenValue token_value;
extern FILE *lexer_input;

// Reset all lexer state before a parse. Source file id zero is reserved for
// unknown locations, so callers normally keep the default id one.
void resetLexer(front::DiagnosticEngine *diagnostics = nullptr,
                front::SourceFileId source_file = 1);
int nextToken();
