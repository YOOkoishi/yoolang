#include "../include/IR/MIR.h"
#include "../include/IRGen/IRGen.h"
#include "../include/front/parser.h"
#include "../include/include.h"
#include "../include/passes/passes.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

extern FILE *lexer_input;

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <input.sy> [--emit-asm] [module_name]" << std::endl;
        return 1;
    }

    const char *filename = argv[1];
    bool emit_ir = false;
    std::string module_name = "test_module";

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--emit-ir") {
            emit_ir = true;
        } else {
            module_name = arg;
        }
    }

    // 1. Lexing + Parsing
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

    // 2. AST -> SSA IR
    irgen::ASTToIRLowering lowering;
    auto module = lowering.lower(*ast, module_name);

    // 3. Verify
    std::string verify_msg;
    if (!module->verify(&verify_msg)) {
        std::cerr << "WARNING: SSA verify FAILED: " << verify_msg << std::endl;
        // 仍然输出 IR 以便调试
    }

    // 4. Print SSA IR
    std::cout << "; === IR for " << filename << " ===" << std::endl;
    std::cout << module->print() << std::endl;

    // 5. SSA IR -> Machine IR -> RISC-V assembly
    if (!emit_ir) {
        std::cout << "\n; === RISC-V Assembly ===" << std::endl;
        passes::SSAToMIRLowering mir_lowering;
        auto machine_module = mir_lowering.lower(*module);
        machine_module->emit();
    }

    return 0;
}
