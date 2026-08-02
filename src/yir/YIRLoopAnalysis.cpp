#include "yir/YIRLoopAnalysis.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace yir {
namespace {

const CondOp *terminating_cond(const Region &region) {
    if (region.operations().empty()) {
        return nullptr;
    }
    return dynamic_cast<const CondOp *>(region.operations().back().get());
}

const ConstI32Op *const_i32_def(const Value *value) {
    return value == nullptr ? nullptr : dynamic_cast<const ConstI32Op *>(value->defining_op());
}

AffineExpr unknown_affine() {
    AffineExpr expr;
    expr.unknown = true;
    return expr;
}

bool const_i32_value(const Value *value, std::int64_t &out) {
    if (auto *constant = const_i32_def(value)) {
        out = constant->value();
        return true;
    }
    return false;
}

const Value *canonical_memory_object(const Value *value) {
    while (value != nullptr) {
        const auto *def = value->defining_op();
        if (const auto *elem_addr = dynamic_cast<const ElemAddrOp *>(def)) {
            value = elem_addr->base();
            continue;
        }
        if (const auto *decay = dynamic_cast<const DecayOp *>(def)) {
            value = decay->array_address();
            continue;
        }
        break;
    }
    return value;
}

bool is_local_unique_object(const Value *value) {
    value = canonical_memory_object(value);
    auto *def = value == nullptr ? nullptr : value->defining_op();
    return dynamic_cast<const ArrayVarOp *>(def) != nullptr ||
           dynamic_cast<const AllocaOp *>(def) != nullptr;
}

bool is_global_object(const Value *value) {
    value = canonical_memory_object(value);
    return value != nullptr && value->defining_op() == nullptr &&
           !value->name().empty() && value->name().front() == '@';
}

bool different_bases_noalias(const Value *lhs, const Value *rhs) {
    lhs = canonical_memory_object(lhs);
    rhs = canonical_memory_object(rhs);
    if (lhs == rhs) {
        return false;
    }
    if (is_local_unique_object(lhs) || is_local_unique_object(rhs)) {
        return true;
    }
    return is_global_object(lhs) && is_global_object(rhs);
}

const ICmpOp *while_condition_compare(const WhileOp &op) {
    auto *cond = terminating_cond(op.cond_region());
    if (cond == nullptr || cond->condition() == nullptr) {
        return nullptr;
    }
    return dynamic_cast<const ICmpOp *>(cond->condition()->defining_op());
}

bool is_add_step(const AssignOp &assign, const Value *iv, const Value *&step) {
    auto *add = dynamic_cast<const AddIOp *>(assign.value()->defining_op());
    if (add == nullptr) {
        return false;
    }
    if (add->lhs() == iv) {
        step = add->rhs();
        return true;
    }
    if (add->rhs() == iv) {
        step = add->lhs();
        return true;
    }
    return false;
}

bool is_sub_step(const AssignOp &assign, const Value *iv, const Value *&step) {
    auto *sub = dynamic_cast<const SubIOp *>(assign.value()->defining_op());
    if (sub == nullptr || sub->lhs() != iv) {
        return false;
    }
    step = sub->rhs();
    return true;
}

const AssignOp *find_latch_update(const Region &body, const Value *iv, const Value *&step,
                                  bool &negative) {
    for (auto it = body.operations().rbegin(); it != body.operations().rend(); ++it) {
        auto *assign = dynamic_cast<const AssignOp *>(it->get());
        if (assign == nullptr || assign->target() != iv) {
            continue;
        }
        negative = false;
        if (is_add_step(*assign, iv, step)) {
            return assign;
        }
        negative = true;
        if (is_sub_step(*assign, iv, step)) {
            return assign;
        }
        return nullptr;
    }
    return nullptr;
}

void add_term(AffineExpr &expr, const Value *value, std::int64_t coefficient) {
    if (coefficient == 0 || value == nullptr) {
        return;
    }
    for (auto &term : expr.terms) {
        if (term.value == value) {
            term.coefficient += coefficient;
            return;
        }
    }
    expr.terms.push_back({value, coefficient});
}

AffineExpr add_expr(AffineExpr lhs, const AffineExpr &rhs, std::int64_t sign = 1) {
    if (lhs.unknown || rhs.unknown) {
        return unknown_affine();
    }
    lhs.constant += sign * rhs.constant;
    for (const auto &term : rhs.terms) {
        add_term(lhs, term.value, sign * term.coefficient);
    }
    lhs.terms.erase(std::remove_if(lhs.terms.begin(), lhs.terms.end(),
                                   [](const AffineTerm &term) {
                                       return term.coefficient == 0;
                                   }),
                    lhs.terms.end());
    return lhs;
}

class Analyzer final {
  public:
    std::vector<LoopSummary> analyze(const Module &module) {
        loops_.clear();
        loop_stack_.clear();
        for (const auto &function : module.functions()) {
            scan_region(function->body(), *function);
        }
        mark_matrix_like_nests();
        return std::move(loops_);
    }

