#include "oir/OIRScalarOpt.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace pass::oir_opt {
namespace {

using DemandMap = std::unordered_map<oir::Value *, std::uint64_t>;

std::uint64_t type_mask(oir::Type *type) {
    auto *integer = dynamic_cast<oir::IntegerType *>(type);
    if (integer == nullptr || integer->bit_width() == 0 || integer->bit_width() > 32) {
        return 0;
    }
    if (integer->bit_width() == 32) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return (std::uint64_t{1} << integer->bit_width()) - 1;
}

bool add_demand(DemandMap &demands, oir::Value *value, std::uint64_t mask) {
    const auto valid = type_mask(value == nullptr ? nullptr : value->type());
    if (valid == 0) {
        return false;
    }
    mask &= valid;
    auto &current = demands[value];
    const auto next = current | mask;
    if (next == current) {
        return false;
    }
    current = next;
    return true;
}

std::uint64_t low_closure(std::uint64_t mask) {
    if (mask == 0) {
        return 0;
    }
    unsigned highest = 0;
    while ((mask >>= 1U) != 0) {
        ++highest;
    }
    return highest >= 31 ? std::numeric_limits<std::uint32_t>::max()
                         : (std::uint64_t{1} << (highest + 1U)) - 1;
}

std::uint64_t possible_bits(oir::Value *value) {
    if (auto constant = int_constant(value)) {
        return static_cast<std::uint32_t>(*constant) & type_mask(value->type());
    }
    return type_mask(value == nullptr ? nullptr : value->type());
}

void seed_observable_operands(oir::Instruction &inst, DemandMap &demands) {
    if (auto *ret = dynamic_cast<oir::ReturnInst *>(&inst)) {
        if (ret->has_value()) {
            add_demand(demands, ret->value(), type_mask(ret->value()->type()));
        }
        return;
    }
    if (auto *branch = dynamic_cast<oir::BranchInst *>(&inst)) {
        if (branch->is_conditional()) {
            add_demand(demands, branch->cond(), type_mask(branch->cond()->type()));
        }
        return;
    }
    if (auto *store = dynamic_cast<oir::StoreInst *>(&inst)) {
        add_demand(demands, store->value(), type_mask(store->value()->type()));
        return;
    }
    if (auto *memset = dynamic_cast<oir::MemZeroInst *>(&inst)) {
        add_demand(demands, memset->byte_value(), type_mask(memset->byte_value()->type()));
        add_demand(demands, memset->byte_count(), type_mask(memset->byte_count()->type()));
        return;
    }
    if (auto *call = dynamic_cast<oir::CallInst *>(&inst)) {
        for (auto *arg : call->args()) {
            add_demand(demands, arg, type_mask(arg->type()));
        }
        return;
    }
    if (auto *gep = dynamic_cast<oir::GetElementPtrInst *>(&inst)) {
        for (auto *index : gep->indices()) {
            add_demand(demands, index, type_mask(index->type()));
        }
        return;
    }
    if (auto *cast = dynamic_cast<oir::CastInst *>(&inst)) {
        if (cast->op() == oir::Instruction::OpID::SIToFP) {
            add_demand(demands, cast->src(), type_mask(cast->src()->type()));
        }
    }
}

bool propagate_instruction_demand(oir::Instruction &inst, DemandMap &demands) {
    const auto found = demands.find(&inst);
    const auto demanded = found == demands.end() ? 0 : found->second;
    if (demanded == 0) {
        return false;
    }

    bool changed = false;
    if (auto *binary = dynamic_cast<oir::BinaryInst *>(&inst)) {
        switch (binary->op()) {
        case oir::Instruction::OpID::And:
            changed |= add_demand(demands, binary->lhs(), demanded & possible_bits(binary->rhs()));
            changed |= add_demand(demands, binary->rhs(), demanded & possible_bits(binary->lhs()));
            return changed;
        case oir::Instruction::OpID::Xor:
            changed |= add_demand(demands, binary->lhs(), demanded);
            changed |= add_demand(demands, binary->rhs(), demanded);
            return changed;
        case oir::Instruction::OpID::Add:
        case oir::Instruction::OpID::Sub:
        case oir::Instruction::OpID::Mul: {
            const auto closure = low_closure(demanded) & type_mask(binary->type());
            changed |= add_demand(demands, binary->lhs(), closure);
            changed |= add_demand(demands, binary->rhs(), closure);
            return changed;
        }
        case oir::Instruction::OpID::SDiv:
        case oir::Instruction::OpID::SRem:
            changed |= add_demand(demands, binary->lhs(), type_mask(binary->lhs()->type()));
            changed |= add_demand(demands, binary->rhs(), type_mask(binary->rhs()->type()));
            return changed;
        default:
            return false;
        }
    }
    if (auto *cmp = dynamic_cast<oir::CmpInst *>(&inst)) {
        changed |= add_demand(demands, cmp->lhs(), type_mask(cmp->lhs()->type()));
        changed |= add_demand(demands, cmp->rhs(), type_mask(cmp->rhs()->type()));
        return changed;
    }
    if (auto *cast = dynamic_cast<oir::CastInst *>(&inst)) {
        if (cast->op() == oir::Instruction::OpID::ZExt) {
            return add_demand(demands, cast->src(), demanded & type_mask(cast->src()->type()));
        }
        return add_demand(demands, cast->src(), type_mask(cast->src()->type()));
    }
    if (auto *phi = dynamic_cast<oir::PhiInst *>(&inst)) {
        for (const auto &[value, block] : phi->incoming()) {
            (void)block;
            changed |= add_demand(demands, value, demanded);
        }
    }
    return changed;
}

bool narrow_constant_operand(oir::Module &module, oir::BinaryInst &binary,
                             const DemandMap &demands) {
    if (binary.op() != oir::Instruction::OpID::And && binary.op() != oir::Instruction::OpID::Xor) {
        return false;
    }
    auto found = demands.find(&binary);
    if (found == demands.end()) {
        return false;
    }

    std::size_t constant_index = 0;
    auto constant = int_constant(binary.lhs());
    if (!constant) {
        constant_index = 1;
        constant = int_constant(binary.rhs());
    }
    if (!constant) {
        return false;
    }

    const auto old_bits = static_cast<std::uint32_t>(*constant) & type_mask(binary.type());
    const auto new_bits = old_bits & found->second;
    if (old_bits == new_bits) {
        return false;
    }
    binary.set_operand(constant_index, make_int_constant(module, binary.type(),
                                                         static_cast<std::int32_t>(new_bits)));
    return true;
}

bool run_on_function(oir::Module &module, oir::Function &function, Stats &stats) {
    if (function.is_external()) {
        return false;
    }
    DemandMap demands;
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            seed_observable_operands(*inst, demands);
        }
    }

    for (unsigned iteration = 0; iteration < 64; ++iteration) {
        bool changed = false;
        for (auto block = function.blocks().rbegin(); block != function.blocks().rend(); ++block) {
            for (auto inst = (*block)->instructions().rbegin();
                 inst != (*block)->instructions().rend(); ++inst) {
                changed |= propagate_instruction_demand(**inst, demands);
            }
        }
        if (!changed) {
            break;
        }
    }

    bool changed = false;
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            auto *binary = dynamic_cast<oir::BinaryInst *>(inst.get());
            if (binary != nullptr && narrow_constant_operand(module, *binary, demands)) {
                ++stats.bdce;
                changed = true;
            }
        }
    }
    return changed;
}

} // namespace

bool eliminate_dead_bits(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        changed |= run_on_function(module, *function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt
