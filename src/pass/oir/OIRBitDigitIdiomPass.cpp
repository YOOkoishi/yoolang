#include "oir/OIRScalarOpt.h"

#include "oir/OIRAnalysis.h"
#include "oir/OIRCFGUtils.h"
#include "pass/oir/OIRCostModel.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

// This pass recognizes the following *semantic* recurrence, rather than a
// source-level helper or symbol name:
//
//   qx(0) = x, qx(k + 1) = sdiv(qx(k), 2)
//   qy(0) = y, qy(k + 1) = sdiv(qy(k), 2)
//   p(0)  = 1, p(k + 1)  = p(k) * 2             (W-bit wrap)
//   r(0)  = 0, r(k + 1)  = r(k) + (c(k) ? p(k) : 0)
//
// for exactly W iterations, where W is the matched fixed-width integer type's
// bit width.  c(k) is one of the three boolean forms matched below.  Signed
// division truncates toward zero, so the kth remainder is the kth magnitude bit
// with value +1 for non-negative inputs and -1 for negative inputs.
// Consequently, for every matched bit pattern (including the signed minimum):
//
//   eq1(rx) && eq1(ry) = (x >= 0 && y >= 0) ? (x & y) : 0
//   eq1(rx) || eq1(ry) = nonneg(x) | nonneg(y)
//   rx != ry           = same_sign(x,y) ? (abs(x) ^ abs(y))
//                                      : (abs(x) | abs(y))
//
// Here abs is two's-complement magnitude modulo 2^W.  The reconstruction sum
// is also modulo 2^W, including the sign-bit weight p(W - 1), so the closed
// forms preserve the original wrap behavior instead of relying on a
// non-negative-input assumption.

enum class DigitBooleanKind {
    AndPositiveBits,
    OrPositiveBits,
    XorSignedRemainders,
};

struct ParityMatch {
    oir::PhiInst *source = nullptr;
    std::vector<oir::Instruction *> instructions;
};

struct BitDigitLoopMatch {
    DigitBooleanKind kind = DigitBooleanKind::AndPositiveBits;
    std::int64_t bit_width = 0;
    oir::BasicBlock *preheader = nullptr;
    oir::BasicBlock *header = nullptr;
    oir::BasicBlock *latch = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::PhiInst *lhs_phi = nullptr;
    oir::PhiInst *rhs_phi = nullptr;
    oir::PhiInst *result_phi = nullptr;
    oir::Value *lhs_initial = nullptr;
    oir::Value *rhs_initial = nullptr;
    std::vector<oir::BasicBlock *> loop_blocks;
    std::unordered_set<oir::Instruction *> recognized;
};

bool contains_block(const oir::Loop &loop, const oir::BasicBlock *block) {
    return std::find(loop.blocks.begin(), loop.blocks.end(), block) != loop.blocks.end();
}

bool has_only_block(const std::vector<oir::BasicBlock *> &blocks, oir::BasicBlock *expected) {
    return blocks.size() == 1 && blocks.front() == expected;
}

bool is_unconditional_to(oir::BasicBlock *from, oir::BasicBlock *to) {
    auto *branch = from == nullptr ? nullptr : dynamic_cast<oir::BranchInst *>(from->terminator());
    return branch != nullptr && !branch->is_conditional() && branch->target_bb() == to &&
           has_only_block(from->successors(), to);
}

oir::Value *incoming_value_from(const oir::PhiInst &phi, oir::BasicBlock *predecessor) {
    oir::Value *value = nullptr;
    unsigned count = 0;
    for (const auto &[incoming, block] : phi.incoming()) {
        if (block == predecessor) {
            value = incoming;
            ++count;
        }
    }
    return count == 1 ? value : nullptr;
}

bool is_i32(const oir::Value *value) {
    // Closed-form materialization currently uses OIR's i32 builders.  Within
    // that supported type domain, derive the recurrence width from the type
    // instead of treating 32 as an idiom or benchmark constant.
    if (value == nullptr) {
        return false;
    }
    auto *integer = dynamic_cast<oir::IntegerType *>(value->type());
    return integer != nullptr && integer->bit_width() == 32;
}

