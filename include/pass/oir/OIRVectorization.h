#pragma once

#include "oir/OIR.h"
#include "oir/OIRAnalysis.h"
#include "pass/oir/OIRVectorizationRemark.h"
#include "target/TargetMachine.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pass::oir_vectorize {

struct MemoryAccess final {
    const oir::Instruction *instruction = nullptr;
    const oir::Value *pointer = nullptr;
    const oir::Value *underlying_object = nullptr;
    bool is_write = false;
    std::optional<std::int64_t> stride_elements;
};

struct LoopAccessInfo final {
    std::vector<MemoryAccess> accesses;
    std::vector<std::pair<std::size_t, std::size_t>> may_alias_pairs;
    bool requires_runtime_alias_check = false;
    bool has_loop_carried_dependence = false;
    std::string explanation;
};

class LoopAccessAnalysis final {
  public:
    LoopAccessInfo analyze(const oir::Loop &loop, const oir::ScalarEvolution &scev,
                           const oir::OIRAliasAnalysis &alias_analysis) const;
};

struct LegalityOptions final {
    bool strict_floating_point = true;
    bool allow_runtime_alias_checks = false;
};

struct LegalityResult final {
    bool legal = false;
    RemarkCode code = RemarkCode::RejectUnsupportedType;
    std::string explanation;
    const oir::PhiInst *canonical_induction = nullptr;
    std::int64_t induction_step = 0;
    std::optional<std::int64_t> constant_trip_count;
    LoopAccessInfo memory;
    bool contains_float = false;
    bool needs_predication = true;
    unsigned scalar_instruction_count = 0;
    unsigned reduction_operation_count = 0;
};

class LoopVectorizationLegality final {
  public:
    explicit LoopVectorizationLegality(LegalityOptions options = {});

    LegalityResult analyze(const oir::Function &function, const oir::Loop &loop,
                           const oir::LoopInfo &loop_info, const oir::ScalarEvolution &scev,
                           const oir::OIRAliasAnalysis &alias_analysis) const;

  private:
    LegalityOptions options_;
};

struct CostModelOptions final {
    bool force = false;
    unsigned expected_trip_count = 64;
    bool explore_interleave = false;
};

struct PlanResult final {
    bool profitable = false;
    RemarkCode code = RemarkCode::RejectCost;
    std::string explanation;
    PlanChoice choice;
};

class RVVCostModel final {
  public:
    RVVCostModel(target::TargetProfile target, CostModelOptions options = {});

    PlanResult choose(const LegalityResult &legality, bool interleave_factor_two_legal = false,
                      std::string interleave_factor_two_rejection = {}) const;

  private:
    target::TargetProfile target_;
    CostModelOptions options_;
};

enum class RecipeKind : std::uint8_t {
    SetVectorLength,
    ActiveLaneMask,
    ScalarAddress,
    StridedIndex,
    WidenLoad,
    WidenStore,
    WidenGather,
    WidenScatter,
    WidenBinary,
    WidenCompare,
    WidenCast,
    MergeSelect,
    WidenReduction,
    InductionUpdate,
    PointerUpdate,
    RemainingUpdate,
};

enum class LanePredicate : std::uint8_t {
    Active,
    Then,
    Else,
};

struct VectorizationRecipe final {
    RecipeKind kind = RecipeKind::WidenBinary;
    const oir::Instruction *scalar_instruction = nullptr;
    std::string explanation;
};

struct PointerInduction final {
    oir::PhiInst *phi = nullptr;
    oir::GetElementPtrInst *update = nullptr;
    std::int64_t stride_elements = 1;
};

struct IntegerReduction final {
    oir::PhiInst *phi = nullptr;
    // Final value carried by the latch.  An unrolled scalar loop may form it
    // through a linear chain of the same associative integer operation.
    oir::BinaryInst *update = nullptr;
    oir::Instruction::OpID operation = oir::Instruction::OpID::Add;
    std::vector<oir::BinaryInst *> chain_updates;
    std::vector<oir::Value *> lane_values;
    // Loop rotation materializes a two-edge exit phi so that the zero-trip
    // edge still returns the seed.  This is the only permitted use of the
    // scalar update in addition to the loop-carried accumulator phi.
    oir::PhiInst *rotated_exit_phi = nullptr;
};

