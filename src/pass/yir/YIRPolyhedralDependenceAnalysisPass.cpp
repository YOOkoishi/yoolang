#include "pass/yir/YIRPolyhedralDependenceAnalysisPass.h"
#include "pass/yir/YIRPolyhedralModelBuildPass.h"
#include "yir/YIR.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pass {

namespace {

struct MemoryStmtAccesses {
    const PolyStmt* stmt = nullptr;
    std::vector<const PolyAccess*> reads;
    std::vector<const PolyAccess*> writes;
};

class DependenceRelationBuilder {
public:
    explicit DependenceRelationBuilder(const PolyScop &scop) : params_(scop.params) {}

    PolyDependence::Relation build(const PolyStmt &source, const PolyAccess &source_access,
                                   const PolyStmt &target,
                                   const PolyAccess &target_access) const {
        PolyDependence::Relation out;
        out.source_dims = source.domain.dims;
        out.target_dims = target.domain.dims;
        out.params = params_;

        const unsigned num_source = static_cast<unsigned>(out.source_dims.size());
        const unsigned num_target = static_cast<unsigned>(out.target_dims.size());
        const unsigned num_params = static_cast<unsigned>(out.params.size());
        std::vector<yir::presburger::IntegerRelation> pieces;
        pieces.emplace_back(num_source, num_target, num_params);

        bool exact = source.domain.valid && target.domain.valid &&
                     source.schedule.valid && target.schedule.valid;
        if (source.schedule.space_dims != target.schedule.space_dims) {
            exact = false;
        }
        add_domain(source.domain, true, out, pieces, exact);
        add_domain(target.domain, false, out, pieces, exact);

        if (source_access.indices.size() != target_access.indices.size()) {
            exact = false;
        } else {
            for (std::size_t dim = 0; dim < source_access.indices.size(); ++dim) {
                add_equality(source_access.indices[dim], true,
                             target_access.indices[dim], false, out, pieces, exact);
            }
        }

        yir::presburger::PresburgerRelation ordered;
        const auto schedule_dims = source.schedule.output_dims.size();
        if (schedule_dims != target.schedule.output_dims.size() || schedule_dims == 0 ||
            (!pieces.empty() && schedule_dims > kMaxRelationDisjuncts / pieces.size())) {
            exact = false;
            for (auto &piece : pieces) ordered.union_in_place(std::move(piece));
        } else {
            for (std::size_t first_diff = 0; first_diff < schedule_dims; ++first_diff) {
                for (const auto &base : pieces) {
                    auto ordered_piece = base;
                    bool piece_exact = true;
                    for (std::size_t prefix = 0; prefix < first_diff; ++prefix) {
                        add_equality(schedule_expr(source, prefix), true,
                                     schedule_expr(target, prefix), false,
                                     out, ordered_piece, piece_exact);
                    }
                    add_strict_order(schedule_expr(source, first_diff),
                                     schedule_expr(target, first_diff), out,
                                     ordered_piece, piece_exact);
                    exact = exact && piece_exact;
                    ordered.union_in_place(std::move(ordered_piece));
                }
            }
        }
        out.constraints = std::move(ordered);
        for (const auto &piece : out.constraints.disjuncts()) {
            out.local_vars = std::max<std::size_t>(out.local_vars, piece.num_local_vars());
        }
        out.exact = exact;
        return out;
    }

private:
    static constexpr std::size_t kMaxRelationDisjuncts = 512;

    static PolyAffineExpr schedule_expr(const PolyStmt &stmt, std::size_t row) {
        const auto &schedule = stmt.schedule;
        if (!schedule.matrix_valid || row >= schedule.matrix.size() ||
            row >= schedule.matrix_constants.size()) {
            return schedule.output_dims[row];
        }
        PolyAffineExpr expr;
        expr.constant = schedule.matrix_constants[row];
        const auto &coefficients = schedule.matrix[row];
        if (coefficients.size() != schedule.space_dims.size()) {
            expr.valid = false;
            return expr;
        }
        const auto count = std::min(coefficients.size(), schedule.input_dims.size());
        for (std::size_t position = 0; position < count; ++position) {
            if (coefficients[position] != 0) {
                expr.terms.emplace_back(schedule.input_dims[position], coefficients[position]);
            }
        }
        for (std::size_t position = count; position < coefficients.size(); ++position) {
            if (coefficients[position] != 0) {
                expr.valid = false;
                break;
            }
        }
        return expr;
    }