bool same_value(oir::Value *lhs, oir::Value *rhs) {
    return lhs == rhs || same_constant_value(lhs, rhs);
}

bool matches_binary(oir::Value *value, oir::Instruction::OpID op, oir::Value *lhs, oir::Value *rhs,
                    bool commutative = false) {
    auto *binary = dynamic_cast<oir::BinaryInst *>(value);
    if (binary == nullptr || binary->op() != op) {
        return false;
    }
    return (same_value(binary->lhs(), lhs) && same_value(binary->rhs(), rhs)) ||
           (commutative && same_value(binary->lhs(), rhs) && same_value(binary->rhs(), lhs));
}

oir::Value *other_cmp_operand(oir::CmpInst &cmp, std::int64_t constant) {
    if (is_int_value(cmp.lhs(), constant)) {
        return cmp.rhs();
    }
    if (is_int_value(cmp.rhs(), constant)) {
        return cmp.lhs();
    }
    return nullptr;
}

std::optional<ParityMatch> match_positive_odd(oir::Value *condition, oir::PhiInst *first,
                                              oir::PhiInst *second) {
    auto *cmp = dynamic_cast<oir::CmpInst *>(condition);
    if (cmp == nullptr || cmp->op() != oir::Instruction::OpID::ICmp ||
        cmp->pred() != oir::CmpPred::EQ) {
        return std::nullopt;
    }
    auto *tested = other_cmp_operand(*cmp, 1);
    if (tested == nullptr) {
        return std::nullopt;
    }

    ParityMatch out;
    out.instructions.push_back(cmp);
    if (auto *rem = dynamic_cast<oir::BinaryInst *>(tested)) {
        if (rem->op() == oir::Instruction::OpID::SRem && is_int_value(rem->rhs(), 2) &&
            (rem->lhs() == first || rem->lhs() == second)) {
            out.source = static_cast<oir::PhiInst *>(rem->lhs());
            out.instructions.push_back(rem);
            return out;
        }
        // LocalSimplify proves `(srem x, 2) == 1` as
        // `(x & 0x80000001) == 1`; the sign bit is part of the mask precisely
        // because negative odd remainders are -1, not +1.
        if (rem->op() == oir::Instruction::OpID::And &&
            is_int_value(rem->rhs(), static_cast<std::int32_t>(0x80000001u)) &&
            (rem->lhs() == first || rem->lhs() == second)) {
            out.source = static_cast<oir::PhiInst *>(rem->lhs());
            out.instructions.push_back(rem);
            return out;
        }
        if (rem->op() == oir::Instruction::OpID::And &&
            is_int_value(rem->lhs(), static_cast<std::int32_t>(0x80000001u)) &&
            (rem->rhs() == first || rem->rhs() == second)) {
            out.source = static_cast<oir::PhiInst *>(rem->rhs());
            out.instructions.push_back(rem);
            return out;
        }
    }
    return std::nullopt;
}

oir::PhiInst *match_signed_rem_two(oir::Value *value, oir::PhiInst *first, oir::PhiInst *second,
                                   std::unordered_set<oir::Instruction *> &recognized) {
    auto *rem = dynamic_cast<oir::BinaryInst *>(value);
    if (rem == nullptr || rem->op() != oir::Instruction::OpID::SRem ||
        !is_int_value(rem->rhs(), 2) || (rem->lhs() != first && rem->lhs() != second)) {
        return nullptr;
    }
    recognized.insert(rem);
    return static_cast<oir::PhiInst *>(rem->lhs());
}

bool has_no_exit_phis(oir::BasicBlock *exit) {
    if (exit == nullptr) {
        return false;
    }
    for (const auto &instruction : exit->instructions()) {
        if (dynamic_cast<oir::PhiInst *>(instruction.get()) != nullptr) {
            return false;
        }
        break;
    }
    return true;
}

bool users_stay_in_loop_except_result(const oir::Loop &loop, const BitDigitLoopMatch &match) {
    bool result_has_external_use = false;
    for (auto *block_const : loop.blocks) {
        auto *block = const_cast<oir::BasicBlock *>(block_const);
        for (const auto &instruction : block->instructions()) {
            auto *value = instruction.get();
            for (auto *user : value->users()) {
                auto *use_instruction = dynamic_cast<oir::Instruction *>(user);
                if (use_instruction == nullptr || use_instruction->parent() == nullptr) {
                    return false;
                }
                if (!contains_block(loop, use_instruction->parent())) {
                    if (value != match.result_phi) {
                        return false;
                    }
                    result_has_external_use = true;
                }
            }
        }
    }
    return result_has_external_use;
}

