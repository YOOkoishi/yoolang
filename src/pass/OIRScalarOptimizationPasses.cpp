#include "../../include/pass/OIRAlgebraicSimplifyPass.h"
#include "../../include/pass/OIRCFGCleanupPass.h"
#include "../../include/pass/OIRConstantFoldPass.h"
#include "../../include/pass/OIRDeadCodeEliminationPass.h"
#include "../../include/pass/OIRADCEPass.h"
#include "../../include/pass/OIRDAEPass.h"
#include "../../include/pass/OIRGVNPass.h"
#include "../../include/pass/OIRGlobalOptPass.h"
#include "../../include/pass/OIRInlinePass.h"
#include "../../include/pass/OIRLICMPass.h"
#include "../../include/pass/OIRLoopCanonicalizePass.h"
#include "../../include/pass/OIRLoopStrengthReductionPass.h"
#include "../../include/pass/OIRMemoryOptPass.h"
#include "../../include/pass/OIRMem2RegPass.h"
#include "../../include/pass/OIROptimizationPipelinePass.h"
#include "../../include/pass/OIRSROAPass.h"
#include "../../include/pass/OIRSCCPPass.h"
#include "../../include/pass/OIRTailRecursionEliminationPass.h"

#include "../../include/oir/OIRScalarOpt.h"

namespace pass {

namespace {

bool run_aggressive_iteration(oir::Module &module, oir_opt::Stats &stats) {
    bool changed = false;
    changed |= oir_opt::canonicalize_loops(module, stats);
    changed |= oir_opt::propagate_global_constants(module, stats);
    changed |= oir_opt::simplify_branches(module, stats);
    changed |= oir_opt::cleanup_cfg(module, stats);
    changed |= oir_opt::promote_global_loads(module, stats);
    changed |= oir_opt::scalar_replacement_of_aggregates(module, stats);
    changed |= oir_opt::promote_memory_to_registers(module, stats);
    changed |= oir_opt::local_simplify(module, stats, oir_opt::SimplifyMode::ConstantFold);
    changed |= oir_opt::local_simplify(module, stats, oir_opt::SimplifyMode::Algebraic);
    changed |= oir_opt::run_sccp(module, stats);
    changed |= oir_opt::simplify_branches(module, stats);
    changed |= oir_opt::cleanup_cfg(module, stats);
    changed |= oir_opt::local_simplify(module, stats, oir_opt::SimplifyMode::ConstantFold);
    changed |= oir_opt::local_simplify(module, stats, oir_opt::SimplifyMode::Algebraic);
    changed |= oir_opt::global_value_numbering(module, stats);
    changed |= oir_opt::eliminate_dead_loads(module, stats);
    changed |= oir_opt::eliminate_dead_stores(module, stats);
    changed |= oir_opt::loop_invariant_code_motion(module, stats);
    changed |= oir_opt::reduce_gep_strength(module, stats);
    changed |= oir_opt::unswitch_loops(module, stats);
    changed |= oir_opt::rotate_loops(module, stats);
    changed |= oir_opt::reduce_gep_strength(module, stats);
    changed |= oir_opt::global_value_numbering(module, stats);
    changed |= oir_opt::eliminate_dead_loads(module, stats);
    changed |= oir_opt::eliminate_dead_stores(module, stats);
    changed |= oir_opt::run_sccp(module, stats);
    changed |= oir_opt::simplify_branches(module, stats);
    changed |= oir_opt::cleanup_cfg(module, stats);
    changed |= oir_opt::aggressive_dead_code_elimination(module, stats);
    changed |= oir_opt::jump_threading(module, stats);
    return changed;
}

} // namespace

namespace oir_opt {

bool optimize_oir_aggressively(oir::Module &module, Stats &stats) {
    bool changed = false;
    changed |= canonicalize_loops(module, stats);
    changed |= propagate_global_constants(module, stats);
    changed |= simplify_branches(module, stats);
    changed |= cleanup_cfg(module, stats);
    changed |= promote_global_loads(module, stats);
    changed |= scalar_replacement_of_aggregates(module, stats);
    changed |= promote_memory_to_registers(module, stats);
    changed |= local_simplify(module, stats, SimplifyMode::ConstantFold);
    changed |= local_simplify(module, stats, SimplifyMode::Algebraic);
    changed |= eliminate_tail_recursion(module, stats);
    changed |= propagate_global_constants(module, stats);
    changed |= inline_functions(module, stats);
    if (changed) {
        changed |= run_sccp(module, stats);
        changed |= global_value_numbering(module, stats);
        changed |= eliminate_dead_code(module, stats);
        changed |= cleanup_cfg(module, stats);
    }

    constexpr unsigned kMaxIterations = 8;
    for (unsigned iteration = 0; iteration < kMaxIterations; ++iteration) {
        if (!run_aggressive_iteration(module, stats)) {
            break;
        }
        changed = true;
    }

    changed |= simplify_branches(module, stats);
    changed |= cleanup_cfg(module, stats);
    changed |= reduce_gep_strength(module, stats);
    changed |= propagate_global_constants(module, stats);
    changed |= promote_global_loads(module, stats);
    changed |= scalar_replacement_of_aggregates(module, stats);
    changed |= promote_memory_to_registers(module, stats);
    changed |= eliminate_dead_loads(module, stats);
    changed |= eliminate_dead_stores(module, stats);
    changed |= aggressive_dead_code_elimination(module, stats);
    changed |= jump_threading(module, stats);
    changed |= eliminate_dead_arguments(module, stats);
    changed |= propagate_global_constants(module, stats);
    changed |= eliminate_dead_code(module, stats);
    return changed;
}

} // namespace oir_opt

std::string_view OIROptimizationPipelinePass::name() const {
    return "OIROptimizationPipelinePass";
}

PassKind OIROptimizationPipelinePass::kind() const {
    return PassKind::Transform;
}

PassResult OIROptimizationPipelinePass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIROptimizationPipelinePass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            return oir_opt::optimize_oir_aggressively(module, stats);
        });
}

