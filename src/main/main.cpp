#include "../include/front/parser.h"
#include "../include/pass/ASTDumpPass.h"
#include "../include/pass/PassManager.h"

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

extern FILE *lexer_input;

namespace {

struct CommandLineOptions {
    std::string input_path;
    bool emit_ast = false;
    bool show_help = false;
};

void print_help(const char *program, std::ostream &out) {
    out << "Usage: " << program << " [options] <input.sy>\n\n"
        << "Options:\n"
        << "  -h, --help    Show this help message\n"
        << "  --emit-ast    Dump the parsed AST through the pass pipeline\n";
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

    return true;
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

pass::PassManager build_frontend_pipeline(const CommandLineOptions &options, std::ostream &out) {
    pass::PassManager pm;
    if (options.emit_ast) {
        pm.emplace_pass<pass::ASTDumpPass>(out);
    }
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

    auto pm = build_frontend_pipeline(options, std::cout);
    if (pm.empty()) {
        return 0;
    }

    return run_pipeline(pm, context, std::cerr);
}
