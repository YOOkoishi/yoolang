#include "pass/yir/YIRPolyhedralCanonicalizePass.h"

#include "pass/ast/ASTToYIRPass.h"
#include "yir/YIR.h"
#include "yir/YIRLoopAnalysis.h"
#include "yir/YIRVerifier.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pass {
namespace {

using OpList = yir::Region::OpList;
using ValueSet = std::unordered_set<const yir::Value *>;
using RegionSet = std::unordered_set<const yir::Region *>;
using UseCounts = std::unordered_map<const yir::Value *, std::size_t>;

struct Stats {
    std::size_t loops_normalized = 0;
    std::size_t affine_operands_canonicalized = 0;
    std::size_t invariants_hoisted = 0;
    std::size_t dead_ops_removed = 0;
    std::size_t symbols_detected = 0;
    std::size_t guards_demoted = 0;

    bool changed() const {
        return loops_normalized != 0 || affine_operands_canonicalized != 0 ||
               invariants_hoisted != 0 || dead_ops_removed != 0 || guards_demoted != 0;
    }

    std::string message() const {
        std::ostringstream oss;
        oss << "loops_normalized=" << loops_normalized
            << ", affine_operands_canonicalized=" << affine_operands_canonicalized
            << ", invariants_hoisted=" << invariants_hoisted
            << ", dead_ops_removed=" << dead_ops_removed
            << ", symbols_detected=" << symbols_detected
            << ", guards_demoted=" << guards_demoted;
        return oss.str();
    }
};

struct AffineTerm {
    yir::Value *value = nullptr;
    std::int64_t coefficient = 0;
};

struct AffineExpr {
    bool valid = true;
    std::int64_t constant = 0;
    std::vector<AffineTerm> terms;
};

struct AffineComponent {
    bool is_constant = false;
    std::int64_t value = 0;
    yir::Value *term = nullptr;
};

AffineExpr invalid_affine() {
    AffineExpr expr;
    expr.valid = false;
    return expr;
}

bool is_i32_value(const yir::Value *value) {
    return value != nullptr && value->type() != nullptr &&
           value->type()->kind() == yir::Type::Kind::I32;
}

const yir::ConstI32Op *const_i32_def(const yir::Value *value) {
    return value == nullptr ? nullptr : dynamic_cast<const yir::ConstI32Op *>(value->defining_op());
}

bool const_i32_value(const yir::Value *value, std::int64_t &out) {
    auto *constant = const_i32_def(value);
    if (constant == nullptr) {
        return false;
    }
    out = constant->value();
    return true;
}

bool fits_i32(std::int64_t value) {
    return value >= std::numeric_limits<int>::min() &&
           value <= std::numeric_limits<int>::max();
}

void add_term(AffineExpr &expr, yir::Value *value, std::int64_t coefficient) {
    if (value == nullptr || coefficient == 0) {
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

void normalize_terms(AffineExpr &expr) {
    expr.terms.erase(std::remove_if(expr.terms.begin(), expr.terms.end(),
                                    [](const AffineTerm &term) {
                                        return term.coefficient == 0;
                                    }),
                     expr.terms.end());
    std::sort(expr.terms.begin(), expr.terms.end(),
              [](const AffineTerm &lhs, const AffineTerm &rhs) {
                  const std::string &lhs_name = lhs.value->name();
                  const std::string &rhs_name = rhs.value->name();
                  if (lhs_name != rhs_name) {
                      return lhs_name < rhs_name;
                  }
                  return std::less<const yir::Value *>{}(lhs.value, rhs.value);
              });
}

AffineExpr add_expr(AffineExpr lhs, const AffineExpr &rhs, std::int64_t sign = 1) {
    if (!lhs.valid || !rhs.valid) {
        return invalid_affine();
    }
    lhs.constant += sign * rhs.constant;
    for (const auto &term : rhs.terms) {
        add_term(lhs, term.value, sign * term.coefficient);
    }
    normalize_terms(lhs);
    return lhs;
}

AffineExpr scale_expr(AffineExpr expr, std::int64_t scale) {
    if (!expr.valid) {
        return expr;
    }
    expr.constant *= scale;
    for (auto &term : expr.terms) {
        term.coefficient *= scale;
    }
    normalize_terms(expr);
    return expr;
}

class AffineParser final {
  public:
    AffineParser() = default;

    explicit AffineParser(RegionSet materialized_regions)
        : materialized_regions_(std::move(materialized_regions)) {
    }

    AffineExpr parse(yir::Value *value) const {
        if (!is_i32_value(value)) {
            return invalid_affine();
        }

        std::int64_t constant = 0;
        if (const_i32_value(value, constant)) {
            AffineExpr expr;
            expr.constant = constant;
            return expr;
        }

        auto *def = value->defining_op();
        if (auto *add = dynamic_cast<yir::AddIOp *>(def)) {
            return add_expr(parse(add->lhs()), parse(add->rhs()));
        }
        if (auto *sub = dynamic_cast<yir::SubIOp *>(def)) {
            return add_expr(parse(sub->lhs()), parse(sub->rhs()), -1);
        }
        if (auto *mul = dynamic_cast<yir::MulIOp *>(def)) {
            std::int64_t scale = 0;
            if (const_i32_value(mul->lhs(), scale)) {
                return scale_expr(parse(mul->rhs()), scale);
            }
            if (const_i32_value(mul->rhs(), scale)) {
                return scale_expr(parse(mul->lhs()), scale);
            }
            return invalid_affine();
        }

        if (def != nullptr && materialized_regions_.find(def->parent()) != materialized_regions_.end()) {
            return invalid_affine();
        }

        AffineExpr expr;
        add_term(expr, value, 1);
        normalize_terms(expr);
        return expr;
    }

  private:
    RegionSet materialized_regions_;
};

std::vector<AffineComponent> components_for(const AffineExpr &expr) {
    std::vector<AffineComponent> components;
    components.reserve(expr.terms.size() + (expr.constant == 0 ? 0 : 1));
    for (const auto &term : expr.terms) {
        components.push_back({false, term.coefficient, term.value});
    }
    if (expr.constant != 0 || components.empty()) {
        components.push_back({true, expr.constant, nullptr});
    }
    return components;
}

bool expression_materializable(const AffineExpr &expr) {
    if (!expr.valid || !fits_i32(expr.constant)) {
        return false;
    }
    return std::all_of(expr.terms.begin(), expr.terms.end(), [](const AffineTerm &term) {
        return fits_i32(term.coefficient);
    });
}

bool matches_component(const yir::Value *value, const AffineComponent &component) {
    if (component.is_constant) {
        std::int64_t constant = 0;
        return const_i32_value(value, constant) && constant == component.value;
    }
    if (component.value == 1) {
        return value == component.term;
    }

    auto *mul = value == nullptr ? nullptr : dynamic_cast<const yir::MulIOp *>(value->defining_op());
    if (mul == nullptr || mul->rhs() != component.term) {
        return false;
    }
    std::int64_t coefficient = 0;
    return const_i32_value(mul->lhs(), coefficient) && coefficient == component.value;
}

bool matches_components_prefix(const yir::Value *value,
                               const std::vector<AffineComponent> &components,
                               std::size_t count) {
    if (count == 0) {
        return false;
    }
    if (count == 1) {
        return matches_component(value, components.front());
    }

    auto *add = value == nullptr ? nullptr : dynamic_cast<const yir::AddIOp *>(value->defining_op());
    if (add == nullptr) {
        return false;
    }
    return matches_components_prefix(add->lhs(), components, count - 1) &&
           matches_component(add->rhs(), components[count - 1]);
}

bool is_structurally_canonical(yir::Value *value, const AffineExpr &expr) {
    if (!expression_materializable(expr)) {
        return true;
    }
    auto components = components_for(expr);
    return matches_components_prefix(value, components, components.size());
}

class AffineMaterializer final {
  public:
    explicit AffineMaterializer(Stats &stats) : stats_(stats) {
    }

    yir::Value *materialize(const AffineExpr &expr, OpList &ops, std::size_t &insert_pos) {
        if (!expression_materializable(expr)) {
            return nullptr;
        }

        auto components = components_for(expr);
        yir::Value *value = materialize_component(components.front(), ops, insert_pos);
        if (value == nullptr) {
            return nullptr;
        }
        for (std::size_t i = 1; i < components.size(); ++i) {
            auto *rhs = materialize_component(components[i], ops, insert_pos);
            if (rhs == nullptr) {
                return nullptr;
            }
            value = insert_op<yir::AddIOp>(ops, insert_pos, value, rhs, fresh_name("add"));
        }
        return value;
    }

  private:
    template <typename OpT, typename... Args>
    yir::Value *insert_op(OpList &ops, std::size_t &insert_pos, Args &&...args) {
        auto op = std::make_unique<OpT>(std::forward<Args>(args)...);
        auto *result = op->result();
        op->set_parent(ops[insert_pos]->parent());
        ops.insert(ops.begin() + static_cast<std::ptrdiff_t>(insert_pos), std::move(op));
        ++insert_pos;
        return result;
    }

    yir::Value *materialize_component(const AffineComponent &component, OpList &ops,
                                      std::size_t &insert_pos) {
        if (component.is_constant) {
            if (!fits_i32(component.value)) {
                return nullptr;
            }
            return insert_op<yir::ConstI32Op>(ops, insert_pos, static_cast<int>(component.value),
                                              fresh_name("c"));
        }
        if (component.value == 1) {
            return component.term;
        }
        if (!fits_i32(component.value)) {
            return nullptr;
        }
        auto *coefficient =
            insert_op<yir::ConstI32Op>(ops, insert_pos, static_cast<int>(component.value),
                                       fresh_name("c"));
        return insert_op<yir::MulIOp>(ops, insert_pos, component.term, coefficient,
                                      fresh_name("mul"));
    }

    std::string fresh_name(const char *stem) {
        return std::string("poly.") + stem + std::to_string(next_temp_++);
    }

    Stats &stats_;
    std::size_t next_temp_ = 0;
};

const yir::CondOp *terminating_cond(const yir::Region &region) {
    if (region.operations().empty()) {
        return nullptr;
    }
    return dynamic_cast<const yir::CondOp *>(region.operations().back().get());
}

bool contains_abrupt_loop_control(const yir::Region &region) {
    for (const auto &op : region.operations()) {
        if (dynamic_cast<const yir::BreakOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ContinueOp *>(op.get()) != nullptr) {
            return true;
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            if (contains_abrupt_loop_control(if_op->then_region()) ||
                (if_op->has_else() && contains_abrupt_loop_control(if_op->else_region()))) {
                return true;
            }
        } else if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
            if (contains_abrupt_loop_control(while_op->cond_region()) ||
                contains_abrupt_loop_control(while_op->body_region())) {
                return true;
            }
        } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
            if (contains_abrupt_loop_control(for_op->body_region())) {
                return true;
            }
        }
    }
    return false;
}

bool is_condition_pure_op(const yir::Operation &op) {
    return dynamic_cast<const yir::ConstI32Op *>(&op) != nullptr ||
           dynamic_cast<const yir::ConstBoolOp *>(&op) != nullptr ||
           dynamic_cast<const yir::AddIOp *>(&op) != nullptr ||
           dynamic_cast<const yir::SubIOp *>(&op) != nullptr ||
           dynamic_cast<const yir::MulIOp *>(&op) != nullptr ||
           dynamic_cast<const yir::ICmpOp *>(&op) != nullptr ||
           dynamic_cast<const yir::CondOp *>(&op) != nullptr;
}

bool condition_region_is_side_effect_free(const yir::Region &region) {
    return std::all_of(region.operations().begin(), region.operations().end(),
                       [](const std::unique_ptr<yir::Operation> &op) {
                           return is_condition_pure_op(*op);
                       });
}

const yir::ICmpOp *condition_compare(const yir::WhileOp &op) {
    auto *cond = terminating_cond(op.cond_region());
    if (cond == nullptr || cond->condition() == nullptr) {
        return nullptr;
    }
    return dynamic_cast<const yir::ICmpOp *>(cond->condition()->defining_op());
}

bool region_may_assign_value(const yir::Region &region, const yir::Value *value);

bool op_may_assign_value(const yir::Operation &op, const yir::Value *value) {
    if (auto *assign = dynamic_cast<const yir::AssignOp *>(&op)) {
        return assign->target() == value;
    }
    if (auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
        return for_op->induction_var() == value || region_may_assign_value(for_op->body_region(), value);
    }
    if (auto *while_op = dynamic_cast<const yir::WhileOp *>(&op)) {
        return region_may_assign_value(while_op->cond_region(), value) ||
               region_may_assign_value(while_op->body_region(), value);
    }
    if (auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
        return region_may_assign_value(if_op->then_region(), value) ||
               (if_op->has_else() && region_may_assign_value(if_op->else_region(), value));
    }
    return false;
}

bool region_may_assign_value(const yir::Region &region, const yir::Value *value) {
    return std::any_of(region.operations().begin(), region.operations().end(),
                       [value](const std::unique_ptr<yir::Operation> &op) {
                           return op_may_assign_value(*op, value);
                       });
}

void collect_assigned(const yir::Region &region, ValueSet &assigned) {
    for (const auto &op : region.operations()) {
        if (auto *assign = dynamic_cast<const yir::AssignOp *>(op.get())) {
            assigned.insert(assign->target());
        } else if (auto *store = dynamic_cast<const yir::ArrayStoreOp *>(op.get())) {
            assigned.insert(store->array());
        } else if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            collect_assigned(if_op->then_region(), assigned);
            if (if_op->has_else()) {
                collect_assigned(if_op->else_region(), assigned);
            }
        } else if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
            collect_assigned(while_op->cond_region(), assigned);
            collect_assigned(while_op->body_region(), assigned);
        } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
            assigned.insert(for_op->induction_var());
            collect_assigned(for_op->body_region(), assigned);
        }
    }
}

