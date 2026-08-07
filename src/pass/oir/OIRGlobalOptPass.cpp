#include "pass/oir/OIRGlobalOptPass.h"

#include "oir/OIRAnalysis.h"
#include "oir/OIRCFGUtils.h"
#include "oir/OIRScalarOpt.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pass::oir_opt {
namespace {

std::uint64_t scalar_element_count(oir::Type *type) {
    if (auto *array = dynamic_cast<oir::ArrayType *>(type)) {
        return array->element_count() * scalar_element_count(array->element_type());
    }
    return 1;
}

oir::Type *scalar_element_type(oir::Type *type) {
    while (auto *array = dynamic_cast<oir::ArrayType *>(type)) {
        type = array->element_type();
    }
    return type;
}

std::optional<std::uint64_t>
linear_index_for(oir::Type *type, const std::vector<std::int64_t> &indices, std::size_t &pos) {
    auto *array = dynamic_cast<oir::ArrayType *>(type);
    if (array == nullptr) {
        return 0;
    }
    if (pos >= indices.size() || indices[pos] < 0 ||
        static_cast<std::uint64_t>(indices[pos]) >= array->element_count()) {
        return std::nullopt;
    }
    const auto index = static_cast<std::uint64_t>(indices[pos++]);
    auto nested = linear_index_for(array->element_type(), indices, pos);
    if (!nested) {
        return std::nullopt;
    }
    return index * scalar_element_count(array->element_type()) + *nested;
}

oir::Value *typed_constant_at_linear_index(oir::Module &module, oir::Constant *constant,
                                           oir::Type *type, std::uint64_t linear_index,
                                           oir::Type *load_type) {
    if (constant == nullptr || type == nullptr) {
        return nullptr;
    }
    if (dynamic_cast<oir::ConstantAggregateZero *>(constant) != nullptr) {
        return make_zero_constant(module, load_type);
    }
    auto *array_type = dynamic_cast<oir::ArrayType *>(type);
    if (array_type == nullptr) {
        return linear_index == 0 && constant->type() == load_type ? constant : nullptr;
    }

    const auto nested_count = scalar_element_count(array_type->element_type());
    if (nested_count == 0) {
        return nullptr;
    }
    const auto element_index = linear_index / nested_count;
    const auto nested_index = linear_index % nested_count;
    auto *array = dynamic_cast<oir::ConstantArray *>(constant);
    if (array == nullptr || element_index >= array->elements().size()) {
        return nullptr;
    }
    return typed_constant_at_linear_index(module, array->elements()[element_index],
                                          array_type->element_type(), nested_index, load_type);
}

bool collect_global_indices(oir::Value *ptr, oir::GlobalVariable *&global,
                            std::vector<std::int64_t> &indices) {
    if (auto *g = dynamic_cast<oir::GlobalVariable *>(ptr)) {
        global = g;
        return true;
    }
    auto *gep = dynamic_cast<oir::GetElementPtrInst *>(ptr);
    if (gep == nullptr || !collect_global_indices(gep->base_ptr(), global, indices)) {
        return false;
    }
    for (auto *index : gep->indices()) {
        auto constant = int_constant(index);
        if (!constant) {
            return false;
        }
        indices.push_back(*constant);
    }
    return true;
}

oir::Value *constant_array_element_for_global(oir::Module &module, oir::GlobalVariable &global,
                                              const std::vector<std::int64_t> &raw_indices,
                                              oir::Type *load_type) {
    if (!global.is_const() || !global.value_type()->is_array()) {
        return nullptr;
    }
    std::vector<std::int64_t> indices = raw_indices;
    if (!indices.empty() && indices.front() == 0) {
        indices.erase(indices.begin());
    }
    std::size_t pos = 0;
    auto linear = linear_index_for(global.value_type(), indices, pos);
    if (!linear || pos != indices.size()) {
        return nullptr;
    }

    auto *element_type = scalar_element_type(global.value_type());
    if (element_type != load_type) {
        return nullptr;
    }
    if (auto *initializer = global.initializer()) {
        return typed_constant_at_linear_index(module, initializer, global.value_type(), *linear,
                                              load_type);
    }
    return make_zero_constant(module, element_type);
}

oir::Value *constant_value_for_global(oir::Module &module, oir::GlobalVariable &global) {
    if (!global.is_const() || !is_scalar_type(global.value_type())) {
        return nullptr;
    }

    if (auto *init = global.initializer()) {
        return init->type() == global.value_type() ? init : nullptr;
    }

    return make_zero_constant(module, global.value_type());
}

} // namespace

