#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

class Expr;

namespace sema {

enum class SignedIntegerWidth : std::uint8_t {
    Bits32 = 32,
    Bits64 = 64,
};

enum class ConstantEvalError {
    None,
    InvalidLiteral,
    WidthMismatch,
    Overflow,
    DivisionByZero,
    NonPositive,
    OutOfRange,
    NonConstant,
    InvalidOperand,
};

std::string_view constant_eval_error_name(ConstantEvalError error);

class CheckedInteger final {
  public:
    static CheckedInteger i32(std::int32_t value) {
        return CheckedInteger(value, SignedIntegerWidth::Bits32);
    }

    static CheckedInteger i64(std::int64_t value) {
        return CheckedInteger(value, SignedIntegerWidth::Bits64);
    }

    std::int64_t value() const {
        return value_;
    }

    SignedIntegerWidth width() const {
        return width_;
    }

  private:
    CheckedInteger(std::int64_t value, SignedIntegerWidth width) : value_(value), width_(width) {
    }

    std::int64_t value_ = 0;
    SignedIntegerWidth width_ = SignedIntegerWidth::Bits32;
};

struct CheckedIntegerResult {
    std::optional<CheckedInteger> value;
    ConstantEvalError error = ConstantEvalError::None;
    std::string message;

    bool ok() const {
        return value.has_value() && error == ConstantEvalError::None;
    }
};

CheckedIntegerResult parse_signed_integer(std::string_view spelling, SignedIntegerWidth width);
CheckedIntegerResult checked_add(CheckedInteger lhs, CheckedInteger rhs);
CheckedIntegerResult checked_sub(CheckedInteger lhs, CheckedInteger rhs);
CheckedIntegerResult checked_mul(CheckedInteger lhs, CheckedInteger rhs);
CheckedIntegerResult checked_div(CheckedInteger lhs, CheckedInteger rhs);
CheckedIntegerResult checked_rem(CheckedInteger lhs, CheckedInteger rhs);
CheckedIntegerResult checked_neg(CheckedInteger value);

// Evaluate an AST integer constant expression without silently narrowing or
// invoking host-language signed overflow. The lookup callback exposes only
// previously checked integer constants; a missing name is non-constant.
using IntegerConstantLookup =
    std::function<std::optional<CheckedInteger>(std::string_view name)>;
CheckedIntegerResult evaluate_integer_constant(const Expr &expression,
                                                const IntegerConstantLookup &lookup = {});

enum class ExtentKind {
    LaneCount,
    ArrayBound,
};

struct CheckedExtentResult {
    std::optional<std::uint64_t> value;
    ConstantEvalError error = ConstantEvalError::None;
    std::string message;

    bool ok() const {
        return value.has_value() && error == ConstantEvalError::None;
    }
};

// Convert an already checked signed constant into a positive source extent.
CheckedExtentResult checked_positive_extent(CheckedInteger value, ExtentKind kind);

// Parse a decimal source extent without first narrowing through int64_t. This
// overload is used to diagnose values greater than UINT64_MAX precisely.
CheckedExtentResult parse_positive_extent(std::string_view spelling, ExtentKind kind);

} // namespace sema
