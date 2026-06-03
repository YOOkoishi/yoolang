#include "pass/yir/YIRLoopAnalysisPass.h"

#include "pass/ast/ASTToYIRPass.h"
#include "yir/YIRLoopAnalysis.h"

namespace pass {

std::string_view YIRLoopAnalysisPass::name() const {
    return "YIRLoopAnalysisPass";
}

PassKind YIRLoopAnalysisPass::kind() const {
    return PassKind::Analysis;
}

PassResult YIRLoopAnalysisPass::run(PassContext &context) {
    auto *artifact = context.get_artifact<std::unique_ptr<yir::Module>>(ASTToYIRPass::kArtifactKey);
    if (artifact == nullptr || *artifact == nullptr) {
        return PassResult::fail("YIRLoopAnalysisPass requires YIR module in pass context");
    }

    context.set_artifact(yir::LoopAnalysis(**artifact));
    return PassResult::ok(false);
}

} // namespace pass
