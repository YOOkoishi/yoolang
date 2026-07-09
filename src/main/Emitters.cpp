#include "main/Emitters.h"

#include "front/parser.h"
#include "mir/MIRPrinter.h"
#include "pass/ast/ASTToYIRPass.h"
#include "pass/CostModel.h"
#include "pass/CostModelDiagnosticsPass.h"
#include "pass/mir/MIRDiagnosticsPass.h"
#include "pass/mir/MIRToAsmPass.h"
#include "yir/YIRPrinter.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <vector>

extern FILE *lexer_input;

namespace driver {
namespace {

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

bool emit_requested_outputs(const CliOptions &options, pass::PassContext &context,
                            std::ostream &out, std::ostream &err) {
    if (options.emit_yir) {
        auto *module =
            context.get_artifact<std::unique_ptr<yir::Module>>(pass::ASTToYIRPass::kArtifactKey);
        if (module == nullptr || *module == nullptr) {
            err << "YIR module was not produced\n";
            return false;
        }
        yir::YIRPrinter printer(out);
        printer.print(**module);
    }

    if (options.emit_oir) {
        auto *module = context.ssa_module();
        if (module == nullptr) {
            err << "OIR module was not produced\n";
            return false;
        }
        out << module->print();
    }

    if (options.emit_mir) {
        if (!options.emit_mir_stage.empty()) {
            auto *stage_dump = context.get_artifact<std::string>(
                pass::MIRDiagnosticsPass::dump_artifact_key(options.emit_mir_stage));
            if (stage_dump == nullptr) {
                err << "MIR stage was not produced: " << options.emit_mir_stage << "\n";
                return false;
            }
            out << *stage_dump;
        } else {
            auto *module = context.machine_module();
            if (module == nullptr) {
                err << "MIR module was not produced\n";
                return false;
            }
            mir::MIRPrinter printer(out);
            printer.print(*module);
        }
    }

    if (options.emit_mir_metrics) {
        auto *metrics = context.get_artifact<std::vector<pass::MIRStageMetrics>>(
            pass::MIRDiagnosticsPass::kMetricsArtifactKey);
        if (metrics == nullptr) {
            err << "MIR metrics were not produced\n";
            return false;
        }
        print_mir_metrics_json(*metrics, out);
    }

    if (options.emit_cost_model) {
        auto *report = context.get_artifact<pass::cost_model::CostModelReport>(
            pass::CostModelDiagnosticsPass::kReportArtifactKey);
        if (report == nullptr) {
            err << "Cost model report was not produced\n";
            return false;
        }
        if (options.emit_cost_model_json) {
            pass::cost_model::print_report_json(*report, out);
        } else {
            pass::cost_model::print_report_text(*report, out);
        }
    }

    if (options.emit_asm) {
        auto *asm_text = context.get_artifact<std::string>(pass::MIRToAsmPass::kArtifactKey);
        if (asm_text == nullptr) {
            err << "Assembly was not produced\n";
            return false;
        }
        out << *asm_text;
    }

    return true;
}

} // namespace driver
