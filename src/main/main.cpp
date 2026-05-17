#include "../include/front/parser.h"
#include "../include/mir/MIRPrinter.h"
#include "../include/pass/ASTDumpPass.h"
#include "../include/pass/ASTSemanticAnalysisPass.h"
#include "../include/pass/ASTToYIRPass.h"
#include "../include/pass/MIRCombinePass.h"
#include "../include/pass/MIRPeepholePass.h"
#include "../include/pass/MIRRegAllocPass.h"
#include "../include/pass/MIRToAsmPass.h"
#include "../include/pass/OIRAlgebraicSimplifyPass.h"
#include "../include/pass/OIRCFGCleanupPass.h"
#include "../include/pass/OIRConstantFoldPass.h"
#include "../include/pass/OIRDeadCodeEliminationPass.h"
#include "../include/pass/OIRGVNPass.h"
#include "../include/pass/OIRGlobalOptPass.h"
#include "../include/pass/OIRInlinePass.h"
#include "../include/pass/OIRLICMPass.h"
#include "../include/pass/OIRLoopStrengthReductionPass.h"
#include "../include/pass/OIRMem2RegPass.h"
#include "../include/pass/OIROptimizationPipelinePass.h"
#include "../include/pass/OIRSCCPPass.h"
#include "../include/pass/OIRTailRecursionEliminationPass.h"
#include "../include/pass/OIRToMIRPass.h"
#include "../include/pass/PassManager.h"
#include "../include/pass/YIRLoopAnalysisPass.h"
#include "../include/pass/YIRLoopCanonicalizePass.h"
#include "../include/pass/YIRLoopOptimizationPass.h"
#include "../include/pass/YIRToOIRPass.h"
#include "../include/yir/YIRPrinter.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

extern FILE *lexer_input;

namespace {

struct CommandLineOptions {
    std::string input_path;
    std::string output_path;
    int opt_level = 0;
    bool emit_ast = false;
    bool emit_yir = false;
    bool emit_oir = false;
    bool emit_mir = false;
    bool emit_asm = false;
    bool show_help = false;
};

void print_help(const char *program, std::ostream &out) {
    out << "Usage: " << program << " <input.sysy> -S -o <output.s>\n"
        << "       " << program << " [options] <input.sy>\n\n"
        << "Options:\n"
        << "  -h, --help       Show this help message\n"
        << "  -S, --emit-asm   Lower to RISC-V assembly (default)\n"
        << "  -o <file>        Write output to <file> instead of stdout\n"
        << "  -O1/-O2/-O3      Enable OIR optimizations, vreg MIR lowering, RA, and MIR peephole "
           "optimizations\n"
        << "  --emit-ast       Dump the parsed AST through the pass pipeline\n"
        << "  --emit-yir       Lower the parsed AST to YIR and dump it\n"
        << "  --emit-oir       Lower the parsed AST to SSA OIR, verify it, and dump it\n"
        << "  --emit-mir       Lower OIR to the RISC-V MIR and dump it\n";
}

bool parse_command_line(int argc, char **argv, CommandLineOptions &options, std::string &error) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            options.show_help = true;
            continue;
        }
        if (arg == "--emit-ast") {
            options.emit_ast = true;
            continue;
        }
        if (arg == "--emit-yir") {
            options.emit_yir = true;
            continue;
        }
        if (arg == "--emit-oir") {
            options.emit_oir = true;
            continue;
        }
        if (arg == "--emit-mir") {
            options.emit_mir = true;
            continue;
        }
        if (arg == "-S" || arg == "--emit-asm") {
            options.emit_asm = true;
            continue;
        }
        if (arg == "-o") {
            if (i + 1 >= argc) {
                error = "-o requires an output file";
                return false;
            }
            options.output_path = argv[++i];
            continue;
        }
        if (arg == "-O1" || arg == "-O2" || arg == "-O3") {
            options.opt_level = arg[2] - '0';
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            error = "unknown option: " + arg;
            return false;
        }
        if (!options.input_path.empty()) {
            error = "multiple input files are not supported";
            return false;
        }
        options.input_path = std::move(arg);
    }

    if (!options.show_help && options.input_path.empty()) {
        error = "missing input file";
        return false;
    }

    if (!options.show_help && !options.emit_ast && !options.emit_yir && !options.emit_oir &&
        !options.emit_mir && !options.emit_asm) {
        options.emit_asm = true;
    }

    return true;
}

bool needs_yir(const CommandLineOptions &options) {
    return options.emit_yir || options.emit_oir || options.emit_mir || options.emit_asm;
}

bool needs_oir(const CommandLineOptions &options) {
    return options.emit_oir || options.emit_mir || options.emit_asm;
}

bool needs_mir(const CommandLineOptions &options) {
    return options.emit_mir || options.emit_asm;
}

bool optimizations_enabled(const CommandLineOptions &options) {
    return options.opt_level >= 1;
}

