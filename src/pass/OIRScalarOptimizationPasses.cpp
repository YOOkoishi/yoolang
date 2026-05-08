#include "../../include/pass/OIRAlgebraicSimplifyPass.h"
#include "../../include/pass/OIRConstantFoldPass.h"
#include "../../include/pass/OIRSCCPPass.h"

#include "../../include/oir/OIRAnalysis.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pass {
namespace {

struct Stats {
    unsigned folded = 0;
    unsigned sccp = 0;
    unsigned branches = 0;
    unsigned dce = 0;

    bool changed() const {
        return folded != 0 || sccp != 0 || branches != 0 || dce != 0;
    }

    std::string message() const {
        std::ostringstream oss;
        oss << "folded=" << folded << " sccp=" << sccp << " branches=" << branches
            << " dce=" << dce;
        return oss.str();
    }
};

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
        return false;
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::Mul:
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
    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        for (auto &block : function->blocks()) {
            for (auto &inst : block->instructions()) {
                for (std::size_t i = 0; i < inst->operand_count(); ++i) {
                    auto *old_operand = inst->operand(i);
                    auto *new_operand = resolve_replacement(replacements, old_operand);
                    if (new_operand != old_operand && new_operand->type() == old_operand->type()) {
                        inst->set_operand(i, new_operand);
                        ++replaced_operands;
                    }
                }
            }
        }
    }
    return replaced_operands;
}

oir::CmpInst *as_zext_cmp(oir::Value *value) {
    auto *cast = dynamic_cast<oir::CastInst *>(value);
    if (cast == nullptr || cast->op() != oir::Instruction::OpID::ZExt) {
        return nullptr;
    }
    return dynamic_cast<oir::CmpInst *>(cast->src());
}

oir::Value *insert_bool_not(oir::Module &module, oir::BasicBlock &block,
                            std::list<std::unique_ptr<oir::Instruction>>::iterator before,
                            oir::Value *value, const std::string &name) {
    auto new_inst = std::make_unique<oir::CmpInst>(module.types().int1_ty(),
                                                   oir::Instruction::OpID::ICmp, oir::CmpPred::EQ,
                                                   value, module.create_i1(false), &block, name);
    auto *raw = new_inst.get();
    raw->set_parent(&block);
    block.instructions().insert(before, std::move(new_inst));
    return raw;
}

oir::Value *simplify_zext_cmp_compare(
    oir::Module &module, oir::BasicBlock &block,
    std::list<std::unique_ptr<oir::Instruction>>::iterator before, oir::CmpInst &cmp) {
    if (cmp.op() != oir::Instruction::OpID::ICmp ||
        (cmp.pred() != oir::CmpPred::EQ && cmp.pred() != oir::CmpPred::NE)) {
        return nullptr;
    }

    oir::CmpInst *src_cmp = nullptr;
    std::optional<std::int64_t> constant;
    if ((src_cmp = as_zext_cmp(cmp.lhs())) != nullptr) {
        constant = int_constant(cmp.rhs());
    } else if ((src_cmp = as_zext_cmp(cmp.rhs())) != nullptr) {
        constant = int_constant(cmp.lhs());
    }

    if (src_cmp == nullptr || !constant.has_value() || (*constant != 0 && *constant != 1)) {
        return nullptr;
    }

    const bool wants_true = (cmp.pred() == oir::CmpPred::NE && *constant == 0) ||
                            (cmp.pred() == oir::CmpPred::EQ && *constant == 1);
    if (wants_true) {
        return src_cmp;
    }
    return insert_bool_not(module, block, before, src_cmp,
                           cmp.name().empty() ? "not.zextcmp" : cmp.name());
}

enum class SimplifyMode {
    ConstantFold,
    Algebraic,
};

