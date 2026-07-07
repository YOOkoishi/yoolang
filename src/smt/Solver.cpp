#include "smt/Solver.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace smt {
namespace {

enum class LitValue {
    False,
    True,
    Unassigned,
};

struct BVBits {
    std::vector<int> bits;
};

struct LinearForm {
    std::uint32_t width = 0;
    std::uint64_t constant = 0;
    std::map<std::string, std::uint64_t> coeffs;
};

std::uint64_t mask_for_width(std::uint32_t width) {
    if (width >= 64) {
        return ~0ULL;
    }
    return (1ULL << width) - 1ULL;
}

void add_coeff(LinearForm &form, std::string key, std::uint64_t delta) {
    const auto mask = mask_for_width(form.width);
    auto it = form.coeffs.find(key);
    const auto old_value = it == form.coeffs.end() ? 0 : it->second;
    auto value = (old_value + delta) & mask;
    if (value == 0) {
        if (it != form.coeffs.end()) {
            form.coeffs.erase(it);
        }
        return;
    }
    if (it != form.coeffs.end()) {
        it->second = value;
    } else {
        form.coeffs.emplace(std::move(key), value);
    }
}

void add_forms(LinearForm &lhs, const LinearForm &rhs, bool negate_rhs) {
    const auto mask = mask_for_width(lhs.width);
    lhs.constant = (lhs.constant + (negate_rhs ? (0 - rhs.constant) : rhs.constant)) & mask;
    for (const auto &[key, coeff] : rhs.coeffs) {
        add_coeff(lhs, key, negate_rhs ? (0 - coeff) & mask : coeff);
    }
}

class SatSolver {
  public:
    int new_var() {
        ++var_count_;
        return var_count_;
    }

    void add_clause(std::vector<int> clause) { clauses_.push_back(std::move(clause)); }

    CheckStatus solve(const std::vector<int> &assumptions, const SolverOptions &options) {
        options_ = options;
        start_ = std::chrono::steady_clock::now();
        decisions_ = 0;
        conflicts_ = 0;
        propagations_ = 0;
        limit_exhausted_ = false;
        assignment_.assign(static_cast<std::size_t>(var_count_ + 1), -1);
        trail_.clear();
        trail_limits_.clear();
        qhead_ = 0;
        if (!rebuild_watches()) {
            return CheckStatus::Unsat;
        }
        for (int assumption : assumptions) {
            if (!assign(assumption)) {
                return CheckStatus::Unsat;
            }
        }
        const bool sat = dfs();
        if (limit_exhausted_) {
            return CheckStatus::Timeout;
        }
        return sat ? CheckStatus::Sat : CheckStatus::Unsat;
    }

    int value_of_var(int var) const {
        if (var <= 0 || var >= static_cast<int>(assignment_.size())) {
            return -1;
        }
        return assignment_[static_cast<std::size_t>(var)];
    }

    std::uint64_t variable_count() const { return static_cast<std::uint64_t>(var_count_); }
    std::uint64_t clause_count() const { return static_cast<std::uint64_t>(clauses_.size()); }
    std::uint64_t decisions() const { return decisions_; }
    std::uint64_t conflicts() const { return conflicts_; }
    std::uint64_t propagations() const { return propagations_; }

  private:
    bool rebuild_watches() {
        watches_.assign(static_cast<std::size_t>((var_count_ + 1) * 2), {});
        watch_pos_.clear();
        watch_pos_.reserve(clauses_.size());
        for (std::size_t clause_index = 0; clause_index < clauses_.size(); ++clause_index) {
            const auto &clause = clauses_[clause_index];
            if (clause.empty()) {
                return false;
            }
            const int first = 0;
            const int second = clause.size() == 1 ? 0 : 1;
            watch_pos_.push_back({first, second});
            watches_[literal_index(clause[static_cast<std::size_t>(first)])].push_back(
                static_cast<int>(clause_index));
            if (second != first) {
                watches_[literal_index(clause[static_cast<std::size_t>(second)])].push_back(
                    static_cast<int>(clause_index));
            }
            if (clause.size() == 1 && !assign(clause.front())) {
                return false;
            }
        }
        return true;
    }

