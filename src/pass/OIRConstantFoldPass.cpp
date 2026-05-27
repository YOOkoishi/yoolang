#include "../../include/pass/OIRConstantFoldPass.h"

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

} // namespace pass
