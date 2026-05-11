#include "../../include/pass/YIRLoopOptimizationPass.h"

#include "../../include/pass/ASTToYIRPass.h"
#include "../../include/yir/YIR.h"
#include "../../include/yir/YIRLoopAnalysis.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace pass {
namespace {

using ValueMap = std::unordered_map<const yir::Value *, yir::Value *>;
using ValueSet = std::unordered_set<const yir::Value *>;
using OpList = yir::Region::OpList;

yir::Value *map_value(yir::Value *value, const ValueMap &map) {
    auto found = map.find(value);
    return found == map.end() ? value : found->second;
}

std::vector<yir::Value *> map_values(const std::vector<yir::Value *> &values, const ValueMap &map) {
    std::vector<yir::Value *> out;
    out.reserve(values.size());
    for (auto *value : values) {
        out.push_back(map_value(value, map));
    }
    return out;
}

std::unique_ptr<yir::Operation> clone_simple_op(const yir::Operation &op, const ValueMap &map) {
    if (auto *constant = dynamic_cast<const yir::ConstI32Op *>(&op)) {
        return std::make_unique<yir::ConstI32Op>(constant->value(), op.result()->name());
    }
    if (auto *constant = dynamic_cast<const yir::ConstF32Op *>(&op)) {
        return std::make_unique<yir::ConstF32Op>(constant->value(), op.result()->name());
    }
    if (auto *constant = dynamic_cast<const yir::ConstBoolOp *>(&op)) {
        return std::make_unique<yir::ConstBoolOp>(constant->value(), op.result()->name());
    }
    if (dynamic_cast<const yir::ZeroOp *>(&op) != nullptr) {
        return std::make_unique<yir::ZeroOp>(op.result()->type(), op.result()->name());
    }
    if (auto *var = dynamic_cast<const yir::VarOp *>(&op)) {
        return std::make_unique<yir::VarOp>(op.result()->type(),
                                            var->has_initializer()
                                                ? map_value(var->initializer(), map)
                                                : nullptr,
                                            op.result()->name());
    }
    if (auto *assign = dynamic_cast<const yir::AssignOp *>(&op)) {
        return std::make_unique<yir::AssignOp>(map_value(assign->target(), map),
                                               map_value(assign->value(), map));
    }
    if (auto *load = dynamic_cast<const yir::ArrayLoadOp *>(&op)) {
        return std::make_unique<yir::ArrayLoadOp>(map_value(load->array(), map),
                                                  map_values(load->indices(), map),
                                                  op.result()->type(), op.result()->name());
    }
    if (auto *store = dynamic_cast<const yir::ArrayStoreOp *>(&op)) {
        return std::make_unique<yir::ArrayStoreOp>(map_value(store->value(), map),
                                                   map_value(store->array(), map),
                                                   map_values(store->indices(), map));
    }
    if (auto *binary = dynamic_cast<const yir::BinaryOpBase *>(&op)) {
        auto *lhs = map_value(binary->lhs(), map);
        auto *rhs = map_value(binary->rhs(), map);
        if (dynamic_cast<const yir::AddIOp *>(&op) != nullptr) {
            return std::make_unique<yir::AddIOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::SubIOp *>(&op) != nullptr) {
            return std::make_unique<yir::SubIOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::MulIOp *>(&op) != nullptr) {
            return std::make_unique<yir::MulIOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::DivSIOp *>(&op) != nullptr) {
            return std::make_unique<yir::DivSIOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::RemSIOp *>(&op) != nullptr) {
            return std::make_unique<yir::RemSIOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::AddFOp *>(&op) != nullptr) {
            return std::make_unique<yir::AddFOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::SubFOp *>(&op) != nullptr) {
            return std::make_unique<yir::SubFOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::MulFOp *>(&op) != nullptr) {
            return std::make_unique<yir::MulFOp>(lhs, rhs, op.result()->name());
        }
        if (dynamic_cast<const yir::DivFOp *>(&op) != nullptr) {
            return std::make_unique<yir::DivFOp>(lhs, rhs, op.result()->name());
        }
        if (auto *icmp = dynamic_cast<const yir::ICmpOp *>(&op)) {
            return std::make_unique<yir::ICmpOp>(icmp->predicate(), lhs, rhs, op.result()->name());
        }
        if (auto *fcmp = dynamic_cast<const yir::FCmpOp *>(&op)) {
            return std::make_unique<yir::FCmpOp>(fcmp->predicate(), lhs, rhs, op.result()->name());
        }
    }
    if (dynamic_cast<const yir::ZExtI1ToI32Op *>(&op) != nullptr) {
        return std::make_unique<yir::ZExtI1ToI32Op>(map_value(op.operands()[0], map),
                                                    op.result()->name());
    }
    if (dynamic_cast<const yir::TruncI32ToI1Op *>(&op) != nullptr) {
        return std::make_unique<yir::TruncI32ToI1Op>(map_value(op.operands()[0], map),
                                                     op.result()->name());
    }
    if (dynamic_cast<const yir::SIToFPOp *>(&op) != nullptr) {
        return std::make_unique<yir::SIToFPOp>(map_value(op.operands()[0], map),
                                               op.result()->name());
    }
    if (dynamic_cast<const yir::FPToSIOp *>(&op) != nullptr) {
        return std::make_unique<yir::FPToSIOp>(map_value(op.operands()[0], map),
                                               op.result()->name());
    }
    if (dynamic_cast<const yir::ToBoolOp *>(&op) != nullptr) {
        return std::make_unique<yir::ToBoolOp>(map_value(op.operands()[0], map),
                                               op.result()->name());
    }
    if (dynamic_cast<const yir::NotOp *>(&op) != nullptr) {
        return std::make_unique<yir::NotOp>(map_value(op.operands()[0], map),
                                            op.result()->name());
    }
    if (auto *call = dynamic_cast<const yir::CallOp *>(&op)) {
        return std::make_unique<yir::CallOp>(call->callee(), map_values(call->args(), map),
                                             op.result() == nullptr ? yir::Type::get_void()
                                                                    : op.result()->type(),
                                             op.result() == nullptr ? "" : op.result()->name());
    }
    if (auto *ret = dynamic_cast<const yir::ReturnOp *>(&op)) {
        return std::make_unique<yir::ReturnOp>(ret->has_value() ? map_value(ret->value(), map)
                                                                : nullptr);
    }
    return nullptr;
}

bool is_unroll_safe(const yir::Region &region) {
    for (const auto &op : region.operations()) {
        if (dynamic_cast<const yir::IfOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::WhileOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ForOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::BreakOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ContinueOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ArrayInitOp *>(op.get()) != nullptr) {
            return false;
        }
        if (clone_simple_op(*op, {}) == nullptr) {
            return false;
        }
    }
    return true;
}

std::size_t operation_count(const yir::Region &region) {
    std::size_t count = 0;
    for (const auto &op : region.operations()) {
        ++count;
    }
    return count;
}

bool const_i32(const yir::Value *value, std::int64_t &out) {
    auto *constant = value == nullptr ? nullptr : dynamic_cast<const yir::ConstI32Op *>(value->defining_op());
    if (constant == nullptr) {
        return false;
    }
    out = constant->value();
    return true;
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

bool is_licm_candidate(const yir::Operation &op) {
    if (op.result() == nullptr) {
        return false;
    }
    if (dynamic_cast<const yir::ConstI32Op *>(&op) != nullptr ||
        dynamic_cast<const yir::ConstF32Op *>(&op) != nullptr ||
        dynamic_cast<const yir::ConstBoolOp *>(&op) != nullptr ||
        dynamic_cast<const yir::ZeroOp *>(&op) != nullptr ||
        dynamic_cast<const yir::AddIOp *>(&op) != nullptr ||
        dynamic_cast<const yir::SubIOp *>(&op) != nullptr ||
        dynamic_cast<const yir::MulIOp *>(&op) != nullptr ||
        dynamic_cast<const yir::AddFOp *>(&op) != nullptr ||
        dynamic_cast<const yir::SubFOp *>(&op) != nullptr ||
        dynamic_cast<const yir::MulFOp *>(&op) != nullptr ||
        dynamic_cast<const yir::ICmpOp *>(&op) != nullptr ||
        dynamic_cast<const yir::FCmpOp *>(&op) != nullptr ||
        dynamic_cast<const yir::ZExtI1ToI32Op *>(&op) != nullptr ||
        dynamic_cast<const yir::TruncI32ToI1Op *>(&op) != nullptr ||
        dynamic_cast<const yir::SIToFPOp *>(&op) != nullptr ||
        dynamic_cast<const yir::FPToSIOp *>(&op) != nullptr ||
        dynamic_cast<const yir::ToBoolOp *>(&op) != nullptr ||
        dynamic_cast<const yir::NotOp *>(&op) != nullptr) {
        return true;
    }
    return false;
}

bool is_constant_like(const yir::Operation &op) {
    return dynamic_cast<const yir::ConstI32Op *>(&op) != nullptr ||
           dynamic_cast<const yir::ConstF32Op *>(&op) != nullptr ||
           dynamic_cast<const yir::ConstBoolOp *>(&op) != nullptr ||
           dynamic_cast<const yir::ZeroOp *>(&op) != nullptr;
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
            if (region_has_call(while_op->cond_region()) ||
                region_has_call(while_op->body_region())) {
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
        if (def != nullptr && defined_in_loop.find(def->result()) != defined_in_loop.end() &&
            hoisted.find(def->result()) == hoisted.end()) {
            return false;
        }
    }
    return true;
}

class Optimizer final {
  public:
    bool run(yir::Module &module) {
        changed_ = false;
        for (auto &function : module.functions()) {
            optimize_region(function->body());
        }
        return changed_;
    }

  private:
    void optimize_region(yir::Region &region) {
        auto &ops = region.operations();
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (auto *if_op = dynamic_cast<yir::IfOp *>(ops[i].get())) {
                optimize_region(if_op->then_region());
                if (if_op->has_else()) {
                    optimize_region(if_op->else_region());
                }
            } else if (auto *while_op = dynamic_cast<yir::WhileOp *>(ops[i].get())) {
                optimize_region(while_op->cond_region());
                optimize_region(while_op->body_region());
                changed_ = hoist_loop_invariants(ops, i, while_op->body_region(), nullptr) ||
                           changed_;
            } else if (auto *for_op = dynamic_cast<yir::ForOp *>(ops[i].get())) {
                if (try_unroll_for(ops, i, *for_op)) {
                    changed_ = true;
                    continue;
                }
                optimize_region(for_op->body_region());
                changed_ = hoist_loop_invariants(ops, i, for_op->body_region(),
                                                 for_op->induction_var()) ||
                           changed_;
            }
        }
    }

    bool hoist_loop_invariants(OpList &parent_ops, std::size_t loop_index, yir::Region &body,
                               const yir::Value *induction_var) {
        ValueSet assigned;
        collect_assigned(body, assigned);
        const bool has_call = region_has_call(body);
        if (induction_var != nullptr) {
            assigned.insert(induction_var);
        }

        ValueSet defined_in_loop;
        for (const auto &op : body.operations()) {
            if (op->result() != nullptr) {
                defined_in_loop.insert(op->result());
            }
        }

        bool changed = false;
        ValueSet hoisted;
        auto &body_ops = body.operations();
        for (std::size_t i = 0; i < body_ops.size();) {
            auto &op = body_ops[i];
            if ((has_call && !is_constant_like(*op)) || !is_licm_candidate(*op) ||
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
            changed = true;
        }
        return changed;
    }

    bool try_unroll_for(OpList &ops, std::size_t &index, const yir::ForOp &for_op) {
        std::int64_t lower = 0;
        std::int64_t upper = 0;
        std::int64_t step = 0;
        if (!const_i32(for_op.lower_bound(), lower) || !const_i32(for_op.upper_bound(), upper) ||
            !const_i32(for_op.step(), step) || step <= 0) {
            return false;
        }
        std::int64_t trip_count = upper <= lower ? 0 : (upper - lower + step - 1) / step;
        if (trip_count < 0 || trip_count > 4 || !is_unroll_safe(for_op.body_region())) {
            return false;
        }
        constexpr std::int64_t kMaxUnrolledOps = 48;
        const std::int64_t unrolled_ops =
            trip_count * static_cast<std::int64_t>(operation_count(for_op.body_region()) + 1);
        if (unrolled_ops > kMaxUnrolledOps) {
            return false;
        }

        std::vector<std::unique_ptr<yir::Operation>> clones;
        clones.reserve(static_cast<std::size_t>(trip_count) *
                       (for_op.body_region().operations().size() + 1));
        for (std::int64_t iter = 0; iter < trip_count; ++iter) {
            ValueMap map;
            auto iv_const = std::make_unique<yir::ConstI32Op>(
                static_cast<int>(lower + iter * step), for_op.induction_var()->name());
            iv_const->set_parent(ops[index]->parent());
            map[for_op.induction_var()] = iv_const->result();
            clones.push_back(std::move(iv_const));

            for (const auto &op : for_op.body_region().operations()) {
                auto clone = clone_simple_op(*op, map);
                if (clone == nullptr) {
                    return false;
                }
                clone->set_parent(ops[index]->parent());
                if (op->result() != nullptr && clone->result() != nullptr) {
                    map[op->result()] = clone->result();
                }
                clones.push_back(std::move(clone));
            }
        }

        auto insert = ops.begin() + static_cast<std::ptrdiff_t>(index);
        insert = ops.erase(insert);
        ops.insert(insert, std::make_move_iterator(clones.begin()),
                   std::make_move_iterator(clones.end()));
        if (clones.empty()) {
            if (index > 0) {
                --index;
            }
        } else {
            index += clones.size() - 1;
        }
        return true;
    }

    bool changed_ = false;
};

} // namespace

std::string_view YIRLoopOptimizationPass::name() const {
    return "YIRLoopOptimizationPass";
}

PassKind YIRLoopOptimizationPass::kind() const {
    return PassKind::Transform;
}

PassResult YIRLoopOptimizationPass::run(PassContext &context) {
    auto *artifact = context.get_artifact<std::unique_ptr<yir::Module>>(ASTToYIRPass::kArtifactKey);
    if (artifact == nullptr || *artifact == nullptr) {
        return PassResult::fail("YIRLoopOptimizationPass requires YIR module in pass context");
    }

    Optimizer optimizer;
    return PassResult::ok(optimizer.run(**artifact));
}

} // namespace pass
