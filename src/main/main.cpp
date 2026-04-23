#include "../include/ast/ast_printer.h"
#include "../include/front/parser.h"

#include <cstdio>
#include <iostream>
#include <string>

extern FILE *lexer_input;

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <input.sy> [--emit-ast]" << std::endl;
        return 1;
    }

    const char *filename = argv[1];
    bool emit_ast = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--emit-ast") {
            emit_ast = true;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            return 1;
        }
    }

    FILE *f = std::fopen(filename, "r");
    if (!f) {
        std::cerr << "Cannot open file: " << filename << std::endl;
        return 1;
    }
    lexer_input = f;

    std::unique_ptr<CompUnit> ast;
    int rc = parse(ast);
    std::fclose(f);
    lexer_input = nullptr;

    if (rc != 0 || !ast) {
        std::cerr << "Parse FAILED for: " << filename << std::endl;
        return 1;
    }

    if (emit_ast) {
        std::cout << print_ast_to_string(*ast);
    }

    return 0;
}
