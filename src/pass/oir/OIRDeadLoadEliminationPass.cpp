#include "pass/oir/OIRDeadLoadEliminationPass.h"

#include "oir/OIRAnalysis.h"
#include "oir/OIRScalarOpt.h"

#include <algorithm>
#include <utility>
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

bool is_large_function(const oir::Function &function) {
    std::size_t block_count = 0;
    std::size_t instruction_count = 0;
    for (const auto &block : function.blocks()) {
        ++block_count;
        instruction_count += block->instructions().size();
    }
    return block_count > 1000 || instruction_count > 8000;
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

void collect_memory_ssa_load_forwards(oir::Function &function, const oir::MemorySSA &memory_ssa,
                                      const oir::OIRAliasAnalysis &aa,
                                      ReplacementMap &replacements) {
    for (auto &block : function.blocks()) {
        for (auto &inst_ptr : block->instructions()) {
            auto *load = dynamic_cast<oir::LoadInst *>(inst_ptr.get());
            if (load == nullptr || replacements.find(load) != replacements.end()) {
                continue;
            }

            auto *clobber = memory_ssa.clobbering_access(*load);
            if (clobber == nullptr) {
                continue;
            }
            auto *store = dynamic_cast<oir::StoreInst *>(clobber->instruction());
            if (store == nullptr ||
                aa.alias(load->ptr(), store->ptr()) != oir::AliasResult::MustAlias ||
                store->value()->type() != load->type()) {
                continue;
            }
            replacements[load] = store->value();
        }
    }
}

} // namespace

bool eliminate_dead_loads(oir::Module &module, Stats &stats) {
    ReplacementMap replacements;
    oir::OIRAliasAnalysis aa;
    oir::FunctionModRefAnalysis modref(module);
    for (auto &function : module.functions()) {
        if (function->is_external() || function->entry_block() == nullptr) {
            continue;
        }
        if (!is_large_function(*function)) {
            oir::MemorySSA memory_ssa(*function, aa, modref);
            collect_memory_ssa_load_forwards(*function, memory_ssa, aa, replacements);
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

namespace pass {

std::string_view OIRDeadLoadEliminationPass::name() const {
    return "OIRDeadLoadEliminationPass";
}

PassKind OIRDeadLoadEliminationPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRDeadLoadEliminationPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRDeadLoadEliminationPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::eliminate_dead_loads(module, stats);
            changed |= oir_opt::eliminate_dead_code(module, stats);
            return changed;
        });
}

} // namespace pass