struct PredicatedScalarInstruction final {
    oir::Instruction *instruction = nullptr;
    LanePredicate predicate = LanePredicate::Active;
};

struct IfConversionRegion final {
    oir::BasicBlock *condition_block = nullptr;
    oir::BasicBlock *then_block = nullptr;
    oir::BasicBlock *else_block = nullptr;
    oir::BasicBlock *merge_block = nullptr;
    oir::BasicBlock *true_predecessor = nullptr;
    oir::BasicBlock *false_predecessor = nullptr;
    oir::CmpInst *condition = nullptr;
    std::vector<oir::PhiInst *> merge_phis;

    bool enabled() const {
        return condition_block != nullptr;
    }
};

struct RuntimeAliasRange final {
    oir::Value *base = nullptr;
    std::int64_t stride_elements = 0;
    std::uint32_t element_bytes = 0;
};

struct RuntimeAliasCheck final {
    RuntimeAliasRange lhs;
    RuntimeAliasRange rhs;
};

struct VectorizationPlan final {
    oir::Function *function = nullptr;
    oir::BasicBlock *preheader = nullptr;
    oir::BasicBlock *header = nullptr;
    oir::BasicBlock *body = nullptr;
    oir::BasicBlock *latch = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::PhiInst *induction = nullptr;
    oir::BinaryInst *induction_update = nullptr;
    // A guarded rotated loop may expose its post-increment through one
    // canonical two-edge exit phi.  The vector update rewires that edge;
    // runtime-versioned scalar clones add their own corresponding edge.
    oir::PhiInst *induction_exit_phi = nullptr;
    oir::CmpInst *loop_condition = nullptr;
    oir::Value *trip_count = nullptr;
    std::int64_t induction_step = 1;
    // A rotated loop executes the lane body before testing its latch condition.
    // This covers both a single-block do-while shape and a guarded, unnested
    // lane diamond whose arms merge at the loop-control latch.
    bool rotated_loop = false;
    oir::ElementCount element_count{1, true};
    oir::Type *configuration_element_type = nullptr;
    PlanChoice choice;
    std::vector<MemoryAccess> memory_accesses;
    std::vector<RuntimeAliasCheck> runtime_alias_checks;
    std::vector<PointerInduction> pointer_inductions;
    std::vector<IntegerReduction> integer_reductions;
    IfConversionRegion if_conversion;
    std::vector<VectorizationRecipe> recipes;
    std::vector<PredicatedScalarInstruction> scalar_instructions_to_widen;
    std::vector<PredicatedScalarInstruction> post_merge_instructions_to_widen;
};

struct PlanBuildResult final {
    bool valid = false;
    RemarkCode code = RemarkCode::RejectNonCanonicalLoop;
    std::string explanation;
    VectorizationPlan plan;
};

class VectorizationPlanner final {
  public:
    PlanBuildResult build(oir::Function &function, const oir::Loop &loop,
                          const LegalityResult &legality, const PlanChoice &choice) const;
};

struct LoopVectorizerOptions final {
    bool enabled = true;
    bool force = false;
    bool strict_floating_point = true;
    unsigned expected_trip_count = 64;
    // O3 may request the verified two-chunk VLA recipe.  O2 and lower stay at
    // factor one, and unsupported recipes fail closed to factor one.
    bool explore_interleave = false;
    // Unknown aliasing is eligible only when the planner can construct every
    // complete affine byte range and install the overflow-safe scalar fallback.
    bool enable_runtime_alias_versioning = true;
    // Optional embedding gate run after the ordinary fail-closed verifier but
    // before the textual transaction commits.  A rejection must leave the
    // original module byte-for-byte unchanged and cannot emit VECTORIZED.
    std::function<bool(const oir::Module &, std::string &)> post_transform_validation;
};

struct LoopVectorizerResult final {
    bool success = true;
    bool changed = false;
    unsigned loops_vectorized = 0;
    std::string message;
};

class LoopVectorizer final {
  public:
    explicit LoopVectorizer(LoopVectorizerOptions options = {});

    LoopVectorizerResult run(oir::Module &module, const target::TargetProfile &target,
                             RemarkLog &remarks) const;

  private:
    LoopVectorizerOptions options_;
};

} // namespace pass::oir_vectorize
