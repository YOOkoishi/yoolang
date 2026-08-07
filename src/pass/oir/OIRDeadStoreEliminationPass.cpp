#include "pass/oir/OIRDeadStoreEliminationPass.h"

#include "oir/OIRAnalysis.h"
#include "oir/OIRDataLayout.h"
#include "oir/OIRScalarOpt.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

struct MemoryEntry {
    oir::Value *ptr = nullptr;
    oir::Value *value = nullptr;
    oir::Instruction *inst = nullptr;
    bool is_store = false;
    bool is_load = false;
};

const oir::AllocaInst *alloca_base(const oir::Value *value) {
    if (auto *alloca = dynamic_cast<const oir::AllocaInst *>(value)) {
        return alloca;
    }
    if (auto *gep = dynamic_cast<const oir::GetElementPtrInst *>(value)) {
        return alloca_base(gep->base_ptr());
    }
    return nullptr;
}

oir::AllocaInst *alloca_base(oir::Value *value) {
    return const_cast<oir::AllocaInst *>(alloca_base(static_cast<const oir::Value *>(value)));
}

struct AllocaWriteOnlyState {
    bool read = false;
    bool escaped = false;
    std::vector<oir::Instruction *> writes;
};

void mark_pointer_operand_escapes(
    oir::Instruction &inst, oir::Value *operand,
    std::unordered_map<oir::AllocaInst *, AllocaWriteOnlyState> &states) {
    auto *base = alloca_base(operand);
    if (base == nullptr) {
        return;
    }

    if (auto *gep = dynamic_cast<oir::GetElementPtrInst *>(&inst)) {
        if (gep->base_ptr() == operand) {
            return;
        }
    }
    if (auto *load = dynamic_cast<oir::LoadInst *>(&inst)) {
        if (load->ptr() == operand) {
            return;
        }
    }
    if (auto *store = dynamic_cast<oir::StoreInst *>(&inst)) {
        if (store->ptr() == operand) {
            return;
        }
    }
    if (auto *memzero = dynamic_cast<oir::MemZeroInst *>(&inst)) {
        if (memzero->ptr() == operand || memzero->byte_count() == operand ||
            memzero->byte_value() == operand) {
            return;
        }
    }

    states[base].escaped = true;
}

void collect_write_only_alloca_state(
    oir::Function &function, std::unordered_map<oir::AllocaInst *, AllocaWriteOnlyState> &states) {
    for (auto &block : function.blocks()) {
        for (auto &inst_ptr : block->instructions()) {
            auto *inst = inst_ptr.get();
            if (auto *alloca = dynamic_cast<oir::AllocaInst *>(inst)) {
                states.try_emplace(alloca);
            }

            if (auto *load = dynamic_cast<oir::LoadInst *>(inst)) {
                if (auto *base = alloca_base(load->ptr())) {
                    states[base].read = true;
                }
            } else if (auto *store = dynamic_cast<oir::StoreInst *>(inst)) {
                if (auto *base = alloca_base(store->ptr())) {
                    states[base].writes.push_back(store);
                }
                if (auto *base = alloca_base(store->value())) {
                    states[base].escaped = true;
                }
            } else if (auto *memzero = dynamic_cast<oir::MemZeroInst *>(inst)) {
                if (auto *base = alloca_base(memzero->ptr())) {
                    states[base].writes.push_back(memzero);
                }
            } else if (auto *ret = dynamic_cast<oir::ReturnInst *>(inst)) {
                if (ret->has_value()) {
                    if (auto *base = alloca_base(ret->value())) {
                        states[base].escaped = true;
                    }
                }
            } else if (auto *call = dynamic_cast<oir::CallInst *>(inst)) {
                for (auto *arg : call->args()) {
                    if (auto *base = alloca_base(arg)) {
                        states[base].escaped = true;
                    }
                }
            }

            for (auto *operand : inst->operands()) {
                mark_pointer_operand_escapes(*inst, operand, states);
            }
        }
    }
}

bool collect_dead_write_only_alloca_stores(oir::Function &function,
                                           std::unordered_set<oir::Instruction *> &dead) {
    std::unordered_map<oir::AllocaInst *, AllocaWriteOnlyState> states;
    collect_write_only_alloca_state(function, states);

    bool changed = false;
    for (auto &[alloca, state] : states) {
        (void)alloca;
        if (state.read || state.escaped) {
            continue;
        }
        for (auto *write : state.writes) {
            changed = dead.insert(write).second || changed;
        }
    }
    return changed;
}