bool expr_uses_value(const AffineExpr &expr, const yir::Value *value) {
    return std::any_of(expr.terms.begin(), expr.terms.end(), [value](const AffineTerm &term) {
        return term.value == value;
    });
}

bool expr_is_invariant_in_region(const AffineExpr &expr, const yir::Region &region,
                                 const yir::Value *iv) {
    if (!expr.valid || expr_uses_value(expr, iv)) {
        return false;
    }
    return std::none_of(expr.terms.begin(), expr.terms.end(), [&](const AffineTerm &term) {
        return region_may_assign_value(region, term.value);
    });
}

struct LoopCondition {
    yir::Value *upper_bound = nullptr;
    bool inclusive = false;
};

bool match_forward_upper_bound(const yir::ICmpOp &icmp, yir::Value *iv, LoopCondition &out) {
    using Predicate = yir::ICmpOp::Predicate;
    if (icmp.lhs() == iv) {
        if (icmp.predicate() == Predicate::Lt) {
            out = {icmp.rhs(), false};
            return true;
        }
        if (icmp.predicate() == Predicate::Le) {
            out = {icmp.rhs(), true};
            return true;
        }
    }
    if (icmp.rhs() == iv) {
        if (icmp.predicate() == Predicate::Gt) {
            out = {icmp.lhs(), false};
            return true;
        }
        if (icmp.predicate() == Predicate::Ge) {
            out = {icmp.lhs(), true};
            return true;
        }
    }
    return false;
}

