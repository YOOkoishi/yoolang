#include "pass/mir/MIRAddressModeCombinePass.h"

#include "pass/mir/MIRCombineCommon.h"

#include <algorithm>
#include <functional>

namespace pass::mir_combine {

bool combine_address_modes(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;

    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            auto &instr = instrs[i];
            if (instr.opcode() != mir::Opcode::LoadMem &&
                instr.opcode() != mir::Opcode::StoreMem) {
                continue;
            }

            const std::size_t addr_index = instr.opcode() == mir::Opcode::LoadMem ? 1 : 0;
            auto &addr = instr.operands()[addr_index];
            if (!addr.is_reg() || !addr.reg_value().is_virtual()) {
                continue;
            }

            const mir::Register address_result = addr.reg_value();
            mir::Register addi_result = address_result;
            auto producer_index = find_def_before(instrs, i, addr.reg_value());
            if (!producer_index) {
                continue;
            }
            std::optional<std::size_t> copy_index;
            const auto &addr_producer = instrs[*producer_index];
            const auto &addr_prod_ops = addr_producer.operands();
            if (addr_producer.opcode() == mir::Opcode::Move && addr_prod_ops.size() >= 2 &&
                addr_prod_ops[1].is_reg() && addr_prod_ops[1].reg_value().is_virtual()) {
                copy_index = producer_index;
                addi_result = addr_prod_ops[1].reg_value();
                producer_index = find_def_before(instrs, *copy_index, addi_result);
                if (!producer_index) {
                    continue;
                }
            }

            const auto &producer = instrs[*producer_index];
            const auto &prod_ops = producer.operands();
            if (producer.opcode() != mir::Opcode::AddI || prod_ops.size() < 3 ||
                !prod_ops[0].is_reg() || !same_reg(prod_ops[0].reg_value(), addi_result) ||
                prod_ops[2].kind() != mir::OperandKind::Imm ||
                !fits_simm12(prod_ops[2].int_value())) {
                continue;
            }

            if (instr.opcode() == mir::Opcode::LoadMem) {
                instrs[i] = mir::MachineInstr(
                    mir::Opcode::LoadMemOffset,
                    {instr.operands()[0], prod_ops[1], prod_ops[2], instr.operands()[2]});
            } else {
                instrs[i] = mir::MachineInstr(
                    mir::Opcode::StoreMemOffset,
                    {prod_ops[1], instr.operands()[1], prod_ops[2], instr.operands()[2]});
            }

            std::vector<std::size_t> erase_indices;
            const bool erase_copy = copy_index && use_count(counts, address_result) == 1;
            if (erase_copy) {
                erase_indices.push_back(*copy_index);
            }
            if ((!copy_index || erase_copy) && use_count(counts, addi_result) == 1) {
                erase_indices.push_back(*producer_index);
            }
            std::sort(erase_indices.begin(), erase_indices.end(), std::greater<std::size_t>());
            erase_indices.erase(std::unique(erase_indices.begin(), erase_indices.end()),
                                erase_indices.end());
            for (auto erase_index : erase_indices) {
                erase_producer(instrs, i, erase_index, stats);
            }
            ++stats.address_folds;
            changed = true;
        }
    }

    return changed;
}

} // namespace pass::mir_combine

namespace pass {

std::string_view MIRAddressModeCombinePass::name() const {
    return "MIRAddressModeCombinePass";
}

PassKind MIRAddressModeCombinePass::kind() const {
    return PassKind::Transform;
}

PassResult MIRAddressModeCombinePass::run(PassContext &context) {
    return mir_combine::run_transform(context, name(), mir_combine::combine_address_modes);
}

} // namespace pass
