#include "../../include/oir/OIRScalarOpt.h"

#include "../../include/oir/OIRAnalysis.h"

#include <algorithm>
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace pass::oir_opt {
namespace {

constexpr unsigned kMaxInlineRounds = 3;
constexpr unsigned kMaxInlineSites = 128;
constexpr unsigned kMaxCalleeBlocks = 8;
constexpr unsigned kMaxCalleeCost = 45;
constexpr unsigned kMaxCalleeReturns = 4;

using ValueMap = std::unordered_map<oir::Value *, oir::Value *>;
using BlockMap = std::unordered_map<oir::BasicBlock *, oir::BasicBlock *>;

struct CalleeInfo {
    unsigned blocks = 0;
    unsigned cost = 0;
    unsigned returns = 0;
};

std::string inline_name(const oir::Function &callee, const oir::Value &value,
                        unsigned inline_index) {
    std::string base = value.name().empty() ? "tmp" : value.name();
    return "inl." + callee.name() + "." + std::to_string(inline_index) + "." + base;
}

void replace_phi_incoming_block(oir::BasicBlock *block, oir::BasicBlock *old_pred,
                                oir::BasicBlock *new_pred) {
    for (auto &inst : block->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        for (std::size_t i = 0; i < phi->incoming().size(); ++i) {
            if (phi->incoming()[i].second == old_pred) {
                phi->set_operand(i * 2 + 1, new_pred);
            }
        }
    }
}

CalleeInfo inspect_callee(const oir::Function &function) {
    CalleeInfo info;
    for (const auto &block : function.blocks()) {
        ++info.blocks;
        for (const auto &inst : block->instructions()) {
            if (inst->op() == oir::Instruction::OpID::Ret) {
                ++info.returns;
                continue;
            }
            if (inst->op() == oir::Instruction::OpID::Br ||
                inst->op() == oir::Instruction::OpID::Phi) {
                continue;
            }
            ++info.cost;
            if (inst->op() == oir::Instruction::OpID::Call) {
                info.cost += 4;
            }
        }
    }
    return info;
}

bool contains_call_to(const oir::Function &function, const oir::Function &target) {
    for (const auto &block : function.blocks()) {
        for (const auto &inst : block->instructions()) {
            auto *call = dynamic_cast<const oir::CallInst *>(inst.get());
            if (call != nullptr && call->callee() == &target) {
                return true;
            }
        }
    }
    return false;
}

bool name_starts_with(const std::string &value, const std::string &prefix) {
    return value.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), value.begin());
}

bool contains_inlined_code(const oir::Function &function) {
    for (const auto &block : function.blocks()) {
        if (name_starts_with(block->name(), "inl.")) {
            return true;
        }
        for (const auto &inst : block->instructions()) {
            if (name_starts_with(inst->name(), "inl.")) {
                return true;
            }
        }
    }
    return false;
}

bool is_eligible_call(const oir::Function &caller, const oir::CallInst &call,
                      oir::Function *callee) {
    if (callee == nullptr || callee == &caller || callee->is_external() ||
        callee->entry_block() == nullptr || callee->name() == "main") {
        return false;
    }
    if (call.type() != callee->return_type() || call.args().size() != callee->args().size()) {
        return false;
    }
    if (contains_call_to(*callee, *callee) || contains_call_to(*callee, caller)) {
        return false;
    }
    if (contains_inlined_code(*callee)) {
        return false;
    }

    auto info = inspect_callee(*callee);
    return info.blocks <= kMaxCalleeBlocks && info.cost <= kMaxCalleeCost && info.returns != 0 &&
           info.returns <= kMaxCalleeReturns;
}

oir::Value *map_value(oir::Value *value, const ValueMap &values, const BlockMap &blocks) {
    if (value == nullptr) {
        return nullptr;
    }
    if (auto *block = dynamic_cast<oir::BasicBlock *>(value)) {
        auto found = blocks.find(block);
        if (found == blocks.end()) {
            throw std::runtime_error("inline clone cannot map callee block %" + block->name());
        }
        return found->second;
    }
    auto found = values.find(value);
    if (found != values.end()) {
        return found->second;
    }
    return value;
}

std::vector<oir::Value *> map_values(const std::vector<oir::Value *> &input,
                                     const ValueMap &values, const BlockMap &blocks) {
    std::vector<oir::Value *> out;
    out.reserve(input.size());
    for (auto *value : input) {
        out.push_back(map_value(value, values, blocks));
    }
    return out;
}