bool is_aggregate_zero_alloca_store(const oir::StoreInst &store) {
    auto *alloca = dynamic_cast<oir::AllocaInst *>(store.ptr());
    return alloca != nullptr && store.value()->type() == alloca->allocated_type() &&
           dynamic_cast<oir::ConstantZero *>(store.value()) != nullptr;
}

std::uint64_t fixed_type_size(oir::Type *type) {
    static const oir::DataLayout layout;
    try {
        return layout.fixed_alloc_size(type).value_or(0);
    } catch (const std::exception &) {
        // DSE cannot prove complete coverage without one exact, non-
        // overflowing allocation size.
        return 0;
    }
}

struct CoveredSlice {
    std::int64_t begin = 0;
    std::int64_t end = 0;
};

bool covers_whole_object(std::vector<CoveredSlice> slices, std::int64_t object_size) {
    if (object_size <= 0 || slices.empty()) {
        return false;
    }
    std::sort(slices.begin(), slices.end(), [](const CoveredSlice &lhs, const CoveredSlice &rhs) {
        return lhs.begin < rhs.begin || (lhs.begin == rhs.begin && lhs.end < rhs.end);
    });

    std::int64_t covered_end = 0;
    for (const auto &slice : slices) {
        if (slice.begin > covered_end) {
            return false;
        }
        if (slice.end > covered_end) {
            covered_end = slice.end;
            if (covered_end >= object_size) {
                return true;
            }
        }
    }
    return false;
}

bool slice_is_covered(const std::vector<CoveredSlice> &slices, CoveredSlice query) {
    if (query.begin < 0 || query.begin >= query.end) {
        return false;
    }
    std::int64_t covered_end = query.begin;
    std::vector<CoveredSlice> sorted = slices;
    std::sort(sorted.begin(), sorted.end(), [](const CoveredSlice &lhs, const CoveredSlice &rhs) {
        return lhs.begin < rhs.begin || (lhs.begin == rhs.begin && lhs.end < rhs.end);
    });
    for (const auto &slice : sorted) {
        if (slice.end <= covered_end) {
            continue;
        }
        if (slice.begin > covered_end) {
            return false;
        }
        covered_end = slice.end;
        if (covered_end >= query.end) {
            return true;
        }
    }
    return false;
}

std::optional<CoveredSlice> scalar_store_slice_for_alloca(const oir::StoreInst &store,
                                                          const oir::AllocaInst &alloca,
                                                          const oir::OIRAliasAnalysis &aa,
                                                          std::int64_t object_size) {
    if (!is_scalar_type(store.value()->type())) {
        return std::nullopt;
    }

    auto loc = aa.memory_location(store.ptr());
    if (loc.base != &alloca || !loc.offset || !loc.size) {
        return std::nullopt;
    }
    const auto begin = *loc.offset;
    const auto end = begin + static_cast<std::int64_t>(*loc.size);
    if (begin < 0 || begin >= end || end > object_size) {
        return std::nullopt;
    }
    return CoveredSlice{begin, end};
}

std::optional<CoveredSlice> memory_slice_for_alloca(oir::Value *ptr, const oir::AllocaInst &alloca,
                                                    const oir::OIRAliasAnalysis &aa) {
    auto loc = aa.memory_location(ptr);
    if (loc.base != &alloca || !loc.offset || !loc.size) {
        return std::nullopt;
    }
    const auto begin = *loc.offset;
    const auto end = begin + static_cast<std::int64_t>(*loc.size);
    if (begin < 0 || begin >= end) {
        return std::nullopt;
    }
    return CoveredSlice{begin, end};
}

bool gep_only_derives_alloca_pointer(const oir::GetElementPtrInst &gep,
                                     const oir::AllocaInst &alloca) {
    if (alloca_base(gep.base_ptr()) != &alloca) {
        return false;
    }
    for (auto *index : gep.indices()) {
        if (alloca_base(index) == &alloca) {
            return false;
        }
    }
    return true;
}

