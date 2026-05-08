#pragma once

#include "oir.h"

#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oir {

class UseAnalysis final {
  public:
    UseAnalysis() = default;
    explicit UseAnalysis(Function &function);
    explicit UseAnalysis(Module &module);

    void scan(Function &function);
    void scan(Module &module);
    void clear();

    std::size_t use_count(const Value *value) const;
    const std::vector<Instruction *> &users(const Value *value) const;
    bool has_uses(const Value *value) const;
    void replace_all_uses_with(Value *old_value, Value *new_value);

  private:
    void scan_instruction(Instruction *instruction);

    Function *function_ = nullptr;
    Module *module_ = nullptr;
    std::unordered_map<const Value *, std::vector<Instruction *>> users_;
};

class DominatorTree final {
  public:
    explicit DominatorTree(const Function &function);

    bool dominates(const BasicBlock *a, const BasicBlock *b) const;
    const BasicBlock *immediate_dominator(const BasicBlock *block) const;
    const std::vector<const BasicBlock *> &children(const BasicBlock *block) const;
    bool is_reachable(const BasicBlock *block) const;

  private:
    using BlockSet = std::unordered_set<const BasicBlock *>;

    void compute_reachable();
    void compute_dominators();
    void compute_immediate_dominators();
    void compute_children();

    const Function *function_ = nullptr;
    std::vector<const BasicBlock *> blocks_;
    std::vector<const BasicBlock *> reachable_blocks_;
    BlockSet reachable_;
    std::unordered_map<const BasicBlock *, BlockSet> dominators_;
    std::unordered_map<const BasicBlock *, const BasicBlock *> idom_;
    std::unordered_map<const BasicBlock *, std::vector<const BasicBlock *>> children_;
};

struct Loop {
    const BasicBlock *header = nullptr;
    std::vector<const BasicBlock *> latches;
    std::vector<const BasicBlock *> blocks;
};

class LoopInfo final {
  public:
    LoopInfo(const Function &function, const DominatorTree &dom_tree);

    const std::vector<Loop> &loops() const;
    const Loop *loop_for(const BasicBlock *block) const;
    bool is_loop_header(const BasicBlock *block) const;

  private:
    void add_back_edge(const BasicBlock *latch, const BasicBlock *header);
    std::vector<const BasicBlock *> collect_natural_loop(const BasicBlock *latch,
                                                         const BasicBlock *header) const;
    void rebuild_block_map();

    const Function *function_ = nullptr;
    const DominatorTree *dom_tree_ = nullptr;
    std::vector<Loop> loops_;
    std::unordered_map<const BasicBlock *, std::size_t> loop_for_block_;
};

enum class AliasResult { NoAlias, MayAlias, MustAlias };

class OIRAliasAnalysis final {
  public:
    AliasResult alias(const Value *a, const Value *b) const;
    bool may_read_memory(const Instruction &inst) const;
    bool may_write_memory(const Instruction &inst) const;
    bool has_side_effect(const Instruction &inst) const;

  private:
    const Value *underlying_object(const Value *value) const;
    bool is_distinct_object(const Value *value) const;
};

} // namespace oir
