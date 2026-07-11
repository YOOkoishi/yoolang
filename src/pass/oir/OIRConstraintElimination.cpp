#include "oir/OIRScalarOpt.h"

#include "oir/OIRAnalysis.h"

#include <algorithm>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pass::oir_opt {
namespace {

enum class RelationKind { EQ, NE, LT, LE };

struct Relation {
    oir::Value *lhs = nullptr;
    oir::Value *rhs = nullptr;
    RelationKind kind = RelationKind::EQ;
};

Relation normalized_relation(const oir::CmpInst &cmp, bool truth) {
    auto pred = cmp.pred();
    auto *lhs = cmp.lhs();
    auto *rhs = cmp.rhs();
    if (!truth) {
        switch (pred) {
        case oir::CmpPred::EQ:
            pred = oir::CmpPred::NE;
            break;
        case oir::CmpPred::NE:
            pred = oir::CmpPred::EQ;
            break;
        case oir::CmpPred::LT:
            pred = oir::CmpPred::GE;
            break;
        case oir::CmpPred::LE:
            pred = oir::CmpPred::GT;
            break;
        case oir::CmpPred::GT:
            pred = oir::CmpPred::LE;
            break;
        case oir::CmpPred::GE:
            pred = oir::CmpPred::LT;
            break;
        }
    }

    switch (pred) {
    case oir::CmpPred::EQ:
        return {lhs, rhs, RelationKind::EQ};
    case oir::CmpPred::NE:
        return {lhs, rhs, RelationKind::NE};
    case oir::CmpPred::LT:
        return {lhs, rhs, RelationKind::LT};
    case oir::CmpPred::LE:
        return {lhs, rhs, RelationKind::LE};
    case oir::CmpPred::GT:
        return {rhs, lhs, RelationKind::LT};
    case oir::CmpPred::GE:
        return {rhs, lhs, RelationKind::LE};
    }
    return {lhs, rhs, RelationKind::EQ};
}

class UnionFind final {
  public:
    void add(oir::Value *value) {
        parent_.try_emplace(value, value);
    }

    oir::Value *find(oir::Value *value) {
        add(value);
        auto *parent = parent_.at(value);
        if (parent != value) {
            parent_[value] = find(parent);
        }
        return parent_[value];
    }

    void unite(oir::Value *lhs, oir::Value *rhs) {
        lhs = find(lhs);
        rhs = find(rhs);
        if (lhs != rhs) {
            parent_[rhs] = lhs;
        }
    }

  private:
    std::unordered_map<oir::Value *, oir::Value *> parent_;
};

struct Edge {
    oir::Value *to = nullptr;
    bool strict = false;
};

struct PairHash {
    std::size_t operator()(const std::pair<oir::Value *, oir::Value *> &pair) const {
        auto lhs = std::hash<oir::Value *>{}(pair.first);
        auto rhs = std::hash<oir::Value *>{}(pair.second);
        return lhs ^ (rhs + 0x9e3779b9U + (lhs << 6U) + (lhs >> 2U));
    }
};

class ConstraintEnvironment final {
  public:
    explicit ConstraintEnvironment(std::vector<Relation> relations)
        : relations_(std::move(relations)) {
        for (const auto &relation : relations_) {
            equality_.add(relation.lhs);
            equality_.add(relation.rhs);
            values_.push_back(relation.lhs);
            values_.push_back(relation.rhs);
            if (relation.kind == RelationKind::EQ) {
                equality_.unite(relation.lhs, relation.rhs);
            }
        }
        unify_equal_constants();
        build_graph();
    }

