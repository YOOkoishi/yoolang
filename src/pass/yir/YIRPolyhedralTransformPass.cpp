#include "pass/yir/YIRPolyhedralTransformPass.h"

#include "pass/ast/ASTToYIRPass.h"
#include "pass/CostModel.h"
#include "pass/yir/YIRPolyhedralCanonicalizePass.h"
#include "pass/yir/YIRPolyhedralDependenceAnalysisPass.h"
#include "pass/yir/YIRPolyhedralModelBuildPass.h"
#include "yir/Presburger.h"
#include "yir/YIR.h"
#include "yir/YIRVerifier.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pass {

namespace {

struct NormalizedAddExpr {
    std::int64_t constant = 0;
    std::vector<std::pair<const yir::Value *, std::int64_t>> terms;

    bool operator==(const NormalizedAddExpr &other) const {
        return constant == other.constant && terms == other.terms;
    }
};

struct NormalizedAddExprHash {
    std::size_t operator()(const NormalizedAddExpr &expr) const {
        std::size_t hash = std::hash<std::int64_t>{}(expr.constant);
        for (const auto &[value, coefficient] : expr.terms) {
            hash ^= std::hash<const yir::Value *>{}(value) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= std::hash<std::int64_t>{}(coefficient) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

using AddAvailability = std::unordered_map<NormalizedAddExpr, yir::AddIOp *, NormalizedAddExprHash>;
using ActiveInductionVars = std::vector<const yir::Value *>;
using ValueMap = std::unordered_map<const yir::Value *, yir::Value *>;

const yir::ConstI32Op *const_i32_def(const yir::Value *value) {
    if (value == nullptr || value->defining_op() == nullptr) {
        return nullptr;
    }
    return dynamic_cast<const yir::ConstI32Op *>(value->defining_op());
}

bool const_i32_value(const yir::Value *value, std::int64_t &out) {
    auto *constant = const_i32_def(value);
    if (constant == nullptr) {
        return false;
    }
    out = constant->value();
    return true;
}

bool same_value_or_i32_constant(const yir::Value *lhs, const yir::Value *rhs) {
    if (lhs == rhs) {
        return true;
    }
    std::int64_t lhs_value = 0;
    std::int64_t rhs_value = 0;
    return const_i32_value(lhs, lhs_value) && const_i32_value(rhs, rhs_value) &&
           lhs_value == rhs_value;
}

void add_term(NormalizedAddExpr &expr, const yir::Value *value, std::int64_t coefficient) {
    if (coefficient == 0) {
        return;
    }
    for (auto &term : expr.terms) {
        if (term.first == value) {
            term.second += coefficient;
            return;
        }
    }
    expr.terms.push_back({value, coefficient});
}

void add_operand(NormalizedAddExpr &expr, const yir::Value *value) {
    std::int64_t constant = 0;
    if (const_i32_value(value, constant)) {
        expr.constant += constant;
        return;
    }
    add_term(expr, value, 1);
}

NormalizedAddExpr normalize_add(const yir::AddIOp &op) {
    NormalizedAddExpr expr;
    add_operand(expr, op.lhs());
    add_operand(expr, op.rhs());
    expr.terms.erase(std::remove_if(expr.terms.begin(), expr.terms.end(),
                                    [](const auto &term) { return term.second == 0; }),
                     expr.terms.end());
    std::sort(expr.terms.begin(), expr.terms.end(), [](const auto &lhs, const auto &rhs) {
        return std::less<const yir::Value *>{}(lhs.first, rhs.first);
    });
    return expr;
}


bool is_active_induction_var(const ActiveInductionVars &active_ivs, const yir::Value *value) {
    return std::find(active_ivs.begin(), active_ivs.end(), value) != active_ivs.end();
}

bool is_active_affine_expr(const NormalizedAddExpr &expr,
                           const ActiveInductionVars &active_ivs) {
    if (expr.terms.empty()) {
        return false;
    }
    return std::all_of(expr.terms.begin(), expr.terms.end(), [&active_ivs](const auto &term) {
        return is_active_induction_var(active_ivs, term.first);
    });
}

yir::Value *map_value(yir::Value *value, const ValueMap &map) {
    auto found = map.find(value);
    return found == map.end() ? value : found->second;
}

std::vector<yir::Value *> map_values(const std::vector<yir::Value *> &values,
                                     const ValueMap &map) {
    std::vector<yir::Value *> mapped;
    mapped.reserve(values.size());
    for (auto *value : values) {
        mapped.push_back(map_value(value, map));
    }
    return mapped;
}

std::unique_ptr<yir::Operation> clone_wave_unroll_op(const yir::Operation &op,
                                                     const ValueMap &map);

bool clone_wave_unroll_region_into(const yir::Region &source, yir::Region &dest,
                                   ValueMap map) {
    for (const auto &op : source.operations()) {
        auto clone = clone_wave_unroll_op(*op, map);
        if (clone == nullptr) {
            return false;
        }
        clone->set_parent(&dest);
        if (op->result() != nullptr && clone->result() != nullptr) {
            map[op->result()] = clone->result();
        }
        dest.operations().push_back(std::move(clone));
    }
    return true;
}

bool clone_wave_unroll_region_into(const yir::Region &source, yir::Region &dest,
                                   ValueMap map, ValueMap &out_map) {
    for (const auto &op : source.operations()) {
        auto clone = clone_wave_unroll_op(*op, map);
        if (clone == nullptr) {
            return false;
        }
        clone->set_parent(&dest);
        if (op->result() != nullptr && clone->result() != nullptr) {
            map[op->result()] = clone->result();
        }
        dest.operations().push_back(std::move(clone));
    }
    out_map = std::move(map);
    return true;
}

bool clone_affine_partition_region_into(
    const yir::Region &source, yir::Region &dest, ValueMap map,
    const yir::Value *source_iv, yir::Value *partition_quotient,
    const yir::Value *source_upper, yir::Value *upper_quotient,
    std::int64_t divisor) {
    for (const auto &op : source.operations()) {
        if (auto *div = dynamic_cast<const yir::DivSIOp *>(op.get())) {
            std::int64_t rhs = 0;
            if (div->lhs() == source_iv && const_i32_value(div->rhs(), rhs) &&
                rhs == divisor) {
                map[div->result()] = partition_quotient;
                continue;
            }
            if (div->lhs() == source_upper && const_i32_value(div->rhs(), rhs) &&
                rhs == divisor) {
                map[div->result()] = upper_quotient;
                continue;
            }
        }

        auto clone = clone_wave_unroll_op(*op, map);
        if (clone == nullptr) {
            return false;
        }
        clone->set_parent(&dest);
        if (op->result() != nullptr && clone->result() != nullptr) {
            map[op->result()] = clone->result();
        }
        dest.operations().push_back(std::move(clone));
    }
    return true;
}

std::unique_ptr<yir::Operation> clone_wave_unroll_op(const yir::Operation &op,
                                                     const ValueMap &map) {
    if (auto *constant = dynamic_cast<const yir::ConstI32Op *>(&op)) {
        return std::make_unique<yir::ConstI32Op>(constant->value(), op.result()->name());
    }
    if (auto *constant = dynamic_cast<const yir::ConstF32Op *>(&op)) {
        return std::make_unique<yir::ConstF32Op>(constant->value(), op.result()->name());
    }
    if (auto *constant = dynamic_cast<const yir::ConstBoolOp *>(&op)) {
        return std::make_unique<yir::ConstBoolOp>(constant->value(), op.result()->name());
    }
    if (dynamic_cast<const yir::ZeroOp *>(&op) != nullptr) {
        return std::make_unique<yir::ZeroOp>(op.result()->type(), op.result()->name());
    }
    if (auto *var = dynamic_cast<const yir::VarOp *>(&op)) {
        return std::make_unique<yir::VarOp>(
            op.result()->type(),
            var->has_initializer() ? map_value(var->initializer(), map) : nullptr,
            op.result()->name());
    }
    if (auto *assign = dynamic_cast<const yir::AssignOp *>(&op)) {
        return std::make_unique<yir::AssignOp>(map_value(assign->target(), map),
                                               map_value(assign->value(), map));
    }
    if (auto *load = dynamic_cast<const yir::ArrayLoadOp *>(&op)) {
        return std::make_unique<yir::ArrayLoadOp>(
            map_value(load->array(), map), map_values(load->indices(), map),
            op.result()->type(), op.result()->name());
    }
    if (auto *store = dynamic_cast<const yir::ArrayStoreOp *>(&op)) {
        return std::make_unique<yir::ArrayStoreOp>(
            map_value(store->value(), map), map_value(store->array(), map),
            map_values(store->indices(), map));
    }
    if (auto *binary = dynamic_cast<const yir::BinaryOpBase *>(&op)) {
        auto *lhs = map_value(binary->lhs(), map);
        auto *rhs = map_value(binary->rhs(), map);
        if (dynamic_cast<const yir::AddIOp *>(&op) != nullptr) {
            return std::make_unique<yir::AddIOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::SubIOp *>(&op) != nullptr) {
            return std::make_unique<yir::SubIOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::MulIOp *>(&op) != nullptr) {
            return std::make_unique<yir::MulIOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::DivSIOp *>(&op) != nullptr) {
            return std::make_unique<yir::DivSIOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::RemSIOp *>(&op) != nullptr) {
            return std::make_unique<yir::RemSIOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::AddFOp *>(&op) != nullptr) {
            return std::make_unique<yir::AddFOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::SubFOp *>(&op) != nullptr) {
            return std::make_unique<yir::SubFOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::MulFOp *>(&op) != nullptr) {
            return std::make_unique<yir::MulFOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::DivFOp *>(&op) != nullptr) {
            return std::make_unique<yir::DivFOp>(lhs, rhs, op.result()->name());
        }
        if (auto *icmp = dynamic_cast<const yir::ICmpOp *>(&op)) {
            return std::make_unique<yir::ICmpOp>(icmp->predicate(), lhs, rhs,
                                                 op.result()->name());
        }
        if (auto *fcmp = dynamic_cast<const yir::FCmpOp *>(&op)) {
            return std::make_unique<yir::FCmpOp>(fcmp->predicate(), lhs, rhs,
                                                 op.result()->name());
        }
    }
    if (dynamic_cast<const yir::ZExtI1ToI32Op *>(&op) != nullptr) {
        return std::make_unique<yir::ZExtI1ToI32Op>(map_value(op.operands()[0], map),
                                                    op.result()->name());
    }
    if (dynamic_cast<const yir::TruncI32ToI1Op *>(&op) != nullptr) {
        return std::make_unique<yir::TruncI32ToI1Op>(map_value(op.operands()[0], map),
                                                     op.result()->name());
    }
    if (dynamic_cast<const yir::SIToFPOp *>(&op) != nullptr) {
        return std::make_unique<yir::SIToFPOp>(map_value(op.operands()[0], map),
                                               op.result()->name());
    }
    if (dynamic_cast<const yir::FPToSIOp *>(&op) != nullptr) {
        return std::make_unique<yir::FPToSIOp>(map_value(op.operands()[0], map),
                                               op.result()->name());
    }
    if (dynamic_cast<const yir::ToBoolOp *>(&op) != nullptr) {
        return std::make_unique<yir::ToBoolOp>(map_value(op.operands()[0], map),
                                               op.result()->name());
    }
    if (dynamic_cast<const yir::NotOp *>(&op) != nullptr) {
        return std::make_unique<yir::NotOp>(map_value(op.operands()[0], map),
                                            op.result()->name());
    }
    if (auto *call = dynamic_cast<const yir::CallOp *>(&op)) {
        return std::make_unique<yir::CallOp>(
            call->callee(), map_values(call->args(), map),
            op.result() == nullptr ? yir::Type::get_void() : op.result()->type(),
            op.result() == nullptr ? "" : op.result()->name());
    }
    if (auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
        auto clone = std::make_unique<yir::IfOp>(map_value(if_op->condition(), map));
        if (!clone_wave_unroll_region_into(if_op->then_region(), clone->then_region(), map)) {
            return nullptr;
        }
        if (if_op->has_else()) {
            clone->set_has_else(true);
            if (!clone_wave_unroll_region_into(if_op->else_region(), clone->else_region(),
                                               map)) {
                return nullptr;
            }
        }
        return clone;
    }
    if (auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
        auto clone = std::make_unique<yir::ForOp>(
            map_value(for_op->induction_var(), map), map_value(for_op->lower_bound(), map),
            map_value(for_op->upper_bound(), map), map_value(for_op->step(), map));
        clone->set_parallel(for_op->is_parallel());
        if (!clone_wave_unroll_region_into(for_op->body_region(), clone->body_region(), map)) {
            return nullptr;
        }
        return clone;
    }
    return nullptr;
}

bool is_wave_unroll_safe(const yir::Region &region) {
    for (const auto &op : region.operations()) {
        if (dynamic_cast<const yir::WhileOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ForOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::BreakOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ContinueOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ReturnOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::CondOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ArrayInitOp *>(op.get()) != nullptr) {
            return false;
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            if (!is_wave_unroll_safe(if_op->then_region()) ||
                (if_op->has_else() && !is_wave_unroll_safe(if_op->else_region()))) {
                return false;
            }
        }
        if (clone_wave_unroll_op(*op, {}) == nullptr) {
            return false;
        }
    }
    return true;
}

bool is_parallel_wave_unroll_safe(const yir::Region &region) {
    for (const auto &op : region.operations()) {
        if (dynamic_cast<const yir::WhileOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::BreakOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ContinueOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ReturnOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::CondOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ArrayInitOp *>(op.get()) != nullptr) {
            return false;
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            if (!is_parallel_wave_unroll_safe(if_op->then_region()) ||
                (if_op->has_else() &&
                 !is_parallel_wave_unroll_safe(if_op->else_region()))) {
                return false;
            }
        }
        if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
            if (!is_parallel_wave_unroll_safe(for_op->body_region())) {
                return false;
            }
        }
        if (clone_wave_unroll_op(*op, {}) == nullptr) {
            return false;
        }
    }
    return true;
}

std::size_t wave_unroll_operation_count(const yir::Region &region) {
    std::size_t count = 0;
    for (const auto &op : region.operations()) {
        ++count;
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            count += wave_unroll_operation_count(if_op->then_region());
            if (if_op->has_else()) {
                count += wave_unroll_operation_count(if_op->else_region());
            }
        } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
            count += wave_unroll_operation_count(for_op->body_region());
        }
    }
    return count;
}

struct ReductionBodyMetrics {
    std::size_t total_loads = 0;
    std::size_t peak_region_loads = 0;
    std::size_t branches = 0;
    bool guarded = false;
};

ReductionBodyMetrics reduction_body_metrics(const yir::Region &region) {
    ReductionBodyMetrics metrics;
    std::size_t direct_loads = 0;
    for (const auto &op : region.operations()) {
        if (dynamic_cast<const yir::ArrayLoadOp *>(op.get()) != nullptr) {
            ++direct_loads;
            ++metrics.total_loads;
            continue;
        }
        auto *if_op = dynamic_cast<const yir::IfOp *>(op.get());
        if (if_op == nullptr) {
            continue;
        }

        metrics.guarded = true;
        ++metrics.branches;
        const auto merge_child = [&](const yir::Region &child) {
            const auto child_metrics = reduction_body_metrics(child);
            metrics.total_loads += child_metrics.total_loads;
            metrics.peak_region_loads =
                std::max(metrics.peak_region_loads, child_metrics.peak_region_loads);
            metrics.branches += child_metrics.branches;
            metrics.guarded = metrics.guarded || child_metrics.guarded;
        };
        merge_child(if_op->then_region());
        if (if_op->has_else()) {
            merge_child(if_op->else_region());
        }
    }
    metrics.peak_region_loads = std::max(metrics.peak_region_loads, direct_loads);
    return metrics;
}

bool clone_wave_unroll_prefix_into(const yir::Region &source, yir::Region &dest,
                                   ValueMap map, std::size_t count) {
    if (count > source.operations().size()) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto &op = source.operations()[i];
        auto clone = clone_wave_unroll_op(*op, map);
        if (clone == nullptr) {
            return false;
        }
        clone->set_parent(&dest);
        if (op->result() != nullptr && clone->result() != nullptr) {
            map[op->result()] = clone->result();
        }
        dest.operations().push_back(std::move(clone));
    }
    return true;
}

std::size_t symbol_index(std::vector<const yir::Value *> &symbols, const yir::Value *value) {
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (symbols[i] == value) {
            return i;
        }
    }
    symbols.push_back(value);
    return symbols.size() - 1;
}

bool presburger_proves_identical_add(const yir::AddIOp &previous, const yir::AddIOp &current) {
    auto lhs = normalize_add(previous);
    auto rhs = normalize_add(current);

    std::vector<const yir::Value *> symbols;
    for (const auto &term : lhs.terms) {
        symbol_index(symbols, term.first);
    }
    for (const auto &term : rhs.terms) {
        symbol_index(symbols, term.first);
    }

    std::vector<std::int64_t> diff(symbols.size(), 0);
    for (const auto &term : lhs.terms) {
        diff[symbol_index(symbols, term.first)] += term.second;
    }
    for (const auto &term : rhs.terms) {
        diff[symbol_index(symbols, term.first)] -= term.second;
    }
    const std::int64_t constant_diff = lhs.constant - rhs.constant;

    const bool identity = constant_diff == 0 &&
                          std::all_of(diff.begin(), diff.end(),
                                      [](std::int64_t coefficient) { return coefficient == 0; });
    if (!identity) {
        return false;
    }

    yir::presburger::IntegerRelation equality(static_cast<unsigned>(diff.size()));
    equality.add_equality(diff, constant_diff);

    // This is the first local use of the FPL-style API: for now the relation proves
    // an identical affine expression and the lex-max query selects the nearest previous
    // equivalent definition in lexical order. Full memory dependence replacement is
    // tracked in docs/polyhedral_transform_todo.md.
    return !equality.is_integer_empty() && equality.find_lexicographic_maximum().has_value();
}

bool poly_affine_expr_equal(const PolyAffineExpr &lhs, const PolyAffineExpr &rhs) {
    if (lhs.kind != rhs.kind || lhs.divisor != rhs.divisor || lhs.valid != rhs.valid ||
        lhs.constant != rhs.constant || lhs.terms != rhs.terms) {
        return false;
    }
    if (lhs.operand != nullptr || rhs.operand != nullptr) {
        if (lhs.operand == nullptr || rhs.operand == nullptr ||
            !poly_affine_expr_equal(*lhs.operand, *rhs.operand)) {
            return false;
        }
    }
    if (lhs.operands.size() != rhs.operands.size()) {
        return false;
    }
    return std::equal(lhs.operands.begin(), lhs.operands.end(), rhs.operands.begin(),
                      [](const auto &left, const auto &right) {
                          return left != nullptr && right != nullptr &&
                                 poly_affine_expr_equal(*left, *right);
                      });
}

std::size_t poly_affine_expr_hash(const PolyAffineExpr &expr) {
    std::size_t hash = std::hash<bool>{}(expr.valid);
    hash ^= std::hash<int>{}(static_cast<int>(expr.kind)) + 0x9e3779b9 + (hash << 6) +
            (hash >> 2);
    hash ^= std::hash<std::int64_t>{}(expr.divisor) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<std::int64_t>{}(expr.constant) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    for (const auto &[value, coefficient] : expr.terms) {
        hash ^= std::hash<const yir::Value *>{}(value) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= std::hash<std::int64_t>{}(coefficient) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    if (expr.operand != nullptr) {
        hash ^= poly_affine_expr_hash(*expr.operand) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    for (const auto &operand : expr.operands) {
        if (operand != nullptr) {
            hash ^= poly_affine_expr_hash(*operand) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
    }
    return hash;
}

struct PolyAccessKey {
    const yir::Value *memory = nullptr;
    std::vector<PolyAffineExpr> indices;

    bool operator==(const PolyAccessKey &other) const {
        return memory == other.memory && indices.size() == other.indices.size() &&
               std::equal(indices.begin(), indices.end(), other.indices.begin(), poly_affine_expr_equal);
    }
};

struct PolyAccessKeyHash {
    std::size_t operator()(const PolyAccessKey &key) const {
        std::size_t hash = std::hash<const yir::Value *>{}(key.memory);
        for (const auto &index : key.indices) {
            hash ^= poly_affine_expr_hash(index) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

struct NearestWrite {
    const PolyStmt *stmt = nullptr;
    std::size_t write_index = 0;
};

struct SameIterationReload {
    yir::ArrayLoadOp *load = nullptr;
    yir::ArrayStoreOp *store = nullptr;
};

struct StencilCarryCandidate {
    yir::ArrayLoadOp *load = nullptr;
    yir::ArrayStoreOp *store = nullptr;
    const PolyAccess *read = nullptr;
    yir::ForOp *loop = nullptr;
};

struct FutureNeighborConstant {
    yir::ArrayLoadOp *load = nullptr;
    int value = 1;
};

struct EnclosingLoopNest {
    std::vector<yir::ForOp *> outer_to_inner;
};

struct ConstantInitialization {
    const PolyStmt *stmt = nullptr;
    yir::ArrayStoreOp *store = nullptr;
    EnclosingLoopNest loops;
};

struct FutureNeighborProof {
    const PolyStmt *write_stmt = nullptr;
    yir::ArrayStoreOp *store = nullptr;
    EnclosingLoopNest update_loops;
};

struct InitializationReduction {
    ConstantInitialization init;
    std::vector<yir::ArrayLoadOp *> required_future_loads;
    FutureNeighborProof proof;
};

struct DeadMemoryWrite {
    yir::ArrayStoreOp *store = nullptr;
    const yir::Value *memory = nullptr;
};

struct SerialWavefrontCandidate {
    yir::ForOp *i_loop = nullptr;
    yir::ForOp *j_loop = nullptr;
    yir::ForOp *k_loop = nullptr;
    bool inner_wave_parallel = false;
};

struct PolyBandTileCandidate {
    yir::ForOp *outer_loop = nullptr;
    yir::ForOp *inner_loop = nullptr;
    const yir::Value *outer_iv = nullptr;
    const yir::Value *inner_iv = nullptr;
    std::size_t depth = 0;
    bool inner_only = false;
    // The surrounding band is imperfect or carries scalar state across the
    // inner loop. Only strip-mine the inner dimension so the original point
    // order and before/after statements remain unchanged.
    bool force_inner_only = false;
    bool relation_proven = false;
};

struct PolyLoopFusionCandidate {
    yir::Region *parent = nullptr;
    yir::ForOp *first_loop = nullptr;
    yir::ForOp *second_loop = nullptr;
    std::size_t first_index = 0;
    std::size_t second_index = 0;
    std::size_t first_scope_id = 0;
    std::size_t second_scope_id = 0;
};

struct PolyBandInterchangeCandidate {
    yir::ForOp *outer_loop = nullptr;
    yir::ForOp *inner_loop = nullptr;
    const yir::Value *outer_iv = nullptr;
    const yir::Value *inner_iv = nullptr;
    yir::AssignOp *redundant_inner_reset = nullptr;
    std::size_t depth = 0;
};

// A logical schedule candidate is expressed in the common SCoP iteration
// space.  The current codegen path only creates permutation candidates, but
// keeping the matrix here makes skew/unimodular candidates use the same
// legality and cost interfaces later.
struct PolyScheduleCandidate {
    std::vector<std::vector<std::int64_t>> band_matrix;
    bool valid = false;
};

bool is_valid_affine_access(const PolyAccess &access) {
    return access.memory != nullptr && std::all_of(access.indices.begin(), access.indices.end(),
                                                   [](const PolyAffineExpr &expr) {
                                                       return expr.is_linear();
                                                   });
}

PolyAccessKey access_key(const PolyAccess &access) {
    PolyAccessKey key;
    key.memory = access.memory;
    key.indices = access.indices;
    return key;
}

bool presburger_proves_identical_affine(const PolyAffineExpr &lhs, const PolyAffineExpr &rhs) {
    if (!lhs.is_linear() || !rhs.is_linear()) {
        return false;
    }

    std::vector<const yir::Value *> symbols;
    for (const auto &term : lhs.terms) {
        symbol_index(symbols, term.first);
    }
    for (const auto &term : rhs.terms) {
        symbol_index(symbols, term.first);
    }

    std::vector<std::int64_t> diff(symbols.size(), 0);
    for (const auto &term : lhs.terms) {
        diff[symbol_index(symbols, term.first)] += term.second;
    }
    for (const auto &term : rhs.terms) {
        diff[symbol_index(symbols, term.first)] -= term.second;
    }
    const std::int64_t constant_diff = lhs.constant - rhs.constant;

    const bool identity = constant_diff == 0 &&
                          std::all_of(diff.begin(), diff.end(),
                                      [](std::int64_t coefficient) { return coefficient == 0; });
    if (!identity) {
        return false;
    }

    yir::presburger::IntegerRelation equality(static_cast<unsigned>(diff.size()));
    equality.add_equality(diff, constant_diff);
    return !equality.is_integer_empty() && equality.find_lexicographic_maximum().has_value();
}

bool presburger_proves_same_access(const PolyAccess &write, const PolyAccess &read) {
    if (write.memory != read.memory || write.indices.size() != read.indices.size()) {
        return false;
    }
    for (std::size_t i = 0; i < write.indices.size(); ++i) {
        if (!presburger_proves_identical_affine(write.indices[i], read.indices[i])) {
            return false;
        }
    }
    return true;
}

bool same_schedule_dims(const PolyStmt &lhs, const PolyStmt &rhs) {
    return lhs.schedule_dims == rhs.schedule_dims;
}

bool schedule_band_is_identity(const PolyStmt &stmt, std::size_t depth,
                               const yir::Value *iv) {
    if (iv == nullptr || depth >= stmt.schedule.space_dims.size() ||
        stmt.schedule.space_dims[depth] != iv) {
        return false;
    }
    if (!stmt.schedule.matrix_valid || stmt.schedule.matrix.empty()) {
        return depth < stmt.schedule_dims.size() && stmt.schedule_dims[depth] == iv;
    }
    const std::size_t row = 1 + depth * 2;
    if (row >= stmt.schedule.matrix.size() ||
        row >= stmt.schedule.matrix_constants.size() ||
        stmt.schedule.matrix_constants[row] != 0 ||
        stmt.schedule.matrix[row].size() != stmt.schedule.space_dims.size()) {
        return false;
    }
    for (std::size_t col = 0; col < stmt.schedule.matrix[row].size(); ++col) {
        const std::int64_t expected = col == depth ? 1 : 0;
        if (stmt.schedule.matrix[row][col] != expected) {
            return false;
        }
    }
    return true;
}

bool same_iteration_domain(const PolyStmt &write_stmt, const PolyStmt &read_stmt) {
    if (!same_schedule_dims(write_stmt, read_stmt) ||
        write_stmt.loop_bounds.size() != read_stmt.loop_bounds.size()) {
        return false;
    }

    for (std::size_t i = 0; i < write_stmt.loop_bounds.size(); ++i) {
        const auto &write_bound = write_stmt.loop_bounds[i];
        const auto &read_bound = read_stmt.loop_bounds[i];
        if (write_bound.iv != read_bound.iv) {
            return false;
        }
        if (!presburger_proves_identical_affine(write_bound.lower, read_bound.lower) ||
            !presburger_proves_identical_affine(write_bound.upper, read_bound.upper)) {
            return false;
        }
    }
    return true;
}

PolyAffineExpr affine_expr_after_step(PolyAffineExpr expr, const yir::Value *iv,
                                      std::int64_t step) {
    if (!expr.valid) {
        return expr;
    }
    for (const auto &term : expr.terms) {
        if (term.first == iv) {
            expr.constant += term.second * step;
        }
    }
    return expr;
}

bool access_matches_next_iteration_write(const PolyAccess &write, const PolyAccess &read,
                                         const yir::Value *iv, std::int64_t step) {
    if (write.memory != read.memory || write.indices.size() != read.indices.size()) {
        return false;
    }
    for (std::size_t i = 0; i < write.indices.size(); ++i) {
        auto next_read = affine_expr_after_step(read.indices[i], iv, step);
        if (!presburger_proves_identical_affine(write.indices[i], next_read)) {
            return false;
        }
    }
    return true;
}

bool is_dim_plus_constant(const PolyAffineExpr &expr, const yir::Value *dim,
                          std::int64_t &constant) {
    if (!expr.valid) {
        return false;
    }

    bool saw_dim = false;
    for (const auto &[value, coefficient] : expr.terms) {
        if (value == dim) {
            if (coefficient != 1 || saw_dim) {
                return false;
            }
            saw_dim = true;
            continue;
        }
        if (coefficient != 0) {
            return false;
        }
    }

    if (!saw_dim) {
        return false;
    }
    constant = expr.constant;
    return true;
}

bool affine_terms_equal_with_constant_delta(const PolyAffineExpr &lhs,
                                            const PolyAffineExpr &rhs,
                                            std::int64_t lhs_minus_rhs) {
    return lhs.is_linear() && rhs.is_linear() && lhs.terms == rhs.terms &&
           lhs.constant - rhs.constant == lhs_minus_rhs;
}

bool affine_constant_value(const PolyAffineExpr &expr, std::int64_t &value) {
    if (!expr.is_linear()) {
        return false;
    }
    for (const auto &[_, coefficient] : expr.terms) {
        if (coefficient != 0) {
            return false;
        }
    }
    value = expr.constant;
    return true;
}

bool is_identity_access_for_dims(const PolyAccess &access,
                                 const std::vector<const yir::Value *> &dims) {
    if (access.indices.size() != dims.size() || !is_valid_affine_access(access)) {
        return false;
    }
    for (std::size_t dim = 0; dim < dims.size(); ++dim) {
        std::int64_t offset = 0;
        if (!is_dim_plus_constant(access.indices[dim], dims[dim], offset) || offset != 0) {
            return false;
        }
    }
    return true;
}

bool single_unit_future_neighbor_dim(const PolyAccess &read,
                                     const std::vector<const yir::Value *> &dims,
                                     std::size_t &future_dim) {
    if (read.indices.size() != dims.size() || !is_valid_affine_access(read)) {
        return false;
    }

    bool saw_positive_offset = false;
    for (std::size_t dim = 0; dim < dims.size(); ++dim) {
        std::int64_t offset = 0;
        if (!is_dim_plus_constant(read.indices[dim], dims[dim], offset)) {
            return false;
        }
        if (offset == 1) {
            if (saw_positive_offset) {
                return false;
            }
            saw_positive_offset = true;
            future_dim = dim;
            continue;
        }
        if (offset != 0) {
            return false;
        }
    }
    return saw_positive_offset;
}

bool is_single_unit_future_neighbor(const PolyAccess &read,
                                    const std::vector<const yir::Value *> &dims) {
    std::size_t future_dim = 0;
    return single_unit_future_neighbor_dim(read, dims, future_dim);
}

bool is_innermost_unit_distance(const std::vector<std::int64_t> &distance) {
    if (distance.empty() || distance.back() != 1) {
        return false;
    }
    return std::all_of(distance.begin(), std::prev(distance.end()), [](std::int64_t value) {
        return value == 0;
    });
}

const PolyStmt *find_stmt_by_id(const PolyScop &scop, std::size_t id) {
    for (const auto &stmt : scop.statements) {
        if (stmt.id == id) {
            return &stmt;
        }
    }
    return nullptr;
}

bool find_operation_index(yir::Region &region, const yir::Operation &target, std::size_t &index) {
    auto &ops = region.operations();
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].get() == &target) {
            index = i;
            return true;
        }
    }
    return false;
}

void erase_nearest_writes_for_memory(
    std::unordered_map<PolyAccessKey, NearestWrite, PolyAccessKeyHash> &nearest_writes,
    const yir::Value *memory) {
    for (auto it = nearest_writes.begin(); it != nearest_writes.end();) {
        if (it->first.memory == memory) {
            it = nearest_writes.erase(it);
        } else {
            ++it;
        }
    }
}

void replace_operands(yir::Operation &op, yir::Value *old_value, yir::Value *new_value) {
    for (auto *&operand : op.operands()) {
        if (operand == old_value) {
            operand = new_value;
        }
    }
}

void replace_value_in_region(yir::Region &region, yir::Value *old_value, yir::Value *new_value);

bool operation_uses_value(const yir::Operation &op, const yir::Value *value);
bool value_depends_on_value(const yir::Value *value, const yir::Value *needle);

bool value_depends_on_value(const yir::Value *value, const yir::Value *needle,
                            std::unordered_set<const yir::Value *> &visiting) {
    if (value == needle) {
        return true;
    }
    if (value == nullptr || !visiting.insert(value).second) {
        return false;
    }
    auto *def = value->defining_op();
    if (def == nullptr) {
        return false;
    }
    for (const auto *operand : def->operands()) {
        if (value_depends_on_value(operand, needle, visiting)) {
            return true;
        }
    }
    return false;
}

bool value_depends_on_value(const yir::Value *value, const yir::Value *needle) {
    std::unordered_set<const yir::Value *> visiting;
    return value_depends_on_value(value, needle, visiting);
}

bool region_uses_value(const yir::Region &region, const yir::Value *value) {
    return std::any_of(region.operations().begin(), region.operations().end(),
                       [value](const auto &op) { return operation_uses_value(*op, value); });
}

bool region_assigns_value(const yir::Region &region, const yir::Value *value) {
    for (const auto &op : region.operations()) {
        if (auto *assign = dynamic_cast<const yir::AssignOp *>(op.get())) {
            if (assign->target() == value) {
                return true;
            }
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            if (region_assigns_value(if_op->then_region(), value) ||
                (if_op->has_else() &&
                 region_assigns_value(if_op->else_region(), value))) {
                return true;
            }
        }
    }
    return false;
}

bool module_uses_value(const yir::Module &module, const yir::Value *value) {
    for (const auto &function : module.functions()) {
        if (region_uses_value(function->body(), value)) {
            return true;
        }
    }
    return false;
}

bool operation_uses_value(const yir::Operation &op, const yir::Value *value) {
    if (std::find(op.operands().begin(), op.operands().end(), value) != op.operands().end()) {
        return true;
    }
    if (auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
        return region_uses_value(if_op->then_region(), value) ||
               (if_op->has_else() && region_uses_value(if_op->else_region(), value));
    }
    if (auto *while_op = dynamic_cast<const yir::WhileOp *>(&op)) {
        return region_uses_value(while_op->cond_region(), value) ||
               region_uses_value(while_op->body_region(), value);
    }
    if (auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
        return region_uses_value(for_op->body_region(), value);
    }
    return false;
}

bool value_used_outside_range(const yir::Region &region, const yir::Value *value,
                              std::size_t first_in_range, std::size_t last_in_range) {
    const auto &ops = region.operations();
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (i >= first_in_range && i <= last_in_range) {
            continue;
        }
        if (operation_uses_value(*ops[i], value)) {
            return true;
        }
    }
    return false;
}

bool operation_writes_memory(const yir::Operation &op, const yir::Value *memory);

bool region_writes_memory(const yir::Region &region, const yir::Value *memory) {
    return std::any_of(region.operations().begin(), region.operations().end(),
                       [memory](const auto &op) { return operation_writes_memory(*op, memory); });
}

bool operation_writes_memory(const yir::Operation &op, const yir::Value *memory) {
    if (auto *store = dynamic_cast<const yir::ArrayStoreOp *>(&op)) {
        return store->array() == memory;
    }
    if (auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
        return region_writes_memory(if_op->then_region(), memory) ||
               (if_op->has_else() && region_writes_memory(if_op->else_region(), memory));
    }
    if (auto *while_op = dynamic_cast<const yir::WhileOp *>(&op)) {
        return region_writes_memory(while_op->cond_region(), memory) ||
               region_writes_memory(while_op->body_region(), memory);
    }
    if (auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
        return region_writes_memory(for_op->body_region(), memory);
    }
    return false;
}

bool operation_contains_call(const yir::Operation &op);

bool operation_is_nested_in_region(const yir::Region &region,
                                   const yir::Operation *target) {
    for (const auto &op : region.operations()) {
        if (op.get() == target) {
            return true;
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            if (operation_is_nested_in_region(if_op->then_region(), target) ||
                (if_op->has_else() && operation_is_nested_in_region(if_op->else_region(), target))) {
                return true;
            }
        } else if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
            if (operation_is_nested_in_region(while_op->cond_region(), target) ||
                operation_is_nested_in_region(while_op->body_region(), target)) {
                return true;
            }
        } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
            if (operation_is_nested_in_region(for_op->body_region(), target)) {
                return true;
            }
        }
    }
    return false;
}

