#include "front/parser.h"
#include "mir/MIRPrinter.h"
#include "pass/ast/ASTDumpPass.h"
#include "pass/ast/ASTSemanticAnalysisPass.h"
#include "pass/ast/ASTToYIRPass.h"
#include "pass/mir/MIRCombinePipelinePass.h"
#include "pass/mir/MIRDiagnosticsPass.h"
#include "pass/mir/MIRListSchedulerPass.h"
#include "pass/mir/MIRPeepholePipelinePass.h"
#include "pass/mir/MIRRegAllocPass.h"
#include "pass/mir/MIRToAsmPass.h"
#include "pass/oir/OIRAlgebraicSimplifyPass.h"
#include "pass/oir/OIRCFGCleanupPass.h"
#include "pass/oir/OIRConstantFoldPass.h"
#include "pass/oir/OIRDeadCodeEliminationPass.h"
#include "pass/oir/OIRGVNPass.h"
#include "pass/oir/OIRGlobalOptPass.h"
#include "pass/oir/OIRInlinePass.h"
#include "pass/oir/OIRLICMPass.h"
#include "pass/oir/OIRLoopStrengthReductionPass.h"
#include "pass/oir/OIRMem2RegPass.h"
#include "pass/oir/OIROptimizationPipelinePass.h"
#include "pass/oir/OIRSCCPPass.h"
#include "pass/oir/OIRTailRecursionEliminationPass.h"
#include "pass/oir/OIRToMIRPass.h"
#include "pass/PassManager.h"
#include "pass/yir/YIRLoopAnalysisPass.h"
#include "pass/yir/YIRLoopOptimizationPass.h"
#include "pass/yir/YIRPolyhedralDumpPass.h"
#include "pass/yir/YIRPolyhedralPipelinePass.h"
#include "pass/yir/YIRPolyhedralTransformPass.h"
#include "pass/yir/YIRToOIRPass.h"
#include "yir/YIRPrinter.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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
    bool emit_mir_metrics = false;
    bool emit_asm = false;
    bool emit_poly = false;
    bool enable_polyhedral = true;
    bool force_polyhedral = false;
    bool show_help = false;
    std::string emit_mir_stage;
};

bool is_valid_mir_stage(const std::string &stage) {
    return stage == "lowered" || stage == "post-combine" || stage == "pre-ra" ||
           stage == "post-ra" || stage == "final";
}