bool instruction_blocks_aggregate_zero_dse(const oir::Instruction &inst,
                                           const oir::AllocaInst &alloca,
                                           const oir::OIRAliasAnalysis &aa,
                                           const oir::FunctionModRefAnalysis &modref,
                                           const std::vector<CoveredSlice> &covered) {
    if (auto *gep = dynamic_cast<const oir::GetElementPtrInst *>(&inst)) {
        bool touches_alloca = alloca_base(gep->base_ptr()) == &alloca;
        for (auto *index : gep->indices()) {
            touches_alloca = touches_alloca || alloca_base(index) == &alloca;
        }
        if (!touches_alloca) {
            return false;
        }
        return !gep_only_derives_alloca_pointer(*gep, alloca);
    }

    if (auto *load = dynamic_cast<const oir::LoadInst *>(&inst)) {
        if (auto slice = memory_slice_for_alloca(load->ptr(), alloca, aa)) {
            return !slice_is_covered(covered, *slice);
        }
        return aa.alias(&alloca, load->ptr()) != oir::AliasResult::NoAlias;
    }

    if (auto *store = dynamic_cast<const oir::StoreInst *>(&inst)) {
        if (alloca_base(store->value()) == &alloca) {
            return true;
        }
        return aa.alias(&alloca, store->ptr()) != oir::AliasResult::NoAlias;
    }

    if (auto *memzero = dynamic_cast<const oir::MemZeroInst *>(&inst)) {
        if (alloca_base(memzero->byte_value()) == &alloca ||
            alloca_base(memzero->byte_count()) == &alloca) {
            return true;
        }
        return aa.alias(&alloca, memzero->ptr()) != oir::AliasResult::NoAlias;
    }

    if (auto *call = dynamic_cast<const oir::CallInst *>(&inst)) {
        for (auto *arg : call->args()) {
            if (alloca_base(arg) == &alloca) {
                return true;
            }
        }
        return modref.call_may_read(*call, &alloca, aa) ||
               modref.call_may_clobber(*call, &alloca, aa);
    }

    if (auto *ret = dynamic_cast<const oir::ReturnInst *>(&inst)) {
        return ret->has_value() && alloca_base(ret->value()) == &alloca;
    }

    for (auto *operand : inst.operands()) {
        if (alloca_base(operand) == &alloca) {
            return true;
        }
    }
    return false;
}

bool later_same_block_stores_cover_zeroed_alloca(const oir::StoreInst &zero_store,
                                                 const oir::OIRAliasAnalysis &aa,
                                                 const oir::FunctionModRefAnalysis &modref) {
    auto *alloca = dynamic_cast<oir::AllocaInst *>(zero_store.ptr());
    auto *block = zero_store.parent();
    if (alloca == nullptr || block == nullptr) {
        return false;
    }
    const auto object_size = static_cast<std::int64_t>(fixed_type_size(alloca->allocated_type()));
    if (object_size <= 0) {
        return false;
    }

    std::vector<CoveredSlice> covered;
    bool after_zero_store = false;
    for (const auto &inst_ptr : block->instructions()) {
        auto *inst = inst_ptr.get();
        if (inst == &zero_store) {
            after_zero_store = true;
            continue;
        }
        if (!after_zero_store) {
            continue;
        }

        if (auto *store = dynamic_cast<oir::StoreInst *>(inst)) {
            if (auto slice = scalar_store_slice_for_alloca(*store, *alloca, aa, object_size)) {
                covered.push_back(*slice);
                if (covers_whole_object(covered, object_size)) {
                    return true;
                }
                continue;
            }
        }

        if (instruction_blocks_aggregate_zero_dse(*inst, *alloca, aa, modref, covered)) {
            return false;
        }
    }
    return false;
}

bool collect_fully_overwritten_aggregate_zero_stores(oir::Function &function,
                                                     const oir::OIRAliasAnalysis &aa,
                                                     const oir::FunctionModRefAnalysis &modref,
                                                     std::unordered_set<oir::Instruction *> &dead) {
    bool changed = false;
    for (auto &block : function.blocks()) {
        for (auto &inst_ptr : block->instructions()) {
            auto *store = dynamic_cast<oir::StoreInst *>(inst_ptr.get());
            if (store == nullptr || dead.find(store) != dead.end() ||
                !is_aggregate_zero_alloca_store(*store)) {
                continue;
            }
            if (later_same_block_stores_cover_zeroed_alloca(*store, aa, modref)) {
                changed = dead.insert(store).second || changed;
            }
        }
    }
    return changed;
}

