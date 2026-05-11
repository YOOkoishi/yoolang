#pragma once

#include "YIR.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yir {

enum class LoopKind {
    For,
    While,
};

enum class DependenceKind {
    Independent,
    SameElement,
    LoopCarriedPossible,
    Unknown,
};

struct AffineTerm {
    const Value *value = nullptr;
    std::int64_t coefficient = 0;
};

struct AffineExpr {
    bool unknown = false;
    std::int64_t constant = 0;
    std::vector<AffineTerm> terms;

    bool is_constant() const {
        return !unknown && terms.empty();
    }
};

struct ArrayAccess {
    const Operation *op = nullptr;
    bool is_store = false;
    const Value *array = nullptr;
    std::vector<AffineExpr> indices;
};

struct ArrayDependence {
    std::size_t first = 0;
    std::size_t second = 0;
    DependenceKind kind = DependenceKind::Unknown;
    std::string reason;
};

struct LoopCanonicalForm {
    const Region *preheader = nullptr;
    const Region *header = nullptr;
    const Region *body = nullptr;
    const Region *latch = nullptr;
    const Region *exit = nullptr;
    std::string preheader_label;
    std::string header_label;
    std::string body_label;
    std::string latch_label;
    std::string exit_label;
};

struct LoopSummary {
    std::size_t id = 0;
    const Function *function = nullptr;
    const Operation *op = nullptr;
    LoopKind kind = LoopKind::While;
    int parent = -1;
    std::size_t depth = 0;
    LoopCanonicalForm canonical;

    const Value *induction_var = nullptr;
    const Value *lower_bound = nullptr;
    const Value *upper_bound = nullptr;
    const Value *step = nullptr;
    bool has_trip_count = false;
    std::int64_t trip_count = 0;

    std::vector<ArrayAccess> array_accesses;
    std::vector<ArrayDependence> dependencies;
    bool matrix_like_nest = false;
};

class LoopAnalysis final {
  public:
    LoopAnalysis() = default;
    explicit LoopAnalysis(const Module &module);

    void analyze(const Module &module);
    void clear();

    const std::vector<LoopSummary> &loops() const {
        return loops_;
    }

    const LoopSummary *summary_for(const Operation *op) const;

  private:
    std::vector<LoopSummary> loops_;
};

std::string affine_expr_to_string(const AffineExpr &expr);
std::string dependence_kind_to_string(DependenceKind kind);

} // namespace yir
