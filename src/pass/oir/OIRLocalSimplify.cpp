#include "oir/OIRScalarOpt.h"

#include "oir/OIRAnalysis.h"

#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace pass::oir_opt {

static std::optional<std::int64_t> all_ones_constant_for_type(oir::Type *type) {
    auto *integer = dynamic_cast<oir::IntegerType *>(type);
    if (integer == nullptr) {
        return std::nullopt;
    }
    if (integer->bit_width() == 1) {
        return 1;
    }
    if (integer->bit_width() == 32) {
        return -1;
    }
    return std::nullopt;
}

oir::CmpInst *as_zext_cmp(oir::Value *value) {
    auto *cast = dynamic_cast<oir::CastInst *>(value);
    if (cast == nullptr || cast->op() != oir::Instruction::OpID::ZExt) {
        return nullptr;
    }
    return dynamic_cast<oir::CmpInst *>(cast->src());
}

oir::Value *insert_bool_not(oir::Module &module, oir::BasicBlock &block,
                            std::list<std::unique_ptr<oir::Instruction>>::iterator before,
                            oir::Value *value, const std::string &name) {
    auto new_inst = std::make_unique<oir::CmpInst>(module.types().int1_ty(),
                                                   oir::Instruction::OpID::ICmp, oir::CmpPred::EQ,
                                                   value, module.create_i1(false), &block, name);
    auto *raw = new_inst.get();
    raw->set_parent(&block);
    block.instructions().insert(before, std::move(new_inst));
    return raw;
}

oir::Value *simplify_zext_cmp_compare(oir::Module &module, oir::BasicBlock &block,
                                      std::list<std::unique_ptr<oir::Instruction>>::iterator before,
                                      oir::CmpInst &cmp) {
    if (cmp.op() != oir::Instruction::OpID::ICmp ||
        (cmp.pred() != oir::CmpPred::EQ && cmp.pred() != oir::CmpPred::NE)) {
        return nullptr;
    }

    oir::CmpInst *src_cmp = nullptr;
    std::optional<std::int64_t> constant;
    if ((src_cmp = as_zext_cmp(cmp.lhs())) != nullptr) {
        constant = int_constant(cmp.rhs());
    } else if ((src_cmp = as_zext_cmp(cmp.rhs())) != nullptr) {
        constant = int_constant(cmp.lhs());
    }

    if (src_cmp == nullptr || !constant.has_value() || (*constant != 0 && *constant != 1)) {
        return nullptr;
    }

    const bool wants_true = (cmp.pred() == oir::CmpPred::NE && *constant == 0) ||
                            (cmp.pred() == oir::CmpPred::EQ && *constant == 1);
    if (wants_true) {
        return src_cmp;
    }
    return insert_bool_not(module, block, before, src_cmp,
                           cmp.name().empty() ? "not.zextcmp" : cmp.name());
}

oir::Value *simplify_signed_odd_remainder_compare(
    oir::Module &module, oir::BasicBlock &block,
    std::list<std::unique_ptr<oir::Instruction>>::iterator before, oir::CmpInst &cmp) {
    if (cmp.op() != oir::Instruction::OpID::ICmp ||
        (cmp.pred() != oir::CmpPred::EQ && cmp.pred() != oir::CmpPred::NE)) {
        return nullptr;
    }

    oir::BinaryInst *rem = nullptr;
    std::optional<std::int64_t> constant;
    if ((rem = dynamic_cast<oir::BinaryInst *>(cmp.lhs())) != nullptr) {
        constant = int_constant(cmp.rhs());
    } else if ((rem = dynamic_cast<oir::BinaryInst *>(cmp.rhs())) != nullptr) {
        constant = int_constant(cmp.lhs());
    }
    if (rem == nullptr || rem->op() != oir::Instruction::OpID::SRem || !constant ||
        *constant != 1) {
        return nullptr;
    }

    auto *int_type = dynamic_cast<oir::IntegerType *>(rem->type());
    auto divisor = int_constant(rem->rhs());
    if (int_type == nullptr || int_type->bit_width() != 32 || !divisor ||
        (*divisor != 2 && *divisor != -2)) {
        return nullptr;
    }

    constexpr std::int64_t kSignedOddMask = -2147483647LL;
    auto mask = std::make_unique<oir::BinaryInst>(
        rem->type(), oir::Instruction::OpID::And, rem->lhs(),
        make_int_constant(module, rem->type(), kSignedOddMask), &block,
        rem->name().empty() ? "srem.odd.mask" : rem->name() + ".odd.mask");
    auto *raw_mask = mask.get();
    raw_mask->set_parent(&block);
    block.instructions().insert(before, std::move(mask));

    auto replacement = std::make_unique<oir::CmpInst>(
        cmp.type(), oir::Instruction::OpID::ICmp, cmp.pred(), raw_mask,
        make_int_constant(module, rem->type(), 1), &block,
        cmp.name().empty() ? "srem.odd" : cmp.name() + ".odd");
    auto *raw_cmp = replacement.get();
    raw_cmp->set_parent(&block);
    block.instructions().insert(before, std::move(replacement));
    return raw_cmp;
}

