#pragma once

#include "OIR.h"
#include "OIRDataLayout.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
    void compute_dom_dfs_numbers();

    const Function *function_ = nullptr;
    std::vector<const BasicBlock *> blocks_;
    std::vector<const BasicBlock *> reachable_blocks_;
    BlockSet reachable_;
    std::unordered_map<const BasicBlock *, BlockSet> dominators_;
    std::unordered_map<const BasicBlock *, const BasicBlock *> idom_;
    std::unordered_map<const BasicBlock *, std::vector<const BasicBlock *>> children_;
    std::unordered_map<const BasicBlock *, std::size_t> dom_pre_;
    std::unordered_map<const BasicBlock *, std::size_t> dom_post_;
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

enum class SCEVKind {
    Unknown,
    Constant,
    Symbol,
    Add,
    Mul,
    AddRec,
};

class SCEVExpr final {
  public:
    SCEVExpr();

    static SCEVExpr unknown();
    static SCEVExpr constant(std::int64_t value);
    static SCEVExpr symbol(const Value *value);
    static SCEVExpr add(SCEVExpr lhs, SCEVExpr rhs);
    static SCEVExpr mul(SCEVExpr lhs, SCEVExpr rhs);
    static SCEVExpr add_rec(SCEVExpr start, SCEVExpr step, const Loop *loop);

    SCEVKind kind() const;
    bool is_unknown() const;
    std::optional<std::int64_t> constant_value() const;
    const Value *symbol_value() const;
    const Loop *loop() const;
    const SCEVExpr *lhs() const;
    const SCEVExpr *rhs() const;
    std::string str() const;

  private:
    explicit SCEVExpr(SCEVKind kind);

    SCEVKind kind_ = SCEVKind::Unknown;
    std::int64_t constant_ = 0;
    const Value *symbol_ = nullptr;
    const Loop *loop_ = nullptr;
    std::shared_ptr<SCEVExpr> lhs_;
    std::shared_ptr<SCEVExpr> rhs_;
};

class ScalarEvolution final {
  public:
    ScalarEvolution(const Function &function, const LoopInfo &loop_info);

    SCEVExpr expression_for(const Value *value, const Loop *loop = nullptr) const;
    std::optional<std::int64_t> constant_trip_count(const Loop &loop) const;
    bool is_loop_invariant(const Value *value, const Loop &loop) const;

  private:
    SCEVExpr expression_for_impl(const Value *value, const Loop *loop,
                                 std::unordered_set<const Value *> &active) const;
    SCEVExpr try_add_rec(const PhiInst &phi, const Loop &loop,
                         std::unordered_set<const Value *> &active) const;
    std::optional<std::int64_t> constant_expr_value(const SCEVExpr &expr) const;

    const Function *function_ = nullptr;
    const LoopInfo *loop_info_ = nullptr;
};

enum class AliasResult { NoAlias, MayAlias, MustAlias };

struct MemoryLocation {
    const Value *base = nullptr;
    std::optional<std::int64_t> offset;
    std::optional<std::uint64_t> size;
};

class OIRAliasAnalysis final {
  public:
    explicit OIRAliasAnalysis(DataLayout data_layout = DataLayout{});

    const DataLayout &data_layout() const;
    AliasResult alias(const Value *a, const Value *b) const;
    MemoryLocation memory_location(const Value *value) const;
    bool points_to_constant_global(const Value *value) const;
    bool call_may_clobber(const CallInst &call, const Value *ptr) const;
    bool may_read_memory(const Instruction &inst) const;
    bool may_write_memory(const Instruction &inst) const;
    bool has_side_effect(const Instruction &inst) const;

  private:
    const Value *underlying_object(const Value *value) const;
    bool is_distinct_object(const Value *value) const;
    std::optional<std::int64_t> constant_gep_offset(const GetElementPtrInst &gep) const;

    DataLayout data_layout_;
};

struct FunctionMemorySummary {
    std::unordered_set<const GlobalVariable *> read_globals;
    std::unordered_set<const GlobalVariable *> written_globals;
    std::unordered_set<std::size_t> read_param_indices;
    std::unordered_set<std::size_t> written_param_indices;
    bool reads_unknown = false;
    bool writes_unknown = false;
    bool reads_all = false;
    bool writes_all = false;
    bool has_side_effect = false;

    bool operator==(const FunctionMemorySummary &other) const;
    bool operator!=(const FunctionMemorySummary &other) const;
    bool may_read_memory() const;
    bool may_write_memory() const;
};

class FunctionModRefAnalysis final {
  public:
    explicit FunctionModRefAnalysis(const Module &module);

