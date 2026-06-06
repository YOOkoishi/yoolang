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
          num_serial_wavefronts_(0), num_parallel_wavefronts_(0),
          num_same_iteration_reloads_(0), num_stencil_carries_(0), num_affine_replacements_(0),
          num_nearest_write_queries_(0), num_nearest_queries_(0) {}

    bool transform() {
        bool changed = false;

        for (const auto& scop : model_info_.models) {
            changed |= try_interchange_or_tile(scop);
        }

        auto for_body_owners = collect_for_body_owners(module_);
        if (apply_first_serial_wavefront(for_body_owners)) {
            changed = true;
            changed |= replace_equivalent_affine_scalars();
            return changed;
        }

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
        return changed;
    }

    std::size_t num_interchanged() const { return num_interchanged_; }
    std::size_t num_tiled() const { return num_tiled_; }
    std::size_t num_serial_wavefronts() const { return num_serial_wavefronts_; }
    std::size_t num_parallel_wavefronts() const { return num_parallel_wavefronts_; }
    std::size_t num_same_iteration_reloads() const { return num_same_iteration_reloads_; }
    std::size_t num_stencil_carries() const { return num_stencil_carries_; }
    std::size_t num_affine_replacements() const { return num_affine_replacements_; }
    std::size_t num_nearest_write_queries() const { return num_nearest_write_queries_; }
    std::size_t num_nearest_queries() const { return num_nearest_queries_; }

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

        if (!rewrite_k_loop_as_wave_guard(candidate, w_var->result())) {
            return false;
        }

        if (candidate.inner_wave_parallel) {
            candidate.i_loop->set_parallel(true);
            ++num_parallel_wavefronts_;
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

    bool rewrite_k_loop_as_wave_guard(const SerialWavefrontCandidate &candidate,
                                      yir::Value *wave_iv) {
        auto *j_body = candidate.k_loop->parent();
        std::size_t k_loop_index = 0;
        if (j_body == nullptr || !find_operation_index(*j_body, *candidate.k_loop, k_loop_index)) {
            return false;
        }

        auto *i_iv = candidate.i_loop->induction_var();
        auto *j_iv = candidate.j_loop->induction_var();
        auto *k_iv = candidate.k_loop->induction_var();
        auto *k_lower = candidate.k_loop->lower_bound();
        auto *k_upper = candidate.k_loop->upper_bound();

        auto sub_wave_i = make_parented_op<yir::SubIOp>(
            *j_body, wave_iv, i_iv, wave_temp_name("sub", next_wave_temp_++));
        auto *wave_minus_i = sub_wave_i->result();
        auto sub_wave_i_j = make_parented_op<yir::SubIOp>(
            *j_body, wave_minus_i, j_iv, wave_temp_name("k", next_wave_temp_++));
        auto *computed_k = sub_wave_i_j->result();
        auto assign_k = make_parented_op<yir::AssignOp>(*j_body, k_iv, computed_k);
        auto cmp_ge = make_parented_op<yir::ICmpOp>(
            *j_body, yir::ICmpOp::Predicate::Ge, k_iv, k_lower,
            wave_temp_name("ge", next_wave_temp_++));

        auto outer_if = make_parented_op<yir::IfOp>(*j_body, cmp_ge->result());
        auto cmp_lt = make_parented_op<yir::ICmpOp>(
            outer_if->then_region(), yir::ICmpOp::Predicate::Lt, k_iv, k_upper,
            wave_temp_name("lt", next_wave_temp_++));
        auto inner_if = make_parented_op<yir::IfOp>(outer_if->then_region(), cmp_lt->result());

        auto &old_body = candidate.k_loop->body_region().operations();
        auto &new_body = inner_if->then_region().operations();
        for (auto &op : old_body) {
            op->set_parent(&inner_if->then_region());
            new_body.push_back(std::move(op));
        }
        old_body.clear();

        outer_if->then_region().operations().push_back(std::move(cmp_lt));
        outer_if->then_region().operations().push_back(std::move(inner_if));
        auto finish_k = make_parented_op<yir::AssignOp>(*j_body, k_iv, k_upper);

        std::vector<std::unique_ptr<yir::Operation>> replacements;
        replacements.push_back(std::move(sub_wave_i));
        replacements.push_back(std::move(sub_wave_i_j));
        replacements.push_back(std::move(assign_k));
        replacements.push_back(std::move(cmp_ge));
        replacements.push_back(std::move(outer_if));
        replacements.push_back(std::move(finish_k));

        auto &j_ops = j_body->operations();
        j_ops.erase(j_ops.begin() + static_cast<std::ptrdiff_t>(k_loop_index));
        for (std::size_t i = 0; i < replacements.size(); ++i) {
            j_ops.insert(j_ops.begin() + static_cast<std::ptrdiff_t>(k_loop_index + i),
                         std::move(replacements[i]));
        }
        return true;
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
    std::size_t num_same_iteration_reloads_;
    std::size_t num_stencil_carries_;
    std::size_t num_affine_replacements_;
    std::size_t num_nearest_write_queries_;
    std::size_t num_nearest_queries_;
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
        << ", same_iteration_reloads=" << transformer.num_same_iteration_reloads()
        << ", stencil_carries=" << transformer.num_stencil_carries()
        << ", affine_replacements=" << transformer.num_affine_replacements()
        << ", nearest_write_queries=" << transformer.num_nearest_write_queries()
        << ", nearest_queries=" << transformer.num_nearest_queries();

    return PassResult::ok(changed, oss.str());
}

} // namespace pass