std::optional<std::int64_t> positive_power_of_two_abs(std::int64_t value) {
    if (value == 0 || value == std::numeric_limits<std::int32_t>::min()) {
        return std::nullopt;
    }
    const auto magnitude = value < 0 ? -value : value;
    if ((magnitude & (magnitude - 1)) != 0) {
        return std::nullopt;
    }
    return magnitude;
}

oir::Value *simplify_signed_remainder_zero_compare(
    oir::Module &module, oir::BasicBlock &block,
    std::list<std::unique_ptr<oir::Instruction>>::iterator before, oir::CmpInst &cmp) {
    if (cmp.op() != oir::Instruction::OpID::ICmp ||
        (cmp.pred() != oir::CmpPred::EQ && cmp.pred() != oir::CmpPred::NE)) {
        return nullptr;
    }

    oir::BinaryInst *rem = nullptr;
    std::optional<std::int64_t> constant;
    if ((rem = dynamic_cast<oir::BinaryInst *>(cmp.lhs())) != nullptr) {
        constant = int_constant(cmp.rhs());
    } else if ((rem = dynamic_cast<oir::BinaryInst *>(cmp.rhs())) != nullptr) {
        constant = int_constant(cmp.lhs());
    }
    if (rem == nullptr || rem->op() != oir::Instruction::OpID::SRem || !constant ||
        *constant != 0) {
        return nullptr;
    }

    auto *int_type = dynamic_cast<oir::IntegerType *>(rem->type());
    auto divisor = int_constant(rem->rhs());
    if (int_type == nullptr || int_type->bit_width() != 32 || !divisor) {
        return nullptr;
    }

    auto magnitude = positive_power_of_two_abs(*divisor);
    if (!magnitude || *magnitude <= 1) {
        return nullptr;
    }

    oir::Value *mask_input = rem->lhs();
    if (*magnitude == 2) {
        auto *product = dynamic_cast<oir::BinaryInst *>(mask_input);
        if (product != nullptr && product->op() == oir::Instruction::OpID::Mul &&
            product->type() == rem->type()) {
            auto low_product = std::make_unique<oir::BinaryInst>(
                rem->type(), oir::Instruction::OpID::And, product->lhs(), product->rhs(), &block,
                product->name().empty() ? "mul.lowbit" : product->name() + ".lowbit");
            auto *raw_low_product = low_product.get();
            raw_low_product->set_parent(&block);
            mask_input = raw_low_product;
            block.instructions().insert(before, std::move(low_product));
        }
    }

    auto mask = std::make_unique<oir::BinaryInst>(
        rem->type(), oir::Instruction::OpID::And, mask_input,
        make_int_constant(module, rem->type(), *magnitude - 1), &block,
        rem->name().empty() ? "srem.zero.mask" : rem->name() + ".zero.mask");
    auto *raw_mask = mask.get();
    raw_mask->set_parent(&block);
    block.instructions().insert(before, std::move(mask));

    auto replacement = std::make_unique<oir::CmpInst>(
        cmp.type(), oir::Instruction::OpID::ICmp, cmp.pred(), raw_mask,
        make_zero_constant(module, rem->type()), &block,
        cmp.name().empty() ? "srem.zero" : cmp.name() + ".zero");
    auto *raw_cmp = replacement.get();
    raw_cmp->set_parent(&block);
    block.instructions().insert(before, std::move(replacement));
    return raw_cmp;
}

