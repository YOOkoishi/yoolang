#include "yir/YIRVerifier.h"

#include <set>
#include <sstream>

namespace yir {
namespace {

bool same_type(const TypePtr &lhs, const TypePtr &rhs) {
    return lhs == rhs;
}

bool is_i1(const TypePtr &type) {
    return type == Type::get_i1();
}

bool is_i32(const TypePtr &type) {
    return type == Type::get_i32();
}

bool is_integer_vector(const TypePtr &type) {
    return type != nullptr && type->is_vector() && type->element() == Type::get_i32();
}

bool is_float_vector(const TypePtr &type) {
    return type != nullptr && type->is_vector() && type->element() == Type::get_f32();
}

bool is_numeric_vector(const TypePtr &type) {
    return is_integer_vector(type) || is_float_vector(type);
}

bool is_integer_value(const TypePtr &type) {
    return is_i32(type) || is_integer_vector(type);
}

bool is_float_value(const TypePtr &type) {
    return type == Type::get_f32() || is_float_vector(type);
}

bool is_power_of_two(std::uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool matching_mask(const TypePtr &mask, const TypePtr &vector) {
    return mask != nullptr && vector != nullptr && mask->is_mask() &&
           (vector->is_vector() || vector->is_mask()) && mask->count() == vector->count();
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

bool constant_tree_matches_type(const Constant &constant, const TypePtr &expected,
                                std::string &message) {
    if (constant.type() != expected) {
        message = "typed YIR initializer type does not match storage type";
        return false;
    }
    if (dynamic_cast<const ConstantAggregateZero *>(&constant) != nullptr) {
        return true;
    }
    if (const auto *integer = dynamic_cast<const ConstantInt *>(&constant)) {
        (void)integer;
        if (expected != Type::get_i1() && expected != Type::get_i32()) {
            message = "YIR integer constant has non-integer type";
            return false;
        }
        return true;
    }
    if (dynamic_cast<const ConstantFloat *>(&constant) != nullptr) {
        if (expected != Type::get_f32()) {
            message = "YIR float constant has non-f32 type";
            return false;
        }
        return true;
    }
    if (const auto *array = dynamic_cast<const ConstantArray *>(&constant)) {
        if (!expected->is_array() || array->elements().size() != expected->count()) {
            message = "YIR array constant shape does not match type";
            return false;
        }
        for (const auto &element : array->elements()) {
            if (element == nullptr ||
                !constant_tree_matches_type(*element, expected->element(), message)) {
                return false;
            }
        }
        return true;
    }
    if (const auto *vector = dynamic_cast<const ConstantVector *>(&constant)) {
        if (!expected->is_vector() || vector->lanes().size() != expected->count()) {
            message = "YIR vector constant shape does not match type";
            return false;
        }
        for (const auto &lane : vector->lanes()) {
            if (lane == nullptr ||
                !constant_tree_matches_type(*lane, expected->element(), message)) {
                return false;
            }
        }
        return true;
    }
    if (dynamic_cast<const ConstantMask *>(&constant) != nullptr) {
        if (!expected->is_mask()) {
            message = "YIR mask constant has non-mask type";
            return false;
        }
        return true;
    }
    message = "unknown YIR typed constant subclass";
    return false;
}

class Verifier {
  public:
    VerifyResult verify(const Module &module) {
        for (const auto &global : module.globals()) {
            if (global->storage_type() == nullptr || global->storage_type()->is_void() ||
                global->storage_type()->kind() == Type::Kind::Func) {
                error("yir.global has invalid storage type");
            }
            if (global->typed_initializer() != nullptr) {
                std::string message;
                if (!constant_tree_matches_type(*global->typed_initializer(),
                                                global->storage_type(), message)) {
                    error(std::move(message));
                }
            }
        }
        for (const auto &function : module.functions()) {
            if (function->return_type() == nullptr) {
                error("yir.func has null return type");
                continue;
            }
            if (function->params().size() != function->param_types().size()) {
                error("yir.func parameter list does not match function type");
            }
            for (std::size_t i = 0;
                 i < function->params().size() && i < function->param_types().size(); ++i) {
                if (!same_type(function->params()[i]->type(), function->param_types()[i])) {
                    error("yir.func parameter value type does not match signature");
                }
            }
            if (function->is_external()) {
                if (!function->body().operations().empty()) {
                    error("yir.declare must not contain a function body");
                }
                continue;
            }
            verify_region(function->body(), function->return_type());
        }
        result_.success = result_.errors.empty();
        return result_;
    }

  private:
    void error(std::string message) {
        result_.errors.push_back(std::move(message));
    }

    void verify_region(const Region &region, const TypePtr &return_type) {
        for (const auto &op : region.operations()) {
            verify_op(*op, return_type);
        }
    }

    void verify_op(const Operation &op, const TypePtr &return_type) {
        for (const auto *operand : op.operands()) {
            if (operand == nullptr || operand->type() == nullptr) {
                error(op.op_name() + " has a null or untyped operand");
            }
        }
        if (op.result() != nullptr && op.result()->type() == nullptr) {
            error(op.op_name() + " has an untyped result");
        }
        if (dynamic_cast<const AllocaOp *>(&op) != nullptr) {
            error("high-level YIR must not use yir.alloca for source-level locals");
        }
        if (const auto *var = dynamic_cast<const VarOp *>(&op)) {
            if (var->has_initializer() &&
                !same_type(var->result()->type(), var->initializer()->type())) {
                error("yir.var initializer type does not match variable type");
            }
        }
        if (const auto *assign = dynamic_cast<const AssignOp *>(&op)) {
            if (!same_type(assign->target()->type(), assign->value()->type())) {
                error("yir.assign source and destination types do not match");
            }
        }
        if (const auto *load = dynamic_cast<const LoadOp *>(&op)) {
            if (!load->address()->type()->is_ptr() ||
                !same_type(load->address()->type()->pointee(), load->result()->type())) {
                error("yir.load pointer pointee does not match result type");
            }
        }
        if (const auto *store = dynamic_cast<const StoreOp *>(&op)) {
            if (!store->address()->type()->is_ptr() ||
                !same_type(store->address()->type()->pointee(), store->value()->type())) {
                error("yir.store pointer pointee does not match value type");
            }
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
        verify_scalar_or_vector_operation(op);
        verify_vector_operation(op);
        if (const auto *cond = dynamic_cast<const CondOp *>(&op)) {
            if (!is_i1(cond->condition()->type())) {
                error("yir.cond requires scalar i1, not vector mask");
            }
        }
        if (const auto *ret = dynamic_cast<const ReturnOp *>(&op)) {
            if (return_type->is_void() != !ret->has_value() ||
                (ret->has_value() && !same_type(ret->value()->type(), return_type))) {
                error("yir.return value does not match function return type");
            }
        }
        if (const auto *if_op = dynamic_cast<const IfOp *>(&op)) {
            if (!is_i1(if_op->condition()->type())) {
                error("yir.if requires scalar i1, not vector mask");
            }
            verify_region(if_op->then_region(), return_type);
            if (if_op->has_else()) {
                verify_region(if_op->else_region(), return_type);
            }
        }
        if (const auto *while_op = dynamic_cast<const WhileOp *>(&op)) {
            verify_region(while_op->cond_region(), return_type);
            verify_region(while_op->body_region(), return_type);
        }
        if (const auto *for_op = dynamic_cast<const ForOp *>(&op)) {
            if (!is_i32(for_op->induction_var()->type()) ||
                !is_i32(for_op->lower_bound()->type()) || !is_i32(for_op->upper_bound()->type()) ||
                !is_i32(for_op->step()->type())) {
                error("yir.for induction and bounds must be scalar i32");
            }
            verify_region(for_op->body_region(), return_type);
        }
    }

    void verify_scalar_or_vector_operation(const Operation &op) {
        if (const auto *binary = dynamic_cast<const BinaryOpBase *>(&op)) {
            const bool integer_op = dynamic_cast<const AddIOp *>(&op) != nullptr ||
                                    dynamic_cast<const SubIOp *>(&op) != nullptr ||
                                    dynamic_cast<const MulIOp *>(&op) != nullptr ||
                                    dynamic_cast<const DivSIOp *>(&op) != nullptr ||
                                    dynamic_cast<const RemSIOp *>(&op) != nullptr ||
                                    dynamic_cast<const AndIOp *>(&op) != nullptr ||
                                    dynamic_cast<const OrIOp *>(&op) != nullptr ||
                                    dynamic_cast<const XorIOp *>(&op) != nullptr;
            const bool float_op = dynamic_cast<const AddFOp *>(&op) != nullptr ||
                                  dynamic_cast<const SubFOp *>(&op) != nullptr ||
                                  dynamic_cast<const MulFOp *>(&op) != nullptr ||
                                  dynamic_cast<const DivFOp *>(&op) != nullptr;
            const bool mask_op = dynamic_cast<const MaskBinaryOp *>(&op) != nullptr;
            if ((integer_op || float_op || mask_op) &&
                (!same_type(binary->lhs()->type(), binary->rhs()->type()) ||
                 !same_type(binary->lhs()->type(), binary->result()->type()))) {
                error(op.op_name() + " operand/result shapes do not match");
                return;
            }
            if (integer_op && !is_integer_value(binary->lhs()->type())) {
                error(op.op_name() + " requires scalar/vector i32 operands");
            }
            if (float_op && !is_float_value(binary->lhs()->type())) {
                error(op.op_name() + " requires scalar/vector f32 operands");
            }
        }
        if (const auto *compare = dynamic_cast<const ICmpOp *>(&op)) {
            if (!same_type(compare->lhs()->type(), compare->rhs()->type()) ||
                !is_integer_value(compare->lhs()->type())) {
                error("yir.icmp requires matching scalar/vector i32 operands");
            }
            TypePtr expected = compare->lhs()->type()->is_vector()
                                   ? Type::get_mask(compare->lhs()->type()->count())
                                   : Type::get_i1();
            if (!same_type(expected, compare->result()->type())) {
                error("yir.icmp result must be scalar i1 or same-shape mask");
            }
        }
        if (const auto *compare = dynamic_cast<const FCmpOp *>(&op)) {
            if (!same_type(compare->lhs()->type(), compare->rhs()->type()) ||
                !is_float_value(compare->lhs()->type())) {
                error("yir.fcmp requires matching scalar/vector f32 operands");
            }
            TypePtr expected = compare->lhs()->type()->is_vector()
                                   ? Type::get_mask(compare->lhs()->type()->count())
                                   : Type::get_i1();
            if (!same_type(expected, compare->result()->type())) {
                error("yir.fcmp result must be scalar i1 or same-shape mask");
            }
        }
        if (const auto *mask_binary = dynamic_cast<const MaskBinaryOp *>(&op)) {
            if (!mask_binary->lhs()->type()->is_mask() ||
                !same_type(mask_binary->lhs()->type(), mask_binary->rhs()->type()) ||
                !same_type(mask_binary->lhs()->type(), mask_binary->result()->type())) {
                error(op.op_name() + " requires matching mask operands and result");
            }
        }

        if (dynamic_cast<const ZExtI1ToI32Op *>(&op) != nullptr) {
            const auto &source = op.operands()[0]->type();
            const auto &dest = op.result()->type();
            if (!((is_i1(source) && is_i32(dest)) ||
                  (source->is_mask() && is_integer_vector(dest) &&
                   source->count() == dest->count()))) {
                error("yir.zext_i1_to_i32 requires i1->i32 or mask->i32 vector");
            }
        }
        if (dynamic_cast<const TruncI32ToI1Op *>(&op) != nullptr) {
            const auto &source = op.operands()[0]->type();
            const auto &dest = op.result()->type();
            if (!((is_i32(source) && is_i1(dest)) ||
                  (is_integer_vector(source) && dest->is_mask() &&
                   source->count() == dest->count()))) {
                error("yir.trunc_i32_to_i1 requires i32->i1 or i32 vector->mask");
            }
        }
        if (dynamic_cast<const SIToFPOp *>(&op) != nullptr) {
            const auto &source = op.operands()[0]->type();
            const auto &dest = op.result()->type();
            if (!((is_i32(source) && dest == Type::get_f32()) ||
                  (is_integer_vector(source) && is_float_vector(dest) &&
                   source->count() == dest->count()))) {
                error("yir.sitofp requires same-shape i32 to f32 conversion");
            }
        }
        if (dynamic_cast<const FPToSIOp *>(&op) != nullptr) {
            const auto &source = op.operands()[0]->type();
            const auto &dest = op.result()->type();
            if (!((source == Type::get_f32() && is_i32(dest)) ||
                  (is_float_vector(source) && is_integer_vector(dest) &&
                   source->count() == dest->count()))) {
                error("yir.fptosi requires same-shape f32 to i32 conversion");
            }
        }
        if (const auto *bit_not = dynamic_cast<const BitNotOp *>(&op)) {
            if (!is_integer_value(bit_not->value()->type()) ||
                !same_type(bit_not->value()->type(), bit_not->result()->type())) {
                error("yir.noti requires scalar/vector i32 operand and matching result");
            }
        }
    }

    void verify_vector_operation(const Operation &op) {
        if (const auto *create = dynamic_cast<const VectorCreateOp *>(&op)) {
            const auto &type = create->result()->type();
            if ((!type->is_vector() && !type->is_mask()) ||
                create->lanes().size() != type->count()) {
                error("yir.vector.create lane count does not match result type");
            } else {
                TypePtr lane_type = type->is_mask() ? Type::get_i1() : type->element();
                for (auto *lane : create->lanes()) {
                    if (!same_type(lane->type(), lane_type)) {
                        error("yir.vector.create lane type does not match result element");
                    }
                }
            }
        }
        if (const auto *splat = dynamic_cast<const SplatOp *>(&op)) {
            const auto &type = splat->result()->type();
            TypePtr lane = type->is_mask() ? Type::get_i1() : type->element();
            if ((!type->is_vector() && !type->is_mask()) ||
                !same_type(lane, splat->scalar()->type())) {
                error("yir.vector.splat scalar does not match vector element type");
            }
        }
        if (const auto *step = dynamic_cast<const StepVectorOp *>(&op)) {
            if (!is_integer_vector(step->result()->type())) {
                error("yir.vector.step requires an i32 vector result");
            }
        }
        if (const auto *extract = dynamic_cast<const ExtractLaneOp *>(&op)) {
            const auto &vector = extract->vector()->type();
            TypePtr expected = vector->is_mask() ? Type::get_i1() : vector->element();
            if ((!vector->is_vector() && !vector->is_mask()) || !is_i32(extract->index()->type()) ||
                !same_type(expected, extract->result()->type())) {
                error("yir.vector.extract has invalid vector, index or result type");
            }
        }
        if (const auto *insert = dynamic_cast<const InsertLaneOp *>(&op)) {
            const auto &vector = insert->vector()->type();
            TypePtr expected = vector->is_mask() ? Type::get_i1() : vector->element();
            if ((!vector->is_vector() && !vector->is_mask()) || !is_i32(insert->index()->type()) ||
                !same_type(expected, insert->lane()->type()) ||
                !same_type(vector, insert->result()->type())) {
                error("yir.vector.insert has invalid vector, index, lane or result type");
            }
        }
        if (const auto *shuffle = dynamic_cast<const ShuffleOp *>(&op)) {
            const auto &lhs = shuffle->lhs()->type();
            if ((!lhs->is_vector() && !lhs->is_mask()) || !same_type(lhs, shuffle->rhs()->type()) ||
                (!shuffle->result()->type()->is_vector() &&
                 !shuffle->result()->type()->is_mask()) ||
                shuffle->indices().size() != shuffle->result()->type()->count()) {
                error("yir.vector.shuffle has incompatible input/output shapes");
            } else {
                for (auto index : shuffle->indices()) {
                    if (index != ShuffleOp::UndefLane && index >= lhs->count() * 2) {
                        error("yir.vector.shuffle index is out of bounds");
                    }
                }
            }
        }
        if (const auto *select = dynamic_cast<const SelectOp *>(&op)) {
            if (!same_type(select->true_value()->type(), select->false_value()->type()) ||
                !same_type(select->true_value()->type(), select->result()->type()) ||
                !matching_mask(select->mask()->type(), select->result()->type())) {
                error("yir.vector.select requires mask and same-shape values");
            }
        }
        if (const auto *cast = dynamic_cast<const VectorCastOp *>(&op)) {
            const auto &source = cast->value()->type();
            const auto &dest = cast->result()->type();
            if (!is_numeric_vector(source) || !is_numeric_vector(dest) ||
                source->count() != dest->count()) {
                error("yir.vector.cast requires same-shape numeric vectors");
            }
        }
        if (const auto *mask_not = dynamic_cast<const MaskNotOp *>(&op)) {
            if (!mask_not->mask()->type()->is_mask() ||
                !same_type(mask_not->mask()->type(), mask_not->result()->type())) {
                error("yir.mask.not requires a mask operand and same-shape result");
            }
        }
        if (const auto *reduce = dynamic_cast<const MaskReduceOp *>(&op)) {
            if (!reduce->mask()->type()->is_mask() || !is_i1(reduce->result()->type())) {
                error("yir.mask reduction requires mask input and scalar i1 result");
            }
        }
        if (const auto *reduce = dynamic_cast<const VectorReduceOp *>(&op)) {
            const auto &vector = reduce->vector()->type();
            if (!is_numeric_vector(vector) ||
                !same_type(vector->element(), reduce->result()->type())) {
                error("yir.vector reduction input/result element types do not match");
            }
            if ((reduce->kind() == VectorReduceOp::Kind::And ||
                 reduce->kind() == VectorReduceOp::Kind::Or ||
                 reduce->kind() == VectorReduceOp::Kind::Xor) &&
                !is_integer_vector(vector)) {
                error("bitwise vector reduction requires i32 vector input");
            }
            if ((is_float_vector(vector) && !reduce->ordered()) ||
                (is_integer_vector(vector) && reduce->ordered())) {
                error("floating vector reductions must be ordered and integer vector reductions "
                      "must be unordered");
            }
        }
        if (const auto *load = dynamic_cast<const MaskedLoadOp *>(&op)) {
            verify_masked_memory(load->passthrough()->type(), load->address()->type(),
                                 load->mask()->type(), load->result()->type(), load->alignment(),
                                 "yir.vector.masked_load");
        }
        if (const auto *store = dynamic_cast<const MaskedStoreOp *>(&op)) {
            verify_masked_memory(store->value()->type(), store->address()->type(),
                                 store->mask()->type(), store->value()->type(), store->alignment(),
                                 "yir.vector.masked_store");
        }
        if (const auto *gather = dynamic_cast<const GatherOp *>(&op)) {
            verify_indexed_memory(gather->passthrough()->type(), gather->base()->type(),
                                  gather->indices()->type(), gather->mask()->type(),
                                  gather->result()->type(), gather->alignment(),
                                  "yir.vector.gather");
        }
        if (const auto *scatter = dynamic_cast<const ScatterOp *>(&op)) {
            verify_indexed_memory(scatter->value()->type(), scatter->base()->type(),
                                  scatter->indices()->type(), scatter->mask()->type(),
                                  scatter->value()->type(), scatter->alignment(),
                                  "yir.vector.scatter");
        }
    }

    void verify_masked_memory(const TypePtr &vector, const TypePtr &address, const TypePtr &mask,
                              const TypePtr &result, std::uint64_t alignment, const char *name) {
        if (!is_numeric_vector(vector) || !same_type(vector, result) || !address->is_ptr() ||
            !same_type(address->pointee(), vector->element()) || !matching_mask(mask, vector) ||
            !is_power_of_two(alignment)) {
            error(std::string(name) + " has invalid vector/pointer/mask/alignment");
        }
    }

    void verify_indexed_memory(const TypePtr &vector, const TypePtr &base, const TypePtr &indices,
                               const TypePtr &mask, const TypePtr &result, std::uint64_t alignment,
                               const char *name) {
        if (!is_numeric_vector(vector) || !same_type(vector, result) || !base->is_ptr() ||
            !same_type(base->pointee(), vector->element()) || !is_integer_vector(indices) ||
            indices->count() != vector->count() || !matching_mask(mask, vector) ||
            !is_power_of_two(alignment)) {
            error(std::string(name) + " has invalid vector/base/index/mask/alignment");
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
        for (auto *index : indices) {
            if (!is_i32(index->type())) {
                error("yir.array_load index must be scalar i32");
            }
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
        for (auto *index : indices) {
            if (!is_i32(index->type())) {
                error("yir.array_store index must be scalar i32");
            }
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
            if (entry.value != nullptr && entry.constant != nullptr) {
                error("yir.array_init entry cannot contain both SSA and constant values");
            }
            if (entry.constant != nullptr) {
                std::string message;
                if (!constant_tree_matches_type(*entry.constant, element, message)) {
                    error("yir.array_init entry " + message);
                }
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
