#include "../../include/oir/OIRScalarOpt.h"

#include "../../include/oir/OIRAnalysis.h"
#include "../../include/oir/OIRCFGUtils.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

using ValueMap = std::unordered_map<oir::Value *, oir::Value *>;
using BlockMap = std::unordered_map<oir::BasicBlock *, oir::BasicBlock *>;

bool contains_block(const oir::Loop &loop, const oir::BasicBlock *block) {
    return std::find(loop.blocks.begin(), loop.blocks.end(), block) != loop.blocks.end();
}

bool contains_nested_loop_header(const oir::Loop &loop, const std::vector<oir::Loop> &loops) {
    for (const auto &candidate : loops) {
        if (candidate.header != loop.header && contains_block(loop, candidate.header)) {
            return true;
        }
    }
    return false;
}

bool starts_with(const std::string &value, const char *prefix) {
    const std::string prefix_string(prefix);
    return value.size() >= prefix_string.size() &&
           value.compare(0, prefix_string.size(), prefix_string) == 0;
}

oir::BasicBlock *mutable_block(const oir::BasicBlock *block) {
    return const_cast<oir::BasicBlock *>(block);
}

oir::Value *map_value(oir::Value *value, const ValueMap &map) {
    auto found = map.find(value);
    return found == map.end() ? value : found->second;
}

std::vector<oir::Value *> map_values(const std::vector<oir::Value *> &values,
                                     const ValueMap &map) {
    std::vector<oir::Value *> out;
    out.reserve(values.size());
    for (auto *value : values) {
        out.push_back(map_value(value, map));
    }
    return out;
}

bool is_cloneable_pure_instruction(const oir::Instruction &inst) {
    switch (inst.op()) {
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::Mul:
    case oir::Instruction::OpID::SDiv:
    case oir::Instruction::OpID::SRem:
    case oir::Instruction::OpID::BitAnd:
    case oir::Instruction::OpID::BitOr:
    case oir::Instruction::OpID::BitXor:
    case oir::Instruction::OpID::FAdd:
    case oir::Instruction::OpID::FSub:
    case oir::Instruction::OpID::FMul:
    case oir::Instruction::OpID::FDiv:
    case oir::Instruction::OpID::ICmp:
    case oir::Instruction::OpID::FCmp:
    case oir::Instruction::OpID::GetElementPtr:
    case oir::Instruction::OpID::ZExt:
    case oir::Instruction::OpID::SIToFP:
    case oir::Instruction::OpID::FPToSI:
        return true;
    case oir::Instruction::OpID::Ret:
    case oir::Instruction::OpID::Br:
    case oir::Instruction::OpID::Alloca:
    case oir::Instruction::OpID::Load:
    case oir::Instruction::OpID::Store:
    case oir::Instruction::OpID::Call:
    case oir::Instruction::OpID::Phi:
        return false;
    }
    return false;
}

std::string suffixed_name(const oir::Value &value, const char *suffix) {
    return value.name().empty() ? "" : value.name() + suffix;
}

