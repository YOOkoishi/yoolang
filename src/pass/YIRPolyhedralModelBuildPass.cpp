#include "../../include/pass/YIRPolyhedralModelBuildPass.h"
#include "../../include/pass/YIRSCoPDetectPass.h"
#include "../../include/pass/YIRPolyhedralCanonicalizePass.h"
#include "../../include/yir/YIR.h"

#include <sstream>
#include <algorithm>

namespace pass {

namespace {

class AffineExtractor {
public:
    PolyAffineExpr extract(const yir::Value* val) {
        PolyAffineExpr expr;
        if (!val) {
            expr.valid = false;
            return expr;
        }

        if (auto* const_op = dynamic_cast<const yir::ConstI32Op*>(val->defining_op())) {
            expr.constant = const_op->value();
            return expr;
        }

        if (auto* add = dynamic_cast<const yir::AddIOp*>(val->defining_op())) {
            auto lhs = extract(add->lhs());
            auto rhs = extract(add->rhs());
            if (lhs.valid && rhs.valid) {
                expr.constant = lhs.constant + rhs.constant;
                expr.terms = lhs.terms;
                for (const auto& t : rhs.terms) expr.terms.push_back(t);
                return expr;
            }
        }
        
        expr.terms.push_back({val, 1});
        return expr;
    }
};

class PolyhedralBuilder {
public:
    explicit PolyhedralBuilder(const YIRSCoPInfo& scop_info, const YIRPolyhedralCanonicalInfo& canonical_info)
        : scop_info_(scop_info), canonical_info_(canonical_info) {}

    PolyModelInfo build() {
        PolyModelInfo model_info;
        for (const auto& scop : scop_info_.scops) {
            PolyScop poly_scop;
            poly_scop.id = scop.id;
            
            for (auto* sym : scop.symbols) {
                poly_scop.params.push_back(sym);
            }

            // O(N) single-pass Traversal for the whole SCoP Region mapping each Operation -> enclosing loops
            std::unordered_map<const yir::Operation*, std::vector<const yir::ForOp*>> enclosing_loops;
            std::vector<const yir::ForOp*> loop_stack;
            build_loop_context(scop.region, loop_stack, enclosing_loops);

            for (const auto& stmt : scop.statements) {
                PolyStmt poly_stmt;
                poly_stmt.id = stmt.id;
                poly_stmt.op = stmt.op;
                poly_stmt.lexical_id = stmt.id;
                
                extract_domain(poly_stmt, enclosing_loops[stmt.op]);
                extract_accesses(*stmt.op, poly_stmt.reads, poly_stmt.writes);
                
                poly_scop.statements.push_back(std::move(poly_stmt));
            }
            model_info.models.push_back(std::move(poly_scop));
        }
        return model_info;
    }

private:
    void build_loop_context(const yir::Region* region, std::vector<const yir::ForOp*>& stack,
                            std::unordered_map<const yir::Operation*, std::vector<const yir::ForOp*>>& map) {
        for (const auto& op : region->operations()) {
            map[op.get()] = stack;
            if (auto* for_op = dynamic_cast<const yir::ForOp*>(op.get())) {
                stack.push_back(for_op);
                build_loop_context(&for_op->body_region(), stack, map);
                stack.pop_back();
            } else if (auto* while_op = dynamic_cast<const yir::WhileOp*>(op.get())) {
                build_loop_context(&while_op->cond_region(), stack, map);
                build_loop_context(&while_op->body_region(), stack, map);
            } else if (auto* if_op = dynamic_cast<const yir::IfOp*>(op.get())) {
                build_loop_context(&if_op->then_region(), stack, map);
                if (if_op->has_else()) build_loop_context(&if_op->else_region(), stack, map);
            }
        }
    }

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

    void extract_accesses(const yir::Operation& op, std::vector<PolyAccess>& reads, std::vector<PolyAccess>& writes) {
        AffineExtractor extractor;
        if (auto* load = dynamic_cast<const yir::ArrayLoadOp*>(&op)) {
            PolyAccess access;
            access.kind = PolyAccess::Kind::Read;
            access.memory = load->array();
            for (auto* index_val : load->indices()) {
                access.indices.push_back(extractor.extract(index_val));
            }
            reads.push_back(std::move(access));
        } else if (auto* store = dynamic_cast<const yir::ArrayStoreOp*>(&op)) {
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
    const YIRPolyhedralCanonicalInfo& canonical_info_;
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
