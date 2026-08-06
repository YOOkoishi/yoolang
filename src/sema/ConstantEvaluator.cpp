#include "sema/ConstantEvaluator.h"

#include "ast/ast.h"

#include <cctype>
#include <limits>
#include <utility>

namespace sema {
namespace {

const char *width_name(SignedIntegerWidth width) {
    return width == SignedIntegerWidth::Bits32 ? "32-bit" : "64-bit";
}

const char *extent_name(ExtentKind kind) {
    return kind == ExtentKind::LaneCount ? "lane count" : "array bound";
}

std::int64_t minimum(SignedIntegerWidth width) {
    return width == SignedIntegerWidth::Bits32
               ? static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min())
               : std::numeric_limits<std::int64_t>::min();
}

std::int64_t maximum(SignedIntegerWidth width) {
    return width == SignedIntegerWidth::Bits32
               ? static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())
               : std::numeric_limits<std::int64_t>::max();
}

CheckedInteger make_integer(std::int64_t value, SignedIntegerWidth width) {
    return width == SignedIntegerWidth::Bits32
               ? CheckedInteger::i32(static_cast<std::int32_t>(value))
               : CheckedInteger::i64(value);
}

CheckedIntegerResult success(std::int64_t value, SignedIntegerWidth width) {
    return CheckedIntegerResult{make_integer(value, width), ConstantEvalError::None, {}};
}

CheckedIntegerResult failure(ConstantEvalError error, std::string message) {
    return CheckedIntegerResult{std::nullopt, error, std::move(message)};
}

CheckedIntegerResult width_failure() {
    return failure(ConstantEvalError::WidthMismatch,
                   "checked integer operands must have the same signed width");
}

CheckedIntegerResult overflow_failure(SignedIntegerWidth width, const char *operation) {
    return failure(ConstantEvalError::Overflow,
                   std::string("signed ") + width_name(width) + " " + operation + " overflow");
}

bool same_width(CheckedInteger lhs, CheckedInteger rhs) {
    return lhs.width() == rhs.width();
}

CheckedIntegerResult checked_wide_result(__int128 value, SignedIntegerWidth width,
                                         const char *operation) {
    if (value < static_cast<__int128>(minimum(width)) ||
        value > static_cast<__int128>(maximum(width))) {
        return overflow_failure(width, operation);
    }
    return success(static_cast<std::int64_t>(value), width);
}

bool is_decimal_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

CheckedIntegerResult evaluate_impl(const Expr &expression,
                                   const IntegerConstantLookup &lookup) {
    if (const auto *literal = dynamic_cast<const IntLiteral *>(&expression)) {
        return success(literal->value, SignedIntegerWidth::Bits32);
    }
    if (dynamic_cast<const FloatLiteral *>(&expression) != nullptr) {
        return failure(ConstantEvalError::InvalidOperand,
                       "floating value is not an integer constant expression");
    }
    if (const auto *lvalue = dynamic_cast<const LValExpr *>(&expression)) {
        if (!lvalue->indices.empty()) {
            return failure(ConstantEvalError::NonConstant,
                           "subscripted value is not an integer constant expression");
        }
        if (lookup) {
            if (auto value = lookup(lvalue->name)) {
                return CheckedIntegerResult{*value, ConstantEvalError::None, {}};
            }
        }
        return failure(ConstantEvalError::NonConstant,
                       "'" + lvalue->name + "' is not a checked integer constant");
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expression)) {
        auto operand = evaluate_impl(*unary->operand, lookup);
        if (!operand.ok()) {
            return operand;
        }
        switch (unary->op) {
        case UnaryOp::Pos:
            return operand;
        case UnaryOp::Neg:
            return checked_neg(*operand.value);
        case UnaryOp::Not:
            return success(operand.value->value() == 0 ? 1 : 0, SignedIntegerWidth::Bits32);
        case UnaryOp::BitNot:
            if (operand.value->width() == SignedIntegerWidth::Bits32) {
                return success(static_cast<std::int32_t>(
                                   ~static_cast<std::int32_t>(operand.value->value())),
                               SignedIntegerWidth::Bits32);
            }
            return success(~operand.value->value(), SignedIntegerWidth::Bits64);
        }
    }
    if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expression)) {
        auto lhs = evaluate_impl(*binary->lhs, lookup);
        if (!lhs.ok()) {
            return lhs;
        }
        if (binary->op == BinaryOp::And && lhs.value->value() == 0) {
            return success(0, SignedIntegerWidth::Bits32);
        }
        if (binary->op == BinaryOp::Or && lhs.value->value() != 0) {
            return success(1, SignedIntegerWidth::Bits32);
        }
        auto rhs = evaluate_impl(*binary->rhs, lookup);
        if (!rhs.ok()) {
            return rhs;
        }
        switch (binary->op) {
        case BinaryOp::Add:
            return checked_add(*lhs.value, *rhs.value);
        case BinaryOp::Sub:
            return checked_sub(*lhs.value, *rhs.value);
        case BinaryOp::Mul:
            return checked_mul(*lhs.value, *rhs.value);
        case BinaryOp::Div:
            return checked_div(*lhs.value, *rhs.value);
        case BinaryOp::Mod:
            return checked_rem(*lhs.value, *rhs.value);
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge:
        case BinaryOp::Eq:
        case BinaryOp::Ne:
        case BinaryOp::BitAnd:
        case BinaryOp::BitXor:
        case BinaryOp::BitOr:
        case BinaryOp::And:
        case BinaryOp::Or:
            break;
        }
        if (lhs.value->width() != rhs.value->width()) {
            return width_failure();
        }
        const auto left = lhs.value->value();
        const auto right = rhs.value->value();
        switch (binary->op) {
        case BinaryOp::Lt:
            return success(left < right, SignedIntegerWidth::Bits32);
        case BinaryOp::Le:
            return success(left <= right, SignedIntegerWidth::Bits32);
        case BinaryOp::Gt:
            return success(left > right, SignedIntegerWidth::Bits32);
        case BinaryOp::Ge:
            return success(left >= right, SignedIntegerWidth::Bits32);
        case BinaryOp::Eq:
            return success(left == right, SignedIntegerWidth::Bits32);
        case BinaryOp::Ne:
            return success(left != right, SignedIntegerWidth::Bits32);
        case BinaryOp::And:
            return success(left != 0 && right != 0, SignedIntegerWidth::Bits32);
        case BinaryOp::Or:
            return success(left != 0 || right != 0, SignedIntegerWidth::Bits32);
        case BinaryOp::BitAnd:
            return success(left & right, lhs.value->width());
        case BinaryOp::BitXor:
            return success(left ^ right, lhs.value->width());
        case BinaryOp::BitOr:
            return success(left | right, lhs.value->width());
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
            break;
        }
    }
    return failure(ConstantEvalError::NonConstant,
                   "expression is not an integer constant expression");
}

} // namespace

