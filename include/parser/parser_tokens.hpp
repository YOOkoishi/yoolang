#pragma once

#include <string>
#include <variant>
#include <vector>

// Token 类型枚举（从 258 开始以避免与 ASCII 字符冲突）
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
};

// Lexer 产生的语义值
struct TokenValue {
    std::string str_val;
    int int_val = 0;
    float float_val = 0.0f;
};

extern TokenValue token_value;
