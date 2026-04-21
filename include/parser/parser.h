#pragma once

#include "../include.h"
#include "../ast/ast.h"
#include "parser_tokens.hpp"

int parse(std::unique_ptr<CompUnit> &ast);

