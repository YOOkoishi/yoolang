#include "../../include/oir/OIRScalarOpt.h"

#include "../../include/oir/OIRAnalysis.h"
#include "../../include/oir/OIRCFGUtils.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
    if (preheader == nullptr || preheader->successors().size() != 1 ||
        preheader->successors().front() != loop.header) {
        return nullptr;
    }
    return preheader;
}

std::vector<std::pair<oir::Value *, oir::BasicBlock *>>
incoming_from_outside(const oir::PhiInst &phi, const oir::Loop &loop) {
    std::vector<std::pair<oir::Value *, oir::BasicBlock *>> incoming;
    for (const auto &item : phi.incoming()) {
        if (!contains_block(loop, item.second)) {
            incoming.push_back(item);
        }
    }
    return incoming;
}

oir::Value *
create_preheader_phi(oir::BasicBlock *preheader, oir::PhiInst &header_phi,
                     const std::vector<std::pair<oir::Value *, oir::BasicBlock *>> &incoming) {
    if (incoming.empty()) {
        return nullptr;
    }

    auto *first = incoming.front().first;
    bool all_same = true;
    for (const auto &item : incoming) {
        if (item.first != first) {
            all_same = false;
            break;
        }
    }
    if (all_same) {
        return first;
    }

    auto phi = std::make_unique<oir::PhiInst>(
        header_phi.type(), preheader,
        header_phi.name().empty() ? "licm.pre" : header_phi.name() + ".pre");
    auto *raw = phi.get();
    raw->set_parent(preheader);
    for (const auto &item : incoming) {
        raw->add_incoming(item.first, item.second);
    }
    preheader->instructions().push_back(std::move(phi));
    return raw;
}

oir::BasicBlock *ensure_preheader(oir::Function &function, const oir::Loop &loop, Stats &stats) {
    if (auto *existing = find_preheader(loop)) {
        return existing;
    }

    std::vector<oir::BasicBlock *> outside_preds;
    for (auto *pred : loop.header->predecessors()) {
        if (!contains_block(loop, pred)) {
            outside_preds.push_back(pred);
        }
    }
    if (outside_preds.empty()) {
        return nullptr;
    }

    auto *preheader = function.create_block("licm.preheader");
    std::unordered_map<oir::PhiInst *, oir::Value *> header_phi_values;
    for (auto &inst : const_cast<oir::BasicBlock *>(loop.header)->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        auto outside_incoming = incoming_from_outside(*phi, loop);
        if (!outside_incoming.empty()) {
            header_phi_values[phi] = create_preheader_phi(preheader, *phi, outside_incoming);
        }
    }

    oir::cfg::append_unconditional_branch(*function.parent(), preheader,
                                          const_cast<oir::BasicBlock *>(loop.header));

    for (auto *pred : outside_preds) {
        oir::cfg::replace_successor(pred, const_cast<oir::BasicBlock *>(loop.header), preheader);
    }

    for (auto &[phi, value] : header_phi_values) {
        phi->add_incoming(value, preheader);
    }

    ++stats.cfg;
    return preheader;
}

bool is_speculatable_divisor(oir::Value *value) {
    auto constant = int_constant(value);
    return constant.has_value() && *constant != 0;
}

bool loop_may_clobber(const oir::Loop &loop, oir::Value *ptr,
                      const oir::OIRAliasAnalysis &alias_analysis,
                      const oir::FunctionModRefAnalysis &modref) {
    for (auto *const_block : loop.blocks) {
        for (const auto &inst : const_block->instructions()) {
            if (auto *store = dynamic_cast<oir::StoreInst *>(inst.get())) {
                if (alias_analysis.alias(ptr, store->ptr()) != oir::AliasResult::NoAlias) {
                    return true;
                }
                continue;
            }
            if (auto *call = dynamic_cast<oir::CallInst *>(inst.get())) {
                if (modref.call_may_clobber(*call, ptr, alias_analysis)) {
                    return true;
                }
            }
        }
    }
    return false;
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
    case oir::Instruction::OpID::SRem: {
        const auto *binary = dynamic_cast<const oir::BinaryInst *>(&inst);
        return binary != nullptr && is_speculatable_divisor(binary->rhs());
    }
    case oir::Instruction::OpID::Ret:
    case oir::Instruction::OpID::Br:
    case oir::Instruction::OpID::Alloca:
    case oir::Instruction::OpID::Store:
    case oir::Instruction::OpID::Call:
    case oir::Instruction::OpID::Phi:
        return false;
    case oir::Instruction::OpID::Load:
        return true;
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
                              const std::unordered_set<oir::Instruction *> &moving,
                              const oir::OIRAliasAnalysis &alias_analysis,
                              const oir::FunctionModRefAnalysis &modref) {
    if (!is_licm_candidate(inst)) {
        return false;
    }
    if (auto *load = dynamic_cast<const oir::LoadInst *>(&inst)) {
        if (!operand_is_invariant(loop, load->ptr(), moving)) {
            return false;
        }
        return alias_analysis.points_to_constant_global(load->ptr()) ||
               !loop_may_clobber(loop, load->ptr(), alias_analysis, modref);
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

bool run_on_loop(oir::Function &function, const oir::Loop &loop, Stats &stats) {
    auto *preheader = ensure_preheader(function, loop, stats);
    if (preheader == nullptr || !preheader->has_terminator()) {
        return false;
    }

    bool changed = false;
    bool keep_going = true;
    oir::OIRAliasAnalysis alias_analysis;
    oir::FunctionModRefAnalysis modref(*function.parent());
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
                if (instruction_is_invariant(loop, *inst, moved, alias_analysis, modref)) {
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
        changed |= run_on_loop(function, loop, stats);
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
