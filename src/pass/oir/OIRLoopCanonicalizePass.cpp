#include "pass/oir/OIRLoopCanonicalizePass.h"

#include "oir/OIRAnalysis.h"
#include "oir/OIRCFGUtils.h"
#include "oir/OIRScalarOpt.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

bool contains_block(const oir::Loop &loop, const oir::BasicBlock *block) {
    return std::find(loop.blocks.begin(), loop.blocks.end(), block) != loop.blocks.end();
}

oir::BasicBlock *mut(const oir::BasicBlock *block) {
    return const_cast<oir::BasicBlock *>(block);
}

void insert_phi_before_non_phi(oir::BasicBlock *block, std::unique_ptr<oir::PhiInst> phi) {
    auto pos = block->instructions().begin();
    while (pos != block->instructions().end() &&
           dynamic_cast<oir::PhiInst *>(pos->get()) != nullptr) {
        ++pos;
    }
    block->instructions().insert(pos, std::move(phi));
}

bool has_phi_incoming_from(const oir::BasicBlock *block, const oir::BasicBlock *pred) {
    for (const auto &inst : block->instructions()) {
        auto *phi = dynamic_cast<const oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        for (const auto &incoming : phi->incoming()) {
            if (incoming.second == pred) {
                return true;
            }
        }
    }
    return false;
}

oir::Value *incoming_value_from(const oir::PhiInst &phi, const oir::BasicBlock *pred) {
    for (const auto &incoming : phi.incoming()) {
        if (incoming.second == pred) {
            return incoming.first;
        }
    }
    return nullptr;
}

oir::BasicBlock *split_edge_preserving_phis(oir::Function &function, oir::BasicBlock *pred,
                                            oir::BasicBlock *succ, const std::string &name) {
    auto *branch = dynamic_cast<oir::BranchInst *>(pred->terminator());
    if (branch == nullptr) {
        return nullptr;
    }

    auto *split = function.create_block(name);
    if (!oir::cfg::replace_branch_target(*branch, succ, split)) {
        function.erase_block(split);
        return nullptr;
    }

    oir::cfg::remove_edge_no_phi_update(pred, succ);
    oir::cfg::add_edge(pred, split);
    oir::cfg::append_unconditional_branch(*function.parent(), split, succ);
    oir::cfg::replace_phi_incoming_block(succ, pred, split);
    return split;
}

bool split_critical_edges(oir::Function &function, Stats &stats) {
    struct Edge {
        oir::BasicBlock *pred = nullptr;
        oir::BasicBlock *succ = nullptr;
    };
    std::vector<Edge> edges;
    for (auto &block : function.blocks()) {
        if (block->successors().size() < 2) {
            continue;
        }
        for (auto *succ : block->successors()) {
            if (succ->predecessors().size() > 1) {
                edges.push_back({block.get(), succ});
            }
        }
    }

    bool changed = false;
    for (const auto &edge : edges) {
        if (edge.pred->parent() != &function || edge.succ->parent() != &function ||
            std::find(edge.pred->successors().begin(), edge.pred->successors().end(), edge.succ) ==
                edge.pred->successors().end()) {
            continue;
        }
        if (split_edge_preserving_phis(function, edge.pred, edge.succ, "crit.split") != nullptr) {
            ++stats.cfg;
            ++stats.loop_canonicalize;
            changed = true;
        }
    }
    return changed;
}

std::vector<oir::BasicBlock *> outside_predecessors(const oir::Loop &loop) {
    std::vector<oir::BasicBlock *> out;
    for (auto *pred : loop.header->predecessors()) {
        if (!contains_block(loop, pred)) {
            out.push_back(pred);
        }
    }
    return out;
}

bool has_canonical_preheader(const oir::Loop &loop) {
    auto outside = outside_predecessors(loop);
    return outside.size() == 1 && outside.front()->successors().size() == 1 &&
           outside.front()->successors().front() == loop.header;
}