    struct LinearForm {
        std::vector<std::int64_t> coefficients;
        std::int64_t constant = 0;
        bool valid = true;
    };

    static bool checked_i64(__int128 value, std::int64_t &out) {
        if (value < std::numeric_limits<std::int64_t>::min() ||
            value > std::numeric_limits<std::int64_t>::max()) {
            return false;
        }
        out = static_cast<std::int64_t>(value);
        return true;
    }

    static void resize_form(LinearForm &form, std::size_t size) {
        form.coefficients.resize(size, 0);
    }

    static LinearForm combine(LinearForm lhs, LinearForm rhs, std::int64_t sign) {
        const auto size = std::max(lhs.coefficients.size(), rhs.coefficients.size());
        resize_form(lhs, size);
        resize_form(rhs, size);
        lhs.valid = lhs.valid && rhs.valid;
        std::int64_t constant = 0;
        if (!checked_i64(static_cast<__int128>(lhs.constant) +
                             static_cast<__int128>(sign) * rhs.constant,
                         constant)) {
            lhs.valid = false;
        } else {
            lhs.constant = constant;
        }
        for (std::size_t index = 0; index < size; ++index) {
            std::int64_t coefficient = 0;
            if (!checked_i64(static_cast<__int128>(lhs.coefficients[index]) +
                                 static_cast<__int128>(sign) * rhs.coefficients[index],
                             coefficient)) {
                lhs.valid = false;
                break;
            }
            lhs.coefficients[index] = coefficient;
        }
        return lhs;
    }

    static LinearForm scale(LinearForm form, std::int64_t factor) {
        std::int64_t constant = 0;
        if (!checked_i64(static_cast<__int128>(form.constant) * factor, constant)) {
            form.valid = false;
            return form;
        }
        form.constant = constant;
        for (auto &coefficient : form.coefficients) {
            std::int64_t scaled = 0;
            if (!checked_i64(static_cast<__int128>(coefficient) * factor, scaled)) {
                form.valid = false;
                return form;
            }
            coefficient = scaled;
        }
        return form;
    }

    static bool affine_constant(const PolyAffineExpr &expr, std::int64_t &value) {
        if (!expr.is_linear() || !expr.terms.empty()) {
            return false;
        }
        value = expr.constant;
        return true;
    }

    LinearForm map_expr(const PolyAffineExpr &expr, bool source_side,
                        const PolyDependence::Relation &space,
                        yir::presburger::IntegerRelation &piece) const {
        LinearForm form;
        form.coefficients.assign(piece.num_vars(), 0);
        form.constant = expr.constant;
        form.valid = expr.valid;

        if (expr.is_linear()) {
            for (const auto &[value, coefficient] : expr.terms) {
                if (coefficient == 0) continue;
                const auto &dims = source_side ? space.source_dims : space.target_dims;
                auto dim = std::find(dims.begin(), dims.end(), value);
                if (dim != dims.end()) {
                    std::size_t index = static_cast<std::size_t>(dim - dims.begin());
                    if (!source_side) index += space.source_dims.size();
                    form.coefficients[index] += coefficient;
                    continue;
                }
                auto param = std::find(space.params.begin(), space.params.end(), value);
                if (param == space.params.end()) {
                    form.valid = false;
                    continue;
                }
                const std::size_t index = space.source_dims.size() + space.target_dims.size() +
                                          static_cast<std::size_t>(param - space.params.begin());
                form.coefficients[index] += coefficient;
            }
            return form;
        }

        if (expr.is_composite_affine()) {
            auto lhs = map_expr(*expr.operands[0], source_side, space, piece);
            auto rhs = map_expr(*expr.operands[1], source_side, space, piece);
            if (expr.kind == PolyAffineExprKind::Add) {
                return combine(std::move(lhs), std::move(rhs), 1);
            }
            if (expr.kind == PolyAffineExprKind::Sub) {
                return combine(std::move(lhs), std::move(rhs), -1);
            }
            std::int64_t factor = 0;
            if (affine_constant(*expr.operands[0], factor)) {
                return scale(std::move(rhs), factor);
            }
            if (affine_constant(*expr.operands[1], factor)) {
                return scale(std::move(lhs), factor);
            }
            form.valid = false;
            return form;
        }

        if (!expr.is_quasi_affine()) {
            form.valid = false;
            return form;
        }

        auto operand = map_expr(*expr.operand, source_side, space, piece);
        if (!operand.valid || expr.divisor <= 0) {
            operand.valid = false;
            return operand;
        }
        const auto local_index = piece.num_vars();
        piece.add_vars(yir::presburger::VarKind::Local, 1);
        resize_form(operand, piece.num_vars());

        auto quotient = operand;
        std::fill(quotient.coefficients.begin(), quotient.coefficients.end(), 0);
        quotient.constant = 0;
        quotient.coefficients[local_index] = 1;

        auto add_floor_constraints = [&]() {
            auto lower = operand;
            lower.coefficients[local_index] -= expr.divisor;
            piece.add_inequality(std::move(lower.coefficients), lower.constant);

            auto upper = scale(operand, -1);
            upper.coefficients[local_index] += expr.divisor;
            std::int64_t constant = 0;
            if (!checked_i64(static_cast<__int128>(upper.constant) + expr.divisor - 1,
                             constant)) {
                quotient.valid = false;
                return;
            }
            piece.add_inequality(std::move(upper.coefficients), constant);
        };

        if (expr.kind == PolyAffineExprKind::FloorDiv ||
            expr.kind == PolyAffineExprKind::Mod) {
            add_floor_constraints();
            if (!quotient.valid) {
                return quotient;
            }
            if (expr.kind == PolyAffineExprKind::FloorDiv) {
                return quotient;
            }
            return combine(std::move(operand), scale(std::move(quotient), expr.divisor), -1);
        }

        // q = ceil(e / d): d*q - e >= 0 and e - d*q + d - 1 >= 0.
        auto upper = scale(operand, -1);
        upper.coefficients[local_index] += expr.divisor;
        piece.add_inequality(std::move(upper.coefficients), upper.constant);
        auto lower = operand;
        lower.coefficients[local_index] -= expr.divisor;
        std::int64_t lower_constant = 0;
        if (!checked_i64(static_cast<__int128>(lower.constant) + expr.divisor - 1,
                         lower_constant)) {
            quotient.valid = false;
            return quotient;
        }
        piece.add_inequality(std::move(lower.coefficients), lower_constant);
        return quotient;
    }