struct LatchInfo {
    const yir::AssignOp *assign = nullptr;
    const yir::Operation *step_op = nullptr;
    yir::Value *step = nullptr;
};

bool find_positive_latch_update(const yir::Region &body, yir::Value *iv, LatchInfo &out) {
    const auto &ops = body.operations();
    if (ops.size() < 2) {
        return false;
    }

    auto *assign = dynamic_cast<const yir::AssignOp *>(ops.back().get());
    if (assign == nullptr || assign->target() != iv) {
        return false;
    }

    auto *add = dynamic_cast<const yir::AddIOp *>(assign->value()->defining_op());
    if (add == nullptr || add != ops[ops.size() - 2].get()) {
        return false;
    }

    yir::Value *step = nullptr;
    if (add->lhs() == iv) {
        step = add->rhs();
    } else if (add->rhs() == iv) {
        step = add->lhs();
    } else {
        return false;
    }

    out = {assign, add, step};
    return true;
}

bool find_loop_lower_bound(const OpList &ops, std::size_t loop_index, yir::Value *iv,
                           yir::Value *&lower_bound) {
    for (std::size_t cursor = loop_index; cursor > 0; --cursor) {
        const std::size_t candidate = cursor - 1;
        if (auto *assign = dynamic_cast<yir::AssignOp *>(ops[candidate].get())) {
            if (assign->target() == iv) {
                lower_bound = assign->value();
                return true;
            }
        } else if (auto *var = dynamic_cast<yir::VarOp *>(ops[candidate].get())) {
            if (var->result() == iv && var->has_initializer()) {
                lower_bound = var->initializer();
                return true;
            }
        }
        if (op_may_assign_value(*ops[candidate], iv)) {
            return false;
        }
    }
    return false;
}