oir::Value *simplify_instruction(oir::Module &module, oir::BasicBlock &block,
                                 std::list<std::unique_ptr<oir::Instruction>>::iterator before,
                                 oir::Instruction &inst, SimplifyMode mode) {
    if (auto *binary = dynamic_cast<oir::BinaryInst *>(&inst)) {
        if (mode == SimplifyMode::ConstantFold) {
            auto lhs_int = int_constant(binary->lhs());
            auto rhs_int = int_constant(binary->rhs());
            if (lhs_int.has_value() && rhs_int.has_value()) {
                auto folded = fold_int_binary(inst.op(), *lhs_int, *rhs_int);
                if (folded.has_value()) {
                    return make_int_constant(module, inst.type(), *folded);
                }
            }

            auto lhs_float = float_constant(binary->lhs());
            auto rhs_float = float_constant(binary->rhs());
            if (lhs_float.has_value() && rhs_float.has_value()) {
                auto folded = fold_float_binary(inst.op(), *lhs_float, *rhs_float);
                if (folded.has_value()) {
                    return module.create_f32(*folded);
                }
            }
            return nullptr;
        }

        switch (inst.op()) {
        case oir::Instruction::OpID::Add:
            if (is_int_value(binary->rhs(), 0)) {
                return binary->lhs();
            }
            if (is_int_value(binary->lhs(), 0)) {
                return binary->rhs();
            }
            break;
        case oir::Instruction::OpID::Sub:
            if (is_int_value(binary->rhs(), 0)) {
                return binary->lhs();
            }
            break;
        case oir::Instruction::OpID::Mul:
            if (is_int_value(binary->rhs(), 1)) {
                return binary->lhs();
            }
            if (is_int_value(binary->lhs(), 1)) {
                return binary->rhs();
            }
            if (is_int_value(binary->rhs(), 0) || is_int_value(binary->lhs(), 0)) {
                return make_zero_constant(module, inst.type());
            }
            break;
        case oir::Instruction::OpID::And:
            if (is_int_value(binary->rhs(), 0) || is_int_value(binary->lhs(), 0)) {
                return make_zero_constant(module, inst.type());
            }
            if (auto all_ones = all_ones_constant_for_type(inst.type())) {
                if (is_int_value(binary->rhs(), *all_ones)) {
                    return binary->lhs();
                }
                if (is_int_value(binary->lhs(), *all_ones)) {
                    return binary->rhs();
                }
            }
            break;
        case oir::Instruction::OpID::Xor:
            if (is_int_value(binary->rhs(), 0)) {
                return binary->lhs();
            }
            if (is_int_value(binary->lhs(), 0)) {
                return binary->rhs();
            }
            if (binary->lhs() == binary->rhs()) {
                return make_zero_constant(module, inst.type());
            }
            break;
        case oir::Instruction::OpID::SDiv:
            if (is_int_value(binary->rhs(), 1)) {
                return binary->lhs();
            }
            break;
        case oir::Instruction::OpID::SRem:
            if (is_int_value(binary->rhs(), 1)) {
                return make_zero_constant(module, inst.type());
            }
            break;
        default:
            break;
        }
        return nullptr;
    }

    if (auto *cmp = dynamic_cast<oir::CmpInst *>(&inst)) {
        if (mode == SimplifyMode::Algebraic) {
            if (auto *replacement =
                    simplify_signed_remainder_zero_compare(module, block, before, *cmp)) {
                return replacement;
            }
            if (auto *replacement =
                    simplify_signed_odd_remainder_compare(module, block, before, *cmp)) {
                return replacement;
            }
            return simplify_zext_cmp_compare(module, block, before, *cmp);
        }

        auto lhs_int = int_constant(cmp->lhs());
        auto rhs_int = int_constant(cmp->rhs());
        if (lhs_int.has_value() && rhs_int.has_value()) {
            return module.create_i1(eval_cmp(cmp->pred(), *lhs_int, *rhs_int));
        }

        auto lhs_float = float_constant(cmp->lhs());
        auto rhs_float = float_constant(cmp->rhs());
        if (lhs_float.has_value() && rhs_float.has_value()) {
            return module.create_i1(eval_fcmp(cmp->pred(), *lhs_float, *rhs_float));
        }
        return nullptr;
    }

    if (auto *cast = dynamic_cast<oir::CastInst *>(&inst)) {
        if (mode != SimplifyMode::ConstantFold) {
            return nullptr;
        }
        switch (inst.op()) {
        case oir::Instruction::OpID::ZExt:
            if (auto constant = int_constant(cast->src())) {
                return make_int_constant(module, inst.type(), *constant == 0 ? 0 : 1);
            }
            break;
        case oir::Instruction::OpID::SIToFP:
            if (auto constant = int_constant(cast->src())) {
                return module.create_f32(static_cast<float>(static_cast<std::int32_t>(*constant)));
            }
            break;
        case oir::Instruction::OpID::FPToSI:
            if (auto constant = float_constant(cast->src())) {
                return module.create_i32(static_cast<std::int32_t>(*constant));
            }
            break;
        default:
            break;
        }
        return nullptr;
    }

    if (auto *gep = dynamic_cast<oir::GetElementPtrInst *>(&inst)) {
        if (mode != SimplifyMode::Algebraic) {
            return nullptr;
        }
        bool all_zero = true;
        for (auto *index : gep->indices()) {
            if (!is_int_value(index, 0)) {
                all_zero = false;
                break;
            }
        }
        if (all_zero && gep->base_ptr()->type() == gep->type()) {
            return gep->base_ptr();
        }
        return nullptr;
    }

    if (auto *phi = dynamic_cast<oir::PhiInst *>(&inst)) {
        if (mode != SimplifyMode::Algebraic) {
            return nullptr;
        }
        if (phi->incoming().empty()) {
            return nullptr;
        }
        auto *first = phi->incoming().front().first;
        for (const auto &incoming : phi->incoming()) {
            if (incoming.first != first) {
                return nullptr;
            }
        }
        return first;
    }

    return nullptr;
}

