#include "pass/oir/OIRSLPVectorizerPass.h"

#include "pass/yir/YIRPolyhedralTransformPass.h"
#include "target/TargetMachine.h"

#include <exception>
#include <utility>

namespace pass {

OIRSLPVectorizerPass::OIRSLPVectorizerPass(
    oir_vectorize::SLPVectorizerOptions options,
    bool require_polyhedral_rvv_preparation)
    : options_(std::move(options)),
      require_polyhedral_rvv_preparation_(require_polyhedral_rvv_preparation) {
}

std::string_view OIRSLPVectorizerPass::name() const {
    return "OIRSLPVectorizerPass";
}

PassKind OIRSLPVectorizerPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRSLPVectorizerPass::run(PassContext &context) {
    auto *module = context.ssa_module();
    if (module == nullptr) {
        return PassResult::fail("OIRSLPVectorizerPass requires OIR module in pass context");
    }
    if (require_polyhedral_rvv_preparation_) {
        const auto *summary = context.get_artifact<YIRPolyhedralTransformSummary>(
            std::string(YIRPolyhedralTransformSummary::kArtifactKey));
        if (summary == nullptr || summary->rvv_preparations == 0) {
            return PassResult::ok(false,
                                  "skipped: no polyhedral RVV preparation");
        }
    }
    const auto *target_machine =
        context.get_artifact<target::TargetMachine>(target::kTargetMachineArtifactKey);
    if (target_machine == nullptr) {
        return PassResult::fail(
            "OIRSLPVectorizerPass requires target.machine artifact in pass context");
    }

    auto *remarks =
        context.get_artifact<oir_vectorize::RemarkLog>(oir_vectorize::kRemarkArtifactKey);
    if (remarks == nullptr) {
        context.set_artifact(oir_vectorize::kRemarkArtifactKey, oir_vectorize::RemarkLog{});
        remarks = context.get_artifact<oir_vectorize::RemarkLog>(oir_vectorize::kRemarkArtifactKey);
    }

    try {
        auto result = oir_vectorize::SLPVectorizer(options_).run(*module, target_machine->profile(),
                                                                 *remarks);
        if (!result.success) {
            return PassResult::fail(result.message.empty() ? "OIR SLP vectorization failed"
                                                           : std::move(result.message));
        }
        if (result.changed)
            context.invalidate_oir_analyses();
        return PassResult::ok(result.changed,
                              "vectorized_slp_packs=" + std::to_string(result.packs_vectorized) +
                                  ", scalar_instructions_replaced=" +
                                  std::to_string(result.scalar_instructions_replaced));
    } catch (const std::exception &error) {
        return PassResult::fail(error.what());
    }
}

} // namespace pass