bool propagate_global_constants(oir::Module &module, Stats &stats) {
    ReplacementMap replacements;

    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }

        for (auto &block : function->blocks()) {
            for (auto &inst : block->instructions()) {
                auto *load = dynamic_cast<oir::LoadInst *>(inst.get());
                if (load == nullptr) {
                    continue;
                }

                auto *global = dynamic_cast<oir::GlobalVariable *>(load->ptr());
                oir::Value *constant = nullptr;
                if (global != nullptr && global->value_type() == load->type()) {
                    constant = constant_value_for_global(module, *global);
                } else {
                    oir::GlobalVariable *indexed_global = nullptr;
                    std::vector<std::int64_t> indices;
                    if (collect_global_indices(load->ptr(), indexed_global, indices) &&
                        indexed_global != nullptr) {
                        constant = constant_array_element_for_global(module, *indexed_global,
                                                                     indices, load->type());
                    }
                }
                if (constant != nullptr && constant->type() == load->type()) {
                    replacements[load] = constant;
                }
            }
        }
    }

    if (replacements.empty()) {
        return false;
    }

    const unsigned replaced = apply_replacements(module, replacements);
    if (replaced == 0) {
        return false;
    }
    stats.globals += static_cast<unsigned>(replacements.size());
    return true;
}

namespace {

struct LoopGlobalAccess {
    oir::GlobalVariable *global = nullptr;
    std::vector<oir::LoadInst *> loads;
    std::vector<oir::StoreInst *> stores;
};

struct ExitEdge {
    oir::BasicBlock *pred = nullptr;
    oir::BasicBlock *succ = nullptr;
};

bool contains_block(const oir::Loop &loop, const oir::BasicBlock *block) {
    return std::find(loop.blocks.begin(), loop.blocks.end(), block) != loop.blocks.end();
}

oir::BasicBlock *mut(const oir::BasicBlock *block) {
    return const_cast<oir::BasicBlock *>(block);
}

bool is_scalar_global(oir::GlobalVariable *global) {
    return global != nullptr && is_scalar_type(global->value_type());
}

oir::GlobalVariable *direct_global_ptr(oir::Value *ptr) {
    auto *global = dynamic_cast<oir::GlobalVariable *>(ptr);
    return is_scalar_global(global) ? global : nullptr;
}

bool is_direct_global_load(const oir::Instruction *inst, oir::GlobalVariable *global) {
    auto *load = dynamic_cast<const oir::LoadInst *>(inst);
    return load != nullptr && load->ptr() == global;
}

bool is_direct_global_store(const oir::Instruction *inst, oir::GlobalVariable *global) {
    auto *store = dynamic_cast<const oir::StoreInst *>(inst);
    return store != nullptr && store->ptr() == global;
}

bool is_entry_hoisted_load(const oir::Function &function, const oir::LoadInst *load) {
    auto *entry = function.entry_block();
    if (entry == nullptr || load->parent() != entry) {
        return false;
    }

    for (const auto &inst : entry->instructions()) {
        if (inst.get() == load) {
            return true;
        }
        if (inst->op() != oir::Instruction::OpID::Alloca &&
            inst->op() != oir::Instruction::OpID::Phi) {
            return false;
        }
    }
    return false;
}

oir::LoadInst *insert_entry_load(oir::Function &function, oir::GlobalVariable *global) {
    auto *entry = function.entry_block();
    if (entry == nullptr) {
        return nullptr;
    }

    auto pos = entry->instructions().begin();
    while (pos != entry->instructions().end() && ((*pos)->op() == oir::Instruction::OpID::Alloca ||
                                                  (*pos)->op() == oir::Instruction::OpID::Phi)) {
        ++pos;
    }

    auto load = std::make_unique<oir::LoadInst>(global->value_type(), global, entry,
                                                global->name().empty() ? "global.load"
                                                                       : global->name() + ".entry");
    auto *raw = load.get();
    raw->set_parent(entry);
    entry->instructions().insert(pos, std::move(load));
    return raw;
}

oir::AllocaInst *insert_entry_alloca(oir::Function &function, oir::Type *type,
                                     const std::string &name) {
    auto *entry = function.entry_block();
    if (entry == nullptr) {
        return nullptr;
    }

    auto pos = entry->instructions().begin();
    while (pos != entry->instructions().end() && (*pos)->op() == oir::Instruction::OpID::Alloca) {
        ++pos;
    }

    auto *ptr_type = function.parent()->types().ptr_ty(type);
    auto alloca = std::make_unique<oir::AllocaInst>(ptr_type, type, entry, name);
    auto *raw = alloca.get();
    raw->set_parent(entry);
    entry->instructions().insert(pos, std::move(alloca));
    return raw;
}

bool function_may_clobber_global(oir::Function &function, oir::GlobalVariable *global,
                                 const oir::OIRAliasAnalysis &alias_analysis,
                                 const oir::FunctionModRefAnalysis &modref) {
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            if (auto *store = dynamic_cast<oir::StoreInst *>(inst.get())) {
                if (alias_analysis.alias(global, store->ptr()) != oir::AliasResult::NoAlias) {
                    return true;
                }
                continue;
            }
            if (auto *call = dynamic_cast<oir::CallInst *>(inst.get())) {
                if (modref.call_may_clobber(*call, global, alias_analysis)) {
                    return true;
                }
            }
        }
    }
    return false;
}

