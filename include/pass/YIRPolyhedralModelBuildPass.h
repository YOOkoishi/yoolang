#pragma once

#include "PassManager.h"
#include <string_view>
#include <vector>
#include <string>
#include <unordered_map>

namespace yir {
class Value;
class Operation;
} // namespace yir

namespace pass {

struct PolyAccess {
    enum class Kind { Read, Write };
    Kind kind;
    const yir::Value *memory;
    // We will just store string text expressions of relation for now
    std::string relation_str; 
};

struct PolyStmt {
    std::size_t id;
    const yir::Operation *op;
    std::vector<const yir::Value *> dims;
    
    std::string domain_str;
    std::vector<PolyAccess> reads;
    std::vector<PolyAccess> writes;
    std::string schedule_str;
};

struct PolyScop {
    std::size_t id;
    std::vector<const yir::Value *> params;
    std::vector<PolyStmt> statements;
};

struct PolyModelInfo {
    std::vector<PolyScop> models;
};

class YIRPolyhedralModelBuildPass final : public Pass {
  public:
    static constexpr std::string_view kArtifactKey = "PolyModelInfo";

    std::string_view name() const override;
    PassKind kind() const override;
    PassResult run(PassContext &context) override;
};

} // namespace pass