    std::optional<bool> evaluate(const oir::CmpInst &cmp) {
        auto relation = normalized_relation(cmp, true);
        equality_.add(relation.lhs);
        equality_.add(relation.rhs);
        add_target_constant_relations(relation.lhs);
        add_target_constant_relations(relation.rhs);
        auto *lhs = canonical_value(relation.lhs);
        auto *rhs = canonical_value(relation.rhs);

        switch (relation.kind) {
        case RelationKind::EQ:
            if (lhs == rhs) {
                return true;
            }
            if (not_equal_.find(ordered_pair(lhs, rhs)) != not_equal_.end() ||
                reachable(lhs, rhs, true) || reachable(rhs, lhs, true)) {
                return false;
            }
            return std::nullopt;
        case RelationKind::NE:
            if (lhs == rhs) {
                return false;
            }
            if (not_equal_.find(ordered_pair(lhs, rhs)) != not_equal_.end() ||
                reachable(lhs, rhs, true) || reachable(rhs, lhs, true)) {
                return true;
            }
            return std::nullopt;
        case RelationKind::LT:
            if (lhs == rhs) {
                return false;
            }
            if (reachable(lhs, rhs, true)) {
                return true;
            }
            if (reachable(rhs, lhs, false)) {
                return false;
            }
            return std::nullopt;
        case RelationKind::LE:
            if (lhs == rhs || reachable(lhs, rhs, false)) {
                return true;
            }
            if (reachable(rhs, lhs, true)) {
                return false;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

  private:
    static std::pair<oir::Value *, oir::Value *> ordered_pair(oir::Value *lhs, oir::Value *rhs) {
        return std::less<oir::Value *>{}(lhs, rhs) ? std::make_pair(lhs, rhs)
                                                   : std::make_pair(rhs, lhs);
    }

    void unify_equal_constants() {
        for (std::size_t i = 0; i < values_.size(); ++i) {
            auto lhs = int_constant(values_[i]);
            if (!lhs) {
                continue;
            }
            for (std::size_t j = i + 1; j < values_.size(); ++j) {
                auto rhs = int_constant(values_[j]);
                if (rhs && values_[i]->type() == values_[j]->type() && *lhs == *rhs) {
                    equality_.unite(values_[i], values_[j]);
                }
            }
        }
    }

    void add_edge(oir::Value *from, oir::Value *to, bool strict) {
        from = equality_.find(from);
        to = equality_.find(to);
        if (from == to) {
            return;
        }
        auto &edges = graph_[from];
        auto found = std::find_if(edges.begin(), edges.end(),
                                  [&](const Edge &edge) { return edge.to == to; });
        if (found == edges.end()) {
            edges.push_back({to, strict});
        } else {
            found->strict = found->strict || strict;
        }
    }

    void build_graph() {
        for (const auto &relation : relations_) {
            auto *lhs = equality_.find(relation.lhs);
            auto *rhs = equality_.find(relation.rhs);
            switch (relation.kind) {
            case RelationKind::EQ:
                break;
            case RelationKind::NE:
                if (lhs != rhs) {
                    not_equal_.insert(ordered_pair(lhs, rhs));
                }
                break;
            case RelationKind::LT:
                add_edge(lhs, rhs, true);
                break;
            case RelationKind::LE:
                add_edge(lhs, rhs, false);
                break;
            }
        }
        for (std::size_t i = 0; i < values_.size(); ++i) {
            auto lhs = int_constant(values_[i]);
            if (!lhs) {
                continue;
            }
            for (std::size_t j = i + 1; j < values_.size(); ++j) {
                auto rhs = int_constant(values_[j]);
                if (!rhs || values_[i]->type() != values_[j]->type() || *lhs == *rhs) {
                    continue;
                }
                if (*lhs < *rhs) {
                    add_edge(values_[i], values_[j], true);
                } else {
                    add_edge(values_[j], values_[i], true);
                }
            }
        }
    }

    void add_target_constant_relations(oir::Value *value) {
        auto constant = int_constant(value);
        if (!constant) {
            return;
        }
        for (auto *known : values_) {
            auto other = int_constant(known);
            if (!other || known->type() != value->type()) {
                continue;
            }
            if (*constant < *other) {
                add_edge(value, known, true);
            } else if (*constant > *other) {
                add_edge(known, value, true);
            }
        }
    }

    oir::Value *canonical_value(oir::Value *value) {
        auto constant = int_constant(value);
        if (constant) {
            for (auto *known : values_) {
                auto other = int_constant(known);
                if (other && known->type() == value->type() && *other == *constant) {
                    return equality_.find(known);
                }
            }
        }
        return equality_.find(value);
    }

    bool reachable(oir::Value *from, oir::Value *to, bool require_strict) const {
        struct State {
            oir::Value *value = nullptr;
            bool strict = false;
        };
        struct StateHash {
            std::size_t operator()(const std::pair<oir::Value *, bool> &state) const {
                return std::hash<oir::Value *>{}(state.first) ^
                       (std::hash<bool>{}(state.second) << 1U);
            }
        };
        std::deque<State> worklist;
        std::unordered_set<std::pair<oir::Value *, bool>, StateHash> seen;
        worklist.push_back({from, false});
        seen.insert({from, false});
        while (!worklist.empty()) {
            auto state = worklist.front();
            worklist.pop_front();
            auto found = graph_.find(state.value);
            if (found == graph_.end()) {
                continue;
            }
            for (const auto &edge : found->second) {
                const bool strict = state.strict || edge.strict;
                if (edge.to == to && (!require_strict || strict)) {
                    return true;
                }
                if (seen.insert({edge.to, strict}).second) {
                    worklist.push_back({edge.to, strict});
                }
            }
        }
        return false;
    }

    std::vector<Relation> relations_;
    std::vector<oir::Value *> values_;
    UnionFind equality_;
    std::unordered_map<oir::Value *, std::vector<Edge>> graph_;
    std::unordered_set<std::pair<oir::Value *, oir::Value *>, PairHash> not_equal_;
};

std::vector<Relation> dominating_relations(const oir::Function &function,
                                           const oir::DominatorTree &dom_tree,
                                           const oir::BasicBlock *target) {
    std::vector<Relation> relations;
    for (const auto &block : function.blocks()) {
        if (!dom_tree.is_reachable(block.get())) {
            continue;
        }
        auto *branch = dynamic_cast<oir::BranchInst *>(block->terminator());
        if (branch == nullptr || !branch->is_conditional() ||
            branch->true_bb() == branch->false_bb()) {
            continue;
        }
        auto *cmp = dynamic_cast<oir::CmpInst *>(branch->cond());
        if (cmp == nullptr || cmp->op() != oir::Instruction::OpID::ICmp) {
            continue;
        }
        auto edge_dominates = [&](const oir::BasicBlock *successor) {
            const auto &predecessors = successor->predecessors();
            return block.get() != target && predecessors.size() == 1 &&
                   predecessors.front() == block.get() && dom_tree.dominates(successor, target);
        };
        const bool true_dominates = edge_dominates(branch->true_bb());
        const bool false_dominates = edge_dominates(branch->false_bb());
        if (true_dominates == false_dominates) {
            continue;
        }
        relations.push_back(normalized_relation(*cmp, true_dominates));
    }
    return relations;
}

bool run_on_function(oir::Module &module, oir::Function &function, Stats &stats) {
    if (function.is_external() || function.entry_block() == nullptr) {
        return false;
    }
    oir::DominatorTree dom_tree(function);
    bool changed = false;
    for (auto &block : function.blocks()) {
        if (!dom_tree.is_reachable(block.get())) {
            continue;
        }
        ConstraintEnvironment environment(dominating_relations(function, dom_tree, block.get()));
        for (auto &inst : block->instructions()) {
            auto *cmp = dynamic_cast<oir::CmpInst *>(inst.get());
            if (cmp == nullptr || cmp->op() != oir::Instruction::OpID::ICmp || !cmp->has_uses()) {
                continue;
            }
            if (auto folded = environment.evaluate(*cmp)) {
                cmp->replace_all_uses_with(module.create_i1(*folded));
                ++stats.constraints;
                changed = true;
            }
        }
    }
    return changed;
}

} // namespace

bool eliminate_redundant_constraints(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        changed |= run_on_function(module, *function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt
