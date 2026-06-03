#include "oir/OIRScalarOpt.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>

namespace pass::oir_opt {
namespace {

struct IntRange {
    std::int64_t lower = std::numeric_limits<std::int32_t>::min();
    std::int64_t upper = std::numeric_limits<std::int32_t>::max();

    bool operator==(const IntRange &other) const {
        return lower == other.lower && upper == other.upper;
    }

    bool operator!=(const IntRange &other) const {
        return !(*this == other);
    }
};

using RangeMap = std::unordered_map<const oir::Value *, IntRange>;

IntRange full_range() {
    return {};
}

IntRange i1_range() {
    return {0, 1};
}

IntRange constant_range(std::int64_t value) {
    return {value, value};
}

bool is_integer_value(const oir::Value *value) {
    return value != nullptr && value->type() != nullptr && value->type()->is_integer();
}

IntRange range_for(const oir::Value *value, const RangeMap &ranges) {
    if (!is_integer_value(value)) {
        return full_range();
    }
    if (auto *constant = dynamic_cast<const oir::ConstantInt *>(value)) {
        return constant_range(constant->value());
    }
    if (dynamic_cast<const oir::ConstantZero *>(value) != nullptr) {
        return constant_range(0);
    }
    auto found = ranges.find(value);
    return found == ranges.end() ? full_range() : found->second;
}

std::optional<IntRange> checked_range(std::int64_t lower, std::int64_t upper) {
    constexpr auto min = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
    constexpr auto max = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
    if (lower < min || upper > max || lower > upper) {
        return std::nullopt;
    }
    return IntRange{lower, upper};
}

IntRange join_ranges(const IntRange &lhs, const IntRange &rhs) {
    return {std::min(lhs.lower, rhs.lower), std::max(lhs.upper, rhs.upper)};
}

std::optional<std::int64_t> constant_int_value(const oir::Value *value) {
    if (auto *constant = dynamic_cast<const oir::ConstantInt *>(value)) {
        return constant->value();
    }
    if (dynamic_cast<const oir::ConstantZero *>(value) != nullptr) {
        return 0;
    }
    return std::nullopt;
}

IntRange eval_binary(const oir::BinaryInst &inst, const RangeMap &ranges) {
    auto lhs = range_for(inst.lhs(), ranges);
    auto rhs = range_for(inst.rhs(), ranges);

    switch (inst.op()) {
    case oir::Instruction::OpID::Add:
        if (auto out = checked_range(lhs.lower + rhs.lower, lhs.upper + rhs.upper)) {
            return *out;
        }
        return full_range();
    case oir::Instruction::OpID::Sub:
        if (auto out = checked_range(lhs.lower - rhs.upper, lhs.upper - rhs.lower)) {
            return *out;
        }
        return full_range();
    case oir::Instruction::OpID::Mul: {
        const std::int64_t values[] = {lhs.lower * rhs.lower, lhs.lower * rhs.upper,
                                       lhs.upper * rhs.lower, lhs.upper * rhs.upper};
        auto [min_it, max_it] = std::minmax_element(std::begin(values), std::end(values));
        if (auto out = checked_range(*min_it, *max_it)) {
            return *out;
        }
        return full_range();
    }
    case oir::Instruction::OpID::SDiv: {
        auto divisor = constant_int_value(inst.rhs());
        if (!divisor || *divisor == 0 ||
            (lhs.lower == std::numeric_limits<std::int32_t>::min() && *divisor == -1)) {
            return full_range();
        }
        auto a = lhs.lower / *divisor;
        auto b = lhs.upper / *divisor;
        return {std::min(a, b), std::max(a, b)};
    }
    case oir::Instruction::OpID::SRem: {
        auto divisor = constant_int_value(inst.rhs());
        if (!divisor || *divisor == 0) {
            return full_range();
        }
        auto magnitude = std::llabs(*divisor);
        if (lhs.lower >= 0) {
            return {0, magnitude - 1};
        }
        if (lhs.upper <= 0) {
            return {1 - magnitude, 0};
        }
        return {1 - magnitude, magnitude - 1};
    }
    default:
        return full_range();
    }
}

struct PhiEdge {
    const oir::BranchInst *branch = nullptr;
    bool true_edge = false;
};

std::optional<PhiEdge> incoming_edge(const oir::BasicBlock *pred,
                                     const oir::BasicBlock *phi_block) {
    if (pred == nullptr || phi_block == nullptr) {
        return std::nullopt;
    }

    if (auto *branch = dynamic_cast<const oir::BranchInst *>(pred->terminator())) {
        if (branch->is_conditional()) {
            if (branch->true_bb() == phi_block) {
                return PhiEdge{branch, true};
            }
            if (branch->false_bb() == phi_block) {
                return PhiEdge{branch, false};
            }
        }
    }

    if (pred->predecessors().size() != 1) {
        return std::nullopt;
    }
    auto *source = pred->predecessors().front();
    auto *branch = dynamic_cast<const oir::BranchInst *>(source->terminator());
    if (branch == nullptr || !branch->is_conditional()) {
        return std::nullopt;
    }
    if (branch->true_bb() == pred) {
        return PhiEdge{branch, true};
    }
    if (branch->false_bb() == pred) {
        return PhiEdge{branch, false};
    }
    return std::nullopt;
}

std::optional<IntRange> max_phi_range(const oir::PhiInst &phi, const RangeMap &ranges) {
    if (phi.incoming().size() != 2) {
        return std::nullopt;
    }

    auto first_edge = incoming_edge(phi.incoming()[0].second, phi.parent());
    auto second_edge = incoming_edge(phi.incoming()[1].second, phi.parent());
    if (!first_edge || !second_edge || first_edge->branch != second_edge->branch ||
        first_edge->true_edge == second_edge->true_edge) {
        return std::nullopt;
    }

    auto *cmp = dynamic_cast<const oir::CmpInst *>(first_edge->branch->cond());
    if (cmp == nullptr || cmp->op() != oir::Instruction::OpID::ICmp ||
        cmp->pred() != oir::CmpPred::LT) {
        return std::nullopt;
    }

    oir::Value *true_value =
        first_edge->true_edge ? phi.incoming()[0].first : phi.incoming()[1].first;
    oir::Value *false_value =
        first_edge->true_edge ? phi.incoming()[1].first : phi.incoming()[0].first;
    if (true_value != cmp->rhs() || false_value != cmp->lhs()) {
        return std::nullopt;
    }

    auto lhs = range_for(cmp->lhs(), ranges);
    auto rhs = range_for(cmp->rhs(), ranges);
    IntRange out{std::max(lhs.lower, rhs.lower), std::max(lhs.upper, rhs.upper)};

    auto *sub = dynamic_cast<const oir::BinaryInst *>(cmp->rhs());
    auto c = sub != nullptr && sub->op() == oir::Instruction::OpID::Sub
                 ? constant_int_value(sub->lhs())
                 : std::nullopt;
    if (c && sub->rhs() == cmp->lhs() && *c >= 0) {
        out.lower = std::max(out.lower, (*c + 1) / 2);
    }
    return out;
}

IntRange eval_phi(const oir::PhiInst &phi, const RangeMap &ranges) {
    if (auto max_range = max_phi_range(phi, ranges)) {
        return *max_range;
    }

    bool first = true;
    IntRange out;
    for (const auto &[value, pred] : phi.incoming()) {
        (void)pred;
        auto incoming = range_for(value, ranges);
        out = first ? incoming : join_ranges(out, incoming);
        first = false;
    }
    return first ? full_range() : out;
}

IntRange eval_instruction(const oir::Instruction &inst, const RangeMap &ranges) {
    if (!is_integer_value(&inst)) {
        return full_range();
    }
    if (inst.type()->is_integer() && dynamic_cast<const oir::IntegerType *>(inst.type())->bit_width() == 1) {
        return i1_range();
    }
    if (auto *binary = dynamic_cast<const oir::BinaryInst *>(&inst)) {
        return eval_binary(*binary, ranges);
    }
    if (auto *phi = dynamic_cast<const oir::PhiInst *>(&inst)) {
        return eval_phi(*phi, ranges);
    }
    return full_range();
}

std::optional<bool> eval_cmp(const oir::CmpInst &cmp, const RangeMap &ranges) {
    if (cmp.op() != oir::Instruction::OpID::ICmp) {
        return std::nullopt;
    }

    auto lhs = range_for(cmp.lhs(), ranges);
    auto rhs = range_for(cmp.rhs(), ranges);
    switch (cmp.pred()) {
    case oir::CmpPred::EQ:
        if (lhs.lower == lhs.upper && rhs.lower == rhs.upper && lhs.lower == rhs.lower) {
            return true;
        }
        if (lhs.upper < rhs.lower || rhs.upper < lhs.lower) {
            return false;
        }
        return std::nullopt;
    case oir::CmpPred::NE:
        if (lhs.lower == lhs.upper && rhs.lower == rhs.upper && lhs.lower == rhs.lower) {
            return false;
        }
        if (lhs.upper < rhs.lower || rhs.upper < lhs.lower) {
            return true;
        }
        return std::nullopt;
    case oir::CmpPred::LT:
        if (lhs.upper < rhs.lower) {
            return true;
        }
        if (lhs.lower >= rhs.upper) {
            return false;
        }
        return std::nullopt;
    case oir::CmpPred::LE:
        if (lhs.upper <= rhs.lower) {
            return true;
        }
        if (lhs.lower > rhs.upper) {
            return false;
        }
        return std::nullopt;
    case oir::CmpPred::GT:
        if (lhs.lower > rhs.upper) {
            return true;
        }
        if (lhs.upper <= rhs.lower) {
            return false;
        }
        return std::nullopt;
    case oir::CmpPred::GE:
        if (lhs.lower >= rhs.upper) {
            return true;
        }
        if (lhs.upper < rhs.lower) {
            return false;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool run_on_function(oir::Module &module, oir::Function &function, Stats &stats) {
    if (function.is_external()) {
        return false;
    }

    RangeMap ranges;
    bool changed = true;
    for (unsigned iteration = 0; changed && iteration < 32; ++iteration) {
        changed = false;
        for (const auto &block : function.blocks()) {
            for (const auto &inst : block->instructions()) {
                if (!is_integer_value(inst.get())) {
                    continue;
                }
                auto next = eval_instruction(*inst, ranges);
                auto found = ranges.find(inst.get());
                if (found == ranges.end() || found->second != next) {
                    ranges[inst.get()] = next;
                    changed = true;
                }
            }
        }
    }

    ReplacementMap replacements;
    for (const auto &block : function.blocks()) {
        for (const auto &inst : block->instructions()) {
            auto *cmp = dynamic_cast<oir::CmpInst *>(inst.get());
            if (cmp == nullptr) {
                continue;
            }
            if (auto folded = eval_cmp(*cmp, ranges)) {
                replacements[cmp] = module.create_i1(*folded);
            }
        }
    }

    const auto replaced = apply_replacements(module, replacements);
    stats.folded += replaced;
    return replaced != 0;
}

} // namespace

bool value_range_propagation(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        changed |= run_on_function(module, *function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt
