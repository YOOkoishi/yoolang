#include "../../include/oir/OIRScalarOpt.h"

#include "../../include/oir/OIR.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pass::oir_opt {
namespace {

enum class BitwiseKind {
    And,
    Or,
    Xor,
};

struct AccumulatorMatch {
    oir::Argument *lhs = nullptr;
    oir::Argument *rhs = nullptr;
    BitwiseKind kind = BitwiseKind::And;
};

std::vector<oir::Instruction *> instructions(oir::BasicBlock *block) {
    std::vector<oir::Instruction *> out;
    if (block == nullptr) {
        return out;
    }
    for (auto &inst : block->instructions()) {
        out.push_back(inst.get());
    }
    return out;
}

oir::BranchInst *as_branch(oir::Instruction *inst) {
    return dynamic_cast<oir::BranchInst *>(inst);
}

oir::ReturnInst *as_return(oir::Instruction *inst) {
    return dynamic_cast<oir::ReturnInst *>(inst);
}

oir::PhiInst *as_phi(oir::Value *value) {
    return dynamic_cast<oir::PhiInst *>(value);
}

oir::BinaryInst *as_binary(oir::Value *value) {
    return dynamic_cast<oir::BinaryInst *>(value);
}

oir::CmpInst *as_cmp(oir::Value *value) {
    return dynamic_cast<oir::CmpInst *>(value);
}

bool is_const(oir::Value *value, std::int64_t expected) {
    auto constant = int_constant(value);
    return constant && *constant == expected;
}

oir::Value *incoming_from(oir::PhiInst *phi, oir::BasicBlock *block) {
    if (phi == nullptr || block == nullptr) {
        return nullptr;
    }
    for (const auto &[value, from] : phi->incoming()) {
        if (from == block) {
            return value;
        }
    }
    return nullptr;
}

bool branches_to(oir::BasicBlock *block, oir::BasicBlock *target) {
    auto instrs = instructions(block);
    if (instrs.empty()) {
        return false;
    }
    auto *branch = as_branch(instrs.back());
    return branch != nullptr && !branch->is_conditional() && branch->target_bb() == target;
}

bool is_binary_const_rhs(oir::Value *value, oir::Instruction::OpID op, oir::Value *lhs,
                         std::int64_t rhs) {
    auto *binary = as_binary(value);
    return binary != nullptr && binary->op() == op && binary->lhs() == lhs &&
           is_const(binary->rhs(), rhs);
}

bool is_add_of(oir::Value *value, oir::Value *lhs, oir::Value *rhs) {
    auto *binary = as_binary(value);
    if (binary == nullptr || binary->op() != oir::Instruction::OpID::Add) {
        return false;
    }
    return (binary->lhs() == lhs && binary->rhs() == rhs) ||
           (binary->lhs() == rhs && binary->rhs() == lhs);
}

bool is_icmp_const(oir::Value *value, oir::CmpPred pred, oir::Value *lhs, std::int64_t rhs) {
    auto *cmp = as_cmp(value);
    return cmp != nullptr && cmp->pred() == pred && cmp->lhs() == lhs &&
           is_const(cmp->rhs(), rhs);
}

bool is_icmp_pair(oir::Value *value, oir::CmpPred pred, oir::Value *lhs, oir::Value *rhs) {
    auto *cmp = as_cmp(value);
    return cmp != nullptr && cmp->pred() == pred &&
           ((cmp->lhs() == lhs && cmp->rhs() == rhs) ||
            (cmp->lhs() == rhs && cmp->rhs() == lhs));
}

bool match_add_block(oir::BasicBlock *block, oir::BasicBlock *latch, oir::Value *result,
                     oir::Value *power, oir::Value *&sum) {
    auto instrs = instructions(block);
    if (instrs.size() != 2 || !branches_to(block, latch)) {
        return false;
    }
    if (!is_add_of(instrs[0], result, power)) {
        return false;
    }
    sum = instrs[0];
    return true;
}

bool match_logic_phi(oir::PhiInst *phi, oir::Value *computed, oir::BasicBlock *computed_block,
                     std::int64_t shortcut_value, oir::BasicBlock *shortcut_block) {
    return incoming_from(phi, computed_block) == computed &&
           is_const(incoming_from(phi, shortcut_block), shortcut_value);
}

std::optional<BitwiseKind>
match_and_or_shape(oir::BasicBlock *body, oir::BranchInst *body_branch, oir::Value *a_is_one,
                   oir::Value *b_is_one, oir::PhiInst *result_update, oir::Value *result,
                   oir::Value *power, oir::BasicBlock *latch) {
    if (body_branch->cond() != a_is_one) {
        return std::nullopt;
    }

    auto try_match = [&](BitwiseKind kind, oir::BasicBlock *compute_block,
                         oir::BasicBlock *logic_block,
                         std::int64_t shortcut_value) -> std::optional<BitwiseKind> {
        auto compute_instrs = instructions(compute_block);
        auto logic_instrs = instructions(logic_block);
        if (compute_instrs.size() != 2 || logic_instrs.size() != 2 ||
            !branches_to(compute_block, logic_block)) {
            return std::nullopt;
        }
        if (!is_icmp_const(compute_instrs[0], oir::CmpPred::EQ, b_is_one, 1)) {
            return std::nullopt;
        }
        auto *logic_phi = as_phi(logic_instrs[0]);
        auto *logic_branch = as_branch(logic_instrs[1]);
        if (logic_phi == nullptr || logic_branch == nullptr || !logic_branch->is_conditional() ||
            logic_branch->cond() != logic_phi ||
            !match_logic_phi(logic_phi, compute_instrs[0], compute_block, shortcut_value, body)) {
            return std::nullopt;
        }

        auto *add_block = logic_branch->true_bb();
        if (logic_branch->false_bb() != latch) {
            return std::nullopt;
        }
        oir::Value *sum = nullptr;
        if (!match_add_block(add_block, latch, result, power, sum) ||
            incoming_from(result_update, add_block) != sum ||
            incoming_from(result_update, logic_block) != result) {
            return std::nullopt;
        }
        return kind;
    };

    if (auto matched = try_match(BitwiseKind::And, body_branch->true_bb(),
                                 body_branch->false_bb(), 0)) {
        return matched;
    }
    return try_match(BitwiseKind::Or, body_branch->false_bb(), body_branch->true_bb(), 1);
}

std::optional<BitwiseKind>
match_xor_shape(oir::BranchInst *body_branch, oir::Value *a_rem, oir::Value *b_rem,
                oir::PhiInst *result_update, oir::Value *result, oir::Value *power,
                oir::BasicBlock *body, oir::BasicBlock *latch) {
    if (!is_icmp_pair(body_branch->cond(), oir::CmpPred::NE, a_rem, b_rem) ||
        body_branch->false_bb() != latch) {
        return std::nullopt;
    }

    oir::Value *sum = nullptr;
    auto *add_block = body_branch->true_bb();
    if (!match_add_block(add_block, latch, result, power, sum) ||
        incoming_from(result_update, add_block) != sum ||
        incoming_from(result_update, body) != result) {
        return std::nullopt;
    }
    return BitwiseKind::Xor;
}

std::optional<AccumulatorMatch> match_bit_accumulator(oir::Function &function) {
    if (function.is_external() || function.args().size() != 2 ||
        function.return_type() != function.parent()->types().int32_ty() ||
        function.args()[0]->type() != function.parent()->types().int32_ty() ||
        function.args()[1]->type() != function.parent()->types().int32_ty()) {
        return std::nullopt;
    }

    auto *entry = function.entry_block();
    auto entry_instrs = instructions(entry);
    if (entry_instrs.size() != 1) {
        return std::nullopt;
    }
    auto *entry_branch = as_branch(entry_instrs[0]);
    if (entry_branch == nullptr || entry_branch->is_conditional()) {
        return std::nullopt;
    }

    auto *header = entry_branch->target_bb();
    auto header_instrs = instructions(header);
    if (header_instrs.size() != 7) {
        return std::nullopt;
    }

    std::array<oir::PhiInst *, 5> phis{};
    for (std::size_t i = 0; i < phis.size(); ++i) {
        phis[i] = as_phi(header_instrs[i]);
        if (phis[i] == nullptr) {
            return std::nullopt;
        }
    }
    auto *loop_cmp = as_cmp(header_instrs[5]);
    auto *loop_branch = as_branch(header_instrs[6]);
    if (loop_cmp == nullptr || loop_branch == nullptr || !loop_branch->is_conditional()) {
        return std::nullopt;
    }

    oir::PhiInst *a_phi = nullptr;
    oir::PhiInst *b_phi = nullptr;
    oir::PhiInst *result_phi = nullptr;
    oir::PhiInst *power_phi = nullptr;
    oir::PhiInst *len_phi = nullptr;
    for (auto *phi : phis) {
        auto *entry_value = incoming_from(phi, entry);
        if (entry_value == function.args()[0].get()) {
            a_phi = phi;
        } else if (entry_value == function.args()[1].get()) {
            b_phi = phi;
        } else if (is_const(entry_value, 0)) {
            result_phi = phi;
        } else if (is_const(entry_value, 1)) {
            power_phi = phi;
        } else if (is_const(entry_value, 32)) {
            len_phi = phi;
        }
    }
    if (a_phi == nullptr || b_phi == nullptr || result_phi == nullptr || power_phi == nullptr ||
        len_phi == nullptr || !is_icmp_const(loop_cmp, oir::CmpPred::NE, len_phi, 0)) {
        return std::nullopt;
    }

    auto *body = loop_branch->true_bb();
    auto *exit = loop_branch->false_bb();
    auto exit_instrs = instructions(exit);
    if (exit_instrs.size() != 1) {
        return std::nullopt;
    }
    auto *ret = as_return(exit_instrs[0]);
    if (ret == nullptr || !ret->has_value() || ret->value() != result_phi) {
        return std::nullopt;
    }

    auto body_instrs = instructions(body);
    if (body_instrs.size() < 6) {
        return std::nullopt;
    }
    auto *body_branch = as_branch(body_instrs.back());
    if (body_branch == nullptr || !body_branch->is_conditional()) {
        return std::nullopt;
    }

    oir::Value *a_rem = nullptr;
    oir::Value *b_rem = nullptr;
    oir::Value *a_div = nullptr;
    oir::Value *b_div = nullptr;
    for (auto *inst : body_instrs) {
        if (is_binary_const_rhs(inst, oir::Instruction::OpID::SRem, a_phi, 2)) {
            a_rem = inst;
        } else if (is_binary_const_rhs(inst, oir::Instruction::OpID::SRem, b_phi, 2)) {
            b_rem = inst;
        } else if (is_binary_const_rhs(inst, oir::Instruction::OpID::SDiv, a_phi, 2)) {
            a_div = inst;
        } else if (is_binary_const_rhs(inst, oir::Instruction::OpID::SDiv, b_phi, 2)) {
            b_div = inst;
        }
    }
    if (a_rem == nullptr || b_rem == nullptr || a_div == nullptr || b_div == nullptr) {
        return std::nullopt;
    }

    oir::BasicBlock *latch = nullptr;
    for (auto &candidate : function.blocks()) {
        if (candidate.get() == entry) {
            continue;
        }
        auto *back_value = incoming_from(len_phi, candidate.get());
        if (back_value != nullptr) {
            latch = candidate.get();
            break;
        }
    }
    if (latch == nullptr || incoming_from(a_phi, latch) != a_div ||
        incoming_from(b_phi, latch) != b_div ||
        !is_binary_const_rhs(incoming_from(power_phi, latch), oir::Instruction::OpID::Mul,
                             power_phi, 2) ||
        !is_binary_const_rhs(incoming_from(len_phi, latch), oir::Instruction::OpID::Sub, len_phi,
                             1) ||
        !branches_to(latch, header)) {
        return std::nullopt;
    }

    auto *result_update_phi = as_phi(incoming_from(result_phi, latch));
    if (result_update_phi == nullptr) {
        return std::nullopt;
    }

    std::optional<BitwiseKind> kind;
    if (is_icmp_const(body_branch->cond(), oir::CmpPred::EQ, a_rem, 1)) {
        kind = match_and_or_shape(body, body_branch, body_branch->cond(), b_rem,
                                  result_update_phi, result_phi, power_phi, latch);
    }
    if (!kind) {
        kind = match_xor_shape(body_branch, a_rem, b_rem, result_update_phi, result_phi,
                               power_phi, body, latch);
    }
    if (!kind) {
        return std::nullopt;
    }

    return AccumulatorMatch{function.args()[0].get(), function.args()[1].get(), *kind};
}

void clear_body(oir::Function &function) {
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            inst->drop_all_operands();
        }
    }
    function.blocks().clear();
}

