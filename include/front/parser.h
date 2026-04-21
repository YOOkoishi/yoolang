#pragma once

#include "../ast/ast.h"
#include "../include.h"
#include "parser_tokens.h"

int parse(std::unique_ptr<CompUnit> &ast);