std::optional<BitDigitLoopMatch> match_bit_digit_loop(const oir::Loop &loop) {
    if (loop.header == nullptr || loop.latches.size() != 1) {
        return std::nullopt;
    }
    auto *header = const_cast<oir::BasicBlock *>(loop.header);
    auto *latch = const_cast<oir::BasicBlock *>(loop.latches.front());
    if (header == latch || !is_unconditional_to(latch, header)) {
        return std::nullopt;
    }

    oir::BasicBlock *preheader = nullptr;
    for (auto *predecessor : header->predecessors()) {
        if (contains_block(loop, predecessor)) {
            continue;
        }
        if (preheader != nullptr) {
            return std::nullopt;
        }
        preheader = predecessor;
    }
    if (preheader == nullptr || !is_unconditional_to(preheader, header)) {
        return std::nullopt;
    }

    std::vector<oir::PhiInst *> phis;
    for (const auto &instruction : header->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(instruction.get());
        if (phi == nullptr) {
            break;
        }
        if (!is_i32(phi) || phi->incoming().size() != 2 ||
            incoming_value_from(*phi, preheader) == nullptr ||
            incoming_value_from(*phi, latch) == nullptr) {
            return std::nullopt;
        }
        phis.push_back(phi);
    }
    if (phis.size() != 5) {
        return std::nullopt;
    }
    auto *matched_integer_type = dynamic_cast<oir::IntegerType *>(phis.front()->type());
    if (matched_integer_type == nullptr || matched_integer_type->bit_width() == 0) {
        return std::nullopt;
    }
    const auto bit_width = static_cast<std::int64_t>(matched_integer_type->bit_width());

    oir::PhiInst *len_phi = nullptr;
    oir::PhiInst *power_phi = nullptr;
    oir::PhiInst *result_phi = nullptr;
    std::vector<oir::PhiInst *> quotient_phis;
    std::unordered_set<oir::Instruction *> recognized;
    for (auto *phi : phis) {
        recognized.insert(phi);
        auto *initial = incoming_value_from(*phi, preheader);
        auto *next = incoming_value_from(*phi, latch);
        auto *next_binary = dynamic_cast<oir::BinaryInst *>(next);
        if (is_int_value(initial, bit_width) && next_binary != nullptr &&
            next_binary->op() == oir::Instruction::OpID::Sub && next_binary->lhs() == phi &&
            is_int_value(next_binary->rhs(), 1)) {
            if (len_phi != nullptr) {
                return std::nullopt;
            }
            len_phi = phi;
            recognized.insert(static_cast<oir::Instruction *>(next));
            continue;
        }
        const bool doubled =
            (next_binary != nullptr && next_binary->op() == oir::Instruction::OpID::Mul &&
             ((next_binary->lhs() == phi && is_int_value(next_binary->rhs(), 2)) ||
              (next_binary->rhs() == phi && is_int_value(next_binary->lhs(), 2)))) ||
            matches_binary(next, oir::Instruction::OpID::Add, phi, phi);
        if (is_int_value(initial, 1) && doubled) {
            if (power_phi != nullptr) {
                return std::nullopt;
            }
            power_phi = phi;
            recognized.insert(static_cast<oir::Instruction *>(next));
            continue;
        }
        if (next_binary != nullptr && next_binary->op() == oir::Instruction::OpID::SDiv &&
            next_binary->lhs() == phi && is_int_value(next_binary->rhs(), 2)) {
            quotient_phis.push_back(phi);
            recognized.insert(static_cast<oir::Instruction *>(next));
            continue;
        }
        if (is_int_value(initial, 0)) {
            if (result_phi != nullptr) {
                return std::nullopt;
            }
            result_phi = phi;
            continue;
        }
        return std::nullopt;
    }
    if (len_phi == nullptr || power_phi == nullptr || result_phi == nullptr ||
        quotient_phis.size() != 2) {
        return std::nullopt;
    }

    auto *lhs_phi = quotient_phis[0];
    auto *rhs_phi = quotient_phis[1];
    auto *lhs_initial = incoming_value_from(*lhs_phi, preheader);
    auto *rhs_initial = incoming_value_from(*rhs_phi, preheader);
    if (lhs_initial == nullptr || rhs_initial == nullptr || !is_i32(lhs_initial) ||
        !is_i32(rhs_initial) || dynamic_cast<oir::UndefValue *>(lhs_initial) != nullptr ||
        dynamic_cast<oir::UndefValue *>(rhs_initial) != nullptr) {
        return std::nullopt;
    }

    auto *header_branch = dynamic_cast<oir::BranchInst *>(header->terminator());
    auto *header_cmp =
        header_branch == nullptr ? nullptr : dynamic_cast<oir::CmpInst *>(header_branch->cond());
    if (header_branch == nullptr || !header_branch->is_conditional() || header_cmp == nullptr ||
        header_cmp->op() != oir::Instruction::OpID::ICmp ||
        header_cmp->pred() != oir::CmpPred::NE || other_cmp_operand(*header_cmp, 0) != len_phi ||
        !contains_block(loop, header_branch->true_bb()) ||
        contains_block(loop, header_branch->false_bb())) {
        return std::nullopt;
    }
    auto *body = header_branch->true_bb();
    auto *exit = header_branch->false_bb();
    if (!has_no_exit_phis(exit)) {
        return std::nullopt;
    }
    recognized.insert(header_cmp);

    // The loop must have one dedicated exit edge from the header.  This keeps
    // the replacement independent of side exits and exceptional control flow.
    for (auto *block_const : loop.blocks) {
        auto *block = const_cast<oir::BasicBlock *>(block_const);
        for (auto *successor : block->successors()) {
            if (!contains_block(loop, successor) && (block != header || successor != exit)) {
                return std::nullopt;
            }
        }
    }

    auto *selected_result = dynamic_cast<oir::PhiInst *>(incoming_value_from(*result_phi, latch));
    if (selected_result == nullptr || selected_result->parent() != latch ||
        selected_result->incoming().size() != 2) {
        return std::nullopt;
    }
    oir::BinaryInst *result_add = nullptr;
    oir::BasicBlock *add_block = nullptr;
    oir::BasicBlock *no_add_block = nullptr;
    for (const auto &[incoming, predecessor] : selected_result->incoming()) {
        auto *binary = dynamic_cast<oir::BinaryInst *>(incoming);
        if (binary != nullptr &&
            matches_binary(binary, oir::Instruction::OpID::Add, result_phi, power_phi, true)) {
            result_add = binary;
            add_block = predecessor;
        } else if (incoming == result_phi) {
            no_add_block = predecessor;
        } else {
            return std::nullopt;
        }
    }
    if (result_add == nullptr || add_block == nullptr || no_add_block == nullptr ||
        add_block == no_add_block || result_add->parent() != add_block ||
        !is_unconditional_to(add_block, latch) || add_block->predecessors().size() != 1) {
        return std::nullopt;
    }
    auto *condition_block = add_block->predecessors().front();
    auto *condition_branch = dynamic_cast<oir::BranchInst *>(condition_block->terminator());
    if (condition_branch == nullptr || !condition_branch->is_conditional() ||
        condition_branch->true_bb() != add_block) {
        return std::nullopt;
    }
    // CFG cleanup normally folds the empty false arm into the condition block,
    // yielding `cond ? add_block : latch`.  Also accept the uncollapsed
    // diamond, but require the result phi's no-add predecessor to agree with
    // the exact false edge in either representation.
    if (condition_branch->false_bb() == latch) {
        if (no_add_block != condition_block) {
            return std::nullopt;
        }
    } else {
        if (condition_branch->false_bb() != no_add_block ||
            !is_unconditional_to(no_add_block, latch) || no_add_block->predecessors().size() != 1 ||
            no_add_block->predecessors().front() != condition_block) {
            return std::nullopt;
        }
    }
    recognized.insert(selected_result);
    recognized.insert(result_add);

    DigitBooleanKind kind;
    std::unordered_set<oir::BasicBlock *> expected_blocks = {header, condition_block, add_block,
                                                             no_add_block, latch};

    auto *condition_cmp = dynamic_cast<oir::CmpInst *>(condition_branch->cond());
    if (condition_cmp != nullptr && condition_cmp->op() == oir::Instruction::OpID::ICmp &&
        condition_cmp->pred() == oir::CmpPred::NE) {
        auto *left_rem = match_signed_rem_two(condition_cmp->lhs(), lhs_phi, rhs_phi, recognized);
        auto *right_rem = match_signed_rem_two(condition_cmp->rhs(), lhs_phi, rhs_phi, recognized);
        if (left_rem == nullptr || right_rem == nullptr || left_rem == right_rem ||
            condition_block != body) {
            return std::nullopt;
        }
        kind = DigitBooleanKind::XorSignedRemainders;
        recognized.insert(condition_cmp);
    } else {
        auto *logic_phi = dynamic_cast<oir::PhiInst *>(condition_branch->cond());
        if (logic_phi == nullptr || logic_phi->parent() != condition_block ||
            logic_phi->incoming().size() != 2) {
            return std::nullopt;
        }
        oir::Value *constant_value = nullptr;
        oir::Value *other_value = nullptr;
        oir::BasicBlock *constant_block = nullptr;
        oir::BasicBlock *other_block = nullptr;
        for (const auto &[incoming, predecessor] : logic_phi->incoming()) {
            if (is_int_value(incoming, 0) || is_int_value(incoming, 1)) {
                if (constant_value != nullptr) {
                    return std::nullopt;
                }
                constant_value = incoming;
                constant_block = predecessor;
            } else {
                other_value = incoming;
                other_block = predecessor;
            }
        }
        auto constant = int_constant(constant_value);
        auto second_parity = match_positive_odd(other_value, lhs_phi, rhs_phi);
        if (!constant.has_value() || (*constant != 0 && *constant != 1) ||
            second_parity == std::nullopt || constant_block == nullptr || other_block == nullptr ||
            constant_block == other_block || !is_unconditional_to(other_block, condition_block)) {
            return std::nullopt;
        }
        oir::BasicBlock *short_circuit_block = nullptr;
        oir::BranchInst *short_branch = nullptr;
        if (constant_block->terminator() != nullptr) {
            auto *candidate = dynamic_cast<oir::BranchInst *>(constant_block->terminator());
            if (candidate != nullptr && candidate->is_conditional()) {
                short_circuit_block = constant_block;
                short_branch = candidate;
            }
        }
        if (short_circuit_block == nullptr) {
            if (!is_unconditional_to(constant_block, condition_block) ||
                constant_block->predecessors().size() != 1 ||
                other_block->predecessors().size() != 1 ||
                constant_block->predecessors().front() != other_block->predecessors().front()) {
                return std::nullopt;
            }
            short_circuit_block = constant_block->predecessors().front();
            short_branch = dynamic_cast<oir::BranchInst *>(short_circuit_block->terminator());
        } else if (other_block->predecessors().size() != 1 ||
                   other_block->predecessors().front() != short_circuit_block) {
            return std::nullopt;
        }
        auto first_parity = short_branch == nullptr
                                ? std::nullopt
                                : match_positive_odd(short_branch->cond(), lhs_phi, rhs_phi);
        if (short_branch == nullptr || !short_branch->is_conditional() ||
            first_parity == std::nullopt || short_circuit_block != body ||
            first_parity->source == second_parity->source) {
            return std::nullopt;
        }
        if (*constant == 0) {
            auto *false_target =
                constant_block == short_circuit_block ? condition_block : constant_block;
            if (short_branch->true_bb() != other_block ||
                short_branch->false_bb() != false_target) {
                return std::nullopt;
            }
            kind = DigitBooleanKind::AndPositiveBits;
        } else {
            auto *true_target =
                constant_block == short_circuit_block ? condition_block : constant_block;
            if (short_branch->true_bb() != true_target || short_branch->false_bb() != other_block) {
                return std::nullopt;
            }
            kind = DigitBooleanKind::OrPositiveBits;
        }
        recognized.insert(logic_phi);
        recognized.insert(first_parity->instructions.begin(), first_parity->instructions.end());
        recognized.insert(second_parity->instructions.begin(), second_parity->instructions.end());
        expected_blocks.insert(short_circuit_block);
        expected_blocks.insert(constant_block);
        expected_blocks.insert(other_block);
    }

    if (expected_blocks.size() != loop.blocks.size()) {
        return std::nullopt;
    }
    for (auto *block_const : loop.blocks) {
        auto *block = const_cast<oir::BasicBlock *>(block_const);
        if (expected_blocks.find(block) == expected_blocks.end() || !block->has_terminator()) {
            return std::nullopt;
        }
        for (const auto &instruction : block->instructions()) {
            if (instruction->is_terminator()) {
                continue;
            }
            if (recognized.find(instruction.get()) == recognized.end()) {
                return std::nullopt;
            }
        }
    }

    BitDigitLoopMatch match;
    match.kind = kind;
    match.bit_width = bit_width;
    match.preheader = preheader;
    match.header = header;
    match.latch = latch;
    match.exit = exit;
    match.lhs_phi = lhs_phi;
    match.rhs_phi = rhs_phi;
    match.result_phi = result_phi;
    match.lhs_initial = lhs_initial;
    match.rhs_initial = rhs_initial;
    match.recognized = std::move(recognized);
    for (auto *block_const : loop.blocks) {
        match.loop_blocks.push_back(const_cast<oir::BasicBlock *>(block_const));
    }
    if (!users_stay_in_loop_except_result(loop, match)) {
        return std::nullopt;
    }
    return match;
}

