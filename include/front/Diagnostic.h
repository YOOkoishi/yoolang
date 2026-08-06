#pragma once

#include "front/SourceLocation.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace front {

enum class DiagnosticSeverity {
    Note,
    Warning,
    Error,
};

// The textual spellings returned by diagnostic_code_name() are part of the
// diagnostic interface and are intentionally independent of message wording.
enum class DiagnosticCode {
    Unknown,
    LexUnexpectedCharacter,
    LexUnterminatedComment,
    LexInvalidIntegerLiteral,
    ParseExpectedToken,
    ParseUnexpectedToken,
    SemaInvalidType,
    SemaTypeMismatch,
    ConstInvalidLiteral,
    ConstWidthMismatch,
    ConstOverflow,
    ConstDivisionByZero,
    ConstNonPositive,
    ConstOutOfRange,
    LexIntegerLiteralOutOfRange,
    LexInvalidFloatLiteral,
    LexFloatLiteralOutOfRange,
    SemaUnknownSymbol,
    SemaRedefinition,
    SemaInvalidConstantExpression,
    SemaInvalidExtent,
    SemaInvalidOperand,
    SemaInvalidConversion,
    SemaArgumentMismatch,
    SemaInvalidInitializer,
    SemaInvalidCondition,
    SemaControlFlow,
    SemaInvalidSubscript,
    SemaLaneOutOfRange,
    SemaInvalidVectorLiteral,
    SemaVariadicAggregate,
    SemaInvalidShuffleIndex,
    SemaConflictingDeclaration,
};

std::string_view diagnostic_severity_name(DiagnosticSeverity severity);
std::string_view diagnostic_code_name(DiagnosticCode code);

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    DiagnosticCode code = DiagnosticCode::Unknown;
    SourceRange range;
    std::string message;
};

class DiagnosticEngine {
  public:
    const Diagnostic &report(DiagnosticSeverity severity, DiagnosticCode code, SourceRange range,
                             std::string message);

    const Diagnostic &note(DiagnosticCode code, SourceRange range, std::string message) {
        return report(DiagnosticSeverity::Note, code, range, std::move(message));
    }

    const Diagnostic &warning(DiagnosticCode code, SourceRange range, std::string message) {
        return report(DiagnosticSeverity::Warning, code, range, std::move(message));
    }

    const Diagnostic &error(DiagnosticCode code, SourceRange range, std::string message) {
        return report(DiagnosticSeverity::Error, code, range, std::move(message));
    }

    const std::vector<Diagnostic> &diagnostics() const {
        return diagnostics_;
    }

    bool has_error() const {
        return error_count_ != 0;
    }

    std::size_t error_count() const {
        return error_count_;
    }

    void clear();

  private:
    std::vector<Diagnostic> diagnostics_;
    std::size_t error_count_ = 0;
};

} // namespace front
