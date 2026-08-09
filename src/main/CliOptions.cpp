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
        << "  -O0              Disable automatic optimization (default)\n"
        << "  -O1              Enable scalar OIR/MIR optimizations\n"
        << "  -O2              Enable scalar optimization and Loop Vectorizer\n"
        << "  -O3              Enable Loop/SLP vectorization and interleave exploration\n"
        << "  -march=<arch>     Select RISC-V ISA (default: rv64gc)\n"
        << "  -mabi=<abi>       Select ABI (currently lp64d)\n"
        << "  -mcpu=<cpu>       Select target CPU (generic-rv64 or generic-rvv)\n"
        << "  -mtune=<cpu>      Select cost tuning (generic-rv64 or generic-rvv)\n"
        << "  -fvectorize / -fno-vectorize\n"
        << "                   Enable or disable Loop Vectorizer\n"
        << "  -fslp-vectorize / -fno-slp-vectorize\n"
        << "                   Enable or disable SLP Vectorizer\n"
        << "  -mrvv-vector-bits=scalable|N\n"
        << "                   Select a VLEN-agnostic or fixed VLEN profile\n"
        << "  -mrvv-deployment=scalar|compile-time|fat\n"
        << "                   Select scalar-only, compile-time RVV, or fat deployment\n"
        << "                   (fat emits rv64gc/RVV variants with runtime dispatch)\n"
        << "  -mvector-abi=standard|psabi-vector\n"
        << "                   Select the public vector calling convention\n"
        << "                   (psabi-vector is currently fail-closed pending fixed\n"
        << "                    vector tuples and GCC/Clang interoperability)\n"
        << "  -Rpass[=<filter>] Emit successful optimization remarks\n"
        << "  -Rpass-missed[=<filter>]\n"
        << "                   Emit missed optimization remarks\n"
        << "  --emit-vector-plan\n"
        << "                   Emit machine-readable loop-vectorization plans/remarks\n"
        << "  --polyhedral     Force the YIR polyhedral pipeline under optimization\n"
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
        << "                   Restrict cost-model decision diagnostics to a pass or transform "
           "name\n"
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
        if (arg == "--emit-vector-plan") {
            options.emit_vector_plan = true;
            continue;
        }
        if (arg == "-fvectorize") {
            options.loop_vectorize_override = true;
            continue;
        }
        if (arg == "-fno-vectorize") {
            options.loop_vectorize_override = false;
            continue;
        }
        if (arg == "-fslp-vectorize") {
            options.slp_vectorize_override = true;
            continue;
        }
        if (arg == "-fno-slp-vectorize") {
            options.slp_vectorize_override = false;
            continue;
        }
        if (arg == "-Rpass") {
            options.rpass = true;
            continue;
        }
        if (arg.rfind("-Rpass=", 0) == 0) {
            options.rpass = true;
            options.rpass_filter = arg.substr(std::string("-Rpass=").size());
            continue;
        }
        if (arg == "-Rpass-missed") {
            options.rpass_missed = true;
            continue;
        }
        if (arg.rfind("-Rpass-missed=", 0) == 0) {
            options.rpass_missed = true;
            options.rpass_missed_filter = arg.substr(std::string("-Rpass-missed=").size());
            continue;
        }
        const std::string march_prefix = "-march=";
        if (arg.rfind(march_prefix, 0) == 0) {
            options.target.march = arg.substr(march_prefix.size());
            options.target.march_explicit = true;
            continue;
        }
        if (arg == "-march") {
            if (i + 1 >= argc) {
                error = "-march requires an architecture";
                return false;
            }
            options.target.march = argv[++i];
            options.target.march_explicit = true;
            continue;
        }
        const std::string mabi_prefix = "-mabi=";
        if (arg.rfind(mabi_prefix, 0) == 0) {
            options.target.mabi = arg.substr(mabi_prefix.size());
            continue;
        }
        if (arg == "-mabi") {
            if (i + 1 >= argc) {
                error = "-mabi requires an ABI";
                return false;
            }
            options.target.mabi = argv[++i];
            continue;
        }
        const std::string mcpu_prefix = "-mcpu=";
        if (arg.rfind(mcpu_prefix, 0) == 0) {
            options.target.cpu = arg.substr(mcpu_prefix.size());
            options.target.cpu_explicit = true;
            continue;
        }
        if (arg == "-mcpu") {
            if (i + 1 >= argc) {
                error = "-mcpu requires a CPU name";
                return false;
            }
            options.target.cpu = argv[++i];
            options.target.cpu_explicit = true;
            continue;
        }
        const std::string mtune_prefix = "-mtune=";
        if (arg.rfind(mtune_prefix, 0) == 0) {
            options.target.tune = arg.substr(mtune_prefix.size());
            options.target.tune_explicit = true;
            continue;
        }
        if (arg == "-mtune") {
            if (i + 1 >= argc) {
                error = "-mtune requires a CPU name";
                return false;
            }
            options.target.tune = argv[++i];
            options.target.tune_explicit = true;
            continue;
        }
        const std::string target_prefix = "--target=";
        if (arg.rfind(target_prefix, 0) == 0) {
            options.target.triple = arg.substr(target_prefix.size());
            continue;
        }
        if (arg == "--target") {
            if (i + 1 >= argc) {
                error = "--target requires a triple";
                return false;
            }
            options.target.triple = argv[++i];
            continue;
        }
        const std::string deployment_prefix = "-mrvv-deployment=";
        if (arg.rfind(deployment_prefix, 0) == 0) {
            if (!target::parse_rvv_deployment(arg.substr(deployment_prefix.size()),
                                              options.target, error)) {
                return false;
            }
            continue;
        }
        if (arg == "-mrvv-deployment") {
            if (i + 1 >= argc) {
                error = "-mrvv-deployment requires scalar, compile-time, or fat";
                return false;
            }
            if (!target::parse_rvv_deployment(argv[++i], options.target, error)) {
                return false;
            }
            continue;
        }
        const std::string vector_bits_prefix = "-mrvv-vector-bits=";
        if (arg.rfind(vector_bits_prefix, 0) == 0) {
            if (!target::parse_vector_bits(arg.substr(vector_bits_prefix.size()), options.target,
                                           error)) {
                return false;
            }
            continue;
        }
        if (arg == "-mrvv-vector-bits") {
            if (i + 1 >= argc) {
                error = "-mrvv-vector-bits requires a value";
                return false;
            }
            if (!target::parse_vector_bits(argv[++i], options.target, error)) {
                return false;
            }
            continue;
        }
        const std::string vector_abi_prefix = "-mvector-abi=";
        if (arg.rfind(vector_abi_prefix, 0) == 0) {
            if (!target::parse_vector_abi(arg.substr(vector_abi_prefix.size()), options.target,
                                          error)) {
                return false;
            }
            continue;
        }
        if (arg == "-mvector-abi") {
            if (i + 1 >= argc) {
                error = "-mvector-abi requires a value";
                return false;
            }
            if (!target::parse_vector_abi(argv[++i], options.target, error)) {
                return false;
            }
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
        if (arg == "-O0" || arg == "-O1" || arg == "-O2" || arg == "-O3") {
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

    if (options.show_help) {
        return true;
    }

    if (options.input_path.empty()) {
        error = "missing input file";
        return false;
    }

    if (!target::finalize_target_profile(options.target, error)) {
        return false;
    }

    // O0 is a hard semantic pipeline boundary: explicit source vectors still
    // lower, but no automatic vectorizer runs even when an enabling flag was
    // also supplied.  At optimized levels the flags may override the default
    // O2/O3 thresholds without bypassing vectorization legality.
    options.loop_vectorize =
        options.opt_level != 0 && options.loop_vectorize_override.value_or(options.opt_level >= 2);
    options.slp_vectorize =
        options.opt_level != 0 && options.slp_vectorize_override.value_or(options.opt_level >= 3);

    if ((options.loop_vectorize || options.slp_vectorize) && !options.target.has_vector() &&
        options.target.deployment != target::DeploymentMode::Multiversion) {
        // Portable source vectors remain legal and scalarize on rv64gc.  Automatic
        // vectorization has no profitable target representation without V/Zve.
        options.loop_vectorize = false;
        options.slp_vectorize = false;
    }

    if (options.emit_poly && options.opt_level == 0) {
        error = "--emit-poly requires an optimization level of at least -O1";
        return false;
    }

    if (options.emit_mir_metrics && (options.emit_ast || options.emit_yir || options.emit_oir ||
                                     options.emit_mir || options.emit_asm || options.emit_poly)) {
        error = "--emit-mir-metrics cannot be combined with other emit options";
        return false;
    }

    if (options.emit_cost_model &&
        (options.emit_ast || options.emit_yir || options.emit_oir || options.emit_mir ||
         options.emit_mir_metrics || options.emit_asm || options.emit_poly ||
         options.emit_vector_plan)) {
        error = "--emit-cost-model cannot be combined with other emit options";
        return false;
    }

    if (!options.emit_ast && !options.emit_yir && !options.emit_oir && !options.emit_mir &&
        !options.emit_mir_metrics && !options.emit_cost_model && !options.emit_asm &&
        !options.emit_poly && !options.emit_vector_plan) {
        options.emit_asm = true;
    }

    if (options.target.deployment == target::DeploymentMode::Multiversion &&
        (!options.emit_asm || options.emit_ast || options.emit_yir || options.emit_oir ||
         options.emit_mir || options.emit_mir_metrics || options.emit_cost_model ||
         options.emit_poly || options.emit_vector_plan)) {
        error = "FAT_UNSUPPORTED_EMIT_MODE: fat deployment currently supports assembly "
                "output only";
        return false;
    }

    return true;
}

} // namespace driver
