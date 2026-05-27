#include "../../include/pass/YIRPolyhedralDependenceAnalysisPass.h"
#include "../../include/pass/YIRPolyhedralModelBuildPass.h"

#include <cstdint>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <utility>

namespace pass {

namespace {

struct MemoryStmtAccesses {
    const PolyStmt* stmt = nullptr;
    std::vector<const PolyAccess*> reads;
    std::vector<const PolyAccess*> writes;
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
    using MemoryAccessMap = std::unordered_map<const yir::Value*, std::vector<MemoryStmtAccesses>>;

    void analyze_scop(const PolyScop& scop, std::vector<PolyDependence>& deps) {
        MemoryAccessMap accesses_by_memory;
        for (const auto& stmt : scop.statements) {
            for (const auto& access : stmt.reads) {
                add_access(accesses_by_memory, stmt, access, true);
            }
            for (const auto& access : stmt.writes) {
                add_access(accesses_by_memory, stmt, access, false);
            }
        }

        for (const auto& entry : accesses_by_memory) {
            analyze_memory_group(entry.second, deps);
        }
    }

    void add_access(MemoryAccessMap& accesses_by_memory, const PolyStmt& stmt,
                    const PolyAccess& access, bool is_read) {
        auto& accesses = accesses_by_memory[access.memory];
        if (accesses.empty() || accesses.back().stmt != &stmt) {
            MemoryStmtAccesses stmt_accesses;
            stmt_accesses.stmt = &stmt;
            accesses.push_back(std::move(stmt_accesses));
        }

        if (is_read) {
            accesses.back().reads.push_back(&access);
        } else {
            accesses.back().writes.push_back(&access);
        }
    }

    void analyze_memory_group(const std::vector<MemoryStmtAccesses>& accesses,
                              std::vector<PolyDependence>& deps) {
        for (std::size_t i = 0; i < accesses.size(); ++i) {
            for (std::size_t j = i; j < accesses.size(); ++j) {
                const auto& source = accesses[i];
                const auto& target = accesses[j];

                check_accesses(*source.stmt, *target.stmt, source.writes, target.reads,
                               PolyDependence::Kind::RAW, deps);
                check_accesses(*source.stmt, *target.stmt, source.reads, target.writes,
                               PolyDependence::Kind::WAR, deps);
                check_accesses(*source.stmt, *target.stmt, source.writes, target.writes,
                               PolyDependence::Kind::WAW, deps);
            }
        }
    }

    void check_accesses(const PolyStmt& source, const PolyStmt& target,
                        const std::vector<const PolyAccess*>& source_accesses,
                        const std::vector<const PolyAccess*>& target_accesses,
                        PolyDependence::Kind kind,
                        std::vector<PolyDependence>& deps) {
        for (const auto* src_acc : source_accesses) {
            for (const auto* tgt_acc : target_accesses) {
                if (src_acc->memory != tgt_acc->memory) {
                    continue;
                }
                if (test_gcd(*src_acc, *tgt_acc)) {
                    PolyDependence dep;
                    dep.source_stmt_id = source.id;
                    dep.target_stmt_id = target.id;
                    dep.memory = src_acc->memory;
                    dep.kind = kind;
                    dep.is_dependent = true;
                    deps.push_back(dep);
                }
            }
        }
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