void erase_instructions(oir::Function &function,
                        const std::unordered_set<oir::Instruction *> &dead) {
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
}

bool promote_function_readonly_globals(oir::Module &module, oir::Function &function,
                                       const oir::FunctionModRefAnalysis &modref, Stats &stats) {
    if (function.is_external() || function.entry_block() == nullptr) {
        return false;
    }

    std::unordered_map<oir::GlobalVariable *, std::vector<oir::LoadInst *>> loads_by_global;
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            auto *load = dynamic_cast<oir::LoadInst *>(inst.get());
            if (load == nullptr) {
                continue;
            }
            if (auto *global = direct_global_ptr(load->ptr())) {
                loads_by_global[global].push_back(load);
            }
        }
    }

    bool changed = false;
    ReplacementMap replacements;
    std::unordered_set<oir::Instruction *> dead;
    oir::OIRAliasAnalysis alias_analysis;
    for (auto &[global, loads] : loads_by_global) {
        if (loads.empty()) {
            continue;
        }
        if (loads.size() == 1 && is_entry_hoisted_load(function, loads.front())) {
            continue;
        }
        if (function_may_clobber_global(function, global, alias_analysis, modref)) {
            continue;
        }

        auto *hoisted = insert_entry_load(function, global);
        if (hoisted == nullptr) {
            continue;
        }
        for (auto *load : loads) {
            replacements[load] = hoisted;
            dead.insert(load);
        }
        stats.globals += static_cast<unsigned>(loads.size());
        changed = true;
    }

    if (!changed) {
        return false;
    }
    apply_replacements(module, replacements);
    erase_instructions(function, dead);
    return true;
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
        header_phi.name().empty() ? "global.pre" : header_phi.name() + ".global.pre");
    auto *raw = phi.get();
    raw->set_parent(preheader);
    for (const auto &item : incoming) {
        raw->add_incoming(item.first, item.second);
    }

    auto pos = preheader->instructions().begin();
    while (pos != preheader->instructions().end() && (*pos)->op() == oir::Instruction::OpID::Phi) {
        ++pos;
    }
    preheader->instructions().insert(pos, std::move(phi));
    return raw;
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

    auto *preheader = function.create_block("global.preheader");
    std::unordered_map<oir::PhiInst *, oir::Value *> header_phi_values;
    for (auto &inst : mut(loop.header)->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        auto outside_incoming = incoming_from_outside(*phi, loop);
        if (!outside_incoming.empty()) {
            header_phi_values[phi] = create_preheader_phi(preheader, *phi, outside_incoming);
        }
    }

    oir::cfg::append_unconditional_branch(*function.parent(), preheader, mut(loop.header));
    for (auto *pred : outside_preds) {
        oir::cfg::replace_successor(pred, mut(loop.header), preheader);
    }
    for (auto &[phi, value] : header_phi_values) {
        phi->add_incoming(value, preheader);
    }

    ++stats.cfg;
    return preheader;
}