  private:
    void scan_region(const Region &region, const Function &function) {
        for (const auto &op : region.operations()) {
            scan_op(*op, function);
        }
    }

    void scan_op(const Operation &op, const Function &function) {
        if (const auto *if_op = dynamic_cast<const IfOp *>(&op)) {
            scan_region(if_op->then_region(), function);
            if (if_op->has_else()) {
                scan_region(if_op->else_region(), function);
            }
            return;
        }
        if (const auto *for_op = dynamic_cast<const ForOp *>(&op)) {
            enter_loop(op, function, LoopKind::For, for_op);
            scan_region(for_op->body_region(), function);
            loop_stack_.pop_back();
            return;
        }
        if (const auto *while_op = dynamic_cast<const WhileOp *>(&op)) {
            enter_loop(op, function, LoopKind::While, while_op);
            scan_region(while_op->cond_region(), function);
            scan_region(while_op->body_region(), function);
            loop_stack_.pop_back();
            return;
        }
    }

    void enter_loop(const Operation &op, const Function &function, LoopKind kind,
                    const ForOp *for_op) {
        LoopSummary summary;
        summary.id = loops_.size();
        summary.function = &function;
        summary.op = &op;
        summary.kind = kind;
        summary.parent = loop_stack_.empty() ? -1 : static_cast<int>(loop_stack_.back());
        summary.depth = loop_stack_.size();
        summary.canonical = canonical_form(op, kind);
        summary.induction_var = for_op->induction_var();
        summary.lower_bound = for_op->lower_bound();
        summary.upper_bound = for_op->upper_bound();
        summary.step = for_op->step();
        compute_trip_count(summary);
        loops_.push_back(std::move(summary));
        loop_stack_.push_back(loops_.back().id);

        auto &stored = loops_.back();
        collect_array_accesses(for_op->body_region(), stored);
        compute_dependencies(stored);
    }

    void enter_loop(const Operation &op, const Function &function, LoopKind kind,
                    const WhileOp *while_op) {
        LoopSummary summary;
        summary.id = loops_.size();
        summary.function = &function;
        summary.op = &op;
        summary.kind = kind;
        summary.parent = loop_stack_.empty() ? -1 : static_cast<int>(loop_stack_.back());
        summary.depth = loop_stack_.size();
        summary.canonical = canonical_form(op, kind);
        infer_while_bounds(*while_op, summary);
        compute_trip_count(summary);
        loops_.push_back(std::move(summary));
        loop_stack_.push_back(loops_.back().id);

        auto &stored = loops_.back();
        collect_array_accesses(while_op->body_region(), stored);
        compute_dependencies(stored);
    }

    LoopCanonicalForm canonical_form(const Operation &op, LoopKind kind) const {
        LoopCanonicalForm form;
        form.preheader = op.parent();
        form.exit = op.parent();
        form.preheader_label = "parent region before loop op";
        form.exit_label = "parent region after loop op";
        if (kind == LoopKind::For) {
            auto &for_op = static_cast<const ForOp &>(op);
            form.header = op.parent();
            form.body = &for_op.body_region();
            form.latch = &for_op.body_region();
            form.header_label = "canonical yir.for compare";
            form.body_label = "yir.for body";
            form.latch_label = "canonical yir.for induction step";
        } else {
            auto &while_op = static_cast<const WhileOp &>(op);
            form.header = &while_op.cond_region();
            form.body = &while_op.body_region();
            form.latch = &while_op.body_region();
            form.header_label = "yir.while cond_region";
            form.body_label = "yir.while body_region";
            form.latch_label = "body backedge/continue";
        }
        return form;
    }

