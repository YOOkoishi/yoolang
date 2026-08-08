#include "pass/mir/MIRToAsmPass.h"

#include "mir/AsmPrinter.h"
#include "mir/MIRVerifier.h"

#include <sstream>

namespace pass {

std::string_view MIRToAsmPass::name() const {
    return "MIRToAsmPass";
}

PassKind MIRToAsmPass::kind() const {
    return PassKind::Lowering;
}

PassResult MIRToAsmPass::run(PassContext &context) {
    auto *module = context.machine_module();
    if (module == nullptr) {
        return PassResult::fail("MIRToAsmPass requires MIR module in pass context");
    }

    auto verify = mir::verify_module(*module, mir::MIRVerificationStage::Final);
    if (!verify.ok) {
        return PassResult::fail(verify.message);
    }

    std::ostringstream oss;
    mir::AsmPrinter printer(oss);
    printer.print(*module);
    context.set_artifact<std::string>(kArtifactKey, oss.str());
    return PassResult::ok(true);
}

} // namespace pass