std::string_view OIRConstantFoldPass::name() const {
    return "OIRConstantFoldPass";
}

PassKind OIRConstantFoldPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRConstantFoldPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRConstantFoldPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed =
                oir_opt::local_simplify(module, stats, oir_opt::SimplifyMode::ConstantFold);
            changed |= oir_opt::simplify_branches(module, stats);
            changed |= oir_opt::eliminate_dead_code(module, stats);
            return changed;
        });
}

std::string_view OIRAlgebraicSimplifyPass::name() const {
    return "OIRAlgebraicSimplifyPass";
}

PassKind OIRAlgebraicSimplifyPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRAlgebraicSimplifyPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRAlgebraicSimplifyPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::local_simplify(module, stats, oir_opt::SimplifyMode::Algebraic);
            changed |= oir_opt::eliminate_dead_code(module, stats);
            return changed;
        });
}

std::string_view OIRCFGCleanupPass::name() const {
    return "OIRCFGCleanupPass";
}

PassKind OIRCFGCleanupPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRCFGCleanupPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(context,
                                      "OIRCFGCleanupPass requires OIR module in pass context",
                                      [](oir::Module &module, oir_opt::Stats &stats) {
                                          bool changed = oir_opt::simplify_branches(module, stats);
                                          changed |= oir_opt::cleanup_cfg(module, stats);
                                          changed |= oir_opt::eliminate_dead_code(module, stats);
                                          return changed;
                                      });
}

std::string_view OIRMem2RegPass::name() const {
    return "OIRMem2RegPass";
}

PassKind OIRMem2RegPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRMem2RegPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(context, "OIRMem2RegPass requires OIR module in pass context",
                                      [](oir::Module &module, oir_opt::Stats &stats) {
                                          bool changed = oir_opt::scalar_replacement_of_aggregates(
                                              module, stats);
                                          changed |=
                                              oir_opt::promote_memory_to_registers(module, stats);
                                          changed |= oir_opt::cleanup_cfg(module, stats);
                                          changed |= oir_opt::eliminate_dead_code(module, stats);
                                          return changed;
                                      });
}

std::string_view OIRSROAPass::name() const {
    return "OIRSROAPass";
}

PassKind OIRSROAPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRSROAPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(context, "OIRSROAPass requires OIR module in pass context",
                                      [](oir::Module &module, oir_opt::Stats &stats) {
                                          bool changed =
                                              oir_opt::scalar_replacement_of_aggregates(module,
                                                                                        stats);
                                          changed |=
                                              oir_opt::promote_memory_to_registers(module, stats);
                                          changed |= oir_opt::eliminate_dead_code(module, stats);
                                          return changed;
                                      });
}

std::string_view OIRLoopCanonicalizePass::name() const {
    return "OIRLoopCanonicalizePass";
}

PassKind OIRLoopCanonicalizePass::kind() const {
    return PassKind::Transform;
}

PassResult OIRLoopCanonicalizePass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRLoopCanonicalizePass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::canonicalize_loops(module, stats);
            changed |= oir_opt::cleanup_cfg(module, stats);
            return changed;
        });
}

std::string_view OIRLICMPass::name() const {
    return "OIRLICMPass";
}

PassKind OIRLICMPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRLICMPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(context, "OIRLICMPass requires OIR module in pass context",
                                      [](oir::Module &module, oir_opt::Stats &stats) {
                                          bool changed =
                                              oir_opt::loop_invariant_code_motion(module, stats);
                                          changed |= oir_opt::eliminate_dead_code(module, stats);
                                          return changed;
                                      });
}

std::string_view OIRLoopStrengthReductionPass::name() const {
    return "OIRLoopStrengthReductionPass";
}

PassKind OIRLoopStrengthReductionPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRLoopStrengthReductionPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRLoopStrengthReductionPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::reduce_gep_strength(module, stats);
            changed |= oir_opt::global_value_numbering(module, stats);
            changed |= oir_opt::eliminate_dead_code(module, stats);
            return changed;
        });
}

std::string_view OIRGVNPass::name() const {
    return "OIRGVNPass";
}

