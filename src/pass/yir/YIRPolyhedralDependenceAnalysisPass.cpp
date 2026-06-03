#include "pass/yir/YIRPolyhedralDependenceAnalysisPass.h"
#include "pass/yir/YIRPolyhedralModelBuildPass.h"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

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

        analyze_loop_carried_raw(accesses, deps);
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
                    PolyDependence dep = make_dependence(source, target, *src_acc, *tgt_acc, kind);
                    classify_existing_order_distance(dep, source, target, *src_acc, *tgt_acc, kind);
                    append_dependence(deps, std::move(dep));
                }
            }
        }
    }

    void analyze_loop_carried_raw(const std::vector<MemoryStmtAccesses>& accesses,
                                  std::vector<PolyDependence>& deps) {
        for (const auto& source : accesses) {
            for (const auto& target : accesses) {
                for (const auto* write : source.writes) {
                    for (const auto* read : target.reads) {
                        if (write->memory != read->memory || !test_gcd(*write, *read)) {
                            continue;
                        }

                        std::vector<std::int64_t> distance;
                        if (!compute_constant_distance(*source.stmt, *write, *target.stmt, *read,
                                                       distance) ||
                            !is_lexicographically_positive(distance)) {
                            continue;
                        }

                        PolyDependence dep = make_dependence(*source.stmt, *target.stmt, *write,
                                                             *read, PolyDependence::Kind::RAW);
                        dep.distance_kind = PolyDependence::DistanceKind::LoopCarriedConstant;
                        dep.distance = std::move(distance);
                        append_dependence(deps, std::move(dep));
                    }
                }
            }
        }
    }

    static PolyDependence make_dependence(const PolyStmt& source, const PolyStmt& target,
                                          const PolyAccess& src_acc,
                                          const PolyAccess&,
                                          PolyDependence::Kind kind) {
        PolyDependence dep;
        dep.source_stmt_id = source.id;
        dep.target_stmt_id = target.id;
        dep.memory = src_acc.memory;
        dep.kind = kind;
        dep.is_dependent = true;
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
        if (same_domain_and_schedule(source, target) && same_access(src_acc, tgt_acc)) {
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
        if (std::all_of(distance.begin(), distance.end(), [](std::int64_t value) {
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
            lhs.domain.size() != rhs.domain.size()) {
            return false;
        }
        for (std::size_t i = 0; i < lhs.schedule_dims.size(); ++i) {
            if (lhs.schedule_dims[i] != rhs.schedule_dims[i]) {
                return false;
            }
        }
        for (std::size_t i = 0; i < lhs.domain.size(); ++i) {
            if (lhs.domain[i].iv != rhs.domain[i].iv ||
                !affine_equal(lhs.domain[i].lower, rhs.domain[i].lower) ||
                !affine_equal(lhs.domain[i].upper, rhs.domain[i].upper)) {
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
        if (lhs.valid != rhs.valid || lhs.constant != rhs.constant ||
            lhs.terms.size() != rhs.terms.size()) {
            return false;
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
