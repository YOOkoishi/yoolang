#include "../../include/pass/OIRAlgebraicSimplifyPass.h"

#include "../../include/oir/OIRScalarOpt.h"

namespace pass {

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

} // namespace pass