oir::Value *simplify_instruction(
    oir::Module &module, oir::BasicBlock &block,
    std::list<std::unique_ptr<oir::Instruction>>::iterator before, oir::Instruction &inst,
    SimplifyMode mode) {
    if (auto *binary = dynamic_cast<oir::BinaryInst *>(&inst)) {
        if (mode == SimplifyMode::ConstantFold) {
            auto lhs_int = int_constant(binary->lhs());
            auto rhs_int = int_constant(binary->rhs());
            if (lhs_int.has_value() && rhs_int.has_value()) {
                auto folded = fold_int_binary(inst.op(), *lhs_int, *rhs_int);
                if (folded.has_value()) {
                    return make_int_constant(module, inst.type(), *folded);
                }
            }

            auto lhs_float = float_constant(binary->lhs());
            auto rhs_float = float_constant(binary->rhs());
            if (lhs_float.has_value() && rhs_float.has_value()) {
                auto folded = fold_float_binary(inst.op(), *lhs_float, *rhs_float);
                if (folded.has_value()) {
                    return module.create_f32(*folded);
                }
            }
            return nullptr;
        }

        switch (inst.op()) {
        case oir::Instruction::OpID::Add:
            if (is_int_value(binary->rhs(), 0)) {
                return binary->lhs();
            }
            if (is_int_value(binary->lhs(), 0)) {
                return binary->rhs();
            }
            break;
        case oir::Instruction::OpID::Sub:
            if (is_int_value(binary->rhs(), 0)) {
                return binary->lhs();
            }
            break;
        case oir::Instruction::OpID::Mul:
            if (is_int_value(binary->rhs(), 1)) {
                return binary->lhs();
            }
            if (is_int_value(binary->lhs(), 1)) {
                return binary->rhs();
            }
            if (is_int_value(binary->rhs(), 0) || is_int_value(binary->lhs(), 0)) {
                return make_zero_constant(module, inst.type());
            }
            break;
        case oir::Instruction::OpID::SDiv:
            if (is_int_value(binary->rhs(), 1)) {
                return binary->lhs();
            }
            break;
        case oir::Instruction::OpID::SRem:
            if (is_int_value(binary->rhs(), 1)) {
                return make_zero_constant(module, inst.type());
            }
            break;
        default:
            break;
        }
        return nullptr;
    }

    if (auto *cmp = dynamic_cast<oir::CmpInst *>(&inst)) {
        if (mode == SimplifyMode::Algebraic) {
            return simplify_zext_cmp_compare(module, block, before, *cmp);
        }

        auto lhs_int = int_constant(cmp->lhs());
        auto rhs_int = int_constant(cmp->rhs());
        if (lhs_int.has_value() && rhs_int.has_value()) {
            return module.create_i1(eval_cmp(cmp->pred(), *lhs_int, *rhs_int));
        }

        auto lhs_float = float_constant(cmp->lhs());
        auto rhs_float = float_constant(cmp->rhs());
        if (lhs_float.has_value() && rhs_float.has_value()) {
            return module.create_i1(eval_fcmp(cmp->pred(), *lhs_float, *rhs_float));
        }
        return nullptr;
    }

    if (auto *cast = dynamic_cast<oir::CastInst *>(&inst)) {
        if (mode != SimplifyMode::ConstantFold) {
            return nullptr;
        }
        switch (inst.op()) {
        case oir::Instruction::OpID::ZExt:
            if (auto constant = int_constant(cast->src())) {
                return make_int_constant(module, inst.type(), *constant == 0 ? 0 : 1);
            }
            break;
        case oir::Instruction::OpID::SIToFP:
            if (auto constant = int_constant(cast->src())) {
                return module.create_f32(static_cast<float>(static_cast<std::int32_t>(*constant)));
            }
            break;
        case oir::Instruction::OpID::FPToSI:
            if (auto constant = float_constant(cast->src())) {
                return module.create_i32(static_cast<std::int32_t>(*constant));
            }
            break;
        default:
            break;
        }
        return nullptr;
    }

    if (auto *gep = dynamic_cast<oir::GetElementPtrInst *>(&inst)) {
        if (mode != SimplifyMode::Algebraic) {
            return nullptr;
        }
        bool all_zero = true;
        for (auto *index : gep->indices()) {
            if (!is_int_value(index, 0)) {
                all_zero = false;
                break;
            }
        }
        if (all_zero && gep->base_ptr()->type() == gep->type()) {
            return gep->base_ptr();
        }
        return nullptr;
    }

    if (auto *phi = dynamic_cast<oir::PhiInst *>(&inst)) {
        if (mode != SimplifyMode::Algebraic) {
            return nullptr;
        }
        if (phi->incoming().empty()) {
            return nullptr;
        }
        auto *first = phi->incoming().front().first;
        for (const auto &incoming : phi->incoming()) {
            if (incoming.first != first) {
                return nullptr;
            }
        }
        return first;
    }

    return nullptr;
}

void remove_phi_incoming_from(oir::BasicBlock *block, oir::BasicBlock *pred) {
    for (auto &inst : block->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        phi->remove_incoming_from(pred);
    }
}