std::string_view constant_eval_error_name(ConstantEvalError error) {
    switch (error) {
    case ConstantEvalError::None:
        return "none";
    case ConstantEvalError::InvalidLiteral:
        return "invalid-literal";
    case ConstantEvalError::WidthMismatch:
        return "width-mismatch";
    case ConstantEvalError::Overflow:
        return "overflow";
    case ConstantEvalError::DivisionByZero:
        return "division-by-zero";
    case ConstantEvalError::NonPositive:
        return "non-positive";
    case ConstantEvalError::OutOfRange:
        return "out-of-range";
    case ConstantEvalError::NonConstant:
        return "non-constant";
    case ConstantEvalError::InvalidOperand:
        return "invalid-operand";
    }
    return "invalid-literal";
}

CheckedIntegerResult parse_signed_integer(std::string_view spelling, SignedIntegerWidth width) {
    if (spelling.empty()) {
        return failure(ConstantEvalError::InvalidLiteral, "empty integer literal");
    }

    std::size_t cursor = 0;
    bool negative = false;
    if (spelling[cursor] == '+' || spelling[cursor] == '-') {
        negative = spelling[cursor] == '-';
        ++cursor;
    }
    if (cursor == spelling.size()) {
        return failure(ConstantEvalError::InvalidLiteral, "integer literal has no digits");
    }

    const std::uint64_t positive_limit =
        width == SignedIntegerWidth::Bits32
            ? static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())
            : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const std::uint64_t negative_limit = positive_limit + 1;
    const std::uint64_t limit = negative ? negative_limit : positive_limit;

    std::uint64_t magnitude = 0;
    for (; cursor < spelling.size(); ++cursor) {
        const char ch = spelling[cursor];
        if (!is_decimal_digit(ch)) {
            return failure(ConstantEvalError::InvalidLiteral,
                           "integer literal contains a non-decimal digit");
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (magnitude > (limit - digit) / 10) {
            return overflow_failure(width, "literal");
        }
        magnitude = magnitude * 10 + digit;
    }

    std::int64_t value = 0;
    if (negative) {
        if (magnitude == negative_limit) {
            value = minimum(width);
        } else {
            value = -static_cast<std::int64_t>(magnitude);
        }
    } else {
        value = static_cast<std::int64_t>(magnitude);
    }
    return success(value, width);
}

CheckedIntegerResult checked_add(CheckedInteger lhs, CheckedInteger rhs) {
    if (!same_width(lhs, rhs)) {
        return width_failure();
    }
    return checked_wide_result(static_cast<__int128>(lhs.value()) + rhs.value(), lhs.width(),
                               "addition");
}

