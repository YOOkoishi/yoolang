#include "../../include/pass/OIRAlgebraicSimplifyPass.h"
#include "../../include/pass/OIRConstantFoldPass.h"
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
            bool changed = oir_opt::local_simplify(module, stats, oir_opt::SimplifyMode::Algebraic);
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
                                          changed |= oir_opt::eliminate_dead_code(module, stats);
                                          return changed;
                                      });
}

} // namespace pass
