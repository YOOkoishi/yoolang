#include "../../include/yir/Presburger.h"

#include <algorithm>
#include <utility>

namespace yir::presburger {

namespace {

constexpr std::int64_t kSearchBound = 16;

bool enumerate_box(unsigned num_vars, std::vector<std::int64_t> &point,
                   std::int64_t lower, std::int64_t upper,
                   const IntegerRelation &relation,
                   std::optional<std::vector<std::int64_t>> &sample) {
    if (point.size() == num_vars) {
        if (relation.contains(point)) {
            sample = point;
            return true;
        }
        return false;
    }
    for (std::int64_t value = lower; value <= upper; ++value) {
        point.push_back(value);
        if (enumerate_box(num_vars, point, lower, upper, relation, sample)) {
            return true;
        }
        point.pop_back();
    }
    return false;
}

bool enumerate_lexmax(unsigned num_vars, std::vector<std::int64_t> &point,
                      std::int64_t lower, std::int64_t upper,
                      const IntegerRelation &relation,
                      std::optional<std::vector<std::int64_t>> &best) {
    if (point.size() == num_vars) {
        if (relation.contains(point) && (!best || point > *best)) {
            best = point;
        }
        return best.has_value();
    }
    for (std::int64_t value = upper; value >= lower; --value) {
        point.push_back(value);
        enumerate_lexmax(num_vars, point, lower, upper, relation, best);
        point.pop_back();
        if (best && point.size() + 1 == num_vars) {
            return true;
        }
    }
    return best.has_value();
}

} // namespace

bool LinearConstraint::is_satisfied_by(const std::vector<std::int64_t> &point) const {
    if (point.size() < coefficients.size()) {
        return false;
    }
    std::int64_t value = constant;
    for (std::size_t i = 0; i < coefficients.size(); ++i) {
        value += coefficients[i] * point[i];
    }
    return equality ? value == 0 : value >= 0;
}

IntegerRelation::IntegerRelation(unsigned num_vars)
    : num_vars_(num_vars), num_domain_vars_(num_vars) {}

IntegerRelation::IntegerRelation(unsigned num_domain_vars, unsigned num_range_vars,
                                 unsigned num_symbol_vars, unsigned num_local_vars)
    : num_vars_(num_domain_vars + num_range_vars + num_symbol_vars + num_local_vars),
      num_domain_vars_(num_domain_vars), num_range_vars_(num_range_vars),
      num_symbol_vars_(num_symbol_vars), num_local_vars_(num_local_vars) {}

unsigned IntegerRelation::num_vars(VarKind kind) const {
    switch (kind) {
    case VarKind::Domain:
        return num_domain_vars_;
    case VarKind::Range:
        return num_range_vars_;
    case VarKind::Symbol:
        return num_symbol_vars_;
    case VarKind::Local:
        return num_local_vars_;
    }
    return 0;
}

void IntegerRelation::add_vars(unsigned count) {
    add_vars(VarKind::Domain, count);
}

void IntegerRelation::add_vars(VarKind kind, unsigned count) {
    if (count == 0) {
        return;
    }
    const unsigned pos = insert_pos(kind);
    num_vars_ += count;
    switch (kind) {
    case VarKind::Domain:
        num_domain_vars_ += count;
        break;
    case VarKind::Range:
        num_range_vars_ += count;
        break;
    case VarKind::Symbol:
        num_symbol_vars_ += count;
        break;
    case VarKind::Local:
        num_local_vars_ += count;
        break;
    }
    for (auto &constraint : constraints_) {
        constraint.coefficients.insert(constraint.coefficients.begin() + static_cast<std::ptrdiff_t>(pos),
                                       count, 0);
    }
}

void IntegerRelation::add_inequality(std::vector<std::int64_t> coefficients,
                                     std::int64_t constant) {
    normalize_coefficients(coefficients);
    constraints_.push_back({std::move(coefficients), constant, false});
}

void IntegerRelation::add_equality(std::vector<std::int64_t> coefficients,
                                   std::int64_t constant) {
    normalize_coefficients(coefficients);
    constraints_.push_back({std::move(coefficients), constant, true});
}

void IntegerRelation::append(const IntegerRelation &other) {
    if (other.num_vars() > num_vars_) {
        add_vars(other.num_vars() - num_vars_);
    }
    for (auto constraint : other.constraints()) {
        normalize_coefficients(constraint.coefficients);
        constraints_.push_back(std::move(constraint));
    }
}

bool IntegerRelation::has_compatible_space(const IntegerRelation &other) const {
    return num_domain_vars_ == other.num_domain_vars_ &&
           num_range_vars_ == other.num_range_vars_ &&
           num_symbol_vars_ == other.num_symbol_vars_ &&
           num_local_vars_ == other.num_local_vars_;
}

IntegerRelation IntegerRelation::intersect(const IntegerRelation &other) const {
    IntegerRelation result = *this;
    result.append(other);
    return result;
}

bool IntegerRelation::contains(const std::vector<std::int64_t> &point) const {
    if (point.size() != num_vars_) {
        return false;
    }
    return std::all_of(constraints_.begin(), constraints_.end(),
                       [&point](const LinearConstraint &constraint) {
                           return constraint.is_satisfied_by(point);
                       });
}

bool IntegerRelation::is_integer_empty() const {
    return !find_integer_sample().has_value();
}

std::optional<std::vector<std::int64_t>> IntegerRelation::find_integer_sample() const {
    return BranchAndBound(*this).find_integer_sample();
}

std::optional<std::vector<std::int64_t>> IntegerRelation::find_lexicographic_maximum() const {
    return BranchAndBound(*this).find_lexicographic_maximum();
}

void IntegerRelation::normalize_coefficients(std::vector<std::int64_t> &coefficients) const {
    coefficients.resize(num_vars_, 0);
}

unsigned IntegerRelation::insert_pos(VarKind kind) const {
    switch (kind) {
    case VarKind::Domain:
        return num_domain_vars_;
    case VarKind::Range:
        return num_domain_vars_ + num_range_vars_;
    case VarKind::Symbol:
        return num_domain_vars_ + num_range_vars_ + num_symbol_vars_;
    case VarKind::Local:
        return num_vars_;
    }
    return num_vars_;
}

PresburgerRelation::PresburgerRelation(IntegerRelation relation) {
    disjuncts_.push_back(std::move(relation));
}

void PresburgerRelation::union_in_place(IntegerRelation relation) {
    disjuncts_.push_back(std::move(relation));
}

void PresburgerRelation::union_in_place(const PresburgerRelation &relation) {
    disjuncts_.insert(disjuncts_.end(), relation.disjuncts().begin(), relation.disjuncts().end());
}

PresburgerRelation PresburgerRelation::intersect(const PresburgerRelation &relation) const {
    PresburgerRelation out;
    for (const auto &lhs : disjuncts_) {
        for (const auto &rhs : relation.disjuncts()) {
            out.union_in_place(lhs.intersect(rhs));
        }
    }
    return out;
}

bool PresburgerRelation::is_integer_empty() const {
    return !find_integer_sample().has_value();
}

std::optional<std::vector<std::int64_t>> PresburgerRelation::find_integer_sample() const {
    for (const auto &disjunct : disjuncts_) {
        if (auto sample = disjunct.find_integer_sample()) {
            return sample;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<std::int64_t>> PresburgerRelation::find_lexicographic_maximum() const {
    std::optional<std::vector<std::int64_t>> best;
    for (const auto &disjunct : disjuncts_) {
        auto candidate = disjunct.find_lexicographic_maximum();
        if (candidate && (!best || *candidate > *best)) {
            best = std::move(candidate);
        }
    }
    return best;
}

bool Simplex::is_rational_empty() const {
    return !find_integer_sample().has_value();
}

std::optional<std::vector<std::int64_t>> Simplex::find_integer_sample() const {
    std::vector<std::int64_t> point;
    std::optional<std::vector<std::int64_t>> sample;
    enumerate_box(relation_.num_vars(), point, -kSearchBound, kSearchBound, relation_, sample);
    return sample;
}

std::optional<std::vector<std::int64_t>> BranchAndBound::find_integer_sample() const {
    return Simplex(relation_).find_integer_sample();
}

std::optional<std::vector<std::int64_t>> BranchAndBound::find_lexicographic_maximum() const {
    std::vector<std::int64_t> point;
    std::optional<std::vector<std::int64_t>> best;
    enumerate_lexmax(relation_.num_vars(), point, -kSearchBound, kSearchBound, relation_, best);
    return best;
}

} // namespace yir::presburger
