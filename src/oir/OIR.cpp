#include "oir/OIR.h"

#include "oir/OIRAnalysis.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace oir {

namespace {

std::size_t hash_combine(std::size_t seed, std::size_t value) {
    // The constant is the 64-bit golden ratio.  This is used only for
    // in-process type uniquing; stable cross-process hashes are not required.
    return seed ^
           (value + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (seed << 6U) + (seed >> 2U));
}

std::string f32_bits_literal(float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t),
                  "OIR requires an IEEE-754 binary32 host float");
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    std::ostringstream oss;
    oss << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << bits;
    return oss.str();
}

std::string op_to_string(Instruction::OpID op) {
    switch (op) {
    case Instruction::OpID::Ret:
        return "ret";
    case Instruction::OpID::Br:
        return "br";
    case Instruction::OpID::Add:
        return "add";
    case Instruction::OpID::Sub:
        return "sub";
    case Instruction::OpID::Mul:
        return "mul";
    case Instruction::OpID::SDiv:
        return "sdiv";
    case Instruction::OpID::SRem:
        return "srem";
    case Instruction::OpID::FAdd:
        return "fadd";
    case Instruction::OpID::FSub:
        return "fsub";
    case Instruction::OpID::FMul:
        return "fmul";
    case Instruction::OpID::FDiv:
        return "fdiv";
    case Instruction::OpID::ICmp:
        return "icmp";
    case Instruction::OpID::FCmp:
        return "fcmp";
    case Instruction::OpID::Alloca:
        return "alloca";
    case Instruction::OpID::Load:
        return "load";
    case Instruction::OpID::Store:
        return "store";
    case Instruction::OpID::GetElementPtr:
        return "gep";
    case Instruction::OpID::Call:
        return "call";
    case Instruction::OpID::MemZero:
        return "memzero";
    case Instruction::OpID::ZExt:
        return "zext";
    case Instruction::OpID::SIToFP:
        return "sitofp";
    case Instruction::OpID::FPToSI:
        return "fptosi";
    case Instruction::OpID::Phi:
        return "phi";
    case Instruction::OpID::And:
        return "and";
    case Instruction::OpID::Or:
        return "or";
    case Instruction::OpID::Xor:
        return "xor";
    case Instruction::OpID::SetVL:
        return "setvl";
    case Instruction::OpID::Splat:
        return "splat";
    case Instruction::OpID::StepVector:
        return "stepvector";
    case Instruction::OpID::ExtractElement:
        return "extractelement";
    case Instruction::OpID::InsertElement:
        return "insertelement";
    case Instruction::OpID::ShuffleVector:
        return "shufflevector";
    case Instruction::OpID::VectorSelect:
        return "select";
    case Instruction::OpID::VectorCast:
        return "vector.cast";
    case Instruction::OpID::FixedABIExtractLane:
        return "abi.fixed.extract";
    case Instruction::OpID::FixedABIPack:
        return "abi.fixed.pack";
    case Instruction::OpID::FixedABIObjectLoadLane:
        return "abi.fixed.load_lane";
    case Instruction::OpID::FixedABIObjectStoreLane:
        return "abi.fixed.store_lane";
    case Instruction::OpID::VPBinary:
        return "vp.binary";
    case Instruction::OpID::VPCmp:
        return "vp.cmp";
    case Instruction::OpID::VPLoad:
        return "vp.load";
    case Instruction::OpID::VPStore:
        return "vp.store";
    case Instruction::OpID::MaskedLoad:
        return "masked.load";
    case Instruction::OpID::MaskedStore:
        return "masked.store";
    case Instruction::OpID::VPGather:
        return "vp.gather";
    case Instruction::OpID::VPScatter:
        return "vp.scatter";
    case Instruction::OpID::VPReduction:
        return "vp.reduce";
    }
    return "unknown";
}

std::string tail_policy_to_string(TailPolicy policy) {
    switch (policy) {
    case TailPolicy::Agnostic:
        return "agnostic";
    case TailPolicy::Undisturbed:
        return "undisturbed";
    }
    return "invalid";
}

std::string mask_policy_to_string(MaskPolicy policy) {
    switch (policy) {
    case MaskPolicy::Agnostic:
        return "agnostic";
    case MaskPolicy::Undisturbed:
        return "undisturbed";
    }
    return "invalid";
}

std::string vector_cast_kind_to_string(VectorCastKind kind) {
    switch (kind) {
    case VectorCastKind::ZExt:
        return "zext";
    case VectorCastKind::SIToFP:
        return "sitofp";
    case VectorCastKind::FPToSI:
        return "fptosi";
    case VectorCastKind::Bitcast:
        return "bitcast";
    }
    return "invalid";
}

std::string reduction_kind_to_string(ReductionKind kind, bool floating) {
    switch (kind) {
    case ReductionKind::Add:
        return floating ? "fadd" : "add";
    case ReductionKind::Mul:
        return floating ? "fmul" : "mul";
    case ReductionKind::Min:
        return floating ? "fmin" : "min";
    case ReductionKind::Max:
        return floating ? "fmax" : "max";
    case ReductionKind::And:
        return "and";
    case ReductionKind::Or:
        return "or";
    case ReductionKind::Xor:
        return "xor";
    }
    return "invalid";
}

std::string typed_value_ref(const Value *value);

std::string vp_metadata_suffix(const VPInstruction &inst) {
    std::ostringstream oss;
    oss << ", mask " << typed_value_ref(inst.active_mask()) << ", evl "
        << typed_value_ref(inst.evl());
    if (inst.has_passthrough()) {
        oss << ", passthrough " << typed_value_ref(inst.passthrough());
    }
    oss << ", tail=" << tail_policy_to_string(inst.tail_policy())
        << ", mask-policy=" << mask_policy_to_string(inst.mask_policy());
    return oss.str();
}

std::string cmp_pred_to_string(CmpPred pred) {
    switch (pred) {
    case CmpPred::EQ:
        return "eq";
    case CmpPred::NE:
        return "ne";
    case CmpPred::LT:
        return "lt";
    case CmpPred::LE:
        return "le";
    case CmpPred::GT:
        return "gt";
    case CmpPred::GE:
        return "ge";
    }
    return "?";
}

std::string value_ref(const Value *value) {
    if (value == nullptr) {
        return "<null>";
    }

    if (dynamic_cast<const Constant *>(value) != nullptr ||
        dynamic_cast<const UndefValue *>(value) != nullptr) {
        return value->print();
    }

    if (dynamic_cast<const BasicBlock *>(value) != nullptr) {
        return "%" + value->name();
    }

    if (dynamic_cast<const Function *>(value) != nullptr ||
        dynamic_cast<const GlobalVariable *>(value) != nullptr) {
        return "@" + value->name();
    }

    if (!value->name().empty()) {
        return "%" + value->name();
    }

    return "<tmp>";
}

std::string prefix_name(const Value *value) {
    if (value == nullptr || value->name().empty()) {
        return "";
    }
    return "%" + value->name() + " = ";
}

std::string typed_value_ref(const Value *value) {
    if (value == nullptr || value->type() == nullptr) {
        return "<null>";
    }
    return value->type()->print() + " " + value_ref(value);
}

std::string function_ref(const Function *function) {
    return function == nullptr ? "<null function>" : "@" + function->name();
}

std::string block_ref(const BasicBlock *block) {
    return block == nullptr ? "<null block>" : "%" + block->name();
}

std::string inst_ref(const Instruction *inst) {
    if (inst == nullptr) {
        return "<null inst>";
    }
    if (!inst->name().empty()) {
        return "%" + inst->name();
    }
    return op_to_string(inst->op()) + " in " + block_ref(inst->parent());
}

struct UseKey {
    const User *user = nullptr;
    std::size_t operand_index = 0;

    bool operator==(const UseKey &other) const {
        return user == other.user && operand_index == other.operand_index;
    }
};

struct UseKeyHash {
    std::size_t operator()(const UseKey &key) const {
        return std::hash<const void *>{}(key.user) ^
               (std::hash<std::size_t>{}(key.operand_index) << 1U);
    }
};

bool contains_block_ptr(const std::vector<BasicBlock *> &blocks, const BasicBlock *needle) {
    return std::find(blocks.begin(), blocks.end(), needle) != blocks.end();
}

bool has_duplicate_block(const std::vector<BasicBlock *> &blocks) {
    std::unordered_set<const BasicBlock *> seen;
    for (auto *block : blocks) {
        if (!seen.insert(block).second) {
            return true;
        }
    }
    return false;
}

std::vector<BasicBlock *> branch_targets(const BranchInst &branch) {
    std::vector<BasicBlock *> targets;
    auto add_unique = [&targets](BasicBlock *target) {
        if (target != nullptr &&
            std::find(targets.begin(), targets.end(), target) == targets.end()) {
            targets.push_back(target);
        }
    };

    if (branch.is_conditional()) {
        add_unique(branch.true_bb());
        add_unique(branch.false_bb());
    } else {
        add_unique(branch.target_bb());
    }
    return targets;
}

bool contains_scalable_vector_storage(const Type *type) {
    if (const auto *vector = dynamic_cast<const VectorType *>(type)) {
        return vector->element_count().is_scalable();
    }
    if (const auto *array = dynamic_cast<const ArrayType *>(type)) {
        return contains_scalable_vector_storage(array->element_type());
    }
    return false;
}

bool is_storable_type(const Type *type) {
    if (type == nullptr || type->is_void() || type->is_label() || type->is_function()) {
        return false;
    }
    if (const auto *array = dynamic_cast<const ArrayType *>(type)) {
        return is_storable_type(array->element_type());
    }
    return type->is_scalar() || type->is_vector();
}

bool is_scalar_i1(const Type *type) {
    const auto *integer = dynamic_cast<const IntegerType *>(type);
    return integer != nullptr && integer->bit_width() == 1;
}

bool is_integer_type_or_vector(const Type *type, bool allow_mask) {
    if (type == nullptr) {
        return false;
    }
    if (type->is_scalar_integer()) {
        return true;
    }
    const auto *vector = dynamic_cast<const VectorType *>(type);
    if (vector == nullptr || !vector->element_type()->is_scalar_integer()) {
        return false;
    }
    return allow_mask || !vector->is_mask();
}

bool is_float_type_or_vector(const Type *type) {
    if (type == nullptr) {
        return false;
    }
    if (type->is_scalar_float()) {
        return true;
    }
    const auto *vector = dynamic_cast<const VectorType *>(type);
    return vector != nullptr && vector->element_type()->is_scalar_float();
}

const FunctionType *called_function_type(const Value *callee) {
    if (callee == nullptr || callee->type() == nullptr) {
        return nullptr;
    }
    if (const auto *direct = dynamic_cast<const FunctionType *>(callee->type())) {
        return direct;
    }
    const auto *pointer = dynamic_cast<const PointerType *>(callee->type());
    return pointer == nullptr ? nullptr
                              : dynamic_cast<const FunctionType *>(pointer->element_type());
}

const Type *gep_result_element_type(const PointerType &base_type, std::size_t index_count) {
    const Type *cursor = base_type.element_type();
    // The first GEP index performs pointer arithmetic and does not descend
    // through the pointee.  Every later index must select an array element.
    for (std::size_t i = 1; i < index_count; ++i) {
        const auto *array = dynamic_cast<const ArrayType *>(cursor);
        if (array == nullptr) {
            return nullptr;
        }
        cursor = array->element_type();
    }
    return cursor;
}

Type *comparison_result_type(Module &module, const Value *lhs) {
    const auto *vector = lhs == nullptr ? nullptr : dynamic_cast<const VectorType *>(lhs->type());
    if (vector == nullptr) {
        return module.types().int1_ty();
    }
    return module.types().vector_ty(module.types().int1_ty(), vector->element_count());
}

bool is_scalar_i32(const Type *type) {
    const auto *integer = dynamic_cast<const IntegerType *>(type);
    return integer != nullptr && integer->bit_width() == 32;
}

bool is_i32_value(std::int64_t value) {
    return value >= std::numeric_limits<std::int32_t>::min() &&
           value <= std::numeric_limits<std::int32_t>::max();
}

bool is_valid_alignment(std::size_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1U)) == 0;
}

bool is_valid_tail_policy(TailPolicy policy) {
    return policy == TailPolicy::Agnostic || policy == TailPolicy::Undisturbed;
}

bool is_valid_mask_policy(MaskPolicy policy) {
    return policy == MaskPolicy::Agnostic || policy == MaskPolicy::Undisturbed;
}

std::string vp_metadata_error(const VPInstruction &inst, const VectorType &lane_type,
                              const Type *passthrough_type, bool require_passthrough,
                              bool require_agnostic_policies, bool require_fixed_full_evl) {
    if (inst.active_mask() == nullptr) {
        return "OIRV_VP_OPERAND_NULL: " + inst_ref(&inst) + " active mask must not be null";
    }
    if (inst.evl() == nullptr) {
        return "OIRV_VP_OPERAND_NULL: " + inst_ref(&inst) + " EVL must not be null";
    }
    const auto *active_mask = dynamic_cast<const VectorType *>(inst.active_mask()->type());
    if (active_mask == nullptr || !active_mask->is_mask() ||
        active_mask->element_count() != lane_type.element_count()) {
        return "OIRV_VP_MASK_SHAPE: " + inst_ref(&inst) +
               " active mask must be a same-shaped i1 vector";
    }
    if (!is_scalar_i32(inst.evl()->type())) {
        return "OIRV_VP_EVL_TYPE: " + inst_ref(&inst) + " EVL must be scalar i32";
    }
    if (const auto *constant_evl = dynamic_cast<const ConstantInt *>(inst.evl())) {
        if (constant_evl->value() < 0 || (!lane_type.element_count().is_scalable() &&
                                          static_cast<std::uint64_t>(constant_evl->value()) >
                                              lane_type.element_count().min_lanes)) {
            return "OIRV_VP_EVL_RANGE: " + inst_ref(&inst) +
                   " constant EVL must be nonnegative and no greater than the fixed lane count";
        }
    }
    if (!is_valid_tail_policy(inst.tail_policy()) || !is_valid_mask_policy(inst.mask_policy())) {
        return "OIRV_VP_POLICY: " + inst_ref(&inst) + " has an invalid policy value";
    }
    if (inst.has_passthrough() != require_passthrough) {
        return "OIRV_VP_PASSTHROUGH: " + inst_ref(&inst) +
               (require_passthrough ? " requires an explicit passthrough"
                                    : " must not have a passthrough");
    }
    if (require_passthrough && inst.passthrough() == nullptr) {
        return "OIRV_VP_OPERAND_NULL: " + inst_ref(&inst) + " passthrough must not be null";
    }
    if (require_passthrough && inst.passthrough()->type() != passthrough_type) {
        return "OIRV_VP_PASSTHROUGH: " + inst_ref(&inst) +
               " passthrough type must match the result type";
    }
    if (require_agnostic_policies && (inst.tail_policy() != TailPolicy::Agnostic ||
                                      inst.mask_policy() != MaskPolicy::Agnostic)) {
        return "OIRV_VP_POLICY: " + inst_ref(&inst) +
               " has no destination lanes and requires agnostic policies";
    }
    if (require_fixed_full_evl) {
        if (lane_type.element_count().is_scalable()) {
            return "OIRV_MASKED_VECTOR_FORM: " + inst_ref(&inst) + " requires a fixed vector type";
        }
        const auto *evl = dynamic_cast<const ConstantInt *>(inst.evl());
        if (evl == nullptr || evl->value() < 0 ||
            static_cast<std::uint64_t>(evl->value()) != lane_type.element_count().min_lanes) {
            return "OIRV_MASKED_VECTOR_FORM: " + inst_ref(&inst) +
                   " EVL must be the fixed lane count";
        }
        if (inst.tail_policy() != TailPolicy::Agnostic) {
            return "OIRV_VP_POLICY: " + inst_ref(&inst) +
                   " fixed masked form requires agnostic tail policy";
        }
    }
    return {};
}

std::string constant_validation_error(const Constant *constant,
                                      const std::unordered_set<const Constant *> &owned,
                                      const std::string &path,
                                      std::unordered_set<const Constant *> &visiting,
                                      std::unordered_set<const Constant *> &validated) {
    if (constant == nullptr) {
        return "OIRV_CONSTANT_NULL: " + path + " contains a null constant";
    }
    if (owned.find(constant) == owned.end()) {
        return "OIRV_CONSTANT_OWNERSHIP: " + path +
               " references a constant not owned by its module";
    }
    if (constant->type() == nullptr) {
        return "OIRV_CONSTANT_TYPE: " + path + " has no type";
    }
    if (validated.find(constant) != validated.end()) {
        return {};
    }
    if (!visiting.insert(constant).second) {
        return "OIRV_CONSTANT_CYCLE: " + path + " contains a cyclic constant tree";
    }

    auto finish = [&]() -> std::string {
        visiting.erase(constant);
        validated.insert(constant);
        return {};
    };
    auto fail_here = [&](std::string message) -> std::string {
        visiting.erase(constant);
        return message;
    };

    if (const auto *integer = dynamic_cast<const ConstantInt *>(constant)) {
        const auto *integer_type = dynamic_cast<const IntegerType *>(integer->type());
        if (integer_type == nullptr || integer_type->bit_width() == 0 ||
            (integer_type->bit_width() == 1 && integer->value() != 0 && integer->value() != 1) ||
            (integer_type->bit_width() == 32 && !is_i32_value(integer->value()))) {
            return fail_here("OIRV_CONSTANT_TYPE: " + path +
                             " has an invalid integer constant type or value");
        }
        return finish();
    }
    if (const auto *floating = dynamic_cast<const ConstantFloat *>(constant)) {
        if (!floating->type()->is_scalar_float()) {
            return fail_here("OIRV_CONSTANT_TYPE: " + path +
                             " float constant requires scalar float type");
        }
        return finish();
    }
    if (dynamic_cast<const ConstantAggregateZero *>(constant) != nullptr) {
        if (!is_storable_type(constant->type())) {
            return fail_here("OIRV_CONSTANT_TYPE: " + path +
                             " zero constant requires a storable type");
        }
        return finish();
    }
    if (const auto *array = dynamic_cast<const ConstantArray *>(constant)) {
        const auto *array_type = dynamic_cast<const ArrayType *>(array->type());
        if (array_type == nullptr || array->elements().size() != array_type->element_count()) {
            return fail_here("OIRV_CONSTANT_SHAPE: " + path +
                             " array constant does not match its array type");
        }
        for (std::size_t i = 0; i < array->elements().size(); ++i) {
            auto *element = array->elements()[i];
            if (element == nullptr || element->type() != array_type->element_type()) {
                return fail_here("OIRV_CONSTANT_SHAPE: " + path + " element " + std::to_string(i) +
                                 " has the wrong array element type");
            }
            auto error = constant_validation_error(
                element, owned, path + "[" + std::to_string(i) + "]", visiting, validated);
            if (!error.empty()) {
                visiting.erase(constant);
                return error;
            }
        }
        return finish();
    }
    if (const auto *vector = dynamic_cast<const ConstantVector *>(constant)) {
        const auto *vector_type = dynamic_cast<const VectorType *>(vector->type());
        if (vector_type == nullptr || vector_type->is_mask() ||
            vector_type->element_count().is_scalable() ||
            vector->elements().size() != vector_type->element_count().min_lanes) {
            return fail_here("OIRV_CONSTANT_SHAPE: " + path +
                             " vector constant requires a matching fixed non-mask type");
        }
        for (std::size_t i = 0; i < vector->elements().size(); ++i) {
            auto *element = vector->elements()[i];
            if (element == nullptr || element->type() != vector_type->element_type()) {
                return fail_here("OIRV_CONSTANT_SHAPE: " + path + " lane " + std::to_string(i) +
                                 " has the wrong vector element type");
            }
            auto error = constant_validation_error(
                element, owned, path + " lane " + std::to_string(i), visiting, validated);
            if (!error.empty()) {
                visiting.erase(constant);
                return error;
            }
        }
        return finish();
    }
    if (const auto *mask = dynamic_cast<const ConstantMask *>(constant)) {
        const auto *mask_type = dynamic_cast<const VectorType *>(mask->type());
        if (mask_type == nullptr || !mask_type->is_mask() ||
            mask_type->element_count().is_scalable()) {
            return fail_here("OIRV_CONSTANT_MASK: " + path + " requires a fixed i1 vector type");
        }
        const auto lanes = mask_type->element_count().min_lanes;
        const auto expected_bytes = lanes / 8U + (lanes % 8U == 0 ? 0U : 1U);
        if (expected_bytes > std::numeric_limits<std::size_t>::max() ||
            mask->packed_bits().size() != static_cast<std::size_t>(expected_bytes)) {
            return fail_here("OIRV_CONSTANT_MASK: " + path +
                             " packed byte count does not match its lanes");
        }
        const auto used_bits = static_cast<unsigned>(lanes % 8U);
        if (used_bits != 0U) {
            const auto used_mask = static_cast<std::uint8_t>((1U << used_bits) - 1U);
            if ((mask->packed_bits().back() & static_cast<std::uint8_t>(~used_mask)) != 0U) {
                return fail_here("OIRV_CONSTANT_MASK: " + path + " has nonzero unused high bits");
            }
        }
        return finish();
    }

    return fail_here("OIRV_CONSTANT_KIND: " + path + " has an unrecognized constant subclass");
}

} // namespace