oir::Instruction *insert_binary(oir::BasicBlock *block, oir::Instruction::OpID op, oir::Value *lhs,
                                oir::Value *rhs, const std::string &name) {
    return block->insert_before_terminator(
        std::make_unique<oir::BinaryInst>(lhs->type(), op, lhs, rhs, block, name));
}

oir::Instruction *insert_icmp(oir::Module &module, oir::BasicBlock *block, oir::CmpPred pred,
                              oir::Value *lhs, oir::Value *rhs, const std::string &name) {
    return block->insert_before_terminator(std::make_unique<oir::CmpInst>(
        module.types().int1_ty(), oir::Instruction::OpID::ICmp, pred, lhs, rhs, block, name));
}

oir::Instruction *insert_zext(oir::Module &module, oir::BasicBlock *block, oir::Value *source,
                              const std::string &name) {
    return block->insert_before_terminator(std::make_unique<oir::CastInst>(
        module.types().int32_ty(), oir::Instruction::OpID::ZExt, source, block, name));
}

oir::Value *insert_nonnegative_value(oir::Module &module, oir::BasicBlock *block, oir::Value *value,
                                     const std::string &prefix) {
    auto *nonnegative = insert_icmp(module, block, oir::CmpPred::GE, value, module.create_i32(0),
                                    prefix + ".nonnegative");
    auto *bit = insert_zext(module, block, nonnegative, prefix + ".nonnegative.bit");
    auto *mask = insert_binary(block, oir::Instruction::OpID::Sub, module.create_i32(0), bit,
                               prefix + ".nonnegative.mask");
    return insert_binary(block, oir::Instruction::OpID::And, value, mask,
                         prefix + ".nonnegative.value");
}

