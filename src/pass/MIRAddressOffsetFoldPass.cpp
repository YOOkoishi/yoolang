#include "../../include/pass/MIRAddressOffsetFoldPass.h"

#include "../../include/pass/MIRPeepholeCommon.h"

namespace pass::mir_peephole {

bool fold_address_offsets(mir::MachineFunction &function, bool post_ra, Stats &stats) {
    (void)post_ra;

    auto counts = count_vregs(function);
    bool changed = false;
    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 1; i < instrs.size();) {
            auto &instr = instrs[i];
            if (instr.opcode() != mir::Opcode::LoadMem &&
                instr.opcode() != mir::Opcode::StoreMem) {
                ++i;
                continue;
            }
            auto &addr = instr.operands()[instr.opcode() == mir::Opcode::LoadMem ? 1 : 0];
            if (!addr.is_reg() || !addr.reg_value().is_virtual() ||
                use_count(counts, addr.reg_value()) != 1) {
                ++i;
                continue;
            }

            auto &producer = instrs[i - 1];
            const auto &prod_ops = producer.operands();
            if (producer.opcode() != mir::Opcode::AddI || prod_ops.size() < 3 ||
                !same_reg(prod_ops[0].reg_value(), addr.reg_value()) ||
                prod_ops[2].kind() != mir::OperandKind::Imm ||
                !fits_simm12(prod_ops[2].int_value())) {
                ++i;
                continue;
            }

            if (instr.opcode() == mir::Opcode::LoadMem) {
                instr = mir::MachineInstr(
                    mir::Opcode::LoadMemOffset,
                    {instr.operands()[0], prod_ops[1], prod_ops[2], instr.operands()[2]});
                ++stats.loads;
            } else {
                instr = mir::MachineInstr(
                    mir::Opcode::StoreMemOffset,
                    {prod_ops[1], instr.operands()[1], prod_ops[2], instr.operands()[2]});
                ++stats.stores;
            }
            instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i - 1));
            --i;
            ++stats.address_folds;
            changed = true;
        }
    }
    return changed;
}

} // namespace pass::mir_peephole

namespace pass {

std::string_view MIRAddressOffsetFoldPass::name() const {
    return "MIRAddressOffsetFoldPass";
}

PassKind MIRAddressOffsetFoldPass::kind() const {
    return PassKind::Transform;
}

PassResult MIRAddressOffsetFoldPass::run(PassContext &context) {
    return mir_peephole::run_transform(context, name(), false,
                                       mir_peephole::fold_address_offsets);
}

} // namespace pass
