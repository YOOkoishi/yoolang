#pragma once

#include "smt/Expr.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace smt {

enum class CheckStatus {
    Sat,
    Unsat,
    Unknown,
    Timeout,
};

struct SolverOptions {
    std::int64_t timeout_us = 0;
    std::uint64_t max_decisions = 200000;
    std::uint64_t max_conflicts = 200000;
    std::uint64_t max_propagations = 2000000;
    std::uint64_t max_sat_variables = 200000;
    std::uint64_t max_clauses = 600000;
};

struct SolverDiagnostics {
    std::int64_t elapsed_us = 0;
    std::uint64_t sat_variables = 0;
    std::uint64_t clauses = 0;
    std::uint64_t decisions = 0;
    std::uint64_t conflicts = 0;
    std::uint64_t propagations = 0;
    std::uint64_t bitblast_nodes = 0;
};

struct BitVectorValue {
    std::uint32_t width = 0;
    std::uint64_t value = 0;
};

class Model {
  public:
    void set_bool(std::string name, bool value);
    void set_bit_vector(std::string name, std::uint32_t width, std::uint64_t value);

    bool bool_value(std::string_view name, bool &out) const;
    bool bit_vector_value(std::string_view name, BitVectorValue &out) const;

    const std::unordered_map<std::string, bool> &bool_values() const { return bool_values_; }
    const std::unordered_map<std::string, BitVectorValue> &bit_vector_values() const {
        return bit_vector_values_;
    }

  private:
    std::unordered_map<std::string, bool> bool_values_;
    std::unordered_map<std::string, BitVectorValue> bit_vector_values_;
};

struct SolverResult {
    CheckStatus status = CheckStatus::Unknown;
    Model model;
    SolverDiagnostics diagnostics;
    std::string reason;

    bool is_sat() const { return status == CheckStatus::Sat; }
    bool is_unsat() const { return status == CheckStatus::Unsat; }
};

class Solver {
  public:
    SolverResult check(const Expr &assertion, const SolverOptions &options = SolverOptions{});
    SolverResult check(const std::vector<Expr> &assertions,
                       const SolverOptions &options = SolverOptions{});
};

std::string to_string(CheckStatus status);

} // namespace smt