void move_canonical_loop_body(yir::ForOp &for_op, yir::WhileOp &while_op,
                              const LatchInfo &latch) {
    auto &src = while_op.body_region().operations();
    auto &dst = for_op.body_region().operations();
    for (auto &op : src) {
        if (op.get() == latch.assign || op.get() == latch.step_op) {
            continue;
        }
        op->set_parent(&for_op.body_region());
        dst.push_back(std::move(op));
    }
    src.clear();
}

bool is_constant_like(const yir::Operation &op) {
    return dynamic_cast<const yir::ConstI32Op *>(&op) != nullptr ||
           dynamic_cast<const yir::ConstF32Op *>(&op) != nullptr ||
           dynamic_cast<const yir::ConstBoolOp *>(&op) != nullptr ||
           dynamic_cast<const yir::ZeroOp *>(&op) != nullptr;
}

bool is_affine_hoist_candidate(const yir::Operation &op) {
    return op.result() != nullptr &&
           (dynamic_cast<const yir::ConstI32Op *>(&op) != nullptr ||
            dynamic_cast<const yir::AddIOp *>(&op) != nullptr ||
            dynamic_cast<const yir::SubIOp *>(&op) != nullptr ||
            dynamic_cast<const yir::MulIOp *>(&op) != nullptr);
}

bool region_has_call(const yir::Region &region) {
    for (const auto &op : region.operations()) {
        if (dynamic_cast<const yir::CallOp *>(op.get()) != nullptr) {
            return true;
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            if (region_has_call(if_op->then_region()) ||
                (if_op->has_else() && region_has_call(if_op->else_region()))) {
                return true;
            }
        } else if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
            if (region_has_call(while_op->cond_region()) || region_has_call(while_op->body_region())) {
                return true;
            }
        } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
            if (region_has_call(for_op->body_region())) {
                return true;
            }
        }
    }
    return false;
}

bool operands_invariant(const yir::Operation &op, const ValueSet &assigned,
                        const ValueSet &defined_in_loop, const ValueSet &hoisted) {
    for (auto *operand : op.operands()) {
        if (assigned.find(operand) != assigned.end()) {
            return false;
        }
        auto *def = operand == nullptr ? nullptr : operand->defining_op();
        if (def != nullptr && def->result() != nullptr &&
            defined_in_loop.find(def->result()) != defined_in_loop.end() &&
            hoisted.find(def->result()) == hoisted.end()) {
            return false;
        }
    }
    return true;
}

bool is_removable_pure_op(const yir::Operation &op) {
    return dynamic_cast<const yir::ConstI32Op *>(&op) != nullptr ||
           dynamic_cast<const yir::ConstBoolOp *>(&op) != nullptr ||
           dynamic_cast<const yir::ZeroOp *>(&op) != nullptr ||
           dynamic_cast<const yir::AddIOp *>(&op) != nullptr ||
           dynamic_cast<const yir::SubIOp *>(&op) != nullptr ||
           dynamic_cast<const yir::MulIOp *>(&op) != nullptr ||
           dynamic_cast<const yir::ICmpOp *>(&op) != nullptr ||
           dynamic_cast<const yir::ToBoolOp *>(&op) != nullptr ||
           dynamic_cast<const yir::NotOp *>(&op) != nullptr;
}

void collect_uses_from_region(const yir::Region &region, UseCounts &uses) {
    for (const auto &op : region.operations()) {
        for (auto *operand : op->operands()) {
            if (operand != nullptr) {
                ++uses[operand];
            }
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            collect_uses_from_region(if_op->then_region(), uses);
            if (if_op->has_else()) {
                collect_uses_from_region(if_op->else_region(), uses);
            }
        } else if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
            collect_uses_from_region(while_op->cond_region(), uses);
            collect_uses_from_region(while_op->body_region(), uses);
        } else if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
            collect_uses_from_region(for_op->body_region(), uses);
        }
    }
}

void collect_uses(const yir::Module &module, UseCounts &uses) {
    for (const auto &function : module.functions()) {
        collect_uses_from_region(function->body(), uses);
    }
}

