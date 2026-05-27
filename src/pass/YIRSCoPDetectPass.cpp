#include "../../include/pass/YIRSCoPDetectPass.h"
#include "../../include/pass/YIRPolyhedralCanonicalizePass.h"
#include "../../include/pass/ASTToYIRPass.h"
#include "../../include/yir/YIR.h"

#include <sstream>
#include <memory>
#include <unordered_set>

namespace pass {

namespace {

class SCoPDetector {
public:
    explicit SCoPDetector(const YIRPolyhedralCanonicalInfo& canonical_info) 
        : canonical_info_(canonical_info), next_scop_id_(0), next_stmt_id_(0) {
        for (const auto& loop_info : canonical_info_.loops) {
            poly_loops_.insert(loop_info.loop);
        }
    }

    YIRSCoPInfo detect(const yir::Module& module) {
        YIRSCoPInfo info;
        for (const auto& func : module.functions()) {
            // Find if there are polyhedral loops in the function.
            // Rather than a slow is_descendant on every loop against func->body(),
            // we just do one quick pass over the function body to see if it hits a recorded ForOp.
            if (has_poly_loop(&func->body())) {
                YIRSCoP scop;
                scop.id = next_scop_id_++;
                scop.region = &func->body();
                
                // Outline statements in O(N) single pass
                outline_statements(&func->body(), scop);
                const size_t MAX_SCOP_STATEMENTS = 50;
                    if (scop.statements.size() > MAX_SCOP_STATEMENTS) continue;
                    info.scops.push_back(std::move(scop));
            }
        }
        return info;
    }

private:
    bool has_poly_loop(const yir::Region* region) const {
        for (const auto& op : region->operations()) {
            if (auto* for_op = dynamic_cast<const yir::ForOp*>(op.get())) {
                if (poly_loops_.count(for_op)) return true;
                if (has_poly_loop(&for_op->body_region())) return true;
            } else if (auto* while_op = dynamic_cast<const yir::WhileOp*>(op.get())) {
                if (has_poly_loop(&while_op->cond_region())) return true;
                if (has_poly_loop(&while_op->body_region())) return true;
            } else if (auto* if_op = dynamic_cast<const yir::IfOp*>(op.get())) {
                if (has_poly_loop(&if_op->then_region())) return true;
                if (if_op->has_else() && has_poly_loop(&if_op->else_region())) return true;
            }
        }
        return false;
    }

    void outline_statements(const yir::Region* region, YIRSCoP& scop) {
        for (const auto& op : region->operations()) {
            YIRSCoPStatement stmt;
            stmt.id = next_stmt_id_++;
            stmt.op = op.get();
            scop.statements.push_back(stmt);
            
            if (auto* for_op = dynamic_cast<const yir::ForOp*>(op.get())) {
                outline_statements(&for_op->body_region(), scop);
            } else if (auto* while_op = dynamic_cast<const yir::WhileOp*>(op.get())) {
                outline_statements(&while_op->cond_region(), scop);
                outline_statements(&while_op->body_region(), scop);
            } else if (auto* if_op = dynamic_cast<const yir::IfOp*>(op.get())) {
                outline_statements(&if_op->then_region(), scop);
                if (if_op->has_else()) outline_statements(&if_op->else_region(), scop);
            }
        }
    }

    const YIRPolyhedralCanonicalInfo& canonical_info_;
    std::unordered_set<const yir::ForOp*> poly_loops_;
    std::size_t next_scop_id_;
    std::size_t next_stmt_id_;
};

} // namespace

std::string_view YIRSCoPDetectPass::name() const {
    return "YIRSCoPDetectPass";
}

PassKind YIRSCoPDetectPass::kind() const {
    return PassKind::Analysis;
}

PassResult YIRSCoPDetectPass::run(PassContext &context) {
    auto *module_ptr = context.get_artifact<std::unique_ptr<yir::Module>>(ASTToYIRPass::kArtifactKey);
    if (!module_ptr || !*module_ptr) {
        return PassResult::fail("YIRSCoPDetectPass requires YIR module.");
    }

    auto *canonical_info = context.get_artifact<YIRPolyhedralCanonicalInfo>(
        std::string(YIRPolyhedralCanonicalizePass::kArtifactKey));
    if (!canonical_info) {
        return PassResult::fail("YIRSCoPDetectPass requires YIRPolyhedralCanonicalInfo.");
    }

    SCoPDetector detector(*canonical_info);
    YIRSCoPInfo info = detector.detect(**module_ptr);
    
    std::size_t num_scops = info.scops.size();
    context.set_artifact<YIRSCoPInfo>(std::string(kArtifactKey), std::move(info));

    std::ostringstream oss;
    oss << "Detected " << num_scops << " SCoPs.";
    return PassResult::ok(false, oss.str());
}

} // namespace pass