    bool dfs() {
        if (!within_limits()) {
            limit_exhausted_ = true;
            return false;
        }
        if (!propagate()) {
            ++conflicts_;
            if (!within_limits()) {
                limit_exhausted_ = true;
            }
            return false;
        }
        if (all_clauses_satisfied()) {
            return true;
        }
        const int decision_lit = choose_decision_literal();
        if (decision_lit == 0) {
            return true;
        }
        if (!can_make_decision()) {
            limit_exhausted_ = true;
            return false;
        }
        ++decisions_;
        push_level();
        if (assign(decision_lit) && dfs()) {
            return true;
        }
        pop_level();
        if (limit_exhausted_) {
            return false;
        }
        if (!can_make_decision()) {
            limit_exhausted_ = true;
            return false;
        }
        ++decisions_;
        push_level();
        if (assign(-decision_lit) && dfs()) {
            return true;
        }
        pop_level();
        return false;
    }

    bool propagate() {
        while (qhead_ < trail_.size()) {
            if (!within_limits()) {
                limit_exhausted_ = true;
                return false;
            }
            ++propagations_;
            const int lit = trail_[qhead_++];
            const int false_lit = -lit;
            auto &watch_list = watches_[literal_index(false_lit)];
            for (std::size_t i = 0; i < watch_list.size();) {
                const int clause_index = watch_list[i];
                auto &clause = clauses_[static_cast<std::size_t>(clause_index)];
                auto &positions = watch_pos_[static_cast<std::size_t>(clause_index)];
                int current_slot = -1;
                if (clause[static_cast<std::size_t>(positions.first)] == false_lit) {
                    current_slot = 0;
                } else if (clause[static_cast<std::size_t>(positions.second)] == false_lit) {
                    current_slot = 1;
                } else {
                    ++i;
                    continue;
                }

                const int current_pos = current_slot == 0 ? positions.first : positions.second;
                const int other_pos = current_slot == 0 ? positions.second : positions.first;
                const int other_lit = clause[static_cast<std::size_t>(other_pos)];
                if (literal_value(other_lit) == LitValue::True) {
                    ++i;
                    continue;
                }

                bool moved = false;
                for (std::size_t next = 0; next < clause.size(); ++next) {
                    if (static_cast<int>(next) == current_pos ||
                        static_cast<int>(next) == other_pos) {
                        continue;
                    }
                    if (literal_value(clause[next]) != LitValue::False) {
                        if (current_slot == 0) {
                            positions.first = static_cast<int>(next);
                        } else {
                            positions.second = static_cast<int>(next);
                        }
                        watch_list[i] = watch_list.back();
                        watch_list.pop_back();
                        watches_[literal_index(clause[next])].push_back(clause_index);
                        moved = true;
                        break;
                    }
                }
                if (moved) {
                    continue;
                }

                if (literal_value(other_lit) == LitValue::False || !assign(other_lit)) {
                    return false;
                }
                ++i;
            }
        }
        return true;
    }

    bool assign(int lit) {
        const int var = std::abs(lit);
        const int value = lit > 0 ? 1 : 0;
        auto &slot = assignment_[static_cast<std::size_t>(var)];
        if (slot != -1) {
            return slot == value;
        }
        slot = value;
        trail_.push_back(lit);
        return true;
    }

    void push_level() { trail_limits_.push_back(trail_.size()); }

    void pop_level() {
        const auto target_size = trail_limits_.back();
        trail_limits_.pop_back();
        while (trail_.size() > target_size) {
            const int var = std::abs(trail_.back());
            assignment_[static_cast<std::size_t>(var)] = -1;
            trail_.pop_back();
        }
        if (qhead_ > trail_.size()) {
            qhead_ = trail_.size();
        }
    }

