#include "../../include/pass/YIRLoopOptimizationPass.h"

#include "../../include/pass/ASTToYIRPass.h"
#include "../../include/yir/YIR.h"
#include "../../include/yir/YIRLoopAnalysis.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

std::unique_ptr<yir::Operation> clone_simple_op(const yir::Operation &op, const ValueMap &map);

bool clone_region_into(const yir::Region &source, yir::Region &dest, ValueMap map) {
    for (const auto &op : source.operations()) {
        auto clone = clone_simple_op(*op, map);
        if (clone == nullptr) {
            return false;
        }
        clone->set_parent(&dest);
        if (op->result() != nullptr && clone->result() != nullptr) {
            map[op->result()] = clone->result();
        }
        dest.operations().push_back(std::move(clone));
    }
    return true;
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
    if (auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
        auto clone = std::make_unique<yir::IfOp>(map_value(if_op->condition(), map));
        if (!clone_region_into(if_op->then_region(), clone->then_region(), map)) {
            return nullptr;
        }
        if (if_op->has_else()) {
            clone->set_has_else(true);
            if (!clone_region_into(if_op->else_region(), clone->else_region(), map)) {
                return nullptr;
            }
        }
        return clone;
    }
    if (auto *ret = dynamic_cast<const yir::ReturnOp *>(&op)) {
        return std::make_unique<yir::ReturnOp>(ret->has_value() ? map_value(ret->value(), map)
                                                                : nullptr);
    }
    return nullptr;
}

bool is_unroll_safe(const yir::Region &region) {
    for (const auto &op : region.operations()) {
        if (dynamic_cast<const yir::WhileOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ForOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::BreakOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ContinueOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ReturnOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::CondOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ArrayInitOp *>(op.get()) != nullptr) {
            return false;
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            if (!is_unroll_safe(if_op->then_region()) ||
                (if_op->has_else() && !is_unroll_safe(if_op->else_region()))) {
                return false;
            }
        }
        if (clone_simple_op(*op, {}) == nullptr) {
            return false;
        }
    }
    return true;
}

bool region_has_if(const yir::Region &region) {
    for (const auto &op : region.operations()) {
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            return true;
        }
    }
    return false;
}

std::size_t operation_count(const yir::Region &region) {
    std::size_t count = 0;
    for (const auto &op : region.operations()) {
        ++count;
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            count += operation_count(if_op->then_region());
            if (if_op->has_else()) {
                count += operation_count(if_op->else_region());
            }
        }
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

bool parse_int64_literal(const std::string &literal, std::int64_t &out) {
    std::size_t begin = 0;
    while (begin < literal.size() &&
           std::isspace(static_cast<unsigned char>(literal[begin])) != 0) {
        ++begin;
    }
    std::size_t end = literal.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(literal[end - 1])) != 0) {
        --end;
    }
    if (begin == end || literal.compare(begin, end - begin, "zero") == 0) {
        out = 0;
        return begin != end;
    }

    std::string trimmed = literal.substr(begin, end - begin);
    char *parse_end = nullptr;
    errno = 0;
    const long long value = std::strtoll(trimmed.c_str(), &parse_end, 0);
    if (errno != 0 || parse_end == trimmed.c_str() || *parse_end != '\0') {
        return false;
    }
    out = static_cast<std::int64_t>(value);
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

bool region_has_control_flow(const yir::Region &region) {
    for (const auto &op : region.operations()) {
        if (dynamic_cast<const yir::IfOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::WhileOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ForOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::BreakOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ContinueOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ReturnOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::CondOp *>(op.get()) != nullptr) {
            return true;
        }
    }
    return false;
}

bool region_is_straight_line_cloneable(const yir::Region &region) {
    if (region_has_control_flow(region) || region_has_call(region)) {
        return false;
    }
    for (const auto &op : region.operations()) {
        if (dynamic_cast<const yir::ArrayInitOp *>(op.get()) != nullptr ||
            clone_simple_op(*op, {}) == nullptr) {
            return false;
        }
    }
    return true;
}

bool dependence_allows_reorder(const yir::LoopSummary *summary) {
    if (summary == nullptr) {
        return false;
    }
    for (const auto &dep : summary->dependencies) {
        if (dep.kind == yir::DependenceKind::Unknown ||
            dep.kind == yir::DependenceKind::LoopCarriedPossible) {
            return false;
        }
    }
    return true;
}

bool has_ranked_array_access(const yir::LoopSummary *summary, std::size_t rank) {
    if (summary == nullptr) {
        return false;
    }
    return std::any_of(summary->array_accesses.begin(), summary->array_accesses.end(),
                       [rank](const yir::ArrayAccess &access) {
                           return access.indices.size() >= rank;
                       });
}

std::int64_t affine_coefficient(const yir::AffineExpr &expr, const yir::Value *value) {
    if (expr.unknown || value == nullptr) {
        return 0;
    }
    std::int64_t out = 0;
    for (const auto &term : expr.terms) {
        if (term.value == value) {
            out += term.coefficient;
        }
    }
    return out;
}

int last_dimension_score(const yir::LoopSummary *summary, const yir::Value *iv) {
    if (summary == nullptr || iv == nullptr) {
        return 0;
    }
    int score = 0;
    for (const auto &access : summary->array_accesses) {
        if (access.indices.empty()) {
            continue;
        }
        const auto &index = access.indices.back();
        if (index.unknown) {
            continue;
        }
        const auto coefficient = affine_coefficient(index, iv);
        if (coefficient != 0) {
            score += access.is_store ? 2 : 1;
        }
    }
    return score;
}

bool value_depends_on(const yir::Value *value, const yir::Value *needle,
                      ValueSet &active) {
    if (value == nullptr || needle == nullptr) {
        return false;
    }
    if (value == needle) {
        return true;
    }
    if (!active.insert(value).second) {
        return false;
    }
    auto *def = value->defining_op();
    if (def == nullptr) {
        return false;
    }
    for (auto *operand : def->operands()) {
        if (value_depends_on(operand, needle, active)) {
            return true;
        }
    }
    return false;
}