bool value_defined_inside_region(const yir::Value *value, const yir::Region &region) {
    return value != nullptr && value->defining_op() != nullptr &&
           operation_is_nested_in_region(region, value->defining_op());
}

bool region_contains_call(const yir::Region &region) {
    return std::any_of(region.operations().begin(), region.operations().end(),
                       [](const auto &op) { return operation_contains_call(*op); });
}

bool operation_contains_call(const yir::Operation &op) {
    if (dynamic_cast<const yir::CallOp *>(&op) != nullptr) {
        return true;
    }
    if (auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
        return region_contains_call(if_op->then_region()) ||
               (if_op->has_else() && region_contains_call(if_op->else_region()));
    }
    if (auto *while_op = dynamic_cast<const yir::WhileOp *>(&op)) {
        return region_contains_call(while_op->cond_region()) ||
               region_contains_call(while_op->body_region());
    }
    if (auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
        return region_contains_call(for_op->body_region());
    }
    return false;
}

bool has_intervening_write_to_memory(const yir::Region &region, const yir::Value *memory,
                                     std::size_t first, std::size_t last) {
    const auto &ops = region.operations();
    if (first > last || first >= ops.size()) {
        return false;
    }
    last = std::min(last, ops.size() - 1);
    for (std::size_t i = first; i <= last; ++i) {
        if (operation_writes_memory(*ops[i], memory)) {
            return true;
        }
    }
    return false;
}

bool value_list_contains(const std::vector<yir::Value *> &values, const yir::Value *target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

bool array_init_entries_use_memory(const yir::ArrayInitOp &init, const yir::Value *memory) {
    return std::any_of(init.entries().begin(), init.entries().end(),
                       [memory](const yir::ArrayInitEntry &entry) {
                           return entry.value == memory;
                       });
}

bool region_observes_memory_except_writes(const yir::Region &region, const yir::Value *memory);

bool operation_observes_memory_except_writes(const yir::Operation &op,
                                             const yir::Value *memory) {
    if (auto *load = dynamic_cast<const yir::ArrayLoadOp *>(&op)) {
        return load->array() == memory || value_list_contains(load->indices(), memory);
    }

    if (auto *store = dynamic_cast<const yir::ArrayStoreOp *>(&op)) {
        return store->value() == memory || value_list_contains(store->indices(), memory);
    }

    if (auto *init = dynamic_cast<const yir::ArrayInitOp *>(&op)) {
        return (init->array() != memory && value_list_contains(init->operands(), memory)) ||
               array_init_entries_use_memory(*init, memory);
    }

    if (auto *elem_addr = dynamic_cast<const yir::ElemAddrOp *>(&op)) {
        return elem_addr->base() == memory || value_list_contains(elem_addr->indices(), memory);
    }

    if (auto *decay = dynamic_cast<const yir::DecayOp *>(&op)) {
        return decay->array_address() == memory;
    }

    if (value_list_contains(op.operands(), memory)) {
        return true;
    }

    if (auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
        return region_observes_memory_except_writes(if_op->then_region(), memory) ||
               (if_op->has_else() &&
                region_observes_memory_except_writes(if_op->else_region(), memory));
    }

    if (auto *while_op = dynamic_cast<const yir::WhileOp *>(&op)) {
        return region_observes_memory_except_writes(while_op->cond_region(), memory) ||
               region_observes_memory_except_writes(while_op->body_region(), memory);
    }

    if (auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
        return region_observes_memory_except_writes(for_op->body_region(), memory);
    }

    return false;
}

bool region_observes_memory_except_writes(const yir::Region &region, const yir::Value *memory) {
    return std::any_of(region.operations().begin(), region.operations().end(),
                       [memory](const auto &op) {
                           return operation_observes_memory_except_writes(*op, memory);
                       });
}

bool module_observes_memory_except_writes(const yir::Module &module, const yir::Value *memory) {
    for (const auto &function : module.functions()) {
        if (region_observes_memory_except_writes(function->body(), memory)) {
            return true;
        }
    }
    return false;
}

bool is_zero_value(const yir::Value *value) {
    std::int64_t constant = 0;
    if (const_i32_value(value, constant)) {
        return constant == 0;
    }
    return value != nullptr &&
           dynamic_cast<const yir::ZeroOp *>(value->defining_op()) != nullptr;
}

bool is_i32_type(const yir::TypePtr &type) {
    return type != nullptr && type->kind() == yir::Type::Kind::I32;
}

PolyAffineExpr affine_expr_from_value(const yir::Value *value);

PolyAffineExpr invalid_poly_affine_expr() {
    PolyAffineExpr expr;
    expr.valid = false;
    return expr;
}

void add_affine_term(PolyAffineExpr &expr, const yir::Value *value, std::int64_t coefficient) {
    if (value == nullptr || coefficient == 0) {
        return;
    }
    for (auto &term : expr.terms) {
        if (term.first == value) {
            term.second += coefficient;
            return;
        }
    }
    expr.terms.push_back({value, coefficient});
}

void normalize_affine_terms(PolyAffineExpr &expr) {
    expr.terms.erase(std::remove_if(expr.terms.begin(), expr.terms.end(),
                                    [](const auto &term) { return term.second == 0; }),
                     expr.terms.end());
    std::sort(expr.terms.begin(), expr.terms.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.first->name() != rhs.first->name()) {
            return lhs.first->name() < rhs.first->name();
        }
        return std::less<const yir::Value *>{}(lhs.first, rhs.first);
    });
}

PolyAffineExpr add_affine_expr(PolyAffineExpr lhs, const PolyAffineExpr &rhs,
                               std::int64_t sign = 1) {
    if (!lhs.valid || !rhs.valid) {
        return invalid_poly_affine_expr();
    }
    lhs.constant += sign * rhs.constant;
    for (const auto &term : rhs.terms) {
        add_affine_term(lhs, term.first, sign * term.second);
    }
    normalize_affine_terms(lhs);
    return lhs;
}

PolyAffineExpr scale_affine_expr(PolyAffineExpr expr, std::int64_t scale) {
    if (!expr.valid) {
        return expr;
    }
    expr.constant *= scale;
    for (auto &term : expr.terms) {
        term.second *= scale;
    }
    normalize_affine_terms(expr);
    return expr;
}

PolyAffineExpr affine_expr_from_value(const yir::Value *value) {
    if (value == nullptr || !is_i32_type(value->type())) {
        return invalid_poly_affine_expr();
    }

    std::int64_t constant = 0;
    if (const_i32_value(value, constant)) {
        PolyAffineExpr expr;
        expr.constant = constant;
        return expr;
    }

    auto *def = value->defining_op();
    if (auto *add = dynamic_cast<const yir::AddIOp *>(def)) {
        return add_affine_expr(affine_expr_from_value(add->lhs()),
                               affine_expr_from_value(add->rhs()));
    }
    if (auto *sub = dynamic_cast<const yir::SubIOp *>(def)) {
        return add_affine_expr(affine_expr_from_value(sub->lhs()),
                               affine_expr_from_value(sub->rhs()), -1);
    }
    if (auto *mul = dynamic_cast<const yir::MulIOp *>(def)) {
        std::int64_t scale = 0;
        if (const_i32_value(mul->lhs(), scale)) {
            return scale_affine_expr(affine_expr_from_value(mul->rhs()), scale);
        }
        if (const_i32_value(mul->rhs(), scale)) {
            return scale_affine_expr(affine_expr_from_value(mul->lhs()), scale);
        }
        return invalid_poly_affine_expr();
    }

    PolyAffineExpr expr;
    add_affine_term(expr, value, 1);
    normalize_affine_terms(expr);
    return expr;
}

bool value_is_dim_plus_constant(const yir::Value *value, const yir::Value *dim,
                                std::int64_t expected_constant) {
    std::int64_t constant = 0;
    return is_dim_plus_constant(affine_expr_from_value(value), dim, constant) &&
           constant == expected_constant;
}

std::size_t count_value_uses_in_region(const yir::Region &region, const yir::Value *value);

std::size_t count_value_uses_in_operation(const yir::Operation &op, const yir::Value *value) {
    std::size_t count = 0;
    for (auto *operand : op.operands()) {
        if (operand == value) {
            ++count;
        }
    }
    if (auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
        count += count_value_uses_in_region(if_op->then_region(), value);
        if (if_op->has_else()) {
            count += count_value_uses_in_region(if_op->else_region(), value);
        }
    } else if (auto *while_op = dynamic_cast<const yir::WhileOp *>(&op)) {
        count += count_value_uses_in_region(while_op->cond_region(), value);
        count += count_value_uses_in_region(while_op->body_region(), value);
    } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
        count += count_value_uses_in_region(for_op->body_region(), value);
    }
    return count;
}

std::size_t count_value_uses_in_region(const yir::Region &region, const yir::Value *value) {
    std::size_t count = 0;
    for (const auto &op : region.operations()) {
        count += count_value_uses_in_operation(*op, value);
    }
    return count;
}

std::size_t count_value_uses_in_module(const yir::Module &module, const yir::Value *value) {
    std::size_t count = 0;
    for (const auto &function : module.functions()) {
        count += count_value_uses_in_region(function->body(), value);
    }
    return count;
}

bool is_dead_pure_result_eliminable(const yir::Operation &op) {
    if (op.result() == nullptr) {
        return false;
    }

    if (dynamic_cast<const yir::ConstI32Op *>(&op) != nullptr ||
        dynamic_cast<const yir::ConstF32Op *>(&op) != nullptr ||
        dynamic_cast<const yir::ConstBoolOp *>(&op) != nullptr ||
        dynamic_cast<const yir::ZeroOp *>(&op) != nullptr ||
        dynamic_cast<const yir::ElemAddrOp *>(&op) != nullptr ||
        dynamic_cast<const yir::DecayOp *>(&op) != nullptr ||
        dynamic_cast<const yir::AddIOp *>(&op) != nullptr ||
        dynamic_cast<const yir::SubIOp *>(&op) != nullptr ||
        dynamic_cast<const yir::MulIOp *>(&op) != nullptr ||
        dynamic_cast<const yir::AddFOp *>(&op) != nullptr ||
        dynamic_cast<const yir::SubFOp *>(&op) != nullptr ||
        dynamic_cast<const yir::MulFOp *>(&op) != nullptr ||
        dynamic_cast<const yir::ICmpOp *>(&op) != nullptr ||
        dynamic_cast<const yir::FCmpOp *>(&op) != nullptr ||
        dynamic_cast<const yir::ZExtI1ToI32Op *>(&op) != nullptr ||
        dynamic_cast<const yir::TruncI32ToI1Op *>(&op) != nullptr ||
        dynamic_cast<const yir::SIToFPOp *>(&op) != nullptr ||
        dynamic_cast<const yir::FPToSIOp *>(&op) != nullptr ||
        dynamic_cast<const yir::ToBoolOp *>(&op) != nullptr ||
        dynamic_cast<const yir::NotOp *>(&op) != nullptr) {
        return true;
    }
    return false;
}

void replace_value_in_nested_regions(yir::Operation &op, yir::Value *old_value,
                                     yir::Value *new_value) {
    if (auto *if_op = dynamic_cast<yir::IfOp *>(&op)) {
        replace_value_in_region(if_op->then_region(), old_value, new_value);
        if (if_op->has_else()) {
            replace_value_in_region(if_op->else_region(), old_value, new_value);
        }
        return;
    }
    if (auto *while_op = dynamic_cast<yir::WhileOp *>(&op)) {
        replace_value_in_region(while_op->cond_region(), old_value, new_value);
        replace_value_in_region(while_op->body_region(), old_value, new_value);
        return;
    }
    if (auto *for_op = dynamic_cast<yir::ForOp *>(&op)) {
        replace_value_in_region(for_op->body_region(), old_value, new_value);
        return;
    }
}

void replace_value_in_region(yir::Region &region, yir::Value *old_value, yir::Value *new_value) {
    for (auto &op : region.operations()) {
        replace_operands(*op, old_value, new_value);
        replace_value_in_nested_regions(*op, old_value, new_value);
    }
}

void replace_value_in_range(yir::Region &region, std::size_t first_op, std::size_t last_op,
                            yir::Value *old_value, yir::Value *new_value) {
    auto &ops = region.operations();
    if (ops.empty() || first_op >= ops.size()) {
        return;
    }
    last_op = std::min(last_op, ops.size() - 1);
    for (std::size_t i = first_op; i <= last_op; ++i) {
        replace_operands(*ops[i], old_value, new_value);
        replace_value_in_nested_regions(*ops[i], old_value, new_value);
    }
}

void replace_value_after(yir::Region &region, std::size_t first_op, yir::Value *old_value,
                         yir::Value *new_value) {
    auto &ops = region.operations();
    for (std::size_t i = first_op; i < ops.size(); ++i) {
        replace_operands(*ops[i], old_value, new_value);
        replace_value_in_nested_regions(*ops[i], old_value, new_value);
    }
}

std::string verify_message(const yir::VerifyResult &result) {
    if (result.errors.empty()) {
        return "YIR verification failed after YIRPolyhedralTransformPass";
    }
    std::ostringstream oss;
    oss << "YIR verification failed after YIRPolyhedralTransformPass: ";
    for (std::size_t i = 0; i < result.errors.size(); ++i) {
        if (i != 0) {
            oss << "; ";
        }
        oss << result.errors[i];
    }
    return oss.str();
}

bool fits_i32(std::int64_t value) {
    return value >= std::numeric_limits<int>::min() &&
           value <= std::numeric_limits<int>::max();
}

struct AffineMaterializedComponent {
    bool is_constant = false;
    std::int64_t value = 0;
    yir::Value *term = nullptr;
};

bool can_materialize_affine_expr(const PolyAffineExpr &expr) {
    if (!expr.is_linear() || !fits_i32(expr.constant)) {
        return false;
    }
    return std::all_of(expr.terms.begin(), expr.terms.end(), [](const auto &term) {
        return fits_i32(term.second);
    });
}

std::vector<AffineMaterializedComponent>
materialized_components_for(const PolyAffineExpr &expr, yir::Value *iv,
                            yir::Value *iv_replacement) {
    std::vector<AffineMaterializedComponent> components;
    components.reserve(expr.terms.size() + (expr.constant == 0 ? 0 : 1));
    for (const auto &[value, coefficient] : expr.terms) {
        auto *term = const_cast<yir::Value *>(value);
        if (value == iv) {
            term = iv_replacement;
        }
        components.push_back({false, coefficient, term});
    }
    if (expr.constant != 0 || components.empty()) {
        components.push_back({true, expr.constant, nullptr});
    }
    return components;
}

template <typename OpT, typename... Args>
OpT *insert_op_before(yir::Region &region, std::size_t &insert_pos, Args &&...args) {
    auto op = std::make_unique<OpT>(std::forward<Args>(args)...);
    auto *raw = op.get();
    raw->set_parent(&region);
    region.operations().insert(region.operations().begin() + static_cast<std::ptrdiff_t>(insert_pos),
                               std::move(op));
    ++insert_pos;
    return raw;
}

void insert_existing_op_before(yir::Region &region, std::size_t &insert_pos,
                               std::unique_ptr<yir::Operation> op) {
    op->set_parent(&region);
    region.operations().insert(region.operations().begin() + static_cast<std::ptrdiff_t>(insert_pos),
                               std::move(op));
    ++insert_pos;
}

std::string stencil_temp_name(const char *stem, std::size_t id) {
    return std::string("poly.stencil.") + stem + std::to_string(id);
}

std::string future_temp_name(const char *stem, std::size_t id) {
    return std::string("poly.future.") + stem + std::to_string(id);
}

std::string init_temp_name(const char *stem, std::size_t id) {
    return std::string("poly.init.") + stem + std::to_string(id);
}

std::string wave_temp_name(const char *stem, std::size_t id) {
    return std::string("poly.wave.") + stem + std::to_string(id);
}

std::string tile_temp_name(const char *stem, std::size_t id) {
    return std::string("poly.tile.") + stem + std::to_string(id);
}

std::string partition_temp_name(const char *stem, std::size_t id) {
    return std::string("poly.partition.") + stem + std::to_string(id);
}

std::string reduction_temp_name(const char *stem, std::size_t id) {
    return std::string("poly.reduction.") + stem + std::to_string(id);
}

std::string tile_iv_name(const yir::Value *iv, const char *stem, std::size_t id) {
    const std::string base = iv == nullptr || iv->name().empty() ? "poly.tile" : iv->name();
    std::string name = base + ".tile";
    if (std::strcmp(stem, "step") == 0) {
        name += ".step";
    }
    if (id > 2) {
        name += std::to_string(id);
    }
    return name;
}

yir::Value *materialize_component(yir::Region &region, std::size_t &insert_pos,
                                  const AffineMaterializedComponent &component,
                                  std::size_t &next_temp) {
    if (component.is_constant) {
        auto *constant = insert_op_before<yir::ConstI32Op>(
            region, insert_pos, static_cast<int>(component.value), stencil_temp_name("c", next_temp++));
        return constant->result();
    }
    if (component.term == nullptr) {
        return nullptr;
    }
    if (component.value == 1) {
        return component.term;
    }

    auto *coefficient = insert_op_before<yir::ConstI32Op>(
        region, insert_pos, static_cast<int>(component.value), stencil_temp_name("c", next_temp++));
    auto *product = insert_op_before<yir::MulIOp>(
        region, insert_pos, coefficient->result(), component.term, stencil_temp_name("mul", next_temp++));
    return product->result();
}

yir::Value *materialize_affine_expr(yir::Region &region, std::size_t &insert_pos,
                                    const PolyAffineExpr &expr, yir::Value *iv,
                                    yir::Value *iv_replacement, std::size_t &next_temp) {
    if (!can_materialize_affine_expr(expr)) {
        return nullptr;
    }

    auto components = materialized_components_for(expr, iv, iv_replacement);
    yir::Value *value = materialize_component(region, insert_pos, components.front(), next_temp);
    if (value == nullptr) {
        return nullptr;
    }
    for (std::size_t i = 1; i < components.size(); ++i) {
        auto *rhs = materialize_component(region, insert_pos, components[i], next_temp);
        if (rhs == nullptr) {
            return nullptr;
        }
        auto *sum = insert_op_before<yir::AddIOp>(
            region, insert_pos, value, rhs, stencil_temp_name("add", next_temp++));
        value = sum->result();
    }
    return value;
}

void collect_for_body_owners(yir::Region &region,
                             std::unordered_map<const yir::Region *, yir::ForOp *> &owners) {
    for (auto &op : region.operations()) {
        if (auto *for_op = dynamic_cast<yir::ForOp *>(op.get())) {
            owners[&for_op->body_region()] = for_op;
            collect_for_body_owners(for_op->body_region(), owners);
            continue;
        }
        if (auto *if_op = dynamic_cast<yir::IfOp *>(op.get())) {
            collect_for_body_owners(if_op->then_region(), owners);
            if (if_op->has_else()) {
                collect_for_body_owners(if_op->else_region(), owners);
            }
            continue;
        }
        if (auto *while_op = dynamic_cast<yir::WhileOp *>(op.get())) {
            collect_for_body_owners(while_op->cond_region(), owners);
            collect_for_body_owners(while_op->body_region(), owners);
        }
    }
}

std::unordered_map<const yir::Region *, yir::ForOp *> collect_for_body_owners(yir::Module &module) {
    std::unordered_map<const yir::Region *, yir::ForOp *> owners;
    for (auto &function : module.functions()) {
        collect_for_body_owners(function->body(), owners);
    }
    return owners;
}

void collect_enclosing_for_region_owners(
    yir::Region &region, yir::ForOp *enclosing_for,
    std::unordered_map<const yir::Region *, yir::ForOp *> &owners) {
    if (enclosing_for != nullptr) {
        owners[&region] = enclosing_for;
    }
    for (auto &op : region.operations()) {
        if (auto *for_op = dynamic_cast<yir::ForOp *>(op.get())) {
            collect_enclosing_for_region_owners(for_op->body_region(), for_op, owners);
            continue;
        }
        if (auto *if_op = dynamic_cast<yir::IfOp *>(op.get())) {
            collect_enclosing_for_region_owners(if_op->then_region(), enclosing_for, owners);
            if (if_op->has_else()) {
                collect_enclosing_for_region_owners(if_op->else_region(), enclosing_for, owners);
            }
            continue;
        }
        if (auto *while_op = dynamic_cast<yir::WhileOp *>(op.get())) {
            collect_enclosing_for_region_owners(while_op->cond_region(), enclosing_for, owners);
            collect_enclosing_for_region_owners(while_op->body_region(), enclosing_for, owners);
        }
    }
}

std::unordered_map<const yir::Region *, yir::ForOp *>
collect_enclosing_for_region_owners(yir::Module &module) {
    std::unordered_map<const yir::Region *, yir::ForOp *> owners;
    for (auto &function : module.functions()) {
        collect_enclosing_for_region_owners(function->body(), nullptr, owners);
    }
    return owners;
}

class PolyhedralTransformer {
public:
    explicit PolyhedralTransformer(yir::Module &module, PolyModelInfo& model_info,
                                   PolyDependenceInfo& dep_info,
                                   const YIRPolyhedralCanonicalInfo& canonical_info,
                                   cost_model::CostModelReport *cost_report = nullptr)
        : module_(module), model_info_(model_info), dep_info_(dep_info),
          canonical_info_(canonical_info), cost_report_(cost_report), num_interchanged_(0), num_tiled_(0),
          num_fused_(0),
          num_serial_wavefronts_(0), num_parallel_wavefronts_(0), num_wave_unrolls_(0),
          num_parallel_wave_unrolls_(0), num_same_iteration_reloads_(0),
          num_stencil_carries_(0), num_future_neighbor_constants_(0),
          num_initialization_reductions_(0), num_affine_replacements_(0),
          num_nearest_write_queries_(0), num_nearest_queries_(0), num_dead_memory_writes_(0),
          num_dead_pure_results_(0), num_unused_globals_(0),
          num_relation_legality_proofs_(0), num_relation_legality_rejections_(0),
          num_relation_legality_unknown_(0), num_domain_partitions_(0),
          num_reduction_privatizations_(0), num_accumulator_promotions_(0) {}

    bool transform(YIRPolyhedralTransformMode mode) {
        bool changed = false;
        if (mode != YIRPolyhedralTransformMode::Local) {
            changed = try_loop_fusion();
            if (changed) {
                structural_change_ = YIRPolyhedralStructuralChange::Fusion;
                // Fusion changes SCoP ownership and invalidates statement
                // operation pointers. The pipeline rebuilds all polyhedral
                // analyses before considering another structural transform.
                return true;
            }

            for (auto& scop : model_info_.models) {
                changed |= try_interchange_or_tile(scop);
            }
            if (changed) {
                structural_change_ = YIRPolyhedralStructuralChange::Schedule;
                if (mode == YIRPolyhedralTransformMode::Structural) {
                    return true;
                }
            }
        }

        if (mode == YIRPolyhedralTransformMode::Structural) {
            return changed;
        }

        auto for_body_owners = collect_for_body_owners(module_);

        std::vector<SameIterationReload> same_iteration_reloads;
        std::vector<StencilCarryCandidate> stencil_carries;
        std::vector<FutureNeighborConstant> future_neighbor_constants =
            find_future_neighbor_constants(for_body_owners);
        std::vector<InitializationReduction> initialization_reductions =
            find_initialization_reductions(for_body_owners);
        for (const auto &scop : model_info_.models) {
            auto reloads = find_same_iteration_reloads(scop);
            same_iteration_reloads.insert(same_iteration_reloads.end(), reloads.begin(), reloads.end());

            auto carries = find_stencil_carry_candidates(scop, for_body_owners);
            stencil_carries.insert(stencil_carries.end(), carries.begin(), carries.end());
        }

        std::unordered_set<yir::ArrayLoadOp *> erased_loads;
        changed |= replace_future_neighbor_constants(future_neighbor_constants, erased_loads);
        const auto future_erased_loads = erased_loads;
        changed |= apply_stencil_carries(stencil_carries, erased_loads);
        changed |= replace_same_iteration_reloads(same_iteration_reloads, erased_loads);
        changed |= replace_equivalent_affine_scalars();
        changed |= eliminate_dead_memory_writes();
        changed |= apply_initialization_reductions(initialization_reductions,
                                                   future_erased_loads);
        changed |= eliminate_dead_pure_results();
        changed |= eliminate_unused_globals();
        changed |= try_reduction_privatizations();
        changed |= try_output_accumulator_promotions();
        // This code regeneration consumes statement-domain information and
        // invalidates statement operation pointers, so it intentionally runs
        // after all model-driven transforms.
        changed |= try_statement_domain_partitions();
        changed |= eliminate_dead_pure_results();
        return changed;
    }

    std::size_t num_interchanged() const { return num_interchanged_; }
    std::size_t num_tiled() const { return num_tiled_; }
    std::size_t num_fused() const { return num_fused_; }
    std::size_t num_serial_wavefronts() const { return num_serial_wavefronts_; }
    std::size_t num_parallel_wavefronts() const { return num_parallel_wavefronts_; }
    std::size_t num_wave_unrolls() const { return num_wave_unrolls_; }
    std::size_t num_parallel_wave_unrolls() const { return num_parallel_wave_unrolls_; }
    std::size_t num_same_iteration_reloads() const { return num_same_iteration_reloads_; }
    std::size_t num_stencil_carries() const { return num_stencil_carries_; }
    std::size_t num_future_neighbor_constants() const { return num_future_neighbor_constants_; }
    std::size_t num_initialization_reductions() const { return num_initialization_reductions_; }
    std::size_t num_affine_replacements() const { return num_affine_replacements_; }
    std::size_t num_nearest_write_queries() const { return num_nearest_write_queries_; }
    std::size_t num_nearest_queries() const { return num_nearest_queries_; }
    std::size_t num_dead_memory_writes() const { return num_dead_memory_writes_; }
    std::size_t num_dead_pure_results() const { return num_dead_pure_results_; }
    std::size_t num_unused_globals() const { return num_unused_globals_; }
    std::size_t num_domain_partitions() const { return num_domain_partitions_; }
    std::size_t num_reduction_privatizations() const {
        return num_reduction_privatizations_;
    }
    std::size_t num_accumulator_promotions() const {
        return num_accumulator_promotions_;
    }
    std::size_t num_relation_legality_proofs() const {
        return num_relation_legality_proofs_;
    }
    std::size_t num_relation_legality_rejections() const {
        return num_relation_legality_rejections_;
    }
    std::size_t num_relation_legality_unknown() const {
        return num_relation_legality_unknown_;
    }
    YIRPolyhedralStructuralChange structural_change() const {
        return structural_change_;
    }

private:
    struct OutputAccumulatorPromotion {
        enum class LaneLowering {
            Scalar,
            // TODO(rvv): select this lowering when vector semantics and target
            // feature plumbing are available. Legality remains shared with the
            // scalar register-blocked form.
            RVV,
        };

        yir::ForOp *output_loop = nullptr;
        yir::AssignOp *reduction_reset = nullptr;
        yir::VarOp *reduction_induction_var = nullptr;
        yir::VarOp *accumulator = nullptr;
        yir::ForOp *reduction_loop = nullptr;
        yir::ArrayStoreOp *output_store = nullptr;
        int factor = 0;
        bool needs_runtime_tail = false;
        bool guarded = false;
        LaneLowering lowering = LaneLowering::Scalar;
    };

    static OutputAccumulatorPromotion::LaneLowering
    choose_reduction_lane_lowering(const ReductionBodyMetrics &metrics) {
        (void)metrics;
        // TODO(rvv): consult target vector features and a vector cost model.
        // The candidate legality contract intentionally does not depend on the
        // eventual scalar or RVV lowering.
        return OutputAccumulatorPromotion::LaneLowering::Scalar;
    }

    static bool output_load_is_lane_local(
        const yir::ArrayLoadOp &load, const yir::ArrayStoreOp &store,
        const yir::Value *output_iv) {
        if (load.indices().size() != store.indices().size()) {
            return false;
        }
        for (std::size_t dim = 0; dim < load.indices().size(); ++dim) {
            if (load.indices()[dim] == output_iv &&
                store.indices()[dim] == output_iv) {
                return true;
            }
        }
        return false;
    }