std::unique_ptr<oir::Instruction> clone_non_phi_instruction(oir::Module &module,
                                                            const oir::Function &callee,
                                                            oir::Instruction &inst,
                                                            oir::BasicBlock *parent,
                                                            const ValueMap &values,
                                                            const BlockMap &blocks,
                                                            unsigned inline_index) {
    const std::string name = inline_name(callee, inst, inline_index);
    switch (inst.op()) {
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::Mul:
    case oir::Instruction::OpID::SDiv:
    case oir::Instruction::OpID::SRem:
    case oir::Instruction::OpID::FAdd:
    case oir::Instruction::OpID::FSub:
    case oir::Instruction::OpID::FMul:
    case oir::Instruction::OpID::FDiv: {
        auto &binary = static_cast<oir::BinaryInst &>(inst);
        return std::make_unique<oir::BinaryInst>(
            inst.type(), inst.op(), map_value(binary.lhs(), values, blocks),
            map_value(binary.rhs(), values, blocks), parent, name);
    }
    case oir::Instruction::OpID::ICmp:
    case oir::Instruction::OpID::FCmp: {
        auto &cmp = static_cast<oir::CmpInst &>(inst);
        return std::make_unique<oir::CmpInst>(
            inst.type(), inst.op(), cmp.pred(), map_value(cmp.lhs(), values, blocks),
            map_value(cmp.rhs(), values, blocks), parent, name);
    }
    case oir::Instruction::OpID::ZExt:
    case oir::Instruction::OpID::SIToFP:
    case oir::Instruction::OpID::FPToSI: {
        auto &cast = static_cast<oir::CastInst &>(inst);
        return std::make_unique<oir::CastInst>(inst.type(), inst.op(),
                                               map_value(cast.src(), values, blocks), parent, name);
    }
    case oir::Instruction::OpID::Alloca: {
        auto &alloca = static_cast<oir::AllocaInst &>(inst);
        return std::make_unique<oir::AllocaInst>(inst.type(), alloca.allocated_type(), parent,
                                                 name);
    }
    case oir::Instruction::OpID::Load: {
        auto &load = static_cast<oir::LoadInst &>(inst);
        return std::make_unique<oir::LoadInst>(inst.type(), map_value(load.ptr(), values, blocks),
                                               parent, name);
    }
    case oir::Instruction::OpID::Store: {
        auto &store = static_cast<oir::StoreInst &>(inst);
        return std::make_unique<oir::StoreInst>(
            module.types().void_ty(), map_value(store.value(), values, blocks),
            map_value(store.ptr(), values, blocks), parent);
    }
    case oir::Instruction::OpID::GetElementPtr: {
        auto &gep = static_cast<oir::GetElementPtrInst &>(inst);
        return std::make_unique<oir::GetElementPtrInst>(
            inst.type(), map_value(gep.base_ptr(), values, blocks),
            map_values(gep.indices(), values, blocks), parent, name);
    }
    case oir::Instruction::OpID::Call: {
        auto &call = static_cast<oir::CallInst &>(inst);
        return std::make_unique<oir::CallInst>(
            inst.type(), map_value(call.callee(), values, blocks),
            map_values(call.args(), values, blocks), parent, name);
    }
    case oir::Instruction::OpID::Phi:
    case oir::Instruction::OpID::Br:
    case oir::Instruction::OpID::Ret:
        break;
    }
    throw std::runtime_error("unsupported instruction while cloning inline body");
}

void append_unconditional_branch(oir::Module &module, oir::BasicBlock *from,
                                 oir::BasicBlock *to) {
    from->add_successor(to);
    to->add_predecessor(from);
    from->append_instruction(
        std::make_unique<oir::BranchInst>(module.types().void_ty(), to, from));
}

void append_conditional_branch(oir::Module &module, oir::BasicBlock *from, oir::Value *cond,
                               oir::BasicBlock *true_bb, oir::BasicBlock *false_bb) {
    from->add_successor(true_bb);
    from->add_successor(false_bb);
    true_bb->add_predecessor(from);
    false_bb->add_predecessor(from);
    from->append_instruction(std::make_unique<oir::BranchInst>(module.types().void_ty(), cond,
                                                               true_bb, false_bb, from));
}

void split_call_block(oir::BasicBlock *block,
                      std::list<std::unique_ptr<oir::Instruction>>::iterator call_it,
                      oir::BasicBlock *continuation) {
    auto original_successors = block->successors();
    for (auto *succ : original_successors) {
        block->remove_successor(succ);
        succ->remove_predecessor(block);
        continuation->add_successor(succ);
        succ->add_predecessor(continuation);
        replace_phi_incoming_block(succ, block, continuation);
    }

    auto &from = block->instructions();
    auto tail_begin = std::next(call_it);
    auto &to = continuation->instructions();
    to.splice(to.end(), from, tail_begin, from.end());
    for (auto &inst : to) {
        inst->set_parent(continuation);
    }
}

oir::Value *materialize_return_value(oir::Module &module, oir::CallInst &call,
                                     oir::BasicBlock *continuation,
                                     const std::vector<std::pair<oir::BasicBlock *, oir::Value *>>
                                         &returns) {
    if (call.type()->is_void()) {
        return nullptr;
    }
    if (returns.empty()) {
        throw std::runtime_error("cannot inline non-void call whose callee has no return value");
    }
    if (returns.size() == 1) {
        return returns.front().second;
    }

    auto phi =
        std::make_unique<oir::PhiInst>(call.type(), continuation,
                                       call.name().empty() ? "inl.ret" : call.name());
    auto *raw = phi.get();
    for (const auto &[block, value] : returns) {
        phi->add_incoming(value, block);
    }
    continuation->instructions().push_front(std::move(phi));
    raw->set_parent(continuation);
    (void)module;
    return raw;
}

