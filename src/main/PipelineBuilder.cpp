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
#include "pass/mir/MIRVectorStatePass.h"
#include "pass/mir/MIRPseudoExpansionPass.h"
#include "pass/mir/MIRToAsmPass.h"
#include "pass/oir/OIRFatMultiversionPass.h"
#include "pass/oir/OIRLoopVectorizerPass.h"
#include "pass/oir/OIROptimizationPipelinePass.h"
#include "pass/oir/OIRPortableVectorScalarizerPass.h"
#include "pass/oir/OIRSLPVectorizerPass.h"
#include "pass/oir/OIRToMIRPass.h"
#include "pass/oir/OIRVectorCleanupPass.h"
#include "pass/yir/YIRLoopAnalysisPass.h"
#include "pass/yir/YIRLoopOptimizationPass.h"
#include "pass/yir/YIRMemoryForwardingPass.h"
#include "pass/yir/YIRPolyhedralDumpPass.h"
#include "pass/yir/YIRPolyhedralPipelinePass.h"
#include "pass/yir/YIRToOIRPass.h"
#include "pass/yir/YIRViewPass.h"

#include <utility>

namespace driver {
namespace {

bool needs_yir(const CliOptions &options) {
    return options.emit_yir || options.emit_oir || options.emit_mir || options.emit_asm ||
           options.emit_poly || options.emit_mir_metrics || options.emit_cost_model ||
           options.emit_vector_plan;
}

bool needs_oir(const CliOptions &options) {
    return options.emit_oir || options.emit_mir || options.emit_asm || options.emit_mir_metrics ||
           options.emit_cost_model || options.emit_vector_plan;
}

bool needs_post_poly_yir_pipeline(const CliOptions &options) {
    return options.emit_yir || options.emit_oir || options.emit_mir || options.emit_asm ||
           options.emit_mir_metrics || options.emit_cost_model || options.emit_vector_plan;
}

bool needs_mir(const CliOptions &options) {
    return options.emit_mir || options.emit_asm || options.emit_mir_metrics ||
           options.emit_cost_model;
}

bool optimizations_enabled(const CliOptions &options) {
    return options.opt_level > 0;
}

bool polyhedral_enabled(const CliOptions &options) {
    return optimizations_enabled(options) && options.enable_polyhedral;
}

bool polyhedral_rvv_preparation_enabled(const CliOptions &options) {
    const bool vector_deployment =
        options.target.has_vector() ||
        options.target.deployment == target::DeploymentMode::Multiversion;
    const bool slp_handoff_allowed =
        !options.slp_vectorize_override.has_value() ||
        options.slp_vectorize_override.value();
    return polyhedral_enabled(options) && vector_deployment &&
           (options.loop_vectorize || options.slp_vectorize) &&
           slp_handoff_allowed;
}

bool mir_diagnostics_enabled(const CliOptions &options) {
    const bool preserve_readable_mir =
        options.emit_mir && !options.emit_asm && options.emit_mir_stage.empty();
    return options.emit_mir_metrics || !options.emit_mir_stage.empty() ||
           preserve_readable_mir;
}

bool cost_model_diagnostics_enabled(const CliOptions &options) {
    return options.emit_cost_model;
}

bool cost_model_active(const CliOptions &options) {
    return optimizations_enabled(options) || options.emit_cost_model;
}

void add_ast_pipeline(pass::PassManager &pm, const CliOptions &options, std::ostream &out) {
    const bool prepare_rvv = polyhedral_rvv_preparation_enabled(options);
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
                        pass::YIRPolyhedralPipelineMode::Force, false, prepare_rvv);
                    pm.add_pass<pass::YIRPolyhedralDumpPass>(out);
                    if (needs_post_poly_yir_pipeline(options)) {
                        pm.add_pass<pass::YIRPolyhedralPipelinePass>(
                            pass::YIRPolyhedralPipelineMode::Force, true, prepare_rvv);
                    }
                } else {
                    pm.add_pass<pass::YIRPolyhedralPipelinePass>(
                        options.force_polyhedral ? pass::YIRPolyhedralPipelineMode::Force
                                                 : pass::YIRPolyhedralPipelineMode::Auto,
                        true, prepare_rvv);
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
    if (options.target.deployment == target::DeploymentMode::Multiversion) {
        pass::OIRFatMultiversionOptions fat_options;
        fat_options.loop_vectorize = options.loop_vectorize;
        fat_options.slp_vectorize = options.slp_vectorize;
        fat_options.slp_polyhedral_rvv_preparation =
            polyhedral_rvv_preparation_enabled(options);
        fat_options.explore_interleave = options.opt_level >= 3;
        fat_options.optimize_mir = optimizations_enabled(options);
        pm.add_pass<pass::OIRFatMultiversionPass>(fat_options);
        return;
    }
    if (optimizations_enabled(options)) {
        pm.add_pass<pass::OIROptimizationPipelinePass>();
    }
    // OIR is target-independent and remains a faithful typed-vector
    // observation point.  Scalarize only when this compilation continues to
    // target-specific MIR; otherwise --emit-oir must retain vector/mask IR.
    if (needs_mir(options) && !options.target.has_vector()) {
        pm.add_pass<pass::OIRPortableVectorScalarizerPass>();
    }
    if (cost_model_diagnostics_enabled(options)) {
        pm.add_pass<pass::CostModelDiagnosticsPass>(pass::cost_model::CostIRStage::OIR, "oir",
                                                    options.cost_model_policy,
                                                    options.cost_model_filter);
    }
    bool produced_vector_ir = false;
    if (options.loop_vectorize) {
        pass::oir_vectorize::LoopVectorizerOptions vectorizer_options;
        vectorizer_options.enabled = true;
        vectorizer_options.explore_interleave = options.opt_level >= 3;
        pm.add_pass<pass::OIRLoopVectorizerPass>(vectorizer_options);
        produced_vector_ir = true;
    }
    const bool polyhedral_slp =
        polyhedral_rvv_preparation_enabled(options) && !options.slp_vectorize;
    if (options.slp_vectorize || polyhedral_slp) {
        pass::oir_vectorize::SLPVectorizerOptions vectorizer_options;
        vectorizer_options.enabled = true;
        pm.add_pass<pass::OIRSLPVectorizerPass>(vectorizer_options,
                                                polyhedral_slp);
        produced_vector_ir = true;
    }
    if (produced_vector_ir) {
        pm.add_pass<pass::OIRVectorCleanupPass>();
    }
}

void add_mir_pipeline(pass::PassManager &pm, const CliOptions &options) {
    if (!needs_mir(options) ||
        options.target.deployment == target::DeploymentMode::Multiversion) {
        return;
    }

    const bool record_diagnostics = mir_diagnostics_enabled(options);
    pm.add_pass<pass::OIRToMIRPass>();
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
    }

