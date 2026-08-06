#include "front/Diagnostic.h"

#include <utility>

namespace front {

std::string_view diagnostic_severity_name(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::Note:
        return "note";
    case DiagnosticSeverity::Warning:
        return "warning";
    case DiagnosticSeverity::Error:
        return "error";
    }
    return "error";
}

std::string_view diagnostic_code_name(DiagnosticCode code) {
    switch (code) {
    case DiagnosticCode::Unknown:
        return "FE0000";
    case DiagnosticCode::LexUnexpectedCharacter:
        return "LX0001";
    case DiagnosticCode::LexUnterminatedComment:
        return "LX0002";
    case DiagnosticCode::LexInvalidIntegerLiteral:
        return "LX0003";
    case DiagnosticCode::LexIntegerLiteralOutOfRange:
        return "LX0004";
    case DiagnosticCode::LexInvalidFloatLiteral:
        return "LX0005";
    case DiagnosticCode::LexFloatLiteralOutOfRange:
        return "LX0006";
    case DiagnosticCode::ParseExpectedToken:
        return "PA0001";
    case DiagnosticCode::ParseUnexpectedToken:
        return "PA0002";
    case DiagnosticCode::SemaInvalidType:
        return "SE0001";
    case DiagnosticCode::SemaTypeMismatch:
        return "SE0002";
    case DiagnosticCode::SemaUnknownSymbol:
        return "SE0003";
    case DiagnosticCode::SemaRedefinition:
        return "SE0004";
    case DiagnosticCode::SemaInvalidConstantExpression:
        return "SE0005";
    case DiagnosticCode::SemaInvalidExtent:
        return "SE0006";
    case DiagnosticCode::SemaInvalidOperand:
        return "SE0007";
    case DiagnosticCode::SemaInvalidConversion:
        return "SE0008";
    case DiagnosticCode::SemaArgumentMismatch:
        return "SE0009";
    case DiagnosticCode::SemaInvalidInitializer:
        return "SE0010";
    case DiagnosticCode::SemaInvalidCondition:
        return "SE0011";
    case DiagnosticCode::SemaControlFlow:
        return "SE0012";
    case DiagnosticCode::SemaInvalidSubscript:
        return "SE0013";
    case DiagnosticCode::SemaLaneOutOfRange:
        return "SE0014";
    case DiagnosticCode::SemaInvalidVectorLiteral:
        return "SE0015";
    case DiagnosticCode::SemaVariadicAggregate:
        return "SE0016";
    case DiagnosticCode::SemaInvalidShuffleIndex:
        return "SE0017";
    case DiagnosticCode::SemaConflictingDeclaration:
        return "SE0018";
    case DiagnosticCode::ConstInvalidLiteral:
        return "CE0001";
    case DiagnosticCode::ConstWidthMismatch:
        return "CE0002";
    case DiagnosticCode::ConstOverflow:
        return "CE0003";
    case DiagnosticCode::ConstDivisionByZero:
        return "CE0004";
    case DiagnosticCode::ConstNonPositive:
        return "CE0005";
    case DiagnosticCode::ConstOutOfRange:
        return "CE0006";
    }
    return "FE0000";
}

const Diagnostic &DiagnosticEngine::report(DiagnosticSeverity severity, DiagnosticCode code,
                                           SourceRange range, std::string message) {
    if (severity == DiagnosticSeverity::Error) {
        ++error_count_;
    }
    diagnostics_.push_back(Diagnostic{severity, code, range, std::move(message)});
    return diagnostics_.back();
}

void DiagnosticEngine::clear() {
    diagnostics_.clear();
    error_count_ = 0;
}

} // namespace front
