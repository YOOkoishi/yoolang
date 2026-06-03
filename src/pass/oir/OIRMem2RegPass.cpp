#include "pass/oir/OIRMem2RegPass.h"

#include "oir/OIRScalarOpt.h"

#include "oir/OIRAnalysis.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

using BlockSet = std::unordered_set<oir::BasicBlock *>;

struct PromotableAlloca {
    oir::AllocaInst *alloca = nullptr;
    std::vector<oir::LoadInst *> loads;
    std::vector<oir::StoreInst *> stores;
    BlockSet def_blocks;
};

bool is_promotable_use(oir::AllocaInst *alloca, oir::Instruction *user,
                       PromotableAlloca &candidate) {
    if (auto *load = dynamic_cast<oir::LoadInst *>(user)) {
        if (load->ptr() != alloca) {
            return false;
        }
        candidate.loads.push_back(load);
        return true;
    }

    if (auto *store = dynamic_cast<oir::StoreInst *>(user)) {
        if (store->ptr() != alloca || store->value() == alloca) {
            return false;
        }
        candidate.stores.push_back(store);
        candidate.def_blocks.insert(store->parent());
        return true;
    }

    return false;
}

std::vector<PromotableAlloca> collect_promotable_allocas(oir::Function &function,
                                                         const oir::UseAnalysis &uses) {
    std::vector<PromotableAlloca> allocas;
    auto *entry = function.entry_block();
    if (entry == nullptr) {
        return allocas;
    }

    for (auto &inst : entry->instructions()) {
        auto *alloca = dynamic_cast<oir::AllocaInst *>(inst.get());
        if (alloca == nullptr || !is_scalar_type(alloca->allocated_type())) {
            continue;
        }

        PromotableAlloca candidate;
        candidate.alloca = alloca;
        bool promotable = true;
        for (auto *user : uses.users(alloca)) {
            if (!is_promotable_use(alloca, user, candidate)) {
                promotable = false;
                break;
            }
        }
        if (promotable) {
            allocas.push_back(std::move(candidate));
        }
    }
    return allocas;
}

std::unordered_map<oir::BasicBlock *, BlockSet>
compute_dominance_frontiers(oir::Function &function, const oir::DominatorTree &dom_tree) {
    std::unordered_map<oir::BasicBlock *, BlockSet> frontiers;
    for (auto &block : function.blocks()) {
        frontiers[block.get()];
    }

    for (auto &block_ptr : function.blocks()) {
        auto *block = block_ptr.get();
        if (!dom_tree.is_reachable(block) || block->predecessors().size() < 2) {
            continue;
        }

        auto *idom = const_cast<oir::BasicBlock *>(dom_tree.immediate_dominator(block));
        for (auto *pred : block->predecessors()) {
            if (!dom_tree.is_reachable(pred)) {
                continue;
            }
            auto *runner = pred;
            while (runner != nullptr && runner != idom) {
                frontiers[runner].insert(block);
                runner = const_cast<oir::BasicBlock *>(dom_tree.immediate_dominator(runner));
            }
        }
    }

    return frontiers;
}

std::string promoted_phi_name(const oir::AllocaInst &alloca) {
    return alloca.name().empty() ? "mem2reg" : alloca.name() + ".ssa";
}

oir::PhiInst *insert_phi(oir::BasicBlock *block, oir::Type *type, const std::string &name) {
    auto phi = std::make_unique<oir::PhiInst>(type, block, name);
    auto *raw = phi.get();
    raw->set_parent(block);

    auto insert_pos = block->instructions().begin();
    while (insert_pos != block->instructions().end() &&
           (*insert_pos)->op() == oir::Instruction::OpID::Phi) {
        ++insert_pos;
    }
    block->instructions().insert(insert_pos, std::move(phi));
    return raw;
}