void remove_edge(oir::BasicBlock *pred, oir::BasicBlock *succ) {
    pred->remove_successor(succ);
    succ->remove_predecessor(pred);
    remove_phi_incoming_from(succ, pred);
}

bool fold_branch(oir::Module &module, oir::BasicBlock &block,
                 std::list<std::unique_ptr<oir::Instruction>>::iterator term_it, bool take_true,
                 Stats &stats) {
    auto *branch = dynamic_cast<oir::BranchInst *>(term_it->get());
    if (branch == nullptr || !branch->is_conditional()) {
        return false;
    }

    auto *target = take_true ? branch->true_bb() : branch->false_bb();
    auto *removed = take_true ? branch->false_bb() : branch->true_bb();
    if (removed != target) {
        remove_edge(&block, removed);
    }
    block.add_successor(target);
    target->add_predecessor(&block);

    auto replacement =
        std::make_unique<oir::BranchInst>(module.types().void_ty(), target, &block);
    replacement->set_parent(&block);
    *term_it = std::move(replacement);
    ++stats.branches;
    return true;
}

bool simplify_branches(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        for (auto &block : function->blocks()) {
            if (!block->has_terminator()) {
                continue;
            }
            auto term_it = std::prev(block->instructions().end());
            auto *branch = dynamic_cast<oir::BranchInst *>(term_it->get());
            if (branch == nullptr || !branch->is_conditional()) {
                continue;
            }
            if (branch->true_bb() == branch->false_bb()) {
                changed |= fold_branch(module, *block, term_it, true, stats);
                continue;
            }
            auto condition = int_constant(branch->cond());
            if (condition.has_value()) {
                changed |= fold_branch(module, *block, term_it, *condition != 0, stats);
            }
        }
    }
    return changed;
}

bool local_simplify(oir::Module &module, Stats &stats, SimplifyMode mode) {
    oir::UseAnalysis uses(module);
    ReplacementMap replacements;
    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        for (auto &block : function->blocks()) {
            for (auto it = block->instructions().begin(); it != block->instructions().end(); ++it) {
                auto *replacement = simplify_instruction(module, *block, it, **it, mode);
                if (replacement != nullptr && replacement != it->get() &&
                    replacement->type() == (*it)->type() && uses.has_uses(it->get())) {
                    replacements[it->get()] = replacement;
                }
            }
        }
    }

    if (apply_replacements(module, replacements) == 0) {
        return false;
    }
    stats.folded += static_cast<unsigned>(replacements.size());
    return true;
}

enum class LatticeKind { Unknown, Constant, Overdefined };

struct LatticeValue {
    LatticeKind kind = LatticeKind::Unknown;
    oir::Value *constant = nullptr;

    static LatticeValue unknown() {
        return {};
    }

    static LatticeValue constant_value(oir::Value *value) {
        return {LatticeKind::Constant, value};
    }

    static LatticeValue overdefined() {
        return {LatticeKind::Overdefined, nullptr};
    }
};

bool lattice_equal(const LatticeValue &lhs, const LatticeValue &rhs) {
    if (lhs.kind != rhs.kind) {
        return false;
    }
    if (lhs.kind != LatticeKind::Constant) {
        return true;
    }
    return same_constant_value(lhs.constant, rhs.constant);
}

LatticeValue join_lattice(const LatticeValue &lhs, const LatticeValue &rhs) {
    if (lhs.kind == LatticeKind::Unknown) {
        return rhs;
    }
    if (rhs.kind == LatticeKind::Unknown) {
        return lhs;
    }
    if (lhs.kind == LatticeKind::Overdefined || rhs.kind == LatticeKind::Overdefined) {
        return LatticeValue::overdefined();
    }
    if (same_constant_value(lhs.constant, rhs.constant)) {
        return lhs;
    }
    return LatticeValue::overdefined();
}

class SCCPSolver final {
  public:
    explicit SCCPSolver(oir::Function &function) : function_(function), module_(*function.parent()) {
    }

    bool run(Stats &stats) {
        if (function_.is_external() || function_.entry_block() == nullptr) {
            return false;
        }

        executable_.insert(function_.entry_block());
        for (auto &arg : function_.args()) {
            value_state_[arg.get()] = LatticeValue::overdefined();
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (auto &block : function_.blocks()) {
                if (!is_executable(block.get())) {
                    continue;
                }
                for (auto &inst : block->instructions()) {
                    changed |= visit_instruction(*inst);
                }
            }
        }

        bool transformed = rewrite_constants(stats);
        return transformed;
    }

