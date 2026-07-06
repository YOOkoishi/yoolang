#pragma once

#include "pass/CostModel.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pass::smt {

struct SMTObligation {
    std::string id;
    cost_model::CostIRStage stage = cost_model::CostIRStage::OIR;
    std::string formula;
    std::int64_t timeout_us = 0;
    std::int64_t estimated_cost_us = 0;
    std::vector<std::string> assumptions;
    std::vector<std::string> guarantees;
};

class SMTProofCache {
  public:
    bool lookup(const SMTObligation &obligation, cost_model::EquivalenceProof &proof) const;
    void store(const SMTObligation &obligation, const cost_model::EquivalenceProof &proof);

  private:
    std::unordered_map<std::string, cost_model::EquivalenceProof> cache_;
};

cost_model::EquivalenceProof prove_obligation(
    const SMTObligation &obligation, cost_model::ProofStatus structural_result,
    std::string summary, SMTProofCache *cache = nullptr);

} // namespace pass::smt
