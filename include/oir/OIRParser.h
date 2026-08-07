#pragma once

#include "oir/OIR.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oir {

struct OIRSourcePosition final {
    std::size_t offset = 0;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct OIRSourceRange final {
    OIRSourcePosition begin;
    OIRSourcePosition end;
};

struct OIRParseError final {
    OIRSourceRange range;
    std::string message;
};

struct OIRParseResult final {
    std::unique_ptr<Module> module;
    std::vector<OIRParseError> errors;

    bool ok() const;
};

class OIRParser final {
  public:
    // This boundary is noexcept by design: malformed input and failures from
    // strict IR constructors are converted into source-ranged diagnostics.
    static OIRParseResult parse(std::string_view source,
                                std::string source_name = "<memory>") noexcept;
};

} // namespace oir