void remove_phi_incoming_from(oir::BasicBlock *block, oir::BasicBlock *pred) {
    for (auto &inst : block->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        phi->remove_incoming_from(pred);
    }
}

void remove_edge(oir::BasicBlock *pred, oir::BasicBlock *succ) {
    pred->remove_successor(succ);
    succ->remove_predecessor(pred);
    remove_phi_incoming_from(succ, pred);
}

bool fold_branch(oir::Module &module, oir::BasicBlock &block,
                 std::list<std::unique_ptr<oir::Instruction>>::iterator term_it, bool take_true,
                 Stats &stats) {
    auto *branch = dynamic_cast<oir::BranchInst *>(term_it->get());
    if (branch == nullptr || !branch->is_conditional()) {
        return false;
    }

    auto *target = take_true ? branch->true_bb() : branch->false_bb();
    auto *removed = take_true ? branch->false_bb() : branch->true_bb();
    if (removed != target) {
        remove_edge(&block, removed);
    }
    block.add_successor(target);
    target->add_predecessor(&block);

    auto replacement = std::make_unique<oir::BranchInst>(module.types().void_ty(), target, &block);
    replacement->set_parent(&block);
    (*term_it)->drop_all_operands();
    *term_it = std::move(replacement);
    ++stats.branches;
    return true;
}

bool simplify_branches(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        for (auto &block : function->blocks()) {
            if (!block->has_terminator()) {
                continue;
            }
            auto term_it = std::prev(block->instructions().end());
            auto *branch = dynamic_cast<oir::BranchInst *>(term_it->get());
            if (branch == nullptr || !branch->is_conditional()) {
                continue;
            }
            if (branch->true_bb() == branch->false_bb()) {
                changed |= fold_branch(module, *block, term_it, true, stats);
                continue;
            }
            auto condition = int_constant(branch->cond());
            if (condition.has_value()) {
                changed |= fold_branch(module, *block, term_it, *condition != 0, stats);
            }
        }
    }
    return changed;
}

struct ConditionalAddDiamond {
    oir::BasicBlock *arm = nullptr;
    oir::BasicBlock *merge = nullptr;
    oir::BinaryInst *arm_add = nullptr;
    oir::PhiInst *phi = nullptr;
    oir::Value *base = nullptr;
    oir::Value *increment = nullptr;
    bool arm_is_true = true;
};

struct ShortCircuitBoolDiamond {
    oir::BasicBlock *arm = nullptr;
    oir::BasicBlock *merge = nullptr;
    oir::CmpInst *arm_cmp = nullptr;
    oir::PhiInst *phi = nullptr;
    bool arm_is_true = true;
    bool pred_value = false;
};

struct ValueSelectDiamond {
    oir::BasicBlock *true_source = nullptr;
    oir::BasicBlock *false_source = nullptr;
    oir::BasicBlock *merge = nullptr;
    oir::PhiInst *phi = nullptr;
    oir::Value *true_value = nullptr;
    oir::Value *false_value = nullptr;
    std::vector<oir::BasicBlock *> removed_blocks;
};

bool has_single_phi(oir::BasicBlock &block) {
    bool seen_phi = false;
    for (auto &inst : block.instructions()) {
        if (inst->op() != oir::Instruction::OpID::Phi) {
            break;
        }
        if (seen_phi) {
            return false;
        }
        seen_phi = true;
    }
    return seen_phi;
}

bool has_successor(oir::BasicBlock &block, oir::BasicBlock *succ) {
    for (auto *candidate : block.successors()) {
        if (candidate == succ) {
            return true;
        }
    }
    return false;
}

bool has_predecessor(oir::BasicBlock &block, oir::BasicBlock *pred) {
    for (auto *candidate : block.predecessors()) {
        if (candidate == pred) {
            return true;
        }
    }
    return false;
}

