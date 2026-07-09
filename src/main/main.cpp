#include "main/CliOptions.h"
#include "main/Emitters.h"
#include "main/PipelineBuilder.h"

#include <fstream>
#include <iostream>
#include <utility>

int main(int argc, char **argv) {
    driver::CliOptions options;
    std::string error;
    if (!driver::parse_command_line(argc, argv, options, error)) {
        std::cerr << "Error: " << error << "\n\n";
        driver::print_help(argv[0], std::cerr);
        return 1;
    }

    if (options.show_help) {
        driver::print_help(argv[0], std::cout);
        return 0;
    }

    auto ast = driver::parse_ast_from_file(options.input_path, std::cerr);
    if (!ast) {
        return 1;
    }

    pass::PassContext context;
    context.set_ast(std::move(ast));
    driver::initialize_cost_model_report(context, options);

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

    auto pm = driver::build_compilation_pipeline(options, *out);
    if (pm.empty()) {
        return 0;
    }

    int rc = driver::run_pipeline(pm, context, std::cerr);
    if (rc != 0) {
        return rc;
    }

    return driver::emit_requested_outputs(options, context, *out, std::cerr) ? 0 : 1;
}
