#include "../../include/pass/MIRCombineDeadDefEliminationPass.h"

#include "../../include/pass/MIRCombineCommon.h"

namespace pass::mir_combine {
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

bool remove_dead_defs(mir::MachineFunction &function, Stats &stats) {
    bool changed = false;
    for (int iteration = 0; iteration < 8; ++iteration) {
        if (!remove_dead_defs_once(function, stats)) {
            break;
        }
        changed = true;
    }
    return changed;
}

} // namespace pass::mir_combine

namespace pass {

std::string_view MIRCombineDeadDefEliminationPass::name() const {
    return "MIRCombineDeadDefEliminationPass";
}

PassKind MIRCombineDeadDefEliminationPass::kind() const {
    return PassKind::Transform;
}

PassResult MIRCombineDeadDefEliminationPass::run(PassContext &context) {
    return mir_combine::run_transform(context, name(), mir_combine::remove_dead_defs);
}

} // namespace pass
