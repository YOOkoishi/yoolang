#pragma once

#include "pass/PassManager.h"
#include <string_view>
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>
#include <memory>

namespace yir {
class Value;
class Operation;
} // namespace yir

namespace pass {

// MLIR-like affine expressions. Linear expressions keep the compact form used
// by the existing dependence code; quasi-affine and composite nodes preserve
// floor/ceil/mod arithmetic and affine combinations until the relation builder
// lowers them to local integer constraints.
enum class PolyAffineExprKind { Linear, FloorDiv, CeilDiv, Mod, Add, Sub, Mul };

struct PolyAffineExpr {
    PolyAffineExprKind kind = PolyAffineExprKind::Linear;
    std::int64_t constant = 0;
    std::vector<std::pair<const yir::Value*, std::int64_t>> terms;
    std::shared_ptr<PolyAffineExpr> operand;
    std::vector<std::shared_ptr<PolyAffineExpr>> operands;
    std::int64_t divisor = 0;
    bool valid = true;

    bool is_linear() const { return valid && kind == PolyAffineExprKind::Linear; }
    bool is_quasi_affine() const {
        return valid && kind != PolyAffineExprKind::Linear && operand != nullptr &&
               (kind == PolyAffineExprKind::FloorDiv || kind == PolyAffineExprKind::CeilDiv ||
                kind == PolyAffineExprKind::Mod) && operand->valid && divisor > 0;
    }
    bool is_composite_affine() const {
        return valid && (kind == PolyAffineExprKind::Add || kind == PolyAffineExprKind::Sub ||
                         kind == PolyAffineExprKind::Mul) && operands.size() == 2 &&
               operands[0] != nullptr && operands[1] != nullptr && operands[0]->valid &&
               operands[1]->valid;
    }
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
    // Common SCoP iteration space used to compare statements with different
    // loop depths.  The entries are ordered by loop position, not by the
    // statement-local Value identity.
    std::vector<const yir::Value *> space_dims;
    std::vector<PolyAffineExpr> output_dims;
    // Explicit affine relation for output_dims.  Each row has one coefficient
    // per space_dims entry and a matching constant.  This keeps schedule
    // construction independent from the source loop nest spelling.
    std::vector<std::vector<std::int64_t>> matrix;
    std::vector<std::int64_t> matrix_constants;
    bool matrix_valid = true;
    std::size_t lexical_order = 0;
    struct Band {
        std::size_t output_offset = 0;
        std::size_t dimension_count = 0;
        bool permutable = false;
        std::vector<bool> coincident;
        std::vector<bool> reduction;
        // Strip-mining is a lowering of this logical band.  The point
        // schedule remains unchanged, while the backend emits tile loops.
        bool tiled = false;
        std::int64_t tile_size = 0;
        // Explicit outer tile coordinate used by the logical schedule.  For a
        // band over i this is floor(i / tile_size); keeping it here makes the
        // schedule map inspectable without replacing the point schedule used
        // by existing dependence code.
        std::shared_ptr<PolyAffineExpr> tile_expr;
        bool point_order_preserving = false;
    };
    std::vector<Band> bands;
    bool optimized = false;
    bool valid = true;
};

struct PolyAffineMap {
    std::vector<const yir::Value *> dims;
    std::vector<const yir::Value *> symbols;
    std::vector<PolyAffineExpr> results;
    bool valid = true;
};

struct PolyIntegerSet {
    std::vector<const yir::Value *> dims;
    std::vector<const yir::Value *> symbols;
    std::vector<PolyDomainConstraint> constraints;
    bool valid = true;
};

struct PolyAccess {
    enum class Kind { Read, Write };
    Kind kind;
    const yir::Value *memory;
    std::vector<PolyAffineExpr> indices; // Representation for A[expr1][expr2]
};

struct PolyStmt {
    enum class ReductionKind { None, Add };
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
    // A reduction is only marked when an array store directly accumulates a
    // load from the same element with integer addition.  Unknown forms remain
    // unmarked and therefore retain the ordinary dependence restrictions.
    ReductionKind reduction_kind = ReductionKind::None;
    const yir::Value *reduction_memory = nullptr;
    std::size_t reduction_group_id = 0;
    std::vector<bool> reduction_dims;
    // True when the source statement is guarded by a pure non-affine
    // predicate. The domain intentionally over-approximates that predicate.
    bool has_opaque_conditions = false;
};

struct PolyScop {
    std::size_t id;
    std::vector<const yir::Value *> params; // Loop invariants/symbols
    std::vector<const yir::Value *> schedule_space_dims;
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
