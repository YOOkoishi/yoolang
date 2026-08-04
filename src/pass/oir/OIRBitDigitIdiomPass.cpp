#include "oir/OIRScalarOpt.h"

#include "oir/OIRAnalysis.h"
#include "oir/OIRCFGUtils.h"
#include "pass/oir/OIRCostModel.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
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
// bit width.  c(k) is one of the three boolean forms matched below.  When both
// inputs are non-negative, signed division/remainder by two exposes ordinary
// 0/1 binary digits and the recurrence is exactly the corresponding direct
// AND/OR/XOR operation.
//
// Negative odd signed remainders are -1 rather than +1, so the direct bitwise
// form is not selected for those inputs.  The rewrite emits a runtime
// `x >= 0 && y >= 0` guard and keeps the original loop as the fallback whenever
// either input is negative.  This makes the fast-path proof independent of any
// closed-form derivation for negative values, including the signed minimum.

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
    // The guarded rewrite adds one fast predecessor and one two-input result
    // phi to this exit.  Reject a shared exit rather than synthesizing phi
    // values for unrelated predecessors.
    if (!has_no_exit_phis(exit) || !has_only_block(exit->predecessors(), header)) {
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

oir::Value *insert_bit_or(oir::BasicBlock *block, oir::Value *lhs, oir::Value *rhs,
                          const std::string &prefix) {
    auto *different =
        insert_binary(block, oir::Instruction::OpID::Xor, lhs, rhs, prefix + ".different");
    auto *both = insert_binary(block, oir::Instruction::OpID::And, lhs, rhs, prefix + ".both");
    return insert_binary(block, oir::Instruction::OpID::Xor, different, both, prefix + ".value");
}

std::int64_t fast_path_instruction_count(DigitBooleanKind kind) {
    switch (kind) {
    case DigitBooleanKind::AndPositiveBits:
        return 7;
    case DigitBooleanKind::OrPositiveBits:
        return 9;
    case DigitBooleanKind::XorSignedRemainders:
        return 7;
    }
    return 9;
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
    estimate.scope = "guarded_loop";
    estimate.proof_kind = pass::cost_model::ProofKind::Composite;
    estimate.proof_status = pass::cost_model::ProofStatus::Proven;
    estimate.proof_rule_id = "oir.signed_digit_reconstruction.i" + std::to_string(match.bit_width) +
                             ".guarded_nonnegative";
    estimate.proof_summary =
        "exact full-width (" + std::to_string(match.bit_width) +
        "-bit) signed-div/rem-by-2 recurrence; nonnegative runtime guard selects the direct "
        "bitwise form and either-negative inputs retain the original loop";
    estimate.proof_obligations = 3;
    estimate.confidence = 0.99;
    estimate.frequency_scale = match.bit_width;
    estimate.frequency_source = pass::cost_model::FrequencySource::ConstantTripCount;
    estimate.has_detailed_instruction_mix = true;
    estimate.before_instrs = static_cast<std::int64_t>(match.recognized.size());
    estimate.after_instrs = fast_path_instruction_count(match.kind);
    const auto div_rem_per_iteration =
        std::count_if(match.recognized.begin(), match.recognized.end(), [](const auto *inst) {
            return inst->op() == oir::Instruction::OpID::SDiv ||
                   inst->op() == oir::Instruction::OpID::SRem;
        });
    estimate.before_int_div_rem =
        static_cast<std::int64_t>(div_rem_per_iteration) * match.bit_width;
    estimate.before_int_alu =
        (match.kind == DigitBooleanKind::XorSignedRemainders ? 7 : 8) * match.bit_width;
    estimate.after_int_alu = match.kind == DigitBooleanKind::OrPositiveBits ? 6 : 4;
    estimate.before_branches =
        (match.kind == DigitBooleanKind::XorSignedRemainders ? 3 : 4) * match.bit_width;
    estimate.after_branches = 2;
    estimate.after_phis = 1;
    // The original loop remains as a cold fallback; the fast path and merge are
    // therefore real static growth even though they replace the 32-iteration
    // dynamic cost on every admitted execution.
    estimate.risk.code_growth = estimate.after_instrs;
    estimate.risk.register_pressure_growth = 1;
    estimate.risk.cleanup_dependency = 1;
    return cost_model_allows_transform(stats, estimate);
}

oir::Value *materialize_nonnegative_closed_form(oir::BasicBlock *block,
                                                const BitDigitLoopMatch &match) {
    switch (match.kind) {
    case DigitBooleanKind::AndPositiveBits:
        return insert_binary(block, oir::Instruction::OpID::And, match.lhs_initial,
                             match.rhs_initial, "bitdigit.and.fast");
    case DigitBooleanKind::OrPositiveBits:
        return insert_bit_or(block, match.lhs_initial, match.rhs_initial, "bitdigit.or.fast");
    case DigitBooleanKind::XorSignedRemainders:
        return insert_binary(block, oir::Instruction::OpID::Xor, match.lhs_initial,
                             match.rhs_initial, "bitdigit.xor.fast");
    }
    return nullptr;
}

bool is_matched_loop_block(const BitDigitLoopMatch &match, const oir::BasicBlock *block) {
    return std::find(match.loop_blocks.begin(), match.loop_blocks.end(), block) !=
           match.loop_blocks.end();
}

std::vector<oir::Value::Use> external_result_uses(const BitDigitLoopMatch &match) {
    std::vector<oir::Value::Use> uses;
    for (const auto &use : match.result_phi->uses()) {
        auto *instruction = dynamic_cast<oir::Instruction *>(use.user);
        if (instruction != nullptr && instruction->parent() != nullptr &&
            !is_matched_loop_block(match, instruction->parent())) {
            uses.push_back(use);
        }
    }
    return uses;
}

void replace_uses(const std::vector<oir::Value::Use> &uses, oir::Value *old_value,
                  oir::Value *new_value) {
    for (const auto &use : uses) {
        if (use.user != nullptr && use.operand_index < use.user->operand_count() &&
            use.user->operand(use.operand_index) == old_value) {
            use.user->set_operand(use.operand_index, new_value);
        }
    }
}

bool rewrite_bit_digit_loop(oir::Module &module, const BitDigitLoopMatch &match, Stats &stats) {
    if (!cost_model_allows(match, stats)) {
        return false;
    }
    auto *preheader_branch = dynamic_cast<oir::BranchInst *>(
        match.preheader == nullptr ? nullptr : match.preheader->terminator());
    auto *function = match.header == nullptr ? nullptr : match.header->parent();
    if (preheader_branch == nullptr || preheader_branch->is_conditional() ||
        preheader_branch->target_bb() != match.header || function == nullptr) {
        return false;
    }

    const auto escaping_uses = external_result_uses(match);
    auto *guard = function->create_block("bitdigit.guard");
    auto *fast = function->create_block("bitdigit.fast");
    auto *lhs_nonnegative = insert_icmp(module, guard, oir::CmpPred::GE, match.lhs_initial,
                                        module.create_i32(0), "bitdigit.lhs.nonnegative");
    auto *rhs_nonnegative = insert_icmp(module, guard, oir::CmpPred::GE, match.rhs_initial,
                                        module.create_i32(0), "bitdigit.rhs.nonnegative");
    auto *both_nonnegative = insert_binary(guard, oir::Instruction::OpID::And, lhs_nonnegative,
                                           rhs_nonnegative, "bitdigit.inputs.nonnegative");
    auto *closed = materialize_nonnegative_closed_form(fast, match);
    if (closed == nullptr) {
        function->erase_block(fast);
        function->erase_block(guard);
        return false;
    }

    // Commit the single fallible CFG edit before wiring either new block into
    // predecessor/successor lists.  On failure both blocks are still detached.
    if (!oir::cfg::replace_branch_target(*preheader_branch, match.header, guard)) {
        function->erase_block(fast);
        function->erase_block(guard);
        return false;
    }

    oir::cfg::remove_edge_no_phi_update(match.preheader, match.header);
    oir::cfg::add_edge(match.preheader, guard);
    oir::cfg::replace_phi_incoming_block(match.header, match.preheader, guard);
    oir::cfg::append_conditional_branch(module, guard, both_nonnegative, fast, match.header);
    oir::cfg::append_unconditional_branch(module, fast, match.exit);

    auto merged_owner =
        std::make_unique<oir::PhiInst>(match.result_phi->type(), match.exit, "bitdigit.result");
    auto *merged = merged_owner.get();
    merged->add_incoming(match.result_phi, match.header);
    merged->add_incoming(closed, fast);
    match.exit->instructions().push_front(std::move(merged_owner));
    merged->set_parent(match.exit);
    replace_uses(escaping_uses, match.result_phi, merged);

    ++stats.folded;
    stats.cfg += 2;
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
