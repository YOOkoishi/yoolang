#include "pass/oir/OIROptimizationPipelinePass.h"

#include "oir/OIRScalarOpt.h"

namespace pass {

namespace {

bool run_aggressive_iteration(oir::Module &module, oir_opt::Stats &stats) {
    bool changed = false;
    changed |= oir_opt::canonicalize_loops(module, stats);
    changed |= oir_opt::propagate_global_constants(module, stats);
    changed |= oir_opt::simplify_branches(module, stats);
    changed |= oir_opt::cleanup_cfg(module, stats);
    changed |= oir_opt::promote_global_loads(module, stats);
    changed |= oir_opt::scalar_replacement_of_aggregates(module, stats);
    changed |= oir_opt::promote_memory_to_registers(module, stats);
    changed |= oir_opt::local_simplify(module, stats, oir_opt::SimplifyMode::ConstantFold);
    changed |= oir_opt::local_simplify(module, stats, oir_opt::SimplifyMode::Algebraic);
    changed |= oir_opt::run_sccp(module, stats);
    changed |= oir_opt::simplify_branches(module, stats);
    changed |= oir_opt::if_convert_conditional_adds(module, stats);
    changed |= oir_opt::cleanup_cfg(module, stats);
    changed |= oir_opt::local_simplify(module, stats, oir_opt::SimplifyMode::ConstantFold);
    changed |= oir_opt::local_simplify(module, stats, oir_opt::SimplifyMode::Algebraic);
    changed |= oir_opt::value_range_propagation(module, stats);
    changed |= oir_opt::global_value_numbering(module, stats);
    changed |= oir_opt::eliminate_dead_loads(module, stats);
    changed |= oir_opt::eliminate_dead_stores(module, stats);
    changed |= oir_opt::loop_invariant_code_motion(module, stats);
    changed |= oir_opt::reduce_gep_strength(module, stats);
    changed |= oir_opt::unswitch_loops(module, stats);
    changed |= oir_opt::rotate_loops(module, stats);
    changed |= oir_opt::eliminate_overwritten_countdown_loops(module, stats);
    changed |= oir_opt::reduce_gep_strength(module, stats);
    changed |= oir_opt::lower_counted_zero_store_loops_to_memzero(module, stats);
    changed |= oir_opt::global_value_numbering(module, stats);
    changed |= oir_opt::eliminate_dead_loads(module, stats);
    changed |= oir_opt::eliminate_dead_stores(module, stats);
    changed |= oir_opt::run_sccp(module, stats);
    changed |= oir_opt::simplify_branches(module, stats);
    changed |= oir_opt::if_convert_conditional_adds(module, stats);
    changed |= oir_opt::cleanup_cfg(module, stats);
    changed |= oir_opt::aggressive_dead_code_elimination(module, stats);
    changed |= oir_opt::jump_threading(module, stats);
    return changed;
}

bool cleanup_after_call_specialization(oir::Module &module, oir_opt::Stats &stats) {
    bool changed = false;
    changed |= oir_opt::propagate_global_constants(module, stats);
    changed |= oir_opt::local_simplify(module, stats, oir_opt::SimplifyMode::ConstantFold);
    changed |= oir_opt::local_simplify(module, stats, oir_opt::SimplifyMode::Algebraic);
    changed |= oir_opt::run_sccp(module, stats);
    changed |= oir_opt::simplify_branches(module, stats);
    changed |= oir_opt::cleanup_cfg(module, stats);
    changed |= oir_opt::value_range_propagation(module, stats);
    changed |= oir_opt::global_value_numbering(module, stats);
    changed |= oir_opt::eliminate_dead_code(module, stats);
    changed |= oir_opt::aggressive_dead_code_elimination(module, stats);
    changed |= oir_opt::cleanup_cfg(module, stats);
    return changed;
}

bool run_call_specialization_window(oir::Module &module, oir_opt::Stats &stats) {
    bool changed = false;
    constexpr unsigned kMaxRounds = 4;
    for (unsigned round = 0; round < kMaxRounds; ++round) {
        if (!oir_opt::specialize_constant_argument_calls(module, stats)) {
            break;
        }
        changed = true;
        changed |= cleanup_after_call_specialization(module, stats);
    }
    return changed;
}

} // namespace

namespace oir_opt {

bool optimize_oir_aggressively(oir::Module &module, Stats &stats) {
    bool changed = false;
    changed |= canonicalize_loops(module, stats);
    changed |= propagate_global_constants(module, stats);
    changed |= simplify_branches(module, stats);
    changed |= cleanup_cfg(module, stats);
    changed |= promote_global_loads(module, stats);
    changed |= scalar_replacement_of_aggregates(module, stats);
    changed |= promote_memory_to_registers(module, stats);
    changed |= local_simplify(module, stats, SimplifyMode::ConstantFold);
    changed |= local_simplify(module, stats, SimplifyMode::Algebraic);
    changed |= eliminate_tail_recursion(module, stats);
    changed |= lower_dense_return_chains(module, stats);
    changed |= propagate_global_constants(module, stats);
    changed |= run_call_specialization_window(module, stats);
    changed |= inline_functions(module, stats);
    if (changed) {
        changed |= run_sccp(module, stats);
        changed |= value_range_propagation(module, stats);
        changed |= global_value_numbering(module, stats);
        changed |= eliminate_dead_code(module, stats);
        changed |= if_convert_conditional_adds(module, stats);
        changed |= cleanup_cfg(module, stats);
    }
    changed |= run_call_specialization_window(module, stats);
    if (inline_functions(module, stats)) {
        changed = true;
        changed |= run_sccp(module, stats);
        changed |= value_range_propagation(module, stats);
        changed |= global_value_numbering(module, stats);
        changed |= eliminate_dead_code(module, stats);
        changed |= if_convert_conditional_adds(module, stats);
        changed |= cleanup_cfg(module, stats);
    }

    constexpr unsigned kMaxIterations = 8;
    for (unsigned iteration = 0; iteration < kMaxIterations; ++iteration) {
        if (!run_aggressive_iteration(module, stats)) {
            break;
        }
        changed = true;
    }

    changed |= simplify_branches(module, stats);
    changed |= if_convert_conditional_adds(module, stats);
    changed |= cleanup_cfg(module, stats);
    changed |= reduce_gep_strength(module, stats);
    changed |= value_range_propagation(module, stats);
    changed |= eliminate_overwritten_countdown_loops(module, stats);
    changed |= lower_counted_zero_store_loops_to_memzero(module, stats);
    changed |= propagate_global_constants(module, stats);
    changed |= promote_global_loads(module, stats);
    changed |= scalar_replacement_of_aggregates(module, stats);
    changed |= promote_memory_to_registers(module, stats);
    changed |= eliminate_dead_loads(module, stats);
    changed |= eliminate_dead_stores(module, stats);
    changed |= aggressive_dead_code_elimination(module, stats);
    changed |= jump_threading(module, stats);
    changed |= eliminate_dead_arguments(module, stats);
    changed |= propagate_global_constants(module, stats);
    changed |= eliminate_dead_code(module, stats);
    return changed;
}

} // namespace oir_opt

std::string_view OIROptimizationPipelinePass::name() const {
    return "OIROptimizationPipelinePass";
}

PassKind OIROptimizationPipelinePass::kind() const {
    return PassKind::Transform;
}

PassResult OIROptimizationPipelinePass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIROptimizationPipelinePass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            return oir_opt::optimize_oir_aggressively(module, stats);
        });
}

} // namespace pass