std::vector<ExitEdge> collect_exit_edges(const oir::Loop &loop) {
    std::vector<ExitEdge> edges;
    for (auto *const_block : loop.blocks) {
        auto *block = mut(const_block);
        for (auto *succ : block->successors()) {
            if (!contains_block(loop, succ)) {
                edges.push_back({block, succ});
            }
        }
    }
    return edges;
}

std::vector<LoopGlobalAccess> collect_loop_globals(const oir::Loop &loop) {
    std::unordered_map<oir::GlobalVariable *, LoopGlobalAccess> by_global;
    for (auto *const_block : loop.blocks) {
        for (auto &inst : mut(const_block)->instructions()) {
            if (auto *load = dynamic_cast<oir::LoadInst *>(inst.get())) {
                if (auto *global = direct_global_ptr(load->ptr())) {
                    auto &access = by_global[global];
                    access.global = global;
                    access.loads.push_back(load);
                }
                continue;
            }
            if (auto *store = dynamic_cast<oir::StoreInst *>(inst.get())) {
                if (auto *global = direct_global_ptr(store->ptr())) {
                    auto &access = by_global[global];
                    access.global = global;
                    access.stores.push_back(store);
                }
            }
        }
    }

    std::vector<LoopGlobalAccess> out;
    for (auto &[global, access] : by_global) {
        if (!access.stores.empty()) {
            out.push_back(std::move(access));
        }
    }
    return out;
}

bool loop_global_access_is_safe(const oir::Loop &loop, oir::GlobalVariable *global,
                                const oir::OIRAliasAnalysis &alias_analysis,
                                const oir::FunctionModRefAnalysis &modref) {
    for (auto *const_block : loop.blocks) {
        for (auto &inst : mut(const_block)->instructions()) {
            for (std::size_t i = 0; i < inst->operand_count(); ++i) {
                if (inst->operand(i) != global) {
                    continue;
                }
                const bool direct_ptr_use = (is_direct_global_load(inst.get(), global) && i == 0) ||
                                            (is_direct_global_store(inst.get(), global) && i == 1);
                if (!direct_ptr_use) {
                    return false;
                }
            }

            if (auto *load = dynamic_cast<oir::LoadInst *>(inst.get())) {
                if (load->ptr() != global &&
                    alias_analysis.alias(global, load->ptr()) != oir::AliasResult::NoAlias) {
                    return false;
                }
                continue;
            }

            if (auto *store = dynamic_cast<oir::StoreInst *>(inst.get())) {
                if (store->ptr() != global &&
                    alias_analysis.alias(global, store->ptr()) != oir::AliasResult::NoAlias) {
                    return false;
                }
                continue;
            }

            if (auto *call = dynamic_cast<oir::CallInst *>(inst.get())) {
                if (modref.call_may_clobber(*call, global, alias_analysis) ||
                    modref.call_may_read(*call, global, alias_analysis)) {
                    return false;
                }
            }
        }
    }
    return true;
}

oir::BasicBlock *split_exit_edge_with_store(oir::Function &function, oir::BasicBlock *pred,
                                            oir::BasicBlock *succ, oir::AllocaInst *slot,
                                            oir::GlobalVariable *global) {
    auto *branch = dynamic_cast<oir::BranchInst *>(pred->terminator());
    if (branch == nullptr) {
        return nullptr;
    }

    auto *split = function.create_block("global.exit");
    if (!oir::cfg::replace_branch_target(*branch, succ, split)) {
        function.erase_block(split);
        return nullptr;
    }

    oir::cfg::remove_edge_no_phi_update(pred, succ);
    oir::cfg::add_edge(pred, split);

    auto load = std::make_unique<oir::LoadInst>(global->value_type(), slot, split,
                                                global->name().empty() ? "global.out"
                                                                       : global->name() + ".out");
    auto *load_raw = load.get();
    split->append_instruction(std::move(load));
    split->append_instruction(std::make_unique<oir::StoreInst>(function.parent()->types().void_ty(),
                                                               load_raw, global, split));
    oir::cfg::append_unconditional_branch(*function.parent(), split, succ);
    oir::cfg::replace_phi_incoming_block(succ, pred, split);
    return split;
}

