#include "pass/oir/OIRInlinePass.h"

#include "oir/OIRScalarOpt.h"

#include "oir/OIRAnalysis.h"
#include "oir/OIRCFGUtils.h"

#include <algorithm>
#include <list>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

constexpr unsigned kMaxInlineRounds = 3;
constexpr unsigned kMaxInlineSites = 128;
constexpr unsigned kMaxCalleeBlocks = 12;
constexpr unsigned kMaxCalleeCost = 45;
constexpr unsigned kMaxCalleeReturns = 4;
constexpr unsigned kMaxCalleeParams = 16;
constexpr unsigned kMaxSpecializedInlineBlocks = 96;
constexpr unsigned kMaxSpecializedInlineCost = 260;
constexpr unsigned kMaxSpecializedInlineReturns = 8;
constexpr unsigned kMaxSpecializationCalleeBlocks = 64;
constexpr unsigned kMaxSpecializationCalleeCost = 180;
constexpr unsigned kMaxSpecializedFunctions = 48;
constexpr unsigned kMaxSpecializedCallSites = 128;
constexpr unsigned kMaxRecursiveInlineDepth = 5;
constexpr unsigned kMaxRecursiveCalleeBlocks = 16;
constexpr unsigned kMaxRecursiveCalleeCost = 90;
constexpr unsigned kMaxRecursiveCalleeReturns = 8;

using ValueMap = std::unordered_map<oir::Value *, oir::Value *>;
using BlockMap = std::unordered_map<oir::BasicBlock *, oir::BasicBlock *>;

struct CalleeInfo {
    unsigned blocks = 0;
    unsigned cost = 0;
    unsigned returns = 0;
};

struct InlineContext {
    std::unordered_map<oir::Function *, std::unique_ptr<oir::Function>> recursive_templates;
};

bool is_constprop_specialization(const oir::Function &function);

