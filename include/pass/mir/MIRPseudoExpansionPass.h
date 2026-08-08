#pragma once

#include "pass/PassManager.h"

#include <string>

namespace mir {
class Module;
}

namespace pass {

// Post-RA target expansion.  RVV pseudos are selected to concrete RVV 1.0
// opcodes here; unsupported shapes fail closed instead of leaking assembler
// macros or silently changing policy semantics.
bool expand_machine_pseudos(mir::Module &module, std::string &error);

class MIRPseudoExpansionPass final : public Pass {
  public:
    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
