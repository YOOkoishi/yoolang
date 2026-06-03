#include "pass/yir/YIRToOIRPass.h"

#include "pass/ast/ASTToYIRPass.h"

#include "pass/yir/YIRToOIRLowerer.h"

namespace pass {

std::string_view YIRToOIRPass::name() const {
    return "YIRToOIRPass";
}

PassKind YIRToOIRPass::kind() const {
    return PassKind::Lowering;
}

PassResult YIRToOIRPass::run(PassContext &context) {
    auto *artifact = context.get_artifact<std::unique_ptr<yir::Module>>(ASTToYIRPass::kArtifactKey);
    if (artifact == nullptr || *artifact == nullptr) {
        return PassResult::fail("YIRToOIRPass requires YIR module in pass context");
    }

    try {
        auto module = yir_to_oir::lower_yir_to_oir(**artifact);
        std::string message;
        if (!module->verify(&message)) {
            return PassResult::fail(message.empty() ? "OIR verification failed" : message);
        }
        context.set_ssa_module(std::move(module));
        return PassResult::ok(true);
    } catch (const std::exception &ex) {
        return PassResult::fail(ex.what());
    }
}

} // namespace pass