const oir::BasicBlock *nearest_common_dominator(const oir::DominatorTree &dom_tree,
                                                const oir::BasicBlock *lhs,
                                                const oir::BasicBlock *rhs) {
    auto *cursor = lhs;
    while (cursor != nullptr && !dom_tree.dominates(cursor, rhs)) {
        cursor = dom_tree.immediate_dominator(cursor);
    }
    return cursor;
}

bool block_prefix_uses_alloca(const oir::BasicBlock *block, const oir::Instruction *before,
                              const oir::AllocaInst *alloca) {
    for (const auto &inst : block->instructions()) {
        if (inst.get() == before) {
            return false;
        }
        for (auto *operand : inst->operands()) {
            if (alloca_base(operand) == alloca) {
                return true;
            }
        }
    }
    return false;
}

bool has_backedge_like_predecessor(const oir::DominatorTree &dom_tree,
                                   const oir::BasicBlock *block) {
    for (auto *pred : block->predecessors()) {
        if (pred != block && dom_tree.dominates(block, pred)) {
            return true;
        }
    }
    return false;
}

bool sink_alloca_zero_stores(oir::Function &function, const oir::DominatorTree &dom_tree,
                             std::unordered_set<oir::Instruction *> &dead) {
    std::vector<oir::StoreInst *> candidates;
    for (auto &block : function.blocks()) {
        if (!dom_tree.is_reachable(block.get())) {
            continue;
        }
        for (auto &inst : block->instructions()) {
            auto *store = dynamic_cast<oir::StoreInst *>(inst.get());
            if (store != nullptr && is_aggregate_zero_alloca_store(*store)) {
                candidates.push_back(store);
            }
        }
    }

    bool changed = false;
    for (auto *store : candidates) {
        if (dead.find(store) != dead.end()) {
            continue;
        }
        auto *alloca = dynamic_cast<oir::AllocaInst *>(store->ptr());
        auto *store_block = store->parent();
        if (store_block == nullptr || !dom_tree.is_reachable(store_block)) {
            continue;
        }
        if (block_prefix_uses_alloca(store_block, store, alloca)) {
            continue;
        }

        const oir::BasicBlock *target = nullptr;
        std::vector<oir::BasicBlock *> use_blocks;
        bool invalid = false;
        for (auto &block : function.blocks()) {
            for (auto &inst : block->instructions()) {
                if (inst.get() == store) {
                    continue;
                }
                bool touches_alloca = false;
                for (auto *operand : inst->operands()) {
                    if (alloca_base(operand) == alloca) {
                        touches_alloca = true;
                        break;
                    }
                }
                if (!touches_alloca) {
                    continue;
                }
                if (std::find(use_blocks.begin(), use_blocks.end(), block.get()) ==
                    use_blocks.end()) {
                    use_blocks.push_back(block.get());
                }
                if (!dom_tree.is_reachable(block.get())) {
                    invalid = true;
                    break;
                }
                target = target == nullptr
                             ? block.get()
                             : nearest_common_dominator(dom_tree, target, block.get());
                if (target == nullptr) {
                    invalid = true;
                    break;
                }
            }
            if (invalid) {
                break;
            }
        }
        while (target != nullptr && target != store_block &&
               has_backedge_like_predecessor(dom_tree, target)) {
            target = dom_tree.immediate_dominator(target);
        }
        bool refined = true;
        while (target != nullptr && refined) {
            refined = false;
            for (auto *succ : target->successors()) {
                bool dominates_all_uses = true;
                for (auto *use_block : use_blocks) {
                    if (!dom_tree.dominates(succ, use_block)) {
                        dominates_all_uses = false;
                        break;
                    }
                }
                if (dominates_all_uses) {
                    target = succ;
                    refined = true;
                    break;
                }
            }
        }
        while (target != nullptr && target != store_block &&
               has_backedge_like_predecessor(dom_tree, target)) {
            target = dom_tree.immediate_dominator(target);
        }
        if (invalid || target == nullptr || target == store_block ||
            !dom_tree.dominates(store_block, target)) {
            continue;
        }

        auto *mutable_target = const_cast<oir::BasicBlock *>(target);
        auto insert_pos = mutable_target->instructions().begin();
        while (insert_pos != mutable_target->instructions().end() &&
               dynamic_cast<oir::PhiInst *>(insert_pos->get()) != nullptr) {
            if (block_prefix_uses_alloca(mutable_target, insert_pos->get(), alloca)) {
                invalid = true;
                break;
            }
            ++insert_pos;
        }
        if (invalid || block_prefix_uses_alloca(mutable_target,
                                                insert_pos == mutable_target->instructions().end()
                                                    ? nullptr
                                                    : insert_pos->get(),
                                                alloca)) {
            continue;
        }

        auto sunk = std::make_unique<oir::StoreInst>(store->type(), store->value(), store->ptr(),
                                                     mutable_target);
        sunk->set_parent(mutable_target);
        mutable_target->instructions().insert(insert_pos, std::move(sunk));
        dead.insert(store);
        changed = true;
    }
    return changed;
}