  private:
    bool is_executable(const oir::BasicBlock *block) const {
        return executable_.find(block) != executable_.end();
    }

    bool mark_executable(oir::BasicBlock *block) {
        return executable_.insert(block).second;
    }

    LatticeValue state_for(oir::Value *value) const {
        if (value == nullptr) {
            return LatticeValue::overdefined();
        }
        if (dynamic_cast<oir::ConstantInt *>(value) != nullptr ||
            dynamic_cast<oir::ConstantFloat *>(value) != nullptr ||
            dynamic_cast<oir::ConstantZero *>(value) != nullptr) {
            return LatticeValue::constant_value(value);
        }
        if (dynamic_cast<oir::UndefValue *>(value) != nullptr) {
            return LatticeValue::unknown();
        }
        if (dynamic_cast<oir::Argument *>(value) != nullptr ||
            dynamic_cast<oir::GlobalVariable *>(value) != nullptr ||
            dynamic_cast<oir::Function *>(value) != nullptr ||
            dynamic_cast<oir::BasicBlock *>(value) != nullptr) {
            return LatticeValue::overdefined();
        }
        auto found = value_state_.find(value);
        return found == value_state_.end() ? LatticeValue::unknown() : found->second;
    }

    bool update_value(oir::Value *value, LatticeValue next) {
        auto current = state_for(value);
        auto joined = join_lattice(current, next);
        if (lattice_equal(current, joined)) {
            return false;
        }
        value_state_[value] = joined;
        return true;
    }

    bool visit_instruction(oir::Instruction &inst) {
        if (auto *branch = dynamic_cast<oir::BranchInst *>(&inst)) {
            if (!branch->is_conditional()) {
                return mark_executable(branch->target_bb());
            }
            auto cond = state_for(branch->cond());
            if (cond.kind == LatticeKind::Constant) {
                auto constant = int_constant(cond.constant);
                if (constant.has_value()) {
                    return mark_executable(*constant != 0 ? branch->true_bb()
                                                          : branch->false_bb());
                }
            }
            if (cond.kind == LatticeKind::Overdefined) {
                bool changed = mark_executable(branch->true_bb());
                changed |= mark_executable(branch->false_bb());
                return changed;
            }
            return false;
        }

        if (inst.type() == nullptr || inst.type()->is_void()) {
            return false;
        }
        return update_value(&inst, evaluate(inst));
    }

    LatticeValue evaluate(oir::Instruction &inst) {
        if (auto *phi = dynamic_cast<oir::PhiInst *>(&inst)) {
            LatticeValue result = LatticeValue::unknown();
            bool saw_incoming = false;
            for (const auto &incoming : phi->incoming()) {
                if (!is_executable(incoming.second)) {
                    continue;
                }
                result = saw_incoming ? join_lattice(result, state_for(incoming.first))
                                      : state_for(incoming.first);
                saw_incoming = true;
            }
            return saw_incoming ? result : LatticeValue::unknown();
        }

        if (auto *binary = dynamic_cast<oir::BinaryInst *>(&inst)) {
            return evaluate_binary(*binary);
        }
        if (auto *cmp = dynamic_cast<oir::CmpInst *>(&inst)) {
            return evaluate_cmp(*cmp);
        }
        if (auto *cast = dynamic_cast<oir::CastInst *>(&inst)) {
            return evaluate_cast(*cast);
        }
        if (auto *gep = dynamic_cast<oir::GetElementPtrInst *>(&inst)) {
            bool all_zero = true;
            for (auto *index : gep->indices()) {
                auto index_state = state_for(index);
                if (index_state.kind != LatticeKind::Constant ||
                    !is_int_value(index_state.constant, 0)) {
                    all_zero = false;
                    break;
                }
            }
            if (all_zero && gep->base_ptr()->type() == gep->type()) {
                return state_for(gep->base_ptr());
            }
        }

        return LatticeValue::overdefined();
    }

