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
#include <memory>
#include <sstream>
#include <unordered_map>
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

class PolyhedralTransformer {
public:
    explicit PolyhedralTransformer(yir::Module &module, const PolyModelInfo& model_info,
                                   const PolyDependenceInfo& dep_info,
                                   const YIRPolyhedralCanonicalInfo& canonical_info)
        : module_(module), model_info_(model_info), dep_info_(dep_info),
          canonical_info_(canonical_info), num_interchanged_(0), num_tiled_(0),
          num_same_iteration_reloads_(0), num_affine_replacements_(0),
          num_nearest_write_queries_(0), num_nearest_queries_(0) {}

    bool transform() {
        bool changed = false;

        for (const auto& scop : model_info_.models) {
            changed |= try_interchange_or_tile(scop);
        }

        changed |= replace_same_iteration_reloads();
        changed |= replace_equivalent_affine_scalars();
        return changed;
    }

    std::size_t num_interchanged() const { return num_interchanged_; }
    std::size_t num_tiled() const { return num_tiled_; }
    std::size_t num_same_iteration_reloads() const { return num_same_iteration_reloads_; }
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

    bool replace_same_iteration_reloads() {
        bool changed = false;
        for (const auto &scop : model_info_.models) {
            for (const auto &reload : find_same_iteration_reloads(scop)) {
                if (reload.load != nullptr && reload.store != nullptr) {
                    changed = replace_load_with_store_value(*reload.load, *reload.store) || changed;
                }
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
    std::size_t num_same_iteration_reloads_;
    std::size_t num_affine_replacements_;
    std::size_t num_nearest_write_queries_;
    std::size_t num_nearest_queries_;
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
        << ", same_iteration_reloads=" << transformer.num_same_iteration_reloads()
        << ", affine_replacements=" << transformer.num_affine_replacements()
        << ", nearest_write_queries=" << transformer.num_nearest_write_queries()
        << ", nearest_queries=" << transformer.num_nearest_queries();

    return PassResult::ok(changed, oss.str());
}

} // namespace pass
