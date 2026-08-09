#pragma once

#include "oir/OIR.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pass::oir_fat {

enum class EligibilityCode : std::uint8_t {
    Eligible,
    InputVerificationFailed,
    VariadicUnsupported,
    NonScalarABIUnsupported,
    FunctionAddressUnsupported,
    IndirectCallUnsupported,
    ReservedSymbol,
    DuplicateSymbol,
    NoDefinitions,
};

std::string_view eligibility_code_name(EligibilityCode code);

struct DefinedFunction final {
    std::string name;
};

struct EligibilityResult final {
    bool eligible = false;
    EligibilityCode code = EligibilityCode::InputVerificationFailed;
    std::vector<DefinedFunction> defined_functions;
    std::string message;
};

EligibilityResult analyze_eligibility(const oir::Module &module);

namespace detail {

// Returns the data/bss object definitions produced only by the RVV branch.
// Public source globals must be byte-identical in both branches; a mismatch is
// rejected instead of silently selecting one branch's initializer.
bool extract_vector_only_globals(std::string_view scalar_assembly, std::string_view vector_assembly,
                                 std::string &output, std::string &error);

} // namespace detail

} // namespace pass::oir_fat