    bool all_clauses_satisfied() const {
        for (const auto &clause : clauses_) {
            bool satisfied = false;
            for (int lit : clause) {
                if (literal_value(lit) == LitValue::True) {
                    satisfied = true;
                    break;
                }
            }
            if (!satisfied) {
                return false;
            }
        }
        return true;
    }

    int choose_decision_literal() const {
        for (const auto &clause : clauses_) {
            bool satisfied = false;
            int candidate = 0;
            for (int lit : clause) {
                const auto value = literal_value(lit);
                if (value == LitValue::True) {
                    satisfied = true;
                    break;
                }
                if (value == LitValue::Unassigned && candidate == 0) {
                    candidate = lit;
                }
            }
            if (!satisfied && candidate != 0) {
                return candidate;
            }
        }
        for (int var = 1; var <= var_count_; ++var) {
            if (assignment_[static_cast<std::size_t>(var)] == -1) {
                return var;
            }
        }
        return 0;
    }

    LitValue literal_value(int lit) const {
        const int var = std::abs(lit);
        const int value = assignment_[static_cast<std::size_t>(var)];
        if (value == -1) {
            return LitValue::Unassigned;
        }
        const bool lit_true = lit > 0 ? value == 1 : value == 0;
        return lit_true ? LitValue::True : LitValue::False;
    }

    std::size_t literal_index(int lit) const {
        const auto var = static_cast<std::size_t>(std::abs(lit));
        return (var - 1) * 2 + (lit < 0 ? 1 : 0);
    }

    bool can_make_decision() const { return decisions_ < options_.max_decisions; }

    bool within_limits() const {
        if (static_cast<std::uint64_t>(var_count_) > options_.max_sat_variables ||
            static_cast<std::uint64_t>(clauses_.size()) > options_.max_clauses ||
            conflicts_ > options_.max_conflicts || propagations_ > options_.max_propagations) {
            return false;
        }
        if (options_.timeout_us > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - start_)
                                     .count();
            if (elapsed > options_.timeout_us) {
                return false;
            }
        }
        return true;
    }

    int var_count_ = 0;
    std::vector<std::vector<int>> clauses_;
    std::vector<std::pair<int, int>> watch_pos_;
    std::vector<std::vector<int>> watches_;
    std::vector<int> assignment_;
    std::vector<int> trail_;
    std::vector<std::size_t> trail_limits_;
    std::size_t qhead_ = 0;
    SolverOptions options_;
    std::chrono::steady_clock::time_point start_;
    std::uint64_t decisions_ = 0;
    std::uint64_t conflicts_ = 0;
    std::uint64_t propagations_ = 0;
    bool limit_exhausted_ = false;
};

class BitBlaster {
  public:
    explicit BitBlaster(SatSolver &sat) : sat_(sat) {}