    static LinearForm subtract(LinearForm lhs, LinearForm rhs) {
        return combine(std::move(lhs), std::move(rhs), -1);
    }

    void add_domain(const PolyDomain &domain, bool source_side,
                    const PolyDependence::Relation &space,
                    std::vector<yir::presburger::IntegerRelation> &pieces,
                    bool &exact) const {
        for (const auto &constraint : domain.constraints) {
            if (constraint.predicate == PolyPredicate::Ne) {
                if (pieces.size() > kMaxRelationDisjuncts / 2) {
                    exact = false;
                    continue;
                }
                std::vector<yir::presburger::IntegerRelation> split;
                split.reserve(pieces.size() * 2);
                for (auto piece : pieces) {
                    auto lhs = map_expr(constraint.lhs, source_side, space, piece);
                    auto rhs = map_expr(constraint.rhs, source_side, space, piece);
                    auto diff = subtract(std::move(lhs), std::move(rhs));
                    exact = exact && diff.valid;
                    if (!diff.valid) {
                        split.push_back(std::move(piece));
                        continue;
                    }
                    auto positive = piece;
                    positive.add_inequality(diff.coefficients, diff.constant - 1);
                    split.push_back(std::move(positive));
                    auto negative = piece;
                    auto coefficients = diff.coefficients;
                    for (auto &coefficient : coefficients) coefficient = -coefficient;
                    negative.add_inequality(std::move(coefficients), -diff.constant - 1);
                    split.push_back(std::move(negative));
                }
                pieces = std::move(split);
                continue;
            }
            for (auto &piece : pieces) {
                auto lhs = map_expr(constraint.lhs, source_side, space, piece);
                auto rhs = map_expr(constraint.rhs, source_side, space, piece);
                auto diff = subtract(std::move(lhs), std::move(rhs));
                exact = exact && diff.valid;
                if (diff.valid) {
                    add_predicate(piece, diff, constraint.predicate);
                }
            }
        }
    }