oir::Value *create_positive_part(oir::Function &function, oir::IRBuilder &builder,
                                 oir::Value *value, const std::string &prefix) {
    auto *module = function.parent();
    auto *positive_block = function.create_block(prefix + ".positive");
    auto *zero_block = function.create_block(prefix + ".zero");
    auto *join_block = function.create_block(prefix + ".join");

    auto *is_positive =
        builder.create_icmp(oir::CmpPred::GT, value, builder.i32(0), prefix + ".is_positive");
    builder.create_cond_br(is_positive, positive_block, zero_block);

    builder.set_insert_point(positive_block);
    builder.create_br(join_block);

    builder.set_insert_point(zero_block);
    builder.create_br(join_block);

    builder.set_insert_point(join_block);
    auto *phi = builder.create_phi(module->types().int32_ty(), prefix + ".value");
    phi->add_incoming(value, positive_block);
    phi->add_incoming(builder.i32(0), zero_block);
    return phi;
}

struct MagnitudeValue {
    oir::Value *value = nullptr;
    oir::Value *is_negative = nullptr;
};

MagnitudeValue create_signed_magnitude(oir::Function &function, oir::IRBuilder &builder,
                                        oir::Value *value, const std::string &prefix) {
    auto *module = function.parent();
    auto *negative_block = function.create_block(prefix + ".negative");
    auto *nonnegative_block = function.create_block(prefix + ".nonnegative");
    auto *join_block = function.create_block(prefix + ".join");

    auto *is_negative =
        builder.create_icmp(oir::CmpPred::LT, value, builder.i32(0), prefix + ".is_negative");
    builder.create_cond_br(is_negative, negative_block, nonnegative_block);

    builder.set_insert_point(negative_block);
    auto *negated =
        builder.create_binary(oir::Instruction::OpID::Sub, builder.i32(0), value, prefix + ".neg");
    builder.create_br(join_block);

    builder.set_insert_point(nonnegative_block);
    builder.create_br(join_block);

    builder.set_insert_point(join_block);
    auto *phi = builder.create_phi(module->types().int32_ty(), prefix + ".magnitude");
    phi->add_incoming(negated, negative_block);
    phi->add_incoming(value, nonnegative_block);
    return MagnitudeValue{phi, is_negative};
}

