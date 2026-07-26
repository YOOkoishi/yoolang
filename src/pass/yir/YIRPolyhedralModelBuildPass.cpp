#include "pass/yir/YIRPolyhedralModelBuildPass.h"
#include "pass/yir/YIRSCoPDetectPass.h"
#include "pass/yir/YIRPolyhedralCanonicalizePass.h"
#include "yir/YIR.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace pass {

namespace {

class AffineExtractor {
public:
    PolyAffineExpr extract(const yir::Value* val) {
        if (val == nullptr) {
            return invalid();
        }

        std::int64_t constant = 0;
        if (const_i32_value(val, constant)) {
            PolyAffineExpr expr;
            expr.constant = constant;
            return expr;
        }

        auto* def = val->defining_op();
        if (auto* add = dynamic_cast<const yir::AddIOp*>(def)) {
            return add_expr(extract(add->lhs()), extract(add->rhs()));
        }
        if (auto* sub = dynamic_cast<const yir::SubIOp*>(def)) {
            return add_expr(extract(sub->lhs()), extract(sub->rhs()), -1);
        }
        if (auto* mul = dynamic_cast<const yir::MulIOp*>(def)) {
            std::int64_t scale = 0;
            if (const_i32_value(mul->lhs(), scale)) {
                return scale_expr(extract(mul->rhs()), scale);
            }
            if (const_i32_value(mul->rhs(), scale)) {
                return scale_expr(extract(mul->lhs()), scale);
            }
            return invalid();
        }

        PolyAffineExpr expr;
        add_term(expr, val, 1);
        return expr;
    }

private:
    static PolyAffineExpr invalid() {
        PolyAffineExpr expr;
        expr.valid = false;
        return expr;
    }

    static bool const_i32_value(const yir::Value* value, std::int64_t& out) {
        auto* constant = value == nullptr
                             ? nullptr
                             : dynamic_cast<const yir::ConstI32Op*>(value->defining_op());
        if (constant == nullptr) {
            return false;
        }
        out = constant->value();
        return true;
    }

    static void add_term(PolyAffineExpr& expr, const yir::Value* value, std::int64_t coefficient) {
        if (value == nullptr || coefficient == 0) {
            return;
        }
        for (auto& term : expr.terms) {
            if (term.first == value) {
                term.second += coefficient;
                normalize_terms(expr);
                return;
            }
        }
        expr.terms.push_back({value, coefficient});
        normalize_terms(expr);
    }

    static void normalize_terms(PolyAffineExpr& expr) {
        expr.terms.erase(std::remove_if(expr.terms.begin(), expr.terms.end(),
                                        [](const auto& term) { return term.second == 0; }),
                         expr.terms.end());
        std::sort(expr.terms.begin(), expr.terms.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.first->name() != rhs.first->name()) {
                return lhs.first->name() < rhs.first->name();
            }
            return lhs.first < rhs.first;
        });
    }

    static PolyAffineExpr add_expr(PolyAffineExpr lhs, const PolyAffineExpr& rhs,
                                   std::int64_t sign = 1) {
        if (!lhs.valid || !rhs.valid) {
            return invalid();
        }
        lhs.constant += sign * rhs.constant;
        for (const auto& term : rhs.terms) {
            add_term(lhs, term.first, sign * term.second);
        }
        normalize_terms(lhs);
        return lhs;
    }

    static PolyAffineExpr scale_expr(PolyAffineExpr expr, std::int64_t scale) {
        if (!expr.valid) {
            return expr;
        }
        expr.constant *= scale;
        for (auto& term : expr.terms) {
            term.second *= scale;
        }
        normalize_terms(expr);
        return expr;
    }
};

class PolyhedralBuilder {
public:
    explicit PolyhedralBuilder(const YIRSCoPInfo& scop_info,
                               const YIRPolyhedralCanonicalInfo&)
        : scop_info_(scop_info) {}

