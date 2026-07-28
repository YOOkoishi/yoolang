#include "yir/Presburger.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace yir::presburger {

namespace {

constexpr std::int64_t kSearchBound = 16;

// Reject a partial assignment as soon as any linear constraint cannot be
// satisfied by the remaining variable intervals.  This keeps the bounded
// fallback useful for relation legality and affine equivalence checks without
// turning a high-dimensional box into a blind Cartesian-product walk.
bool partial_constraints_possible(const IntegerRelation &relation,
                                  const std::vector<std::int64_t> &point,
                                  const std::vector<std::int64_t> &lower,
                                  const std::vector<std::int64_t> &upper) {
    for (const auto &constraint : relation.constraints()) {
        if (constraint.coefficients.size() != lower.size()) {
            return false;
        }
        __int128 minimum = constraint.constant;
        __int128 maximum = constraint.constant;
        for (std::size_t index = 0; index < constraint.coefficients.size(); ++index) {
            const auto coefficient = constraint.coefficients[index];
            if (index < point.size()) {
                const auto contribution = static_cast<__int128>(coefficient) * point[index];
                minimum += contribution;
                maximum += contribution;
                continue;
            }
            if (coefficient >= 0) {
                minimum += static_cast<__int128>(coefficient) * lower[index];
                maximum += static_cast<__int128>(coefficient) * upper[index];
            } else {
                minimum += static_cast<__int128>(coefficient) * upper[index];
                maximum += static_cast<__int128>(coefficient) * lower[index];
            }
        }
        if (constraint.equality) {
            if (minimum > 0 || maximum < 0) {
                return false;
            }
        } else if (maximum < 0) {
            return false;
        }
    }
    return true;
}

bool enumerate_box(unsigned num_vars, std::vector<std::int64_t> &point,
                   const std::vector<std::int64_t> &lower,
                   const std::vector<std::int64_t> &upper,
                   const IntegerRelation &relation,
                   std::optional<std::vector<std::int64_t>> &sample) {
    if (point.size() == num_vars) {
        if (relation.contains(point)) {
            sample = point;
            return true;
        }
        return false;
    }
    if (lower[point.size()] > upper[point.size()]) {
        return false;
    }
    for (std::int64_t value = lower[point.size()];;) {
        point.push_back(value);
        if (partial_constraints_possible(relation, point, lower, upper) &&
            enumerate_box(num_vars, point, lower, upper, relation, sample)) {
            return true;
        }
        point.pop_back();
        if (value == upper[point.size()]) {
            break;
        }
        ++value;
    }
    return false;
}

IntegerFeasibility enumerate_box_with_budget(
    unsigned num_vars, std::vector<std::int64_t> &point,
    const std::vector<std::int64_t> &lower,
    const std::vector<std::int64_t> &upper, const IntegerRelation &relation,
    std::size_t &remaining_nodes) {
    if (remaining_nodes == 0) {
        return IntegerFeasibility::Unknown;
    }
    --remaining_nodes;
    if (point.size() == num_vars) {
        return relation.contains(point) ? IntegerFeasibility::NonEmpty
                                        : IntegerFeasibility::Empty;
    }

    const auto dim = point.size();
    bool saw_unknown = false;
    for (std::int64_t value = lower[dim]; value <= upper[dim]; ++value) {
        point.push_back(value);
        if (partial_constraints_possible(relation, point, lower, upper)) {
            const auto result = enumerate_box_with_budget(
                num_vars, point, lower, upper, relation, remaining_nodes);
            point.pop_back();
            if (result == IntegerFeasibility::NonEmpty) {
                return result;
            }
            saw_unknown = saw_unknown || result == IntegerFeasibility::Unknown;
        } else {
            point.pop_back();
        }
        if (value == std::numeric_limits<std::int64_t>::max()) {
            break;
        }
    }
    return saw_unknown ? IntegerFeasibility::Unknown : IntegerFeasibility::Empty;
}

bool enumerate_lexmax(unsigned num_vars, std::vector<std::int64_t> &point,
                      const std::vector<std::int64_t> &lower,
                      const std::vector<std::int64_t> &upper,
                      const IntegerRelation &relation,
                      std::optional<std::vector<std::int64_t>> &best) {
    if (point.size() == num_vars) {
        if (relation.contains(point) && (!best || point > *best)) {
            best = point;
        }
        return best.has_value();
    }
    if (lower[point.size()] > upper[point.size()]) {
        return false;
    }
    for (std::int64_t value = upper[point.size()];;) {
        point.push_back(value);
        if (partial_constraints_possible(relation, point, lower, upper)) {
            enumerate_lexmax(num_vars, point, lower, upper, relation, best);
        }
        point.pop_back();
        if (best && point.size() + 1 == num_vars) {
            return true;
        }
        if (value == lower[point.size()]) {
            break;
        }
        --value;
    }
    return best.has_value();
}

std::int64_t clamp_i64(__int128 value) {
    if (value < std::numeric_limits<std::int64_t>::min()) {
        return std::numeric_limits<std::int64_t>::min();
    }
    if (value > std::numeric_limits<std::int64_t>::max()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(value);
}

std::int64_t floor_div(__int128 numerator, __int128 denominator) {
    const auto quotient = numerator / denominator;
    const auto remainder = numerator % denominator;
    const auto result = remainder != 0 && ((remainder < 0) != (denominator < 0))
                            ? quotient - 1
                            : quotient;
    return clamp_i64(result);
}

std::int64_t ceil_div(__int128 numerator, __int128 denominator) {
    const auto quotient = numerator / denominator;
    const auto remainder = numerator % denominator;
    const auto result = remainder != 0 && ((remainder > 0) == (denominator > 0))
                            ? quotient + 1
                            : quotient;
    return clamp_i64(result);
}

std::int64_t saturating_add(std::int64_t lhs, std::int64_t rhs) {
    return clamp_i64(static_cast<__int128>(lhs) + rhs);
}

std::int64_t saturating_sub(std::int64_t lhs, std::int64_t rhs) {
    return clamp_i64(static_cast<__int128>(lhs) - rhs);
}

void derive_box(const IntegerRelation &relation, std::vector<std::int64_t> &lower,
                std::vector<std::int64_t> &upper) {
    struct Bounds {
        std::int64_t lower = 0;
        std::int64_t upper = 0;
        bool has_lower = false;
        bool has_upper = false;
    };
    std::vector<Bounds> bounds(relation.num_vars());
    for (const auto &constraint : relation.constraints()) {
        std::size_t variable = 0;
        std::int64_t coefficient = 0;
        bool single_variable = true;
        for (std::size_t i = 0; i < constraint.coefficients.size(); ++i) {
            if (constraint.coefficients[i] == 0) {
                continue;
            }
            if (coefficient != 0) {
                single_variable = false;
                break;
            }
            variable = i;
            coefficient = constraint.coefficients[i];
        }
        if (!single_variable || coefficient == 0 || variable >= bounds.size()) {
            continue;
        }
        auto &bound = bounds[variable];
        if (constraint.equality) {
            const __int128 numerator = -static_cast<__int128>(constraint.constant);
            if (numerator % coefficient == 0) {
                const __int128 quotient = numerator / coefficient;
                if (quotient < std::numeric_limits<std::int64_t>::min() ||
                    quotient > std::numeric_limits<std::int64_t>::max()) {
                    bound.lower = 1;
                    bound.upper = 0;
                } else {
                    const auto value = static_cast<std::int64_t>(quotient);
                    bound.lower = bound.has_lower ? std::max(bound.lower, value) : value;
                    bound.upper = bound.has_upper ? std::min(bound.upper, value) : value;
                }
            } else {
                bound.lower = 1;
                bound.upper = 0;
            }
            bound.has_lower = true;
            bound.has_upper = true;
            continue;
        }
        if (coefficient > 0) {
            const auto value =
                ceil_div(-static_cast<__int128>(constraint.constant), coefficient);
            bound.lower = bound.has_lower ? std::max(bound.lower, value) : value;
            bound.has_lower = true;
        } else {
            const auto value = floor_div(static_cast<__int128>(constraint.constant),
                                         -static_cast<__int128>(coefficient));
            bound.upper = bound.has_upper ? std::min(bound.upper, value) : value;
            bound.has_upper = true;
        }
    }

    lower.resize(bounds.size());
    upper.resize(bounds.size());
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        const auto &bound = bounds[i];
        if (bound.has_lower && bound.has_upper) {
            lower[i] = bound.lower;
            upper[i] = bound.upper;
        } else if (bound.has_lower) {
            lower[i] = bound.lower;
            upper[i] = saturating_add(bound.lower, 2 * kSearchBound);
        } else if (bound.has_upper) {
            lower[i] = saturating_sub(bound.upper, 2 * kSearchBound);
            upper[i] = bound.upper;
        } else {
            lower[i] = -kSearchBound;
            upper[i] = kSearchBound;
        }
    }
}

bool has_explicit_box(const IntegerRelation &relation) {
    std::vector<bool> has_lower(relation.num_vars(), false);
    std::vector<bool> has_upper(relation.num_vars(), false);
    for (const auto &constraint : relation.constraints()) {
        std::size_t variable = 0;
        std::int64_t coefficient = 0;
        bool single_variable = true;
        for (std::size_t index = 0; index < constraint.coefficients.size(); ++index) {
            if (constraint.coefficients[index] == 0) {
                continue;
            }
            if (coefficient != 0) {
                single_variable = false;
                break;
            }
            variable = index;
            coefficient = constraint.coefficients[index];
        }
        if (!single_variable || coefficient == 0 || variable >= relation.num_vars()) {
            continue;
        }
        if (constraint.equality) {
            has_lower[variable] = true;
            has_upper[variable] = true;
        } else if (coefficient > 0) {
            has_lower[variable] = true;
        } else {
            has_upper[variable] = true;
        }
    }
    return std::all_of(has_lower.begin(), has_lower.end(), [](bool value) { return value; }) &&
           std::all_of(has_upper.begin(), has_upper.end(), [](bool value) { return value; });
}

} // namespace

