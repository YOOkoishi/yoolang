#include "../../include/pass/OIRAlgebraicSimplifyPass.h"
#include "../../include/pass/OIRCFGCleanupPass.h"
#include "../../include/pass/OIRConstantFoldPass.h"
#include "../../include/pass/OIRDeadCodeEliminationPass.h"
#include "../../include/pass/OIRGVNPass.h"
#include "../../include/pass/OIRSCCPPass.h"

#include "../../include/oir/OIRScalarOpt.h"

namespace pass {

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
            bool changed =
                oir_opt::local_simplify(module, stats, oir_opt::SimplifyMode::Algebraic);
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
    return oir_opt::run_oir_transform(
        context, "OIRCFGCleanupPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::simplify_branches(module, stats);
            changed |= oir_opt::cleanup_cfg(module, stats);
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
