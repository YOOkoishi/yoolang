#pragma once

#include "PassManager.h"

#include <string>
#include <string_view>

namespace pass {

class MIRToAsmPass final : public Pass {
  public:
    static constexpr const char *kArtifactKey = "mir.asm";

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