void print_help(const char *program, std::ostream &out) {
    out << "Usage: " << program << " <input.sysy> -S -o <output.s>\n"
        << "       " << program << " [options] <input.sy>\n\n"
        << "Options:\n"
        << "  -h, --help       Show this help message\n"
        << "  -S, --emit-asm   Lower to RISC-V assembly (default)\n"
        << "  -o <file>        Write output to <file> instead of stdout\n"
        << "  -O1              Enable OIR optimizations, vreg MIR lowering, RA, and MIR "
           "optimizations\n"
        << "  --polyhedral     Force the YIR polyhedral pipeline under -O1\n"
        << "  --emit-ast       Dump the parsed AST through the pass pipeline\n"
        << "  --emit-yir       Lower the parsed AST to YIR and dump it\n"
        << "  --emit-oir       Lower the parsed AST to SSA OIR, verify it, and dump it\n"
        << "  --emit-mir       Lower OIR to the RISC-V MIR and dump it\n"
        << "  --emit-mir-stage=<stage>\n"
        << "                   Dump MIR after lowered/post-combine/pre-ra/post-ra/final\n"
        << "  --emit-mir-metrics\n"
        << "                   Dump JSON MIR metrics for all recorded backend stages\n"
        << "  --emit-poly      Dump YIR polyhedral SCoP/model/dependence artifacts\n";
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
        const std::string mir_stage_prefix = "--emit-mir-stage=";
        if (arg.rfind(mir_stage_prefix, 0) == 0) {
            options.emit_mir = true;
            options.emit_mir_stage = arg.substr(mir_stage_prefix.size());
            if (!is_valid_mir_stage(options.emit_mir_stage)) {
                error = "unknown MIR stage: " + options.emit_mir_stage;
                return false;
            }
            continue;
        }
        if (arg == "--emit-mir-stage") {
            if (i + 1 >= argc) {
                error = "--emit-mir-stage requires a stage name";
                return false;
            }
            options.emit_mir = true;
            options.emit_mir_stage = argv[++i];
            if (!is_valid_mir_stage(options.emit_mir_stage)) {
                error = "unknown MIR stage: " + options.emit_mir_stage;
                return false;
            }
            continue;
        }
        if (arg == "--emit-mir-metrics") {
            options.emit_mir_metrics = true;
            continue;
        }
        if (arg == "--emit-poly") {
            options.emit_poly = true;
            continue;
        }
        if (arg == "-S" || arg == "--emit-asm") {
            options.emit_asm = true;
            continue;
        }
        if (arg == "--polyhedral") {
            options.enable_polyhedral = true;
            options.force_polyhedral = true;
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
        if (arg == "-O1") {
            options.opt_level = 1;
            continue;
        }
        if (arg == "-O2" || arg == "-O3") {
            error = "only -O1 is supported";
            return false;
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

    if (!options.show_help && options.emit_poly && options.opt_level != 1) {
        error = "--emit-poly requires -O1";
        return false;
    }

    if (!options.show_help && options.emit_mir_metrics &&
        (options.emit_ast || options.emit_yir || options.emit_oir || options.emit_mir ||
         options.emit_asm || options.emit_poly)) {
        error = "--emit-mir-metrics cannot be combined with other emit options";
        return false;
    }

    if (!options.show_help && !options.emit_ast && !options.emit_yir && !options.emit_oir &&
        !options.emit_mir && !options.emit_mir_metrics && !options.emit_asm &&
        !options.emit_poly) {
        options.emit_asm = true;
    }

    return true;
}

bool needs_yir(const CommandLineOptions &options) {
    return options.emit_yir || options.emit_oir || options.emit_mir || options.emit_asm ||
           options.emit_poly || options.emit_mir_metrics;
}

bool needs_oir(const CommandLineOptions &options) {
    return options.emit_oir || options.emit_mir || options.emit_asm || options.emit_mir_metrics;
}

bool needs_post_poly_yir_pipeline(const CommandLineOptions &options) {
    return options.emit_yir || options.emit_oir || options.emit_mir || options.emit_asm ||
           options.emit_mir_metrics;
}

bool needs_mir(const CommandLineOptions &options) {
    return options.emit_mir || options.emit_asm || options.emit_mir_metrics;
}

bool optimizations_enabled(const CommandLineOptions &options) {
    return options.opt_level == 1;
}

bool polyhedral_enabled(const CommandLineOptions &options) {
    return optimizations_enabled(options) && options.enable_polyhedral;
}

bool mir_diagnostics_enabled(const CommandLineOptions &options) {
    return options.emit_mir_metrics || !options.emit_mir_stage.empty();
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
            if (polyhedral_enabled(options)) {
                if (options.emit_poly) {
                    pm.add_pass<pass::YIRPolyhedralPipelinePass>(
                        pass::YIRPolyhedralPipelineMode::Force, false);
                    pm.add_pass<pass::YIRPolyhedralDumpPass>(out);
                    if (needs_post_poly_yir_pipeline(options)) {
                        pm.add_pass<pass::YIRPolyhedralTransformPass>();
                    }
                } else {
                    pm.add_pass<pass::YIRPolyhedralPipelinePass>(
                        options.force_polyhedral ? pass::YIRPolyhedralPipelineMode::Force
                                                 : pass::YIRPolyhedralPipelineMode::Auto);
                }
            }
            if (needs_post_poly_yir_pipeline(options)) {
                pm.add_pass<pass::YIRLoopOptimizationPass>();
                pm.add_pass<pass::YIRLoopAnalysisPass>();
            }
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
    const bool record_diagnostics = mir_diagnostics_enabled(options);
    pm.add_pass<pass::OIRToMIRPass>(use_virtual_registers);
    if (record_diagnostics) {
        pm.add_pass<pass::MIRDiagnosticsPass>("lowered", mir::MIRVerificationStage::PreRA);
    }

    if (optimizations_enabled(options)) {
        pm.add_pass<pass::MIRCombinePipelinePass>();
        if (record_diagnostics) {
            pm.add_pass<pass::MIRDiagnosticsPass>("post-combine",
                                                  mir::MIRVerificationStage::PreRA);
        }
        pm.add_pass<pass::MIRPeepholePipelinePass>(false);
        pm.add_pass<pass::MIRListSchedulerPass>(false);
        if (record_diagnostics) {
            pm.add_pass<pass::MIRDiagnosticsPass>("pre-ra", mir::MIRVerificationStage::PreRA);
        }
        pm.add_pass<pass::MIRRegAllocPass>();
        if (record_diagnostics) {
            pm.add_pass<pass::MIRDiagnosticsPass>("post-ra", mir::MIRVerificationStage::PostRA);
        }
        pm.add_pass<pass::MIRPeepholePipelinePass>(true);
        pm.add_pass<pass::MIRListSchedulerPass>(true);
        if (record_diagnostics) {
            pm.add_pass<pass::MIRDiagnosticsPass>("final", mir::MIRVerificationStage::PostRA);
        }
    } else if (record_diagnostics) {
        pm.add_pass<pass::MIRDiagnosticsPass>("final", mir::MIRVerificationStage::PreRA);
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

std::string json_escape(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += ch;
            break;
        }
    }
    return out;
}

void print_metric_field(std::ostream &out, const char *name, std::int64_t value, bool trailing) {
    out << "      \"" << name << "\": " << value;
    if (trailing) {
        out << ",";
    }
    out << "\n";
}

void print_mir_metrics_json(const std::vector<pass::MIRStageMetrics> &metrics,
                            std::ostream &out) {
    out << "{\n";
    out << "  \"mir_stage_metrics\": [\n";
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        const auto &stage = metrics[i];
        out << "    {\n";
        out << "      \"stage\": \"" << json_escape(stage.stage) << "\",\n";
        print_metric_field(out, "functions", stage.functions, true);
        print_metric_field(out, "basic_blocks", stage.basic_blocks, true);
        print_metric_field(out, "instructions", stage.instructions, true);
        print_metric_field(out, "moves", stage.moves, true);
        print_metric_field(out, "jumps", stage.jumps, true);
        print_metric_field(out, "branches", stage.branches, true);
        print_metric_field(out, "loads", stage.loads, true);
        print_metric_field(out, "stores", stage.stores, true);
        print_metric_field(out, "load_slots", stage.load_slots, true);
        print_metric_field(out, "store_slots", stage.store_slots, true);
        print_metric_field(out, "spills", stage.spills, true);
        print_metric_field(out, "stack_slots", stage.stack_slots, true);
        print_metric_field(out, "calls", stage.calls, false);
        out << "    }";
        if (i + 1 != metrics.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
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
        if (!options.emit_mir_stage.empty()) {
            auto *stage_dump = context.get_artifact<std::string>(
                pass::MIRDiagnosticsPass::dump_artifact_key(options.emit_mir_stage));
            if (stage_dump == nullptr) {
                std::cerr << "MIR stage was not produced: " << options.emit_mir_stage << "\n";
                return 1;
            }
            *out << *stage_dump;
        } else {
            auto *module = context.machine_module();
            if (module == nullptr) {
                std::cerr << "MIR module was not produced\n";
                return 1;
            }
            mir::MIRPrinter printer(*out);
            printer.print(*module);
        }
    }

    if (options.emit_mir_metrics) {
        auto *metrics = context.get_artifact<std::vector<pass::MIRStageMetrics>>(
            pass::MIRDiagnosticsPass::kMetricsArtifactKey);
        if (metrics == nullptr) {
            std::cerr << "MIR metrics were not produced\n";
            return 1;
        }
        print_mir_metrics_json(*metrics, *out);
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