    static void add_predicate(yir::presburger::IntegerRelation &relation,
                              const LinearForm &diff, PolyPredicate predicate) {
        auto coefficients = diff.coefficients;
        auto constant = diff.constant;
        switch (predicate) {
        case PolyPredicate::Eq:
            relation.add_equality(std::move(coefficients), constant);
            return;
        case PolyPredicate::Ge:
            relation.add_inequality(std::move(coefficients), constant);
            return;
        case PolyPredicate::Gt:
            relation.add_inequality(std::move(coefficients), constant - 1);
            return;
        case PolyPredicate::Le:
            for (auto &coefficient : coefficients) coefficient = -coefficient;
            relation.add_inequality(std::move(coefficients), -constant);
            return;
        case PolyPredicate::Lt:
            for (auto &coefficient : coefficients) coefficient = -coefficient;
            relation.add_inequality(std::move(coefficients), -constant - 1);
            return;
        case PolyPredicate::Ne:
            return;
        }
    }

    void add_equality(const PolyAffineExpr &lhs, bool lhs_source,
                      const PolyAffineExpr &rhs, bool rhs_source,
                      const PolyDependence::Relation &space,
                      std::vector<yir::presburger::IntegerRelation> &pieces,
                      bool &exact) const {
        for (auto &piece : pieces) {
            add_equality(lhs, lhs_source, rhs, rhs_source, space, piece, exact);
        }
    }

    void add_equality(const PolyAffineExpr &lhs, bool lhs_source,
                      const PolyAffineExpr &rhs, bool rhs_source,
                      const PolyDependence::Relation &space,
                      yir::presburger::IntegerRelation &piece, bool &exact) const {
        auto lhs_form = map_expr(lhs, lhs_source, space, piece);
        auto rhs_form = map_expr(rhs, rhs_source, space, piece);
        auto diff = subtract(std::move(lhs_form), std::move(rhs_form));
        exact = exact && diff.valid;
        if (!diff.valid) return;
        piece.add_equality(std::move(diff.coefficients), diff.constant);
    }

    void add_strict_order(const PolyAffineExpr &source_time,
                          const PolyAffineExpr &target_time,
                          const PolyDependence::Relation &space,
                          yir::presburger::IntegerRelation &piece, bool &exact) const {
        auto target_form = map_expr(target_time, false, space, piece);
        auto source_form = map_expr(source_time, true, space, piece);
        auto diff = subtract(std::move(target_form), std::move(source_form));
        exact = exact && diff.valid;
        if (!diff.valid) return;
        piece.add_inequality(std::move(diff.coefficients), diff.constant - 1);
    }

    std::vector<const yir::Value *> params_;
};

class GCDDependenceTester {
public:
    explicit GCDDependenceTester(const PolyModelInfo& model_info) : model_info_(model_info) {}

    PolyDependenceInfo analyze() {
        PolyDependenceInfo info;
        for (const auto& scop : model_info_.models) {
            analyze_scop(scop, info.dependences);
        }
        return info;
    }

private:
    static const yir::Value *canonical_memory_object(const yir::Value *value) {
        while (value != nullptr) {
            const auto *def = value->defining_op();
            if (const auto *elem_addr = dynamic_cast<const yir::ElemAddrOp *>(def)) {
                value = elem_addr->base();
                continue;
            }
            if (const auto *decay = dynamic_cast<const yir::DecayOp *>(def)) {
                value = decay->array_address();
                continue;
            }
            break;
        }
        return value;
    }

    static bool is_local_unique_object(const yir::Value *value) {
        value = canonical_memory_object(value);
        const auto *def = value == nullptr ? nullptr : value->defining_op();
        return dynamic_cast<const yir::ArrayVarOp *>(def) != nullptr ||
               dynamic_cast<const yir::AllocaOp *>(def) != nullptr;
    }

    static bool is_global_object(const yir::Value *value) {
        value = canonical_memory_object(value);
        return value != nullptr && value->defining_op() == nullptr &&
               !value->name().empty() && value->name().front() == '@';
    }

    static bool different_bases_noalias(const yir::Value *lhs, const yir::Value *rhs) {
        lhs = canonical_memory_object(lhs);
        rhs = canonical_memory_object(rhs);
        if (lhs == rhs) {
            return false;
        }
        if (is_local_unique_object(lhs) || is_local_unique_object(rhs)) {
            return true;
        }
        return is_global_object(lhs) && is_global_object(rhs);
    }

    static bool may_alias_memory(const yir::Value *lhs, const yir::Value *rhs) {
        return lhs == rhs || !different_bases_noalias(lhs, rhs);
    }

    static bool same_reduction_group(const PolyStmt &source, const PolyStmt &target,
                                     const yir::Value *memory) {
        return source.reduction_kind == PolyStmt::ReductionKind::Add &&
               target.reduction_kind == PolyStmt::ReductionKind::Add &&
               source.reduction_group_id == target.reduction_group_id &&
               source.reduction_memory == memory && target.reduction_memory == memory;
    }