bool sweep_dead_ops(yir::Region &region, const UseCounts &uses, Stats &stats) {
    bool changed = false;
    auto &ops = region.operations();
    for (std::size_t i = 0; i < ops.size();) {
        if (auto *if_op = dynamic_cast<yir::IfOp *>(ops[i].get())) {
            changed = sweep_dead_ops(if_op->then_region(), uses, stats) || changed;
            if (if_op->has_else()) {
                changed = sweep_dead_ops(if_op->else_region(), uses, stats) || changed;
            }
        } else if (auto *while_op = dynamic_cast<yir::WhileOp *>(ops[i].get())) {
            changed = sweep_dead_ops(while_op->cond_region(), uses, stats) || changed;
            changed = sweep_dead_ops(while_op->body_region(), uses, stats) || changed;
        } else if (auto *for_op = dynamic_cast<yir::ForOp *>(ops[i].get())) {
            changed = sweep_dead_ops(for_op->body_region(), uses, stats) || changed;
        }

        auto *result = ops[i]->result();
        auto found = result == nullptr ? uses.end() : uses.find(result);
        const bool unused = result != nullptr && found == uses.end();
        if (unused && is_removable_pure_op(*ops[i])) {
            ops.erase(ops.begin() + static_cast<std::ptrdiff_t>(i));
            ++stats.dead_ops_removed;
            changed = true;
            continue;
        }
        ++i;
    }
    return changed;
}

std::string verify_message(const yir::VerifyResult &result) {
    if (result.errors.empty()) {
        return "YIR verification failed after YIRPolyhedralCanonicalizePass";
    }
    std::ostringstream oss;
    oss << "YIR verification failed after YIRPolyhedralCanonicalizePass: ";
    for (std::size_t i = 0; i < result.errors.size(); ++i) {
        if (i != 0) {
            oss << "; ";
        }
        oss << result.errors[i];
    }
    return oss.str();
}

class Canonicalizer final {
  public:
    explicit Canonicalizer(Stats &stats) : stats_(stats), materializer_(stats) {
    }

    bool run(yir::Module &module) {
        for (auto &function : module.functions()) {
            normalize_loops(function->body());
        }
        for (auto &function : module.functions()) {
            canonicalize_affine_operands(function->body());
        }
        for (auto &function : module.functions()) {
            hoist_loop_invariants(function->body());
        }
        eliminate_dead_affine_ops(module);
        return stats_.changed();
    }

