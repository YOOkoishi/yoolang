#include "pass/oir/OIRDeadStoreEliminationPass.h"

#include "oir/OIRAnalysis.h"
#include "oir/OIRScalarOpt.h"

#include <algorithm>
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

void mark_pointer_operand_escapes(oir::Instruction &inst, oir::Value *operand,
                                  std::unordered_map<oir::AllocaInst *,
                                                     AllocaWriteOnlyState> &states) {
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
    oir::Function &function,
    std::unordered_map<oir::AllocaInst *, AllocaWriteOnlyState> &states) {
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

bool collect_dead_write_only_alloca_stores(
    oir::Function &function, std::unordered_set<oir::Instruction *> &dead) {
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
    memory.erase(std::remove_if(memory.begin(), memory.end(), [&](const MemoryEntry &entry) {
                     return aa.alias(ptr, entry.ptr) != oir::AliasResult::NoAlias;
                 }),
                 memory.end());
}

void invalidate_for_call(std::vector<MemoryEntry> &memory, const oir::CallInst &call,
                         const oir::OIRAliasAnalysis &aa,
                         const oir::FunctionModRefAnalysis &modref,
                         bool preserve_stores_read_by_call) {
    memory.erase(std::remove_if(memory.begin(), memory.end(), [&](const MemoryEntry &entry) {
                     if (preserve_stores_read_by_call && entry.is_store &&
                         modref.call_may_read(call, entry.ptr, aa)) {
                         return true;
                     }
                     return modref.call_may_clobber(call, entry.ptr, aa);
                 }),
                 memory.end());
}

bool dse_instruction(oir::Instruction *inst, std::vector<MemoryEntry> &memory,
                     const oir::OIRAliasAnalysis &aa,
                     const oir::FunctionModRefAnalysis &modref,
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

void collect_redundant_loaded_value_writebacks(
    oir::Function &function, const oir::OIRAliasAnalysis &aa,
    const oir::FunctionModRefAnalysis &modref, std::unordered_set<oir::Instruction *> &dead) {
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
