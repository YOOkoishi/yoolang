#include "../../include/pass/YIRPolyhedralModelBuildPass.h"
#include "../../include/pass/YIRSCoPDetectPass.h"
#include "../../include/pass/YIRPolyhedralCanonicalizePass.h"
#include "../../include/yir/YIR.h"

#include <sstream>
#include <algorithm>

namespace pass {

namespace {

class PolyhedralBuilder {
public:
    explicit PolyhedralBuilder(const YIRSCoPInfo& scop_info, const YIRPolyhedralCanonicalInfo& canonical_info)
        : scop_info_(scop_info), canonical_info_(canonical_info) {}

    PolyModelInfo build() {
        PolyModelInfo model_info;
        for (const auto& scop : scop_info_.scops) {
            PolyScop poly_scop;
            poly_scop.id = scop.id;

            // Collect parameters (symbols)
            for (auto* sym : scop.symbols) {
                poly_scop.params.push_back(sym);
            }

            // Extract model for each statement
            for (const auto& stmt : scop.statements) {
                PolyStmt poly_stmt;
                poly_stmt.id = stmt.id;
                poly_stmt.op = stmt.op;

                // Very basic placeholder extraction
                poly_stmt.schedule_str = extract_schedule(stmt.op);
                poly_stmt.domain_str = extract_domain(stmt.op);

                extract_accesses(*stmt.op, poly_stmt.reads, poly_stmt.writes);

                poly_scop.statements.push_back(std::move(poly_stmt));
            }
            model_info.models.push_back(std::move(poly_scop));
        }
        return model_info;
    }

private:
    std::string extract_schedule(const yir::Operation* op) {
        // Placeholder for phase 2.3: poly-schedule-extract
        std::stringstream ss;
        ss << "[i, j, " << op << "]";
        return ss.str();
    }

    std::string extract_domain(const yir::Operation* op) {
        // Placeholder for phase 2.1: poly-domain-extract
        return "{ [i, j] : 0 <= i < N and 0 <= j < M }";
    }

    void extract_accesses(const yir::Operation& op, std::vector<PolyAccess>& reads, std::vector<PolyAccess>& writes) {
        // Placeholder for phase 2.2: poly-access-extract
        if (auto* load = dynamic_cast<const yir::ArrayLoadOp*>(&op)) {
            reads.push_back({PolyAccess::Kind::Read, load->array(), "A[i, j]"});
        } else if (auto* store = dynamic_cast<const yir::ArrayStoreOp*>(&op)) {
            writes.push_back({PolyAccess::Kind::Write, store->array(), "A[i, j]"});
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
    oss << "Built " << num_models << " Polyhedral Models.";
    return PassResult::ok(false, oss.str());
}

} // namespace pass