bool value_depends_on(const yir::Value *value, const yir::Value *needle) {
    ValueSet active;
    return value_depends_on(value, needle, active);
}

bool loop_bounds_independent(const yir::ForOp &loop, const yir::Value *iv) {
    return !value_depends_on(loop.lower_bound(), iv) &&
           !value_depends_on(loop.upper_bound(), iv) && !value_depends_on(loop.step(), iv);
}

bool is_reduction_assign(const yir::AssignOp &assign) {
    auto *value_op = assign.value() == nullptr ? nullptr : assign.value()->defining_op();
    auto *binary = dynamic_cast<const yir::BinaryOpBase *>(value_op);
    if (binary == nullptr) {
        return false;
    }

    const bool target_lhs = binary->lhs() == assign.target();
    const bool target_rhs = binary->rhs() == assign.target();
    if (!target_lhs && !target_rhs) {
        return false;
    }
    if (dynamic_cast<const yir::AddIOp *>(value_op) != nullptr ||
        dynamic_cast<const yir::AddFOp *>(value_op) != nullptr ||
        dynamic_cast<const yir::MulIOp *>(value_op) != nullptr ||
        dynamic_cast<const yir::MulFOp *>(value_op) != nullptr) {
        return true;
    }
    if ((dynamic_cast<const yir::SubIOp *>(value_op) != nullptr ||
         dynamic_cast<const yir::SubFOp *>(value_op) != nullptr) &&
        target_lhs) {
        return true;
    }
    return false;
}

enum class ReductionCombineKind {
    Add,
    Mul,
};

struct ReductionInfo {
    ReductionCombineKind combine = ReductionCombineKind::Add;
    int identity = 0;
};

bool is_scalar_type(const yir::TypePtr &type) {
    return type != nullptr &&
           (type->kind() == yir::Type::Kind::I1 || type->kind() == yir::Type::Kind::I32 ||
            type->kind() == yir::Type::Kind::F32);
}

bool is_local_i32_var(const yir::Value *value) {
    return value != nullptr && value->type() == yir::Type::get_i32() &&
           dynamic_cast<const yir::VarOp *>(value->defining_op()) != nullptr;
}

bool supported_int_reduction(const yir::AssignOp &assign, ReductionInfo &info,
                             const yir::Operation *&value_op) {
    if (!is_local_i32_var(assign.target())) {
        return false;
    }

    value_op = assign.value() == nullptr ? nullptr : assign.value()->defining_op();
    auto *binary = dynamic_cast<const yir::BinaryOpBase *>(value_op);
    if (binary == nullptr) {
        return false;
    }

    const bool target_lhs = binary->lhs() == assign.target();
    const bool target_rhs = binary->rhs() == assign.target();
    if (!target_lhs && !target_rhs) {
        return false;
    }
    auto *other = target_lhs ? binary->rhs() : binary->lhs();
    if (value_depends_on(other, assign.target())) {
        return false;
    }

    if (dynamic_cast<const yir::AddIOp *>(value_op) != nullptr) {
        info.combine = ReductionCombineKind::Add;
        info.identity = 0;
        return true;
    }
    if (dynamic_cast<const yir::SubIOp *>(value_op) != nullptr && target_lhs) {
        info.combine = ReductionCombineKind::Add;
        info.identity = 0;
        return true;
    }
    if (dynamic_cast<const yir::MulIOp *>(value_op) != nullptr) {
        info.combine = ReductionCombineKind::Mul;
        info.identity = 1;
        return true;
    }
    return false;
}

ValueSet direct_local_scalar_vars(const yir::Region &region) {
    ValueSet locals;
    for (const auto &op : region.operations()) {
        if (auto *var = dynamic_cast<const yir::VarOp *>(op.get())) {
            locals.insert(var->result());
        }
    }
    return locals;
}

void collect_local_scalar_vars_recursive(const yir::Region &region, ValueSet &locals) {
    for (const auto &op : region.operations()) {
        if (auto *var = dynamic_cast<const yir::VarOp *>(op.get())) {
            if (is_scalar_type(var->result()->type())) {
                locals.insert(var->result());
            }
            continue;
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            collect_local_scalar_vars_recursive(if_op->then_region(), locals);
            if (if_op->has_else()) {
                collect_local_scalar_vars_recursive(if_op->else_region(), locals);
            }
            continue;
        }
        if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
            collect_local_scalar_vars_recursive(while_op->cond_region(), locals);
            collect_local_scalar_vars_recursive(while_op->body_region(), locals);
            continue;
        }
        if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
            collect_local_scalar_vars_recursive(for_op->body_region(), locals);
            continue;
        }
    }
}

bool value_set_contains_dependency(const yir::Value *value, const ValueSet &values) {
    for (auto *candidate : values) {
        if (value_depends_on(value, candidate)) {
            return true;
        }
    }
    return false;
}

bool collect_reduction_infos(const yir::Region &region, const ValueSet &locals,
                             std::unordered_map<const yir::Value *, ReductionInfo> &reductions,
                             ValueSet &allowed_reduction_results,
                             std::unordered_set<const yir::Operation *> &allowed_reduction_ops,
                             std::unordered_set<const yir::AssignOp *> &allowed_assigns) {
    for (const auto &op : region.operations()) {
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            if (!collect_reduction_infos(if_op->then_region(), locals, reductions,
                                         allowed_reduction_results, allowed_reduction_ops,
                                         allowed_assigns)) {
                return false;
            }
            if (if_op->has_else() &&
                !collect_reduction_infos(if_op->else_region(), locals, reductions,
                                         allowed_reduction_results, allowed_reduction_ops,
                                         allowed_assigns)) {
                return false;
            }
            continue;
        }

        auto *assign = dynamic_cast<const yir::AssignOp *>(op.get());
        if (assign == nullptr) {
            continue;
        }
        if (locals.find(assign->target()) != locals.end()) {
            continue;
        }

        ReductionInfo info;
        const yir::Operation *value_op = nullptr;
        if (!supported_int_reduction(*assign, info, value_op)) {
            return false;
        }

        auto found = reductions.find(assign->target());
        if (found != reductions.end() && found->second.combine != info.combine) {
            return false;
        }
        reductions[assign->target()] = info;
        allowed_assigns.insert(assign);
        allowed_reduction_ops.insert(value_op);
        if (value_op != nullptr && value_op->result() != nullptr) {
            allowed_reduction_results.insert(value_op->result());
        }
    }
    return true;
}