bool match_add_arm(oir::BasicBlock &pred, oir::BasicBlock *arm, oir::BasicBlock *merge,
                   bool arm_is_true, ConditionalAddDiamond &out) {
    if (arm == nullptr || merge == nullptr || arm == merge || arm->predecessors().size() != 1 ||
        arm->predecessors().front() != &pred || arm->instructions().size() != 2 ||
        !has_single_phi(*merge)) {
        return false;
    }

    auto inst_it = arm->instructions().begin();
    auto *add = dynamic_cast<oir::BinaryInst *>(inst_it->get());
    ++inst_it;
    auto *arm_branch = dynamic_cast<oir::BranchInst *>(inst_it->get());
    if (add == nullptr || add->op() != oir::Instruction::OpID::Add ||
        arm_branch == nullptr || arm_branch->is_conditional() ||
        arm_branch->target_bb() != merge ||
        add->type() != pred.parent()->parent()->types().int32_ty()) {
        return false;
    }

    auto *phi = dynamic_cast<oir::PhiInst *>(merge->instructions().front().get());
    if (phi == nullptr || phi->type() != add->type()) {
        return false;
    }

    oir::Value *from_arm = nullptr;
    oir::Value *from_pred = nullptr;
    for (const auto &[value, from] : phi->incoming()) {
        if (from == arm) {
            from_arm = value;
        } else if (from == &pred) {
            from_pred = value;
        } else {
            return false;
        }
    }
    if (from_arm != add || from_pred == nullptr) {
        return false;
    }

    oir::Value *increment = nullptr;
    if (add->lhs() == from_pred) {
        increment = add->rhs();
    } else if (add->rhs() == from_pred) {
        increment = add->lhs();
    } else {
        return false;
    }

    out = {arm, merge, add, phi, from_pred, increment, arm_is_true};
    return true;
}

bool match_conditional_add_diamond(oir::BasicBlock &block, oir::BranchInst &branch,
                                   ConditionalAddDiamond &out) {
    if (!branch.is_conditional() || branch.cond() == nullptr ||
        branch.cond()->type() != block.parent()->parent()->types().int1_ty()) {
        return false;
    }
    if (match_add_arm(block, branch.true_bb(), branch.false_bb(), true, out)) {
        return true;
    }
    return match_add_arm(block, branch.false_bb(), branch.true_bb(), false, out);
}

bool match_bool_arm(oir::BasicBlock &pred, oir::BasicBlock *arm, oir::BasicBlock *merge,
                    bool arm_is_true, ShortCircuitBoolDiamond &out) {
    if (arm == nullptr || merge == nullptr || arm == merge || arm->predecessors().size() != 1 ||
        arm->predecessors().front() != &pred || arm->instructions().size() != 2 ||
        !has_single_phi(*merge)) {
        return false;
    }

    auto inst_it = arm->instructions().begin();
    auto *cmp = dynamic_cast<oir::CmpInst *>(inst_it->get());
    ++inst_it;
    auto *arm_branch = dynamic_cast<oir::BranchInst *>(inst_it->get());
    if (cmp == nullptr || cmp->type() != pred.parent()->parent()->types().int1_ty() ||
        arm_branch == nullptr || arm_branch->is_conditional() ||
        arm_branch->target_bb() != merge) {
        return false;
    }

    auto *phi = dynamic_cast<oir::PhiInst *>(merge->instructions().front().get());
    if (phi == nullptr || phi->type() != pred.parent()->parent()->types().int1_ty()) {
        return false;
    }

    bool saw_arm = false;
    std::optional<std::int64_t> pred_value;
    for (const auto &[value, from] : phi->incoming()) {
        if (from == arm) {
            if (value != cmp) {
                return false;
            }
            saw_arm = true;
        } else if (from == &pred) {
            pred_value = int_constant(value);
        } else {
            return false;
        }
    }
    if (!saw_arm || !pred_value || (*pred_value != 0 && *pred_value != 1)) {
        return false;
    }

    out = {arm, merge, cmp, phi, arm_is_true, *pred_value != 0};
    return true;
}

bool match_short_circuit_bool_diamond(oir::BasicBlock &block, oir::BranchInst &branch,
                                      ShortCircuitBoolDiamond &out) {
    if (!branch.is_conditional() || branch.cond() == nullptr ||
        branch.cond()->type() != block.parent()->parent()->types().int1_ty()) {
        return false;
    }
    if (match_bool_arm(block, branch.true_bb(), branch.false_bb(), true, out)) {
        return true;
    }
    return match_bool_arm(block, branch.false_bb(), branch.true_bb(), false, out);
}

oir::BasicBlock *empty_select_arm_target(oir::BasicBlock &pred, oir::BasicBlock *arm) {
    if (arm == nullptr || arm == &pred || arm->predecessors().size() != 1 ||
        arm->predecessors().front() != &pred || arm->instructions().size() != 1) {
        return nullptr;
    }

    auto *arm_branch = dynamic_cast<oir::BranchInst *>(arm->instructions().front().get());
    if (arm_branch == nullptr || arm_branch->is_conditional()) {
        return nullptr;
    }
    return arm_branch->target_bb();
}

