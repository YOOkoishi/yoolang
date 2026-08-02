#include "oir/OIRAnalysis.h"
#include "oir/OIRCFGUtils.h"
#include "oir/OIRScalarOpt.h"
#include "pass/oir/OIRCountdownLoopAnalysis.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

class FinalValueMaterializer final {
  public:
    FinalValueMaterializer(oir::Module &module, oir::BasicBlock &source, oir::BasicBlock &dest,
                           oir::PhiInst &induction,
                           const oir::FunctionModRefAnalysis &modref)
        : module_(module), source_(source), dest_(dest), induction_(induction), modref_(modref) {
    }

    oir::Value *materialize(oir::Value *value) {
        if (value == &induction_) {
            return make_int_constant(module_, induction_.type(), 1);
        }

        auto *inst = dynamic_cast<oir::Instruction *>(value);
        if (inst == nullptr || inst->parent() != &source_) {
            return value;
        }

        auto found = cloned_.find(inst);
        if (found != cloned_.end()) {
            return found->second;
        }
        if (!active_.insert(inst).second) {
            return nullptr;
        }

        std::unique_ptr<oir::Instruction> clone = clone_instruction(*inst);
        active_.erase(inst);
        if (clone == nullptr) {
            return nullptr;
        }

        auto *raw = clone.get();
        cloned_[inst] = raw;
        dest_.append_instruction(std::move(clone));
        return raw;
    }

  private:
    std::string clone_name(const oir::Instruction &inst) const {
        return inst.name().empty() ? "" : inst.name() + ".final";
    }

    std::vector<oir::Value *> materialize_values(const std::vector<oir::Value *> &values) {
        std::vector<oir::Value *> out;
        out.reserve(values.size());
        for (auto *value : values) {
            auto *mapped = materialize(value);
            if (mapped == nullptr) {
                return {};
            }
            out.push_back(mapped);
        }
        return out;
    }

    std::unique_ptr<oir::Instruction> clone_instruction(oir::Instruction &inst) {
        if (auto *binary = dynamic_cast<oir::BinaryInst *>(&inst)) {
            auto *lhs = materialize(binary->lhs());
            auto *rhs = materialize(binary->rhs());
            if (lhs == nullptr || rhs == nullptr) {
                return nullptr;
            }
            return std::make_unique<oir::BinaryInst>(binary->type(), binary->op(), lhs, rhs,
                                                     &dest_, clone_name(inst));
        }

        if (auto *cmp = dynamic_cast<oir::CmpInst *>(&inst)) {
            auto *lhs = materialize(cmp->lhs());
            auto *rhs = materialize(cmp->rhs());
            if (lhs == nullptr || rhs == nullptr) {
                return nullptr;
            }
            return std::make_unique<oir::CmpInst>(cmp->type(), cmp->op(), cmp->pred(), lhs, rhs,
                                                  &dest_, clone_name(inst));
        }

        if (auto *cast = dynamic_cast<oir::CastInst *>(&inst)) {
            auto *src = materialize(cast->src());
            if (src == nullptr) {
                return nullptr;
            }
            return std::make_unique<oir::CastInst>(cast->type(), cast->op(), src, &dest_,
                                                   clone_name(inst));
        }

        if (auto *gep = dynamic_cast<oir::GetElementPtrInst *>(&inst)) {
            auto *base = materialize(gep->base_ptr());
            auto indices = materialize_values(gep->indices());
            if (base == nullptr || indices.size() != gep->indices().size()) {
                return nullptr;
            }
            return std::make_unique<oir::GetElementPtrInst>(gep->type(), base, indices, &dest_,
                                                            clone_name(inst));
        }

        if (auto *load = dynamic_cast<oir::LoadInst *>(&inst)) {
            auto *ptr = materialize(load->ptr());
            if (ptr == nullptr) {
                return nullptr;
            }
            return std::make_unique<oir::LoadInst>(load->type(), ptr, &dest_, clone_name(inst));
        }

        if (auto *call = dynamic_cast<oir::CallInst *>(&inst)) {
            if (modref_.call_has_side_effect(*call)) {
                return nullptr;
            }
            auto *callee = materialize(call->callee());
            auto args = materialize_values(call->args());
            if (callee == nullptr || args.size() != call->args().size()) {
                return nullptr;
            }
            return std::make_unique<oir::CallInst>(call->type(), callee, args, &dest_,
                                                   clone_name(inst));
        }

        return nullptr;
    }

    oir::Module &module_;
    oir::BasicBlock &source_;
    oir::BasicBlock &dest_;
    oir::PhiInst &induction_;
    const oir::FunctionModRefAnalysis &modref_;
    std::unordered_map<oir::Instruction *, oir::Value *> cloned_;
    std::unordered_set<oir::Instruction *> active_;
};

bool rewrite_countdown_loop(oir::Function &function, const RotatedOverwrittenCountdownLoop &loop,
                            const oir::FunctionModRefAnalysis &modref, Stats &stats) {
    if (loop.body == nullptr || loop.preheader == nullptr || loop.exit == nullptr ||
        loop.induction == nullptr) {
        return false;
    }

    auto *final_block = function.create_block("countdown.final");
    FinalValueMaterializer materializer(*function.parent(), *loop.body, *final_block,
                                        *loop.induction, modref);

    struct PhiUpdate {
        oir::PhiInst *phi = nullptr;
        oir::Value *value = nullptr;
    };
    std::vector<PhiUpdate> updates;
    for (auto &inst : loop.exit->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }
        for (const auto &[value, pred] : phi->incoming()) {
            if (pred != loop.body) {
                continue;
            }
            auto *materialized = materializer.materialize(value);
            if (materialized == nullptr) {
                function.erase_block(final_block);
                return false;
            }
            updates.push_back({phi, materialized});
        }
    }

    if (updates.empty()) {
        function.erase_block(final_block);
        return false;
    }

    oir::cfg::append_unconditional_branch(*function.parent(), final_block, loop.exit);
    for (auto update : updates) {
        update.phi->add_incoming(update.value, final_block);
    }
    if (!oir::cfg::replace_successor(loop.preheader, loop.body, final_block)) {
        return false;
    }

    ++stats.cfg;
    stats.dce += static_cast<unsigned>(loop.body->instructions().size());
    return true;
}

bool run_on_function(oir::Function &function, const oir::FunctionModRefAnalysis &modref,
                     Stats &stats) {
    if (function.is_external() || function.entry_block() == nullptr) {
        return false;
    }

    oir::DominatorTree dom_tree(function);
    oir::LoopInfo loop_info(function, dom_tree);
    for (const auto &loop : loop_info.loops()) {
        auto match = match_rotated_overwritten_countdown_loop(loop, modref);
        if (match.has_value() && rewrite_countdown_loop(function, *match, modref, stats)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool eliminate_overwritten_countdown_loops(oir::Module &module, Stats &stats) {
    bool changed = false;
    constexpr unsigned kMaxIterations = 16;
    for (unsigned iteration = 0; iteration < kMaxIterations; ++iteration) {
        bool iteration_changed = false;
        oir::FunctionModRefAnalysis modref(module);
        for (auto &function : module.functions()) {
            iteration_changed |= run_on_function(*function, modref, stats);
        }
        changed |= iteration_changed;
        if (!iteration_changed) {
            break;
        }
    }
    return changed;
}

} // namespace pass::oir_opt
