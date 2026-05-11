#include "../../include/pass/YIRLoopCanonicalizePass.h"

#include "../../include/pass/ASTToYIRPass.h"
#include "../../include/yir/YIR.h"

#include <algorithm>
#include <memory>

namespace pass {
namespace {

using OpList = yir::Region::OpList;

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

const yir::CondOp *terminating_cond(const yir::Region &region) {
    if (region.operations().empty()) {
        return nullptr;
    }
    return dynamic_cast<const yir::CondOp *>(region.operations().back().get());
}

bool condition_region_is_side_effect_free(const yir::Region &region) {
    for (const auto &op : region.operations()) {
        if (dynamic_cast<const yir::ConstI32Op *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ICmpOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::CondOp *>(op.get()) != nullptr) {
            continue;
        }
        return false;
    }
    return true;
}

const yir::ICmpOp *condition_compare(const yir::WhileOp &op) {
    auto *cond = terminating_cond(op.cond_region());
    if (cond == nullptr || cond->condition() == nullptr) {
        return nullptr;
    }
    return dynamic_cast<const yir::ICmpOp *>(cond->condition()->defining_op());
}

bool parse_positive_latch(const yir::AssignOp &assign, yir::Value *iv, yir::Value *&step,
                          const yir::Operation *&step_op) {
    auto *add = dynamic_cast<const yir::AddIOp *>(assign.value()->defining_op());
    if (add == nullptr) {
        return false;
    }
    if (add->lhs() == iv) {
        step = add->rhs();
        step_op = add;
        return true;
    }
    if (add->rhs() == iv) {
        step = add->lhs();
        step_op = add;
        return true;
    }
    return false;
}

bool find_latch_assignment(const yir::Region &body, yir::Value *iv, const yir::AssignOp *&assign,
                           yir::Value *&step, const yir::Operation *&step_op) {
    if (body.operations().empty()) {
        return false;
    }
    assign = dynamic_cast<const yir::AssignOp *>(body.operations().back().get());
    if (assign == nullptr || assign->target() != iv) {
        return false;
    }
    return parse_positive_latch(*assign, iv, step, step_op);
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

yir::Value *materialize_loop_operand(yir::Value *value, OpList &ops, std::size_t &insert_pos) {
    auto *constant = value == nullptr ? nullptr : dynamic_cast<yir::ConstI32Op *>(value->defining_op());
    if (constant == nullptr || constant->parent() == nullptr) {
        return value;
    }
    if (constant->parent() == ops[insert_pos]->parent()) {
        auto found = std::find_if(ops.begin(), ops.begin() + static_cast<std::ptrdiff_t>(insert_pos),
                                  [constant](const std::unique_ptr<yir::Operation> &op) {
                                      return op.get() == constant;
                                  });
        if (found != ops.begin() + static_cast<std::ptrdiff_t>(insert_pos)) {
            return value;
        }
    }

    auto clone = std::make_unique<yir::ConstI32Op>(constant->value(), value->name());
    auto *result = clone->result();
    clone->set_parent(const_cast<yir::Region *>(ops[insert_pos]->parent()));
    ops.insert(ops.begin() + static_cast<std::ptrdiff_t>(insert_pos), std::move(clone));
    ++insert_pos;
    return result;
}

void move_canonical_body(yir::ForOp &for_op, yir::WhileOp &while_op,
                         const yir::AssignOp *latch_assign, const yir::Operation *step_op,
                         const yir::Value *step) {
    auto &src = while_op.body_region().operations();
    auto &dst = for_op.body_region().operations();
    for (auto &op : src) {
        if (op.get() == latch_assign || op.get() == step_op ||
            (step != nullptr && op.get() == step->defining_op())) {
            continue;
        }
        op->set_parent(&for_op.body_region());
        dst.push_back(std::move(op));
    }
    src.clear();
}

bool try_canonicalize_while(OpList &ops, std::size_t &index) {
    if (index == 0) {
        return false;
    }
    auto *while_op = dynamic_cast<yir::WhileOp *>(ops[index].get());
    if (while_op == nullptr || contains_abrupt_loop_control(while_op->body_region()) ||
        !condition_region_is_side_effect_free(while_op->cond_region())) {
        return false;
    }

    auto *icmp = condition_compare(*while_op);
    if (icmp == nullptr || icmp->predicate() != yir::ICmpOp::Predicate::Lt) {
        return false;
    }
    yir::Value *iv = icmp->lhs();
    yir::Value *lower_bound = nullptr;
    std::size_t init_index = index;
    bool erase_init_assign = false;
    for (std::size_t cursor = index; cursor > 0; --cursor) {
        std::size_t candidate = cursor - 1;
        if (auto *assign = dynamic_cast<yir::AssignOp *>(ops[candidate].get())) {
            if (assign->target() == iv) {
                lower_bound = assign->value();
                init_index = candidate;
                erase_init_assign = candidate + 1 == index;
                break;
            }
        } else if (auto *var = dynamic_cast<yir::VarOp *>(ops[candidate].get())) {
            if (var->result() == iv && var->has_initializer()) {
                lower_bound = var->initializer();
                init_index = candidate;
                break;
            }
        }
        if (op_may_assign_value(*ops[candidate], iv)) {
            lower_bound = iv;
            init_index = candidate;
            break;
        }
    }
    if (lower_bound == nullptr) {
        return false;
    }

    const yir::AssignOp *latch_assign = nullptr;
    yir::Value *step = nullptr;
    const yir::Operation *step_op = nullptr;
    if (!find_latch_assignment(while_op->body_region(), iv, latch_assign, step, step_op)) {
        return false;
    }
    yir::Value *body_step_value = step;

    std::size_t insert_pos = index;
    auto *lower = materialize_loop_operand(lower_bound, ops, insert_pos);
    auto *upper = materialize_loop_operand(icmp->rhs(), ops, insert_pos);
    step = materialize_loop_operand(step, ops, insert_pos);
    index = insert_pos;

    auto for_op = std::make_unique<yir::ForOp>(iv, lower, upper, step);
    for_op->set_parent(ops[index]->parent());
    move_canonical_body(*for_op, *while_op, latch_assign, step_op, body_step_value);

    ops[index] = std::move(for_op);
    if (erase_init_assign) {
        ops.erase(ops.begin() + static_cast<std::ptrdiff_t>(init_index));
        if (init_index < index) {
            --index;
        }
    }
    return true;
}

class Canonicalizer final {
  public:
    bool run(yir::Module &module) {
        changed_ = false;
        for (auto &function : module.functions()) {
            canonicalize_region(function->body());
        }
        return changed_;
    }

  private:
    void canonicalize_region(yir::Region &region) {
        auto &ops = region.operations();
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (auto *if_op = dynamic_cast<yir::IfOp *>(ops[i].get())) {
                canonicalize_region(if_op->then_region());
                if (if_op->has_else()) {
                    canonicalize_region(if_op->else_region());
                }
            } else if (auto *while_op = dynamic_cast<yir::WhileOp *>(ops[i].get())) {
                canonicalize_region(while_op->cond_region());
                canonicalize_region(while_op->body_region());
                changed_ = try_canonicalize_while(ops, i) || changed_;
            } else if (auto *for_op = dynamic_cast<yir::ForOp *>(ops[i].get())) {
                canonicalize_region(for_op->body_region());
            }
        }
    }

    bool changed_ = false;
};

} // namespace

std::string_view YIRLoopCanonicalizePass::name() const {
    return "YIRLoopCanonicalizePass";
}

PassKind YIRLoopCanonicalizePass::kind() const {
    return PassKind::Transform;
}

PassResult YIRLoopCanonicalizePass::run(PassContext &context) {
    auto *artifact = context.get_artifact<std::unique_ptr<yir::Module>>(ASTToYIRPass::kArtifactKey);
    if (artifact == nullptr || *artifact == nullptr) {
        return PassResult::fail("YIRLoopCanonicalizePass requires YIR module in pass context");
    }

    Canonicalizer canonicalizer;
    return PassResult::ok(canonicalizer.run(**artifact));
}

} // namespace pass
