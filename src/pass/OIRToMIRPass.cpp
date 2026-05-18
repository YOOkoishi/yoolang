#include "../../include/pass/OIRToMIRPass.h"

#include "../../include/mir/MIRVerifier.h"

#include "../../include/pass/OIRToMIRCommon.h"

#include <memory>

namespace pass {

OIRToMIRPass::OIRToMIRPass(bool use_virtual_registers)
    : use_virtual_registers_(use_virtual_registers) {
}

std::string_view OIRToMIRPass::name() const {
    return "OIRToMIRPass";
}

PassKind OIRToMIRPass::kind() const {
    return PassKind::Lowering;
}

PassResult OIRToMIRPass::run(PassContext &context) {
    auto *module = context.ssa_module();
    if (module == nullptr) {
        return PassResult::fail("OIRToMIRPass requires OIR module in pass context");
    }

    try {
        std::unique_ptr<mir::Module> lowered;
        const bool conservative_lowering =
            !use_virtual_registers_ || oir_to_mir::should_use_conservative_lowering_for_size(*module);
        if (!conservative_lowering) {
            lowered = oir_to_mir::lower_with_vregs(*module);
        } else {
            lowered = oir_to_mir::lower_with_stack_slots(*module);
        }
        auto verify = mir::verify_module(*lowered, mir::MIRVerificationStage::PreRA);
        if (!verify.ok) {
            return PassResult::fail(verify.message);
        }
        context.set_artifact("MIRConservativeLowering", conservative_lowering);
        context.set_machine_module(std::move(lowered));
        return PassResult::ok(true);
    } catch (const std::exception &ex) {
        return PassResult::fail(ex.what());
    }
}

} // namespace pass