void clone_callee_into_caller(oir::Module &module, oir::Function &caller, oir::Function &callee,
                              oir::CallInst &call, oir::BasicBlock *continuation,
                              ValueMap &values, BlockMap &blocks,
                              std::vector<std::pair<oir::BasicBlock *, oir::Value *>> &returns,
                              unsigned inline_index) {
    auto args = call.args();
    for (std::size_t i = 0; i < callee.args().size(); ++i) {
        values[callee.args()[i].get()] = args[i];
    }

    for (const auto &block : callee.blocks()) {
        auto *out_block = blocks.at(block.get());
        for (const auto &inst : block->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }
            auto out_phi = std::make_unique<oir::PhiInst>(
                phi->type(), out_block, inline_name(callee, *phi, inline_index));
            values[phi] = out_phi.get();
            out_block->append_instruction(std::move(out_phi));
        }
    }

    for (const auto &block : callee.blocks()) {
        auto *out_block = blocks.at(block.get());
        for (const auto &inst_ptr : block->instructions()) {
            auto *inst = inst_ptr.get();
            if (inst->op() == oir::Instruction::OpID::Phi) {
                continue;
            }
            if (auto *ret = dynamic_cast<oir::ReturnInst *>(inst)) {
                returns.push_back(
                    {out_block, ret->has_value() ? map_value(ret->value(), values, blocks)
                                                  : nullptr});
                append_unconditional_branch(module, out_block, continuation);
                continue;
            }
            if (auto *br = dynamic_cast<oir::BranchInst *>(inst)) {
                if (br->is_conditional()) {
                    append_conditional_branch(
                        module, out_block, map_value(br->cond(), values, blocks),
                        static_cast<oir::BasicBlock *>(map_value(br->true_bb(), values, blocks)),
                        static_cast<oir::BasicBlock *>(map_value(br->false_bb(), values, blocks)));
                } else {
                    append_unconditional_branch(
                        module, out_block,
                        static_cast<oir::BasicBlock *>(map_value(br->target_bb(), values, blocks)));
                }
                continue;
            }

            auto cloned = clone_non_phi_instruction(module, callee, *inst, out_block, values,
                                                    blocks, inline_index);
            auto *raw = cloned.get();
            out_block->append_instruction(std::move(cloned));
            values[inst] = raw;
        }
    }

    for (const auto &block : callee.blocks()) {
        auto *out_block = blocks.at(block.get());
        auto out_it = out_block->instructions().begin();
        for (const auto &inst : block->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }
            auto *out_phi = static_cast<oir::PhiInst *>(out_it->get());
            for (const auto &[value, from] : phi->incoming()) {
                out_phi->add_incoming(map_value(value, values, blocks), blocks.at(from));
            }
            ++out_it;
        }
    }
}

bool inline_call(oir::Module &module, oir::Function &caller, oir::BasicBlock *block,
                 std::list<std::unique_ptr<oir::Instruction>>::iterator call_it,
                 unsigned inline_index) {
    auto *call = static_cast<oir::CallInst *>(call_it->get());
    auto *callee = dynamic_cast<oir::Function *>(call->callee());
    if (!is_eligible_call(caller, *call, callee)) {
        return false;
    }

    ValueMap values;
    BlockMap blocks;
    std::vector<std::pair<oir::BasicBlock *, oir::Value *>> returns;

    for (const auto &callee_block : callee->blocks()) {
        blocks[callee_block.get()] =
            caller.create_block("inl." + callee->name() + "." + callee_block->name());
    }
    auto *continuation =
        caller.create_block("inl." + callee->name() + ".cont." + std::to_string(inline_index));

    split_call_block(block, call_it, continuation);
    clone_callee_into_caller(module, caller, *callee, *call, continuation, values, blocks, returns,
                             inline_index);
    append_unconditional_branch(module, block, blocks.at(callee->entry_block()));

    if (!call->type()->is_void()) {
        ReplacementMap replacements;
        replacements[call] = materialize_return_value(module, *call, continuation, returns);
        apply_replacements(module, replacements);
    }
    block->instructions().erase(call_it);
    return true;
}

bool inline_one_call(oir::Module &module, oir::Function &function, unsigned inline_index) {
    for (auto &block : function.blocks()) {
        for (auto it = block->instructions().begin(); it != block->instructions().end(); ++it) {
            if ((*it)->op() != oir::Instruction::OpID::Call) {
                continue;
            }
            if (inline_call(module, function, block.get(), it, inline_index)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool inline_functions(oir::Module &module, Stats &stats) {
    bool changed = false;
    unsigned inline_index = 0;

    for (unsigned round = 0; round < kMaxInlineRounds; ++round) {
        bool round_changed = false;
        for (auto &function : module.functions()) {
            if (function->is_external()) {
                continue;
            }

            while (inline_index < kMaxInlineSites &&
                   inline_one_call(module, *function, inline_index + 1)) {
                ++inline_index;
                ++stats.inlined;
                changed = true;
                round_changed = true;
            }
        }
        if (!round_changed || inline_index >= kMaxInlineSites) {
            break;
        }
    }

    return changed;
}

} // namespace pass::oir_opt