    PolyModelInfo build() {
        PolyModelInfo model_info;
        for (const auto& scop : scop_info_.scops) {
            PolyScop poly_scop;
            poly_scop.id = scop.id;

            std::size_t schedule_depth = 0;
            for (const auto &stmt : scop.statements) {
                schedule_depth = std::max(schedule_depth, stmt.enclosing_loops.size());
            }

            std::unordered_set<const yir::Value*> params;
            for (auto* sym : scop.symbols) {
                poly_scop.params.push_back(sym);
                params.insert(sym);
            }

            for (const auto& stmt : scop.statements) {
                PolyStmt poly_stmt;
                poly_stmt.id = stmt.id;
                poly_stmt.op = stmt.op;
                poly_stmt.op_name = stmt.op == nullptr ? "<null>" : stmt.op->op_name();
                poly_stmt.lexical_id = stmt.id;

                extract_domain(poly_stmt, scop.region, stmt.enclosing_loops,
                               stmt.path_conditions, schedule_depth);
                append_domain_params(poly_stmt, params, poly_scop.params);
                extract_accesses(*stmt.op, params, poly_stmt.reads, poly_stmt.writes);

                if (!poly_stmt.reads.empty() || !poly_stmt.writes.empty()) {
                    poly_scop.statements.push_back(std::move(poly_stmt));
                }
            }
            for (auto &stmt : poly_scop.statements) {
                stmt.domain.params = poly_scop.params;
            }
            if (!poly_scop.statements.empty()) {
                model_info.models.push_back(std::move(poly_scop));
            }
        }
        return model_info;
    }

private:
    static PolyPredicate invert_predicate(PolyPredicate predicate) {
        switch (predicate) {
        case PolyPredicate::Eq: return PolyPredicate::Ne;
        case PolyPredicate::Ne: return PolyPredicate::Eq;
        case PolyPredicate::Lt: return PolyPredicate::Ge;
        case PolyPredicate::Le: return PolyPredicate::Gt;
        case PolyPredicate::Gt: return PolyPredicate::Le;
        case PolyPredicate::Ge: return PolyPredicate::Lt;
        }
        return predicate;
    }

    static PolyPredicate convert_predicate(yir::ICmpOp::Predicate predicate) {
        using YIRPredicate = yir::ICmpOp::Predicate;
        switch (predicate) {
        case YIRPredicate::Eq: return PolyPredicate::Eq;
        case YIRPredicate::Ne: return PolyPredicate::Ne;
        case YIRPredicate::Lt: return PolyPredicate::Lt;
        case YIRPredicate::Le: return PolyPredicate::Le;
        case YIRPredicate::Gt: return PolyPredicate::Gt;
        case YIRPredicate::Ge: return PolyPredicate::Ge;
        }
        return PolyPredicate::Eq;
    }

    static PolyAffineExpr constant_expr(std::int64_t value) {
        PolyAffineExpr expr;
        expr.constant = value;
        return expr;
    }

