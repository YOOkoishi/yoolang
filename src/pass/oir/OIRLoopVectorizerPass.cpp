#include "pass/oir/OIRLoopVectorizerPass.h"

#include "target/TargetMachine.h"

#include <exception>
#include <utility>

namespace pass {

OIRLoopVectorizerPass::OIRLoopVectorizerPass(
    oir_vectorize::LoopVectorizerOptions options)
    : options_(std::move(options)) {
}

std::string_view OIRLoopVectorizerPass::name() const {
    return "OIRLoopVectorizerPass";
}

PassKind OIRLoopVectorizerPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRLoopVectorizerPass::run(PassContext &context) {
    auto *module = context.ssa_module();
    if (module == nullptr) {
        return PassResult::fail("OIRLoopVectorizerPass requires OIR module in pass context");
    }
    const auto *target_machine = context.get_artifact<target::TargetMachine>(
        target::kTargetMachineArtifactKey);
    if (target_machine == nullptr) {
        return PassResult::fail(
            "OIRLoopVectorizerPass requires target.machine artifact in pass context");
    }

    auto *remarks = context.get_artifact<oir_vectorize::RemarkLog>(
        oir_vectorize::kRemarkArtifactKey);
    if (remarks == nullptr) {
        context.set_artifact(oir_vectorize::kRemarkArtifactKey,
                             oir_vectorize::RemarkLog{});
        remarks = context.get_artifact<oir_vectorize::RemarkLog>(
            oir_vectorize::kRemarkArtifactKey);
    }

    try {
        auto result = oir_vectorize::LoopVectorizer(options_).run(
            *module, target_machine->profile(), *remarks);
        if (!result.success) {
            return PassResult::fail(result.message.empty()
                                        ? "OIR loop vectorization failed"
                                        : std::move(result.message));
        }
        if (result.changed) {
            context.invalidate_oir_analyses();
        }
        return PassResult::ok(result.changed,
                              "vectorized_loops=" +
                                  std::to_string(result.loops_vectorized));
    } catch (const std::exception &error) {
        return PassResult::fail(error.what());
    }
}

} // namespace pass