std::unordered_map<oir::AllocaInst *, std::unordered_map<oir::BasicBlock *, oir::PhiInst *>>
place_phi_nodes(const std::vector<PromotableAlloca> &allocas,
                const std::unordered_map<oir::BasicBlock *, BlockSet> &frontiers) {
    std::unordered_map<oir::AllocaInst *, std::unordered_map<oir::BasicBlock *, oir::PhiInst *>>
        phis;

    for (const auto &candidate : allocas) {
        std::deque<oir::BasicBlock *> worklist(candidate.def_blocks.begin(),
                                               candidate.def_blocks.end());
        BlockSet queued(candidate.def_blocks.begin(), candidate.def_blocks.end());

        while (!worklist.empty()) {
            auto *block = worklist.front();
            worklist.pop_front();

            auto found = frontiers.find(block);
            if (found == frontiers.end()) {
                continue;
            }
            for (auto *frontier_block : found->second) {
                auto &alloca_phis = phis[candidate.alloca];
                if (alloca_phis.find(frontier_block) != alloca_phis.end()) {
                    continue;
                }

                auto *phi = insert_phi(frontier_block, candidate.alloca->allocated_type(),
                                       promoted_phi_name(*candidate.alloca));
                alloca_phis[frontier_block] = phi;
                if (queued.insert(frontier_block).second) {
                    worklist.push_back(frontier_block);
                }
            }
        }
    }

    return phis;
}

bool has_promoted_phi(
    const std::unordered_map<oir::AllocaInst *,
                             std::unordered_map<oir::BasicBlock *, oir::PhiInst *>> &phis,
    oir::AllocaInst *alloca, oir::BasicBlock *block) {
    auto found = phis.find(alloca);
    return found != phis.end() && found->second.find(block) != found->second.end();
}

oir::Value *current_value(oir::Module &module,
                          const std::unordered_map<oir::AllocaInst *, oir::Value *> &current,
                          oir::AllocaInst *alloca) {
    auto found = current.find(alloca);
    if (found != current.end()) {
        return found->second;
    }
    return module.create_undef(alloca->allocated_type());
}

void rename_block(
    oir::Module &module, const oir::DominatorTree &dom_tree, oir::BasicBlock *block,
    const std::vector<PromotableAlloca> &allocas,
    const std::unordered_map<oir::AllocaInst *,
                             std::unordered_map<oir::BasicBlock *, oir::PhiInst *>> &phis,
    std::unordered_map<oir::AllocaInst *, oir::Value *> &current, ReplacementMap &replacements,
    std::unordered_set<oir::Instruction *> &dead) {
    std::vector<std::pair<oir::AllocaInst *, oir::Value *>> saved;

    for (const auto &candidate : allocas) {
        auto phi_map = phis.find(candidate.alloca);
        if (phi_map == phis.end()) {
            continue;
        }
        auto phi = phi_map->second.find(block);
        if (phi == phi_map->second.end()) {
            continue;
        }
        saved.push_back({candidate.alloca, current[candidate.alloca]});
        current[candidate.alloca] = phi->second;
    }

    for (auto &inst_ptr : block->instructions()) {
        auto *inst = inst_ptr.get();
        if (auto *alloca = dynamic_cast<oir::AllocaInst *>(inst)) {
            if (std::any_of(allocas.begin(), allocas.end(), [alloca](const auto &candidate) {
                    return candidate.alloca == alloca;
                })) {
                dead.insert(inst);
            }
            continue;
        }

        if (auto *load = dynamic_cast<oir::LoadInst *>(inst)) {
            auto *alloca = dynamic_cast<oir::AllocaInst *>(load->ptr());
            if (alloca != nullptr &&
                std::any_of(allocas.begin(), allocas.end(), [alloca](const auto &candidate) {
                    return candidate.alloca == alloca;
                })) {
                replacements[load] = current_value(module, current, alloca);
                dead.insert(load);
            }
            continue;
        }

        if (auto *store = dynamic_cast<oir::StoreInst *>(inst)) {
            auto *alloca = dynamic_cast<oir::AllocaInst *>(store->ptr());
            if (alloca != nullptr &&
                std::any_of(allocas.begin(), allocas.end(), [alloca](const auto &candidate) {
                    return candidate.alloca == alloca;
                })) {
                saved.push_back({alloca, current[alloca]});
                current[alloca] = store->value();
                dead.insert(store);
            }
        }
    }

    for (auto *succ : block->successors()) {
        for (const auto &candidate : allocas) {
            if (!has_promoted_phi(phis, candidate.alloca, succ)) {
                continue;
            }
            auto *phi = phis.at(candidate.alloca).at(succ);
            phi->add_incoming(current_value(module, current, candidate.alloca), block);
        }
    }

    for (auto *child : dom_tree.children(block)) {
        rename_block(module, dom_tree, const_cast<oir::BasicBlock *>(child), allocas, phis, current,
                     replacements, dead);
    }

    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
        if (it->second == nullptr) {
            current.erase(it->first);
        } else {
            current[it->first] = it->second;
        }
    }
}

