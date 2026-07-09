#include "main/CliOptions.h"

#include <ostream>
#include <utility>

namespace driver {
namespace {

bool is_valid_mir_stage(const std::string &stage) {
    return stage == "lowered" || stage == "post-combine" || stage == "pre-ra" ||
           stage == "post-ra" || stage == "final";
}

} // namespace

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
        << "  --emit-cost-model[=json]\n"
        << "                   Dump cost-model summaries and decisions without emitting assembly\n"
        << "  --cost-model-filter=<pass-or-transform>\n"
        << "                   Restrict cost-model decision diagnostics to a pass or transform name\n"
        << "  --cost-model-policy=conservative|balanced|aggressive\n"
        << "                   Select the cost-model profitability policy (default: balanced)\n"
        << "  --emit-poly      Dump YIR polyhedral SCoP/model/dependence artifacts\n";
}

bool parse_command_line(int argc, char **argv, CliOptions &options, std::string &error) {
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
        if (arg == "--emit-cost-model") {
            options.emit_cost_model = true;
            continue;
        }
        const std::string emit_cost_model_prefix = "--emit-cost-model=";
        if (arg.rfind(emit_cost_model_prefix, 0) == 0) {
            const auto mode = arg.substr(emit_cost_model_prefix.size());
            if (mode != "json" && mode != "text") {
                error = "unknown cost-model emit mode: " + mode;
                return false;
            }
            options.emit_cost_model = true;
            options.emit_cost_model_json = mode == "json";
            continue;
        }
        const std::string cost_model_filter_prefix = "--cost-model-filter=";
        if (arg.rfind(cost_model_filter_prefix, 0) == 0) {
            options.cost_model_filter = arg.substr(cost_model_filter_prefix.size());
            continue;
        }
        if (arg == "--cost-model-filter") {
            if (i + 1 >= argc) {
                error = "--cost-model-filter requires a pass or transform name";
                return false;
            }
            options.cost_model_filter = argv[++i];
            continue;
        }
        const std::string cost_model_policy_prefix = "--cost-model-policy=";
        if (arg.rfind(cost_model_policy_prefix, 0) == 0) {
            const auto policy = arg.substr(cost_model_policy_prefix.size());
            if (!pass::cost_model::parse_policy_kind(policy, options.cost_model_policy)) {
                error = "unknown cost-model policy: " + policy;
                return false;
            }
            continue;
        }
        if (arg == "--cost-model-policy") {
            if (i + 1 >= argc) {
                error = "--cost-model-policy requires conservative, balanced, or aggressive";
                return false;
            }
            const std::string policy = argv[++i];
            if (!pass::cost_model::parse_policy_kind(policy, options.cost_model_policy)) {
                error = "unknown cost-model policy: " + policy;
                return false;
            }
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

    if (!options.show_help && options.emit_cost_model &&
        (options.emit_ast || options.emit_yir || options.emit_oir || options.emit_mir ||
         options.emit_mir_metrics || options.emit_asm || options.emit_poly)) {
        error = "--emit-cost-model cannot be combined with other emit options";
        return false;
    }

    if (!options.show_help && !options.emit_ast && !options.emit_yir && !options.emit_oir &&
        !options.emit_mir && !options.emit_mir_metrics && !options.emit_cost_model &&
        !options.emit_asm && !options.emit_poly) {
        options.emit_asm = true;
    }

    return true;
}

} // namespace driver
