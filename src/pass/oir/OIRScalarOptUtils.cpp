#include "oir/OIRScalarOpt.h"

#include <limits>
#include <sstream>
#include <unordered_set>

namespace pass::oir_opt {

bool Stats::changed() const {
    return folded != 0 || sccp != 0 || branches != 0 || dce != 0 || cfg != 0 || gvn != 0 ||
           mem2reg != 0 || licm != 0 || loop_rotate != 0 || loop_unswitch != 0 || inlined != 0 ||
           globals != 0 || tail_recursion != 0 || lsr != 0 || loop_canonicalize != 0 || sroa != 0 ||
           dse != 0 || dle != 0 || adce != 0 || jump_threading != 0 || dae != 0 || memzero != 0 ||
           loop_bound_tighten != 0 || loop_unroll != 0 || specialized != 0;
}

std::string Stats::message() const {
    std::ostringstream oss;
    oss << "folded=" << folded << " sccp=" << sccp << " branches=" << branches << " dce=" << dce
        << " cfg=" << cfg << " gvn=" << gvn << " mem2reg=" << mem2reg << " licm=" << licm
        << " rotate=" << loop_rotate << " unswitch=" << loop_unswitch << " inlined=" << inlined
        << " globals=" << globals << " tailrec=" << tail_recursion << " lsr=" << lsr
        << " loopcanon=" << loop_canonicalize << " sroa=" << sroa << " dse=" << dse
        << " dle=" << dle << " adce=" << adce << " jumpthread=" << jump_threading << " dae=" << dae
        << " memzero=" << memzero << " looptighten=" << loop_bound_tighten
        << " loopunroll=" << loop_unroll << " specialized=" << specialized;
    return oss.str();
}

bool is_integer_type(oir::Type *type, std::size_t bits = 0) {
    auto *int_ty = dynamic_cast<oir::IntegerType *>(type);
    return int_ty != nullptr && (bits == 0 || int_ty->bit_width() == bits);
}

bool is_scalar_type(oir::Type *type) {
    return type != nullptr && (type->is_integer() || type->is_float() || type->is_pointer());
}

std::optional<std::int64_t> int_constant(oir::Value *value) {
    if (auto *constant = dynamic_cast<oir::ConstantInt *>(value)) {
        return constant->value();
    }
    if (dynamic_cast<oir::ConstantZero *>(value) != nullptr && value->type()->is_integer()) {
        return 0;
    }
    return std::nullopt;
}

std::optional<float> float_constant(oir::Value *value) {
    if (auto *constant = dynamic_cast<oir::ConstantFloat *>(value)) {
        return constant->value();
    }
    if (dynamic_cast<oir::ConstantZero *>(value) != nullptr && value->type()->is_float()) {
        return 0.0F;
    }
    return std::nullopt;
}

bool is_int_value(oir::Value *value, std::int64_t expected) {
    auto constant = int_constant(value);
    return constant.has_value() && *constant == expected;
}

oir::ConstantInt *make_int_constant(oir::Module &module, oir::Type *type, std::int64_t value) {
    auto *int_ty = dynamic_cast<oir::IntegerType *>(type);
    if (int_ty != nullptr && int_ty->bit_width() == 1) {
        return module.create_i1(value != 0);
    }
    return module.create_i32(static_cast<std::int32_t>(value));
}

oir::Value *make_zero_constant(oir::Module &module, oir::Type *type) {
    if (type->is_integer()) {
        return make_int_constant(module, type, 0);
    }
    if (type->is_float()) {
        return module.create_f32(0.0F);
    }
    return module.create_zero(type);
}

bool same_constant_value(oir::Value *lhs, oir::Value *rhs) {
    if (lhs == rhs) {
        return true;
    }
    if (lhs == nullptr || rhs == nullptr || lhs->type() != rhs->type()) {
        return false;
    }
    auto lhs_int = int_constant(lhs);
    auto rhs_int = int_constant(rhs);
    if (lhs_int.has_value() && rhs_int.has_value()) {
        return *lhs_int == *rhs_int;
    }
    auto lhs_float = float_constant(lhs);
    auto rhs_float = float_constant(rhs);
    if (lhs_float.has_value() && rhs_float.has_value()) {
        return *lhs_float == *rhs_float;
    }
    return dynamic_cast<oir::ConstantZero *>(lhs) != nullptr &&
           dynamic_cast<oir::ConstantZero *>(rhs) != nullptr;
}

bool eval_cmp(oir::CmpPred pred, std::int64_t lhs, std::int64_t rhs) {
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
    return false;
}

bool eval_fcmp(oir::CmpPred pred, float lhs, float rhs) {
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
    return false;
}

std::optional<std::int64_t> fold_int_binary(oir::Instruction::OpID op, std::int64_t lhs,
                                            std::int64_t rhs) {
    const std::int64_t min_i32 = std::numeric_limits<std::int32_t>::min();
    switch (op) {
    case oir::Instruction::OpID::Add:
        return static_cast<std::int32_t>(lhs + rhs);
    case oir::Instruction::OpID::Sub:
        return static_cast<std::int32_t>(lhs - rhs);
    case oir::Instruction::OpID::Mul:
        return static_cast<std::int32_t>(lhs * rhs);
    case oir::Instruction::OpID::And:
        return static_cast<std::int32_t>(lhs) & static_cast<std::int32_t>(rhs);
    case oir::Instruction::OpID::SDiv:
        if (rhs == 0) {
            return std::nullopt;
        }
        if (lhs == min_i32 && rhs == -1) {
            return static_cast<std::int32_t>(lhs);
        }
        return static_cast<std::int32_t>(lhs / rhs);
    case oir::Instruction::OpID::SRem:
        if (rhs == 0) {
            return std::nullopt;
        }
        if (lhs == min_i32 && rhs == -1) {
            return 0;
        }
        return static_cast<std::int32_t>(lhs % rhs);
    default:
        return std::nullopt;
    }
}

std::optional<float> fold_float_binary(oir::Instruction::OpID op, float lhs, float rhs) {
    switch (op) {
    case oir::Instruction::OpID::FAdd:
        return lhs + rhs;
    case oir::Instruction::OpID::FSub:
        return lhs - rhs;
    case oir::Instruction::OpID::FMul:
        return lhs * rhs;
    case oir::Instruction::OpID::FDiv:
        return lhs / rhs;
    default:
        return std::nullopt;
    }
}

bool is_pure_instruction(const oir::Instruction &inst) {
    switch (inst.op()) {
    case oir::Instruction::OpID::Ret:
    case oir::Instruction::OpID::Br:
    case oir::Instruction::OpID::Store:
    case oir::Instruction::OpID::Call:
    case oir::Instruction::OpID::MemZero:
        return false;
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::Mul:
    case oir::Instruction::OpID::And:
    case oir::Instruction::OpID::SDiv:
    case oir::Instruction::OpID::SRem:
    case oir::Instruction::OpID::FAdd:
    case oir::Instruction::OpID::FSub:
    case oir::Instruction::OpID::FMul:
    case oir::Instruction::OpID::FDiv:
    case oir::Instruction::OpID::ICmp:
    case oir::Instruction::OpID::FCmp:
    case oir::Instruction::OpID::Alloca:
    case oir::Instruction::OpID::Load:
    case oir::Instruction::OpID::GetElementPtr:
    case oir::Instruction::OpID::ZExt:
    case oir::Instruction::OpID::SIToFP:
    case oir::Instruction::OpID::FPToSI:
    case oir::Instruction::OpID::Phi:
        return true;
    }
    return false;
}

using ReplacementMap = std::unordered_map<oir::Value *, oir::Value *>;

oir::Value *resolve_replacement(const ReplacementMap &replacements, oir::Value *value) {
    std::unordered_set<oir::Value *> seen;
    auto *current = value;
    while (current != nullptr && seen.insert(current).second) {
        auto found = replacements.find(current);
        if (found == replacements.end()) {
            return current;
        }
        current = found->second;
    }
    return value;
}

unsigned apply_replacements(oir::Module &module, const ReplacementMap &replacements) {
    if (replacements.empty()) {
        return 0;
    }

    unsigned replaced_operands = 0;
    (void)module;
    for (const auto &[old_value, replacement] : replacements) {
        auto *new_value = resolve_replacement(replacements, replacement);
        if (old_value == nullptr || new_value == nullptr || old_value == new_value ||
            old_value->type() != new_value->type()) {
            continue;
        }
        const auto before = old_value->use_count();
        old_value->replace_all_uses_with(new_value);
        replaced_operands += static_cast<unsigned>(before - old_value->use_count());
    }
    return replaced_operands;
}

} // namespace pass::oir_opt