    void analyze_scop(const PolyScop& scop, std::vector<PolyDependence>& deps) {
        // Keep all accesses in one statement-ordered group. Distinct formal
        // array parameters may name the same (or overlapping) storage, so
        // grouping solely by Value identity would incorrectly erase their
        // dependences and make loop interchange/unroll-and-jam unsound.
        std::vector<MemoryStmtAccesses> accesses;
        for (const auto& stmt : scop.statements) {
            MemoryStmtAccesses stmt_accesses;
            stmt_accesses.stmt = &stmt;
            for (const auto& access : stmt.reads) {
                stmt_accesses.reads.push_back(&access);
            }
            for (const auto& access : stmt.writes) {
                stmt_accesses.writes.push_back(&access);
            }
            if (!stmt_accesses.reads.empty() || !stmt_accesses.writes.empty()) {
                accesses.push_back(std::move(stmt_accesses));
            }
        }

        analyze_memory_group(scop, accesses, deps);
    }

    void analyze_memory_group(const PolyScop &scop,
                              const std::vector<MemoryStmtAccesses>& accesses,
                              std::vector<PolyDependence>& deps) {
        for (std::size_t i = 0; i < accesses.size(); ++i) {
            for (std::size_t j = i; j < accesses.size(); ++j) {
                const auto& source = accesses[i];
                const auto& target = accesses[j];

                check_accesses(scop, *source.stmt, *target.stmt, source.writes, target.reads,
                               PolyDependence::Kind::RAW, deps);
                check_accesses(scop, *source.stmt, *target.stmt, source.reads, target.writes,
                               PolyDependence::Kind::WAR, deps);
                check_accesses(scop, *source.stmt, *target.stmt, source.writes, target.writes,
                               PolyDependence::Kind::WAW, deps);
            }
        }

        analyze_loop_carried_raw(scop, accesses, deps);
    }

    void check_accesses(const PolyScop &scop, const PolyStmt& source, const PolyStmt& target,
                        const std::vector<const PolyAccess*>& source_accesses,
                        const std::vector<const PolyAccess*>& target_accesses,
                        PolyDependence::Kind kind,
                        std::vector<PolyDependence>& deps) {
        for (const auto* src_acc : source_accesses) {
            for (const auto* tgt_acc : target_accesses) {
                if (!may_alias_memory(src_acc->memory, tgt_acc->memory)) {
                    continue;
                }
                if (src_acc->memory != tgt_acc->memory) {
                    // Different parameters may also be offset views into the
                    // same allocation. Their logical subscripts therefore do
                    // not provide a sound equality relation. Record an
                    // unknown dependence instead of applying the GCD test.
                    PolyDependence dep = make_dependence(
                        scop, source, target, *src_acc, *tgt_acc, kind);
                    dep.relation.exact = false;
                    append_dependence(deps, std::move(dep));
                    continue;
                }
                if (can_prove_empty_dependence(source, target, *src_acc, *tgt_acc)) {
                    continue;
                }
                if (test_gcd(*src_acc, *tgt_acc)) {
                    PolyDependence dep = make_dependence(
                        scop, source, target, *src_acc, *tgt_acc, kind);
                    dep.is_reduction =
                        same_reduction_group(source, target, src_acc->memory);
                    classify_existing_order_distance(dep, source, target, *src_acc, *tgt_acc, kind);
                    append_dependence(deps, std::move(dep));
                }
            }
        }
    }

    void analyze_loop_carried_raw(const PolyScop &scop,
                                  const std::vector<MemoryStmtAccesses>& accesses,
                                  std::vector<PolyDependence>& deps) {
        for (const auto& source : accesses) {
            for (const auto& target : accesses) {
                for (const auto* write : source.writes) {
                    for (const auto* read : target.reads) {
                        if (write->memory != read->memory || !test_gcd(*write, *read)) {
                            continue;
                        }

                        const bool is_reduction = same_reduction_group(
                            *source.stmt, *target.stmt, write->memory);
                        std::vector<std::int64_t> distance;
                        const bool has_constant_distance = compute_constant_distance(
                            *source.stmt, *write, *target.stmt, *read, distance);
                        const bool same_invariant_access =
                            same_domain_and_schedule(*source.stmt, *target.stmt) &&
                            same_access(*write, *read);
                        if ((!has_constant_distance && !same_invariant_access) ||
                            (has_constant_distance &&
                             !is_lexicographically_positive(distance))) {
                            continue;
                        }

                        PolyDependence dep = make_dependence(
                            scop, *source.stmt, *target.stmt, *write, *read,
                            PolyDependence::Kind::RAW);
                        dep.is_reduction = is_reduction;
                        if (has_constant_distance) {
                            dep.distance_kind =
                                PolyDependence::DistanceKind::LoopCarriedConstant;
                            dep.distance = std::move(distance);
                        }
                        append_dependence(deps, std::move(dep));
                    }
                }
            }
        }
    }