void erase_dead_instructions(oir::Function &function,
                             const std::unordered_set<oir::Instruction *> &dead) {
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            if (dead.find(inst.get()) != dead.end()) {
                inst->drop_all_operands();
            }
        }
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
}

bool has_incoming_from(const oir::PhiInst &phi, oir::BasicBlock *pred) {
    for (const auto &incoming : phi.incoming()) {
        if (incoming.second == pred) {
            return true;
        }
    }
    return false;
}

void fill_missing_phi_incomings(
    oir::Module &module,
    const std::unordered_map<oir::AllocaInst *,
                             std::unordered_map<oir::BasicBlock *, oir::PhiInst *>> &phis) {
    for (const auto &[alloca, alloca_phis] : phis) {
        for (const auto &[block, phi] : alloca_phis) {
            for (auto *pred : block->predecessors()) {
                if (!has_incoming_from(*phi, pred)) {
                    phi->add_incoming(module.create_undef(alloca->allocated_type()), pred);
                }
            }
        }
    }
}

bool run_on_function(oir::Module &module, oir::Function &function, Stats &stats) {
    if (function.is_external() || function.entry_block() == nullptr) {
        return false;
    }

    oir::DominatorTree dom_tree(function);
    oir::UseAnalysis uses(function);
    auto allocas = collect_promotable_allocas(function, uses);
    allocas.erase(std::remove_if(allocas.begin(), allocas.end(),
                                 [&dom_tree](const PromotableAlloca &candidate) {
                                     auto reachable_load = [&dom_tree](oir::LoadInst *load) {
                                         return dom_tree.is_reachable(load->parent());
                                     };
                                     auto reachable_store = [&dom_tree](oir::StoreInst *store) {
                                         return dom_tree.is_reachable(store->parent());
                                     };
                                     return !std::all_of(candidate.loads.begin(),
                                                         candidate.loads.end(), reachable_load) ||
                                            !std::all_of(candidate.stores.begin(),
                                                         candidate.stores.end(), reachable_store);
                                 }),
                  allocas.end());
    if (allocas.empty()) {
        return false;
    }

    auto frontiers = compute_dominance_frontiers(function, dom_tree);
    auto phis = place_phi_nodes(allocas, frontiers);

    std::unordered_map<oir::AllocaInst *, oir::Value *> current;
    ReplacementMap replacements;
    std::unordered_set<oir::Instruction *> dead;
    rename_block(module, dom_tree, function.entry_block(), allocas, phis, current, replacements,
                 dead);
    fill_missing_phi_incomings(module, phis);
    apply_replacements(module, replacements);
    erase_dead_instructions(function, dead);

    stats.mem2reg += static_cast<unsigned>(dead.size());
    for (const auto &[alloca, alloca_phis] : phis) {
        (void)alloca;
        stats.mem2reg += static_cast<unsigned>(alloca_phis.size());
    }
    return !dead.empty() || !phis.empty();
}

} // namespace

bool promote_memory_to_registers(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        changed |= run_on_function(module, *function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt

namespace pass {

std::string_view OIRMem2RegPass::name() const {
    return "OIRMem2RegPass";
}

PassKind OIRMem2RegPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRMem2RegPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(context, "OIRMem2RegPass requires OIR module in pass context",
                                      [](oir::Module &module, oir_opt::Stats &stats) {
                                          bool changed = oir_opt::scalar_replacement_of_aggregates(
                                              module, stats);
                                          changed |=
                                              oir_opt::promote_memory_to_registers(module, stats);
                                          changed |= oir_opt::cleanup_cfg(module, stats);
                                          changed |= oir_opt::eliminate_dead_code(module, stats);
                                          return changed;
                                      });
}

} // namespace pass
