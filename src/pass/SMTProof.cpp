#include "pass/SMTProof.h"

#include <utility>

namespace pass::smt {
namespace {

constexpr const char *kStaticSolverId = "yoolang-static-bv32-smt";
constexpr const char *kCachedSolverId = "yoolang-static-bv32-smt-cache";

std::string cache_key(const SMTObligation &obligation) {
    std::string key = std::string(cost_model::to_string(obligation.stage));
    key += "|";
    key += obligation.formula;
    key += "|";
    key += std::to_string(obligation.timeout_us);
    key += "|";
    key += std::to_string(obligation.estimated_cost_us);
    for (const auto &assumption : obligation.assumptions) {
        key += "|a:";
        key += assumption;
    }
    for (const auto &guarantee : obligation.guarantees) {
        key += "|g:";
        key += guarantee;
    }
    return key;
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
    proof.status = structural_result;
    proof.summary = std::move(summary);
    proof.rule_id = obligation.id;
    proof.solver_id = kStaticSolverId;
    proof.time_us = obligation.estimated_cost_us;
    proof.obligations = 1;

    if (structural_result != cost_model::ProofStatus::Unknown &&
        (obligation.timeout_us <= 0 || obligation.estimated_cost_us > obligation.timeout_us)) {
        proof.status = cost_model::ProofStatus::Timeout;
        proof.summary = "SMT proof budget exceeded";
        proof.time_us = obligation.timeout_us;
    }

    if (cache != nullptr) {
        cache->store(obligation, proof);
    }
    return proof;
}

} // namespace pass::smt