oir::Value *insert_bit_or(oir::BasicBlock *block, oir::Value *lhs, oir::Value *rhs,
                          const std::string &prefix) {
    auto *different =
        insert_binary(block, oir::Instruction::OpID::Xor, lhs, rhs, prefix + ".different");
    auto *both = insert_binary(block, oir::Instruction::OpID::And, lhs, rhs, prefix + ".both");
    return insert_binary(block, oir::Instruction::OpID::Xor, different, both, prefix + ".value");
}

struct MagnitudeValue {
    oir::Value *negative_bit = nullptr;
    oir::Value *magnitude = nullptr;
};

MagnitudeValue insert_magnitude(oir::Module &module, oir::BasicBlock *block, oir::Value *value,
                                const std::string &prefix) {
    auto *negative = insert_icmp(module, block, oir::CmpPred::LT, value, module.create_i32(0),
                                 prefix + ".negative");
    auto *negative_bit = insert_zext(module, block, negative, prefix + ".negative.bit");
    auto *mask = insert_binary(block, oir::Instruction::OpID::Sub, module.create_i32(0),
                               negative_bit, prefix + ".negative.mask");
    auto *flipped =
        insert_binary(block, oir::Instruction::OpID::Xor, value, mask, prefix + ".flipped");
    auto *magnitude =
        insert_binary(block, oir::Instruction::OpID::Sub, flipped, mask, prefix + ".magnitude");
    return {negative_bit, magnitude};
}