    bool validate_accumulator_reduction_body(
        const yir::Region &region, const yir::Value *accumulator,
        const yir::ArrayStoreOp &output_store, const yir::Value *output_iv,
        bool &saw_update) const {
        for (const auto &op : region.operations()) {
            if (dynamic_cast<const yir::CallOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ArrayStoreOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::WhileOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ForOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ReturnOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::BreakOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ContinueOp *>(op.get()) != nullptr) {
                return false;
            }
            if (auto *load = dynamic_cast<const yir::ArrayLoadOp *>(op.get())) {
                if (!is_global_memory(load->array()) ||
                    (load->array() == output_store.array() &&
                     !output_load_is_lane_local(*load, output_store, output_iv))) {
                    return false;
                }
            }
            if (auto *assign = dynamic_cast<const yir::AssignOp *>(op.get())) {
                if (assign->target() != accumulator) {
                    return false;
                }
                auto *add = dynamic_cast<const yir::AddIOp *>(
                    assign->value() == nullptr ? nullptr
                                               : assign->value()->defining_op());
                if (add == nullptr ||
                    ((add->lhs() == accumulator) == (add->rhs() == accumulator))) {
                    return false;
                }
                saw_update = true;
            }
            if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
                if (!validate_accumulator_reduction_body(
                        if_op->then_region(), accumulator, output_store,
                        output_iv, saw_update) ||
                    (if_op->has_else() &&
                     !validate_accumulator_reduction_body(
                         if_op->else_region(), accumulator, output_store,
                         output_iv, saw_update))) {
                    return false;
                }
            }
        }
        return true;
    }

    void record_accumulator_promotion_rejection(
        const yir::ForOp &output_loop, std::string summary) const {
        if (cost_report_ == nullptr) {
            return;
        }
        cost_model::TransformDecision decision;
        decision.action = cost_model::DecisionAction::Reject;
        decision.reject_reason = cost_model::RejectReason::RegisterPressureTooHigh;
        decision.legal = true;
        decision.profitable = false;
        decision.confidence = 1.0;
        decision.transform = std::string(
            cost_model::to_string(cost_model::TransformKind::LoopUnroll));
        decision.pass_name = "YIRPolyhedralTransformPass";
        decision.candidate_id = "output-accumulator-promotion";
        decision.scope = output_loop.induction_var() == nullptr
                             ? "loop"
                             : output_loop.induction_var()->name();
        decision.reason = std::string(cost_model::to_string(decision.reject_reason));
        decision.proof.kind = cost_model::ProofKind::Structural;
        decision.proof.status = cost_model::ProofStatus::Proven;
        decision.proof.summary = std::move(summary);
        cost_report_->decisions.push_back(std::move(decision));
    }

    bool accumulator_promotion_is_profitable(
        const yir::ForOp &output_loop, const yir::ForOp &reduction_loop,
        int factor, std::size_t body_cost,
        const ReductionBodyMetrics &body_metrics) const {
        std::int64_t reduction_lower = 0;
        std::int64_t reduction_upper = 0;
        std::int64_t reduction_step = 0;
        std::int64_t reduction_trip = 16;
        bool exact_reduction_trip = false;
        if (const_i32_value(reduction_loop.lower_bound(), reduction_lower) &&
            const_i32_value(reduction_loop.upper_bound(), reduction_upper) &&
            const_i32_value(reduction_loop.step(), reduction_step) &&
            reduction_step > 0 && reduction_upper > reduction_lower) {
            reduction_trip =
                (reduction_upper - reduction_lower + reduction_step - 1) /
                reduction_step;
            exact_reduction_trip = true;
        }

        const auto target = active_target_profile();
        const auto body_cycles =
            static_cast<std::int64_t>(body_cost) +
            static_cast<std::int64_t>(body_metrics.total_loads) *
                std::max(0, target.load - target.alu_i32) +
            static_cast<std::int64_t>(body_metrics.branches) *
                std::max(0, target.unpredictable_branch - target.alu_i32);
        const auto loop_control_cycles =
            static_cast<std::int64_t>(target.branch + 2 * target.alu_i32);

        cost_model::TransformCandidate model_candidate;
        model_candidate.kind = cost_model::TransformKind::LoopUnroll;
        model_candidate.stage = cost_model::CostIRStage::YIR;
        model_candidate.pass_name = "YIRPolyhedralTransformPass";
        model_candidate.candidate_id =
            "output-accumulator-promotion-factor-" + std::to_string(factor);
        model_candidate.scope = output_loop.induction_var() == nullptr
                                    ? "loop"
                                    : output_loop.induction_var()->name();
        model_candidate.frequency.source =
            exact_reduction_trip ? cost_model::FrequencySource::ConstantTripCount
                                 : cost_model::FrequencySource::StructuredYIRLoop;
        model_candidate.frequency.scale = reduction_trip;
        model_candidate.frequency.exact_trip_count = exact_reduction_trip;
        model_candidate.frequency.confidence = exact_reduction_trip ? 0.92 : 0.68;
        model_candidate.proof.kind = cost_model::ProofKind::Structural;
        model_candidate.proof.status = cost_model::ProofStatus::Proven;
        model_candidate.proof.summary =
            body_metrics.guarded
                ? "independent output points share one side-effect-free guarded integer reduction loop"
                : "independent output points share one side-effect-free integer reduction loop";

        auto &before = model_candidate.before;
        before.static_instrs = static_cast<std::int64_t>(body_cost) + 3;
        before.loads = static_cast<std::int64_t>(body_metrics.total_loads);
        before.branches = 1 + static_cast<std::int64_t>(body_metrics.branches);
        before.max_live_values =
            static_cast<std::int64_t>(body_metrics.peak_region_loads) + 3;
        before.estimated_cycles =
            static_cast<std::int64_t>(factor) * reduction_trip *
            (body_cycles + loop_control_cycles);

        auto &after = model_candidate.after;
        after.static_instrs =
            static_cast<std::int64_t>(factor) * static_cast<std::int64_t>(body_cost) +
            factor * 2 + 3;
        after.loads = static_cast<std::int64_t>(factor) *
                      static_cast<std::int64_t>(body_metrics.total_loads);
        after.branches =
            1 + static_cast<std::int64_t>(factor) *
                    static_cast<std::int64_t>(body_metrics.branches);
        after.max_live_values =
            before.max_live_values + 2 * static_cast<std::int64_t>(factor - 1);
        after.estimated_cycles =
            reduction_trip *
                (static_cast<std::int64_t>(factor) * body_cycles +
                 loop_control_cycles) +
            factor * 2;

        model_candidate.risk.code_growth =
            after.static_instrs - before.static_instrs;
        model_candidate.risk.live_range_growth =
            2 * static_cast<std::int64_t>(factor - 1);
        // The lanes execute sequentially inside one reduction iteration. Loads
        // in mutually nested control regions are not simultaneously live, so
        // total load count is a poor pressure proxy for guarded reductions.
        // Only the additional lane accumulator and output index remain live
        // across the shared reduction loop for each extra lane.
        model_candidate.risk.register_pressure_growth =
            2 * static_cast<std::int64_t>(factor - 1);
        model_candidate.required_cleanup_passes = {
            "YIRMemoryForwardingPass", "YIRLoopOptimizationPass"};

        const auto decision = cost_model::decide(
            model_candidate, active_cost_policy(), target);
        if (cost_report_ != nullptr) {
            cost_report_->decisions.push_back(decision);
        }
        return decision.profitable;
    }

    bool match_output_accumulator_promotion(
        yir::ForOp &output_loop, OutputAccumulatorPromotion &candidate) const {
        std::int64_t lower = 0;
        std::int64_t upper = 0;
        std::int64_t step = 0;
        const bool exact_upper = const_i32_value(output_loop.upper_bound(), upper);
        if (!const_i32_value(output_loop.lower_bound(), lower) || lower != 0 ||
            !const_i32_value(output_loop.step(), step) || step != 1 ||
            (exact_upper && upper <= lower)) {
            return false;
        }

        auto &ops = output_loop.body_region().operations();
        if (ops.size() != 4) {
            return false;
        }
        auto *reset = dynamic_cast<yir::AssignOp *>(ops[0].get());
        auto *reduction_induction_var = dynamic_cast<yir::VarOp *>(ops[0].get());
        auto *accumulator = dynamic_cast<yir::VarOp *>(ops[1].get());
        auto *reduction_loop = dynamic_cast<yir::ForOp *>(ops[2].get());
        auto *store = dynamic_cast<yir::ArrayStoreOp *>(ops[3].get());
        if (accumulator == nullptr || reduction_loop == nullptr || store == nullptr ||
            accumulator->result() == nullptr ||
            !accumulator->has_initializer() || store->value() != accumulator->result() ||
            !is_global_memory(store->array()) ||
            value_depends_on_value(reduction_loop->lower_bound(),
                                   output_loop.induction_var()) ||
            value_depends_on_value(reduction_loop->upper_bound(),
                                   output_loop.induction_var()) ||
            value_depends_on_value(reduction_loop->step(),
                                   output_loop.induction_var())) {
            return false;
        }

        const bool resets_external_induction =
            reset != nullptr && reset->target() == reduction_loop->induction_var() &&
            same_value_or_i32_constant(reset->value(),
                                       reduction_loop->lower_bound());
        const bool owns_induction_var =
            reduction_induction_var != nullptr &&
            reduction_induction_var->result() == reduction_loop->induction_var() &&
            reduction_induction_var->has_initializer() &&
            same_value_or_i32_constant(reduction_induction_var->initializer(),
                                       reduction_loop->lower_bound());
        if (!resets_external_induction && !owns_induction_var) {
            return false;
        }

        bool output_indexed = false;
        for (const auto *index : store->indices()) {
            if (index == output_loop.induction_var()) {
                output_indexed = true;
            }
            if (value_depends_on_value(index, reduction_loop->induction_var())) {
                return false;
            }
        }
        if (!output_indexed) {
            return false;
        }

        bool saw_update = false;
        if (!validate_accumulator_reduction_body(
                reduction_loop->body_region(), accumulator->result(), *store,
                output_loop.induction_var(), saw_update) ||
            !saw_update || !is_wave_unroll_safe(reduction_loop->body_region())) {
            return false;
        }

        const auto trip_count = exact_upper ? upper - lower : 0;
        if (exact_upper && trip_count < 32) {
            return false;
        }

        const auto body_cost = wave_unroll_operation_count(reduction_loop->body_region());
        const auto body_metrics =
            reduction_body_metrics(reduction_loop->body_region());
        if (body_metrics.peak_region_loads > 4 ||
            (!exact_upper && body_metrics.peak_region_loads > 3)) {
            record_accumulator_promotion_rejection(
                output_loop,
                "reduction has too many simultaneously active loads for scalar register blocking");
            return false;
        }
        int factor = 0;
        if (body_metrics.guarded) {
            factor = body_cost <= 48 ? 2 : 0;
        } else if (!exact_upper) {
            factor = body_cost <= 32 ? 2 : 0;
        } else if (body_cost <= 16 && body_metrics.total_loads <= 1 &&
                   trip_count % 4 == 0) {
            factor = 4;
        } else if (body_cost <= 32 && trip_count % 2 == 0) {
            factor = 2;
        }
        if (factor == 0) {
            return false;
        }
        if (!accumulator_promotion_is_profitable(
                output_loop, *reduction_loop, factor, body_cost, body_metrics)) {
            return false;
        }

        candidate = {&output_loop, reset, reduction_induction_var, accumulator,
                     reduction_loop, store, factor, !exact_upper,
                     body_metrics.guarded,
                     choose_reduction_lane_lowering(body_metrics)};
        return true;
    }

    bool apply_output_accumulator_promotion(
        yir::Region &parent, std::size_t output_index,
        const OutputAccumulatorPromotion &candidate) {
        auto *output_loop = candidate.output_loop;
        auto *accumulator = candidate.accumulator;
        auto *reduction_loop = candidate.reduction_loop;
        auto *output_store = candidate.output_store;
        if (output_loop == nullptr || accumulator == nullptr || reduction_loop == nullptr ||
            output_store == nullptr || candidate.factor < 2 ||
            candidate.lowering != OutputAccumulatorPromotion::LaneLowering::Scalar ||
            output_index >= parent.operations().size() ||
            induction_value_is_live_after(parent, output_index,
                                          output_loop->induction_var())) {
            return false;
        }

        auto *original_upper = output_loop->upper_bound();
        std::vector<std::unique_ptr<yir::Operation>> setup_ops;
        yir::Value *paired_upper = original_upper;
        std::unique_ptr<yir::IfOp> tail_if;
        if (candidate.needs_runtime_tail) {
            auto divisor = std::make_unique<yir::ConstI32Op>(
                candidate.factor,
                reduction_temp_name("output_divisor", next_reduction_temp_++));
            auto *divisor_value = divisor->result();
            divisor->set_parent(&parent);
            setup_ops.push_back(std::move(divisor));

            auto pair_count = std::make_unique<yir::DivSIOp>(
                original_upper, divisor_value,
                reduction_temp_name("output_pairs", next_reduction_temp_++));
            auto *pair_count_value = pair_count->result();
            pair_count->set_parent(&parent);
            setup_ops.push_back(std::move(pair_count));

            auto aligned_upper = std::make_unique<yir::MulIOp>(
                pair_count_value, divisor_value,
                reduction_temp_name("output_aligned", next_reduction_temp_++));
            paired_upper = aligned_upper->result();
            aligned_upper->set_parent(&parent);
            setup_ops.push_back(std::move(aligned_upper));

            auto tail_cmp = std::make_unique<yir::ICmpOp>(
                yir::ICmpOp::Predicate::Lt, paired_upper, original_upper,
                reduction_temp_name("output_has_tail", next_reduction_temp_++));
            auto *tail_cmp_value = tail_cmp->result();
            tail_cmp->set_parent(&parent);
            setup_ops.push_back(std::move(tail_cmp));

            tail_if = std::make_unique<yir::IfOp>(tail_cmp_value);
            tail_if->set_parent(&parent);
            ValueMap tail_map{{output_loop->induction_var(), paired_upper}};
            if (!clone_wave_unroll_region_into(
                    output_loop->body_region(), tail_if->then_region(),
                    std::move(tail_map))) {
                return false;
            }
        }

        auto promoted_step = std::make_unique<yir::ConstI32Op>(
            candidate.factor,
            reduction_temp_name("output_step", next_reduction_temp_++));
        auto *promoted_step_value = promoted_step->result();
        promoted_step->set_parent(&parent);

        using OpList = yir::Region::OpList;
        OpList promoted_body;
        auto append = [&](std::unique_ptr<yir::Operation> op) {
            op->set_parent(&output_loop->body_region());
            promoted_body.push_back(std::move(op));
        };

        yir::Value *promoted_reduction_iv = reduction_loop->induction_var();
        ValueMap reduction_map;
        if (candidate.reduction_induction_var != nullptr) {
            auto promoted_iv = std::make_unique<yir::VarOp>(
                candidate.reduction_induction_var->result()->type(),
                candidate.reduction_induction_var->initializer(),
                reduction_temp_name("iv", next_reduction_temp_++));
            promoted_reduction_iv = promoted_iv->result();
            reduction_map.emplace(reduction_loop->induction_var(),
                                  promoted_reduction_iv);
            append(std::move(promoted_iv));
        } else {
            append(std::make_unique<yir::AssignOp>(
                reduction_loop->induction_var(), reduction_loop->lower_bound()));
        }

        std::vector<ValueMap> lane_maps;
        lane_maps.reserve(static_cast<std::size_t>(candidate.factor));

        for (int lane = 0; lane < candidate.factor; ++lane) {
            yir::Value *lane_iv = output_loop->induction_var();
            if (lane != 0) {
                auto lane_constant = std::make_unique<yir::ConstI32Op>(
                    lane, reduction_temp_name("lane", next_reduction_temp_++));
                auto *lane_constant_value = lane_constant->result();
                append(std::move(lane_constant));
                auto lane_add = std::make_unique<yir::AddIOp>(
                    output_loop->induction_var(), lane_constant_value,
                    reduction_temp_name("output", next_reduction_temp_++));
                lane_iv = lane_add->result();
                append(std::move(lane_add));
            }

            ValueMap lane_map = reduction_map;
            lane_map.emplace(output_loop->induction_var(), lane_iv);
            auto lane_accumulator = std::make_unique<yir::VarOp>(
                accumulator->result()->type(),
                map_value(accumulator->initializer(), lane_map),
                reduction_temp_name("acc", next_reduction_temp_++));
            auto *lane_accumulator_value = lane_accumulator->result();
            append(std::move(lane_accumulator));
            lane_map.emplace(accumulator->result(), lane_accumulator_value);
            lane_maps.push_back(std::move(lane_map));
        }

        auto promoted_reduction = std::make_unique<yir::ForOp>(
            promoted_reduction_iv,
            map_value(reduction_loop->lower_bound(), reduction_map),
            map_value(reduction_loop->upper_bound(), reduction_map),
            map_value(reduction_loop->step(), reduction_map));
        promoted_reduction->set_parent(&output_loop->body_region());
        for (int lane = 0; lane < candidate.factor; ++lane) {
            if (!clone_wave_unroll_region_into(
                    reduction_loop->body_region(), promoted_reduction->body_region(),
                    lane_maps[static_cast<std::size_t>(lane)])) {
                return false;
            }
        }
        promoted_body.push_back(std::move(promoted_reduction));

        for (int lane = 0; lane < candidate.factor; ++lane) {
            auto cloned_store = clone_wave_unroll_op(
                *output_store, lane_maps[static_cast<std::size_t>(lane)]);
            if (cloned_store == nullptr) {
                return false;
            }
            append(std::move(cloned_store));
        }

        output_loop->body_region().operations() = std::move(promoted_body);
        output_loop->operands()[2] = paired_upper;
        output_loop->operands()[3] = promoted_step_value;
        setup_ops.push_back(std::move(promoted_step));
        auto &parent_ops = parent.operations();
        parent_ops.insert(
            parent_ops.begin() + static_cast<std::ptrdiff_t>(output_index),
            std::make_move_iterator(setup_ops.begin()),
            std::make_move_iterator(setup_ops.end()));
        if (tail_if != nullptr) {
            parent_ops.insert(
                parent_ops.begin() + static_cast<std::ptrdiff_t>(
                                         output_index + setup_ops.size() + 1),
                std::move(tail_if));
        }
        ++num_accumulator_promotions_;
        return true;
    }

    bool try_output_accumulator_promotions(yir::Region &region) {
        bool changed = false;
        auto &ops = region.operations();
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (auto *loop = dynamic_cast<yir::ForOp *>(ops[i].get())) {
                OutputAccumulatorPromotion candidate;
                if (match_output_accumulator_promotion(*loop, candidate) &&
                    apply_output_accumulator_promotion(region, i, candidate)) {
                    changed = true;
                    ++i;
                    continue;
                }
                changed = try_output_accumulator_promotions(loop->body_region()) || changed;
                continue;
            }
            if (auto *if_op = dynamic_cast<yir::IfOp *>(ops[i].get())) {
                changed = try_output_accumulator_promotions(if_op->then_region()) || changed;
                if (if_op->has_else()) {
                    changed = try_output_accumulator_promotions(if_op->else_region()) || changed;
                }
            }
        }
        return changed;
    }

    bool try_output_accumulator_promotions() {
        bool changed = false;
        for (auto &function : module_.functions()) {
            changed = try_output_accumulator_promotions(function->body()) || changed;
        }
        return changed;
    }

    struct ReductionPrivatization {
        yir::ArrayLoadOp *load = nullptr;
        yir::ArrayStoreOp *store = nullptr;
        yir::ForOp *scope_loop = nullptr;
    };

    bool is_global_memory(const yir::Value *memory) const {
        return std::any_of(module_.globals().begin(), module_.globals().end(),
                           [memory](const auto &global) {
                               return global != nullptr && global->address() == memory;
                           });
    }

    static bool region_only_has_reduction_memory_accesses(
        const yir::Region &region, const yir::Value *memory,
        const yir::ArrayLoadOp *reduction_load,
        const yir::ArrayStoreOp *reduction_store) {
        for (const auto &op : region.operations()) {
            if (auto *load = dynamic_cast<const yir::ArrayLoadOp *>(op.get())) {
                if (load->array() == memory && load != reduction_load) {
                    return false;
                }
            } else if (auto *store = dynamic_cast<const yir::ArrayStoreOp *>(op.get())) {
                if (store->array() == memory && store != reduction_store) {
                    return false;
                }
            }

            if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
                if (!region_only_has_reduction_memory_accesses(
                        if_op->then_region(), memory, reduction_load, reduction_store) ||
                    (if_op->has_else() &&
                     !region_only_has_reduction_memory_accesses(
                         if_op->else_region(), memory, reduction_load,
                         reduction_store))) {
                    return false;
                }
            } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
                if (!region_only_has_reduction_memory_accesses(
                        for_op->body_region(), memory, reduction_load, reduction_store)) {
                    return false;
                }
            }
        }
        return true;
    }

    std::vector<ReductionPrivatization> find_reduction_privatizations() {
        struct ReductionGroup {
            const PolyStmt *store_stmt = nullptr;
            yir::ArrayStoreOp *store = nullptr;
            yir::ArrayLoadOp *load = nullptr;
        };

        std::unordered_map<std::size_t, ReductionGroup> groups;
        for (const auto &scop : model_info_.models) {
            for (const auto &stmt : scop.statements) {
                if (stmt.reduction_kind != PolyStmt::ReductionKind::Add ||
                    stmt.reduction_group_id == 0) {
                    continue;
                }
                auto &group = groups[stmt.reduction_group_id];
                if (auto *store = dynamic_cast<yir::ArrayStoreOp *>(
                        const_cast<yir::Operation *>(stmt.op))) {
                    group.store_stmt = &stmt;
                    group.store = store;
                } else if (auto *load = dynamic_cast<yir::ArrayLoadOp *>(
                               const_cast<yir::Operation *>(stmt.op))) {
                    group.load = load;
                }
            }
        }

        auto owners = collect_enclosing_for_region_owners(module_);
        std::vector<ReductionPrivatization> candidates;
        for (const auto &[_, group] : groups) {
            if (group.store_stmt == nullptr || group.store == nullptr || group.load == nullptr ||
                group.store->parent() == nullptr || group.load->parent() == nullptr ||
                group.store->array() != group.load->array() ||
                !is_global_memory(group.store->array())) {
                continue;
            }

            const auto &reduction_dims = group.store_stmt->reduction_dims;
            if (reduction_dims.empty()) {
                continue;
            }
            const auto first_reduction =
                std::find(reduction_dims.begin(), reduction_dims.end(), true);
            if (first_reduction == reduction_dims.end() ||
                !std::all_of(first_reduction, reduction_dims.end(),
                             [](bool reduction) { return reduction; })) {
                continue;
            }
            const std::size_t reduction_depth = static_cast<std::size_t>(
                first_reduction - reduction_dims.begin());
            auto loops = collect_enclosing_loop_nest(group.store->parent(), owners);
            if (!loop_nest_matches_stmt(loops, *group.store_stmt) ||
                reduction_depth >= loops.outer_to_inner.size()) {
                continue;
            }
            auto *scope_loop = loops.outer_to_inner[reduction_depth];
            if (scope_loop == nullptr || scope_loop->parent() == nullptr ||
                !region_only_has_reduction_memory_accesses(
                    scope_loop->body_region(), group.store->array(), group.load,
                    group.store)) {
                continue;
            }
            const auto store_indices = group.store->indices();
            const bool indices_available =
                std::all_of(store_indices.begin(), store_indices.end(),
                            [scope_loop](const yir::Value *index) {
                                return !value_defined_inside_region(
                                    index, scope_loop->body_region());
                            });
            if (!indices_available) {
                continue;
            }
            candidates.push_back({group.load, group.store, scope_loop});
        }
        return candidates;
    }

    bool apply_reduction_privatization(const ReductionPrivatization &candidate) {
        auto *load = candidate.load;
        auto *store = candidate.store;
        auto *scope_loop = candidate.scope_loop;
        if (load == nullptr || store == nullptr || scope_loop == nullptr ||
            load->parent() == nullptr || store->parent() == nullptr ||
            scope_loop->parent() == nullptr || load->result() == nullptr ||
            store->value() == nullptr) {
            return false;
        }

        auto *scope_parent = scope_loop->parent();
        std::size_t scope_index = 0;
        std::size_t load_index = 0;
        std::size_t store_index = 0;
        if (!find_operation_index(*scope_parent, *scope_loop, scope_index) ||
            !find_operation_index(*load->parent(), *load, load_index) ||
            !find_operation_index(*store->parent(), *store, store_index)) {
            return false;
        }

        auto indices = store->indices();
        auto *memory = store->array();
        auto initial_load = std::make_unique<yir::ArrayLoadOp>(
            memory, indices, load->result()->type(),
            reduction_temp_name("initial", next_reduction_temp_++));
        auto *initial_value = initial_load->result();
        initial_load->set_parent(scope_parent);
        auto accumulator = std::make_unique<yir::VarOp>(
            load->result()->type(), initial_value,
            reduction_temp_name("acc", next_reduction_temp_++));
        auto *accumulator_value = accumulator->result();
        accumulator->set_parent(scope_parent);

        replace_value_in_region(scope_loop->body_region(), load->result(),
                                accumulator_value);
        auto *load_parent = load->parent();
        load_parent->operations().erase(
            load_parent->operations().begin() + static_cast<std::ptrdiff_t>(load_index));

        // The load erase may shift the store when both are in the same region.
        auto *store_parent = store->parent();
        if (!find_operation_index(*store_parent, *store, store_index)) {
            return false;
        }
        auto assign = std::make_unique<yir::AssignOp>(accumulator_value, store->value());
        assign->set_parent(store_parent);
        store_parent->operations()[store_index] = std::move(assign);

        auto final_store = std::make_unique<yir::ArrayStoreOp>(
            accumulator_value, memory, std::move(indices));
        final_store->set_parent(scope_parent);
        auto &parent_ops = scope_parent->operations();
        parent_ops.insert(parent_ops.begin() + static_cast<std::ptrdiff_t>(scope_index),
                          std::move(initial_load));
        parent_ops.insert(parent_ops.begin() + static_cast<std::ptrdiff_t>(scope_index + 1),
                          std::move(accumulator));
        parent_ops.insert(parent_ops.begin() + static_cast<std::ptrdiff_t>(scope_index + 3),
                          std::move(final_store));
        ++num_reduction_privatizations_;
        return true;
    }

    bool try_reduction_privatizations() {
        bool changed = false;
        const auto candidates = find_reduction_privatizations();
        for (const auto &candidate : candidates) {
            changed = apply_reduction_privatization(candidate) || changed;
        }
        return changed;
    }

    static bool is_zero_affine_expr(const PolyAffineExpr &expr) {
        return expr.is_linear() && expr.constant == 0 && expr.terms.empty();
    }

    static bool is_mod_two_of_dim(const PolyAffineExpr &expr,
                                  const yir::Value *dim) {
        return expr.kind == PolyAffineExprKind::Mod && expr.divisor == 2 &&
               expr.operand != nullptr && expr.operand->is_linear() &&
               expr.operand->constant == 0 && expr.operand->terms.size() == 1 &&
               expr.operand->terms.front().first == dim &&
               expr.operand->terms.front().second == 1;
    }

    bool model_has_parity_partition(const yir::Value *iv) const {
        bool has_even = false;
        bool has_odd = false;
        for (const auto &scop : model_info_.models) {
            for (const auto &stmt : scop.statements) {
                if (std::find(stmt.schedule_dims.begin(), stmt.schedule_dims.end(), iv) ==
                    stmt.schedule_dims.end()) {
                    continue;
                }
                for (const auto &constraint : stmt.domain.constraints) {
                    const bool matches =
                        (is_mod_two_of_dim(constraint.lhs, iv) &&
                         is_zero_affine_expr(constraint.rhs)) ||
                        (is_mod_two_of_dim(constraint.rhs, iv) &&
                         is_zero_affine_expr(constraint.lhs));
                    if (!matches) {
                        continue;
                    }
                    has_even = has_even || constraint.predicate == PolyPredicate::Eq;
                    has_odd = has_odd || constraint.predicate == PolyPredicate::Ne;
                }
            }
        }
        return has_even && has_odd;
    }

    static bool induction_value_is_live_after(const yir::Region &parent,
                                              std::size_t loop_index,
                                              const yir::Value *iv) {
        const auto &ops = parent.operations();
        for (std::size_t i = loop_index + 1; i < ops.size(); ++i) {
            if (auto *assign = dynamic_cast<const yir::AssignOp *>(ops[i].get())) {
                if (assign->target() == iv) {
                    return assign->value() == iv;
                }
            }
            if (operation_uses_value(*ops[i], iv)) {
                return true;
            }
        }
        return false;
    }

    struct ParityBranch {
        yir::ForOp *loop = nullptr;
        yir::IfOp *branch = nullptr;
        yir::Value *divisor_value = nullptr;
        bool then_is_even = true;
    };

    bool match_parity_branch(yir::ForOp &loop, yir::Region &parent,
                             std::size_t loop_index, ParityBranch &candidate) const {
        std::int64_t lower = 0;
        std::int64_t step = 0;
        if (!const_i32_value(loop.lower_bound(), lower) || lower != 0 ||
            !const_i32_value(loop.step(), step) || step != 1 ||
            !model_has_parity_partition(loop.induction_var()) ||
            induction_value_is_live_after(parent, loop_index, loop.induction_var())) {
            return false;
        }

        auto &body_ops = loop.body_region().operations();
        if (body_ops.size() != 3) {
            return false;
        }
        auto *rem = dynamic_cast<yir::RemSIOp *>(body_ops[0].get());
        auto *cmp = dynamic_cast<yir::ICmpOp *>(body_ops[1].get());
        auto *branch = dynamic_cast<yir::IfOp *>(body_ops[2].get());
        if (rem == nullptr || cmp == nullptr || branch == nullptr || !branch->has_else() ||
            branch->condition() != cmp->result() || rem->lhs() != loop.induction_var() ||
            !is_wave_unroll_safe(branch->then_region()) ||
            !is_wave_unroll_safe(branch->else_region()) ||
            region_assigns_value(branch->then_region(), loop.induction_var()) ||
            region_assigns_value(branch->else_region(), loop.induction_var())) {
            return false;
        }

        std::int64_t divisor = 0;
        if (!const_i32_value(rem->rhs(), divisor) || divisor != 2) {
            return false;
        }
        const yir::Value *other = nullptr;
        if (cmp->lhs() == rem->result()) {
            other = cmp->rhs();
        } else if (cmp->rhs() == rem->result()) {
            other = cmp->lhs();
        } else {
            return false;
        }
        std::int64_t residue = 0;
        if (!const_i32_value(other, residue) || residue != 0 ||
            (cmp->predicate() != yir::ICmpOp::Predicate::Eq &&
             cmp->predicate() != yir::ICmpOp::Predicate::Ne)) {
            return false;
        }

        candidate.loop = &loop;
        candidate.branch = branch;
        candidate.divisor_value = rem->rhs();
        candidate.then_is_even = cmp->predicate() == yir::ICmpOp::Predicate::Eq;
        return true;
    }

    bool apply_parity_partition(yir::Region &parent, std::size_t loop_index,
                                const ParityBranch &candidate) {
        auto *loop = candidate.loop;
        auto *branch = candidate.branch;
        if (loop == nullptr || branch == nullptr || loop_index >= parent.operations().size()) {
            return false;
        }

        auto pair_count = std::make_unique<yir::DivSIOp>(
            loop->upper_bound(), candidate.divisor_value,
            partition_temp_name("count", next_partition_temp_++));
        auto *pair_count_value = pair_count->result();
        pair_count->set_parent(&parent);

        auto quotient = std::make_unique<yir::VarOp>(
            yir::Type::get_i32(), loop->lower_bound(),
            partition_temp_name("q", next_partition_temp_++));
        auto *quotient_value = quotient->result();
        quotient->set_parent(&parent);

        auto paired_loop = std::make_unique<yir::ForOp>(
            quotient_value, loop->lower_bound(), pair_count_value, loop->step());
        paired_loop->set_parent(&parent);
        auto *paired_loop_raw = paired_loop.get();

        auto even_iv = std::make_unique<yir::MulIOp>(
            quotient_value, candidate.divisor_value,
            partition_temp_name("even", next_partition_temp_++));
        auto *even_iv_value = even_iv->result();
        even_iv->set_parent(&paired_loop_raw->body_region());
        paired_loop_raw->body_region().operations().push_back(std::move(even_iv));

        const auto &even_region = candidate.then_is_even ? branch->then_region()
                                                         : branch->else_region();
        const auto &odd_region = candidate.then_is_even ? branch->else_region()
                                                        : branch->then_region();
        ValueMap even_map{{loop->induction_var(), even_iv_value}};
        if (!clone_affine_partition_region_into(
                even_region, paired_loop_raw->body_region(), std::move(even_map),
                loop->induction_var(), quotient_value, loop->upper_bound(),
                pair_count_value, 2)) {
            return false;
        }

        auto odd_iv = std::make_unique<yir::AddIOp>(
            even_iv_value, loop->step(),
            partition_temp_name("odd", next_partition_temp_++));
        auto *odd_iv_value = odd_iv->result();
        odd_iv->set_parent(&paired_loop_raw->body_region());
        paired_loop_raw->body_region().operations().push_back(std::move(odd_iv));
        ValueMap odd_map{{loop->induction_var(), odd_iv_value}};
        if (!clone_affine_partition_region_into(
                odd_region, paired_loop_raw->body_region(), std::move(odd_map),
                loop->induction_var(), quotient_value, loop->upper_bound(),
                pair_count_value, 2)) {
            return false;
        }

        auto tail_iv = std::make_unique<yir::MulIOp>(
            pair_count_value, candidate.divisor_value,
            partition_temp_name("tail", next_partition_temp_++));
        auto *tail_iv_value = tail_iv->result();
        tail_iv->set_parent(&parent);
        auto tail_cmp = std::make_unique<yir::ICmpOp>(
            yir::ICmpOp::Predicate::Lt, tail_iv_value, loop->upper_bound(),
            partition_temp_name("has_tail", next_partition_temp_++));
        auto *tail_cmp_value = tail_cmp->result();
        tail_cmp->set_parent(&parent);
        auto tail_if = std::make_unique<yir::IfOp>(tail_cmp_value);
        tail_if->set_parent(&parent);
        ValueMap tail_map{{loop->induction_var(), tail_iv_value}};
        if (!clone_affine_partition_region_into(
                even_region, tail_if->then_region(), std::move(tail_map),
                loop->induction_var(), pair_count_value, loop->upper_bound(),
                pair_count_value, 2)) {
            return false;
        }

        std::vector<std::unique_ptr<yir::Operation>> replacement;
        replacement.push_back(std::move(pair_count));
        replacement.push_back(std::move(quotient));
        replacement.push_back(std::move(paired_loop));
        replacement.push_back(std::move(tail_iv));
        replacement.push_back(std::move(tail_cmp));
        replacement.push_back(std::move(tail_if));
        auto &ops = parent.operations();
        auto insertion = ops.erase(ops.begin() + static_cast<std::ptrdiff_t>(loop_index));
        ops.insert(insertion, std::make_move_iterator(replacement.begin()),
                   std::make_move_iterator(replacement.end()));
        ++num_domain_partitions_;
        return true;
    }

    bool try_statement_domain_partitions(yir::Region &region) {
        bool changed = false;
        auto &ops = region.operations();
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (auto *loop = dynamic_cast<yir::ForOp *>(ops[i].get())) {
                ParityBranch candidate;
                if (match_parity_branch(*loop, region, i, candidate) &&
                    apply_parity_partition(region, i, candidate)) {
                    changed = true;
                    i += 5;
                    continue;
                }
                changed = try_statement_domain_partitions(loop->body_region()) || changed;
                continue;
            }
            if (auto *if_op = dynamic_cast<yir::IfOp *>(ops[i].get())) {
                changed = try_statement_domain_partitions(if_op->then_region()) || changed;
                if (if_op->has_else()) {
                    changed = try_statement_domain_partitions(if_op->else_region()) || changed;
                }
            }
        }
        return changed;
    }

    bool try_statement_domain_partitions() {
        bool changed = false;
        for (auto &function : module_.functions()) {
            changed = try_statement_domain_partitions(function->body()) || changed;
        }
        return changed;
    }

    cost_model::CostModelPolicy active_cost_policy() const {
        return cost_model::policy_for_kind(
            cost_report_ != nullptr ? cost_report_->policy
                                    : cost_model::CostModelPolicyKind::Balanced);
    }

    cost_model::TargetCostProfile active_target_profile() const {
        if (cost_report_ != nullptr && !cost_report_->target.arch.empty()) {
            return cost_report_->target;
        }
        return cost_model::default_target_profile();
    }

    bool try_loop_fusion() {
        bool changed = false;
        for (;;) {
            PolyLoopFusionCandidate candidate;
            if (!find_poly_loop_fusion_candidate(candidate) ||
                !poly_loop_fusion_is_legal(candidate) ||
                !poly_loop_fusion_is_profitable(candidate) ||
                !apply_poly_loop_fusion(candidate)) {
                break;
            }
            fused_scope_ids_.insert(candidate.first_scope_id);
            fused_scope_ids_.insert(candidate.second_scope_id);
            ++num_fused_;
            changed = true;
        }
        return changed;
    }

    static bool fusion_setup_is_hoistable(const yir::Operation &op) {
        return dynamic_cast<const yir::ConstI32Op *>(&op) != nullptr ||
               dynamic_cast<const yir::ConstF32Op *>(&op) != nullptr ||
               dynamic_cast<const yir::ConstBoolOp *>(&op) != nullptr ||
               dynamic_cast<const yir::ZeroOp *>(&op) != nullptr ||
               dynamic_cast<const yir::VarOp *>(&op) != nullptr ||
               dynamic_cast<const yir::AddIOp *>(&op) != nullptr ||
               dynamic_cast<const yir::SubIOp *>(&op) != nullptr ||
               dynamic_cast<const yir::MulIOp *>(&op) != nullptr ||
               dynamic_cast<const yir::AddFOp *>(&op) != nullptr ||
               dynamic_cast<const yir::SubFOp *>(&op) != nullptr ||
               dynamic_cast<const yir::MulFOp *>(&op) != nullptr ||
               dynamic_cast<const yir::ICmpOp *>(&op) != nullptr ||
               dynamic_cast<const yir::FCmpOp *>(&op) != nullptr ||
               dynamic_cast<const yir::ZExtI1ToI32Op *>(&op) != nullptr ||
               dynamic_cast<const yir::TruncI32ToI1Op *>(&op) != nullptr ||
               dynamic_cast<const yir::SIToFPOp *>(&op) != nullptr ||
               dynamic_cast<const yir::FPToSIOp *>(&op) != nullptr ||
               dynamic_cast<const yir::ToBoolOp *>(&op) != nullptr ||
               dynamic_cast<const yir::NotOp *>(&op) != nullptr;
    }

    static bool fusion_setup_is_hoistable_before(const yir::Operation &op,
                                                  const yir::ForOp &loop) {
        if (auto *assign = dynamic_cast<const yir::AssignOp *>(&op)) {
            const auto *target = assign->target();
            if (target == nullptr ||
                dynamic_cast<const yir::VarOp *>(target->defining_op()) == nullptr ||
                value_depends_on_value(assign->value(), loop.induction_var())) {
                return false;
            }
            return target == loop.induction_var() ||
                   !region_uses_value(loop.body_region(), target);
        }
        if (!fusion_setup_is_hoistable(op)) {
            return false;
        }
        return std::none_of(op.operands().begin(), op.operands().end(), [&](const auto *operand) {
            return value_depends_on_value(operand, loop.induction_var());
        });
    }

    static bool fusion_region_is_simple(const yir::Region &region) {
        for (const auto &op : region.operations()) {
            if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
                if (!fusion_region_is_simple(for_op->body_region())) {
                    return false;
                }
                continue;
            }
            if (dynamic_cast<const yir::ConstI32Op *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ConstF32Op *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ConstBoolOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ZeroOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::VarOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::AssignOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ArrayLoadOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ArrayStoreOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::AddIOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::SubIOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::MulIOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::DivSIOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::RemSIOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::AddFOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::SubFOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::MulFOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::DivFOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ICmpOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::FCmpOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ZExtI1ToI32Op *>(op.get()) != nullptr ||
                dynamic_cast<const yir::TruncI32ToI1Op *>(op.get()) != nullptr ||
                dynamic_cast<const yir::SIToFPOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::FPToSIOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ToBoolOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::NotOp *>(op.get()) != nullptr) {
                continue;
            }
            return false;
        }
        return true;
    }

    static bool fusion_region_contains_loop(const yir::Region &region) {
        for (const auto &op : region.operations()) {
            if (dynamic_cast<const yir::ForOp *>(op.get()) != nullptr) {
                return true;
            }
        }
        return false;
    }

    static void collect_region_operations(
        const yir::Region &region,
        std::unordered_set<const yir::Operation *> &operations) {
        for (const auto &op : region.operations()) {
            operations.insert(op.get());
            if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
                collect_region_operations(if_op->then_region(), operations);
                if (if_op->has_else()) {
                    collect_region_operations(if_op->else_region(), operations);
                }
            } else if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
                collect_region_operations(while_op->cond_region(), operations);
                collect_region_operations(while_op->body_region(), operations);
            } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
                collect_region_operations(for_op->body_region(), operations);
            }
        }
    }

    const PolyScop *scop_for_fusion_loop(const yir::ForOp &loop) const {
        std::unordered_set<const yir::Operation *> body_operations;
        collect_region_operations(loop.body_region(), body_operations);
        for (const auto &scop : model_info_.models) {
            bool saw_statement = false;
            bool all_inside = true;
            for (const auto &stmt : scop.statements) {
                if (stmt.op != nullptr && body_operations.count(stmt.op) != 0) {
                    saw_statement = true;
                } else {
                    all_inside = false;
                }
            }
            if (saw_statement && all_inside) {
                return &scop;
            }
        }
        return nullptr;
    }

    static bool fusion_bounds_are_equal(const yir::ForOp &first,
                                        const yir::ForOp &second) {
        return poly_affine_expr_equal(affine_expr_from_value(first.lower_bound()),
                                      affine_expr_from_value(second.lower_bound())) &&
               poly_affine_expr_equal(affine_expr_from_value(first.upper_bound()),
                                      affine_expr_from_value(second.upper_bound())) &&
               poly_affine_expr_equal(affine_expr_from_value(first.step()),
                                      affine_expr_from_value(second.step()));
    }

    bool fusion_results_are_local(const yir::Region &region) const {
        for (const auto &op : region.operations()) {
            if (op->result() == nullptr) {
                continue;
            }
            if (count_value_uses_in_module(module_, op->result()) !=
                count_value_uses_in_region(region, op->result())) {
                return false;
            }
        }
        return true;
    }

    static void collect_fusion_memory_accesses(
        const PolyScop &scop,
        const std::unordered_set<const yir::Operation *> &body_operations,
        std::unordered_map<const yir::Value *, std::pair<bool, bool>> &accesses) {
        for (const auto &stmt : scop.statements) {
            if (stmt.op == nullptr || body_operations.count(stmt.op) == 0) {
                continue;
            }
            for (const auto &read : stmt.reads) {
                accesses[read.memory].first = true;
            }
            for (const auto &write : stmt.writes) {
                accesses[write.memory].second = true;
            }
        }
    }

    static bool fusion_memory_is_independent(const PolyScop &first_scop,
                                             const yir::ForOp &first_loop,
                                             const PolyScop &second_scop,
                                             const yir::ForOp &second_loop) {
        std::unordered_set<const yir::Operation *> first_operations;
        std::unordered_set<const yir::Operation *> second_operations;
        collect_region_operations(first_loop.body_region(), first_operations);
        collect_region_operations(second_loop.body_region(), second_operations);

        std::unordered_map<const yir::Value *, std::pair<bool, bool>> first_accesses;
        std::unordered_map<const yir::Value *, std::pair<bool, bool>> second_accesses;
        collect_fusion_memory_accesses(first_scop, first_operations, first_accesses);
        collect_fusion_memory_accesses(second_scop, second_operations, second_accesses);

        for (const auto &[memory, first_flags] : first_accesses) {
            const auto second = second_accesses.find(memory);
            if (second == second_accesses.end()) {
                continue;
            }
            if (first_flags.second || second->second.second) {
                return false;
            }
        }
        return true;
    }

    static bool fusion_affine_expr_equal_by_dims(
        const PolyAffineExpr &lhs, const PolyAffineExpr &rhs,
        const std::vector<const yir::Value *> &lhs_dims,
        const std::vector<const yir::Value *> &rhs_dims) {
        if (!lhs.is_linear() || !rhs.is_linear() || lhs.constant != rhs.constant ||
            lhs_dims.size() != rhs_dims.size()) {
            return false;
        }

        std::vector<std::int64_t> lhs_coefficients(lhs_dims.size(), 0);
        std::vector<std::int64_t> rhs_coefficients(rhs_dims.size(), 0);
        std::vector<std::pair<const yir::Value *, std::int64_t>> lhs_symbols;
        std::vector<std::pair<const yir::Value *, std::int64_t>> rhs_symbols;
        for (const auto &[value, coefficient] : lhs.terms) {
            const auto found = std::find(lhs_dims.begin(), lhs_dims.end(), value);
            if (found != lhs_dims.end()) {
                lhs_coefficients[static_cast<std::size_t>(found - lhs_dims.begin())] +=
                    coefficient;
            } else {
                lhs_symbols.push_back({value, coefficient});
            }
        }
        for (const auto &[value, coefficient] : rhs.terms) {
            const auto found = std::find(rhs_dims.begin(), rhs_dims.end(), value);
            if (found != rhs_dims.end()) {
                rhs_coefficients[static_cast<std::size_t>(found - rhs_dims.begin())] +=
                    coefficient;
            } else {
                rhs_symbols.push_back({value, coefficient});
            }
        }
        return lhs_coefficients == rhs_coefficients && lhs_symbols == rhs_symbols;
    }

    static bool fusion_accesses_match(
        const PolyAccess &first, const PolyAccess &second,
        const std::vector<const yir::Value *> &first_dims,
        const std::vector<const yir::Value *> &second_dims) {
        if (first.memory != second.memory || first.indices.size() != second.indices.size()) {
            return false;
        }
        for (std::size_t index = 0; index < first.indices.size(); ++index) {
            if (!fusion_affine_expr_equal_by_dims(first.indices[index], second.indices[index],
                                                  first_dims, second_dims)) {
                return false;
            }
        }
        return true;
    }

    static void collect_fusion_accesses_by_kind(
        const PolyScop &scop, const std::unordered_set<const yir::Operation *> &body_operations,
        std::unordered_map<const yir::Value *, std::vector<const PolyAccess *>> &reads,
        std::unordered_map<const yir::Value *, std::vector<const PolyAccess *>> &writes) {
        for (const auto &stmt : scop.statements) {
            if (stmt.op == nullptr || body_operations.count(stmt.op) == 0) {
                continue;
            }
            for (const auto &read : stmt.reads) {
                reads[read.memory].push_back(&read);
            }
            for (const auto &write : stmt.writes) {
                writes[write.memory].push_back(&write);
            }
        }
    }

    static bool fusion_memory_is_forward_producer_consumer(
        const PolyScop &first_scop, const yir::ForOp &first_loop,
        const PolyScop &second_scop, const yir::ForOp &second_loop,
        std::int64_t *forwarded_loads = nullptr) {
        if (forwarded_loads != nullptr) {
            *forwarded_loads = 0;
        }
        std::unordered_set<const yir::Operation *> first_operations;
        std::unordered_set<const yir::Operation *> second_operations;
        collect_region_operations(first_loop.body_region(), first_operations);
        collect_region_operations(second_loop.body_region(), second_operations);

        std::unordered_map<const yir::Value *, std::vector<const PolyAccess *>> first_reads;
        std::unordered_map<const yir::Value *, std::vector<const PolyAccess *>> first_writes;
        std::unordered_map<const yir::Value *, std::vector<const PolyAccess *>> second_reads;
        std::unordered_map<const yir::Value *, std::vector<const PolyAccess *>> second_writes;
        collect_fusion_accesses_by_kind(first_scop, first_operations, first_reads, first_writes);
        collect_fusion_accesses_by_kind(second_scop, second_operations, second_reads,
                                        second_writes);

        std::vector<const yir::Value *> first_dims;
        std::vector<const yir::Value *> second_dims;
        for (const auto &stmt : first_scop.statements) {
            if (!stmt.schedule_dims.empty()) {
                first_dims = stmt.schedule_dims;
                break;
            }
        }
        for (const auto &stmt : second_scop.statements) {
            if (!stmt.schedule_dims.empty()) {
                second_dims = stmt.schedule_dims;
                break;
            }
        }
        if (first_dims.size() != second_dims.size() || first_dims.empty()) {
            return false;
        }

        for (const auto &[memory, _] : first_reads) {
            if (second_writes.find(memory) != second_writes.end()) {
                return false;
            }
        }

        bool saw_forward_memory = false;
        for (const auto &[memory, first_flags] : first_writes) {
            const auto second_read = second_reads.find(memory);
            const auto second_write = second_writes.find(memory);
            const auto first_read = first_reads.find(memory);
            if (first_read != first_reads.end()) {
                return false;
            }
            if (second_read == second_reads.end()) {
                if (second_write != second_writes.end()) {
                    return false;
                }
                continue;
            }
            if (first_flags.empty() || second_read->second.empty()) {
                return false;
            }
            const auto *canonical_write = first_flags.front();
            if (canonical_write == nullptr ||
                !std::all_of(first_flags.begin(), first_flags.end(),
                             [&](const auto *write) {
                                 return write != nullptr && fusion_accesses_match(
                                     *canonical_write, *write, first_dims, first_dims);
                             })) {
                return false;
            }
            for (const auto *read : second_read->second) {
                if (!fusion_accesses_match(*canonical_write, *read, first_dims,
                                           second_dims)) {
                    return false;
                }
                if (forwarded_loads != nullptr) {
                    ++*forwarded_loads;
                }
            }
            if (second_write != second_writes.end()) {
                for (const auto *write : second_write->second) {
                    if (!fusion_accesses_match(*canonical_write, *write, first_dims,
                                               second_dims)) {
                        return false;
                    }
                }
            }
            saw_forward_memory = true;
        }

        // A producer-consumer pair must actually share a forward edge.  A
        // pair with no shared memory is handled by the independent case.
        return saw_forward_memory;
    }

    bool fusion_pair_matches(const yir::ForOp &first_loop,
                             const yir::ForOp &second_loop,
                             const PolyScop &first_scop,
                             const PolyScop &second_scop) const {
        if (first_scop.id == second_scop.id ||
            first_loop.is_parallel() != second_loop.is_parallel() ||
            !fusion_bounds_are_equal(first_loop, second_loop) ||
            !fusion_region_is_simple(first_loop.body_region()) ||
            !fusion_region_is_simple(second_loop.body_region()) ||
            !poly_band_scalar_state_is_iteration_local(first_loop.body_region()) ||
            !poly_band_scalar_state_is_iteration_local(second_loop.body_region()) ||
            !fusion_results_are_local(first_loop.body_region()) ||
            !fusion_results_are_local(second_loop.body_region()) ||
            !poly_band_has_complete_memory_model(first_scop, first_loop.body_region()) ||
            !poly_band_has_complete_memory_model(second_scop, second_loop.body_region()) ||
            (!fusion_memory_is_independent(first_scop, first_loop, second_scop, second_loop) &&
             !fusion_memory_is_forward_producer_consumer(first_scop, first_loop,
                                                         second_scop, second_loop))) {
            return false;
        }

        const auto *first_iv = first_loop.induction_var();
        const auto *second_iv = second_loop.induction_var();
        if (first_iv == nullptr || second_iv == nullptr) {
            return false;
        }

        // Frontend canonicalization may reuse one source-level loop variable
        // for consecutive equal-domain loops.  In that case no IV remapping or
        // escape check is needed: both bodies already use the fused IV.
        if (first_iv == second_iv) {
            return true;
        }
        if (region_uses_value(first_loop.body_region(), second_iv) ||
            region_uses_value(second_loop.body_region(), first_iv)) {
            return false;
        }

        const auto body_uses = count_value_uses_in_region(second_loop.body_region(), second_iv);
        return count_value_uses_in_module(module_, second_iv) == body_uses + 1;
    }

    bool find_poly_loop_fusion_candidate_in_region(
        yir::Region &region, PolyLoopFusionCandidate &candidate) const {
        auto &ops = region.operations();
        for (std::size_t first_index = 0; first_index < ops.size(); ++first_index) {
            auto *first_loop = dynamic_cast<yir::ForOp *>(ops[first_index].get());
            if (first_loop == nullptr) {
                continue;
            }
            for (std::size_t second_index = first_index + 1;
                 second_index < ops.size(); ++second_index) {
                auto *second_loop = dynamic_cast<yir::ForOp *>(ops[second_index].get());
                if (second_loop == nullptr) {
                    if (!fusion_setup_is_hoistable_before(*ops[second_index], *first_loop)) {
                        break;
                    }
                    continue;
                }

                const auto *first_scop = scop_for_fusion_loop(*first_loop);
                const auto *second_scop = scop_for_fusion_loop(*second_loop);
                if (first_scop != nullptr && second_scop != nullptr &&
                    fusion_pair_matches(*first_loop, *second_loop,
                                        *first_scop, *second_scop)) {
                    candidate = {&region, first_loop, second_loop, first_index,
                                 second_index, first_scop->id, second_scop->id};
                    return true;
                }
                break;
            }
        }

        for (auto &op : ops) {
            if (auto *for_op = dynamic_cast<yir::ForOp *>(op.get())) {
                if (find_poly_loop_fusion_candidate_in_region(for_op->body_region(), candidate)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool find_poly_loop_fusion_candidate(PolyLoopFusionCandidate &candidate) const {
        for (auto &function : module_.functions()) {
            if (find_poly_loop_fusion_candidate_in_region(function->body(), candidate)) {
                return true;
            }
        }
        return false;
    }

    const PolyScop *find_scop(std::size_t id) const {
        for (const auto &scop : model_info_.models) {
            if (scop.id == id) {
                return &scop;
            }
        }
        return nullptr;
    }

    bool poly_loop_fusion_is_legal(const PolyLoopFusionCandidate &candidate) const {
        const auto *first_scop = find_scop(candidate.first_scope_id);
        const auto *second_scop = find_scop(candidate.second_scope_id);
        if (candidate.parent == nullptr || candidate.first_loop == nullptr ||
            candidate.second_loop == nullptr || first_scop == nullptr ||
            second_scop == nullptr || candidate.second_index <= candidate.first_index ||
            candidate.second_index >= candidate.parent->operations().size()) {
            return false;
        }
        for (std::size_t i = candidate.first_index + 1; i < candidate.second_index; ++i) {
            if (!fusion_setup_is_hoistable_before(*candidate.parent->operations()[i],
                                                  *candidate.first_loop)) {
                return false;
            }
        }
        return fusion_pair_matches(*candidate.first_loop, *candidate.second_loop,
                                   *first_scop, *second_scop);
    }

    bool poly_loop_fusion_is_profitable(const PolyLoopFusionCandidate &candidate) {
        std::int64_t first_ops = 0;
        std::int64_t first_memory = 0;
        std::int64_t first_branches = 0;
        std::int64_t first_locals = 0;
        bool first_nested = false;
        estimate_region_shape(candidate.first_loop->body_region(), first_ops, first_memory,
                              first_branches, first_locals, first_nested);

        std::int64_t second_ops = 0;
        std::int64_t second_memory = 0;
        std::int64_t second_branches = 0;
        std::int64_t second_locals = 0;
        bool second_nested = false;
        estimate_region_shape(candidate.second_loop->body_region(), second_ops, second_memory,
                              second_branches, second_locals, second_nested);

        cost_model::TransformCandidate model_candidate;
        model_candidate.kind = cost_model::TransformKind::LoopFusion;
        model_candidate.stage = cost_model::CostIRStage::YIR;
        model_candidate.pass_name = "YIRPolyhedralTransformPass";
        model_candidate.candidate_id = "band-fusion";
        model_candidate.scope = std::to_string(candidate.first_scope_id) + ":" +
                                std::to_string(candidate.second_scope_id);
        model_candidate.frequency.source = cost_model::FrequencySource::StructuredYIRLoop;
        model_candidate.frequency.scale = 4;
        model_candidate.frequency.confidence = 0.86;
        model_candidate.proof.kind = cost_model::ProofKind::Dependence;
        model_candidate.proof.status = cost_model::ProofStatus::Proven;
        const auto *first_scop = find_scop(candidate.first_scope_id);
        const auto *second_scop = find_scop(candidate.second_scope_id);
        const bool independent =
            first_scop != nullptr && second_scop != nullptr &&
            fusion_memory_is_independent(*first_scop, *candidate.first_loop, *second_scop,
                                         *candidate.second_loop);
        std::int64_t forwarded_loads = 0;
        if (!independent && first_scop != nullptr && second_scop != nullptr) {
            (void)fusion_memory_is_forward_producer_consumer(
                *first_scop, *candidate.first_loop, *second_scop, *candidate.second_loop,
                &forwarded_loads);
        }
        const bool pointwise =
            !fusion_region_contains_loop(candidate.first_loop->body_region()) &&
            !fusion_region_contains_loop(candidate.second_loop->body_region());
        if (!pointwise) {
            // Outer-band fusion improves temporal locality but leaves the
            // producer and consumer point loops separate.  Only the eventual
            // innermost fusion may claim direct load forwarding.
            forwarded_loads = 0;
        }
        model_candidate.proof.summary =
            independent ? "equal domains and independent writable memories"
                        : "equal domains and same-iteration producer-consumer dependence";
        if (!pointwise) {
            model_candidate.proof.summary += "; outer band preserves point order";
        }

        auto &before = model_candidate.before;
        before.static_instrs = std::max<std::int64_t>(1, first_ops + second_ops + 2);
        before.loads = first_memory + second_memory;
        before.branches = first_branches + second_branches + 2;
        before.jumps = 2;
        before.estimated_cycles =
            (first_ops + second_ops + (first_memory + second_memory) * 4 +
             (first_branches + second_branches + 2) * 3 + 2) *
            model_candidate.frequency.scale;

        auto &after = model_candidate.after;
        after = before;
        after.loads = std::max<std::int64_t>(0, before.loads - forwarded_loads);
        after.static_instrs = std::max<std::int64_t>(1, before.static_instrs - 1);
        after.branches = std::max<std::int64_t>(0, before.branches - 1);
        after.jumps = std::max<std::int64_t>(0, before.jumps - 1);
        after.estimated_cycles = std::max<std::int64_t>(
            1, before.estimated_cycles -
                   (4 + forwarded_loads * 4) * model_candidate.frequency.scale);
        model_candidate.risk.live_range_growth =
            first_locals + second_locals > 4 ? 1 : 0;
        model_candidate.risk.register_pressure_growth =
            first_locals + second_locals > 6 ? 1 : 0;

        const auto decision = cost_model::decide(
            model_candidate,
            active_cost_policy(), active_target_profile());
        if (cost_report_ != nullptr) {
            cost_report_->decisions.push_back(decision);
        }
        return decision.profitable;
    }

    static bool apply_poly_loop_fusion(const PolyLoopFusionCandidate &candidate) {
        if (candidate.parent == nullptr || candidate.first_loop == nullptr ||
            candidate.second_loop == nullptr) {
            return false;
        }
        auto &ops = candidate.parent->operations();
        if (candidate.second_index >= ops.size() ||
            ops[candidate.first_index].get() != candidate.first_loop ||
            ops[candidate.second_index].get() != candidate.second_loop) {
            return false;
        }

        std::rotate(ops.begin() + static_cast<std::ptrdiff_t>(candidate.first_index),
                    ops.begin() + static_cast<std::ptrdiff_t>(candidate.first_index + 1),
                    ops.begin() + static_cast<std::ptrdiff_t>(candidate.second_index));
        const std::size_t first_index = candidate.second_index - 1;
        const std::size_t second_index = candidate.second_index;
        if (ops[first_index].get() != candidate.first_loop ||
            ops[second_index].get() != candidate.second_loop) {
            return false;
        }

        replace_value_in_region(candidate.second_loop->body_region(),
                                candidate.second_loop->induction_var(),
                                candidate.first_loop->induction_var());
        auto &first_body = candidate.first_loop->body_region();
        auto &second_body_ops = candidate.second_loop->body_region().operations();
        first_body.operations().reserve(first_body.operations().size() +
                                        second_body_ops.size());
        for (auto &op : second_body_ops) {
            op->set_parent(&first_body);
            first_body.operations().push_back(std::move(op));
        }
        second_body_ops.clear();
        ops.erase(ops.begin() + static_cast<std::ptrdiff_t>(second_index));
        return true;
    }

    bool try_interchange_or_tile(PolyScop& scop) {
        (void)canonical_info_;
        bool changed = false;
        const std::size_t dimensions = scop.schedule_space_dims.size();
        const std::size_t max_interchanges =
            dimensions > 1 ? dimensions * (dimensions - 1) / 2 : 0;
        for (std::size_t iteration = 0; iteration < max_interchanges; ++iteration) {
            auto interchange_owners = collect_enclosing_for_region_owners(module_);
            PolyBandInterchangeCandidate interchange;
            if (!find_poly_band_interchange_candidate(scop, interchange_owners,
                                                      interchange) ||
                !poly_band_interchange_is_legal(scop, interchange) ||
                !poly_band_interchange_backend_is_profitable(scop, interchange) ||
                !apply_poly_band_interchange(interchange)) {
                break;
            }
            update_scop_after_band_permutation(scop, interchange.depth);
            update_dependences_after_band_permutation(scop, interchange.depth);
            ++num_interchanged_;
            changed = true;
        }

        auto for_body_owners = collect_enclosing_for_region_owners(module_);
        struct ProfitableTile {
            PolyBandTileCandidate candidate;
            int tile_size = 0;
        };
        std::vector<ProfitableTile> profitable_tiles;
        for (std::size_t depth = 0; depth + 1 < dimensions; ++depth) {
            PolyBandTileCandidate candidate;
            if (!find_poly_band_tile_candidate(scop, for_body_owners, candidate, depth) ||
                !poly_band_tile_is_legal(scop, candidate) ||
                !poly_band_tile_is_profitable(scop, candidate)) {
                continue;
            }
            const int tile_size = tile_size_for_candidate(candidate);
            if (tile_size > 0) {
                profitable_tiles.push_back({candidate, tile_size});
            }
        }

        // Analyze the complete logical band before changing its physical nest.
        // Point loops keep their identity while tile loops are inserted, so
        // accepted adjacent pairs can then be lowered from outer to inner.
        for (auto &tile : profitable_tiles) {
            if (!apply_poly_band_tiling(tile.candidate)) {
                continue;
            }
            mark_scop_bands_tiled(scop, tile.candidate.depth, tile.tile_size,
                                  tile.candidate.inner_only);
            ++num_tiled_;
            changed = true;
        }
        return changed;
    }

    bool find_poly_band_interchange_candidate(
        const PolyScop &scop,
        const std::unordered_map<const yir::Region *, yir::ForOp *> &for_body_owners,
        PolyBandInterchangeCandidate &candidate) const {
        const PolyStmt *anchor = nullptr;
        for (const auto &stmt : scop.statements) {
            if (stmt.op != nullptr && stmt.schedule_dims.size() >= 2 &&
                stmt.loop_bounds.size() >= 2 &&
                (!stmt.reads.empty() || !stmt.writes.empty())) {
                anchor = &stmt;
                break;
            }
        }
        if (anchor == nullptr || anchor->op->parent() == nullptr) {
            return false;
        }

        auto anchor_loops = collect_enclosing_loop_nest(
            const_cast<yir::Operation *>(anchor->op)->parent(), for_body_owners);
        bool found = false;
        int best_score = 0;
        for (std::size_t depth = 0; depth + 1 < anchor_loops.outer_to_inner.size(); ++depth) {
            auto *outer_loop = anchor_loops.outer_to_inner[depth];
            auto *inner_loop = anchor_loops.outer_to_inner[depth + 1];
            if (outer_loop == nullptr || inner_loop == nullptr ||
                inner_loop->parent() != &outer_loop->body_region() ||
                anchor->schedule_dims.size() <= depth + 1 ||
                anchor->schedule_dims[depth] != outer_loop->induction_var() ||
                anchor->schedule_dims[depth + 1] != inner_loop->induction_var() ||
                !schedule_band_is_identity(*anchor, depth, outer_loop->induction_var()) ||
                !schedule_band_is_identity(*anchor, depth + 1, inner_loop->induction_var()) ||
                !loop_has_unit_step(*outer_loop) || !loop_has_unit_step(*inner_loop)) {
                continue;
            }

            std::size_t inner_index = 0;
            if (!find_operation_index(outer_loop->body_region(), *inner_loop, inner_index) ||
                inner_index + 1 != outer_loop->body_region().operations().size() ||
                inner_index > 1 ||
                poly_band_region_has_unsupported_control(inner_loop->body_region()) ||
                region_contains_call(inner_loop->body_region()) ||
                !poly_band_scalar_state_is_iteration_local(inner_loop->body_region())) {
                continue;
            }

            yir::AssignOp *redundant_inner_reset = nullptr;
            if (inner_index == 1) {
                redundant_inner_reset = dynamic_cast<yir::AssignOp *>(
                    outer_loop->body_region().operations().front().get());
                std::int64_t reset_value = 0;
                std::int64_t lower_value = 0;
                if (redundant_inner_reset == nullptr ||
                    redundant_inner_reset->target() != inner_loop->induction_var() ||
                    (redundant_inner_reset->value() != inner_loop->lower_bound() &&
                     (!const_i32_value(redundant_inner_reset->value(), reset_value) ||
                      !const_i32_value(inner_loop->lower_bound(), lower_value) ||
                      reset_value != lower_value))) {
                    continue;
                }
            }

            const auto *outer_iv = outer_loop->induction_var();
            const auto *inner_iv = inner_loop->induction_var();
            bool compatible = true;
            for (const auto &stmt : scop.statements) {
                if (stmt.op == nullptr || stmt.op->parent() == nullptr ||
                    stmt.schedule_dims.size() <= depth + 1 ||
                    stmt.loop_bounds.size() <= depth + 1 ||
                    stmt.schedule_dims[depth] != outer_iv ||
                    stmt.schedule_dims[depth + 1] != inner_iv ||
                    !schedule_band_is_identity(stmt, depth, outer_iv) ||
                    !schedule_band_is_identity(stmt, depth + 1, inner_iv) ||
                    stmt.loop_bounds[depth].iv != outer_iv ||
                    stmt.loop_bounds[depth + 1].iv != inner_iv) {
                    compatible = false;
                    break;
                }
                auto stmt_loops = collect_enclosing_loop_nest(
                    const_cast<yir::Operation *>(stmt.op)->parent(), for_body_owners);
                if (stmt_loops.outer_to_inner.size() <= depth + 1 ||
                    stmt_loops.outer_to_inner[depth] != outer_loop ||
                    stmt_loops.outer_to_inner[depth + 1] != inner_loop) {
                    compatible = false;
                    break;
                }
            }
            if (!compatible || !poly_band_has_complete_memory_model(scop, outer_loop->body_region()) ||
                !poly_band_has_rectangular_bounds(*anchor, outer_iv, inner_iv, depth)) {
                continue;
            }

            PolyBandInterchangeCandidate current{
                outer_loop, inner_loop, outer_iv, inner_iv, redundant_inner_reset, depth};
            if (!poly_band_interchange_is_legal(scop, current)) {
                continue;
            }
            const int score = poly_band_interchange_schedule_score(scop, current);
            if (score <= 0 ||
                (found && (score < best_score ||
                           (score == best_score && depth <= candidate.depth)))) {
                continue;
            }
            candidate = current;
            best_score = score;
            found = true;
        }
        return found;
    }

    static bool affine_expr_uses_dim(const PolyAffineExpr &expr,
                                     const yir::Value *dim) {
        if (!expr.is_linear() || dim == nullptr) {
            return true;
        }
        return std::any_of(expr.terms.begin(), expr.terms.end(), [dim](const auto &term) {
            return term.first == dim && term.second != 0;
        });
    }

    static bool poly_band_has_rectangular_bounds(const PolyStmt &stmt,
                                                 const yir::Value *outer_iv,
                                                 const yir::Value *inner_iv,
                                                 std::size_t depth = 0) {
        if (stmt.loop_bounds.size() <= depth + 1 ||
            stmt.loop_bounds[depth].iv != outer_iv ||
            stmt.loop_bounds[depth + 1].iv != inner_iv) {
            return false;
        }
        const auto &outer = stmt.loop_bounds[depth];
        const auto &inner = stmt.loop_bounds[depth + 1];
        return !affine_expr_uses_dim(outer.lower, inner_iv) &&
               !affine_expr_uses_dim(outer.upper, inner_iv) &&
               !affine_expr_uses_dim(inner.lower, outer_iv) &&
               !affine_expr_uses_dim(inner.upper, outer_iv);
    }

    static void collect_poly_band_local_vars(
        const yir::Region &region, std::unordered_set<const yir::Value *> &locals) {
        for (const auto &op : region.operations()) {
            if (auto *var = dynamic_cast<const yir::VarOp *>(op.get())) {
                locals.insert(var->result());
            }
            if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
                collect_poly_band_local_vars(if_op->then_region(), locals);
                if (if_op->has_else()) {
                    collect_poly_band_local_vars(if_op->else_region(), locals);
                }
            } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
                locals.insert(for_op->induction_var());
                collect_poly_band_local_vars(for_op->body_region(), locals);
            }
        }
    }

    static bool poly_band_assignments_target_locals(
        const yir::Region &region,
        const std::unordered_set<const yir::Value *> &locals) {
        for (const auto &op : region.operations()) {
            if (auto *assign = dynamic_cast<const yir::AssignOp *>(op.get())) {
                if (locals.find(assign->target()) == locals.end()) {
                    return false;
                }
            }
            if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
                if (!poly_band_assignments_target_locals(if_op->then_region(), locals) ||
                    (if_op->has_else() &&
                     !poly_band_assignments_target_locals(if_op->else_region(), locals))) {
                    return false;
                }
            } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
                if (!poly_band_assignments_target_locals(for_op->body_region(), locals)) {
                    return false;
                }
            }
        }
        return true;
    }

    static bool poly_band_scalar_state_is_iteration_local(const yir::Region &region) {
        std::unordered_set<const yir::Value *> locals;
        collect_poly_band_local_vars(region, locals);
        return poly_band_assignments_target_locals(region, locals);
    }

    static bool poly_band_region_memory_is_modeled(
        const yir::Region &region,
        const std::unordered_set<const yir::Operation *> &modeled) {
        for (const auto &op : region.operations()) {
            if ((dynamic_cast<const yir::ArrayLoadOp *>(op.get()) != nullptr ||
                 dynamic_cast<const yir::ArrayStoreOp *>(op.get()) != nullptr) &&
                modeled.find(op.get()) == modeled.end()) {
                return false;
            }
            if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
                if (!poly_band_region_memory_is_modeled(if_op->then_region(), modeled) ||
                    (if_op->has_else() &&
                     !poly_band_region_memory_is_modeled(if_op->else_region(), modeled))) {
                    return false;
                }
            } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
                if (!poly_band_region_memory_is_modeled(for_op->body_region(), modeled)) {
                    return false;
                }
            }
        }
        return true;
    }

    static bool poly_band_has_complete_memory_model(const PolyScop &scop,
                                                    const yir::Region &region) {
        std::unordered_set<const yir::Operation *> modeled;
        for (const auto &stmt : scop.statements) {
            if (stmt.op != nullptr) {
                modeled.insert(stmt.op);
            }
        }
        return poly_band_region_memory_is_modeled(region, modeled);
    }

    static bool direct_relation_distance(const PolyDependence &dep,
                                         const yir::Value *source_dim,
                                         const yir::Value *target_dim,
                                         std::int64_t &distance) {
        if (!dep.relation.exact || source_dim == nullptr || target_dim == nullptr ||
            dep.relation.constraints.disjuncts().empty()) {
            return false;
        }
        const auto source_it = std::find(dep.relation.source_dims.begin(),
                                         dep.relation.source_dims.end(), source_dim);
        const auto target_it = std::find(dep.relation.target_dims.begin(),
                                         dep.relation.target_dims.end(), target_dim);
        if (source_it == dep.relation.source_dims.end() ||
            target_it == dep.relation.target_dims.end()) {
            return false;
        }
        const std::size_t source_index =
            static_cast<std::size_t>(source_it - dep.relation.source_dims.begin());
        const std::size_t target_index = dep.relation.source_dims.size() +
            static_cast<std::size_t>(target_it - dep.relation.target_dims.begin());

        bool have_distance = false;
        std::int64_t common_distance = 0;
        for (const auto &piece : dep.relation.constraints.disjuncts()) {
            bool piece_has_distance = false;
            std::int64_t piece_distance = 0;
            for (const auto &constraint : piece.constraints()) {
                if (!constraint.equality || constraint.coefficients.size() <= target_index) {
                    continue;
                }
                bool only_pair = true;
                for (std::size_t i = 0; i < constraint.coefficients.size(); ++i) {
                    if (i != source_index && i != target_index &&
                        constraint.coefficients[i] != 0) {
                        only_pair = false;
                        break;
                    }
                }
                if (!only_pair) {
                    continue;
                }
                const auto source_coefficient = constraint.coefficients[source_index];
                const auto target_coefficient = constraint.coefficients[target_index];
                if (source_coefficient == 1 && target_coefficient == -1) {
                    piece_distance = constraint.constant;
                } else if (source_coefficient == -1 && target_coefficient == 1) {
                    piece_distance = -constraint.constant;
                } else {
                    continue;
                }
                piece_has_distance = true;
                break;
            }
            if (!piece_has_distance ||
                (have_distance && piece_distance != common_distance)) {
                return false;
            }
            common_distance = piece_distance;
            have_distance = true;
        }
        distance = common_distance;
        return have_distance;
    }

    static bool dependence_band_distance(const PolyDependence &dep,
                                         const PolyBandInterchangeCandidate &candidate,
                                         std::int64_t &outer_distance,
                                         std::int64_t &inner_distance) {
        if (dep.distance_kind == PolyDependence::DistanceKind::SameIteration) {
            outer_distance = 0;
            inner_distance = 0;
            return true;
        }
        if (dep.distance_kind == PolyDependence::DistanceKind::LoopCarriedConstant &&
            dep.distance.size() > candidate.depth + 1) {
            outer_distance = dep.distance[candidate.depth];
            inner_distance = dep.distance[candidate.depth + 1];
            return true;
        }
        return direct_relation_distance(dep, candidate.outer_iv, candidate.outer_iv,
                                        outer_distance) &&
               direct_relation_distance(dep, candidate.inner_iv, candidate.inner_iv,
                                        inner_distance);
    }

    // A schedule change must preserve the integer iteration lattice. Checking
    // unimodularity here rejects fractional/non-invertible candidates before
    // dependence legality or code generation can observe them. The matrices
    // considered by this pass are small, so an exact Bareiss-style determinant
    // over __int128 is sufficient and avoids a floating-point test.
    static bool schedule_matrix_is_unimodular(
        const std::vector<std::vector<std::int64_t>> &matrix) {
        const std::size_t dimension = matrix.size();
        if (dimension == 0) {
            return false;
        }
        for (const auto &row : matrix) {
            if (row.size() != dimension) {
                return false;
            }
        }
        if (dimension == 1) {
            return matrix.front().front() == 1 || matrix.front().front() == -1;
        }

        std::vector<std::vector<__int128>> work(
            dimension, std::vector<__int128>(dimension, 0));
        for (std::size_t row = 0; row < dimension; ++row) {
            for (std::size_t column = 0; column < dimension; ++column) {
                work[row][column] = matrix[row][column];
            }
        }

        __int128 previous_pivot = 1;
        int sign = 1;
        for (std::size_t pivot = 0; pivot + 1 < dimension; ++pivot) {
            std::size_t pivot_row = pivot;
            while (pivot_row < dimension && work[pivot_row][pivot] == 0) {
                ++pivot_row;
            }
            if (pivot_row == dimension) {
                return false;
            }
            if (pivot_row != pivot) {
                std::swap(work[pivot_row], work[pivot]);
                sign = -sign;
            }
            const __int128 pivot_value = work[pivot][pivot];
            for (std::size_t row = pivot + 1; row < dimension; ++row) {
                for (std::size_t column = pivot + 1; column < dimension; ++column) {
                    const __int128 numerator =
                        work[row][column] * pivot_value -
                        work[row][pivot] * work[pivot][column];
                    if (previous_pivot == 0 || numerator % previous_pivot != 0) {
                        return false;
                    }
                    work[row][column] = numerator / previous_pivot;
                }
            }
            previous_pivot = pivot_value;
        }

        const __int128 determinant = static_cast<__int128>(sign) * work.back().back();
        return determinant == 1 || determinant == -1;
    }

    enum class RelationScheduleLegality { Legal, Illegal, Unknown };

    // The local Presburger implementation intentionally has a bounded search
    // fallback.  Only use it as a proof when every relation variable has a
    // small explicit +/-1 bound; otherwise an empty search box would be an
    // unknown result, not proof that the transformed schedule is legal.
    static bool relation_piece_is_small_bounded(
        const yir::presburger::IntegerRelation &piece) {
        constexpr std::int64_t kMaxSpan = 64;
        const std::size_t variables = piece.num_vars();
        std::vector<std::int64_t> lower(variables, 0);
        std::vector<std::int64_t> upper(variables, 0);
        std::vector<bool> has_lower(variables, false);
        std::vector<bool> has_upper(variables, false);

        for (const auto &constraint : piece.constraints()) {
            if (constraint.coefficients.size() != variables) {
                return false;
            }
            std::size_t variable = 0;
            std::int64_t coefficient = 0;
            bool single_variable = true;
            for (std::size_t index = 0; index < variables; ++index) {
                if (constraint.coefficients[index] == 0) {
                    continue;
                }
                if (coefficient != 0) {
                    single_variable = false;
                    break;
                }
                variable = index;
                coefficient = constraint.coefficients[index];
            }
            if (!single_variable || coefficient == 0 ||
                (coefficient != 1 && coefficient != -1)) {
                continue;
            }

            if (constraint.equality) {
                if (coefficient == 1 &&
                    constraint.constant == std::numeric_limits<std::int64_t>::min()) {
                    return false;
                }
                const auto value = coefficient == 1 ? -constraint.constant
                                                    : constraint.constant;
                lower[variable] = has_lower[variable]
                                      ? std::max(lower[variable], value)
                                      : value;
                upper[variable] = has_upper[variable]
                                      ? std::min(upper[variable], value)
                                      : value;
                has_lower[variable] = true;
                has_upper[variable] = true;
                continue;
            }
            if (coefficient == 1) {
                if (constraint.constant == std::numeric_limits<std::int64_t>::min()) {
                    return false;
                }
                const auto bound = -constraint.constant;
                lower[variable] = has_lower[variable]
                                      ? std::max(lower[variable], bound)
                                      : bound;
                has_lower[variable] = true;
            } else {
                const auto bound = constraint.constant;
                upper[variable] = has_upper[variable]
                                      ? std::min(upper[variable], bound)
                                      : bound;
                has_upper[variable] = true;
            }
        }

        for (std::size_t variable = 0; variable < variables; ++variable) {
            if (!has_lower[variable] || !has_upper[variable] ||
                upper[variable] < lower[variable] ||
                upper[variable] - lower[variable] > kMaxSpan) {
                return false;
            }
        }
        return true;
    }

    static RelationScheduleLegality
    violation_relation_legality(const yir::presburger::IntegerRelation &relation) {
        constexpr std::size_t kMaxSearchNodes = 2'000'000;
        switch (relation.check_integer_feasibility(kMaxSearchNodes)) {
        case yir::presburger::IntegerFeasibility::Empty:
            return RelationScheduleLegality::Legal;
        case yir::presburger::IntegerFeasibility::NonEmpty:
            return RelationScheduleLegality::Illegal;
        case yir::presburger::IntegerFeasibility::Unknown:
            return RelationScheduleLegality::Unknown;
        }
        return RelationScheduleLegality::Unknown;
    }

    static bool schedule_difference_coefficients(
        const PolyScop &scop, const PolyDependence &dep,
        const PolyScheduleCandidate &candidate, std::size_t row,
        std::vector<std::int64_t> &difference) {
        const auto &relation = dep.relation;
        if (row >= candidate.band_matrix.size() ||
            candidate.band_matrix[row].size() != scop.schedule_space_dims.size()) {
            return false;
        }
        difference.assign(relation.source_dims.size() + relation.target_dims.size() +
                              relation.params.size(),
                          0);
        for (std::size_t column = 0; column < scop.schedule_space_dims.size(); ++column) {
            const auto coefficient = candidate.band_matrix[row][column];
            if (coefficient == 0) {
                continue;
            }
            const auto dimension = scop.schedule_space_dims[column];
            const auto source = std::find(relation.source_dims.begin(),
                                          relation.source_dims.end(), dimension);
            if (source != relation.source_dims.end()) {
                const auto index = static_cast<std::size_t>(source - relation.source_dims.begin());
                difference[index] += coefficient;
            }
            const auto target = std::find(relation.target_dims.begin(),
                                          relation.target_dims.end(), dimension);
            if (target != relation.target_dims.end()) {
                const auto index = relation.source_dims.size() +
                                   static_cast<std::size_t>(target - relation.target_dims.begin());
                difference[index] -= coefficient;
            }
        }
        return true;
    }

    RelationScheduleLegality relation_schedule_legality(
        const PolyScop &scop, const PolyDependence &dep,
        const PolyScheduleCandidate &candidate) const {
        if (!dep.relation.exact || dep.relation.constraints.disjuncts().empty() ||
            candidate.band_matrix.size() != scop.schedule_space_dims.size()) {
            return RelationScheduleLegality::Unknown;
        }

        bool checked_piece = false;
        for (const auto &base : dep.relation.constraints.disjuncts()) {
            if (!relation_piece_is_small_bounded(base)) {
                return RelationScheduleLegality::Unknown;
            }
            checked_piece = true;
            for (std::size_t first_difference = 0;
                 first_difference < candidate.band_matrix.size(); ++first_difference) {
                std::vector<std::int64_t> difference;
                if (!schedule_difference_coefficients(scop, dep, candidate,
                                                       first_difference, difference)) {
                    return RelationScheduleLegality::Unknown;
                }
                auto violating = base;
                for (std::size_t prefix = 0; prefix < first_difference; ++prefix) {
                    std::vector<std::int64_t> prefix_difference;
                    if (!schedule_difference_coefficients(scop, dep, candidate, prefix,
                                                           prefix_difference)) {
                        return RelationScheduleLegality::Unknown;
                    }
                    violating.add_equality(std::move(prefix_difference), 0);
                }
                // A dependence is illegal when source - target is >= 1 after
                // all earlier schedule rows are equal.
                violating.add_inequality(std::move(difference), -1);
                const auto violation = violation_relation_legality(violating);
                if (violation == RelationScheduleLegality::Illegal) {
                    return RelationScheduleLegality::Illegal;
                }
                if (violation == RelationScheduleLegality::Unknown) {
                    return RelationScheduleLegality::Unknown;
                }
            }

            const auto *source_stmt = find_stmt_by_id(scop, dep.source_stmt_id);
            const auto *target_stmt = find_stmt_by_id(scop, dep.target_stmt_id);
            if (source_stmt == nullptr || target_stmt == nullptr) {
                return RelationScheduleLegality::Unknown;
            }
            if (source_stmt->schedule.lexical_order >=
                target_stmt->schedule.lexical_order) {
                auto collapsed = base;
                for (std::size_t row = 0; row < candidate.band_matrix.size(); ++row) {
                    std::vector<std::int64_t> difference;
                    if (!schedule_difference_coefficients(scop, dep, candidate, row,
                                                           difference)) {
                        return RelationScheduleLegality::Unknown;
                    }
                    collapsed.add_equality(std::move(difference), 0);
                }
                const auto violation = violation_relation_legality(collapsed);
                if (violation == RelationScheduleLegality::Illegal) {
                    return RelationScheduleLegality::Illegal;
                }
                if (violation == RelationScheduleLegality::Unknown) {
                    return RelationScheduleLegality::Unknown;
                }
            }
        }
        return checked_piece ? RelationScheduleLegality::Legal
                             : RelationScheduleLegality::Unknown;
    }

    static bool relation_dimension_index(const std::vector<const yir::Value *> &dims,
                                         const yir::Value *value, std::size_t &index) {
        const auto found = std::find(dims.begin(), dims.end(), value);
        if (found == dims.end()) {
            return false;
        }
        index = static_cast<std::size_t>(found - dims.begin());
        return true;
    }

    static bool relation_single_variable_bounds(
        const yir::presburger::IntegerRelation &piece, std::size_t variable,
        std::int64_t &lower, std::int64_t &upper) {
        bool have_lower = false;
        bool have_upper = false;
        for (const auto &constraint : piece.constraints()) {
            if (constraint.coefficients.size() != piece.num_vars()) {
                return false;
            }
            bool single_variable = true;
            for (std::size_t index = 0; index < constraint.coefficients.size(); ++index) {
                if (index != variable && constraint.coefficients[index] != 0) {
                    single_variable = false;
                    break;
                }
            }
            if (!single_variable) {
                continue;
            }
            const auto coefficient = constraint.coefficients[variable];
            if (coefficient != 1 && coefficient != -1) {
                continue;
            }
            if (constraint.equality) {
                if (coefficient == 1 &&
                    constraint.constant == std::numeric_limits<std::int64_t>::min()) {
                    return false;
                }
                const auto value = coefficient == 1 ? -constraint.constant
                                                    : constraint.constant;
                lower = have_lower ? std::max(lower, value) : value;
                upper = have_upper ? std::min(upper, value) : value;
                have_lower = true;
                have_upper = true;
            } else if (coefficient == 1) {
                if (constraint.constant == std::numeric_limits<std::int64_t>::min()) {
                    return false;
                }
                const auto bound = -constraint.constant;
                lower = have_lower ? std::max(lower, bound) : bound;
                have_lower = true;
            } else {
                const auto bound = constraint.constant;
                upper = have_upper ? std::min(upper, bound) : bound;
                have_upper = true;
            }
        }
        return have_lower && have_upper && lower <= upper;
    }

    static bool floor_div_i128(__int128 numerator, __int128 denominator,
                               std::int64_t &result) {
        if (denominator <= 0) {
            return false;
        }
        __int128 quotient = numerator / denominator;
        if (numerator < 0 && numerator % denominator != 0) {
            --quotient;
        }
        if (quotient < std::numeric_limits<std::int64_t>::min() ||
            quotient > std::numeric_limits<std::int64_t>::max()) {
            return false;
        }
        result = static_cast<std::int64_t>(quotient);
        return true;
    }

    static bool add_floor_schedule_coordinate(
        yir::presburger::IntegerRelation &piece, std::size_t value_index,
        std::size_t tile_index, std::int64_t loop_lower, std::int64_t tile_size) {
        if (tile_size <= 0 || value_index >= piece.num_vars() ||
            tile_index >= piece.num_vars()) {
            return false;
        }
        std::vector<std::int64_t> lower_constraint(piece.num_vars(), 0);
        lower_constraint[value_index] = 1;
        lower_constraint[tile_index] = -tile_size;
        piece.add_inequality(std::move(lower_constraint), -loop_lower);

        std::vector<std::int64_t> upper_constraint(piece.num_vars(), 0);
        upper_constraint[value_index] = -1;
        upper_constraint[tile_index] = tile_size;
        const __int128 constant = static_cast<__int128>(loop_lower) + tile_size - 1;
        if (constant < std::numeric_limits<std::int64_t>::min() ||
            constant > std::numeric_limits<std::int64_t>::max()) {
            return false;
        }
        piece.add_inequality(std::move(upper_constraint), static_cast<std::int64_t>(constant));
        return true;
    }

    RelationScheduleLegality relation_tile_schedule_legality(
        const PolyScop &scop, const PolyDependence &dep,
        const PolyBandTileCandidate &candidate) const {
        if (!dep.relation.exact || dep.relation.constraints.disjuncts().empty() ||
            candidate.outer_loop == nullptr || candidate.inner_loop == nullptr ||
            candidate.depth + 1 >= scop.schedule_space_dims.size()) {
            return RelationScheduleLegality::Unknown;
        }
        const int tile_size = tile_size_for_candidate(candidate);
        if (tile_size <= 0) {
            return RelationScheduleLegality::Unknown;
        }
        std::int64_t outer_lower = 0;
        std::int64_t inner_lower = 0;
        if (!const_i32_value(candidate.outer_loop->lower_bound(), outer_lower) ||
            !const_i32_value(candidate.inner_loop->lower_bound(), inner_lower)) {
            return RelationScheduleLegality::Unknown;
        }

        const auto &relation = dep.relation;
        std::size_t source_outer = 0;
        std::size_t source_inner = 0;
        std::size_t target_outer = 0;
        std::size_t target_inner = 0;
        if (!relation_dimension_index(relation.source_dims, candidate.outer_iv, source_outer) ||
            !relation_dimension_index(relation.source_dims, candidate.inner_iv, source_inner) ||
            !relation_dimension_index(relation.target_dims, candidate.outer_iv, target_outer) ||
            !relation_dimension_index(relation.target_dims, candidate.inner_iv, target_inner)) {
            return RelationScheduleLegality::Unknown;
        }
        const std::size_t target_offset = relation.source_dims.size();

        bool checked_piece = false;
        for (const auto &base : relation.constraints.disjuncts()) {
            auto tiled = base;
            const std::size_t source_outer_q = tiled.num_vars();
            tiled.add_vars(yir::presburger::VarKind::Local, 4);
            const std::size_t source_inner_q = source_outer_q + 2;
            const std::size_t target_outer_q = source_outer_q + 1;
            const std::size_t target_inner_q = source_outer_q + 3;

            if (!add_floor_schedule_coordinate(tiled, source_outer, source_outer_q,
                                               outer_lower, tile_size) ||
                !add_floor_schedule_coordinate(tiled, target_offset + target_outer,
                                               target_outer_q, outer_lower, tile_size) ||
                !add_floor_schedule_coordinate(tiled, source_inner, source_inner_q,
                                               inner_lower, tile_size) ||
                !add_floor_schedule_coordinate(tiled, target_offset + target_inner,
                                               target_inner_q, inner_lower, tile_size)) {
                return RelationScheduleLegality::Unknown;
            }

            // The relation already carries explicit bounds for each point
            // variable. Add conservative tile-coordinate bounds derived from
            // those intervals before invoking the budgeted solver.
            const auto add_q_bounds = [&](std::size_t value_lower_index,
                                          std::size_t value_upper_index,
                                          std::size_t q_index,
                                          std::int64_t lower) {
                std::int64_t value_lower = 0;
                std::int64_t value_upper = 0;
                if (!relation_single_variable_bounds(base, value_lower_index,
                                                     value_lower, value_upper)) {
                    return false;
                }
                if (value_upper_index != value_lower_index &&
                    !relation_single_variable_bounds(base, value_upper_index,
                                                     value_lower, value_upper)) {
                    return false;
                }
                std::int64_t q_lower = 0;
                std::int64_t q_upper = 0;
                if (!floor_div_i128(static_cast<__int128>(value_lower) - lower,
                                    tile_size, q_lower) ||
                    !floor_div_i128(static_cast<__int128>(value_upper) - lower,
                                    tile_size, q_upper) || q_lower > q_upper) {
                    return false;
                }
                std::vector<std::int64_t> q_lower_constraint(tiled.num_vars(), 0);
                q_lower_constraint[q_index] = 1;
                if (q_lower == std::numeric_limits<std::int64_t>::min()) {
                    return false;
                }
                tiled.add_inequality(std::move(q_lower_constraint), -q_lower);
                std::vector<std::int64_t> q_upper_constraint(tiled.num_vars(), 0);
                q_upper_constraint[q_index] = -1;
                tiled.add_inequality(std::move(q_upper_constraint), q_upper);
                return true;
            };
            if (!add_q_bounds(source_outer, source_outer, source_outer_q, outer_lower) ||
                !add_q_bounds(target_offset + target_outer, target_offset + target_outer,
                              target_outer_q, outer_lower) ||
                !add_q_bounds(source_inner, source_inner, source_inner_q, inner_lower) ||
                !add_q_bounds(target_offset + target_inner, target_offset + target_inner,
                              target_inner_q, inner_lower)) {
                return RelationScheduleLegality::Unknown;
            }

            const auto point_difference = [&](std::size_t dimension) {
                std::vector<std::int64_t> difference(tiled.num_vars(), 0);
                difference[dimension] = 1;
                difference[target_offset + dimension] = -1;
                return difference;
            };
            const auto tile_difference = [&](std::size_t source_q,
                                             std::size_t target_q) {
                std::vector<std::int64_t> difference(tiled.num_vars(), 0);
                difference[source_q] = 1;
                difference[target_q] = -1;
                return difference;
            };

            std::vector<std::vector<std::int64_t>> schedule_rows;
            for (std::size_t dimension = 0; dimension < candidate.depth; ++dimension) {
                schedule_rows.push_back(point_difference(dimension));
            }
            schedule_rows.push_back(tile_difference(source_outer_q, target_outer_q));
            schedule_rows.push_back(tile_difference(source_inner_q, target_inner_q));
            schedule_rows.push_back(point_difference(candidate.depth));
            schedule_rows.push_back(point_difference(candidate.depth + 1));
            for (std::size_t dimension = candidate.depth + 2;
                 dimension < scop.schedule_space_dims.size(); ++dimension) {
                schedule_rows.push_back(point_difference(dimension));
            }

            checked_piece = true;
            if (!relation_piece_is_small_bounded(tiled)) {
                return RelationScheduleLegality::Unknown;
            }
            for (std::size_t first_difference = 0;
                 first_difference < schedule_rows.size(); ++first_difference) {
                auto violating = tiled;
                for (std::size_t prefix = 0; prefix < first_difference; ++prefix) {
                    violating.add_equality(schedule_rows[prefix], 0);
                }
                violating.add_inequality(schedule_rows[first_difference], -1);
                const auto violation = violation_relation_legality(violating);
                if (violation == RelationScheduleLegality::Illegal) {
                    return RelationScheduleLegality::Illegal;
                }
                if (violation == RelationScheduleLegality::Unknown) {
                    return RelationScheduleLegality::Unknown;
                }
            }

            const auto *source_stmt = find_stmt_by_id(scop, dep.source_stmt_id);
            const auto *target_stmt = find_stmt_by_id(scop, dep.target_stmt_id);
            if (source_stmt == nullptr || target_stmt == nullptr) {
                return RelationScheduleLegality::Unknown;
            }
            if (source_stmt->schedule.lexical_order >= target_stmt->schedule.lexical_order) {
                auto collapsed = tiled;
                for (const auto &row : schedule_rows) {
                    collapsed.add_equality(row, 0);
                }
                const auto violation = violation_relation_legality(collapsed);
                if (violation == RelationScheduleLegality::Illegal) {
                    return RelationScheduleLegality::Illegal;
                }
                if (violation == RelationScheduleLegality::Unknown) {
                    return RelationScheduleLegality::Unknown;
                }
            }
        }
        return checked_piece ? RelationScheduleLegality::Legal
                             : RelationScheduleLegality::Unknown;
    }

    void record_relation_schedule_rejection(
        const PolyScop &scop, const PolyBandInterchangeCandidate &candidate,
        const PolyDependence &dep) const {
        if (cost_report_ == nullptr) {
            return;
        }
        cost_model::TransformCandidate rejected;
        rejected.kind = cost_model::TransformKind::LoopInterchange;
        rejected.stage = cost_model::CostIRStage::YIR;
        rejected.pass_name = "YIRPolyhedralTransformPass";
        rejected.candidate_id = "band-interchange-relation";
        rejected.scope = std::to_string(scop.id) + ":" +
                         std::to_string(candidate.depth);
        rejected.frequency.confidence = 1.0;
        rejected.frequency.source = cost_model::FrequencySource::StructuredYIRLoop;
        rejected.proof.kind = cost_model::ProofKind::Dependence;
        rejected.proof.status = cost_model::ProofStatus::Refuted;
        rejected.proof.summary =
            "Presburger dependence relation contains a transformed lexicographic order violation";
        rejected.proof.solver_id = "bounded-presburger-exact-domain";
        rejected.proof.obligations = 1;
        rejected.reason_hints.push_back(
            "dependence " + std::to_string(dep.source_stmt_id) + " -> " +
            std::to_string(dep.target_stmt_id));
        cost_report_->decisions.push_back(cost_model::decide(
            rejected,
            active_cost_policy(), active_target_profile()));
    }

    static PolyScheduleCandidate make_interchange_schedule_candidate(
        const PolyScop &scop, const PolyBandInterchangeCandidate &candidate) {
        PolyScheduleCandidate result;
        const std::size_t dimensions = scop.schedule_space_dims.size();
        if (candidate.depth + 1 >= dimensions) {
            return result;
        }
        result.band_matrix.assign(dimensions, std::vector<std::int64_t>(dimensions, 0));
        for (std::size_t row = 0; row < dimensions; ++row) {
            result.band_matrix[row][row] = 1;
        }
        std::swap(result.band_matrix[candidate.depth][candidate.depth],
                  result.band_matrix[candidate.depth][candidate.depth + 1]);
        std::swap(result.band_matrix[candidate.depth + 1][candidate.depth],
                  result.band_matrix[candidate.depth + 1][candidate.depth + 1]);
        result.valid = schedule_matrix_is_unimodular(result.band_matrix);
        return result;
    }

    static PolyScheduleCandidate make_wavefront_schedule_candidate(
        const PolyScop &scop) {
        PolyScheduleCandidate result;
        if (scop.schedule_space_dims.size() != 3) {
            return result;
        }
        // w = i + j + k is the outer schedule dimension; the remaining rows
        // provide a deterministic intra-wave order for the serial prototype.
        result.band_matrix = {{1, 1, 1}, {1, 0, 0}, {0, 1, 0}};
        result.valid = schedule_matrix_is_unimodular(result.band_matrix);
        return result;
    }

    static bool lexicographically_positive_schedule_distance(
        const std::vector<std::int64_t> &distance) {
        for (const auto value : distance) {
            if (value > 0) {
                return true;
            }
            if (value < 0) {
                return false;
            }
        }
        return false;
    }

    static bool apply_schedule_matrix_to_distance(
        const PolyScheduleCandidate &candidate,
        const std::vector<std::int64_t> &distance,
        std::vector<std::int64_t> &transformed) {
        if (!candidate.valid || candidate.band_matrix.empty() ||
            distance.size() > candidate.band_matrix.size()) {
            return false;
        }
        const std::size_t dimensions = candidate.band_matrix.size();
        transformed.assign(dimensions, 0);
        for (std::size_t row = 0; row < dimensions; ++row) {
            if (candidate.band_matrix[row].size() != dimensions) {
                return false;
            }
            __int128 value = 0;
            for (std::size_t column = 0; column < distance.size(); ++column) {
                value += static_cast<__int128>(candidate.band_matrix[row][column]) *
                         static_cast<__int128>(distance[column]);
            }
            if (value > std::numeric_limits<std::int64_t>::max()) {
                transformed[row] = std::numeric_limits<std::int64_t>::max();
            } else if (value < std::numeric_limits<std::int64_t>::min()) {
                transformed[row] = std::numeric_limits<std::int64_t>::min();
            } else {
                transformed[row] = static_cast<std::int64_t>(value);
            }
        }
        return true;
    }

    bool poly_band_interchange_is_legal(
        const PolyScop &scop, const PolyBandInterchangeCandidate &candidate) const {
        const auto schedule_candidate =
            make_interchange_schedule_candidate(scop, candidate);
        if (!schedule_candidate.valid) {
            return false;
        }
        for (const auto &dep : dep_info_.dependences) {
            if (!dep.is_dependent || find_stmt_by_id(scop, dep.source_stmt_id) == nullptr ||
                find_stmt_by_id(scop, dep.target_stmt_id) == nullptr) {
                continue;
            }
            if (dep.is_reduction) {
                continue;
            }
            if (dep.distance_kind == PolyDependence::DistanceKind::LoopCarriedConstant) {
                std::vector<std::int64_t> transformed;
                if (!apply_schedule_matrix_to_distance(schedule_candidate, dep.distance,
                                                       transformed) ||
                    !lexicographically_positive_schedule_distance(transformed)) {
                    return false;
                }
                continue;
            }
            const auto relation_legality =
                relation_schedule_legality(scop, dep, schedule_candidate);
            if (relation_legality == RelationScheduleLegality::Illegal) {
                ++num_relation_legality_rejections_;
                record_relation_schedule_rejection(scop, candidate, dep);
                return false;
            }
            if (relation_legality == RelationScheduleLegality::Legal) {
                ++num_relation_legality_proofs_;
                continue;
            }
            ++num_relation_legality_unknown_;
            std::int64_t outer_distance = 0;
            std::int64_t inner_distance = 0;
            if (!dependence_band_distance(dep, candidate, outer_distance, inner_distance)) {
                return false;
            }
            if (inner_distance < 0 ||
                (inner_distance == 0 && outer_distance < 0)) {
                return false;
            }
        }
        return true;
    }

    bool wavefront_schedule_is_legal(const PolyScop &scop,
                                     const PolyScheduleCandidate &candidate) const {
        if (!candidate.valid) {
            return false;
        }
        const auto belongs_to_scop = [&scop](const PolyDependence &dep) {
            bool source_found = false;
            bool target_found = false;
            for (const auto &stmt : scop.statements) {
                source_found = source_found || stmt.id == dep.source_stmt_id;
                target_found = target_found || stmt.id == dep.target_stmt_id;
            }
            return source_found && target_found;
        };
        for (const auto &dep : dep_info_.dependences) {
            if (!dep.is_dependent || !belongs_to_scop(dep) || dep.is_reduction) {
                continue;
            }
            if (dep.distance_kind != PolyDependence::DistanceKind::LoopCarriedConstant) {
                // A same-wave or unknown dependence cannot be scheduled by the
                // serial wavefront prototype without an additional relation proof.
                return false;
            }
            std::vector<std::int64_t> transformed;
            if (!apply_schedule_matrix_to_distance(candidate, dep.distance, transformed) ||
                transformed.empty() || transformed.front() <= 0) {
                return false;
            }
        }
        return true;
    }

    static std::int64_t affine_dim_coefficient(const PolyAffineExpr &expr,
                                               const yir::Value *dim) {
        std::int64_t coefficient = 0;
        if (!expr.is_linear() || dim == nullptr) {
            return 0;
        }
        for (const auto &term : expr.terms) {
            if (term.first == dim) {
                coefficient += term.second;
            }
        }
        return coefficient;
    }

    static void add_poly_band_locality_score(const std::vector<PolyAccess> &accesses,
                                             int weight,
                                             const PolyBandInterchangeCandidate &candidate,
                                             int &outer_score, int &inner_score) {
        for (const auto &access : accesses) {
            if (!is_valid_affine_access(access) || access.indices.size() < 2) {
                continue;
            }
            const auto &last = access.indices.back();
            if (affine_dim_coefficient(last, candidate.outer_iv) != 0) {
                outer_score += weight;
            }
            if (affine_dim_coefficient(last, candidate.inner_iv) != 0) {
                inner_score += weight;
            }
        }
    }

    static int poly_band_interchange_profitability_score(
        const PolyScop &scop, const PolyBandInterchangeCandidate &candidate) {
        int outer_score = 0;
        int inner_score = 0;
        for (const auto &stmt : scop.statements) {
            add_poly_band_locality_score(stmt.reads, 1, candidate,
                                         outer_score, inner_score);
            add_poly_band_locality_score(stmt.writes, 2, candidate,
                                         outer_score, inner_score);
        }
        if (outer_score <= inner_score) {
            return 0;
        }

        const auto &bounds = scop.statements.front().loop_bounds;
        if (bounds.size() <= candidate.depth + 1) {
            return 0;
        }
        for (std::size_t dim = candidate.depth; dim <= candidate.depth + 1; ++dim) {
            std::int64_t lower = 0;
            std::int64_t upper = 0;
            if (affine_constant_value(bounds[dim].lower, lower) &&
                affine_constant_value(bounds[dim].upper, upper) && upper - lower < 16) {
                return 0;
            }
        }
        return outer_score - inner_score;
    }

    static std::int64_t nonnegative_distance(std::int64_t distance) {
        if (distance >= 0) {
            return distance;
        }
        if (distance == std::numeric_limits<std::int64_t>::min()) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return -distance;
    }

    int poly_band_interchange_proximity_score(
        const PolyScop &scop, const PolyBandInterchangeCandidate &candidate) const {
        const auto schedule_candidate =
            make_interchange_schedule_candidate(scop, candidate);
        if (!schedule_candidate.valid) {
            return 0;
        }
        int score = 0;
        for (const auto &dep : dep_info_.dependences) {
            if (!dep.is_dependent || find_stmt_by_id(scop, dep.source_stmt_id) == nullptr ||
                find_stmt_by_id(scop, dep.target_stmt_id) == nullptr ||
                dep.is_reduction ||
                dep.distance_kind != PolyDependence::DistanceKind::LoopCarriedConstant ||
                dep.distance.size() <= candidate.depth + 1) {
                continue;
            }

            std::vector<std::int64_t> transformed;
            if (!apply_schedule_matrix_to_distance(schedule_candidate, dep.distance,
                                                   transformed) ||
                transformed.size() <= candidate.depth + 1) {
                continue;
            }
            const auto before_inner = nonnegative_distance(dep.distance[candidate.depth + 1]);
            const auto after_inner = nonnegative_distance(transformed[candidate.depth + 1]);
            if (before_inner > after_inner) {
                const auto gain = std::min<std::int64_t>(before_inner - after_inner, 16);
                score += static_cast<int>(gain);
            }
        }
        return score;
    }

    int poly_band_interchange_schedule_score(
        const PolyScop &scop, const PolyBandInterchangeCandidate &candidate) const {
        const int locality = poly_band_interchange_profitability_score(scop, candidate);
        if (locality <= 0) {
            return locality;
        }
        return locality + poly_band_interchange_proximity_score(scop, candidate);
    }

    static bool apply_poly_band_interchange(
        const PolyBandInterchangeCandidate &candidate) {
        if (candidate.outer_loop == nullptr || candidate.inner_loop == nullptr ||
            candidate.inner_loop->parent() != &candidate.outer_loop->body_region()) {
            return false;
        }
        if (candidate.redundant_inner_reset != nullptr) {
            std::size_t reset_index = 0;
            if (!find_operation_index(candidate.outer_loop->body_region(),
                                      *candidate.redundant_inner_reset, reset_index)) {
                return false;
            }
            candidate.outer_loop->body_region().operations().erase(
                candidate.outer_loop->body_region().operations().begin() +
                static_cast<std::ptrdiff_t>(reset_index));
        }
        const bool outer_parallel = candidate.outer_loop->is_parallel();
        const bool inner_parallel = candidate.inner_loop->is_parallel();
        for (std::size_t operand = 0; operand < 4; ++operand) {
            std::swap(candidate.outer_loop->operands()[operand],
                      candidate.inner_loop->operands()[operand]);
        }
        candidate.outer_loop->set_parallel(inner_parallel);
        candidate.inner_loop->set_parallel(outer_parallel);
        return true;
    }

    // Keep the polyhedral model in the same coordinate system as the YIR nest
    // after an interchange.  The transform swaps loop operands (including the
    // induction variables), so both the statement-local dimensions and the
    // common schedule matrix must be permuted together.
    static void update_scop_after_band_permutation(PolyScop &scop,
                                                    std::size_t depth) {
        if (depth + 1 >= scop.schedule_space_dims.size()) {
            return;
        }
        std::swap(scop.schedule_space_dims[depth],
                  scop.schedule_space_dims[depth + 1]);

        for (auto &stmt : scop.statements) {
            auto swap_dimensions = [depth](auto &values) {
                if (depth + 1 < values.size()) {
                    const auto first = values[depth];
                    values[depth] = values[depth + 1];
                    values[depth + 1] = first;
                }
            };
            swap_dimensions(stmt.dims);
            swap_dimensions(stmt.schedule_dims);
            swap_dimensions(stmt.domain.dims);
            swap_dimensions(stmt.loop_bounds);
            swap_dimensions(stmt.reduction_dims);

            auto &schedule = stmt.schedule;
            swap_dimensions(schedule.input_dims);
            schedule.space_dims = scop.schedule_space_dims;

            const std::size_t first_row = 1 + depth * 2;
            const std::size_t second_row = first_row + 2;
            if (second_row < schedule.output_dims.size()) {
                std::swap(schedule.output_dims[first_row],
                          schedule.output_dims[second_row]);
            }
            if (second_row < schedule.matrix.size()) {
                std::swap(schedule.matrix[first_row], schedule.matrix[second_row]);
            }
            if (second_row < schedule.matrix_constants.size()) {
                std::swap(schedule.matrix_constants[first_row],
                          schedule.matrix_constants[second_row]);
            }
            for (auto &row : schedule.matrix) {
                if (depth + 1 < row.size()) {
                    std::swap(row[depth], row[depth + 1]);
                }
            }
            if (depth + 1 < schedule.bands.size()) {
                std::swap(schedule.bands[depth], schedule.bands[depth + 1]);
                schedule.bands[depth].output_offset = 1 + depth * 2;
                schedule.bands[depth + 1].output_offset = 1 + (depth + 1) * 2;
            }
        }
    }

    static void mark_scop_bands_tiled(PolyScop &scop, std::size_t depth,
                                      std::int64_t tile_size,
                                      bool inner_only = false) {
        if (tile_size <= 0) {
            return;
        }
        for (auto &stmt : scop.statements) {
            const std::size_t first_depth = inner_only ? depth + 1 : depth;
            for (std::size_t band_depth = first_depth; band_depth <= depth + 1; ++band_depth) {
                if (band_depth >= stmt.schedule.bands.size()) {
                    continue;
                }
                auto &band = stmt.schedule.bands[band_depth];
                band.tiled = true;
                band.tile_size = tile_size;
                band.point_order_preserving = inner_only;
                const std::size_t output_row = 1 + band_depth * 2;
                if (output_row < stmt.schedule.output_dims.size()) {
                    auto tile_operand = std::make_shared<PolyAffineExpr>(
                        stmt.schedule.output_dims[output_row]);
                    auto tile_expr = std::make_shared<PolyAffineExpr>();
                    tile_expr->kind = PolyAffineExprKind::FloorDiv;
                    tile_expr->operand = std::move(tile_operand);
                    tile_expr->divisor = tile_size;
                    tile_expr->valid = true;
                    band.tile_expr = std::move(tile_expr);
                }
            }
            stmt.schedule.optimized = true;
        }
    }

    void update_dependences_after_band_permutation(const PolyScop &scop,
                                                   std::size_t depth) {
        const auto belongs_to_scop = [&scop](const PolyDependence &dep) {
            bool source_found = false;
            bool target_found = false;
            for (const auto &stmt : scop.statements) {
                source_found = source_found || stmt.id == dep.source_stmt_id;
                target_found = target_found || stmt.id == dep.target_stmt_id;
            }
            return source_found && target_found;
        };

        for (auto &dep : dep_info_.dependences) {
            if (!belongs_to_scop(dep)) {
                continue;
            }
            if (depth + 1 < dep.distance.size()) {
                std::swap(dep.distance[depth], dep.distance[depth + 1]);
            }
            // The relation contains the old lexicographic ordering.  Its
            // domain/access constraints remain useful, but the old order is
            // no longer an exact proof after the permutation.
            dep.relation.exact = false;
        }
    }

    bool find_poly_band_tile_candidate(
        const PolyScop &scop,
        const std::unordered_map<const yir::Region *, yir::ForOp *> &for_body_owners,
        PolyBandTileCandidate &candidate, std::size_t depth = 0) const {
        std::size_t num_reads = 0;
        std::size_t num_writes = 0;
        const PolyStmt *write_anchor = nullptr;
        const PolyStmt *memory_anchor = nullptr;
        for (const auto &stmt : scop.statements) {
            num_reads += stmt.reads.size();
            num_writes += stmt.writes.size();
            if (stmt.schedule_dims.size() <= depth + 1 ||
                stmt.loop_bounds.size() <= depth + 1 ||
                (stmt.reads.empty() && stmt.writes.empty())) {
                continue;
            }
            if (memory_anchor == nullptr) {
                memory_anchor = &stmt;
            }
            if (write_anchor == nullptr && !stmt.writes.empty()) {
                write_anchor = &stmt;
            }
        }
        const PolyStmt *anchor = write_anchor != nullptr ? write_anchor : memory_anchor;
        if (anchor == nullptr || num_reads == 0 || num_writes == 0 ||
            anchor->op == nullptr || anchor->op->parent() == nullptr) {
            return false;
        }

        auto anchor_loops = collect_enclosing_loop_nest(
            const_cast<yir::Operation *>(anchor->op)->parent(), for_body_owners);
        if (anchor_loops.outer_to_inner.size() <= depth + 1) {
            return false;
        }

        auto *outer_loop = anchor_loops.outer_to_inner[depth];
        auto *inner_loop = anchor_loops.outer_to_inner[depth + 1];
        if (outer_loop == nullptr || inner_loop == nullptr ||
            inner_loop->parent() != &outer_loop->body_region() ||
            anchor->schedule_dims.size() <= depth + 1 ||
            outer_loop->induction_var() != anchor->schedule_dims[depth] ||
            inner_loop->induction_var() != anchor->schedule_dims[depth + 1] ||
            !schedule_band_is_identity(*anchor, depth, outer_loop->induction_var()) ||
            !schedule_band_is_identity(*anchor, depth + 1, inner_loop->induction_var()) ||
            !loop_has_unit_step(*outer_loop) || !loop_has_unit_step(*inner_loop)) {
            return false;
        }

        auto *outer_parent = outer_loop->parent();
        std::size_t ignored_outer_index = 0;
        std::size_t inner_index = 0;
        if (outer_parent == nullptr ||
            !find_operation_index(*outer_parent, *outer_loop, ignored_outer_index) ||
            !find_operation_index(outer_loop->body_region(), *inner_loop, inner_index)) {
            return false;
        }

        const auto *outer_iv = outer_loop->induction_var();
        const auto *inner_iv = inner_loop->induction_var();
        const bool has_outer_point_statement = std::any_of(
            scop.statements.begin(), scop.statements.end(),
            [&](const PolyStmt &stmt) {
                return stmt.op != nullptr &&
                       stmt.op->parent() == &outer_loop->body_region() &&
                       stmt.schedule_dims.size() == depth + 1 &&
                       stmt.loop_bounds.size() == depth + 1 &&
                       stmt.schedule_dims[depth] == outer_iv &&
                       stmt.loop_bounds[depth].iv == outer_iv;
            });
        const bool local_scalar_state =
            poly_band_scalar_state_is_iteration_local(inner_loop->body_region());
        const bool force_inner_only =
            has_outer_point_statement || !local_scalar_state;
        const bool perfect_loop_shell =
            inner_index + 1 == outer_loop->body_region().operations().size() &&
            inner_index <= 1;
        if (!force_inner_only && !perfect_loop_shell) {
            return false;
        }
        if (!force_inner_only && inner_index == 1) {
            auto *first = outer_loop->body_region().operations().front().get();
            auto *inner_iv_decl = dynamic_cast<yir::VarOp *>(first);
            auto *inner_iv_reset = dynamic_cast<yir::AssignOp *>(first);
            std::int64_t reset_value = 0;
            std::int64_t lower_value = 0;
            const bool valid_decl = inner_iv_decl != nullptr &&
                                    inner_iv_decl->result() == inner_loop->induction_var();
            const bool valid_reset =
                inner_iv_reset != nullptr &&
                inner_iv_reset->target() == inner_loop->induction_var() &&
                (inner_iv_reset->value() == inner_loop->lower_bound() ||
                 (const_i32_value(inner_iv_reset->value(), reset_value) &&
                  const_i32_value(inner_loop->lower_bound(), lower_value) &&
                  reset_value == lower_value));
            if (!valid_decl && !valid_reset) {
                return false;
            }
        }

        if (poly_band_region_has_unsupported_control(outer_loop->body_region()) ||
            region_contains_call(outer_loop->body_region()) ||
            (force_inner_only &&
             !poly_inner_strip_structure_is_safe(*outer_loop, *inner_loop, inner_index))) {
            return false;
        }

        for (const auto &stmt : scop.statements) {
            if (stmt.op == nullptr || stmt.op->parent() == nullptr) {
                return false;
            }

            auto stmt_loops = collect_enclosing_loop_nest(
                const_cast<yir::Operation *>(stmt.op)->parent(), for_body_owners);
            const bool inside_full_band =
                stmt.schedule_dims.size() > depth + 1 &&
                stmt.loop_bounds.size() > depth + 1 &&
                stmt.schedule_dims[depth] == outer_iv &&
                stmt.schedule_dims[depth + 1] == inner_iv &&
                schedule_band_is_identity(stmt, depth, outer_iv) &&
                schedule_band_is_identity(stmt, depth + 1, inner_iv) &&
                stmt.loop_bounds[depth].iv == outer_iv &&
                stmt.loop_bounds[depth + 1].iv == inner_iv &&
                stmt_loops.outer_to_inner.size() > depth + 1 &&
                stmt_loops.outer_to_inner[depth] == outer_loop &&
                stmt_loops.outer_to_inner[depth + 1] == inner_loop;
            if (inside_full_band) {
                continue;
            }

            const bool outer_point_statement =
                force_inner_only && stmt.schedule_dims.size() == depth + 1 &&
                stmt.loop_bounds.size() == depth + 1 &&
                stmt.schedule_dims[depth] == outer_iv &&
                stmt.loop_bounds[depth].iv == outer_iv &&
                stmt_loops.outer_to_inner.size() == depth + 1 &&
                stmt_loops.outer_to_inner[depth] == outer_loop &&
                stmt.op->parent() == &outer_loop->body_region();
            if (!outer_point_statement) {
                return false;
            }
        }

        if (!poly_band_has_complete_memory_model(scop, outer_loop->body_region()) ||
            !poly_band_has_rectangular_bounds(*anchor, outer_iv, inner_iv, depth)) {
            return false;
        }

        candidate.outer_loop = outer_loop;
        candidate.inner_loop = inner_loop;
        candidate.outer_iv = outer_iv;
        candidate.inner_iv = inner_iv;
        candidate.depth = depth;
        candidate.force_inner_only = force_inner_only;
        return true;
    }

    static bool poly_inner_strip_structure_is_safe(
        const yir::ForOp &outer_loop, const yir::ForOp &inner_loop,
        std::size_t inner_index) {
        const auto &ops = outer_loop.body_region().operations();
        if (inner_index >= ops.size() || ops[inner_index].get() != &inner_loop) {
            return false;
        }

        const auto valid_iv_reset = [&](const yir::Operation &op) {
            auto *assign = dynamic_cast<const yir::AssignOp *>(&op);
            if (assign == nullptr || assign->target() != inner_loop.induction_var()) {
                return false;
            }
            if (assign->value() == inner_loop.lower_bound()) {
                return true;
            }
            std::int64_t assigned = 0;
            std::int64_t lower = 0;
            return const_i32_value(assign->value(), assigned) &&
                   const_i32_value(inner_loop.lower_bound(), lower) && assigned == lower;
        };

        for (std::size_t index = 0; index < ops.size(); ++index) {
            if (index == inner_index) {
                continue;
            }
            const auto &op = *ops[index];
            if (dynamic_cast<const yir::ForOp *>(&op) != nullptr ||
                dynamic_cast<const yir::WhileOp *>(&op) != nullptr ||
                dynamic_cast<const yir::BreakOp *>(&op) != nullptr ||
                dynamic_cast<const yir::ContinueOp *>(&op) != nullptr ||
                dynamic_cast<const yir::ReturnOp *>(&op) != nullptr ||
                dynamic_cast<const yir::CondOp *>(&op) != nullptr ||
                operation_contains_call(op)) {
                return false;
            }
            if (operation_uses_value(op, inner_loop.induction_var()) &&
                !(index < inner_index && valid_iv_reset(op))) {
                return false;
            }
        }
        return true;
    }

    static bool poly_band_region_has_unsupported_control(const yir::Region &region) {
        for (const auto &op : region.operations()) {
            if (dynamic_cast<const yir::WhileOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::BreakOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ContinueOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ReturnOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::CondOp *>(op.get()) != nullptr) {
                return true;
            }
            if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
                if (poly_band_region_has_unsupported_control(if_op->then_region()) ||
                    (if_op->has_else() &&
                     poly_band_region_has_unsupported_control(if_op->else_region()))) {
                    return true;
                }
                continue;
            }
            if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
                if (poly_band_region_has_unsupported_control(for_op->body_region())) {
                    return true;
                }
            }
        }
        return false;
    }

    bool poly_band_tile_is_legal(const PolyScop &scop,
                                 PolyBandTileCandidate &candidate) const {
        if (candidate.outer_iv == nullptr || candidate.inner_iv == nullptr) {
            return false;
        }

        candidate.inner_only = candidate.force_inner_only;
        candidate.relation_proven = false;
        if (candidate.force_inner_only) {
            // Inner strip-mining preserves the complete original point order,
            // including statements before and after an imperfect inner loop.
            return true;
        }
        for (const auto &dep : dep_info_.dependences) {
            if (!dep.is_dependent) {
                continue;
            }
            if (dep.is_reduction) {
                continue;
            }
            const auto *source = find_stmt_by_id(scop, dep.source_stmt_id);
            const auto *target = find_stmt_by_id(scop, dep.target_stmt_id);
            if (source == nullptr || target == nullptr) {
                continue;
            }

            PolyBandInterchangeCandidate band{candidate.outer_loop, candidate.inner_loop,
                                               candidate.outer_iv, candidate.inner_iv, nullptr,
                                               candidate.depth};
            std::int64_t outer_distance = 0;
            std::int64_t inner_distance = 0;
            if (dependence_band_distance(dep, band, outer_distance, inner_distance)) {
                if (outer_distance < 0 ||
                    (outer_distance == 0 && inner_distance < 0)) {
                    return false;
                }
                if (inner_distance < 0) {
                    candidate.inner_only = true;
                }
                continue;
            }
            if (dep.relation.exact) {
                const auto tile_legality =
                    relation_tile_schedule_legality(scop, dep, candidate);
                if (tile_legality == RelationScheduleLegality::Legal) {
                    candidate.relation_proven = true;
                    continue;
                }
                if (tile_legality == RelationScheduleLegality::Illegal) {
                    candidate.inner_only = true;
                    continue;
                }
            }
            if (unknown_dependence_is_inner_partitioned(*source, *target, dep,
                                                        candidate.inner_iv)) {
                continue;
            }
            if (dep.relation.exact) {
                candidate.inner_only = true;
                continue;
            }
            return false;
        }
        return true;
    }

    static void estimate_region_shape(const yir::Region &region, std::int64_t &ops,
                                      std::int64_t &memory_ops, std::int64_t &branches,
                                      std::int64_t &locals, bool &nested_loop) {
        for (const auto &op : region.operations()) {
            ++ops;
            if (dynamic_cast<const yir::ArrayLoadOp *>(op.get()) != nullptr ||
                dynamic_cast<const yir::ArrayStoreOp *>(op.get()) != nullptr) {
                ++memory_ops;
            }
            if (dynamic_cast<const yir::IfOp *>(op.get()) != nullptr) {
                ++branches;
            }
            if (dynamic_cast<const yir::VarOp *>(op.get()) != nullptr) {
                ++locals;
            }
            if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
                estimate_region_shape(if_op->then_region(), ops, memory_ops, branches, locals,
                                      nested_loop);
                if (if_op->has_else()) {
                    estimate_region_shape(if_op->else_region(), ops, memory_ops, branches,
                                          locals, nested_loop);
                }
            } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
                nested_loop = true;
                estimate_region_shape(for_op->body_region(), ops, memory_ops, branches, locals,
                                      nested_loop);
            }
        }
    }

    static std::int64_t array_element_count(const yir::Value *memory) {
        if (memory == nullptr || memory->type() == nullptr) {
            return 0;
        }
        auto type = memory->type();
        if (type->is_ptr()) {
            type = type->pointee();
        }
        std::int64_t count = 1;
        while (type != nullptr && type->is_array()) {
            if (type->count() == 0 || count > std::numeric_limits<std::int64_t>::max() /
                                             static_cast<std::int64_t>(type->count())) {
                return 0;
            }
            count *= static_cast<std::int64_t>(type->count());
            type = type->element();
        }
        return type != nullptr && type->kind() == yir::Type::Kind::I32 ? count : 0;
    }

    static bool scop_has_reduction_dimension(const PolyScop &scop,
                                             std::size_t depth) {
        return std::any_of(scop.statements.begin(), scop.statements.end(),
                           [depth](const PolyStmt &stmt) {
                               return stmt.reduction_kind !=
                                          PolyStmt::ReductionKind::None &&
                                      stmt.reduction_dims.size() > depth &&
                                      stmt.reduction_dims[depth];
                           });
    }

    static bool scop_has_opaque_conditions(const PolyScop &scop) {
        return std::any_of(scop.statements.begin(), scop.statements.end(),
                           [](const PolyStmt &stmt) {
                               return stmt.has_opaque_conditions;
                           });
    }

    static std::int64_t poly_band_reuse_accesses(
        const PolyScop &scop, const PolyBandTileCandidate &candidate) {
        std::unordered_map<const yir::Value *, std::int64_t> read_counts;
        for (const auto &stmt : scop.statements) {
            for (const auto &access : stmt.reads) {
                ++read_counts[access.memory];
            }
        }

        std::int64_t reusable = 0;
        for (const auto &stmt : scop.statements) {
            for (const auto &access : stmt.reads) {
                // Tiny invariant accumulators already stay hot without tiling.
                if (!is_valid_affine_access(access) ||
                    array_element_count(access.memory) <= 256) {
                    continue;
                }
                const bool misses_outer =
                    affine_dim_coefficient_in_access(access, candidate.outer_iv) == 0;
                const bool misses_inner =
                    affine_dim_coefficient_in_access(access, candidate.inner_iv) == 0;
                if (misses_outer || misses_inner || read_counts[access.memory] > 1) {
                    ++reusable;
                }
            }
        }
        return reusable;
    }

    static std::int64_t affine_dim_coefficient_in_access(
        const PolyAccess &access, const yir::Value *dim) {
        std::int64_t coefficient = 0;
        for (const auto &index : access.indices) {
            coefficient += nonnegative_distance(affine_dim_coefficient(index, dim));
        }
        return coefficient;
    }

    bool poly_band_interchange_backend_is_profitable(
        const PolyScop &scop, const PolyBandInterchangeCandidate &candidate) {
        const int locality_score =
            poly_band_interchange_schedule_score(scop, candidate);
        if (locality_score <= 0 || candidate.outer_loop == nullptr) {
            return false;
        }

        std::int64_t ops = 0;
        std::int64_t memory_ops = 0;
        std::int64_t branches = 0;
        std::int64_t locals = 0;
        bool nested_loop = false;
        estimate_region_shape(candidate.outer_loop->body_region(), ops, memory_ops, branches,
                              locals, nested_loop);

        cost_model::TransformCandidate model_candidate;
        model_candidate.kind = cost_model::TransformKind::LoopInterchange;
        model_candidate.stage = cost_model::CostIRStage::YIR;
        model_candidate.pass_name = "YIRPolyhedralTransformPass";
        model_candidate.candidate_id = "band-interchange";
        model_candidate.scope =
            std::to_string(scop.id) + ":" + std::to_string(candidate.depth);
        model_candidate.frequency.source =
            cost_model::FrequencySource::StructuredYIRLoop;
        model_candidate.frequency.scale = nested_loop ? 4 : 2;
        model_candidate.frequency.confidence = 0.82;
        model_candidate.proof.kind = cost_model::ProofKind::Dependence;
        model_candidate.proof.status = cost_model::ProofStatus::Proven;
        model_candidate.proof.summary =
            scop_has_reduction_dimension(scop, candidate.depth) ||
                    scop_has_reduction_dimension(scop, candidate.depth + 1)
                ? "affine schedule matrix permutation preserves dependences and associative integer reduction"
                : "affine schedule matrix permutation preserves dependence order";
        if (scop_has_opaque_conditions(scop)) {
            model_candidate.proof.summary +=
                "; pure opaque guards conservatively over-approximated";
        }

        auto &before = model_candidate.before;
        before.static_instrs = std::max<std::int64_t>(1, ops);
        before.loads = memory_ops;
        before.branches = branches;
        const auto target = active_target_profile();
        before.estimated_cycles =
            (ops * target.alu_i32 + memory_ops * target.load +
             branches * target.branch) * model_candidate.frequency.scale;

        auto &after = model_candidate.after;
        after = before;
        const std::int64_t raw_locality_gain = static_cast<std::int64_t>(locality_score) *
                                                target.load * model_candidate.frequency.scale;
        // Interchange changes traversal order; it does not eliminate all memory
        // operations.  Cap the modeled gain so a locality heuristic cannot turn
        // a small loop body into an impossible zero-cycle kernel.
        const std::int64_t locality_gain = std::min(
            raw_locality_gain, std::max<std::int64_t>(1, before.estimated_cycles / 3));
        after.estimated_cycles =
            before.estimated_cycles - locality_gain;
        model_candidate.risk.live_range_growth = locals > 4 ? 1 : 0;

        const auto decision = cost_model::decide(
            model_candidate, active_cost_policy(), active_target_profile());
        if (cost_report_ != nullptr) {
            cost_report_->decisions.push_back(decision);
        }
        return decision.profitable;
    }

    bool poly_band_tile_is_profitable(const PolyScop &scop,
                                      const PolyBandTileCandidate &candidate) {
        if (candidate.outer_loop == nullptr) {
            return false;
        }

        std::int64_t ops = 0;
        std::int64_t memory_ops = 0;
        std::int64_t branches = 0;
        std::int64_t locals = 0;
        bool nested_loop = false;
        estimate_region_shape(candidate.outer_loop->body_region(), ops, memory_ops, branches,
                              locals, nested_loop);
        std::unordered_set<const yir::Value *> memories;
        std::int64_t working_set = 0;
        for (const auto &stmt : scop.statements) {
            for (const auto &access : stmt.reads) {
                if (memories.insert(access.memory).second) {
                    working_set += array_element_count(access.memory);
                }
            }
            for (const auto &access : stmt.writes) {
                if (memories.insert(access.memory).second) {
                    working_set += array_element_count(access.memory);
                }
            }
        }

        std::int64_t module_ops = 0;
        std::int64_t ignored_memory = 0;
        std::int64_t ignored_branches = 0;
        std::int64_t ignored_locals = 0;
        bool ignored_nested = false;
        for (const auto &function : module_.functions()) {
            estimate_region_shape(function->body(), module_ops, ignored_memory,
                                  ignored_branches, ignored_locals, ignored_nested);
        }

        cost_model::TransformCandidate model_candidate;
        model_candidate.kind = cost_model::TransformKind::LoopTiling;
        model_candidate.stage = cost_model::CostIRStage::YIR;
        model_candidate.pass_name = "YIRPolyhedralTransformPass";
        model_candidate.candidate_id = "band-tile";
        model_candidate.scope = std::to_string(scop.id);
        model_candidate.frequency.source = cost_model::FrequencySource::StructuredYIRLoop;
        model_candidate.frequency.confidence = nested_loop ? 0.72 : 0.84;
        // The body shape is measured once, while a nested band executes it many
        // times.  Keep a small lower-bound multiplier so setup is not compared
        // against a single iteration of the loop nest.
        const std::int64_t dynamic_multiplier = nested_loop ? 4 : 1;
        const std::int64_t reuse_accesses =
            poly_band_reuse_accesses(scop, candidate);
        model_candidate.frequency.scale = dynamic_multiplier;
        model_candidate.proof.kind = cost_model::ProofKind::Dependence;
        model_candidate.proof.status = cost_model::ProofStatus::Proven;
        const int tile_size = tile_size_for_candidate(candidate);
        if (tile_size <= 0) {
            return false;
        }
        const std::string legality_summary =
            scop_has_reduction_dimension(scop, candidate.depth) ||
                    scop_has_reduction_dimension(scop, candidate.depth + 1)
                ? "polyhedral legality with associative integer reduction"
                : "polyhedral dependence legality";
        model_candidate.proof.summary =
            legality_summary +
            (scop_has_opaque_conditions(scop)
                 ? "; pure opaque guards conservatively over-approximated"
                 : "") +
            (candidate.inner_only ? "; point-order-preserving inner strip-mining tile="
             : (candidate.relation_proven
                    ? "; Presburger tiled schedule legality; backend rectangular tiling tile="
                    : "; backend rectangular tiling tile=")) +
            std::to_string(tile_size);

        auto &before = model_candidate.before;
        before.static_instrs = std::max(ops, module_ops);
        before.loads = memory_ops;
        before.branches = branches;
        const auto target = active_target_profile();
        before.estimated_cycles =
            (ops * target.alu_i32 + memory_ops * target.load +
             branches * target.branch) * dynamic_multiplier;

        auto &after = model_candidate.after;
        after = before;
        const std::int64_t setup_ops = candidate.inner_only ? 4 : (nested_loop ? 10 : 8);
        after.static_instrs += setup_ops;
        const std::int64_t added_loops = candidate.inner_only ? 1 : 2;
        after.branches += added_loops;
        after.jumps += added_loops;
        after.pointer_arith += added_loops;
        const std::int64_t pressure_features =
            static_cast<std::int64_t>(memories.size()) + locals + branches +
            (nested_loop ? 2 : 0) + added_loops;
        const std::int64_t pressure_budget =
            branches > 0 || scop_has_opaque_conditions(scop) ? 7 : 8;
        // Tiled YIR keeps tile bounds, point IVs, and memory bases live across
        // the inner body. Model pressure before MIR lowering so an apparent
        // locality win cannot hide the spill traffic introduced by lowering.
        after.estimated_spills = candidate.inner_only
            ? 0
            : std::max<std::int64_t>(0, pressure_features - pressure_budget);
        // Tiling pays for itself through repeated inner-loop reuse.  Estimate that
        // dynamic benefit separately from the small, mostly static setup cost; a
        // large working set gets a deliberately smaller gain because it is less
        // likely to fit the target cache.
        const std::int64_t reuse_gain_per_access =
            nested_loop ? (working_set >= 1'000'000 ? 12 : 24) :
                          (working_set >= 1'000'000 ? 8 : 16);
        // Point-order-preserving strip-mining does not change reuse distance by
        // itself. Until lowering has a tile-aware vectorizer/unroller, crediting
        // it with cache locality would accept extra loops that only increase
        // register pressure and spills.
        const std::int64_t raw_locality_gain = candidate.inner_only
            ? 0
            : reuse_accesses * reuse_gain_per_access * dynamic_multiplier;
        const std::int64_t locality_gain = std::min(
            raw_locality_gain, std::max<std::int64_t>(1, before.estimated_cycles * 4 / 5));
        after.estimated_cycles = before.estimated_cycles + setup_ops +
                                 target.branch * added_loops +
                                 target.alu_i32 * added_loops - locality_gain +
                                 after.estimated_spills *
                                     (target.spill_load + target.spill_store) *
                                     dynamic_multiplier;

        model_candidate.risk.code_growth = setup_ops;
        model_candidate.risk.live_range_growth = (candidate.inner_only ? 1 : 2) + locals;
        model_candidate.risk.register_pressure_growth =
            candidate.inner_only
                ? 1 + locals
                : (working_set >= 1'000'000 ? 2 * locals + (nested_loop ? 7 : 3)
                                            : 2 + locals + after.estimated_spills);
        model_candidate.risk.memory_pressure_growth = candidate.inner_only ? 1
                                                                           : (nested_loop ? 2 : 1);
        model_candidate.risk.cleanup_dependency = 1;

        const auto decision = cost_model::decide(
            model_candidate, active_cost_policy(), active_target_profile());
        if (cost_report_ != nullptr) {
            cost_report_->decisions.push_back(decision);
        }
        return decision.profitable;
    }

    static bool unknown_dependence_is_inner_partitioned(const PolyStmt &source,
                                                       const PolyStmt &target,
                                                       const PolyDependence &dep,
                                                       const yir::Value *inner_iv) {
        std::vector<const PolyAccess *> source_accesses;
        std::vector<const PolyAccess *> target_accesses;
        switch (dep.kind) {
        case PolyDependence::Kind::RAW:
            collect_accesses_for_memory(source.writes, dep.memory, source_accesses);
            collect_accesses_for_memory(target.reads, dep.memory, target_accesses);
            break;
        case PolyDependence::Kind::WAR:
            collect_accesses_for_memory(source.reads, dep.memory, source_accesses);
            collect_accesses_for_memory(target.writes, dep.memory, target_accesses);
            break;
        case PolyDependence::Kind::WAW:
            collect_accesses_for_memory(source.writes, dep.memory, source_accesses);
            collect_accesses_for_memory(target.writes, dep.memory, target_accesses);
            break;
        case PolyDependence::Kind::RAR:
            return true;
        }

        if (source_accesses.empty() || target_accesses.empty()) {
            return false;
        }
        for (const auto *source_access : source_accesses) {
            for (const auto *target_access : target_accesses) {
                if (!accesses_share_exact_inner_partition(*source_access, *target_access,
                                                          inner_iv)) {
                    return false;
                }
            }
        }
        return true;
    }

    static void collect_accesses_for_memory(const std::vector<PolyAccess> &accesses,
                                            const yir::Value *memory,
                                            std::vector<const PolyAccess *> &out) {
        for (const auto &access : accesses) {
            if (access.memory == memory) {
                out.push_back(&access);
            }
        }
    }

    static bool accesses_share_exact_inner_partition(const PolyAccess &source,
                                                     const PolyAccess &target,
                                                     const yir::Value *inner_iv) {
        if (source.memory != target.memory || source.indices.size() != target.indices.size()) {
            return false;
        }
        for (std::size_t i = 0; i < source.indices.size(); ++i) {
            if (is_exact_dim(source.indices[i], inner_iv) &&
                is_exact_dim(target.indices[i], inner_iv)) {
                return true;
            }
        }
        return false;
    }

    static bool is_exact_dim(const PolyAffineExpr &expr, const yir::Value *dim) {
        if (!expr.is_linear() || expr.constant != 0 || dim == nullptr || expr.terms.size() != 1) {
            return false;
        }
        return expr.terms.front().first == dim && expr.terms.front().second == 1;
    }

    bool apply_poly_inner_strip_mining(const PolyBandTileCandidate &candidate) {
        auto *outer_loop = candidate.outer_loop;
        auto *inner_loop = candidate.inner_loop;
        if (outer_loop == nullptr || inner_loop == nullptr || outer_loop->parent() == nullptr ||
            inner_loop->parent() != &outer_loop->body_region()) {
            return false;
        }
        const int tile_size = tile_size_for_candidate(candidate);
        if (tile_size <= 0) {
            return false;
        }

        auto *parent = outer_loop->parent();
        std::size_t outer_index = 0;
        std::size_t inner_index = 0;
        if (!find_operation_index(*parent, *outer_loop, outer_index) ||
            !find_operation_index(outer_loop->body_region(), *inner_loop, inner_index)) {
            return false;
        }

        auto *inner_lower = inner_loop->lower_bound();
        auto *inner_upper = inner_loop->upper_bound();
        auto *inner_step = inner_loop->step();
        auto *inner_iv = inner_loop->induction_var();
        if (value_defined_inside_region(inner_lower, outer_loop->body_region()) ||
            value_defined_inside_region(inner_upper, outer_loop->body_region()) ||
            value_defined_inside_region(inner_step, outer_loop->body_region())) {
            return false;
        }
        std::int64_t ignored = 0;
        const bool static_bounds = const_i32_value(inner_lower, ignored) &&
                                   const_i32_value(inner_upper, ignored);

        std::size_t insert_pos = outer_index;
        auto *tile_step_value = insert_op_before<yir::ConstI32Op>(
            *parent, insert_pos, tile_size,
            static_bounds ? tile_iv_name(inner_iv, "step", next_tile_temp_++)
                          : tile_temp_name("step", next_tile_temp_++))->result();
        auto *inner_tile_var = insert_op_before<yir::VarOp>(
            *parent, insert_pos, yir::Type::get_i32(), inner_lower,
            static_bounds ? tile_iv_name(inner_iv, "inner", next_tile_temp_++)
                          : tile_temp_name("inner", next_tile_temp_++));

        auto inner_tile = std::make_unique<yir::ForOp>(
            inner_tile_var->result(), inner_lower, inner_upper, tile_step_value);
        inner_tile->set_parent(&outer_loop->body_region());
        auto *inner_tile_raw = inner_tile.get();
        auto *inner_limit = append_poly_tile_upper(
            inner_tile_raw->body_region(), inner_tile_var->result(), tile_step_value,
            inner_upper, !can_omit_poly_tile_clamp(*inner_loop, tile_size),
            static_bounds, "inner.limit");

        inner_loop->operands()[1] = inner_tile_var->result();
        inner_loop->operands()[2] = inner_limit;
        inner_loop->operands()[3] = inner_step;

        auto &outer_body_ops = outer_loop->body_region().operations();
        auto moved_inner = std::move(outer_body_ops[inner_index]);
        moved_inner->set_parent(&inner_tile_raw->body_region());
        inner_tile_raw->body_region().operations().push_back(std::move(moved_inner));
        outer_body_ops[inner_index] = std::move(inner_tile);
        return true;
    }

    bool apply_poly_band_tiling(const PolyBandTileCandidate &candidate) {
        if (candidate.inner_only) {
            return apply_poly_inner_strip_mining(candidate);
        }
        auto *outer_loop = candidate.outer_loop;
        auto *inner_loop = candidate.inner_loop;
        if (outer_loop == nullptr || inner_loop == nullptr || outer_loop->parent() == nullptr ||
            inner_loop->parent() != &outer_loop->body_region()) {
            return false;
        }

        auto *parent = outer_loop->parent();
        std::size_t outer_index = 0;
        if (!find_operation_index(*parent, *outer_loop, outer_index)) {
            return false;
        }

        const int tile_size = choose_poly_band_tile_size(*outer_loop, *inner_loop);
        if (tile_size == 0) {
            return false;
        }

        auto *outer_iv = outer_loop->induction_var();
        auto *inner_iv = inner_loop->induction_var();
        auto *outer_lower = outer_loop->lower_bound();
        auto *outer_upper = outer_loop->upper_bound();
        auto *outer_step = outer_loop->step();
        auto *inner_lower = inner_loop->lower_bound();
        auto *inner_upper = inner_loop->upper_bound();
        auto *inner_step = inner_loop->step();
        if (value_defined_inside_region(inner_lower, outer_loop->body_region()) ||
            value_defined_inside_region(inner_upper, outer_loop->body_region()) ||
            value_defined_inside_region(inner_step, outer_loop->body_region())) {
            return false;
        }
        std::int64_t ignored = 0;
        const bool static_bounds = const_i32_value(outer_lower, ignored) &&
                                   const_i32_value(outer_upper, ignored) &&
                                   const_i32_value(inner_lower, ignored) &&
                                   const_i32_value(inner_upper, ignored);

        std::size_t insert_pos = outer_index;
        auto *tile_step_value = insert_op_before<yir::ConstI32Op>(
            *parent, insert_pos, tile_size,
            static_bounds ? tile_iv_name(outer_iv, "step", next_tile_temp_++)
                          : tile_temp_name("step", next_tile_temp_++))->result();
        auto *outer_tile_var = insert_op_before<yir::VarOp>(
            *parent, insert_pos, yir::Type::get_i32(), outer_lower,
            static_bounds ? tile_iv_name(outer_iv, "outer", next_tile_temp_++)
                          : tile_temp_name("outer", next_tile_temp_++));
        auto *inner_tile_var = insert_op_before<yir::VarOp>(
            *parent, insert_pos, yir::Type::get_i32(), inner_lower,
            static_bounds ? tile_iv_name(inner_iv, "inner", next_tile_temp_++)
                          : tile_temp_name("inner", next_tile_temp_++));

        auto outer_tile = std::make_unique<yir::ForOp>(
            outer_tile_var->result(), outer_lower, outer_upper, tile_step_value);
        outer_tile->set_parent(parent);
        auto *outer_tile_raw = outer_tile.get();

        auto *outer_limit = append_poly_tile_upper(
            outer_tile_raw->body_region(), outer_tile_var->result(), tile_step_value,
            outer_upper, !can_omit_poly_tile_clamp(*outer_loop, tile_size),
            static_bounds, "outer.limit");

        auto inner_tile = std::make_unique<yir::ForOp>(
            inner_tile_var->result(), inner_lower, inner_upper, tile_step_value);
        inner_tile->set_parent(&outer_tile_raw->body_region());
        auto *inner_tile_raw = inner_tile.get();
        outer_tile_raw->body_region().operations().push_back(std::move(inner_tile));

        auto *inner_limit = append_poly_tile_upper(
            inner_tile_raw->body_region(), inner_tile_var->result(), tile_step_value,
            inner_upper, !can_omit_poly_tile_clamp(*inner_loop, tile_size), static_bounds,
            "inner.limit");

        outer_loop->operands()[1] = outer_tile_var->result();
        outer_loop->operands()[2] = outer_limit;
        outer_loop->operands()[3] = outer_step;
        inner_loop->operands()[1] = inner_tile_var->result();
        inner_loop->operands()[2] = inner_limit;
        inner_loop->operands()[3] = inner_step;

        auto &parent_ops = parent->operations();
        auto moved_outer = std::move(parent_ops[insert_pos]);
        moved_outer->set_parent(&inner_tile_raw->body_region());
        inner_tile_raw->body_region().operations().push_back(std::move(moved_outer));
        parent_ops[insert_pos] = std::move(outer_tile);
        return true;
    }

    static int choose_poly_band_tile_size(const yir::ForOp &outer_loop,
                                          const yir::ForOp &inner_loop) {
        std::int64_t outer_lower = 0;
        std::int64_t outer_upper = 0;
        std::int64_t inner_lower = 0;
        std::int64_t inner_upper = 0;
        if (const_i32_value(outer_loop.lower_bound(), outer_lower) &&
            const_i32_value(outer_loop.upper_bound(), outer_upper) &&
            const_i32_value(inner_loop.lower_bound(), inner_lower) &&
            const_i32_value(inner_loop.upper_bound(), inner_upper)) {
            const auto outer_trip = outer_upper - outer_lower;
            const auto inner_trip = inner_upper - inner_lower;
            if (outer_trip < 64 || inner_trip < 64) {
                return 0;
            }
            for (int candidate : {20, 16, 10, 8}) {
                if (outer_trip % candidate == 0 && inner_trip % candidate == 0) {
                    return candidate;
                }
            }
            return 0;
        }
        return 16;
    }

    static int choose_inner_strip_mine_size(const yir::ForOp &inner_loop) {
        std::int64_t lower = 0;
        std::int64_t upper = 0;
        if (const_i32_value(inner_loop.lower_bound(), lower) &&
            const_i32_value(inner_loop.upper_bound(), upper)) {
            const auto trip_count = upper - lower;
            if (trip_count < 64) {
                return 0;
            }
            for (int candidate : {20, 16, 10, 8}) {
                if (trip_count % candidate == 0) {
                    return candidate;
                }
            }
            return 0;
        }
        return 16;
    }

    static int tile_size_for_candidate(const PolyBandTileCandidate &candidate) {
        if (candidate.outer_loop == nullptr || candidate.inner_loop == nullptr) {
            return 0;
        }
        return candidate.inner_only
                   ? choose_inner_strip_mine_size(*candidate.inner_loop)
                   : choose_poly_band_tile_size(*candidate.outer_loop,
                                                *candidate.inner_loop);
    }

    static bool can_omit_poly_tile_clamp(const yir::ForOp &loop, int tile_size) {
        std::int64_t lower = 0;
        std::int64_t upper = 0;
        std::int64_t step = 0;
        if (tile_size <= 0 || !const_i32_value(loop.lower_bound(), lower) ||
            !const_i32_value(loop.upper_bound(), upper) ||
            !const_i32_value(loop.step(), step) || step != 1 || upper < lower) {
            return false;
        }
        return (upper - lower) % tile_size == 0;
    }

    yir::Value *append_poly_tile_upper(yir::Region &region, yir::Value *tile_iv,
                                       yir::Value *tile_step, yir::Value *upper,
                                       bool needs_clamp, bool static_bounds, const char *stem) {
        const auto bound_name = [&](std::size_t id) {
            if (!static_bounds) {
                return tile_temp_name(stem, id);
            }
            const std::string base = tile_iv == nullptr || tile_iv->name().empty()
                                         ? "poly.tile"
                                         : tile_iv->name();
            return base + ".end" + (id > 5 ? std::to_string(id) : "");
        };
        auto add = make_parented_op<yir::AddIOp>(
            region, tile_iv, tile_step, bound_name(next_tile_temp_++));
        auto *tile_end = add->result();
        region.operations().push_back(std::move(add));
        if (!needs_clamp) {
            return tile_end;
        }

        auto limit = make_parented_op<yir::VarOp>(
            region, yir::Type::get_i32(), tile_end, bound_name(next_tile_temp_++));
        auto *limit_value = limit->result();
        region.operations().push_back(std::move(limit));

        auto cmp = make_parented_op<yir::ICmpOp>(
            region, yir::ICmpOp::Predicate::Gt, limit_value, upper,
            tile_temp_name("gt", next_tile_temp_++));
        auto clamp = make_parented_op<yir::IfOp>(region, cmp->result());
        clamp->then_region().operations().push_back(
            make_parented_op<yir::AssignOp>(clamp->then_region(), limit_value, upper));
        region.operations().push_back(std::move(cmp));
        region.operations().push_back(std::move(clamp));
        return limit_value;
    }

    bool apply_first_serial_wavefront(
        const std::unordered_map<const yir::Region *, yir::ForOp *> &for_body_owners) {
        for (const auto &scop : model_info_.models) {
            SerialWavefrontCandidate candidate;
            if (find_serial_wavefront_candidate(scop, for_body_owners, candidate) &&
                apply_serial_wavefront(candidate)) {
                ++num_serial_wavefronts_;
                return true;
            }
        }
        return false;
    }

    bool find_serial_wavefront_candidate(
        const PolyScop &scop,
        const std::unordered_map<const yir::Region *, yir::ForOp *> &for_body_owners,
        SerialWavefrontCandidate &candidate) const {
        const PolyStmt *write_stmt = nullptr;
        std::size_t num_reads = 0;
        for (const auto &stmt : scop.statements) {
            if (!stmt.reads.empty()) {
                num_reads += stmt.reads.size();
            }
            if (!stmt.writes.empty()) {
                if (stmt.writes.size() != 1 || write_stmt != nullptr) {
                    return false;
                }
                write_stmt = &stmt;
            }
        }

        if (write_stmt == nullptr || num_reads < 6 || write_stmt->schedule_dims.size() != 3 ||
            write_stmt->loop_bounds.size() != 3 ||
            write_stmt->writes.front().indices.size() != 3) {
            return false;
        }

        const auto &dims = write_stmt->schedule_dims;
        const auto &write = write_stmt->writes.front();
        if (!is_valid_affine_access(write)) {
            return false;
        }
        for (std::size_t dim = 0; dim < 3; ++dim) {
            std::int64_t offset = 0;
            if (!is_dim_plus_constant(write.indices[dim], dims[dim], offset) || offset != 0) {
                return false;
            }
        }

        auto *store = dynamic_cast<yir::ArrayStoreOp *>(
            const_cast<yir::Operation *>(write_stmt->op));
        if (store == nullptr || store->parent() == nullptr) {
            return false;
        }

        auto k_owner = for_body_owners.find(store->parent());
        if (k_owner == for_body_owners.end() || k_owner->second == nullptr ||
            k_owner->second->induction_var() != dims[2]) {
            return false;
        }
        auto *k_loop = k_owner->second;
        auto *j_region = k_loop->parent();
        auto j_owner = for_body_owners.find(j_region);
        if (j_owner == for_body_owners.end() || j_owner->second == nullptr ||
            j_owner->second->induction_var() != dims[1]) {
            return false;
        }
        auto *j_loop = j_owner->second;
        auto *i_region = j_loop->parent();
        auto i_owner = for_body_owners.find(i_region);
        if (i_owner == for_body_owners.end() || i_owner->second == nullptr ||
            i_owner->second->induction_var() != dims[0]) {
            return false;
        }
        auto *i_loop = i_owner->second;

        if (!loop_has_unit_step(*i_loop) || !loop_has_unit_step(*j_loop) ||
            !loop_has_unit_step(*k_loop)) {
            return false;
        }

        for (const auto &stmt : scop.statements) {
            if (stmt.schedule_dims != dims) {
                return false;
            }
            if (stmt.op == nullptr || const_cast<yir::Operation *>(stmt.op)->parent() != store->parent()) {
                return false;
            }
            for (const auto &read : stmt.reads) {
                if (!read_is_wavefront_compatible(read, write.memory, dims)) {
                    return false;
                }
            }
        }

        if (!wavefront_schedule_is_legal(scop,
                                         make_wavefront_schedule_candidate(scop))) {
            return false;
        }

        candidate = {i_loop, j_loop, k_loop, true};
        return true;
    }

    static bool loop_has_unit_step(const yir::ForOp &loop) {
        std::int64_t step = 0;
        return const_i32_value(loop.step(), step) && step == 1;
    }

    static bool read_is_wavefront_compatible(const PolyAccess &read, const yir::Value *memory,
                                             const std::vector<const yir::Value *> &dims) {
        if (read.memory != memory || read.indices.size() != 3 || !is_valid_affine_access(read)) {
            return false;
        }
        std::int64_t offset_sum = 0;
        for (std::size_t dim = 0; dim < 3; ++dim) {
            std::int64_t offset = 0;
            if (!is_dim_plus_constant(read.indices[dim], dims[dim], offset)) {
                return false;
            }
            offset_sum += offset;
        }
        return offset_sum != 0;
    }

    bool apply_serial_wavefront(const SerialWavefrontCandidate &candidate) {
        if (candidate.i_loop == nullptr || candidate.j_loop == nullptr ||
            candidate.k_loop == nullptr || candidate.i_loop->parent() == nullptr ||
            candidate.j_loop->parent() == nullptr || candidate.k_loop->parent() == nullptr) {
            return false;
        }

        auto *outer_parent = candidate.i_loop->parent();
        std::size_t i_loop_index = 0;
        if (!find_operation_index(*outer_parent, *candidate.i_loop, i_loop_index)) {
            return false;
        }

        auto *w_lower = materialize_wave_lower(*outer_parent, i_loop_index, *candidate.i_loop,
                                               *candidate.j_loop, *candidate.k_loop);
        auto *w_upper = materialize_wave_upper(*outer_parent, i_loop_index, *candidate.i_loop,
                                               *candidate.j_loop, *candidate.k_loop);
        if (w_lower == nullptr || w_upper == nullptr) {
            return false;
        }

        auto *w_var = insert_op_before<yir::VarOp>(
            *outer_parent, i_loop_index, yir::Type::get_i32(), w_lower,
            wave_temp_name("w", next_wave_temp_++));

        auto w_loop = std::make_unique<yir::ForOp>(w_var->result(), w_lower, w_upper,
                                                   candidate.i_loop->step());
        w_loop->set_parent(outer_parent);

        auto &outer_ops = outer_parent->operations();
        auto moved_i_loop = std::move(outer_ops[i_loop_index]);
        moved_i_loop->set_parent(&w_loop->body_region());
        w_loop->body_region().operations().push_back(std::move(moved_i_loop));
        outer_ops[i_loop_index] = std::move(w_loop);

        if (!rewrite_k_loop_as_bounded_wavefront(candidate, w_var->result())) {
            return false;
        }

        if (candidate.inner_wave_parallel) {
            candidate.i_loop->set_parallel(true);
            ++num_parallel_wavefronts_;
        }
        if (apply_wavefront_plane_tiling(candidate)) {
            ++num_tiled_;
        }
        return true;
    }

    yir::Value *materialize_wave_lower(yir::Region &region, std::size_t &insert_pos,
                                       const yir::ForOp &i_loop, const yir::ForOp &j_loop,
                                       const yir::ForOp &k_loop) {
        auto *ij = insert_op_before<yir::AddIOp>(
            region, insert_pos, i_loop.lower_bound(), j_loop.lower_bound(),
            wave_temp_name("lb", next_wave_temp_++));
        auto *ijk = insert_op_before<yir::AddIOp>(
            region, insert_pos, ij->result(), k_loop.lower_bound(),
            wave_temp_name("lb", next_wave_temp_++));
        return ijk->result();
    }

    yir::Value *materialize_wave_upper(yir::Region &region, std::size_t &insert_pos,
                                       const yir::ForOp &i_loop, const yir::ForOp &j_loop,
                                       const yir::ForOp &k_loop) {
        auto *ij = insert_op_before<yir::AddIOp>(
            region, insert_pos, i_loop.upper_bound(), j_loop.upper_bound(),
            wave_temp_name("ub", next_wave_temp_++));
        auto *ijk = insert_op_before<yir::AddIOp>(
            region, insert_pos, ij->result(), k_loop.upper_bound(),
            wave_temp_name("ub", next_wave_temp_++));
        auto *minus_two = insert_op_before<yir::ConstI32Op>(
            region, insert_pos, -2, wave_temp_name("c", next_wave_temp_++));
        auto *upper = insert_op_before<yir::AddIOp>(
            region, insert_pos, ijk->result(), minus_two->result(),
            wave_temp_name("ub", next_wave_temp_++));
        return upper->result();
    }

    bool rewrite_k_loop_as_bounded_wavefront(const SerialWavefrontCandidate &candidate,
                                             yir::Value *wave_iv) {
        auto *w_body = candidate.i_loop->parent();
        std::size_t i_loop_index = 0;
        if (w_body == nullptr || !find_operation_index(*w_body, *candidate.i_loop,
                                                       i_loop_index)) {
            return false;
        }

        auto *i_body = candidate.j_loop->parent();
        std::size_t j_loop_index = 0;
        if (i_body == nullptr || i_body != &candidate.i_loop->body_region() ||
            !find_operation_index(*i_body, *candidate.j_loop, j_loop_index)) {
            return false;
        }

        auto *j_body = candidate.k_loop->parent();
        std::size_t k_loop_index = 0;
        if (j_body == nullptr || j_body != &candidate.j_loop->body_region() ||
            !find_operation_index(*j_body, *candidate.k_loop, k_loop_index)) {
            return false;
        }

        auto *i_iv = candidate.i_loop->induction_var();
        auto *j_iv = candidate.j_loop->induction_var();
        auto *k_iv = candidate.k_loop->induction_var();
        auto *i_lower = candidate.i_loop->lower_bound();
        auto *i_upper = candidate.i_loop->upper_bound();
        auto *j_lower = candidate.j_loop->lower_bound();
        auto *j_upper = candidate.j_loop->upper_bound();
        auto *k_lower = candidate.k_loop->lower_bound();
        auto *k_upper = candidate.k_loop->upper_bound();

        std::size_t w_insert_pos = i_loop_index;
        auto *minus_one = insert_op_before<yir::ConstI32Op>(
            *w_body, w_insert_pos, -1, wave_temp_name("c", next_wave_temp_++));
        auto *one = insert_op_before<yir::ConstI32Op>(
            *w_body, w_insert_pos, 1, wave_temp_name("c", next_wave_temp_++));
        auto *j_last = insert_op_before<yir::AddIOp>(
            *w_body, w_insert_pos, j_upper, minus_one->result(),
            wave_temp_name("jlast", next_wave_temp_++));
        auto *k_last = insert_op_before<yir::AddIOp>(
            *w_body, w_insert_pos, k_upper, minus_one->result(),
            wave_temp_name("klast", next_wave_temp_++));

        auto *w_minus_j_last = insert_op_before<yir::SubIOp>(
            *w_body, w_insert_pos, wave_iv, j_last->result(),
            wave_temp_name("sub", next_wave_temp_++));
        auto *i_lower_candidate = insert_op_before<yir::SubIOp>(
            *w_body, w_insert_pos, w_minus_j_last->result(), k_last->result(),
            wave_temp_name("ilb", next_wave_temp_++));
        auto *bounded_i_lower = insert_clamped_wave_lower(
            *w_body, w_insert_pos, i_lower_candidate->result(), i_lower, "ilb", "ilt");

        auto *w_minus_j_lower = insert_op_before<yir::SubIOp>(
            *w_body, w_insert_pos, wave_iv, j_lower,
            wave_temp_name("sub", next_wave_temp_++));
        auto *w_minus_jk_lower = insert_op_before<yir::SubIOp>(
            *w_body, w_insert_pos, w_minus_j_lower->result(), k_lower,
            wave_temp_name("sub", next_wave_temp_++));
        auto *i_upper_candidate = insert_op_before<yir::AddIOp>(
            *w_body, w_insert_pos, w_minus_jk_lower->result(), one->result(),
            wave_temp_name("iub", next_wave_temp_++));
        auto *bounded_i_upper = insert_clamped_wave_upper(
            *w_body, w_insert_pos, i_upper_candidate->result(), i_upper, "iub", "igt");

        candidate.i_loop->operands()[1] = bounded_i_lower;
        candidate.i_loop->operands()[2] = bounded_i_upper;

        auto &w_ops = w_body->operations();
        w_ops.insert(w_ops.begin() + static_cast<std::ptrdiff_t>(w_insert_pos + 1),
                     make_parented_op<yir::AssignOp>(*w_body, i_iv, i_upper));

        std::size_t i_insert_pos = j_loop_index;
        auto *wave_minus_i = insert_op_before<yir::SubIOp>(
            *i_body, i_insert_pos, wave_iv, i_iv,
            wave_temp_name("sub", next_wave_temp_++));
        auto *j_lower_candidate = insert_op_before<yir::SubIOp>(
            *i_body, i_insert_pos, wave_minus_i->result(), k_last->result(),
            wave_temp_name("jlb", next_wave_temp_++));
        auto *bounded_j_lower = insert_clamped_wave_lower(
            *i_body, i_insert_pos, j_lower_candidate->result(), j_lower, "jlb", "jlt");

        auto *w_i_minus_k_lower = insert_op_before<yir::SubIOp>(
            *i_body, i_insert_pos, wave_minus_i->result(), k_lower,
            wave_temp_name("sub", next_wave_temp_++));
        auto *j_upper_candidate = insert_op_before<yir::AddIOp>(
            *i_body, i_insert_pos, w_i_minus_k_lower->result(), one->result(),
            wave_temp_name("jub", next_wave_temp_++));
        auto *bounded_j_upper = insert_clamped_wave_upper(
            *i_body, i_insert_pos, j_upper_candidate->result(), j_upper, "jub", "jgt");

        candidate.j_loop->operands()[1] = bounded_j_lower;
        candidate.j_loop->operands()[2] = bounded_j_upper;

        auto &i_ops = i_body->operations();
        i_ops.insert(i_ops.begin() + static_cast<std::ptrdiff_t>(i_insert_pos + 1),
                     make_parented_op<yir::AssignOp>(*i_body, k_iv, k_upper));
        i_ops.insert(i_ops.begin() + static_cast<std::ptrdiff_t>(i_insert_pos + 2),
                     make_parented_op<yir::AssignOp>(*i_body, j_iv, j_upper));

        std::vector<std::unique_ptr<yir::Operation>> replacements;
        auto compute_k = make_parented_op<yir::SubIOp>(
            *j_body, wave_minus_i->result(), j_iv, wave_temp_name("k", next_wave_temp_++));
        auto *computed_k = compute_k->result();
        replacements.push_back(std::move(compute_k));
        replacements.push_back(make_parented_op<yir::AssignOp>(*j_body, k_iv, computed_k));

        auto &old_body = candidate.k_loop->body_region().operations();
        for (auto &op : old_body) {
            op->set_parent(j_body);
            replacements.push_back(std::move(op));
        }
        old_body.clear();
        replacements.push_back(make_parented_op<yir::AssignOp>(*j_body, k_iv, k_upper));

        auto &j_ops = j_body->operations();
        j_ops.erase(j_ops.begin() + static_cast<std::ptrdiff_t>(k_loop_index));
        for (std::size_t i = 0; i < replacements.size(); ++i) {
            j_ops.insert(j_ops.begin() + static_cast<std::ptrdiff_t>(k_loop_index + i),
                         std::move(replacements[i]));
        }
        return true;
    }

    yir::Value *insert_clamped_wave_lower(yir::Region &region, std::size_t &insert_pos,
                                          yir::Value *candidate, yir::Value *lower,
                                          const char *limit_name, const char *cmp_name) {
        auto *limit = insert_op_before<yir::VarOp>(
            region, insert_pos, yir::Type::get_i32(), candidate,
            wave_temp_name(limit_name, next_wave_temp_++));
        auto *cmp = insert_op_before<yir::ICmpOp>(
            region, insert_pos, yir::ICmpOp::Predicate::Lt, limit->result(), lower,
            wave_temp_name(cmp_name, next_wave_temp_++));
        auto *clamp = insert_op_before<yir::IfOp>(region, insert_pos, cmp->result());
        auto assign = make_parented_op<yir::AssignOp>(
            clamp->then_region(), limit->result(), lower);
        clamp->then_region().operations().push_back(std::move(assign));
        return limit->result();
    }

    yir::Value *insert_clamped_wave_upper(yir::Region &region, std::size_t &insert_pos,
                                          yir::Value *candidate, yir::Value *upper,
                                          const char *limit_name, const char *cmp_name) {
        auto *limit = insert_op_before<yir::VarOp>(
            region, insert_pos, yir::Type::get_i32(), candidate,
            wave_temp_name(limit_name, next_wave_temp_++));
        auto *cmp = insert_op_before<yir::ICmpOp>(
            region, insert_pos, yir::ICmpOp::Predicate::Gt, limit->result(), upper,
            wave_temp_name(cmp_name, next_wave_temp_++));
        auto *clamp = insert_op_before<yir::IfOp>(region, insert_pos, cmp->result());
        auto assign = make_parented_op<yir::AssignOp>(
            clamp->then_region(), limit->result(), upper);
        clamp->then_region().operations().push_back(std::move(assign));
        return limit->result();
    }

    bool apply_wavefront_plane_tiling(const SerialWavefrontCandidate &candidate) {
        constexpr int kWaveTileSize = 8;
        if (!candidate.inner_wave_parallel || candidate.i_loop == nullptr ||
            candidate.j_loop == nullptr || candidate.i_loop->parent() == nullptr ||
            candidate.j_loop->parent() == nullptr ||
            candidate.j_loop->parent() != &candidate.i_loop->body_region()) {
            return false;
        }
        if (!loop_has_unit_step(*candidate.i_loop) || !loop_has_unit_step(*candidate.j_loop)) {
            return false;
        }

        auto *w_body = candidate.i_loop->parent();
        auto *i_body = candidate.j_loop->parent();
        std::size_t i_loop_index = 0;
        std::size_t j_loop_index = 0;
        if (!find_operation_index(*w_body, *candidate.i_loop, i_loop_index) ||
            !find_operation_index(*i_body, *candidate.j_loop, j_loop_index)) {
            return false;
        }

        auto *i_lower = candidate.i_loop->lower_bound();
        auto *i_upper = candidate.i_loop->upper_bound();
        auto *i_step = candidate.i_loop->step();
        auto *j_lower = candidate.j_loop->lower_bound();
        auto *j_upper = candidate.j_loop->upper_bound();
        auto *j_step = candidate.j_loop->step();

        std::size_t insert_pos = i_loop_index;
        auto *tile_step_op = insert_op_before<yir::ConstI32Op>(
            *w_body, insert_pos, kWaveTileSize, wave_temp_name("tile", next_wave_temp_++));
        auto *tile_step = tile_step_op->result();
        auto *i_tile_var = insert_op_before<yir::VarOp>(
            *w_body, insert_pos, yir::Type::get_i32(), i_lower,
            wave_temp_name("it", next_wave_temp_++));

        auto i_tile_loop =
            std::make_unique<yir::ForOp>(i_tile_var->result(), i_lower, i_upper, tile_step);
        i_tile_loop->set_parent(w_body);
        auto i_tile_end = make_parented_op<yir::AddIOp>(
            i_tile_loop->body_region(), i_tile_var->result(), tile_step,
            wave_temp_name("iend", next_wave_temp_++));
        auto *i_tile_end_value = i_tile_end->result();
        i_tile_loop->body_region().operations().push_back(std::move(i_tile_end));
        auto *i_tile_limit = append_clamped_tile_upper(
            i_tile_loop->body_region(), i_tile_end_value, i_upper, "ilim", "igt");

        candidate.i_loop->operands()[1] = i_tile_var->result();
        candidate.i_loop->operands()[2] = i_tile_limit;
        candidate.i_loop->operands()[3] = i_step;

        auto &w_ops = w_body->operations();
        auto moved_i_loop = std::move(w_ops[insert_pos]);
        moved_i_loop->set_parent(&i_tile_loop->body_region());
        i_tile_loop->body_region().operations().push_back(std::move(moved_i_loop));
        w_ops[insert_pos] = std::move(i_tile_loop);

        if (!tile_wavefront_j_loop(*candidate.j_loop, tile_step, j_lower, j_upper, j_step)) {
            return false;
        }
        if (unroll_wavefront_point_loop(*candidate.j_loop)) {
            ++num_wave_unrolls_;
        }
        if (unroll_parallel_wavefront_loop(*candidate.i_loop)) {
            ++num_parallel_wave_unrolls_;
        }

        return true;
    }

    bool tile_wavefront_j_loop(yir::ForOp &j_loop, yir::Value *tile_step,
                               yir::Value *j_lower, yir::Value *j_upper,
                               yir::Value *j_step) {
        auto *i_body = j_loop.parent();
        std::size_t j_loop_index = 0;
        if (i_body == nullptr || tile_step == nullptr ||
            !find_operation_index(*i_body, j_loop, j_loop_index)) {
            return false;
        }

        std::size_t insert_pos = j_loop_index;
        auto *j_tile_var = insert_op_before<yir::VarOp>(
            *i_body, insert_pos, yir::Type::get_i32(), j_lower,
            wave_temp_name("jt", next_wave_temp_++));

        auto j_tile_loop =
            std::make_unique<yir::ForOp>(j_tile_var->result(), j_lower, j_upper, tile_step);
        j_tile_loop->set_parent(i_body);
        auto j_tile_end = make_parented_op<yir::AddIOp>(
            j_tile_loop->body_region(), j_tile_var->result(), tile_step,
            wave_temp_name("jend", next_wave_temp_++));
        auto *j_tile_end_value = j_tile_end->result();
        j_tile_loop->body_region().operations().push_back(std::move(j_tile_end));
        auto *j_tile_limit = append_clamped_tile_upper(
            j_tile_loop->body_region(), j_tile_end_value, j_upper, "jlim", "jgt");

        j_loop.operands()[1] = j_tile_var->result();
        j_loop.operands()[2] = j_tile_limit;
        j_loop.operands()[3] = j_step;

        auto &i_ops = i_body->operations();
        auto moved_j_loop = std::move(i_ops[insert_pos]);
        moved_j_loop->set_parent(&j_tile_loop->body_region());
        j_tile_loop->body_region().operations().push_back(std::move(moved_j_loop));
        i_ops[insert_pos] = std::move(j_tile_loop);

        return true;
    }

    bool unroll_wavefront_point_loop(yir::ForOp &loop) {
        constexpr int kWaveUnrollFactor = 2;
        constexpr std::size_t kMaxWaveUnrolledOps = 192;

        auto *parent = loop.parent();
        std::size_t loop_index = 0;
        if (parent == nullptr || !find_operation_index(*parent, loop, loop_index)) {
            return false;
        }

        std::int64_t step = 0;
        if (!const_i32_value(loop.step(), step) || step != 1 ||
            !is_wave_unroll_safe(loop.body_region())) {
            return false;
        }

        const std::size_t original_count = loop.body_region().operations().size();
        const std::size_t recursive_count = wave_unroll_operation_count(loop.body_region());
        if (original_count == 0 ||
            recursive_count * static_cast<std::size_t>(kWaveUnrollFactor) >
                kMaxWaveUnrolledOps) {
            return false;
        }

        std::size_t insert_pos = loop_index;
        auto *unroll_step = insert_op_before<yir::ConstI32Op>(
            *parent, insert_pos, kWaveUnrollFactor,
            wave_temp_name("unroll.step", next_wave_temp_++));
        loop.operands()[3] = unroll_step->result();

        for (int lane = 1; lane < kWaveUnrollFactor; ++lane) {
            auto lane_const = make_parented_op<yir::ConstI32Op>(
                loop.body_region(), lane, wave_temp_name("lane", next_wave_temp_++));
            auto *lane_const_value = lane_const->result();
            loop.body_region().operations().push_back(std::move(lane_const));

            auto lane_iv = make_parented_op<yir::AddIOp>(
                loop.body_region(), loop.induction_var(), lane_const_value,
                wave_temp_name("iv", next_wave_temp_++));
            auto *lane_iv_value = lane_iv->result();
            loop.body_region().operations().push_back(std::move(lane_iv));

            auto lane_in_bounds = make_parented_op<yir::ICmpOp>(
                loop.body_region(), yir::ICmpOp::Predicate::Lt, lane_iv_value,
                loop.upper_bound(), wave_temp_name("lane.lt", next_wave_temp_++));
            auto *lane_in_bounds_value = lane_in_bounds->result();
            loop.body_region().operations().push_back(std::move(lane_in_bounds));

            auto lane_if =
                make_parented_op<yir::IfOp>(loop.body_region(), lane_in_bounds_value);
            ValueMap map;
            map[loop.induction_var()] = lane_iv_value;
            if (!clone_wave_unroll_prefix_into(loop.body_region(), lane_if->then_region(),
                                               std::move(map), original_count)) {
                return false;
            }
            loop.body_region().operations().push_back(std::move(lane_if));
        }

        return true;
    }

    bool unroll_parallel_wavefront_loop(yir::ForOp &loop) {
        constexpr int kParallelWaveUnrollFactor = 2;
        constexpr std::size_t kMaxParallelWaveUnrolledOps = 512;

        auto *parent = loop.parent();
        std::size_t loop_index = 0;
        if (parent == nullptr || !loop.is_parallel() ||
            !find_operation_index(*parent, loop, loop_index)) {
            return false;
        }

        std::int64_t step = 0;
        if (!const_i32_value(loop.step(), step) || step != 1 ||
            !is_parallel_wave_unroll_safe(loop.body_region())) {
            return false;
        }

        const std::size_t original_count = loop.body_region().operations().size();
        const std::size_t recursive_count = wave_unroll_operation_count(loop.body_region());
        if (original_count == 0 ||
            recursive_count * static_cast<std::size_t>(kParallelWaveUnrollFactor) >
                kMaxParallelWaveUnrolledOps) {
            return false;
        }

        std::size_t insert_pos = loop_index;
        auto *unroll_step = insert_op_before<yir::ConstI32Op>(
            *parent, insert_pos, kParallelWaveUnrollFactor,
            wave_temp_name("par.unroll.step", next_wave_temp_++));
        loop.operands()[3] = unroll_step->result();

        for (int lane = 1; lane < kParallelWaveUnrollFactor; ++lane) {
            auto lane_const = make_parented_op<yir::ConstI32Op>(
                loop.body_region(), lane, wave_temp_name("par.lane", next_wave_temp_++));
            auto *lane_const_value = lane_const->result();
            loop.body_region().operations().push_back(std::move(lane_const));

            auto lane_iv = make_parented_op<yir::AddIOp>(
                loop.body_region(), loop.induction_var(), lane_const_value,
                wave_temp_name("par.iv", next_wave_temp_++));
            auto *lane_iv_value = lane_iv->result();
            loop.body_region().operations().push_back(std::move(lane_iv));

            auto lane_in_bounds = make_parented_op<yir::ICmpOp>(
                loop.body_region(), yir::ICmpOp::Predicate::Lt, lane_iv_value,
                loop.upper_bound(), wave_temp_name("par.lane.lt", next_wave_temp_++));
            auto *lane_in_bounds_value = lane_in_bounds->result();
            loop.body_region().operations().push_back(std::move(lane_in_bounds));

            auto lane_if =
                make_parented_op<yir::IfOp>(loop.body_region(), lane_in_bounds_value);
            ValueMap map;
            map[loop.induction_var()] = lane_iv_value;
            if (!clone_wave_unroll_prefix_into(loop.body_region(), lane_if->then_region(),
                                               std::move(map), original_count)) {
                return false;
            }
            loop.body_region().operations().push_back(std::move(lane_if));
        }

        return true;
    }

    yir::Value *append_clamped_tile_upper(yir::Region &region, yir::Value *tile_end,
                                          yir::Value *upper, const char *limit_name,
                                          const char *cmp_name) {
        auto limit = make_parented_op<yir::VarOp>(
            region, yir::Type::get_i32(), tile_end, wave_temp_name(limit_name, next_wave_temp_++));
        auto *limit_value = limit->result();
        region.operations().push_back(std::move(limit));

        auto cmp = make_parented_op<yir::ICmpOp>(
            region, yir::ICmpOp::Predicate::Gt, limit_value, upper,
            wave_temp_name(cmp_name, next_wave_temp_++));
        auto clamp = make_parented_op<yir::IfOp>(region, cmp->result());
        auto assign = make_parented_op<yir::AssignOp>(clamp->then_region(), limit_value, upper);
        clamp->then_region().operations().push_back(std::move(assign));

        region.operations().push_back(std::move(cmp));
        region.operations().push_back(std::move(clamp));
        return limit_value;
    }

    template <typename OpT, typename... Args>
    std::unique_ptr<OpT> make_parented_op(yir::Region &parent, Args &&...args) {
        auto op = std::make_unique<OpT>(std::forward<Args>(args)...);
        op->set_parent(&parent);
        return op;
    }

    static EnclosingLoopNest collect_enclosing_loop_nest(
        yir::Region *region,
        const std::unordered_map<const yir::Region *, yir::ForOp *> &for_body_owners) {
        EnclosingLoopNest nest;
        auto *current = region;
        while (current != nullptr) {
            auto owner = for_body_owners.find(current);
            if (owner == for_body_owners.end() || owner->second == nullptr) {
                break;
            }
            nest.outer_to_inner.push_back(owner->second);
            current = owner->second->parent();
        }
        std::reverse(nest.outer_to_inner.begin(), nest.outer_to_inner.end());
        return nest;
    }

    static bool loop_nest_matches_stmt(const EnclosingLoopNest &nest, const PolyStmt &stmt) {
        if (nest.outer_to_inner.size() != stmt.schedule_dims.size() ||
            stmt.loop_bounds.size() != stmt.schedule_dims.size()) {
            return false;
        }
        for (std::size_t i = 0; i < stmt.schedule_dims.size(); ++i) {
            auto *loop = nest.outer_to_inner[i];
            if (loop == nullptr || loop->induction_var() != stmt.schedule_dims[i] ||
                stmt.loop_bounds[i].iv != stmt.schedule_dims[i]) {
                return false;
            }
            std::int64_t step = 0;
            if (!const_i32_value(loop->step(), step) || step != 1) {
                return false;
            }
        }
        return true;
    }

    static bool simple_initializer_covers_unit_interior(const PolyStmt &init_stmt,
                                                       const PolyStmt &update_stmt) {
        if (init_stmt.loop_bounds.size() != update_stmt.loop_bounds.size() ||
            init_stmt.schedule_dims != update_stmt.schedule_dims) {
            return false;
        }

        for (std::size_t dim = 0; dim < init_stmt.loop_bounds.size(); ++dim) {
            std::int64_t init_lower = 0;
            std::int64_t update_lower = 0;
            if (!affine_constant_value(init_stmt.loop_bounds[dim].lower, init_lower) ||
                !affine_constant_value(update_stmt.loop_bounds[dim].lower, update_lower) ||
                update_lower - init_lower != 1) {
                return false;
            }

            if (!affine_terms_equal_with_constant_delta(update_stmt.loop_bounds[dim].upper,
                                                        init_stmt.loop_bounds[dim].upper, -1)) {
                return false;
            }
        }
        return true;
    }

    static bool same_outer_parent_ordered(const EnclosingLoopNest &init_loops,
                                          const EnclosingLoopNest &update_loops,
                                          const yir::Value *memory) {
        if (init_loops.outer_to_inner.empty() || update_loops.outer_to_inner.empty()) {
            return false;
        }
        auto *init_outer = init_loops.outer_to_inner.front();
        auto *update_outer = update_loops.outer_to_inner.front();
        if (init_outer == nullptr || update_outer == nullptr || init_outer == update_outer ||
            init_outer->parent() == nullptr || init_outer->parent() != update_outer->parent()) {
            return false;
        }

        auto *parent = init_outer->parent();
        std::size_t init_index = 0;
        std::size_t update_index = 0;
        if (!find_operation_index(*parent, *init_outer, init_index) ||
            !find_operation_index(*parent, *update_outer, update_index) ||
            init_index >= update_index) {
            return false;
        }

        const auto &ops = parent->operations();
        for (std::size_t i = init_index + 1; i < update_index; ++i) {
            if (operation_contains_call(*ops[i]) || operation_writes_memory(*ops[i], memory)) {
                return false;
            }
        }
        return true;
    }

    bool is_constant_one_initialization(
        const PolyStmt &stmt,
        const std::unordered_map<const yir::Region *, yir::ForOp *> &for_body_owners,
        ConstantInitialization &init) const {
        if (!stmt.reads.empty() || stmt.writes.size() != 1 ||
            stmt.schedule_dims.empty() || stmt.loop_bounds.size() != stmt.schedule_dims.size()) {
            return false;
        }

        auto *store =
            dynamic_cast<yir::ArrayStoreOp *>(const_cast<yir::Operation *>(stmt.op));
        std::int64_t value = 0;
        if (store == nullptr || store->parent() == nullptr || store->array() != stmt.writes[0].memory ||
            !const_i32_value(store->value(), value) || value != 1 ||
            !is_identity_access_for_dims(stmt.writes[0], stmt.schedule_dims)) {
            return false;
        }

        auto loops = collect_enclosing_loop_nest(store->parent(), for_body_owners);
        if (!loop_nest_matches_stmt(loops, stmt)) {
            return false;
        }

        init = ConstantInitialization{&stmt, store, std::move(loops)};
        return true;
    }

    const PolyStmt *find_single_identity_update_store(
        const PolyScop &scop, const yir::Value *memory,
        const std::vector<const yir::Value *> &dims,
        const std::unordered_map<const yir::Region *, yir::ForOp *> &for_body_owners,
        EnclosingLoopNest &update_loops) const {
        const PolyStmt *write_stmt = nullptr;
        for (const auto &stmt : scop.statements) {
            for (const auto &write : stmt.writes) {
                if (write.memory != memory) {
                    continue;
                }
                if (write_stmt != nullptr || stmt.writes.size() != 1 ||
                    stmt.schedule_dims != dims || !is_identity_access_for_dims(write, dims)) {
                    return nullptr;
                }

                auto *store =
                    dynamic_cast<yir::ArrayStoreOp *>(const_cast<yir::Operation *>(stmt.op));
                if (store == nullptr || store->parent() == nullptr || store->array() != memory) {
                    return nullptr;
                }
                auto loops = collect_enclosing_loop_nest(store->parent(), for_body_owners);
                if (!loop_nest_matches_stmt(loops, stmt)) {
                    return nullptr;
                }
                update_loops = std::move(loops);
                write_stmt = &stmt;
            }
        }
        return write_stmt;
    }

    bool prove_future_neighbor_candidate(
        const PolyScop &scop, const ConstantInitialization &init,
        const std::unordered_map<const yir::Region *, yir::ForOp *> &for_body_owners,
        FutureNeighborProof &proof) const {
        if (init.stmt == nullptr || init.store == nullptr || init.stmt->writes.size() != 1) {
            return false;
        }

        const auto *memory = init.stmt->writes[0].memory;
        EnclosingLoopNest update_loops;
        const auto *write_stmt = find_single_identity_update_store(
            scop, memory, init.stmt->schedule_dims, for_body_owners, update_loops);
        if (write_stmt == nullptr ||
            !simple_initializer_covers_unit_interior(*init.stmt, *write_stmt) ||
            !same_outer_parent_ordered(init.loops, update_loops, memory)) {
            return false;
        }

        auto *store =
            dynamic_cast<yir::ArrayStoreOp *>(const_cast<yir::Operation *>(write_stmt->op));
        if (store == nullptr || store->parent() == nullptr) {
            return false;
        }

        proof = FutureNeighborProof{write_stmt, store, std::move(update_loops)};
        return true;
    }

    static bool is_nonpositive_unit_neighbor_read(
        const PolyAccess &read, const std::vector<const yir::Value *> &dims) {
        if (read.indices.size() != dims.size() || !is_valid_affine_access(read)) {
            return false;
        }

        bool saw_negative_offset = false;
        for (std::size_t dim = 0; dim < dims.size(); ++dim) {
            std::int64_t offset = 0;
            if (!is_dim_plus_constant(read.indices[dim], dims[dim], offset)) {
                return false;
            }
            if (offset > 0 || offset < -1) {
                return false;
            }
            saw_negative_offset = saw_negative_offset || offset == -1;
        }
        return saw_negative_offset;
    }

    static bool assign_resets_value_without_reading_old_value(const yir::Operation &op,
                                                              const yir::Value *value) {
        auto *assign = dynamic_cast<const yir::AssignOp *>(&op);
        return assign != nullptr && assign->target() == value &&
               !value_depends_on_value(assign->value(), value);
    }

    static bool induction_vars_reset_before_update(const EnclosingLoopNest &init_loops,
                                                   const EnclosingLoopNest &update_loops) {
        if (init_loops.outer_to_inner.empty() || update_loops.outer_to_inner.empty()) {
            return false;
        }

        auto *init_outer = init_loops.outer_to_inner.front();
        auto *update_outer = update_loops.outer_to_inner.front();
        if (init_outer == nullptr || update_outer == nullptr || init_outer->parent() == nullptr ||
            init_outer->parent() != update_outer->parent()) {
            return false;
        }

        auto *parent = init_outer->parent();
        std::size_t init_index = 0;
        std::size_t update_index = 0;
        if (!find_operation_index(*parent, *init_outer, init_index) ||
            !find_operation_index(*parent, *update_outer, update_index) ||
            init_index >= update_index) {
            return false;
        }

        std::vector<const yir::Value *> vars;
        vars.reserve(init_loops.outer_to_inner.size());
        for (auto *loop : init_loops.outer_to_inner) {
            if (loop == nullptr || loop->induction_var() == nullptr) {
                return false;
            }
            vars.push_back(loop->induction_var());
        }

        std::vector<bool> reset(vars.size(), false);
        const auto &ops = parent->operations();
        for (std::size_t i = init_index + 1; i < update_index; ++i) {
            for (std::size_t var_index = 0; var_index < vars.size(); ++var_index) {
                if (reset[var_index]) {
                    continue;
                }

                const auto *var = vars[var_index];
                if (assign_resets_value_without_reading_old_value(*ops[i], var)) {
                    reset[var_index] = true;
                    continue;
                }
                if (operation_uses_value(*ops[i], var)) {
                    return false;
                }
            }
        }

        return std::all_of(reset.begin(), reset.end(), [](bool value) { return value; });
    }

    static bool no_intervening_memory_observation(const EnclosingLoopNest &init_loops,
                                                  const EnclosingLoopNest &update_loops,
                                                  const yir::Value *memory) {
        if (init_loops.outer_to_inner.empty() || update_loops.outer_to_inner.empty()) {
            return false;
        }

        auto *init_outer = init_loops.outer_to_inner.front();
        auto *update_outer = update_loops.outer_to_inner.front();
        if (init_outer == nullptr || update_outer == nullptr || init_outer->parent() == nullptr ||
            init_outer->parent() != update_outer->parent()) {
            return false;
        }

        auto *parent = init_outer->parent();
        std::size_t init_index = 0;
        std::size_t update_index = 0;
        if (!find_operation_index(*parent, *init_outer, init_index) ||
            !find_operation_index(*parent, *update_outer, update_index) ||
            init_index >= update_index) {
            return false;
        }

        const auto &ops = parent->operations();
        for (std::size_t i = init_index + 1; i < update_index; ++i) {
            if (operation_observes_memory_except_writes(*ops[i], memory)) {
                return false;
            }
        }
        return true;
    }

    bool collect_future_neighbor_constants_from_scop(
        const PolyScop &scop, const ConstantInitialization &init,
        const std::unordered_map<const yir::Region *, yir::ForOp *> &for_body_owners,
        std::unordered_set<yir::ArrayLoadOp *> &seen_loads,
        std::vector<FutureNeighborConstant> &candidates) const {
        FutureNeighborProof proof;
        if (!prove_future_neighbor_candidate(scop, init, for_body_owners, proof)) {
            return false;
        }

        const auto *memory = init.stmt->writes[0].memory;
        const auto *write_stmt = proof.write_stmt;
        auto *store = proof.store;

        bool found = false;
        for (const auto &stmt : scop.statements) {
            if (stmt.reads.size() != 1 || !stmt.writes.empty() ||
                stmt.schedule_dims != write_stmt->schedule_dims ||
                !same_iteration_domain(*write_stmt, stmt) ||
                stmt.lexical_id >= write_stmt->lexical_id) {
                continue;
            }

            const auto &read = stmt.reads.front();
            if (read.memory != memory ||
                !is_single_unit_future_neighbor(read, write_stmt->schedule_dims)) {
                continue;
            }

            auto *load =
                dynamic_cast<yir::ArrayLoadOp *>(const_cast<yir::Operation *>(stmt.op));
            if (load == nullptr || load->parent() == nullptr || load->array() != memory ||
                load->parent() != store->parent() || !seen_loads.insert(load).second) {
                continue;
            }

            std::size_t load_index = 0;
            std::size_t store_index = 0;
            if (!find_operation_index(*load->parent(), *load, load_index) ||
                !find_operation_index(*store->parent(), *store, store_index) ||
                load_index >= store_index) {
                continue;
            }

            candidates.push_back({load, 1});
            found = true;
        }
        return found;
    }

    std::vector<FutureNeighborConstant> find_future_neighbor_constants(
        const std::unordered_map<const yir::Region *, yir::ForOp *> &for_body_owners) const {
        std::vector<FutureNeighborConstant> candidates;
        std::unordered_map<const yir::Value *, ConstantInitialization> available_inits;
        std::unordered_set<yir::ArrayLoadOp *> seen_loads;

        for (const auto &scop : model_info_.models) {
            for (const auto &[_, init] : available_inits) {
                collect_future_neighbor_constants_from_scop(scop, init, for_body_owners,
                                                            seen_loads, candidates);
            }

            std::unordered_map<const yir::Value *, std::size_t> write_counts;
            for (const auto &stmt : scop.statements) {
                for (const auto &write : stmt.writes) {
                    if (write.memory != nullptr) {
                        ++write_counts[write.memory];
                    }
                }
            }
            for (const auto &[memory, _] : write_counts) {
                available_inits.erase(memory);
            }

            for (const auto &stmt : scop.statements) {
                if (stmt.writes.size() != 1 || write_counts[stmt.writes.front().memory] != 1) {
                    continue;
                }
                ConstantInitialization init;
                if (is_constant_one_initialization(stmt, for_body_owners, init)) {
                    available_inits[stmt.writes.front().memory] = std::move(init);
                }
            }
        }

        return candidates;
    }

    bool collect_initialization_reduction_from_scop(
        const PolyScop &scop, const ConstantInitialization &init,
        const std::unordered_map<const yir::Region *, yir::ForOp *> &for_body_owners,
        InitializationReduction &reduction) const {
        FutureNeighborProof proof;
        if (!prove_future_neighbor_candidate(scop, init, for_body_owners, proof) ||
            init.loops.outer_to_inner.size() != 3 ||
            proof.write_stmt == nullptr || proof.write_stmt->schedule_dims.size() != 3 ||
            proof.store == nullptr || proof.store->parent() == nullptr ||
            !induction_vars_reset_before_update(init.loops, proof.update_loops) ||
            !no_intervening_memory_observation(init.loops, proof.update_loops,
                                               init.stmt->writes[0].memory)) {
            return false;
        }

        const auto *memory = init.stmt->writes[0].memory;
        const auto &dims = proof.write_stmt->schedule_dims;
        std::vector<bool> covered_future_dims(dims.size(), false);
        std::unordered_set<yir::ArrayLoadOp *> seen_required_loads;
        std::vector<yir::ArrayLoadOp *> required_future_loads;

        std::size_t store_index = 0;
        if (!find_operation_index(*proof.store->parent(), *proof.store, store_index)) {
            return false;
        }

        for (const auto &stmt : scop.statements) {
            if (stmt.reads.empty()) {
                continue;
            }

            bool reads_target_memory = false;
            for (const auto &read : stmt.reads) {
                reads_target_memory = reads_target_memory || read.memory == memory;
            }
            if (!reads_target_memory) {
                continue;
            }

            if (stmt.reads.size() != 1 || !stmt.writes.empty() ||
                stmt.schedule_dims != dims || !same_iteration_domain(*proof.write_stmt, stmt) ||
                stmt.lexical_id >= proof.write_stmt->lexical_id) {
                return false;
            }

            const auto &read = stmt.reads.front();
            auto *load =
                dynamic_cast<yir::ArrayLoadOp *>(const_cast<yir::Operation *>(stmt.op));
            if (load == nullptr || load->parent() == nullptr || load->array() != memory ||
                load->parent() != proof.store->parent()) {
                return false;
            }

            std::size_t load_index = 0;
            if (!find_operation_index(*load->parent(), *load, load_index) ||
                load_index >= store_index) {
                return false;
            }

            std::size_t future_dim = 0;
            if (single_unit_future_neighbor_dim(read, dims, future_dim)) {
                covered_future_dims[future_dim] = true;
                if (seen_required_loads.insert(load).second) {
                    required_future_loads.push_back(load);
                }
                continue;
            }

            if (!is_nonpositive_unit_neighbor_read(read, dims)) {
                return false;
            }
        }

        if (!std::all_of(covered_future_dims.begin(), covered_future_dims.end(),
                         [](bool covered) { return covered; }) ||
            required_future_loads.empty()) {
            return false;
        }

        reduction = InitializationReduction{init, std::move(required_future_loads),
                                            std::move(proof)};
        return true;
    }

    std::vector<InitializationReduction> find_initialization_reductions(
        const std::unordered_map<const yir::Region *, yir::ForOp *> &for_body_owners) const {
        std::vector<InitializationReduction> reductions;
        std::unordered_map<const yir::Value *, ConstantInitialization> available_inits;
        std::unordered_set<yir::ForOp *> seen_init_loops;

        for (const auto &scop : model_info_.models) {
            for (const auto &[_, init] : available_inits) {
                if (init.loops.outer_to_inner.empty() ||
                    seen_init_loops.find(init.loops.outer_to_inner.front()) !=
                        seen_init_loops.end()) {
                    continue;
                }

                InitializationReduction reduction;
                if (collect_initialization_reduction_from_scop(scop, init, for_body_owners,
                                                               reduction)) {
                    seen_init_loops.insert(init.loops.outer_to_inner.front());
                    reductions.push_back(std::move(reduction));
                }
            }

            std::unordered_map<const yir::Value *, std::size_t> write_counts;
            for (const auto &stmt : scop.statements) {
                for (const auto &write : stmt.writes) {
                    if (write.memory != nullptr) {
                        ++write_counts[write.memory];
                    }
                }
            }
            for (const auto &[memory, _] : write_counts) {
                available_inits.erase(memory);
            }

            for (const auto &stmt : scop.statements) {
                if (stmt.writes.size() != 1 || write_counts[stmt.writes.front().memory] != 1) {
                    continue;
                }
                ConstantInitialization init;
                if (is_constant_one_initialization(stmt, for_body_owners, init)) {
                    available_inits[stmt.writes.front().memory] = std::move(init);
                }
            }
        }

        return reductions;
    }

    static bool init_scaffold_region_is_safe(
        const yir::Region &region, const yir::ArrayStoreOp *target_store,
        const yir::Value *target_memory,
        const std::unordered_set<const yir::ForOp *> &allowed_loops,
        const std::unordered_set<const yir::Value *> &allowed_assigned_values,
        bool &saw_target_store) {
        for (const auto &op : region.operations()) {
            if (op.get() == target_store) {
                auto *store = dynamic_cast<const yir::ArrayStoreOp *>(op.get());
                if (store == nullptr || store->array() != target_memory || saw_target_store) {
                    return false;
                }
                saw_target_store = true;
                continue;
            }

            if (auto *store = dynamic_cast<const yir::ArrayStoreOp *>(op.get())) {
                (void)store;
                return false;
            }

            if (auto *assign = dynamic_cast<const yir::AssignOp *>(op.get())) {
                if (allowed_assigned_values.find(assign->target()) ==
                    allowed_assigned_values.end()) {
                    return false;
                }
                continue;
            }

            if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
                if (allowed_loops.find(for_op) == allowed_loops.end() ||
                    !init_scaffold_region_is_safe(for_op->body_region(), target_store,
                                                  target_memory, allowed_loops,
                                                  allowed_assigned_values,
                                                  saw_target_store)) {
                    return false;
                }
                continue;
            }

            return false;
        }
        return true;
    }

    static bool init_loop_is_reducible_now(const ConstantInitialization &init) {
        if (init.stmt == nullptr || init.store == nullptr || init.stmt->writes.size() != 1 ||
            init.loops.outer_to_inner.size() != 3) {
            return false;
        }

        std::unordered_set<const yir::ForOp *> allowed_loops;
        std::unordered_set<const yir::Value *> allowed_assigned_values;
        for (auto *loop : init.loops.outer_to_inner) {
            if (loop == nullptr || loop->parent() == nullptr || loop->induction_var() == nullptr) {
                return false;
            }
            allowed_loops.insert(loop);
            allowed_assigned_values.insert(loop->induction_var());
        }

        auto *outer = init.loops.outer_to_inner.front();
        bool saw_target_store = false;
        return init_scaffold_region_is_safe(outer->body_region(), init.store,
                                            init.stmt->writes.front().memory,
                                            allowed_loops, allowed_assigned_values,
                                            saw_target_store) &&
               saw_target_store;
    }

    std::unique_ptr<yir::Operation> make_boundary_face_loop(
        yir::Region &parent, const ConstantInitialization &init, yir::Value *store_value,
        std::size_t fixed_dim, yir::Value *fixed_value,
        std::size_t outer_dim, std::size_t inner_dim) {
        auto *memory = init.stmt->writes.front().memory;
        auto *outer_source = init.loops.outer_to_inner[outer_dim];
        auto *inner_source = init.loops.outer_to_inner[inner_dim];

        auto outer_loop = std::make_unique<yir::ForOp>(
            outer_source->induction_var(), outer_source->lower_bound(),
            outer_source->upper_bound(), outer_source->step());
        outer_loop->set_parent(&parent);

        auto inner_loop = std::make_unique<yir::ForOp>(
            inner_source->induction_var(), inner_source->lower_bound(),
            inner_source->upper_bound(), inner_source->step());
        inner_loop->set_parent(&outer_loop->body_region());

        std::vector<yir::Value *> indices(init.loops.outer_to_inner.size(), nullptr);
        indices[fixed_dim] = fixed_value;
        indices[outer_dim] = outer_source->induction_var();
        indices[inner_dim] = inner_source->induction_var();

        auto store = make_parented_op<yir::ArrayStoreOp>(
            inner_loop->body_region(), store_value, const_cast<yir::Value *>(memory),
            std::move(indices));
        inner_loop->body_region().operations().push_back(std::move(store));
        outer_loop->body_region().operations().push_back(std::move(inner_loop));
        return outer_loop;
    }

    bool apply_initialization_reduction(
        const InitializationReduction &reduction,
        const std::unordered_set<yir::ArrayLoadOp *> &future_erased_loads) {
        for (auto *load : reduction.required_future_loads) {
            if (load == nullptr || future_erased_loads.find(load) == future_erased_loads.end()) {
                return false;
            }
        }

        const auto &init = reduction.init;
        if (!init_loop_is_reducible_now(init)) {
            return false;
        }

        auto *outer = init.loops.outer_to_inner.front();
        auto *parent = outer->parent();
        std::size_t outer_index = 0;
        if (parent == nullptr || !find_operation_index(*parent, *outer, outer_index)) {
            return false;
        }

        std::size_t insert_pos = outer_index;
        auto *store_value = insert_op_before<yir::ConstI32Op>(
            *parent, insert_pos, 1, init_temp_name("c", next_init_temp_++))->result();
        auto *i_last = insert_op_before<yir::SubIOp>(
            *parent, insert_pos, init.loops.outer_to_inner[0]->upper_bound(),
            init.loops.outer_to_inner[0]->step(),
            init_temp_name("ilast", next_init_temp_++))->result();
        auto *j_last = insert_op_before<yir::SubIOp>(
            *parent, insert_pos, init.loops.outer_to_inner[1]->upper_bound(),
            init.loops.outer_to_inner[1]->step(),
            init_temp_name("jlast", next_init_temp_++))->result();
        auto *k_last = insert_op_before<yir::SubIOp>(
            *parent, insert_pos, init.loops.outer_to_inner[2]->upper_bound(),
            init.loops.outer_to_inner[2]->step(),
            init_temp_name("klast", next_init_temp_++))->result();

        std::vector<std::unique_ptr<yir::Operation>> replacements;
        replacements.push_back(make_boundary_face_loop(
            *parent, init, store_value, 0, init.loops.outer_to_inner[0]->lower_bound(), 1, 2));
        replacements.push_back(make_boundary_face_loop(
            *parent, init, store_value, 0, i_last, 1, 2));
        replacements.push_back(make_boundary_face_loop(
            *parent, init, store_value, 1, init.loops.outer_to_inner[1]->lower_bound(), 0, 2));
        replacements.push_back(make_boundary_face_loop(
            *parent, init, store_value, 1, j_last, 0, 2));
        replacements.push_back(make_boundary_face_loop(
            *parent, init, store_value, 2, init.loops.outer_to_inner[2]->lower_bound(), 0, 1));
        replacements.push_back(make_boundary_face_loop(
            *parent, init, store_value, 2, k_last, 0, 1));

        auto &ops = parent->operations();
        ops.erase(ops.begin() + static_cast<std::ptrdiff_t>(insert_pos));
        for (std::size_t i = 0; i < replacements.size(); ++i) {
            ops.insert(ops.begin() + static_cast<std::ptrdiff_t>(insert_pos + i),
                       std::move(replacements[i]));
        }

        ++num_initialization_reductions_;
        return true;
    }

    bool apply_initialization_reductions(
        const std::vector<InitializationReduction> &reductions,
        const std::unordered_set<yir::ArrayLoadOp *> &future_erased_loads) {
        bool changed = false;
        std::unordered_set<yir::ForOp *> erased_init_loops;
        for (const auto &reduction : reductions) {
            auto *outer = reduction.init.loops.outer_to_inner.empty()
                              ? nullptr
                              : reduction.init.loops.outer_to_inner.front();
            if (outer == nullptr || erased_init_loops.find(outer) != erased_init_loops.end()) {
                continue;
            }
            if (apply_initialization_reduction(reduction, future_erased_loads)) {
                erased_init_loops.insert(outer);
                changed = true;
            }
        }
        return changed;
    }

    bool replace_load_with_constant(yir::ArrayLoadOp &load, int value) {
        auto *parent = load.parent();
        if (parent == nullptr || load.result() == nullptr) {
            return false;
        }

        std::size_t load_index = 0;
        if (!find_operation_index(*parent, load, load_index)) {
            return false;
        }

        std::size_t insert_pos = load_index;
        auto *constant = insert_op_before<yir::ConstI32Op>(
            *parent, insert_pos, value, future_temp_name("c", next_future_temp_++));
        auto *old_value = load.result();
        replace_value_after(*parent, load_index + 2, old_value, constant->result());
        parent->operations().erase(parent->operations().begin() +
                                   static_cast<std::ptrdiff_t>(load_index + 1));
        ++num_future_neighbor_constants_;
        return true;
    }

    bool replace_future_neighbor_constants(
        const std::vector<FutureNeighborConstant> &candidates,
        std::unordered_set<yir::ArrayLoadOp *> &erased_loads) {
        bool changed = false;
        for (const auto &candidate : candidates) {
            if (candidate.load == nullptr || erased_loads.find(candidate.load) != erased_loads.end()) {
                continue;
            }
            if (replace_load_with_constant(*candidate.load, candidate.value)) {
                erased_loads.insert(candidate.load);
                changed = true;
            }
        }
        return changed;
    }

    std::vector<StencilCarryCandidate> find_stencil_carry_candidates(
        const PolyScop &scop,
        const std::unordered_map<const yir::Region *, yir::ForOp *> &for_body_owners) {
        std::vector<StencilCarryCandidate> candidates;
        std::unordered_set<yir::ArrayLoadOp *> seen_loads;

        for (const auto &dep : dep_info_.dependences) {
            if (dep.kind != PolyDependence::Kind::RAW ||
                dep.distance_kind != PolyDependence::DistanceKind::LoopCarriedConstant ||
                !is_innermost_unit_distance(dep.distance)) {
                continue;
            }

            const auto *write_stmt = find_stmt_by_id(scop, dep.source_stmt_id);
            const auto *read_stmt = find_stmt_by_id(scop, dep.target_stmt_id);
            if (write_stmt == nullptr || read_stmt == nullptr || write_stmt->writes.size() != 1 ||
                read_stmt->reads.size() != 1 || !same_iteration_domain(*write_stmt, *read_stmt)) {
                continue;
            }

            const auto &write = write_stmt->writes.front();
            const auto &read = read_stmt->reads.front();
            if (!is_valid_affine_access(write) || !is_valid_affine_access(read) ||
                write.memory != dep.memory || read.memory != dep.memory) {
                continue;
            }

            auto *load = dynamic_cast<yir::ArrayLoadOp *>(
                const_cast<yir::Operation *>(read_stmt->op));
            auto *store = dynamic_cast<yir::ArrayStoreOp *>(
                const_cast<yir::Operation *>(write_stmt->op));
            if (load == nullptr || store == nullptr || load->parent() == nullptr ||
                load->parent() != store->parent()) {
                continue;
            }

            auto owner = for_body_owners.find(load->parent());
            if (owner == for_body_owners.end() || owner->second == nullptr) {
                continue;
            }
            auto *loop = owner->second;
            if (read_stmt->schedule_dims.empty() ||
                read_stmt->schedule_dims.back() != loop->induction_var()) {
                continue;
            }

            std::int64_t step = 0;
            if (!const_i32_value(loop->step(), step) || step != 1) {
                continue;
            }

            if (!access_matches_next_iteration_write(write, read, loop->induction_var(), step)) {
                continue;
            }
            if (!seen_loads.insert(load).second) {
                continue;
            }

            candidates.push_back({load, store, &read, loop});
        }

        return candidates;
    }

    bool apply_stencil_carries(const std::vector<StencilCarryCandidate> &candidates,
                               std::unordered_set<yir::ArrayLoadOp *> &erased_loads) {
        bool changed = false;
        for (const auto &candidate : candidates) {
            if (candidate.load == nullptr || candidate.store == nullptr ||
                erased_loads.find(candidate.load) != erased_loads.end()) {
                continue;
            }
            if (apply_stencil_carry(candidate)) {
                erased_loads.insert(candidate.load);
                changed = true;
            }
        }
        return changed;
    }

    bool apply_stencil_carry(const StencilCarryCandidate &candidate) {
        auto *load = candidate.load;
        auto *store = candidate.store;
        auto *loop = candidate.loop;
        if (load == nullptr || store == nullptr || loop == nullptr || candidate.read == nullptr ||
            load->parent() == nullptr || store->parent() != load->parent() ||
            loop->parent() == nullptr || load->result() == nullptr || store->value() == nullptr) {
            return false;
        }

        auto *body = load->parent();
        std::size_t load_index = 0;
        std::size_t store_index = 0;
        if (!find_operation_index(*body, *load, load_index) ||
            !find_operation_index(*body, *store, store_index) || load_index >= store_index) {
            return false;
        }

        if (value_used_outside_range(*body, load->result(), load_index + 1, store_index)) {
            return false;
        }
        if (has_intervening_write_to_memory(*body, load->array(), load_index + 1,
                                            store_index == 0 ? 0 : store_index - 1)) {
            return false;
        }

        auto *preheader = loop->parent();
        std::size_t loop_index = 0;
        if (!find_operation_index(*preheader, *loop, loop_index)) {
            return false;
        }
        if (std::any_of(candidate.read->indices.begin(), candidate.read->indices.end(),
                        [](const PolyAffineExpr &expr) {
                            return !can_materialize_affine_expr(expr);
                        })) {
            return false;
        }

        std::size_t next_temp = next_stencil_temp_;
        std::size_t insert_pos = loop_index;
        std::vector<yir::Value *> initial_indices;
        initial_indices.reserve(candidate.read->indices.size());
        for (const auto &index : candidate.read->indices) {
            auto *materialized =
                materialize_affine_expr(*preheader, insert_pos, index, loop->induction_var(),
                                        loop->lower_bound(), next_temp);
            if (materialized == nullptr) {
                next_stencil_temp_ = next_temp;
                return false;
            }
            initial_indices.push_back(materialized);
        }

        auto *initial_load = insert_op_before<yir::ArrayLoadOp>(
            *preheader, insert_pos, load->array(), std::move(initial_indices),
            load->result()->type(), stencil_temp_name("init", next_temp++));
        auto *carry_var = insert_op_before<yir::VarOp>(
            *preheader, insert_pos, load->result()->type(), initial_load->result(),
            stencil_temp_name("carry", next_temp++));
        next_stencil_temp_ = next_temp;

        replace_value_in_range(*body, load_index + 1, store_index, load->result(),
                               carry_var->result());
        auto &body_ops = body->operations();
        body_ops.erase(body_ops.begin() + static_cast<std::ptrdiff_t>(load_index));
        if (load_index < store_index) {
            --store_index;
        }

        auto assign = std::make_unique<yir::AssignOp>(carry_var->result(), store->value());
        assign->set_parent(body);
        body_ops.insert(body_ops.begin() + static_cast<std::ptrdiff_t>(store_index + 1),
                        std::move(assign));

        ++num_stencil_carries_;
        return true;
    }

    std::vector<SameIterationReload> find_same_iteration_reloads(const PolyScop &scop) {
        std::unordered_map<PolyAccessKey, NearestWrite, PolyAccessKeyHash> nearest_writes;
        std::vector<SameIterationReload> reloads;

        for (const auto &stmt : scop.statements) {
            for (const auto &read : stmt.reads) {
                if (!is_valid_affine_access(read)) {
                    continue;
                }
                auto found = nearest_writes.find(access_key(read));
                if (found == nearest_writes.end()) {
                    continue;
                }

                ++num_nearest_write_queries_;
                const auto &nearest = found->second;
                if (nearest.stmt == nullptr || nearest.write_index >= nearest.stmt->writes.size()) {
                    continue;
                }
                const auto &write_stmt = *nearest.stmt;
                const auto &write = write_stmt.writes[nearest.write_index];
                if (write_stmt.lexical_id >= stmt.lexical_id ||
                    !same_iteration_domain(write_stmt, stmt) ||
                    !presburger_proves_same_access(write, read)) {
                    continue;
                }

                auto *load = dynamic_cast<yir::ArrayLoadOp *>(const_cast<yir::Operation *>(stmt.op));
                auto *store = dynamic_cast<yir::ArrayStoreOp *>(
                    const_cast<yir::Operation *>(write_stmt.op));
                if (load == nullptr || store == nullptr || load->parent() != store->parent()) {
                    continue;
                }
                reloads.push_back({load, store});
            }

            for (std::size_t write_index = 0; write_index < stmt.writes.size(); ++write_index) {
                const auto &write = stmt.writes[write_index];
                if (write.memory == nullptr) {
                    continue;
                }

                // Any intervening write to the same memory object may alias this simple model.
                // Keep only the current exact write as a candidate for later same-iteration reads.
                erase_nearest_writes_for_memory(nearest_writes, write.memory);
                if (is_valid_affine_access(write)) {
                    nearest_writes[access_key(write)] = NearestWrite{&stmt, write_index};
                }
            }
        }

        return reloads;
    }

    bool replace_load_with_store_value(yir::ArrayLoadOp &load, yir::ArrayStoreOp &store) {
        auto *parent = load.parent();
        if (parent == nullptr || parent != store.parent() || load.result() == nullptr ||
            store.value() == nullptr || store.value() == load.result()) {
            return false;
        }

        std::size_t load_index = 0;
        std::size_t store_index = 0;
        if (!find_operation_index(*parent, load, load_index) ||
            !find_operation_index(*parent, store, store_index) || store_index >= load_index) {
            return false;
        }

        auto *replacement = store.value();
        auto *old_value = load.result();
        replace_value_after(*parent, load_index + 1, old_value, replacement);
        parent->operations().erase(parent->operations().begin() + static_cast<std::ptrdiff_t>(load_index));
        ++num_same_iteration_reloads_;
        return true;
    }

    bool replace_same_iteration_reloads(
        const std::vector<SameIterationReload> &reloads,
        const std::unordered_set<yir::ArrayLoadOp *> &unavailable_loads) {
        bool changed = false;
        std::unordered_set<yir::ArrayLoadOp *> erased_loads = unavailable_loads;
        for (const auto &reload : reloads) {
            if (reload.load != nullptr && reload.store != nullptr &&
                erased_loads.find(reload.load) == erased_loads.end() &&
                replace_load_with_store_value(*reload.load, *reload.store)) {
                erased_loads.insert(reload.load);
                changed = true;
            }
        }
        return changed;
    }

    std::vector<DeadMemoryWrite> find_dead_memory_writes() const {
        std::unordered_map<const yir::Value *, bool> observed_cache;
        std::unordered_map<const yir::Value *, bool> model_read_cache;
        std::vector<DeadMemoryWrite> writes;
        std::unordered_set<yir::ArrayStoreOp *> seen_stores;

        for (const auto &scop : model_info_.models) {
            for (const auto &stmt : scop.statements) {
                if (stmt.writes.empty()) {
                    continue;
                }

                auto *store =
                    dynamic_cast<yir::ArrayStoreOp *>(const_cast<yir::Operation *>(stmt.op));
                if (store == nullptr || !is_zero_value(store->value())) {
                    continue;
                }

                for (const auto &write : stmt.writes) {
                    if (write.memory == nullptr || write.memory != store->array()) {
                        continue;
                    }

                    auto model_read = model_read_cache.find(write.memory);
                    if (model_read == model_read_cache.end()) {
                        model_read =
                            model_read_cache.emplace(write.memory, model_reads_memory(write.memory))
                                .first;
                    }
                    if (model_read->second) {
                        continue;
                    }

                    auto observed = observed_cache.find(write.memory);
                    if (observed == observed_cache.end()) {
                        observed = observed_cache
                                       .emplace(write.memory,
                                                module_observes_memory_except_writes(module_,
                                                                                    write.memory))
                                       .first;
                    }
                    if (observed->second || !seen_stores.insert(store).second) {
                        continue;
                    }

                    writes.push_back({store, write.memory});
                }
            }
        }

        return writes;
    }

    bool model_reads_memory(const yir::Value *memory) const {
        for (const auto &scop : model_info_.models) {
            for (const auto &stmt : scop.statements) {
                for (const auto &read : stmt.reads) {
                    if (read.memory == memory) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool eliminate_dead_memory_writes() {
        bool changed = false;
        for (const auto &write : find_dead_memory_writes()) {
            auto *store = write.store;
            if (store == nullptr || store->parent() == nullptr) {
                continue;
            }

            std::size_t store_index = 0;
            auto *parent = store->parent();
            if (!find_operation_index(*parent, *store, store_index)) {
                continue;
            }

            parent->operations().erase(parent->operations().begin() +
                                       static_cast<std::ptrdiff_t>(store_index));
            ++num_dead_memory_writes_;
            changed = true;
        }
        return changed;
    }

    bool eliminate_unused_globals() {
        bool changed = false;
        auto &globals = module_.globals();
        for (std::size_t i = 0; i < globals.size();) {
            auto *global = globals[i].get();
            if (global == nullptr ||
                count_value_uses_in_module(module_, global->address()) != 0) {
                ++i;
                continue;
            }
            globals.erase(globals.begin() + static_cast<std::ptrdiff_t>(i));
            ++num_unused_globals_;
            changed = true;
        }
        return changed;
    }

    bool eliminate_dead_pure_results() {
        bool changed = false;
        bool changed_round = true;
        while (changed_round) {
            changed_round = false;
            for (auto &function : module_.functions()) {
                changed_round = eliminate_dead_pure_results(function->body()) || changed_round;
            }
            changed = changed || changed_round;
        }
        return changed;
    }

    bool eliminate_dead_pure_results(yir::Region &region) {
        bool changed = false;
        auto &ops = region.operations();
        for (std::size_t i = 0; i < ops.size();) {
            auto *op = ops[i].get();

            if (auto *if_op = dynamic_cast<yir::IfOp *>(op)) {
                changed = eliminate_dead_pure_results(if_op->then_region()) || changed;
                if (if_op->has_else()) {
                    changed = eliminate_dead_pure_results(if_op->else_region()) || changed;
                }
            } else if (auto *while_op = dynamic_cast<yir::WhileOp *>(op)) {
                changed = eliminate_dead_pure_results(while_op->cond_region()) || changed;
                changed = eliminate_dead_pure_results(while_op->body_region()) || changed;
            } else if (auto *for_op = dynamic_cast<yir::ForOp *>(op)) {
                changed = eliminate_dead_pure_results(for_op->body_region()) || changed;
            }

            if (is_dead_pure_result_eliminable(*op) &&
                !module_uses_value(module_, op->result())) {
                ops.erase(ops.begin() + static_cast<std::ptrdiff_t>(i));
                ++num_dead_pure_results_;
                changed = true;
                continue;
            }

            ++i;
        }
        return changed;
    }

    bool replace_equivalent_affine_scalars() {
        bool changed = false;
        for (auto &function : module_.functions()) {
            AddAvailability available;
            ActiveInductionVars active_ivs;
            changed = replace_equivalent_affine_scalars(function->body(), available, active_ivs) || changed;
        }
        return changed;
    }

    bool replace_equivalent_affine_scalars(yir::Region &region, AddAvailability available,
                                           ActiveInductionVars active_ivs) {
        bool changed = false;
        auto &ops = region.operations();
        for (std::size_t i = 0; i < ops.size();) {
            auto *op = ops[i].get();
            bool erased = false;

            if (auto *add = dynamic_cast<yir::AddIOp *>(op)) {
                auto key = normalize_add(*add);
                if (is_active_affine_expr(key, active_ivs)) {
                    auto found = available.find(key);
                    if (found != available.end()) {
                        ++num_nearest_queries_;
                        auto *previous = found->second;
                        if (presburger_proves_identical_add(*previous, *add)) {
                            replace_value_after(region, i + 1, add->result(), previous->result());
                            ops.erase(ops.begin() + static_cast<std::ptrdiff_t>(i));
                            erased = true;
                            ++num_affine_replacements_;
                            changed = true;
                        }
                    }
                    if (!erased) {
                        available[std::move(key)] = add;
                    }
                }
            }

            if (erased) {
                continue;
            }

            if (auto *if_op = dynamic_cast<yir::IfOp *>(op)) {
                changed = replace_equivalent_affine_scalars(if_op->then_region(), available,
                                                            active_ivs) || changed;
                if (if_op->has_else()) {
                    changed = replace_equivalent_affine_scalars(if_op->else_region(), available,
                                                                active_ivs) || changed;
                }
            } else if (auto *while_op = dynamic_cast<yir::WhileOp *>(op)) {
                AddAvailability empty_available;
                changed = replace_equivalent_affine_scalars(while_op->cond_region(), empty_available,
                                                            active_ivs) || changed;
                changed = replace_equivalent_affine_scalars(while_op->body_region(), empty_available,
                                                            active_ivs) || changed;
            } else if (auto *for_op = dynamic_cast<yir::ForOp *>(op)) {
                auto nested_active_ivs = active_ivs;
                nested_active_ivs.push_back(for_op->induction_var());
                changed = replace_equivalent_affine_scalars(for_op->body_region(), available,
                                                            nested_active_ivs) || changed;
            }

            ++i;
        }
        return changed;
    }

    yir::Module &module_;
    PolyModelInfo& model_info_;
    PolyDependenceInfo& dep_info_;
    const YIRPolyhedralCanonicalInfo& canonical_info_;
    cost_model::CostModelReport *cost_report_;

    std::size_t num_interchanged_;
    std::size_t num_tiled_;
    std::size_t num_fused_;
    std::size_t num_serial_wavefronts_;
    std::size_t num_parallel_wavefronts_;
    std::size_t num_wave_unrolls_;
    std::size_t num_parallel_wave_unrolls_;
    std::size_t num_same_iteration_reloads_;
    std::size_t num_stencil_carries_;
    std::size_t num_future_neighbor_constants_;
    std::size_t num_initialization_reductions_;
    std::size_t num_affine_replacements_;
    std::size_t num_nearest_write_queries_;
    std::size_t num_nearest_queries_;
    std::size_t num_dead_memory_writes_;
    std::size_t num_dead_pure_results_;
    std::size_t num_unused_globals_;
    mutable std::size_t num_relation_legality_proofs_;
    mutable std::size_t num_relation_legality_rejections_;
    mutable std::size_t num_relation_legality_unknown_;
    std::size_t num_domain_partitions_;
    std::size_t num_reduction_privatizations_;
    std::size_t num_accumulator_promotions_;
    std::unordered_set<std::size_t> fused_scope_ids_;
    std::size_t next_stencil_temp_ = 0;
    std::size_t next_future_temp_ = 0;
    std::size_t next_init_temp_ = 0;
    std::size_t next_wave_temp_ = 0;
    std::size_t next_tile_temp_ = 0;
    std::size_t next_partition_temp_ = 0;
    std::size_t next_reduction_temp_ = 0;
    YIRPolyhedralStructuralChange structural_change_ =
        YIRPolyhedralStructuralChange::None;
};

} // namespace

std::string_view YIRPolyhedralTransformPass::name() const {
    return "YIRPolyhedralTransformPass";
}

PassKind YIRPolyhedralTransformPass::kind() const {
    return PassKind::Transform;
}

PassResult YIRPolyhedralTransformPass::run(PassContext &context) {
    auto *artifact = context.get_artifact<std::unique_ptr<yir::Module>>(ASTToYIRPass::kArtifactKey);
    if (artifact == nullptr || *artifact == nullptr) {
        return PassResult::fail("YIRPolyhedralTransformPass requires YIR module in pass context.");
    }

    auto *model_info = context.get_artifact<PolyModelInfo>(std::string(YIRPolyhedralModelBuildPass::kArtifactKey));
    if (!model_info) {
        return PassResult::fail("YIRPolyhedralTransformPass requires PolyModelInfo.");
    }

    auto *dep_info = context.get_artifact<PolyDependenceInfo>(std::string(YIRPolyhedralDependenceAnalysisPass::kArtifactKey));
    if (!dep_info) {
        return PassResult::fail("YIRPolyhedralTransformPass requires PolyDependenceInfo.");
    }

    auto *canonical_info = context.get_artifact<YIRPolyhedralCanonicalInfo>(std::string(YIRPolyhedralCanonicalizePass::kArtifactKey));
    if (!canonical_info) {
        return PassResult::fail("YIRPolyhedralTransformPass requires YIRPolyhedralCanonicalInfo.");
    }

    auto *cost_report = context.get_artifact<cost_model::CostModelReport>(
        cost_model::kReportArtifactKey);
    if (cost_report == nullptr) {
        context.set_artifact<cost_model::CostModelReport>(
            cost_model::kReportArtifactKey, cost_model::CostModelReport{});
        cost_report = context.get_artifact<cost_model::CostModelReport>(
            cost_model::kReportArtifactKey);
    }

    PolyhedralTransformer transformer(**artifact, *model_info, *dep_info, *canonical_info,
                                      cost_report);
    bool changed = transformer.transform(mode_);

    if (changed) {
        auto verify = yir::verify_high_level_yir(**artifact);
        if (!verify.success) {
            return PassResult::fail(verify_message(verify));
        }
    }

    std::ostringstream oss;
    oss << "interchange=" << transformer.num_interchanged()
        << ", tiling=" << transformer.num_tiled()
        << ", fusion=" << transformer.num_fused()
        << ", serial_wavefronts=" << transformer.num_serial_wavefronts()
        << ", parallel_wavefronts=" << transformer.num_parallel_wavefronts()
        << ", wave_unrolls=" << transformer.num_wave_unrolls()
        << ", parallel_wave_unrolls=" << transformer.num_parallel_wave_unrolls()
        << ", same_iteration_reloads=" << transformer.num_same_iteration_reloads()
        << ", stencil_carries=" << transformer.num_stencil_carries()
        << ", future_neighbor_constants=" << transformer.num_future_neighbor_constants()
        << ", initialization_reductions=" << transformer.num_initialization_reductions()
        << ", affine_replacements=" << transformer.num_affine_replacements()
        << ", nearest_write_queries=" << transformer.num_nearest_write_queries()
        << ", nearest_queries=" << transformer.num_nearest_queries()
        << ", dead_memory_writes=" << transformer.num_dead_memory_writes()
        << ", dead_pure_results=" << transformer.num_dead_pure_results()
        << ", unused_globals=" << transformer.num_unused_globals()
        << ", relation_legality_proofs=" << transformer.num_relation_legality_proofs()
        << ", relation_legality_rejections=" << transformer.num_relation_legality_rejections()
        << ", relation_legality_unknown=" << transformer.num_relation_legality_unknown()
        << ", domain_partitions=" << transformer.num_domain_partitions()
        << ", reduction_privatizations="
        << transformer.num_reduction_privatizations()
        << ", accumulator_promotions=" << transformer.num_accumulator_promotions();

    context.set_artifact<YIRPolyhedralTransformSummary>(
        std::string(YIRPolyhedralTransformSummary::kArtifactKey),
        YIRPolyhedralTransformSummary{
            transformer.structural_change(),
            transformer.num_interchanged(),
            transformer.num_tiled(),
            transformer.num_fused(),
        });

    return PassResult::ok(changed, oss.str());
}

} // namespace pass
