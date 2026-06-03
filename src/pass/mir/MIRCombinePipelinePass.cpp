#include "pass/mir/MIRCombinePipelinePass.h"

#include "mir/MIRVerifier.h"
#include "pass/mir/MIRCombineCommon.h"

namespace pass {
namespace {

bool optimize_function(mir::MachineFunction &function, mir_combine::Stats &stats) {
    bool changed = false;
    for (int iteration = 0; iteration < 8; ++iteration) {
        bool iteration_changed = false;
        iteration_changed |= mir_combine::combine_immediates(function, stats);
        iteration_changed |= mir_combine::combine_address_modes(function, stats);
        iteration_changed |= mir_combine::combine_compare_branches(function, stats);
        iteration_changed |= mir_combine::combine_rem_zero_branches(function, stats);
        iteration_changed |= mir_combine::combine_bit_idioms(function, stats);
        iteration_changed |= mir_combine::remove_dead_defs(function, stats);
        if (!iteration_changed) {
            break;
        }
        changed = true;
    }
    return changed;
}

} // namespace

std::string_view MIRCombinePipelinePass::name() const {
    return "MIRCombinePipelinePass";
}

PassKind MIRCombinePipelinePass::kind() const {
    return PassKind::Transform;
}

PassResult MIRCombinePipelinePass::run(PassContext &context) {
    auto *module = context.machine_module();
    if (module == nullptr) {
        return PassResult::fail("MIRCombinePipelinePass requires MIR module in pass context");
    }

    mir_combine::Stats total;
    bool changed = false;
    for (auto &function : module->functions()) {
        if (function->is_external()) {
            continue;
        }
        function->rebuild_cfg();
        changed |= optimize_function(*function, total);
        function->rebuild_cfg();
    }

    auto verify = mir::verify_module(*module, mir::MIRVerificationStage::PreRA);
    if (!verify.ok) {
        return PassResult::fail(verify.message);
    }

    context.set_artifact(std::string(name()), total.message());
    return PassResult::ok(changed, total.message());
}

} // namespace pass