    void infer_while_bounds(const WhileOp &op, LoopSummary &summary) const {
        auto *icmp = while_condition_compare(op);
        if (icmp == nullptr) {
            return;
        }

        const Value *iv = nullptr;
        const Value *upper = nullptr;
        if (icmp->predicate() == ICmpOp::Predicate::Lt || icmp->predicate() == ICmpOp::Predicate::Le) {
            iv = icmp->lhs();
            upper = icmp->rhs();
        } else if (icmp->predicate() == ICmpOp::Predicate::Gt ||
                   icmp->predicate() == ICmpOp::Predicate::Ge) {
            iv = icmp->lhs();
            upper = icmp->rhs();
        } else {
            return;
        }

        const Value *step = nullptr;
        bool negative = false;
        if (find_latch_update(op.body_region(), iv, step, negative) == nullptr) {
            return;
        }
        summary.induction_var = iv;
        summary.upper_bound = upper;
        summary.step = step;
    }

    void compute_trip_count(LoopSummary &summary) const {
        std::int64_t lower = 0;
        std::int64_t upper = 0;
        std::int64_t step = 0;
        if (!const_i32_value(summary.lower_bound, lower) ||
            !const_i32_value(summary.upper_bound, upper) || !const_i32_value(summary.step, step) ||
            step <= 0) {
            return;
        }
        if (upper <= lower) {
            summary.trip_count = 0;
        } else {
            summary.trip_count = (upper - lower + step - 1) / step;
        }
        summary.has_trip_count = true;
    }

    void collect_array_accesses(const Region &region, LoopSummary &summary) const {
        for (const auto &op : region.operations()) {
            if (const auto *load = dynamic_cast<const ArrayLoadOp *>(op.get())) {
                summary.array_accesses.push_back({op.get(), false, load->array(),
                                                  affine_indices(load->indices())});
                continue;
            }
            if (const auto *store = dynamic_cast<const ArrayStoreOp *>(op.get())) {
                summary.array_accesses.push_back({op.get(), true, store->array(),
                                                  affine_indices(store->indices())});
                continue;
            }
            if (const auto *if_op = dynamic_cast<const IfOp *>(op.get())) {
                collect_array_accesses(if_op->then_region(), summary);
                if (if_op->has_else()) {
                    collect_array_accesses(if_op->else_region(), summary);
                }
                continue;
            }
            if (const auto *while_op = dynamic_cast<const WhileOp *>(op.get())) {
                collect_array_accesses(while_op->cond_region(), summary);
                collect_array_accesses(while_op->body_region(), summary);
                continue;
            }
            if (const auto *for_op = dynamic_cast<const ForOp *>(op.get())) {
                collect_array_accesses(for_op->body_region(), summary);
                continue;
            }
        }
    }

    std::vector<AffineExpr> affine_indices(const std::vector<Value *> &indices) const {
        std::vector<AffineExpr> out;
        out.reserve(indices.size());
        for (auto *index : indices) {
            out.push_back(affine_expr(index));
        }
        return out;
    }

    AffineExpr affine_expr(const Value *value) const {
        std::int64_t constant = 0;
        if (const_i32_value(value, constant)) {
            AffineExpr expr;
            expr.constant = constant;
            return expr;
        }
        if (is_loop_induction(value)) {
            AffineExpr expr;
            add_term(expr, value, 1);
            return expr;
        }
        auto *def = value == nullptr ? nullptr : value->defining_op();
        if (auto *add = dynamic_cast<const AddIOp *>(def)) {
            return add_expr(affine_expr(add->lhs()), affine_expr(add->rhs()));
        }
        if (auto *sub = dynamic_cast<const SubIOp *>(def)) {
            return add_expr(affine_expr(sub->lhs()), affine_expr(sub->rhs()), -1);
        }
        if (auto *mul = dynamic_cast<const MulIOp *>(def)) {
            std::int64_t c = 0;
            if (const_i32_value(mul->lhs(), c)) {
                return scale_expr(affine_expr(mul->rhs()), c);
            }
            if (const_i32_value(mul->rhs(), c)) {
                return scale_expr(affine_expr(mul->lhs()), c);
            }
        }
        if (dynamic_cast<const ZExtI1ToI32Op *>(def) != nullptr ||
            dynamic_cast<const TruncI32ToI1Op *>(def) != nullptr) {
            return affine_expr(def->operands()[0]);
        }
        return unknown_affine();
    }

    AffineExpr scale_expr(AffineExpr expr, std::int64_t scale) const {
        if (expr.unknown) {
            return expr;
        }
        expr.constant *= scale;
        for (auto &term : expr.terms) {
            term.coefficient *= scale;
        }
        return expr;
    }

    bool is_loop_induction(const Value *value) const {
        for (auto id : loop_stack_) {
            if (loops_[id].induction_var == value) {
                return true;
            }
        }
        return false;
    }

