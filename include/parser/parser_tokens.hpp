#pragma once

#include <string>
#include <vector>

enum yytokentype {
    INT = 258,
    FLOAT = 259,
    VOID = 260,
    RETURN = 261,
    CONST = 262,
    IF = 263,
    ELSE = 264,
    WHILE = 265,
    BREAK = 266,
    CONTINUE = 267,
    LE = 268,
    GE = 269,
    EQ = 270,
    NE = 271,
    LOR = 272,
    LAND = 273,
    IDENT = 274,
    INT_CONST = 275,
    FLOAT_CONST = 276,
    UNOT = 277,
    UMINUS = 278,
    UPLUS = 279
};

#define INT 258
#define FLOAT 259
#define VOID 260
#define RETURN 261
#define CONST 262
#define IF 263
#define ELSE 264
#define WHILE 265
#define BREAK 266
#define CONTINUE 267
#define LE 268
#define GE 269
#define EQ 270
#define NE 271
#define LOR 272
#define LAND 273
#define IDENT 274
#define INT_CONST 275
#define FLOAT_CONST 276
#define UNOT 277
#define UMINUS 278
#define UPLUS 279

typedef union YYSTYPE {
    std::string *str_val;
    int int_val;
    float float_val;
    void *expr;
    void *stmt;
    void *decl_stmt;
    void *var_decl;
    void *func_def;
    void *comp_unit;
    void *init_val;
    void *param_val;
    std::vector<void *> *expr_vec;
    std::vector<void *> *stmt_vec;
    std::vector<void *> *var_decl_vec;
    std::vector<void *> *init_val_vec;
    std::vector<void *> *param_vec;
} YYSTYPE;

extern YYSTYPE yylval;