#include "../../include/oir/OIRAnalysis.h"
#include "../../include/oir/OIRScalarOpt.h"

#include <algorithm>
#include <memory>
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

bool dse_block(oir::BasicBlock &block, const oir::OIRAliasAnalysis &aa,
               const oir::FunctionModRefAnalysis &modref,
               std::unordered_set<oir::Instruction *> &dead) {
    bool changed = false;
    std::vector<MemoryEntry> memory;
    for (auto &inst_ptr : block.instructions()) {
        auto *inst = inst_ptr.get();
        if (auto *store = dynamic_cast<oir::StoreInst *>(inst)) {
            for (auto it = memory.rbegin(); it != memory.rend(); ++it) {
                auto alias = aa.alias(store->ptr(), it->ptr);
                if (alias == oir::AliasResult::NoAlias) {
                    continue;
                }
                if (it->is_load) {
                    break;
                }
                if (alias == oir::AliasResult::MustAlias && it->is_store) {
                    dead.insert(it->inst);
                    changed = true;
                }
                break;
            }
            invalidate_aliasing(memory, store->ptr(), aa);
            memory.push_back({store->ptr(), store->value(), store, true, false});
            continue;
        }
        if (auto *load = dynamic_cast<oir::LoadInst *>(inst)) {
            memory.push_back({load->ptr(), load, load, false, true});
            continue;
        }
        if (auto *call = dynamic_cast<oir::CallInst *>(inst)) {
            invalidate_for_call(memory, *call, aa, modref, true);
        }
    }
    return changed;
}

bool dle_block(oir::BasicBlock &block, const oir::OIRAliasAnalysis &aa,
               const oir::FunctionModRefAnalysis &modref, ReplacementMap &replacements) {
    bool changed = false;
    std::vector<MemoryEntry> memory;
    for (auto &inst_ptr : block.instructions()) {
        auto *inst = inst_ptr.get();
        if (auto *load = dynamic_cast<oir::LoadInst *>(inst)) {
            for (auto it = memory.rbegin(); it != memory.rend(); ++it) {
                auto alias = aa.alias(load->ptr(), it->ptr);
                if (alias == oir::AliasResult::NoAlias) {
                    continue;
                }
                if (alias == oir::AliasResult::MustAlias && it->value != nullptr &&
                    it->value->type() == load->type()) {
                    replacements[load] = it->value;
                    changed = true;
                }
                break;
            }
            memory.push_back({load->ptr(), load, load, false, true});
            continue;
        }
        if (auto *store = dynamic_cast<oir::StoreInst *>(inst)) {
            invalidate_aliasing(memory, store->ptr(), aa);
            memory.push_back({store->ptr(), store->value(), store, true, false});
            continue;
        }
        if (auto *call = dynamic_cast<oir::CallInst *>(inst)) {
            invalidate_for_call(memory, *call, aa, modref, false);
        }
    }
    return changed;
}

void dle_dom_block(const oir::DominatorTree &dom_tree, const oir::BasicBlock *block,
                   const oir::OIRAliasAnalysis &aa, std::vector<MemoryEntry> memory,
                   const oir::FunctionModRefAnalysis &modref, ReplacementMap &replacements) {
    auto *mutable_block = const_cast<oir::BasicBlock *>(block);
    for (auto &inst_ptr : mutable_block->instructions()) {
        auto *inst = inst_ptr.get();
        if (auto *load = dynamic_cast<oir::LoadInst *>(inst)) {
            for (auto it = memory.rbegin(); it != memory.rend(); ++it) {
                auto alias = aa.alias(load->ptr(), it->ptr);
                if (alias == oir::AliasResult::NoAlias) {
                    continue;
                }
                if (alias == oir::AliasResult::MustAlias && it->value != nullptr &&
                    it->value->type() == load->type()) {
                    replacements[load] = it->value;
                }
                break;
            }
            memory.push_back({load->ptr(), load, load, false, true});
            continue;
        }
        if (auto *store = dynamic_cast<oir::StoreInst *>(inst)) {
            invalidate_aliasing(memory, store->ptr(), aa);
            memory.push_back({store->ptr(), store->value(), store, true, false});
            continue;
        }
        if (auto *call = dynamic_cast<oir::CallInst *>(inst)) {
            invalidate_for_call(memory, *call, aa, modref, false);
        }
    }

    for (auto *child : dom_tree.children(block)) {
        std::vector<MemoryEntry> child_memory;
        if (child->predecessors().size() == 1 && child->predecessors().front() == block) {
            child_memory = memory;
        }
        dle_dom_block(dom_tree, child, aa, std::move(child_memory), modref, replacements);
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
        for (auto &block : function->blocks()) {
            dse_block(*block, aa, modref, dead);
        }
        if (erase_instructions(*function, dead)) {
            stats.dse += static_cast<unsigned>(dead.size());
            changed = true;
        }
    }
    return changed;
}

bool eliminate_dead_loads(oir::Module &module, Stats &stats) {
    ReplacementMap replacements;
    oir::OIRAliasAnalysis aa;
    oir::FunctionModRefAnalysis modref(module);
    for (auto &function : module.functions()) {
        if (function->is_external() || function->entry_block() == nullptr) {
            continue;
        }
        oir::DominatorTree dom_tree(*function);
        dle_dom_block(dom_tree, function->entry_block(), aa, {}, modref, replacements);
        for (auto &block : function->blocks()) {
            dle_block(*block, aa, modref, replacements);
        }
    }
    const unsigned replaced = apply_replacements(module, replacements);
    if (replaced == 0) {
        return false;
    }
    stats.dle += static_cast<unsigned>(replacements.size());
    return true;
}

} // namespace pass::oir_opt