bool create_preheader(oir::Function &function, const oir::Loop &loop, Stats &stats) {
    auto outside = outside_predecessors(loop);
    if (outside.empty() || has_canonical_preheader(loop)) {
        return false;
    }

    auto *header = mut(loop.header);
    auto *preheader = function.create_block("loop.preheader");
    std::unordered_map<oir::PhiInst *, oir::Value *> replacement_incoming;

    for (auto &inst : header->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }

        std::vector<std::pair<oir::Value *, oir::BasicBlock *>> incoming;
        for (auto *pred : outside) {
            if (auto *value = incoming_value_from(*phi, pred)) {
                incoming.push_back({value, pred});
            }
        }
        if (incoming.empty()) {
            continue;
        }

        auto *first = incoming.front().first;
        bool all_same = true;
        for (const auto &item : incoming) {
            all_same = all_same && item.first == first;
        }
        if (all_same) {
            replacement_incoming[phi] = first;
            continue;
        }

        auto pre_phi = std::make_unique<oir::PhiInst>(
            phi->type(), preheader, phi->name().empty() ? "pre.lcssa" : phi->name() + ".pre");
        auto *raw = pre_phi.get();
        for (const auto &[value, pred] : incoming) {
            raw->add_incoming(value, pred);
        }
        insert_phi_before_non_phi(preheader, std::move(pre_phi));
        replacement_incoming[phi] = raw;
    }

    for (auto *pred : outside) {
        oir::cfg::replace_successor(pred, header, preheader);
    }
    oir::cfg::append_unconditional_branch(*function.parent(), preheader, header);
    for (const auto &[phi, value] : replacement_incoming) {
        phi->add_incoming(value, preheader);
    }

    ++stats.cfg;
    ++stats.loop_canonicalize;
    return true;
}

bool create_single_latch(oir::Function &function, const oir::Loop &loop, Stats &stats) {
    if (loop.latches.size() <= 1) {
        return false;
    }
    for (auto *latch : loop.latches) {
        if (latch == loop.header) {
            return false;
        }
    }

    auto *header = mut(loop.header);
    auto *new_latch = function.create_block("loop.latch");
    std::unordered_map<oir::PhiInst *, oir::Value *> replacement_incoming;
    for (auto &inst : header->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        auto latch_phi = std::make_unique<oir::PhiInst>(
            phi->type(), new_latch, phi->name().empty() ? "latch.phi" : phi->name() + ".latch");
        auto *raw = latch_phi.get();
        bool any = false;
        for (auto *latch : loop.latches) {
            if (auto *value = incoming_value_from(*phi, latch)) {
                raw->add_incoming(value, mut(latch));
                any = true;
            }
        }
        if (any) {
            insert_phi_before_non_phi(new_latch, std::move(latch_phi));
            replacement_incoming[phi] = raw;
        }
    }

    for (auto *const_latch : loop.latches) {
        auto *latch = mut(const_latch);
        oir::cfg::replace_successor(latch, header, new_latch);
    }
    oir::cfg::append_unconditional_branch(*function.parent(), new_latch, header);
    for (const auto &[phi, value] : replacement_incoming) {
        phi->add_incoming(value, new_latch);
    }

    ++stats.cfg;
    ++stats.loop_canonicalize;
    return true;
}

bool split_dedicated_exits(oir::Function &function, const oir::Loop &loop, Stats &stats) {
    struct Edge {
        oir::BasicBlock *pred = nullptr;
        oir::BasicBlock *exit = nullptr;
    };
    std::vector<Edge> edges;
    for (auto *const_block : loop.blocks) {
        auto *block = mut(const_block);
        for (auto *succ : block->successors()) {
            if (contains_block(loop, succ)) {
                continue;
            }
            bool has_outside_pred = false;
            for (auto *pred : succ->predecessors()) {
                if (!contains_block(loop, pred)) {
                    has_outside_pred = true;
                    break;
                }
            }
            if (has_outside_pred) {
                edges.push_back({block, succ});
            }
        }
    }

    bool changed = false;
    for (const auto &edge : edges) {
        if (edge.pred->parent() != &function || edge.exit->parent() != &function) {
            continue;
        }
        if (split_edge_preserving_phis(function, edge.pred, edge.exit, "loop.exit") != nullptr) {
            ++stats.cfg;
            ++stats.loop_canonicalize;
            changed = true;
        }
    }
    return changed;
}

std::vector<oir::BasicBlock *> exit_blocks(const oir::Loop &loop) {
    std::vector<oir::BasicBlock *> exits;
    for (auto *block : loop.blocks) {
        for (auto *succ : block->successors()) {
            if (contains_block(loop, succ)) {
                continue;
            }
            if (std::find(exits.begin(), exits.end(), succ) == exits.end()) {
                exits.push_back(succ);
            }
        }
    }
    return exits;
}

