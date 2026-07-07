#include "smt/Expr.h"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace smt {
namespace {

constexpr std::uint32_t kMaxBVWidth = 64;

std::uint64_t mask_for_width(std::uint32_t width) {
    if (width >= 64) {
        return ~0ULL;
    }
    return (1ULL << width) - 1ULL;
}

void require_valid(const Expr &expr, std::string_view op) {
    if (!expr.valid()) {
        throw std::invalid_argument(std::string(op) + " received an invalid SMT expression");
    }
}

void require_width(std::uint32_t width, std::string_view op) {
    if (width == 0 || width > kMaxBVWidth) {
        throw std::invalid_argument(std::string(op) + " requires a bit-vector width in 1..64");
    }
}

std::string render(const Expr &expr) {
    if (!expr.valid()) {
        return "<invalid>";
    }
    const auto &node = *expr.raw();
    switch (node.kind) {
    case ExprKind::BoolConst:
        return node.bool_value ? "true" : "false";
    case ExprKind::BVConst: {
        std::ostringstream out;
        out << "(_ bv" << node.bv_value << " " << node.sort.width() << ")";
        return out.str();
    }
    case ExprKind::Var:
        return node.name;
    case ExprKind::Not:
        return "(not " + render(node.args[0]) + ")";
    case ExprKind::And:
        return "(and " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::Or:
        return "(or " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::Xor:
        return "(xor " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::Implies:
        return "(=> " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::Equal:
        return "(= " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::Distinct:
        return "(distinct " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::BVNot:
        return "(bvnot " + render(node.args[0]) + ")";
    case ExprKind::BVAnd:
        return "(bvand " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::BVOr:
        return "(bvor " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::BVXor:
        return "(bvxor " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::BVAdd:
        return "(bvadd " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::BVSub:
        return "(bvsub " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::BVNeg:
        return "(bvneg " + render(node.args[0]) + ")";
    case ExprKind::BVUlt:
        return "(bvult " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::BVUle:
        return "(bvule " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::BVSlt:
        return "(bvslt " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::BVSle:
        return "(bvsle " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::BVShl:
        return "((_ bvshl " + std::to_string(node.shift_amount) + ") " +
               render(node.args[0]) + ")";
    case ExprKind::BVLshr:
        return "((_ bvlshr " + std::to_string(node.shift_amount) + ") " +
               render(node.args[0]) + ")";
    case ExprKind::BVAshr:
        return "((_ bvashr " + std::to_string(node.shift_amount) + ") " +
               render(node.args[0]) + ")";
    case ExprKind::BVConcat:
        return "(concat " + render(node.args[0]) + " " + render(node.args[1]) + ")";
    case ExprKind::BVExtract:
        return "((_ extract " + std::to_string(node.extract_high) + " " +
               std::to_string(node.extract_low) + ") " + render(node.args[0]) + ")";
    }
    return "<unknown>";
}

} // namespace

Sort Sort::bool_sort() { return Sort(SortKind::Bool, 1); }

Sort Sort::bit_vector(std::uint32_t width) {
    require_width(width, "bit_vector");
    return Sort(SortKind::BitVector, width);
}

bool Sort::operator==(const Sort &other) const {
    return kind_ == other.kind_ && width_ == other.width_;
}

ExprKind Expr::kind() const {
    require_valid(*this, "kind");
    return node_->kind;
}

Sort Expr::sort() const {
    require_valid(*this, "sort");
    return node_->sort;
}

std::string_view Expr::name() const {
    require_valid(*this, "name");
    return node_->name;
}

bool Expr::bool_value() const {
    require_valid(*this, "bool_value");
    return node_->bool_value;
}

std::uint64_t Expr::bv_value() const {
    require_valid(*this, "bv_value");
    return node_->bv_value;
}

std::uint32_t Expr::shift_amount() const {
    require_valid(*this, "shift_amount");
    return node_->shift_amount;
}

std::uint32_t Expr::extract_high() const {
    require_valid(*this, "extract_high");
    return node_->extract_high;
}

std::uint32_t Expr::extract_low() const {
    require_valid(*this, "extract_low");
    return node_->extract_low;
}

const std::vector<Expr> &Expr::args() const {
    require_valid(*this, "args");
    return node_->args;
}

Expr ExprBuilder::bool_const(bool value) {
    auto expr = make(ExprKind::BoolConst, Sort::bool_sort());
    auto node = std::const_pointer_cast<Node>(expr.node_);
    node->bool_value = value;
    return expr;
}

Expr ExprBuilder::bool_var(std::string name) {
    if (name.empty()) {
        throw std::invalid_argument("bool_var requires a non-empty name");
    }
    auto expr = make(ExprKind::Var, Sort::bool_sort());
    auto node = std::const_pointer_cast<Node>(expr.node_);
    node->name = std::move(name);
    return expr;
}