    static bool find_lexical_rank(const yir::Region &region,
                                  const yir::Operation *target,
                                  std::size_t &cursor, std::size_t &rank) {
        for (const auto &op : region.operations()) {
            const std::size_t current = cursor++;
            if (op.get() == target) {
                rank = current;
                return true;
            }
            if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
                if (find_lexical_rank(if_op->then_region(), target, cursor, rank) ||
                    (if_op->has_else() &&
                     find_lexical_rank(if_op->else_region(), target, cursor, rank))) {
                    return true;
                }
                continue;
            }
            if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
                if (find_lexical_rank(for_op->body_region(), target, cursor, rank)) {
                    return true;
                }
            }
        }
        return false;
    }

    static bool lexical_rank_in(const yir::Region &region,
                                const yir::Operation *target,
                                std::size_t &rank) {
        std::size_t cursor = 0;
        return find_lexical_rank(region, target, cursor, rank);
    }

    void extract_condition(const yir::Value *condition, bool negated,
                           std::vector<PolyDomainConstraint> &constraints,
                           bool &valid) const {
        if (condition == nullptr) {
            valid = false;
            return;
        }
        auto *def = condition->defining_op();
        if (auto *not_op = dynamic_cast<const yir::NotOp *>(def)) {
            extract_condition(not_op->operands().front(), !negated, constraints, valid);
            return;
        }

        AffineExtractor extractor;
        PolyDomainConstraint constraint;
        if (auto *cmp = dynamic_cast<const yir::ICmpOp *>(def)) {
            constraint.lhs = extractor.extract(cmp->lhs());
            constraint.rhs = extractor.extract(cmp->rhs());
            constraint.predicate = convert_predicate(cmp->predicate());
        } else if (auto *to_bool = dynamic_cast<const yir::ToBoolOp *>(def)) {
            constraint.lhs = extractor.extract(to_bool->operands().front());
            constraint.rhs = constant_expr(0);
            constraint.predicate = PolyPredicate::Ne;
        } else if (auto *constant = dynamic_cast<const yir::ConstBoolOp *>(def)) {
            if (constant->value() == negated) valid = false;
            return;
        } else {
            constraint.lhs = extractor.extract(condition);
            constraint.rhs = constant_expr(0);
            constraint.predicate = PolyPredicate::Ne;
        }
        if (negated) constraint.predicate = invert_predicate(constraint.predicate);
        if (!constraint.lhs.valid || !constraint.rhs.valid) valid = false;
        constraints.push_back(std::move(constraint));
    }

    void extract_domain(
        PolyStmt& poly_stmt, const yir::Region *scop_region,
        const std::vector<const yir::ForOp*>& enclosing_loops,
        const std::vector<YIRSCoPStatement::PathCondition> &path_conditions,
        std::size_t schedule_depth) {
        AffineExtractor extractor;
        for (const auto* for_op : enclosing_loops) {
            poly_stmt.dims.push_back(for_op->induction_var());
            poly_stmt.schedule_dims.push_back(for_op->induction_var());
            poly_stmt.domain.dims.push_back(for_op->induction_var());
            poly_stmt.schedule.input_dims.push_back(for_op->induction_var());

            PolyLoopBound bound;
            bound.iv = for_op->induction_var();
            bound.lower = extractor.extract(for_op->lower_bound());
            bound.upper = extractor.extract(for_op->upper_bound());
            poly_stmt.loop_bounds.push_back(bound);

            const auto iv = extractor.extract(for_op->induction_var());
            poly_stmt.domain.constraints.push_back(
                {bound.lower, iv, PolyPredicate::Le});
            poly_stmt.domain.constraints.push_back(
                {iv, bound.upper, PolyPredicate::Lt});
            if (!bound.lower.valid || !bound.upper.valid) poly_stmt.domain.valid = false;
        }

        for (const auto &path_condition : path_conditions) {
            extract_condition(path_condition.condition, path_condition.negated,
                              poly_stmt.domain.constraints, poly_stmt.domain.valid);
        }

        std::size_t root_rank = 0;
        if (scop_region == nullptr || enclosing_loops.empty() ||
            !lexical_rank_in(*scop_region, enclosing_loops.front(), root_rank)) {
            poly_stmt.schedule.valid = false;
            root_rank = poly_stmt.lexical_id;
        }
        poly_stmt.schedule.output_dims.push_back(
            constant_expr(static_cast<std::int64_t>(root_rank)));

        for (std::size_t dim = 0; dim < schedule_depth; ++dim) {
            if (dim >= enclosing_loops.size()) {
                poly_stmt.schedule.output_dims.push_back(constant_expr(0));
                poly_stmt.schedule.output_dims.push_back(constant_expr(0));
                continue;
            }

            const auto *loop = enclosing_loops[dim];
            poly_stmt.schedule.output_dims.push_back(
                extractor.extract(loop->induction_var()));

            const yir::Operation *next = dim + 1 < enclosing_loops.size()
                                             ? static_cast<const yir::Operation *>(
                                                   enclosing_loops[dim + 1])
                                             : poly_stmt.op;
            std::size_t lexical_rank = 0;
            if (!lexical_rank_in(loop->body_region(), next, lexical_rank)) {
                poly_stmt.schedule.valid = false;
                lexical_rank = poly_stmt.lexical_id;
            }
            poly_stmt.schedule.output_dims.push_back(
                constant_expr(static_cast<std::int64_t>(lexical_rank)));
        }
        poly_stmt.schedule.lexical_order = poly_stmt.lexical_id;
        poly_stmt.schedule.output_dims.push_back(
            constant_expr(static_cast<std::int64_t>(poly_stmt.lexical_id)));
    }

    static void append_domain_params(PolyStmt &stmt,
                                     std::unordered_set<const yir::Value *> &params,
                                     std::vector<const yir::Value *> &scop_params) {
        std::unordered_set<const yir::Value *> dims(stmt.domain.dims.begin(),
                                                    stmt.domain.dims.end());
        const auto visit_expr = [&](const PolyAffineExpr &expr) {
            for (const auto &[value, coefficient] : expr.terms) {
                if (coefficient == 0 || dims.count(value) != 0 || !params.insert(value).second) {
                    continue;
                }
                scop_params.push_back(value);
            }
        };
        for (const auto &constraint : stmt.domain.constraints) {
            visit_expr(constraint.lhs);
            visit_expr(constraint.rhs);
        }
    }

    // Try to delinearize a flattened 1D access `e = high * stride + low` where `stride`
    // is a SCoP parameter and `high`/`low` are independently affine in the loop IVs and
    // symbols. On success, fills `out_dims` with [high_expr, low_expr] (high-order first)
    // and returns true. Conservative — only handles a single stride factor. Used to
    // recover `A[i*N + j]` as `A[i][j]`.
    static bool try_delinearize_1d(const yir::Value* index,
                                   const std::unordered_set<const yir::Value*>& params,
                                   std::vector<PolyAffineExpr>& out_dims) {
        if (index == nullptr || index->defining_op() == nullptr) {
            return false;
        }
        auto* add = dynamic_cast<const yir::AddIOp*>(index->defining_op());
        if (add == nullptr) {
            return false;
        }

        const auto try_split = [&](const yir::Value* mul_side, const yir::Value* low_side) {
            auto* mul = mul_side == nullptr
                            ? nullptr
                            : dynamic_cast<const yir::MulIOp*>(mul_side->defining_op());
            if (mul == nullptr) {
                return false;
            }
            const yir::Value* stride = nullptr;
            const yir::Value* high_side = nullptr;
            if (params.count(mul->lhs()) != 0) {
                stride = mul->lhs();
                high_side = mul->rhs();
            } else if (params.count(mul->rhs()) != 0) {
                stride = mul->rhs();
                high_side = mul->lhs();
            } else {
                return false;
            }

            AffineExtractor extractor;
            auto high_expr = extractor.extract(high_side);
            auto low_expr = extractor.extract(low_side);
            if (!high_expr.valid || !low_expr.valid) {
                return false;
            }

            out_dims.clear();
            out_dims.push_back(std::move(high_expr));
            out_dims.push_back(std::move(low_expr));
            (void)stride;
            return true;
        };

        return try_split(add->lhs(), add->rhs()) || try_split(add->rhs(), add->lhs());
    }

    void extract_accesses(const yir::Operation& op,
                          const std::unordered_set<const yir::Value*>& params,
                          std::vector<PolyAccess>& reads, std::vector<PolyAccess>& writes) {
        AffineExtractor extractor;
        const auto fill_indices = [&](const std::vector<yir::Value*>& index_values,
                                      std::vector<PolyAffineExpr>& dest) {
            if (index_values.size() == 1) {
                std::vector<PolyAffineExpr> delinearized;
                if (try_delinearize_1d(index_values.front(), params, delinearized)) {
                    dest = std::move(delinearized);
                    return;
                }
            }
            for (auto* index_val : index_values) {
                dest.push_back(extractor.extract(index_val));
            }
        };

        if (auto* load = dynamic_cast<const yir::ArrayLoadOp*>(&op)) {
            PolyAccess access;
            access.kind = PolyAccess::Kind::Read;
            access.memory = load->array();
            fill_indices(load->indices(), access.indices);
            reads.push_back(std::move(access));
            return;
        }

        if (auto* store = dynamic_cast<const yir::ArrayStoreOp*>(&op)) {
            PolyAccess access;
            access.kind = PolyAccess::Kind::Write;
            access.memory = store->array();
            fill_indices(store->indices(), access.indices);
            writes.push_back(std::move(access));
        }
    }

    const YIRSCoPInfo& scop_info_;
};

} // namespace

