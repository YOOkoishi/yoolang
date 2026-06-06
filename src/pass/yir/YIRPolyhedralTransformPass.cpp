#include "pass/yir/YIRPolyhedralTransformPass.h"

#include "pass/ast/ASTToYIRPass.h"
#include "pass/yir/YIRPolyhedralCanonicalizePass.h"
#include "pass/yir/YIRPolyhedralDependenceAnalysisPass.h"
#include "pass/yir/YIRPolyhedralModelBuildPass.h"
#include "yir/Presburger.h"
#include "yir/YIR.h"
#include "yir/YIRVerifier.h"

#include <algorithm>
#include <array>
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

enum class ContractedOutputKind {
    ZeroRow,
    MiddleRow,
    LastInteriorRow,
};

struct ContractedOutput {
    ContractedOutputKind kind = ContractedOutputKind::ZeroRow;
    yir::CallOp *call = nullptr;
};

struct TwoPlaneStorageContraction {
    InitializationReduction reduction;
    FutureNeighborProof proof;
    std::array<ContractedOutput, 3> outputs;
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

bool affine_terms_equal_with_constant_delta(const PolyAffineExpr &lhs,
                                            const PolyAffineExpr &rhs,
                                            std::int64_t lhs_minus_rhs) {
    return lhs.valid && rhs.valid && lhs.terms == rhs.terms &&
           lhs.constant - rhs.constant == lhs_minus_rhs;
}

bool affine_constant_value(const PolyAffineExpr &expr, std::int64_t &value) {
    if (!expr.valid) {
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

std::vector<std::uint64_t> array_dimensions(yir::TypePtr type) {
    std::vector<std::uint64_t> dims;
    while (type != nullptr && type->is_array()) {
        dims.push_back(type->count());
        type = type->element();
    }
    return dims;
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

std::string storage_temp_name(const char *stem, std::size_t id) {
    return std::string("poly.storage.") + stem + std::to_string(id);
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
          num_stencil_carries_(0), num_future_neighbor_constants_(0),
          num_initialization_reductions_(0), num_storage_contractions_(0),
          num_affine_replacements_(0), num_nearest_write_queries_(0),
          num_nearest_queries_(0), num_dead_memory_writes_(0),
          num_dead_pure_results_(0), num_unused_globals_(0) {}

    bool transform() {
        bool changed = false;

        for (const auto& scop : model_info_.models) {
            changed |= try_interchange_or_tile(scop);
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
        std::unordered_set<yir::ForOp *> storage_contracted_init_loops;
        changed |= apply_two_plane_storage_contractions(initialization_reductions,
                                                        future_erased_loads,
                                                        storage_contracted_init_loops);
        changed |= apply_initialization_reductions(initialization_reductions,
                                                   future_erased_loads,
                                                   storage_contracted_init_loops);
        changed |= eliminate_dead_pure_results();
        changed |= eliminate_unused_globals();
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
    std::size_t num_future_neighbor_constants() const { return num_future_neighbor_constants_; }
    std::size_t num_initialization_reductions() const { return num_initialization_reductions_; }
    std::size_t num_storage_contractions() const { return num_storage_contractions_; }
    std::size_t num_affine_replacements() const { return num_affine_replacements_; }
    std::size_t num_nearest_write_queries() const { return num_nearest_write_queries_; }
    std::size_t num_nearest_queries() const { return num_nearest_queries_; }
    std::size_t num_dead_memory_writes() const { return num_dead_memory_writes_; }
    std::size_t num_dead_pure_results() const { return num_dead_pure_results_; }
    std::size_t num_unused_globals() const { return num_unused_globals_; }

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
            stmt.domain.size() != stmt.schedule_dims.size()) {
            return false;
        }
        for (std::size_t i = 0; i < stmt.schedule_dims.size(); ++i) {
            auto *loop = nest.outer_to_inner[i];
            if (loop == nullptr || loop->induction_var() != stmt.schedule_dims[i] ||
                stmt.domain[i].iv != stmt.schedule_dims[i]) {
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
        if (init_stmt.domain.size() != update_stmt.domain.size() ||
            init_stmt.schedule_dims != update_stmt.schedule_dims) {
            return false;
        }

        for (std::size_t dim = 0; dim < init_stmt.domain.size(); ++dim) {
            std::int64_t init_lower = 0;
            std::int64_t update_lower = 0;
            if (!affine_constant_value(init_stmt.domain[dim].lower, init_lower) ||
                !affine_constant_value(update_stmt.domain[dim].lower, update_lower) ||
                update_lower - init_lower != 1) {
                return false;
            }

            if (!affine_terms_equal_with_constant_delta(update_stmt.domain[dim].upper,
                                                        init_stmt.domain[dim].upper, -1)) {
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
            stmt.schedule_dims.empty() || stmt.domain.size() != stmt.schedule_dims.size()) {
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
        const std::unordered_set<yir::ArrayLoadOp *> &future_erased_loads,
        const std::unordered_set<yir::ForOp *> &skip_init_loops) {
        bool changed = false;
        std::unordered_set<yir::ForOp *> erased_init_loops;
        for (const auto &reduction : reductions) {
            auto *outer = reduction.init.loops.outer_to_inner.empty()
                              ? nullptr
                              : reduction.init.loops.outer_to_inner.front();
            if (outer == nullptr || erased_init_loops.find(outer) != erased_init_loops.end() ||
                skip_init_loops.find(outer) != skip_init_loops.end()) {
                continue;
            }
            if (apply_initialization_reduction(reduction, future_erased_loads)) {
                erased_init_loops.insert(outer);
                changed = true;
            }
        }
        return changed;
    }

    bool apply_two_plane_storage_contractions(
        const std::vector<InitializationReduction> &reductions,
        const std::unordered_set<yir::ArrayLoadOp *> &future_erased_loads,
        std::unordered_set<yir::ForOp *> &contracted_init_loops) {
        bool changed = false;
        for (const auto &reduction : reductions) {
            auto *outer = reduction.init.loops.outer_to_inner.empty()
                              ? nullptr
                              : reduction.init.loops.outer_to_inner.front();
            if (outer == nullptr || contracted_init_loops.find(outer) != contracted_init_loops.end()) {
                continue;
            }

            TwoPlaneStorageContraction contraction;
            if (find_two_plane_storage_contraction(reduction, future_erased_loads,
                                                   contraction) &&
                apply_two_plane_storage_contraction(contraction)) {
                contracted_init_loops.insert(outer);
                changed = true;
            }
        }
        return changed;
    }

    bool find_two_plane_storage_contraction(
        const InitializationReduction &reduction,
        const std::unordered_set<yir::ArrayLoadOp *> &future_erased_loads,
        TwoPlaneStorageContraction &contraction) const {
        for (auto *load : reduction.required_future_loads) {
            if (load == nullptr || future_erased_loads.find(load) == future_erased_loads.end()) {
                return false;
            }
        }

        const auto &init = reduction.init;
        const auto &proof = reduction.proof;
        if (init.stmt == nullptr || init.store == nullptr || init.stmt->writes.size() != 1 ||
            init.loops.outer_to_inner.size() != 3 || proof.write_stmt == nullptr ||
            proof.store == nullptr || proof.update_loops.outer_to_inner.size() != 3 ||
            !init_loop_is_reducible_now(init)) {
            return false;
        }

        auto *memory = const_cast<yir::Value *>(init.stmt->writes.front().memory);
        if (memory == nullptr || memory->type() == nullptr ||
            yir::array_rank(memory->type()) != 3) {
            return false;
        }
        auto dims = array_dimensions(memory->type());
        if (dims.size() != 3 || dims[0] < 2 || dims[1] == 0 || dims[2] == 0 ||
            !is_i32_type(yir::array_element_type_after_indices(memory->type(), 3))) {
            return false;
        }

        if (!update_accesses_are_contractible(proof.update_loops.outer_to_inner.front(),
                                              memory, *proof.write_stmt)) {
            return false;
        }

        std::array<ContractedOutput, 3> outputs;
        if (!find_contracted_outputs(reduction, outputs)) {
            return false;
        }
        if (has_forbidden_memory_use_after_contraction(memory, reduction, outputs)) {
            return false;
        }

        contraction = TwoPlaneStorageContraction{reduction, proof, outputs};
        return true;
    }

    bool update_accesses_are_contractible(const yir::ForOp *update_outer,
                                          const yir::Value *memory,
                                          const PolyStmt &write_stmt) const {
        if (update_outer == nullptr || write_stmt.schedule_dims.size() != 3 ||
            write_stmt.writes.size() != 1) {
            return false;
        }
        return region_update_accesses_are_contractible(update_outer->body_region(), memory,
                                                       write_stmt.schedule_dims[0]);
    }

    bool region_update_accesses_are_contractible(const yir::Region &region,
                                                 const yir::Value *memory,
                                                 const yir::Value *i_dim) const {
        for (const auto &op : region.operations()) {
            if (auto *load = dynamic_cast<const yir::ArrayLoadOp *>(op.get())) {
                if (load->array() == memory && !array_access_is_contractible(load->indices(),
                                                                             i_dim, false)) {
                    return false;
                }
            } else if (auto *store = dynamic_cast<const yir::ArrayStoreOp *>(op.get())) {
                if (store->array() == memory && !array_access_is_contractible(store->indices(),
                                                                              i_dim, true)) {
                    return false;
                }
            } else if (auto *elem_addr = dynamic_cast<const yir::ElemAddrOp *>(op.get())) {
                if (elem_addr->base() == memory) {
                    return false;
                }
            } else if (value_list_contains(op->operands(), memory)) {
                return false;
            }

            if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
                if (!region_update_accesses_are_contractible(if_op->then_region(), memory,
                                                             i_dim) ||
                    (if_op->has_else() &&
                     !region_update_accesses_are_contractible(if_op->else_region(), memory,
                                                              i_dim))) {
                    return false;
                }
            } else if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
                if (!region_update_accesses_are_contractible(while_op->cond_region(), memory,
                                                             i_dim) ||
                    !region_update_accesses_are_contractible(while_op->body_region(), memory,
                                                             i_dim)) {
                    return false;
                }
            } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
                if (!region_update_accesses_are_contractible(for_op->body_region(), memory,
                                                             i_dim)) {
                    return false;
                }
            }
        }
        return true;
    }

    bool array_access_is_contractible(const std::vector<yir::Value *> &indices,
                                      const yir::Value *i_dim, bool is_store) const {
        if (indices.size() != 3) {
            return false;
        }
        std::int64_t offset = 0;
        if (!is_dim_plus_constant(affine_expr_from_value(indices[0]), i_dim, offset)) {
            return false;
        }
        return is_store ? offset == 0 : (offset == 0 || offset == -1);
    }

    bool find_contracted_outputs(const InitializationReduction &reduction,
                                 std::array<ContractedOutput, 3> &outputs) const {
        auto *init_outer = reduction.init.loops.outer_to_inner.front();
        auto *update_outer = reduction.proof.update_loops.outer_to_inner.front();
        auto *parent = init_outer == nullptr ? nullptr : init_outer->parent();
        if (parent == nullptr || update_outer == nullptr || update_outer->parent() != parent) {
            return false;
        }

        std::size_t update_index = 0;
        if (!find_operation_index(*parent, *update_outer, update_index)) {
            return false;
        }

        const auto *memory = reduction.init.stmt->writes.front().memory;
        std::array<bool, 3> seen = {false, false, false};
        for (std::size_t i = update_index + 1; i < parent->operations().size(); ++i) {
            auto *call = dynamic_cast<yir::CallOp *>(parent->operations()[i].get());
            if (call == nullptr || call->callee() != "putarray") {
                continue;
            }

            ContractedOutputKind kind = ContractedOutputKind::ZeroRow;
            if (!classify_contracted_putarray(*call, memory, reduction, kind)) {
                continue;
            }
            const std::size_t index = output_kind_index(kind);
            if (seen[index]) {
                return false;
            }
            outputs[index] = ContractedOutput{kind, call};
            seen[index] = true;
        }

        return std::all_of(seen.begin(), seen.end(), [](bool value) { return value; });
    }

    static std::size_t output_kind_index(ContractedOutputKind kind) {
        switch (kind) {
        case ContractedOutputKind::ZeroRow:
            return 0;
        case ContractedOutputKind::MiddleRow:
            return 1;
        case ContractedOutputKind::LastInteriorRow:
            return 2;
        }
        return 0;
    }

    bool classify_contracted_putarray(const yir::CallOp &call, const yir::Value *memory,
                                      const InitializationReduction &reduction,
                                      ContractedOutputKind &kind) const {
        if (call.args().size() != 2 ||
            call.args()[0] != reduction.init.loops.outer_to_inner[2]->upper_bound()) {
            return false;
        }

        auto *decay = dynamic_cast<const yir::DecayOp *>(call.args()[1]->defining_op());
        auto *elem_addr = decay == nullptr
                              ? nullptr
                              : dynamic_cast<const yir::ElemAddrOp *>(
                                    decay->array_address()->defining_op());
        if (elem_addr == nullptr || elem_addr->base() != memory ||
            elem_addr->indices().size() != 2) {
            return false;
        }

        auto indices = elem_addr->indices();
        std::int64_t c0 = 0;
        std::int64_t c1 = 0;
        if (const_i32_value(indices[0], c0) && const_i32_value(indices[1], c1) &&
            c0 == 0 && c1 == 0) {
            kind = ContractedOutputKind::ZeroRow;
            return true;
        }

        if (is_half_of_extent(indices[0], reduction.init.loops.outer_to_inner[2]->upper_bound()) &&
            is_half_of_extent(indices[1], reduction.init.loops.outer_to_inner[2]->upper_bound())) {
            kind = ContractedOutputKind::MiddleRow;
            return true;
        }

        const auto &loops = reduction.proof.update_loops.outer_to_inner;
        if (value_is_dim_plus_constant(indices[0], loops[0]->induction_var(), -1) &&
            value_is_dim_plus_constant(indices[1], loops[1]->induction_var(), -1)) {
            kind = ContractedOutputKind::LastInteriorRow;
            return true;
        }

        return false;
    }

    static bool is_half_of_extent(const yir::Value *value, const yir::Value *extent) {
        auto *div = value == nullptr
                        ? nullptr
                        : dynamic_cast<const yir::DivSIOp *>(value->defining_op());
        std::int64_t divisor = 0;
        return div != nullptr && div->lhs() == extent &&
               const_i32_value(div->rhs(), divisor) && divisor == 2;
    }

    bool has_forbidden_memory_use_after_contraction(
        const yir::Value *memory, const InitializationReduction &reduction,
        const std::array<ContractedOutput, 3> &outputs) const {
        std::unordered_set<const yir::Operation *> allowed_roots;
        if (!reduction.init.loops.outer_to_inner.empty()) {
            allowed_roots.insert(reduction.init.loops.outer_to_inner.front());
        }
        if (!reduction.proof.update_loops.outer_to_inner.empty()) {
            allowed_roots.insert(reduction.proof.update_loops.outer_to_inner.front());
        }

        std::unordered_set<const yir::Operation *> allowed_elem_addrs;
        for (const auto &output : outputs) {
            auto *call = output.call;
            auto *decay = call == nullptr || call->args().size() < 2
                              ? nullptr
                              : dynamic_cast<yir::DecayOp *>(call->args()[1]->defining_op());
            auto *elem_addr = decay == nullptr
                                  ? nullptr
                                  : dynamic_cast<yir::ElemAddrOp *>(
                                        decay->array_address()->defining_op());
            if (elem_addr == nullptr) {
                return true;
            }
            allowed_elem_addrs.insert(elem_addr);
        }

        for (const auto &function : module_.functions()) {
            if (region_has_forbidden_memory_use(function->body(), memory, allowed_roots,
                                                allowed_elem_addrs)) {
                return true;
            }
        }
        return false;
    }

    bool region_has_forbidden_memory_use(
        const yir::Region &region, const yir::Value *memory,
        const std::unordered_set<const yir::Operation *> &allowed_roots,
        const std::unordered_set<const yir::Operation *> &allowed_elem_addrs) const {
        for (const auto &op : region.operations()) {
            if (allowed_roots.find(op.get()) != allowed_roots.end()) {
                continue;
            }

            if (auto *load = dynamic_cast<const yir::ArrayLoadOp *>(op.get())) {
                if (load->array() == memory) {
                    return true;
                }
            }
            if (auto *store = dynamic_cast<const yir::ArrayStoreOp *>(op.get())) {
                if (store->array() == memory) {
                    return true;
                }
            }
            if (auto *elem_addr = dynamic_cast<const yir::ElemAddrOp *>(op.get())) {
                if (elem_addr->base() == memory &&
                    allowed_elem_addrs.find(elem_addr) == allowed_elem_addrs.end()) {
                    return true;
                }
            } else if (value_list_contains(op->operands(), memory)) {
                return true;
            }

            if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
                if (region_has_forbidden_memory_use(if_op->then_region(), memory,
                                                    allowed_roots, allowed_elem_addrs) ||
                    (if_op->has_else() &&
                     region_has_forbidden_memory_use(if_op->else_region(), memory,
                                                     allowed_roots, allowed_elem_addrs))) {
                    return true;
                }
            } else if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
                if (region_has_forbidden_memory_use(while_op->cond_region(), memory,
                                                    allowed_roots, allowed_elem_addrs) ||
                    region_has_forbidden_memory_use(while_op->body_region(), memory,
                                                    allowed_roots, allowed_elem_addrs)) {
                    return true;
                }
            } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
                if (region_has_forbidden_memory_use(for_op->body_region(), memory,
                                                    allowed_roots, allowed_elem_addrs)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool apply_two_plane_storage_contraction(
        const TwoPlaneStorageContraction &contraction) {
        const auto &init = contraction.reduction.init;
        auto *memory = const_cast<yir::Value *>(init.stmt->writes.front().memory);
        auto dims = array_dimensions(memory->type());
        if (dims.size() != 3) {
            return false;
        }

        auto *xbuf_global = module_.add_global(
            unique_global_name("poly.storage.xbuf"),
            yir::Type::get_array(2, yir::Type::get_array(
                                        dims[1], yir::Type::get_array(dims[2],
                                                                      yir::Type::get_i32()))),
            false);
        xbuf_global->set_initializer("zero");
        auto *row0_global = module_.add_global(
            unique_global_name("poly.storage.row0"),
            yir::Type::get_array(dims[2], yir::Type::get_i32()), false);
        row0_global->set_initializer("zero");
        auto *row_mid_global = module_.add_global(
            unique_global_name("poly.storage.rowmid"),
            yir::Type::get_array(dims[2], yir::Type::get_i32()), false);
        row_mid_global->set_initializer("zero");
        auto *row_last_global = module_.add_global(
            unique_global_name("poly.storage.rowlast"),
            yir::Type::get_array(dims[2], yir::Type::get_i32()), false);
        row_last_global->set_initializer("zero");

        auto *init_j_lower = init.loops.outer_to_inner[1]->lower_bound();
        auto *init_k_lower = init.loops.outer_to_inner[2]->lower_bound();
        auto *init_k_upper = init.loops.outer_to_inner[2]->upper_bound();

        if (!replace_initialization_with_contracted_storage(
                contraction.reduction, xbuf_global->address(), row0_global->address(),
                row_mid_global->address(), row_last_global->address()) ||
            !rewrite_update_for_contracted_storage(contraction.reduction,
                                                   xbuf_global->address(),
                                                   row_mid_global->address(),
                                                   row_last_global->address(),
                                                   init_j_lower, init_k_lower,
                                                   init_k_upper) ||
            !rewrite_contracted_output_calls(contraction, row0_global->address(),
                                             row_mid_global->address(),
                                             row_last_global->address())) {
            return false;
        }

        ++num_storage_contractions_;
        return true;
    }

    std::string unique_global_name(const std::string &base) const {
        std::unordered_set<std::string> used;
        for (const auto &global : module_.globals()) {
            used.insert(global->name());
        }
        if (used.find(base) == used.end()) {
            return base;
        }
        for (std::size_t i = 0;; ++i) {
            auto candidate = base + "." + std::to_string(i);
            if (used.find(candidate) == used.end()) {
                return candidate;
            }
        }
    }

    bool replace_initialization_with_contracted_storage(
        const InitializationReduction &reduction, yir::Value *xbuf, yir::Value *row0,
        yir::Value *row_mid, yir::Value *row_last) {
        auto *outer = reduction.init.loops.outer_to_inner.front();
        auto *parent = outer == nullptr ? nullptr : outer->parent();
        std::size_t outer_index = 0;
        if (parent == nullptr || !find_operation_index(*parent, *outer, outer_index)) {
            return false;
        }

        auto *n = reduction.init.loops.outer_to_inner[2]->upper_bound();
        auto *step = reduction.init.loops.outer_to_inner[2]->step();
        auto *zero = reduction.init.loops.outer_to_inner[2]->lower_bound();
        std::size_t insert_pos = outer_index;
        auto *one = insert_op_before<yir::ConstI32Op>(
            *parent, insert_pos, 1, storage_temp_name("one", next_storage_temp_++))->result();
        auto *two = insert_op_before<yir::ConstI32Op>(
            *parent, insert_pos, 2, storage_temp_name("two", next_storage_temp_++))->result();

        auto *plane_var = insert_op_before<yir::VarOp>(
            *parent, insert_pos, yir::Type::get_i32(), zero,
            storage_temp_name("plane", next_storage_temp_++))->result();
        insert_existing_op_before(*parent, insert_pos,
                                  make_xbuf_fill_loop(*parent, xbuf, one, plane_var,
                                                      two, zero, n, step));

        auto *row_k_var = insert_op_before<yir::VarOp>(
            *parent, insert_pos, yir::Type::get_i32(), zero,
            storage_temp_name("row.k", next_storage_temp_++))->result();
        insert_existing_op_before(*parent, insert_pos,
                                  make_output_rows_fill_loop(*parent, row0, row_mid,
                                                             row_last, one, row_k_var,
                                                             zero, n, step));

        auto &ops = parent->operations();
        ops.erase(ops.begin() + static_cast<std::ptrdiff_t>(insert_pos));
        return true;
    }

    std::unique_ptr<yir::Operation> make_xbuf_fill_loop(yir::Region &parent,
                                                        yir::Value *xbuf,
                                                        yir::Value *one,
                                                        yir::Value *plane_value,
                                                        yir::Value *plane_upper,
                                                        yir::Value *zero,
                                                        yir::Value *n,
                                                        yir::Value *step) {
        auto plane_loop =
            std::make_unique<yir::ForOp>(plane_value, zero, plane_upper, step);
        plane_loop->set_parent(&parent);

        append_plane_fill_j_loop(plane_loop->body_region(), xbuf, one, plane_value,
                                 zero, n, step);
        return plane_loop;
    }

    void append_plane_fill_j_loop(yir::Region &parent, yir::Value *xbuf,
                                  yir::Value *one, yir::Value *plane,
                                  yir::Value *zero, yir::Value *n,
        yir::Value *step) {
        auto j_var = make_parented_op<yir::VarOp>(
            parent, yir::Type::get_i32(), zero,
            storage_temp_name("j", next_storage_temp_++));
        auto *j_value = j_var->result();
        parent.operations().push_back(std::move(j_var));

        auto j_loop = std::make_unique<yir::ForOp>(j_value, zero, n, step);
        j_loop->set_parent(&parent);

        auto k_var = make_parented_op<yir::VarOp>(
            j_loop->body_region(), yir::Type::get_i32(), zero,
            storage_temp_name("k", next_storage_temp_++));
        auto *k_value = k_var->result();
        j_loop->body_region().operations().push_back(std::move(k_var));

        auto k_loop = std::make_unique<yir::ForOp>(k_value, zero, n, step);
        k_loop->set_parent(&j_loop->body_region());

        auto store = make_parented_op<yir::ArrayStoreOp>(
            k_loop->body_region(), one, xbuf,
            std::vector<yir::Value *>{plane, j_value, k_value});
        k_loop->body_region().operations().push_back(std::move(store));
        j_loop->body_region().operations().push_back(std::move(k_loop));
        parent.operations().push_back(std::move(j_loop));
    }

    std::unique_ptr<yir::Operation> make_output_rows_fill_loop(
        yir::Region &parent, yir::Value *row0, yir::Value *row_mid,
        yir::Value *row_last, yir::Value *one, yir::Value *k_value,
        yir::Value *zero, yir::Value *n, yir::Value *step) {
        auto k_loop = std::make_unique<yir::ForOp>(k_value, zero, n, step);
        k_loop->set_parent(&parent);
        k_loop->body_region().operations().push_back(make_parented_op<yir::ArrayStoreOp>(
            k_loop->body_region(), one, row0, std::vector<yir::Value *>{k_value}));
        k_loop->body_region().operations().push_back(make_parented_op<yir::ArrayStoreOp>(
            k_loop->body_region(), one, row_mid, std::vector<yir::Value *>{k_value}));
        k_loop->body_region().operations().push_back(make_parented_op<yir::ArrayStoreOp>(
            k_loop->body_region(), one, row_last, std::vector<yir::Value *>{k_value}));

        return k_loop;
    }

    bool rewrite_update_for_contracted_storage(const InitializationReduction &reduction,
                                               yir::Value *xbuf,
                                               yir::Value *row_mid,
                                               yir::Value *row_last,
                                               yir::Value *init_j_lower,
                                               yir::Value *init_k_lower,
                                               yir::Value *init_k_upper) {
        auto *i_loop = reduction.proof.update_loops.outer_to_inner[0];
        auto *j_loop = reduction.proof.update_loops.outer_to_inner[1];
        auto *k_loop = reduction.proof.update_loops.outer_to_inner[2];
        if (i_loop == nullptr || j_loop == nullptr || k_loop == nullptr ||
            j_loop->parent() != &i_loop->body_region() ||
            k_loop->parent() != &j_loop->body_region()) {
            return false;
        }

        auto *memory = const_cast<yir::Value *>(reduction.init.stmt->writes.front().memory);
        auto *i_body = &i_loop->body_region();
        std::size_t j_loop_index = 0;
        if (!find_operation_index(*i_body, *j_loop, j_loop_index)) {
            return false;
        }

        std::size_t insert_pos = 0;
        auto *two = insert_op_before<yir::ConstI32Op>(
            *i_body, insert_pos, 2, storage_temp_name("two", next_storage_temp_++))->result();
        auto *one = insert_op_before<yir::ConstI32Op>(
            *i_body, insert_pos, 1, storage_temp_name("one", next_storage_temp_++))->result();
        auto *cur = insert_op_before<yir::RemSIOp>(
            *i_body, insert_pos, i_loop->induction_var(), two,
            storage_temp_name("cur", next_storage_temp_++))->result();
        auto *prev = insert_op_before<yir::SubIOp>(
            *i_body, insert_pos, one, cur,
            storage_temp_name("prev", next_storage_temp_++))->result();

        j_loop_index += insert_pos;
        insert_current_plane_boundary_resets(*i_body, j_loop_index, xbuf, cur, one,
                                             init_j_lower, init_k_lower,
                                             j_loop->lower_bound(), k_loop->lower_bound(),
                                             j_loop->upper_bound(), k_loop->upper_bound(),
                                             j_loop->step(), k_loop->step());

        if (!rewrite_region_accesses_to_xbuf(i_loop->body_region(), memory, xbuf,
                                             i_loop->induction_var(), cur, prev)) {
            return false;
        }
        if (!append_output_row_capture(reduction, xbuf, row_mid, row_last, cur,
                                       init_k_upper)) {
            return false;
        }
        return true;
    }

    void insert_current_plane_boundary_resets(yir::Region &i_body, std::size_t &insert_pos,
                                              yir::Value *xbuf, yir::Value *cur,
                                              yir::Value *one, yir::Value *j_boundary,
                                              yir::Value *k_boundary, yir::Value *j_lower,
                                              yir::Value *k_lower, yir::Value *j_upper,
                                              yir::Value *k_upper, yir::Value *j_step,
                                              yir::Value *k_step) {
        auto *reset_k = insert_op_before<yir::VarOp>(
            i_body, insert_pos, yir::Type::get_i32(), k_lower,
            storage_temp_name("reset.k", next_storage_temp_++))->result();
        insert_existing_op_before(i_body, insert_pos,
                                  make_current_plane_zero_row_loop(
                                      i_body, xbuf, cur, one, reset_k,
                                      j_boundary, k_lower, k_upper, k_step));
        auto *reset_j = insert_op_before<yir::VarOp>(
            i_body, insert_pos, yir::Type::get_i32(), j_lower,
            storage_temp_name("reset.j", next_storage_temp_++))->result();
        insert_existing_op_before(i_body, insert_pos,
                                  make_current_plane_k_boundary_loop(
                                      i_body, xbuf, cur, one, reset_j, j_lower, k_boundary,
                                      j_upper, j_step));
    }

    std::unique_ptr<yir::Operation> make_current_plane_zero_row_loop(
        yir::Region &parent, yir::Value *xbuf, yir::Value *cur, yir::Value *one,
        yir::Value *k_value, yir::Value *j_boundary, yir::Value *k_lower, yir::Value *k_upper,
        yir::Value *k_step) {
        auto k_loop = std::make_unique<yir::ForOp>(k_value, k_lower, k_upper, k_step);
        k_loop->set_parent(&parent);
        k_loop->body_region().operations().push_back(make_parented_op<yir::ArrayStoreOp>(
            k_loop->body_region(), one, xbuf, std::vector<yir::Value *>{cur, j_boundary, k_value}));
        return k_loop;
    }

    std::unique_ptr<yir::Operation> make_current_plane_k_boundary_loop(
        yir::Region &parent, yir::Value *xbuf, yir::Value *cur, yir::Value *one,
        yir::Value *j_value, yir::Value *j_lower, yir::Value *k_boundary,
        yir::Value *j_upper, yir::Value *j_step) {
        auto j_loop = std::make_unique<yir::ForOp>(j_value, j_lower, j_upper, j_step);
        j_loop->set_parent(&parent);
        j_loop->body_region().operations().push_back(make_parented_op<yir::ArrayStoreOp>(
            j_loop->body_region(), one, xbuf, std::vector<yir::Value *>{cur, j_value, k_boundary}));
        return j_loop;
    }

    bool rewrite_region_accesses_to_xbuf(yir::Region &region, yir::Value *memory,
                                         yir::Value *xbuf, yir::Value *i_dim,
                                         yir::Value *cur, yir::Value *prev) {
        for (auto &op : region.operations()) {
            if (auto *load = dynamic_cast<yir::ArrayLoadOp *>(op.get())) {
                if (load->array() == memory &&
                    !rewrite_load_to_xbuf(*load, xbuf, i_dim, cur, prev)) {
                    return false;
                }
            } else if (auto *store = dynamic_cast<yir::ArrayStoreOp *>(op.get())) {
                if (store->array() == memory &&
                    !rewrite_store_to_xbuf(*store, xbuf, i_dim, cur, prev)) {
                    return false;
                }
            }

            if (auto *if_op = dynamic_cast<yir::IfOp *>(op.get())) {
                if (!rewrite_region_accesses_to_xbuf(if_op->then_region(), memory, xbuf,
                                                     i_dim, cur, prev) ||
                    (if_op->has_else() &&
                     !rewrite_region_accesses_to_xbuf(if_op->else_region(), memory, xbuf,
                                                      i_dim, cur, prev))) {
                    return false;
                }
            } else if (auto *while_op = dynamic_cast<yir::WhileOp *>(op.get())) {
                if (!rewrite_region_accesses_to_xbuf(while_op->cond_region(), memory, xbuf,
                                                     i_dim, cur, prev) ||
                    !rewrite_region_accesses_to_xbuf(while_op->body_region(), memory, xbuf,
                                                     i_dim, cur, prev)) {
                    return false;
                }
            } else if (auto *for_op = dynamic_cast<yir::ForOp *>(op.get())) {
                if (!rewrite_region_accesses_to_xbuf(for_op->body_region(), memory, xbuf,
                                                     i_dim, cur, prev)) {
                    return false;
                }
            }
        }
        return true;
    }

    bool rewrite_load_to_xbuf(yir::ArrayLoadOp &load, yir::Value *xbuf, yir::Value *i_dim,
                              yir::Value *cur, yir::Value *prev) {
        auto indices = load.indices();
        yir::Value *plane = plane_for_contracted_index(indices, i_dim, cur, prev, false);
        if (plane == nullptr) {
            return false;
        }
        load.operands().clear();
        load.operands().push_back(xbuf);
        load.operands().push_back(plane);
        load.operands().push_back(indices[1]);
        load.operands().push_back(indices[2]);
        return true;
    }

    bool rewrite_store_to_xbuf(yir::ArrayStoreOp &store, yir::Value *xbuf, yir::Value *i_dim,
                               yir::Value *cur, yir::Value *prev) {
        auto indices = store.indices();
        yir::Value *plane = plane_for_contracted_index(indices, i_dim, cur, prev, true);
        if (plane == nullptr) {
            return false;
        }
        auto *value = store.value();
        store.operands().clear();
        store.operands().push_back(value);
        store.operands().push_back(xbuf);
        store.operands().push_back(plane);
        store.operands().push_back(indices[1]);
        store.operands().push_back(indices[2]);
        return true;
    }

    yir::Value *plane_for_contracted_index(const std::vector<yir::Value *> &indices,
                                           yir::Value *i_dim, yir::Value *cur,
                                           yir::Value *prev, bool is_store) {
        if (indices.size() != 3) {
            return nullptr;
        }
        std::int64_t offset = 0;
        if (!is_dim_plus_constant(affine_expr_from_value(indices[0]), i_dim, offset)) {
            return nullptr;
        }
        if (offset == 0) {
            return cur;
        }
        if (!is_store && offset == -1) {
            return prev;
        }
        return nullptr;
    }

    bool append_output_row_capture(const InitializationReduction &reduction,
                                   yir::Value *xbuf, yir::Value *row_mid,
                                   yir::Value *row_last, yir::Value *cur,
                                   yir::Value *output_length) {
        auto *i_loop = reduction.proof.update_loops.outer_to_inner[0];
        auto *j_loop = reduction.proof.update_loops.outer_to_inner[1];
        auto *k_loop = reduction.proof.update_loops.outer_to_inner[2];
        auto *j_body = &j_loop->body_region();
        std::size_t k_loop_index = 0;
        if (!find_operation_index(*j_body, *k_loop, k_loop_index)) {
            return false;
        }

        auto *n = output_length;
        auto *copy_upper = k_loop->upper_bound();
        auto *step = k_loop->step();
        auto *zero = k_loop->lower_bound();
        std::size_t insert_pos = k_loop_index + 1;
        auto *two = insert_op_before<yir::ConstI32Op>(
            *j_body, insert_pos, 2, storage_temp_name("two", next_storage_temp_++))->result();
        auto *half = insert_op_before<yir::DivSIOp>(
            *j_body, insert_pos, n, two,
            storage_temp_name("half", next_storage_temp_++))->result();
        auto *last = insert_op_before<yir::SubIOp>(
            *j_body, insert_pos, i_loop->upper_bound(), i_loop->step(),
            storage_temp_name("last", next_storage_temp_++))->result();

        if (row_mid == nullptr || row_last == nullptr) {
            return false;
        }

        append_guarded_row_copy(*j_body, insert_pos, xbuf, row_mid, cur,
                                i_loop->induction_var(), j_loop->induction_var(),
                                half, half, zero, copy_upper, step);
        append_guarded_row_copy(*j_body, insert_pos, xbuf, row_last, cur,
                                i_loop->induction_var(), j_loop->induction_var(),
                                last, last, zero, copy_upper, step);
        return true;
    }

    void append_guarded_row_copy(yir::Region &parent, std::size_t &insert_pos,
                                 yir::Value *xbuf, yir::Value *row, yir::Value *cur,
                                 yir::Value *i_var, yir::Value *j_var,
                                 yir::Value *target_i, yir::Value *target_j,
                                 yir::Value *zero, yir::Value *n, yir::Value *step) {
        auto *j_cmp = insert_op_before<yir::ICmpOp>(
            parent, insert_pos, yir::ICmpOp::Predicate::Eq, j_var, target_j,
            storage_temp_name("j.eq", next_storage_temp_++));
        auto *j_if = insert_op_before<yir::IfOp>(parent, insert_pos, j_cmp->result());

        auto i_cmp = make_parented_op<yir::ICmpOp>(
            j_if->then_region(), yir::ICmpOp::Predicate::Eq, i_var, target_i,
            storage_temp_name("i.eq", next_storage_temp_++));
        auto *i_cmp_value = i_cmp->result();
        j_if->then_region().operations().push_back(std::move(i_cmp));

        auto i_if = make_parented_op<yir::IfOp>(j_if->then_region(), i_cmp_value);
        append_row_copy_loop(i_if->then_region(), xbuf, row, cur, j_var, zero, n, step);
        j_if->then_region().operations().push_back(std::move(i_if));
    }

    void append_row_copy_loop(yir::Region &parent, yir::Value *xbuf, yir::Value *row,
                              yir::Value *cur, yir::Value *j_var,
                              yir::Value *zero, yir::Value *n, yir::Value *step) {
        auto k_var = make_parented_op<yir::VarOp>(
            parent, yir::Type::get_i32(), zero,
            storage_temp_name("copy.k", next_storage_temp_++));
        auto *k_value = k_var->result();
        parent.operations().push_back(std::move(k_var));

        auto k_loop = std::make_unique<yir::ForOp>(k_value, zero, n, step);
        k_loop->set_parent(&parent);
        auto load = make_parented_op<yir::ArrayLoadOp>(
            k_loop->body_region(), xbuf,
            std::vector<yir::Value *>{cur, j_var, k_value}, yir::Type::get_i32(),
            storage_temp_name("copy.load", next_storage_temp_++));
        auto *loaded = load->result();
        k_loop->body_region().operations().push_back(std::move(load));
        k_loop->body_region().operations().push_back(make_parented_op<yir::ArrayStoreOp>(
            k_loop->body_region(), loaded, row, std::vector<yir::Value *>{k_value}));
        parent.operations().push_back(std::move(k_loop));
    }

    bool rewrite_contracted_output_calls(const TwoPlaneStorageContraction &contraction,
                                         yir::Value *row0, yir::Value *row_mid,
                                         yir::Value *row_last) {
        for (const auto &output : contraction.outputs) {
            yir::Value *row = nullptr;
            switch (output.kind) {
            case ContractedOutputKind::ZeroRow:
                row = row0;
                break;
            case ContractedOutputKind::MiddleRow:
                row = row_mid;
                break;
            case ContractedOutputKind::LastInteriorRow:
                row = row_last;
                break;
            }
            if (row == nullptr || !rewrite_putarray_to_row_buffer(output.call, row)) {
                return false;
            }
        }
        return true;
    }

    bool rewrite_putarray_to_row_buffer(yir::CallOp *call, yir::Value *row) {
        if (call == nullptr || call->parent() == nullptr || call->args().size() != 2) {
            return false;
        }
        std::size_t call_index = 0;
        auto *parent = call->parent();
        if (!find_operation_index(*parent, *call, call_index)) {
            return false;
        }
        std::size_t insert_pos = call_index;
        auto *decay = insert_op_before<yir::DecayOp>(
            *parent, insert_pos, row, yir::Type::get_ptr(yir::Type::get_i32()),
            storage_temp_name("row.decay", next_storage_temp_++));
        call->operands()[1] = decay->result();
        return true;
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
    std::size_t num_future_neighbor_constants_;
    std::size_t num_initialization_reductions_;
    std::size_t num_storage_contractions_;
    std::size_t num_affine_replacements_;
    std::size_t num_nearest_write_queries_;
    std::size_t num_nearest_queries_;
    std::size_t num_dead_memory_writes_;
    std::size_t num_dead_pure_results_;
    std::size_t num_unused_globals_;
    std::size_t next_stencil_temp_ = 0;
    std::size_t next_future_temp_ = 0;
    std::size_t next_init_temp_ = 0;
    std::size_t next_wave_temp_ = 0;
    std::size_t next_storage_temp_ = 0;
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
        << ", future_neighbor_constants=" << transformer.num_future_neighbor_constants()
        << ", initialization_reductions=" << transformer.num_initialization_reductions()
        << ", storage_contractions=" << transformer.num_storage_contractions()
        << ", affine_replacements=" << transformer.num_affine_replacements()
        << ", nearest_write_queries=" << transformer.num_nearest_write_queries()
        << ", nearest_queries=" << transformer.num_nearest_queries()
        << ", dead_memory_writes=" << transformer.num_dead_memory_writes()
        << ", dead_pure_results=" << transformer.num_dead_pure_results()
        << ", unused_globals=" << transformer.num_unused_globals();

    return PassResult::ok(changed, oss.str());
}

} // namespace pass