Expr ExprBuilder::bv_const(std::uint32_t width, std::uint64_t value) {
    require_width(width, "bv_const");
    auto expr = make(ExprKind::BVConst, Sort::bit_vector(width));
    auto node = std::const_pointer_cast<Node>(expr.node_);
    node->bv_value = value & mask_for_width(width);
    return expr;
}

Expr ExprBuilder::bv_var(std::uint32_t width, std::string name) {
    require_width(width, "bv_var");
    if (name.empty()) {
        throw std::invalid_argument("bv_var requires a non-empty name");
    }
    auto expr = make(ExprKind::Var, Sort::bit_vector(width));
    auto node = std::const_pointer_cast<Node>(expr.node_);
    node->name = std::move(name);
    return expr;
}

Expr ExprBuilder::bool_not(const Expr &value) {
    require_bool(value, "bool_not");
    return make(ExprKind::Not, Sort::bool_sort(), {value});
}

Expr ExprBuilder::bool_and(const Expr &lhs, const Expr &rhs) {
    require_bool(lhs, "bool_and");
    require_bool(rhs, "bool_and");
    return make(ExprKind::And, Sort::bool_sort(), {lhs, rhs});
}

Expr ExprBuilder::bool_or(const Expr &lhs, const Expr &rhs) {
    require_bool(lhs, "bool_or");
    require_bool(rhs, "bool_or");
    return make(ExprKind::Or, Sort::bool_sort(), {lhs, rhs});
}

Expr ExprBuilder::bool_xor(const Expr &lhs, const Expr &rhs) {
    require_bool(lhs, "bool_xor");
    require_bool(rhs, "bool_xor");
    return make(ExprKind::Xor, Sort::bool_sort(), {lhs, rhs});
}

Expr ExprBuilder::implies(const Expr &lhs, const Expr &rhs) {
    require_bool(lhs, "implies");
    require_bool(rhs, "implies");
    return make(ExprKind::Implies, Sort::bool_sort(), {lhs, rhs});
}

Expr ExprBuilder::equal(const Expr &lhs, const Expr &rhs) {
    require_same_sort(lhs, rhs, "equal");
    return make(ExprKind::Equal, Sort::bool_sort(), {lhs, rhs});
}

Expr ExprBuilder::distinct(const Expr &lhs, const Expr &rhs) {
    require_same_sort(lhs, rhs, "distinct");
    return make(ExprKind::Distinct, Sort::bool_sort(), {lhs, rhs});
}

Expr ExprBuilder::bv_not(const Expr &value) {
    require_bv(value, "bv_not");
    return make(ExprKind::BVNot, value.sort(), {value});
}

Expr ExprBuilder::bv_and(const Expr &lhs, const Expr &rhs) {
    require_same_bv_width(lhs, rhs, "bv_and");
    return make(ExprKind::BVAnd, lhs.sort(), {lhs, rhs});
}

Expr ExprBuilder::bv_or(const Expr &lhs, const Expr &rhs) {
    require_same_bv_width(lhs, rhs, "bv_or");
    return make(ExprKind::BVOr, lhs.sort(), {lhs, rhs});
}

Expr ExprBuilder::bv_xor(const Expr &lhs, const Expr &rhs) {
    require_same_bv_width(lhs, rhs, "bv_xor");
    return make(ExprKind::BVXor, lhs.sort(), {lhs, rhs});
}

Expr ExprBuilder::bv_add(const Expr &lhs, const Expr &rhs) {
    require_same_bv_width(lhs, rhs, "bv_add");
    return make(ExprKind::BVAdd, lhs.sort(), {lhs, rhs});
}

Expr ExprBuilder::bv_sub(const Expr &lhs, const Expr &rhs) {
    require_same_bv_width(lhs, rhs, "bv_sub");
    return make(ExprKind::BVSub, lhs.sort(), {lhs, rhs});
}

Expr ExprBuilder::bv_neg(const Expr &value) {
    require_bv(value, "bv_neg");
    return make(ExprKind::BVNeg, value.sort(), {value});
}

Expr ExprBuilder::bv_ult(const Expr &lhs, const Expr &rhs) {
    require_same_bv_width(lhs, rhs, "bv_ult");
    return make(ExprKind::BVUlt, Sort::bool_sort(), {lhs, rhs});
}

Expr ExprBuilder::bv_ule(const Expr &lhs, const Expr &rhs) {
    require_same_bv_width(lhs, rhs, "bv_ule");
    return make(ExprKind::BVUle, Sort::bool_sort(), {lhs, rhs});
}

Expr ExprBuilder::bv_slt(const Expr &lhs, const Expr &rhs) {
    require_same_bv_width(lhs, rhs, "bv_slt");
    return make(ExprKind::BVSlt, Sort::bool_sort(), {lhs, rhs});
}

Expr ExprBuilder::bv_sle(const Expr &lhs, const Expr &rhs) {
    require_same_bv_width(lhs, rhs, "bv_sle");
    return make(ExprKind::BVSle, Sort::bool_sort(), {lhs, rhs});
}

