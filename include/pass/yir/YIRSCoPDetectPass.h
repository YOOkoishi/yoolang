#pragma once

#include "pass/PassManager.h"
#include <string_view>
#include <string>
#include <vector>
#include <unordered_set>

namespace yir {
class ForOp;
class Operation;
class Region;
class Value;
} // namespace yir

namespace pass {

struct YIRSCoPStatement {
    struct PathCondition {
        const yir::Value *condition = nullptr;
        bool negated = false;
    };

    std::size_t id;
    const yir::Operation *op;
    std::string op_name;
    std::vector<const yir::ForOp *> enclosing_loops;
    std::vector<PathCondition> path_conditions;
    // The statement is guarded by a condition that is pure but not affine.
    // Such predicates are conservatively omitted from the integer domain.
    bool has_opaque_conditions = false;
};

struct YIRSCoP {
    std::size_t id;
    const yir::Region *region;
    std::vector<YIRSCoPStatement> statements;
    std::unordered_set<const yir::Value *> symbols;
    bool has_opaque_conditions = false;
};

struct YIRSCoPInfo {
    std::vector<YIRSCoP> scops;
};

class YIRSCoPDetectPass final : public Pass {
  public:
    static constexpr std::string_view kArtifactKey = "YIRSCoPInfo";

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