    void compute_dependencies(LoopSummary &summary) const {
        for (std::size_t i = 0; i < summary.array_accesses.size(); ++i) {
            for (std::size_t j = i + 1; j < summary.array_accesses.size(); ++j) {
                const auto &lhs = summary.array_accesses[i];
                const auto &rhs = summary.array_accesses[j];
                if (!lhs.is_store && !rhs.is_store) {
                    continue;
                }
                if (lhs.array != rhs.array) {
                    const bool noalias = different_bases_noalias(lhs.array, rhs.array);
                    summary.dependencies.push_back(
                        {i, j,
                         noalias ? DependenceKind::Independent
                                 : DependenceKind::Unknown,
                         noalias ? "different unique base arrays"
                                 : "different bases may alias"});
                    continue;
                }
                summary.dependencies.push_back({i, j, classify_access_pair(lhs, rhs),
                                                "same base array"});
            }
        }
    }

    DependenceKind classify_access_pair(const ArrayAccess &lhs, const ArrayAccess &rhs) const {
        if (lhs.indices.size() != rhs.indices.size()) {
            return DependenceKind::Unknown;
        }

        bool all_same = true;
        for (std::size_t i = 0; i < lhs.indices.size(); ++i) {
            const auto &a = lhs.indices[i];
            const auto &b = rhs.indices[i];
            if (a.unknown || b.unknown) {
                return DependenceKind::Unknown;
            }
            if (a.is_constant() && b.is_constant() && a.constant != b.constant) {
                return DependenceKind::Independent;
            }
            if (!same_affine(a, b)) {
                all_same = false;
            }
        }
        if (all_same) {
            return DependenceKind::SameElement;
        }
        return DependenceKind::LoopCarriedPossible;
    }

    bool same_affine(const AffineExpr &lhs, const AffineExpr &rhs) const {
        if (lhs.unknown || rhs.unknown || lhs.constant != rhs.constant ||
            lhs.terms.size() != rhs.terms.size()) {
            return false;
        }
        for (const auto &term : lhs.terms) {
            auto found = std::find_if(rhs.terms.begin(), rhs.terms.end(),
                                      [&](const AffineTerm &rhs_term) {
                                          return rhs_term.value == term.value &&
                                                 rhs_term.coefficient == term.coefficient;
                                      });
            if (found == rhs.terms.end()) {
                return false;
            }
        }
        return true;
    }

    void mark_matrix_like_nests() {
        for (auto &loop : loops_) {
            bool has_nested_child = false;
            for (const auto &candidate : loops_) {
                if (candidate.parent == static_cast<int>(loop.id)) {
                    has_nested_child = true;
                    break;
                }
            }
            if (!has_nested_child) {
                continue;
            }
            loop.matrix_like_nest =
                std::any_of(loop.array_accesses.begin(), loop.array_accesses.end(),
                            [](const ArrayAccess &access) { return access.indices.size() >= 2; });
        }
    }

    std::vector<LoopSummary> loops_;
    std::vector<std::size_t> loop_stack_;
};

} // namespace

LoopAnalysis::LoopAnalysis(const Module &module) {
    analyze(module);
}

void LoopAnalysis::analyze(const Module &module) {
    Analyzer analyzer;
    loops_ = analyzer.analyze(module);
}

void LoopAnalysis::clear() {
    loops_.clear();
}

const LoopSummary *LoopAnalysis::summary_for(const Operation *op) const {
    auto found = std::find_if(loops_.begin(), loops_.end(),
                              [op](const LoopSummary &summary) { return summary.op == op; });
    return found == loops_.end() ? nullptr : &*found;
}

std::string affine_expr_to_string(const AffineExpr &expr) {
    if (expr.unknown) {
        return "<unknown>";
    }
    std::ostringstream oss;
    bool wrote = false;
    for (const auto &term : expr.terms) {
        if (wrote && term.coefficient >= 0) {
            oss << '+';
        }
        oss << term.coefficient << '*'
            << (term.value == nullptr || term.value->name().empty() ? "iv" : term.value->name());
        wrote = true;
    }
    if (expr.constant != 0 || !wrote) {
        if (wrote && expr.constant >= 0) {
            oss << '+';
        }
        oss << expr.constant;
    }
    return oss.str();
}

std::string dependence_kind_to_string(DependenceKind kind) {
    switch (kind) {
    case DependenceKind::Independent:
        return "independent";
    case DependenceKind::SameElement:
        return "same-element";
    case DependenceKind::LoopCarriedPossible:
        return "loop-carried-possible";
    case DependenceKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace yir