oir::Value *phi_incoming_from(oir::PhiInst &phi, oir::BasicBlock *from) {
    oir::Value *value = nullptr;
    for (const auto &[incoming_value, incoming_block] : phi.incoming()) {
        if (incoming_block != from) {
            continue;
        }
        if (value != nullptr) {
            return nullptr;
        }
        value = incoming_value;
    }
    return value;
}

bool finish_value_select_match(oir::BasicBlock &block, oir::BasicBlock *true_source,
                               oir::BasicBlock *false_source, oir::BasicBlock *merge,
                               std::vector<oir::BasicBlock *> removed_blocks,
                               ValueSelectDiamond &out) {
    auto *i32 = block.parent()->parent()->types().int32_ty();
    if (true_source == nullptr || false_source == nullptr || true_source == false_source ||
        merge == nullptr || !has_single_phi(*merge) || merge->instructions().empty()) {
        return false;
    }

    auto *phi = dynamic_cast<oir::PhiInst *>(merge->instructions().front().get());
    if (phi == nullptr || phi->type() != i32 || phi->incoming().size() != 2) {
        return false;
    }

    auto *true_value = phi_incoming_from(*phi, true_source);
    auto *false_value = phi_incoming_from(*phi, false_source);
    if (true_value == nullptr || false_value == nullptr || true_value->type() != i32 ||
        false_value->type() != i32 || true_value == false_value) {
        return false;
    }

    out = {true_source, false_source, merge, phi, true_value, false_value, removed_blocks};
    return true;
}

bool match_value_select_diamond(oir::BasicBlock &block, oir::BranchInst &branch,
                                ValueSelectDiamond &out) {
    if (!branch.is_conditional() || branch.cond() == nullptr ||
        branch.cond()->type() != block.parent()->parent()->types().int1_ty()) {
        return false;
    }

    auto *true_target = branch.true_bb();
    auto *false_target = branch.false_bb();
    if (empty_select_arm_target(block, true_target) == false_target) {
        return finish_value_select_match(block, true_target, &block, false_target, {true_target},
                                         out);
    }
    if (empty_select_arm_target(block, false_target) == true_target) {
        return finish_value_select_match(block, &block, false_target, true_target, {false_target},
                                         out);
    }

    auto *true_merge = empty_select_arm_target(block, true_target);
    auto *false_merge = empty_select_arm_target(block, false_target);
    if (true_merge == nullptr || true_merge != false_merge) {
        return false;
    }
    return finish_value_select_match(block, true_target, false_target, true_merge,
                                     {true_target, false_target}, out);
}

oir::Value *insert_condition_mask(oir::Module &module, oir::BasicBlock &block,
                                  std::list<std::unique_ptr<oir::Instruction>>::iterator before,
                                  oir::Value *condition, bool use_true_arm) {
    oir::Value *selected = condition;
    if (!use_true_arm) {
        selected = insert_bool_not(module, block, before, condition, "ifc.not");
    }

    auto zext = std::make_unique<oir::CastInst>(module.types().int32_ty(),
                                                oir::Instruction::OpID::ZExt, selected, &block,
                                                "ifc.zext");
    auto *zext_raw = zext.get();
    zext_raw->set_parent(&block);
    block.instructions().insert(before, std::move(zext));

    auto mask = std::make_unique<oir::BinaryInst>(
        module.types().int32_ty(), oir::Instruction::OpID::Sub, module.create_i32(0), zext_raw,
        &block, "ifc.mask");
    auto *mask_raw = mask.get();
    mask_raw->set_parent(&block);
    block.instructions().insert(before, std::move(mask));
    return mask_raw;
}

oir::Value *insert_zext_condition(oir::Module &module, oir::BasicBlock &block,
                                  std::list<std::unique_ptr<oir::Instruction>>::iterator before,
                                  oir::Value *condition, bool use_true_arm,
                                  const std::string &name) {
    oir::Value *selected = condition;
    if (!use_true_arm) {
        selected = insert_bool_not(module, block, before, condition, "ifc.not");
    }

    auto zext = std::make_unique<oir::CastInst>(module.types().int32_ty(),
                                                oir::Instruction::OpID::ZExt, selected, &block,
                                                name);
    auto *zext_raw = zext.get();
    zext_raw->set_parent(&block);
    block.instructions().insert(before, std::move(zext));
    return zext_raw;
}