bool LinearConstraint::is_satisfied_by(const std::vector<std::int64_t> &point) const {
    if (point.size() < coefficients.size()) {
        return false;
    }
    __int128 value = constant;
    for (std::size_t i = 0; i < coefficients.size(); ++i) {
        value += static_cast<__int128>(coefficients[i]) * point[i];
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

IntegerFeasibility
IntegerRelation::check_integer_feasibility(std::size_t max_search_nodes) const {
    if (max_search_nodes == 0) {
        return IntegerFeasibility::Unknown;
    }
    if (!has_explicit_box(*this)) {
        return IntegerFeasibility::Unknown;
    }
    std::vector<std::int64_t> point;
    std::vector<std::int64_t> lower;
    std::vector<std::int64_t> upper;
    derive_box(*this, lower, upper);
    return enumerate_box_with_budget(num_vars_, point, lower, upper, *this,
                                     max_search_nodes);
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
    std::vector<std::int64_t> lower;
    std::vector<std::int64_t> upper;
    derive_box(relation_, lower, upper);
    std::optional<std::vector<std::int64_t>> sample;
    enumerate_box(relation_.num_vars(), point, lower, upper, relation_, sample);
    return sample;
}

std::optional<std::vector<std::int64_t>> BranchAndBound::find_integer_sample() const {
    return Simplex(relation_).find_integer_sample();
}

std::optional<std::vector<std::int64_t>> BranchAndBound::find_lexicographic_maximum() const {
    std::vector<std::int64_t> point;
    std::vector<std::int64_t> lower;
    std::vector<std::int64_t> upper;
    derive_box(relation_, lower, upper);
    std::optional<std::vector<std::int64_t>> best;
    enumerate_lexmax(relation_.num_vars(), point, lower, upper, relation_, best);
    return best;
}

} // namespace yir::presburger
