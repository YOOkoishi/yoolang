#pragma once

#include "oir/OIRAnalysis.h"

#include <optional>
#include <unordered_set>

namespace pass::oir_opt {

struct RotatedOverwrittenCountdownLoop {
    oir::BasicBlock *preheader = nullptr;
    oir::BasicBlock *body = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::PhiInst *induction = nullptr;
    oir::Value *start = nullptr;
    oir::BinaryInst *next = nullptr;
};

std::optional<RotatedOverwrittenCountdownLoop>
match_rotated_overwritten_countdown_loop(const oir::Loop &loop,
                                         const oir::FunctionModRefAnalysis &modref);

bool may_have_guarded_overwritten_countdown_inline_barrier(const oir::Module &module);

std::unordered_set<const oir::BasicBlock *>
find_guarded_overwritten_countdown_inline_barriers(const oir::Function &function,
                                                   const oir::FunctionModRefAnalysis &modref);

} // namespace pass::oir_opt