bool rewrite_accumulator(oir::Function &function, const AccumulatorMatch &match) {
    auto &module = *function.parent();
    clear_body(function);

    auto *entry = function.create_block("bitacc.entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);

    oir::Value *result = nullptr;
    if (match.kind == BitwiseKind::Xor) {
        auto lhs = create_signed_magnitude(function, builder, match.lhs, "bitacc.lhs");
        auto rhs = create_signed_magnitude(function, builder, match.rhs, "bitacc.rhs");
        auto *common =
            builder.create_binary(oir::Instruction::OpID::And, lhs.value, rhs.value, "bitacc.and");
        auto *sum =
            builder.create_binary(oir::Instruction::OpID::Add, lhs.value, rhs.value, "bitacc.sum");
        auto *twice_common =
            builder.create_binary(oir::Instruction::OpID::Add, common, common, "bitacc.twice");
        auto *xor_value =
            builder.create_binary(oir::Instruction::OpID::Sub, sum, twice_common, "bitacc.xor");
        auto *sign_diff = builder.create_icmp(oir::CmpPred::NE, lhs.is_negative, rhs.is_negative,
                                             "bitacc.sign_diff");
        auto *sign_diff_i32 =
            builder.create_zext(sign_diff, module.types().int32_ty(), "bitacc.sign_diff.i32");
        auto *sign_mask = builder.create_binary(oir::Instruction::OpID::Sub, builder.i32(0),
                                                sign_diff_i32, "bitacc.sign_mask");
        auto *extra =
            builder.create_binary(oir::Instruction::OpID::And, common, sign_mask, "bitacc.extra");
        result = builder.create_binary(oir::Instruction::OpID::Add, xor_value, extra,
                                       "bitacc.xor.signed");
    } else {
        auto *lhs = create_positive_part(function, builder, match.lhs, "bitacc.lhs");
        auto *rhs = create_positive_part(function, builder, match.rhs, "bitacc.rhs");
        auto *common =
            builder.create_binary(oir::Instruction::OpID::And, lhs, rhs, "bitacc.and");
        result = common;
        if (match.kind == BitwiseKind::Or) {
            auto *sum = builder.create_binary(oir::Instruction::OpID::Add, lhs, rhs, "bitacc.sum");
            result = builder.create_binary(oir::Instruction::OpID::Sub, sum, common, "bitacc.or");
        }
    }
    builder.create_ret(result);
    return true;
}

} // namespace

bool simplify_bitwise_accumulator_loops(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        auto match = match_bit_accumulator(*function);
        if (!match) {
            continue;
        }
        if (rewrite_accumulator(*function, *match)) {
            changed = true;
            ++stats.lsr;
        }
    }
    return changed;
}

} // namespace pass::oir_opt