  private:
    void normalize_loops(yir::Region &region) {
        auto &ops = region.operations();
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (auto *if_op = dynamic_cast<yir::IfOp *>(ops[i].get())) {
                normalize_loops(if_op->then_region());
                if (if_op->has_else()) {
                    normalize_loops(if_op->else_region());
                }
            } else if (auto *while_op = dynamic_cast<yir::WhileOp *>(ops[i].get())) {
                normalize_loops(while_op->cond_region());
                normalize_loops(while_op->body_region());
                try_demote_continue_guard(*while_op);
                try_normalize_while(ops, i);
            } else if (auto *for_op = dynamic_cast<yir::ForOp *>(ops[i].get())) {
                normalize_loops(for_op->body_region());
            }
        }
    }

    bool try_demote_continue_guard(yir::WhileOp &while_op) {
        auto &body_ops = while_op.body_region().operations();
        if (body_ops.size() < 3) {
            return false;
        }

        // Find the leading IfOp, allowing only pure prefix ops before it (typically
        // the ICmp that produced the guard condition and any constants it consumes).
        std::size_t guard_index = 0;
        while (guard_index < body_ops.size()) {
            if (dynamic_cast<yir::IfOp *>(body_ops[guard_index].get()) != nullptr) {
                break;
            }
            if (!is_condition_pure_op(*body_ops[guard_index])) {
                return false;
            }
            ++guard_index;
        }
        if (guard_index >= body_ops.size()) {
            return false;
        }
        auto *guard_if = dynamic_cast<yir::IfOp *>(body_ops[guard_index].get());
        if (guard_if == nullptr || guard_if->has_else()) {
            return false;
        }

        auto *guard_icmp =
            guard_if->condition() == nullptr
                ? nullptr
                : dynamic_cast<yir::ICmpOp *>(guard_if->condition()->defining_op());
        if (guard_icmp == nullptr) {
            return false;
        }

        // The guard then-arm must contain exactly: addi iv,step ; assign iv ; continue,
        // optionally preceded by pure ops (e.g. a const.i32 step) that the latch uses.
        auto &then_ops = guard_if->then_region().operations();
        if (then_ops.size() < 3) {
            return false;
        }
        if (dynamic_cast<yir::ContinueOp *>(then_ops.back().get()) == nullptr) {
            return false;
        }
        const std::size_t then_count = then_ops.size();
        auto *then_assign = dynamic_cast<yir::AssignOp *>(then_ops[then_count - 2].get());
        auto *then_add = dynamic_cast<yir::AddIOp *>(then_ops[then_count - 3].get());
        if (then_assign == nullptr || then_add == nullptr ||
            then_assign->value()->defining_op() != then_add) {
            return false;
        }
        for (std::size_t k = 0; k + 3 < then_count; ++k) {
            if (!is_condition_pure_op(*then_ops[k])) {
                return false;
            }
        }
        yir::Value *iv = then_assign->target();
        if (iv == nullptr) {
            return false;
        }
        yir::Value *guard_step = nullptr;
        if (then_add->lhs() == iv) {
            guard_step = then_add->rhs();
        } else if (then_add->rhs() == iv) {
            guard_step = then_add->lhs();
        } else {
            return false;
        }

        // The body must end in the same shape latch on the same iv.
        LatchInfo body_latch;
        if (!find_positive_latch_update(while_op.body_region(), iv, body_latch)) {
            return false;
        }

        // Step values must be the same constant.
        AffineParser parser({&while_op.cond_region(), &while_op.body_region()});
        auto guard_step_expr = parser.parse(guard_step);
        auto body_step_expr = parser.parse(body_latch.step);
        if (!guard_step_expr.valid || !body_step_expr.valid ||
            !guard_step_expr.terms.empty() || !body_step_expr.terms.empty() ||
            guard_step_expr.constant <= 0 ||
            guard_step_expr.constant != body_step_expr.constant) {
            return false;
        }

        // Reject if the residual body still has any abrupt control flow we cannot
        // safely fold under a single residual `if` (a break or continue would change
        // semantics if it ends up nested).
        for (std::size_t i = guard_index + 1; i < body_ops.size(); ++i) {
            auto *op = body_ops[i].get();
            if (op == body_latch.step_op || op == body_latch.assign) {
                continue;
            }
            if (dynamic_cast<yir::BreakOp *>(op) != nullptr ||
                dynamic_cast<yir::ContinueOp *>(op) != nullptr) {
                return false;
            }
        }

        using Predicate = yir::ICmpOp::Predicate;
        const Predicate inverted = [](Predicate p) {
            switch (p) {
            case Predicate::Eq:
                return Predicate::Ne;
            case Predicate::Ne:
                return Predicate::Eq;
            case Predicate::Lt:
                return Predicate::Ge;
            case Predicate::Le:
                return Predicate::Gt;
            case Predicate::Gt:
                return Predicate::Le;
            case Predicate::Ge:
                return Predicate::Lt;
            }
            return p;
        }(guard_icmp->predicate());

        std::string flip_name = std::string("poly.guard.flip") +
                                std::to_string(stats_.guards_demoted);
        auto inverted_icmp = std::make_unique<yir::ICmpOp>(
            inverted, guard_icmp->lhs(), guard_icmp->rhs(), std::move(flip_name));
        inverted_icmp->set_parent(&while_op.body_region());
        auto *inverted_value = inverted_icmp->result();

        auto residual_if = std::make_unique<yir::IfOp>(inverted_value);
        residual_if->set_parent(&while_op.body_region());
        auto &residual_then = residual_if->then_region();

        OpList replacement;
        replacement.reserve(body_ops.size() + 2);
        // Preserve any pure prefix ops (e.g. the original icmp + constants), then
        // emit the inverted icmp + residual if.
        for (std::size_t i = 0; i < guard_index; ++i) {
            body_ops[i]->set_parent(&while_op.body_region());
            replacement.push_back(std::move(body_ops[i]));
        }
        replacement.push_back(std::move(inverted_icmp));
        replacement.push_back(std::move(residual_if));

        // Move every body op except the trailing latch into the residual then-arm,
        // skipping the original guard if (which we are dropping in favour of its
        // residual). The trailing latch ops stay at the body level so the existing
        // while->for canonicalizer recognizes them.
        auto *residual_if_raw = dynamic_cast<yir::IfOp *>(replacement.back().get());
        for (std::size_t i = guard_index + 1; i < body_ops.size(); ++i) {
            auto &op = body_ops[i];
            if (op.get() == body_latch.step_op || op.get() == body_latch.assign) {
                op->set_parent(&while_op.body_region());
                replacement.push_back(std::move(op));
                continue;
            }
            op->set_parent(&residual_if_raw->then_region());
            residual_if_raw->then_region().operations().push_back(std::move(op));
        }

        body_ops = std::move(replacement);
        ++stats_.guards_demoted;
        (void)residual_then;
        return true;
    }

    bool try_normalize_while(OpList &ops, std::size_t &index) {
        auto *while_op = dynamic_cast<yir::WhileOp *>(ops[index].get());
        if (while_op == nullptr || contains_abrupt_loop_control(while_op->body_region()) ||
            !condition_region_is_side_effect_free(while_op->cond_region())) {
            return false;
        }
        auto *icmp = condition_compare(*while_op);
        if (icmp == nullptr) {
            return false;
        }

        yir::Value *candidate_iv = nullptr;
        LatchInfo latch;
        for (auto *candidate : {icmp->lhs(), icmp->rhs()}) {
            LatchInfo candidate_latch;
            if (find_positive_latch_update(while_op->body_region(), candidate, candidate_latch)) {
                candidate_iv = candidate;
                latch = candidate_latch;
                break;
            }
        }
        if (candidate_iv == nullptr) {
            return false;
        }

        LoopCondition condition;
        if (!match_forward_upper_bound(*icmp, candidate_iv, condition)) {
            return false;
        }

        yir::Value *lower_bound = nullptr;
        if (!find_loop_lower_bound(ops, index, candidate_iv, lower_bound)) {
            return false;
        }

        AffineParser parser(RegionSet{&while_op->cond_region(), &while_op->body_region()});
        auto lower_expr = parser.parse(lower_bound);
        auto upper_expr = parser.parse(condition.upper_bound);
        auto step_expr = parser.parse(latch.step);
        if (!step_expr.valid || !step_expr.terms.empty() || step_expr.constant <= 0 ||
            !expr_is_invariant_in_region(lower_expr, while_op->body_region(), candidate_iv) ||
            !expr_is_invariant_in_region(upper_expr, while_op->body_region(), candidate_iv)) {
            return false;
        }
        if (condition.inclusive) {
            ++upper_expr.constant;
        }

        if (!expression_materializable(lower_expr) || !expression_materializable(upper_expr) ||
            !expression_materializable(step_expr)) {
            return false;
        }

        auto *lower = materializer_.materialize(lower_expr, ops, index);
        auto *upper = materializer_.materialize(upper_expr, ops, index);
        auto *step = materializer_.materialize(step_expr, ops, index);
        if (lower == nullptr || upper == nullptr || step == nullptr) {
            return false;
        }

        auto for_op = std::make_unique<yir::ForOp>(candidate_iv, lower, upper, step);
        for_op->set_parent(ops[index]->parent());
        move_canonical_loop_body(*for_op, *while_op, latch);

        ops[index] = std::move(for_op);
        ++stats_.loops_normalized;
        return true;
    }

    void canonicalize_affine_operands(yir::Region &region) {
        auto &ops = region.operations();
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (auto *assign = dynamic_cast<yir::AssignOp *>(ops[i].get())) {
                canonicalize_operand(ops, i, 1);
                (void)assign;
            } else if (auto *var = dynamic_cast<yir::VarOp *>(ops[i].get())) {
                if (var->has_initializer()) {
                    canonicalize_operand(ops, i, 0);
                }
            } else if (auto *load = dynamic_cast<yir::ArrayLoadOp *>(ops[i].get())) {
                (void)load;
                for (std::size_t operand = 1; operand < ops[i]->operands().size(); ++operand) {
                    canonicalize_operand(ops, i, operand);
                }
            } else if (auto *store = dynamic_cast<yir::ArrayStoreOp *>(ops[i].get())) {
                (void)store;
                for (std::size_t operand = 2; operand < ops[i]->operands().size(); ++operand) {
                    canonicalize_operand(ops, i, operand);
                }
            } else if (auto *elem_addr = dynamic_cast<yir::ElemAddrOp *>(ops[i].get())) {
                (void)elem_addr;
                for (std::size_t operand = 1; operand < ops[i]->operands().size(); ++operand) {
                    canonicalize_operand(ops, i, operand);
                }
            } else if (dynamic_cast<yir::ICmpOp *>(ops[i].get()) != nullptr) {
                canonicalize_operand(ops, i, 0);
                canonicalize_operand(ops, i, 1);
            } else if (dynamic_cast<yir::ReturnOp *>(ops[i].get()) != nullptr &&
                       !ops[i]->operands().empty()) {
                canonicalize_operand(ops, i, 0);
            }

            if (auto *if_op = dynamic_cast<yir::IfOp *>(ops[i].get())) {
                canonicalize_affine_operands(if_op->then_region());
                if (if_op->has_else()) {
                    canonicalize_affine_operands(if_op->else_region());
                }
            } else if (auto *while_op = dynamic_cast<yir::WhileOp *>(ops[i].get())) {
                canonicalize_affine_operands(while_op->cond_region());
                canonicalize_affine_operands(while_op->body_region());
            } else if (auto *for_op = dynamic_cast<yir::ForOp *>(ops[i].get())) {
                canonicalize_operand(ops, i, 1);
                canonicalize_operand(ops, i, 2);
                canonicalize_operand(ops, i, 3);
                canonicalize_affine_operands(for_op->body_region());
            }
        }
    }

    bool canonicalize_operand(OpList &ops, std::size_t &index, std::size_t operand_index) {
        if (operand_index >= ops[index]->operands().size()) {
            return false;
        }
        auto *old_value = ops[index]->operands()[operand_index];
        AffineParser parser;
        auto expr = parser.parse(old_value);
        if (!expr.valid || is_structurally_canonical(old_value, expr)) {
            return false;
        }
        auto *new_value = materializer_.materialize(expr, ops, index);
        if (new_value == nullptr || new_value == old_value) {
            return false;
        }
        ops[index]->operands()[operand_index] = new_value;
        ++stats_.affine_operands_canonicalized;
        return true;
    }

    void hoist_loop_invariants(yir::Region &region) {
        auto &ops = region.operations();
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (auto *if_op = dynamic_cast<yir::IfOp *>(ops[i].get())) {
                hoist_loop_invariants(if_op->then_region());
                if (if_op->has_else()) {
                    hoist_loop_invariants(if_op->else_region());
                }
            } else if (auto *while_op = dynamic_cast<yir::WhileOp *>(ops[i].get())) {
                hoist_loop_invariants(while_op->cond_region());
                hoist_loop_invariants(while_op->body_region());
                hoist_from_loop_body(ops, i, while_op->body_region(), nullptr);
            } else if (auto *for_op = dynamic_cast<yir::ForOp *>(ops[i].get())) {
                hoist_loop_invariants(for_op->body_region());
                hoist_from_loop_body(ops, i, for_op->body_region(), for_op->induction_var());
            }
        }
    }

    bool hoist_from_loop_body(OpList &parent_ops, std::size_t &loop_index, yir::Region &body,
                              const yir::Value *induction_var) {
        ValueSet assigned;
        collect_assigned(body, assigned);
        if (induction_var != nullptr) {
            assigned.insert(induction_var);
        }

        ValueSet defined_in_loop;
        for (const auto &op : body.operations()) {
            if (op->result() != nullptr) {
                defined_in_loop.insert(op->result());
            }
        }

        const bool has_call = region_has_call(body);
        bool changed = false;
        ValueSet hoisted;
        auto &body_ops = body.operations();
        for (std::size_t i = 0; i < body_ops.size();) {
            auto &op = body_ops[i];
            if ((has_call && !is_constant_like(*op)) || !is_affine_hoist_candidate(*op) ||
                !operands_invariant(*op, assigned, defined_in_loop, hoisted)) {
                ++i;
                continue;
            }

            hoisted.insert(op->result());
            op->set_parent(parent_ops[loop_index]->parent());
            parent_ops.insert(parent_ops.begin() + static_cast<std::ptrdiff_t>(loop_index),
                              std::move(op));
            ++loop_index;
            body_ops.erase(body_ops.begin() + static_cast<std::ptrdiff_t>(i));
            ++stats_.invariants_hoisted;
            changed = true;
        }
        return changed;
    }

    void eliminate_dead_affine_ops(yir::Module &module) {
        bool changed = false;
        do {
            UseCounts uses;
            collect_uses(module, uses);
            changed = false;
            for (auto &function : module.functions()) {
                changed = sweep_dead_ops(function->body(), uses, stats_) || changed;
            }
        } while (changed);
    }

    Stats &stats_;
    AffineMaterializer materializer_;
};

