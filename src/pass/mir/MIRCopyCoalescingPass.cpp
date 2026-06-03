#include "pass/mir/MIRCopyCoalescingPass.h"

#include "pass/mir/MIRPeepholeCommon.h"

namespace pass::mir_peephole {
namespace {

unsigned replace_reg_uses(mir::MachineInstr &instr, const mir::Register &old_reg,
                          const mir::Register &new_reg) {
    unsigned replaced = 0;
    for (auto &operand : instr.operands()) {
        if (!operand.is_reg() || !operand.is_use() || operand.is_implicit() ||
            !same_reg(operand.reg_value(), old_reg)) {
            continue;
        }
        operand.set_reg(new_reg);
        ++replaced;
    }
    return replaced;
}

bool can_copy_propagate_source(const mir::Register &src) {
    return src.is_virtual() || is_zero_reg(src);
}

bool coalesce_copies_once(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;

    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size();) {
            auto &instr = instrs[i];
            const auto &ops = instr.operands();
            if (!is_move(instr.opcode()) || ops.size() < 2 || !ops[0].is_reg() ||
                !ops[1].is_reg()) {
                ++i;
                continue;
            }

            auto dst = ops[0].reg_value();
            auto src = ops[1].reg_value();
            if (same_reg(dst, src)) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                ++stats.copies;
                changed = true;
                continue;
            }
            if (!dst.is_virtual() || !can_copy_propagate_source(src) ||
                def_count(counts, dst) != 1) {
                ++i;
                continue;
            }

            const unsigned total_uses = use_count(counts, dst);
            if (total_uses == 0) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                ++stats.copies;
                changed = true;
                continue;
            }

            unsigned replaced = 0;
            bool blocked = false;
            for (std::size_t j = i + 1; j < instrs.size(); ++j) {
                if (defines_reg(instrs[j], src) || defines_reg(instrs[j], dst)) {
                    blocked = true;
                    break;
                }
                replaced += replace_reg_uses(instrs[j], dst, src);
                if (replaced == total_uses) {
                    break;
                }
            }

            if (!blocked && replaced == total_uses) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                ++stats.copies;
                changed = true;
                continue;
            }
            ++i;
        }
    }

    return changed;
}

} // namespace

bool coalesce_copies(mir::MachineFunction &function, bool post_ra, Stats &stats) {
    (void)post_ra;

    bool changed = false;
    for (int iteration = 0; iteration < 8; ++iteration) {
        if (!coalesce_copies_once(function, stats)) {
            break;
        }
        changed = true;
    }
    return changed;
}

} // namespace pass::mir_peephole

namespace pass {

std::string_view MIRCopyCoalescingPass::name() const {
    return "MIRCopyCoalescingPass";
}

PassKind MIRCopyCoalescingPass::kind() const {
    return PassKind::Transform;
}

PassResult MIRCopyCoalescingPass::run(PassContext &context) {
    return mir_peephole::run_transform(context, name(), false, mir_peephole::coalesce_copies);
}

} // namespace pass
