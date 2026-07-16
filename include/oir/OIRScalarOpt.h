#pragma once

#include "pass/CostModel.h"
#include "pass/PassManager.h"

#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace pass::oir_opt {

struct Stats {
    unsigned folded = 0;
    unsigned sccp = 0;
    unsigned branches = 0;
    unsigned dce = 0;
    unsigned cfg = 0;
    unsigned gvn = 0;
    unsigned mem2reg = 0;
    unsigned licm = 0;
    unsigned loop_rotate = 0;
    unsigned loop_unswitch = 0;
    unsigned loop_unroll = 0;
    unsigned inlined = 0;
    unsigned globals = 0;
    unsigned tail_recursion = 0;
    unsigned lsr = 0;
    unsigned loop_canonicalize = 0;
    unsigned sroa = 0;
    unsigned dse = 0;
    unsigned dle = 0;
    unsigned adce = 0;
    unsigned jump_threading = 0;
    unsigned dae = 0;
    unsigned memzero = 0;
    unsigned loop_bound_tighten = 0;
    unsigned specialized = 0;
    unsigned cost_model_candidates = 0;

    // Recursive inlining is deliberately a single, bounded phase per function.  The
    // aggressive pipeline may revisit ordinary inlining after cleanup, but restarting
    // recursive expansion from depth zero would defeat its growth/frequency budget.
    std::unordered_set<const oir::Function *> recursively_inlined_functions;

    pass::cost_model::CostModelReport *cost_model_report = nullptr;
    pass::cost_model::CostModelPolicyKind cost_model_policy =
        pass::cost_model::CostModelPolicyKind::Balanced;
    std::string cost_model_filter;

    bool changed() const;
    std::string message() const;
};

enum class SimplifyMode {
    ConstantFold,
    Algebraic,
};

using ReplacementMap = std::unordered_map<oir::Value *, oir::Value *>;

bool is_scalar_type(oir::Type *type);
std::optional<std::int64_t> int_constant(oir::Value *value);
std::optional<float> float_constant(oir::Value *value);
bool is_int_value(oir::Value *value, std::int64_t expected);
oir::ConstantInt *make_int_constant(oir::Module &module, oir::Type *type, std::int64_t value);
oir::Value *make_zero_constant(oir::Module &module, oir::Type *type);
bool same_constant_value(oir::Value *lhs, oir::Value *rhs);
bool eval_cmp(oir::CmpPred pred, std::int64_t lhs, std::int64_t rhs);
bool eval_fcmp(oir::CmpPred pred, float lhs, float rhs);
std::optional<std::int64_t> fold_int_binary(oir::Instruction::OpID op, std::int64_t lhs,
                                            std::int64_t rhs);
std::optional<float> fold_float_binary(oir::Instruction::OpID op, float lhs, float rhs);
bool is_pure_instruction(const oir::Instruction &inst);
unsigned apply_replacements(oir::Module &module, const ReplacementMap &replacements);

bool local_simplify(oir::Module &module, Stats &stats, SimplifyMode mode);
bool value_range_propagation(oir::Module &module, Stats &stats);
bool lower_dense_return_chains(oir::Module &module, Stats &stats);
bool simplify_branches(oir::Module &module, Stats &stats);
bool if_convert_conditional_adds(oir::Module &module, Stats &stats);
bool run_sccp(oir::Module &module, Stats &stats);
bool eliminate_dead_code(oir::Module &module, Stats &stats);
bool cleanup_cfg(oir::Module &module, Stats &stats);
bool scalar_replacement_of_aggregates(oir::Module &module, Stats &stats);
bool promote_memory_to_registers(oir::Module &module, Stats &stats);
bool canonicalize_loops(oir::Module &module, Stats &stats);
bool eliminate_dead_stores(oir::Module &module, Stats &stats);
bool eliminate_dead_loads(oir::Module &module, Stats &stats);
bool loop_invariant_code_motion(oir::Module &module, Stats &stats);
bool rotate_loops(oir::Module &module, Stats &stats);
bool unswitch_loops(oir::Module &module, Stats &stats);
bool unroll_small_constant_loops(oir::Module &module, Stats &stats);
bool tighten_monotonic_guarded_loop_bounds(oir::Module &module, Stats &stats);
bool lower_counted_zero_store_loops_to_memzero(oir::Module &module, Stats &stats);
bool eliminate_overwritten_countdown_loops(oir::Module &module, Stats &stats);
bool global_value_numbering(oir::Module &module, Stats &stats);
bool pre_inline_load_call_cse(oir::Module &module, Stats &stats);
bool aggressive_dead_code_elimination(oir::Module &module, Stats &stats);
bool jump_threading(oir::Module &module, Stats &stats);
bool inline_functions(oir::Module &module, Stats &stats);
bool specialize_constant_argument_calls(oir::Module &module, Stats &stats);
bool eliminate_dead_arguments(oir::Module &module, Stats &stats);
bool propagate_global_constants(oir::Module &module, Stats &stats);
bool promote_global_loads(oir::Module &module, Stats &stats);
bool eliminate_tail_recursion(oir::Module &module, Stats &stats);
bool reduce_gep_strength(oir::Module &module, Stats &stats);
bool optimize_oir_aggressively(oir::Module &module, Stats &stats);
bool verify_oir(oir::Module &module, std::string &message);

template <typename Fn>
PassResult run_oir_transform(PassContext &context, const std::string &missing_message, Fn &&fn) {
    auto *module = context.ssa_module();
    if (module == nullptr) {
        return PassResult::fail(missing_message);
    }

    try {
        Stats stats;
        auto *cost_model_report =
            context.get_artifact<cost_model::CostModelReport>(cost_model::kReportArtifactKey);
        if (cost_model_report != nullptr) {
            stats.cost_model_report = cost_model_report;
            stats.cost_model_policy = cost_model_report->policy;
            stats.cost_model_filter = cost_model_report->filter;
        }
        bool changed = fn(*module, stats);
        if (changed) {
            context.invalidate_oir_analyses();
        }

        std::string message;
        if (!verify_oir(*module, message)) {
            return PassResult::fail(message.empty() ? "OIR verification failed after transform"
                                                    : message);
        }
        return PassResult::ok(changed, stats.message());
    } catch (const std::exception &ex) {
        return PassResult::fail(ex.what());
    }
}

} // namespace pass::oir_opt