    int bool_expr(const Expr &expr) {
        auto found = bool_cache_.find(expr.raw());
        if (found != bool_cache_.end()) {
            return found->second;
        }
        int lit = 0;
        const auto &node = *expr.raw();
        switch (node.kind) {
        case ExprKind::BoolConst:
            lit = node.bool_value ? true_lit() : false_lit();
            break;
        case ExprKind::Var:
            if (!node.sort.is_bool()) {
                throw std::invalid_argument("bit-vector variable used as Boolean");
            }
            lit = bool_var(node.name);
            break;
        case ExprKind::Not:
            lit = -bool_expr(node.args[0]);
            break;
        case ExprKind::And:
            lit = make_and(bool_expr(node.args[0]), bool_expr(node.args[1]));
            break;
        case ExprKind::Or:
            lit = make_or(bool_expr(node.args[0]), bool_expr(node.args[1]));
            break;
        case ExprKind::Xor:
            lit = make_xor(bool_expr(node.args[0]), bool_expr(node.args[1]));
            break;
        case ExprKind::Implies:
            lit = make_or(-bool_expr(node.args[0]), bool_expr(node.args[1]));
            break;
        case ExprKind::Equal:
            lit = equal_expr(node.args[0], node.args[1]);
            break;
        case ExprKind::Distinct:
            lit = -equal_expr(node.args[0], node.args[1]);
            break;
        case ExprKind::BVUlt:
            lit = unsigned_less_than(bv_expr(node.args[0]).bits, bv_expr(node.args[1]).bits);
            break;
        case ExprKind::BVUle:
            lit = make_or(unsigned_less_than(bv_expr(node.args[0]).bits, bv_expr(node.args[1]).bits),
                          equal_bits(bv_expr(node.args[0]).bits, bv_expr(node.args[1]).bits));
            break;
        case ExprKind::BVSlt:
            lit = signed_less_than(bv_expr(node.args[0]).bits, bv_expr(node.args[1]).bits);
            break;
        case ExprKind::BVSle:
            lit = make_or(signed_less_than(bv_expr(node.args[0]).bits, bv_expr(node.args[1]).bits),
                          equal_bits(bv_expr(node.args[0]).bits, bv_expr(node.args[1]).bits));
            break;
        default:
            throw std::invalid_argument("bit-vector expression used as Boolean");
        }
        bool_cache_[expr.raw()] = lit;
        ++bitblast_nodes_;
        return lit;
    }

    BVBits bv_expr(const Expr &expr) {
        auto found = bv_cache_.find(expr.raw());
        if (found != bv_cache_.end()) {
            return found->second;
        }
        BVBits result;
        const auto &node = *expr.raw();
        switch (node.kind) {
        case ExprKind::BVConst:
            result.bits = const_bits(node.sort.width(), node.bv_value);
            break;
        case ExprKind::Var:
            if (!node.sort.is_bit_vector()) {
                throw std::invalid_argument("Boolean variable used as bit-vector");
            }
            result.bits = bv_var(node.name, node.sort.width());
            break;
        case ExprKind::BVNot:
            result.bits = map_unary(bv_expr(node.args[0]).bits, [](int bit) { return -bit; });
            break;
        case ExprKind::BVAnd:
            result.bits =
                map_binary(bv_expr(node.args[0]).bits, bv_expr(node.args[1]).bits,
                           [this](int lhs, int rhs) { return make_and(lhs, rhs); });
            break;
        case ExprKind::BVOr:
            result.bits =
                map_binary(bv_expr(node.args[0]).bits, bv_expr(node.args[1]).bits,
                           [this](int lhs, int rhs) { return make_or(lhs, rhs); });
            break;
        case ExprKind::BVXor:
            result.bits =
                map_binary(bv_expr(node.args[0]).bits, bv_expr(node.args[1]).bits,
                           [this](int lhs, int rhs) { return make_xor(lhs, rhs); });
            break;
        case ExprKind::BVAdd:
            result.bits = add_bits(bv_expr(node.args[0]).bits, bv_expr(node.args[1]).bits);
            break;
        case ExprKind::BVSub:
            result.bits = add_bits(bv_expr(node.args[0]).bits, negate_bits(bv_expr(node.args[1]).bits));
            break;
        case ExprKind::BVNeg:
            result.bits = negate_bits(bv_expr(node.args[0]).bits);
            break;
        case ExprKind::BVShl:
            result.bits = shift_left(bv_expr(node.args[0]).bits, node.shift_amount);
            break;
        case ExprKind::BVLshr:
            result.bits = logical_shift_right(bv_expr(node.args[0]).bits, node.shift_amount);
            break;
        case ExprKind::BVAshr:
            result.bits = arithmetic_shift_right(bv_expr(node.args[0]).bits, node.shift_amount);
            break;
        case ExprKind::BVConcat:
            result.bits = concat_bits(bv_expr(node.args[0]).bits, bv_expr(node.args[1]).bits);
            break;
        case ExprKind::BVExtract:
            result.bits = extract_bits(bv_expr(node.args[0]).bits, node.extract_high,
                                       node.extract_low);
            break;
        default:
            throw std::invalid_argument("Boolean expression used as bit-vector");
        }
        bv_cache_[expr.raw()] = result;
        ++bitblast_nodes_;
        return result;
    }

