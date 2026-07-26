#include "pass/yir/YIRPolyhedralDumpPass.h"

#include "pass/yir/YIRPolyhedralCanonicalizePass.h"
#include "pass/yir/YIRPolyhedralDependenceAnalysisPass.h"
#include "pass/yir/YIRPolyhedralModelBuildPass.h"
#include "pass/yir/YIRSCoPDetectPass.h"
#include "yir/YIR.h"

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace pass {
namespace {

std::string value_name(const yir::Value *value) {
    if (value == nullptr) {
        return "<null>";
    }
    std::string name = value->name();
    if (name.empty()) {
        return "<unnamed>";
    }
    if (name[0] != '%' && name[0] != '@') {
        name.insert(name.begin(), '%');
    }
    return name;
}

std::vector<const yir::Value *> sorted_values(const std::vector<const yir::Value *> &values) {
    auto out = values;
    std::sort(out.begin(), out.end(), [](const auto *lhs, const auto *rhs) {
        auto lhs_name = value_name(lhs);
        auto rhs_name = value_name(rhs);
        if (lhs_name != rhs_name) {
            return lhs_name < rhs_name;
        }
        return lhs < rhs;
    });
    return out;
}

template <typename SetT> std::vector<const yir::Value *> sorted_values_from_set(const SetT &values) {
    std::vector<const yir::Value *> out(values.begin(), values.end());
    return sorted_values(out);
}

std::string affine_expr(const PolyAffineExpr &expr) {
    if (!expr.valid) {
        return "<non-affine>";
    }

    std::ostringstream out;
    bool wrote = false;
    for (const auto &[value, coefficient] : expr.terms) {
        if (coefficient == 0) {
            continue;
        }
        if (wrote) {
            out << (coefficient < 0 ? " - " : " + ");
        } else if (coefficient < 0) {
            out << '-';
        }

        const auto abs_coeff = coefficient < 0 ? -coefficient : coefficient;
        if (abs_coeff != 1) {
            out << abs_coeff << '*';
        }
        out << value_name(value);
        wrote = true;
    }

    if (expr.constant != 0 || !wrote) {
        if (wrote) {
            out << (expr.constant < 0 ? " - " : " + ");
            out << (expr.constant < 0 ? -expr.constant : expr.constant);
        } else {
            out << expr.constant;
        }
    }
    return out.str();
}

std::string value_list(const std::vector<const yir::Value *> &values) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << value_name(values[i]);
    }
    out << ']';
    return out.str();
}

std::string domain_string(const PolyStmt &stmt) {
    std::ostringstream out;
    for (std::size_t i = 0; i < stmt.loop_bounds.size(); ++i) {
        if (i != 0) {
            out << "; ";
        }
        const auto &bound = stmt.loop_bounds[i];
        out << value_name(bound.iv) << " in [" << affine_expr(bound.lower) << ", "
            << affine_expr(bound.upper) << ')';
    }
    return out.str();
}

std::string predicate_string(PolyPredicate predicate) {
    switch (predicate) {
    case PolyPredicate::Eq: return "==";
    case PolyPredicate::Ne: return "!=";
    case PolyPredicate::Lt: return "<";
    case PolyPredicate::Le: return "<=";
    case PolyPredicate::Gt: return ">";
    case PolyPredicate::Ge: return ">=";
    }
    return "?";
}

std::string explicit_domain_string(const PolyStmt &stmt) {
    std::ostringstream out;
    out << "dims=" << value_list(stmt.domain.dims)
        << " params=" << value_list(sorted_values(stmt.domain.params)) << " { ";
    for (std::size_t i = 0; i < stmt.domain.constraints.size(); ++i) {
        if (i != 0) out << " and ";
        const auto &constraint = stmt.domain.constraints[i];
        out << affine_expr(constraint.lhs) << ' ' << predicate_string(constraint.predicate)
            << ' ' << affine_expr(constraint.rhs);
    }
    out << " }";
    if (!stmt.domain.valid) out << " <invalid>";
    return out.str();
}

std::string schedule_map_string(const PolyStmt &stmt) {
    std::ostringstream out;
    out << value_list(stmt.schedule.input_dims) << " -> [";
    for (std::size_t i = 0; i < stmt.schedule.output_dims.size(); ++i) {
        if (i != 0) out << ", ";
        out << affine_expr(stmt.schedule.output_dims[i]);
    }
    out << ']';
    return out.str();
}

std::string access_kind(const PolyAccess &access) {
    return access.kind == PolyAccess::Kind::Read ? "read" : "write";
}

std::string access_string(const PolyAccess &access) {
    std::ostringstream out;
    out << access_kind(access) << ' ' << value_name(access.memory) << '[';
    for (std::size_t i = 0; i < access.indices.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << affine_expr(access.indices[i]);
    }
    out << ']';
    return out.str();
}

std::string dependence_kind(PolyDependence::Kind kind) {
    switch (kind) {
    case PolyDependence::Kind::RAW:
        return "RAW";
    case PolyDependence::Kind::WAR:
        return "WAR";
    case PolyDependence::Kind::WAW:
        return "WAW";
    case PolyDependence::Kind::RAR:
        return "RAR";
    }
    return "unknown";
}

std::string distance_kind(PolyDependence::DistanceKind kind) {
    switch (kind) {
    case PolyDependence::DistanceKind::SameIteration:
        return "same-iteration";
    case PolyDependence::DistanceKind::LoopCarriedConstant:
        return "loop-carried";
    case PolyDependence::DistanceKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::string distance_vector(const std::vector<std::int64_t> &distance) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < distance.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << distance[i];
    }
    out << ']';
    return out.str();
}

