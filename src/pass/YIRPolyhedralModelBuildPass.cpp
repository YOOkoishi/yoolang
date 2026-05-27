#include "../../include/pass/YIRPolyhedralModelBuildPass.h"
#include "../../include/pass/YIRSCoPDetectPass.h"
#include "../../include/pass/YIRPolyhedralCanonicalizePass.h"
#include "../../include/yir/YIR.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
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

            for (auto* sym : scop.symbols) {
                poly_scop.params.push_back(sym);
            }

            for (const auto& stmt : scop.statements) {
                PolyStmt poly_stmt;
                poly_stmt.id = stmt.id;
                poly_stmt.op = stmt.op;
                poly_stmt.lexical_id = stmt.id;

                extract_domain(poly_stmt, stmt.enclosing_loops);
                extract_accesses(*stmt.op, poly_stmt.reads, poly_stmt.writes);

                if (!poly_stmt.reads.empty() || !poly_stmt.writes.empty()) {
                    poly_scop.statements.push_back(std::move(poly_stmt));
                }
            }
            if (!poly_scop.statements.empty()) {
                model_info.models.push_back(std::move(poly_scop));
            }
        }
        return model_info;
    }

private:
    void extract_domain(PolyStmt& poly_stmt, const std::vector<const yir::ForOp*>& enclosing_loops) {
        AffineExtractor extractor;
        for (const auto* for_op : enclosing_loops) {
            poly_stmt.dims.push_back(for_op->induction_var());
            poly_stmt.schedule_dims.push_back(for_op->induction_var());

            PolyLoopBound bound;
            bound.iv = for_op->induction_var();
            bound.lower = extractor.extract(for_op->lower_bound());
            bound.upper = extractor.extract(for_op->upper_bound());
            poly_stmt.domain.push_back(std::move(bound));
        }
    }

    void extract_accesses(const yir::Operation& op, std::vector<PolyAccess>& reads,
                          std::vector<PolyAccess>& writes) {
        AffineExtractor extractor;
        if (auto* load = dynamic_cast<const yir::ArrayLoadOp*>(&op)) {
            PolyAccess access;
            access.kind = PolyAccess::Kind::Read;
            access.memory = load->array();
            for (auto* index_val : load->indices()) {
                access.indices.push_back(extractor.extract(index_val));
            }
            reads.push_back(std::move(access));
            return;
        }

        if (auto* store = dynamic_cast<const yir::ArrayStoreOp*>(&op)) {
            PolyAccess access;
            access.kind = PolyAccess::Kind::Write;
            access.memory = store->array();
            for (auto* index_val : store->indices()) {
                access.indices.push_back(extractor.extract(index_val));
            }
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