bool definition_dominates_exit_preds(const oir::Instruction &def, oir::BasicBlock *exit,
                                     const oir::Loop &loop,
                                     const oir::DominatorTree &dom_tree) {
    for (auto *pred : exit->predecessors()) {
        if (contains_block(loop, pred) && !dom_tree.dominates(def.parent(), pred)) {
            return false;
        }
    }
    return true;
}

bool apply_lcssa(oir::Function &function, const oir::Loop &loop, Stats &stats) {
    oir::DominatorTree dom_tree(function);
    auto exits = exit_blocks(loop);
    if (exits.empty()) {
        return false;
    }

    bool changed = false;
    std::unordered_map<oir::Instruction *, std::unordered_map<oir::BasicBlock *, oir::PhiInst *>>
        exit_phis;

    for (auto *const_block : loop.blocks) {
        for (auto &inst_ptr : mut(const_block)->instructions()) {
            auto *def = inst_ptr.get();
            if (def->type() == nullptr || def->type()->is_void() || !def->has_uses()) {
                continue;
            }

            auto uses = def->uses();
            for (const auto &use : uses) {
                auto *user_inst = dynamic_cast<oir::Instruction *>(use.user);
                if (user_inst == nullptr || contains_block(loop, user_inst->parent())) {
                    continue;
                }
                if (dynamic_cast<oir::PhiInst *>(user_inst) != nullptr) {
                    continue;
                }

                oir::PhiInst *chosen = nullptr;
                for (auto *exit : exits) {
                    if (!dom_tree.dominates(exit, user_inst->parent()) ||
                        !definition_dominates_exit_preds(*def, exit, loop, dom_tree)) {
                        continue;
                    }
                    auto &slot = exit_phis[def][exit];
                    if (slot == nullptr) {
                        auto phi = std::make_unique<oir::PhiInst>(
                            def->type(), exit,
                            def->name().empty() ? "lcssa" : def->name() + ".lcssa");
                        slot = phi.get();
                        for (auto *pred : exit->predecessors()) {
                            if (contains_block(loop, pred)) {
                                slot->add_incoming(def, pred);
                            }
                        }
                        insert_phi_before_non_phi(exit, std::move(phi));
                    }
                    chosen = slot;
                    break;
                }
                if (chosen != nullptr && user_inst->operand(use.operand_index) == def) {
                    user_inst->set_operand(use.operand_index, chosen);
                    changed = true;
                }
            }
        }
    }

    if (changed) {
        ++stats.loop_canonicalize;
    }
    return changed;
}

bool canonicalize_function(oir::Function &function, Stats &stats) {
    if (function.is_external() || function.entry_block() == nullptr) {
        return false;
    }

    bool changed = false;
    constexpr unsigned kMaxIterations = 64;
    for (unsigned iteration = 0; iteration < kMaxIterations; ++iteration) {
        bool iteration_changed = false;
        iteration_changed |= split_critical_edges(function, stats);

        oir::DominatorTree dom_tree(function);
        oir::LoopInfo loop_info(function, dom_tree);
        auto loops = loop_info.loops();
        std::sort(loops.begin(), loops.end(), [](const oir::Loop &lhs, const oir::Loop &rhs) {
            return lhs.blocks.size() < rhs.blocks.size();
        });

        for (const auto &loop : loops) {
            if (create_preheader(function, loop, stats) ||
                create_single_latch(function, loop, stats) ||
                split_dedicated_exits(function, loop, stats)) {
                iteration_changed = true;
                break;
            }
        }

        changed |= iteration_changed;
        if (!iteration_changed) {
            break;
        }
    }

    oir::DominatorTree dom_tree(function);
    oir::LoopInfo loop_info(function, dom_tree);
    auto loops = loop_info.loops();
    std::sort(loops.begin(), loops.end(), [](const oir::Loop &lhs, const oir::Loop &rhs) {
        return lhs.blocks.size() < rhs.blocks.size();
    });
    for (const auto &loop : loops) {
        changed |= apply_lcssa(function, loop, stats);
    }
    return changed;
}

} // namespace

bool canonicalize_loops(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        changed |= canonicalize_function(*function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt

namespace pass {

std::string_view OIRLoopCanonicalizePass::name() const {
    return "OIRLoopCanonicalizePass";
}

PassKind OIRLoopCanonicalizePass::kind() const {
    return PassKind::Transform;
}

PassResult OIRLoopCanonicalizePass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRLoopCanonicalizePass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::canonicalize_loops(module, stats);
            changed |= oir_opt::cleanup_cfg(module, stats);
            return changed;
        });
}

} // namespace pass
