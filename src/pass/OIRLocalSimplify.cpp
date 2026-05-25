#include "../../include/oir/OIRScalarOpt.h"

#include "../../include/oir/OIRAnalysis.h"

#include <iterator>
#include <memory>
#include <optional>

namespace pass::oir_opt {

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
        case oir::Instruction::OpID::And:
            if (is_int_value(binary->rhs(), 0) || is_int_value(binary->lhs(), 0)) {
                return make_zero_constant(module, inst.type());
            }
            if (is_int_value(binary->rhs(), -1)) {
                return binary->lhs();
            }
            if (is_int_value(binary->lhs(), -1)) {
                return binary->rhs();
            }
            break;
        case oir::Instruction::OpID::Shl:
        case oir::Instruction::OpID::LShr:
        case oir::Instruction::OpID::AShr:
            if (is_int_value(binary->rhs(), 0)) {
                return binary->lhs();
            }
            if (is_int_value(binary->lhs(), 0)) {
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