std::unique_ptr<oir::Instruction> clone_instruction(const oir::Instruction &inst,
                                                    const ValueMap &map,
                                                    oir::BasicBlock *parent,
                                                    const char *suffix) {
    if (auto *binary = dynamic_cast<const oir::BinaryInst *>(&inst)) {
        return std::make_unique<oir::BinaryInst>(binary->type(), binary->op(),
                                                 map_value(binary->lhs(), map),
                                                 map_value(binary->rhs(), map), parent,
                                                 suffixed_name(inst, suffix));
    }
    if (auto *cmp = dynamic_cast<const oir::CmpInst *>(&inst)) {
        return std::make_unique<oir::CmpInst>(cmp->type(), cmp->op(), cmp->pred(),
                                              map_value(cmp->lhs(), map),
                                              map_value(cmp->rhs(), map), parent,
                                              suffixed_name(inst, suffix));
    }
    if (auto *cast = dynamic_cast<const oir::CastInst *>(&inst)) {
        return std::make_unique<oir::CastInst>(cast->type(), cast->op(),
                                               map_value(cast->src(), map), parent,
                                               suffixed_name(inst, suffix));
    }
    if (auto *alloca = dynamic_cast<const oir::AllocaInst *>(&inst)) {
        return std::make_unique<oir::AllocaInst>(alloca->type(), alloca->allocated_type(), parent,
                                                 suffixed_name(inst, suffix));
    }
    if (auto *gep = dynamic_cast<const oir::GetElementPtrInst *>(&inst)) {
        return std::make_unique<oir::GetElementPtrInst>(
            gep->type(), map_value(gep->base_ptr(), map), map_values(gep->indices(), map), parent,
            suffixed_name(inst, suffix));
    }
    if (auto *load = dynamic_cast<const oir::LoadInst *>(&inst)) {
        return std::make_unique<oir::LoadInst>(load->type(), map_value(load->ptr(), map), parent,
                                               suffixed_name(inst, suffix));
    }
    if (auto *store = dynamic_cast<const oir::StoreInst *>(&inst)) {
        return std::make_unique<oir::StoreInst>(store->type(), map_value(store->value(), map),
                                                map_value(store->ptr(), map), parent);
    }
    if (auto *call = dynamic_cast<const oir::CallInst *>(&inst)) {
        return std::make_unique<oir::CallInst>(call->type(), map_value(call->callee(), map),
                                               map_values(call->args(), map), parent,
                                               suffixed_name(inst, suffix));
    }
    if (auto *ret = dynamic_cast<const oir::ReturnInst *>(&inst)) {
        return std::make_unique<oir::ReturnInst>(ret->type(),
                                                 ret->has_value()
                                                     ? map_value(ret->value(), map)
                                                     : nullptr,
                                                 parent);
    }
    if (auto *branch = dynamic_cast<const oir::BranchInst *>(&inst)) {
        if (branch->is_conditional()) {
            return std::make_unique<oir::BranchInst>(
                branch->type(), map_value(branch->cond(), map),
                static_cast<oir::BasicBlock *>(map_value(branch->true_bb(), map)),
                static_cast<oir::BasicBlock *>(map_value(branch->false_bb(), map)), parent);
        }
        return std::make_unique<oir::BranchInst>(
            branch->type(), static_cast<oir::BasicBlock *>(map_value(branch->target_bb(), map)),
            parent);
    }
    if (auto *phi = dynamic_cast<const oir::PhiInst *>(&inst)) {
        auto clone = std::make_unique<oir::PhiInst>(phi->type(), parent,
                                                    suffixed_name(inst, suffix));
        for (const auto &incoming : phi->incoming()) {
            clone->add_incoming(map_value(incoming.first, map),
                                static_cast<oir::BasicBlock *>(map_value(incoming.second, map)));
        }
        return clone;
    }
    return nullptr;
}

void replace_terminator_with_br(oir::BasicBlock *block, oir::BasicBlock *target) {
    if (block->has_terminator()) {
        block->terminator()->drop_all_operands();
        block->instructions().pop_back();
    }
    oir::cfg::append_unconditional_branch(*block->parent()->parent(), block, target);
}