    static PolyDependence make_dependence(const PolyScop &scop,
                                          const PolyStmt& source, const PolyStmt& target,
                                          const PolyAccess& src_acc,
                                          const PolyAccess& tgt_acc,
                                          PolyDependence::Kind kind) {
        PolyDependence dep;
        dep.source_stmt_id = source.id;
        dep.target_stmt_id = target.id;
        dep.memory = src_acc.memory;
        dep.kind = kind;
        dep.is_dependent = true;
        dep.relation = DependenceRelationBuilder(scop).build(
            source, src_acc, target, tgt_acc);
        return dep;
    }

    static void append_dependence(std::vector<PolyDependence>& deps, PolyDependence dep) {
        const auto duplicate = std::any_of(deps.begin(), deps.end(), [&](const auto& existing) {
            return existing.kind == dep.kind && existing.source_stmt_id == dep.source_stmt_id &&
                   existing.target_stmt_id == dep.target_stmt_id && existing.memory == dep.memory &&
                   existing.distance_kind == dep.distance_kind && existing.distance == dep.distance;
        });
        if (!duplicate) {
            deps.push_back(std::move(dep));
        }
    }

    static void classify_existing_order_distance(PolyDependence& dep, const PolyStmt& source,
                                                 const PolyStmt& target,
                                                 const PolyAccess& src_acc,
                                                 const PolyAccess& tgt_acc,
                                                 PolyDependence::Kind kind) {
        if (source.id < target.id && same_domain_and_schedule(source, target) &&
            same_access(src_acc, tgt_acc)) {
            dep.distance_kind = PolyDependence::DistanceKind::SameIteration;
            dep.distance.assign(source.schedule_dims.size(), 0);
            return;
        }

        if (kind != PolyDependence::Kind::RAW) {
            return;
        }

        std::vector<std::int64_t> distance;
        if (!compute_constant_distance(source, src_acc, target, tgt_acc, distance)) {
            return;
        }
        if (source.id < target.id &&
            std::all_of(distance.begin(), distance.end(), [](std::int64_t value) {
                return value == 0;
            })) {
            dep.distance_kind = PolyDependence::DistanceKind::SameIteration;
            dep.distance = std::move(distance);
        } else if (is_lexicographically_positive(distance)) {
            dep.distance_kind = PolyDependence::DistanceKind::LoopCarriedConstant;
            dep.distance = std::move(distance);
        }
    }

    static bool compute_constant_distance(const PolyStmt& source, const PolyAccess& src_acc,
                                          const PolyStmt& target, const PolyAccess& tgt_acc,
                                          std::vector<std::int64_t>& distance) {
        if (src_acc.memory != tgt_acc.memory || !same_domain_and_schedule(source, target)) {
            return false;
        }
        if (src_acc.indices.size() != tgt_acc.indices.size() ||
            src_acc.indices.size() != source.schedule_dims.size()) {
            return false;
        }

        distance.clear();
        distance.reserve(source.schedule_dims.size());
        for (std::size_t i = 0; i < source.schedule_dims.size(); ++i) {
            std::int64_t src_constant = 0;
            std::int64_t tgt_constant = 0;
            if (!is_dim_plus_constant(src_acc.indices[i], source.schedule_dims[i], src_constant) ||
                !is_dim_plus_constant(tgt_acc.indices[i], target.schedule_dims[i], tgt_constant)) {
                return false;
            }
            distance.push_back(src_constant - tgt_constant);
        }
        return true;
    }

    static bool is_dim_plus_constant(const PolyAffineExpr& expr, const yir::Value* dim,
                                     std::int64_t& constant) {
        if (!expr.valid) {
            return false;
        }

        bool saw_dim = false;
        for (const auto& term : expr.terms) {
            if (term.first == dim) {
                if (term.second != 1 || saw_dim) {
                    return false;
                }
                saw_dim = true;
                continue;
            }
            if (term.second != 0) {
                return false;
            }
        }

        if (!saw_dim) {
            return false;
        }
        constant = expr.constant;
        return true;
    }

