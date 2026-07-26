#pragma once

#include "pass/PassManager.h"
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

enum class PolyPredicate { Eq, Ne, Lt, Le, Gt, Ge };

struct PolyDomainConstraint {
    PolyAffineExpr lhs;
    PolyAffineExpr rhs;
    PolyPredicate predicate = PolyPredicate::Eq;
};

struct PolyDomain {
    std::vector<const yir::Value *> dims;
    std::vector<const yir::Value *> params;
    std::vector<PolyDomainConstraint> constraints;
    bool valid = true;
};

struct PolyAffineSchedule {
    std::vector<const yir::Value *> input_dims;
    std::vector<PolyAffineExpr> output_dims;
    std::size_t lexical_order = 0;
    bool valid = true;
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
    std::string op_name;
    std::vector<const yir::Value *> dims; // The nested loops surrounding this stmt
    
    // Compatibility summary used by the existing specialized transforms.
    std::vector<PolyLoopBound> loop_bounds;
    PolyDomain domain;
    std::vector<PolyAccess> reads;
    std::vector<PolyAccess> writes;
    
    // Compatibility loop-IV summary used by the existing specialized transforms.
    std::vector<const yir::Value *> schedule_dims;
    PolyAffineSchedule schedule;
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
