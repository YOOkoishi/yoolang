#pragma once

#include "PassManager.h"
#include <string_view>
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>

namespace yir {
class Value;
class Operation;
} // namespace yir

namespace pass {

// A simple affine expression: c0*v0 + c1*v1 + ... + constant
struct PolyAffineExpr {
    std::int64_t constant = 0;
    std::vector<std::pair<const yir::Value*, std::int64_t>> terms;
    bool valid = true;
};

struct PolyLoopBound {
    const yir::Value* iv; // Induction variable
    PolyAffineExpr lower;
    PolyAffineExpr upper;
};

struct PolyAccess {
    enum class Kind { Read, Write };
    Kind kind;
    const yir::Value *memory;
    std::vector<PolyAffineExpr> indices; // Representation for A[expr1][expr2]
};

struct PolyStmt {
    std::size_t id;
    const yir::Operation *op;
    std::vector<const yir::Value *> dims; // The nested loops surrounding this stmt
    
    std::vector<PolyLoopBound> domain;
    std::vector<PolyAccess> reads;
    std::vector<PolyAccess> writes;
    
    // Schedule: simply depth -> index, e.g., [i, j, stmt_lexical_id]
    std::vector<const yir::Value *> schedule_dims;
    std::size_t lexical_id;
};

struct PolyScop {
    std::size_t id;
    std::vector<const yir::Value *> params; // Loop invariants/symbols
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