std::unique_ptr<CompUnit> parse_ast_from_file(const std::string &input_path, std::ostream &err) {
    FILE *input = std::fopen(input_path.c_str(), "r");
    if (input == nullptr) {
        err << "Cannot open file: " << input_path << std::endl;
        return nullptr;
    }

    lexer_input = input;

    std::unique_ptr<CompUnit> ast;
    int rc = parse(ast);

    std::fclose(input);
    lexer_input = nullptr;

    if (rc != 0 || !ast) {
        err << "Parse FAILED for: " << input_path << std::endl;
        return nullptr;
    }

    return ast;
}

void add_ast_pipeline(pass::PassManager &pm, const CommandLineOptions &options, std::ostream &out) {
    if (options.emit_ast) {
        pm.add_pass<pass::ASTDumpPass>(out);
    }
    if (needs_yir(options)) {
        pm.add_pass<pass::ASTSemanticAnalysisPass>();
        pm.add_pass<pass::ASTToYIRPass>();
        if (optimizations_enabled(options)) {
            pm.add_pass<pass::YIRLoopCanonicalizePass>();
            pm.add_pass<pass::YIRLoopOptimizationPass>();
            pm.add_pass<pass::YIRLoopAnalysisPass>();
        }
    }
}

void add_oir_pipeline(pass::PassManager &pm, const CommandLineOptions &options) {
    if (!needs_oir(options)) {
        return;
    }

    pm.add_pass<pass::YIRToOIRPass>();
    if (optimizations_enabled(options)) {
        pm.add_pass<pass::OIROptimizationPipelinePass>();
    }
}

void add_mir_pipeline(pass::PassManager &pm, const CommandLineOptions &options) {
    if (!needs_mir(options)) {
        return;
    }

    const bool emit_readable_prera_mir = options.emit_mir && !options.emit_asm;
    const bool use_virtual_registers = optimizations_enabled(options) || emit_readable_prera_mir;
    pm.add_pass<pass::OIRToMIRPass>(use_virtual_registers);

    if (optimizations_enabled(options)) {
        pm.add_pass<pass::MIRCombinePass>();
        pm.add_pass<pass::MIRPeepholePass>(false);
        pm.add_pass<pass::MIRRegAllocPass>();
        pm.add_pass<pass::MIRPeepholePass>(true);
    }
}

void add_asm_pipeline(pass::PassManager &pm, const CommandLineOptions &options) {
    if (options.emit_asm) {
        pm.add_pass<pass::MIRToAsmPass>();
    }
}

pass::PassManager build_compilation_pipeline(const CommandLineOptions &options, std::ostream &out) {
    pass::PassManager pm;
    add_ast_pipeline(pm, options, out);
    add_oir_pipeline(pm, options);
    add_mir_pipeline(pm, options);
    add_asm_pipeline(pm, options);
    return pm;
}

int run_pipeline(pass::PassManager &pm, pass::PassContext &context, std::ostream &err) {
    auto result = pm.run(context);
    if (result.success) {
        return 0;
    }

    const auto &execution = result.executions.back();
    err << "Pass FAILED: " << execution.name;
    if (!execution.result.message.empty()) {
        err << ": " << execution.result.message;
    }
    err << std::endl;
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    CommandLineOptions options;
    std::string error;
    if (!parse_command_line(argc, argv, options, error)) {
        std::cerr << "Error: " << error << "\n\n";
        print_help(argv[0], std::cerr);
        return 1;
    }

    if (options.show_help) {
        print_help(argv[0], std::cout);
        return 0;
    }

    auto ast = parse_ast_from_file(options.input_path, std::cerr);
    if (!ast) {
        return 1;
    }

    pass::PassContext context;
    context.set_ast(std::move(ast));

    std::ofstream output_file;
    std::ostream *out = &std::cout;
    if (!options.output_path.empty()) {
        output_file.open(options.output_path, std::ios::out | std::ios::trunc);
        if (!output_file) {
            std::cerr << "Cannot open output file: " << options.output_path << std::endl;
            return 1;
        }
        out = &output_file;
    }

    auto pm = build_compilation_pipeline(options, *out);
    if (pm.empty()) {
        return 0;
    }

    int rc = run_pipeline(pm, context, std::cerr);
    if (rc != 0) {
        return rc;
    }

    if (options.emit_yir) {
        auto *module =
            context.get_artifact<std::unique_ptr<yir::Module>>(pass::ASTToYIRPass::kArtifactKey);
        if (module == nullptr || *module == nullptr) {
            std::cerr << "YIR module was not produced\n";
            return 1;
        }
        yir::YIRPrinter printer(*out);
        printer.print(**module);
    }

    if (options.emit_oir) {
        auto *module = context.ssa_module();
        if (module == nullptr) {
            std::cerr << "OIR module was not produced\n";
            return 1;
        }
        *out << module->print();
    }

    if (options.emit_mir) {
        auto *module = context.machine_module();
        if (module == nullptr) {
            std::cerr << "MIR module was not produced\n";
            return 1;
        }
        mir::MIRPrinter printer(*out);
        printer.print(*module);
    }

    if (options.emit_asm) {
        auto *asm_text = context.get_artifact<std::string>(pass::MIRToAsmPass::kArtifactKey);
        if (asm_text == nullptr) {
            std::cerr << "Assembly was not produced\n";
            return 1;
        }
        *out << *asm_text;
    }

    return 0;
}
