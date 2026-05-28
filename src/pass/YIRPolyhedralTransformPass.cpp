#include "../../include/pass/YIRPolyhedralTransformPass.h"

#include "../../include/pass/ASTToYIRPass.h"
#include "../../include/pass/YIRPolyhedralCanonicalizePass.h"
#include "../../include/pass/YIRPolyhedralDependenceAnalysisPass.h"
#include "../../include/pass/YIRPolyhedralModelBuildPass.h"
#include "../../include/yir/Presburger.h"
#include "../../include/yir/YIR.h"
#include "../../include/yir/YIRVerifier.h"

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
          num_affine_replacements_(0), num_nearest_queries_(0) {}

    bool transform() {
        bool changed = false;

        for (const auto& scop : model_info_.models) {
            changed |= try_interchange_or_tile(scop);
        }

        changed |= replace_equivalent_affine_scalars();
        return changed;
    }

    std::size_t num_interchanged() const { return num_interchanged_; }
    std::size_t num_tiled() const { return num_tiled_; }
    std::size_t num_affine_replacements() const { return num_affine_replacements_; }
    std::size_t num_nearest_queries() const { return num_nearest_queries_; }

private:
    bool try_interchange_or_tile(const PolyScop& scop) {
        (void)scop;
        (void)dep_info_;
        (void)canonical_info_;
        // TODO(polyhedral): use dependence directions to legalize schedule transforms.
        return false;
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
    std::size_t num_affine_replacements_;
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
        << ", affine_replacements=" << transformer.num_affine_replacements()
        << ", nearest_queries=" << transformer.num_nearest_queries();

    return PassResult::ok(changed, oss.str());
}

} // namespace pass