bool reduction_target_uses_are_isolated(
    const yir::Region &region, const ValueSet &targets, const ValueSet &allowed_results,
    const std::unordered_set<const yir::Operation *> &allowed_reduction_ops,
    const std::unordered_set<const yir::AssignOp *> &allowed_assigns) {
    for (const auto &op : region.operations()) {
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            if (value_set_contains_dependency(if_op->condition(), targets) ||
                !reduction_target_uses_are_isolated(if_op->then_region(), targets,
                                                    allowed_results, allowed_reduction_ops,
                                                    allowed_assigns) ||
                (if_op->has_else() &&
                 !reduction_target_uses_are_isolated(if_op->else_region(), targets,
                                                     allowed_results, allowed_reduction_ops,
                                                     allowed_assigns))) {
                return false;
            }
            continue;
        }

        if (allowed_reduction_ops.find(op.get()) != allowed_reduction_ops.end()) {
            for (auto *operand : op->operands()) {
                if (targets.find(operand) == targets.end() &&
                    value_set_contains_dependency(operand, targets)) {
                    return false;
                }
            }
            continue;
        }

        if (auto *assign = dynamic_cast<const yir::AssignOp *>(op.get())) {
            if (allowed_assigns.find(assign) != allowed_assigns.end()) {
                continue;
            }
            if (value_set_contains_dependency(assign->value(), targets) ||
                allowed_results.find(assign->value()) != allowed_results.end()) {
                return false;
            }
            continue;
        }

        for (auto *operand : op->operands()) {
            if (targets.find(operand) != targets.end() ||
                allowed_results.find(operand) != allowed_results.end() ||
                value_set_contains_dependency(operand, targets)) {
                return false;
            }
        }
    }
    return true;
}

bool scalar_assignments_are_vectorizable(const yir::Region &region,
                                         const yir::Value *induction_var,
                                         bool allow_reductions) {
    auto locals = direct_local_scalar_vars(region);
    for (const auto &op : region.operations()) {
        auto *assign = dynamic_cast<const yir::AssignOp *>(op.get());
        if (assign == nullptr) {
            continue;
        }
        if (assign->target() == induction_var) {
            return false;
        }
        if (locals.find(assign->target()) != locals.end()) {
            continue;
        }
        if (allow_reductions && is_reduction_assign(*assign)) {
            continue;
        }
        return false;
    }
    return true;
}

bool has_nonlocal_scalar_assignment(const yir::Region &region,
                                    const yir::Value *first_induction,
                                    const yir::Value *second_induction) {
    auto locals = direct_local_scalar_vars(region);
    for (const auto &op : region.operations()) {
        auto *assign = dynamic_cast<const yir::AssignOp *>(op.get());
        if (assign == nullptr) {
            continue;
        }
        if (assign->target() == first_induction || assign->target() == second_induction) {
            return true;
        }
        if (locals.find(assign->target()) == locals.end()) {
            return true;
        }
    }
    return false;
}

bool region_may_assign_value(const yir::Region &region, const yir::Value *value);

bool op_may_assign_value(const yir::Operation &op, const yir::Value *value) {
    if (auto *assign = dynamic_cast<const yir::AssignOp *>(&op)) {
        return assign->target() == value;
    }
    if (auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
        return region_may_assign_value(if_op->then_region(), value) ||
               (if_op->has_else() && region_may_assign_value(if_op->else_region(), value));
    }
    if (auto *while_op = dynamic_cast<const yir::WhileOp *>(&op)) {
        return region_may_assign_value(while_op->cond_region(), value) ||
               region_may_assign_value(while_op->body_region(), value);
    }
    if (auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
        return for_op->induction_var() == value ||
               region_may_assign_value(for_op->body_region(), value);
    }
    return false;
}

bool region_may_assign_value(const yir::Region &region, const yir::Value *value) {
    return std::any_of(region.operations().begin(), region.operations().end(),
                       [value](const std::unique_ptr<yir::Operation> &op) {
                           return op_may_assign_value(*op, value);
                       });
}

std::size_t count_assignments_to_value(const yir::Region &region, const yir::Value *value) {
    std::size_t count = 0;
    for (const auto &op : region.operations()) {
        if (auto *assign = dynamic_cast<const yir::AssignOp *>(op.get())) {
            if (assign->target() == value) {
                ++count;
            }
            continue;
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            count += count_assignments_to_value(if_op->then_region(), value);
            if (if_op->has_else()) {
                count += count_assignments_to_value(if_op->else_region(), value);
            }
            continue;
        }
        if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
            count += count_assignments_to_value(while_op->cond_region(), value);
            count += count_assignments_to_value(while_op->body_region(), value);
            continue;
        }
        if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
            if (for_op->induction_var() == value) {
                ++count;
            }
            count += count_assignments_to_value(for_op->body_region(), value);
            continue;
        }
    }
    return count;
}

const yir::CondOp *terminating_cond(const yir::Region &region) {
    if (region.operations().empty()) {
        return nullptr;
    }
    return dynamic_cast<const yir::CondOp *>(region.operations().back().get());
}