std::int64_t closed_instruction_count(DigitBooleanKind kind) {
    switch (kind) {
    case DigitBooleanKind::AndPositiveBits:
        return 9;
    case DigitBooleanKind::OrPositiveBits:
        return 11;
    case DigitBooleanKind::XorSignedRemainders:
        return 16;
    }
    return 16;
}

bool cost_model_allows(const BitDigitLoopMatch &match, Stats &stats) {
    if (match.bit_width <= 0) {
        return false;
    }
    OIRTransformCostEstimate estimate;
    estimate.kind = pass::cost_model::TransformKind::LoopIdiom;
    estimate.pass_name = "OIRBitDigitIdiom";
    estimate.candidate_id =
        match.kind == DigitBooleanKind::AndPositiveBits
            ? "signed-digit.and"
            : (match.kind == DigitBooleanKind::OrPositiveBits ? "signed-digit.or"
                                                              : "signed-digit.xor");
    estimate.scope = "loop";
    estimate.proof_kind = pass::cost_model::ProofKind::Structural;
    estimate.proof_status = pass::cost_model::ProofStatus::Proven;
    estimate.proof_rule_id =
        "oir.signed_digit_reconstruction.i" + std::to_string(match.bit_width) + ".full_width";
    estimate.proof_summary =
        "exact full-width (" + std::to_string(match.bit_width) +
        "-bit) signed-div/rem-by-2 recurrence; closed form preserves negative remainders and "
        "modulo-2^" +
        std::to_string(match.bit_width) + " power/result wrap";
    estimate.confidence = 0.99;
    estimate.frequency_scale = match.bit_width;
    estimate.frequency_source = pass::cost_model::FrequencySource::ConstantTripCount;
    estimate.has_detailed_instruction_mix = true;
    estimate.before_instrs = static_cast<std::int64_t>(match.recognized.size());
    estimate.after_instrs = closed_instruction_count(match.kind);
    const auto div_rem_per_iteration =
        std::count_if(match.recognized.begin(), match.recognized.end(), [](const auto *inst) {
            return inst->op() == oir::Instruction::OpID::SDiv ||
                   inst->op() == oir::Instruction::OpID::SRem;
        });
    estimate.before_int_div_rem =
        static_cast<std::int64_t>(div_rem_per_iteration) * match.bit_width;
    estimate.before_int_alu =
        (match.kind == DigitBooleanKind::XorSignedRemainders ? 7 : 8) * match.bit_width;
    estimate.after_int_alu = estimate.after_instrs;
    estimate.before_branches =
        (match.kind == DigitBooleanKind::XorSignedRemainders ? 3 : 4) * match.bit_width;
    estimate.after_branches = 0;
    estimate.risk.code_growth =
        std::max<std::int64_t>(0, estimate.after_instrs - estimate.before_instrs);
    estimate.risk.register_pressure_growth =
        match.kind == DigitBooleanKind::XorSignedRemainders ? 2 : 1;
    return cost_model_allows_transform(stats, estimate);
}