PassKind OIRGVNPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRGVNPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(context, "OIRGVNPass requires OIR module in pass context",
                                      [](oir::Module &module, oir_opt::Stats &stats) {
                                          bool changed =
                                              oir_opt::global_value_numbering(module, stats);
                                          changed |= oir_opt::eliminate_dead_code(module, stats);
                                          return changed;
                                      });
}

std::string_view OIRDeadStoreEliminationPass::name() const {
    return "OIRDeadStoreEliminationPass";
}

PassKind OIRDeadStoreEliminationPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRDeadStoreEliminationPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRDeadStoreEliminationPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::eliminate_dead_stores(module, stats);
            changed |= oir_opt::eliminate_dead_code(module, stats);
            return changed;
        });
}

std::string_view OIRDeadLoadEliminationPass::name() const {
    return "OIRDeadLoadEliminationPass";
}

PassKind OIRDeadLoadEliminationPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRDeadLoadEliminationPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRDeadLoadEliminationPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::eliminate_dead_loads(module, stats);
            changed |= oir_opt::eliminate_dead_code(module, stats);
            return changed;
        });
}

std::string_view OIRSCCPPass::name() const {
    return "OIRSCCPPass";
}

PassKind OIRSCCPPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRSCCPPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(context, "OIRSCCPPass requires OIR module in pass context",
                                      [](oir::Module &module, oir_opt::Stats &stats) {
                                          bool changed = oir_opt::run_sccp(module, stats);
                                          changed |= oir_opt::simplify_branches(module, stats);
                                          changed |= oir_opt::cleanup_cfg(module, stats);
                                          changed |= oir_opt::eliminate_dead_code(module, stats);
                                          return changed;
                                      });
}

std::string_view OIRInlinePass::name() const {
    return "OIRInlinePass";
}

PassKind OIRInlinePass::kind() const {
    return PassKind::Transform;
}

PassResult OIRInlinePass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRInlinePass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::inline_functions(module, stats);
            if (changed) {
                changed |= oir_opt::run_sccp(module, stats);
                changed |= oir_opt::global_value_numbering(module, stats);
            }
            changed |= oir_opt::simplify_branches(module, stats);
            changed |= oir_opt::cleanup_cfg(module, stats);
            changed |= oir_opt::eliminate_dead_code(module, stats);
            return changed;
        });
}

std::string_view OIRGlobalOptPass::name() const {
    return "OIRGlobalOptPass";
}

PassKind OIRGlobalOptPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRGlobalOptPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(context,
                                      "OIRGlobalOptPass requires OIR module in pass context",
                                      [](oir::Module &module, oir_opt::Stats &stats) {
                                          bool changed =
                                              oir_opt::propagate_global_constants(module, stats);
                                          changed |= oir_opt::promote_global_loads(module, stats);
                                          changed |=
                                              oir_opt::scalar_replacement_of_aggregates(module,
                                                                                        stats);
                                          changed |=
                                              oir_opt::promote_memory_to_registers(module, stats);
                                          changed |= oir_opt::eliminate_dead_code(module, stats);
                                          return changed;
                                      });
}

std::string_view OIRADCEPass::name() const {
    return "OIRADCEPass";
}

PassKind OIRADCEPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRADCEPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(context, "OIRADCEPass requires OIR module in pass context",
                                      [](oir::Module &module, oir_opt::Stats &stats) {
                                          bool changed =
                                              oir_opt::aggressive_dead_code_elimination(module,
                                                                                        stats);
                                          changed |= oir_opt::cleanup_cfg(module, stats);
                                          return changed;
                                      });
}

std::string_view OIRJumpThreadingPass::name() const {
    return "OIRJumpThreadingPass";
}

PassKind OIRJumpThreadingPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRJumpThreadingPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRJumpThreadingPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::jump_threading(module, stats);
            changed |= oir_opt::simplify_branches(module, stats);
            changed |= oir_opt::cleanup_cfg(module, stats);
            changed |= oir_opt::aggressive_dead_code_elimination(module, stats);
            return changed;
        });
}

std::string_view OIRDAEPass::name() const {
    return "OIRDAEPass";
}

PassKind OIRDAEPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRDAEPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(context, "OIRDAEPass requires OIR module in pass context",
                                      [](oir::Module &module, oir_opt::Stats &stats) {
                                          return oir_opt::eliminate_dead_arguments(module, stats);
                                      });
}

std::string_view OIRTailRecursionEliminationPass::name() const {
    return "OIRTailRecursionEliminationPass";
}

PassKind OIRTailRecursionEliminationPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRTailRecursionEliminationPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRTailRecursionEliminationPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::eliminate_tail_recursion(module, stats);
            changed |= oir_opt::simplify_branches(module, stats);
            changed |= oir_opt::cleanup_cfg(module, stats);
            changed |= oir_opt::eliminate_dead_code(module, stats);
            return changed;
        });
}

std::string_view OIRDeadCodeEliminationPass::name() const {
    return "OIRDeadCodeEliminationPass";
}

PassKind OIRDeadCodeEliminationPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRDeadCodeEliminationPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRDeadCodeEliminationPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            return oir_opt::eliminate_dead_code(module, stats);
        });
}

} // namespace pass