std::string inline_name(const oir::Function &callee, const oir::Value &value,
                        unsigned inline_index) {
    std::string base = value.name().empty() ? "tmp" : value.name();
    return "inl." + callee.name() + "." + std::to_string(inline_index) + "." + base;
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

std::vector<oir::Function *> direct_callees(const oir::Function &function) {
    std::vector<oir::Function *> out;
    for (const auto &block : function.blocks()) {
        for (const auto &inst : block->instructions()) {
            auto *call = dynamic_cast<const oir::CallInst *>(inst.get());
            if (call == nullptr) {
                continue;
            }
            auto *callee = dynamic_cast<oir::Function *>(call->callee());
            if (callee != nullptr && !callee->is_external()) {
                out.push_back(callee);
            }
        }
    }
    return out;
}

bool reaches_function(const oir::Function &function, const oir::Function &target,
                      std::unordered_set<const oir::Function *> &seen) {
    if (!seen.insert(&function).second) {
        return false;
    }
    for (auto *callee : direct_callees(function)) {
        if (callee == &target || reaches_function(*callee, target, seen)) {
            return true;
        }
    }
    return false;
}

bool is_recursive_in_call_graph(const oir::Function &function) {
    std::unordered_set<const oir::Function *> seen;
    return reaches_function(function, function, seen);
}

bool reaches_recursive_function(const oir::Function &function,
                                std::unordered_set<const oir::Function *> &seen) {
    if (!seen.insert(&function).second) {
        return false;
    }
    for (auto *callee : direct_callees(function)) {
        if (is_recursive_in_call_graph(*callee) || reaches_recursive_function(*callee, seen)) {
            return true;
        }
    }
    return false;
}

bool has_recursive_call_graph_dependency(const oir::Function &function) {
    if (is_recursive_in_call_graph(function)) {
        return true;
    }
    std::unordered_set<const oir::Function *> seen;
    return reaches_recursive_function(function, seen);
}

std::string recursive_inline_marker(const oir::Function &function) {
    return "rinl." + function.name() + ".";
}

std::string recursive_inline_prefix(const oir::Function &function, unsigned depth) {
    std::string out;
    const std::string marker = recursive_inline_marker(function);
    for (unsigned i = 0; i < depth; ++i) {
        out += marker;
    }
    return out;
}

unsigned recursive_inline_depth(const oir::Function &function, const oir::BasicBlock &block) {
    const std::string marker = recursive_inline_marker(function);
    unsigned depth = 0;
    std::size_t pos = block.name().find(marker);
    while (pos != std::string::npos) {
        ++depth;
        pos = block.name().find(marker, pos + marker.size());
    }
    return depth;
}

void append_reachable_blocks(oir::BasicBlock *block, std::unordered_set<oir::BasicBlock *> &seen,
                             std::vector<oir::BasicBlock *> &out) {
    if (block == nullptr || !seen.insert(block).second) {
        return;
    }
    out.push_back(block);
    for (auto *succ : block->successors()) {
        append_reachable_blocks(succ, seen, out);
    }
}

std::vector<oir::BasicBlock *> clone_order(const oir::Function &function) {
    std::vector<oir::BasicBlock *> out;
    std::unordered_set<oir::BasicBlock *> seen;
    append_reachable_blocks(function.entry_block(), seen, out);
    for (const auto &block : function.blocks()) {
        if (seen.insert(block.get()).second) {
            out.push_back(block.get());
        }
    }
    return out;
}

bool has_compatible_call_shape(const oir::CallInst &call, const oir::Function &callee) {
    return call.type() == callee.return_type() && call.args().size() == callee.args().size() &&
           callee.args().size() <= kMaxCalleeParams;
}

bool has_scalar_recursive_signature(const oir::Function &function) {
    if (!function.return_type()->is_void() && !is_scalar_type(function.return_type())) {
        return false;
    }
    return std::all_of(function.args().begin(), function.args().end(), [](const auto &arg) {
        return is_scalar_type(arg->type());
    });
}

bool has_supported_recursive_body(const oir::Function &body, const oir::Function &recursive_target) {
    for (const auto &block : body.blocks()) {
        for (const auto &inst : block->instructions()) {
            if (inst->op() == oir::Instruction::OpID::SDiv ||
                inst->op() == oir::Instruction::OpID::SRem ||
                inst->op() == oir::Instruction::OpID::FDiv) {
                return false;
            }
            if (auto *call = dynamic_cast<const oir::CallInst *>(inst.get())) {
                if (call->callee() != &recursive_target) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool is_eligible_non_recursive_call(const oir::Function &caller, const oir::CallInst &call,
                                    oir::Function *callee) {
    if (callee == nullptr || callee == &caller || callee->is_external() ||
        callee->entry_block() == nullptr || callee->name() == "main" ||
        !has_compatible_call_shape(call, *callee)) {
        return false;
    }
    if (contains_call_to(*callee, *callee) || contains_call_to(*callee, caller)) {
        return false;
    }

    auto info = inspect_callee(*callee);
    if (is_constprop_specialization(*callee)) {
        return info.blocks <= kMaxSpecializedInlineBlocks &&
               info.cost <= kMaxSpecializedInlineCost && info.returns != 0 &&
               info.returns <= kMaxSpecializedInlineReturns;
    }
    return info.blocks <= kMaxCalleeBlocks && info.cost <= kMaxCalleeCost && info.returns != 0 &&
           info.returns <= kMaxCalleeReturns;
}

bool is_eligible_recursive_call(const oir::Function &caller, const oir::BasicBlock &block,
                                const oir::CallInst &call, const oir::Function &callee_template) {
    if (call.callee() != &caller || caller.is_external() || caller.entry_block() == nullptr ||
        caller.name() == "main" || !has_compatible_call_shape(call, callee_template) ||
        !has_scalar_recursive_signature(caller) ||
        !has_supported_recursive_body(callee_template, caller) ||
        recursive_inline_depth(caller, block) >= kMaxRecursiveInlineDepth) {
        return false;
    }

    auto info = inspect_callee(callee_template);
    return info.blocks <= kMaxRecursiveCalleeBlocks && info.cost <= kMaxRecursiveCalleeCost &&
           info.returns != 0 && info.returns <= kMaxRecursiveCalleeReturns;
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
    case oir::Instruction::OpID::And:
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
    case oir::Instruction::OpID::MemZero: {
        auto &memzero = static_cast<oir::MemZeroInst &>(inst);
        return std::make_unique<oir::MemZeroInst>(
            module.types().void_ty(), map_value(memzero.ptr(), values, blocks),
            map_value(memzero.byte_value(), values, blocks),
            map_value(memzero.byte_count(), values, blocks), parent);
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

std::unique_ptr<oir::Function> clone_function_template(oir::Function &source) {
    auto out = std::make_unique<oir::Function>(source.function_type(), source.name(),
                                               source.parent(), source.is_external());
    ValueMap values;
    BlockMap blocks;

    for (const auto &arg : source.args()) {
        values[arg.get()] = out->add_argument(arg->type(), arg->name());
    }
    for (const auto &block : source.blocks()) {
        blocks[block.get()] = out->create_block(block->name());
    }

    auto &module = *source.parent();
    for (const auto &block : source.blocks()) {
        auto *out_block = blocks.at(block.get());
        for (const auto &inst : block->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }
            auto out_phi = std::make_unique<oir::PhiInst>(phi->type(), out_block, phi->name());
            values[phi] = out_phi.get();
            out_block->append_instruction(std::move(out_phi));
        }
    }

    for (const auto &block : source.blocks()) {
        auto *out_block = blocks.at(block.get());
        for (const auto &inst_ptr : block->instructions()) {
            auto *inst = inst_ptr.get();
            if (inst->op() == oir::Instruction::OpID::Phi) {
                continue;
            }
            if (auto *ret = dynamic_cast<oir::ReturnInst *>(inst)) {
                out_block->append_instruction(std::make_unique<oir::ReturnInst>(
                    module.types().void_ty(),
                    ret->has_value() ? map_value(ret->value(), values, blocks) : nullptr,
                    out_block));
                continue;
            }
            if (auto *br = dynamic_cast<oir::BranchInst *>(inst)) {
                if (br->is_conditional()) {
                    auto *true_bb = static_cast<oir::BasicBlock *>(
                        map_value(br->true_bb(), values, blocks));
                    auto *false_bb = static_cast<oir::BasicBlock *>(
                        map_value(br->false_bb(), values, blocks));
                    out_block->append_instruction(std::make_unique<oir::BranchInst>(
                        module.types().void_ty(), map_value(br->cond(), values, blocks), true_bb,
                        false_bb, out_block));
                    out_block->add_successor(true_bb);
                    out_block->add_successor(false_bb);
                    true_bb->add_predecessor(out_block);
                    false_bb->add_predecessor(out_block);
                } else {
                    auto *target = static_cast<oir::BasicBlock *>(
                        map_value(br->target_bb(), values, blocks));
                    out_block->append_instruction(std::make_unique<oir::BranchInst>(
                        module.types().void_ty(), target, out_block));
                    out_block->add_successor(target);
                    target->add_predecessor(out_block);
                }
                continue;
            }

            auto cloned =
                clone_non_phi_instruction(module, source, *inst, out_block, values, blocks, 0);
            values[inst] = cloned.get();
            out_block->append_instruction(std::move(cloned));
        }
    }

    for (const auto &block : source.blocks()) {
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

    return out;
}

bool is_constprop_specialization(const oir::Function &function) {
    return function.name().rfind("__yo_constprop.", 0) == 0;
}

bool is_specializable_constant(oir::Value *value) {
    return int_constant(value).has_value() || float_constant(value).has_value();
}

std::string constant_key(oir::Value *value) {
    if (auto constant = int_constant(value)) {
        return "i" + std::to_string(*constant);
    }
    if (auto constant = float_constant(value)) {
        std::ostringstream oss;
        oss << "f" << *constant;
        return oss.str();
    }
    return "*";
}

std::string specialization_key(const oir::Function &callee, const oir::CallInst &call) {
    std::ostringstream oss;
    oss << static_cast<const void *>(&callee);
    auto args = call.args();
    for (std::size_t i = 0; i < args.size(); ++i) {
        oss << ";";
        if (is_specializable_constant(args[i])) {
            oss << i << "=" << constant_key(args[i]);
        } else {
            oss << i << "=*";
        }
    }
    return oss.str();
}

std::string next_specialization_name(oir::Module &module, const oir::Function &callee,
                                     unsigned &next_id) {
    while (true) {
        std::string name = "__yo_constprop." + callee.name() + "." + std::to_string(next_id++);
        if (module.get_function(name) == nullptr) {
            return name;
        }
    }
}

bool has_constant_argument(const oir::CallInst &call) {
    for (auto *arg : call.args()) {
        if (is_specializable_constant(arg)) {
            return true;
        }
    }
    return false;
}

bool has_live_pointer_argument_after_specialization(const oir::CallInst &call,
                                                    const oir::Function &callee) {
    auto args = call.args();
    if (args.size() != callee.args().size()) {
        return true;
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (is_specializable_constant(args[i])) {
            continue;
        }
        if (callee.args()[i]->type()->is_pointer()) {
            return true;
        }
    }
    return false;
}

bool specialized_constant_argument_feeds_phi(const oir::CallInst &call,
                                             const oir::Function &callee) {
    auto args = call.args();
    if (args.size() != callee.args().size()) {
        return true;
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!is_specializable_constant(args[i])) {
            continue;
        }
        for (auto *user : callee.args()[i]->users()) {
            if (dynamic_cast<oir::PhiInst *>(user) != nullptr) {
                return true;
            }
        }
    }
    return false;
}

bool is_eligible_for_constant_specialization(const oir::Function &caller, const oir::CallInst &call,
                                             oir::Function *callee) {
    if (callee == nullptr || callee == &caller || callee->is_external() ||
        callee->entry_block() == nullptr || callee->name() == "main" ||
        is_constprop_specialization(*callee) ||
        !has_compatible_call_shape(call, *callee) || !has_constant_argument(call) ||
        specialized_constant_argument_feeds_phi(call, *callee) ||
        has_recursive_call_graph_dependency(*callee)) {
        return false;
    }

    auto info = inspect_callee(*callee);
    if ((info.blocks > kMaxCalleeBlocks || info.cost > kMaxCalleeCost) &&
        has_live_pointer_argument_after_specialization(call, *callee)) {
        return false;
    }
    return info.blocks <= kMaxSpecializationCalleeBlocks &&
           info.cost <= kMaxSpecializationCalleeCost && info.returns != 0;
}

oir::Function *clone_constant_specialization(oir::Module &module, oir::Function &callee,
                                             const oir::CallInst &call, unsigned &next_id) {
    auto args = call.args();
    std::vector<oir::Type *> param_types;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!is_specializable_constant(args[i])) {
            param_types.push_back(callee.args()[i]->type());
        }
    }

    auto *clone = module.create_function(next_specialization_name(module, callee, next_id),
                                         module.types().func_ty(callee.return_type(), param_types),
                                         false);

    ValueMap values;
    BlockMap blocks;
    std::size_t next_arg = 0;
    for (std::size_t i = 0; i < callee.args().size(); ++i) {
        if (is_specializable_constant(args[i])) {
            values[callee.args()[i].get()] = args[i];
            continue;
        }
        auto *arg = clone->args()[next_arg++].get();
        if (!callee.args()[i]->name().empty()) {
            arg->set_name(callee.args()[i]->name());
        }
        values[callee.args()[i].get()] = arg;
    }

    for (const auto &block : callee.blocks()) {
        blocks[block.get()] = clone->create_block("constprop." + block->name());
    }

    for (const auto &block : callee.blocks()) {
        auto *out_block = blocks.at(block.get());
        for (const auto &inst : block->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }
            auto out_phi =
                std::make_unique<oir::PhiInst>(phi->type(), out_block, phi->name());
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
                out_block->append_instruction(std::make_unique<oir::ReturnInst>(
                    module.types().void_ty(),
                    ret->has_value() ? map_value(ret->value(), values, blocks) : nullptr,
                    out_block));
                continue;
            }
            if (auto *br = dynamic_cast<oir::BranchInst *>(inst)) {
                if (br->is_conditional()) {
                    auto *true_bb = static_cast<oir::BasicBlock *>(
                        map_value(br->true_bb(), values, blocks));
                    auto *false_bb = static_cast<oir::BasicBlock *>(
                        map_value(br->false_bb(), values, blocks));
                    out_block->append_instruction(std::make_unique<oir::BranchInst>(
                        module.types().void_ty(), map_value(br->cond(), values, blocks), true_bb,
                        false_bb, out_block));
                    out_block->add_successor(true_bb);
                    out_block->add_successor(false_bb);
                    true_bb->add_predecessor(out_block);
                    false_bb->add_predecessor(out_block);
                } else {
                    auto *target = static_cast<oir::BasicBlock *>(
                        map_value(br->target_bb(), values, blocks));
                    out_block->append_instruction(std::make_unique<oir::BranchInst>(
                        module.types().void_ty(), target, out_block));
                    out_block->add_successor(target);
                    target->add_predecessor(out_block);
                }
                continue;
            }

            auto cloned =
                clone_non_phi_instruction(module, callee, *inst, out_block, values, blocks, 0);
            values[inst] = cloned.get();
            out_block->append_instruction(std::move(cloned));
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

    return clone;
}

void retarget_call_to_specialization(oir::CallInst &call, oir::Function &clone) {
    auto args = call.args();
    call.set_operand(0, &clone);
    for (std::size_t i = args.size(); i > 0; --i) {
        if (is_specializable_constant(args[i - 1])) {
            call.remove_arg(i - 1);
        }
    }
}

oir::Function &recursive_template_for(InlineContext &context, oir::Function &function) {
    auto found = context.recursive_templates.find(&function);
    if (found != context.recursive_templates.end()) {
        return *found->second;
    }
    auto inserted = context.recursive_templates.emplace(&function, clone_function_template(function));
    return *inserted.first->second;
}

void split_call_block(oir::BasicBlock *block,
                      std::list<std::unique_ptr<oir::Instruction>>::iterator call_it,
                      oir::BasicBlock *continuation) {
    auto original_successors = block->successors();
    for (auto *succ : original_successors) {
        oir::cfg::move_successor_edge(block, continuation, succ);
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

    auto ordered_blocks = clone_order(callee);
    for (auto *block : ordered_blocks) {
        auto *out_block = blocks.at(block);
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

    for (auto *block : ordered_blocks) {
        auto *out_block = blocks.at(block);
        for (const auto &inst_ptr : block->instructions()) {
            auto *inst = inst_ptr.get();
            if (inst->op() == oir::Instruction::OpID::Phi) {
                continue;
            }
            if (auto *ret = dynamic_cast<oir::ReturnInst *>(inst)) {
                returns.push_back(
                    {out_block, ret->has_value() ? map_value(ret->value(), values, blocks)
                                                  : nullptr});
                oir::cfg::append_unconditional_branch(module, out_block, continuation);
                continue;
            }
            if (auto *br = dynamic_cast<oir::BranchInst *>(inst)) {
                if (br->is_conditional()) {
                    oir::cfg::append_conditional_branch(
                        module, out_block, map_value(br->cond(), values, blocks),
                        static_cast<oir::BasicBlock *>(map_value(br->true_bb(), values, blocks)),
                        static_cast<oir::BasicBlock *>(map_value(br->false_bb(), values, blocks)));
                } else {
                    oir::cfg::append_unconditional_branch(
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

    for (auto *block : ordered_blocks) {
        auto *out_block = blocks.at(block);
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

bool inline_call(oir::Module &module, InlineContext &context, oir::Function &caller,
                 oir::BasicBlock *block,
                 std::list<std::unique_ptr<oir::Instruction>>::iterator call_it,
                 unsigned inline_index) {
    auto *call = static_cast<oir::CallInst *>(call_it->get());
    auto *callee = dynamic_cast<oir::Function *>(call->callee());
    bool self_recursive = callee == &caller;
    oir::Function *clone_source = callee;
    if (self_recursive) {
        clone_source = &recursive_template_for(context, caller);
        if (!is_eligible_recursive_call(caller, *block, *call, *clone_source)) {
            return false;
        }
    } else if (!is_eligible_non_recursive_call(caller, *call, callee)) {
        return false;
    }

    ValueMap values;
    BlockMap blocks;
    std::vector<std::pair<oir::BasicBlock *, oir::Value *>> returns;

    const std::string block_prefix =
        self_recursive
            ? recursive_inline_prefix(caller, recursive_inline_depth(caller, *block) + 1)
            : "inl." + callee->name() + ".";
    for (const auto &callee_block : clone_source->blocks()) {
        blocks[callee_block.get()] = caller.create_block(block_prefix + callee_block->name());
    }
    auto *continuation =
        caller.create_block(block_prefix + "cont." + std::to_string(inline_index));

    split_call_block(block, call_it, continuation);
    clone_callee_into_caller(module, caller, *clone_source, *call, continuation, values, blocks,
                             returns, inline_index);
    oir::cfg::append_unconditional_branch(module, block, blocks.at(clone_source->entry_block()));

    if (!call->type()->is_void()) {
        ReplacementMap replacements;
        replacements[call] = materialize_return_value(module, *call, continuation, returns);
        apply_replacements(module, replacements);
    }
    (*call_it)->drop_all_operands();
    block->instructions().erase(call_it);
    return true;
}

bool inline_one_call(oir::Module &module, InlineContext &context, oir::Function &function,
                     unsigned inline_index) {
    for (auto &block : function.blocks()) {
        for (auto it = block->instructions().begin(); it != block->instructions().end(); ++it) {
            if ((*it)->op() != oir::Instruction::OpID::Call) {
                continue;
            }
            if (inline_call(module, context, function, block.get(), it, inline_index)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool specialize_constant_argument_calls(oir::Module &module, Stats &stats) {
    struct Site {
        oir::Function *caller = nullptr;
        oir::Function *callee = nullptr;
        oir::CallInst *call = nullptr;
        std::string key;
    };

    std::vector<Site> sites;
    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        for (auto &block : function->blocks()) {
            for (auto &inst : block->instructions()) {
                auto *call = dynamic_cast<oir::CallInst *>(inst.get());
                if (call == nullptr) {
                    continue;
                }
                auto *callee = dynamic_cast<oir::Function *>(call->callee());
                if (!is_eligible_for_constant_specialization(*function, *call, callee)) {
                    continue;
                }
                sites.push_back({function.get(), callee, call, specialization_key(*callee, *call)});
                if (sites.size() >= kMaxSpecializedCallSites) {
                    break;
                }
            }
            if (sites.size() >= kMaxSpecializedCallSites) {
                break;
            }
        }
        if (sites.size() >= kMaxSpecializedCallSites) {
            break;
        }
    }

    if (sites.empty()) {
        return false;
    }

    bool changed = false;
    unsigned next_id = 0;
    std::unordered_map<std::string, oir::Function *> clones;
    for (auto &site : sites) {
        if (site.call == nullptr || site.callee == nullptr ||
            dynamic_cast<oir::Function *>(site.call->callee()) != site.callee) {
            continue;
        }

        auto found = clones.find(site.key);
        oir::Function *clone = nullptr;
        if (found != clones.end()) {
            clone = found->second;
        } else {
            if (clones.size() >= kMaxSpecializedFunctions) {
                break;
            }
            clone = clone_constant_specialization(module, *site.callee, *site.call, next_id);
            clones.emplace(site.key, clone);
        }
        retarget_call_to_specialization(*site.call, *clone);
        ++stats.specialized;
        changed = true;
        (void)site.caller;
    }

    return changed;
}

bool inline_functions(oir::Module &module, Stats &stats) {
    bool changed = false;
    unsigned inline_index = 0;
    InlineContext context;

    for (unsigned round = 0; round < kMaxInlineRounds; ++round) {
        bool round_changed = false;
        for (auto &function : module.functions()) {
            if (function->is_external()) {
                continue;
            }

            while (inline_index < kMaxInlineSites &&
                   inline_one_call(module, context, *function, inline_index + 1)) {
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

namespace pass {

std::string_view OIRInlinePass::name() const {
    return "OIRInlinePass";
}

PassKind OIRInlinePass::kind() const {
    return PassKind::Transform;
}

PassResult OIRInlinePass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRInlinePass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::inline_functions(module, stats);
            if (changed) {
                changed |= oir_opt::run_sccp(module, stats);
                changed |= oir_opt::global_value_numbering(module, stats);
            }
            changed |= oir_opt::simplify_branches(module, stats);
            changed |= oir_opt::cleanup_cfg(module, stats);
            changed |= oir_opt::eliminate_dead_code(module, stats);
            return changed;
        });
}

} // namespace pass