ElementCount::ElementCount(std::uint64_t min_lanes, bool scalable)
    : min_lanes(min_lanes), scalable(scalable) {
    if (min_lanes == 0) {
        throw std::invalid_argument("OIR vector element count must be positive");
    }
}

ElementCount ElementCount::get_fixed(std::uint64_t lanes) {
    return ElementCount(lanes, false);
}

ElementCount ElementCount::get_scalable(std::uint64_t min_lanes) {
    return ElementCount(min_lanes, true);
}

bool ElementCount::is_fixed() const {
    return !scalable;
}

bool ElementCount::is_scalable() const {
    return scalable;
}

bool ElementCount::operator==(const ElementCount &other) const {
    return min_lanes == other.min_lanes && scalable == other.scalable;
}

bool ElementCount::operator!=(const ElementCount &other) const {
    return !(*this == other);
}

Type::Type(TypeID id) : id_(id) {
}

Type::TypeID Type::id() const {
    return id_;
}

bool Type::is_void() const {
    return id_ == TypeID::Void;
}

bool Type::is_label() const {
    return id_ == TypeID::Label;
}

bool Type::is_integer() const {
    return id_ == TypeID::Integer;
}

bool Type::is_float() const {
    return id_ == TypeID::Float;
}

bool Type::is_pointer() const {
    return id_ == TypeID::Pointer;
}

bool Type::is_array() const {
    return id_ == TypeID::Array;
}

bool Type::is_function() const {
    return id_ == TypeID::Function;
}

bool Type::is_vector() const {
    return id_ == TypeID::Vector;
}

bool Type::is_scalar_integer() const {
    return is_integer();
}

bool Type::is_scalar_float() const {
    return is_float();
}

bool Type::is_scalar_numeric() const {
    return is_scalar_integer() || is_scalar_float();
}

bool Type::is_scalar() const {
    return is_scalar_numeric() || is_pointer();
}

bool Type::is_fixed_vector() const {
    const auto *vector = dynamic_cast<const VectorType *>(this);
    return vector != nullptr && vector->element_count().is_fixed();
}

bool Type::is_scalable_vector() const {
    const auto *vector = dynamic_cast<const VectorType *>(this);
    return vector != nullptr && vector->element_count().is_scalable();
}

bool Type::is_mask() const {
    const auto *vector = dynamic_cast<const VectorType *>(this);
    return vector != nullptr && vector->is_mask();
}

VoidType::VoidType() : Type(TypeID::Void) {
}

std::string VoidType::print() const {
    return "void";
}

LabelType::LabelType() : Type(TypeID::Label) {
}

std::string LabelType::print() const {
    return "label";
}

IntegerType::IntegerType(std::size_t bit_width) : Type(TypeID::Integer), bit_width_(bit_width) {
}

std::size_t IntegerType::bit_width() const {
    return bit_width_;
}

std::string IntegerType::print() const {
    return "i" + std::to_string(bit_width_);
}

FloatType::FloatType() : Type(TypeID::Float) {
}

std::string FloatType::print() const {
    return "float";
}

PointerType::PointerType(Type *element_type) : Type(TypeID::Pointer), element_type_(element_type) {
}

Type *PointerType::element_type() const {
    return element_type_;
}

std::string PointerType::print() const {
    return element_type_->print() + "*";
}

ArrayType::ArrayType(Type *element_type, std::size_t element_count)
    : Type(TypeID::Array), element_type_(element_type), element_count_(element_count) {
}

Type *ArrayType::element_type() const {
    return element_type_;
}

std::size_t ArrayType::element_count() const {
    return element_count_;
}

std::string ArrayType::print() const {
    return "[" + std::to_string(element_count_) + " x " + element_type_->print() + "]";
}

FunctionType::FunctionType(Type *return_type, std::vector<Type *> param_types, bool is_variadic)
    : Type(TypeID::Function), return_type_(return_type), param_types_(std::move(param_types)),
      is_variadic_(is_variadic) {
}

Type *FunctionType::return_type() const {
    return return_type_;
}

const std::vector<Type *> &FunctionType::param_types() const {
    return param_types_;
}

bool FunctionType::is_variadic() const {
    return is_variadic_;
}

std::string FunctionType::print() const {
    std::ostringstream oss;
    oss << return_type_->print() << " (";
    for (std::size_t i = 0; i < param_types_.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << param_types_[i]->print();
    }
    if (is_variadic_) {
        if (!param_types_.empty()) {
            oss << ", ";
        }
        oss << "...";
    }
    oss << ")";
    return oss.str();
}

VectorType::VectorType(Type *element_type, ElementCount element_count)
    : Type(TypeID::Vector), element_type_(element_type), element_count_(element_count) {
    if (element_type_ == nullptr) {
        throw std::invalid_argument("OIR vector element type must not be null");
    }
    const auto *integer = dynamic_cast<const IntegerType *>(element_type_);
    const bool supported_integer =
        integer != nullptr && (integer->bit_width() == 1 || integer->bit_width() == 32);
    if (!supported_integer && !element_type_->is_scalar_float()) {
        throw std::invalid_argument("OIR vector element type must be i1, i32, or float");
    }
}

Type *VectorType::element_type() const {
    return element_type_;
}

const ElementCount &VectorType::element_count() const {
    return element_count_;
}

bool VectorType::is_mask() const {
    const auto *integer = dynamic_cast<const IntegerType *>(element_type_);
    return integer != nullptr && integer->bit_width() == 1;
}

bool VectorType::is_integer_vector() const {
    return element_type_->is_scalar_integer() && !is_mask();
}

bool VectorType::is_float_vector() const {
    return element_type_->is_scalar_float();
}

std::string VectorType::print() const {
    std::ostringstream oss;
    oss << '<';
    if (element_count_.is_scalable()) {
        oss << "vscale x ";
    }
    oss << element_count_.min_lanes << " x " << element_type_->print() << '>';
    return oss.str();
}

bool TypeContext::ArrayTypeKey::operator==(const ArrayTypeKey &other) const {
    return element_type == other.element_type && element_count == other.element_count;
}

std::size_t TypeContext::ArrayTypeKeyHash::operator()(const ArrayTypeKey &key) const {
    auto hash = std::hash<const void *>{}(key.element_type);
    return hash_combine(hash, std::hash<std::size_t>{}(key.element_count));
}

bool TypeContext::FunctionTypeKey::operator==(const FunctionTypeKey &other) const {
    return return_type == other.return_type && param_types == other.param_types &&
           is_variadic == other.is_variadic;
}

std::size_t TypeContext::FunctionTypeKeyHash::operator()(const FunctionTypeKey &key) const {
    auto hash = std::hash<const void *>{}(key.return_type);
    for (auto *param : key.param_types) {
        hash = hash_combine(hash, std::hash<const void *>{}(param));
    }
    hash = hash_combine(hash, std::hash<bool>{}(key.is_variadic));
    return hash;
}

bool TypeContext::VectorTypeKey::operator==(const VectorTypeKey &other) const {
    return element_type == other.element_type && element_count == other.element_count;
}

std::size_t TypeContext::VectorTypeKeyHash::operator()(const VectorTypeKey &key) const {
    auto hash = std::hash<const void *>{}(key.element_type);
    hash = hash_combine(hash, std::hash<std::uint64_t>{}(key.element_count.min_lanes));
    return hash_combine(hash, std::hash<bool>{}(key.element_count.scalable));
}

TypeContext::TypeContext()
    : void_ty_(std::make_unique<VoidType>()), label_ty_(std::make_unique<LabelType>()),
      int1_ty_(std::make_unique<IntegerType>(1)), int32_ty_(std::make_unique<IntegerType>(32)),
      float_ty_(std::make_unique<FloatType>()) {
}

VoidType *TypeContext::void_ty() const {
    return void_ty_.get();
}

LabelType *TypeContext::label_ty() const {
    return label_ty_.get();
}

IntegerType *TypeContext::int1_ty() const {
    return int1_ty_.get();
}

IntegerType *TypeContext::int32_ty() const {
    return int32_ty_.get();
}

FloatType *TypeContext::float_ty() const {
    return float_ty_.get();
}

PointerType *TypeContext::ptr_ty(Type *element_type) {
    if (element_type == nullptr) {
        throw std::invalid_argument("OIR pointer element type must not be null");
    }
    auto found = pointer_types_.find(element_type);
    if (found != pointer_types_.end()) {
        return found->second;
    }

    auto ptr = std::make_unique<PointerType>(element_type);
    auto *raw = ptr.get();
    owned_composite_types_.push_back(std::move(ptr));
    pointer_types_.emplace(element_type, raw);
    return raw;
}

ArrayType *TypeContext::array_ty(Type *element_type, std::size_t element_count) {
    if (element_type == nullptr) {
        throw std::invalid_argument("OIR array element type must not be null");
    }
    ArrayTypeKey key{element_type, element_count};
    auto found = array_types_.find(key);
    if (found != array_types_.end()) {
        return found->second;
    }

    auto arr = std::make_unique<ArrayType>(element_type, element_count);
    auto *raw = arr.get();
    owned_composite_types_.push_back(std::move(arr));
    array_types_.emplace(key, raw);
    return raw;
}

FunctionType *TypeContext::func_ty(Type *return_type, const std::vector<Type *> &param_types,
                                   bool is_variadic) {
    if (return_type == nullptr || std::any_of(param_types.begin(), param_types.end(),
                                              [](const Type *type) { return type == nullptr; })) {
        throw std::invalid_argument("OIR function types must not contain null types");
    }
    FunctionTypeKey key{return_type, param_types, is_variadic};
    auto found = function_types_.find(key);
    if (found != function_types_.end()) {
        return found->second;
    }

    auto fn = std::make_unique<FunctionType>(return_type, param_types, is_variadic);
    auto *raw = fn.get();
    owned_composite_types_.push_back(std::move(fn));
    function_types_.emplace(std::move(key), raw);
    return raw;
}

VectorType *TypeContext::vector_ty(Type *element_type, ElementCount element_count) {
    VectorTypeKey key{element_type, element_count};
    auto found = vector_types_.find(key);
    if (found != vector_types_.end()) {
        return found->second;
    }

    auto vector = std::make_unique<VectorType>(element_type, element_count);
    auto *raw = vector.get();
    owned_composite_types_.push_back(std::move(vector));
    vector_types_.emplace(std::move(key), raw);
    return raw;
}

VectorType *TypeContext::fixed_vector_ty(Type *element_type, std::uint64_t lanes) {
    return vector_ty(element_type, ElementCount::get_fixed(lanes));
}

VectorType *TypeContext::scalable_vector_ty(Type *element_type, std::uint64_t min_lanes) {
    return vector_ty(element_type, ElementCount::get_scalable(min_lanes));
}

Value::Value(Type *type, const std::string &name) : type_(type), name_(name) {
}

Type *Value::type() const {
    return type_;
}

const std::string &Value::name() const {
    return name_;
}

void Value::set_name(const std::string &name) {
    name_ = name;
}

void Value::set_type(Type *type) {
    type_ = type;
}

const std::vector<Value::Use> &Value::uses() const {
    return uses_;
}

std::vector<User *> Value::users() const {
    std::vector<User *> out;
    out.reserve(uses_.size());
    for (const auto &use : uses_) {
        out.push_back(use.user);
    }
    return out;
}

std::size_t Value::use_count() const {
    return uses_.size();
}

bool Value::has_uses() const {
    return !uses_.empty();
}

void Value::replace_all_uses_with(Value *new_value) {
    if (new_value == nullptr || new_value == this || new_value->type() != type()) {
        return;
    }

    auto worklist = uses_;
    for (const auto &use : worklist) {
        if (use.user != nullptr && use.operand_index < use.user->operand_count() &&
            use.user->operand(use.operand_index) == this) {
            use.user->set_operand(use.operand_index, new_value);
        }
    }
}

void Value::add_use(User *user, std::size_t operand_index) {
    uses_.push_back({user, operand_index});
}

void Value::remove_use(User *user, std::size_t operand_index) {
    auto found = std::find_if(uses_.begin(), uses_.end(), [user, operand_index](const Use &use) {
        return use.user == user && use.operand_index == operand_index;
    });
    if (found != uses_.end()) {
        uses_.erase(found);
    }
}

Constant::Constant(Type *type, const std::string &name) : Value(type, name) {
}

ConstantInt::ConstantInt(Type *type, std::int64_t value)
    : Constant(type, std::to_string(value)), value_(value) {
    const auto *integer = dynamic_cast<const IntegerType *>(type);
    if (integer == nullptr || integer->bit_width() == 0) {
        throw std::invalid_argument("OIR integer constant requires a nonzero-width integer type");
    }
    if (integer->bit_width() == 1 && value != 0 && value != 1) {
        throw std::invalid_argument("OIR i1 constant value must be zero or one");
    }
    if (integer->bit_width() == 32 && !is_i32_value(value)) {
        throw std::invalid_argument("OIR i32 constant value is outside the signed 32-bit range");
    }
}

std::int64_t ConstantInt::value() const {
    return value_;
}

std::string ConstantInt::print() const {
    return std::to_string(value_);
}

ConstantFloat::ConstantFloat(Type *type, float value)
    : Constant(type, f32_bits_literal(value)), value_(value) {
    if (type == nullptr || !type->is_scalar_float()) {
        throw std::invalid_argument("OIR float constant requires a scalar float type");
    }
}

float ConstantFloat::value() const {
    return value_;
}

std::string ConstantFloat::print() const {
    return f32_bits_literal(value_);
}

ConstantAggregateZero::ConstantAggregateZero(Type *type) : Constant(type, "zero") {
    if (!is_storable_type(type)) {
        throw std::invalid_argument("OIR zero constant requires a storable type");
    }
}

std::string ConstantAggregateZero::print() const {
    return "zero";
}

ConstantArray::ConstantArray(ArrayType *type, std::vector<Constant *> elements)
    : Constant(type, "constant.array"), elements_(std::move(elements)) {
    if (type == nullptr) {
        throw std::invalid_argument("OIR array constant requires an array type");
    }
    if (elements_.size() != type->element_count()) {
        throw std::invalid_argument("OIR array constant element count does not match its type");
    }
    for (auto *element : elements_) {
        if (element == nullptr || element->type() != type->element_type()) {
            throw std::invalid_argument("OIR array constant element type does not match its type");
        }
    }
}

ArrayType *ConstantArray::array_type() const {
    return static_cast<ArrayType *>(type());
}

const std::vector<Constant *> &ConstantArray::elements() const {
    return elements_;
}

std::string ConstantArray::print() const {
    std::ostringstream oss;
    oss << '[';
    for (std::size_t i = 0; i < elements_.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << typed_value_ref(elements_[i]);
    }
    oss << ']';
    return oss.str();
}

ConstantVector::ConstantVector(VectorType *type, std::vector<Constant *> elements)
    : Constant(type, "constant.vector"), elements_(std::move(elements)) {
    if (type == nullptr || type->is_mask()) {
        throw std::invalid_argument("OIR vector constant requires a non-mask vector type");
    }
    if (type->element_count().is_scalable()) {
        throw std::invalid_argument("OIR explicit vector constant requires a fixed element count");
    }
    if (elements_.size() != type->element_count().min_lanes) {
        throw std::invalid_argument("OIR vector constant element count does not match its type");
    }
    for (auto *element : elements_) {
        if (element == nullptr || element->type() != type->element_type()) {
            throw std::invalid_argument("OIR vector constant element type does not match its type");
        }
    }
}

VectorType *ConstantVector::vector_type() const {
    return static_cast<VectorType *>(type());
}

const std::vector<Constant *> &ConstantVector::elements() const {
    return elements_;
}

std::string ConstantVector::print() const {
    std::ostringstream oss;
    oss << '<';
    for (std::size_t i = 0; i < elements_.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << typed_value_ref(elements_[i]);
    }
    oss << '>';
    return oss.str();
}

ConstantMask::ConstantMask(VectorType *type, std::vector<std::uint8_t> packed_bits)
    : Constant(type, "constant.mask"), packed_bits_(std::move(packed_bits)) {
    if (type == nullptr || !type->is_mask()) {
        throw std::invalid_argument("OIR mask constant requires an i1 vector type");
    }
    if (type->element_count().is_scalable()) {
        throw std::invalid_argument("OIR mask constant requires a fixed element count");
    }

    const auto lanes = type->element_count().min_lanes;
    const auto required_bytes_u64 = lanes / 8U + (lanes % 8U == 0 ? 0U : 1U);
    if (required_bytes_u64 > std::numeric_limits<std::size_t>::max() ||
        packed_bits_.size() != static_cast<std::size_t>(required_bytes_u64)) {
        throw std::invalid_argument("OIR mask constant byte count does not match its lane count");
    }
    const auto used_bits_in_last_byte = static_cast<unsigned>(lanes % 8U);
    if (used_bits_in_last_byte != 0U) {
        const auto used_mask = static_cast<std::uint8_t>((1U << used_bits_in_last_byte) - 1U);
        if ((packed_bits_.back() & static_cast<std::uint8_t>(~used_mask)) != 0U) {
            throw std::invalid_argument("OIR mask constant has nonzero unused high bits");
        }
    }
}

VectorType *ConstantMask::mask_type() const {
    return static_cast<VectorType *>(type());
}

std::uint64_t ConstantMask::lane_count() const {
    return mask_type()->element_count().min_lanes;
}

bool ConstantMask::lane(std::uint64_t index) const {
    if (index >= lane_count()) {
        throw std::out_of_range("OIR mask constant lane index is out of range");
    }
    const auto byte_index = static_cast<std::size_t>(index / 8U);
    const auto bit_index = static_cast<unsigned>(index % 8U);
    return ((packed_bits_[byte_index] >> bit_index) & 1U) != 0U;
}

const std::vector<std::uint8_t> &ConstantMask::packed_bits() const {
    return packed_bits_;
}

std::string ConstantMask::print() const {
    std::ostringstream oss;
    oss << '<';
    for (std::uint64_t i = 0; i < lane_count(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << "i1 " << (lane(i) ? '1' : '0');
    }
    oss << '>';
    return oss.str();
}

UndefValue::UndefValue(Type *type) : Value(type, "undef") {
}

std::string UndefValue::print() const {
    return "undef";
}

User::User(Type *type, const std::string &name) : Value(type, name) {
}

User::~User() {
    drop_all_operands();
}

void User::add_operand(Value *value) {
    if (value != nullptr) {
        value->add_use(this, operands_.size());
    }
    operands_.push_back(value);
}

Value *User::operand(std::size_t index) const {
    return operands_[index];
}

void User::set_operand(std::size_t index, Value *value) {
    auto *old_value = operands_[index];
    if (old_value == value) {
        return;
    }
    if (old_value != nullptr) {
        old_value->remove_use(this, index);
    }
    operands_[index] = value;
    if (value != nullptr) {
        value->add_use(this, index);
    }
}

void User::replace_operand(Value *old_value, Value *new_value) {
    replace_operands(old_value, new_value);
}

std::size_t User::replace_operands(Value *old_value, Value *new_value) {
    std::size_t replaced = 0;
    for (std::size_t i = 0; i < operands_.size(); ++i) {
        if (operands_[i] == old_value) {
            set_operand(i, new_value);
            ++replaced;
        }
    }
    return replaced;
}

void User::drop_all_operands() {
    for (std::size_t i = 0; i < operands_.size(); ++i) {
        if (operands_[i] != nullptr) {
            operands_[i]->remove_use(this, i);
        }
    }
    operands_.clear();
}

void User::erase_operands(std::size_t first, std::size_t count) {
    if (first >= operands_.size() || count == 0) {
        return;
    }

    const auto last = std::min(first + count, operands_.size());
    for (std::size_t i = first; i < last; ++i) {
        if (operands_[i] != nullptr) {
            operands_[i]->remove_use(this, i);
        }
    }
    for (std::size_t i = last; i < operands_.size(); ++i) {
        if (operands_[i] != nullptr) {
            operands_[i]->remove_use(this, i);
        }
    }

    operands_.erase(operands_.begin() + static_cast<std::ptrdiff_t>(first),
                    operands_.begin() + static_cast<std::ptrdiff_t>(last));

    for (std::size_t i = first; i < operands_.size(); ++i) {
        if (operands_[i] != nullptr) {
            operands_[i]->add_use(this, i);
        }
    }
}

std::size_t User::operand_count() const {
    return operands_.size();
}

const std::vector<Value *> &User::operands() const {
    return operands_;
}

Instruction::Instruction(Type *type, OpID op, BasicBlock *parent, const std::string &name)
    : User(type, name), op_(op), parent_(parent) {
}

Instruction::OpID Instruction::op() const {
    return op_;
}

BasicBlock *Instruction::parent() const {
    return parent_;
}

void Instruction::set_parent(BasicBlock *parent) {
    parent_ = parent;
}

bool Instruction::is_terminator() const {
    return op_ == OpID::Ret || op_ == OpID::Br;
}

BinaryInst::BinaryInst(Type *type, OpID op, Value *lhs, Value *rhs, BasicBlock *parent,
                       const std::string &name)
    : Instruction(type, op, parent, name) {
    add_operand(lhs);
    add_operand(rhs);
}

Value *BinaryInst::lhs() const {
    return operand(0);
}

Value *BinaryInst::rhs() const {
    return operand(1);
}

std::string BinaryInst::print() const {
    return prefix_name(this) + op_to_string(op()) + " " + typed_value_ref(lhs()) + ", " +
           value_ref(rhs());
}

CmpInst::CmpInst(Type *result_type, OpID op, CmpPred pred, Value *lhs, Value *rhs,
                 BasicBlock *parent, const std::string &name)
    : Instruction(result_type, op, parent, name), pred_(pred) {
    add_operand(lhs);
    add_operand(rhs);
}

CmpPred CmpInst::pred() const {
    return pred_;
}

Value *CmpInst::lhs() const {
    return operand(0);
}

Value *CmpInst::rhs() const {
    return operand(1);
}

std::string CmpInst::print() const {
    return prefix_name(this) + op_to_string(op()) + " " + cmp_pred_to_string(pred_) + " " +
           typed_value_ref(lhs()) + ", " + value_ref(rhs());
}

CastInst::CastInst(Type *dst_type, OpID op, Value *src, BasicBlock *parent, const std::string &name)
    : Instruction(dst_type, op, parent, name) {
    add_operand(src);
}

Value *CastInst::src() const {
    return operand(0);
}

std::string CastInst::print() const {
    return prefix_name(this) + op_to_string(op()) + " " + typed_value_ref(src()) + " to " +
           type()->print();
}

SetVLInst::SetVLInst(Type *result_type, VectorType *vector_type, Value *avl, BasicBlock *parent,
                     const std::string &name)
    : Instruction(result_type, OpID::SetVL, parent, name), vector_type_(vector_type) {
    add_operand(avl);
}

VectorType *SetVLInst::vector_type() const {
    return vector_type_;
}

Value *SetVLInst::avl() const {
    return operand(0);
}

std::string SetVLInst::print() const {
    return prefix_name(this) + "setvl " +
           (vector_type_ == nullptr ? std::string("<null>") : vector_type_->print()) + ", " +
           typed_value_ref(avl());
}

SplatInst::SplatInst(VectorType *result_type, Value *scalar, BasicBlock *parent,
                     const std::string &name)
    : Instruction(result_type, OpID::Splat, parent, name) {
    add_operand(scalar);
}

Value *SplatInst::scalar() const {
    return operand(0);
}

std::string SplatInst::print() const {
    return prefix_name(this) + "splat " + typed_value_ref(scalar()) + " to " + type()->print();
}

StepVectorInst::StepVectorInst(VectorType *result_type, BasicBlock *parent, const std::string &name)
    : Instruction(result_type, OpID::StepVector, parent, name) {
}

std::string StepVectorInst::print() const {
    return prefix_name(this) + "stepvector " + type()->print();
}

ExtractElementInst::ExtractElementInst(Type *result_type, Value *vector, Value *index,
                                       BasicBlock *parent, const std::string &name)
    : Instruction(result_type, OpID::ExtractElement, parent, name) {
    add_operand(vector);
    add_operand(index);
}

Value *ExtractElementInst::vector() const {
    return operand(0);
}

Value *ExtractElementInst::index() const {
    return operand(1);
}

std::string ExtractElementInst::print() const {
    return prefix_name(this) + "extractelement " + typed_value_ref(vector()) + ", " +
           typed_value_ref(index());
}

InsertElementInst::InsertElementInst(VectorType *result_type, Value *vector, Value *element,
                                     Value *index, BasicBlock *parent, const std::string &name)
    : Instruction(result_type, OpID::InsertElement, parent, name) {
    add_operand(vector);
    add_operand(element);
    add_operand(index);
}

Value *InsertElementInst::vector() const {
    return operand(0);
}

Value *InsertElementInst::element() const {
    return operand(1);
}

Value *InsertElementInst::index() const {
    return operand(2);
}

std::string InsertElementInst::print() const {
    return prefix_name(this) + "insertelement " + typed_value_ref(vector()) + ", " +
           typed_value_ref(element()) + ", " + typed_value_ref(index());
}

ShuffleVectorInst::ShuffleVectorInst(VectorType *result_type, Value *lhs, Value *rhs,
                                     std::vector<std::int64_t> shuffle_mask, BasicBlock *parent,
                                     const std::string &name)
    : Instruction(result_type, OpID::ShuffleVector, parent, name),
      shuffle_mask_(std::move(shuffle_mask)) {
    add_operand(lhs);
    add_operand(rhs);
}

Value *ShuffleVectorInst::lhs() const {
    return operand(0);
}

Value *ShuffleVectorInst::rhs() const {
    return operand(1);
}

const std::vector<std::int64_t> &ShuffleVectorInst::shuffle_mask() const {
    return shuffle_mask_;
}

std::string ShuffleVectorInst::print() const {
    std::ostringstream oss;
    oss << prefix_name(this) << "shufflevector " << typed_value_ref(lhs()) << ", "
        << typed_value_ref(rhs()) << ", [";
    for (std::size_t i = 0; i < shuffle_mask_.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << shuffle_mask_[i];
    }
    oss << "] to " << type()->print();
    return oss.str();
}

VectorSelectInst::VectorSelectInst(VectorType *result_type, Value *condition, Value *true_value,
                                   Value *false_value, BasicBlock *parent, const std::string &name)
    : Instruction(result_type, OpID::VectorSelect, parent, name) {
    add_operand(condition);
    add_operand(true_value);
    add_operand(false_value);
}

Value *VectorSelectInst::condition() const {
    return operand(0);
}

Value *VectorSelectInst::true_value() const {
    return operand(1);
}

Value *VectorSelectInst::false_value() const {
    return operand(2);
}

std::string VectorSelectInst::print() const {
    return prefix_name(this) + "select " + typed_value_ref(condition()) + ", " +
           typed_value_ref(true_value()) + ", " + typed_value_ref(false_value());
}

VectorCastInst::VectorCastInst(VectorType *result_type, VectorCastKind kind, Value *source,
                               BasicBlock *parent, const std::string &name)
    : Instruction(result_type, OpID::VectorCast, parent, name), kind_(kind) {
    add_operand(source);
}

VectorCastKind VectorCastInst::kind() const {
    return kind_;
}

Value *VectorCastInst::source() const {
    return operand(0);
}

std::string VectorCastInst::print() const {
    return prefix_name(this) + "vector." + vector_cast_kind_to_string(kind_) + " " +
           typed_value_ref(source()) + " to " + type()->print();
}

FixedABIExtractLaneInst::FixedABIExtractLaneInst(Type *result_type, Value *aggregate,
                                                 std::uint64_t lane_index, BasicBlock *parent,
                                                 const std::string &name)
    : Instruction(result_type, OpID::FixedABIExtractLane, parent, name), lane_index_(lane_index) {
    add_operand(aggregate);
}

Value *FixedABIExtractLaneInst::aggregate() const {
    return operand(0);
}

std::uint64_t FixedABIExtractLaneInst::lane_index() const {
    return lane_index_;
}

std::string FixedABIExtractLaneInst::print() const {
    return prefix_name(this) + "abi.fixed.extract " + typed_value_ref(aggregate()) + ", lane " +
           std::to_string(lane_index_);
}

FixedABIPackInst::FixedABIPackInst(VectorType *result_type, const std::vector<Value *> &lane_values,
                                   BasicBlock *parent, const std::string &name)
    : Instruction(result_type, OpID::FixedABIPack, parent, name) {
    for (auto *value : lane_values)
        add_operand(value);
}

const std::vector<Value *> &FixedABIPackInst::lane_values() const {
    return operands();
}

std::string FixedABIPackInst::print() const {
    std::ostringstream oss;
    oss << prefix_name(this) << "abi.fixed.pack " << type()->print() << " [";
    for (std::size_t lane = 0; lane < lane_values().size(); ++lane) {
        if (lane != 0)
            oss << ", ";
        oss << typed_value_ref(lane_values()[lane]);
    }
    oss << "]";
    return oss.str();
}

FixedABIObjectLoadLaneInst::FixedABIObjectLoadLaneInst(Type *result_type, Value *object_ptr,
                                                       std::uint64_t lane_index, BasicBlock *parent,
                                                       const std::string &name)
    : Instruction(result_type, OpID::FixedABIObjectLoadLane, parent, name),
      lane_index_(lane_index) {
    add_operand(object_ptr);
}

Value *FixedABIObjectLoadLaneInst::object_ptr() const {
    return operand(0);
}

std::uint64_t FixedABIObjectLoadLaneInst::lane_index() const {
    return lane_index_;
}

std::string FixedABIObjectLoadLaneInst::print() const {
    return prefix_name(this) + "abi.fixed.load_lane " + typed_value_ref(object_ptr()) + ", lane " +
           std::to_string(lane_index_);
}

FixedABIObjectStoreLaneInst::FixedABIObjectStoreLaneInst(Type *void_type, Value *lane_value,
                                                         Value *object_ptr,
                                                         std::uint64_t lane_index,
                                                         BasicBlock *parent)
    : Instruction(void_type, OpID::FixedABIObjectStoreLane, parent), lane_index_(lane_index) {
    add_operand(lane_value);
    add_operand(object_ptr);
}

Value *FixedABIObjectStoreLaneInst::lane_value() const {
    return operand(0);
}

Value *FixedABIObjectStoreLaneInst::object_ptr() const {
    return operand(1);
}

std::uint64_t FixedABIObjectStoreLaneInst::lane_index() const {
    return lane_index_;
}

std::string FixedABIObjectStoreLaneInst::print() const {
    return "abi.fixed.store_lane " + typed_value_ref(lane_value()) + ", " +
           typed_value_ref(object_ptr()) + ", lane " + std::to_string(lane_index_);
}

VPInstruction::VPInstruction(Type *type, OpID op, const VPMetadata &metadata, bool has_passthrough,
                             BasicBlock *parent, const std::string &name)
    : Instruction(type, op, parent, name), has_passthrough_(has_passthrough),
      tail_policy_(metadata.tail_policy), mask_policy_(metadata.mask_policy) {
    add_operand(metadata.active_mask);
    add_operand(metadata.evl);
    if (has_passthrough_) {
        add_operand(metadata.passthrough);
    }
}

Value *VPInstruction::active_mask() const {
    return operand(0);
}

Value *VPInstruction::evl() const {
    return operand(1);
}

bool VPInstruction::has_passthrough() const {
    return has_passthrough_;
}

Value *VPInstruction::passthrough() const {
    return has_passthrough_ ? operand(2) : nullptr;
}

TailPolicy VPInstruction::tail_policy() const {
    return tail_policy_;
}

MaskPolicy VPInstruction::mask_policy() const {
    return mask_policy_;
}

VPMetadata VPInstruction::metadata() const {
    return {active_mask(), evl(), passthrough(), tail_policy_, mask_policy_};
}

std::size_t VPInstruction::data_operand_offset() const {
    return has_passthrough_ ? 3U : 2U;
}

VPBinaryInst::VPBinaryInst(VectorType *result_type, OpID binary_op, Value *lhs, Value *rhs,
                           const VPMetadata &metadata, BasicBlock *parent, const std::string &name)
    : VPInstruction(result_type, OpID::VPBinary, metadata, true, parent, name),
      binary_op_(binary_op) {
    add_operand(lhs);
    add_operand(rhs);
}

Instruction::OpID VPBinaryInst::binary_op() const {
    return binary_op_;
}

Value *VPBinaryInst::lhs() const {
    return operand(data_operand_offset());
}

Value *VPBinaryInst::rhs() const {
    return operand(data_operand_offset() + 1U);
}

std::string VPBinaryInst::print() const {
    return prefix_name(this) + "vp." + op_to_string(binary_op_) + " " + typed_value_ref(lhs()) +
           ", " + value_ref(rhs()) + vp_metadata_suffix(*this);
}

VPCmpInst::VPCmpInst(VectorType *result_type, OpID comparison_op, CmpPred pred, Value *lhs,
                     Value *rhs, const VPMetadata &metadata, BasicBlock *parent,
                     const std::string &name)
    : VPInstruction(result_type, OpID::VPCmp, metadata, true, parent, name),
      comparison_op_(comparison_op), pred_(pred) {
    add_operand(lhs);
    add_operand(rhs);
}

Instruction::OpID VPCmpInst::comparison_op() const {
    return comparison_op_;
}

CmpPred VPCmpInst::pred() const {
    return pred_;
}

Value *VPCmpInst::lhs() const {
    return operand(data_operand_offset());
}

Value *VPCmpInst::rhs() const {
    return operand(data_operand_offset() + 1U);
}

std::string VPCmpInst::print() const {
    return prefix_name(this) + "vp." + op_to_string(comparison_op_) + " " +
           cmp_pred_to_string(pred_) + " " + typed_value_ref(lhs()) + ", " + value_ref(rhs()) +
           vp_metadata_suffix(*this);
}

VPLoadInst::VPLoadInst(OpID op, VectorType *result_type, Value *ptr, std::size_t alignment,
                       const VPMetadata &metadata, BasicBlock *parent, const std::string &name)
    : VPInstruction(result_type, op, metadata, true, parent, name), alignment_(alignment) {
    add_operand(ptr);
}

Value *VPLoadInst::ptr() const {
    return operand(data_operand_offset());
}

std::size_t VPLoadInst::alignment() const {
    return alignment_;
}

bool VPLoadInst::is_masked_form() const {
    return op() == OpID::MaskedLoad;
}

std::string VPLoadInst::print() const {
    return prefix_name(this) + op_to_string(op()) + " " + type()->print() + ", " +
           typed_value_ref(ptr()) + ", align " + std::to_string(alignment_) +
           vp_metadata_suffix(*this);
}

VPStoreInst::VPStoreInst(OpID op, Type *void_type, Value *value, Value *ptr, std::size_t alignment,
                         const VPMetadata &metadata, BasicBlock *parent)
    : VPInstruction(void_type, op, metadata, false, parent), alignment_(alignment) {
    add_operand(value);
    add_operand(ptr);
}

Value *VPStoreInst::value() const {
    return operand(data_operand_offset());
}

Value *VPStoreInst::ptr() const {
    return operand(data_operand_offset() + 1U);
}

std::size_t VPStoreInst::alignment() const {
    return alignment_;
}

bool VPStoreInst::is_masked_form() const {
    return op() == OpID::MaskedStore;
}

std::string VPStoreInst::print() const {
    return op_to_string(op()) + " " + typed_value_ref(value()) + ", " + typed_value_ref(ptr()) +
           ", align " + std::to_string(alignment_) + vp_metadata_suffix(*this);
}

VPGatherInst::VPGatherInst(VectorType *result_type, Value *base_ptr, Value *indices,
                           std::size_t alignment, const VPMetadata &metadata, BasicBlock *parent,
                           const std::string &name)
    : VPInstruction(result_type, OpID::VPGather, metadata, true, parent, name),
      alignment_(alignment) {
    add_operand(base_ptr);
    add_operand(indices);
}

Value *VPGatherInst::base_ptr() const {
    return operand(data_operand_offset());
}

Value *VPGatherInst::indices() const {
    return operand(data_operand_offset() + 1U);
}

std::size_t VPGatherInst::alignment() const {
    return alignment_;
}

std::string VPGatherInst::print() const {
    return prefix_name(this) + "vp.gather " + type()->print() + ", base " +
           typed_value_ref(base_ptr()) + ", indices " + typed_value_ref(indices()) + ", align " +
           std::to_string(alignment_) + vp_metadata_suffix(*this);
}

VPScatterInst::VPScatterInst(Type *void_type, Value *value, Value *base_ptr, Value *indices,
                             std::size_t alignment, const VPMetadata &metadata, BasicBlock *parent)
    : VPInstruction(void_type, OpID::VPScatter, metadata, false, parent), alignment_(alignment) {
    add_operand(value);
    add_operand(base_ptr);
    add_operand(indices);
}

Value *VPScatterInst::value() const {
    return operand(data_operand_offset());
}

Value *VPScatterInst::base_ptr() const {
    return operand(data_operand_offset() + 1U);
}

Value *VPScatterInst::indices() const {
    return operand(data_operand_offset() + 2U);
}

std::size_t VPScatterInst::alignment() const {
    return alignment_;
}

std::string VPScatterInst::print() const {
    return "vp.scatter " + typed_value_ref(value()) + ", base " + typed_value_ref(base_ptr()) +
           ", indices " + typed_value_ref(indices()) + ", align " + std::to_string(alignment_) +
           vp_metadata_suffix(*this);
}

VPReductionInst::VPReductionInst(Type *result_type, ReductionKind kind, bool ordered, Value *vector,
                                 const VPMetadata &metadata, BasicBlock *parent,
                                 const std::string &name)
    : VPInstruction(result_type, OpID::VPReduction, metadata, true, parent, name), kind_(kind),
      ordered_(ordered) {
    add_operand(vector);
}

ReductionKind VPReductionInst::kind() const {
    return kind_;
}

bool VPReductionInst::ordered() const {
    return ordered_;
}

Value *VPReductionInst::vector() const {
    return operand(data_operand_offset());
}

std::string VPReductionInst::print() const {
    const auto *vector_type = dynamic_cast<const VectorType *>(vector()->type());
    const bool floating = vector_type != nullptr && vector_type->element_type()->is_scalar_float();
    return prefix_name(this) + "vp.reduce." + (ordered_ ? "ordered." : "") +
           reduction_kind_to_string(kind_, floating) + " " + typed_value_ref(vector()) +
           vp_metadata_suffix(*this);
}

AllocaInst::AllocaInst(Type *ptr_type, Type *allocated_type, BasicBlock *parent,
                       const std::string &name)
    : Instruction(ptr_type, OpID::Alloca, parent, name), allocated_type_(allocated_type) {
}

Type *AllocaInst::allocated_type() const {
    return allocated_type_;
}

std::string AllocaInst::print() const {
    return prefix_name(this) + "alloca " + allocated_type_->print();
}

GetElementPtrInst::GetElementPtrInst(Type *ptr_type, Value *base_ptr,
                                     const std::vector<Value *> &indices, BasicBlock *parent,
                                     const std::string &name)
    : Instruction(ptr_type, OpID::GetElementPtr, parent, name) {
    add_operand(base_ptr);
    for (auto *idx : indices) {
        add_operand(idx);
    }
}

Value *GetElementPtrInst::base_ptr() const {
    return operand(0);
}

std::vector<Value *> GetElementPtrInst::indices() const {
    std::vector<Value *> out;
    for (std::size_t i = 1; i < operand_count(); ++i) {
        out.push_back(operand(i));
    }
    return out;
}

std::string GetElementPtrInst::print() const {
    std::ostringstream oss;
    oss << prefix_name(this) << "gep " << typed_value_ref(base_ptr());
    for (std::size_t i = 1; i < operand_count(); ++i) {
        oss << ", " << typed_value_ref(operand(i));
    }
    oss << " to " << type()->print();
    return oss.str();
}

LoadInst::LoadInst(Type *loaded_type, Value *ptr, BasicBlock *parent, const std::string &name)
    : Instruction(loaded_type, OpID::Load, parent, name) {
    add_operand(ptr);
}

Value *LoadInst::ptr() const {
    return operand(0);
}

std::string LoadInst::print() const {
    return prefix_name(this) + "load " + type()->print() + ", " + typed_value_ref(ptr());
}

StoreInst::StoreInst(Type *void_type, Value *value, Value *ptr, BasicBlock *parent)
    : Instruction(void_type, OpID::Store, parent, "") {
    add_operand(value);
    add_operand(ptr);
}

Value *StoreInst::value() const {
    return operand(0);
}

Value *StoreInst::ptr() const {
    return operand(1);
}

std::string StoreInst::print() const {
    return "store " + typed_value_ref(value()) + ", " + typed_value_ref(ptr());
}

MemZeroInst::MemZeroInst(Type *void_type, Value *ptr, Value *byte_value, Value *byte_count,
                         BasicBlock *parent)
    : Instruction(void_type, OpID::MemZero, parent, "") {
    add_operand(ptr);
    add_operand(byte_value);
    add_operand(byte_count);
}

Value *MemZeroInst::ptr() const {
    return operand(0);
}

Value *MemZeroInst::byte_value() const {
    return operand(1);
}

Value *MemZeroInst::byte_count() const {
    return operand(2);
}

std::string MemZeroInst::print() const {
    if (auto *byte = dynamic_cast<ConstantInt *>(byte_value())) {
        if (byte->value() == 0) {
            return "memzero " + typed_value_ref(ptr()) + ", " + typed_value_ref(byte_count());
        }
    }
    return "memset " + typed_value_ref(ptr()) + ", " + typed_value_ref(byte_value()) + ", " +
           typed_value_ref(byte_count());
}

CallInst::CallInst(Type *return_type, Value *callee, const std::vector<Value *> &args,
                   BasicBlock *parent, const std::string &name)
    : Instruction(return_type, OpID::Call, parent, name) {
    add_operand(callee);
    for (auto *arg : args) {
        add_operand(arg);
    }
}

Value *CallInst::callee() const {
    return operand(0);
}

std::vector<Value *> CallInst::args() const {
    std::vector<Value *> out;
    for (std::size_t i = 1; i < operand_count(); ++i) {
        out.push_back(operand(i));
    }
    return out;
}

void CallInst::remove_arg(std::size_t arg_index) {
    erase_operands(arg_index + 1, 1);
}

std::string CallInst::print() const {
    std::ostringstream oss;
    if (!name().empty()) {
        oss << "%" << name() << " = ";
    }
    oss << "call " << type()->print() << " " << value_ref(callee()) << "(";
    for (std::size_t i = 1; i < operand_count(); ++i) {
        if (i != 1) {
            oss << ", ";
        }
        oss << typed_value_ref(operand(i));
    }
    oss << ")";
    return oss.str();
}

ReturnInst::ReturnInst(Type *void_type, Value *value, BasicBlock *parent)
    : Instruction(void_type, OpID::Ret, parent, "") {
    if (value != nullptr) {
        add_operand(value);
    }
}

bool ReturnInst::has_value() const {
    return operand_count() == 1;
}

Value *ReturnInst::value() const {
    if (!has_value()) {
        return nullptr;
    }
    return operand(0);
}

std::string ReturnInst::print() const {
    if (!has_value()) {
        return "ret void";
    }
    return "ret " + typed_value_ref(value());
}

BranchInst::BranchInst(Type *void_type, BasicBlock *target, BasicBlock *parent)
    : Instruction(void_type, OpID::Br, parent, "") {
    add_operand(target);
}

BranchInst::BranchInst(Type *void_type, Value *cond, BasicBlock *true_bb, BasicBlock *false_bb,
                       BasicBlock *parent)
    : Instruction(void_type, OpID::Br, parent, "") {
    add_operand(cond);
    add_operand(true_bb);
    add_operand(false_bb);
}

bool BranchInst::is_conditional() const {
    return operand_count() == 3;
}

Value *BranchInst::cond() const {
    if (!is_conditional()) {
        return nullptr;
    }
    return operand(0);
}

BasicBlock *BranchInst::true_bb() const {
    if (!is_conditional()) {
        return nullptr;
    }
    return static_cast<BasicBlock *>(operand(1));
}

BasicBlock *BranchInst::false_bb() const {
    if (!is_conditional()) {
        return nullptr;
    }
    return static_cast<BasicBlock *>(operand(2));
}

BasicBlock *BranchInst::target_bb() const {
    if (is_conditional()) {
        return nullptr;
    }
    return static_cast<BasicBlock *>(operand(0));
}

std::string BranchInst::print() const {
    if (is_conditional()) {
        return "br " + typed_value_ref(cond()) + ", " + value_ref(true_bb()) + ", " +
               value_ref(false_bb());
    }
    return "br " + value_ref(target_bb());
}

PhiInst::PhiInst(Type *type, BasicBlock *parent, const std::string &name)
    : Instruction(type, OpID::Phi, parent, name) {
}

void PhiInst::add_incoming(Value *value, BasicBlock *from) {
    incoming_.push_back({value, from});
    add_operand(value);
    add_operand(from);
}

void PhiInst::remove_incoming_from(BasicBlock *from) {
    for (std::size_t i = 0; i < incoming_.size();) {
        if (incoming_[i].second != from) {
            ++i;
            continue;
        }
        incoming_.erase(incoming_.begin() + static_cast<std::ptrdiff_t>(i));
        erase_operands(i * 2, 2);
    }
}

const std::vector<std::pair<Value *, BasicBlock *>> &PhiInst::incoming() const {
    return incoming_;
}

void PhiInst::set_operand(std::size_t index, Value *value) {
    User::set_operand(index, value);
    const std::size_t incoming_index = index / 2;
    if (incoming_index >= incoming_.size()) {
        return;
    }
    if (index % 2 == 0) {
        incoming_[incoming_index].first = value;
    } else {
        incoming_[incoming_index].second = static_cast<BasicBlock *>(value);
    }
}

std::string PhiInst::print() const {
    std::ostringstream oss;
    oss << prefix_name(this) << "phi ";
    for (std::size_t i = 0; i < incoming_.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << "[" << value_ref(incoming_[i].first) << ", " << value_ref(incoming_[i].second)
            << "]";
    }
    oss << " : " << type()->print();
    return oss.str();
}

Argument::Argument(Type *type, const std::string &name, Function *parent, std::size_t index)
    : Value(type, name), parent_(parent), index_(index) {
}

Function *Argument::parent() const {
    return parent_;
}

std::size_t Argument::index() const {
    return index_;
}

void Argument::set_index(std::size_t index) {
    index_ = index;
}

std::string Argument::print() const {
    return type()->print() + " %" + name();
}

BasicBlock::BasicBlock(Type *label_type, const std::string &name, Function *parent)
    : Value(label_type, name), parent_(parent) {
}

Instruction *BasicBlock::append_instruction(std::unique_ptr<Instruction> inst) {
    auto *raw = inst.get();
    raw->set_parent(this);
    instructions_.push_back(std::move(inst));
    return raw;
}

Instruction *BasicBlock::insert_before_terminator(std::unique_ptr<Instruction> inst) {
    auto *raw = inst.get();
    raw->set_parent(this);
    if (has_terminator()) {
        instructions_.insert(std::prev(instructions_.end()), std::move(inst));
    } else {
        instructions_.push_back(std::move(inst));
    }
    return raw;
}

bool BasicBlock::has_terminator() const {
    if (instructions_.empty()) {
        return false;
    }
    return instructions_.back()->is_terminator();
}

Instruction *BasicBlock::terminator() const {
    if (!has_terminator()) {
        return nullptr;
    }
    return instructions_.back().get();
}

void BasicBlock::add_predecessor(BasicBlock *pred) {
    if (std::find(predecessors_.begin(), predecessors_.end(), pred) == predecessors_.end()) {
        predecessors_.push_back(pred);
    }
}

void BasicBlock::add_successor(BasicBlock *succ) {
    if (std::find(successors_.begin(), successors_.end(), succ) == successors_.end()) {
        successors_.push_back(succ);
    }
}

void BasicBlock::remove_predecessor(BasicBlock *pred) {
    predecessors_.erase(std::remove(predecessors_.begin(), predecessors_.end(), pred),
                        predecessors_.end());
}

void BasicBlock::remove_successor(BasicBlock *succ) {
    successors_.erase(std::remove(successors_.begin(), successors_.end(), succ), successors_.end());
}

const std::vector<BasicBlock *> &BasicBlock::predecessors() const {
    return predecessors_;
}

const std::vector<BasicBlock *> &BasicBlock::successors() const {
    return successors_;
}

std::list<std::unique_ptr<Instruction>> &BasicBlock::instructions() {
    return instructions_;
}

const std::list<std::unique_ptr<Instruction>> &BasicBlock::instructions() const {
    return instructions_;
}

Function *BasicBlock::parent() const {
    return parent_;
}

std::string BasicBlock::print() const {
    return name() + ":";
}

Function::Function(FunctionType *type, const std::string &name, Module *parent, bool is_external)
    : Value(type, name), parent_(parent), is_external_(is_external), next_block_id_(0) {
}

Function::~Function() {
    for (auto &block : blocks_) {
        for (auto &inst : block->instructions()) {
            inst->drop_all_operands();
        }
    }
}

FunctionType *Function::function_type() const {
    return static_cast<FunctionType *>(type());
}

Type *Function::return_type() const {
    return function_type()->return_type();
}

Module *Function::parent() const {
    return parent_;
}

bool Function::is_external() const {
    return is_external_;
}

void Function::set_external(bool is_external) {
    is_external_ = is_external;
}

void Function::set_function_type(FunctionType *type) {
    set_type(type);
}

Argument *Function::add_argument(Type *type, const std::string &name) {
    auto arg = std::make_unique<Argument>(type, name, this, args_.size());
    auto *raw = arg.get();
    args_.push_back(std::move(arg));
    return raw;
}

void Function::keep_arguments(const std::vector<bool> &keep) {
    if (keep.size() != args_.size()) {
        return;
    }
    std::vector<std::unique_ptr<Argument>> kept;
    kept.reserve(args_.size());
    for (std::size_t i = 0; i < args_.size(); ++i) {
        if (!keep[i]) {
            continue;
        }
        args_[i]->set_index(kept.size());
        kept.push_back(std::move(args_[i]));
    }
    args_ = std::move(kept);
}

BasicBlock *Function::create_block(const std::string &name) {
    // 始终生成唯一名：原名 + 计数器
    std::string block_name = name.empty() ? "bb" : name;
    block_name += "." + std::to_string(next_block_id_++);
    auto block = std::make_unique<BasicBlock>(parent_->types().label_ty(), block_name, this);
    auto *raw = block.get();
    blocks_.push_back(std::move(block));
    return raw;
}

BasicBlock *Function::create_block_exact(const std::string &name) {
    if (name.empty()) {
        throw std::invalid_argument("OIR exact basic block name must not be empty");
    }
    for (const auto &block : blocks_) {
        if (block->name() == name) {
            throw std::invalid_argument("duplicate OIR basic block name: " + name);
        }
    }
    auto block = std::make_unique<BasicBlock>(parent_->types().label_ty(), name, this);
    auto *raw = block.get();
    blocks_.push_back(std::move(block));
    return raw;
}

void Function::erase_block(BasicBlock *block) {
    auto it = std::find_if(
        blocks_.begin(), blocks_.end(),
        [block](const std::unique_ptr<BasicBlock> &candidate) { return candidate.get() == block; });
    if (it != blocks_.end()) {
        for (auto &inst : (*it)->instructions()) {
            inst->drop_all_operands();
        }
        blocks_.erase(it);
    }
}

BasicBlock *Function::entry_block() const {
    if (blocks_.empty()) {
        return nullptr;
    }
    return blocks_.front().get();
}

std::vector<std::unique_ptr<Argument>> &Function::args() {
    return args_;
}

const std::vector<std::unique_ptr<Argument>> &Function::args() const {
    return args_;
}

std::list<std::unique_ptr<BasicBlock>> &Function::blocks() {
    return blocks_;
}

const std::list<std::unique_ptr<BasicBlock>> &Function::blocks() const {
    return blocks_;
}

std::string Function::print() const {
    std::ostringstream oss;
    oss << return_type()->print() << " @" << name() << "(";
    for (std::size_t i = 0; i < args_.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << args_[i]->print();
    }
    if (function_type()->is_variadic()) {
        if (!args_.empty()) {
            oss << ", ";
        }
        oss << "...";
    }
    oss << ")";
    return oss.str();
}

GlobalVariable::GlobalVariable(Type *ptr_type, Type *value_type, const std::string &name,
                               bool is_const, Constant *initializer)
    : Value(ptr_type, name), value_type_(value_type), is_const_(is_const),
      initializer_(initializer) {
}

Type *GlobalVariable::value_type() const {
    return value_type_;
}

bool GlobalVariable::is_const() const {
    return is_const_;
}

Constant *GlobalVariable::initializer() const {
    return initializer_;
}

void GlobalVariable::set_initializer(Constant *initializer) {
    initializer_ = initializer;
}

Constant *GlobalVariable::init_value() const {
    return initializer();
}

std::string GlobalVariable::print() const {
    std::ostringstream oss;
    oss << "@" << name() << " = " << (is_const_ ? "constant " : "global ") << value_type_->print();
    if (initializer_ != nullptr) {
        oss << " " << initializer_->print();
    }
    return oss.str();
}

Module::Module(const std::string &name) : name_(name) {
}

void Module::replace_with(Module &&other) {
    if (this == &other) {
        return;
    }

    // Destroy users before the constants and types referenced by their operands.
    functions_.clear();
    function_table_.clear();
    globals_.clear();
    global_table_.clear();
    owned_constants_.clear();

    name_ = std::move(other.name_);
    types_ = std::move(other.types_);
    owned_constants_ = std::move(other.owned_constants_);
    globals_ = std::move(other.globals_);
    functions_ = std::move(other.functions_);
    function_table_ = std::move(other.function_table_);
    global_table_ = std::move(other.global_table_);
    for (auto &function : functions_) {
        function->parent_ = this;
    }
}

const std::string &Module::name() const {
    return name_;
}

TypeContext &Module::types() {
    return types_;
}

const TypeContext &Module::types() const {
    return types_;
}

Function *Module::create_function(const std::string &name, FunctionType *type, bool is_external) {
    auto found = function_table_.find(name);
    if (found != function_table_.end()) {
        return found->second;
    }

    auto fn = std::make_unique<Function>(type, name, this, is_external);
    auto *raw = fn.get();

    const auto &param_types = type->param_types();
    for (std::size_t i = 0; i < param_types.size(); ++i) {
        raw->add_argument(param_types[i], "arg" + std::to_string(i));
    }

    functions_.push_back(std::move(fn));
    function_table_[name] = raw;
    return raw;
}

Function *Module::get_function(const std::string &name) const {
    auto found = function_table_.find(name);
    if (found == function_table_.end()) {
        return nullptr;
    }
    return found->second;
}

GlobalVariable *Module::create_global(const std::string &name, Type *value_type, bool is_const,
                                      Constant *initializer) {
    auto found = global_table_.find(name);
    if (found != global_table_.end()) {
        return found->second;
    }

    auto *ptr_type = types_.ptr_ty(value_type);
    auto global =
        std::make_unique<GlobalVariable>(ptr_type, value_type, name, is_const, initializer);
    auto *raw = global.get();
    globals_.push_back(std::move(global));
    global_table_[name] = raw;
    return raw;
}

GlobalVariable *Module::get_global(const std::string &name) const {
    auto found = global_table_.find(name);
    if (found == global_table_.end()) {
        return nullptr;
    }
    return found->second;
}

ConstantInt *Module::create_i1(bool value) {
    auto c = std::make_unique<ConstantInt>(types_.int1_ty(), value ? 1 : 0);
    auto *raw = c.get();
    owned_constants_.push_back(std::move(c));
    return raw;
}

ConstantInt *Module::create_i32(std::int64_t value) {
    auto c = std::make_unique<ConstantInt>(types_.int32_ty(), value);
    auto *raw = c.get();
    owned_constants_.push_back(std::move(c));
    return raw;
}

ConstantFloat *Module::create_f32(float value) {
    auto c = std::make_unique<ConstantFloat>(types_.float_ty(), value);
    auto *raw = c.get();
    owned_constants_.push_back(std::move(c));
    return raw;
}

ConstantZero *Module::create_zero(Type *type) {
    auto zero = std::make_unique<ConstantZero>(type);
    auto *raw = zero.get();
    owned_constants_.push_back(std::move(zero));
    return raw;
}

ConstantArray *Module::create_constant_array(ArrayType *type,
                                             const std::vector<Constant *> &elements) {
    auto constant = std::make_unique<ConstantArray>(type, elements);
    auto *raw = constant.get();
    owned_constants_.push_back(std::move(constant));
    return raw;
}

ConstantVector *Module::create_constant_vector(VectorType *type,
                                               const std::vector<Constant *> &elements) {
    auto constant = std::make_unique<ConstantVector>(type, elements);
    auto *raw = constant.get();
    owned_constants_.push_back(std::move(constant));
    return raw;
}

ConstantMask *Module::create_constant_mask(VectorType *type,
                                           const std::vector<std::uint8_t> &packed_bits) {
    auto constant = std::make_unique<ConstantMask>(type, packed_bits);
    auto *raw = constant.get();
    owned_constants_.push_back(std::move(constant));
    return raw;
}

UndefValue *Module::create_undef(Type *type) {
    auto undef = std::make_unique<UndefValue>(type);
    auto *raw = undef.get();
    owned_constants_.push_back(std::move(undef));
    return raw;
}

std::vector<std::unique_ptr<GlobalVariable>> &Module::globals() {
    return globals_;
}

const std::vector<std::unique_ptr<GlobalVariable>> &Module::globals() const {
    return globals_;
}

const std::vector<std::unique_ptr<Value>> &Module::owned_constants() const {
    return owned_constants_;
}

std::vector<std::unique_ptr<Function>> &Module::functions() {
    return functions_;
}

const std::vector<std::unique_ptr<Function>> &Module::functions() const {
    return functions_;
}

std::string Module::print() const {
    std::ostringstream oss;
    oss << "; module: " << name_ << "\n\n";

    for (const auto &global : globals_) {
        oss << global->print() << "\n";
    }

    if (!globals_.empty()) {
        oss << "\n";
    }

    for (const auto &function : functions_) {
        if (function->is_external()) {
            oss << "declare " << function->print() << "\n\n";
            continue;
        }

        oss << "define " << function->print() << " {\n";
        for (const auto &block : function->blocks()) {
            oss << block->print() << "\n";
            for (const auto &inst : block->instructions()) {
                oss << "  " << inst->print() << "\n";
            }
        }
        oss << "}\n\n";
    }

    return oss.str();
}

bool Module::verify(std::string *message) const {
    auto result = Verifier::verify_module(*this);
    if (message != nullptr) {
        *message = result.message;
    }
    return result.ok;
}

IRBuilder::IRBuilder(Module *module) : module_(module), insert_block_(nullptr) {
}

Module *IRBuilder::module() const {
    return module_;
}

BasicBlock *IRBuilder::insert_block() const {
    return insert_block_;
}

void IRBuilder::set_insert_point(BasicBlock *block) {
    insert_block_ = block;
}

void IRBuilder::clear_insert_point() {
    insert_block_ = nullptr;
}

ConstantInt *IRBuilder::i1(bool value) const {
    return module_->create_i1(value);
}

ConstantInt *IRBuilder::i32(std::int64_t value) const {
    return module_->create_i32(value);
}

ConstantFloat *IRBuilder::f32(float value) const {
    return module_->create_f32(value);
}

ConstantZero *IRBuilder::zero(Type *type) const {
    return module_->create_zero(type);
}

UndefValue *IRBuilder::undef(Type *type) const {
    return module_->create_undef(type);
}

BinaryInst *IRBuilder::create_binary(Instruction::OpID op, Value *lhs, Value *rhs,
                                     const std::string &name) {
    return append(std::make_unique<BinaryInst>(lhs->type(), op, lhs, rhs, insert_block_, name));
}

CmpInst *IRBuilder::create_icmp(CmpPred pred, Value *lhs, Value *rhs, const std::string &name) {
    return append(std::make_unique<CmpInst>(comparison_result_type(*module_, lhs),
                                            Instruction::OpID::ICmp, pred, lhs, rhs, insert_block_,
                                            name));
}

CmpInst *IRBuilder::create_fcmp(CmpPred pred, Value *lhs, Value *rhs, const std::string &name) {
    return append(std::make_unique<CmpInst>(comparison_result_type(*module_, lhs),
                                            Instruction::OpID::FCmp, pred, lhs, rhs, insert_block_,
                                            name));
}

CastInst *IRBuilder::create_zext(Value *src, Type *dst_type, const std::string &name) {
    return append(
        std::make_unique<CastInst>(dst_type, Instruction::OpID::ZExt, src, insert_block_, name));
}

CastInst *IRBuilder::create_sitofp(Value *src, Type *dst_type, const std::string &name) {
    return append(
        std::make_unique<CastInst>(dst_type, Instruction::OpID::SIToFP, src, insert_block_, name));
}

CastInst *IRBuilder::create_fptosi(Value *src, Type *dst_type, const std::string &name) {
    return append(
        std::make_unique<CastInst>(dst_type, Instruction::OpID::FPToSI, src, insert_block_, name));
}

SetVLInst *IRBuilder::create_set_vl(VectorType *vector_type, Value *avl, const std::string &name) {
    return append(std::make_unique<SetVLInst>(module_->types().int32_ty(), vector_type, avl,
                                              insert_block_, name));
}

SplatInst *IRBuilder::create_splat(VectorType *result_type, Value *scalar,
                                   const std::string &name) {
    return append(std::make_unique<SplatInst>(result_type, scalar, insert_block_, name));
}

StepVectorInst *IRBuilder::create_step_vector(VectorType *result_type, const std::string &name) {
    return append(std::make_unique<StepVectorInst>(result_type, insert_block_, name));
}

ExtractElementInst *IRBuilder::create_extract_element(Value *vector, Value *index,
                                                      const std::string &name) {
    const auto *vector_type =
        vector == nullptr ? nullptr : dynamic_cast<const VectorType *>(vector->type());
    if (vector_type == nullptr) {
        throw std::invalid_argument("OIR extractelement builder requires a vector operand");
    }
    return append(std::make_unique<ExtractElementInst>(vector_type->element_type(), vector, index,
                                                       insert_block_, name));
}

InsertElementInst *IRBuilder::create_insert_element(Value *vector, Value *element, Value *index,
                                                    const std::string &name) {
    auto *vector_type = vector == nullptr ? nullptr : dynamic_cast<VectorType *>(vector->type());
    if (vector_type == nullptr) {
        throw std::invalid_argument("OIR insertelement builder requires a vector operand");
    }
    return append(std::make_unique<InsertElementInst>(vector_type, vector, element, index,
                                                      insert_block_, name));
}

ShuffleVectorInst *IRBuilder::create_shuffle_vector(VectorType *result_type, Value *lhs, Value *rhs,
                                                    const std::vector<std::int64_t> &shuffle_mask,
                                                    const std::string &name) {
    return append(std::make_unique<ShuffleVectorInst>(result_type, lhs, rhs, shuffle_mask,
                                                      insert_block_, name));
}

VectorSelectInst *IRBuilder::create_vector_select(Value *condition, Value *true_value,
                                                  Value *false_value, const std::string &name) {
    auto *result_type =
        true_value == nullptr ? nullptr : dynamic_cast<VectorType *>(true_value->type());
    if (result_type == nullptr) {
        throw std::invalid_argument("OIR vector select builder requires vector values");
    }
    return append(std::make_unique<VectorSelectInst>(result_type, condition, true_value,
                                                     false_value, insert_block_, name));
}

VectorCastInst *IRBuilder::create_vector_cast(VectorCastKind kind, VectorType *result_type,
                                              Value *source, const std::string &name) {
    return append(std::make_unique<VectorCastInst>(result_type, kind, source, insert_block_, name));
}

FixedABIExtractLaneInst *IRBuilder::create_fixed_abi_extract_lane(Value *aggregate,
                                                                  std::uint64_t lane_index,
                                                                  const std::string &name) {
    auto *vector_type =
        aggregate == nullptr ? nullptr : dynamic_cast<VectorType *>(aggregate->type());
    if (vector_type == nullptr || vector_type->element_count().is_scalable() ||
        lane_index >= vector_type->element_count().min_lanes) {
        throw std::invalid_argument(
            "OIR fixed ABI extract builder requires an in-range fixed-vector lane");
    }
    return append(std::make_unique<FixedABIExtractLaneInst>(vector_type->element_type(), aggregate,
                                                            lane_index, insert_block_, name));
}

FixedABIPackInst *IRBuilder::create_fixed_abi_pack(VectorType *result_type,
                                                   const std::vector<Value *> &lane_values,
                                                   const std::string &name) {
    if (result_type == nullptr || result_type->element_count().is_scalable() ||
        lane_values.size() != result_type->element_count().min_lanes ||
        std::any_of(lane_values.begin(), lane_values.end(), [&](const Value *value) {
            return value == nullptr || value->type() != result_type->element_type();
        })) {
        throw std::invalid_argument(
            "OIR fixed ABI pack builder requires one correctly typed lane per fixed lane");
    }
    return append(
        std::make_unique<FixedABIPackInst>(result_type, lane_values, insert_block_, name));
}

FixedABIObjectLoadLaneInst *IRBuilder::create_fixed_abi_object_load_lane(Value *object_ptr,
                                                                         std::uint64_t lane_index,
                                                                         const std::string &name) {
    auto *pointer_type =
        object_ptr == nullptr ? nullptr : dynamic_cast<PointerType *>(object_ptr->type());
    auto *vector_type = pointer_type == nullptr
                            ? nullptr
                            : dynamic_cast<VectorType *>(pointer_type->element_type());
    if (vector_type == nullptr || vector_type->element_count().is_scalable() ||
        lane_index >= vector_type->element_count().min_lanes) {
        throw std::invalid_argument(
            "OIR fixed ABI object load builder requires ptr<fixed-vector> and in-range lane");
    }
    return append(std::make_unique<FixedABIObjectLoadLaneInst>(
        vector_type->element_type(), object_ptr, lane_index, insert_block_, name));
}

FixedABIObjectStoreLaneInst *
IRBuilder::create_fixed_abi_object_store_lane(Value *lane_value, Value *object_ptr,
                                              std::uint64_t lane_index) {
    auto *pointer_type =
        object_ptr == nullptr ? nullptr : dynamic_cast<PointerType *>(object_ptr->type());
    auto *vector_type = pointer_type == nullptr
                            ? nullptr
                            : dynamic_cast<VectorType *>(pointer_type->element_type());
    if (lane_value == nullptr || vector_type == nullptr ||
        vector_type->element_count().is_scalable() ||
        lane_index >= vector_type->element_count().min_lanes ||
        lane_value->type() != vector_type->element_type()) {
        throw std::invalid_argument(
            "OIR fixed ABI object store builder requires ptr<fixed-vector>, matching value, "
            "and in-range lane");
    }
    return append(std::make_unique<FixedABIObjectStoreLaneInst>(
        module_->types().void_ty(), lane_value, object_ptr, lane_index, insert_block_));
}

VPBinaryInst *IRBuilder::create_vp_binary(Instruction::OpID binary_op, Value *lhs, Value *rhs,
                                          Value *active_mask, Value *evl, Value *passthrough,
                                          TailPolicy tail_policy, MaskPolicy mask_policy,
                                          const std::string &name) {
    auto *result_type = lhs == nullptr ? nullptr : dynamic_cast<VectorType *>(lhs->type());
    if (result_type == nullptr) {
        throw std::invalid_argument("OIR VP binary builder requires vector operands");
    }
    VPMetadata metadata{active_mask, evl, passthrough, tail_policy, mask_policy};
    return append(std::make_unique<VPBinaryInst>(result_type, binary_op, lhs, rhs, metadata,
                                                 insert_block_, name));
}

VPCmpInst *IRBuilder::create_vp_icmp(CmpPred pred, Value *lhs, Value *rhs, Value *active_mask,
                                     Value *evl, Value *passthrough, TailPolicy tail_policy,
                                     MaskPolicy mask_policy, const std::string &name) {
    const auto *operand_type =
        lhs == nullptr ? nullptr : dynamic_cast<const VectorType *>(lhs->type());
    if (operand_type == nullptr) {
        throw std::invalid_argument("OIR VP compare builder requires vector operands");
    }
    auto *result_type =
        module_->types().vector_ty(module_->types().int1_ty(), operand_type->element_count());
    VPMetadata metadata{active_mask, evl, passthrough, tail_policy, mask_policy};
    return append(std::make_unique<VPCmpInst>(result_type, Instruction::OpID::ICmp, pred, lhs, rhs,
                                              metadata, insert_block_, name));
}

VPCmpInst *IRBuilder::create_vp_fcmp(CmpPred pred, Value *lhs, Value *rhs, Value *active_mask,
                                     Value *evl, Value *passthrough, TailPolicy tail_policy,
                                     MaskPolicy mask_policy, const std::string &name) {
    const auto *operand_type =
        lhs == nullptr ? nullptr : dynamic_cast<const VectorType *>(lhs->type());
    if (operand_type == nullptr) {
        throw std::invalid_argument("OIR VP compare builder requires vector operands");
    }
    auto *result_type =
        module_->types().vector_ty(module_->types().int1_ty(), operand_type->element_count());
    VPMetadata metadata{active_mask, evl, passthrough, tail_policy, mask_policy};
    return append(std::make_unique<VPCmpInst>(result_type, Instruction::OpID::FCmp, pred, lhs, rhs,
                                              metadata, insert_block_, name));
}

VPLoadInst *IRBuilder::create_vp_load(VectorType *result_type, Value *ptr, Value *active_mask,
                                      Value *evl, Value *passthrough, TailPolicy tail_policy,
                                      MaskPolicy mask_policy, std::size_t alignment,
                                      const std::string &name) {
    VPMetadata metadata{active_mask, evl, passthrough, tail_policy, mask_policy};
    return append(std::make_unique<VPLoadInst>(Instruction::OpID::VPLoad, result_type, ptr,
                                               alignment, metadata, insert_block_, name));
}

VPStoreInst *IRBuilder::create_vp_store(Value *value, Value *ptr, Value *active_mask, Value *evl,
                                        TailPolicy tail_policy, MaskPolicy mask_policy,
                                        std::size_t alignment) {
    VPMetadata metadata{active_mask, evl, nullptr, tail_policy, mask_policy};
    return append(std::make_unique<VPStoreInst>(Instruction::OpID::VPStore,
                                                module_->types().void_ty(), value, ptr, alignment,
                                                metadata, insert_block_));
}

VPLoadInst *IRBuilder::create_masked_load(VectorType *result_type, Value *ptr, Value *active_mask,
                                          Value *evl, Value *passthrough, TailPolicy tail_policy,
                                          MaskPolicy mask_policy, std::size_t alignment,
                                          const std::string &name) {
    VPMetadata metadata{active_mask, evl, passthrough, tail_policy, mask_policy};
    return append(std::make_unique<VPLoadInst>(Instruction::OpID::MaskedLoad, result_type, ptr,
                                               alignment, metadata, insert_block_, name));
}

VPStoreInst *IRBuilder::create_masked_store(Value *value, Value *ptr, Value *active_mask,
                                            Value *evl, TailPolicy tail_policy,
                                            MaskPolicy mask_policy, std::size_t alignment) {
    VPMetadata metadata{active_mask, evl, nullptr, tail_policy, mask_policy};
    return append(std::make_unique<VPStoreInst>(Instruction::OpID::MaskedStore,
                                                module_->types().void_ty(), value, ptr, alignment,
                                                metadata, insert_block_));
}

VPGatherInst *IRBuilder::create_vp_gather(VectorType *result_type, Value *base_ptr, Value *indices,
                                          Value *active_mask, Value *evl, Value *passthrough,
                                          TailPolicy tail_policy, MaskPolicy mask_policy,
                                          std::size_t alignment, const std::string &name) {
    VPMetadata metadata{active_mask, evl, passthrough, tail_policy, mask_policy};
    return append(std::make_unique<VPGatherInst>(result_type, base_ptr, indices, alignment,
                                                 metadata, insert_block_, name));
}

VPScatterInst *IRBuilder::create_vp_scatter(Value *value, Value *base_ptr, Value *indices,
                                            Value *active_mask, Value *evl, TailPolicy tail_policy,
                                            MaskPolicy mask_policy, std::size_t alignment) {
    VPMetadata metadata{active_mask, evl, nullptr, tail_policy, mask_policy};
    return append(std::make_unique<VPScatterInst>(module_->types().void_ty(), value, base_ptr,
                                                  indices, alignment, metadata, insert_block_));
}

VPReductionInst *IRBuilder::create_vp_reduction(ReductionKind kind, bool ordered, Value *vector,
                                                Value *active_mask, Value *evl, Value *passthrough,
                                                TailPolicy tail_policy, MaskPolicy mask_policy,
                                                const std::string &name) {
    const auto *vector_type =
        vector == nullptr ? nullptr : dynamic_cast<const VectorType *>(vector->type());
    if (vector_type == nullptr) {
        throw std::invalid_argument("OIR VP reduction builder requires a vector operand");
    }
    VPMetadata metadata{active_mask, evl, passthrough, tail_policy, mask_policy};
    return append(std::make_unique<VPReductionInst>(vector_type->element_type(), kind, ordered,
                                                    vector, metadata, insert_block_, name));
}

AllocaInst *IRBuilder::create_alloca(Type *allocated_type, const std::string &name) {
    auto *ptr_type = module_->types().ptr_ty(allocated_type);
    return append(std::make_unique<AllocaInst>(ptr_type, allocated_type, insert_block_, name));
}

LoadInst *IRBuilder::create_load(Value *ptr, Type *loaded_type, const std::string &name) {
    return append(std::make_unique<LoadInst>(loaded_type, ptr, insert_block_, name));
}

StoreInst *IRBuilder::create_store(Value *value, Value *ptr) {
    return append(
        std::make_unique<StoreInst>(module_->types().void_ty(), value, ptr, insert_block_));
}

MemZeroInst *IRBuilder::create_memzero(Value *ptr, Value *byte_count) {
    return create_memset(ptr, module_->create_i32(0), byte_count);
}

MemZeroInst *IRBuilder::create_memset(Value *ptr, Value *byte_value, Value *byte_count) {
    return append(std::make_unique<MemZeroInst>(module_->types().void_ty(), ptr, byte_value,
                                                byte_count, insert_block_));
}

GetElementPtrInst *IRBuilder::create_gep(Value *base_ptr, Type *result_ptr_type,
                                         const std::vector<Value *> &indices,
                                         const std::string &name) {
    return append(std::make_unique<GetElementPtrInst>(result_ptr_type, base_ptr, indices,
                                                      insert_block_, name));
}

CallInst *IRBuilder::create_call(Value *callee, Type *return_type, const std::vector<Value *> &args,
                                 const std::string &name) {
    return append(std::make_unique<CallInst>(return_type, callee, args, insert_block_, name));
}

ReturnInst *IRBuilder::create_ret(Value *value) {
    return append(std::make_unique<ReturnInst>(module_->types().void_ty(), value, insert_block_));
}

BranchInst *IRBuilder::create_br(BasicBlock *target) {
    insert_block_->add_successor(target);
    target->add_predecessor(insert_block_);
    return append(std::make_unique<BranchInst>(module_->types().void_ty(), target, insert_block_));
}

BranchInst *IRBuilder::create_cond_br(Value *cond, BasicBlock *true_bb, BasicBlock *false_bb) {
    insert_block_->add_successor(true_bb);
    insert_block_->add_successor(false_bb);
    true_bb->add_predecessor(insert_block_);
    false_bb->add_predecessor(insert_block_);
    return append(std::make_unique<BranchInst>(module_->types().void_ty(), cond, true_bb, false_bb,
                                               insert_block_));
}

PhiInst *IRBuilder::create_phi(Type *type, const std::string &name) {
    return append(std::make_unique<PhiInst>(type, insert_block_, name));
}

VerifyResult Verifier::verify_module(const Module &module) {
    auto fail = [](std::string message) { return VerifyResult{false, std::move(message)}; };

    auto use_list_error = [](const Value *value) -> std::string {
        if (value == nullptr) {
            return "";
        }
        for (const auto &use : value->uses()) {
            if (use.user == nullptr) {
                return "value " + value_ref(value) + " has a null use-list user";
            }
            if (use.operand_index >= use.user->operand_count()) {
                return "value " + value_ref(value) + " has stale use-list index in user " +
                       value_ref(use.user);
            }
            if (use.user->operand(use.operand_index) != value) {
                return "value " + value_ref(value) + " has stale use-list entry in user " +
                       value_ref(use.user);
            }
        }
        return "";
    };

    for (const auto &function_ptr : module.functions()) {
        const auto *function = function_ptr.get();
        if (function->parent() != &module) {
            return fail("function " + function_ref(function) + " has wrong module parent");
        }

        const auto *function_type = dynamic_cast<const FunctionType *>(function->type());
        if (function_type == nullptr) {
            return fail("OIRV_FUNCTION_TYPE: function " + function_ref(function) +
                        " does not have a function type");
        }
        if ((!function_type->return_type()->is_void() &&
             !is_storable_type(function_type->return_type())) ||
            function_type->return_type()->is_label() ||
            function_type->return_type()->is_function()) {
            return fail("OIRV_FUNCTION_RETURN_TYPE: function " + function_ref(function) +
                        " has an invalid return type");
        }
        if (contains_scalable_vector_storage(function_type->return_type())) {
            return fail("OIRV_SCALABLE_ABI: function " + function_ref(function) +
                        " cannot return a scalable vector type");
        }
        if (function->args().size() != function_type->param_types().size()) {
            return fail("OIRV_FUNCTION_ARGUMENT_COUNT: function " + function_ref(function) +
                        " argument list does not match its function type");
        }

        for (std::size_t i = 0; i < function->args().size(); ++i) {
            const auto *arg = function->args()[i].get();
            if (arg->parent() != function || arg->index() != i) {
                return fail("argument %" + arg->name() + " in " + function_ref(function) +
                            " has inconsistent parent or index");
            }
            auto *param_type = function_type->param_types()[i];
            if (arg->type() != param_type) {
                return fail("OIRV_FUNCTION_ARGUMENT_TYPE: argument %" + arg->name() + " in " +
                            function_ref(function) + " does not match parameter type");
            }
            if (!is_storable_type(param_type)) {
                return fail("OIRV_FUNCTION_ARGUMENT_TYPE: function " + function_ref(function) +
                            " has a non-value parameter type");
            }
            if (contains_scalable_vector_storage(param_type)) {
                return fail("OIRV_SCALABLE_ABI: function " + function_ref(function) +
                            " cannot pass scalable vectors in the ordinary function ABI");
            }
            if (auto error = use_list_error(arg); !error.empty()) {
                return fail(error);
            }
        }
        if (auto error = use_list_error(function); !error.empty()) {
            return fail(error);
        }
        if (function->is_external()) {
            continue;
        }

        if (function->blocks().empty()) {
            return fail("function " + function_ref(function) + " has no basic blocks");
        }

        std::unordered_set<const BasicBlock *> block_set;
        std::unordered_map<const Instruction *, const BasicBlock *> inst_blocks;
        std::unordered_map<const Instruction *, std::size_t> inst_indices;
        for (const auto &block_ptr : function->blocks()) {
            const auto *block = block_ptr.get();
            block_set.insert(block);
            if (block->parent() != function) {
                return fail("block " + block_ref(block) + " has wrong function parent in " +
                            function_ref(function));
            }
            const auto &insts = block->instructions();

            if (insts.empty()) {
                return fail("block " + block_ref(block) + " in " + function_ref(function) +
                            " is empty");
            }

            bool saw_non_phi = false;
            std::size_t inst_index = 0;
            for (auto it = insts.begin(); it != insts.end(); ++it) {
                const auto *inst = it->get();
                const bool is_last = std::next(it) == insts.end();
                inst_blocks[inst] = block;
                inst_indices[inst] = inst_index++;

                if (inst->parent() != block) {
                    return fail("instruction " + inst_ref(inst) + " has wrong parent; expected " +
                                block_ref(block));
                }

                if (inst->is_terminator() && !is_last) {
                    return fail("terminator " + inst_ref(inst) + " in block " + block_ref(block) +
                                " is not the last instruction");
                }
                if (inst->op() == Instruction::OpID::Phi) {
                    if (saw_non_phi) {
                        return fail("phi instruction " + inst_ref(inst) + " in block " +
                                    block_ref(block) + " appears after a non-phi instruction");
                    }
                } else {
                    saw_non_phi = true;
                }
            }

            if (!block->has_terminator()) {
                return fail("block " + block_ref(block) + " in " + function_ref(function) +
                            " has no terminator");
            }
            if (has_duplicate_block(block->predecessors())) {
                return fail("block " + block_ref(block) + " has duplicate predecessors");
            }
            if (has_duplicate_block(block->successors())) {
                return fail("block " + block_ref(block) + " has duplicate successors");
            }

            for (auto *pred : block->predecessors()) {
                if (pred == nullptr || pred->parent() != function) {
                    return fail("block " + block_ref(block) +
                                " has predecessor outside its function");
                }
                if (!contains_block_ptr(pred->successors(), block)) {
                    return fail("CFG mismatch: predecessor " + block_ref(pred) +
                                " does not list successor " + block_ref(block));
                }
            }
            for (auto *succ : block->successors()) {
                if (succ == nullptr || succ->parent() != function) {
                    return fail("block " + block_ref(block) +
                                " has successor outside its function");
                }
                if (!contains_block_ptr(succ->predecessors(), block)) {
                    return fail("CFG mismatch: successor " + block_ref(succ) +
                                " does not list predecessor " + block_ref(block));
                }
            }

            const auto *terminator = block->terminator();
            if (const auto *br = dynamic_cast<const BranchInst *>(terminator)) {
                if (br->operand_count() != 1 && br->operand_count() != 3) {
                    return fail("branch in " + block_ref(block) + " has invalid operand count");
                }
                auto targets = branch_targets(*br);
                for (auto *target : targets) {
                    if (target->parent() != function) {
                        return fail("branch in " + block_ref(block) + " targets block outside " +
                                    function_ref(function));
                    }
                    if (!contains_block_ptr(block->successors(), target)) {
                        return fail("CFG mismatch: branch in " + block_ref(block) + " targets " +
                                    block_ref(target) + " but successor list is missing it");
                    }
                }
                for (auto *succ : block->successors()) {
                    if (!contains_block_ptr(targets, succ)) {
                        return fail("CFG mismatch: block " + block_ref(block) +
                                    " lists successor " + block_ref(succ) +
                                    " not named by its terminator");
                    }
                }
            } else if (dynamic_cast<const ReturnInst *>(terminator) != nullptr) {
                if (!block->successors().empty()) {
                    return fail("return block " + block_ref(block) +
                                " must not have CFG successors");
                }
            } else {
                return fail("block " + block_ref(block) + " has unknown terminator kind");
            }
        }

        DominatorTree dom_tree(*function);
        auto normal_def_error = [&](const Instruction *def, const Instruction *use) -> std::string {
            if (def == use) {
                return "instruction " + inst_ref(use) + " uses itself";
            }
            auto def_block_found = inst_blocks.find(def);
            auto use_block_found = inst_blocks.find(use);
            if (def_block_found == inst_blocks.end() || use_block_found == inst_blocks.end()) {
                return "instruction operand in " + inst_ref(use) +
                       " is not defined in the same function";
            }
            auto *def_block = def_block_found->second;
            auto *use_block = use_block_found->second;
            if (def_block == use_block) {
                if (inst_indices.at(def) >= inst_indices.at(use)) {
                    return "definition " + inst_ref(def) + " does not precede use in " +
                           inst_ref(use);
                }
                return "";
            }
            if (dom_tree.is_reachable(use_block) && !dom_tree.dominates(def_block, use_block)) {
                return "definition " + inst_ref(def) + " in " + block_ref(def_block) +
                       " does not dominate use in " + inst_ref(use);
            }
            return "";
        };

        auto phi_def_error = [&](const Instruction *def, const PhiInst *phi,
                                 const BasicBlock *pred) -> std::string {
            auto def_block_found = inst_blocks.find(def);
            if (def_block_found == inst_blocks.end()) {
                return "phi " + inst_ref(phi) + " uses instruction from another function";
            }
            auto *def_block = def_block_found->second;
            if (def_block == pred) {
                return "";
            }
            if (dom_tree.is_reachable(pred) && !dom_tree.dominates(def_block, pred)) {
                return "phi " + inst_ref(phi) + " incoming definition " + inst_ref(def) +
                       " does not dominate predecessor " + block_ref(pred);
            }
            return "";
        };

        auto check_operand_parent = [&](const Value *operand, const Instruction *inst,
                                        std::size_t index) -> VerifyResult {
            if (operand == nullptr) {
                if (dynamic_cast<const VPInstruction *>(inst) != nullptr) {
                    return fail("OIRV_VP_OPERAND_NULL: instruction " + inst_ref(inst) +
                                " has null operand " + std::to_string(index));
                }
                if (inst->op() == Instruction::OpID::Splat ||
                    inst->op() == Instruction::OpID::ExtractElement ||
                    inst->op() == Instruction::OpID::InsertElement ||
                    inst->op() == Instruction::OpID::ShuffleVector ||
                    inst->op() == Instruction::OpID::VectorSelect ||
                    inst->op() == Instruction::OpID::VectorCast) {
                    return fail("OIRV_VECTOR_OPERAND_NULL: instruction " + inst_ref(inst) +
                                " has null operand " + std::to_string(index));
                }
                return fail("instruction " + inst_ref(inst) + " has null operand " +
                            std::to_string(index));
            }
            if (auto *arg = dynamic_cast<const Argument *>(operand)) {
                if (arg->parent() != function) {
                    return fail("instruction " + inst_ref(inst) +
                                " uses argument from another function");
                }
            } else if (auto *def = dynamic_cast<const Instruction *>(operand)) {
                if (def->parent() == nullptr || def->parent()->parent() != function) {
                    return fail("instruction " + inst_ref(inst) +
                                " uses instruction from another function");
                }
                if (inst->op() != Instruction::OpID::Phi) {
                    if (auto error = normal_def_error(def, inst); !error.empty()) {
                        return fail(error);
                    }
                }
            } else if (auto *bb = dynamic_cast<const BasicBlock *>(operand)) {
                if (bb->parent() != function) {
                    return fail("instruction " + inst_ref(inst) +
                                " uses block from another function");
                }
            } else if (auto *callee = dynamic_cast<const Function *>(operand)) {
                if (callee->parent() != &module) {
                    return fail("instruction " + inst_ref(inst) +
                                " uses function from another module");
                }
            }
            return {true, "ok"};
        };

        std::unordered_map<const Value *, std::vector<UseKey>> actual_uses;
        for (const auto &block_ptr : function->blocks()) {
            for (const auto &inst_ptr : block_ptr->instructions()) {
                const auto *inst = inst_ptr.get();
                for (std::size_t i = 0; i < inst->operand_count(); ++i) {
                    auto *operand = inst->operand(i);
                    if (operand != nullptr) {
                        actual_uses[operand].push_back({inst, i});
                    }
                }
            }
        }
        for (const auto &[value, uses] : actual_uses) {
            if (auto error = use_list_error(value); !error.empty()) {
                return fail(error);
            }

            std::unordered_set<UseKey, UseKeyHash> recorded;
            for (const auto &use : value->uses()) {
                recorded.insert({use.user, use.operand_index});
            }
            for (const auto &use : uses) {
                if (recorded.find(use) == recorded.end()) {
                    return fail("missing use-list entry for operand " +
                                std::to_string(use.operand_index) + " of " + value_ref(use.user) +
                                " using " + value_ref(value));
                }
            }
        }

        for (const auto &block_ptr : function->blocks()) {
            const auto *block = block_ptr.get();
            if (auto error = use_list_error(block); !error.empty()) {
                return fail(error);
            }
            for (const auto &inst_ptr : block->instructions()) {
                const auto *inst = inst_ptr.get();
                if (auto error = use_list_error(inst); !error.empty()) {
                    return fail(error);
                }
                for (std::size_t i = 0; i < inst->operand_count(); ++i) {
                    auto result = check_operand_parent(inst->operand(i), inst, i);
                    if (!result.ok) {
                        return result;
                    }
                    if (inst->operand(i)->type() == nullptr) {
                        return fail("OIRV_NULL_TYPE: operand " + std::to_string(i) + " of " +
                                    inst_ref(inst) + " has no type");
                    }
                }
                if (inst->type() == nullptr) {
                    return fail("OIRV_NULL_TYPE: instruction " + inst_ref(inst) +
                                " has no result type");
                }

                switch (inst->op()) {
                case Instruction::OpID::Br: {
                    const auto *br = dynamic_cast<const BranchInst *>(inst);
                    if (br == nullptr) {
                        return fail("branch instruction type mismatch in " + block_ref(block));
                    }
                    if (!br->type()->is_void()) {
                        return fail("OIRV_BRANCH_RESULT_TYPE: branch in " + block_ref(block) +
                                    " must have void result type");
                    }
                    if (!br->is_conditional() && br->target_bb() == nullptr) {
                        return fail("unconditional branch in " + block_ref(block) +
                                    " is missing target");
                    }
                    if (br->is_conditional()) {
                        if (br->cond() == nullptr || br->true_bb() == nullptr ||
                            br->false_bb() == nullptr) {
                            return fail("conditional branch in " + block_ref(block) +
                                        " is incomplete");
                        }
                        if (!is_scalar_i1(br->cond()->type())) {
                            return fail("OIRV_BRANCH_CONDITION: conditional branch in " +
                                        block_ref(block) + " expects scalar i1 condition");
                        }
                    }
                    break;
                }
                case Instruction::OpID::Ret: {
                    const auto *ret = dynamic_cast<const ReturnInst *>(inst);
                    if (ret == nullptr) {
                        return fail("return instruction type mismatch in " + block_ref(block));
                    }
                    if (!ret->type()->is_void()) {
                        return fail("OIRV_RETURN_RESULT_TYPE: return in " + block_ref(block) +
                                    " must have void instruction type");
                    }
                    if (ret->operand_count() > 1) {
                        return fail("return in " + block_ref(block) + " has too many operands");
                    }
                    if (function->return_type()->is_void()) {
                        if (ret->has_value()) {
                            return fail("OIRV_RETURN_TYPE: void function " +
                                        function_ref(function) + " cannot return a value");
                        }
                    } else {
                        if (!ret->has_value()) {
                            return fail("OIRV_RETURN_TYPE: non-void function " +
                                        function_ref(function) + " must return a value");
                        }
                        if (ret->value()->type() != function->return_type()) {
                            return fail("OIRV_RETURN_TYPE: return type mismatch in function " +
                                        function_ref(function));
                        }
                    }
                    break;
                }
                case Instruction::OpID::Load: {
                    const auto *load = dynamic_cast<const LoadInst *>(inst);
                    if (load == nullptr) {
                        return fail("load instruction type mismatch in " + block_ref(block));
                    }
                    if (load->operand_count() != 1) {
                        return fail("OIRV_LOAD_OPERANDS: load " + inst_ref(load) +
                                    " must have exactly one operand");
                    }
                    auto *ptr_ty = dynamic_cast<PointerType *>(load->ptr()->type());
                    if (ptr_ty == nullptr) {
                        return fail("load " + inst_ref(load) + " expects pointer operand");
                    }
                    if (ptr_ty->element_type() != load->type()) {
                        return fail("load " + inst_ref(load) +
                                    " result type does not match pointer element type");
                    }
                    break;
                }
                case Instruction::OpID::Store: {
                    const auto *store = dynamic_cast<const StoreInst *>(inst);
                    if (store == nullptr) {
                        return fail("store instruction type mismatch in " + block_ref(block));
                    }
                    if (store->operand_count() != 2 || !store->type()->is_void()) {
                        return fail("OIRV_STORE_SHAPE: store in " + block_ref(block) +
                                    " must have two operands and void result type");
                    }
                    auto *ptr_ty = dynamic_cast<PointerType *>(store->ptr()->type());
                    if (ptr_ty == nullptr) {
                        return fail("store in " + block_ref(block) + " expects pointer operand");
                    }
                    if (ptr_ty->element_type() != store->value()->type()) {
                        return fail("store in " + block_ref(block) + " value type mismatch");
                    }
                    break;
                }
                case Instruction::OpID::MemZero: {
                    const auto *memzero = dynamic_cast<const MemZeroInst *>(inst);
                    if (memzero == nullptr) {
                        return fail("memzero instruction type mismatch in " + block_ref(block));
                    }
                    if (memzero->operand_count() != 3 || !memzero->type()->is_void()) {
                        return fail("OIRV_MEMZERO_SHAPE: memzero in " + block_ref(block) +
                                    " must have three operands and void result type");
                    }
                    if (!memzero->ptr()->type()->is_pointer()) {
                        return fail("memzero in " + block_ref(block) + " expects pointer operand");
                    }
                    auto *value_ty = dynamic_cast<IntegerType *>(memzero->byte_value()->type());
                    if (value_ty == nullptr || value_ty->bit_width() != 32) {
                        return fail("memzero in " + block_ref(block) + " expects i32 byte value");
                    }
                    auto *byte_value = dynamic_cast<ConstantInt *>(memzero->byte_value());
                    if (byte_value == nullptr || byte_value->value() < 0 ||
                        byte_value->value() > 255) {
                        return fail("memzero in " + block_ref(block) +
                                    " expects constant byte value in [0, 255]");
                    }
                    auto *count_ty = dynamic_cast<IntegerType *>(memzero->byte_count()->type());
                    if (count_ty == nullptr || count_ty->bit_width() != 32) {
                        return fail("memzero in " + block_ref(block) + " expects i32 byte count");
                    }
                    break;
                }
                case Instruction::OpID::Alloca: {
                    const auto *alloca = dynamic_cast<const AllocaInst *>(inst);
                    if (alloca == nullptr) {
                        return fail("alloca instruction type mismatch in " + block_ref(block));
                    }
                    if (alloca->operand_count() != 0) {
                        return fail("OIRV_ALLOCA_OPERANDS: alloca " + inst_ref(alloca) +
                                    " must not have operands");
                    }
                    const auto *pointer = dynamic_cast<const PointerType *>(alloca->type());
                    if (pointer == nullptr || alloca->allocated_type() == nullptr ||
                        pointer->element_type() != alloca->allocated_type()) {
                        return fail("OIRV_ALLOCA_TYPE: alloca " + inst_ref(alloca) +
                                    " result pointer must match its allocated type");
                    }
                    if (!is_storable_type(alloca->allocated_type())) {
                        return fail("OIRV_ALLOCA_TYPE: alloca " + inst_ref(alloca) +
                                    " requires a sized storage type");
                    }
                    if (contains_scalable_vector_storage(alloca->allocated_type())) {
                        return fail("OIRV_SCALABLE_ALLOCA: alloca " + inst_ref(alloca) +
                                    " cannot allocate scalable vector storage");
                    }
                    break;
                }
                case Instruction::OpID::Call: {
                    const auto *call = dynamic_cast<const CallInst *>(inst);
                    if (call == nullptr) {
                        return fail("call instruction type mismatch in " + block_ref(block));
                    }
                    if (call->operand_count() == 0) {
                        return fail("OIRV_CALL_CALLEE_TYPE: call " + inst_ref(call) +
                                    " is missing a callee");
                    }
                    const auto *callee_type = called_function_type(call->callee());
                    if (callee_type == nullptr) {
                        return fail("OIRV_CALL_CALLEE_TYPE: call " + inst_ref(call) +
                                    " callee must have function or function-pointer type");
                    }
                    if (call->type() != callee_type->return_type()) {
                        return fail("OIRV_CALL_RETURN_TYPE: call " + inst_ref(call) +
                                    " result type does not match callee return type");
                    }
                    if (contains_scalable_vector_storage(call->type())) {
                        return fail("OIRV_SCALABLE_ABI: call " + inst_ref(call) +
                                    " cannot return a scalable vector through the ordinary ABI");
                    }

                    auto args = call->args();
                    for (std::size_t i = 0; i < args.size(); ++i) {
                        if (!is_storable_type(args[i]->type())) {
                            return fail("OIRV_CALL_ARGUMENT_TYPE: argument " + std::to_string(i) +
                                        " of call " + inst_ref(call) +
                                        " is not a first-class value type");
                        }
                        if (contains_scalable_vector_storage(args[i]->type())) {
                            return fail("OIRV_SCALABLE_ABI: argument " + std::to_string(i) +
                                        " of call " + inst_ref(call) +
                                        " cannot use a scalable vector in the ordinary ABI");
                        }
                    }

                    const auto fixed_arity = callee_type->param_types().size();
                    if ((!callee_type->is_variadic() && args.size() != fixed_arity) ||
                        (callee_type->is_variadic() && args.size() < fixed_arity)) {
                        return fail("OIRV_CALL_ARITY: call " + inst_ref(call) + " supplies " +
                                    std::to_string(args.size()) +
                                    " argument(s), but callee expects " +
                                    std::to_string(fixed_arity) +
                                    (callee_type->is_variadic() ? " or more" : ""));
                    }
                    for (std::size_t i = 0; i < std::min(args.size(), fixed_arity); ++i) {
                        if (args[i]->type() != callee_type->param_types()[i]) {
                            return fail("OIRV_CALL_ARGUMENT_TYPE: argument " + std::to_string(i) +
                                        " of call " + inst_ref(call) +
                                        " does not match callee parameter type");
                        }
                    }
                    break;
                }
                case Instruction::OpID::ZExt:
                case Instruction::OpID::SIToFP:
                case Instruction::OpID::FPToSI: {
                    const auto *cast = dynamic_cast<const CastInst *>(inst);
                    if (cast == nullptr) {
                        return fail("cast instruction type mismatch in " + block_ref(block));
                    }
                    if (cast->operand_count() != 1) {
                        return fail("OIRV_CAST_OPERANDS: cast " + inst_ref(cast) +
                                    " must have exactly one operand");
                    }
                    if (inst->op() == Instruction::OpID::ZExt) {
                        const auto *source = dynamic_cast<const IntegerType *>(cast->src()->type());
                        const auto *destination = dynamic_cast<const IntegerType *>(cast->type());
                        if (source == nullptr || destination == nullptr ||
                            destination->bit_width() <= source->bit_width()) {
                            return fail("OIRV_ZEXT_TYPE: zext " + inst_ref(cast) +
                                        " requires scalar integer widening");
                        }
                    } else if (inst->op() == Instruction::OpID::SIToFP) {
                        if (!cast->src()->type()->is_scalar_integer() ||
                            !cast->type()->is_scalar_float()) {
                            return fail("OIRV_SITOFP_TYPE: sitofp " + inst_ref(cast) +
                                        " requires scalar integer to scalar float");
                        }
                    } else if (!cast->src()->type()->is_scalar_float() ||
                               !cast->type()->is_scalar_integer()) {
                        return fail("OIRV_FPTOSI_TYPE: fptosi " + inst_ref(cast) +
                                    " requires scalar float to scalar integer");
                    }
                    break;
                }
                case Instruction::OpID::SetVL: {
                    const auto *setvl = dynamic_cast<const SetVLInst *>(inst);
                    const auto *configuration = setvl == nullptr ? nullptr : setvl->vector_type();
                    if (setvl == nullptr || setvl->operand_count() != 1 ||
                        !is_scalar_i32(setvl->type()) || setvl->avl() == nullptr ||
                        !is_scalar_i32(setvl->avl()->type())) {
                        return fail("OIRV_SETVL_SHAPE: setvl " + inst_ref(inst) +
                                    " requires one scalar i32 AVL and scalar i32 result");
                    }
                    if (configuration == nullptr || !configuration->element_count().is_scalable() ||
                        configuration->is_mask() ||
                        (!configuration->is_integer_vector() &&
                         !configuration->is_float_vector())) {
                        return fail("OIRV_SETVL_CONFIG: setvl " + inst_ref(inst) +
                                    " requires a scalable i32 or float vector configuration");
                    }
                    break;
                }
                case Instruction::OpID::Splat: {
                    const auto *splat = dynamic_cast<const SplatInst *>(inst);
                    const auto *result = dynamic_cast<const VectorType *>(inst->type());
                    if (splat == nullptr || result == nullptr || splat->operand_count() != 1 ||
                        splat->scalar()->type() != result->element_type()) {
                        return fail("OIRV_SPLAT_SHAPE: splat " + inst_ref(inst) +
                                    " requires one scalar matching its vector element type");
                    }
                    break;
                }
                case Instruction::OpID::StepVector: {
                    const auto *step = dynamic_cast<const StepVectorInst *>(inst);
                    const auto *result = dynamic_cast<const VectorType *>(inst->type());
                    if (step == nullptr || result == nullptr || step->operand_count() != 0 ||
                        !result->is_integer_vector()) {
                        return fail("OIRV_STEPVECTOR_TYPE: stepvector " + inst_ref(inst) +
                                    " requires a non-mask integer vector result");
                    }
                    break;
                }
                case Instruction::OpID::ExtractElement: {
                    const auto *extract = dynamic_cast<const ExtractElementInst *>(inst);
                    const auto *vector =
                        extract == nullptr
                            ? nullptr
                            : dynamic_cast<const VectorType *>(extract->vector()->type());
                    if (extract == nullptr || extract->operand_count() != 2 || vector == nullptr ||
                        extract->type() != vector->element_type()) {
                        return fail("OIRV_EXTRACT_SHAPE: extractelement " + inst_ref(inst) +
                                    " result must match its vector element type");
                    }
                    if (!is_scalar_i32(extract->index()->type())) {
                        return fail("OIRV_VECTOR_INDEX_TYPE: extractelement " + inst_ref(inst) +
                                    " index must be scalar i32");
                    }
                    if (vector->element_count().is_fixed()) {
                        if (const auto *index = dynamic_cast<const ConstantInt *>(extract->index());
                            index != nullptr &&
                            (index->value() < 0 || static_cast<std::uint64_t>(index->value()) >=
                                                       vector->element_count().min_lanes)) {
                            return fail("OIRV_VECTOR_INDEX_BOUNDS: extractelement " +
                                        inst_ref(inst) + " constant index is out of bounds");
                        }
                    }
                    break;
                }
                case Instruction::OpID::InsertElement: {
                    const auto *insert = dynamic_cast<const InsertElementInst *>(inst);
                    const auto *vector =
                        insert == nullptr
                            ? nullptr
                            : dynamic_cast<const VectorType *>(insert->vector()->type());
                    if (insert == nullptr || insert->operand_count() != 3 || vector == nullptr ||
                        insert->type() != vector ||
                        insert->element()->type() != vector->element_type()) {
                        return fail("OIRV_INSERT_SHAPE: insertelement " + inst_ref(inst) +
                                    " operands and result must have one vector shape");
                    }
                    if (!is_scalar_i32(insert->index()->type())) {
                        return fail("OIRV_VECTOR_INDEX_TYPE: insertelement " + inst_ref(inst) +
                                    " index must be scalar i32");
                    }
                    if (vector->element_count().is_fixed()) {
                        if (const auto *index = dynamic_cast<const ConstantInt *>(insert->index());
                            index != nullptr &&
                            (index->value() < 0 || static_cast<std::uint64_t>(index->value()) >=
                                                       vector->element_count().min_lanes)) {
                            return fail("OIRV_VECTOR_INDEX_BOUNDS: insertelement " +
                                        inst_ref(inst) + " constant index is out of bounds");
                        }
                    }
                    break;
                }
                case Instruction::OpID::ShuffleVector: {
                    const auto *shuffle = dynamic_cast<const ShuffleVectorInst *>(inst);
                    const auto *lhs_type =
                        shuffle == nullptr
                            ? nullptr
                            : dynamic_cast<const VectorType *>(shuffle->lhs()->type());
                    const auto *result_type = dynamic_cast<const VectorType *>(inst->type());
                    if (shuffle == nullptr || shuffle->operand_count() != 2 ||
                        lhs_type == nullptr || result_type == nullptr ||
                        shuffle->rhs()->type() != lhs_type ||
                        lhs_type->element_count().is_scalable() ||
                        result_type->element_count().is_scalable() ||
                        result_type->element_type() != lhs_type->element_type() ||
                        shuffle->shuffle_mask().size() != result_type->element_count().min_lanes) {
                        return fail("OIRV_SHUFFLE_SHAPE: shufflevector " + inst_ref(inst) +
                                    " requires fixed, element-compatible vectors and one index "
                                    "per result lane");
                    }
                    const auto lanes = lhs_type->element_count().min_lanes;
                    if (lanes >
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 2)) {
                        return fail("OIRV_SHUFFLE_BOUNDS: shufflevector source is too large");
                    }
                    const auto bound = static_cast<std::int64_t>(lanes * 2U);
                    for (auto index : shuffle->shuffle_mask()) {
                        if (index < -1 || index >= bound) {
                            return fail("OIRV_SHUFFLE_BOUNDS: shufflevector " + inst_ref(inst) +
                                        " index is outside [-1, 2 * source lanes)");
                        }
                    }
                    break;
                }
                case Instruction::OpID::VectorSelect: {
                    const auto *select = dynamic_cast<const VectorSelectInst *>(inst);
                    const auto *result = dynamic_cast<const VectorType *>(inst->type());
                    const auto *condition =
                        select == nullptr
                            ? nullptr
                            : dynamic_cast<const VectorType *>(select->condition()->type());
                    if (select == nullptr || select->operand_count() != 3 || result == nullptr ||
                        condition == nullptr || !condition->is_mask() ||
                        condition->element_count() != result->element_count() ||
                        select->true_value()->type() != result ||
                        select->false_value()->type() != result) {
                        return fail("OIRV_VECTOR_SELECT_SHAPE: select " + inst_ref(inst) +
                                    " requires a same-shaped mask and vector values");
                    }
                    break;
                }
                case Instruction::OpID::VectorCast: {
                    const auto *cast = dynamic_cast<const VectorCastInst *>(inst);
                    const auto *source =
                        cast == nullptr ? nullptr
                                        : dynamic_cast<const VectorType *>(cast->source()->type());
                    const auto *result = dynamic_cast<const VectorType *>(inst->type());
                    if (cast == nullptr || cast->operand_count() != 1 || source == nullptr ||
                        result == nullptr || source->element_count() != result->element_count()) {
                        return fail("OIRV_VECTOR_CAST_SHAPE: vector cast " + inst_ref(inst) +
                                    " requires equal source and result ElementCount");
                    }
                    bool valid_cast = false;
                    switch (cast->kind()) {
                    case VectorCastKind::ZExt: {
                        const auto *src_int =
                            dynamic_cast<const IntegerType *>(source->element_type());
                        const auto *dst_int =
                            dynamic_cast<const IntegerType *>(result->element_type());
                        valid_cast = src_int != nullptr && dst_int != nullptr &&
                                     dst_int->bit_width() > src_int->bit_width();
                        break;
                    }
                    case VectorCastKind::SIToFP:
                        valid_cast = source->element_type()->is_scalar_integer() &&
                                     !source->is_mask() &&
                                     result->element_type()->is_scalar_float();
                        break;
                    case VectorCastKind::FPToSI:
                        valid_cast = source->element_type()->is_scalar_float() &&
                                     result->element_type()->is_scalar_integer() &&
                                     !result->is_mask();
                        break;
                    case VectorCastKind::Bitcast:
                        valid_cast =
                            ((source->element_type()->is_scalar_integer() && !source->is_mask() &&
                              result->element_type()->is_scalar_float()) ||
                             (source->element_type()->is_scalar_float() &&
                              result->element_type()->is_scalar_integer() && !result->is_mask()));
                        break;
                    default:
                        valid_cast = false;
                    }
                    if (!valid_cast) {
                        return fail("OIRV_VECTOR_CAST_TYPE: vector cast " + inst_ref(inst) +
                                    " has an invalid element-family conversion");
                    }
                    break;
                }
                case Instruction::OpID::FixedABIExtractLane: {
                    const auto *extract = dynamic_cast<const FixedABIExtractLaneInst *>(inst);
                    const auto *aggregate_type =
                        extract == nullptr
                            ? nullptr
                            : dynamic_cast<const VectorType *>(extract->aggregate()->type());
                    if (extract == nullptr || extract->operand_count() != 1 ||
                        aggregate_type == nullptr ||
                        aggregate_type->element_count().is_scalable() ||
                        extract->type() != aggregate_type->element_type()) {
                        return fail("OIRV_FIXED_ABI_EXTRACT_SHAPE: " + inst_ref(inst) +
                                    " requires one fixed-vector aggregate and its scalar "
                                    "element result");
                    }
                    if (extract->lane_index() >= aggregate_type->element_count().min_lanes) {
                        return fail("OIRV_FIXED_ABI_LANE_BOUNDS: " + inst_ref(inst) +
                                    " lane index is outside the fixed aggregate");
                    }
                    if (dynamic_cast<const Argument *>(extract->aggregate()) == nullptr &&
                        dynamic_cast<const CallInst *>(extract->aggregate()) == nullptr &&
                        dynamic_cast<const LoadInst *>(extract->aggregate()) == nullptr) {
                        return fail("OIRV_FIXED_ABI_EXTRACT_SOURCE: " + inst_ref(inst) +
                                    " may only unpack an incoming argument, call result, or "
                                    "aggregate load");
                    }
                    break;
                }
                case Instruction::OpID::FixedABIPack: {
                    const auto *pack = dynamic_cast<const FixedABIPackInst *>(inst);
                    const auto *aggregate_type = dynamic_cast<const VectorType *>(inst->type());
                    if (pack == nullptr || aggregate_type == nullptr ||
                        aggregate_type->element_count().is_scalable() ||
                        pack->operand_count() != aggregate_type->element_count().min_lanes) {
                        return fail("OIRV_FIXED_ABI_PACK_SHAPE: " + inst_ref(inst) +
                                    " requires exactly one lane per fixed-vector result");
                    }
                    for (auto *lane : pack->lane_values()) {
                        if (lane == nullptr || lane->type() != aggregate_type->element_type()) {
                            return fail("OIRV_FIXED_ABI_PACK_LANE_TYPE: " + inst_ref(inst) +
                                        " lane type does not match the aggregate element");
                        }
                    }
                    if (!pack->has_uses()) {
                        return fail("OIRV_FIXED_ABI_PACK_CONSUMER: " + inst_ref(inst) +
                                    " must feed a return, call argument, or aggregate store");
                    }
                    for (const auto &use : pack->uses()) {
                        const bool return_value =
                            dynamic_cast<const ReturnInst *>(use.user) != nullptr &&
                            use.operand_index == 0;
                        const bool call_argument =
                            dynamic_cast<const CallInst *>(use.user) != nullptr &&
                            use.operand_index > 0;
                        const bool store_value =
                            dynamic_cast<const StoreInst *>(use.user) != nullptr &&
                            use.operand_index == 0;
                        if (!return_value && !call_argument && !store_value) {
                            return fail("OIRV_FIXED_ABI_PACK_CONSUMER: " + inst_ref(inst) +
                                        " has a non-boundary consumer");
                        }
                    }
                    break;
                }
                case Instruction::OpID::FixedABIObjectLoadLane: {
                    const auto *load = dynamic_cast<const FixedABIObjectLoadLaneInst *>(inst);
                    const auto *pointer =
                        load == nullptr
                            ? nullptr
                            : dynamic_cast<const PointerType *>(load->object_ptr()->type());
                    const auto *object_type =
                        pointer == nullptr
                            ? nullptr
                            : dynamic_cast<const VectorType *>(pointer->element_type());
                    if (load == nullptr || load->operand_count() != 1 || object_type == nullptr ||
                        object_type->element_count().is_scalable() ||
                        load->type() != object_type->element_type()) {
                        return fail("OIRV_FIXED_ABI_OBJECT_POINTER: " + inst_ref(inst) +
                                    " requires ptr<fixed-vector> and its scalar element "
                                    "result");
                    }
                    if (load->lane_index() >= object_type->element_count().min_lanes) {
                        return fail("OIRV_FIXED_ABI_LANE_BOUNDS: " + inst_ref(inst) +
                                    " lane index is outside the fixed object");
                    }
                    break;
                }
                case Instruction::OpID::FixedABIObjectStoreLane: {
                    const auto *store = dynamic_cast<const FixedABIObjectStoreLaneInst *>(inst);
                    const auto *pointer =
                        store == nullptr
                            ? nullptr
                            : dynamic_cast<const PointerType *>(store->object_ptr()->type());
                    const auto *object_type =
                        pointer == nullptr
                            ? nullptr
                            : dynamic_cast<const VectorType *>(pointer->element_type());
                    if (store == nullptr || store->operand_count() != 2 ||
                        !inst->type()->is_void() || object_type == nullptr ||
                        object_type->element_count().is_scalable() ||
                        store->lane_value()->type() != object_type->element_type()) {
                        return fail("OIRV_FIXED_ABI_OBJECT_STORE_SHAPE: " + inst_ref(inst) +
                                    " requires a matching scalar value and "
                                    "ptr<fixed-vector>");
                    }
                    if (store->lane_index() >= object_type->element_count().min_lanes) {
                        return fail("OIRV_FIXED_ABI_LANE_BOUNDS: " + inst_ref(inst) +
                                    " lane index is outside the fixed object");
                    }
                    break;
                }
                case Instruction::OpID::VPBinary: {
                    const auto *binary = dynamic_cast<const VPBinaryInst *>(inst);
                    const auto *result = dynamic_cast<const VectorType *>(inst->type());
                    if (binary == nullptr || binary->operand_count() != 5 || result == nullptr ||
                        binary->lhs()->type() != result || binary->rhs()->type() != result) {
                        return fail("OIRV_VP_BINARY_SHAPE: VP binary " + inst_ref(inst) +
                                    " operands and result must have one vector shape");
                    }
                    const auto binary_op = binary->binary_op();
                    const bool floating_op = binary_op == Instruction::OpID::FAdd ||
                                             binary_op == Instruction::OpID::FSub ||
                                             binary_op == Instruction::OpID::FMul ||
                                             binary_op == Instruction::OpID::FDiv;
                    const bool integer_op = binary_op == Instruction::OpID::Add ||
                                            binary_op == Instruction::OpID::Sub ||
                                            binary_op == Instruction::OpID::Mul ||
                                            binary_op == Instruction::OpID::SDiv ||
                                            binary_op == Instruction::OpID::SRem ||
                                            binary_op == Instruction::OpID::And ||
                                            binary_op == Instruction::OpID::Or ||
                                            binary_op == Instruction::OpID::Xor;
                    const bool bitwise = binary_op == Instruction::OpID::And ||
                                         binary_op == Instruction::OpID::Or ||
                                         binary_op == Instruction::OpID::Xor;
                    if ((!floating_op && !integer_op) ||
                        (floating_op && !result->is_float_vector()) ||
                        (integer_op && (!result->element_type()->is_scalar_integer() ||
                                        (result->is_mask() && !bitwise)))) {
                        return fail("OIRV_VP_BINARY_FAMILY: VP binary " + inst_ref(inst) +
                                    " opcode does not match its vector element family");
                    }
                    if (auto error =
                            vp_metadata_error(*binary, *result, result, true, false, false);
                        !error.empty()) {
                        return fail(error);
                    }
                    break;
                }
                case Instruction::OpID::VPCmp: {
                    const auto *compare = dynamic_cast<const VPCmpInst *>(inst);
                    const auto *lhs_type =
                        compare == nullptr
                            ? nullptr
                            : dynamic_cast<const VectorType *>(compare->lhs()->type());
                    const auto *result = dynamic_cast<const VectorType *>(inst->type());
                    if (compare == nullptr || compare->operand_count() != 5 ||
                        lhs_type == nullptr || result == nullptr ||
                        compare->rhs()->type() != lhs_type || !result->is_mask() ||
                        result->element_count() != lhs_type->element_count()) {
                        return fail("OIRV_VP_CMP_SHAPE: VP compare " + inst_ref(inst) +
                                    " requires same-shaped operands and i1 mask result");
                    }
                    if ((compare->comparison_op() == Instruction::OpID::ICmp &&
                         !lhs_type->element_type()->is_scalar_integer()) ||
                        (compare->comparison_op() == Instruction::OpID::FCmp &&
                         !lhs_type->is_float_vector()) ||
                        (compare->comparison_op() != Instruction::OpID::ICmp &&
                         compare->comparison_op() != Instruction::OpID::FCmp)) {
                        return fail("OIRV_VP_CMP_FAMILY: VP compare " + inst_ref(inst) +
                                    " opcode does not match its operand family");
                    }
                    if (auto error =
                            vp_metadata_error(*compare, *lhs_type, result, true, false, false);
                        !error.empty()) {
                        return fail(error);
                    }
                    break;
                }
                case Instruction::OpID::VPLoad:
                case Instruction::OpID::MaskedLoad: {
                    const auto *load = dynamic_cast<const VPLoadInst *>(inst);
                    const auto *result = dynamic_cast<const VectorType *>(inst->type());
                    const auto *pointer =
                        load == nullptr ? nullptr
                                        : dynamic_cast<const PointerType *>(load->ptr()->type());
                    const bool masked = inst->op() == Instruction::OpID::MaskedLoad;
                    if (load == nullptr || load->operand_count() != 4 || result == nullptr ||
                        pointer == nullptr || pointer->element_type() != result->element_type()) {
                        return fail("OIRV_VP_MEMORY_POINTER: " + op_to_string(inst->op()) + " " +
                                    inst_ref(inst) +
                                    " pointer must address the vector element type");
                    }
                    if (!is_valid_alignment(load->alignment())) {
                        return fail("OIRV_VP_MEMORY_ALIGNMENT: " + inst_ref(inst) +
                                    " alignment must be a nonzero power of two");
                    }
                    if (auto error = vp_metadata_error(*load, *result, result, true, false, masked);
                        !error.empty()) {
                        return fail(error);
                    }
                    break;
                }
                case Instruction::OpID::VPStore:
                case Instruction::OpID::MaskedStore: {
                    const auto *store = dynamic_cast<const VPStoreInst *>(inst);
                    const auto *value_type =
                        store == nullptr ? nullptr
                                         : dynamic_cast<const VectorType *>(store->value()->type());
                    const auto *pointer =
                        store == nullptr ? nullptr
                                         : dynamic_cast<const PointerType *>(store->ptr()->type());
                    const bool masked = inst->op() == Instruction::OpID::MaskedStore;
                    if (store == nullptr || store->operand_count() != 4 ||
                        !store->type()->is_void() || value_type == nullptr || pointer == nullptr ||
                        pointer->element_type() != value_type->element_type()) {
                        return fail("OIRV_VP_MEMORY_POINTER: " + op_to_string(inst->op()) + " " +
                                    inst_ref(inst) +
                                    " requires void result and pointer to vector element type");
                    }
                    if (!is_valid_alignment(store->alignment())) {
                        return fail("OIRV_VP_MEMORY_ALIGNMENT: " + inst_ref(inst) +
                                    " alignment must be a nonzero power of two");
                    }
                    if (auto error =
                            vp_metadata_error(*store, *value_type, nullptr, false, true, masked);
                        !error.empty()) {
                        return fail(error);
                    }
                    break;
                }
                case Instruction::OpID::VPGather: {
                    const auto *gather = dynamic_cast<const VPGatherInst *>(inst);
                    const auto *result = dynamic_cast<const VectorType *>(inst->type());
                    const auto *pointer =
                        gather == nullptr
                            ? nullptr
                            : dynamic_cast<const PointerType *>(gather->base_ptr()->type());
                    const auto *indices =
                        gather == nullptr
                            ? nullptr
                            : dynamic_cast<const VectorType *>(gather->indices()->type());
                    if (gather == nullptr || gather->operand_count() != 5 || result == nullptr ||
                        pointer == nullptr || pointer->element_type() != result->element_type() ||
                        indices == nullptr || !indices->is_integer_vector() ||
                        indices->element_count() != result->element_count()) {
                        return fail("OIRV_VP_GATHER_SHAPE: VP gather " + inst_ref(inst) +
                                    " requires element pointer and same-shaped i32 indices");
                    }
                    if (!is_valid_alignment(gather->alignment())) {
                        return fail("OIRV_VP_MEMORY_ALIGNMENT: " + inst_ref(inst) +
                                    " alignment must be a nonzero power of two");
                    }
                    if (auto error =
                            vp_metadata_error(*gather, *result, result, true, false, false);
                        !error.empty()) {
                        return fail(error);
                    }
                    break;
                }
                case Instruction::OpID::VPScatter: {
                    const auto *scatter = dynamic_cast<const VPScatterInst *>(inst);
                    const auto *value_type =
                        scatter == nullptr
                            ? nullptr
                            : dynamic_cast<const VectorType *>(scatter->value()->type());
                    const auto *pointer =
                        scatter == nullptr
                            ? nullptr
                            : dynamic_cast<const PointerType *>(scatter->base_ptr()->type());
                    const auto *indices =
                        scatter == nullptr
                            ? nullptr
                            : dynamic_cast<const VectorType *>(scatter->indices()->type());
                    if (scatter == nullptr || scatter->operand_count() != 5 ||
                        !scatter->type()->is_void() || value_type == nullptr ||
                        pointer == nullptr ||
                        pointer->element_type() != value_type->element_type() ||
                        indices == nullptr || !indices->is_integer_vector() ||
                        indices->element_count() != value_type->element_count()) {
                        return fail("OIRV_VP_SCATTER_SHAPE: VP scatter " + inst_ref(inst) +
                                    " requires element pointer and same-shaped i32 indices");
                    }
                    if (!is_valid_alignment(scatter->alignment())) {
                        return fail("OIRV_VP_MEMORY_ALIGNMENT: " + inst_ref(inst) +
                                    " alignment must be a nonzero power of two");
                    }
                    if (auto error =
                            vp_metadata_error(*scatter, *value_type, nullptr, false, true, false);
                        !error.empty()) {
                        return fail(error);
                    }
                    break;
                }
                case Instruction::OpID::VPReduction: {
                    const auto *reduction = dynamic_cast<const VPReductionInst *>(inst);
                    const auto *vector =
                        reduction == nullptr
                            ? nullptr
                            : dynamic_cast<const VectorType *>(reduction->vector()->type());
                    if (reduction == nullptr || reduction->operand_count() != 4 ||
                        vector == nullptr || reduction->type() != vector->element_type()) {
                        return fail("OIRV_VP_REDUCTION_SHAPE: VP reduction " + inst_ref(inst) +
                                    " result must match its vector element type");
                    }
                    bool valid_kind = true;
                    switch (reduction->kind()) {
                    case ReductionKind::Add:
                    case ReductionKind::Mul:
                    case ReductionKind::Min:
                    case ReductionKind::Max:
                        break;
                    case ReductionKind::And:
                    case ReductionKind::Or:
                    case ReductionKind::Xor:
                        valid_kind = vector->element_type()->is_scalar_integer();
                        break;
                    default:
                        valid_kind = false;
                    }
                    if (!valid_kind ||
                        (!vector->element_type()->is_scalar_integer() &&
                         !vector->element_type()->is_scalar_float()) ||
                        (vector->element_type()->is_scalar_float() && !reduction->ordered()) ||
                        (vector->element_type()->is_scalar_integer() && reduction->ordered())) {
                        return fail("OIRV_VP_REDUCTION_TYPE: VP reduction " + inst_ref(inst) +
                                    " has invalid element family, kind, or ordering");
                    }
                    if (auto error = vp_metadata_error(*reduction, *vector, vector->element_type(),
                                                       true, true, false);
                        !error.empty()) {
                        return fail(error);
                    }
                    break;
                }
                case Instruction::OpID::Add:
                case Instruction::OpID::Sub:
                case Instruction::OpID::Mul:
                case Instruction::OpID::And:
                case Instruction::OpID::Or:
                case Instruction::OpID::Xor:
                case Instruction::OpID::SDiv:
                case Instruction::OpID::SRem:
                case Instruction::OpID::FAdd:
                case Instruction::OpID::FSub:
                case Instruction::OpID::FMul:
                case Instruction::OpID::FDiv: {
                    const auto *bin = dynamic_cast<const BinaryInst *>(inst);
                    if (bin == nullptr) {
                        return fail("binary instruction type mismatch in " + block_ref(block));
                    }
                    if (bin->operand_count() != 2) {
                        return fail("OIRV_BINARY_OPERANDS: binary instruction " + inst_ref(bin) +
                                    " must have exactly two operands");
                    }
                    if (bin->lhs()->type() != bin->rhs()->type() ||
                        bin->lhs()->type() != bin->type()) {
                        return fail("OIRV_BINARY_SHAPE: binary instruction " + inst_ref(bin) +
                                    " operands and result must have one canonical type and shape");
                    }
                    const bool floating_op = inst->op() == Instruction::OpID::FAdd ||
                                             inst->op() == Instruction::OpID::FSub ||
                                             inst->op() == Instruction::OpID::FMul ||
                                             inst->op() == Instruction::OpID::FDiv;
                    const bool bitwise_op = inst->op() == Instruction::OpID::And ||
                                            inst->op() == Instruction::OpID::Or ||
                                            inst->op() == Instruction::OpID::Xor;
                    if (floating_op) {
                        if (!is_float_type_or_vector(bin->type())) {
                            return fail("OIRV_BINARY_TYPE_FAMILY: " + op_to_string(inst->op()) +
                                        " instruction " + inst_ref(bin) +
                                        " requires scalar or same-shaped vector float operands");
                        }
                    } else if (!is_integer_type_or_vector(bin->type(), bitwise_op)) {
                        return fail("OIRV_BINARY_TYPE_FAMILY: " + op_to_string(inst->op()) +
                                    " instruction " + inst_ref(bin) +
                                    " requires scalar integer or same-shaped vector integer " +
                                    (bitwise_op ? "operands" : "non-mask operands"));
                    }
                    break;
                }
                case Instruction::OpID::ICmp:
                case Instruction::OpID::FCmp: {
                    const auto *cmp = dynamic_cast<const CmpInst *>(inst);
                    if (cmp == nullptr) {
                        return fail("compare instruction type mismatch in " + block_ref(block));
                    }
                    if (cmp->operand_count() != 2) {
                        return fail("OIRV_CMP_OPERANDS: compare " + inst_ref(cmp) +
                                    " must have exactly two operands");
                    }
                    if (cmp->lhs()->type() != cmp->rhs()->type()) {
                        return fail("OIRV_CMP_SHAPE: compare " + inst_ref(cmp) +
                                    " operands must have one canonical type and shape");
                    }
                    if (inst->op() == Instruction::OpID::ICmp) {
                        if (!is_integer_type_or_vector(cmp->lhs()->type(), true)) {
                            return fail("OIRV_CMP_TYPE_FAMILY: icmp " + inst_ref(cmp) +
                                        " requires scalar or vector integer operands");
                        }
                    } else if (!is_float_type_or_vector(cmp->lhs()->type())) {
                        return fail("OIRV_CMP_TYPE_FAMILY: fcmp " + inst_ref(cmp) +
                                    " requires scalar or vector float operands");
                    }

                    const auto *operand_vector =
                        dynamic_cast<const VectorType *>(cmp->lhs()->type());
                    if (operand_vector == nullptr) {
                        if (!is_scalar_i1(cmp->type())) {
                            return fail("OIRV_CMP_RESULT_SHAPE: scalar compare " + inst_ref(cmp) +
                                        " result must be scalar i1");
                        }
                    } else {
                        const auto *result_vector = dynamic_cast<const VectorType *>(cmp->type());
                        if (result_vector == nullptr || !result_vector->is_mask() ||
                            result_vector->element_count() != operand_vector->element_count()) {
                            return fail("OIRV_CMP_RESULT_SHAPE: vector compare " + inst_ref(cmp) +
                                        " result must be a same-shaped i1 mask");
                        }
                    }
                    break;
                }
                case Instruction::OpID::GetElementPtr: {
                    const auto *gep = dynamic_cast<const GetElementPtrInst *>(inst);
                    if (gep == nullptr) {
                        return fail("gep instruction type mismatch in " + block_ref(block));
                    }
                    if (gep->operand_count() == 0) {
                        return fail("OIRV_GEP_BASE_TYPE: gep " + inst_ref(gep) +
                                    " is missing its base pointer");
                    }
                    const auto *base_pointer =
                        dynamic_cast<const PointerType *>(gep->base_ptr()->type());
                    const auto *result_pointer = dynamic_cast<const PointerType *>(gep->type());
                    if (base_pointer == nullptr || result_pointer == nullptr) {
                        return fail("OIRV_GEP_BASE_TYPE: gep " + inst_ref(gep) +
                                    " expects pointer base and pointer result");
                    }
                    for (auto *index : gep->indices()) {
                        if (!index->type()->is_scalar_integer()) {
                            return fail("OIRV_GEP_INDEX_TYPE: gep " + inst_ref(gep) +
                                        " index must be a scalar integer");
                        }
                    }
                    const auto *expected_element =
                        gep_result_element_type(*base_pointer, gep->indices().size());
                    if (expected_element == nullptr) {
                        return fail("OIRV_GEP_PATH: gep " + inst_ref(gep) +
                                    " has an index beyond its aggregate path");
                    }
                    // OIR's existing array decay form may flatten one or more
                    // remaining array layers without spelling their zero indices.
                    // This is address-equivalent to repeatedly selecting element 0;
                    // no unrelated pointee type is accepted.
                    bool result_matches = result_pointer->element_type() == expected_element;
                    const Type *candidate = expected_element;
                    while (!result_matches && candidate != nullptr && gep->indices().size() >= 2) {
                        const auto *array = dynamic_cast<const ArrayType *>(candidate);
                        candidate = array == nullptr ? nullptr : array->element_type();
                        result_matches = result_pointer->element_type() == candidate;
                    }
                    if (!result_matches) {
                        return fail("OIRV_GEP_RESULT_TYPE: gep " + inst_ref(gep) +
                                    " result pointee does not match indexed element type");
                    }
                    break;
                }
                case Instruction::OpID::Phi: {
                    const auto *phi = dynamic_cast<const PhiInst *>(inst);
                    if (phi == nullptr) {
                        return fail("phi instruction type mismatch in " + block_ref(block));
                    }
                    if (!is_storable_type(phi->type())) {
                        return fail("OIRV_PHI_TYPE: phi " + inst_ref(phi) +
                                    " must have a first-class value type");
                    }
                    if (phi->operand_count() != phi->incoming().size() * 2) {
                        return fail("phi " + inst_ref(phi) +
                                    " operand count does not match incoming list");
                    }
                    if (phi->incoming().size() != block->predecessors().size()) {
                        return fail("phi " + inst_ref(phi) +
                                    " incoming count does not match predecessor count in " +
                                    block_ref(block));
                    }
                    std::unordered_set<const BasicBlock *> incoming_preds;
                    for (std::size_t i = 0; i < phi->incoming().size(); ++i) {
                        const auto &item = phi->incoming()[i];
                        if (item.first == nullptr || item.second == nullptr) {
                            return fail("phi " + inst_ref(phi) + " has null incoming");
                        }
                        if (phi->operand(i * 2) != item.first ||
                            phi->operand(i * 2 + 1) != item.second) {
                            return fail("phi " + inst_ref(phi) +
                                        " operands and incoming list are out of sync");
                        }
                        if (!incoming_preds.insert(item.second).second) {
                            return fail("phi " + inst_ref(phi) +
                                        " has duplicate incoming predecessor " +
                                        block_ref(item.second));
                        }
                        if (item.first->type() != phi->type()) {
                            return fail("OIRV_PHI_TYPE: phi " + inst_ref(phi) +
                                        " incoming value type mismatch");
                        }
                        if (!contains_block_ptr(block->predecessors(), item.second)) {
                            return fail("phi " + inst_ref(phi) + " incoming predecessor " +
                                        block_ref(item.second) + " is not a CFG predecessor of " +
                                        block_ref(block));
                        }
                        if (auto *arg = dynamic_cast<const Argument *>(item.first)) {
                            if (arg->parent() != function) {
                                return fail("phi " + inst_ref(phi) +
                                            " uses argument from another function");
                            }
                        } else if (auto *def = dynamic_cast<const Instruction *>(item.first)) {
                            if (auto error = phi_def_error(def, phi, item.second); !error.empty()) {
                                return fail(error);
                            }
                        }
                    }
                    break;
                }
                default:
                    return fail("OIRV_UNKNOWN_OPCODE: verifier has no rule for instruction " +
                                inst_ref(inst));
                }
            }
        }
    }

    std::unordered_set<const Constant *> owned_constants;
    for (const auto &owned_value : module.owned_constants()) {
        if (const auto *constant = dynamic_cast<const Constant *>(owned_value.get())) {
            owned_constants.insert(constant);
            continue;
        }
        if (dynamic_cast<const UndefValue *>(owned_value.get()) == nullptr) {
            return fail("OIRV_CONSTANT_KIND: module owns an unsupported constant-pool value");
        }
    }

    std::unordered_set<const Constant *> visiting_constants;
    std::unordered_set<const Constant *> validated_constants;
    for (const auto &owned_value : module.owned_constants()) {
        if (const auto *constant = dynamic_cast<const Constant *>(owned_value.get())) {
            auto error = constant_validation_error(constant, owned_constants, "module constant",
                                                   visiting_constants, validated_constants);
            if (!error.empty()) {
                return fail(error);
            }
        } else if (owned_value->type() == nullptr || !is_storable_type(owned_value->type())) {
            return fail("OIRV_UNDEF_TYPE: undef requires a storable type");
        }
        if (auto error = use_list_error(owned_value.get()); !error.empty()) {
            return fail(error);
        }
    }

    for (const auto &global : module.globals()) {
        const auto *pointer = dynamic_cast<const PointerType *>(global->type());
        if (pointer == nullptr || global->value_type() == nullptr ||
            pointer->element_type() != global->value_type()) {
            return fail("OIRV_GLOBAL_TYPE: global @" + global->name() +
                        " address type must point to its value type");
        }
        if (!is_storable_type(global->value_type())) {
            return fail("OIRV_GLOBAL_TYPE: global @" + global->name() +
                        " requires a sized storage type");
        }
        if (contains_scalable_vector_storage(global->value_type())) {
            return fail("OIRV_SCALABLE_GLOBAL: global @" + global->name() +
                        " cannot contain scalable vector storage");
        }
        if (global->initializer() != nullptr &&
            global->initializer()->type() != global->value_type()) {
            return fail("OIRV_GLOBAL_INITIALIZER_TYPE: global @" + global->name() +
                        " initializer type does not match its value type");
        }
        if (global->initializer() != nullptr) {
            auto error = constant_validation_error(global->initializer(), owned_constants,
                                                   "global @" + global->name(), visiting_constants,
                                                   validated_constants);
            if (!error.empty()) {
                return fail(error);
            }
        }
        if (auto error = use_list_error(global.get()); !error.empty()) {
            return fail(error);
        }
    }

    return {true, "ok"};
}

} // namespace oir
