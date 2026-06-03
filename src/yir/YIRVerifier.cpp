#include "yir/YIRVerifier.h"

#include <set>
#include <sstream>

namespace yir {
namespace {

bool same_type(const TypePtr &lhs, const TypePtr &rhs) {
    if (lhs == rhs) {
        return true;
    }
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    return lhs->str() == rhs->str();
}

TypePtr indexed_element_type(TypePtr base_type, std::size_t index_count) {
    TypePtr current = std::move(base_type);
    if (current != nullptr && current->is_ptr()) {
        if (index_count == 0) {
            return current->pointee();
        }
        current = current->pointee();
        --index_count;
    }
    for (std::size_t i = 0; i < index_count; ++i) {
        if (current == nullptr || !current->is_array()) {
            return nullptr;
        }
        current = current->element();
    }
    return current;
}

std::size_t indexable_rank(TypePtr type) {
    if (type == nullptr) {
        return 0;
    }
    if (type->is_ptr()) {
        return 1 + array_rank(type->pointee());
    }
    return array_rank(type);
}

std::vector<std::uint64_t> static_array_dimensions(TypePtr type) {
    std::vector<std::uint64_t> dims;
    while (type != nullptr && type->is_array()) {
        dims.push_back(type->count());
        type = type->element();
    }
    return dims;
}

const ConstI32Op *const_i32_def(const Value *value) {
    if (value == nullptr || value->defining_op() == nullptr) {
        return nullptr;
    }
    return dynamic_cast<const ConstI32Op *>(value->defining_op());
}

std::string coord_key(const std::vector<std::uint64_t> &indices) {
    std::ostringstream oss;
    for (std::uint64_t index : indices) {
        oss << index << ',';
    }
    return oss.str();
}

class Verifier {
  public:
    VerifyResult verify(const Module &module) {
        for (const auto &function : module.functions()) {
            verify_region(function->body());
        }
        result_.success = result_.errors.empty();
        return result_;
    }

  private:
    void error(std::string message) {
        result_.errors.push_back(std::move(message));
    }

    void verify_region(const Region &region) {
        for (const auto &op : region.operations()) {
            verify_op(*op);
        }
    }

    void verify_op(const Operation &op) {
        if (dynamic_cast<const AllocaOp *>(&op) != nullptr) {
            error("high-level YIR must not use yir.alloca for source-level locals");
        }
        if (const auto *load = dynamic_cast<const ArrayLoadOp *>(&op)) {
            verify_array_load(*load);
        }
        if (const auto *store = dynamic_cast<const ArrayStoreOp *>(&op)) {
            verify_array_store(*store);
        }
        if (const auto *init = dynamic_cast<const ArrayInitOp *>(&op)) {
            verify_array_init(*init);
        }
        if (const auto *if_op = dynamic_cast<const IfOp *>(&op)) {
            verify_region(if_op->then_region());
            if (if_op->has_else()) {
                verify_region(if_op->else_region());
            }
        }
        if (const auto *while_op = dynamic_cast<const WhileOp *>(&op)) {
            verify_region(while_op->cond_region());
            verify_region(while_op->body_region());
        }
        if (const auto *for_op = dynamic_cast<const ForOp *>(&op)) {
            verify_region(for_op->body_region());
        }
    }

    void verify_array_load(const ArrayLoadOp &op) {
        auto indices = op.indices();
        std::size_t rank = indexable_rank(op.array()->type());
        if (indices.size() != rank) {
            error("yir.array_load index count does not match array rank");
            return;
        }
        TypePtr element = indexed_element_type(op.array()->type(), indices.size());
        if (!same_type(element, op.result()->type())) {
            error("yir.array_load result type does not match indexed element type");
        }
        verify_constant_bounds(op.array()->type(), indices, "yir.array_load");
    }

    void verify_array_store(const ArrayStoreOp &op) {
        auto indices = op.indices();
        std::size_t rank = indexable_rank(op.array()->type());
        if (indices.size() != rank) {
            error("yir.array_store index count does not match array rank");
            return;
        }
        TypePtr element = indexed_element_type(op.array()->type(), indices.size());
        if (!same_type(element, op.value()->type())) {
            error("yir.array_store value type does not match indexed element type");
        }
        verify_constant_bounds(op.array()->type(), indices, "yir.array_store");
    }

    void verify_array_init(const ArrayInitOp &op) {
        auto dims = static_array_dimensions(op.array_type());
        TypePtr element = op.array_type();
        while (element != nullptr && element->is_array()) {
            element = element->element();
        }

        std::set<std::string> seen;
        for (const auto &entry : op.entries()) {
            if (entry.indices.size() != dims.size()) {
                error("yir.array_init entry rank does not match array rank");
                continue;
            }
            for (std::size_t i = 0; i < entry.indices.size(); ++i) {
                if (entry.indices[i] >= dims[i]) {
                    error("yir.array_init entry index is out of bounds");
                }
            }
            if (entry.value != nullptr && !same_type(element, entry.value->type())) {
                error("yir.array_init entry value type does not match element type");
            }
            if (!seen.insert(coord_key(entry.indices)).second) {
                error("yir.array_init contains duplicate index entry");
            }
        }
    }

    void verify_constant_bounds(TypePtr base_type, const std::vector<Value *> &indices,
                                const char *op_name) {
        auto dims = static_array_dimensions(base_type);
        if (dims.size() != indices.size()) {
            return;
        }
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const auto *constant = const_i32_def(indices[i]);
            if (constant != nullptr && constant->value() >= 0 &&
                static_cast<std::uint64_t>(constant->value()) >= dims[i]) {
                error(std::string(op_name) + " constant index is out of bounds");
            }
        }
    }

    VerifyResult result_;
};

} // namespace

VerifyResult verify_high_level_yir(const Module &module) {
    Verifier verifier;
    return verifier.verify(module);
}

} // namespace yir