class PolyhedralInfoCollector final {
  public:
    explicit PolyhedralInfoCollector(Stats &stats) : stats_(stats) {
    }

    YIRPolyhedralCanonicalInfo collect(const yir::Module &module) {
        YIRPolyhedralCanonicalInfo info;
        std::vector<const yir::Value *> dimensions;
        for (const auto &function : module.functions()) {
            scan_region(function->body(), dimensions, info);
        }
        return info;
    }

  private:
    void scan_region(const yir::Region &region, std::vector<const yir::Value *> &dimensions,
                     YIRPolyhedralCanonicalInfo &info) {
        for (const auto &op : region.operations()) {
            if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
                scan_region(if_op->then_region(), dimensions, info);
                if (if_op->has_else()) {
                    scan_region(if_op->else_region(), dimensions, info);
                }
                continue;
            }
            if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
                scan_region(while_op->cond_region(), dimensions, info);
                scan_region(while_op->body_region(), dimensions, info);
                continue;
            }
            if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
                dimensions.push_back(for_op->induction_var());
                YIRPolyhedralLoopInfo loop_info;
                loop_info.loop = for_op;
                loop_info.dimensions = dimensions;
                collect_loop_symbols(*for_op, loop_info.symbols);
                stats_.symbols_detected += loop_info.symbols.size();
                info.loops.push_back(std::move(loop_info));
                scan_region(for_op->body_region(), dimensions, info);
                dimensions.pop_back();
            }
        }
    }

    void collect_loop_symbols(const yir::ForOp &for_op,
                              std::vector<const yir::Value *> &symbols) const {
        ValueSet assigned;
        collect_assigned(for_op.body_region(), assigned);
        assigned.insert(for_op.induction_var());

        ValueSet seen;
        collect_affine_symbols(for_op.lower_bound(), assigned, seen, symbols);
        collect_affine_symbols(for_op.upper_bound(), assigned, seen, symbols);
        collect_affine_symbols(for_op.step(), assigned, seen, symbols);
    }

    void collect_affine_symbols(yir::Value *value, const ValueSet &assigned, ValueSet &seen,
                                std::vector<const yir::Value *> &symbols) const {
        AffineParser parser;
        auto expr = parser.parse(value);
        if (!expr.valid) {
            return;
        }
        for (const auto &term : expr.terms) {
            if (assigned.find(term.value) != assigned.end()) {
                continue;
            }
            if (seen.insert(term.value).second) {
                symbols.push_back(term.value);
            }
        }
    }

    Stats &stats_;
};

} // namespace

std::string_view YIRPolyhedralCanonicalizePass::name() const {
    return "YIRPolyhedralCanonicalizePass";
}

PassKind YIRPolyhedralCanonicalizePass::kind() const {
    return PassKind::Transform;
}

PassResult YIRPolyhedralCanonicalizePass::run(PassContext &context) {
    auto *artifact = context.get_artifact<std::unique_ptr<yir::Module>>(ASTToYIRPass::kArtifactKey);
    if (artifact == nullptr || *artifact == nullptr) {
        return PassResult::fail("YIRPolyhedralCanonicalizePass requires YIR module in pass context");
    }

    Stats stats;
    Canonicalizer canonicalizer(stats);
    canonicalizer.run(**artifact);

    auto verify = yir::verify_high_level_yir(**artifact);
    if (!verify.success) {
        return PassResult::fail(verify_message(verify));
    }

    PolyhedralInfoCollector info_collector(stats);
    auto info = info_collector.collect(**artifact);
    context.set_artifact<YIRPolyhedralCanonicalInfo>(std::string(kArtifactKey), std::move(info));
    context.erase_artifact(typeid(yir::LoopAnalysis).name());

    return PassResult::ok(stats.changed(), stats.message());
}

} // namespace pass
