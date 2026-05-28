#include "../../include/oir/OIRAnalysis.h"
#include "../../include/oir/OIRCFGUtils.h"
#include "../../include/oir/OIRScalarOpt.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

bool contains_block(const oir::Loop &loop, const oir::BasicBlock *block) {
    return std::find(loop.blocks.begin(), loop.blocks.end(), block) != loop.blocks.end();
}

oir::BasicBlock *mut(const oir::BasicBlock *block) {
    return const_cast<oir::BasicBlock *>(block);
}

bool is_zero(oir::Value *value) {
    auto constant = int_constant(value);
    return constant.has_value() && *constant == 0;
}

bool is_one(oir::Value *value) {
    auto constant = int_constant(value);
    return constant.has_value() && *constant == 1;
}

bool is_minus_one(oir::Value *value) {
    auto constant = int_constant(value);
    return constant.has_value() && *constant == -1;
}

bool is_known_nonnegative_count(oir::Value *value) {
    if (auto constant = int_constant(value)) {
        return *constant >= 0;
    }

    auto *call = dynamic_cast<oir::CallInst *>(value);
    auto *callee = call == nullptr ? nullptr : dynamic_cast<oir::Function *>(call->callee());
    if (callee == nullptr) {
        return false;
    }
    return callee->name() == "getarray" || callee->name() == "getfarray";
}

bool cmp_ne_value_zero(oir::Value *cond, oir::Value *value) {
    auto *cmp = dynamic_cast<oir::CmpInst *>(cond);
    if (cmp == nullptr || cmp->pred() != oir::CmpPred::NE) {
        return false;
    }
    return (cmp->lhs() == value && is_zero(cmp->rhs())) ||
           (cmp->rhs() == value && is_zero(cmp->lhs()));
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
    return preheader;
}

struct CountdownLoop {
    oir::BasicBlock *preheader = nullptr;
    oir::BasicBlock *header = nullptr;
    oir::BasicBlock *exit = nullptr;
    oir::PhiInst *induction = nullptr;
    oir::Value *start = nullptr;
    oir::BinaryInst *next = nullptr;
};

std::pair<oir::Value *, oir::Value *> incoming_pair(const oir::PhiInst &phi,
                                                    oir::BasicBlock *preheader,
                                                    oir::BasicBlock *latch) {
    oir::Value *start = nullptr;
    oir::Value *back = nullptr;
    for (const auto &[value, pred] : phi.incoming()) {
        if (pred == preheader) {
            start = value;
        } else if (pred == latch) {
            back = value;
        }
    }
    return {start, back};
}

bool is_decrement_by_one(const oir::BinaryInst &next, const oir::PhiInst &phi) {
    if (next.op() == oir::Instruction::OpID::Sub && next.lhs() == &phi && is_one(next.rhs())) {
        return true;
    }
    if (next.op() != oir::Instruction::OpID::Add) {
        return false;
    }
    return (next.lhs() == &phi && is_minus_one(next.rhs())) ||
           (next.rhs() == &phi && is_minus_one(next.lhs()));
}

CountdownLoop match_countdown_loop(const oir::Loop &loop) {
    CountdownLoop out;
    if (loop.blocks.size() != 1 || loop.header == nullptr) {
        return out;
    }

    auto *header = mut(loop.header);
    auto *preheader = find_preheader(loop);
    auto *preheader_branch =
        preheader == nullptr ? nullptr : dynamic_cast<oir::BranchInst *>(preheader->terminator());
    auto *loop_branch = dynamic_cast<oir::BranchInst *>(header->terminator());
    if (preheader_branch == nullptr || loop_branch == nullptr ||
        !preheader_branch->is_conditional() || !loop_branch->is_conditional()) {
        return out;
    }
    if (loop_branch->true_bb() != header || loop_branch->false_bb() == header) {
        return out;
    }

    auto *exit = loop_branch->false_bb();
    if (preheader_branch->true_bb() != header || preheader_branch->false_bb() != exit) {
        return out;
    }

    for (auto &inst : header->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }

        auto [start, back] = incoming_pair(*phi, preheader, header);
        auto *next = dynamic_cast<oir::BinaryInst *>(back);
        if (start == nullptr || next == nullptr || !is_decrement_by_one(*next, *phi) ||
            !is_known_nonnegative_count(start)) {
            continue;
        }
        if (!cmp_ne_value_zero(preheader_branch->cond(), start) ||
            !cmp_ne_value_zero(loop_branch->cond(), next)) {
            continue;
        }

        out.preheader = preheader;
        out.header = header;
        out.exit = exit;
        out.induction = phi;
        out.start = start;
        out.next = next;
        return out;
    }

    return out;
}

bool loop_body_has_no_effects(const oir::BasicBlock &header,
                              const oir::FunctionModRefAnalysis &modref) {
    for (const auto &inst : header.instructions()) {
        if (dynamic_cast<const oir::PhiInst *>(inst.get()) != nullptr || inst->is_terminator()) {
            continue;
        }
        if (auto *call = dynamic_cast<const oir::CallInst *>(inst.get())) {
            if (modref.call_has_side_effect(*call)) {
                return false;
            }
            continue;
        }
        if (inst->op() == oir::Instruction::OpID::Store ||
            inst->op() == oir::Instruction::OpID::Alloca) {
            return false;
        }
        if (!is_pure_instruction(*inst)) {
            return false;
        }
    }
    return true;
}

bool external_uses_are_exit_phis(const oir::BasicBlock &header, const oir::BasicBlock &exit) {
    for (const auto &inst : header.instructions()) {
        if (inst->is_terminator()) {
            continue;
        }
        for (auto *user : inst->users()) {
            auto *user_inst = dynamic_cast<oir::Instruction *>(user);
            if (user_inst != nullptr && user_inst->parent() == &header) {
                continue;
            }
            auto *phi = dynamic_cast<oir::PhiInst *>(user);
            if (phi == nullptr || phi->parent() != &exit) {
                return false;
            }
        }
    }
    return true;
}

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

bool rewrite_countdown_loop(oir::Function &function, const CountdownLoop &loop,
                            const oir::FunctionModRefAnalysis &modref, Stats &stats) {
    if (loop.header == nullptr || loop.preheader == nullptr || loop.exit == nullptr ||
        loop.induction == nullptr) {
        return false;
    }
    if (!loop_body_has_no_effects(*loop.header, modref) ||
        !external_uses_are_exit_phis(*loop.header, *loop.exit)) {
        return false;
    }

    auto *final_block = function.create_block("countdown.final");
    FinalValueMaterializer materializer(*function.parent(), *loop.header, *final_block,
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
            if (pred != loop.header) {
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
    if (!oir::cfg::replace_successor(loop.preheader, loop.header, final_block)) {
        return false;
    }

    ++stats.cfg;
    stats.dce += static_cast<unsigned>(loop.header->instructions().size());
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
        if (rewrite_countdown_loop(function, match_countdown_loop(loop), modref, stats)) {
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
