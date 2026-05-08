#include "../../include/oir/OIRScalarOpt.h"

#include "../../include/oir/OIRAnalysis.h"

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

bool contains_block(const oir::Loop &loop, const oir::BasicBlock *block) {
    return std::find(loop.blocks.begin(), loop.blocks.end(), block) != loop.blocks.end();
}

oir::BasicBlock *find_preheader(const oir::Loop &loop) {
    oir::BasicBlock *preheader = nullptr;
    for (auto *pred : loop.header->predecessors()) {
        if (contains_block(loop, pred)) {
            continue;
        }
        if (preheader != nullptr) {
            return nullptr;
        }
        preheader = pred;
    }
    return preheader;
}

bool is_licm_candidate(const oir::Instruction &inst) {
    switch (inst.op()) {
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::Mul:
    case oir::Instruction::OpID::FAdd:
    case oir::Instruction::OpID::FSub:
    case oir::Instruction::OpID::FMul:
    case oir::Instruction::OpID::FDiv:
    case oir::Instruction::OpID::ICmp:
    case oir::Instruction::OpID::FCmp:
    case oir::Instruction::OpID::GetElementPtr:
    case oir::Instruction::OpID::ZExt:
    case oir::Instruction::OpID::SIToFP:
    case oir::Instruction::OpID::FPToSI:
        return true;
    case oir::Instruction::OpID::SDiv:
    case oir::Instruction::OpID::SRem:
    case oir::Instruction::OpID::Ret:
    case oir::Instruction::OpID::Br:
    case oir::Instruction::OpID::Alloca:
    case oir::Instruction::OpID::Load:
    case oir::Instruction::OpID::Store:
    case oir::Instruction::OpID::Call:
    case oir::Instruction::OpID::Phi:
        return false;
    }
    return false;
}

bool operand_is_invariant(const oir::Loop &loop, oir::Value *value,
                          const std::unordered_set<oir::Instruction *> &moving) {
    auto *inst = dynamic_cast<oir::Instruction *>(value);
    if (inst == nullptr) {
        return true;
    }
    if (!contains_block(loop, inst->parent())) {
        return true;
    }
    return moving.find(inst) != moving.end();
}

bool instruction_is_invariant(const oir::Loop &loop, const oir::Instruction &inst,
                              const std::unordered_set<oir::Instruction *> &moving) {
    if (!is_licm_candidate(inst)) {
        return false;
    }
    for (auto *operand : inst.operands()) {
        if (!operand_is_invariant(loop, operand, moving)) {
            return false;
        }
    }
    return true;
}

void move_before_terminator(oir::Instruction *inst, oir::BasicBlock *dest) {
    auto *source = inst->parent();
    std::unique_ptr<oir::Instruction> owned;
    for (auto it = source->instructions().begin(); it != source->instructions().end(); ++it) {
        if (it->get() != inst) {
            continue;
        }
        owned = std::move(*it);
        source->instructions().erase(it);
        break;
    }
    if (owned == nullptr) {
        return;
    }

    owned->set_parent(dest);
    if (dest->has_terminator()) {
        dest->instructions().insert(std::prev(dest->instructions().end()), std::move(owned));
    } else {
        dest->instructions().push_back(std::move(owned));
    }
}

bool run_on_loop(const oir::Loop &loop, Stats &stats) {
    auto *preheader = find_preheader(loop);
    if (preheader == nullptr || !preheader->has_terminator()) {
        return false;
    }

    bool changed = false;
    bool keep_going = true;
    std::unordered_set<oir::Instruction *> moved;
    while (keep_going) {
        keep_going = false;
        std::vector<oir::Instruction *> to_move;
        for (auto *const_block : loop.blocks) {
            auto *block = const_cast<oir::BasicBlock *>(const_block);
            for (auto &inst : block->instructions()) {
                if (moved.find(inst.get()) != moved.end()) {
                    continue;
                }
                if (instruction_is_invariant(loop, *inst, moved)) {
                    to_move.push_back(inst.get());
                }
            }
        }

        for (auto *inst : to_move) {
            moved.insert(inst);
            move_before_terminator(inst, preheader);
            ++stats.licm;
            changed = true;
            keep_going = true;
        }
    }

    return changed;
}

bool run_on_function(oir::Function &function, Stats &stats) {
    if (function.is_external() || function.entry_block() == nullptr) {
        return false;
    }

    oir::DominatorTree dom_tree(function);
    oir::LoopInfo loop_info(function, dom_tree);
    bool changed = false;
    auto loops = loop_info.loops();
    std::sort(loops.begin(), loops.end(), [](const oir::Loop &lhs, const oir::Loop &rhs) {
        return lhs.blocks.size() < rhs.blocks.size();
    });
    for (const auto &loop : loops) {
        changed |= run_on_loop(loop, stats);
    }
    return changed;
}

} // namespace

bool loop_invariant_code_motion(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        changed |= run_on_function(*function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt
