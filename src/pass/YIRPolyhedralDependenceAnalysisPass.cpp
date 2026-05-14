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
        // Collect all accesses
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
                if (src_acc.memory == tgt_acc.memory) {
                    bool dependent = test_dependence(src_acc, tgt_acc);
                    if (dependent) {
                        PolyDependence dep;
                        dep.kind = kind;
                        dep.source_stmt_id = source.id;
                        dep.target_stmt_id = target.id;
                        dep.memory = src_acc.memory;
                        dep.is_dependent = true;
                        deps.push_back(dep);
                    }
                }
            }
        }
    }

    bool test_dependence(const PolyAccess& src, const PolyAccess& tgt) {
        // If dimensionality differs, assume dependent/unsafe
        if (src.indices.size() != tgt.indices.size()) return true;
        
        // Simple GCD Test:
        // For f(i) = a*i + c1, g(j) = b*j + c2
        // If a and b are both constants, gcd(a, b) must divide (c2 - c1)
        for (std::size_t i = 0; i < src.indices.size(); ++i) {
            const auto& src_idx = src.indices[i];
            const auto& tgt_idx = tgt.indices[i];
            
            if (!src_idx.valid || !tgt_idx.valid) return true;
            
            // Collect all coefficients
            std::int64_t gcd_val = 0;
            for (const auto& term : src_idx.terms) {
                gcd_val = std::gcd(gcd_val, term.second);
            }
            for (const auto& term : tgt_idx.terms) {
                gcd_val = std::gcd(gcd_val, term.second);
            }
            
            if (gcd_val > 0) {
                std::int64_t c_diff = tgt_idx.constant - src_idx.constant;
                if (c_diff % gcd_val != 0) {
                    // Independence proven!
                    return false;
                }
            }
        }
        
        return true;
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
    auto *model_info = context.get_artifact<PolyModelInfo>(std::string(YIRPolyhedralModelBuildPass::kArtifactKey));
    if (!model_info) {
        return PassResult::fail("YIRPolyhedralDependenceAnalysisPass requires PolyModelInfo.");
    }

    GCDDependenceTester tester(*model_info);
    PolyDependenceInfo info = tester.analyze();
    
    std::size_t num_deps = info.dependences.size();
    context.set_artifact<PolyDependenceInfo>(std::string(kArtifactKey), std::move(info));

    std::ostringstream oss;
    oss << "Analyzed " << num_deps << " data dependences.";
    return PassResult::ok(false, oss.str());
}

} // namespace pass