    void assert_lit(int lit) { sat_.add_clause({lit}); }

    std::uint64_t bitblast_nodes() const { return bitblast_nodes_; }

    void extract_model(const SatSolver &sat, Model &model) const {
        for (const auto &[name, lit] : bool_vars_) {
            model.set_bool(name, literal_is_true(sat, lit));
        }
        for (const auto &[name, bits] : bv_vars_) {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < bits.size() && i < 64; ++i) {
                if (literal_is_true(sat, bits[i])) {
                    value |= 1ULL << i;
                }
            }
            model.set_bit_vector(name, static_cast<std::uint32_t>(bits.size()), value);
        }
    }

  private:
    int true_lit() {
        if (true_lit_ == 0) {
            true_lit_ = sat_.new_var();
            sat_.add_clause({true_lit_});
        }
        return true_lit_;
    }

    int false_lit() { return -true_lit(); }

    int bool_var(const std::string &name) {
        auto found = bool_vars_.find(name);
        if (found != bool_vars_.end()) {
            return found->second;
        }
        const int lit = sat_.new_var();
        bool_vars_[name] = lit;
        return lit;
    }

    std::vector<int> bv_var(const std::string &name, std::uint32_t width) {
        auto found = bv_vars_.find(name);
        if (found != bv_vars_.end()) {
            if (found->second.size() != width) {
                throw std::invalid_argument("bit-vector variable reused with a different width");
            }
            return found->second;
        }
        std::vector<int> bits;
        bits.reserve(width);
        for (std::uint32_t i = 0; i < width; ++i) {
            bits.push_back(sat_.new_var());
        }
        bv_vars_[name] = bits;
        return bits;
    }

    std::vector<int> const_bits(std::uint32_t width, std::uint64_t value) {
        std::vector<int> bits;
        bits.reserve(width);
        for (std::uint32_t i = 0; i < width; ++i) {
            bits.push_back(((value >> i) & 1ULL) != 0 ? true_lit() : false_lit());
        }
        return bits;
    }

    int equal_expr(const Expr &lhs, const Expr &rhs) {
        if (linear_equivalent(lhs, rhs)) {
            return true_lit();
        }
        if (lhs.sort().is_bool()) {
            return make_bool_equal(bool_expr(lhs), bool_expr(rhs));
        }
        return equal_bits(bv_expr(lhs).bits, bv_expr(rhs).bits);
    }

    bool linear_equivalent(const Expr &lhs, const Expr &rhs) {
        if (!lhs.sort().is_bit_vector() || lhs.sort() != rhs.sort()) {
            return lhs.raw() == rhs.raw();
        }
        auto lhs_form = linear_form(lhs);
        auto rhs_form = linear_form(rhs);
        if (!lhs_form || !rhs_form || lhs_form->width != rhs_form->width) {
            return lhs.raw() == rhs.raw();
        }
        add_forms(*lhs_form, *rhs_form, true);
        lhs_form->constant &= mask_for_width(lhs_form->width);
        return lhs_form->constant == 0 && lhs_form->coeffs.empty();
    }

    std::optional<LinearForm> linear_form(const Expr &expr) {
        auto found = linear_cache_.find(expr.raw());
        if (found != linear_cache_.end()) {
            return found->second;
        }
        if (!expr.sort().is_bit_vector()) {
            return std::nullopt;
        }
        const auto &node = *expr.raw();
        const auto width = node.sort.width();
        LinearForm form;
        form.width = width;
        switch (node.kind) {
        case ExprKind::BVConst:
            form.constant = node.bv_value & mask_for_width(width);
            break;
        case ExprKind::Var:
            add_coeff(form, "var:" + node.name + ":" + std::to_string(width), 1);
            break;
        case ExprKind::BVAdd: {
            auto lhs = linear_form(node.args[0]);
            auto rhs = linear_form(node.args[1]);
            if (!lhs || !rhs) {
                return std::nullopt;
            }
            form = *lhs;
            add_forms(form, *rhs, false);
            break;
        }
        case ExprKind::BVSub: {
            auto lhs = linear_form(node.args[0]);
            auto rhs = linear_form(node.args[1]);
            if (!lhs || !rhs) {
                return std::nullopt;
            }
            form = *lhs;
            add_forms(form, *rhs, true);
            break;
        }
        case ExprKind::BVNeg: {
            auto inner = linear_form(node.args[0]);
            if (!inner) {
                return std::nullopt;
            }
            form.constant = (0 - inner->constant) & mask_for_width(width);
            for (const auto &[key, coeff] : inner->coeffs) {
                add_coeff(form, key, (0 - coeff) & mask_for_width(width));
            }
            break;
        }
        default:
            return std::nullopt;
        }
        form.constant &= mask_for_width(width);
        linear_cache_[expr.raw()] = form;
        return form;
    }

    int equal_bits(const std::vector<int> &lhs, const std::vector<int> &rhs) {
        require_same_width(lhs, rhs);
        int result = true_lit();
        for (std::size_t i = 0; i < lhs.size(); ++i) {
            result = make_and(result, make_bool_equal(lhs[i], rhs[i]));
        }
        return result;
    }

    int unsigned_less_than(const std::vector<int> &lhs, const std::vector<int> &rhs) {
        require_same_width(lhs, rhs);
        int lt = false_lit();
        int eq = true_lit();
        for (std::size_t offset = 0; offset < lhs.size(); ++offset) {
            const std::size_t i = lhs.size() - 1 - offset;
            const int bit_lt = make_and(make_and(eq, -lhs[i]), rhs[i]);
            lt = make_or(lt, bit_lt);
            eq = make_and(eq, make_bool_equal(lhs[i], rhs[i]));
        }
        return lt;
    }

    int signed_less_than(const std::vector<int> &lhs, const std::vector<int> &rhs) {
        require_same_width(lhs, rhs);
        const int lhs_sign = lhs.back();
        const int rhs_sign = rhs.back();
        const int signs_differ = make_xor(lhs_sign, rhs_sign);
        const int lhs_negative_rhs_positive = make_and(lhs_sign, -rhs_sign);
        return make_or(lhs_negative_rhs_positive,
                       make_and(-signs_differ, unsigned_less_than(lhs, rhs)));
    }

    std::vector<int> add_bits(const std::vector<int> &lhs, const std::vector<int> &rhs) {
        require_same_width(lhs, rhs);
        std::vector<int> out;
        out.reserve(lhs.size());
        int carry = false_lit();
        for (std::size_t i = 0; i < lhs.size(); ++i) {
            const int axb = make_xor(lhs[i], rhs[i]);
            out.push_back(make_xor(axb, carry));
            carry = make_or(make_and(lhs[i], rhs[i]), make_and(carry, axb));
        }
        return out;
    }

    std::vector<int> negate_bits(const std::vector<int> &bits) {
        std::vector<int> inverted;
        inverted.reserve(bits.size());
        for (int bit : bits) {
            inverted.push_back(-bit);
        }
        return add_bits(inverted, const_bits(static_cast<std::uint32_t>(bits.size()), 1));
    }

    std::vector<int> shift_left(const std::vector<int> &bits, std::uint32_t amount) {
        std::vector<int> out(bits.size(), false_lit());
        for (std::size_t i = 0; i < bits.size(); ++i) {
            if (i >= amount) {
                out[i] = bits[i - amount];
            }
        }
        return out;
    }

    std::vector<int> logical_shift_right(const std::vector<int> &bits, std::uint32_t amount) {
        std::vector<int> out(bits.size(), false_lit());
        for (std::size_t i = 0; i < bits.size(); ++i) {
            const std::size_t src = i + amount;
            if (src < bits.size()) {
                out[i] = bits[src];
            }
        }
        return out;
    }

    std::vector<int> arithmetic_shift_right(const std::vector<int> &bits, std::uint32_t amount) {
        std::vector<int> out(bits.size(), bits.back());
        for (std::size_t i = 0; i < bits.size(); ++i) {
            const std::size_t src = i + amount;
            if (src < bits.size()) {
                out[i] = bits[src];
            }
        }
        return out;
    }

    std::vector<int> concat_bits(const std::vector<int> &high, const std::vector<int> &low) {
        std::vector<int> out;
        out.reserve(high.size() + low.size());
        out.insert(out.end(), low.begin(), low.end());
        out.insert(out.end(), high.begin(), high.end());
        return out;
    }

    std::vector<int> extract_bits(const std::vector<int> &bits, std::uint32_t high,
                                  std::uint32_t low) {
        if (low > high || high >= bits.size()) {
            throw std::invalid_argument("invalid bit-vector extract");
        }
        std::vector<int> out;
        out.reserve(high - low + 1);
        for (std::uint32_t i = low; i <= high; ++i) {
            out.push_back(bits[i]);
        }
        return out;
    }

    template <typename Fn>
    std::vector<int> map_unary(const std::vector<int> &bits, Fn fn) {
        std::vector<int> out;
        out.reserve(bits.size());
        for (int bit : bits) {
            out.push_back(fn(bit));
        }
        return out;
    }

    template <typename Fn>
    std::vector<int> map_binary(const std::vector<int> &lhs, const std::vector<int> &rhs,
                                Fn fn) {
        require_same_width(lhs, rhs);
        std::vector<int> out;
        out.reserve(lhs.size());
        for (std::size_t i = 0; i < lhs.size(); ++i) {
            out.push_back(fn(lhs[i], rhs[i]));
        }
        return out;
    }

    int make_and(int lhs, int rhs) {
        if (lhs == false_lit() || rhs == false_lit()) {
            return false_lit();
        }
        if (lhs == true_lit()) {
            return rhs;
        }
        if (rhs == true_lit()) {
            return lhs;
        }
        if (lhs == rhs) {
            return lhs;
        }
        if (lhs == -rhs) {
            return false_lit();
        }
        const int out = sat_.new_var();
        sat_.add_clause({-out, lhs});
        sat_.add_clause({-out, rhs});
        sat_.add_clause({out, -lhs, -rhs});
        return out;
    }

    int make_or(int lhs, int rhs) {
        if (lhs == true_lit() || rhs == true_lit()) {
            return true_lit();
        }
        if (lhs == false_lit()) {
            return rhs;
        }
        if (rhs == false_lit()) {
            return lhs;
        }
        if (lhs == rhs) {
            return lhs;
        }
        if (lhs == -rhs) {
            return true_lit();
        }
        const int out = sat_.new_var();
        sat_.add_clause({out, -lhs});
        sat_.add_clause({out, -rhs});
        sat_.add_clause({-out, lhs, rhs});
        return out;
    }

    int make_xor(int lhs, int rhs) {
        if (lhs == false_lit()) {
            return rhs;
        }
        if (rhs == false_lit()) {
            return lhs;
        }
        if (lhs == true_lit()) {
            return -rhs;
        }
        if (rhs == true_lit()) {
            return -lhs;
        }
        if (lhs == rhs) {
            return false_lit();
        }
        if (lhs == -rhs) {
            return true_lit();
        }
        const int out = sat_.new_var();
        sat_.add_clause({-out, -lhs, -rhs});
        sat_.add_clause({-out, lhs, rhs});
        sat_.add_clause({out, -lhs, rhs});
        sat_.add_clause({out, lhs, -rhs});
        return out;
    }

    int make_bool_equal(int lhs, int rhs) { return -make_xor(lhs, rhs); }

    void require_same_width(const std::vector<int> &lhs, const std::vector<int> &rhs) const {
        if (lhs.size() != rhs.size() || lhs.empty()) {
            throw std::invalid_argument("bit-vector operands must have the same non-zero width");
        }
    }

    bool literal_is_true(const SatSolver &sat, int lit) const {
        const int value = sat.value_of_var(std::abs(lit));
        if (value == -1) {
            return false;
        }
        return lit > 0 ? value == 1 : value == 0;
    }

    SatSolver &sat_;
    int true_lit_ = 0;
    std::unordered_map<const Node *, int> bool_cache_;
    std::unordered_map<const Node *, BVBits> bv_cache_;
    std::unordered_map<const Node *, std::optional<LinearForm>> linear_cache_;
    std::unordered_map<std::string, int> bool_vars_;
    std::unordered_map<std::string, std::vector<int>> bv_vars_;
    std::uint64_t bitblast_nodes_ = 0;
};

