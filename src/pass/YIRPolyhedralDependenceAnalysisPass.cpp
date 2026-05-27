#include "../../include/pass/YIRPolyhedralDependenceAnalysisPass.h"
#include "../../include/pass/YIRPolyhedralModelBuildPass.h"
#include <sstream>
#include <numeric>

namespace pass {

namespace {

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
    void analyze_scop(const PolyScop& scop, std::vector<PolyDependence>& deps) {
        for (std::size_t i = 0; i < scop.statements.size(); ++i) {
            for (std::size_t j = i; j < scop.statements.size(); ++j) {
                const auto& source = scop.statements[i];
                const auto& target = scop.statements[j];
                
                check_accesses(source, target, source.writes, target.reads, PolyDependence::Kind::RAW, deps);
                check_accesses(source, target, source.reads, target.writes, PolyDependence::Kind::WAR, deps);
                check_accesses(source, target, source.writes, target.writes, PolyDependence::Kind::WAW, deps);
            }
        }
    }

    void check_accesses(const PolyStmt& source, const PolyStmt& target,
                        const std::vector<PolyAccess>& source_accesses,
                        const std::vector<PolyAccess>& target_accesses,
                        PolyDependence::Kind kind,
                        std::vector<PolyDependence>& deps) {
        for (const auto& src_acc : source_accesses) {
            for (const auto& tgt_acc : target_accesses) {
                // VERY IMPORTANT OPTIMIZATION:
                // Only test dependence if they access the same array memory!
                if (src_acc.memory != tgt_acc.memory) {
                    continue;
                }
                
                if (test_gcd(src_acc, tgt_acc)) {
                    PolyDependence dep;
                    dep.source_stmt_id = source.id;
                    dep.target_stmt_id = target.id;
                    dep.kind = kind;
                    deps.push_back(dep);
                }
            }
        }
    }

    bool test_gcd(const PolyAccess& src, const PolyAccess& tgt) {
        if (src.indices.size() != tgt.indices.size()) return false;

        for (size_t i = 0; i < src.indices.size(); ++i) {
            const auto& src_expr = src.indices[i];
            const auto& tgt_expr = tgt.indices[i];

            if (!src_expr.valid || !tgt_expr.valid) return true; // Conservative
            
            int diff = std::abs(src_expr.constant - tgt_expr.constant);
            int g = 0;
            
            for (const auto& term : src_expr.terms) {
                g = std::gcd(g, std::abs(term.second));
            }
            for (const auto& term : tgt_expr.terms) {
                g = std::gcd(g, std::abs(term.second));
            }
            
            if (g != 0 && diff % g != 0) {
                return false; // Independent
            }
        }
        return true; // Dependent
    }

    const PolyModelInfo& model_info_;
};

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
    
    std::size_t num_deps = dep_info.dependences.size();
    context.set_artifact<PolyDependenceInfo>(std::string(kArtifactKey), std::move(dep_info));

    std::ostringstream oss;
    oss << "Generated " << num_deps << " Data Dependences.";
    return PassResult::ok(false, oss.str());
}

} // namespace pass