yir::Value *truthy_while_condition_value(const yir::WhileOp &op) {
    auto *cond = terminating_cond(op.cond_region());
    if (cond == nullptr || cond->condition() == nullptr) {
        return nullptr;
    }
    if (auto *to_bool = dynamic_cast<yir::ToBoolOp *>(cond->condition()->defining_op())) {
        return to_bool->operands()[0];
    }
    return cond->condition();
}

struct NestedForCandidate {
    yir::ForOp *inner = nullptr;
    std::size_t inner_index = 0;
    bool has_redundant_reset = false;
};

bool same_int_constant_value(const yir::Value *lhs, const yir::Value *rhs) {
    std::int64_t lhs_value = 0;
    std::int64_t rhs_value = 0;
    return const_i32(lhs, lhs_value) && const_i32(rhs, rhs_value) && lhs_value == rhs_value;
}

bool is_redundant_inner_iv_reset(const yir::Operation &op, const yir::ForOp &inner) {
    auto *assign = dynamic_cast<const yir::AssignOp *>(&op);
    return assign != nullptr && assign->target() == inner.induction_var() &&
           (assign->value() == inner.lower_bound() ||
            same_int_constant_value(assign->value(), inner.lower_bound()));
}

NestedForCandidate nested_for_with_optional_reset(yir::ForOp &outer) {
    auto &ops = outer.body_region().operations();
    if (ops.size() == 1) {
        return {dynamic_cast<yir::ForOp *>(ops.front().get()), 0, false};
    }
    if (ops.size() == 2) {
        auto *inner = dynamic_cast<yir::ForOp *>(ops[1].get());
        if (inner != nullptr && is_redundant_inner_iv_reset(*ops[0], *inner)) {
            return {inner, 1, true};
        }
    }
    return {};
}

void erase_redundant_reset(yir::ForOp &outer, const NestedForCandidate &nest) {
    if (!nest.has_redundant_reset || nest.inner_index == 0) {
        return;
    }
    auto &ops = outer.body_region().operations();
    ops.erase(ops.begin() + static_cast<std::ptrdiff_t>(nest.inner_index - 1));
}

std::int64_t trip_count(std::int64_t lower, std::int64_t upper, std::int64_t step) {
    if (step <= 0 || upper <= lower) {
        return 0;
    }
    return (upper - lower + step - 1) / step;
}

yir::Value *insert_const_i32_before(OpList &ops, std::size_t &index, int value,
                                    const std::string &name) {
    auto constant = std::make_unique<yir::ConstI32Op>(value, name);
    auto *result = constant->result();
    constant->set_parent(ops[index]->parent());
    ops.insert(ops.begin() + static_cast<std::ptrdiff_t>(index), std::move(constant));
    ++index;
    return result;
}

void append_clone_sequence(const yir::Region &source, yir::Region &dest, ValueMap map,
                           std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const auto &op = source.operations()[i];
        auto clone = clone_simple_op(*op, map);
        if (clone == nullptr) {
            return;
        }
        clone->set_parent(&dest);
        if (op->result() != nullptr && clone->result() != nullptr) {
            map[op->result()] = clone->result();
        }
        dest.operations().push_back(std::move(clone));
    }
}

int choose_tile_size(std::int64_t outer_trip_count, std::int64_t inner_trip_count) {
    const std::int64_t trip = std::min(outer_trip_count, inner_trip_count);
    if (trip < 64) {
        return 0;
    }
    for (int candidate : {20, 16, 10, 8}) {
        if (outer_trip_count % candidate == 0 && inner_trip_count % candidate == 0) {
            return candidate;
        }
    }
    return 0;
}

int choose_unroll_factor(std::int64_t trip_count, std::size_t body_ops,
                         int max_factor, std::size_t max_unrolled_ops,
                         bool has_control_flow) {
    if (trip_count <= 0 || max_factor < 2 || body_ops == 0) {
        return 0;
    }

    const int preferred = has_control_flow || body_ops > 12 ? std::min(max_factor, 4) : max_factor;
    for (int candidate : {preferred, 8, 4, 2}) {
        if (candidate < 2 || candidate > max_factor || trip_count < candidate * 4 ||
            trip_count % candidate != 0) {
            continue;
        }
        if (body_ops * static_cast<std::size_t>(candidate) <= max_unrolled_ops) {
            return candidate;
        }
    }
    return 0;
}

std::string derived_name(const yir::Value *value, const char *suffix, const char *fallback) {
    if (value != nullptr && !value->name().empty()) {
        return value->name() + suffix;
    }
    return fallback;
}

class Optimizer final {
  public:
    bool run(yir::Module &module) {
        changed_ = false;
        collect_constant_globals(module);
        constexpr unsigned kMaxIterations = 6;
        for (unsigned iteration = 0; iteration < kMaxIterations; ++iteration) {
            iteration_changed_ = false;
            yir::LoopAnalysis analysis(module);
            for (auto &function : module.functions()) {
                optimize_region(function->body(), analysis);
            }
            changed_ = changed_ || iteration_changed_;
            if (!iteration_changed_) {
                break;
            }
        }
        return changed_;
    }

  private:
    void collect_constant_globals(const yir::Module &module) {
        constant_globals_.clear();
        for (const auto &global : module.globals()) {
            if (!global->is_const() || global->storage_type() != yir::Type::get_i32()) {
                continue;
            }
            std::int64_t value = 0;
            if (parse_int64_literal(global->initializer(), value)) {
                constant_globals_[global->address()] = value;
            }
        }
    }

    bool const_i32_value(const yir::Value *value, std::int64_t &out) const {
        if (const_i32(value, out)) {
            return true;
        }
        auto found = constant_globals_.find(value);
        if (found == constant_globals_.end()) {
            return false;
        }
        out = found->second;
        return out >= std::numeric_limits<int>::min() &&
               out <= std::numeric_limits<int>::max();
    }

