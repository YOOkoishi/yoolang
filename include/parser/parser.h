#pragma once

#include "../include.h"
#include "../ast/ast.h"
#include "parser_tokens.hpp"

int yyparse(std::unique_ptr<CompUnit> &ast);
void yyerror(std::unique_ptr<CompUnit> &ast, const char *s);

