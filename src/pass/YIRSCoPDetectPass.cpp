#include "../../include/pass/YIRSCoPDetectPass.h"
#include "../../include/pass/YIRPolyhedralCanonicalizePass.h"
#include "../../include/pass/ASTToYIRPass.h"
#include "../../include/yir/YIR.h"

#include <sstream>
#include <memory>

namespace pass {

namespace {

class SCoPDetector {
public:
    explicit SCoPDetector(const YIRPolyhedralCanonicalInfo& canonical_info)
        : canonical_info_(canonical_info), next_scop_id_(0), next_stmt_id_(0) {}

    bool is_descendant(const yir::Region* region, const yir::Operation* target) const {
        for (const auto& op : region->operations()) {
            if (op.get() == target) return true;
            if (auto* if_op = dynamic_cast<const yir::IfOp*>(op.get())) {
                if (is_descendant(&if_op->then_region(), target)) return true;
                if (if_op->has_else() && is_descendant(&if_op->else_region(), target)) return true;
            } else if (auto* while_op = dynamic_cast<const yir::WhileOp*>(op.get())) {
                if (is_descendant(&while_op->cond_region(), target)) return true;
                if (is_descendant(&while_op->body_region(), target)) return true;
            } else if (auto* for_op = dynamic_cast<const yir::ForOp*>(op.get())) {
                if (is_descendant(&for_op->body_region(), target)) return true;
            }
        }
        return false;
    }

    YIRSCoPInfo detect(const yir::Module& module) {
        YIRSCoPInfo info;
        for (const auto& func : module.functions()) {
            bool has_poly_loop = false;
            for (const auto& loop_seq : canonical_info_.loops) {
                if (is_descendant(&func->body(), loop_seq.loop)) {
                    has_poly_loop = true;
                    break;
                }
            }

            if (has_poly_loop) {
                YIRSCoP scop;
                scop.id = next_scop_id_++;
                scop.region = &func->body();

                // Outline statements
                for (const auto& op : func->body().operations()) {
                    YIRSCoPStatement stmt;
                    stmt.id = next_stmt_id_++;
                    stmt.op = op.get();
                    scop.statements.push_back(stmt);
                }
                info.scops.push_back(std::move(scop));
            }
        }
        return info;
    }

private:
    const YIRPolyhedralCanonicalInfo& canonical_info_;
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
