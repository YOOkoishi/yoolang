#pragma once

#include "pass/PassManager.h"

#include <string_view>
#include <unordered_set>
#include <vector>

namespace yir {
class ForOp;
class Function;
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

// In Auto mode, only functions selected by the profitability pre-scan are
// allowed to receive polyhedral canonicalization rewrites. Force mode leaves
// this artifact absent and preserves the historical whole-module behavior.
struct YIRPolyhedralFunctionSelection {
    std::unordered_set<const yir::Function *> functions;

    static constexpr std::string_view kArtifactKey = "YIRPolyhedralFunctionSelection";
};

class YIRPolyhedralCanonicalizePass final : public Pass {
  public:
    static constexpr std::string_view kArtifactKey = "YIRPolyhedralCanonicalInfo";

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