oir::Value *materialize_closed_form(oir::Module &module, const BitDigitLoopMatch &match) {
    auto *block = match.preheader;
    switch (match.kind) {
    case DigitBooleanKind::AndPositiveBits: {
        auto *lhs = insert_nonnegative_value(module, block, match.lhs_initial, "bitdigit.lhs");
        auto *rhs = insert_nonnegative_value(module, block, match.rhs_initial, "bitdigit.rhs");
        return insert_binary(block, oir::Instruction::OpID::And, lhs, rhs, "bitdigit.and");
    }
    case DigitBooleanKind::OrPositiveBits: {
        auto *lhs = insert_nonnegative_value(module, block, match.lhs_initial, "bitdigit.lhs");
        auto *rhs = insert_nonnegative_value(module, block, match.rhs_initial, "bitdigit.rhs");
        return insert_bit_or(block, lhs, rhs, "bitdigit.or");
    }
    case DigitBooleanKind::XorSignedRemainders: {
        auto lhs = insert_magnitude(module, block, match.lhs_initial, "bitdigit.lhs");
        auto rhs = insert_magnitude(module, block, match.rhs_initial, "bitdigit.rhs");
        auto *different = insert_binary(block, oir::Instruction::OpID::Xor, lhs.magnitude,
                                        rhs.magnitude, "bitdigit.xor.different");
        auto *both = insert_binary(block, oir::Instruction::OpID::And, lhs.magnitude, rhs.magnitude,
                                   "bitdigit.xor.both");
        auto *sign_different = insert_binary(block, oir::Instruction::OpID::Xor, lhs.negative_bit,
                                             rhs.negative_bit, "bitdigit.xor.sign.different");
        auto *sign_mask = insert_binary(block, oir::Instruction::OpID::Sub, module.create_i32(0),
                                        sign_different, "bitdigit.xor.sign.mask");
        auto *extra = insert_binary(block, oir::Instruction::OpID::And, both, sign_mask,
                                    "bitdigit.xor.extra");
        return insert_binary(block, oir::Instruction::OpID::Xor, different, extra, "bitdigit.xor");
    }
    }
    return nullptr;
}