    if (record_diagnostics) {
        pm.add_pass<pass::MIRDiagnosticsPass>("pre-ra", mir::MIRVerificationStage::PreRA);
    }
    pm.add_pass<pass::MIRVectorRegAllocPass>();
    pm.add_pass<pass::MIRVectorStatePass>();
    pm.add_pass<pass::MIRScalarRegAllocPass>();
    if (record_diagnostics) {
        pm.add_pass<pass::MIRDiagnosticsPass>("post-ra", mir::MIRVerificationStage::PostRA);
    }

    if (optimizations_enabled(options)) {
        pm.add_pass<pass::MIRPeepholePipelinePass>(true);
        pm.add_pass<pass::MIRListSchedulerPass>(true);
    }

    pm.add_pass<pass::MIRPseudoExpansionPass>();

    if (record_diagnostics) {
        pm.add_pass<pass::MIRDiagnosticsPass>("final", mir::MIRVerificationStage::Final);
    }
    if (cost_model_diagnostics_enabled(options)) {
        pm.add_pass<pass::CostModelDiagnosticsPass>(
            pass::cost_model::CostIRStage::FinalMIR, "final-mir", options.cost_model_policy,
            options.cost_model_filter);
    }
}

void add_asm_pipeline(pass::PassManager &pm, const CliOptions &options) {
    if (options.emit_asm &&
        options.target.deployment != target::DeploymentMode::Multiversion) {
        pm.add_pass<pass::MIRToAsmPass>();
    }
}

} // namespace

void initialize_cost_model_report(pass::PassContext &context, const CliOptions &options) {
    if (!cost_model_active(options)) {
        return;
    }
    pass::cost_model::CostModelReport report;
    report.target = pass::cost_model::target_profile_for(options.target);
    report.policy = options.cost_model_policy;
    report.filter = options.cost_model_filter;
    context.set_artifact(pass::cost_model::kReportArtifactKey, std::move(report));
}

void initialize_target_machine(pass::PassContext &context, const CliOptions &options) {
    context.set_artifact(target::kTargetMachineArtifactKey,
                         target::TargetMachine(options.target));
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
