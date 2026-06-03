#include "pass/mir/MIRBitIdiomCombinePass.h"

#include "pass/mir/MIRCombineCommon.h"

#include <utility>

namespace pass::mir_combine {

bool combine_bit_idioms(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;

    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size(); ++i) {
            auto &instr = instrs[i];
            const auto ops = instr.operands();
            if (ops.empty()) {
                continue;
            }

            auto replace_current = [&](mir::MachineInstr replacement) {
                instrs[i] = std::move(replacement);
                ++stats.bit_idioms;
                changed = true;
            };

            if (is_move(instr.opcode()) && ops.size() >= 2 && same_reg(ops[0], ops[1])) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                --i;
                ++stats.bit_idioms;
                changed = true;
                continue;
            }

            if (ops.size() >= 3 &&
                (instr.opcode() == mir::Opcode::Xor || instr.opcode() == mir::Opcode::SubW) &&
                same_reg(ops[1], ops[2])) {
                replace_current(make_move_like(ops[0], zero_reg()));
                continue;
            }

            if (ops.size() >= 3 && instr.opcode() == mir::Opcode::Xor && is_zero_reg(ops[2])) {
                replace_current(make_move_like(ops[0], ops[1]));
                continue;
            }
            if (ops.size() >= 3 && instr.opcode() == mir::Opcode::Xor && is_zero_reg(ops[1])) {
                replace_current(make_move_like(ops[0], ops[2]));
                continue;
            }

            if (ops.size() >= 3 && instr.opcode() == mir::Opcode::And && is_zero_reg(ops[2])) {
                replace_current(make_move_like(ops[0], zero_reg()));
                continue;
            }
            if (ops.size() >= 3 && instr.opcode() == mir::Opcode::And && is_zero_reg(ops[1])) {
                replace_current(make_move_like(ops[0], zero_reg()));
                continue;
            }

            if ((instr.opcode() == mir::Opcode::XorI ||
                 instr.opcode() == mir::Opcode::AddI ||
                 instr.opcode() == mir::Opcode::AddIW ||
                 instr.opcode() == mir::Opcode::SllI ||
                 instr.opcode() == mir::Opcode::SllIW ||
                 instr.opcode() == mir::Opcode::SraI ||
                 instr.opcode() == mir::Opcode::SraIW ||
                 instr.opcode() == mir::Opcode::SrliW) &&
                ops.size() >= 3 && ops[2].kind() == mir::OperandKind::Imm &&
                ops[2].int_value() == 0) {
                replace_current(make_move_like(ops[0], ops[1]));
                continue;
            }

            if (instr.opcode() == mir::Opcode::AndI && ops.size() >= 3 &&
                ops[2].kind() == mir::OperandKind::Imm) {
                if (ops[2].int_value() == 0) {
                    replace_current(make_move_like(ops[0], zero_reg()));
                    continue;
                }
                if (ops[2].int_value() == -1) {
                    replace_current(make_move_like(ops[0], ops[1]));
                    continue;
                }

                if (ops[1].is_reg() && single_use_vreg(counts, ops[1].reg_value())) {
                    auto producer_index = find_def_before(instrs, i, ops[1].reg_value());
                    if (producer_index) {
                        const auto &producer = instrs[*producer_index];
                        const auto &prod_ops = producer.operands();
                        if (producer.opcode() == mir::Opcode::AndI && prod_ops.size() >= 3 &&
                            prod_ops[2].kind() == mir::OperandKind::Imm) {
                            const auto combined = prod_ops[2].int_value() & ops[2].int_value();
                            if (fits_simm12(combined)) {
                                instrs[i] = mir::MachineInstr(
                                    mir::Opcode::AndI,
                                    {ops[0], prod_ops[1], mir::MachineOperand::imm(combined)});
                                erase_producer(instrs, i, *producer_index, stats);
                                ++stats.bit_idioms;
                                changed = true;
                            }
                        }
                    }
                }
            }
        }
    }

    return changed;
}

} // namespace pass::mir_combine

namespace pass {

std::string_view MIRBitIdiomCombinePass::name() const {
    return "MIRBitIdiomCombinePass";
}

PassKind MIRBitIdiomCombinePass::kind() const {
    return PassKind::Transform;
}

PassResult MIRBitIdiomCombinePass::run(PassContext &context) {
    return mir_combine::run_transform(context, name(), mir_combine::combine_bit_idioms);
}

} // namespace pass