std::string_view YIRPolyhedralModelBuildPass::name() const {
    return "YIRPolyhedralModelBuildPass";
}

PassKind YIRPolyhedralModelBuildPass::kind() const {
    return PassKind::Analysis;
}

PassResult YIRPolyhedralModelBuildPass::run(PassContext &context) {
    auto *scop_info = context.get_artifact<YIRSCoPInfo>(std::string(YIRSCoPDetectPass::kArtifactKey));
    if (!scop_info) {
        return PassResult::fail("YIRPolyhedralModelBuildPass requires YIRSCoPInfo.");
    }

    auto *canonical_info = context.get_artifact<YIRPolyhedralCanonicalInfo>(
        std::string(YIRPolyhedralCanonicalizePass::kArtifactKey));
    if (!canonical_info) {
        return PassResult::fail("YIRPolyhedralModelBuildPass requires YIRPolyhedralCanonicalInfo.");
    }

    PolyhedralBuilder builder(*scop_info, *canonical_info);
    PolyModelInfo info = builder.build();

    std::size_t num_models = info.models.size();
    context.set_artifact<PolyModelInfo>(std::string(kArtifactKey), std::move(info));

    std::ostringstream oss;
    oss << "Built " << num_models << " Custom Polyhedral Models.";
    return PassResult::ok(false, oss.str());
}

} // namespace pass
