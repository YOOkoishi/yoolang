#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "front/parser.h"
#include "front/parser_tokens.h"
#include "include.h"
#include "liveness_analysis.h"
#include "pass/PassManager.h"
#include "pass/ast/ASTSemanticAnalysisPass.h"
#include "pass/ast/ASTToYIRPass.h"
#include "pass/yir/YIRLoopAnalysisPass.h"
#include "pass/yir/YIRLoopOptimizationPass.h"
#include "pass/yir/YIRMemoryForwardingPass.h"
#include "pass/yir/YIRPolyhedralPipelinePass.h"
#include "pass/yir/YIRViewPass.h"
#include "pipeline_diff.h"
#include "yir/YIR.h"
#include "yir/YIRPrinter.h"

extern FILE *lexer_input;

// Reset the lexer's internal state between parses
void reset_lexer_state();

namespace {

struct StageCapture {
    std::string name;
    yir_analysis::ModuleLiveness liveness;
};

yir_analysis::PipelineStageInfo liveness_to_info(
    const std::string &stage_name, const yir_analysis::ModuleLiveness &liveness) {
    yir_analysis::PipelineStageInfo info;
    info.stage_name = stage_name;
    info.total_live_ranges = 0;
    info.total_blocks = 0;

    for (const auto &func : liveness.functions) {
        for (const auto &block : func.blocks) {
            info.total_live_ranges +=
                static_cast<int>(block.live_in.size() + block.live_out.size());
            info.live_in_counts_per_block.push_back(
                static_cast<int>(block.live_in.size()));
            ++info.total_blocks;
        }
    }
    return info;
}

std::unique_ptr<CompUnit> parse_ast(const std::string &path, std::ostream &err) {
    FILE *input = std::fopen(path.c_str(), "r");
    if (input == nullptr) {
        err << "Cannot open file: " << path << std::endl;
        return nullptr;
    }

    lexer_input = input;
    std::unique_ptr<CompUnit> ast;
    int rc = parse(ast);
    std::fclose(input);
    lexer_input = nullptr;

    if (rc != 0 || !ast) {
        err << "Parse FAILED for: " << path << " (rc=" << rc << ")" << std::endl;
        return nullptr;
    }

    return ast;
}

bool run_pipeline_for_stages(const std::string &input_path,
                              std::vector<StageCapture> &stages, std::ostream &err) {
    for (int mode = 0; mode < 2; ++mode) {
        reset_lexer_state();
        auto ast = parse_ast(input_path, err);
        if (!ast) {
            return false;
        }

        pass::PassContext context;
        context.set_ast(std::move(ast));

        if (mode == 0) {
            pass::PassManager pm;
            pm.add_pass<pass::ASTSemanticAnalysisPass>();
            pm.add_pass<pass::ASTToYIRPass>();
            auto result = pm.run(context);
            if (!result.success) {
                err << "Failed to generate raw YIR" << std::endl;
                return false;
            }

            auto *module = context.get_artifact<std::unique_ptr<yir::Module>>(
                pass::ASTToYIRPass::kArtifactKey);
            if (module == nullptr || *module == nullptr) {
                err << "Raw YIR module was not produced" << std::endl;
                return false;
            }

            StageCapture stage;
            stage.name = "raw-yir";
            stage.liveness = yir_analysis::compute_yir_liveness(**module);
            stages.push_back(std::move(stage));
        } else {
            pass::PassManager pm;
            pm.add_pass<pass::ASTSemanticAnalysisPass>();
            pm.add_pass<pass::ASTToYIRPass>();
            pm.add_pass<pass::YIRViewPass>();
            pm.add_pass<pass::YIRPolyhedralPipelinePass>(
                pass::YIRPolyhedralPipelineMode::Auto);
            pm.add_pass<pass::YIRMemoryForwardingPass>();
            pm.add_pass<pass::YIRLoopOptimizationPass>();
            pm.add_pass<pass::YIRLoopAnalysisPass>();

            auto result = pm.run(context);
            if (!result.success) {
                err << "Failed to run optimized YIR pipeline" << std::endl;
                return false;
            }

            auto *module = context.get_artifact<std::unique_ptr<yir::Module>>(
                pass::ASTToYIRPass::kArtifactKey);
            if (module == nullptr || *module == nullptr) {
                err << "Optimized YIR module was not produced" << std::endl;
                return false;
            }

            StageCapture stage;
            stage.name = "post-yir-opt";
            stage.liveness = yir_analysis::compute_yir_liveness(**module);
            stages.push_back(std::move(stage));
        }
    }

    return true;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: yir-pipeline-analyzer <input.sy>" << std::endl;
        std::cerr << std::endl;
        std::cerr << "  Runs yoolang's YIR pipeline at multiple stages and computes liveness"
                  << std::endl;
        std::cerr << "  analysis at each stage. Reports how live ranges change across stages"
                  << std::endl;
        std::cerr << "  to detect pass conflicts and information loss." << std::endl;
        std::cerr << std::endl;
        std::cerr << "  Output: JSON liveness data for each stage, followed by a diff report."
                  << std::endl;
        return 1;
    }

    std::string input_path = argv[1];

    std::vector<StageCapture> stages;
    if (!run_pipeline_for_stages(input_path, stages, std::cerr)) {
        return 1;
    }

    for (const auto &stage : stages) {
        std::cout << "=== " << stage.name << " liveness ===" << std::endl;
        std::cout << yir_analysis::liveness_to_json(stage.liveness) << std::endl;
    }

    std::vector<yir_analysis::PipelineStageInfo> stage_infos;
    for (const auto &stage : stages) {
        stage_infos.push_back(liveness_to_info(stage.name, stage.liveness));
    }

    auto diffs = yir_analysis::compute_pipeline_diffs(stage_infos);
    std::cout << yir_analysis::diffs_to_report(diffs) << std::endl;

    return 0;
}