    const FunctionMemorySummary &summary(const Function *function) const;
    bool call_may_clobber(const CallInst &call, const Value *ptr,
                          const OIRAliasAnalysis &alias_analysis) const;
    bool call_may_read(const CallInst &call, const Value *ptr,
                       const OIRAliasAnalysis &alias_analysis) const;
    bool call_has_side_effect(const CallInst &call) const;
    bool call_may_read_memory(const CallInst &call) const;
    bool call_may_write_memory(const CallInst &call) const;

  private:
    FunctionMemorySummary external_summary(const Function &function) const;
    FunctionMemorySummary unknown_external_summary() const;
    FunctionMemorySummary scan_function(const Function &function) const;
    FunctionMemorySummary call_summary(const CallInst &call) const;

    const Module *module_ = nullptr;
    std::unordered_map<const Function *, FunctionMemorySummary> summaries_;
};

enum class MemoryAccessKind {
    LiveOnEntry,
    Use,
    Def,
    Phi,
};

class MemoryAccess final {
  public:
    MemoryAccessKind kind() const;
    unsigned id() const;
    Instruction *instruction() const;
    BasicBlock *block() const;
    MemoryAccess *defining_access() const;
    const std::vector<std::pair<BasicBlock *, MemoryAccess *>> &incoming() const;

    bool is_live_on_entry() const;
    bool is_use() const;
    bool is_def() const;
    bool is_phi() const;

  private:
    MemoryAccess(unsigned id, MemoryAccessKind kind, BasicBlock *block,
                 Instruction *instruction = nullptr);

    void set_defining_access(MemoryAccess *access);
    void clear_incoming();
    void add_incoming(BasicBlock *block, MemoryAccess *access);

    unsigned id_ = 0;
    MemoryAccessKind kind_ = MemoryAccessKind::LiveOnEntry;
    BasicBlock *block_ = nullptr;
    Instruction *instruction_ = nullptr;
    MemoryAccess *defining_access_ = nullptr;
    std::vector<std::pair<BasicBlock *, MemoryAccess *>> incoming_;

    friend class MemorySSA;
};

class MemorySSA final {
  public:
    MemorySSA(Function &function, const OIRAliasAnalysis &alias_analysis,
              const FunctionModRefAnalysis &modref);

    MemoryAccess *live_on_entry() const;
    MemoryAccess *access_for(const Instruction *instruction) const;
    MemoryAccess *memory_phi(const BasicBlock *block) const;
    const std::vector<std::unique_ptr<MemoryAccess>> &accesses() const;

    MemoryAccess *clobbering_access(const LoadInst &load) const;
    MemoryAccess *clobbering_access(const StoreInst &store) const;
    MemoryAccess *clobbering_access(const CallInst &call) const;
    MemoryAccess *clobbering_access(const Instruction &memory_instruction) const;

  private:
    void build();
    std::vector<BasicBlock *> reachable_reverse_postorder() const;
    MemoryAccess *create_access(MemoryAccessKind kind, BasicBlock *block,
                                Instruction *instruction = nullptr);
    MemoryAccess *entry_access_for(BasicBlock *block,
                                   const std::unordered_set<BasicBlock *> &reachable) const;
    void scan_block(BasicBlock *block, MemoryAccess *start_access);
    void populate_phi_incomings(const std::unordered_set<BasicBlock *> &reachable);

    MemoryAccess *find_pointer_clobber(MemoryAccess *access, const Value *ptr,
                                       std::unordered_set<const MemoryAccess *> &active,
                                       std::unordered_map<const MemoryAccess *, MemoryAccess *>
                                           &memo) const;
    MemoryAccess *find_call_read_clobber(MemoryAccess *access, const CallInst &call,
                                         std::unordered_set<const MemoryAccess *> &active,
                                         std::unordered_map<const MemoryAccess *, MemoryAccess *>
                                             &memo) const;
    bool access_clobbers_pointer(const MemoryAccess &access, const Value *ptr) const;
    bool access_clobbers_call_read(const MemoryAccess &access, const CallInst &call) const;

    Function *function_ = nullptr;
    const OIRAliasAnalysis *alias_analysis_ = nullptr;
    const FunctionModRefAnalysis *modref_ = nullptr;
    std::vector<std::unique_ptr<MemoryAccess>> accesses_;
    MemoryAccess *live_on_entry_ = nullptr;
    std::unordered_map<const Instruction *, MemoryAccess *> access_for_inst_;
    std::unordered_map<const BasicBlock *, MemoryAccess *> phi_for_block_;
    std::unordered_map<const BasicBlock *, MemoryAccess *> block_end_access_;
};

} // namespace oir