std::string dependence_distance(const PolyDependence &dep) {
    std::ostringstream out;
    out << " distance=" << distance_kind(dep.distance_kind);
    if (!dep.distance.empty()) {
        out << ' ' << distance_vector(dep.distance);
    }
    return out.str();
}

void dump_scops(std::ostream &out, const YIRSCoPInfo &scop_info) {
    out << "  scops: " << scop_info.scops.size() << '\n';
    for (const auto &scop : scop_info.scops) {
        out << "  scop #" << scop.id << " {\n";
        out << "    symbols: " << value_list(sorted_values_from_set(scop.symbols)) << '\n';
        out << "    statements: " << scop.statements.size() << '\n';
        for (const auto &stmt : scop.statements) {
            out << "    stmt #" << stmt.id << " op=" << stmt.op_name;
            out << " loops=";
            std::vector<const yir::Value *> loops;
            for (const auto *loop : stmt.enclosing_loops) {
                loops.push_back(loop == nullptr ? nullptr : loop->induction_var());
            }
            out << value_list(loops) << '\n';
        }
        out << "  }\n";
    }
}

void dump_models(std::ostream &out, const PolyModelInfo &model_info) {
    out << "  models: " << model_info.models.size() << '\n';
    for (const auto &model : model_info.models) {
        out << "  model #" << model.id << " {\n";
        out << "    params: " << value_list(sorted_values(model.params)) << '\n';
        out << "    statements: " << model.statements.size() << '\n';
        for (const auto &stmt : model.statements) {
            out << "    stmt #" << stmt.id << " op=" << stmt.op_name << '\n';
            out << "      schedule: " << value_list(stmt.schedule_dims) << '\n';
            out << "      domain: " << domain_string(stmt) << '\n';
            out << "      schedule-map: " << schedule_map_string(stmt) << '\n';
            out << "      domain-set: " << explicit_domain_string(stmt) << '\n';
            for (const auto &read : stmt.reads) {
                out << "      " << access_string(read) << '\n';
            }
            for (const auto &write : stmt.writes) {
                out << "      " << access_string(write) << '\n';
            }
        }
        out << "  }\n";
    }
}

void dump_dependences(std::ostream &out, const PolyDependenceInfo &dep_info) {
    auto deps = dep_info.dependences;
    std::sort(deps.begin(), deps.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.source_stmt_id != rhs.source_stmt_id) {
            return lhs.source_stmt_id < rhs.source_stmt_id;
        }
        if (lhs.target_stmt_id != rhs.target_stmt_id) {
            return lhs.target_stmt_id < rhs.target_stmt_id;
        }
        if (lhs.kind != rhs.kind) {
            return static_cast<int>(lhs.kind) < static_cast<int>(rhs.kind);
        }
        return value_name(lhs.memory) < value_name(rhs.memory);
    });

    out << "  dependences: " << deps.size() << '\n';
    for (const auto &dep : deps) {
        out << "  dependence " << dependence_kind(dep.kind) << " stmt #" << dep.source_stmt_id
            << " -> stmt #" << dep.target_stmt_id << " memory=" << value_name(dep.memory)
            << " dependent=" << (dep.is_dependent ? "true" : "false")
            << dependence_distance(dep)
            << " relation={source=" << value_list(dep.relation.source_dims)
            << ", target=" << value_list(dep.relation.target_dims)
            << ", params=" << value_list(sorted_values(dep.relation.params))
            << ", disjuncts=" << dep.relation.constraints.disjuncts().size()
            << ", exact=" << (dep.relation.exact ? "true" : "false") << "}" << '\n';
    }
}

} // namespace

YIRPolyhedralDumpPass::YIRPolyhedralDumpPass(std::ostream &out) : out_(out) {}

std::string_view YIRPolyhedralDumpPass::name() const {
    return "YIRPolyhedralDumpPass";
}

PassKind YIRPolyhedralDumpPass::kind() const {
    return PassKind::Analysis;
}

PassResult YIRPolyhedralDumpPass::run(PassContext &context) {
    auto *canonical_info = context.get_artifact<YIRPolyhedralCanonicalInfo>(
        std::string(YIRPolyhedralCanonicalizePass::kArtifactKey));
    auto *scop_info = context.get_artifact<YIRSCoPInfo>(std::string(YIRSCoPDetectPass::kArtifactKey));
    auto *model_info = context.get_artifact<PolyModelInfo>(
        std::string(YIRPolyhedralModelBuildPass::kArtifactKey));
    auto *dep_info = context.get_artifact<PolyDependenceInfo>(
        std::string(YIRPolyhedralDependenceAnalysisPass::kArtifactKey));

    if (canonical_info == nullptr || scop_info == nullptr || model_info == nullptr ||
        dep_info == nullptr) {
        return PassResult::fail(
            "YIRPolyhedralDumpPass requires -O1 --polyhedral before --emit-poly");
    }

    out_ << "polyhedral {\n";
    out_ << "  canonical_loops: " << canonical_info->loops.size() << '\n';
    dump_scops(out_, *scop_info);
    dump_models(out_, *model_info);
    dump_dependences(out_, *dep_info);
    out_ << "}\n";

    return PassResult::ok(false, "dumped polyhedral artifacts");
}

} // namespace pass
