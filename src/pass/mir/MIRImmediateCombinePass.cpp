#include "pass/mir/MIRImmediateCombinePass.h"

#include "pass/mir/MIRCombineCommon.h"

#include <utility>

namespace pass::mir_combine {

bool combine_immediates(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;

    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            auto &instr = instrs[i];
            auto ops = instr.operands();
            if (ops.size() < 3 || !ops[0].is_reg()) {
                continue;
            }

            auto replace_with = [&](mir::Opcode opcode, std::vector<mir::MachineOperand> operands,
                                    std::size_t producer) {
                instrs[i] = mir::MachineInstr(opcode, std::move(operands));
                erase_producer(instrs, i, producer, stats);
                ++stats.immediates;
                changed = true;
            };

            if (instr.opcode() == mir::Opcode::Add || instr.opcode() == mir::Opcode::AddW) {
                auto imm_rhs = find_single_use_load_imm(instrs, i, ops[2], counts);
                if (imm_rhs && fits_simm12(imm_rhs->value)) {
                    replace_with(instr.opcode() == mir::Opcode::Add ? mir::Opcode::AddI
                                                                    : mir::Opcode::AddIW,
                                 {ops[0], ops[1], mir::MachineOperand::imm(imm_rhs->value)},
                                 imm_rhs->producer);
                    continue;
                }

                auto imm_lhs = find_single_use_load_imm(instrs, i, ops[1], counts);
                if (imm_lhs && fits_simm12(imm_lhs->value)) {
                    replace_with(instr.opcode() == mir::Opcode::Add ? mir::Opcode::AddI
                                                                    : mir::Opcode::AddIW,
                                 {ops[0], ops[2], mir::MachineOperand::imm(imm_lhs->value)},
                                 imm_lhs->producer);
                    continue;
                }
            }

            if (instr.opcode() == mir::Opcode::SubW) {
                auto imm_rhs = find_single_use_load_imm(instrs, i, ops[2], counts);
                if (imm_rhs && neg_fits_simm12(imm_rhs->value)) {
                    replace_with(mir::Opcode::AddIW,
                                 {ops[0], ops[1], mir::MachineOperand::imm(-imm_rhs->value)},
                                 imm_rhs->producer);
                    continue;
                }
                auto imm_lhs = find_single_use_load_imm(instrs, i, ops[1], counts);
                if (imm_lhs && imm_lhs->value == 0) {
                    replace_with(mir::Opcode::SubW,
                                 {ops[0], mir::MachineOperand::reg_use(zero_reg()), ops[2]},
                                 imm_lhs->producer);
                    continue;
                }
            }

            if (instr.opcode() == mir::Opcode::And || instr.opcode() == mir::Opcode::Xor) {
                auto imm_rhs = find_single_use_load_imm(instrs, i, ops[2], counts);
                bool rhs = true;
                auto imm = imm_rhs;
                if (!imm || !fits_simm12(imm->value)) {
                    rhs = false;
                    imm = find_single_use_load_imm(instrs, i, ops[1], counts);
                }
                if (imm && fits_simm12(imm->value)) {
                    const auto value_operand = rhs ? ops[1] : ops[2];
                    auto opcode = instr.opcode() == mir::Opcode::And ? mir::Opcode::AndI
                                                                     : mir::Opcode::XorI;
                    replace_with(opcode,
                                 {ops[0], value_operand, mir::MachineOperand::imm(imm->value)},
                                 imm->producer);
                    continue;
                }
            }

            if (instr.opcode() == mir::Opcode::MulW) {
                auto imm_rhs = find_single_use_load_imm(instrs, i, ops[2], counts);
                bool rhs = true;
                auto imm = imm_rhs;
                if (!imm) {
                    rhs = false;
                    imm = find_single_use_load_imm(instrs, i, ops[1], counts);
                }
                if (!imm) {
                    continue;
                }

                const auto value_operand = rhs ? ops[1] : ops[2];
                if (imm->value == 0) {
                    replace_with(mir::Opcode::Move,
                                 {ops[0], mir::MachineOperand::reg_use(zero_reg())},
                                 imm->producer);
                    continue;
                }
                if (imm->value == 1) {
                    replace_with(mir::Opcode::Move, {ops[0], value_operand}, imm->producer);
                    continue;
                }
                if (imm->value > 0 && is_power_of_two(static_cast<std::uint64_t>(imm->value))) {
                    auto shift = log2_u64(static_cast<std::uint64_t>(imm->value));
                    if (shift < 32) {
                        replace_with(mir::Opcode::SllIW,
                                     {ops[0], value_operand, mir::MachineOperand::imm(shift)},
                                     imm->producer);
                    }
                }
            }
        }
    }

    return changed;
}

} // namespace pass::mir_combine

namespace pass {

std::string_view MIRImmediateCombinePass::name() const {
    return "MIRImmediateCombinePass";
}

PassKind MIRImmediateCombinePass::kind() const {
    return PassKind::Transform;
}

PassResult MIRImmediateCombinePass::run(PassContext &context) {
    return mir_combine::run_transform(context, name(), mir_combine::combine_immediates);
}

} // namespace pass
