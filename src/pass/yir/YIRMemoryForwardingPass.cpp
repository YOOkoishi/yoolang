#include "pass/yir/YIRMemoryForwardingPass.h"

#include "pass/ast/ASTToYIRPass.h"
#include "yir/YIR.h"
#include "yir/YIRVerifier.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pass {
namespace {

using ValueSet = std::unordered_set<const yir::Value *>;

struct AffineTerm {
    const yir::Value *value = nullptr;
    std::int64_t coefficient = 0;
};

struct AffineExpr {
    bool valid = true;
    std::int64_t constant = 0;
    std::vector<AffineTerm> terms;
};

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

AffineExpr invalid_affine() {
    AffineExpr expr;
    expr.valid = false;
    return expr;
}

void normalize_affine(AffineExpr &expr) {
    if (!expr.valid) {
        return;
    }
    std::sort(expr.terms.begin(), expr.terms.end(),
              [](const AffineTerm &lhs, const AffineTerm &rhs) {
                  return lhs.value < rhs.value;
              });
    std::vector<AffineTerm> normalized;
    for (const auto &term : expr.terms) {
        if (term.coefficient == 0) {
            continue;
        }
        if (!normalized.empty() && normalized.back().value == term.value) {
            normalized.back().coefficient += term.coefficient;
            if (normalized.back().coefficient == 0) {
                normalized.pop_back();
            }
        } else {
            normalized.push_back(term);
        }
    }
    expr.terms = std::move(normalized);
}

void add_term(AffineExpr &expr, const yir::Value *value, std::int64_t coefficient) {
    if (!expr.valid || value == nullptr || coefficient == 0) {
        return;
    }
    expr.terms.push_back({value, coefficient});
    normalize_affine(expr);
}

AffineExpr add_expr(AffineExpr lhs, const AffineExpr &rhs, std::int64_t scale = 1) {
    if (!lhs.valid || !rhs.valid) {
        return invalid_affine();
    }
    lhs.constant += rhs.constant * scale;
    for (const auto &term : rhs.terms) {
        lhs.terms.push_back({term.value, term.coefficient * scale});
    }
    normalize_affine(lhs);
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
    normalize_affine(expr);
    return expr;
}

AffineExpr affine_expr(const yir::Value *value, ValueSet &active) {
    if (value == nullptr || !active.insert(value).second) {
        return invalid_affine();
    }
    std::int64_t constant = 0;
    if (const_i32_value(value, constant)) {
        AffineExpr expr;
        expr.constant = constant;
        return expr;
    }

    auto *def = value->defining_op();
    if (auto *add = dynamic_cast<const yir::AddIOp *>(def)) {
        return add_expr(affine_expr(add->lhs(), active), affine_expr(add->rhs(), active));
    }
    if (auto *sub = dynamic_cast<const yir::SubIOp *>(def)) {
        return add_expr(affine_expr(sub->lhs(), active), affine_expr(sub->rhs(), active), -1);
    }
    if (auto *mul = dynamic_cast<const yir::MulIOp *>(def)) {
        std::int64_t scale = 0;
        if (const_i32_value(mul->lhs(), scale)) {
            return scale_expr(affine_expr(mul->rhs(), active), scale);
        }
        if (const_i32_value(mul->rhs(), scale)) {
            return scale_expr(affine_expr(mul->lhs(), active), scale);
        }
        return invalid_affine();
    }

    AffineExpr expr;
    add_term(expr, value, 1);
    return expr;
}

AffineExpr affine_expr(const yir::Value *value) {
    ValueSet active;
    return affine_expr(value, active);
}

bool affine_equal(const AffineExpr &lhs, const AffineExpr &rhs) {
    if (!lhs.valid || !rhs.valid || lhs.constant != rhs.constant ||
        lhs.terms.size() != rhs.terms.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.terms.size(); ++i) {
        if (lhs.terms[i].value != rhs.terms[i].value ||
            lhs.terms[i].coefficient != rhs.terms[i].coefficient) {
            return false;
        }
    }
    return true;
}

bool affine_same_terms_different_constant(const AffineExpr &lhs, const AffineExpr &rhs) {
    if (!lhs.valid || !rhs.valid || lhs.constant == rhs.constant ||
        lhs.terms.size() != rhs.terms.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.terms.size(); ++i) {
        if (lhs.terms[i].value != rhs.terms[i].value ||
            lhs.terms[i].coefficient != rhs.terms[i].coefficient) {
            return false;
        }
    }
    return true;
}

bool same_index(const yir::Value *lhs, const yir::Value *rhs) {
    if (lhs == rhs) {
        return true;
    }
    return affine_equal(affine_expr(lhs), affine_expr(rhs));
}

bool distinct_index(const yir::Value *lhs, const yir::Value *rhs) {
    return affine_same_terms_different_constant(affine_expr(lhs), affine_expr(rhs));
}

enum class AccessKind {
    ArrayElement,
    Address,
};

struct Access {
    AccessKind kind = AccessKind::ArrayElement;
    const yir::Value *base = nullptr;
    std::vector<const yir::Value *> indices;
};

struct MemoryEntry {
    Access access;
    yir::Value *value = nullptr;
};

using MemoryState = std::vector<MemoryEntry>;

bool is_local_unique_object(const yir::Value *value) {
    auto *def = value == nullptr ? nullptr : value->defining_op();
    return dynamic_cast<const yir::ArrayVarOp *>(def) != nullptr ||
           dynamic_cast<const yir::AllocaOp *>(def) != nullptr;
}

bool is_global_object(const yir::Value *value) {
    return value != nullptr && value->defining_op() == nullptr && !value->name().empty() &&
           value->name().front() == '@';
}

bool different_bases_noalias(const yir::Value *lhs, const yir::Value *rhs) {
    if (lhs == rhs) {
        return false;
    }
    if (is_local_unique_object(lhs) || is_local_unique_object(rhs)) {
        return true;
    }
    return is_global_object(lhs) && is_global_object(rhs);
}

bool same_access(const Access &lhs, const Access &rhs) {
    if (lhs.kind != rhs.kind || lhs.base != rhs.base ||
        lhs.indices.size() != rhs.indices.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.indices.size(); ++i) {
        if (!same_index(lhs.indices[i], rhs.indices[i])) {
            return false;
        }
    }
    return true;
}

bool noalias_access(const Access &lhs, const Access &rhs) {
    if (lhs.kind != rhs.kind) {
        return different_bases_noalias(lhs.base, rhs.base);
    }
    if (lhs.base != rhs.base) {
        return different_bases_noalias(lhs.base, rhs.base);
    }
    if (lhs.kind == AccessKind::Address) {
        return false;
    }
    if (lhs.indices.size() != rhs.indices.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.indices.size(); ++i) {
        if (distinct_index(lhs.indices[i], rhs.indices[i])) {
            return true;
        }
    }
    return false;
}

std::vector<const yir::Value *> as_const_indices(const std::vector<yir::Value *> &indices) {
    std::vector<const yir::Value *> out;
    out.reserve(indices.size());
    for (auto *index : indices) {
        out.push_back(index);
    }
    return out;
}

bool access_from_address(const yir::Value *address, Access &out) {
    if (address == nullptr) {
        return false;
    }
    if (auto *elem = dynamic_cast<const yir::ElemAddrOp *>(address->defining_op())) {
        out.kind = AccessKind::ArrayElement;
        out.base = elem->base();
        out.indices = as_const_indices(elem->indices());
        return true;
    }
    out.kind = AccessKind::Address;
    out.base = address;
    out.indices.clear();
    return true;
}

bool load_access(const yir::Operation &op, Access &out, yir::Value *&loaded_value) {
    if (auto *load = dynamic_cast<const yir::ArrayLoadOp *>(&op)) {
        out.kind = AccessKind::ArrayElement;
        out.base = load->array();
        out.indices = as_const_indices(load->indices());
        loaded_value = const_cast<yir::Value *>(load->result());
        return loaded_value != nullptr;
    }
    if (auto *load = dynamic_cast<const yir::LoadOp *>(&op)) {
        loaded_value = const_cast<yir::Value *>(load->result());
        return loaded_value != nullptr && access_from_address(load->address(), out);
    }
    return false;
}

bool store_access(const yir::Operation &op, Access &out, yir::Value *&stored_value) {
    if (auto *store = dynamic_cast<const yir::ArrayStoreOp *>(&op)) {
        out.kind = AccessKind::ArrayElement;
        out.base = store->array();
        out.indices = as_const_indices(store->indices());
        stored_value = store->value();
        return stored_value != nullptr;
    }
    if (auto *store = dynamic_cast<const yir::StoreOp *>(&op)) {
        stored_value = store->value();
        return stored_value != nullptr && access_from_address(store->address(), out);
    }
    return false;
}

bool value_references_base(const yir::Value *value, const yir::Value *base, ValueSet &active) {
    if (value == nullptr || base == nullptr) {
        return false;
    }
    if (value == base) {
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
        if (value_references_base(operand, base, active)) {
            return true;
        }
    }
    return false;
}

bool value_references_base(const yir::Value *value, const yir::Value *base) {
    ValueSet active;
    return value_references_base(value, base, active);
}

bool call_may_clobber(const yir::CallOp &call, const Access &access) {
    if (is_local_unique_object(access.base)) {
        return std::any_of(call.args().begin(), call.args().end(), [&](const yir::Value *arg) {
            return value_references_base(arg, access.base);
        });
    }
    return true;
}

void replace_operands(yir::Operation &op, yir::Value *old_value, yir::Value *new_value) {
    for (auto *&operand : op.operands()) {
        if (operand == old_value) {
            operand = new_value;
        }
    }
}

void replace_value_in_region(yir::Region &region, yir::Value *old_value, yir::Value *new_value);

void replace_value_in_nested_regions(yir::Operation &op, yir::Value *old_value,
                                     yir::Value *new_value) {
    if (auto *if_op = dynamic_cast<yir::IfOp *>(&op)) {
        replace_value_in_region(if_op->then_region(), old_value, new_value);
        if (if_op->has_else()) {
            replace_value_in_region(if_op->else_region(), old_value, new_value);
        }
    } else if (auto *while_op = dynamic_cast<yir::WhileOp *>(&op)) {
        replace_value_in_region(while_op->cond_region(), old_value, new_value);
        replace_value_in_region(while_op->body_region(), old_value, new_value);
    } else if (auto *for_op = dynamic_cast<yir::ForOp *>(&op)) {
        replace_value_in_region(for_op->body_region(), old_value, new_value);
    }
}

void replace_value_in_region(yir::Region &region, yir::Value *old_value, yir::Value *new_value) {
    for (auto &op : region.operations()) {
        replace_operands(*op, old_value, new_value);
        replace_value_in_nested_regions(*op, old_value, new_value);
    }
}

void replace_value_after(yir::Region &region, std::size_t first_op, yir::Value *old_value,
                         yir::Value *new_value) {
    auto &ops = region.operations();
    for (std::size_t i = first_op; i < ops.size(); ++i) {
        replace_operands(*ops[i], old_value, new_value);
        replace_value_in_nested_regions(*ops[i], old_value, new_value);
    }
}

void invalidate_aliasing(MemoryState &state, const Access &access) {
    state.erase(std::remove_if(state.begin(), state.end(),
                               [&](const MemoryEntry &entry) {
                                   return !noalias_access(entry.access, access);
                               }),
                state.end());
}

void remember_value(MemoryState &state, const Access &access, yir::Value *value) {
    for (auto &entry : state) {
        if (same_access(entry.access, access)) {
            entry.value = value;
            return;
        }
    }
    state.push_back({access, value});
}

void invalidate_for_call(MemoryState &state, const yir::CallOp &call) {
    state.erase(std::remove_if(state.begin(), state.end(),
                               [&](const MemoryEntry &entry) {
                                   return call_may_clobber(call, entry.access);
                               }),
                state.end());
}

bool access_depends_on(const Access &access, const yir::Value *value) {
    if (value_references_base(access.base, value)) {
        return true;
    }
    return std::any_of(access.indices.begin(), access.indices.end(), [&](const yir::Value *index) {
        return value_references_base(index, value);
    });
}

void invalidate_for_assignment(MemoryState &state, const yir::Value *target) {
    if (target == nullptr) {
        return;
    }
    state.erase(std::remove_if(state.begin(), state.end(),
                               [&](const MemoryEntry &entry) {
                                   return access_depends_on(entry.access, target) ||
                                          value_references_base(entry.value, target);
                               }),
                state.end());
}

MemoryState merge_states(const MemoryState &lhs, const MemoryState &rhs) {
    MemoryState merged;
    for (const auto &left : lhs) {
        auto found = std::find_if(rhs.begin(), rhs.end(), [&](const MemoryEntry &right) {
            return left.value == right.value && same_access(left.access, right.access);
        });
        if (found != rhs.end()) {
            merged.push_back(left);
        }
    }
    return merged;
}

std::string verify_message(const yir::VerifyResult &result) {
    std::ostringstream oss;
    oss << "YIR verification failed";
    for (const auto &error : result.errors) {
        oss << "\n" << error;
    }
    return oss.str();
}

class Forwarder final {
  public:
    bool run(yir::Module &module) {
        changed_ = false;
        for (auto &function : module.functions()) {
            MemoryState state;
            forward_region(function->body(), state);
        }
        return changed_;
    }

