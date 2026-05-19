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

ValueSet direct_local_scalar_vars(const yir::Region &region) {
    ValueSet locals;
    for (const auto &op : region.operations()) {
        if (auto *var = dynamic_cast<const yir::VarOp *>(op.get())) {
            locals.insert(var->result());
        }
    }
    return locals;
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

yir::ForOp *only_nested_for(yir::ForOp &loop) {
    auto &ops = loop.body_region().operations();
    if (ops.size() != 1) {
        return nullptr;
    }
    return dynamic_cast<yir::ForOp *>(ops.front().get());
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

    bool try_interchange(OpList &, std::size_t &, yir::ForOp &outer,
                         const yir::LoopAnalysis &analysis) {
        auto *inner = only_nested_for(outer);
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

        std::swap(outer.operands()[0], inner->operands()[0]);
        std::swap(outer.operands()[1], inner->operands()[1]);
        std::swap(outer.operands()[2], inner->operands()[2]);
        std::swap(outer.operands()[3], inner->operands()[3]);
        return true;
    }

    bool try_tile_perfect_nest(OpList &ops, std::size_t &index, yir::ForOp &outer,
                               const yir::LoopAnalysis &analysis) {
        auto *inner = only_nested_for(outer);
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
        auto *inner = only_nested_for(outer);
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