oir::Value *insert_profitable_i32_select_expr(
    oir::Module &module, oir::BasicBlock &block,
    std::list<std::unique_ptr<oir::Instruction>>::iterator before, oir::Value *condition,
    oir::Value *true_value, oir::Value *false_value, const std::string &name) {
    auto true_constant = int_constant(true_value);
    auto false_constant = int_constant(false_value);
    if (!true_constant || !false_constant) {
        return nullptr;
    }

    if (*true_constant == 1 && *false_constant == 0) {
        return insert_zext_condition(module, block, before, condition, true, name);
    }
    if (*true_constant == 0 && *false_constant == 1) {
        return insert_zext_condition(module, block, before, condition, false, name);
    }
    if (*true_constant == -1 && *false_constant == 0) {
        auto *mask = insert_condition_mask(module, block, before, condition, true);
        mask->set_name(name);
        return mask;
    }
    if (*true_constant == 0 && *false_constant == -1) {
        auto *mask = insert_condition_mask(module, block, before, condition, false);
        mask->set_name(name);
        return mask;
    }

    return nullptr;
}

oir::Value *insert_arm_condition(oir::Module &module, oir::BasicBlock &block,
                                 std::list<std::unique_ptr<oir::Instruction>>::iterator before,
                                 oir::Value *condition, bool use_true_arm) {
    if (use_true_arm) {
        return condition;
    }
    return insert_bool_not(module, block, before, condition, "ifc.not");
}

void erase_phi(oir::BasicBlock &block, oir::PhiInst &phi) {
    for (auto it = block.instructions().begin(); it != block.instructions().end();) {
        if (it->get() != &phi) {
            ++it;
            continue;
        }
        (*it)->drop_all_operands();
        block.instructions().erase(it);
        return;
    }
}

void replace_conditional_branch_with_merge(oir::Module &module, oir::BasicBlock &block,
                                           std::list<std::unique_ptr<oir::Instruction>>::iterator
                                               term_it,
                                           oir::BasicBlock *removed,
                                           oir::BasicBlock *merge) {
    remove_edge(&block, removed);
    auto replacement = std::make_unique<oir::BranchInst>(module.types().void_ty(), merge, &block);
    replacement->set_parent(&block);
    (*term_it)->drop_all_operands();
    *term_it = std::move(replacement);
}

void replace_conditional_branch_with_merge(
    oir::Module &module, oir::BasicBlock &block,
    std::list<std::unique_ptr<oir::Instruction>>::iterator term_it,
    const std::vector<oir::BasicBlock *> &removed_blocks, oir::BasicBlock *merge) {
    for (auto *removed : removed_blocks) {
        remove_edge(&block, removed);
    }
    if (!has_successor(block, merge)) {
        block.add_successor(merge);
    }
    if (!has_predecessor(*merge, &block)) {
        merge->add_predecessor(&block);
    }

    auto replacement = std::make_unique<oir::BranchInst>(module.types().void_ty(), merge, &block);
    replacement->set_parent(&block);
    (*term_it)->drop_all_operands();
    *term_it = std::move(replacement);
}

bool convert_short_circuit_bool(oir::Module &module, oir::BasicBlock &block,
                                std::list<std::unique_ptr<oir::Instruction>>::iterator term_it,
                                ShortCircuitBoolDiamond &diamond, Stats &stats) {
    auto *branch = static_cast<oir::BranchInst *>(term_it->get());
    auto *arm_condition =
        insert_arm_condition(module, block, term_it, branch->cond(), diamond.arm_is_true);

    auto cloned_cmp = std::make_unique<oir::CmpInst>(
        diamond.arm_cmp->type(), diamond.arm_cmp->op(), diamond.arm_cmp->pred(),
        diamond.arm_cmp->lhs(), diamond.arm_cmp->rhs(), &block,
        diamond.arm_cmp->name().empty() ? "ifc.cmp" : diamond.arm_cmp->name() + ".ifc");
    auto *cmp_raw = cloned_cmp.get();
    cmp_raw->set_parent(&block);
    block.instructions().insert(term_it, std::move(cloned_cmp));

    oir::Value *replacement_value = nullptr;
    if (!diamond.pred_value) {
        auto combined = std::make_unique<oir::BinaryInst>(
            module.types().int1_ty(), oir::Instruction::OpID::And, arm_condition, cmp_raw, &block,
            diamond.phi->name().empty() ? "ifc.and" : diamond.phi->name() + ".ifc");
        auto *combined_raw = combined.get();
        combined_raw->set_parent(&block);
        replacement_value = combined_raw;
        block.instructions().insert(term_it, std::move(combined));
    } else {
        auto *not_cmp = insert_bool_not(module, block, term_it, cmp_raw, "ifc.notcmp");
        auto both = std::make_unique<oir::BinaryInst>(module.types().int1_ty(),
                                                      oir::Instruction::OpID::And, arm_condition,
                                                      not_cmp, &block, "ifc.block");
        auto *both_raw = both.get();
        both_raw->set_parent(&block);
        block.instructions().insert(term_it, std::move(both));
        replacement_value =
            insert_bool_not(module, block, term_it, both_raw,
                            diamond.phi->name().empty() ? "ifc.or" : diamond.phi->name() + ".ifc");
    }

    ReplacementMap replacements;
    replacements[diamond.phi] = replacement_value;
    apply_replacements(module, replacements);
    erase_phi(*diamond.merge, *diamond.phi);
    replace_conditional_branch_with_merge(module, block, term_it, diamond.arm, diamond.merge);
    ++stats.branches;
    return true;
}

