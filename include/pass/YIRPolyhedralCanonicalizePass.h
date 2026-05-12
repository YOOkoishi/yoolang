#pragma once

#include "PassManager.h"

#include <string_view>
#include <vector>

namespace yir {
class ForOp;
class Value;
} // namespace yir

namespace pass {

struct YIRPolyhedralLoopInfo {
    const yir::ForOp *loop = nullptr;
    std::vector<const yir::Value *> dimensions;
    std::vector<const yir::Value *> symbols;
};

struct YIRPolyhedralCanonicalInfo {
    std::vector<YIRPolyhedralLoopInfo> loops;
};

class YIRPolyhedralCanonicalizePass final : public Pass {
  public:
    static constexpr std::string_view kArtifactKey = "YIRPolyhedralCanonicalInfo";

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