CheckedIntegerResult checked_sub(CheckedInteger lhs, CheckedInteger rhs) {
    if (!same_width(lhs, rhs)) {
        return width_failure();
    }
    return checked_wide_result(static_cast<__int128>(lhs.value()) - rhs.value(), lhs.width(),
                               "subtraction");
}

CheckedIntegerResult checked_mul(CheckedInteger lhs, CheckedInteger rhs) {
    if (!same_width(lhs, rhs)) {
        return width_failure();
    }
    return checked_wide_result(static_cast<__int128>(lhs.value()) * rhs.value(), lhs.width(),
                               "multiplication");
}

CheckedIntegerResult checked_div(CheckedInteger lhs, CheckedInteger rhs) {
    if (!same_width(lhs, rhs)) {
        return width_failure();
    }
    if (rhs.value() == 0) {
        return failure(ConstantEvalError::DivisionByZero,
                       "division by zero in integer constant expression");
    }
    if (lhs.value() == minimum(lhs.width()) && rhs.value() == -1) {
        return overflow_failure(lhs.width(), "division");
    }
    return success(lhs.value() / rhs.value(), lhs.width());
}

CheckedIntegerResult checked_rem(CheckedInteger lhs, CheckedInteger rhs) {
    if (!same_width(lhs, rhs)) {
        return width_failure();
    }
    if (rhs.value() == 0) {
        return failure(ConstantEvalError::DivisionByZero,
                       "remainder by zero in integer constant expression");
    }
    if (lhs.value() == minimum(lhs.width()) && rhs.value() == -1) {
        return overflow_failure(lhs.width(), "remainder");
    }
    return success(lhs.value() % rhs.value(), lhs.width());
}

CheckedIntegerResult checked_neg(CheckedInteger value) {
    if (value.value() == minimum(value.width())) {
        return overflow_failure(value.width(), "negation");
    }
    return success(-value.value(), value.width());
}

CheckedIntegerResult evaluate_integer_constant(const Expr &expression,
                                                const IntegerConstantLookup &lookup) {
    return evaluate_impl(expression, lookup);
}

CheckedExtentResult checked_positive_extent(CheckedInteger value, ExtentKind kind) {
    if (value.value() <= 0) {
        return CheckedExtentResult{std::nullopt, ConstantEvalError::NonPositive,
                                   std::string(extent_name(kind)) +
                                       " must be a strictly positive integer"};
    }
    return CheckedExtentResult{
        static_cast<std::uint64_t>(value.value()), ConstantEvalError::None, {}};
}

CheckedExtentResult parse_positive_extent(std::string_view spelling, ExtentKind kind) {
    if (spelling.empty()) {
        return CheckedExtentResult{std::nullopt, ConstantEvalError::InvalidLiteral,
                                   std::string(extent_name(kind)) + " is empty"};
    }

    std::size_t cursor = 0;
    bool negative = false;
    if (spelling[cursor] == '+' || spelling[cursor] == '-') {
        negative = spelling[cursor] == '-';
        ++cursor;
    }
    if (cursor == spelling.size()) {
        return CheckedExtentResult{std::nullopt, ConstantEvalError::InvalidLiteral,
                                   std::string(extent_name(kind)) + " has no digits"};
    }
    for (std::size_t i = cursor; i < spelling.size(); ++i) {
        if (!is_decimal_digit(spelling[i])) {
            return CheckedExtentResult{std::nullopt, ConstantEvalError::InvalidLiteral,
                                       std::string(extent_name(kind)) +
                                           " contains a non-decimal digit"};
        }
    }
    if (negative) {
        return CheckedExtentResult{std::nullopt, ConstantEvalError::NonPositive,
                                   std::string(extent_name(kind)) +
                                       " must be a strictly positive integer"};
    }

    std::uint64_t value = 0;
    constexpr std::uint64_t limit = std::numeric_limits<std::uint64_t>::max();
    for (; cursor < spelling.size(); ++cursor) {
        const std::uint64_t digit = static_cast<std::uint64_t>(spelling[cursor] - '0');
        if (value > (limit - digit) / 10) {
            return CheckedExtentResult{std::nullopt, ConstantEvalError::OutOfRange,
                                       std::string(extent_name(kind)) + " exceeds uint64 range"};
        }
        value = value * 10 + digit;
    }
    if (value == 0) {
        return CheckedExtentResult{std::nullopt, ConstantEvalError::NonPositive,
                                   std::string(extent_name(kind)) +
                                       " must be a strictly positive integer"};
    }
    return CheckedExtentResult{value, ConstantEvalError::None, {}};
}

} // namespace sema