bool erase_instructions(oir::Function &function,
                        const std::unordered_set<oir::Instruction *> &dead) {
    if (dead.empty()) {
        return false;
    }
    for (auto *inst : dead) {
        inst->drop_all_operands();
    }
    for (auto &block : function.blocks()) {
        for (auto it = block->instructions().begin(); it != block->instructions().end();) {
            if (dead.find(it->get()) != dead.end()) {
                it = block->instructions().erase(it);
            } else {
                ++it;
            }
        }
    }
    return true;
}

void invalidate_aliasing(std::vector<MemoryEntry> &memory, oir::Value *ptr,
                         const oir::OIRAliasAnalysis &aa) {
    memory.erase(std::remove_if(memory.begin(), memory.end(),
                                [&](const MemoryEntry &entry) {
                                    return aa.alias(ptr, entry.ptr) != oir::AliasResult::NoAlias;
                                }),
                 memory.end());
}

void invalidate_for_call(std::vector<MemoryEntry> &memory, const oir::CallInst &call,
                         const oir::OIRAliasAnalysis &aa, const oir::FunctionModRefAnalysis &modref,
                         bool preserve_stores_read_by_call) {
    memory.erase(std::remove_if(memory.begin(), memory.end(),
                                [&](const MemoryEntry &entry) {
                                    if (preserve_stores_read_by_call && entry.is_store &&
                                        modref.call_may_read(call, entry.ptr, aa)) {
                                        return true;
                                    }
                                    return modref.call_may_clobber(call, entry.ptr, aa);
                                }),
                 memory.end());
}

bool dse_instruction(oir::Instruction *inst, std::vector<MemoryEntry> &memory,
                     const oir::OIRAliasAnalysis &aa, const oir::FunctionModRefAnalysis &modref,
                     std::unordered_set<oir::Instruction *> &dead) {
    if (auto *store = dynamic_cast<oir::StoreInst *>(inst)) {
        bool changed = false;
        for (auto it = memory.rbegin(); it != memory.rend(); ++it) {
            auto alias = aa.alias(store->ptr(), it->ptr);
            if (alias == oir::AliasResult::NoAlias) {
                continue;
            }
            if (it->is_load) {
                break;
            }
            if (alias == oir::AliasResult::MustAlias && it->is_store) {
                changed = dead.insert(it->inst).second || changed;
            }
            break;
        }
        invalidate_aliasing(memory, store->ptr(), aa);
        memory.push_back({store->ptr(), store->value(), store, true, false});
        return changed;
    }

    if (auto *load = dynamic_cast<oir::LoadInst *>(inst)) {
        memory.push_back({load->ptr(), load, load, false, true});
        return false;
    }

    if (auto *call = dynamic_cast<oir::CallInst *>(inst)) {
        invalidate_for_call(memory, *call, aa, modref, true);
    }
    return false;
}

bool dse_block(oir::BasicBlock &block, const oir::OIRAliasAnalysis &aa,
               const oir::FunctionModRefAnalysis &modref,
               std::unordered_set<oir::Instruction *> &dead) {
    bool changed = false;
    std::vector<MemoryEntry> memory;
    for (auto &inst_ptr : block.instructions()) {
        changed = dse_instruction(inst_ptr.get(), memory, aa, modref, dead) || changed;
    }
    return changed;
}