    LatticeValue evaluate_binary(oir::BinaryInst &inst) {
        auto lhs = state_for(inst.lhs());
        auto rhs = state_for(inst.rhs());

        if (lhs.kind == LatticeKind::Constant && rhs.kind == LatticeKind::Constant) {
            if (auto lhs_int = int_constant(lhs.constant)) {
                auto rhs_int = int_constant(rhs.constant);
                if (rhs_int.has_value()) {
                    auto folded = fold_int_binary(inst.op(), *lhs_int, *rhs_int);
                    if (folded.has_value()) {
                        return LatticeValue::constant_value(
                            make_int_constant(module_, inst.type(), *folded));
                    }
                }
            }
            if (auto lhs_float = float_constant(lhs.constant)) {
                auto rhs_float = float_constant(rhs.constant);
                if (rhs_float.has_value()) {
                    auto folded = fold_float_binary(inst.op(), *lhs_float, *rhs_float);
                    if (folded.has_value()) {
                        return LatticeValue::constant_value(module_.create_f32(*folded));
                    }
                }
            }
        }

        switch (inst.op()) {
        case oir::Instruction::OpID::Add:
            if (rhs.kind == LatticeKind::Constant && is_int_value(rhs.constant, 0)) {
                return lhs;
            }
            if (lhs.kind == LatticeKind::Constant && is_int_value(lhs.constant, 0)) {
                return rhs;
            }
            break;
        case oir::Instruction::OpID::Sub:
            if (rhs.kind == LatticeKind::Constant && is_int_value(rhs.constant, 0)) {
                return lhs;
            }
            break;
        case oir::Instruction::OpID::Mul:
            if ((rhs.kind == LatticeKind::Constant && is_int_value(rhs.constant, 0)) ||
                (lhs.kind == LatticeKind::Constant && is_int_value(lhs.constant, 0))) {
                return LatticeValue::constant_value(make_zero_constant(module_, inst.type()));
            }
            if (rhs.kind == LatticeKind::Constant && is_int_value(rhs.constant, 1)) {
                return lhs;
            }
            if (lhs.kind == LatticeKind::Constant && is_int_value(lhs.constant, 1)) {
                return rhs;
            }
            break;
        case oir::Instruction::OpID::SDiv:
            if (rhs.kind == LatticeKind::Constant && is_int_value(rhs.constant, 1)) {
                return lhs;
            }
            break;
        case oir::Instruction::OpID::SRem:
            if (rhs.kind == LatticeKind::Constant && is_int_value(rhs.constant, 1)) {
                return LatticeValue::constant_value(make_zero_constant(module_, inst.type()));
            }
            break;
        default:
            break;
        }

        if (lhs.kind == LatticeKind::Overdefined || rhs.kind == LatticeKind::Overdefined) {
            return LatticeValue::overdefined();
        }
        return LatticeValue::unknown();
    }

    LatticeValue evaluate_cmp(oir::CmpInst &inst) {
        auto lhs = state_for(inst.lhs());
        auto rhs = state_for(inst.rhs());
        if (lhs.kind == LatticeKind::Constant && rhs.kind == LatticeKind::Constant) {
            if (auto lhs_int = int_constant(lhs.constant)) {
                auto rhs_int = int_constant(rhs.constant);
                if (rhs_int.has_value()) {
                    return LatticeValue::constant_value(
                        module_.create_i1(eval_cmp(inst.pred(), *lhs_int, *rhs_int)));
                }
            }
            if (auto lhs_float = float_constant(lhs.constant)) {
                auto rhs_float = float_constant(rhs.constant);
                if (rhs_float.has_value()) {
                    return LatticeValue::constant_value(
                        module_.create_i1(eval_fcmp(inst.pred(), *lhs_float, *rhs_float)));
                }
            }
        }
        if (lhs.kind == LatticeKind::Overdefined || rhs.kind == LatticeKind::Overdefined) {
            return LatticeValue::overdefined();
        }
        return LatticeValue::unknown();
    }

    LatticeValue evaluate_cast(oir::CastInst &inst) {
        auto src = state_for(inst.src());
        if (src.kind == LatticeKind::Constant) {
            switch (inst.op()) {
            case oir::Instruction::OpID::ZExt:
                if (auto constant = int_constant(src.constant)) {
                    return LatticeValue::constant_value(
                        make_int_constant(module_, inst.type(), *constant == 0 ? 0 : 1));
                }
                break;
            case oir::Instruction::OpID::SIToFP:
                if (auto constant = int_constant(src.constant)) {
                    return LatticeValue::constant_value(module_.create_f32(
                        static_cast<float>(static_cast<std::int32_t>(*constant))));
                }
                break;
            case oir::Instruction::OpID::FPToSI:
                if (auto constant = float_constant(src.constant)) {
                    return LatticeValue::constant_value(
                        module_.create_i32(static_cast<std::int32_t>(*constant)));
                }
                break;
            default:
                break;
            }
        }
        if (src.kind == LatticeKind::Overdefined) {
            return LatticeValue::overdefined();
        }
        return LatticeValue::unknown();
    }

