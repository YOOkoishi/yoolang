#include "oir/OIRScalarOpt.h"

#include "oir/OIRCFGUtils.h"
#include "pass/oir/OIRCostModel.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pass::oir_opt {
namespace {

enum class ExitValueKind {
    State,
    Induction,
    Invariant,
};

struct ExitPhiMapping {
    oir::PhiInst *phi = nullptr;
    ExitValueKind kind = ExitValueKind::Invariant;
    oir::Value *invariant = nullptr;
};

struct AffineModRecurrenceMatch {
    oir::BasicBlock *preheader = nullptr;
    oir::BasicBlock *header = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::BranchInst *preheader_branch = nullptr;
    oir::PhiInst *induction_phi = nullptr;
    oir::PhiInst *state_phi = nullptr;
    oir::BinaryInst *induction_next = nullptr;
    oir::BinaryInst *state_add = nullptr;
    oir::BinaryInst *state_next = nullptr;
    oir::CmpInst *loop_cmp = nullptr;
    oir::Value *bound = nullptr;
    oir::Value *state_initial = nullptr;
    oir::Value *step = nullptr;
    oir::Value *modulus = nullptr;
    std::vector<ExitPhiMapping> exit_phis;
};

bool is_i32_value(const oir::Value *value) {
    if (value == nullptr) {
        return false;
    }
    auto *integer = dynamic_cast<oir::IntegerType *>(value->type());
    return integer != nullptr && integer->bit_width() == 32;
}

bool is_stable_runtime_value(const oir::Value *value) {
    // Undef is allowed to choose a different value at every use.  The guarded
    // closed form intentionally reuses both the initial state and trip bound,
    // so pointer identity alone is not an equality proof for undef.
    return value != nullptr && dynamic_cast<const oir::UndefValue *>(value) == nullptr;
}

bool has_exact_blocks(const std::vector<oir::BasicBlock *> &blocks, oir::BasicBlock *first,
                      oir::BasicBlock *second) {
    return blocks.size() == 2 && std::count(blocks.begin(), blocks.end(), first) == 1 &&
           std::count(blocks.begin(), blocks.end(), second) == 1;
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

bool same_value(oir::Value *lhs, oir::Value *rhs) {
    return lhs == rhs || same_constant_value(lhs, rhs);
}

oir::Value *positive_constant_addend(oir::BinaryInst *add, oir::Value *recurrence,
                                     std::int64_t &constant) {
    if (add == nullptr || add->op() != oir::Instruction::OpID::Add) {
        return nullptr;
    }
    oir::Value *candidate = nullptr;
    if (add->lhs() == recurrence) {
        candidate = add->rhs();
    } else if (add->rhs() == recurrence) {
        candidate = add->lhs();
    } else {
        return nullptr;
    }
    auto value = int_constant(candidate);
    if (!value.has_value() || *value <= 0) {
        return nullptr;
    }
    constant = *value;
    return candidate;
}

oir::BinaryInst *unit_increment_from(const oir::PhiInst &phi, oir::BasicBlock *header) {
    auto *back = incoming_value_from(phi, header);
    auto *add = dynamic_cast<oir::BinaryInst *>(back);
    if (add == nullptr || add->parent() != header || add->op() != oir::Instruction::OpID::Add) {
        return nullptr;
    }
    if ((add->lhs() == &phi && is_int_value(add->rhs(), 1)) ||
        (add->rhs() == &phi && is_int_value(add->lhs(), 1))) {
        return add;
    }
    return nullptr;
}

oir::Value *match_strict_upper_bound(const oir::CmpInst &cmp, oir::Value *next) {
    if (cmp.op() != oir::Instruction::OpID::ICmp) {
        return nullptr;
    }
    if (cmp.pred() == oir::CmpPred::LT && cmp.lhs() == next) {
        return cmp.rhs();
    }
    if (cmp.pred() == oir::CmpPred::GT && cmp.rhs() == next) {
        return cmp.lhs();
    }
    return nullptr;
}

bool matches_positive_entry_test(const oir::CmpInst &cmp, oir::Value *bound) {
    if (cmp.op() != oir::Instruction::OpID::ICmp) {
        return false;
    }
    return (cmp.pred() == oir::CmpPred::LT && is_int_value(cmp.lhs(), 0) && cmp.rhs() == bound) ||
           (cmp.pred() == oir::CmpPred::GT && cmp.lhs() == bound && is_int_value(cmp.rhs(), 0));
}

bool instruction_is_defined_in(oir::Value *value, oir::BasicBlock *block) {
    auto *instruction = dynamic_cast<oir::Instruction *>(value);
    return instruction != nullptr && instruction->parent() == block;
}

std::optional<AffineModRecurrenceMatch> match_affine_mod_recurrence(oir::BasicBlock &header) {
    auto *loop_branch = dynamic_cast<oir::BranchInst *>(header.terminator());
    if (loop_branch == nullptr || !loop_branch->is_conditional() ||
        loop_branch->true_bb() != &header || loop_branch->false_bb() == &header) {
        return std::nullopt;
    }
    auto *exit = loop_branch->false_bb();
    if (!has_exact_blocks(header.successors(), &header, exit) ||
        header.predecessors().size() != 2 ||
        std::count(header.predecessors().begin(), header.predecessors().end(), &header) != 1) {
        return std::nullopt;
    }

    oir::BasicBlock *preheader = nullptr;
    for (auto *predecessor : header.predecessors()) {
        if (predecessor != &header) {
            preheader = predecessor;
        }
    }
    if (preheader == nullptr || !has_exact_blocks(preheader->successors(), &header, exit) ||
        !has_exact_blocks(exit->predecessors(), preheader, &header)) {
        return std::nullopt;
    }

    auto *preheader_branch = dynamic_cast<oir::BranchInst *>(preheader->terminator());
    auto *entry_cmp = preheader_branch == nullptr
                          ? nullptr
                          : dynamic_cast<oir::CmpInst *>(preheader_branch->cond());
    if (preheader_branch == nullptr || !preheader_branch->is_conditional() ||
        preheader_branch->true_bb() != &header || preheader_branch->false_bb() != exit ||
        entry_cmp == nullptr || entry_cmp->parent() != preheader) {
        return std::nullopt;
    }

    std::vector<oir::PhiInst *> phis;
    bool saw_non_phi = false;
    for (const auto &instruction : header.instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(instruction.get());
        if (phi != nullptr) {
            if (saw_non_phi) {
                return std::nullopt;
            }
            phis.push_back(phi);
        } else {
            saw_non_phi = true;
        }
    }
    if (phis.size() != 2) {
        return std::nullopt;
    }

    oir::PhiInst *induction_phi = nullptr;
    oir::BinaryInst *induction_next = nullptr;
    for (auto *phi : phis) {
        if (!is_i32_value(phi) || phi->incoming().size() != 2 ||
            !is_int_value(incoming_value_from(*phi, preheader), 0)) {
            continue;
        }
        if (auto *next = unit_increment_from(*phi, &header)) {
            if (induction_phi != nullptr) {
                return std::nullopt;
            }
            induction_phi = phi;
            induction_next = next;
        }
    }
    if (induction_phi == nullptr || induction_next == nullptr) {
        return std::nullopt;
    }
    auto *state_phi = phis.front() == induction_phi ? phis.back() : phis.front();
    if (!is_i32_value(state_phi) || state_phi->incoming().size() != 2) {
        return std::nullopt;
    }
    auto *state_initial = incoming_value_from(*state_phi, preheader);
    auto *state_next_value = incoming_value_from(*state_phi, &header);
    auto *state_next = dynamic_cast<oir::BinaryInst *>(state_next_value);
    if (!is_i32_value(state_initial) || !is_stable_runtime_value(state_initial) ||
        instruction_is_defined_in(state_initial, &header) || state_next == nullptr ||
        state_next->parent() != &header || state_next->op() != oir::Instruction::OpID::SRem) {
        return std::nullopt;
    }
    auto *state_add = dynamic_cast<oir::BinaryInst *>(state_next->lhs());
    std::int64_t step_value = 0;
    auto *step = positive_constant_addend(state_add, state_phi, step_value);
    auto modulus_value = int_constant(state_next->rhs());
    if (state_add == nullptr || state_add->parent() != &header || step == nullptr ||
        !modulus_value.has_value() || *modulus_value <= 0) {
        return std::nullopt;
    }

    auto *loop_cmp = dynamic_cast<oir::CmpInst *>(loop_branch->cond());
    if (loop_cmp == nullptr || loop_cmp->parent() != &header) {
        return std::nullopt;
    }
    auto *bound = match_strict_upper_bound(*loop_cmp, induction_next);
    if (!is_i32_value(bound) || !is_stable_runtime_value(bound) ||
        instruction_is_defined_in(bound, &header) ||
        !matches_positive_entry_test(*entry_cmp, bound)) {
        return std::nullopt;
    }

    std::unordered_set<const oir::Instruction *> expected{
        induction_phi, state_phi, induction_next, state_add, state_next, loop_cmp, loop_branch};
    if (expected.size() != 7 || header.instructions().size() != expected.size()) {
        return std::nullopt;
    }
    for (const auto &instruction : header.instructions()) {
        if (expected.find(instruction.get()) == expected.end()) {
            return std::nullopt;
        }
    }

    std::vector<ExitPhiMapping> exit_phis;
    bool has_state_liveout = false;
    saw_non_phi = false;
    for (const auto &instruction : exit->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(instruction.get());
        if (phi == nullptr) {
            saw_non_phi = true;
            continue;
        }
        if (saw_non_phi || phi->incoming().size() != 2) {
            return std::nullopt;
        }
        auto *entry_value = incoming_value_from(*phi, preheader);
        auto *loop_value = incoming_value_from(*phi, &header);
        if (entry_value == nullptr || loop_value == nullptr) {
            return std::nullopt;
        }
        ExitPhiMapping mapping;
        mapping.phi = phi;
        if (same_value(entry_value, state_initial) && loop_value == state_next) {
            mapping.kind = ExitValueKind::State;
            has_state_liveout = true;
        } else if (is_int_value(entry_value, 0) && loop_value == induction_next) {
            mapping.kind = ExitValueKind::Induction;
        } else if (same_value(entry_value, loop_value)) {
            mapping.kind = ExitValueKind::Invariant;
            mapping.invariant = entry_value;
        } else {
            return std::nullopt;
        }
        exit_phis.push_back(mapping);
    }
    if (!has_state_liveout) {
        return std::nullopt;
    }

    std::unordered_set<const oir::PhiInst *> supported_exit_phis;
    for (const auto &mapping : exit_phis) {
        supported_exit_phis.insert(mapping.phi);
    }
    for (const auto &instruction : header.instructions()) {
        for (const auto &use : instruction->uses()) {
            auto *user = dynamic_cast<oir::Instruction *>(use.user);
            if (user == nullptr || user->parent() == &header) {
                continue;
            }
            auto *phi = dynamic_cast<oir::PhiInst *>(user);
            if (user->parent() != exit || phi == nullptr ||
                supported_exit_phis.find(phi) == supported_exit_phis.end()) {
                return std::nullopt;
            }
        }
    }

    AffineModRecurrenceMatch match;
    match.preheader = preheader;
    match.header = &header;
    match.exit = exit;
    match.preheader_branch = preheader_branch;
    match.induction_phi = induction_phi;
    match.state_phi = state_phi;
    match.induction_next = induction_next;
    match.state_add = state_add;
    match.state_next = state_next;
    match.loop_cmp = loop_cmp;
    match.bound = bound;
    match.state_initial = state_initial;
    match.step = step;
    match.modulus = state_next->rhs();
    match.exit_phis = std::move(exit_phis);
    return match;
}

bool cost_model_allows_recurrence(Stats &stats) {
    OIRTransformCostEstimate estimate;
    estimate.kind = pass::cost_model::TransformKind::LoopIdiom;
    estimate.pass_name = "OIRAffineModRecurrence";
    estimate.candidate_id =
        "affine-mod-recurrence." + std::to_string(++stats.cost_model_candidates);
    estimate.scope = "single_block_loop";
    estimate.proof_kind = pass::cost_model::ProofKind::Composite;
    estimate.proof_status = pass::cost_model::ProofStatus::Proven;
    estimate.proof_summary =
        "unit-trip structure plus nonnegative and no-signed-overflow runtime guards; "
        "unchanged loop fallback";
    estimate.proof_rule_id = "oir.i32.affine_mod.guarded_closure";
    estimate.proof_obligations = 4;
    estimate.confidence = 0.92;
    estimate.frequency_scale = 3;
    estimate.frequency_source = pass::cost_model::FrequencySource::ValueRangeTripCount;
    estimate.has_detailed_instruction_mix = true;

    // The runtime threshold admits the fast path only when at least three original
    // iterations would execute.  Model that boundary, where the transform is least
    // profitable, rather than assuming a benchmark-specific trip count.
    estimate.before_instrs = 15;
    estimate.before_int_alu = 9;
    estimate.before_int_div_rem = 3;
    estimate.before_branches = 3;
    estimate.before_phis = 6;
    estimate.after_instrs = 12;
    estimate.after_int_alu = 5;
    estimate.after_int_mul = 1;
    estimate.after_int_div_rem = 2;
    estimate.after_branches = 4;
    estimate.risk.code_growth = 12;
    estimate.risk.live_range_growth = 2;
    estimate.risk.register_pressure_growth = 2;
    estimate.risk.cleanup_dependency = 1;
    return cost_model_allows_transform(stats, estimate);
}

void add_fast_exit_incoming(const AffineModRecurrenceMatch &match, oir::BasicBlock *fast,
                            oir::Value *fast_state) {
    for (const auto &mapping : match.exit_phis) {
        switch (mapping.kind) {
        case ExitValueKind::State:
            mapping.phi->add_incoming(fast_state, fast);
            break;
        case ExitValueKind::Induction:
            mapping.phi->add_incoming(match.bound, fast);
            break;
        case ExitValueKind::Invariant:
            mapping.phi->add_incoming(mapping.invariant, fast);
            break;
        }
    }
}

bool rewrite_affine_mod_recurrence(oir::Function &function, const AffineModRecurrenceMatch &match,
                                   Stats &stats) {
    auto *module = function.parent();
    auto *trip_guard = function.create_block("affine.mod.guard.trip");
    auto *state_guard = function.create_block("affine.mod.guard.state");
    auto *range_guard = function.create_block("affine.mod.guard.range");
    auto *fallback = function.create_block("affine.mod.fallback");
    auto *fast = function.create_block("affine.mod.fast");

    // The match was taken from this exact terminator, so failure is defensive.  Do
    // it before adding any new CFG edges to keep that path transactionally clean.
    if (!oir::cfg::replace_branch_target(*match.preheader_branch, match.header, trip_guard)) {
        function.erase_block(fast);
        function.erase_block(fallback);
        function.erase_block(range_guard);
        function.erase_block(state_guard);
        function.erase_block(trip_guard);
        return false;
    }

    oir::IRBuilder builder(module);
    builder.set_insert_point(trip_guard);
    auto *enough_trips = builder.create_icmp(oir::CmpPred::GE, match.bound, module->create_i32(3),
                                             "affine.mod.enough.trips");
    builder.create_cond_br(enough_trips, state_guard, fallback);

    builder.set_insert_point(state_guard);
    auto *nonnegative = builder.create_icmp(oir::CmpPred::GE, match.state_initial,
                                            module->create_i32(0), "affine.mod.nonnegative");
    builder.create_cond_br(nonnegative, range_guard, fallback);

    builder.set_insert_point(range_guard);
    auto *headroom = builder.create_binary(
        oir::Instruction::OpID::Sub, module->create_i32(std::numeric_limits<std::int32_t>::max()),
        match.state_initial, "affine.mod.headroom");
    auto *safe_trip_limit = builder.create_binary(oir::Instruction::OpID::SDiv, headroom,
                                                  match.step, "affine.mod.safe.trip.limit");
    auto *fits = builder.create_icmp(oir::CmpPred::LE, match.bound, safe_trip_limit,
                                     "affine.mod.no.overflow");
    builder.create_cond_br(fits, fast, fallback);

    builder.set_insert_point(fallback);
    builder.create_br(match.header);

    builder.set_insert_point(fast);
    auto *scaled = builder.create_binary(oir::Instruction::OpID::Mul, match.bound, match.step,
                                         "affine.mod.scaled.step");
    auto *total = builder.create_binary(oir::Instruction::OpID::Add, match.state_initial, scaled,
                                        "affine.mod.total");
    auto *fast_state = builder.create_binary(oir::Instruction::OpID::SRem, total, match.modulus,
                                             "affine.mod.closed.form");
    builder.create_br(match.exit);

    oir::cfg::remove_edge_no_phi_update(match.preheader, match.header);
    oir::cfg::add_edge(match.preheader, trip_guard);
    oir::cfg::replace_phi_incoming_block(match.header, match.preheader, fallback);
    add_fast_exit_incoming(match, fast, fast_state);

    ++stats.folded;
    ++stats.cfg;
    return true;
}

} // namespace

bool fold_guarded_affine_mod_recurrences(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function_owner : module.functions()) {
        auto &function = *function_owner;
        if (function.is_external()) {
            continue;
        }
        for (auto block_it = function.blocks().begin(); block_it != function.blocks().end();
             ++block_it) {
            auto match = match_affine_mod_recurrence(**block_it);
            if (!match.has_value() ||
                !stats.affine_mod_recurrence_candidates.insert(match->header).second ||
                !cost_model_allows_recurrence(stats)) {
                continue;
            }
            changed |= rewrite_affine_mod_recurrence(function, *match, stats);
        }
    }
    return changed;
}

} // namespace pass::oir_opt
