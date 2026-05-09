#pragma once

#include "../../include/pass/PassManager.h"

#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

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
    unsigned inlined = 0;

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
bool simplify_branches(oir::Module &module, Stats &stats);
bool run_sccp(oir::Module &module, Stats &stats);
bool eliminate_dead_code(oir::Module &module, Stats &stats);
bool cleanup_cfg(oir::Module &module, Stats &stats);
bool promote_memory_to_registers(oir::Module &module, Stats &stats);
bool loop_invariant_code_motion(oir::Module &module, Stats &stats);
bool global_value_numbering(oir::Module &module, Stats &stats);
bool inline_functions(oir::Module &module, Stats &stats);
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
