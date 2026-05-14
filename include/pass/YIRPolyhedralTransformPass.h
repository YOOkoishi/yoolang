#pragma once

#include "PassManager.h"
#include <string_view>

namespace pass {

class YIRPolyhedralTransformPass final : public Pass {
  public:
    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