void replace_terminator_with_cond_br(oir::BasicBlock *block, oir::Value *condition,
                                     oir::BasicBlock *true_bb, oir::BasicBlock *false_bb) {
    if (block->has_terminator()) {
        block->terminator()->drop_all_operands();
        block->instructions().pop_back();
    }
    oir::cfg::append_conditional_branch(*block->parent()->parent(), block, condition, true_bb,
                                        false_bb);
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

oir::BasicBlock *single_latch(const oir::Loop &loop) {
    if (loop.latches.size() != 1 || loop.latches.front() == loop.header) {
        return nullptr;
    }
    return mutable_block(loop.latches.front());
}

bool block_starts_with_phi(const oir::BasicBlock &block) {
    return !block.instructions().empty() &&
           dynamic_cast<const oir::PhiInst *>(block.instructions().front().get()) != nullptr;
}

bool non_phi_header_values_used_outside_header(const oir::Function &function,
                                               const oir::BasicBlock &header) {
    std::unordered_set<const oir::Instruction *> header_defs;
    for (const auto &inst : header.instructions()) {
        if (inst->op() == oir::Instruction::OpID::Phi || inst->is_terminator()) {
            continue;
        }
        header_defs.insert(inst.get());
    }
    if (header_defs.empty()) {
        return false;
    }

    for (const auto &block : function.blocks()) {
        for (const auto &inst : block->instructions()) {
            if (block.get() == &header) {
                continue;
            }
            for (auto *operand : inst->operands()) {
                auto *operand_inst = dynamic_cast<const oir::Instruction *>(operand);
                if (operand_inst != nullptr && header_defs.find(operand_inst) != header_defs.end()) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool header_phi_used_by_external_phi(const oir::Function &function, const oir::Loop &loop,
                                     const oir::BasicBlock &header) {
    std::unordered_set<const oir::Value *> header_phis;
    for (const auto &inst : header.instructions()) {
        auto *phi = dynamic_cast<const oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        header_phis.insert(phi);
    }
    if (header_phis.empty()) {
        return false;
    }

    for (const auto &block : function.blocks()) {
        if (contains_block(loop, block.get())) {
            continue;
        }
        for (const auto &inst : block->instructions()) {
            if (dynamic_cast<const oir::PhiInst *>(inst.get()) == nullptr) {
                break;
            }
            for (auto *operand : inst->operands()) {
                if (header_phis.find(operand) != header_phis.end()) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool header_condition_has_div_or_rem(const oir::BasicBlock &header) {
    for (const auto &inst : header.instructions()) {
        if (inst->op() == oir::Instruction::OpID::Phi) {
            continue;
        }
        if (inst->is_terminator()) {
            break;
        }
        switch (inst->op()) {
        case oir::Instruction::OpID::SDiv:
        case oir::Instruction::OpID::SRem:
        case oir::Instruction::OpID::FDiv:
            return true;
        default:
            break;
        }
    }
    return false;
}

bool header_condition_uses_addrec(const oir::Loop &loop, const oir::BranchInst &branch,
                                  const oir::ScalarEvolution &scev) {
    if (!branch.is_conditional()) {
        return false;
    }
    auto *cmp = dynamic_cast<oir::CmpInst *>(branch.cond());
    if (cmp == nullptr || cmp->op() != oir::Instruction::OpID::ICmp) {
        return false;
    }
    auto lhs = scev.expression_for(cmp->lhs(), &loop);
    auto rhs = scev.expression_for(cmp->rhs(), &loop);
    return lhs.kind() == oir::SCEVKind::AddRec || rhs.kind() == oir::SCEVKind::AddRec;
}

bool loop_has_non_condition_side_exit(const oir::Loop &loop, const oir::BasicBlock *condition_exit) {
    for (auto *block : loop.blocks) {
        for (auto *succ : block->successors()) {
            if (contains_block(loop, succ) || succ == condition_exit) {
                continue;
            }
            return true;
        }
    }
    return false;
}

struct HeaderPhiIncoming {
    oir::Value *outside = nullptr;
    oir::Value *latch = nullptr;
};

bool collect_header_phi_incoming(const oir::BasicBlock &header, oir::BasicBlock *preheader,
                                 oir::BasicBlock *latch,
                                 std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> &incoming) {
    for (const auto &inst : header.instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        HeaderPhiIncoming values;
        for (const auto &item : phi->incoming()) {
            if (item.second == preheader) {
                values.outside = item.first;
            } else if (item.second == latch) {
                values.latch = item.first;
            } else {
                return false;
            }
        }
        if (values.outside == nullptr || values.latch == nullptr) {
            return false;
        }
        incoming[phi] = values;
    }
    return true;
}

bool clone_header_condition(const oir::BasicBlock &header, oir::BasicBlock *dest,
                            const ValueMap &seed_map, oir::Value *condition,
                            oir::Value *&cloned_condition) {
    ValueMap map = seed_map;
    for (const auto &inst : header.instructions()) {
        if (inst->op() == oir::Instruction::OpID::Phi) {
            continue;
        }
        if (inst->is_terminator()) {
            break;
        }
        if (!is_cloneable_pure_instruction(*inst)) {
            return false;
        }
        auto clone = clone_instruction(*inst, map, dest, ".rot");
        if (clone == nullptr || clone->type() == nullptr || clone->type()->is_void()) {
            return false;
        }
        auto *raw = clone.get();
        dest->insert_before_terminator(std::move(clone));
        map[inst.get()] = raw;
    }
    cloned_condition = map_value(condition, map);
    return cloned_condition != nullptr;
}

void move_header_phis_to_body(oir::BasicBlock *header, oir::BasicBlock *body) {
    std::vector<std::unique_ptr<oir::Instruction>> phis;
    while (!header->instructions().empty()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(header->instructions().front().get());
        if (phi == nullptr) {
            break;
        }
        phis.push_back(std::move(header->instructions().front()));
        header->instructions().pop_front();
    }

    for (auto it = phis.rbegin(); it != phis.rend(); ++it) {
        (*it)->set_parent(body);
        body->instructions().push_front(std::move(*it));
    }
}

void split_exit_phi_incoming(oir::BasicBlock *exit, oir::BasicBlock *old_header,
                             oir::BasicBlock *preheader, oir::BasicBlock *latch,
                             const std::unordered_map<oir::PhiInst *, HeaderPhiIncoming>
                                 &header_phi_values) {
    for (auto &inst : exit->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }

        oir::Value *incoming_value = nullptr;
        for (const auto &incoming : phi->incoming()) {
            if (incoming.second == old_header) {
                incoming_value = incoming.first;
                break;
            }
        }
        if (incoming_value == nullptr) {
            continue;
        }

        auto *incoming_phi = dynamic_cast<oir::PhiInst *>(incoming_value);
        auto found = incoming_phi == nullptr ? header_phi_values.end()
                                             : header_phi_values.find(incoming_phi);
        oir::Value *preheader_value =
            found == header_phi_values.end() ? incoming_value : found->second.outside;
        oir::Value *latch_value =
            found == header_phi_values.end() ? incoming_value : found->second.latch;

        phi->remove_incoming_from(old_header);
        phi->add_incoming(preheader_value, preheader);
        phi->add_incoming(latch_value, latch);
    }
}

bool block_has_direct_non_phi_use(const oir::BasicBlock &block, const oir::Value *value) {
    for (const auto &inst : block.instructions()) {
        if (dynamic_cast<const oir::PhiInst *>(inst.get()) != nullptr) {
            continue;
        }
        for (auto *operand : inst->operands()) {
            if (operand == value) {
                return true;
            }
        }
    }
    return false;
}

std::vector<oir::BasicBlock *> normal_exit_reachable_blocks(oir::BasicBlock *exit,
                                                            const oir::Loop &loop) {
    std::vector<oir::BasicBlock *> out;
    std::unordered_set<oir::BasicBlock *> seen;
    std::deque<oir::BasicBlock *> worklist;
    seen.insert(exit);
    worklist.push_back(exit);
    while (!worklist.empty()) {
        auto *block = worklist.front();
        worklist.pop_front();
        out.push_back(block);
        for (auto *succ : block->successors()) {
            if (contains_block(loop, succ)) {
                continue;
            }
            if (seen.insert(succ).second) {
                worklist.push_back(succ);
            }
        }
    }
    return out;
}

bool reachable_blocks_have_direct_use(const std::vector<oir::BasicBlock *> &blocks,
                                      const oir::Value *value) {
    return std::any_of(blocks.begin(), blocks.end(), [value](const oir::BasicBlock *block) {
        return block_has_direct_non_phi_use(*block, value);
    });
}

void replace_direct_non_phi_uses_in_blocks(const std::vector<oir::BasicBlock *> &blocks,
                                           oir::Value *old_value, oir::Value *new_value) {
    for (auto *block : blocks) {
        for (auto &inst : block->instructions()) {
            if (inst.get() == new_value || dynamic_cast<oir::PhiInst *>(inst.get()) != nullptr) {
                continue;
            }
            for (std::size_t i = 0; i < inst->operand_count(); ++i) {
                if (inst->operand(i) == old_value) {
                    inst->set_operand(i, new_value);
                }
            }
        }
    }
}

bool can_materialize_direct_exit_uses(const oir::BasicBlock &exit,
                                      const oir::BasicBlock *old_header) {
    for (auto *pred : exit.predecessors()) {
        if (pred != old_header) {
            return false;
        }
    }
    return true;
}

void materialize_rotated_exit_values(
    oir::Function &function, const oir::Loop &loop, oir::BasicBlock *exit,
    oir::BasicBlock *old_header, oir::BasicBlock *preheader, oir::BasicBlock *latch,
    const std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> &header_phi_values) {
    (void)function;
    auto reachable = normal_exit_reachable_blocks(exit, loop);
    for (const auto &[header_phi, values] : header_phi_values) {
        if (!reachable_blocks_have_direct_use(reachable, header_phi)) {
            continue;
        }

        auto exit_phi = std::make_unique<oir::PhiInst>(
            header_phi->type(), exit,
            header_phi->name().empty() ? "rot.exit" : header_phi->name() + ".rot.exit");
        auto *raw = exit_phi.get();
        raw->add_incoming(values.outside, preheader);
        raw->add_incoming(values.latch, latch);

        auto insert_pos = exit->instructions().begin();
        while (insert_pos != exit->instructions().end() &&
               dynamic_cast<oir::PhiInst *>(insert_pos->get()) != nullptr) {
            ++insert_pos;
        }
        exit->instructions().insert(insert_pos, std::move(exit_phi));
        replace_direct_non_phi_uses_in_blocks(reachable, header_phi, raw);
    }
}

bool rotate_loop(oir::Function &function, const oir::Loop &loop, const oir::ScalarEvolution &scev,
                 Stats &stats) {
    auto *header = mutable_block(loop.header);
    auto *preheader = find_preheader(loop);
    auto *latch = single_latch(loop);
    if (preheader == nullptr || latch == nullptr || !preheader->has_terminator() ||
        !latch->has_terminator() || block_starts_with_phi(*latch)) {
        return false;
    }

    auto *preheader_branch = dynamic_cast<oir::BranchInst *>(preheader->terminator());
    auto *latch_branch = dynamic_cast<oir::BranchInst *>(latch->terminator());
    auto *header_branch = dynamic_cast<oir::BranchInst *>(header->terminator());
    if (preheader_branch == nullptr || preheader_branch->is_conditional() ||
        preheader_branch->target_bb() != header || latch_branch == nullptr ||
        latch_branch->is_conditional() || latch_branch->target_bb() != header ||
        header_branch == nullptr || !header_branch->is_conditional()) {
        return false;
    }
    if (!header_condition_uses_addrec(loop, *header_branch, scev)) {
        return false;
    }

    auto *true_bb = header_branch->true_bb();
    auto *false_bb = header_branch->false_bb();
    if (true_bb == false_bb) {
        return false;
    }
    const bool true_in_loop = contains_block(loop, true_bb);
    const bool false_in_loop = contains_block(loop, false_bb);
    if (true_in_loop == false_in_loop) {
        return false;
    }

    auto *body = true_in_loop ? true_bb : false_bb;
    auto *exit = true_in_loop ? false_bb : true_bb;
    if (body->predecessors().size() != 1 || body->predecessors().front() != header ||
        block_starts_with_phi(*body) ||
        non_phi_header_values_used_outside_header(function, *header) ||
        header_phi_used_by_external_phi(function, loop, *header) ||
        header_condition_has_div_or_rem(*header) ||
        loop_has_non_condition_side_exit(loop, exit)) {
        return false;
    }

    std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> header_phi_values;
    if (!collect_header_phi_incoming(*header, preheader, latch, header_phi_values)) {
        return false;
    }
    auto reachable_after_exit = normal_exit_reachable_blocks(exit, loop);
    for (const auto &[phi, values] : header_phi_values) {
        (void)values;
        if (reachable_blocks_have_direct_use(reachable_after_exit, phi) &&
            !can_materialize_direct_exit_uses(*exit, header)) {
            return false;
        }
    }

    ValueMap preheader_map;
    ValueMap latch_map;
    for (const auto &[phi, values] : header_phi_values) {
        preheader_map[phi] = values.outside;
        latch_map[phi] = values.latch;
    }

    oir::Value *preheader_condition = nullptr;
    oir::Value *latch_condition = nullptr;
    if (!clone_header_condition(*header, preheader, preheader_map, header_branch->cond(),
                                preheader_condition) ||
        !clone_header_condition(*header, latch, latch_map, header_branch->cond(),
                                latch_condition)) {
        return false;
    }

    split_exit_phi_incoming(exit, header, preheader, latch, header_phi_values);
    materialize_rotated_exit_values(function, loop, exit, header, preheader, latch,
                                    header_phi_values);
    move_header_phis_to_body(header, body);

    oir::cfg::remove_edge_no_phi_update(preheader, header);
    oir::cfg::remove_edge_no_phi_update(latch, header);
    oir::cfg::remove_edge_no_phi_update(header, body);
    oir::cfg::remove_edge_no_phi_update(header, exit);

    oir::cfg::add_edge(preheader, body);
    oir::cfg::add_edge(preheader, exit);
    oir::cfg::add_edge(latch, body);
    oir::cfg::add_edge(latch, exit);

    replace_terminator_with_cond_br(preheader, preheader_condition, true_bb, false_bb);
    replace_terminator_with_cond_br(latch, latch_condition, true_bb, false_bb);

    function.erase_block(header);
    ++stats.loop_rotate;
    ++stats.cfg;
    return true;
}

std::vector<oir::BasicBlock *> function_ordered_loop_blocks(oir::Function &function,
                                                            const oir::Loop &loop) {
    std::vector<oir::BasicBlock *> out;
    for (auto &block : function.blocks()) {
        if (contains_block(loop, block.get())) {
            out.push_back(block.get());
        }
    }
    return out;
}

std::size_t loop_instruction_count(const std::vector<oir::BasicBlock *> &blocks) {
    std::size_t count = 0;
    for (auto *block : blocks) {
        count += block->instructions().size();
    }
    return count;
}

bool value_defined_in_loop(const oir::Value *value, const oir::Loop &loop) {
    auto *inst = dynamic_cast<const oir::Instruction *>(value);
    return inst != nullptr && contains_block(loop, inst->parent());
}

void push_unique_value(std::vector<oir::Value *> &values, oir::Value *value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

std::vector<oir::BasicBlock *> loop_exit_blocks(const std::vector<oir::BasicBlock *> &blocks,
                                                const oir::Loop &loop) {
    std::vector<oir::BasicBlock *> exits;
    for (auto *block : blocks) {
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

std::vector<oir::Value *> loop_values_used_on_exit_edge(oir::BasicBlock *exit,
                                                        const oir::Loop &loop) {
    std::vector<oir::Value *> values;
    for (const auto &inst : exit->instructions()) {
        if (dynamic_cast<const oir::PhiInst *>(inst.get()) != nullptr) {
            continue;
        }
        for (auto *operand : inst->operands()) {
            if (value_defined_in_loop(operand, loop)) {
                push_unique_value(values, operand);
            }
        }
    }

    for (auto *succ : exit->successors()) {
        for (const auto &inst : succ->instructions()) {
            auto *phi = dynamic_cast<const oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }
            for (const auto &incoming : phi->incoming()) {
                if (incoming.second == exit && value_defined_in_loop(incoming.first, loop)) {
                    push_unique_value(values, incoming.first);
                }
            }
        }
    }
    return values;
}

bool direct_loop_value_uses_are_repairable(const std::vector<oir::BasicBlock *> &blocks,
                                           const oir::Loop &loop) {
    for (auto *exit : loop_exit_blocks(blocks, loop)) {
        if (loop_values_used_on_exit_edge(exit, loop).empty()) {
            continue;
        }
        for (auto *pred : exit->predecessors()) {
            if (!contains_block(loop, pred)) {
                return false;
            }
        }
    }
    return true;
}

void replace_non_phi_uses_in_block(oir::BasicBlock *block, oir::Value *old_value,
                                   oir::Value *new_value) {
    for (auto &inst : block->instructions()) {
        if (inst.get() == new_value || dynamic_cast<oir::PhiInst *>(inst.get()) != nullptr) {
            continue;
        }
        for (std::size_t i = 0; i < inst->operand_count(); ++i) {
            if (inst->operand(i) == old_value) {
                inst->set_operand(i, new_value);
            }
        }
    }
}

void replace_phi_uses_on_successor_edges(oir::BasicBlock *exit, oir::Value *old_value,
                                         oir::Value *new_value) {
    for (auto *succ : exit->successors()) {
        for (auto &inst : succ->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }
            for (std::size_t i = 0; i < phi->incoming().size(); ++i) {
                const auto &incoming = phi->incoming()[i];
                if (incoming.second == exit && incoming.first == old_value) {
                    phi->set_operand(i * 2, new_value);
                }
            }
        }
    }
}

void materialize_unswitched_exit_values(const std::vector<oir::BasicBlock *> &original_blocks,
                                        const oir::Loop &loop, const BlockMap &block_map,
                                        const ValueMap &value_map) {
    std::unordered_map<oir::BasicBlock *, oir::BasicBlock *> original_for_clone;
    for (const auto &[orig, clone] : block_map) {
        original_for_clone[clone] = orig;
    }

    for (auto *exit : loop_exit_blocks(original_blocks, loop)) {
        auto direct_values = loop_values_used_on_exit_edge(exit, loop);

        for (auto *value : direct_values) {
            auto exit_phi = std::make_unique<oir::PhiInst>(
                value->type(), exit,
                value->name().empty() ? "usw.exit" : value->name() + ".usw.exit");
            auto *raw = exit_phi.get();
            for (auto *pred : exit->predecessors()) {
                auto clone_found = original_for_clone.find(pred);
                if (clone_found != original_for_clone.end()) {
                    raw->add_incoming(map_value(value, value_map), pred);
                } else {
                    raw->add_incoming(value, pred);
                }
            }

            auto insert_pos = exit->instructions().begin();
            while (insert_pos != exit->instructions().end() &&
                   dynamic_cast<oir::PhiInst *>(insert_pos->get()) != nullptr) {
                ++insert_pos;
            }
            exit->instructions().insert(insert_pos, std::move(exit_phi));
            replace_non_phi_uses_in_block(exit, value, raw);
            replace_phi_uses_on_successor_edges(exit, value, raw);
        }
    }
}

oir::BranchInst *find_unswitch_branch(const oir::Loop &loop, const oir::ScalarEvolution &scev) {
    if (loop.header != nullptr && starts_with(loop.header->name(), "usw.")) {
        return nullptr;
    }
    for (auto *const_block : loop.blocks) {
        if (const_block == loop.header) {
            continue;
        }
        auto *block = mutable_block(const_block);
        auto *branch = dynamic_cast<oir::BranchInst *>(block->terminator());
        if (branch == nullptr || !branch->is_conditional()) {
            continue;
        }
        if (!contains_block(loop, branch->true_bb()) ||
            !contains_block(loop, branch->false_bb())) {
            continue;
        }
        if (scev.is_loop_invariant(branch->cond(), loop)) {
            return branch;
        }
    }
    return nullptr;
}

bool is_safely_speculatable_for_unswitch(const oir::Instruction &inst) {
    if (!is_cloneable_pure_instruction(inst)) {
        return false;
    }
    switch (inst.op()) {
    case oir::Instruction::OpID::SDiv:
    case oir::Instruction::OpID::SRem:
    case oir::Instruction::OpID::FDiv:
        return false;
    default:
        return true;
    }
}

oir::Value *materialize_invariant_value(oir::Value *value, const oir::Loop &loop,
                                        oir::BasicBlock *preheader, ValueMap &map) {
    auto found = map.find(value);
    if (found != map.end()) {
        return found->second;
    }

    auto *inst = dynamic_cast<oir::Instruction *>(value);
    if (inst == nullptr || !contains_block(loop, inst->parent())) {
        map[value] = value;
        return value;
    }
    if (!is_safely_speculatable_for_unswitch(*inst)) {
        return nullptr;
    }

    for (std::size_t i = 0; i < inst->operand_count(); ++i) {
        auto *mapped_operand = materialize_invariant_value(inst->operand(i), loop, preheader, map);
        if (mapped_operand == nullptr) {
            return nullptr;
        }
        map[inst->operand(i)] = mapped_operand;
    }

    auto clone = clone_instruction(*inst, map, preheader, ".usw.cond");
    if (clone == nullptr || clone->type() == nullptr || clone->type()->is_void()) {
        return nullptr;
    }
    auto *raw = clone.get();
    preheader->insert_before_terminator(std::move(clone));
    map[value] = raw;
    return raw;
}

void fix_cloned_operands(const std::vector<oir::BasicBlock *> &clones, const ValueMap &map) {
    for (auto *block : clones) {
        for (auto &inst : block->instructions()) {
            for (std::size_t i = 0; i < inst->operand_count(); ++i) {
                auto *old_operand = inst->operand(i);
                auto *new_operand = map_value(old_operand, map);
                if (old_operand != new_operand) {
                    inst->set_operand(i, new_operand);
                }
            }
        }
    }
}

void build_cloned_cfg_and_exit_phis(const oir::Loop &loop,
                                    const std::vector<oir::BasicBlock *> &original_blocks,
                                    const BlockMap &block_map, const ValueMap &value_map) {
    for (auto *orig : original_blocks) {
        auto *clone = block_map.at(orig);
        for (auto *succ : orig->successors()) {
            auto block_found = block_map.find(succ);
            auto *clone_succ =
                block_found == block_map.end() ? succ : block_found->second;
            oir::cfg::add_edge(clone, clone_succ);

            if (block_found != block_map.end()) {
                continue;
            }
            for (auto &inst : succ->instructions()) {
                auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
                if (phi == nullptr) {
                    break;
                }
                for (const auto &incoming : phi->incoming()) {
                    if (incoming.second == orig) {
                        phi->add_incoming(map_value(incoming.first, value_map), clone);
                        break;
                    }
                }
            }
        }
    }
}

bool clone_loop(oir::Function &function, const oir::Loop &loop,
                std::vector<oir::BasicBlock *> &original_blocks, BlockMap &block_map,
                ValueMap &value_map) {
    original_blocks = function_ordered_loop_blocks(function, loop);
    for (auto *block : original_blocks) {
        auto *clone = function.create_block("usw." + block->name());
        block_map[block] = clone;
        value_map[block] = clone;
    }

    std::vector<oir::BasicBlock *> clone_blocks;
    clone_blocks.reserve(original_blocks.size());
    for (auto *block : original_blocks) {
        auto *clone_block = block_map.at(block);
        clone_blocks.push_back(clone_block);
        for (const auto &inst : block->instructions()) {
            auto clone = clone_instruction(*inst, value_map, clone_block, ".usw");
            if (clone == nullptr) {
                return false;
            }
            auto *raw = clone.get();
            clone_block->append_instruction(std::move(clone));
            value_map[inst.get()] = raw;
        }
    }

    fix_cloned_operands(clone_blocks, value_map);
    build_cloned_cfg_and_exit_phis(loop, original_blocks, block_map, value_map);
    return true;
}

void force_branch_direction(oir::BranchInst &branch, bool take_true) {
    if (!branch.is_conditional()) {
        return;
    }
    auto *block = branch.parent();
    auto *taken = take_true ? branch.true_bb() : branch.false_bb();
    auto *removed = take_true ? branch.false_bb() : branch.true_bb();
    if (taken != removed) {
        oir::cfg::remove_edge(block, removed);
    }
    oir::cfg::add_edge(block, taken);
    replace_terminator_with_br(block, taken);
}

bool unswitch_loop(oir::Function &function, const oir::Loop &loop,
                   const oir::ScalarEvolution &scev, Stats &stats) {
    auto *preheader = find_preheader(loop);
    if (preheader == nullptr || !preheader->has_terminator()) {
        return false;
    }
    auto *preheader_branch = dynamic_cast<oir::BranchInst *>(preheader->terminator());
    if (preheader_branch == nullptr || preheader_branch->is_conditional() ||
        preheader_branch->target_bb() != loop.header) {
        return false;
    }

    auto original_blocks = function_ordered_loop_blocks(function, loop);
    if (original_blocks.empty() || original_blocks.size() > 12 ||
        loop_instruction_count(original_blocks) > 120 ||
        !direct_loop_value_uses_are_repairable(original_blocks, loop)) {
        return false;
    }

    auto *branch = find_unswitch_branch(loop, scev);
    if (branch == nullptr) {
        return false;
    }

    ValueMap condition_map;
    auto *unswitch_condition =
        materialize_invariant_value(branch->cond(), loop, preheader, condition_map);
    if (unswitch_condition == nullptr) {
        return false;
    }

    BlockMap block_map;
    ValueMap value_map;
    if (!clone_loop(function, loop, original_blocks, block_map, value_map)) {
        return false;
    }
    materialize_unswitched_exit_values(original_blocks, loop, block_map, value_map);

    auto *clone_header = block_map.at(mutable_block(loop.header));
    auto *clone_branch = dynamic_cast<oir::BranchInst *>(map_value(branch, value_map));
    if (clone_branch == nullptr) {
        return false;
    }

    oir::cfg::add_edge(preheader, clone_header);
    replace_terminator_with_cond_br(preheader, unswitch_condition, mutable_block(loop.header),
                                    clone_header);
    force_branch_direction(*branch, true);
    force_branch_direction(*clone_branch, false);

    ++stats.loop_unswitch;
    ++stats.cfg;
    return true;
}

bool rotate_loops_in_function(oir::Function &function, Stats &stats) {
    bool changed = false;
    constexpr unsigned kMaxRotationsPerFunction = 32;
    for (unsigned iteration = 0; iteration < kMaxRotationsPerFunction; ++iteration) {
        oir::DominatorTree dom_tree(function);
        oir::LoopInfo loop_info(function, dom_tree);
        auto loops = loop_info.loops();
        std::sort(loops.begin(), loops.end(), [](const oir::Loop &lhs, const oir::Loop &rhs) {
            return lhs.blocks.size() < rhs.blocks.size();
        });

        bool rotated = false;
        oir::ScalarEvolution scev(function, loop_info);
        for (const auto &loop : loops) {
            if (contains_nested_loop_header(loop, loops)) {
                continue;
            }
            if (rotate_loop(function, loop, scev, stats)) {
                changed = true;
                rotated = true;
                break;
            }
        }
        if (!rotated) {
            break;
        }
    }
    return changed;
}

bool unswitch_loops_in_function(oir::Function &function, Stats &stats) {
    bool changed = false;
    constexpr unsigned kMaxUnswitchesPerFunction = 4;
    for (unsigned iteration = 0; iteration < kMaxUnswitchesPerFunction; ++iteration) {
        oir::DominatorTree dom_tree(function);
        oir::LoopInfo loop_info(function, dom_tree);
        oir::ScalarEvolution scev(function, loop_info);
        auto loops = loop_info.loops();
        std::sort(loops.begin(), loops.end(), [](const oir::Loop &lhs, const oir::Loop &rhs) {
            return lhs.blocks.size() < rhs.blocks.size();
        });

        bool unswitched = false;
        for (const auto &loop : loops) {
            if (unswitch_loop(function, loop, scev, stats)) {
                changed = true;
                unswitched = true;
                break;
            }
        }
        if (!unswitched) {
            break;
        }
    }
    return changed;
}

} // namespace

bool rotate_loops(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        if (function->is_external() || function->entry_block() == nullptr) {
            continue;
        }
        changed |= rotate_loops_in_function(*function, stats);
    }
    return changed;
}

bool unswitch_loops(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        if (function->is_external() || function->entry_block() == nullptr) {
            continue;
        }
        changed |= unswitch_loops_in_function(*function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt
