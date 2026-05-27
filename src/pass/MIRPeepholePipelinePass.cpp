#include "../../include/pass/MIRPeepholePipelinePass.h"

#include "../../include/mir/MIRVerifier.h"
#include "../../include/pass/MIRPeepholeCommon.h"

namespace pass {
namespace {

bool optimize_function(mir::MachineFunction &function, bool post_ra,
                       mir_peephole::Stats &stats) {
    bool changed = false;

    if (!post_ra) {
        for (int iteration = 0; iteration < 6; ++iteration) {
            bool iteration_changed = false;
            iteration_changed |= mir_peephole::coalesce_copies(function, post_ra, stats);
            iteration_changed |= mir_peephole::fuse_compare_branches(function, post_ra, stats);
            iteration_changed |= mir_peephole::local_cse(function, post_ra, stats);
            iteration_changed |= mir_peephole::fold_address_offsets(function, post_ra, stats);
            iteration_changed |= mir_peephole::cleanup_jumps(function, post_ra, stats);
            iteration_changed |= mir_peephole::remove_dead_defs(function, post_ra, stats);
            if (!iteration_changed) {
                break;
            }
            changed = true;
        }
    }

    changed |= mir_peephole::cleanup_jumps(function, post_ra, stats);
    changed |= mir_peephole::simplify_blocks(function, post_ra, stats);
    changed |= mir_peephole::cleanup_jumps(function, post_ra, stats);
    return changed;
}

} // namespace

MIRPeepholePipelinePass::MIRPeepholePipelinePass(bool post_ra) : post_ra_(post_ra) {
}

std::string_view MIRPeepholePipelinePass::name() const {
    return post_ra_ ? "MIRPostRAPeepholePipelinePass" : "MIRPreRAPeepholePipelinePass";
}

PassKind MIRPeepholePipelinePass::kind() const {
    return PassKind::Transform;
}

PassResult MIRPeepholePipelinePass::run(PassContext &context) {
    auto *module = context.machine_module();
    if (module == nullptr) {
        return PassResult::fail("MIRPeepholePipelinePass requires MIR module in pass context");
    }

    mir_peephole::Stats total;
    bool changed = false;
    for (auto &function : module->functions()) {
        if (function->is_external()) {
            continue;
        }
        function->rebuild_cfg();
        changed |= optimize_function(*function, post_ra_, total);
        function->rebuild_cfg();
    }

    auto verify = mir::verify_module(
        *module, post_ra_ ? mir::MIRVerificationStage::PostRA : mir::MIRVerificationStage::PreRA);
    if (!verify.ok) {
        return PassResult::fail(verify.message);
    }

    context.set_artifact(std::string(name()), total.message());
    return PassResult::ok(changed, total.message());
}

} // namespace pass
