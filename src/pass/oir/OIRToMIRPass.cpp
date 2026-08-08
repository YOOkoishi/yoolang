#include "pass/oir/OIRToMIRPass.h"

#include "mir/MIRVerifier.h"

#include "pass/oir/OIRToMIRCommon.h"
#include "target/TargetMachine.h"

namespace pass {

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
        const auto *target_machine = context.get_artifact<target::TargetMachine>(
            target::kTargetMachineArtifactKey);
        target::TargetMachine default_target_machine;
        const auto &profile = target_machine == nullptr ? default_target_machine.profile()
                                                        : target_machine->profile();
        auto lowered = oir_to_mir::lower_with_vregs(*module, profile);
        auto &target_info = lowered->target();
        target_info.triple = profile.triple;
        target_info.arch = profile.march;
        target_info.abi = profile.mabi;
        target_info.cpu = profile.cpu;
        target_info.tune = profile.tune;
        target_info.xlen_bits = profile.xlen_bits;
        target_info.flen_bits = profile.flen_bits;
        target_info.minimum_vlen_bits = profile.minimum_vlen_bits;
        target_info.abi_vlen_bits =
            profile.vector_abi == target::VectorABI::PsABIVector &&
                    profile.fixed_vector_bits.has_value()
                ? *profile.fixed_vector_bits
                : 0U;
        target_info.stack_align = profile.stack_alignment;
        target_info.has_vector = profile.has_vector();
        target_info.psabi_vector = profile.vector_abi == target::VectorABI::PsABIVector;
        auto verify = mir::verify_module(*lowered, mir::MIRVerificationStage::PreRA);
        if (!verify.ok) {
            return PassResult::fail(verify.message);
        }
        context.set_machine_module(std::move(lowered));
        return PassResult::ok(true);
    } catch (const std::exception &ex) {
        return PassResult::fail(ex.what());
    }
}

} // namespace pass