Expr ExprBuilder::bv_shl(const Expr &value, std::uint32_t amount) {
    return make_shift(ExprKind::BVShl, value, amount);
}

Expr ExprBuilder::bv_lshr(const Expr &value, std::uint32_t amount) {
    return make_shift(ExprKind::BVLshr, value, amount);
}

Expr ExprBuilder::bv_ashr(const Expr &value, std::uint32_t amount) {
    return make_shift(ExprKind::BVAshr, value, amount);
}

Expr ExprBuilder::concat(const Expr &high, const Expr &low) {
    require_bv(high, "concat");
    require_bv(low, "concat");
    const auto width = high.sort().width() + low.sort().width();
    require_width(width, "concat");
    return make(ExprKind::BVConcat, Sort::bit_vector(width), {high, low});
}

Expr ExprBuilder::extract(const Expr &value, std::uint32_t high, std::uint32_t low) {
    require_bv(value, "extract");
    if (low > high || high >= value.sort().width()) {
        throw std::invalid_argument("extract requires 0 <= low <= high < width");
    }
    auto expr = make(ExprKind::BVExtract, Sort::bit_vector(high - low + 1), {value});
    auto node = std::const_pointer_cast<Node>(expr.node_);
    node->extract_high = high;
    node->extract_low = low;
    return expr;
}

Expr ExprBuilder::make(ExprKind kind, Sort sort, std::vector<Expr> args) {
    auto node = std::make_shared<Node>();
    node->kind = kind;
    node->sort = sort;
    node->args = std::move(args);
    node->id = next_id_++;
    return Expr(std::move(node));
}

Expr ExprBuilder::make_shift(ExprKind kind, const Expr &value, std::uint32_t amount) {
    require_bv(value, "shift");
    auto expr = make(kind, value.sort(), {value});
    auto node = std::const_pointer_cast<Node>(expr.node_);
    node->shift_amount = amount;
    return expr;
}

void ExprBuilder::require_bool(const Expr &expr, std::string_view op) const {
    require_valid(expr, op);
    if (!expr.sort().is_bool()) {
        throw std::invalid_argument(std::string(op) + " requires a Boolean expression");
    }
}

void ExprBuilder::require_bv(const Expr &expr, std::string_view op) const {
    require_valid(expr, op);
    if (!expr.sort().is_bit_vector()) {
        throw std::invalid_argument(std::string(op) + " requires a bit-vector expression");
    }
}

void ExprBuilder::require_same_sort(const Expr &lhs, const Expr &rhs,
                                    std::string_view op) const {
    require_valid(lhs, op);
    require_valid(rhs, op);
    if (lhs.sort() != rhs.sort()) {
        throw std::invalid_argument(std::string(op) + " requires matching sorts");
    }
}

void ExprBuilder::require_same_bv_width(const Expr &lhs, const Expr &rhs,
                                        std::string_view op) const {
    require_same_sort(lhs, rhs, op);
    if (!lhs.sort().is_bit_vector()) {
        throw std::invalid_argument(std::string(op) + " requires bit-vector operands");
    }
}

std::string to_string(const Sort &sort) {
    if (sort.is_bool()) {
        return "Bool";
    }
    return "(_ BitVec " + std::to_string(sort.width()) + ")";
}

std::string to_string(ExprKind kind) {
    switch (kind) {
    case ExprKind::BoolConst:
        return "BoolConst";
    case ExprKind::BVConst:
        return "BVConst";
    case ExprKind::Var:
        return "Var";
    case ExprKind::Not:
        return "Not";
    case ExprKind::And:
        return "And";
    case ExprKind::Or:
        return "Or";
    case ExprKind::Xor:
        return "Xor";
    case ExprKind::Implies:
        return "Implies";
    case ExprKind::Equal:
        return "Equal";
    case ExprKind::Distinct:
        return "Distinct";
    case ExprKind::BVNot:
        return "BVNot";
    case ExprKind::BVAnd:
        return "BVAnd";
    case ExprKind::BVOr:
        return "BVOr";
    case ExprKind::BVXor:
        return "BVXor";
    case ExprKind::BVAdd:
        return "BVAdd";
    case ExprKind::BVSub:
        return "BVSub";
    case ExprKind::BVNeg:
        return "BVNeg";
    case ExprKind::BVUlt:
        return "BVUlt";
    case ExprKind::BVUle:
        return "BVUle";
    case ExprKind::BVSlt:
        return "BVSlt";
    case ExprKind::BVSle:
        return "BVSle";
    case ExprKind::BVShl:
        return "BVShl";
    case ExprKind::BVLshr:
        return "BVLshr";
    case ExprKind::BVAshr:
        return "BVAshr";
    case ExprKind::BVConcat:
        return "BVConcat";
    case ExprKind::BVExtract:
        return "BVExtract";
    }
    return "Unknown";
}

std::string to_string(const Expr &expr) { return render(expr); }

} // namespace smt
