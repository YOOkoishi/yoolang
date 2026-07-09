#include "main/PipelineBuilder.h"

#include "pass/ast/ASTDumpPass.h"
#include "pass/ast/ASTSemanticAnalysisPass.h"
#include "pass/ast/ASTToYIRPass.h"
#include "pass/CostModel.h"
#include "pass/CostModelDiagnosticsPass.h"
#include "pass/mir/MIRCombinePipelinePass.h"
#include "pass/mir/MIRDiagnosticsPass.h"
#include "pass/mir/MIRListSchedulerPass.h"
#include "pass/mir/MIRPeepholePipelinePass.h"
#include "pass/mir/MIRRegAllocPass.h"
#include "pass/mir/MIRToAsmPass.h"
#include "pass/oir/OIROptimizationPipelinePass.h"
#include "pass/oir/OIRToMIRPass.h"
#include "pass/yir/YIRLoopAnalysisPass.h"
#include "pass/yir/YIRLoopOptimizationPass.h"
#include "pass/yir/YIRMemoryForwardingPass.h"
#include "pass/yir/YIRPolyhedralDumpPass.h"
#include "pass/yir/YIRPolyhedralPipelinePass.h"
#include "pass/yir/YIRPolyhedralTransformPass.h"
#include "pass/yir/YIRToOIRPass.h"
#include "pass/yir/YIRViewPass.h"

#include <utility>

namespace driver {
namespace {

bool needs_yir(const CliOptions &options) {
    return options.emit_yir || options.emit_oir || options.emit_mir || options.emit_asm ||
           options.emit_poly || options.emit_mir_metrics || options.emit_cost_model;
}

bool needs_oir(const CliOptions &options) {
    return options.emit_oir || options.emit_mir || options.emit_asm || options.emit_mir_metrics ||
           options.emit_cost_model;
}

bool needs_post_poly_yir_pipeline(const CliOptions &options) {
    return options.emit_yir || options.emit_oir || options.emit_mir || options.emit_asm ||
           options.emit_mir_metrics || options.emit_cost_model;
}

bool needs_mir(const CliOptions &options) {
    return options.emit_mir || options.emit_asm || options.emit_mir_metrics ||
           options.emit_cost_model;
}

bool optimizations_enabled(const CliOptions &options) {
    return options.opt_level == 1;
}

bool polyhedral_enabled(const CliOptions &options) {
    return optimizations_enabled(options) && options.enable_polyhedral;
}

bool mir_diagnostics_enabled(const CliOptions &options) {
    return options.emit_mir_metrics || !options.emit_mir_stage.empty();
}

bool cost_model_diagnostics_enabled(const CliOptions &options) {
    return options.emit_cost_model;
}

bool cost_model_active(const CliOptions &options) {
    return optimizations_enabled(options) || options.emit_cost_model;
}

void add_ast_pipeline(pass::PassManager &pm, const CliOptions &options, std::ostream &out) {
    if (options.emit_ast) {
        pm.add_pass<pass::ASTDumpPass>(out);
    }
    if (needs_yir(options)) {
        pm.add_pass<pass::ASTSemanticAnalysisPass>();
        pm.add_pass<pass::ASTToYIRPass>();
        if (optimizations_enabled(options)) {
            pm.add_pass<pass::YIRViewPass>();
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
                pm.add_pass<pass::YIRMemoryForwardingPass>();
                pm.add_pass<pass::YIRLoopOptimizationPass>();
                pm.add_pass<pass::YIRLoopAnalysisPass>();
            }
        }
    }
}

void add_oir_pipeline(pass::PassManager &pm, const CliOptions &options) {
    if (!needs_oir(options)) {
        return;
    }

    pm.add_pass<pass::YIRToOIRPass>();
    if (cost_model_diagnostics_enabled(options)) {
        pm.add_pass<pass::CostModelDiagnosticsPass>(
            pass::cost_model::CostIRStage::OIR, "oir-before", options.cost_model_policy,
            options.cost_model_filter);
    }
    if (optimizations_enabled(options)) {
        pm.add_pass<pass::OIROptimizationPipelinePass>();
    }
    if (cost_model_diagnostics_enabled(options)) {
        pm.add_pass<pass::CostModelDiagnosticsPass>(pass::cost_model::CostIRStage::OIR, "oir",
                                                    options.cost_model_policy,
                                                    options.cost_model_filter);
    }
}

void add_mir_pipeline(pass::PassManager &pm, const CliOptions &options) {
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
        if (cost_model_diagnostics_enabled(options)) {
            pm.add_pass<pass::CostModelDiagnosticsPass>(
                pass::cost_model::CostIRStage::FinalMIR, "final-mir",
                options.cost_model_policy, options.cost_model_filter);
        }
    } else if (record_diagnostics) {
        pm.add_pass<pass::MIRDiagnosticsPass>("final", mir::MIRVerificationStage::PreRA);
    } else if (cost_model_diagnostics_enabled(options)) {
        pm.add_pass<pass::CostModelDiagnosticsPass>(
            pass::cost_model::CostIRStage::PreRAMIR, "lowered-mir", options.cost_model_policy,
            options.cost_model_filter);
    }
}

void add_asm_pipeline(pass::PassManager &pm, const CliOptions &options) {
    if (options.emit_asm) {
        pm.add_pass<pass::MIRToAsmPass>();
    }
}

} // namespace

void initialize_cost_model_report(pass::PassContext &context, const CliOptions &options) {
    if (!cost_model_active(options)) {
        return;
    }
    pass::cost_model::CostModelReport report;
    report.target = pass::cost_model::default_target_profile();
    report.policy = options.cost_model_policy;
    report.filter = options.cost_model_filter;
    context.set_artifact(pass::cost_model::kReportArtifactKey, std::move(report));
}

pass::PassManager build_compilation_pipeline(const CliOptions &options, std::ostream &out) {
    pass::PassManager pm;
    add_ast_pipeline(pm, options, out);
    add_oir_pipeline(pm, options);
    add_mir_pipeline(pm, options);
    add_asm_pipeline(pm, options);
    return pm;
}

} // namespace driver