bool convert_conditional_add(oir::Module &module, oir::BasicBlock &block,
                             std::list<std::unique_ptr<oir::Instruction>>::iterator term_it,
                             ConditionalAddDiamond &diamond, Stats &stats) {
    auto *branch = static_cast<oir::BranchInst *>(term_it->get());
    auto *mask = insert_condition_mask(module, block, term_it, branch->cond(), diamond.arm_is_true);

    auto masked_increment = std::make_unique<oir::BinaryInst>(
        diamond.increment->type(), oir::Instruction::OpID::And, diamond.increment, mask, &block,
        "ifc.inc");
    auto *masked_raw = masked_increment.get();
    masked_raw->set_parent(&block);
    block.instructions().insert(term_it, std::move(masked_increment));

    auto selected_add = std::make_unique<oir::BinaryInst>(
        diamond.base->type(), oir::Instruction::OpID::Add, diamond.base, masked_raw, &block,
        diamond.phi->name().empty() ? "ifc.add" : diamond.phi->name() + ".ifc");
    auto *selected_raw = selected_add.get();
    selected_raw->set_parent(&block);
    block.instructions().insert(term_it, std::move(selected_add));

    ReplacementMap replacements;
    replacements[diamond.phi] = selected_raw;
    apply_replacements(module, replacements);
    erase_phi(*diamond.merge, *diamond.phi);

    replace_conditional_branch_with_merge(module, block, term_it, diamond.arm, diamond.merge);
    ++stats.branches;
    return true;
}

bool convert_value_select(oir::Module &module, oir::BasicBlock &block,
                          std::list<std::unique_ptr<oir::Instruction>>::iterator term_it,
                          ValueSelectDiamond &diamond, Stats &stats) {
    auto *branch = static_cast<oir::BranchInst *>(term_it->get());
    auto *selected = insert_profitable_i32_select_expr(
        module, block, term_it, branch->cond(), diamond.true_value, diamond.false_value,
        diamond.phi->name().empty() ? "ifc.select" : diamond.phi->name() + ".ifc");
    if (selected == nullptr) {
        return false;
    }

    ReplacementMap replacements;
    replacements[diamond.phi] = selected;
    apply_replacements(module, replacements);
    erase_phi(*diamond.merge, *diamond.phi);

    replace_conditional_branch_with_merge(module, block, term_it, diamond.removed_blocks,
                                          diamond.merge);
    ++stats.branches;
    return true;
}

bool if_convert_conditional_adds(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        for (auto &block : function->blocks()) {
            if (!block->has_terminator()) {
                continue;
            }
            auto term_it = std::prev(block->instructions().end());
            auto *branch = dynamic_cast<oir::BranchInst *>(term_it->get());
            if (branch == nullptr) {
                continue;
            }
            ShortCircuitBoolDiamond bool_diamond;
            if (match_short_circuit_bool_diamond(*block, *branch, bool_diamond)) {
                changed |= convert_short_circuit_bool(module, *block, term_it, bool_diamond, stats);
                continue;
            }
            ConditionalAddDiamond diamond;
            if (!match_conditional_add_diamond(*block, *branch, diamond)) {
                ValueSelectDiamond select_diamond;
                if (match_value_select_diamond(*block, *branch, select_diamond)) {
                    changed |= convert_value_select(module, *block, term_it, select_diamond, stats);
                }
                continue;
            }
            changed |= convert_conditional_add(module, *block, term_it, diamond, stats);
        }
    }
    return changed;
}

bool local_simplify(oir::Module &module, Stats &stats, SimplifyMode mode) {
    oir::UseAnalysis uses(module);
    ReplacementMap replacements;
    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        for (auto &block : function->blocks()) {
            for (auto it = block->instructions().begin(); it != block->instructions().end(); ++it) {
                auto *replacement = simplify_instruction(module, *block, it, **it, mode);
                if (replacement != nullptr && replacement != it->get() &&
                    replacement->type() == (*it)->type() && uses.has_uses(it->get())) {
                    replacements[it->get()] = replacement;
                }
            }
        }
    }

    if (apply_replacements(module, replacements) == 0) {
        return false;
    }
    stats.folded += static_cast<unsigned>(replacements.size());
    return true;
}

} // namespace pass::oir_opt
