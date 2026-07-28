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
    explicit AffineExtractor(
        const std::unordered_set<const yir::Value *> *known_nonnegative = nullptr)
        : known_nonnegative_(known_nonnegative) {}

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
        if (auto *div = dynamic_cast<const yir::DivSIOp *>(def)) {
            std::int64_t divisor = 0;
            if (!const_i32_value(div->rhs(), divisor) || divisor <= 0) {
                return invalid();
            }
            auto operand = extract(div->lhs());
            if (!is_known_nonnegative(operand)) {
                return invalid();
            }
            return quasi_affine(PolyAffineExprKind::FloorDiv, std::move(operand), divisor);
        }
        if (auto *rem = dynamic_cast<const yir::RemSIOp *>(def)) {
            std::int64_t divisor = 0;
            if (!const_i32_value(rem->rhs(), divisor) || divisor <= 0) {
                return invalid();
            }
            auto operand = extract(rem->lhs());
            if (!is_known_nonnegative(operand)) {
                return invalid();
            }
            return quasi_affine(PolyAffineExprKind::Mod, std::move(operand), divisor);
        }

        PolyAffineExpr expr;
        add_term(expr, val, 1);
        return expr;
    }

private:
    bool is_known_nonnegative(const PolyAffineExpr &expr) const {
        if (!expr.valid) {
            return false;
        }
        if (expr.is_composite_affine()) {
            const auto &lhs = *expr.operands[0];
            const auto &rhs = *expr.operands[1];
            if (expr.kind == PolyAffineExprKind::Add) {
                return is_known_nonnegative(lhs) && is_known_nonnegative(rhs);
            }
            if (expr.kind == PolyAffineExprKind::Mul) {
                if (lhs.is_linear() && lhs.terms.empty() && lhs.constant >= 0) {
                    return lhs.constant == 0 || is_known_nonnegative(rhs);
                }
                if (rhs.is_linear() && rhs.terms.empty() && rhs.constant >= 0) {
                    return rhs.constant == 0 || is_known_nonnegative(lhs);
                }
            }
            return false;
        }
        if (expr.is_quasi_affine()) {
            return expr.kind == PolyAffineExprKind::FloorDiv ||
                   expr.kind == PolyAffineExprKind::CeilDiv ||
                   expr.kind == PolyAffineExprKind::Mod;
        }
        if (!expr.is_linear() || expr.constant < 0 || known_nonnegative_ == nullptr) {
            return expr.is_linear() && expr.terms.empty() && expr.constant >= 0;
        }
        return std::all_of(expr.terms.begin(), expr.terms.end(), [this](const auto &term) {
            return term.second >= 0 && known_nonnegative_->count(term.first) != 0;
        });
    }

    static PolyAffineExpr invalid() {
        PolyAffineExpr expr;
        expr.valid = false;
        return expr;
    }

    static PolyAffineExpr quasi_affine(PolyAffineExprKind kind, PolyAffineExpr operand,
                                       std::int64_t divisor) {
        if (!operand.valid || divisor <= 0) {
            return invalid();
        }
        PolyAffineExpr expr;
        expr.kind = kind;
        expr.operand = std::make_shared<PolyAffineExpr>(std::move(operand));
        expr.divisor = divisor;
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
        if (!lhs.is_linear() || !rhs.is_linear()) {
            PolyAffineExpr expr;
            expr.kind = sign == 1 ? PolyAffineExprKind::Add : PolyAffineExprKind::Sub;
            expr.operands.push_back(std::make_shared<PolyAffineExpr>(std::move(lhs)));
            expr.operands.push_back(std::make_shared<PolyAffineExpr>(rhs));
            return expr;
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
            return invalid();
        }
        if (!expr.is_linear()) {
            if (scale == 1) {
                return expr;
            }
            PolyAffineExpr factor;
            factor.constant = scale;
            PolyAffineExpr result;
            result.kind = PolyAffineExprKind::Mul;
            result.operands.push_back(std::make_shared<PolyAffineExpr>(std::move(factor)));
            result.operands.push_back(std::make_shared<PolyAffineExpr>(std::move(expr)));
            return result;
        }
        expr.constant *= scale;
        for (auto& term : expr.terms) {
            term.second *= scale;
        }
        normalize_terms(expr);
        return expr;
    }

    const std::unordered_set<const yir::Value *> *known_nonnegative_;
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
                poly_stmt.has_opaque_conditions = stmt.has_opaque_conditions;

                const auto known_nonnegative =
                    collect_known_nonnegative_values(stmt.enclosing_loops);
                extract_domain(poly_stmt, scop.region, stmt.enclosing_loops,
                               stmt.path_conditions, schedule_depth, known_nonnegative);
                append_domain_params(poly_stmt, params, poly_scop.params);
                extract_accesses(*stmt.op, params, known_nonnegative,
                                 poly_stmt.reads, poly_stmt.writes);

                if (!poly_stmt.reads.empty() || !poly_stmt.writes.empty()) {
                    poly_scop.statements.push_back(std::move(poly_stmt));
                }
            }
            if (scop.region != nullptr) {
                annotate_reductions(poly_scop, *scop.region);
            }
            finalize_schedule_space(poly_scop);
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
    static void finalize_schedule_space(PolyScop &scop) {
        const PolyStmt *anchor = nullptr;
        for (const auto &stmt : scop.statements) {
            if (anchor == nullptr || stmt.schedule_dims.size() > anchor->schedule_dims.size()) {
                anchor = &stmt;
            }
        }
        if (anchor != nullptr) {
            scop.schedule_space_dims = anchor->schedule_dims;
        }

        for (auto &stmt : scop.statements) {
            auto &schedule = stmt.schedule;
            schedule.space_dims = scop.schedule_space_dims;
            schedule.matrix.assign(schedule.output_dims.size(),
                                   std::vector<std::int64_t>(schedule.space_dims.size(), 0));
            schedule.matrix_constants.assign(schedule.output_dims.size(), 0);
            schedule.matrix_valid = true;
            for (std::size_t row = 0; row < schedule.output_dims.size(); ++row) {
                const auto &expr = schedule.output_dims[row];
                if (!expr.is_linear()) {
                    schedule.matrix_valid = false;
                    continue;
                }
                schedule.matrix_constants[row] = expr.constant;
                for (const auto &[value, coefficient] : expr.terms) {
                    const auto local = std::find(stmt.schedule.input_dims.begin(),
                                                  stmt.schedule.input_dims.end(), value);
                    if (local == stmt.schedule.input_dims.end()) {
                        schedule.matrix_valid = false;
                        continue;
                    }
                    const auto position = static_cast<std::size_t>(
                        local - stmt.schedule.input_dims.begin());
                    if (position >= schedule.space_dims.size()) {
                        schedule.matrix_valid = false;
                        continue;
                    }
                    schedule.matrix[row][position] += coefficient;
                }
            }
        }
    }

    static bool same_array_element(const yir::ArrayLoadOp &load,
                                   const yir::ArrayStoreOp &store) {
        if (load.array() != store.array()) {
            return false;
        }
        const auto load_indices = load.indices();
        const auto store_indices = store.indices();
        if (load_indices.size() != store_indices.size()) {
            return false;
        }
        for (std::size_t i = 0; i < load_indices.size(); ++i) {
            if (load_indices[i] == store_indices[i]) {
                continue;
            }
            auto *load_constant = dynamic_cast<const yir::ConstI32Op *>(
                load_indices[i] == nullptr ? nullptr : load_indices[i]->defining_op());
            auto *store_constant = dynamic_cast<const yir::ConstI32Op *>(
                store_indices[i] == nullptr ? nullptr : store_indices[i]->defining_op());
            if (load_constant == nullptr || store_constant == nullptr ||
                load_constant->value() != store_constant->value()) {
                return false;
            }
        }
        return true;
    }

    static const yir::ArrayLoadOp *reduction_load_for_store(
        const yir::ArrayStoreOp &store) {
        if (store.value() == nullptr) {
            return nullptr;
        }
        auto *add = dynamic_cast<const yir::AddIOp *>(store.value()->defining_op());
        if (add == nullptr) {
            return nullptr;
        }

        const auto *lhs_load = dynamic_cast<const yir::ArrayLoadOp *>(add->lhs()->defining_op());
        const auto *rhs_load = dynamic_cast<const yir::ArrayLoadOp *>(add->rhs()->defining_op());
        const yir::ArrayLoadOp *load = nullptr;
        if (lhs_load != nullptr && same_array_element(*lhs_load, store)) {
            load = lhs_load;
        } else if (rhs_load != nullptr && same_array_element(*rhs_load, store)) {
            load = rhs_load;
        }
        return load;
    }

    static std::vector<bool> reduction_dimensions(const PolyStmt &store_stmt) {
        std::vector<bool> dims(store_stmt.schedule_dims.size(), true);
        for (std::size_t depth = 0; depth < store_stmt.schedule_dims.size(); ++depth) {
            const auto *dim = store_stmt.schedule_dims[depth];
            for (const auto &access : store_stmt.writes) {
                for (const auto &index : access.indices) {
                    if (!index.is_linear()) {
                        dims[depth] = false;
                        continue;
                    }
                    const bool used = std::any_of(
                        index.terms.begin(), index.terms.end(), [dim](const auto &term) {
                            return term.first == dim && term.second != 0;
                        });
                    if (used) {
                        dims[depth] = false;
                    }
                }
            }
        }
        return dims;
    }

    static std::size_t count_value_uses(const yir::Region &region,
                                        const yir::Value *value) {
        std::size_t uses = 0;
        for (const auto &op : region.operations()) {
            uses += static_cast<std::size_t>(
                std::count(op->operands().begin(), op->operands().end(), value));
            if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
                uses += count_value_uses(if_op->then_region(), value);
                if (if_op->has_else()) {
                    uses += count_value_uses(if_op->else_region(), value);
                }
            } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
                uses += count_value_uses(for_op->body_region(), value);
            }
        }
        return uses;
    }

    static void annotate_reductions(PolyScop &scop, const yir::Region &root) {
        for (auto &store_stmt : scop.statements) {
            auto *store = dynamic_cast<const yir::ArrayStoreOp *>(store_stmt.op);
            if (store == nullptr) {
                continue;
            }
            const auto *load = reduction_load_for_store(*store);
            if (load == nullptr || load->result() == nullptr ||
                store->value() == nullptr ||
                count_value_uses(root, load->result()) != 1 ||
                count_value_uses(root, store->value()) != 1) {
                continue;
            }

            const auto dims = reduction_dimensions(store_stmt);
            store_stmt.reduction_kind = PolyStmt::ReductionKind::Add;
            store_stmt.reduction_memory = store->array();
            store_stmt.reduction_group_id = store_stmt.id + 1;
            store_stmt.reduction_dims = dims;

            for (auto &load_stmt : scop.statements) {
                if (load_stmt.op != load) {
                    continue;
                }
                load_stmt.reduction_kind = PolyStmt::ReductionKind::Add;
                load_stmt.reduction_memory = load->array();
                load_stmt.reduction_group_id = store_stmt.id + 1;
                load_stmt.reduction_dims = dims;
                break;
            }
        }
    }

    static bool const_i32_value(const yir::Value *value, std::int64_t &out) {
        auto *constant = value == nullptr
                             ? nullptr
                             : dynamic_cast<const yir::ConstI32Op *>(value->defining_op());
        if (constant == nullptr) {
            return false;
        }
        out = constant->value();
        return true;
    }

    static std::unordered_set<const yir::Value *> collect_known_nonnegative_values(
        const std::vector<const yir::ForOp *> &loops) {
        std::unordered_set<const yir::Value *> values;
        for (const auto *loop : loops) {
            std::int64_t lower = 0;
            std::int64_t step = 0;
            if (loop != nullptr && const_i32_value(loop->lower_bound(), lower) && lower >= 0 &&
                const_i32_value(loop->step(), step) && step > 0) {
                values.insert(loop->induction_var());
                // A statement in this loop only executes when
                // lower <= iv < upper, hence upper is positive here.
                values.insert(loop->upper_bound());
            }
        }
        return values;
    }

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
                           bool &valid,
                           const std::unordered_set<const yir::Value *> &known_nonnegative) const {
        if (condition == nullptr) {
            valid = false;
            return;
        }
        auto *def = condition->defining_op();
        if (auto *not_op = dynamic_cast<const yir::NotOp *>(def)) {
            extract_condition(not_op->operands().front(), !negated, constraints, valid,
                              known_nonnegative);
            return;
        }

        AffineExtractor extractor(&known_nonnegative);
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
        // A pure non-affine predicate is represented by the unconstrained
        // over-approximation of its surrounding loop domain. This keeps the
        // memory statements available to dependence analysis while retaining
        // a marker on PolyStmt for conservative transform policy.
        if (!constraint.lhs.valid || !constraint.rhs.valid) return;
        constraints.push_back(std::move(constraint));
    }

    void extract_domain(
        PolyStmt& poly_stmt, const yir::Region *scop_region,
        const std::vector<const yir::ForOp*>& enclosing_loops,
        const std::vector<YIRSCoPStatement::PathCondition> &path_conditions,
        std::size_t schedule_depth,
        const std::unordered_set<const yir::Value *> &known_nonnegative) {
        AffineExtractor extractor(&known_nonnegative);
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
                              poly_stmt.domain.constraints, poly_stmt.domain.valid,
                              known_nonnegative);
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
        for (std::size_t dim = 0; dim < schedule_depth; ++dim) {
            PolyAffineSchedule::Band band;
            band.output_offset = 1 + dim * 2;
            band.dimension_count = 1;
            band.permutable = false;
            band.coincident.push_back(false);
            band.reduction.push_back(false);
            poly_stmt.schedule.bands.push_back(std::move(band));
        }
    }

    static void append_domain_params(PolyStmt &stmt,
                                     std::unordered_set<const yir::Value *> &params,
                                     std::vector<const yir::Value *> &scop_params) {
        std::unordered_set<const yir::Value *> dims(stmt.domain.dims.begin(),
                                                    stmt.domain.dims.end());
        const auto visit_expr = [&](const PolyAffineExpr &expr, const auto &self) -> void {
            for (const auto &[value, coefficient] : expr.terms) {
                if (coefficient == 0 || dims.count(value) != 0 || !params.insert(value).second) {
                    continue;
                }
                scop_params.push_back(value);
            }
            if (expr.operand != nullptr) {
                self(*expr.operand, self);
            }
            for (const auto &operand : expr.operands) {
                if (operand != nullptr) {
                    self(*operand, self);
                }
            }
        };
        for (const auto &constraint : stmt.domain.constraints) {
            visit_expr(constraint.lhs, visit_expr);
            visit_expr(constraint.rhs, visit_expr);
        }
    }

    // Try to delinearize a flattened 1D access `e = high * stride + low` where `stride`
    // is a SCoP parameter and `high`/`low` are independently affine in the loop IVs and
    // symbols. On success, fills `out_dims` with [high_expr, low_expr] (high-order first)
    // and returns true. Conservative — only handles a single stride factor. Used to
    // recover `A[i*N + j]` as `A[i][j]`.
    static bool try_delinearize_1d(const yir::Value* index,
                                   const std::unordered_set<const yir::Value*>& params,
                                   const std::unordered_set<const yir::Value*>& known_nonnegative,
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

            AffineExtractor extractor(&known_nonnegative);
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
                          const std::unordered_set<const yir::Value*>& known_nonnegative,
                          std::vector<PolyAccess>& reads, std::vector<PolyAccess>& writes) {
        AffineExtractor extractor(&known_nonnegative);
        const auto fill_indices = [&](const std::vector<yir::Value*>& index_values,
                                      std::vector<PolyAffineExpr>& dest) {
            if (index_values.size() == 1) {
                std::vector<PolyAffineExpr> delinearized;
                if (try_delinearize_1d(index_values.front(), params, known_nonnegative,
                                       delinearized)) {
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