    bool rewrite_constants(Stats &stats) {
        oir::UseAnalysis uses(module_);
        ReplacementMap replacements;
        for (auto &block : function_.blocks()) {
            for (auto &inst : block->instructions()) {
                auto found = value_state_.find(inst.get());
                if (found == value_state_.end() ||
                    found->second.kind != LatticeKind::Constant ||
                    found->second.constant == inst.get() || !is_scalar_type(inst->type()) ||
                    found->second.constant->type() != inst->type() || !uses.has_uses(inst.get())) {
                    continue;
                }
                replacements[inst.get()] = found->second.constant;
            }
        }
        if (apply_replacements(module_, replacements) == 0) {
            return false;
        }
        stats.sccp += static_cast<unsigned>(replacements.size());
        return true;
    }

    oir::Function &function_;
    oir::Module &module_;
    std::unordered_set<const oir::BasicBlock *> executable_;
    std::unordered_map<oir::Value *, LatticeValue> value_state_;
};

bool run_sccp(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        SCCPSolver solver(*function);
        changed |= solver.run(stats);
    }
    return changed;
}

bool eliminate_dead_code(oir::Module &module, Stats &stats) {
    bool changed = false;
    bool keep_going = true;
    while (keep_going) {
        keep_going = false;
        oir::UseAnalysis uses(module);
        for (auto &function : module.functions()) {
            if (function->is_external()) {
                continue;
            }
            for (auto &block : function->blocks()) {
                for (auto it = block->instructions().begin(); it != block->instructions().end();) {
                    if (is_pure_instruction(**it) && !uses.has_uses(it->get())) {
                        it = block->instructions().erase(it);
                        ++stats.dce;
                        changed = true;
                        keep_going = true;
                        continue;
                    }
                    ++it;
                }
            }
        }
    }
    return changed;
}

bool verify_oir(oir::Module &module, std::string &message) {
    message.clear();
    return module.verify(&message);
}

} // namespace

template <typename Fn>
PassResult run_oir_transform(PassContext &context, const std::string &missing_message, Fn &&fn) {
    auto *module = context.ssa_module();
    if (module == nullptr) {
        return PassResult::fail(missing_message);
    }

    try {
        Stats stats;
        bool changed = fn(*module, stats);
        if (changed) {
            context.invalidate_oir_analyses();
        }

        std::string message;
        if (!verify_oir(*module, message)) {
            return PassResult::fail(message.empty() ? "OIR verification failed after transform"
                                                    : message);
        }
        return PassResult::ok(changed, stats.message());
    } catch (const std::exception &ex) {
        return PassResult::fail(ex.what());
    }
}

std::string_view OIRConstantFoldPass::name() const {
    return "OIRConstantFoldPass";
}

PassKind OIRConstantFoldPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRConstantFoldPass::run(PassContext &context) {
    return run_oir_transform(
        context, "OIRConstantFoldPass requires OIR module in pass context",
        [](oir::Module &module, Stats &stats) {
            bool changed = local_simplify(module, stats, SimplifyMode::ConstantFold);
            changed |= simplify_branches(module, stats);
            changed |= eliminate_dead_code(module, stats);
            return changed;
        });
}

std::string_view OIRAlgebraicSimplifyPass::name() const {
    return "OIRAlgebraicSimplifyPass";
}

PassKind OIRAlgebraicSimplifyPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRAlgebraicSimplifyPass::run(PassContext &context) {
    return run_oir_transform(
        context, "OIRAlgebraicSimplifyPass requires OIR module in pass context",
        [](oir::Module &module, Stats &stats) {
            bool changed = local_simplify(module, stats, SimplifyMode::Algebraic);
            changed |= eliminate_dead_code(module, stats);
            return changed;
        });
}

std::string_view OIRSCCPPass::name() const {
    return "OIRSCCPPass";
}

PassKind OIRSCCPPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRSCCPPass::run(PassContext &context) {
    return run_oir_transform(context, "OIRSCCPPass requires OIR module in pass context",
                             [](oir::Module &module, Stats &stats) {
                                 bool changed = run_sccp(module, stats);
                                 changed |= simplify_branches(module, stats);
                                 changed |= eliminate_dead_code(module, stats);
                                 return changed;
                             });
}

} // namespace pass