bool promote_loop_global(oir::Function &function, const oir::Loop &loop,
                         const LoopGlobalAccess &access, Stats &stats) {
    auto *preheader = ensure_preheader(function, loop, stats);
    if (preheader == nullptr || !preheader->has_terminator()) {
        return false;
    }

    auto *global = access.global;
    auto *slot =
        insert_entry_alloca(function, global->value_type(),
                            global->name().empty() ? "global.slot" : global->name() + ".slot");
    if (slot == nullptr) {
        return false;
    }

    auto pre_load = std::make_unique<oir::LoadInst>(
        global->value_type(), global, preheader,
        global->name().empty() ? "global.preload" : global->name() + ".preload");
    auto *pre_load_raw = pre_load.get();
    preheader->insert_before_terminator(std::move(pre_load));
    preheader->insert_before_terminator(std::make_unique<oir::StoreInst>(
        function.parent()->types().void_ty(), pre_load_raw, slot, preheader));

    for (auto *load : access.loads) {
        load->set_operand(0, slot);
    }
    for (auto *store : access.stores) {
        store->set_operand(1, slot);
    }

    auto exits = collect_exit_edges(loop);
    for (const auto &edge : exits) {
        if (edge.pred->parent() != &function || edge.succ->parent() != &function) {
            continue;
        }
        if (split_exit_edge_with_store(function, edge.pred, edge.succ, slot, global) != nullptr) {
            ++stats.cfg;
        }
    }

    stats.globals += static_cast<unsigned>(access.loads.size() + access.stores.size());
    return true;
}

bool promote_loop_globals(oir::Module &module, oir::Function &function,
                          const oir::FunctionModRefAnalysis &modref, Stats &stats) {
    (void)module;
    if (function.is_external() || function.entry_block() == nullptr) {
        return false;
    }

    bool changed = false;
    constexpr unsigned kMaxPromotions = 64;
    for (unsigned iteration = 0; iteration < kMaxPromotions; ++iteration) {
        oir::DominatorTree dom_tree(function);
        oir::LoopInfo loop_info(function, dom_tree);
        oir::OIRAliasAnalysis alias_analysis;
        bool promoted = false;

        for (const auto &loop : loop_info.loops()) {
            for (const auto &access : collect_loop_globals(loop)) {
                if (!loop_global_access_is_safe(loop, access.global, alias_analysis, modref)) {
                    continue;
                }
                if (promote_loop_global(function, loop, access, stats)) {
                    changed = true;
                    promoted = true;
                    break;
                }
            }
            if (promoted) {
                break;
            }
        }

        if (!promoted) {
            break;
        }
    }
    return changed;
}

} // namespace

bool promote_global_loads(oir::Module &module, Stats &stats) {
    bool changed = false;
    oir::FunctionModRefAnalysis modref(module);
    for (auto &function : module.functions()) {
        changed |= promote_function_readonly_globals(module, *function, modref, stats);
    }
    for (auto &function : module.functions()) {
        changed |= promote_loop_globals(module, *function, modref, stats);
    }
    return changed;
}

} // namespace pass::oir_opt

namespace pass {

std::string_view OIRGlobalOptPass::name() const {
    return "OIRGlobalOptPass";
}

PassKind OIRGlobalOptPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRGlobalOptPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRGlobalOptPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::propagate_global_constants(module, stats);
            changed |= oir_opt::promote_global_loads(module, stats);
            changed |= oir_opt::scalar_replacement_of_aggregates(module, stats);
            changed |= oir_opt::promote_memory_to_registers(module, stats);
            changed |= oir_opt::eliminate_dead_code(module, stats);
            return changed;
        });
}

} // namespace pass
