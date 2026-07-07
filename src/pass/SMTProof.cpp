#include "pass/SMTProof.h"

#include <cstdint>
#include <sstream>
#include <string_view>
#include <utility>

namespace pass::smt {
namespace {

constexpr const char *kSolverId = "yoolang-qfbv-smt";
constexpr const char *kCachedSolverId = "yoolang-qfbv-smt-cache";

void append_field(std::string &out, std::string_view label, std::string_view value) {
    out += label;
    out += '#';
    out += std::to_string(value.size());
    out += ':';
    out.append(value);
}

void append_number(std::string &out, std::string_view label, std::uint64_t value) {
    out += label;
    out += '=';
    out += std::to_string(value);
    out += ';';
}

void append_sort_key(std::string &out, const ::smt::Sort &sort) {
    append_number(out, "sort.kind", static_cast<std::uint64_t>(sort.kind()));
    append_number(out, "sort.width", sort.width());
}

void append_expr_key(std::string &out, const ::smt::Expr &expr) {
    if (!expr.valid()) {
        append_field(out, "expr", "invalid");
        return;
    }

    append_field(out, "expr", "node");
    append_number(out, "kind", static_cast<std::uint64_t>(expr.kind()));
    append_sort_key(out, expr.sort());
    append_number(out, "argc", expr.args().size());

    switch (expr.kind()) {
    case ::smt::ExprKind::BoolConst:
        append_number(out, "bool", expr.bool_value() ? 1 : 0);
        break;
    case ::smt::ExprKind::BVConst:
        append_number(out, "bv", expr.bv_value());
        break;
    case ::smt::ExprKind::Var:
        append_field(out, "name", expr.name());
        break;
    case ::smt::ExprKind::BVShl:
    case ::smt::ExprKind::BVLshr:
    case ::smt::ExprKind::BVAshr:
        append_number(out, "shift", expr.shift_amount());
        break;
    case ::smt::ExprKind::BVExtract:
        append_number(out, "extract.high", expr.extract_high());
        append_number(out, "extract.low", expr.extract_low());
        break;
    case ::smt::ExprKind::Not:
    case ::smt::ExprKind::And:
    case ::smt::ExprKind::Or:
    case ::smt::ExprKind::Xor:
    case ::smt::ExprKind::Implies:
    case ::smt::ExprKind::Equal:
    case ::smt::ExprKind::Distinct:
    case ::smt::ExprKind::BVNot:
    case ::smt::ExprKind::BVAnd:
    case ::smt::ExprKind::BVOr:
    case ::smt::ExprKind::BVXor:
    case ::smt::ExprKind::BVAdd:
    case ::smt::ExprKind::BVSub:
    case ::smt::ExprKind::BVNeg:
    case ::smt::ExprKind::BVUlt:
    case ::smt::ExprKind::BVUle:
    case ::smt::ExprKind::BVSlt:
    case ::smt::ExprKind::BVSle:
    case ::smt::ExprKind::BVConcat:
        break;
    }

    for (const auto &arg : expr.args()) {
        append_expr_key(out, arg);
    }
    append_field(out, "end", "expr");
}

std::string cache_key(const SMTObligation &obligation) {
    std::string key = std::string(cost_model::to_string(obligation.stage));
    key += "|";
    key += obligation.formula;
    key += "|";
    key += std::to_string(obligation.timeout_us);
    key += "|";
    key += std::to_string(obligation.estimated_cost_us);
    key += "|opt.timeout_us:";
    key += std::to_string(obligation.options.timeout_us);
    key += "|opt.max_decisions:";
    key += std::to_string(obligation.options.max_decisions);
    key += "|opt.max_conflicts:";
    key += std::to_string(obligation.options.max_conflicts);
    key += "|opt.max_propagations:";
    key += std::to_string(obligation.options.max_propagations);
    key += "|opt.max_sat_variables:";
    key += std::to_string(obligation.options.max_sat_variables);
    key += "|opt.max_clauses:";
    key += std::to_string(obligation.options.max_clauses);
    for (const auto &assumption : obligation.assumptions) {
        key += "|a:";
        key += assumption;
    }
    for (const auto &guarantee : obligation.guarantees) {
        key += "|g:";
        key += guarantee;
    }
    for (const auto &assertion : obligation.assertions) {
        key += "|assert:";
        append_expr_key(key, assertion);
    }
    return key;
}

std::string describe_model(const ::smt::Model &model) {
    std::ostringstream out;
    bool first = true;
    for (const auto &[name, value] : model.bool_values()) {
        if (!first) {
            out << ", ";
        }
        first = false;
        out << name << "=" << (value ? "true" : "false");
    }
    for (const auto &[name, value] : model.bit_vector_values()) {
        if (!first) {
            out << ", ";
        }
        first = false;
        out << name << "=bv" << value.value << ":" << value.width;
    }
    return first ? "model unavailable" : out.str();
}

} // namespace

bool SMTProofCache::lookup(const SMTObligation &obligation,
                           cost_model::EquivalenceProof &proof) const {
    auto found = cache_.find(cache_key(obligation));
    if (found == cache_.end()) {
        return false;
    }
    proof = found->second;
    proof.solver_id = kCachedSolverId;
    if (!proof.summary.empty()) {
        proof.summary = "cached; " + proof.summary;
    }
    return true;
}

void SMTProofCache::store(const SMTObligation &obligation,
                          const cost_model::EquivalenceProof &proof) {
    cache_[cache_key(obligation)] = proof;
}

cost_model::EquivalenceProof prove_obligation(
    const SMTObligation &obligation, cost_model::ProofStatus structural_result,
    std::string summary, SMTProofCache *cache) {
    cost_model::EquivalenceProof proof;
    if (cache != nullptr && cache->lookup(obligation, proof)) {
        return proof;
    }

    proof.kind = cost_model::ProofKind::SMT;
    proof.summary = std::move(summary);
    proof.rule_id = obligation.id;
    proof.solver_id = kSolverId;
    proof.time_us = 0;
    proof.obligations =
        obligation.assertions.empty() ? 1 : static_cast<std::int64_t>(obligation.assertions.size());

    if (obligation.assertions.empty()) {
        proof.status = structural_result;
        proof.summary = proof.summary.empty() ? "no typed SMT assertions supplied" : proof.summary;
        if (cache != nullptr) {
            cache->store(obligation, proof);
        }
        return proof;
    }

    if (obligation.timeout_us <= 0 || obligation.estimated_cost_us > obligation.timeout_us) {
        proof.status = cost_model::ProofStatus::Timeout;
        proof.summary = "SMT proof budget exceeded";
        proof.time_us = obligation.timeout_us;
        if (cache != nullptr) {
            cache->store(obligation, proof);
        }
        return proof;
    }

    ::smt::SolverOptions options = obligation.options;
    if (options.timeout_us <= 0) {
        options.timeout_us = obligation.timeout_us;
    }

    ::smt::Solver solver;
    const auto result = solver.check(obligation.assertions, options);
    proof.time_us = result.diagnostics.elapsed_us;
    switch (result.status) {
    case ::smt::CheckStatus::Unsat:
        proof.status = cost_model::ProofStatus::Proven;
        proof.summary = proof.summary.empty()
                            ? "SMT proved counterexample formula unsatisfiable"
                            : "SMT proved counterexample formula unsatisfiable; " + proof.summary;
        break;
    case ::smt::CheckStatus::Sat:
        proof.status = cost_model::ProofStatus::Refuted;
        proof.summary = "SMT found counterexample: " + describe_model(result.model);
        break;
    case ::smt::CheckStatus::Timeout:
        proof.status = cost_model::ProofStatus::Timeout;
        proof.summary = "SMT proof budget exceeded";
        if (proof.time_us <= 0) {
            proof.time_us = obligation.timeout_us;
        }
        break;
    case ::smt::CheckStatus::Unknown:
        proof.status = cost_model::ProofStatus::Unknown;
        proof.summary = result.reason.empty() ? "SMT solver returned unknown" : result.reason;
        break;
    }

    if (cache != nullptr) {
        cache->store(obligation, proof);
    }
    return proof;
}

} // namespace pass::smt