bool is_redundant_loaded_value_writeback(const oir::StoreInst &store,
                                         const oir::OIRAliasAnalysis &aa,
                                         const oir::FunctionModRefAnalysis &modref) {
    auto *load = dynamic_cast<oir::LoadInst *>(store.value());
    if (load == nullptr || load->ptr() != store.ptr() || load->parent() != store.parent()) {
        return false;
    }

    bool after_load = false;
    for (const auto &inst_ptr : load->parent()->instructions()) {
        auto *inst = inst_ptr.get();
        if (inst == load) {
            after_load = true;
            continue;
        }
        if (!after_load) {
            continue;
        }
        if (inst == &store) {
            return true;
        }

        if (auto *between_store = dynamic_cast<oir::StoreInst *>(inst)) {
            auto alias = aa.alias(load->ptr(), between_store->ptr());
            if (alias != oir::AliasResult::NoAlias && between_store->value() != load) {
                return false;
            }
            continue;
        }

        if (auto *call = dynamic_cast<oir::CallInst *>(inst)) {
            if (modref.call_may_clobber(*call, load->ptr(), aa)) {
                return false;
            }
        }
    }

    return false;
}

void collect_redundant_loaded_value_writebacks(oir::Function &function,
                                               const oir::OIRAliasAnalysis &aa,
                                               const oir::FunctionModRefAnalysis &modref,
                                               std::unordered_set<oir::Instruction *> &dead) {
    for (auto &block : function.blocks()) {
        for (auto &inst_ptr : block->instructions()) {
            auto *store = dynamic_cast<oir::StoreInst *>(inst_ptr.get());
            if (store != nullptr && dead.find(store) == dead.end() &&
                is_redundant_loaded_value_writeback(*store, aa, modref)) {
                dead.insert(store);
            }
        }
    }
}

void dse_dom_block(const oir::DominatorTree &dom_tree, const oir::BasicBlock *block,
                   const oir::OIRAliasAnalysis &aa, std::vector<MemoryEntry> memory,
                   const oir::FunctionModRefAnalysis &modref,
                   std::unordered_set<oir::Instruction *> &dead) {
    auto *mutable_block = const_cast<oir::BasicBlock *>(block);
    for (auto &inst_ptr : mutable_block->instructions()) {
        dse_instruction(inst_ptr.get(), memory, aa, modref, dead);
    }

    for (auto *child : dom_tree.children(block)) {
        std::vector<MemoryEntry> child_memory;
        if (child->predecessors().size() == 1 && child->predecessors().front() == block &&
            block->successors().size() == 1) {
            child_memory = memory;
        }
        dse_dom_block(dom_tree, child, aa, std::move(child_memory), modref, dead);
    }
}

} // namespace

bool eliminate_dead_stores(oir::Module &module, Stats &stats) {
    bool changed = false;
    oir::OIRAliasAnalysis aa;
    oir::FunctionModRefAnalysis modref(module);
    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        std::unordered_set<oir::Instruction *> dead;
        if (function->entry_block() != nullptr) {
            oir::DominatorTree dom_tree(*function);
            dse_dom_block(dom_tree, function->entry_block(), aa, {}, modref, dead);
            collect_fully_overwritten_aggregate_zero_stores(*function, aa, modref, dead);
            sink_alloca_zero_stores(*function, dom_tree, dead);
            for (auto &block : function->blocks()) {
                if (!dom_tree.is_reachable(block.get())) {
                    dse_block(*block, aa, modref, dead);
                }
            }
        } else {
            for (auto &block : function->blocks()) {
                dse_block(*block, aa, modref, dead);
            }
        }
        collect_dead_write_only_alloca_stores(*function, dead);
        collect_redundant_loaded_value_writebacks(*function, aa, modref, dead);
        if (erase_instructions(*function, dead)) {
            stats.dse += static_cast<unsigned>(dead.size());
            changed = true;
        }
    }
    return changed;
}

} // namespace pass::oir_opt

namespace pass {

std::string_view OIRDeadStoreEliminationPass::name() const {
    return "OIRDeadStoreEliminationPass";
}

PassKind OIRDeadStoreEliminationPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRDeadStoreEliminationPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRDeadStoreEliminationPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::eliminate_dead_stores(module, stats);
            changed |= oir_opt::eliminate_dead_code(module, stats);
            return changed;
        });
}

} // namespace pass
