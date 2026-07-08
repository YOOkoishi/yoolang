#include "oir/OIRScalarOpt.h"

#include "oir/OIRAnalysis.h"
#include "oir/OIRCFGUtils.h"
#include "pass/oir/OIRCostModel.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
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

bool cost_model_allows_loop_transform(Stats &stats, pass::cost_model::TransformKind kind,
                                      const std::string &candidate_id,
                                      std::int64_t before_instrs,
                                      std::int64_t after_instrs,
                                      std::int64_t before_branches,
                                      std::int64_t after_branches,
                                      std::int64_t code_growth,
                                      double confidence,
                                      bool exact_first_peel_has_coupled_lsr = false) {
    OIRTransformCostEstimate estimate;
    estimate.kind = kind;
    estimate.pass_name = "OIRLoopTransforms";
    estimate.candidate_id = candidate_id;
    estimate.scope = "loop";
    estimate.proof_kind = pass::cost_model::ProofKind::Structural;
    estimate.proof_summary = "loop shape, dominance, and side-exit checks";
    estimate.confidence = confidence;
    estimate.before_instrs = before_instrs;
    estimate.after_instrs = after_instrs;
    estimate.before_branches = before_branches;
    estimate.after_branches = after_branches;
    estimate.risk.code_growth = code_growth;
    estimate.risk.cleanup_dependency = code_growth > 0 ? 1 : 0;
    if (kind == pass::cost_model::TransformKind::LoopRotate) {
        estimate.bypass_profitability = true;
        estimate.bypass_reason = "AlwaysOnCanonicalization";
    } else if (kind == pass::cost_model::TransformKind::LoopUnswitch) {
        estimate.bypass_profitability = true;
        estimate.bypass_reason = "LoopInvariantBranch";
    } else if (kind == pass::cost_model::TransformKind::LoopUnroll && before_branches > 0 &&
               after_branches == 0 && code_growth <= 512) {
        estimate.bypass_profitability = true;
        estimate.bypass_reason = "ExactSmallTripCount";
    } else if (kind == pass::cost_model::TransformKind::LoopUnroll &&
               starts_with(candidate_id, "first-peel.") && before_branches > after_branches &&
               code_growth <= 128 && exact_first_peel_has_coupled_lsr) {
        estimate.bypass_profitability = true;
        estimate.bypass_reason = "ExactFirstIterationPeelWithStackLSR";
    }
    return cost_model_allows_transform(stats, estimate);
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
    case oir::Instruction::OpID::And:
    case oir::Instruction::OpID::Xor:
    case oir::Instruction::OpID::SDiv:
    case oir::Instruction::OpID::SRem:
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
    case oir::Instruction::OpID::MemZero:
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
    if (auto *memzero = dynamic_cast<const oir::MemZeroInst *>(&inst)) {
        return std::make_unique<oir::MemZeroInst>(
            memzero->type(), map_value(memzero->ptr(), map),
            map_value(memzero->byte_value(), map),
            map_value(memzero->byte_count(), map), parent);
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

    if (!cost_model_allows_loop_transform(
            stats, pass::cost_model::TransformKind::LoopRotate,
            "rotate." + std::to_string(stats.loop_rotate + 1), 4, 3, 1, 1, 0, 0.66)) {
        return false;
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

struct SingleBlockUnrollMatch {
    oir::BasicBlock *header = nullptr;
    oir::BasicBlock *preheader = nullptr;
    oir::BasicBlock *exit = nullptr;
    bool backedge_is_true = false;
    std::int64_t trip_count = 0;
};

struct MultiBlockUnrollMatch {
    oir::BasicBlock *header = nullptr;
    oir::BasicBlock *preheader = nullptr;
    oir::BasicBlock *latch = nullptr;
    oir::BasicBlock *exit = nullptr;
    std::vector<oir::BasicBlock *> blocks;
    std::int64_t trip_count = 0;
    bool has_call = false;
};

struct FinalIterationPeelMatch {
    MultiBlockUnrollMatch loop;
    oir::CallInst *call = nullptr;
    oir::CmpInst *latch_cmp = nullptr;
    std::size_t latch_bound_operand = 0;
    std::int64_t peeled_bound = 0;
};

struct FirstIterationGuardMatch {
    oir::BranchInst *branch = nullptr;
    bool residual_takes_true = false;
};

bool is_unroll_cloneable_instruction(const oir::Instruction &inst) {
    switch (inst.op()) {
    case oir::Instruction::OpID::Ret:
    case oir::Instruction::OpID::Alloca:
        return false;
    default:
        return true;
    }
}

std::optional<std::int64_t> latch_update_step(const oir::PhiInst *phi,
                                              const HeaderPhiIncoming &incoming) {
    auto *binary = dynamic_cast<oir::BinaryInst *>(incoming.latch);
    if (binary == nullptr) {
        return std::nullopt;
    }
    if (binary->op() == oir::Instruction::OpID::Add) {
        if (binary->lhs() == nullptr || binary->rhs() == nullptr) {
            return std::nullopt;
        }
        if (binary->lhs() == phi) {
            if (auto step = int_constant(binary->rhs())) {
                return step;
            }
        }
        if (binary->rhs() == phi) {
            if (auto step = int_constant(binary->lhs())) {
                return step;
            }
        }
    }
    if (binary->op() == oir::Instruction::OpID::Sub && binary->lhs() != nullptr) {
        if (binary->lhs() == phi) {
            if (auto step = int_constant(binary->rhs())) {
                return -*step;
            }
        }
    }
    return std::nullopt;
}

oir::CmpPred swap_unroll_cmp_operands(oir::CmpPred pred) {
    switch (pred) {
    case oir::CmpPred::LT:
        return oir::CmpPred::GT;
    case oir::CmpPred::LE:
        return oir::CmpPred::GE;
    case oir::CmpPred::GT:
        return oir::CmpPred::LT;
    case oir::CmpPred::GE:
        return oir::CmpPred::LE;
    case oir::CmpPred::EQ:
    case oir::CmpPred::NE:
        return pred;
    }
    return pred;
}

std::optional<std::int64_t> continuing_trip_count_for(std::int64_t start, std::int64_t bound,
                                                       std::int64_t step, oir::CmpPred pred) {
    if (step == 0) {
        return std::nullopt;
    }
    switch (pred) {
    case oir::CmpPred::LT: {
        if (step <= 0) {
            return std::nullopt;
        }
        const std::int64_t distance = bound - start;
        if (distance <= 0) {
            return 0;
        }
        return (distance + step - 1) / step;
    }
    case oir::CmpPred::LE: {
        if (step <= 0) {
            return std::nullopt;
        }
        const std::int64_t distance = bound - start;
        if (distance < 0) {
            return 0;
        }
        return distance / step + 1;
    }
    case oir::CmpPred::GT: {
        if (step >= 0) {
            return std::nullopt;
        }
        const std::int64_t step_abs = -step;
        const std::int64_t distance = start - bound;
        if (distance <= 0) {
            return 0;
        }
        return (distance + step_abs - 1) / step_abs;
    }
    case oir::CmpPred::GE: {
        if (step >= 0) {
            return std::nullopt;
        }
        const std::int64_t step_abs = -step;
        const std::int64_t distance = start - bound;
        if (distance < 0) {
            return 0;
        }
        return distance / step_abs + 1;
    }
    case oir::CmpPred::EQ:
    case oir::CmpPred::NE:
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::int64_t> constant_latch_trip_count(
    oir::BasicBlock *header, oir::BasicBlock *preheader, oir::BasicBlock *latch,
    const oir::BranchInst &branch) {
    auto *cmp = dynamic_cast<oir::CmpInst *>(branch.cond());
    if (cmp == nullptr || cmp->op() != oir::Instruction::OpID::ICmp) {
        return std::nullopt;
    }

    std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> incoming;
    if (!collect_header_phi_incoming(*header, preheader, latch, incoming)) {
        return std::nullopt;
    }

    auto count_from_latch = [&](oir::Value *candidate, oir::Value *bound,
                                oir::CmpPred pred) -> std::optional<std::int64_t> {
        auto limit = int_constant(bound);
        if (!limit.has_value()) {
            return std::nullopt;
        }
        for (const auto &[phi, values] : incoming) {
            if (candidate != values.latch) {
                continue;
            }
            auto start = int_constant(values.outside);
            auto step = latch_update_step(phi, values);
            if (!start.has_value() || !step.has_value()) {
                return std::nullopt;
            }
            auto first_latch_value = static_cast<std::int32_t>(*start + *step);
            auto continuing = continuing_trip_count_for(first_latch_value, *limit, *step, pred);
            if (!continuing.has_value()) {
                return std::nullopt;
            }
            return *continuing + 1;
        }
        return std::nullopt;
    };

    if (auto count = count_from_latch(cmp->lhs(), cmp->rhs(), cmp->pred())) {
        return count;
    }
    return count_from_latch(cmp->rhs(), cmp->lhs(), swap_unroll_cmp_operands(cmp->pred()));
}

std::optional<bool> evaluate_integer_cmp(std::int64_t lhs, std::int64_t rhs,
                                         oir::CmpPred pred) {
    switch (pred) {
    case oir::CmpPred::EQ:
        return lhs == rhs;
    case oir::CmpPred::NE:
        return lhs != rhs;
    case oir::CmpPred::LT:
        return lhs < rhs;
    case oir::CmpPred::LE:
        return lhs <= rhs;
    case oir::CmpPred::GT:
        return lhs > rhs;
    case oir::CmpPred::GE:
        return lhs >= rhs;
    }
    return std::nullopt;
}

bool loop_reachable_from_has_call(oir::BasicBlock *start, const oir::Loop &loop) {
    std::deque<oir::BasicBlock *> worklist;
    std::unordered_set<oir::BasicBlock *> seen;
    worklist.push_back(start);
    seen.insert(start);

    while (!worklist.empty()) {
        auto *block = worklist.front();
        worklist.pop_front();
        for (const auto &inst : block->instructions()) {
            if (inst->op() == oir::Instruction::OpID::Call) {
                return true;
            }
        }
        for (auto *succ : block->successors()) {
            if (!contains_block(loop, succ)) {
                continue;
            }
            if (seen.insert(succ).second) {
                worklist.push_back(succ);
            }
        }
    }
    return false;
}

std::optional<bool> residual_outcome_for_first_iteration_guard(
    const oir::CmpInst &cmp, const oir::Loop &loop, const MultiBlockUnrollMatch &match,
    const std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> &phi_incoming) {
    auto try_phi_constant = [&](oir::Value *candidate, oir::Value *constant,
                                oir::CmpPred pred) -> std::optional<bool> {
        auto *phi = dynamic_cast<oir::PhiInst *>(candidate);
        auto guard_constant = int_constant(constant);
        if (phi == nullptr || phi->parent() != match.header || !guard_constant.has_value()) {
            return std::nullopt;
        }

        auto found = phi_incoming.find(phi);
        if (found == phi_incoming.end()) {
            return std::nullopt;
        }
        auto start = int_constant(found->second.outside);
        auto step = latch_update_step(phi, found->second);
        if (!start.has_value() || !step.has_value()) {
            return std::nullopt;
        }

        std::optional<bool> first_outcome;
        std::optional<bool> residual_outcome;
        for (std::int64_t iteration = 0; iteration < match.trip_count; ++iteration) {
            const std::int64_t value = *start + *step * iteration;
            auto outcome = evaluate_integer_cmp(value, *guard_constant, pred);
            if (!outcome.has_value()) {
                return std::nullopt;
            }
            if (iteration == 0) {
                first_outcome = *outcome;
                continue;
            }
            if (!residual_outcome.has_value()) {
                residual_outcome = *outcome;
                continue;
            }
            if (*residual_outcome != *outcome) {
                return std::nullopt;
            }
        }
        if (!first_outcome.has_value() || !residual_outcome.has_value() ||
            *first_outcome == *residual_outcome) {
            return std::nullopt;
        }
        (void)loop;
        return residual_outcome;
    };

    if (auto outcome = try_phi_constant(cmp.lhs(), cmp.rhs(), cmp.pred())) {
        return outcome;
    }
    return try_phi_constant(cmp.rhs(), cmp.lhs(), swap_unroll_cmp_operands(cmp.pred()));
}

std::optional<FirstIterationGuardMatch> find_first_iteration_guard(
    const oir::Loop &loop, const MultiBlockUnrollMatch &match,
    const std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> &phi_incoming) {
    for (auto *block : match.blocks) {
        if (block == match.latch) {
            continue;
        }
        auto *branch = dynamic_cast<oir::BranchInst *>(block->terminator());
        if (branch == nullptr || !branch->is_conditional() ||
            branch->true_bb() == branch->false_bb() ||
            !contains_block(loop, branch->true_bb()) ||
            !contains_block(loop, branch->false_bb())) {
            continue;
        }
        if (!loop_reachable_from_has_call(branch->true_bb(), loop) &&
            !loop_reachable_from_has_call(branch->false_bb(), loop)) {
            continue;
        }

        auto *cmp = dynamic_cast<oir::CmpInst *>(branch->cond());
        if (cmp == nullptr || cmp->op() != oir::Instruction::OpID::ICmp) {
            continue;
        }
        auto residual_outcome =
            residual_outcome_for_first_iteration_guard(*cmp, loop, match, phi_incoming);
        if (!residual_outcome.has_value()) {
            continue;
        }
        return FirstIterationGuardMatch{branch, *residual_outcome};
    }
    return std::nullopt;
}

oir::AllocaInst *underlying_alloca(oir::Value *value) {
    std::unordered_set<oir::Value *> seen;
    auto *current = value;
    while (current != nullptr && seen.insert(current).second) {
        if (auto *alloca = dynamic_cast<oir::AllocaInst *>(current)) {
            return alloca;
        }
        auto *gep = dynamic_cast<oir::GetElementPtrInst *>(current);
        if (gep == nullptr) {
            return nullptr;
        }
        current = gep->base_ptr();
    }
    return nullptr;
}

bool stack_object_is_non_escaping_impl(oir::Value *value, std::unordered_set<oir::Value *> &seen) {
    if (value == nullptr || !seen.insert(value).second) {
        return true;
    }
    for (auto *user : value->users()) {
        auto *inst = dynamic_cast<oir::Instruction *>(user);
        if (inst == nullptr) {
            return false;
        }
        if (auto *gep = dynamic_cast<oir::GetElementPtrInst *>(inst)) {
            if (gep->base_ptr() != value || !stack_object_is_non_escaping_impl(gep, seen)) {
                return false;
            }
            continue;
        }
        if (dynamic_cast<oir::PhiInst *>(inst) != nullptr) {
            if (!stack_object_is_non_escaping_impl(inst, seen)) {
                return false;
            }
            continue;
        }
        if (auto *load = dynamic_cast<oir::LoadInst *>(inst)) {
            if (load->ptr() == value) {
                continue;
            }
            return false;
        }
        if (auto *store = dynamic_cast<oir::StoreInst *>(inst)) {
            if (store->ptr() == value && store->value() != value) {
                continue;
            }
            return false;
        }
        if (auto *memzero = dynamic_cast<oir::MemZeroInst *>(inst)) {
            if (memzero->ptr() == value) {
                continue;
            }
            return false;
        }
        return false;
    }
    return true;
}

bool stack_object_is_non_escaping(oir::AllocaInst *alloca) {
    std::unordered_set<oir::Value *> seen;
    return stack_object_is_non_escaping_impl(alloca, seen);
}

oir::CallInst *first_call_before_terminator(oir::BasicBlock *block) {
    if (block == nullptr) {
        return nullptr;
    }
    for (const auto &inst : block->instructions()) {
        if (inst->is_terminator()) {
            return nullptr;
        }
        if (auto *call = dynamic_cast<oir::CallInst *>(inst.get())) {
            return call;
        }
    }
    return nullptr;
}

std::optional<std::int64_t> affine_header_phi_offset(
    oir::Value *value, const std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> &phi_incoming,
    oir::PhiInst *&matched_phi) {
    if (auto *phi = dynamic_cast<oir::PhiInst *>(value)) {
        if (phi_incoming.find(phi) == phi_incoming.end()) {
            return std::nullopt;
        }
        matched_phi = phi;
        return 0;
    }
    auto *binary = dynamic_cast<oir::BinaryInst *>(value);
    if (binary == nullptr) {
        return std::nullopt;
    }
    if (binary->op() == oir::Instruction::OpID::Add) {
        if (auto rhs = int_constant(binary->rhs())) {
            if (auto lhs = affine_header_phi_offset(binary->lhs(), phi_incoming, matched_phi)) {
                return *lhs + *rhs;
            }
        }
        if (auto lhs = int_constant(binary->lhs())) {
            if (auto rhs = affine_header_phi_offset(binary->rhs(), phi_incoming, matched_phi)) {
                return *lhs + *rhs;
            }
        }
    }
    if (binary->op() == oir::Instruction::OpID::Sub) {
        if (auto rhs = int_constant(binary->rhs())) {
            if (auto lhs = affine_header_phi_offset(binary->lhs(), phi_incoming, matched_phi)) {
                return *lhs - *rhs;
            }
        }
    }
    return std::nullopt;
}

bool first_peel_has_coupled_stack_lsr_shape(
    const MultiBlockUnrollMatch &match,
    const std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> &phi_incoming,
    const FirstIterationGuardMatch &guard) {
    auto *call = first_call_before_terminator(match.latch);
    if (call == nullptr) {
        return false;
    }

    std::vector<oir::BasicBlock *> path;
    auto *block = guard.residual_takes_true ? guard.branch->true_bb() : guard.branch->false_bb();
    std::unordered_set<oir::BasicBlock *> seen_path;
    while (block != nullptr && seen_path.insert(block).second) {
        if (std::find(match.blocks.begin(), match.blocks.end(), block) == match.blocks.end()) {
            return false;
        }
        path.push_back(block);
        if (block == match.latch) {
            break;
        }
        if (block->successors().size() != 1) {
            return false;
        }
        for (const auto &inst : block->instructions()) {
            if (inst->op() == oir::Instruction::OpID::Call) {
                return false;
            }
        }
        block = block->successors().front();
    }
    if (path.empty() || path.back() != match.latch) {
        return false;
    }

    struct GroupKey {
        oir::AllocaInst *alloca = nullptr;
        oir::PhiInst *phi = nullptr;
    };
    std::vector<GroupKey> groups;
    std::unordered_map<oir::AllocaInst *, bool> alloca_non_escape;

    for (auto *path_block : path) {
        for (const auto &inst : path_block->instructions()) {
            if (inst.get() == call) {
                break;
            }
            if (inst->op() == oir::Instruction::OpID::Call) {
                return false;
            }
            auto *gep = dynamic_cast<oir::GetElementPtrInst *>(inst.get());
            if (gep == nullptr) {
                continue;
            }
            auto *alloca = underlying_alloca(gep->base_ptr());
            if (alloca == nullptr) {
                continue;
            }
            auto [escape_it, inserted] = alloca_non_escape.emplace(alloca, false);
            if (inserted) {
                escape_it->second = stack_object_is_non_escaping(alloca);
            }
            if (!escape_it->second) {
                continue;
            }
            auto indices = gep->indices();
            oir::PhiInst *matched_phi = nullptr;
            unsigned affine_indices = 0;
            for (auto *index : indices) {
                oir::PhiInst *candidate_phi = nullptr;
                if (affine_header_phi_offset(index, phi_incoming, candidate_phi).has_value()) {
                    matched_phi = candidate_phi;
                    ++affine_indices;
                }
            }
            if (matched_phi == nullptr || affine_indices != 1) {
                continue;
            }
            bool seen_group = false;
            for (const auto &group : groups) {
                if (group.alloca == alloca && group.phi == matched_phi) {
                    seen_group = true;
                    break;
                }
            }
            if (!seen_group) {
                groups.push_back({alloca, matched_phi});
            }
        }
    }

    return groups.size() >= 2 && groups.size() <= 4;
}

std::optional<SingleBlockUnrollMatch> match_single_block_unroll_loop(
    const oir::Loop &loop, const oir::ScalarEvolution &scev) {
    if (loop.header == nullptr || loop.blocks.size() != 1 || loop.blocks.front() != loop.header) {
        return std::nullopt;
    }

    auto *header = mutable_block(loop.header);
    auto *branch = dynamic_cast<oir::BranchInst *>(header->terminator());
    if (branch == nullptr || !branch->is_conditional()) {
        return std::nullopt;
    }

    bool backedge_is_true = false;
    oir::BasicBlock *exit = nullptr;
    if (branch->true_bb() == header && branch->false_bb() != header) {
        backedge_is_true = true;
        exit = branch->false_bb();
    } else if (branch->false_bb() == header && branch->true_bb() != header) {
        backedge_is_true = false;
        exit = branch->true_bb();
    } else {
        return std::nullopt;
    }
    if (exit == nullptr || contains_block(loop, exit)) {
        return std::nullopt;
    }

    auto *preheader = find_preheader(loop);
    if (preheader == nullptr || !preheader->has_terminator()) {
        return std::nullopt;
    }
    if (std::count(header->predecessors().begin(), header->predecessors().end(), preheader) != 1 ||
        std::count(header->predecessors().begin(), header->predecessors().end(), header) != 1) {
        return std::nullopt;
    }

    auto trip_count = constant_latch_trip_count(header, preheader, header, *branch);
    if (!trip_count.has_value()) {
        trip_count = scev.constant_trip_count(loop);
    }
    constexpr std::int64_t kMaxUnrollTripCount = 16;
    if (!trip_count.has_value() || *trip_count <= 1 || *trip_count > kMaxUnrollTripCount) {
        return std::nullopt;
    }

    for (const auto &inst : header->instructions()) {
        if (inst->op() == oir::Instruction::OpID::Phi) {
            continue;
        }
        if (inst->is_terminator()) {
            break;
        }
        if (!is_unroll_cloneable_instruction(*inst)) {
            return std::nullopt;
        }
        if (inst->op() == oir::Instruction::OpID::Call) {
            return std::nullopt;
        }
    }

    return SingleBlockUnrollMatch{header, preheader, exit, backedge_is_true, *trip_count};
}

bool loop_body_is_acyclic_except_latch(const oir::Loop &loop, oir::BasicBlock *latch) {
    enum class VisitState { Visiting, Done };
    std::unordered_map<const oir::BasicBlock *, VisitState> state;

    auto dfs = [&](const oir::BasicBlock *block, const auto &self) -> bool {
        auto found = state.find(block);
        if (found != state.end()) {
            return found->second == VisitState::Done;
        }
        state[block] = VisitState::Visiting;
        for (auto *succ : block->successors()) {
            if (!contains_block(loop, succ)) {
                continue;
            }
            if (block == latch && succ == loop.header) {
                continue;
            }
            if (!self(succ, self)) {
                return false;
            }
        }
        state[block] = VisitState::Done;
        return true;
    };

    return dfs(loop.header, dfs);
}

std::optional<MultiBlockUnrollMatch> match_multi_block_unroll_loop(
    const oir::Loop &loop, const oir::ScalarEvolution &scev) {
    if (loop.header == nullptr || loop.blocks.size() <= 1) {
        return std::nullopt;
    }

    auto *header = mutable_block(loop.header);
    auto *preheader = find_preheader(loop);
    auto *latch = mutable_block(single_latch(loop));
    if (preheader == nullptr || latch == nullptr || !preheader->has_terminator() ||
        !latch->has_terminator()) {
        return std::nullopt;
    }
    if (preheader->name().find(".unr") != std::string::npos) {
        return std::nullopt;
    }

    auto *preheader_branch = dynamic_cast<oir::BranchInst *>(preheader->terminator());
    auto *latch_branch = dynamic_cast<oir::BranchInst *>(latch->terminator());
    if (preheader_branch == nullptr || preheader_branch->is_conditional() ||
        preheader_branch->target_bb() != header || latch_branch == nullptr ||
        !latch_branch->is_conditional()) {
        return std::nullopt;
    }

    oir::BasicBlock *exit = nullptr;
    if (latch_branch->true_bb() == header && !contains_block(loop, latch_branch->false_bb())) {
        exit = latch_branch->false_bb();
    } else if (latch_branch->false_bb() == header &&
               !contains_block(loop, latch_branch->true_bb())) {
        exit = latch_branch->true_bb();
    } else {
        return std::nullopt;
    }
    if (exit == nullptr) {
        return std::nullopt;
    }

    for (auto *block : loop.blocks) {
        if (block == loop.header) {
            continue;
        }
        for (auto *pred : block->predecessors()) {
            if (!contains_block(loop, pred)) {
                return std::nullopt;
            }
        }
    }
    for (auto *block : loop.blocks) {
        for (auto *succ : block->successors()) {
            if (contains_block(loop, succ)) {
                continue;
            }
            if (block != latch || succ != exit) {
                return std::nullopt;
            }
        }
    }
    if (!loop_body_is_acyclic_except_latch(loop, latch)) {
        return std::nullopt;
    }

    auto trip_count = constant_latch_trip_count(header, preheader, latch, *latch_branch);
    if (!trip_count.has_value()) {
        trip_count = scev.constant_trip_count(loop);
    }
    constexpr std::int64_t kMaxUnrollTripCount = 16;
    if (!trip_count.has_value() || *trip_count <= 1 || *trip_count > kMaxUnrollTripCount) {
        return std::nullopt;
    }

    auto blocks = function_ordered_loop_blocks(*header->parent(), loop);
    bool has_call = false;
    for (auto *block : blocks) {
        for (const auto &inst : block->instructions()) {
            if (block == header && inst->op() == oir::Instruction::OpID::Phi) {
                continue;
            }
            if (!is_unroll_cloneable_instruction(*inst)) {
                return std::nullopt;
            }
            if (inst->op() == oir::Instruction::OpID::Call) {
                has_call = true;
            }
        }
    }

    return MultiBlockUnrollMatch{header, preheader, latch, exit, std::move(blocks), *trip_count,
                                 has_call};
}

bool collect_single_block_phi_incoming(
    oir::BasicBlock *header, oir::BasicBlock *preheader,
    std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> &incoming) {
    return collect_header_phi_incoming(*header, preheader, header, incoming);
}

void rewrite_exit_phi_incoming(oir::BasicBlock *exit, oir::BasicBlock *old_pred,
                               oir::BasicBlock *new_pred, const ValueMap &final_map) {
    for (auto &inst : exit->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        for (std::size_t i = 0; i < phi->incoming().size(); ++i) {
            if (phi->incoming()[i].second != old_pred) {
                continue;
            }
            phi->set_operand(i * 2, map_value(phi->incoming()[i].first, final_map));
            phi->set_operand(i * 2 + 1, new_pred);
        }
    }
}

void fix_cloned_operands(const std::vector<oir::BasicBlock *> &clones, const ValueMap &map);
void force_branch_direction(oir::BranchInst &branch, bool take_true);

bool unroll_single_block_loop(oir::Function &function, const oir::Loop &loop,
                              const oir::ScalarEvolution &scev, Stats &stats) {
    auto match = match_single_block_unroll_loop(loop, scev);
    if (!match.has_value()) {
        return false;
    }

    std::vector<oir::BasicBlock *> blocks = {match->header};
    if (!direct_loop_value_uses_are_repairable(blocks, loop)) {
        return false;
    }
    auto direct_exit_values = loop_values_used_on_exit_edge(match->exit, loop);

    const auto body_instrs = static_cast<std::int64_t>(loop_instruction_count(blocks));
    const auto cloned_instrs = body_instrs * match->trip_count;
    constexpr std::int64_t kMaxClonedInstructions = 512;
    if (cloned_instrs > kMaxClonedInstructions) {
        return false;
    }
    if (!cost_model_allows_loop_transform(
            stats, pass::cost_model::TransformKind::LoopUnroll,
            "unroll." + std::to_string(stats.loop_unroll + 1), body_instrs * match->trip_count,
            cloned_instrs, match->trip_count, 0, cloned_instrs - body_instrs, 0.78)) {
        return false;
    }

    std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> phi_incoming;
    if (!collect_single_block_phi_incoming(match->header, match->preheader, phi_incoming)) {
        return false;
    }

    auto *module = function.parent();
    std::vector<oir::BasicBlock *> unrolled_blocks;
    unrolled_blocks.reserve(static_cast<std::size_t>(match->trip_count));
    ValueMap iteration_map;
    for (const auto &[phi, incoming] : phi_incoming) {
        iteration_map[phi] = incoming.outside;
    }

    ValueMap final_map;
    for (std::int64_t iteration = 0; iteration < match->trip_count; ++iteration) {
        auto *clone_block = function.create_block(match->header->name() + ".unr");
        unrolled_blocks.push_back(clone_block);
        ValueMap body_map = iteration_map;

        for (const auto &inst : match->header->instructions()) {
            if (inst->op() == oir::Instruction::OpID::Phi) {
                continue;
            }
            if (inst->is_terminator()) {
                break;
            }
            auto clone = clone_instruction(*inst, body_map, clone_block, ".unr");
            if (clone == nullptr) {
                return false;
            }
            auto *raw = clone.get();
            clone_block->append_instruction(std::move(clone));
            body_map[inst.get()] = raw;
        }

        final_map = body_map;
        ValueMap next_iteration_map;
        for (const auto &[phi, incoming] : phi_incoming) {
            next_iteration_map[phi] = map_value(incoming.latch, body_map);
        }
        iteration_map = std::move(next_iteration_map);
    }

    replace_terminator_with_br(match->preheader, unrolled_blocks.front());
    oir::cfg::remove_edge_no_phi_update(match->preheader, match->header);

    for (std::size_t i = 0; i < unrolled_blocks.size(); ++i) {
        auto *target = i + 1 < unrolled_blocks.size() ? unrolled_blocks[i + 1] : match->exit;
        oir::cfg::append_unconditional_branch(*module, unrolled_blocks[i], target);
    }

    rewrite_exit_phi_incoming(match->exit, match->header, unrolled_blocks.back(), final_map);
    for (auto *value : direct_exit_values) {
        auto *mapped = map_value(value, final_map);
        if (mapped != value) {
            replace_non_phi_uses_in_block(match->exit, value, mapped);
            replace_phi_uses_on_successor_edges(match->exit, value, mapped);
        }
    }
    oir::cfg::remove_edge_no_phi_update(match->header, match->exit);
    oir::cfg::remove_edge_no_phi_update(match->header, match->header);
    function.erase_block(match->header);

    ++stats.loop_unroll;
    ++stats.cfg;
    return true;
}

struct UnrolledIteration {
    oir::BasicBlock *header = nullptr;
    oir::BasicBlock *latch = nullptr;
    std::vector<oir::BasicBlock *> blocks;
    ValueMap value_map;
};

std::optional<UnrolledIteration> clone_unroll_iteration(
    oir::Function &function, const MultiBlockUnrollMatch &match,
    const ValueMap &iteration_map, std::int64_t iteration) {
    BlockMap block_map;
    ValueMap value_map = iteration_map;
    UnrolledIteration out;
    out.blocks.reserve(match.blocks.size());

    for (auto *block : match.blocks) {
        auto *clone = function.create_block(block->name() + ".unr" + std::to_string(iteration));
        block_map[block] = clone;
        value_map[block] = clone;
        out.blocks.push_back(clone);
        if (block == match.header) {
            out.header = clone;
        }
        if (block == match.latch) {
            out.latch = clone;
        }
    }

    for (auto *block : match.blocks) {
        auto *clone_block = block_map.at(block);
        for (const auto &inst : block->instructions()) {
            if (block == match.header && inst->op() == oir::Instruction::OpID::Phi) {
                continue;
            }
            auto clone = clone_instruction(*inst, value_map, clone_block, ".unr");
            if (clone == nullptr) {
                return std::nullopt;
            }
            auto *raw = clone.get();
            clone_block->append_instruction(std::move(clone));
            value_map[inst.get()] = raw;
        }
    }

    fix_cloned_operands(out.blocks, value_map);
    for (auto *block : match.blocks) {
        if (block == match.latch) {
            continue;
        }
        auto *clone = block_map.at(block);
        for (auto *succ : block->successors()) {
            auto found = block_map.find(succ);
            if (found != block_map.end()) {
                oir::cfg::add_edge(clone, found->second);
            }
        }
    }

    out.value_map = std::move(value_map);
    return out;
}

std::optional<UnrolledIteration> clone_first_peel_iteration(
    oir::Function &function, const MultiBlockUnrollMatch &match,
    const ValueMap &iteration_map) {
    BlockMap block_map;
    ValueMap value_map = iteration_map;
    UnrolledIteration out;
    out.blocks.reserve(match.blocks.size());

    for (auto *block : match.blocks) {
        auto *clone = function.create_block(block->name() + ".peel.first");
        block_map[block] = clone;
        value_map[block] = clone;
        out.blocks.push_back(clone);
        if (block == match.header) {
            out.header = clone;
        }
        if (block == match.latch) {
            out.latch = clone;
        }
    }

    for (auto *block : match.blocks) {
        auto *clone_block = block_map.at(block);
        for (const auto &inst : block->instructions()) {
            if (block == match.header && inst->op() == oir::Instruction::OpID::Phi) {
                continue;
            }
            auto clone = clone_instruction(*inst, value_map, clone_block, ".peel.first");
            if (clone == nullptr) {
                return std::nullopt;
            }
            auto *raw = clone.get();
            clone_block->append_instruction(std::move(clone));
            value_map[inst.get()] = raw;
        }
    }

    fix_cloned_operands(out.blocks, value_map);
    for (auto *block : match.blocks) {
        if (block == match.latch) {
            continue;
        }
        auto *clone = block_map.at(block);
        for (auto *succ : block->successors()) {
            auto found = block_map.find(succ);
            if (found != block_map.end()) {
                oir::cfg::add_edge(clone, found->second);
            }
        }
    }

    out.value_map = std::move(value_map);
    return out;
}

bool rewrite_header_phi_initial_values_after_first_peel(
    oir::BasicBlock *header, oir::BasicBlock *preheader, oir::BasicBlock *peeled_latch,
    const std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> &phi_incoming,
    const ValueMap &peeled_value_map) {
    for (auto &inst : header->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        auto found = phi_incoming.find(phi);
        if (found == phi_incoming.end()) {
            return false;
        }
        bool updated = false;
        for (std::size_t i = 0; i < phi->incoming().size(); ++i) {
            if (phi->incoming()[i].second != preheader) {
                continue;
            }
            phi->set_operand(i * 2, map_value(found->second.latch, peeled_value_map));
            phi->set_operand(i * 2 + 1, peeled_latch);
            updated = true;
            break;
        }
        if (!updated) {
            return false;
        }
    }
    return true;
}

void remove_original_loop_cfg_edges(const std::vector<oir::BasicBlock *> &blocks) {
    for (auto *block : blocks) {
        auto successors = block->successors();
        for (auto *succ : successors) {
            oir::cfg::remove_edge_no_phi_update(block, succ);
        }
    }
}

bool unroll_multi_block_loop(oir::Function &function, const oir::Loop &loop,
                             const oir::ScalarEvolution &scev, Stats &stats) {
    auto match = match_multi_block_unroll_loop(loop, scev);
    if (!match.has_value()) {
        return false;
    }
    if (match->has_call) {
        return false;
    }

    if (!direct_loop_value_uses_are_repairable(match->blocks, loop)) {
        return false;
    }
    auto direct_exit_values = loop_values_used_on_exit_edge(match->exit, loop);

    const auto body_instrs = static_cast<std::int64_t>(loop_instruction_count(match->blocks));
    const auto cloned_instrs = body_instrs * match->trip_count;
    constexpr std::int64_t kMaxClonedInstructions = 512;
    if (cloned_instrs > kMaxClonedInstructions) {
        return false;
    }
    if (!cost_model_allows_loop_transform(
            stats, pass::cost_model::TransformKind::LoopUnroll,
            "unroll." + std::to_string(stats.loop_unroll + 1), body_instrs * match->trip_count,
            cloned_instrs, match->trip_count, 0, cloned_instrs - body_instrs, 0.74)) {
        return false;
    }

    std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> phi_incoming;
    if (!collect_header_phi_incoming(*match->header, match->preheader, match->latch,
                                     phi_incoming)) {
        return false;
    }

    ValueMap iteration_map;
    for (const auto &[phi, incoming] : phi_incoming) {
        iteration_map[phi] = incoming.outside;
    }

    std::vector<UnrolledIteration> iterations;
    iterations.reserve(static_cast<std::size_t>(match->trip_count));
    ValueMap final_map;
    for (std::int64_t iteration = 0; iteration < match->trip_count; ++iteration) {
        auto cloned = clone_unroll_iteration(function, *match, iteration_map, iteration);
        if (!cloned.has_value()) {
            return false;
        }
        final_map = cloned->value_map;

        ValueMap next_iteration_map;
        for (const auto &[phi, incoming] : phi_incoming) {
            next_iteration_map[phi] = map_value(incoming.latch, cloned->value_map);
        }
        iteration_map = std::move(next_iteration_map);
        iterations.push_back(std::move(*cloned));
    }

    replace_terminator_with_br(match->preheader, iterations.front().header);
    oir::cfg::remove_edge_no_phi_update(match->preheader, match->header);
    for (std::size_t i = 0; i < iterations.size(); ++i) {
        auto *target = i + 1 < iterations.size() ? iterations[i + 1].header : match->exit;
        replace_terminator_with_br(iterations[i].latch, target);
    }

    rewrite_exit_phi_incoming(match->exit, match->latch, iterations.back().latch, final_map);
    for (auto *value : direct_exit_values) {
        auto *mapped = map_value(value, final_map);
        if (mapped != value) {
            replace_non_phi_uses_in_block(match->exit, value, mapped);
            replace_phi_uses_on_successor_edges(match->exit, value, mapped);
        }
    }

    remove_original_loop_cfg_edges(match->blocks);
    for (auto *block : match->blocks) {
        function.erase_block(block);
    }

    ++stats.loop_unroll;
    ++stats.cfg;
    return true;
}

bool peel_first_iteration_loop(oir::Function &function, const oir::Loop &loop,
                               const oir::ScalarEvolution &scev, Stats &stats) {
    auto match = match_multi_block_unroll_loop(loop, scev);
    if (!match.has_value() || !match->has_call || match->trip_count <= 1 ||
        match->trip_count > 16) {
        return false;
    }
    if (match->preheader->name().find(".peel.first") != std::string::npos ||
        match->header->name().find(".peel.first") != std::string::npos) {
        return false;
    }
    if (!direct_loop_value_uses_are_repairable(match->blocks, loop)) {
        return false;
    }

    std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> phi_incoming;
    if (!collect_header_phi_incoming(*match->header, match->preheader, match->latch,
                                     phi_incoming)) {
        return false;
    }
    auto guard = find_first_iteration_guard(loop, *match, phi_incoming);
    if (!guard.has_value()) {
        return false;
    }
    const bool has_coupled_lsr =
        first_peel_has_coupled_stack_lsr_shape(*match, phi_incoming, *guard);

    const auto body_instrs = static_cast<std::int64_t>(loop_instruction_count(match->blocks));
    constexpr std::int64_t kMaxPeeledInstructions = 128;
    if (body_instrs > kMaxPeeledInstructions) {
        return false;
    }
    if (!cost_model_allows_loop_transform(
            stats, pass::cost_model::TransformKind::LoopUnroll,
            "first-peel." + std::to_string(stats.loop_unroll + 1),
            body_instrs * match->trip_count, body_instrs * match->trip_count + body_instrs,
            match->trip_count, match->trip_count - 1, body_instrs, 0.76,
            has_coupled_lsr)) {
        return false;
    }

    ValueMap first_iteration_map;
    for (const auto &[phi, incoming] : phi_incoming) {
        first_iteration_map[phi] = incoming.outside;
    }

    auto peeled = clone_first_peel_iteration(function, *match, first_iteration_map);
    if (!peeled.has_value() || peeled->header == nullptr || peeled->latch == nullptr) {
        return false;
    }

    replace_terminator_with_br(match->preheader, peeled->header);
    oir::cfg::remove_edge_no_phi_update(match->preheader, match->header);
    replace_terminator_with_br(peeled->latch, match->header);
    if (!rewrite_header_phi_initial_values_after_first_peel(
            match->header, match->preheader, peeled->latch, phi_incoming, peeled->value_map)) {
        return false;
    }
    force_branch_direction(*guard->branch, guard->residual_takes_true);

    ++stats.loop_unroll;
    ++stats.cfg;
    return true;
}

bool is_final_peel_post_call_instruction(const oir::Instruction &inst) {
    switch (inst.op()) {
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::GetElementPtr:
    case oir::Instruction::OpID::ICmp:
        return true;
    default:
        return false;
    }
}

bool exit_block_is_void_return_only(const oir::BasicBlock &block) {
    auto *ret = dynamic_cast<oir::ReturnInst *>(block.terminator());
    if (ret == nullptr || ret->has_value()) {
        return false;
    }
    for (const auto &inst : block.instructions()) {
        if (inst.get() == ret) {
            return true;
        }
        if (dynamic_cast<oir::PhiInst *>(inst.get()) == nullptr) {
            return false;
        }
    }
    return false;
}

std::optional<FinalIterationPeelMatch> match_final_iteration_peel_loop(
    oir::Function &function, const oir::Loop &loop, const oir::ScalarEvolution &scev) {
    if (function.return_type() == nullptr || !function.return_type()->is_void()) {
        return std::nullopt;
    }
    auto base = match_multi_block_unroll_loop(loop, scev);
    if (!base.has_value() || !base->has_call || base->trip_count <= 1 ||
        base->trip_count > 16 || !exit_block_is_void_return_only(*base->exit) ||
        !loop_values_used_on_exit_edge(base->exit, loop).empty()) {
        return std::nullopt;
    }

    auto *latch_branch = dynamic_cast<oir::BranchInst *>(base->latch->terminator());
    auto *latch_cmp = latch_branch == nullptr ? nullptr
                                              : dynamic_cast<oir::CmpInst *>(latch_branch->cond());
    if (latch_branch == nullptr || latch_cmp == nullptr || latch_cmp->op() != oir::Instruction::OpID::ICmp ||
        latch_branch->true_bb() != base->header || latch_branch->false_bb() != base->exit) {
        return std::nullopt;
    }

    std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> phi_incoming;
    if (!collect_header_phi_incoming(*base->header, base->preheader, base->latch, phi_incoming)) {
        return std::nullopt;
    }

    std::size_t bound_operand = 0;
    std::int64_t peeled_bound = 0;
    bool found_bound = false;
    auto try_match_bound = [&](oir::Value *candidate, oir::Value *bound,
                               oir::CmpPred pred, std::size_t operand_index) {
        if (found_bound || (pred != oir::CmpPred::LT && pred != oir::CmpPred::LE)) {
            return;
        }
        auto limit = int_constant(bound);
        if (!limit.has_value()) {
            return;
        }
        for (const auto &[phi, incoming] : phi_incoming) {
            if (candidate != incoming.latch) {
                continue;
            }
            auto step = latch_update_step(phi, incoming);
            if (!step.has_value() || *step != 1) {
                continue;
            }
            bound_operand = operand_index;
            peeled_bound = *limit - 1;
            found_bound = true;
            return;
        }
    };
    try_match_bound(latch_cmp->lhs(), latch_cmp->rhs(), latch_cmp->pred(), 1);
    try_match_bound(latch_cmp->rhs(), latch_cmp->lhs(),
                    swap_unroll_cmp_operands(latch_cmp->pred()), 0);
    if (!found_bound) {
        return std::nullopt;
    }

    oir::CallInst *call = nullptr;
    for (auto *block : base->blocks) {
        bool after_call = false;
        for (const auto &inst : block->instructions()) {
            if (inst->is_terminator()) {
                break;
            }
            auto *candidate_call = dynamic_cast<oir::CallInst *>(inst.get());
            if (candidate_call != nullptr) {
                if (call != nullptr || block != base->latch || candidate_call->use_count() != 0) {
                    return std::nullopt;
                }
                call = candidate_call;
                after_call = true;
                continue;
            }
            if (after_call && !is_final_peel_post_call_instruction(*inst)) {
                return std::nullopt;
            }
        }
    }
    if (call == nullptr) {
        return std::nullopt;
    }
    if (dynamic_cast<oir::Function *>(call->callee()) != &function) {
        return std::nullopt;
    }

    return FinalIterationPeelMatch{std::move(*base), call, latch_cmp, bound_operand,
                                   peeled_bound};
}

std::optional<UnrolledIteration> clone_final_peel_iteration(
    oir::Function &function, const MultiBlockUnrollMatch &match, const ValueMap &iteration_map) {
    BlockMap block_map;
    ValueMap value_map = iteration_map;
    UnrolledIteration out;
    out.blocks.reserve(match.blocks.size());

    for (auto *block : match.blocks) {
        auto *clone = function.create_block(block->name() + ".peel");
        block_map[block] = clone;
        value_map[block] = clone;
        out.blocks.push_back(clone);
        if (block == match.header) {
            out.header = clone;
        }
        if (block == match.latch) {
            out.latch = clone;
        }
    }

    for (auto *block : match.blocks) {
        auto *clone_block = block_map.at(block);
        for (const auto &inst : block->instructions()) {
            if (block == match.header && inst->op() == oir::Instruction::OpID::Phi) {
                continue;
            }
            if (block == match.latch && inst->is_terminator()) {
                break;
            }
            auto clone = clone_instruction(*inst, value_map, clone_block, ".peel");
            if (clone == nullptr) {
                return std::nullopt;
            }
            auto *raw = clone.get();
            clone_block->append_instruction(std::move(clone));
            value_map[inst.get()] = raw;
            if (block == match.latch && inst->op() == oir::Instruction::OpID::Call) {
                break;
            }
        }
    }

    fix_cloned_operands(out.blocks, value_map);
    for (auto *block : match.blocks) {
        auto *clone = block_map.at(block);
        if (block == match.latch) {
            continue;
        }
        for (auto *succ : block->successors()) {
            auto found = block_map.find(succ);
            if (found != block_map.end()) {
                oir::cfg::add_edge(clone, found->second);
            }
        }
    }

    out.value_map = std::move(value_map);
    return out;
}

bool peel_final_iteration_loop(oir::Function &function, const oir::Loop &loop,
                               const oir::ScalarEvolution &scev, Stats &stats) {
    constexpr bool kEnableFinalIterationPeel = false;
    if (!kEnableFinalIterationPeel) {
        (void)function;
        (void)loop;
        (void)scev;
        (void)stats;
        return false;
    }

    auto match = match_final_iteration_peel_loop(function, loop, scev);
    if (!match.has_value()) {
        return false;
    }

    const auto body_instrs = static_cast<std::int64_t>(loop_instruction_count(match->loop.blocks));
    constexpr std::int64_t kMaxPeeledInstructions = 128;
    if (body_instrs > kMaxPeeledInstructions) {
        return false;
    }
    if (!cost_model_allows_loop_transform(
            stats, pass::cost_model::TransformKind::LoopUnroll,
            "final-peel." + std::to_string(stats.loop_unroll + 1),
            body_instrs * match->loop.trip_count, body_instrs * match->loop.trip_count + body_instrs,
            match->loop.trip_count, match->loop.trip_count - 1, body_instrs, 0.72)) {
        return false;
    }

    std::unordered_map<oir::PhiInst *, HeaderPhiIncoming> phi_incoming;
    if (!collect_header_phi_incoming(*match->loop.header, match->loop.preheader,
                                     match->loop.latch, phi_incoming)) {
        return false;
    }
    ValueMap final_iteration_map;
    for (const auto &[phi, incoming] : phi_incoming) {
        final_iteration_map[phi] = incoming.latch;
    }

    auto peeled = clone_final_peel_iteration(function, match->loop, final_iteration_map);
    if (!peeled.has_value() || peeled->header == nullptr || peeled->latch == nullptr) {
        return false;
    }

    match->latch_cmp->set_operand(
        match->latch_bound_operand,
        make_int_constant(*function.parent(), match->latch_cmp->operand(match->latch_bound_operand)->type(),
                          match->peeled_bound));

    auto *latch_branch = static_cast<oir::BranchInst *>(match->loop.latch->terminator());
    if (!oir::cfg::replace_branch_target(*latch_branch, match->loop.exit, peeled->header)) {
        return false;
    }
    oir::cfg::remove_edge_no_phi_update(match->loop.latch, match->loop.exit);
    oir::cfg::add_edge(match->loop.latch, peeled->header);
    oir::cfg::append_unconditional_branch(*function.parent(), peeled->latch, match->loop.exit);
    oir::cfg::replace_phi_incoming_block(match->loop.exit, match->loop.latch, peeled->latch);

    ++stats.loop_unroll;
    ++stats.cfg;
    return true;
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

    const auto original_instrs =
        static_cast<std::int64_t>(loop_instruction_count(original_blocks));
    if (!cost_model_allows_loop_transform(
            stats, pass::cost_model::TransformKind::LoopUnswitch,
            "unswitch." + std::to_string(stats.loop_unswitch + 1), original_instrs + 6,
            original_instrs, 1, 0, original_instrs / 2, 0.62)) {
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

struct NormalizedCmp {
    oir::CmpPred pred = oir::CmpPred::EQ;
    oir::Value *lhs = nullptr;
    oir::Value *rhs = nullptr;
};

struct ZeroStoreLoopMatch {
    oir::BasicBlock *header = nullptr;
    oir::BasicBlock *preheader = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::Value *start_ptr = nullptr;
    oir::Value *bound = nullptr;
    std::int64_t start = 0;
    std::uint8_t byte_value = 0;
    oir::CmpPred pred = oir::CmpPred::LT;
    std::uint64_t element_size = 0;
};

struct GuardedLoopBoundMatch {
    oir::BasicBlock *header = nullptr;
    oir::BasicBlock *preheader = nullptr;
    oir::BasicBlock *body = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::BasicBlock *skipped = nullptr;
    oir::BasicBlock *active = nullptr;
    oir::BasicBlock *latch = nullptr;
    oir::PhiInst *iv = nullptr;
    oir::CmpInst *header_cmp = nullptr;
    oir::Value *upper = nullptr;
    oir::Value *limit = nullptr;
};

oir::CmpPred negate_predicate(oir::CmpPred pred) {
    switch (pred) {
    case oir::CmpPred::EQ:
        return oir::CmpPred::NE;
    case oir::CmpPred::NE:
        return oir::CmpPred::EQ;
    case oir::CmpPred::LT:
        return oir::CmpPred::GE;
    case oir::CmpPred::LE:
        return oir::CmpPred::GT;
    case oir::CmpPred::GT:
        return oir::CmpPred::LE;
    case oir::CmpPred::GE:
        return oir::CmpPred::LT;
    }
    return pred;
}

oir::CmpPred swap_predicate_operands(oir::CmpPred pred) {
    switch (pred) {
    case oir::CmpPred::LT:
        return oir::CmpPred::GT;
    case oir::CmpPred::LE:
        return oir::CmpPred::GE;
    case oir::CmpPred::GT:
        return oir::CmpPred::LT;
    case oir::CmpPred::GE:
        return oir::CmpPred::LE;
    case oir::CmpPred::EQ:
    case oir::CmpPred::NE:
        return pred;
    }
    return pred;
}

std::uint64_t oir_type_size(oir::Type *type) {
    if (type == nullptr || type->is_void() || type->is_label() || type->is_function()) {
        return 0;
    }
    if (auto *integer = dynamic_cast<oir::IntegerType *>(type)) {
        return (integer->bit_width() + 7) / 8;
    }
    if (type->is_float()) {
        return 4;
    }
    if (type->is_pointer()) {
        return 8;
    }
    if (auto *array = dynamic_cast<oir::ArrayType *>(type)) {
        return oir_type_size(array->element_type()) * array->element_count();
    }
    return 0;
}

std::optional<std::uint8_t> repeated_byte_store_value(oir::Value *value) {
    auto int_zero = int_constant(value);
    if (int_zero.has_value()) {
        auto *integer = dynamic_cast<oir::IntegerType *>(value->type());
        if (integer == nullptr || integer->bit_width() != 32) {
            return std::nullopt;
        }
        const auto word = static_cast<std::uint32_t>(*int_zero);
        const auto byte = static_cast<std::uint8_t>(word & 0xffU);
        const auto repeated = static_cast<std::uint32_t>(byte) |
                              (static_cast<std::uint32_t>(byte) << 8U) |
                              (static_cast<std::uint32_t>(byte) << 16U) |
                              (static_cast<std::uint32_t>(byte) << 24U);
        if (word == repeated) {
            return byte;
        }
        return std::nullopt;
    }
    auto float_zero = float_constant(value);
    if (float_zero.has_value()) {
        return *float_zero == 0.0F ? std::optional<std::uint8_t>(0) : std::nullopt;
    }
    if (dynamic_cast<oir::ConstantZero *>(value) != nullptr) {
        return 0;
    }
    return std::nullopt;
}

bool is_allowed_zero_loop_instruction(const oir::Instruction &inst) {
    switch (inst.op()) {
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::Mul:
    case oir::Instruction::OpID::And:
    case oir::Instruction::OpID::Xor:
    case oir::Instruction::OpID::ICmp:
    case oir::Instruction::OpID::GetElementPtr:
    case oir::Instruction::OpID::ZExt:
        return true;
    default:
        return false;
    }
}

oir::Value *incoming_value_from(const oir::PhiInst &phi, const oir::BasicBlock *pred) {
    for (const auto &incoming : phi.incoming()) {
        if (incoming.second == pred) {
            return incoming.first;
        }
    }
    return nullptr;
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

bool loop_defs_have_no_external_uses(const oir::Loop &loop) {
    for (auto *const_block : loop.blocks) {
        for (const auto &inst : const_block->instructions()) {
            for (const auto &use : inst->uses()) {
                auto *user_inst = dynamic_cast<oir::Instruction *>(use.user);
                if (user_inst != nullptr && !contains_block(loop, user_inst->parent())) {
                    return false;
                }
            }
        }
    }
    return true;
}

oir::StoreInst *single_byte_pattern_store(const oir::BasicBlock &block,
                                          std::uint64_t &element_size,
                                          std::uint8_t &byte_value) {
    oir::StoreInst *store = nullptr;
    for (const auto &inst : block.instructions()) {
        if (dynamic_cast<const oir::PhiInst *>(inst.get()) != nullptr || inst->is_terminator()) {
            continue;
        }
        if (auto *candidate = dynamic_cast<oir::StoreInst *>(inst.get())) {
            auto pattern = repeated_byte_store_value(candidate->value());
            if (store != nullptr || !pattern.has_value()) {
                return nullptr;
            }
            element_size = oir_type_size(candidate->value()->type());
            if (element_size != 4) {
                return nullptr;
            }
            byte_value = *pattern;
            store = candidate;
            continue;
        }
        if (!is_allowed_zero_loop_instruction(*inst)) {
            return nullptr;
        }
    }
    return store;
}

bool match_unit_gep_from_phi(oir::Value *value, oir::PhiInst *phi) {
    auto *gep = dynamic_cast<oir::GetElementPtrInst *>(value);
    if (gep == nullptr || gep->base_ptr() != phi) {
        return false;
    }
    auto indices = gep->indices();
    return indices.size() == 1 && is_int_value(indices.front(), 1);
}

oir::BinaryInst *match_increment_by_one(oir::Value *value, oir::PhiInst *phi) {
    auto *binary = dynamic_cast<oir::BinaryInst *>(value);
    if (binary == nullptr || binary->op() != oir::Instruction::OpID::Add) {
        return nullptr;
    }
    if ((binary->lhs() == phi && is_int_value(binary->rhs(), 1)) ||
        (binary->rhs() == phi && is_int_value(binary->lhs(), 1))) {
        return binary;
    }
    return nullptr;
}

bool is_i32_value(oir::Value *value) {
    auto *type = value == nullptr ? nullptr : dynamic_cast<oir::IntegerType *>(value->type());
    return type != nullptr && type->bit_width() == 32;
}

oir::BasicBlock *unconditional_branch_target(oir::BasicBlock *block) {
    auto *branch = block == nullptr ? nullptr : dynamic_cast<oir::BranchInst *>(block->terminator());
    if (branch == nullptr || branch->is_conditional()) {
        return nullptr;
    }
    return branch->target_bb();
}

bool is_loop_local_unit_increment(oir::Value *value, oir::PhiInst *iv,
                                  std::unordered_set<oir::Value *> &active) {
    if (match_increment_by_one(value, iv) != nullptr) {
        return true;
    }

    auto *phi = dynamic_cast<oir::PhiInst *>(value);
    if (phi == nullptr || phi->incoming().empty()) {
        return false;
    }
    if (!active.insert(value).second) {
        return false;
    }
    for (const auto &[incoming_value, pred] : phi->incoming()) {
        (void)pred;
        if (!is_loop_local_unit_increment(incoming_value, iv, active)) {
            active.erase(value);
            return false;
        }
    }
    active.erase(value);
    return true;
}

bool is_loop_local_unit_increment(oir::Value *value, oir::PhiInst *iv) {
    std::unordered_set<oir::Value *> active;
    return is_loop_local_unit_increment(value, iv, active);
}

bool block_has_only_guard_progress(const oir::BasicBlock &block) {
    for (const auto &inst : block.instructions()) {
        if (dynamic_cast<const oir::PhiInst *>(inst.get()) != nullptr || inst->is_terminator()) {
            continue;
        }
        if (!is_allowed_zero_loop_instruction(*inst)) {
            return false;
        }
    }
    return true;
}

bool match_guarded_upper_limit(const oir::CmpInst &cmp, oir::PhiInst *iv, oir::Value *&limit,
                               bool &active_on_true) {
    if (cmp.pred() == oir::CmpPred::LT && cmp.rhs() == iv) {
        limit = cmp.lhs();
        active_on_true = false;
        return true;
    }
    if (cmp.pred() == oir::CmpPred::GT && cmp.lhs() == iv) {
        limit = cmp.rhs();
        active_on_true = false;
        return true;
    }
    if (cmp.pred() == oir::CmpPred::LE && cmp.lhs() == iv) {
        limit = cmp.rhs();
        active_on_true = true;
        return true;
    }
    if (cmp.pred() == oir::CmpPred::GE && cmp.rhs() == iv) {
        limit = cmp.lhs();
        active_on_true = true;
        return true;
    }
    return false;
}

bool guard_path_flows_to_latch(oir::BasicBlock *block, oir::BasicBlock *latch) {
    return block == latch || unconditional_branch_target(block) == latch;
}

bool skipped_guard_path_is_empty(oir::BasicBlock *block, oir::BasicBlock *latch) {
    return block == latch ||
           (unconditional_branch_target(block) == latch && block_has_only_guard_progress(*block));
}

std::optional<GuardedLoopBoundMatch> match_monotonic_guarded_loop_bound(
    const oir::Loop &loop) {
    if (loop.header == nullptr || loop.blocks.size() > 8 || !loop_defs_have_no_external_uses(loop)) {
        return std::nullopt;
    }

    auto *header = mutable_block(loop.header);
    auto outside = outside_predecessors(loop);
    auto *latch = single_latch(loop);
    if (outside.size() != 1 || latch == nullptr || unconditional_branch_target(latch) != header ||
        !block_has_only_guard_progress(*latch)) {
        return std::nullopt;
    }
    auto *preheader = outside.front();

    auto *header_branch = dynamic_cast<oir::BranchInst *>(header->terminator());
    if (header_branch == nullptr || !header_branch->is_conditional()) {
        return std::nullopt;
    }
    auto *header_cmp = dynamic_cast<oir::CmpInst *>(header_branch->cond());
    if (header_cmp == nullptr || header_cmp->op() != oir::Instruction::OpID::ICmp ||
        header_cmp->pred() != oir::CmpPred::LT) {
        return std::nullopt;
    }

    auto *iv = dynamic_cast<oir::PhiInst *>(header_cmp->lhs());
    auto *upper = header_cmp->rhs();
    if (iv == nullptr || iv->parent() != header || !is_i32_value(iv) || !is_i32_value(upper) ||
        value_defined_in_loop(upper, loop)) {
        return std::nullopt;
    }

    if (!contains_block(loop, header_branch->true_bb()) ||
        contains_block(loop, header_branch->false_bb())) {
        return std::nullopt;
    }
    auto *body = header_branch->true_bb();
    auto *exit = header_branch->false_bb();
    if (body == latch || contains_block(loop, exit) || body->predecessors().size() != 1 ||
        body->predecessors().front() != header) {
        return std::nullopt;
    }

    auto *start = incoming_value_from(*iv, preheader);
    auto *back = incoming_value_from(*iv, latch);
    if (start == nullptr || back == nullptr || value_defined_in_loop(start, loop) ||
        !is_loop_local_unit_increment(back, iv)) {
        return std::nullopt;
    }

    auto *body_branch = dynamic_cast<oir::BranchInst *>(body->terminator());
    if (body_branch == nullptr || !body_branch->is_conditional() ||
        !contains_block(loop, body_branch->true_bb()) ||
        !contains_block(loop, body_branch->false_bb())) {
        return std::nullopt;
    }
    auto *guard_cmp = dynamic_cast<oir::CmpInst *>(body_branch->cond());
    if (guard_cmp == nullptr || guard_cmp->op() != oir::Instruction::OpID::ICmp) {
        return std::nullopt;
    }

    oir::Value *limit = nullptr;
    bool active_on_true = false;
    if (!match_guarded_upper_limit(*guard_cmp, iv, limit, active_on_true)) {
        return std::nullopt;
    }
    if (!is_i32_value(limit) || value_defined_in_loop(limit, loop)) {
        return std::nullopt;
    }

    auto *active = active_on_true ? body_branch->true_bb() : body_branch->false_bb();
    auto *skipped = active_on_true ? body_branch->false_bb() : body_branch->true_bb();
    if (skipped == active || active == latch || !guard_path_flows_to_latch(active, latch) ||
        !skipped_guard_path_is_empty(skipped, latch)) {
        return std::nullopt;
    }

    GuardedLoopBoundMatch match;
    match.header = header;
    match.preheader = preheader;
    match.body = body;
    match.exit = exit;
    match.skipped = skipped;
    match.active = active;
    match.latch = latch;
    match.iv = iv;
    match.header_cmp = header_cmp;
    match.upper = upper;
    match.limit = limit;
    return match;
}

void force_guard_to_active_path(const GuardedLoopBoundMatch &match) {
    oir::cfg::remove_edge(match.body, match.skipped);
    replace_terminator_with_br(match.body, match.active);
}

bool rewrite_monotonic_guarded_loop_bound(oir::Function &function,
                                          const GuardedLoopBoundMatch &match, Stats &stats) {
    auto *preheader_branch = dynamic_cast<oir::BranchInst *>(match.preheader->terminator());
    if (preheader_branch == nullptr) {
        return false;
    }

    auto *module = function.parent();
    auto *test_block = function.create_block("loop.bound.test");
    auto *plus_block = function.create_block("loop.bound.plus");
    auto *merge_block = function.create_block("loop.bound.merge");

    oir::IRBuilder builder(module);
    builder.set_insert_point(test_block);
    auto *use_tight_bound =
        builder.create_icmp(oir::CmpPred::LT, match.limit, match.upper, "loop.bound.tight");
    builder.create_cond_br(use_tight_bound, plus_block, merge_block);

    builder.set_insert_point(plus_block);
    auto *limit_next =
        builder.create_binary(oir::Instruction::OpID::Add, match.limit, module->create_i32(1),
                              "loop.bound.next");
    builder.create_br(merge_block);

    builder.set_insert_point(merge_block);
    auto *effective_upper = builder.create_phi(match.upper->type(), "loop.bound");
    effective_upper->add_incoming(limit_next, plus_block);
    effective_upper->add_incoming(match.upper, test_block);
    builder.create_br(match.header);

    if (!oir::cfg::replace_branch_target(*preheader_branch, match.header, test_block)) {
        oir::cfg::remove_edge_no_phi_update(test_block, plus_block);
        oir::cfg::remove_edge_no_phi_update(test_block, merge_block);
        oir::cfg::remove_edge_no_phi_update(plus_block, merge_block);
        oir::cfg::remove_edge_no_phi_update(merge_block, match.header);
        function.erase_block(test_block);
        function.erase_block(plus_block);
        function.erase_block(merge_block);
        return false;
    }

    oir::cfg::remove_edge_no_phi_update(match.preheader, match.header);
    oir::cfg::add_edge(match.preheader, test_block);
    oir::cfg::replace_phi_incoming_block(match.header, match.preheader, merge_block);
    match.header_cmp->set_operand(1, effective_upper);
    force_guard_to_active_path(match);

    ++stats.loop_bound_tighten;
    ++stats.cfg;
    return true;
}

std::optional<NormalizedCmp> normalized_continue_cmp(const oir::BranchInst &branch,
                                                     bool continues_on_true) {
    auto *cmp = dynamic_cast<oir::CmpInst *>(branch.cond());
    if (cmp == nullptr || cmp->op() != oir::Instruction::OpID::ICmp) {
        return std::nullopt;
    }
    NormalizedCmp out;
    out.pred = continues_on_true ? cmp->pred() : negate_predicate(cmp->pred());
    out.lhs = cmp->lhs();
    out.rhs = cmp->rhs();
    return out;
}

bool match_continue_bound(const NormalizedCmp &cmp, oir::Value *next, oir::Value *&bound,
                          oir::CmpPred &pred) {
    if (cmp.lhs == next) {
        pred = cmp.pred;
        bound = cmp.rhs;
    } else if (cmp.rhs == next) {
        pred = swap_predicate_operands(cmp.pred);
        bound = cmp.lhs;
    } else {
        return false;
    }
    return pred == oir::CmpPred::LT || pred == oir::CmpPred::LE;
}

bool match_induction(const oir::Loop &loop, oir::BasicBlock *preheader,
                     const NormalizedCmp &continue_cmp, ZeroStoreLoopMatch &match) {
    for (auto &inst : mutable_block(loop.header)->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        auto *start_value = incoming_value_from(*phi, preheader);
        auto *next_value = incoming_value_from(*phi, mutable_block(loop.header));
        auto start = int_constant(start_value);
        if (start_value == nullptr || next_value == nullptr || !start.has_value()) {
            continue;
        }
        auto *next = match_increment_by_one(next_value, phi);
        if (next == nullptr) {
            continue;
        }

        oir::Value *bound = nullptr;
        oir::CmpPred pred = oir::CmpPred::LT;
        if (!match_continue_bound(continue_cmp, next, bound, pred) ||
            value_defined_in_loop(bound, loop)) {
            continue;
        }

        match.start = *start;
        match.bound = bound;
        match.pred = pred;
        return true;
    }
    return false;
}

bool match_pointer_progression(const oir::Loop &loop, oir::BasicBlock *preheader,
                               oir::StoreInst &store, ZeroStoreLoopMatch &match) {
    auto *ptr_phi = dynamic_cast<oir::PhiInst *>(store.ptr());
    if (ptr_phi == nullptr || ptr_phi->parent() != loop.header) {
        return false;
    }
    auto *start_ptr = incoming_value_from(*ptr_phi, preheader);
    auto *next_ptr = incoming_value_from(*ptr_phi, mutable_block(loop.header));
    if (start_ptr == nullptr || next_ptr == nullptr || value_defined_in_loop(start_ptr, loop) ||
        !match_unit_gep_from_phi(next_ptr, ptr_phi)) {
        return false;
    }
    match.start_ptr = start_ptr;
    return true;
}

std::optional<std::int64_t> constant_iteration_count(const ZeroStoreLoopMatch &match) {
    auto bound = int_constant(match.bound);
    if (!bound.has_value()) {
        return std::nullopt;
    }
    std::int64_t count = *bound - match.start;
    if (match.pred == oir::CmpPred::LE) {
        ++count;
    }
    return count;
}

bool is_int_constant_value(oir::Value *value, std::int64_t expected) {
    auto constant = int_constant(value);
    return constant.has_value() && *constant == expected;
}

bool match_entry_guard_bound(const NormalizedCmp &cmp, const ZeroStoreLoopMatch &match) {
    oir::CmpPred pred = cmp.pred;
    if (is_int_constant_value(cmp.lhs, match.start) && cmp.rhs == match.bound) {
        return pred == match.pred;
    }
    if (cmp.lhs == match.bound && is_int_constant_value(cmp.rhs, match.start)) {
        pred = swap_predicate_operands(pred);
        return pred == match.pred;
    }
    return false;
}

bool dynamic_count_has_entry_guard(const ZeroStoreLoopMatch &match) {
    auto *branch = dynamic_cast<oir::BranchInst *>(match.preheader->terminator());
    if (branch == nullptr || !branch->is_conditional()) {
        return false;
    }
    if (branch->true_bb() != match.header && branch->false_bb() != match.header) {
        return false;
    }
    auto guard = normalized_continue_cmp(*branch, branch->true_bb() == match.header);
    return guard.has_value() && match_entry_guard_bound(*guard, match);
}

oir::Value *materialize_byte_count(oir::Module &module, oir::BasicBlock *block,
                                   const ZeroStoreLoopMatch &match) {
    if (auto count = constant_iteration_count(match)) {
        if (*count <= 0) {
            return nullptr;
        }
        const auto bytes = *count * static_cast<std::int64_t>(match.element_size);
        if (bytes < 0 || bytes > std::numeric_limits<std::int32_t>::max()) {
            return nullptr;
        }
        return module.create_i32(bytes);
    }

    if (!dynamic_count_has_entry_guard(match)) {
        return nullptr;
    }

    oir::IRBuilder builder(&module);
    builder.set_insert_point(block);
    oir::Value *count = match.bound;
    if (match.start != 0) {
        count = builder.create_binary(oir::Instruction::OpID::Sub, count,
                                      module.create_i32(match.start), "memzero.count");
    }
    if (match.pred == oir::CmpPred::LE) {
        count = builder.create_binary(oir::Instruction::OpID::Add, count, module.create_i32(1),
                                      "memzero.count");
    }
    if (match.element_size != 1) {
        count = builder.create_binary(oir::Instruction::OpID::Mul, count,
                                      module.create_i32(static_cast<std::int64_t>(match.element_size)),
                                      "memzero.bytes");
    }
    return count;
}

std::optional<ZeroStoreLoopMatch> match_zero_store_loop(const oir::Loop &loop) {
    if (loop.blocks.size() != 1 || loop.header == nullptr || !loop_defs_have_no_external_uses(loop)) {
        return std::nullopt;
    }

    auto *header = mutable_block(loop.header);
    auto *branch = dynamic_cast<oir::BranchInst *>(header->terminator());
    if (branch == nullptr || !branch->is_conditional()) {
        return std::nullopt;
    }

    const bool true_continues = branch->true_bb() == header;
    const bool false_continues = branch->false_bb() == header;
    if (true_continues == false_continues) {
        return std::nullopt;
    }
    auto *exit = true_continues ? branch->false_bb() : branch->true_bb();
    if (exit == nullptr || contains_block(loop, exit)) {
        return std::nullopt;
    }

    auto outside = outside_predecessors(loop);
    if (outside.size() != 1) {
        return std::nullopt;
    }
    auto *preheader = outside.front();

    std::uint64_t element_size = 0;
    std::uint8_t byte_value = 0;
    auto *store = single_byte_pattern_store(*header, element_size, byte_value);
    if (store == nullptr) {
        return std::nullopt;
    }

    auto continue_cmp = normalized_continue_cmp(*branch, true_continues);
    if (!continue_cmp.has_value()) {
        return std::nullopt;
    }

    ZeroStoreLoopMatch match;
    match.header = header;
    match.preheader = preheader;
    match.exit = exit;
    match.element_size = element_size;
    match.byte_value = byte_value;
    if (!match_induction(loop, preheader, *continue_cmp, match) ||
        !match_pointer_progression(loop, preheader, *store, match)) {
        return std::nullopt;
    }
    return match;
}

bool rewrite_zero_store_loop(oir::Function &function, const ZeroStoreLoopMatch &match,
                             Stats &stats) {
    auto *memzero_block = function.create_block("memzero");
    auto *byte_count = materialize_byte_count(*function.parent(), memzero_block, match);
    if (byte_count == nullptr) {
        function.erase_block(memzero_block);
        return false;
    }

    oir::IRBuilder builder(function.parent());
    builder.set_insert_point(memzero_block);
    if (match.byte_value == 0) {
        builder.create_memzero(match.start_ptr, byte_count);
    } else {
        builder.create_memset(match.start_ptr,
                              function.parent()->create_i32(static_cast<std::int64_t>(match.byte_value)),
                              byte_count);
    }
    builder.create_br(match.exit);

    if (!oir::cfg::replace_successor(match.preheader, match.header, memzero_block)) {
        oir::cfg::remove_edge_no_phi_update(memzero_block, match.exit);
        function.erase_block(memzero_block);
        return false;
    }

    oir::cfg::replace_phi_incoming_block(match.exit, match.header, memzero_block);
    oir::cfg::remove_edge_no_phi_update(match.header, match.header);
    oir::cfg::remove_edge_no_phi_update(match.header, match.exit);
    function.erase_block(match.header);

    ++stats.memzero;
    ++stats.cfg;
    return true;
}

bool lower_zero_store_loops_in_function(oir::Function &function, Stats &stats) {
    bool changed = false;
    constexpr unsigned kMaxMemZeroLoopsPerFunction = 64;
    for (unsigned iteration = 0; iteration < kMaxMemZeroLoopsPerFunction; ++iteration) {
        oir::DominatorTree dom_tree(function);
        oir::LoopInfo loop_info(function, dom_tree);
        auto loops = loop_info.loops();
        std::sort(loops.begin(), loops.end(), [](const oir::Loop &lhs, const oir::Loop &rhs) {
            return lhs.blocks.size() < rhs.blocks.size();
        });

        bool rewritten = false;
        for (const auto &loop : loops) {
            auto match = match_zero_store_loop(loop);
            if (!match.has_value()) {
                continue;
            }
            if (rewrite_zero_store_loop(function, *match, stats)) {
                changed = true;
                rewritten = true;
                break;
            }
        }
        if (!rewritten) {
            break;
        }
    }
    return changed;
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

bool unroll_small_constant_loops_in_function(oir::Function &function, Stats &stats) {
    bool changed = false;
    constexpr unsigned kMaxUnrollsPerFunction = 16;
    for (unsigned iteration = 0; iteration < kMaxUnrollsPerFunction; ++iteration) {
        oir::DominatorTree dom_tree(function);
        oir::LoopInfo loop_info(function, dom_tree);
        oir::ScalarEvolution scev(function, loop_info);
        auto loops = loop_info.loops();
        std::sort(loops.begin(), loops.end(), [](const oir::Loop &lhs, const oir::Loop &rhs) {
            return lhs.blocks.size() < rhs.blocks.size();
        });

        bool unrolled = false;
        for (const auto &loop : loops) {
            if (unroll_single_block_loop(function, loop, scev, stats)) {
                changed = true;
                unrolled = true;
                break;
            }
            if (unroll_multi_block_loop(function, loop, scev, stats)) {
                changed = true;
                unrolled = true;
                break;
            }
            if (peel_first_iteration_loop(function, loop, scev, stats)) {
                changed = true;
                unrolled = true;
                break;
            }
            if (peel_final_iteration_loop(function, loop, scev, stats)) {
                changed = true;
                unrolled = true;
                break;
            }
        }
        if (!unrolled) {
            break;
        }
    }
    return changed;
}

bool tighten_guarded_loop_bounds_in_function(oir::Function &function, Stats &stats) {
    bool changed = false;
    constexpr unsigned kMaxTightensPerFunction = 32;
    for (unsigned iteration = 0; iteration < kMaxTightensPerFunction; ++iteration) {
        oir::DominatorTree dom_tree(function);
        oir::LoopInfo loop_info(function, dom_tree);
        auto loops = loop_info.loops();
        std::sort(loops.begin(), loops.end(), [](const oir::Loop &lhs, const oir::Loop &rhs) {
            return lhs.blocks.size() < rhs.blocks.size();
        });

        bool tightened = false;
        for (const auto &loop : loops) {
            auto match = match_monotonic_guarded_loop_bound(loop);
            if (!match.has_value()) {
                continue;
            }
            if (rewrite_monotonic_guarded_loop_bound(function, *match, stats)) {
                changed = true;
                tightened = true;
                break;
            }
        }
        if (!tightened) {
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

bool unroll_small_constant_loops(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        if (function->is_external() || function->entry_block() == nullptr) {
            continue;
        }
        changed |= unroll_small_constant_loops_in_function(*function, stats);
    }
    return changed;
}

bool tighten_monotonic_guarded_loop_bounds(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        if (function->is_external() || function->entry_block() == nullptr) {
            continue;
        }
        changed |= tighten_guarded_loop_bounds_in_function(*function, stats);
    }
    return changed;
}

bool lower_counted_zero_store_loops_to_memzero(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        if (function->is_external() || function->entry_block() == nullptr) {
            continue;
        }
        changed |= lower_zero_store_loops_in_function(*function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt
