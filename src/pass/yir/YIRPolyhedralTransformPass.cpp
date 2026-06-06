#include "pass/yir/YIRPolyhedralTransformPass.h"

#include "pass/ast/ASTToYIRPass.h"
#include "pass/yir/YIRPolyhedralCanonicalizePass.h"
#include "pass/yir/YIRPolyhedralDependenceAnalysisPass.h"
#include "pass/yir/YIRPolyhedralModelBuildPass.h"
#include "yir/Presburger.h"
#include "yir/YIR.h"
#include "yir/YIRVerifier.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
    return lhs.valid == rhs.valid && lhs.constant == rhs.constant && lhs.terms == rhs.terms;
}

std::size_t poly_affine_expr_hash(const PolyAffineExpr &expr) {
    std::size_t hash = std::hash<bool>{}(expr.valid);
    hash ^= std::hash<std::int64_t>{}(expr.constant) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    for (const auto &[value, coefficient] : expr.terms) {
        hash ^= std::hash<const yir::Value *>{}(value) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= std::hash<std::int64_t>{}(coefficient) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
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

bool is_valid_affine_access(const PolyAccess &access) {
    return access.memory != nullptr && std::all_of(access.indices.begin(), access.indices.end(),
                                                   [](const PolyAffineExpr &expr) {
                                                       return expr.valid;
                                                   });
}

PolyAccessKey access_key(const PolyAccess &access) {
    PolyAccessKey key;
    key.memory = access.memory;
    key.indices = access.indices;
    return key;
}

bool presburger_proves_identical_affine(const PolyAffineExpr &lhs, const PolyAffineExpr &rhs) {
    if (!lhs.valid || !rhs.valid) {
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

bool same_iteration_domain(const PolyStmt &write_stmt, const PolyStmt &read_stmt) {
    if (!same_schedule_dims(write_stmt, read_stmt) ||
        write_stmt.domain.size() != read_stmt.domain.size()) {
        return false;
    }

    for (std::size_t i = 0; i < write_stmt.domain.size(); ++i) {
        const auto &write_bound = write_stmt.domain[i];
        const auto &read_bound = read_stmt.domain[i];
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

bool region_uses_value(const yir::Region &region, const yir::Value *value) {
    return std::any_of(region.operations().begin(), region.operations().end(),
                       [value](const auto &op) { return operation_uses_value(*op, value); });
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
    if (!expr.valid || !fits_i32(expr.constant)) {
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

std::string stencil_temp_name(const char *stem, std::size_t id) {
    return std::string("poly.stencil.") + stem + std::to_string(id);
}

std::string wave_temp_name(const char *stem, std::size_t id) {
    return std::string("poly.wave.") + stem + std::to_string(id);
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

class PolyhedralTransformer {
public:
    explicit PolyhedralTransformer(yir::Module &module, const PolyModelInfo& model_info,
                                   const PolyDependenceInfo& dep_info,
                                   const YIRPolyhedralCanonicalInfo& canonical_info)
        : module_(module), model_info_(model_info), dep_info_(dep_info),
          canonical_info_(canonical_info), num_interchanged_(0), num_tiled_(0),
          num_serial_wavefronts_(0), num_parallel_wavefronts_(0), num_wave_unrolls_(0),
          num_parallel_wave_unrolls_(0), num_same_iteration_reloads_(0),
          num_stencil_carries_(0), num_affine_replacements_(0), num_nearest_write_queries_(0),
          num_nearest_queries_(0), num_dead_memory_writes_(0) {}

    bool transform() {
        bool changed = false;

        for (const auto& scop : model_info_.models) {
            changed |= try_interchange_or_tile(scop);
        }

        auto for_body_owners = collect_for_body_owners(module_);

        std::vector<SameIterationReload> same_iteration_reloads;
        std::vector<StencilCarryCandidate> stencil_carries;
        for (const auto &scop : model_info_.models) {
            auto reloads = find_same_iteration_reloads(scop);
            same_iteration_reloads.insert(same_iteration_reloads.end(), reloads.begin(), reloads.end());

            auto carries = find_stencil_carry_candidates(scop, for_body_owners);
            stencil_carries.insert(stencil_carries.end(), carries.begin(), carries.end());
        }

        std::unordered_set<yir::ArrayLoadOp *> erased_loads;
        changed |= apply_stencil_carries(stencil_carries, erased_loads);
        changed |= replace_same_iteration_reloads(same_iteration_reloads, erased_loads);
        changed |= replace_equivalent_affine_scalars();
        changed |= eliminate_dead_memory_writes();
        return changed;
    }

    std::size_t num_interchanged() const { return num_interchanged_; }
    std::size_t num_tiled() const { return num_tiled_; }
    std::size_t num_serial_wavefronts() const { return num_serial_wavefronts_; }
    std::size_t num_parallel_wavefronts() const { return num_parallel_wavefronts_; }
    std::size_t num_wave_unrolls() const { return num_wave_unrolls_; }
    std::size_t num_parallel_wave_unrolls() const { return num_parallel_wave_unrolls_; }
    std::size_t num_same_iteration_reloads() const { return num_same_iteration_reloads_; }
    std::size_t num_stencil_carries() const { return num_stencil_carries_; }
    std::size_t num_affine_replacements() const { return num_affine_replacements_; }
    std::size_t num_nearest_write_queries() const { return num_nearest_write_queries_; }
    std::size_t num_nearest_queries() const { return num_nearest_queries_; }
    std::size_t num_dead_memory_writes() const { return num_dead_memory_writes_; }

private:
    bool try_interchange_or_tile(const PolyScop& scop) {
        (void)scop;
        (void)dep_info_;
        (void)canonical_info_;
        // TODO(polyhedral): use dependence directions to legalize schedule transforms.
        return false;
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
            write_stmt->domain.size() != 3 || write_stmt->writes.front().indices.size() != 3) {
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
    const PolyModelInfo& model_info_;
    const PolyDependenceInfo& dep_info_;
    const YIRPolyhedralCanonicalInfo& canonical_info_;

    std::size_t num_interchanged_;
    std::size_t num_tiled_;
    std::size_t num_serial_wavefronts_;
    std::size_t num_parallel_wavefronts_;
    std::size_t num_wave_unrolls_;
    std::size_t num_parallel_wave_unrolls_;
    std::size_t num_same_iteration_reloads_;
    std::size_t num_stencil_carries_;
    std::size_t num_affine_replacements_;
    std::size_t num_nearest_write_queries_;
    std::size_t num_nearest_queries_;
    std::size_t num_dead_memory_writes_;
    std::size_t next_stencil_temp_ = 0;
    std::size_t next_wave_temp_ = 0;
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

    PolyhedralTransformer transformer(**artifact, *model_info, *dep_info, *canonical_info);
    bool changed = transformer.transform();

    if (changed) {
        auto verify = yir::verify_high_level_yir(**artifact);
        if (!verify.success) {
            return PassResult::fail(verify_message(verify));
        }
    }

    std::ostringstream oss;
    oss << "interchange=" << transformer.num_interchanged()
        << ", tiling=" << transformer.num_tiled()
        << ", serial_wavefronts=" << transformer.num_serial_wavefronts()
        << ", parallel_wavefronts=" << transformer.num_parallel_wavefronts()
        << ", wave_unrolls=" << transformer.num_wave_unrolls()
        << ", parallel_wave_unrolls=" << transformer.num_parallel_wave_unrolls()
        << ", same_iteration_reloads=" << transformer.num_same_iteration_reloads()
        << ", stencil_carries=" << transformer.num_stencil_carries()
        << ", affine_replacements=" << transformer.num_affine_replacements()
        << ", nearest_write_queries=" << transformer.num_nearest_write_queries()
        << ", nearest_queries=" << transformer.num_nearest_queries()
        << ", dead_memory_writes=" << transformer.num_dead_memory_writes();

    return PassResult::ok(changed, oss.str());
}

} // namespace pass
