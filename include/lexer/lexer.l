%option noyywrap
%option nounput
%option noinput

%{

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

// 词法分析器依赖 Bison 生成的 token 定义
#include "parser/parser_tokens.hpp"

using namespace std;

%}

/* 空白符和注释 */
WhiteSpace    [ \t\n\r]+
LineComment   "//"[^\n\r]*
BlockComment  "/*"([^*]|\*+[^*/])*"*/"

/* 标识符 */
Identifier    [a-zA-Z_][a-zA-Z0-9_]*

/* 整数字面量 */
Decimal       [1-9][0-9]*
Octal         0[0-7]*
Hexadecimal   0[xX][0-9a-fA-F]+
Exponent      [eE][+-]?[0-9]+
FloatA        ([0-9]+\.[0-9]*|[0-9]*\.[0-9]+)({Exponent})?
FloatB        [0-9]+{Exponent}
FloatLiteral  ({FloatA}|{FloatB})

%%

{WhiteSpace}    { /* 忽略 */ }
{LineComment}   { /* 忽略 */ }
{BlockComment}  { /* 忽略 */ }

"int"           { return INT; }
"float"         { return FLOAT; }
"void"          { return VOID; }
"return"        { return RETURN; }
"const"         { return CONST; }
"if"            { return IF; }
"else"          { return ELSE; }
"while"         { return WHILE; }
"break"         { return BREAK; }
"continue"      { return CONTINUE; }

"<="            { return LE; }
">="            { return GE; }
"=="            { return EQ; }
"!="            { return NE; }
"||"            { return LOR; }
"&&"            { return LAND; }

{Identifier}     { yylval.str_val = new string(yytext); return IDENT; }

{FloatLiteral}   { yylval.float_val = strtof(yytext, nullptr); return FLOAT_CONST; }
{Decimal}        { yylval.int_val = strtol(yytext, nullptr, 0); return INT_CONST; }
{Octal}          { yylval.int_val = strtol(yytext, nullptr, 0); return INT_CONST; }
{Hexadecimal}    { yylval.int_val = strtol(yytext, nullptr, 0); return INT_CONST; }

"("             { return yytext[0]; }
")"             { return yytext[0]; }
"["             { return yytext[0]; }
"]"             { return yytext[0]; }
"{"             { return yytext[0]; }
"}"             { return yytext[0]; }
";"             { return yytext[0]; }
","             { return yytext[0]; }

"-"             { return yytext[0]; }
"+"             { return yytext[0]; }
"*"             { return yytext[0]; }
"/"             { return yytext[0]; }
"%"             { return yytext[0]; }
"!"             { return yytext[0]; }
"<"             { return yytext[0]; }
">"             { return yytext[0]; }
"="             { return yytext[0]; }

.                { std::cerr << "Lexical error: " << yytext << std::endl; return yytext[0]; }

%%