  private:
    void forward_region(yir::Region &region, MemoryState &state) {
        auto &ops = region.operations();
        for (std::size_t i = 0; i < ops.size();) {
            auto *op = ops[i].get();

            Access access;
            yir::Value *loaded_value = nullptr;
            if (load_access(*op, access, loaded_value)) {
                auto found = std::find_if(state.rbegin(), state.rend(),
                                          [&](const MemoryEntry &entry) {
                                              return same_access(entry.access, access);
                                          });
                if (found != state.rend() && found->value != nullptr &&
                    found->value->type() == loaded_value->type() && found->value != loaded_value) {
                    replace_value_after(region, i + 1, loaded_value, found->value);
                    ops.erase(ops.begin() + static_cast<std::ptrdiff_t>(i));
                    changed_ = true;
                    continue;
                }
                remember_value(state, access, loaded_value);
                ++i;
                continue;
            }

            yir::Value *stored_value = nullptr;
            if (store_access(*op, access, stored_value)) {
                invalidate_aliasing(state, access);
                remember_value(state, access, stored_value);
                ++i;
                continue;
            }

            if (auto *if_op = dynamic_cast<yir::IfOp *>(op)) {
                MemoryState then_state = state;
                MemoryState else_state = state;
                forward_region(if_op->then_region(), then_state);
                if (if_op->has_else()) {
                    forward_region(if_op->else_region(), else_state);
                }
                state = merge_states(then_state, else_state);
                ++i;
                continue;
            }

            if (auto *while_op = dynamic_cast<yir::WhileOp *>(op)) {
                MemoryState nested_state;
                forward_region(while_op->cond_region(), nested_state);
                nested_state.clear();
                forward_region(while_op->body_region(), nested_state);
                invalidate_for_region_effects(while_op->cond_region(), state);
                invalidate_for_region_effects(while_op->body_region(), state);
                ++i;
                continue;
            }

            if (auto *for_op = dynamic_cast<yir::ForOp *>(op)) {
                MemoryState nested_state;
                forward_region(for_op->body_region(), nested_state);
                invalidate_for_region_effects(for_op->body_region(), state);
                ++i;
                continue;
            }

            if (auto *call = dynamic_cast<yir::CallOp *>(op)) {
                invalidate_for_call(state, *call);
                ++i;
                continue;
            }

            if (auto *assign = dynamic_cast<yir::AssignOp *>(op)) {
                invalidate_for_assignment(state, assign->target());
                ++i;
                continue;
            }

            if (auto *init = dynamic_cast<yir::ArrayInitOp *>(op)) {
                Access whole_array;
                whole_array.kind = AccessKind::ArrayElement;
                whole_array.base = init->array();
                invalidate_aliasing(state, whole_array);
                ++i;
                continue;
            }

            ++i;
        }
    }

    void invalidate_for_region_effects(yir::Region &region, MemoryState &state) {
        for (auto &op : region.operations()) {
            Access access;
            yir::Value *stored_value = nullptr;
            if (store_access(*op, access, stored_value)) {
                invalidate_aliasing(state, access);
                continue;
            }
            if (auto *init = dynamic_cast<yir::ArrayInitOp *>(op.get())) {
                Access whole_array;
                whole_array.kind = AccessKind::ArrayElement;
                whole_array.base = init->array();
                invalidate_aliasing(state, whole_array);
                continue;
            }
            if (auto *call = dynamic_cast<yir::CallOp *>(op.get())) {
                invalidate_for_call(state, *call);
                continue;
            }
            if (auto *assign = dynamic_cast<yir::AssignOp *>(op.get())) {
                invalidate_for_assignment(state, assign->target());
                continue;
            }
            if (auto *if_op = dynamic_cast<yir::IfOp *>(op.get())) {
                MemoryState then_state = state;
                MemoryState else_state = state;
                invalidate_for_region_effects(if_op->then_region(), then_state);
                if (if_op->has_else()) {
                    invalidate_for_region_effects(if_op->else_region(), else_state);
                }
                state = merge_states(then_state, else_state);
            } else if (auto *while_op = dynamic_cast<yir::WhileOp *>(op.get())) {
                invalidate_for_region_effects(while_op->cond_region(), state);
                invalidate_for_region_effects(while_op->body_region(), state);
            } else if (auto *for_op = dynamic_cast<yir::ForOp *>(op.get())) {
                invalidate_for_region_effects(for_op->body_region(), state);
            }
        }
    }

    bool changed_ = false;
};

} // namespace

std::string_view YIRMemoryForwardingPass::name() const {
    return "YIRMemoryForwardingPass";
}

PassKind YIRMemoryForwardingPass::kind() const {
    return PassKind::Transform;
}

PassResult YIRMemoryForwardingPass::run(PassContext &context) {
    auto *artifact = context.get_artifact<std::unique_ptr<yir::Module>>(ASTToYIRPass::kArtifactKey);
    if (artifact == nullptr || *artifact == nullptr) {
        return PassResult::fail("YIRMemoryForwardingPass requires YIR module in pass context");
    }

    Forwarder forwarder;
    const bool changed = forwarder.run(**artifact);
    if (changed) {
        auto verify = yir::verify_high_level_yir(**artifact);
        if (!verify.success) {
            return PassResult::fail(verify_message(verify));
        }
    }

    return PassResult::ok(changed);
}

} // namespace pass
