#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace yir::presburger {

// MLIR FlatLinearValueConstraints/IntegerRelation inspired storage.
// Inequalities are represented as: sum(coefficients[i] * x_i) + constant >= 0.
// Equalities are represented as: sum(coefficients[i] * x_i) + constant == 0.
struct LinearConstraint {
    std::vector<std::int64_t> coefficients;
    std::int64_t constant = 0;
    bool equality = false;

    bool is_satisfied_by(const std::vector<std::int64_t> &point) const;
};

class IntegerRelation {
  public:
    explicit IntegerRelation(unsigned num_vars = 0);

    unsigned num_vars() const {
        return num_vars_;
    }
    const std::vector<LinearConstraint> &constraints() const {
        return constraints_;
    }

    void add_vars(unsigned count);
    void add_inequality(std::vector<std::int64_t> coefficients, std::int64_t constant);
    void add_equality(std::vector<std::int64_t> coefficients, std::int64_t constant);
    void append(const IntegerRelation &other);

    bool contains(const std::vector<std::int64_t> &point) const;
    bool is_integer_empty() const;
    std::optional<std::vector<std::int64_t>> find_integer_sample() const;
    std::optional<std::vector<std::int64_t>> find_lexicographic_maximum() const;

  private:
    void normalize_coefficients(std::vector<std::int64_t> &coefficients) const;

    unsigned num_vars_ = 0;
    std::vector<LinearConstraint> constraints_;
};

class PresburgerRelation {
  public:
    PresburgerRelation() = default;
    explicit PresburgerRelation(IntegerRelation relation);

    const std::vector<IntegerRelation> &disjuncts() const {
        return disjuncts_;
    }

    void union_in_place(IntegerRelation relation);
    void union_in_place(const PresburgerRelation &relation);

    bool is_integer_empty() const;
    std::optional<std::vector<std::int64_t>> find_integer_sample() const;
    std::optional<std::vector<std::int64_t>> find_lexicographic_maximum() const;

  private:
    std::vector<IntegerRelation> disjuncts_;
};

class Simplex {
  public:
    explicit Simplex(const IntegerRelation &relation) : relation_(relation) {}

    // TODO(polyhedral): replace the bounded fallback in the .cpp with a real tableau simplex.
    bool is_rational_empty() const;
    std::optional<std::vector<std::int64_t>> find_integer_sample() const;

  private:
    const IntegerRelation &relation_;
};

class BranchAndBound {
  public:
    explicit BranchAndBound(const IntegerRelation &relation) : relation_(relation) {}

    // TODO(polyhedral): split fractional simplex samples and branch on integer dimensions.
    std::optional<std::vector<std::int64_t>> find_integer_sample() const;
    std::optional<std::vector<std::int64_t>> find_lexicographic_maximum() const;

  private:
    const IntegerRelation &relation_;
};

} // namespace yir::presburger
