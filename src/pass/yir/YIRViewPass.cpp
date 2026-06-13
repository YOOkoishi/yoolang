#include "pass/yir/YIRViewPass.h"

#include "pass/ast/ASTToYIRPass.h"
#include "yir/YIR.h"
#include "yir/YIRVerifier.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pass {
namespace {

using OpList = yir::Region::OpList;
using ValueSet = std::unordered_set<const yir::Value *>;

std::string verify_message(const yir::VerifyResult &verify) {
    if (verify.errors.empty()) {
        return "YIR verification failed";
    }
    std::ostringstream oss;
    for (std::size_t i = 0; i < verify.errors.size(); ++i) {
        if (i != 0) {
            oss << "; ";
        }
        oss << verify.errors[i];
    }
    return oss.str();
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

std::vector<std::uint64_t> static_array_dimensions(yir::TypePtr type) {
    std::vector<std::uint64_t> dims;
    while (type != nullptr && type->is_array()) {
        dims.push_back(type->count());
        type = type->element();
    }
    return dims;
}

bool is_local_array(const yir::Value *value) {
    return value != nullptr && value->type() != nullptr && value->type()->is_array() &&
           dynamic_cast<const yir::ArrayVarOp *>(value->defining_op()) != nullptr;
}

bool is_static_array_storage(const yir::Value *value) {
    return value != nullptr && value->type() != nullptr && value->type()->is_array();
}

struct AffineExpr {
    std::unordered_map<const yir::Value *, std::int64_t> terms;
    std::int64_t constant = 0;
};

bool add_checked(std::int64_t lhs, std::int64_t rhs, std::int64_t &out) {
    if ((rhs > 0 && lhs > INT64_MAX - rhs) || (rhs < 0 && lhs < INT64_MIN - rhs)) {
        return false;
    }
    out = lhs + rhs;
    return true;
}

bool mul_checked(std::int64_t lhs, std::int64_t rhs, std::int64_t &out) {
    if (lhs == 0 || rhs == 0) {
        out = 0;
        return true;
    }
    if (lhs == -1 && rhs == INT64_MIN) {
        return false;
    }
    if (rhs == -1 && lhs == INT64_MIN) {
        return false;
    }
    const std::int64_t result = lhs * rhs;
    if (result / rhs != lhs) {
        return false;
    }
    out = result;
    return true;
}

bool fits_i32(std::int64_t value) {
    return value >= std::numeric_limits<int>::min() && value <= std::numeric_limits<int>::max();
}

bool add_affine(AffineExpr &lhs, const AffineExpr &rhs, std::int64_t scale) {
    std::int64_t scaled_const = 0;
    if (!mul_checked(rhs.constant, scale, scaled_const) ||
        !add_checked(lhs.constant, scaled_const, lhs.constant)) {
        return false;
    }

    for (const auto &[value, coeff] : rhs.terms) {
        std::int64_t scaled = 0;
        if (!mul_checked(coeff, scale, scaled)) {
            return false;
        }
        auto &slot = lhs.terms[value];
        if (!add_checked(slot, scaled, slot)) {
            return false;
        }
        if (slot == 0) {
            lhs.terms.erase(value);
        }
    }
    return true;
}

bool parse_affine(const yir::Value *value, AffineExpr &out, ValueSet &active);

bool parse_binary_affine(const yir::BinaryOpBase &binary, const yir::Operation &op, AffineExpr &out,
                         ValueSet &active) {
    if (dynamic_cast<const yir::AddIOp *>(&op) != nullptr ||
        dynamic_cast<const yir::SubIOp *>(&op) != nullptr) {
        AffineExpr lhs;
        AffineExpr rhs;
        if (!parse_affine(binary.lhs(), lhs, active) || !parse_affine(binary.rhs(), rhs, active)) {
            return false;
        }
        out = std::move(lhs);
        return add_affine(out, rhs, dynamic_cast<const yir::SubIOp *>(&op) != nullptr ? -1 : 1);
    }

    if (dynamic_cast<const yir::MulIOp *>(&op) != nullptr) {
        std::int64_t lhs_const = 0;
        std::int64_t rhs_const = 0;
        const bool lhs_is_const = const_i32_value(binary.lhs(), lhs_const);
        const bool rhs_is_const = const_i32_value(binary.rhs(), rhs_const);
        const yir::Value *expr_value = nullptr;
        std::int64_t scale = 0;
        if (lhs_is_const) {
            expr_value = binary.rhs();
            scale = lhs_const;
        } else if (rhs_is_const) {
            expr_value = binary.lhs();
            scale = rhs_const;
        } else {
            return false;
        }

        AffineExpr expr;
        if (!parse_affine(expr_value, expr, active)) {
            return false;
        }
        out = {};
        return add_affine(out, expr, scale);
    }

    return false;
}

bool parse_affine(const yir::Value *value, AffineExpr &out, ValueSet &active) {
    if (value == nullptr) {
        return false;
    }

    std::int64_t constant = 0;
    if (const_i32_value(value, constant)) {
        out = {};
        out.constant = constant;
        return true;
    }

    if (!active.insert(value).second) {
        return false;
    }

    auto *def = value->defining_op();
    if (auto *binary = dynamic_cast<const yir::BinaryOpBase *>(def)) {
        const bool ok = parse_binary_affine(*binary, *def, out, active);
        active.erase(value);
        if (ok) {
            return true;
        }
    } else if (dynamic_cast<const yir::ZExtI1ToI32Op *>(def) != nullptr ||
               dynamic_cast<const yir::TruncI32ToI1Op *>(def) != nullptr) {
        active.erase(value);
        return false;
    } else {
        active.erase(value);
    }

    out = {};
    out.terms[value] = 1;
    return true;
}

bool parse_affine(const yir::Value *value, AffineExpr &out) {
    ValueSet active;
    return parse_affine(value, out, active);
}

struct LoopInfo {
    yir::Value *iv = nullptr;
    yir::Value *lower = nullptr;
    yir::Value *upper = nullptr;
    yir::Value *step = nullptr;
    yir::Region *body = nullptr;
    const yir::AssignOp *latch_assign = nullptr;
    const yir::Operation *step_op = nullptr;
    const yir::Operation *step_value_op = nullptr;
};

const yir::CondOp *terminating_cond(const yir::Region &region) {
    if (region.operations().empty()) {
        return nullptr;
    }
    return dynamic_cast<const yir::CondOp *>(region.operations().back().get());
}

const yir::ICmpOp *condition_compare(const yir::WhileOp &op) {
    auto *cond = terminating_cond(op.cond_region());
    if (cond == nullptr || cond->condition() == nullptr) {
        return nullptr;
    }
    auto *condition = cond->condition();
    if (auto *to_bool = dynamic_cast<const yir::ToBoolOp *>(condition->defining_op())) {
        condition = to_bool->operands()[0];
    }
    return dynamic_cast<const yir::ICmpOp *>(condition->defining_op());
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

bool op_may_assign_value(const yir::Operation &op, const yir::Value *value);

bool region_may_assign_value(const yir::Region &region, const yir::Value *value) {
    return std::any_of(region.operations().begin(), region.operations().end(),
                       [value](const std::unique_ptr<yir::Operation> &op) {
                           return op_may_assign_value(*op, value);
                       });
}

bool op_may_assign_value(const yir::Operation &op, const yir::Value *value) {
    if (auto *assign = dynamic_cast<const yir::AssignOp *>(&op)) {
        return assign->target() == value;
    }
    if (auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
        return for_op->induction_var() == value ||
               region_may_assign_value(for_op->body_region(), value);
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

bool find_lower_bound(const OpList &ops, std::size_t index, yir::Value *iv, yir::Value *&lower) {
    for (std::size_t cursor = index; cursor > 0; --cursor) {
        const std::size_t candidate = cursor - 1;
        if (auto *assign = dynamic_cast<const yir::AssignOp *>(ops[candidate].get())) {
            if (assign->target() == iv) {
                lower = assign->value();
                return true;
            }
        } else if (auto *var = dynamic_cast<const yir::VarOp *>(ops[candidate].get())) {
            if (var->result() == iv && var->has_initializer()) {
                lower = var->initializer();
                return true;
            }
        }
        if (op_may_assign_value(*ops[candidate], iv)) {
            return false;
        }
    }
    return false;
}

bool parse_while_loop(const OpList &ops, std::size_t index, yir::WhileOp &while_op, LoopInfo &out) {
    auto *icmp = condition_compare(while_op);
    if (icmp == nullptr || icmp->predicate() != yir::ICmpOp::Predicate::Lt) {
        return false;
    }

    yir::Value *lower = nullptr;
    if (!find_lower_bound(ops, index, icmp->lhs(), lower)) {
        return false;
    }

    const auto &body_ops = while_op.body_region().operations();
    if (body_ops.empty()) {
        return false;
    }
    auto *latch_assign = dynamic_cast<const yir::AssignOp *>(body_ops.back().get());
    if (latch_assign == nullptr || latch_assign->target() != icmp->lhs()) {
        return false;
    }

    yir::Value *step = nullptr;
    const yir::Operation *step_op = nullptr;
    if (!parse_positive_latch(*latch_assign, icmp->lhs(), step, step_op)) {
        return false;
    }

    out.iv = icmp->lhs();
    out.lower = lower;
    out.upper = icmp->rhs();
    out.step = step;
    out.body = &while_op.body_region();
    out.latch_assign = latch_assign;
    out.step_op = step_op;
    out.step_value_op = step == nullptr ? nullptr : step->defining_op();
    return true;
}

bool parse_loop_at(const OpList &ops, std::size_t index, LoopInfo &out) {
    if (index >= ops.size()) {
        return false;
    }
    if (auto *for_op = dynamic_cast<yir::ForOp *>(ops[index].get())) {
        out.iv = for_op->induction_var();
        out.lower = for_op->lower_bound();
        out.upper = for_op->upper_bound();
        out.step = for_op->step();
        out.body = &for_op->body_region();
        return true;
    }
    if (auto *while_op = dynamic_cast<yir::WhileOp *>(ops[index].get())) {
        return parse_while_loop(ops, index, *while_op, out);
    }
    return false;
}

bool is_latch_helper(const yir::Operation *op, const LoopInfo &loop) {
    return op == loop.latch_assign || op == loop.step_op ||
           (loop.step_value_op != nullptr && op == loop.step_value_op);
}

bool is_leaf_pure_op(const yir::Operation &op) {
    return dynamic_cast<const yir::ConstI32Op *>(&op) != nullptr ||
           dynamic_cast<const yir::ConstF32Op *>(&op) != nullptr ||
           dynamic_cast<const yir::ConstBoolOp *>(&op) != nullptr ||
           dynamic_cast<const yir::ZeroOp *>(&op) != nullptr ||
           dynamic_cast<const yir::BinaryOpBase *>(&op) != nullptr ||
           dynamic_cast<const yir::ICmpOp *>(&op) != nullptr ||
           dynamic_cast<const yir::FCmpOp *>(&op) != nullptr ||
           dynamic_cast<const yir::ZExtI1ToI32Op *>(&op) != nullptr ||
           dynamic_cast<const yir::TruncI32ToI1Op *>(&op) != nullptr ||
           dynamic_cast<const yir::SIToFPOp *>(&op) != nullptr ||
           dynamic_cast<const yir::FPToSIOp *>(&op) != nullptr ||
           dynamic_cast<const yir::ToBoolOp *>(&op) != nullptr ||
           dynamic_cast<const yir::NotOp *>(&op) != nullptr;
}

bool loop_bounds_are_full(const LoopInfo &loop, std::int64_t &upper) {
    std::int64_t lower = 0;
    std::int64_t step = 0;
    return const_i32_value(loop.lower, lower) && lower == 0 && const_i32_value(loop.step, step) &&
           step == 1 && const_i32_value(loop.upper, upper) && upper >= 0;
}

struct LoopDim {
    yir::Value *iv = nullptr;
    std::int64_t upper = 0;
};

struct InitShape {
    std::vector<LoopDim> loops;
    yir::ArrayStoreOp *store = nullptr;
};

bool collect_init_shape(const OpList &ops, std::size_t index, InitShape &shape);

bool validate_loop_payload(const OpList &ops, const LoopInfo &loop, std::size_t child_index,
                           const yir::ArrayStoreOp *store) {
    yir::Value *child_iv = nullptr;
    if (store == nullptr) {
        LoopInfo child_loop;
        if (!parse_loop_at(ops, child_index, child_loop)) {
            return false;
        }
        child_iv = child_loop.iv;
    }

    const yir::ArrayLoadOp *source_load =
        store == nullptr ? nullptr
                         : dynamic_cast<const yir::ArrayLoadOp *>(store->value()->defining_op());

    for (std::size_t i = 0; i < ops.size(); ++i) {
        auto *op = ops[i].get();
        if (is_latch_helper(op, loop) || i == child_index || op == store) {
            continue;
        }
        if (source_load != nullptr && op == source_load) {
            continue;
        }
        if (is_leaf_pure_op(*op)) {
            continue;
        }
        if (auto *assign = dynamic_cast<const yir::AssignOp *>(op)) {
            if (child_iv != nullptr && assign->target() == child_iv) {
                continue;
            }
        }
        return false;
    }
    return true;
}

bool collect_init_shape_from_loop(const OpList &ops, std::size_t index, InitShape &shape) {
    LoopInfo loop;
    if (!parse_loop_at(ops, index, loop)) {
        return false;
    }

    std::int64_t upper = 0;
    if (!loop_bounds_are_full(loop, upper)) {
        return false;
    }
    shape.loops.push_back({loop.iv, upper});

    auto &body_ops = loop.body->operations();
    std::size_t child_loop_index = body_ops.size();
    yir::ArrayStoreOp *store = nullptr;

    for (std::size_t i = 0; i < body_ops.size(); ++i) {
        auto *op = body_ops[i].get();
        if (is_latch_helper(op, loop)) {
            continue;
        }
        LoopInfo child;
        if (parse_loop_at(body_ops, i, child)) {
            if (child_loop_index != body_ops.size() || store != nullptr) {
                return false;
            }
            child_loop_index = i;
            continue;
        }
        if (auto *array_store = dynamic_cast<yir::ArrayStoreOp *>(op)) {
            if (store != nullptr || child_loop_index != body_ops.size()) {
                return false;
            }
            store = array_store;
        }
    }

    if (child_loop_index != body_ops.size()) {
        if (!validate_loop_payload(body_ops, loop, child_loop_index, nullptr)) {
            return false;
        }
        return collect_init_shape(body_ops, child_loop_index, shape);
    }

    if (store == nullptr) {
        return false;
    }
    if (!validate_loop_payload(body_ops, loop, body_ops.size(), store)) {
        return false;
    }
    shape.store = store;
    return true;
}

bool collect_init_shape(const OpList &ops, std::size_t index, InitShape &shape) {
    return collect_init_shape_from_loop(ops, index, shape);
}

struct SourceIndexExpr {
    std::vector<std::int64_t> coeffs;
    std::int64_t constant = 0;
};

struct ViewCandidate {
    const yir::Operation *root = nullptr;
    yir::Value *view = nullptr;
    yir::Value *source = nullptr;
    std::vector<SourceIndexExpr> source_indices;
};

bool build_candidate(const yir::Operation *root, const InitShape &shape, ViewCandidate &out) {
    if (shape.store == nullptr || shape.loops.empty()) {
        return false;
    }

    auto *source_load = dynamic_cast<const yir::ArrayLoadOp *>(shape.store->value()->defining_op());
    if (source_load == nullptr) {
        return false;
    }

    yir::Value *view = shape.store->array();
    yir::Value *source = source_load->array();
    if (view == nullptr || source == nullptr || view == source || !is_local_array(view) ||
        !is_static_array_storage(source)) {
        return false;
    }

    auto view_dims = static_array_dimensions(view->type());
    if (view_dims.empty() || view_dims.size() != shape.store->indices().size() ||
        view_dims.size() != shape.loops.size()) {
        return false;
    }

    std::unordered_map<const yir::Value *, std::size_t> view_dim_for_iv;
    ValueSet used_ivs;
    auto store_indices = shape.store->indices();
    for (std::size_t dim = 0; dim < store_indices.size(); ++dim) {
        AffineExpr expr;
        if (!parse_affine(store_indices[dim], expr) || expr.constant != 0 ||
            expr.terms.size() != 1) {
            return false;
        }
        const auto [iv, coeff] = *expr.terms.begin();
        if (coeff != 1 || !used_ivs.insert(iv).second) {
            return false;
        }
        auto loop = std::find_if(shape.loops.begin(), shape.loops.end(),
                                 [iv](const LoopDim &candidate) { return candidate.iv == iv; });
        if (loop == shape.loops.end() ||
            static_cast<std::uint64_t>(loop->upper) != view_dims[dim]) {
            return false;
        }
        view_dim_for_iv[iv] = dim;
    }

    std::vector<SourceIndexExpr> source_indices;
    for (auto *index : source_load->indices()) {
        AffineExpr expr;
        if (!parse_affine(index, expr)) {
            return false;
        }
        SourceIndexExpr remap;
        remap.coeffs.assign(view_dims.size(), 0);
        remap.constant = expr.constant;
        if (!fits_i32(remap.constant)) {
            return false;
        }
        for (const auto &[value, coeff] : expr.terms) {
            auto dim = view_dim_for_iv.find(value);
            if (dim == view_dim_for_iv.end()) {
                return false;
            }
            if (!fits_i32(coeff) ||
                !add_checked(remap.coeffs[dim->second], coeff, remap.coeffs[dim->second]) ||
                !fits_i32(remap.coeffs[dim->second])) {
                return false;
            }
        }
        source_indices.push_back(std::move(remap));
    }

    out.root = root;
    out.view = view;
    out.source = source;
    out.source_indices = std::move(source_indices);
    return true;
}

std::vector<ViewCandidate> collect_direct_candidates(yir::Region &region) {
    std::vector<ViewCandidate> candidates;
    const auto &ops = region.operations();
    for (std::size_t i = 0; i < ops.size(); ++i) {
        InitShape shape;
        if (!collect_init_shape(ops, i, shape)) {
            continue;
        }
        ViewCandidate candidate;
        if (build_candidate(ops[i].get(), shape, candidate)) {
            candidates.push_back(std::move(candidate));
        }
    }
    return candidates;
}

void collect_written_arrays(const yir::Operation &op, ValueSet &written);

void collect_written_arrays(const yir::Region &region, ValueSet &written) {
    for (const auto &op : region.operations()) {
        collect_written_arrays(*op, written);
    }
}

void collect_written_arrays(const yir::Operation &op, ValueSet &written) {
    if (auto *store = dynamic_cast<const yir::ArrayStoreOp *>(&op)) {
        written.insert(store->array());
        return;
    }
    if (auto *init = dynamic_cast<const yir::ArrayInitOp *>(&op)) {
        written.insert(init->array());
        return;
    }
    if (auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
        collect_written_arrays(if_op->then_region(), written);
        if (if_op->has_else()) {
            collect_written_arrays(if_op->else_region(), written);
        }
        return;
    }
    if (auto *while_op = dynamic_cast<const yir::WhileOp *>(&op)) {
        collect_written_arrays(while_op->cond_region(), written);
        collect_written_arrays(while_op->body_region(), written);
        return;
    }
    if (auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
        collect_written_arrays(for_op->body_region(), written);
    }
}

void collect_escaped_arrays(const yir::Operation &op, ValueSet &escaped);

void collect_escaped_arrays(const yir::Region &region, ValueSet &escaped) {
    for (const auto &op : region.operations()) {
        collect_escaped_arrays(*op, escaped);
    }
}

void collect_escaped_arrays(const yir::Operation &op, ValueSet &escaped) {
    if (auto *decay = dynamic_cast<const yir::DecayOp *>(&op)) {
        escaped.insert(decay->array_address());
        return;
    }
    if (auto *elem_addr = dynamic_cast<const yir::ElemAddrOp *>(&op)) {
        escaped.insert(elem_addr->base());
        return;
    }
    if (auto *ret = dynamic_cast<const yir::ReturnOp *>(&op)) {
        if (ret->has_value() && ret->value() != nullptr && ret->value()->type() != nullptr &&
            ret->value()->type()->is_array()) {
            escaped.insert(ret->value());
        }
        return;
    }
    if (auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
        collect_escaped_arrays(if_op->then_region(), escaped);
        if (if_op->has_else()) {
            collect_escaped_arrays(if_op->else_region(), escaped);
        }
        return;
    }
    if (auto *while_op = dynamic_cast<const yir::WhileOp *>(&op)) {
        collect_escaped_arrays(while_op->cond_region(), escaped);
        collect_escaped_arrays(while_op->body_region(), escaped);
        return;
    }
    if (auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
        collect_escaped_arrays(for_op->body_region(), escaped);
    }
}

bool op_invalidates_candidate(const yir::Operation &op, const ViewCandidate &candidate);

bool region_invalidates_candidate(const yir::Region &region, const ViewCandidate &candidate) {
    return std::any_of(region.operations().begin(), region.operations().end(),
                       [&](const std::unique_ptr<yir::Operation> &op) {
                           return op_invalidates_candidate(*op, candidate);
                       });
}

bool op_invalidates_candidate(const yir::Operation &op, const ViewCandidate &candidate) {
    if (dynamic_cast<const yir::CallOp *>(&op) != nullptr) {
        return true;
    }
    if (auto *store = dynamic_cast<const yir::ArrayStoreOp *>(&op)) {
        return store->array() == candidate.view || store->array() == candidate.source;
    }
    if (auto *init = dynamic_cast<const yir::ArrayInitOp *>(&op)) {
        return init->array() == candidate.view || init->array() == candidate.source;
    }
    if (auto *decay = dynamic_cast<const yir::DecayOp *>(&op)) {
        return decay->array_address() == candidate.view ||
               decay->array_address() == candidate.source;
    }
    if (auto *elem_addr = dynamic_cast<const yir::ElemAddrOp *>(&op)) {
        return elem_addr->base() == candidate.view || elem_addr->base() == candidate.source;
    }
    if (auto *ret = dynamic_cast<const yir::ReturnOp *>(&op)) {
        return ret->has_value() &&
               (ret->value() == candidate.view || ret->value() == candidate.source);
    }
    if (auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
        return region_invalidates_candidate(if_op->then_region(), candidate) ||
               (if_op->has_else() && region_invalidates_candidate(if_op->else_region(), candidate));
    }
    if (auto *while_op = dynamic_cast<const yir::WhileOp *>(&op)) {
        return region_invalidates_candidate(while_op->cond_region(), candidate) ||
               region_invalidates_candidate(while_op->body_region(), candidate);
    }
    if (auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
        return region_invalidates_candidate(for_op->body_region(), candidate);
    }
    return false;
}

template <typename OpT, typename... Args>
yir::Value *insert_before(OpList &ops, std::size_t &index, Args &&...args) {
    auto op = std::make_unique<OpT>(std::forward<Args>(args)...);
    auto *result = op->result();
    op->set_parent(ops[index]->parent());
    ops.insert(ops.begin() + static_cast<std::ptrdiff_t>(index), std::move(op));
    ++index;
    return result;
}

std::string derived_name(const yir::ArrayLoadOp &load, const char *suffix, std::size_t ordinal) {
    std::string base = load.result() == nullptr || load.result()->name().empty()
                           ? "view.idx"
                           : load.result()->name();
    base += suffix;
    base += std::to_string(ordinal);
    return base;
}

yir::Value *materialize_source_index(const SourceIndexExpr &expr,
                                     const std::vector<yir::Value *> &view_indices, OpList &ops,
                                     std::size_t &index, const yir::ArrayLoadOp &load,
                                     std::size_t ordinal) {
    yir::Value *accum = nullptr;
    if (expr.constant != 0) {
        accum = insert_before<yir::ConstI32Op>(ops, index, static_cast<int>(expr.constant),
                                               derived_name(load, ".c", ordinal));
    }

    for (std::size_t dim = 0; dim < expr.coeffs.size(); ++dim) {
        const auto coeff = expr.coeffs[dim];
        if (coeff == 0) {
            continue;
        }

        yir::Value *term = view_indices[dim];
        if (coeff != 1) {
            auto *scale = insert_before<yir::ConstI32Op>(ops, index, static_cast<int>(coeff),
                                                         derived_name(load, ".scale", ordinal));
            term = insert_before<yir::MulIOp>(ops, index, term, scale,
                                              derived_name(load, ".mul", ordinal));
        }

        if (accum == nullptr) {
            accum = term;
        } else {
            accum = insert_before<yir::AddIOp>(ops, index, accum, term,
                                               derived_name(load, ".add", ordinal));
        }
    }

    if (accum == nullptr) {
        accum = insert_before<yir::ConstI32Op>(ops, index, 0, derived_name(load, ".zero", ordinal));
    }
    return accum;
}

bool rewrite_load(yir::ArrayLoadOp &load, const ViewCandidate &candidate, OpList &ops,
                  std::size_t &index) {
    auto view_indices = load.indices();
    if (view_indices.size() != candidate.source_indices.front().coeffs.size()) {
        return false;
    }

    std::vector<yir::Value *> new_indices;
    new_indices.reserve(candidate.source_indices.size());
    for (std::size_t i = 0; i < candidate.source_indices.size(); ++i) {
        new_indices.push_back(materialize_source_index(candidate.source_indices[i], view_indices,
                                                       ops, index, load, i));
    }

    auto &operands = load.operands();
    operands.clear();
    operands.push_back(candidate.source);
    operands.insert(operands.end(), new_indices.begin(), new_indices.end());
    return true;
}

using CandidateByRoot = std::unordered_map<const yir::Operation *, const ViewCandidate *>;
using ActiveCandidates = std::unordered_map<const yir::Value *, const ViewCandidate *>;

void erase_invalidated(ActiveCandidates &active, const yir::Operation &op) {
    for (auto it = active.begin(); it != active.end();) {
        if (op_invalidates_candidate(op, *it->second)) {
            it = active.erase(it);
        } else {
            ++it;
        }
    }
}

ActiveCandidates candidates_safe_in_loop(const ActiveCandidates &active, const yir::Operation &op) {
    ActiveCandidates nested;
    for (const auto &[view, candidate] : active) {
        if (!op_invalidates_candidate(op, *candidate)) {
            nested.emplace(view, candidate);
        }
    }
    return nested;
}

class Rewriter final {
  public:
    bool run(yir::Module &module) {
        changed_ = false;
        for (auto &function : module.functions()) {
            rewrite_function(*function);
        }
        return changed_;
    }

  private:
    void rewrite_function(yir::Function &function) {
        candidates_ = collect_direct_candidates(function.body());
        candidates_by_root_.clear();
        for (const auto &candidate : candidates_) {
            candidates_by_root_.emplace(candidate.root, &candidate);
        }

        ActiveCandidates active;
        ValueSet written;
        ValueSet escaped;
        rewrite_region(function.body(), active, written, escaped, true);
    }

    void rewrite_region(yir::Region &region, ActiveCandidates &active, ValueSet &written,
                        ValueSet &escaped, bool allow_activation) {
        auto &ops = region.operations();
        for (std::size_t i = 0; i < ops.size(); ++i) {
            auto *op = ops[i].get();

            auto candidate = candidates_by_root_.find(op);
            if (allow_activation && candidate != candidates_by_root_.end()) {
                const auto *view_candidate = candidate->second;
                if (written.find(view_candidate->view) == written.end() &&
                    escaped.find(view_candidate->view) == escaped.end() &&
                    escaped.find(view_candidate->source) == escaped.end()) {
                    active[view_candidate->view] = view_candidate;
                }
                written.insert(view_candidate->view);
                continue;
            }

            if (auto *load = dynamic_cast<yir::ArrayLoadOp *>(op)) {
                auto found = active.find(load->array());
                if (found != active.end() && rewrite_load(*load, *found->second, ops, i)) {
                    changed_ = true;
                    op = ops[i].get();
                }
            }

            if (auto *if_op = dynamic_cast<yir::IfOp *>(op)) {
                ActiveCandidates then_active = active;
                ActiveCandidates else_active = active;
                rewrite_region(if_op->then_region(), then_active, written, escaped, false);
                if (if_op->has_else()) {
                    rewrite_region(if_op->else_region(), else_active, written, escaped, false);
                }
                erase_invalidated(active, *if_op);
            } else if (auto *while_op = dynamic_cast<yir::WhileOp *>(op)) {
                ActiveCandidates nested = candidates_safe_in_loop(active, *while_op);
                rewrite_region(while_op->cond_region(), nested, written, escaped, false);
                rewrite_region(while_op->body_region(), nested, written, escaped, false);
                erase_invalidated(active, *while_op);
            } else if (auto *for_op = dynamic_cast<yir::ForOp *>(op)) {
                ActiveCandidates nested = candidates_safe_in_loop(active, *for_op);
                rewrite_region(for_op->body_region(), nested, written, escaped, false);
                erase_invalidated(active, *for_op);
            } else {
                erase_invalidated(active, *op);
            }

            collect_written_arrays(*op, written);
            collect_escaped_arrays(*op, escaped);
        }
    }

    std::vector<ViewCandidate> candidates_;
    CandidateByRoot candidates_by_root_;
    bool changed_ = false;
};

} // namespace

std::string_view YIRViewPass::name() const {
    return "YIRViewPass";
}

PassKind YIRViewPass::kind() const {
    return PassKind::Transform;
}

PassResult YIRViewPass::run(PassContext &context) {
    auto *artifact = context.get_artifact<std::unique_ptr<yir::Module>>(ASTToYIRPass::kArtifactKey);
    if (artifact == nullptr || *artifact == nullptr) {
        return PassResult::fail("YIRViewPass requires YIR module in pass context");
    }

    Rewriter rewriter;
    const bool changed = rewriter.run(**artifact);
    if (changed) {
        auto verify = yir::verify_high_level_yir(**artifact);
        if (!verify.success) {
            return PassResult::fail(verify_message(verify));
        }
    }

    return PassResult::ok(changed);
}

} // namespace pass