bool rewrite_bit_digit_loop(oir::Module &module, const BitDigitLoopMatch &match, Stats &stats) {
    if (!cost_model_allows(match, stats)) {
        return false;
    }
    // The matched preheader has one unconditional edge to the header.  Commit
    // that guaranteed CFG edit first, so an unexpected stale candidate cannot
    // leave a partially materialized closed form behind.
    if (!oir::cfg::replace_successor(match.preheader, match.header, match.exit)) {
        return false;
    }
    auto *closed = materialize_closed_form(module, match);
    if (closed == nullptr) {
        // DigitBooleanKind is exhaustive; this is defensive against a future
        // enum extension that forgets to add a closed form.
        throw std::runtime_error("missing signed bit-digit closed form");
    }
    match.result_phi->replace_all_uses_with(closed);

    // The matcher proved that only result_phi escapes and replaced all its
    // uses.  Remove the now-unreachable pure loop immediately so inlining and
    // its size model see the closed form rather than dead CFG.
    for (auto *block : match.loop_blocks) {
        auto successors = block->successors();
        for (auto *successor : successors) {
            oir::cfg::remove_edge(block, successor);
        }
    }
    for (auto *block : match.loop_blocks) {
        oir::cfg::drop_all_references(*block);
    }
    auto *function = match.header->parent();
    for (auto *block : match.loop_blocks) {
        function->erase_block(block);
    }
    ++stats.folded;
    return true;
}

bool optimize_function(oir::Module &module, oir::Function &function, Stats &stats) {
    if (function.is_external() || function.entry_block() == nullptr) {
        return false;
    }
    bool changed = false;
    constexpr unsigned kMaxRewrites = 64;
    for (unsigned rewrite = 0; rewrite < kMaxRewrites; ++rewrite) {
        oir::DominatorTree dom_tree(function);
        oir::LoopInfo loop_info(function, dom_tree);
        bool iteration_changed = false;
        auto loops = loop_info.loops();
        std::sort(loops.begin(), loops.end(), [](const oir::Loop &lhs, const oir::Loop &rhs) {
            return lhs.blocks.size() < rhs.blocks.size();
        });
        for (const auto &loop : loops) {
            auto match = match_bit_digit_loop(loop);
            if (match.has_value() && rewrite_bit_digit_loop(module, *match, stats)) {
                iteration_changed = true;
                changed = true;
                break;
            }
        }
        if (!iteration_changed) {
            break;
        }
    }
    return changed;
}

} // namespace

bool fold_signed_bit_digit_reconstruction_loops(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        changed |= optimize_function(module, *function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt
