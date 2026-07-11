#include "oir/OIRScalarOpt.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace pass::oir_opt {
namespace {

using OpID = oir::Instruction::OpID;

bool is_reassociable(OpID op, oir::Type *type) {
    auto *integer = dynamic_cast<oir::IntegerType *>(type);
    if (integer == nullptr) {
        return false;
    }
    if (op == OpID::And || op == OpID::Xor) {
        return true;
    }
    return integer->bit_width() == 32 && (op == OpID::Add || op == OpID::Mul);
}

std::int64_t identity_for(OpID op, std::size_t bits) {
    switch (op) {
    case OpID::Mul:
        return 1;
    case OpID::And:
        return bits == 1 ? 1 : -1;
    case OpID::Add:
    case OpID::Xor:
    default:
        return 0;
    }
}

void collect_terms(oir::Value *value, OpID op, oir::Type *type, bool is_root,
                   std::vector<oir::Value *> &terms) {
    auto *binary = dynamic_cast<oir::BinaryInst *>(value);
    if (binary == nullptr || binary->op() != op || binary->type() != type ||
        (!is_root && binary->use_count() != 1)) {
        terms.push_back(value);
        return;
    }
    collect_terms(binary->lhs(), op, type, false, terms);
    collect_terms(binary->rhs(), op, type, false, terms);
}

std::string ordinal_key(std::size_t ordinal) {
    std::ostringstream out;
    out << std::setw(12) << std::setfill('0') << ordinal;
    return out.str();
}

std::string value_key(oir::Value *value,
                      const std::unordered_map<const oir::Value *, std::size_t> &ordinals) {
    if (auto *argument = dynamic_cast<oir::Argument *>(value)) {
        return "0:" + ordinal_key(argument->index()) + ":" + argument->name();
    }
    if (auto *global = dynamic_cast<oir::GlobalVariable *>(value)) {
        return "1:" + global->name();
    }
    auto found = ordinals.find(value);
    if (found != ordinals.end()) {
        return "2:" + ordinal_key(found->second) + ":" + value->name();
    }
    return "3:" + value->name();
}

bool matches_left_deep(oir::Value *value, OpID op, oir::Type *type,
                       const std::vector<oir::Value *> &terms, std::size_t count, bool is_root) {
    auto same_term = [](oir::Value *lhs, oir::Value *rhs) {
        return lhs == rhs || same_constant_value(lhs, rhs);
    };
    if (count == 1) {
        return same_term(value, terms.front());
    }
    auto *binary = dynamic_cast<oir::BinaryInst *>(value);
    if (binary == nullptr || binary->op() != op || binary->type() != type ||
        (!is_root && binary->use_count() != 1) || !same_term(binary->rhs(), terms[count - 1])) {
        return false;
    }
    return matches_left_deep(binary->lhs(), op, type, terms, count - 1, false);
}

std::vector<oir::Value *>
canonical_terms(oir::Module &module, oir::BinaryInst &root, std::vector<oir::Value *> terms,
                const std::unordered_map<const oir::Value *, std::size_t> &ordinals) {
    std::vector<oir::Value *> values;
    std::optional<std::int64_t> folded_constant;
    for (auto *term : terms) {
        auto constant = int_constant(term);
        if (!constant) {
            values.push_back(term);
            continue;
        }
        if (!folded_constant) {
            folded_constant = *constant;
            continue;
        }
        folded_constant = fold_int_binary(root.op(), *folded_constant, *constant);
    }

    std::stable_sort(values.begin(), values.end(), [&](oir::Value *lhs, oir::Value *rhs) {
        return value_key(lhs, ordinals) < value_key(rhs, ordinals);
    });

    if (root.op() == OpID::And) {
        values.erase(std::unique(values.begin(), values.end()), values.end());
    } else if (root.op() == OpID::Xor) {
        std::vector<oir::Value *> reduced;
        for (std::size_t i = 0; i < values.size();) {
            std::size_t end = i + 1;
            while (end < values.size() && values[end] == values[i]) {
                ++end;
            }
            if ((end - i) % 2 != 0) {
                reduced.push_back(values[i]);
            }
            i = end;
        }
        values = std::move(reduced);
    }

    auto *integer = static_cast<oir::IntegerType *>(root.type());
    const auto identity = identity_for(root.op(), integer->bit_width());
    if (folded_constant &&
        ((root.op() == OpID::Mul || root.op() == OpID::And) && *folded_constant == 0)) {
        return {make_int_constant(module, root.type(), 0)};
    }
    if (folded_constant && (*folded_constant != identity || values.empty())) {
        values.push_back(make_int_constant(module, root.type(), *folded_constant));
    }
    if (values.empty()) {
        values.push_back(make_int_constant(module, root.type(), identity));
    }
    return values;
}

bool reassociate_root(oir::Module &module, oir::BinaryInst &root,
                      const std::unordered_map<const oir::Value *, std::size_t> &ordinals) {
    if (!root.has_uses() || !is_reassociable(root.op(), root.type())) {
        return false;
    }

    std::vector<oir::Value *> flattened;
    collect_terms(&root, root.op(), root.type(), true, flattened);
    if (flattened.size() > 16) {
        return false;
    }
    auto planned = canonical_terms(module, root, std::move(flattened), ordinals);
    if (planned.size() == 1) {
        if (planned.front() == &root) {
            return false;
        }
        root.replace_all_uses_with(planned.front());
        return true;
    }
    if (matches_left_deep(&root, root.op(), root.type(), planned, planned.size(), true)) {
        return false;
    }

    auto *block = root.parent();
    if (block == nullptr) {
        return false;
    }
    auto before = std::find_if(block->instructions().begin(), block->instructions().end(),
                               [&](const auto &inst) { return inst.get() == &root; });
    if (before == block->instructions().end()) {
        return false;
    }

    oir::Value *accumulator = planned.front();
    for (std::size_t i = 1; i < planned.size(); ++i) {
        const bool is_last = i + 1 == planned.size();
        std::string name = is_last && !root.name().empty() ? root.name() + ".reassoc"
                                                           : "reassoc." + std::to_string(i);
        auto combined = std::make_unique<oir::BinaryInst>(root.type(), root.op(), accumulator,
                                                          planned[i], block, name);
        accumulator = combined.get();
        block->instructions().insert(before, std::move(combined));
    }
    root.replace_all_uses_with(accumulator);
    return true;
}

bool run_on_function(oir::Module &module, oir::Function &function, Stats &stats) {
    if (function.is_external()) {
        return false;
    }
    std::unordered_map<const oir::Value *, std::size_t> ordinals;
    std::vector<oir::BinaryInst *> candidates;
    std::size_t ordinal = 0;
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            ordinals[inst.get()] = ordinal++;
            if (auto *binary = dynamic_cast<oir::BinaryInst *>(inst.get())) {
                candidates.push_back(binary);
            }
        }
    }

    bool changed = false;
    for (auto *candidate : candidates) {
        if (reassociate_root(module, *candidate, ordinals)) {
            ++stats.reassociated;
            changed = true;
        }
    }
    return changed;
}

} // namespace

bool reassociate_integer_expressions(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        changed |= run_on_function(module, *function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt
