#include "oir/OIRScalarOpt.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

IntRange range_for(const oir::Value *value, const RangeMap &ranges,
                   const RangeMap *fallback = nullptr) {
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
    if (found != ranges.end()) {
        return found->second;
    }
    if (fallback != nullptr) {
        found = fallback->find(value);
        if (found != fallback->end()) {
            return found->second;
        }
    }
    if (auto *integer = dynamic_cast<const oir::IntegerType *>(value->type());
        integer != nullptr && integer->bit_width() == 1) {
        return i1_range();
    }
    return full_range();
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

std::uint64_t abs_u64(std::int64_t value) {
    if (value >= 0) {
        return static_cast<std::uint64_t>(value);
    }
    return static_cast<std::uint64_t>(-(value + 1)) + 1;
}

bool is_power_of_two(std::uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

std::pair<std::int64_t, std::int64_t> integer_bounds(const oir::Type *type) {
    if (auto *integer = dynamic_cast<const oir::IntegerType *>(type)) {
        if (integer->bit_width() == 1) {
            return {0, 1};
        }
    }
    return {std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()};
}

bool is_full_range_for_value(const oir::Value *value, const IntRange &range) {
    if (!is_integer_value(value)) {
        return true;
    }
    auto [lower, upper] = integer_bounds(value->type());
    return range.lower == lower && range.upper == upper;
}

void store_narrow_range(RangeMap &ranges, const oir::Value *value, const IntRange &range) {
    if (!is_integer_value(value) || is_full_range_for_value(value, range)) {
        ranges.erase(value);
        return;
    }
    ranges[value] = range;
}

std::optional<IntRange> intersect_ranges(const IntRange &lhs, const IntRange &rhs) {
    return checked_range(std::max(lhs.lower, rhs.lower), std::min(lhs.upper, rhs.upper));
}

IntRange eval_binary(const oir::BinaryInst &inst, const RangeMap &ranges,
                     const RangeMap *fallback = nullptr) {
    auto lhs = range_for(inst.lhs(), ranges, fallback);
    auto rhs = range_for(inst.rhs(), ranges, fallback);

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
    case oir::Instruction::OpID::And: {
        auto mask = constant_int_value(inst.rhs());
        if (!mask) {
            mask = constant_int_value(inst.lhs());
        }
        if (mask && *mask >= 0) {
            return {0, *mask};
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
        auto magnitude = static_cast<std::int64_t>(abs_u64(*divisor));
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

std::optional<IntRange> max_phi_range(const oir::PhiInst &phi, const RangeMap &ranges,
                                      const RangeMap *fallback = nullptr) {
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

    auto lhs = range_for(cmp->lhs(), ranges, fallback);
    auto rhs = range_for(cmp->rhs(), ranges, fallback);
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

IntRange eval_phi(const oir::PhiInst &phi, const RangeMap &ranges,
                  const RangeMap *fallback = nullptr) {
    if (auto max_range = max_phi_range(phi, ranges, fallback)) {
        return *max_range;
    }

    bool first = true;
    IntRange out;
    for (const auto &[value, pred] : phi.incoming()) {
        (void)pred;
        auto incoming = range_for(value, ranges, fallback);
        out = first ? incoming : join_ranges(out, incoming);
        first = false;
    }
    return first ? full_range() : out;
}

IntRange eval_instruction(const oir::Instruction &inst, const RangeMap &ranges,
                          const RangeMap *fallback = nullptr) {
    if (!is_integer_value(&inst)) {
        return full_range();
    }
    if (inst.type()->is_integer() && dynamic_cast<const oir::IntegerType *>(inst.type())->bit_width() == 1) {
        return i1_range();
    }
    if (auto *binary = dynamic_cast<const oir::BinaryInst *>(&inst)) {
        return eval_binary(*binary, ranges, fallback);
    }
    if (auto *phi = dynamic_cast<const oir::PhiInst *>(&inst)) {
        return eval_phi(*phi, ranges, fallback);
    }
    return full_range();
}

std::optional<bool> eval_cmp(const oir::CmpInst &cmp, const RangeMap &ranges,
                             const RangeMap *fallback = nullptr) {
    if (cmp.op() != oir::Instruction::OpID::ICmp) {
        return std::nullopt;
    }

    auto lhs = range_for(cmp.lhs(), ranges, fallback);
    auto rhs = range_for(cmp.rhs(), ranges, fallback);
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

oir::CmpPred invert_predicate(oir::CmpPred pred) {
    switch (pred) {
    case oir::CmpPred::EQ:
        return oir::CmpPred::NE;
    case oir::CmpPred::NE:
        return oir::CmpPred::EQ;
    case oir::CmpPred::LT:
        return oir::CmpPred::GE;
    case oir::CmpPred::LE:
        return oir::CmpPred::GT;
    case oir::CmpPred::GT:
        return oir::CmpPred::LE;
    case oir::CmpPred::GE:
        return oir::CmpPred::LT;
    }
    return pred;
}

oir::CmpPred swap_predicate(oir::CmpPred pred) {
    switch (pred) {
    case oir::CmpPred::LT:
        return oir::CmpPred::GT;
    case oir::CmpPred::LE:
        return oir::CmpPred::GE;
    case oir::CmpPred::GT:
        return oir::CmpPred::LT;
    case oir::CmpPred::GE:
        return oir::CmpPred::LE;
    case oir::CmpPred::EQ:
    case oir::CmpPred::NE:
        return pred;
    }
    return pred;
}

void refine_value_range(RangeMap &ranges, const oir::Value *value, const IntRange &constraint,
                        const RangeMap *fallback = nullptr) {
    if (!is_integer_value(value)) {
        return;
    }
    auto current = range_for(value, ranges, fallback);
    if (auto refined = intersect_ranges(current, constraint)) {
        store_narrow_range(ranges, value, *refined);
    }
}

void apply_icmp_constraint(RangeMap &ranges, const oir::CmpInst &cmp, bool branch_taken,
                           const RangeMap *fallback = nullptr) {
    auto pred = branch_taken ? cmp.pred() : invert_predicate(cmp.pred());
    const oir::Value *value = cmp.lhs();
    auto constant = constant_int_value(cmp.rhs());
    if (!constant) {
        constant = constant_int_value(cmp.lhs());
        value = cmp.rhs();
        pred = swap_predicate(pred);
    }
    if (!constant || !is_integer_value(value)) {
        return;
    }

    auto [type_min, type_max] = integer_bounds(value->type());
    const auto c = *constant;
    std::optional<IntRange> constraint;
    switch (pred) {
    case oir::CmpPred::EQ:
        if (c >= type_min && c <= type_max) {
            constraint = IntRange{c, c};
        }
        break;
    case oir::CmpPred::LT:
        if (c > type_min) {
            constraint = IntRange{type_min, c - 1};
        }
        break;
    case oir::CmpPred::LE:
        if (c >= type_min) {
            constraint = IntRange{type_min, std::min(c, type_max)};
        }
        break;
    case oir::CmpPred::GT:
        if (c < type_max) {
            constraint = IntRange{c + 1, type_max};
        }
        break;
    case oir::CmpPred::GE:
        if (c <= type_max) {
            constraint = IntRange{std::max(c, type_min), type_max};
        }
        break;
    case oir::CmpPred::NE:
        break;
    }
    if (constraint) {
        refine_value_range(ranges, value, *constraint, fallback);
    }
}

void apply_branch_constraint(RangeMap &ranges, const oir::BranchInst &branch,
                             const oir::BasicBlock *succ,
                             const RangeMap *fallback = nullptr) {
    if (!branch.is_conditional()) {
        return;
    }
    auto *cmp = dynamic_cast<const oir::CmpInst *>(branch.cond());
    if (cmp == nullptr) {
        return;
    }
    if (succ == branch.true_bb()) {
        apply_icmp_constraint(ranges, *cmp, true, fallback);
    } else if (succ == branch.false_bb()) {
        apply_icmp_constraint(ranges, *cmp, false, fallback);
    }
}

using BlockRangeMap = std::unordered_map<const oir::BasicBlock *, RangeMap>;

void merge_successor_entry(const oir::BasicBlock *succ, const RangeMap &candidate,
                           BlockRangeMap &next_entries,
                           std::unordered_set<const oir::BasicBlock *> &seen) {
    auto [seen_it, first] = seen.insert(succ);
    (void)seen_it;
    auto &entry = next_entries[succ];
    if (first) {
        entry = candidate;
        return;
    }

    std::unordered_set<const oir::Value *> keys;
    for (const auto &[value, range] : entry) {
        (void)range;
        keys.insert(value);
    }
    for (const auto &[value, range] : candidate) {
        (void)range;
        keys.insert(value);
    }

    RangeMap merged;
    for (auto *value : keys) {
        auto joined = join_ranges(range_for(value, entry), range_for(value, candidate));
        store_narrow_range(merged, value, joined);
    }
    entry = std::move(merged);
}

BlockRangeMap compute_block_entry_ranges(const oir::Function &function,
                                         const RangeMap &global_ranges) {
    BlockRangeMap entries;
    for (const auto &block : function.blocks()) {
        entries[block.get()] = {};
    }

    constexpr unsigned kMaxIterations = 64;
    for (unsigned iteration = 0; iteration < kMaxIterations; ++iteration) {
        BlockRangeMap next_entries;
        std::unordered_set<const oir::BasicBlock *> seen;

        for (const auto &block_ptr : function.blocks()) {
            auto *block = block_ptr.get();
            RangeMap local;
            auto found_entry = entries.find(block);
            if (found_entry != entries.end()) {
                local = found_entry->second;
            }

            auto *term = block->terminator();
            for (auto *succ : block->successors()) {
                RangeMap candidate = local;
                if (auto *branch = dynamic_cast<const oir::BranchInst *>(term)) {
                    apply_branch_constraint(candidate, *branch, succ, &global_ranges);
                }
                merge_successor_entry(succ, candidate, next_entries, seen);
            }
        }

        bool changed = false;
        for (const auto &block : function.blocks()) {
            auto *raw = block.get();
            RangeMap next;
            auto found = next_entries.find(raw);
            if (found != next_entries.end()) {
                next = std::move(found->second);
            }
            if (entries[raw] != next) {
                entries[raw] = std::move(next);
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }

    return entries;
}

oir::Value *insert_nonnegative_pow2_rem_mask(
    oir::Module &module, oir::BasicBlock &block,
    std::list<std::unique_ptr<oir::Instruction>>::iterator before, oir::BinaryInst &inst,
    const RangeMap &ranges, const RangeMap *fallback = nullptr) {
    if (inst.op() != oir::Instruction::OpID::SRem || !inst.type()->is_integer()) {
        return nullptr;
    }

    auto divisor = constant_int_value(inst.rhs());
    if (!divisor || *divisor == 0) {
        return nullptr;
    }
    auto magnitude = abs_u64(*divisor);
    if (magnitude <= 1 || !is_power_of_two(magnitude)) {
        return nullptr;
    }

    auto lhs_range = range_for(inst.lhs(), ranges, fallback);
    if (lhs_range.lower < 0) {
        return nullptr;
    }

    const auto mask = static_cast<std::int64_t>(magnitude - 1);
    auto *mask_value = make_int_constant(module, inst.type(), mask);
    std::string name = inst.name().empty() ? "rem.mask" : inst.name() + ".mask";
    auto replacement = std::make_unique<oir::BinaryInst>(
        inst.type(), oir::Instruction::OpID::And, inst.lhs(), mask_value, &block, name);
    auto *raw = replacement.get();
    raw->set_parent(&block);
    block.instructions().insert(before, std::move(replacement));
    return raw;
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

    auto block_entry_ranges = compute_block_entry_ranges(function, ranges);

    ReplacementMap replacements;
    for (auto &block : function.blocks()) {
        RangeMap local;
        auto found_entry = block_entry_ranges.find(block.get());
        if (found_entry != block_entry_ranges.end()) {
            local = found_entry->second;
        }

        for (auto it = block->instructions().begin(); it != block->instructions().end(); ++it) {
            if (auto *cmp = dynamic_cast<oir::CmpInst *>(it->get())) {
                if (auto folded = eval_cmp(*cmp, local, &ranges)) {
                    replacements[cmp] = module.create_i1(*folded);
                }
            }

            if (auto *binary = dynamic_cast<oir::BinaryInst *>(it->get());
                binary != nullptr && binary->has_uses()) {
                if (auto *replacement =
                        insert_nonnegative_pow2_rem_mask(module, *block, it, *binary, local,
                                                         &ranges)) {
                    replacements[binary] = replacement;
                }
            }

            if (is_integer_value(it->get())) {
                auto range = eval_instruction(**it, local, &ranges);
                store_narrow_range(local, it->get(), range);
            }
        }
    }

    const auto replaced = apply_replacements(module, replacements);
    stats.folded += static_cast<unsigned>(replacements.size());
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
