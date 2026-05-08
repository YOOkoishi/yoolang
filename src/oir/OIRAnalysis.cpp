#include "../../include/oir/OIRAnalysis.h"

#include <algorithm>
#include <deque>

namespace oir {
namespace {

template <typename T> bool contains_value(const std::vector<T> &values, T value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

UseAnalysis::UseAnalysis(Function &function) {
    scan(function);
}

UseAnalysis::UseAnalysis(Module &module) {
    scan(module);
}

void UseAnalysis::scan(Function &function) {
    clear();
    function_ = &function;
    for (auto &block : function.blocks()) {
        for (auto &instruction : block->instructions()) {
            scan_instruction(instruction.get());
        }
    }
}

void UseAnalysis::scan(Module &module) {
    clear();
    module_ = &module;
    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        for (auto &block : function->blocks()) {
            for (auto &instruction : block->instructions()) {
                scan_instruction(instruction.get());
            }
        }
    }
}

void UseAnalysis::clear() {
    function_ = nullptr;
    module_ = nullptr;
    users_.clear();
}

std::size_t UseAnalysis::use_count(const Value *value) const {
    return users(value).size();
}

const std::vector<Instruction *> &UseAnalysis::users(const Value *value) const {
    static const std::vector<Instruction *> empty;
    auto found = users_.find(value);
    return found == users_.end() ? empty : found->second;
}

bool UseAnalysis::has_uses(const Value *value) const {
    return use_count(value) != 0;
}

void UseAnalysis::replace_all_uses_with(Value *old_value, Value *new_value) {
    if (old_value == nullptr || old_value == new_value) {
        return;
    }

    auto worklist = users(old_value);
    for (auto *user : worklist) {
        user->replace_operands(old_value, new_value);
    }

    if (module_ != nullptr) {
        scan(*module_);
    } else if (function_ != nullptr) {
        scan(*function_);
    }
}

void UseAnalysis::scan_instruction(Instruction *instruction) {
    for (auto *operand : instruction->operands()) {
        users_[operand].push_back(instruction);
    }
}

DominatorTree::DominatorTree(const Function &function) : function_(&function) {
    for (const auto &block : function.blocks()) {
        blocks_.push_back(block.get());
    }
    compute_reachable();
    compute_dominators();
    compute_immediate_dominators();
    compute_children();
}

bool DominatorTree::dominates(const BasicBlock *a, const BasicBlock *b) const {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    if (a == b) {
        return true;
    }
    // Unreachable blocks are modeled conservatively: they only dominate themselves.
    if (!is_reachable(a) || !is_reachable(b)) {
        return false;
    }

    auto found = dominators_.find(b);
    return found != dominators_.end() && found->second.find(a) != found->second.end();
}

const BasicBlock *DominatorTree::immediate_dominator(const BasicBlock *block) const {
    auto found = idom_.find(block);
    return found == idom_.end() ? nullptr : found->second;
}

const std::vector<const BasicBlock *> &DominatorTree::children(const BasicBlock *block) const {
    static const std::vector<const BasicBlock *> empty;
    auto found = children_.find(block);
    return found == children_.end() ? empty : found->second;
}

bool DominatorTree::is_reachable(const BasicBlock *block) const {
    return reachable_.find(block) != reachable_.end();
}

void DominatorTree::compute_reachable() {
    auto *entry = function_->entry_block();
    if (entry == nullptr) {
        return;
    }

    std::deque<const BasicBlock *> worklist;
    reachable_.insert(entry);
    worklist.push_back(entry);
    while (!worklist.empty()) {
        auto *block = worklist.front();
        worklist.pop_front();
        reachable_blocks_.push_back(block);
        for (auto *succ : block->successors()) {
            if (reachable_.insert(succ).second) {
                worklist.push_back(succ);
            }
        }
    }
}

void DominatorTree::compute_dominators() {
    BlockSet all_reachable(reachable_blocks_.begin(), reachable_blocks_.end());
    auto *entry = function_->entry_block();

    for (auto *block : blocks_) {
        if (!is_reachable(block)) {
            dominators_[block] = {block};
        } else if (block == entry) {
            dominators_[block] = {block};
        } else {
            dominators_[block] = all_reachable;
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto *block : reachable_blocks_) {
            if (block == entry) {
                continue;
            }

            BlockSet next;
            bool saw_reachable_pred = false;
            for (auto *pred : block->predecessors()) {
                if (!is_reachable(pred)) {
                    continue;
                }
                if (!saw_reachable_pred) {
                    next = dominators_.at(pred);
                    saw_reachable_pred = true;
                    continue;
                }

                for (auto it = next.begin(); it != next.end();) {
                    if (dominators_.at(pred).find(*it) == dominators_.at(pred).end()) {
                        it = next.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            if (!saw_reachable_pred) {
                next.clear();
            }
            next.insert(block);
            if (next != dominators_.at(block)) {
                dominators_[block] = std::move(next);
                changed = true;
            }
        }
    }
}

void DominatorTree::compute_immediate_dominators() {
    auto *entry = function_->entry_block();
    for (auto *block : blocks_) {
        if (!is_reachable(block) || block == entry) {
            idom_[block] = nullptr;
            continue;
        }

        const auto &doms = dominators_.at(block);
        const BasicBlock *best = nullptr;
        for (auto *candidate : doms) {
            if (candidate == block) {
                continue;
            }

            bool dominated_by_all_other_strict_doms = true;
            for (auto *other : doms) {
                if (other == block || other == candidate) {
                    continue;
                }
                if (dominators_.at(candidate).find(other) == dominators_.at(candidate).end()) {
                    dominated_by_all_other_strict_doms = false;
                    break;
                }
            }
            if (dominated_by_all_other_strict_doms) {
                best = candidate;
                break;
            }
        }
        idom_[block] = best;
    }
}

void DominatorTree::compute_children() {
    for (auto *block : blocks_) {
        children_[block];
    }
    for (const auto &item : idom_) {
        if (item.second != nullptr) {
            children_[item.second].push_back(item.first);
        }
    }
}

LoopInfo::LoopInfo(const Function &function, const DominatorTree &dom_tree)
    : function_(&function), dom_tree_(&dom_tree) {
    for (const auto &block : function.blocks()) {
        if (!dom_tree.is_reachable(block.get())) {
            continue;
        }
        for (auto *succ : block->successors()) {
            if (dom_tree.is_reachable(succ) && dom_tree.dominates(succ, block.get())) {
                add_back_edge(block.get(), succ);
            }
        }
    }
    rebuild_block_map();
}

const std::vector<Loop> &LoopInfo::loops() const {
    return loops_;
}

const Loop *LoopInfo::loop_for(const BasicBlock *block) const {
    auto found = loop_for_block_.find(block);
    return found == loop_for_block_.end() ? nullptr : &loops_[found->second];
}

bool LoopInfo::is_loop_header(const BasicBlock *block) const {
    for (const auto &loop : loops_) {
        if (loop.header == block) {
            return true;
        }
    }
    return false;
}

void LoopInfo::add_back_edge(const BasicBlock *latch, const BasicBlock *header) {
    auto loop_blocks = collect_natural_loop(latch, header);

    auto found = std::find_if(loops_.begin(), loops_.end(),
                              [header](const Loop &loop) { return loop.header == header; });
    if (found == loops_.end()) {
        Loop loop;
        loop.header = header;
        loop.latches.push_back(latch);
        loop.blocks = std::move(loop_blocks);
        loops_.push_back(std::move(loop));
        return;
    }

    if (!contains_value(found->latches, latch)) {
        found->latches.push_back(latch);
    }
    for (auto *block : loop_blocks) {
        if (!contains_value(found->blocks, block)) {
            found->blocks.push_back(block);
        }
    }
}

std::vector<const BasicBlock *> LoopInfo::collect_natural_loop(const BasicBlock *latch,
                                                               const BasicBlock *header) const {
    std::vector<const BasicBlock *> blocks;
    std::unordered_set<const BasicBlock *> seen;
    std::deque<const BasicBlock *> worklist;

    seen.insert(header);
    seen.insert(latch);
    blocks.push_back(header);
    if (latch != header) {
        blocks.push_back(latch);
        worklist.push_back(latch);
    }

    while (!worklist.empty()) {
        auto *block = worklist.front();
        worklist.pop_front();
        for (auto *pred : block->predecessors()) {
            if (!dom_tree_->is_reachable(pred)) {
                continue;
            }
            if (seen.insert(pred).second) {
                blocks.push_back(pred);
                if (pred != header) {
                    worklist.push_back(pred);
                }
            }
        }
    }
    return blocks;
}

void LoopInfo::rebuild_block_map() {
    loop_for_block_.clear();
    for (std::size_t i = 0; i < loops_.size(); ++i) {
        for (auto *block : loops_[i].blocks) {
            auto found = loop_for_block_.find(block);
            if (found == loop_for_block_.end() ||
                loops_[i].blocks.size() < loops_[found->second].blocks.size()) {
                loop_for_block_[block] = i;
            }
        }
    }
}

AliasResult OIRAliasAnalysis::alias(const Value *a, const Value *b) const {
    if (a == b) {
        return AliasResult::MustAlias;
    }
    if (a == nullptr || b == nullptr) {
        return AliasResult::MayAlias;
    }

    auto *root_a = underlying_object(a);
    auto *root_b = underlying_object(b);
    if (root_a == root_b && is_distinct_object(root_a)) {
        return AliasResult::MayAlias;
    }
    if (root_a != nullptr && root_b != nullptr && root_a != root_b && is_distinct_object(root_a) &&
        is_distinct_object(root_b)) {
        return AliasResult::NoAlias;
    }

    return AliasResult::MayAlias;
}

bool OIRAliasAnalysis::may_read_memory(const Instruction &inst) const {
    switch (inst.op()) {
    case Instruction::OpID::Load:
    case Instruction::OpID::Call:
        return true;
    default:
        return false;
    }
}

bool OIRAliasAnalysis::may_write_memory(const Instruction &inst) const {
    switch (inst.op()) {
    case Instruction::OpID::Store:
    case Instruction::OpID::Call:
        return true;
    default:
        return false;
    }
}

bool OIRAliasAnalysis::has_side_effect(const Instruction &inst) const {
    switch (inst.op()) {
    case Instruction::OpID::Store:
    case Instruction::OpID::Call:
        return true;
    case Instruction::OpID::Ret:
    case Instruction::OpID::Br:
        // Treat terminators as side-effecting from a DCE perspective: removing them changes CFG.
        return true;
    default:
        return false;
    }
}

const Value *OIRAliasAnalysis::underlying_object(const Value *value) const {
    auto *gep = dynamic_cast<const GetElementPtrInst *>(value);
    if (gep != nullptr) {
        return underlying_object(gep->base_ptr());
    }
    if (is_distinct_object(value)) {
        return value;
    }
    return nullptr;
}

bool OIRAliasAnalysis::is_distinct_object(const Value *value) const {
    return dynamic_cast<const AllocaInst *>(value) != nullptr ||
           dynamic_cast<const GlobalVariable *>(value) != nullptr;
}

} // namespace oir