    void optimize_region(yir::Region &region, const yir::LoopAnalysis &analysis) {
        auto &ops = region.operations();
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (auto *if_op = dynamic_cast<yir::IfOp *>(ops[i].get())) {
                optimize_region(if_op->then_region(), analysis);
                if (if_op->has_else()) {
                    optimize_region(if_op->else_region(), analysis);
                }
            } else if (auto *while_op = dynamic_cast<yir::WhileOp *>(ops[i].get())) {
                optimize_region(while_op->cond_region(), analysis);
                optimize_region(while_op->body_region(), analysis);
                mark_changed(hoist_loop_invariants(ops, i, while_op->body_region(), nullptr));
            } else if (auto *for_op = dynamic_cast<yir::ForOp *>(ops[i].get())) {
                if (hoist_loop_invariants(ops, i, for_op->body_region(),
                                          for_op->induction_var())) {
                    mark_changed(true);
                    continue;
                }
                if (try_interchange(ops, i, *for_op, analysis) ||
                    try_tile_perfect_nest(ops, i, *for_op, analysis) ||
                    try_unroll_jam(ops, i, *for_op, analysis) ||
                    try_vectorize_inner_loop(ops, i, *for_op, analysis) ||
                    try_unroll_for(ops, i, *for_op)) {
                    mark_changed(true);
                    continue;
                }
                optimize_region(for_op->body_region(), analysis);
                mark_changed(hoist_loop_invariants(ops, i, for_op->body_region(),
                                                   for_op->induction_var()));
            }
        }
    }

    void mark_changed(bool changed) {
        iteration_changed_ = iteration_changed_ || changed;
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

    bool initial_i32_before(const OpList &ops, std::size_t index, const yir::Value *value,
                            std::int64_t &out) const {
        for (std::size_t cursor = index; cursor > 0; --cursor) {
            const std::size_t candidate = cursor - 1;
            if (auto *assign = dynamic_cast<const yir::AssignOp *>(ops[candidate].get())) {
                if (assign->target() == value) {
                    return const_i32_value(assign->value(), out);
                }
            } else if (auto *var = dynamic_cast<const yir::VarOp *>(ops[candidate].get())) {
                if (var->result() == value && var->has_initializer()) {
                    return const_i32_value(var->initializer(), out);
                }
            }

            if (op_may_assign_value(*ops[candidate], value)) {
                return false;
            }
        }
        return false;
    }

    bool countdown_latch_step(const yir::Region &body, const yir::Value *iv,
                              std::int64_t &step) const {
        if (body.operations().empty() || count_assignments_to_value(body, iv) != 1) {
            return false;
        }
        auto *assign = dynamic_cast<const yir::AssignOp *>(body.operations().back().get());
        if (assign == nullptr || assign->target() != iv) {
            return false;
        }
        auto *sub = dynamic_cast<const yir::SubIOp *>(assign->value()->defining_op());
        if (sub == nullptr || sub->lhs() != iv || !const_i32_value(sub->rhs(), step)) {
            return false;
        }
        return step > 0;
    }

    bool try_unroll_countdown_while(OpList &ops, std::size_t &index,
                                    const yir::WhileOp &while_op) {
        auto *iv = truthy_while_condition_value(while_op);
        if (iv == nullptr || !is_unroll_safe(while_op.body_region())) {
            return false;
        }

        std::int64_t initial = 0;
        std::int64_t step = 0;
        if (!initial_i32_before(ops, index, iv, initial) ||
            !countdown_latch_step(while_op.body_region(), iv, step) || initial <= 0 ||
            initial % step != 0) {
            return false;
        }

        const std::int64_t trip_count = initial / step;
        const auto body_ops = operation_count(while_op.body_region()) + 1;
        const std::int64_t max_unrolled_ops = region_has_if(while_op.body_region()) ? 1024 : 512;
        if (trip_count <= 0 || trip_count > 64 ||
            trip_count * static_cast<std::int64_t>(body_ops) > max_unrolled_ops) {
            return false;
        }

        std::vector<std::unique_ptr<yir::Operation>> clones;
        clones.reserve(static_cast<std::size_t>(trip_count) *
                       while_op.body_region().operations().size());
        for (std::int64_t iter = 0; iter < trip_count; ++iter) {
            ValueMap map;
            for (const auto &op : while_op.body_region().operations()) {
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
        if (!clones.empty()) {
            index += clones.size() - 1;
        } else if (index > 0) {
            --index;
        }
        return true;
    }

    bool try_reduction_register_unroll(OpList &ops, std::size_t &index, yir::ForOp &for_op,
                                       const yir::LoopAnalysis &analysis) {
        if (!is_unroll_safe(for_op.body_region())) {
            return false;
        }

        const auto *summary = analysis.summary_for(&for_op);
        if (summary != nullptr && !dependence_allows_reorder(summary)) {
            return false;
        }

        std::int64_t lower = 0;
        std::int64_t upper = 0;
        std::int64_t step = 0;
        if (!const_i32_value(for_op.lower_bound(), lower) ||
            !const_i32_value(for_op.upper_bound(), upper) ||
            !const_i32_value(for_op.step(), step) || step != 1) {
            return false;
        }

        ValueSet locals;
        collect_local_scalar_vars_recursive(for_op.body_region(), locals);
        locals.insert(for_op.induction_var());

        std::unordered_map<const yir::Value *, ReductionInfo> reductions;
        ValueSet allowed_results;
        std::unordered_set<const yir::Operation *> allowed_reduction_ops;
        std::unordered_set<const yir::AssignOp *> allowed_assigns;
        if (!collect_reduction_infos(for_op.body_region(), locals, reductions, allowed_results,
                                     allowed_reduction_ops, allowed_assigns) ||
            reductions.empty()) {
            return false;
        }

        ValueSet targets;
        for (const auto &[target, info] : reductions) {
            (void)info;
            targets.insert(target);
        }
        if (!reduction_target_uses_are_isolated(for_op.body_region(), targets, allowed_results,
                                                allowed_reduction_ops, allowed_assigns)) {
            return false;
        }

        auto &body_ops = for_op.body_region().operations();
        const std::size_t original_count = body_ops.size();
        const auto counted_ops = operation_count(for_op.body_region());
        if (original_count == 0 || counted_ops > 96) {
            return false;
        }

        const auto count = trip_count(lower, upper, step);
        const int factor = choose_unroll_factor(count, counted_ops, 8, 320,
                                                region_has_if(for_op.body_region()));
        if (factor == 0) {
            return false;
        }

        auto *new_step =
            insert_const_i32_before(ops, index, static_cast<int>(step * factor),
                                    derived_name(for_op.induction_var(), ".red.step", "red.step"));
        for_op.operands()[3] = new_step;

        auto *parent = ops[index]->parent();
        std::unordered_map<const yir::Value *, std::vector<yir::Value *>> lane_accumulators;
        for (const auto &[target, info] : reductions) {
            auto identity = std::make_unique<yir::ConstI32Op>(
                info.identity, derived_name(target, ".red.identity", "red.identity"));
            auto *identity_value = identity->result();
            identity->set_parent(parent);
            ops.insert(ops.begin() + static_cast<std::ptrdiff_t>(index), std::move(identity));
            ++index;

            for (int lane = 1; lane < factor; ++lane) {
                auto lane_var = std::make_unique<yir::VarOp>(
                    yir::Type::get_i32(), identity_value,
                    derived_name(target, ".red.acc", "red.acc"));
                auto *lane_value = lane_var->result();
                lane_var->set_parent(parent);
                ops.insert(ops.begin() + static_cast<std::ptrdiff_t>(index),
                           std::move(lane_var));
                ++index;
                lane_accumulators[target].push_back(lane_value);
            }
        }

        for (int lane = 1; lane < factor; ++lane) {
            auto lane_const = std::make_unique<yir::ConstI32Op>(
                static_cast<int>(step * lane),
                derived_name(for_op.induction_var(), ".red.lane", "red.lane"));
            auto *lane_value = lane_const->result();
            lane_const->set_parent(&for_op.body_region());
            body_ops.push_back(std::move(lane_const));

            auto offset = std::make_unique<yir::AddIOp>(
                for_op.induction_var(), lane_value,
                derived_name(for_op.induction_var(), ".red.iv", "red.iv"));
            auto *offset_value = offset->result();
            offset->set_parent(&for_op.body_region());
            body_ops.push_back(std::move(offset));

            ValueMap map;
            map[for_op.induction_var()] = offset_value;
            for (const auto &[target, lanes] : lane_accumulators) {
                map[const_cast<yir::Value *>(target)] =
                    lanes[static_cast<std::size_t>(lane - 1)];
            }
            append_clone_sequence(for_op.body_region(), for_op.body_region(), std::move(map),
                                  original_count);
        }

        std::vector<std::unique_ptr<yir::Operation>> combines;
        for (const auto &[target, info] : reductions) {
            for (auto *lane_acc : lane_accumulators[target]) {
                std::unique_ptr<yir::Operation> combine;
                if (info.combine == ReductionCombineKind::Mul) {
                    combine = std::make_unique<yir::MulIOp>(
                        const_cast<yir::Value *>(target), lane_acc,
                        derived_name(target, ".red.combine", "red.combine"));
                } else {
                    combine = std::make_unique<yir::AddIOp>(
                        const_cast<yir::Value *>(target), lane_acc,
                        derived_name(target, ".red.combine", "red.combine"));
                }
                auto *combined_value = combine->result();
                combine->set_parent(parent);
                combines.push_back(std::move(combine));

                auto assign =
                    std::make_unique<yir::AssignOp>(const_cast<yir::Value *>(target),
                                                    combined_value);
                assign->set_parent(parent);
                combines.push_back(std::move(assign));
            }
        }

        auto insert = ops.begin() + static_cast<std::ptrdiff_t>(index + 1);
        ops.insert(insert, std::make_move_iterator(combines.begin()),
                   std::make_move_iterator(combines.end()));
        return true;
    }

    bool try_interchange(OpList &, std::size_t &, yir::ForOp &outer,
                         const yir::LoopAnalysis &analysis) {
        auto nest = nested_for_with_optional_reset(outer);
        auto *inner = nest.inner;
        if (inner == nullptr || !region_is_straight_line_cloneable(inner->body_region())) {
            return false;
        }
        if (!loop_bounds_independent(*inner, outer.induction_var()) ||
            has_nonlocal_scalar_assignment(inner->body_region(), outer.induction_var(),
                                           inner->induction_var())) {
            return false;
        }

        const auto *inner_summary = analysis.summary_for(inner);
        if (!dependence_allows_reorder(inner_summary) ||
            !has_ranked_array_access(inner_summary, 2)) {
            return false;
        }

        const int outer_score = last_dimension_score(inner_summary, outer.induction_var());
        const int inner_score = last_dimension_score(inner_summary, inner->induction_var());
        if (outer_score <= inner_score) {
            return false;
        }

        erase_redundant_reset(outer, nest);
        std::swap(outer.operands()[0], inner->operands()[0]);
        std::swap(outer.operands()[1], inner->operands()[1]);
        std::swap(outer.operands()[2], inner->operands()[2]);
        std::swap(outer.operands()[3], inner->operands()[3]);
        return true;
    }

    bool try_tile_perfect_nest(OpList &ops, std::size_t &index, yir::ForOp &outer,
                               const yir::LoopAnalysis &analysis) {
        auto nest = nested_for_with_optional_reset(outer);
        auto *inner = nest.inner;
        if (inner == nullptr || !region_is_straight_line_cloneable(inner->body_region())) {
            return false;
        }
        if (!loop_bounds_independent(*inner, outer.induction_var()) ||
            has_nonlocal_scalar_assignment(inner->body_region(), outer.induction_var(),
                                           inner->induction_var())) {
            return false;
        }

        const auto *inner_summary = analysis.summary_for(inner);
        if (!dependence_allows_reorder(inner_summary) ||
            !has_ranked_array_access(inner_summary, 2)) {
            return false;
        }

        std::int64_t outer_lower = 0;
        std::int64_t outer_upper = 0;
        std::int64_t outer_step = 0;
        std::int64_t inner_lower = 0;
        std::int64_t inner_upper = 0;
        std::int64_t inner_step = 0;
        if (!const_i32_value(outer.lower_bound(), outer_lower) ||
            !const_i32_value(outer.upper_bound(), outer_upper) ||
            !const_i32_value(outer.step(), outer_step) ||
            !const_i32_value(inner->lower_bound(), inner_lower) ||
            !const_i32_value(inner->upper_bound(), inner_upper) ||
            !const_i32_value(inner->step(), inner_step) || outer_step != 1 || inner_step != 1) {
            return false;
        }

        const std::int64_t outer_trip = trip_count(outer_lower, outer_upper, outer_step);
        const std::int64_t inner_trip = trip_count(inner_lower, inner_upper, inner_step);
        const int tile_size = choose_tile_size(outer_trip, inner_trip);
        if (tile_size == 0) {
            return false;
        }

        auto *parent = ops[index]->parent();
        auto *outer_iv = outer.induction_var();
        auto *inner_iv = inner->induction_var();
        auto *outer_lower_value = outer.lower_bound();
        auto *outer_upper_value = outer.upper_bound();
        auto *outer_step_value = outer.step();
        auto *inner_lower_value = inner->lower_bound();
        auto *inner_upper_value = inner->upper_bound();
        auto *inner_step_value = inner->step();

        auto *outer_tile_step =
            insert_const_i32_before(ops, index, tile_size,
                                    derived_name(outer_iv, ".tile.step", "tile.step"));
        auto *inner_tile_step =
            insert_const_i32_before(ops, index, tile_size,
                                    derived_name(inner_iv, ".tile.step", "tile.step"));

        auto outer_tile_var = std::make_unique<yir::VarOp>(
            yir::Type::get_i32(), outer_lower_value,
            derived_name(outer_iv, ".tile", "tile.outer"));
        auto *outer_tile_iv = outer_tile_var->result();
        outer_tile_var->set_parent(parent);
        ops.insert(ops.begin() + static_cast<std::ptrdiff_t>(index),
                   std::move(outer_tile_var));
        ++index;

        auto inner_tile_var = std::make_unique<yir::VarOp>(
            yir::Type::get_i32(), inner_lower_value,
            derived_name(inner_iv, ".tile", "tile.inner"));
        auto *inner_tile_iv = inner_tile_var->result();
        inner_tile_var->set_parent(parent);
        ops.insert(ops.begin() + static_cast<std::ptrdiff_t>(index),
                   std::move(inner_tile_var));
        ++index;

        std::vector<std::unique_ptr<yir::Operation>> payload;
        auto &old_payload = inner->body_region().operations();
        payload.reserve(old_payload.size());
        for (auto &op : old_payload) {
            payload.push_back(std::move(op));
        }
        old_payload.clear();

        auto outer_tile =
            std::make_unique<yir::ForOp>(outer_tile_iv, outer_lower_value, outer_upper_value,
                                         outer_tile_step);
        outer_tile->set_parent(parent);

        auto inner_tile =
            std::make_unique<yir::ForOp>(inner_tile_iv, inner_lower_value, inner_upper_value,
                                         inner_tile_step);
        auto *inner_tile_raw = inner_tile.get();
        inner_tile->set_parent(&outer_tile->body_region());
        outer_tile->body_region().operations().push_back(std::move(inner_tile));

        auto outer_limit = std::make_unique<yir::AddIOp>(
            outer_tile_iv, outer_tile_step, derived_name(outer_iv, ".tile.end", "tile.outer.end"));
        auto *outer_limit_value = outer_limit->result();
        outer_limit->set_parent(&inner_tile_raw->body_region());
        inner_tile_raw->body_region().operations().push_back(std::move(outer_limit));

        auto inner_limit = std::make_unique<yir::AddIOp>(
            inner_tile_iv, inner_tile_step, derived_name(inner_iv, ".tile.end", "tile.inner.end"));
        auto *inner_limit_value = inner_limit->result();
        inner_limit->set_parent(&inner_tile_raw->body_region());
        inner_tile_raw->body_region().operations().push_back(std::move(inner_limit));

        auto point_outer =
            std::make_unique<yir::ForOp>(outer_iv, outer_tile_iv, outer_limit_value,
                                         outer_step_value);
        auto *point_outer_raw = point_outer.get();
        point_outer->set_parent(&inner_tile_raw->body_region());
        inner_tile_raw->body_region().operations().push_back(std::move(point_outer));

        auto point_inner =
            std::make_unique<yir::ForOp>(inner_iv, inner_tile_iv, inner_limit_value,
                                         inner_step_value);
        auto *point_inner_raw = point_inner.get();
        point_inner->set_parent(&point_outer_raw->body_region());
        point_outer_raw->body_region().operations().push_back(std::move(point_inner));

        for (auto &op : payload) {
            op->set_parent(&point_inner_raw->body_region());
            point_inner_raw->body_region().operations().push_back(std::move(op));
        }

        ops[index] = std::move(outer_tile);
        return true;
    }

    bool try_unroll_jam(OpList &ops, std::size_t &index, yir::ForOp &outer,
                        const yir::LoopAnalysis &analysis) {
        auto nest = nested_for_with_optional_reset(outer);
        auto *inner = nest.inner;
        if (inner == nullptr || !region_is_straight_line_cloneable(inner->body_region())) {
            return false;
        }
        if (has_nonlocal_scalar_assignment(inner->body_region(), outer.induction_var(),
                                           inner->induction_var())) {
            return false;
        }
        const auto *inner_summary = analysis.summary_for(inner);
        if (!dependence_allows_reorder(inner_summary) ||
            !has_ranked_array_access(inner_summary, 1)) {
            return false;
        }

        std::int64_t lower = 0;
        std::int64_t upper = 0;
        std::int64_t step = 0;
        if (!const_i32_value(outer.lower_bound(), lower) ||
            !const_i32_value(outer.upper_bound(), upper) ||
            !const_i32_value(outer.step(), step) || step != 1) {
            return false;
        }

        constexpr int kFactor = 2;
        const auto count = trip_count(lower, upper, step);
        if (count < 16 || count % kFactor != 0) {
            return false;
        }

        auto &outer_ops = outer.body_region().operations();
        auto &inner_ops = inner->body_region().operations();
        const std::size_t original_inner_count = inner_ops.size();
        if (original_inner_count == 0 || original_inner_count > 48) {
            return false;
        }

        auto *new_step =
            insert_const_i32_before(ops, index, static_cast<int>(step * kFactor),
                                    derived_name(outer.induction_var(), ".jam.step", "jam.step"));
        outer.operands()[3] = new_step;

        for (int lane = 1; lane < kFactor; ++lane) {
            auto lane_const = std::make_unique<yir::ConstI32Op>(
                static_cast<int>(step * lane),
                derived_name(outer.induction_var(), ".jam.lane", "jam.lane"));
            auto *lane_value = lane_const->result();
            lane_const->set_parent(&outer.body_region());
            outer_ops.insert(outer_ops.end() - 1, std::move(lane_const));

            auto offset = std::make_unique<yir::AddIOp>(
                outer.induction_var(), lane_value,
                derived_name(outer.induction_var(), ".jam.iv", "jam.iv"));
            auto *offset_value = offset->result();
            offset->set_parent(&outer.body_region());
            outer_ops.insert(outer_ops.end() - 1, std::move(offset));

            ValueMap map;
            map[outer.induction_var()] = offset_value;
            append_clone_sequence(inner->body_region(), inner->body_region(), std::move(map),
                                  original_inner_count);
        }
        return true;
    }

    bool try_vectorize_inner_loop(OpList &ops, std::size_t &index, yir::ForOp &for_op,
                                  const yir::LoopAnalysis &analysis) {
        if (!region_is_straight_line_cloneable(for_op.body_region()) ||
            !scalar_assignments_are_vectorizable(for_op.body_region(),
                                                 for_op.induction_var(), true)) {
            return false;
        }

        const auto *summary = analysis.summary_for(&for_op);
        if (!dependence_allows_reorder(summary) || summary->array_accesses.empty()) {
            return false;
        }

        std::int64_t lower = 0;
        std::int64_t upper = 0;
        std::int64_t step = 0;
        if (!const_i32_value(for_op.lower_bound(), lower) ||
            !const_i32_value(for_op.upper_bound(), upper) ||
            !const_i32_value(for_op.step(), step) || step != 1) {
            return false;
        }

        constexpr int kFactor = 4;
        const auto count = trip_count(lower, upper, step);
        if (count < 16 || count % kFactor != 0) {
            return false;
        }

        auto &body_ops = for_op.body_region().operations();
        const std::size_t original_count = body_ops.size();
        if (original_count == 0 || original_count > 64) {
            return false;
        }

        auto *new_step =
            insert_const_i32_before(ops, index, static_cast<int>(step * kFactor),
                                    derived_name(for_op.induction_var(), ".vec.step", "vec.step"));
        for_op.operands()[3] = new_step;

        for (int lane = 1; lane < kFactor; ++lane) {
            auto lane_const = std::make_unique<yir::ConstI32Op>(
                static_cast<int>(step * lane),
                derived_name(for_op.induction_var(), ".vec.lane", "vec.lane"));
            auto *lane_value = lane_const->result();
            lane_const->set_parent(&for_op.body_region());
            body_ops.push_back(std::move(lane_const));

            auto offset = std::make_unique<yir::AddIOp>(
                for_op.induction_var(), lane_value,
                derived_name(for_op.induction_var(), ".vec.iv", "vec.iv"));
            auto *offset_value = offset->result();
            offset->set_parent(&for_op.body_region());
            body_ops.push_back(std::move(offset));

            ValueMap map;
            map[for_op.induction_var()] = offset_value;
            append_clone_sequence(for_op.body_region(), for_op.body_region(), std::move(map),
                                  original_count);
        }
        return true;
    }

    bool try_unroll_for(OpList &ops, std::size_t &index, const yir::ForOp &for_op) {
        std::int64_t lower = 0;
        std::int64_t upper = 0;
        std::int64_t step = 0;
        if (!const_i32_value(for_op.lower_bound(), lower) ||
            !const_i32_value(for_op.upper_bound(), upper) ||
            !const_i32_value(for_op.step(), step) || step <= 0) {
            return false;
        }
        std::int64_t trip_count = upper <= lower ? 0 : (upper - lower + step - 1) / step;
        const auto body_ops = static_cast<std::int64_t>(operation_count(for_op.body_region()) + 1);
        const std::int64_t max_trip_count = body_ops <= 12 ? 16 : 8;
        if (trip_count < 0 || trip_count > max_trip_count ||
            !is_unroll_safe(for_op.body_region())) {
            return false;
        }
        const std::int64_t kMaxUnrolledOps = region_has_if(for_op.body_region()) ? 768 : 192;
        const std::int64_t unrolled_ops = trip_count * body_ops;
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
        const auto final_value = static_cast<int>(lower + trip_count * step);
        auto final_const =
            std::make_unique<yir::ConstI32Op>(final_value, for_op.induction_var()->name());
        final_const->set_parent(ops[index]->parent());
        auto *final_result = final_const->result();
        clones.push_back(std::move(final_const));

        auto final_assign =
            std::make_unique<yir::AssignOp>(for_op.induction_var(), final_result);
        final_assign->set_parent(ops[index]->parent());
        clones.push_back(std::move(final_assign));

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
    bool iteration_changed_ = false;
    std::unordered_map<const yir::Value *, std::int64_t> constant_globals_;
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
