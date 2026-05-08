#include "../../include/oir/OIRScalarOpt.h"

#include "../../include/oir/OIRAnalysis.h"

#include <unordered_map>
#include <unordered_set>

namespace pass::oir_opt {

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
    explicit SCCPSolver(oir::Function &function)
        : function_(function), module_(*function.parent()) {
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
                    return mark_executable(*constant != 0 ? branch->true_bb() : branch->false_bb());
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
                if (found == value_state_.end() || found->second.kind != LatticeKind::Constant ||
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

} // namespace pass::oir_opt