std::int64_t elapsed_us(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

} // namespace

void Model::set_bool(std::string name, bool value) { bool_values_[std::move(name)] = value; }

void Model::set_bit_vector(std::string name, std::uint32_t width, std::uint64_t value) {
    bit_vector_values_[std::move(name)] = BitVectorValue{width, value};
}

bool Model::bool_value(std::string_view name, bool &out) const {
    auto found = bool_values_.find(std::string(name));
    if (found == bool_values_.end()) {
        return false;
    }
    out = found->second;
    return true;
}

bool Model::bit_vector_value(std::string_view name, BitVectorValue &out) const {
    auto found = bit_vector_values_.find(std::string(name));
    if (found == bit_vector_values_.end()) {
        return false;
    }
    out = found->second;
    return true;
}

SolverResult Solver::check(const Expr &assertion, const SolverOptions &options) {
    return check(std::vector<Expr>{assertion}, options);
}

SolverResult Solver::check(const std::vector<Expr> &assertions, const SolverOptions &options) {
    const auto start = std::chrono::steady_clock::now();
    SolverResult result;
    try {
        SatSolver sat;
        BitBlaster bitblaster(sat);
        for (const auto &assertion : assertions) {
            if (!assertion.valid() || !assertion.sort().is_bool()) {
                result.status = CheckStatus::Unknown;
                result.reason = "all solver assertions must be Boolean";
                result.diagnostics.elapsed_us = elapsed_us(start);
                return result;
            }
            bitblaster.assert_lit(bitblaster.bool_expr(assertion));
        }
        const auto status = sat.solve({}, options);
        result.status = status;
        result.diagnostics.elapsed_us = elapsed_us(start);
        result.diagnostics.sat_variables = sat.variable_count();
        result.diagnostics.clauses = sat.clause_count();
        result.diagnostics.decisions = sat.decisions();
        result.diagnostics.conflicts = sat.conflicts();
        result.diagnostics.propagations = sat.propagations();
        result.diagnostics.bitblast_nodes = bitblaster.bitblast_nodes();
        if (status == CheckStatus::Sat) {
            bitblaster.extract_model(sat, result.model);
            result.reason = "satisfiable";
        } else if (status == CheckStatus::Unsat) {
            result.reason = "unsatisfiable";
        } else if (status == CheckStatus::Timeout) {
            result.reason = "resource limit exhausted";
        }
    } catch (const std::exception &error) {
        result.status = CheckStatus::Unknown;
        result.reason = error.what();
        result.diagnostics.elapsed_us = elapsed_us(start);
    }
    return result;
}

std::string to_string(CheckStatus status) {
    switch (status) {
    case CheckStatus::Sat:
        return "Sat";
    case CheckStatus::Unsat:
        return "Unsat";
    case CheckStatus::Unknown:
        return "Unknown";
    case CheckStatus::Timeout:
        return "Timeout";
    }
    return "Unknown";
}

} // namespace smt
