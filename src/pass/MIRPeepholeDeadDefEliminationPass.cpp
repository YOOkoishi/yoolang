#include "../../include/pass/MIRPeepholeDeadDefEliminationPass.h"

#include "../../include/pass/MIRPeepholeCommon.h"

namespace pass::mir_peephole {
namespace {

bool remove_dead_defs_once(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;
    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size();) {
            const auto defs = instrs[i].defs();
            if (defs.size() == 1 && defs[0].is_virtual() && is_pure_def(instrs[i].opcode()) &&
                use_count(counts, defs[0]) == 0) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                ++stats.dead;
                changed = true;
                continue;
            }
            ++i;
        }
    }
    return changed;
}

} // namespace

bool remove_dead_defs(mir::MachineFunction &function, bool post_ra, Stats &stats) {
    (void)post_ra;

    bool changed = false;
    for (int iteration = 0; iteration < 8; ++iteration) {
        if (!remove_dead_defs_once(function, stats)) {
            break;
        }
        changed = true;
    }
    return changed;
}

} // namespace pass::mir_peephole

namespace pass {

std::string_view MIRPeepholeDeadDefEliminationPass::name() const {
    return "MIRPeepholeDeadDefEliminationPass";
}

PassKind MIRPeepholeDeadDefEliminationPass::kind() const {
    return PassKind::Transform;
}

PassResult MIRPeepholeDeadDefEliminationPass::run(PassContext &context) {
    return mir_peephole::run_transform(context, name(), false, mir_peephole::remove_dead_defs);
}

} // namespace pass
