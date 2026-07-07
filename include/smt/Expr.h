#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace smt {

enum class SortKind {
    Bool,
    BitVector,
};

class Sort {
  public:
    static Sort bool_sort();
    static Sort bit_vector(std::uint32_t width);

    SortKind kind() const { return kind_; }
    bool is_bool() const { return kind_ == SortKind::Bool; }
    bool is_bit_vector() const { return kind_ == SortKind::BitVector; }
    std::uint32_t width() const { return width_; }
    bool operator==(const Sort &other) const;
    bool operator!=(const Sort &other) const { return !(*this == other); }

  private:
    Sort(SortKind kind, std::uint32_t width) : kind_(kind), width_(width) {}

    SortKind kind_ = SortKind::Bool;
    std::uint32_t width_ = 1;
};

enum class ExprKind {
    BoolConst,
    BVConst,
    Var,
    Not,
    And,
    Or,
    Xor,
    Implies,
    Equal,
    Distinct,
    BVNot,
    BVAnd,
    BVOr,
    BVXor,
    BVAdd,
    BVSub,
    BVNeg,
    BVUlt,
    BVUle,
    BVSlt,
    BVSle,
    BVShl,
    BVLshr,
    BVAshr,
    BVConcat,
    BVExtract,
};

struct Node;

class Expr {
  public:
    Expr() = default;

    bool valid() const { return static_cast<bool>(node_); }
    ExprKind kind() const;
    Sort sort() const;
    std::string_view name() const;
    bool bool_value() const;
    std::uint64_t bv_value() const;
    std::uint32_t shift_amount() const;
    std::uint32_t extract_high() const;
    std::uint32_t extract_low() const;
    const std::vector<Expr> &args() const;
    const Node *raw() const { return node_.get(); }

  private:
    explicit Expr(std::shared_ptr<const Node> node) : node_(std::move(node)) {}

    std::shared_ptr<const Node> node_;

    friend class ExprBuilder;
    friend std::string to_string(const Expr &expr);
};

struct Node {
    ExprKind kind = ExprKind::BoolConst;
    Sort sort = Sort::bool_sort();
    std::vector<Expr> args;
    std::string name;
    bool bool_value = false;
    std::uint64_t bv_value = 0;
    std::uint32_t shift_amount = 0;
    std::uint32_t extract_high = 0;
    std::uint32_t extract_low = 0;
    std::uint64_t id = 0;
};

class ExprBuilder {
  public:
    Expr bool_const(bool value);
    Expr bool_var(std::string name);

    Expr bv_const(std::uint32_t width, std::uint64_t value);
    Expr bv_var(std::uint32_t width, std::string name);

    Expr bool_not(const Expr &value);
    Expr bool_and(const Expr &lhs, const Expr &rhs);
    Expr bool_or(const Expr &lhs, const Expr &rhs);
    Expr bool_xor(const Expr &lhs, const Expr &rhs);
    Expr implies(const Expr &lhs, const Expr &rhs);

    Expr equal(const Expr &lhs, const Expr &rhs);
    Expr distinct(const Expr &lhs, const Expr &rhs);

    Expr bv_not(const Expr &value);
    Expr bv_and(const Expr &lhs, const Expr &rhs);
    Expr bv_or(const Expr &lhs, const Expr &rhs);
    Expr bv_xor(const Expr &lhs, const Expr &rhs);
    Expr bv_add(const Expr &lhs, const Expr &rhs);
    Expr bv_sub(const Expr &lhs, const Expr &rhs);
    Expr bv_neg(const Expr &value);

    Expr bv_ult(const Expr &lhs, const Expr &rhs);
    Expr bv_ule(const Expr &lhs, const Expr &rhs);
    Expr bv_slt(const Expr &lhs, const Expr &rhs);
    Expr bv_sle(const Expr &lhs, const Expr &rhs);

    Expr bv_shl(const Expr &value, std::uint32_t amount);
    Expr bv_lshr(const Expr &value, std::uint32_t amount);
    Expr bv_ashr(const Expr &value, std::uint32_t amount);
    Expr concat(const Expr &high, const Expr &low);
    Expr extract(const Expr &value, std::uint32_t high, std::uint32_t low);

  private:
    Expr make(ExprKind kind, Sort sort, std::vector<Expr> args = {});
    Expr make_shift(ExprKind kind, const Expr &value, std::uint32_t amount);
    void require_bool(const Expr &expr, std::string_view op) const;
    void require_bv(const Expr &expr, std::string_view op) const;
    void require_same_sort(const Expr &lhs, const Expr &rhs, std::string_view op) const;
    void require_same_bv_width(const Expr &lhs, const Expr &rhs, std::string_view op) const;

    std::uint64_t next_id_ = 1;
};

std::string to_string(const Sort &sort);
std::string to_string(ExprKind kind);
std::string to_string(const Expr &expr);

} // namespace smt