    static bool access_is_injective_on_schedule(const PolyStmt &stmt,
                                                const PolyAccess &access) {
        if (stmt.schedule_dims.empty() || !stmt.domain.valid || !stmt.schedule.valid) {
            return false;
        }
        for (const auto *dim : stmt.schedule_dims) {
            const bool represented = std::any_of(
                access.indices.begin(), access.indices.end(), [dim](const auto &index) {
                    std::int64_t ignored = 0;
                    return is_dim_plus_constant(index, dim, ignored);
                });
            if (!represented) return false;
        }
        return true;
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

    static bool domains_have_opposite_constraint(const PolyDomain &source,
                                                 const PolyDomain &target) {
        for (const auto &lhs : source.constraints) {
            for (const auto &rhs : target.constraints) {
                if (lhs.predicate == invert_predicate(rhs.predicate) &&
                    affine_equal(lhs.lhs, rhs.lhs) && affine_equal(lhs.rhs, rhs.rhs)) {
                    return true;
                }
            }
        }
        return false;
    }

    static bool can_prove_empty_dependence(const PolyStmt &source,
                                           const PolyStmt &target,
                                           const PolyAccess &source_access,
                                           const PolyAccess &target_access) {
        if (!same_domain_and_schedule(source, target) ||
            !same_access(source_access, target_access) ||
            !access_is_injective_on_schedule(source, source_access) ||
            !access_is_injective_on_schedule(target, target_access)) {
            return false;
        }
        return source.id == target.id ||
               domains_have_opposite_constraint(source.domain, target.domain);
    }

    static bool is_lexicographically_positive(const std::vector<std::int64_t>& distance) {
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

    static bool same_domain_and_schedule(const PolyStmt& lhs, const PolyStmt& rhs) {
        if (lhs.schedule_dims.size() != rhs.schedule_dims.size() ||
            lhs.loop_bounds.size() != rhs.loop_bounds.size()) {
            return false;
        }
        for (std::size_t i = 0; i < lhs.schedule_dims.size(); ++i) {
            if (lhs.schedule_dims[i] != rhs.schedule_dims[i]) {
                return false;
            }
        }
        for (std::size_t i = 0; i < lhs.loop_bounds.size(); ++i) {
            if (lhs.loop_bounds[i].iv != rhs.loop_bounds[i].iv ||
                !affine_equal(lhs.loop_bounds[i].lower, rhs.loop_bounds[i].lower) ||
                !affine_equal(lhs.loop_bounds[i].upper, rhs.loop_bounds[i].upper)) {
                return false;
            }
        }
        return true;
    }

    static bool same_access(const PolyAccess& lhs, const PolyAccess& rhs) {
        if (lhs.memory != rhs.memory || lhs.indices.size() != rhs.indices.size()) {
            return false;
        }
        for (std::size_t i = 0; i < lhs.indices.size(); ++i) {
            if (!affine_equal(lhs.indices[i], rhs.indices[i])) {
                return false;
            }
        }
        return true;
    }

    static bool affine_equal(const PolyAffineExpr& lhs, const PolyAffineExpr& rhs) {
        if (lhs.kind != rhs.kind || lhs.divisor != rhs.divisor ||
            lhs.valid != rhs.valid || lhs.constant != rhs.constant ||
            lhs.terms.size() != rhs.terms.size()) {
            return false;
        }
        if (!lhs.is_linear() || !rhs.is_linear()) {
            if (lhs.is_quasi_affine() && rhs.is_quasi_affine() && lhs.operand != nullptr &&
                rhs.operand != nullptr && affine_equal(*lhs.operand, *rhs.operand)) {
                return true;
            }
            if (lhs.operands.size() != rhs.operands.size()) {
                return false;
            }
            return std::equal(lhs.operands.begin(), lhs.operands.end(), rhs.operands.begin(),
                              [](const auto &left, const auto &right) {
                                  return left != nullptr && right != nullptr &&
                                         affine_equal(*left, *right);
                              });
        }
        for (std::size_t i = 0; i < lhs.terms.size(); ++i) {
            if (lhs.terms[i].first != rhs.terms[i].first ||
                lhs.terms[i].second != rhs.terms[i].second) {
                return false;
            }
        }
        return true;
    }

    bool test_gcd(const PolyAccess& src, const PolyAccess& tgt) {
        if (src.indices.size() != tgt.indices.size()) {
            return true;
        }

        for (std::size_t i = 0; i < src.indices.size(); ++i) {
            const auto& src_expr = src.indices[i];
            const auto& tgt_expr = tgt.indices[i];

            if (!src_expr.valid || !tgt_expr.valid) {
                return true;
            }

            std::int64_t diff = abs_i64(src_expr.constant - tgt_expr.constant);
            std::int64_t gcd = 0;

            for (const auto& term : src_expr.terms) {
                gcd = std::gcd(gcd, abs_i64(term.second));
            }
            for (const auto& term : tgt_expr.terms) {
                gcd = std::gcd(gcd, abs_i64(term.second));
            }

            if (gcd != 0 && diff % gcd != 0) {
                return false;
            }
        }
        return true;
    }

    static std::int64_t abs_i64(std::int64_t value) {
        return value < 0 ? -value : value;
    }

    const PolyModelInfo& model_info_;
};

bool dependence_belongs_to_scop(const PolyScop &scop, const PolyDependence &dep) {
    bool source_found = false;
    bool target_found = false;
    for (const auto &stmt : scop.statements) {
        source_found = source_found || stmt.id == dep.source_stmt_id;
        target_found = target_found || stmt.id == dep.target_stmt_id;
    }
    return source_found && target_found;
}

void annotate_schedule_bands(PolyModelInfo &model_info,
                             const PolyDependenceInfo &dep_info) {
    for (auto &scop : model_info.models) {
        std::size_t max_depth = 0;
        for (const auto &stmt : scop.statements) {
            max_depth = std::max(max_depth, stmt.schedule_dims.size());
        }

        for (std::size_t depth = 0; depth < max_depth; ++depth) {
            bool coincident = true;
            bool permutable = true;
            const bool reduction = std::any_of(
                scop.statements.begin(), scop.statements.end(), [depth](const PolyStmt &stmt) {
                    return stmt.reduction_kind != PolyStmt::ReductionKind::None &&
                           stmt.reduction_dims.size() > depth && stmt.reduction_dims[depth];
                });
            if (reduction) {
                coincident = false;
            }
            for (const auto &dep : dep_info.dependences) {
                if (!dep.is_dependent || !dependence_belongs_to_scop(scop, dep)) {
                    continue;
                }
                if (dep.is_reduction) {
                    continue;
                }
                if (dep.distance_kind == PolyDependence::DistanceKind::SameIteration) {
                    continue;
                }
                if (dep.distance_kind != PolyDependence::DistanceKind::LoopCarriedConstant ||
                    dep.distance.size() <= depth) {
                    coincident = false;
                    permutable = false;
                    continue;
                }
                if (dep.distance[depth] != 0) {
                    coincident = false;
                    permutable = false;
                }
            }

            for (auto &stmt : scop.statements) {
                if (!stmt.schedule.valid || stmt.schedule.bands.size() <= depth) {
                    continue;
                }
                auto &band = stmt.schedule.bands[depth];
                band.permutable = permutable;
                band.coincident.assign(band.dimension_count, coincident);
                band.reduction.assign(band.dimension_count, reduction);
                stmt.schedule.optimized = true;
            }
        }
    }
}

} // namespace

std::string_view YIRPolyhedralDependenceAnalysisPass::name() const {
    return "YIRPolyhedralDependenceAnalysisPass";
}

PassKind YIRPolyhedralDependenceAnalysisPass::kind() const {
    return PassKind::Analysis;
}

PassResult YIRPolyhedralDependenceAnalysisPass::run(PassContext &context) {
    auto *model_info = context.get_artifact<PolyModelInfo>(
        std::string(YIRPolyhedralModelBuildPass::kArtifactKey));

    if (!model_info) {
        return PassResult::fail("YIRPolyhedralDependenceAnalysisPass requires PolyModelInfo.");
    }

    GCDDependenceTester tester(*model_info);
    PolyDependenceInfo dep_info = tester.analyze();
    annotate_schedule_bands(*model_info, dep_info);

    std::size_t num_deps = dep_info.dependences.size();
    context.set_artifact<PolyDependenceInfo>(std::string(kArtifactKey), std::move(dep_info));

    std::ostringstream oss;
    oss << "Generated " << num_deps << " Data Dependences.";
    return PassResult::ok(false, oss.str());
}

} // namespace pass
