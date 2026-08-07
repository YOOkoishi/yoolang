#include "pass/oir/OIRPortableVectorScalarizerPass.h"

#include "oir/OIRCFGUtils.h"
#include "oir/OIRParser.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pass::oir_portable {
namespace {

using Code = ScalarizationReasonCode;

struct ScalarizationFailure final : std::exception {
    ScalarizationFailure(Code code, std::string message) : code(code), message(std::move(message)) {
    }

    const char *what() const noexcept override {
        return message.c_str();
    }

    Code code;
    std::string message;
};

[[noreturn]] void reject(Code code, std::string message) {
    throw ScalarizationFailure(code, std::move(message));
}

bool type_contains_scalable_impl(const oir::Type *type,
                                 std::unordered_set<const oir::Type *> &seen) {
    if (type == nullptr || !seen.insert(type).second)
        return false;
    if (const auto *vector = dynamic_cast<const oir::VectorType *>(type))
        return vector->element_count().is_scalable();
    if (const auto *pointer = dynamic_cast<const oir::PointerType *>(type))
        return type_contains_scalable_impl(pointer->element_type(), seen);
    if (const auto *array = dynamic_cast<const oir::ArrayType *>(type))
        return type_contains_scalable_impl(array->element_type(), seen);
    if (const auto *function = dynamic_cast<const oir::FunctionType *>(type)) {
        if (type_contains_scalable_impl(function->return_type(), seen))
            return true;
        for (auto *parameter : function->param_types()) {
            if (type_contains_scalable_impl(parameter, seen))
                return true;
        }
    }
    return false;
}

bool type_contains_scalable(const oir::Type *type) {
    std::unordered_set<const oir::Type *> seen;
    return type_contains_scalable_impl(type, seen);
}

oir::VectorType *as_vector_type(oir::Value *value) {
    return value == nullptr ? nullptr : dynamic_cast<oir::VectorType *>(value->type());
}

bool instruction_involves_direct_vector(const oir::Instruction &instruction) {
    if (instruction.type() != nullptr && instruction.type()->is_vector())
        return true;
    for (auto *operand : instruction.operands()) {
        if (operand != nullptr && operand->type() != nullptr && operand->type()->is_vector())
            return true;
    }
    return false;
}

bool is_fixed_abi_boundary_op(oir::Instruction::OpID op) {
    using Op = oir::Instruction::OpID;
    return op == Op::FixedABIExtractLane || op == Op::FixedABIPack ||
           op == Op::FixedABIObjectLoadLane || op == Op::FixedABIObjectStoreLane;
}

bool uses_only_fixed_abi_extracts(const oir::Value &value) {
    return std::all_of(value.uses().begin(), value.uses().end(), [](const oir::Value::Use &use) {
        return use.operand_index == 0 &&
               dynamic_cast<const oir::FixedABIExtractLaneInst *>(use.user) != nullptr;
    });
}

const oir::FunctionType *called_function_type(const oir::Value *callee) {
    if (callee == nullptr || callee->type() == nullptr)
        return nullptr;
    if (const auto *function = dynamic_cast<const oir::FunctionType *>(callee->type()))
        return function;
    const auto *pointer = dynamic_cast<const oir::PointerType *>(callee->type());
    return pointer == nullptr
               ? nullptr
               : dynamic_cast<const oir::FunctionType *>(pointer->element_type());
}

bool is_normalized_boundary_consumer(const oir::Instruction &instruction) {
    using Op = oir::Instruction::OpID;
    switch (instruction.op()) {
    case Op::Ret: {
        const auto *ret = dynamic_cast<const oir::ReturnInst *>(&instruction);
        return ret != nullptr &&
               (!ret->has_value() || !ret->value()->type()->is_vector() ||
                dynamic_cast<const oir::FixedABIPackInst *>(ret->value()) != nullptr);
    }
    case Op::Load:
        return !instruction.type()->is_vector() || uses_only_fixed_abi_extracts(instruction);
    case Op::Store: {
        const auto *store = dynamic_cast<const oir::StoreInst *>(&instruction);
        return store != nullptr &&
               (!store->value()->type()->is_vector() ||
                dynamic_cast<const oir::FixedABIPackInst *>(store->value()) != nullptr);
    }
    case Op::Call: {
        const auto *call = dynamic_cast<const oir::CallInst *>(&instruction);
        if (call == nullptr)
            return false;
        if (call->type()->is_vector() && !uses_only_fixed_abi_extracts(*call))
            return false;
        for (auto *argument : call->args()) {
            if (argument->type()->is_vector() &&
                dynamic_cast<const oir::FixedABIPackInst *>(argument) == nullptr)
                return false;
        }
        return true;
    }
    default:
        return false;
    }
}

bool function_requires_scalarization(const oir::Function &function) {
    for (const auto &block : function.blocks()) {
        for (const auto &instruction : block->instructions()) {
            if (instruction_involves_direct_vector(*instruction) &&
                !is_fixed_abi_boundary_op(instruction->op()) &&
                !is_normalized_boundary_consumer(*instruction))
                return true;
        }
    }
    return false;
}

bool is_supported_vector_instruction(const oir::Instruction &instruction) {
    using Op = oir::Instruction::OpID;
    switch (instruction.op()) {
    case Op::Add:
    case Op::Sub:
    case Op::Mul:
    case Op::SDiv:
    case Op::SRem:
    case Op::And:
    case Op::Or:
    case Op::Xor:
    case Op::FAdd:
    case Op::FSub:
    case Op::FMul:
    case Op::FDiv:
    case Op::ICmp:
    case Op::FCmp:
    case Op::Phi:
    case Op::Splat:
    case Op::StepVector:
    case Op::ExtractElement:
    case Op::InsertElement:
    case Op::ShuffleVector:
    case Op::VectorSelect:
    case Op::VectorCast:
    case Op::FixedABIExtractLane:
    case Op::FixedABIPack:
    case Op::FixedABIObjectLoadLane:
    case Op::FixedABIObjectStoreLane:
    case Op::VPBinary:
    case Op::VPCmp:
    case Op::VPLoad:
    case Op::VPStore:
    case Op::MaskedLoad:
    case Op::MaskedStore:
    case Op::VPGather:
    case Op::VPScatter:
    case Op::VPReduction:
    case Op::Ret:
    case Op::Load:
    case Op::Store:
    case Op::Call:
        return true;
    case Op::SetVL:
    case Op::Br:
    case Op::Alloca:
    case Op::MemZero:
    case Op::GetElementPtr:
    case Op::ZExt:
    case Op::SIToFP:
    case Op::FPToSI:
        return false;
    }
    return false;
}

bool preflight(const oir::Module &module) {
    bool has_fixed_vector = false;

    for (const auto &global : module.globals()) {
        if (type_contains_scalable(global->value_type())) {
            reject(Code::ScalableVectorUnsupported,
                   "PORTABLE_SCALABLE_VECTOR_UNSUPPORTED: global @" + global->name() +
                       " contains a scalable vector type");
        }
    }

    for (const auto &function : module.functions()) {
        if (type_contains_scalable(function->function_type())) {
            reject(Code::ScalableVectorUnsupported,
                   "PORTABLE_SCALABLE_VECTOR_UNSUPPORTED: function @" + function->name() +
                       " has a scalable-vector signature");
        }
        if (function->is_external())
            continue;

        for (const auto &block : function->blocks()) {
            for (const auto &owned : block->instructions()) {
                const auto &instruction = *owned;
                if (instruction.op() == oir::Instruction::OpID::SetVL) {
                    reject(Code::ScalableVectorUnsupported,
                           "PORTABLE_SCALABLE_VECTOR_UNSUPPORTED: setvl is not legal in the "
                           "fixed-vector portable path");
                }
                if (type_contains_scalable(instruction.type())) {
                    reject(Code::ScalableVectorUnsupported,
                           "PORTABLE_SCALABLE_VECTOR_UNSUPPORTED: instruction %" +
                               instruction.name() + " produces a scalable vector");
                }
                for (auto *operand : instruction.operands()) {
                    if (operand != nullptr && type_contains_scalable(operand->type())) {
                        reject(Code::ScalableVectorUnsupported,
                               "PORTABLE_SCALABLE_VECTOR_UNSUPPORTED: instruction %" +
                                   instruction.name() + " consumes a scalable vector");
                    }
                }

                if (!instruction_involves_direct_vector(instruction))
                    continue;

                if (const auto *call = dynamic_cast<const oir::CallInst *>(&instruction)) {
                    const auto *callee_type = called_function_type(call->callee());
                    if (callee_type != nullptr && callee_type->is_variadic()) {
                        const auto fixed_arity = callee_type->param_types().size();
                        const auto arguments = call->args();
                        for (std::size_t index = fixed_arity; index < arguments.size(); ++index) {
                            if (arguments[index]->type()->is_vector()) {
                                reject(Code::AggregateABIUnavailable,
                                       "PORTABLE_VECTOR_AGGREGATE_ABI_UNAVAILABLE: fixed "
                                       "vector/mask variadic arguments are not supported");
                            }
                        }
                    }
                }

                if (is_fixed_abi_boundary_op(instruction.op()) ||
                    is_normalized_boundary_consumer(instruction))
                    continue;

                has_fixed_vector = true;
                if (!is_supported_vector_instruction(instruction)) {
                    reject(Code::UnsupportedOperation,
                           "PORTABLE_VECTOR_OPERATION_UNSUPPORTED: opcode " +
                               std::to_string(static_cast<unsigned>(instruction.op())) +
                               " has vector operands but no audited scalarization");
                }
            }
        }
    }
    return has_fixed_vector;
}

class Transformer final {
  public:
    Transformer(oir::Module &module, PortableVectorScalarizerResult &result)
        : module_(module), result_(result) {
    }

    void run() {
        for (auto &function : module_.functions()) {
            if (!function->is_external() && function_requires_scalarization(*function))
                transform_function(*function);
        }
    }

  private:
    using Lanes = std::vector<oir::Value *>;

    struct InsertionPoint final {
        oir::BasicBlock *block = nullptr;
        oir::Instruction *before = nullptr;
    };

    oir::Instruction *insert_owned(InsertionPoint point,
                                   std::unique_ptr<oir::Instruction> instruction) {
        if (point.block == nullptr || instruction == nullptr)
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: null insertion point or instruction");
        auto *raw = instruction.get();
        raw->set_parent(point.block);
        if (point.before == nullptr) {
            point.block->instructions().push_back(std::move(instruction));
        } else {
            auto &instructions = point.block->instructions();
            auto position = std::find_if(instructions.begin(), instructions.end(),
                                         [&](const std::unique_ptr<oir::Instruction> &candidate) {
                                             return candidate.get() == point.before;
                                         });
            if (position == instructions.end())
                reject(Code::MutationFailed,
                       "PORTABLE_MUTATION_FAILED: anchor instruction is not in its parent block");
            instructions.insert(position, std::move(instruction));
        }
        return raw;
    }

    oir::Instruction *insert(InsertionPoint point,
                             std::unique_ptr<oir::Instruction> instruction) {
        auto *raw = insert_owned(point, std::move(instruction));
        ++result_.scalar_instructions_created;
        return raw;
    }

    template <typename T, typename... Args> T *insert_as(InsertionPoint point, Args &&...args) {
        return static_cast<T *>(insert(point, std::make_unique<T>(std::forward<Args>(args)...)));
    }

    template <typename T, typename... Args>
    T *insert_boundary_as(InsertionPoint point, Args &&...args) {
        auto *raw = static_cast<T *>(
            insert_owned(point, std::make_unique<T>(std::forward<Args>(args)...)));
        ++result_.boundary_instructions_created;
        return raw;
    }

    InsertionPoint before(oir::Instruction &instruction) const {
        return {instruction.parent(), &instruction};
    }

    InsertionPoint after(oir::Instruction &instruction) const {
        auto *block = instruction.parent();
        if (block == nullptr)
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: instruction has no parent block");
        auto &instructions = block->instructions();
        auto position = std::find_if(
            instructions.begin(), instructions.end(),
            [&](const std::unique_ptr<oir::Instruction> &candidate) {
                return candidate.get() == &instruction;
            });
        if (position == instructions.end())
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: instruction is not in its parent block");
        ++position;
        return {block, position == instructions.end() ? nullptr : position->get()};
    }

    std::string fresh_name(std::string base) {
        if (base.empty())
            base = "portable";
        if (used_names_.insert(base).second)
            return base;
        for (unsigned suffix = 0;; ++suffix) {
            auto candidate = base + "." + std::to_string(suffix);
            if (used_names_.insert(candidate).second)
                return candidate;
        }
    }

    std::string lane_name(const oir::Instruction &instruction, std::uint64_t lane,
                          std::string_view suffix = {}) {
        std::string base = instruction.name().empty() ? "portable" : instruction.name();
        base += ".lane" + std::to_string(lane);
        if (!suffix.empty()) {
            base += ".";
            base += suffix;
        }
        return fresh_name(std::move(base));
    }

    oir::BasicBlock *create_block(oir::Function &function, std::string_view base) {
        for (;;) {
            auto *block = function.create_block(std::string(base));
            if (used_names_.insert(block->name()).second)
                return block;
            function.erase_block(block);
        }
    }

    oir::Value *scalar_zero(oir::Type *type) {
        if (const auto *integer = dynamic_cast<const oir::IntegerType *>(type)) {
            if (integer->bit_width() == 1)
                return module_.create_i1(false);
            if (integer->bit_width() == 32)
                return module_.create_i32(0);
        }
        if (type->is_scalar_float())
            return module_.create_f32(0.0F);
        return module_.create_zero(type);
    }

    const Lanes &lanes(oir::Value *value) {
        auto found = lane_values_.find(value);
        if (found != lane_values_.end())
            return found->second;
        const auto *type = as_vector_type(value);
        if (type == nullptr || type->element_count().is_scalable())
            reject(Code::UnsupportedOperation,
                   "PORTABLE_VECTOR_OPERATION_UNSUPPORTED: expected a fixed vector value");

        Lanes result;
        result.reserve(static_cast<std::size_t>(type->element_count().min_lanes));
        if (auto *constant = dynamic_cast<oir::ConstantVector *>(value)) {
            result.assign(constant->elements().begin(), constant->elements().end());
        } else if (auto *mask = dynamic_cast<oir::ConstantMask *>(value)) {
            for (std::uint64_t lane = 0; lane < mask->lane_count(); ++lane)
                result.push_back(module_.create_i1(mask->lane(lane)));
        } else if (dynamic_cast<oir::ConstantAggregateZero *>(value) != nullptr) {
            for (std::uint64_t lane = 0; lane < type->element_count().min_lanes; ++lane)
                result.push_back(scalar_zero(type->element_type()));
        } else if (dynamic_cast<oir::UndefValue *>(value) != nullptr) {
            for (std::uint64_t lane = 0; lane < type->element_count().min_lanes; ++lane)
                result.push_back(module_.create_undef(type->element_type()));
        } else {
            reject(Code::UnsupportedOperation,
                   "PORTABLE_VECTOR_OPERATION_UNSUPPORTED: vector definition was not "
                   "scalarized before use");
        }
        auto inserted = lane_values_.emplace(value, std::move(result));
        return inserted.first->second;
    }

    void set_lanes(oir::Value &value, Lanes values) {
        const auto *type = as_vector_type(&value);
        if (type == nullptr || values.size() != type->element_count().min_lanes)
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: scalar lane map does not match vector shape");
        if (!lane_values_.emplace(&value, std::move(values)).second)
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: vector value was scalarized twice");
    }

    oir::BinaryInst *emit_binary(InsertionPoint point, oir::Instruction::OpID op, oir::Value *lhs,
                                 oir::Value *rhs, std::string name) {
        return insert_as<oir::BinaryInst>(point, lhs->type(), op, lhs, rhs, point.block,
                                          std::move(name));
    }

    oir::CmpInst *emit_compare(InsertionPoint point, oir::Instruction::OpID op, oir::CmpPred pred,
                               oir::Value *lhs, oir::Value *rhs, std::string name) {
        return insert_as<oir::CmpInst>(point, module_.types().int1_ty(), op, pred, lhs, rhs,
                                       point.block, std::move(name));
    }

    oir::CastInst *emit_cast(InsertionPoint point, oir::Instruction::OpID op, oir::Type *target,
                             oir::Value *source, std::string name) {
        return insert_as<oir::CastInst>(point, target, op, source, point.block, std::move(name));
    }

    oir::FixedABIExtractLaneInst *emit_boundary_extract(
        InsertionPoint point, oir::Value *aggregate, std::uint64_t lane,
        std::string name) {
        const auto *type = as_vector_type(aggregate);
        if (type == nullptr || type->element_count().is_scalable() ||
            lane >= type->element_count().min_lanes) {
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: malformed fixed ABI extract request");
        }
        return insert_boundary_as<oir::FixedABIExtractLaneInst>(
            point, type->element_type(), aggregate, lane, point.block, std::move(name));
    }

    oir::FixedABIPackInst *emit_boundary_pack(InsertionPoint point,
                                              oir::VectorType *type,
                                              const Lanes &values,
                                              std::string name) {
        if (type == nullptr || type->element_count().is_scalable() ||
            values.size() != type->element_count().min_lanes) {
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: malformed fixed ABI pack request");
        }
        return insert_boundary_as<oir::FixedABIPackInst>(
            point, type, values, point.block, std::move(name));
    }

    oir::FixedABIObjectLoadLaneInst *emit_boundary_object_load(
        InsertionPoint point, oir::VectorType *type, oir::Value *object_ptr,
        std::uint64_t lane, std::string name) {
        return insert_boundary_as<oir::FixedABIObjectLoadLaneInst>(
            point, type->element_type(), object_ptr, lane, point.block,
            std::move(name));
    }

    oir::FixedABIObjectStoreLaneInst *emit_boundary_object_store(
        InsertionPoint point, oir::Value *value, oir::Value *object_ptr,
        std::uint64_t lane) {
        return insert_boundary_as<oir::FixedABIObjectStoreLaneInst>(
            point, module_.types().void_ty(), value, object_ptr, lane, point.block);
    }

    oir::GetElementPtrInst *emit_element_address(InsertionPoint point, oir::Value *base,
                                                 oir::Value *index, std::string name) {
        auto *pointer = dynamic_cast<oir::PointerType *>(base->type());
        if (pointer == nullptr || pointer->element_type()->is_vector())
            reject(Code::AggregateABIUnavailable,
                   "PORTABLE_VECTOR_AGGREGATE_ABI_UNAVAILABLE: scalar memory lane requires an "
                   "element pointer");
        return insert_as<oir::GetElementPtrInst>(
            point, pointer, base, std::vector<oir::Value *>{index}, point.block, std::move(name));
    }

    void move_tail_to_continuation(oir::Instruction &anchor, oir::BasicBlock &continuation) {
        auto *block = anchor.parent();
        auto original_successors = block->successors();
        for (auto *successor : original_successors)
            oir::cfg::move_successor_edge(block, &continuation, successor);

        auto &instructions = block->instructions();
        auto position = std::find_if(instructions.begin(), instructions.end(),
                                     [&](const std::unique_ptr<oir::Instruction> &candidate) {
                                         return candidate.get() == &anchor;
                                     });
        if (position == instructions.end())
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: conditional anchor is not in its block");
        continuation.instructions().splice(continuation.instructions().end(), instructions,
                                           position, instructions.end());
        for (auto &instruction : continuation.instructions())
            instruction->set_parent(&continuation);
    }

    using ValueEmitter = std::function<oir::Value *(InsertionPoint)>;
    using VoidEmitter = std::function<void(InsertionPoint)>;

    oir::Value *conditional_value(oir::Instruction &anchor, oir::Value *condition, oir::Type *type,
                                  const ValueEmitter &emit_true, oir::Value *false_value,
                                  std::string name) {
        if (condition == nullptr || condition->type() != module_.types().int1_ty() ||
            false_value == nullptr || false_value->type() != type)
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: malformed scalar conditional value");
        if (const auto *constant = dynamic_cast<const oir::ConstantInt *>(condition)) {
            if (constant->value() == 0)
                return false_value;
            auto *value = emit_true(before(anchor));
            if (value == nullptr || value->type() != type)
                reject(Code::MutationFailed,
                       "PORTABLE_MUTATION_FAILED: conditional true value has wrong type");
            return value;
        }

        auto *block = anchor.parent();
        auto *function = block->parent();
        auto *taken = create_block(*function, "portable.if.true");
        auto *not_taken = create_block(*function, "portable.if.false");
        auto *continuation = create_block(*function, "portable.if.cont");
        result_.cfg_blocks_created += 3;
        move_tail_to_continuation(anchor, *continuation);

        if (oir::cfg::append_conditional_branch(module_, block, condition, taken, not_taken) ==
            nullptr)
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: could not append conditional branch");
        ++result_.scalar_instructions_created;
        auto *true_value = emit_true({taken, nullptr});
        if (true_value == nullptr || true_value->type() != type)
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: conditional true value has wrong type");
        if (oir::cfg::append_unconditional_branch(module_, taken, continuation) == nullptr ||
            oir::cfg::append_unconditional_branch(module_, not_taken, continuation) == nullptr)
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: could not close conditional diamond");
        result_.scalar_instructions_created += 2;

        auto *phi = insert_as<oir::PhiInst>({continuation, &anchor}, type, continuation,
                                            fresh_name(std::move(name)));
        phi->add_incoming(true_value, taken);
        phi->add_incoming(false_value, not_taken);
        return phi;
    }

    void conditional_effect(oir::Instruction &anchor, oir::Value *condition,
                            const VoidEmitter &emit_true) {
        if (condition == nullptr || condition->type() != module_.types().int1_ty())
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: malformed guarded scalar effect");
        if (const auto *constant = dynamic_cast<const oir::ConstantInt *>(condition)) {
            if (constant->value() != 0)
                emit_true(before(anchor));
            return;
        }

        auto *block = anchor.parent();
        auto *function = block->parent();
        auto *taken = create_block(*function, "portable.mem.true");
        auto *not_taken = create_block(*function, "portable.mem.false");
        auto *continuation = create_block(*function, "portable.mem.cont");
        result_.cfg_blocks_created += 3;
        move_tail_to_continuation(anchor, *continuation);
        if (oir::cfg::append_conditional_branch(module_, block, condition, taken, not_taken) ==
            nullptr)
            reject(Code::MutationFailed, "PORTABLE_MUTATION_FAILED: could not append memory guard");
        ++result_.scalar_instructions_created;
        emit_true({taken, nullptr});
        if (oir::cfg::append_unconditional_branch(module_, taken, continuation) == nullptr ||
            oir::cfg::append_unconditional_branch(module_, not_taken, continuation) == nullptr)
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: could not close guarded memory diamond");
        result_.scalar_instructions_created += 2;
    }

    std::optional<std::uint64_t> constant_lane_index(oir::Value *index, std::uint64_t lane_count) {
        const auto *constant = dynamic_cast<const oir::ConstantInt *>(index);
        if (constant == nullptr)
            return std::nullopt;
        if (constant->value() < 0 || static_cast<std::uint64_t>(constant->value()) >= lane_count)
            reject(Code::UnsupportedOperation,
                   "PORTABLE_VECTOR_OPERATION_UNSUPPORTED: constant lane index is out of "
                   "range");
        return static_cast<std::uint64_t>(constant->value());
    }

    oir::Value *dynamic_extract(oir::ExtractElementInst &instruction, const Lanes &source) {
        auto *result = source.front();
        for (std::uint64_t lane = 1; lane < source.size(); ++lane) {
            auto *equal = emit_compare(before(instruction), oir::Instruction::OpID::ICmp,
                                       oir::CmpPred::EQ, instruction.index(),
                                       module_.create_i32(static_cast<std::int64_t>(lane)),
                                       lane_name(instruction, lane, "index"));
            result = conditional_value(
                instruction, equal, result->type(),
                [&](InsertionPoint) -> oir::Value * { return source[lane]; }, result,
                lane_name(instruction, lane, "pick"));
        }
        return result;
    }

    Lanes dynamic_insert(oir::InsertElementInst &instruction, const Lanes &source) {
        Lanes result;
        result.reserve(source.size());
        for (std::uint64_t lane = 0; lane < source.size(); ++lane) {
            auto *equal = emit_compare(before(instruction), oir::Instruction::OpID::ICmp,
                                       oir::CmpPred::EQ, instruction.index(),
                                       module_.create_i32(static_cast<std::int64_t>(lane)),
                                       lane_name(instruction, lane, "index"));
            result.push_back(conditional_value(
                instruction, equal, source[lane]->type(),
                [&](InsertionPoint) -> oir::Value * { return instruction.element(); }, source[lane],
                lane_name(instruction, lane, "insert")));
        }
        return result;
    }

    void require_full_fixed_evl(const oir::VPInstruction &instruction, std::uint64_t lanes) const {
        const auto *evl = dynamic_cast<const oir::ConstantInt *>(instruction.evl());
        if (evl == nullptr || evl->value() < 0 ||
            static_cast<std::uint64_t>(evl->value()) != lanes) {
            reject(Code::FixedEVLRequired,
                   "PORTABLE_FIXED_EVL_REQUIRED: fixed VP operation requires constant EVL equal "
                   "to its lane count");
        }
    }

    oir::Value *emit_reduction_combine(oir::VPReductionInst &instruction, oir::Value *accumulator,
                                       oir::Value *lane, std::uint64_t lane_index) {
        using Kind = oir::ReductionKind;
        using Op = oir::Instruction::OpID;
        const bool floating = lane->type()->is_scalar_float();
        switch (instruction.kind()) {
        case Kind::Add:
            return emit_binary(before(instruction), floating ? Op::FAdd : Op::Add, accumulator,
                               lane, lane_name(instruction, lane_index, "reduce"));
        case Kind::Mul:
            return emit_binary(before(instruction), floating ? Op::FMul : Op::Mul, accumulator,
                               lane, lane_name(instruction, lane_index, "reduce"));
        case Kind::And:
            return emit_binary(before(instruction), Op::And, accumulator, lane,
                               lane_name(instruction, lane_index, "reduce"));
        case Kind::Or:
            return emit_binary(before(instruction), Op::Or, accumulator, lane,
                               lane_name(instruction, lane_index, "reduce"));
        case Kind::Xor:
            return emit_binary(before(instruction), Op::Xor, accumulator, lane,
                               lane_name(instruction, lane_index, "reduce"));
        case Kind::Min:
        case Kind::Max: {
            const auto pred = instruction.kind() == Kind::Min ? oir::CmpPred::LT : oir::CmpPred::GT;
            auto *compare =
                emit_compare(before(instruction), floating ? Op::FCmp : Op::ICmp, pred, lane,
                             accumulator, lane_name(instruction, lane_index, "reduce.cmp"));
            return conditional_value(
                instruction, compare, accumulator->type(),
                [&](InsertionPoint) -> oir::Value * { return lane; }, accumulator,
                lane_name(instruction, lane_index, "reduce.pick"));
        }
        }
        reject(Code::UnsupportedOperation,
               "PORTABLE_VECTOR_OPERATION_UNSUPPORTED: unknown reduction kind");
    }

    void transform_phi(oir::PhiInst &phi) {
        const auto *type = as_vector_type(&phi);
        if (type == nullptr)
            return;
        Lanes values;
        values.reserve(static_cast<std::size_t>(type->element_count().min_lanes));
        for (std::uint64_t lane = 0; lane < type->element_count().min_lanes; ++lane) {
            values.push_back(insert_as<oir::PhiInst>(before(phi), type->element_type(),
                                                     phi.parent(), lane_name(phi, lane, "phi")));
        }
        set_lanes(phi, std::move(values));
        vector_phis_.push_back(&phi);
        mark_erased(phi);
    }

    void finish_phis() {
        for (auto *phi : vector_phis_) {
            const auto &scalar_phis = lanes(phi);
            for (const auto &[incoming, predecessor] : phi->incoming()) {
                const auto &incoming_lanes = lanes(incoming);
                for (std::size_t lane = 0; lane < scalar_phis.size(); ++lane) {
                    auto *scalar_phi = dynamic_cast<oir::PhiInst *>(scalar_phis[lane]);
                    if (scalar_phi == nullptr)
                        reject(Code::MutationFailed,
                               "PORTABLE_MUTATION_FAILED: vector phi lane is not a scalar phi");
                    scalar_phi->add_incoming(incoming_lanes[lane], predecessor);
                }
            }
        }
    }

    void transform_binary(oir::BinaryInst &instruction) {
        const auto *type = as_vector_type(&instruction);
        if (type == nullptr)
            return;
        const auto &lhs = lanes(instruction.lhs());
        const auto &rhs = lanes(instruction.rhs());
        Lanes values;
        values.reserve(lhs.size());
        for (std::uint64_t lane = 0; lane < lhs.size(); ++lane) {
            values.push_back(emit_binary(before(instruction), instruction.op(), lhs[lane],
                                         rhs[lane], lane_name(instruction, lane)));
        }
        set_lanes(instruction, std::move(values));
        mark_erased(instruction);
    }

    void transform_compare(oir::CmpInst &instruction) {
        const auto *operand_type = as_vector_type(instruction.lhs());
        if (operand_type == nullptr)
            return;
        const auto &lhs = lanes(instruction.lhs());
        const auto &rhs = lanes(instruction.rhs());
        Lanes values;
        values.reserve(lhs.size());
        for (std::uint64_t lane = 0; lane < lhs.size(); ++lane) {
            values.push_back(emit_compare(before(instruction), instruction.op(), instruction.pred(),
                                          lhs[lane], rhs[lane], lane_name(instruction, lane)));
        }
        set_lanes(instruction, std::move(values));
        mark_erased(instruction);
    }

    void transform_splat(oir::SplatInst &instruction) {
        const auto *type = as_vector_type(&instruction);
        Lanes values(static_cast<std::size_t>(type->element_count().min_lanes),
                     instruction.scalar());
        set_lanes(instruction, std::move(values));
        mark_erased(instruction);
    }

    void transform_step(oir::StepVectorInst &instruction) {
        const auto *type = as_vector_type(&instruction);
        if (type == nullptr || !type->is_integer_vector() || type->is_mask())
            reject(Code::UnsupportedOperation,
                   "PORTABLE_VECTOR_OPERATION_UNSUPPORTED: stepvector requires a fixed i32 "
                   "vector");
        Lanes values;
        values.reserve(static_cast<std::size_t>(type->element_count().min_lanes));
        for (std::uint64_t lane = 0; lane < type->element_count().min_lanes; ++lane)
            values.push_back(module_.create_i32(static_cast<std::int64_t>(lane)));
        set_lanes(instruction, std::move(values));
        mark_erased(instruction);
    }

    void transform_extract(oir::ExtractElementInst &instruction) {
        const auto &source = lanes(instruction.vector());
        auto index = constant_lane_index(instruction.index(), source.size());
        auto *replacement = index ? source[*index] : dynamic_extract(instruction, source);
        instruction.replace_all_uses_with(replacement);
        mark_erased(instruction);
    }

    void transform_insert(oir::InsertElementInst &instruction) {
        const auto &source = lanes(instruction.vector());
        Lanes values(source.begin(), source.end());
        auto index = constant_lane_index(instruction.index(), source.size());
        if (index)
            values[*index] = instruction.element();
        else
            values = dynamic_insert(instruction, source);
        set_lanes(instruction, std::move(values));
        mark_erased(instruction);
    }

    void transform_shuffle(oir::ShuffleVectorInst &instruction) {
        const auto &lhs = lanes(instruction.lhs());
        const auto &rhs = lanes(instruction.rhs());
        Lanes values;
        values.reserve(instruction.shuffle_mask().size());
        for (auto index : instruction.shuffle_mask()) {
            if (index == -1) {
                values.push_back(module_.create_undef(lhs.front()->type()));
            } else if (index >= 0 && static_cast<std::uint64_t>(index) < lhs.size()) {
                values.push_back(lhs[static_cast<std::size_t>(index)]);
            } else if (index >= 0 && static_cast<std::uint64_t>(index) < lhs.size() + rhs.size()) {
                values.push_back(rhs[static_cast<std::size_t>(index) - lhs.size()]);
            } else {
                reject(Code::UnsupportedOperation,
                       "PORTABLE_VECTOR_OPERATION_UNSUPPORTED: shuffle lane is out of range");
            }
        }
        set_lanes(instruction, std::move(values));
        mark_erased(instruction);
    }

    void transform_select(oir::VectorSelectInst &instruction) {
        const auto &condition = lanes(instruction.condition());
        const auto &true_values = lanes(instruction.true_value());
        const auto &false_values = lanes(instruction.false_value());
        Lanes values;
        values.reserve(condition.size());
        for (std::uint64_t lane = 0; lane < condition.size(); ++lane) {
            values.push_back(conditional_value(
                instruction, condition[lane], true_values[lane]->type(),
                [&](InsertionPoint) -> oir::Value * { return true_values[lane]; },
                false_values[lane], lane_name(instruction, lane, "select")));
        }
        set_lanes(instruction, std::move(values));
        mark_erased(instruction);
    }

    void transform_cast(oir::VectorCastInst &instruction) {
        const auto *result_type = as_vector_type(&instruction);
        const auto &source = lanes(instruction.source());
        Lanes values;
        values.reserve(source.size());
        for (std::uint64_t lane = 0; lane < source.size(); ++lane) {
            switch (instruction.kind()) {
            case oir::VectorCastKind::ZExt:
                values.push_back(emit_cast(before(instruction), oir::Instruction::OpID::ZExt,
                                           result_type->element_type(), source[lane],
                                           lane_name(instruction, lane)));
                break;
            case oir::VectorCastKind::SIToFP:
                values.push_back(emit_cast(before(instruction), oir::Instruction::OpID::SIToFP,
                                           result_type->element_type(), source[lane],
                                           lane_name(instruction, lane)));
                break;
            case oir::VectorCastKind::FPToSI:
                values.push_back(emit_cast(before(instruction), oir::Instruction::OpID::FPToSI,
                                           result_type->element_type(), source[lane],
                                           lane_name(instruction, lane)));
                break;
            case oir::VectorCastKind::Bitcast:
                if (source[lane]->type() != result_type->element_type())
                    reject(Code::UnsupportedOperation,
                           "PORTABLE_VECTOR_OPERATION_UNSUPPORTED: scalar OIR has no audited "
                           "cross-family bitcast");
                values.push_back(source[lane]);
                break;
            }
        }
        set_lanes(instruction, std::move(values));
        mark_erased(instruction);
    }

    void transform_object_load(oir::LoadInst &instruction) {
        auto *type = as_vector_type(&instruction);
        if (type == nullptr)
            return;
        Lanes values;
        values.reserve(static_cast<std::size_t>(type->element_count().min_lanes));
        for (std::uint64_t lane = 0; lane < type->element_count().min_lanes; ++lane) {
            values.push_back(emit_boundary_object_load(
                before(instruction), type, instruction.ptr(), lane,
                lane_name(instruction, lane, "object.load")));
        }

        std::vector<oir::FixedABIExtractLaneInst *> old_extracts;
        for (const auto &use : instruction.uses()) {
            if (auto *extract =
                    dynamic_cast<oir::FixedABIExtractLaneInst *>(use.user))
                old_extracts.push_back(extract);
        }
        for (auto *extract : old_extracts) {
            extract->replace_all_uses_with(values[extract->lane_index()]);
            mark_erased(*extract);
        }

        set_lanes(instruction, std::move(values));
        ++result_.memory_operations_scalarized;
        mark_erased(instruction);
    }

    void transform_object_store(oir::StoreInst &instruction) {
        auto *type = as_vector_type(instruction.value());
        if (type == nullptr)
            return;
        const auto &values = lanes(instruction.value());
        for (std::uint64_t lane = 0; lane < values.size(); ++lane) {
            (void)emit_boundary_object_store(before(instruction), values[lane],
                                             instruction.ptr(), lane);
        }
        ++result_.memory_operations_scalarized;
        mark_erased(instruction);
    }

    void transform_call(oir::CallInst &instruction) {
        auto arguments = instruction.args();
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            auto *type = as_vector_type(arguments[index]);
            if (type == nullptr ||
                dynamic_cast<oir::FixedABIPackInst *>(arguments[index]) != nullptr)
                continue;
            std::string base = instruction.name().empty() ? "portable.call" : instruction.name();
            auto *pack = emit_boundary_pack(
                before(instruction), type, lanes(arguments[index]),
                fresh_name(base + ".arg" + std::to_string(index) + ".pack"));
            instruction.set_operand(index + 1U, pack);
        }

        auto *result_type = as_vector_type(&instruction);
        if (result_type == nullptr)
            return;
        Lanes values;
        values.reserve(static_cast<std::size_t>(result_type->element_count().min_lanes));
        auto point = after(instruction);
        for (std::uint64_t lane = 0; lane < result_type->element_count().min_lanes; ++lane) {
            values.push_back(emit_boundary_extract(
                point, &instruction, lane, lane_name(instruction, lane, "call.result")));
        }
        set_lanes(instruction, std::move(values));
    }

    void transform_return(oir::ReturnInst &instruction) {
        if (!instruction.has_value())
            return;
        auto *type = as_vector_type(instruction.value());
        if (type == nullptr ||
            dynamic_cast<oir::FixedABIPackInst *>(instruction.value()) != nullptr)
            return;
        auto *pack = emit_boundary_pack(before(instruction), type,
                                        lanes(instruction.value()),
                                        fresh_name("portable.return.pack"));
        instruction.set_operand(0, pack);
    }

    void transform_vp_binary(oir::VPBinaryInst &instruction) {
        const auto *type = as_vector_type(&instruction);
        require_full_fixed_evl(instruction, type->element_count().min_lanes);
        const auto &active = lanes(instruction.active_mask());
        const auto &passthrough = lanes(instruction.passthrough());
        const auto &lhs = lanes(instruction.lhs());
        const auto &rhs = lanes(instruction.rhs());
        Lanes values;
        values.reserve(lhs.size());
        for (std::uint64_t lane = 0; lane < lhs.size(); ++lane) {
            values.push_back(conditional_value(
                instruction, active[lane], type->element_type(),
                [&](InsertionPoint point) -> oir::Value * {
                    return emit_binary(point, instruction.binary_op(), lhs[lane], rhs[lane],
                                       lane_name(instruction, lane, "active"));
                },
                passthrough[lane], lane_name(instruction, lane, "merge")));
        }
        set_lanes(instruction, std::move(values));
        mark_erased(instruction);
    }

    void transform_vp_compare(oir::VPCmpInst &instruction) {
        const auto *type = as_vector_type(&instruction);
        require_full_fixed_evl(instruction, type->element_count().min_lanes);
        const auto &active = lanes(instruction.active_mask());
        const auto &passthrough = lanes(instruction.passthrough());
        const auto &lhs = lanes(instruction.lhs());
        const auto &rhs = lanes(instruction.rhs());
        Lanes values;
        values.reserve(lhs.size());
        for (std::uint64_t lane = 0; lane < lhs.size(); ++lane) {
            values.push_back(conditional_value(
                instruction, active[lane], module_.types().int1_ty(),
                [&](InsertionPoint point) -> oir::Value * {
                    return emit_compare(point, instruction.comparison_op(), instruction.pred(),
                                        lhs[lane], rhs[lane],
                                        lane_name(instruction, lane, "active"));
                },
                passthrough[lane], lane_name(instruction, lane, "merge")));
        }
        set_lanes(instruction, std::move(values));
        mark_erased(instruction);
    }

    void transform_load(oir::VPLoadInst &instruction) {
        const auto *type = as_vector_type(&instruction);
        require_full_fixed_evl(instruction, type->element_count().min_lanes);
        const auto &active = lanes(instruction.active_mask());
        const auto &passthrough = lanes(instruction.passthrough());
        Lanes values;
        values.reserve(active.size());
        for (std::uint64_t lane = 0; lane < active.size(); ++lane) {
            values.push_back(conditional_value(
                instruction, active[lane], type->element_type(),
                [&](InsertionPoint point) -> oir::Value * {
                    auto *address =
                        emit_element_address(point, instruction.ptr(),
                                             module_.create_i32(static_cast<std::int64_t>(lane)),
                                             lane_name(instruction, lane, "addr"));
                    return insert_as<oir::LoadInst>(point, type->element_type(), address,
                                                    point.block,
                                                    lane_name(instruction, lane, "load"));
                },
                passthrough[lane], lane_name(instruction, lane, "merge")));
        }
        set_lanes(instruction, std::move(values));
        ++result_.memory_operations_scalarized;
        mark_erased(instruction);
    }

    void transform_store(oir::VPStoreInst &instruction) {
        const auto *type = as_vector_type(instruction.value());
        require_full_fixed_evl(instruction, type->element_count().min_lanes);
        const auto &active = lanes(instruction.active_mask());
        const auto &values = lanes(instruction.value());
        for (std::uint64_t lane = 0; lane < active.size(); ++lane) {
            conditional_effect(instruction, active[lane], [&](InsertionPoint point) {
                auto *address = emit_element_address(
                    point, instruction.ptr(), module_.create_i32(static_cast<std::int64_t>(lane)),
                    "portable.store.addr");
                insert_as<oir::StoreInst>(point, module_.types().void_ty(), values[lane], address,
                                          point.block);
            });
        }
        ++result_.memory_operations_scalarized;
        mark_erased(instruction);
    }

    void transform_gather(oir::VPGatherInst &instruction) {
        const auto *type = as_vector_type(&instruction);
        require_full_fixed_evl(instruction, type->element_count().min_lanes);
        const auto &active = lanes(instruction.active_mask());
        const auto &passthrough = lanes(instruction.passthrough());
        const auto &indices = lanes(instruction.indices());
        Lanes values;
        values.reserve(active.size());
        for (std::uint64_t lane = 0; lane < active.size(); ++lane) {
            values.push_back(conditional_value(
                instruction, active[lane], type->element_type(),
                [&](InsertionPoint point) -> oir::Value * {
                    auto *address =
                        emit_element_address(point, instruction.base_ptr(), indices[lane],
                                             lane_name(instruction, lane, "gather.addr"));
                    return insert_as<oir::LoadInst>(point, type->element_type(), address,
                                                    point.block,
                                                    lane_name(instruction, lane, "gather"));
                },
                passthrough[lane], lane_name(instruction, lane, "merge")));
        }
        set_lanes(instruction, std::move(values));
        ++result_.memory_operations_scalarized;
        mark_erased(instruction);
    }

    void transform_scatter(oir::VPScatterInst &instruction) {
        const auto *type = as_vector_type(instruction.value());
        require_full_fixed_evl(instruction, type->element_count().min_lanes);
        const auto &active = lanes(instruction.active_mask());
        const auto &values = lanes(instruction.value());
        const auto &indices = lanes(instruction.indices());
        for (std::uint64_t lane = 0; lane < active.size(); ++lane) {
            conditional_effect(instruction, active[lane], [&](InsertionPoint point) {
                auto *address = emit_element_address(point, instruction.base_ptr(), indices[lane],
                                                     "portable.scatter.addr");
                insert_as<oir::StoreInst>(point, module_.types().void_ty(), values[lane], address,
                                          point.block);
            });
        }
        ++result_.memory_operations_scalarized;
        mark_erased(instruction);
    }

    void transform_reduction(oir::VPReductionInst &instruction) {
        const auto &values = lanes(instruction.vector());
        const auto &active = lanes(instruction.active_mask());
        require_full_fixed_evl(instruction, values.size());
        auto *accumulator = instruction.passthrough();
        for (std::uint64_t lane = 0; lane < values.size(); ++lane) {
            auto *combined = emit_reduction_combine(instruction, accumulator, values[lane], lane);
            accumulator = conditional_value(
                instruction, active[lane], accumulator->type(),
                [&](InsertionPoint) -> oir::Value * { return combined; }, accumulator,
                lane_name(instruction, lane, "reduce.active"));
        }
        instruction.replace_all_uses_with(accumulator);
        mark_erased(instruction);
    }

    void transform_instruction(oir::Instruction &instruction) {
        using Op = oir::Instruction::OpID;
        if (!instruction_involves_direct_vector(instruction) ||
            is_fixed_abi_boundary_op(instruction.op()) ||
            is_normalized_boundary_consumer(instruction))
            return;
        switch (instruction.op()) {
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::SDiv:
        case Op::SRem:
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::FAdd:
        case Op::FSub:
        case Op::FMul:
        case Op::FDiv:
            transform_binary(dynamic_cast<oir::BinaryInst &>(instruction));
            return;
        case Op::ICmp:
        case Op::FCmp:
            transform_compare(dynamic_cast<oir::CmpInst &>(instruction));
            return;
        case Op::Phi:
            return;
        case Op::Splat:
            transform_splat(dynamic_cast<oir::SplatInst &>(instruction));
            return;
        case Op::StepVector:
            transform_step(dynamic_cast<oir::StepVectorInst &>(instruction));
            return;
        case Op::ExtractElement:
            transform_extract(dynamic_cast<oir::ExtractElementInst &>(instruction));
            return;
        case Op::InsertElement:
            transform_insert(dynamic_cast<oir::InsertElementInst &>(instruction));
            return;
        case Op::ShuffleVector:
            transform_shuffle(dynamic_cast<oir::ShuffleVectorInst &>(instruction));
            return;
        case Op::VectorSelect:
            transform_select(dynamic_cast<oir::VectorSelectInst &>(instruction));
            return;
        case Op::VectorCast:
            transform_cast(dynamic_cast<oir::VectorCastInst &>(instruction));
            return;
        case Op::Load:
            transform_object_load(dynamic_cast<oir::LoadInst &>(instruction));
            return;
        case Op::Store:
            transform_object_store(dynamic_cast<oir::StoreInst &>(instruction));
            return;
        case Op::Call:
            transform_call(dynamic_cast<oir::CallInst &>(instruction));
            return;
        case Op::Ret:
            transform_return(dynamic_cast<oir::ReturnInst &>(instruction));
            return;
        case Op::VPBinary:
            transform_vp_binary(dynamic_cast<oir::VPBinaryInst &>(instruction));
            return;
        case Op::VPCmp:
            transform_vp_compare(dynamic_cast<oir::VPCmpInst &>(instruction));
            return;
        case Op::VPLoad:
        case Op::MaskedLoad:
            transform_load(dynamic_cast<oir::VPLoadInst &>(instruction));
            return;
        case Op::VPStore:
        case Op::MaskedStore:
            transform_store(dynamic_cast<oir::VPStoreInst &>(instruction));
            return;
        case Op::VPGather:
            transform_gather(dynamic_cast<oir::VPGatherInst &>(instruction));
            return;
        case Op::VPScatter:
            transform_scatter(dynamic_cast<oir::VPScatterInst &>(instruction));
            return;
        case Op::VPReduction:
            transform_reduction(dynamic_cast<oir::VPReductionInst &>(instruction));
            return;
        case Op::FixedABIExtractLane:
        case Op::FixedABIPack:
        case Op::FixedABIObjectLoadLane:
        case Op::FixedABIObjectStoreLane:
            return;
        case Op::SetVL:
        case Op::Br:
        case Op::Alloca:
        case Op::MemZero:
        case Op::GetElementPtr:
        case Op::ZExt:
        case Op::SIToFP:
        case Op::FPToSI:
            break;
        }
        reject(Code::UnsupportedOperation,
               "PORTABLE_VECTOR_OPERATION_UNSUPPORTED: vector instruction escaped preflight");
    }

    void mark_erased(oir::Instruction &instruction) {
        if (erase_set_.insert(&instruction).second) {
            erase_order_.push_back(&instruction);
            ++result_.vector_instructions_scalarized;
        }
    }

    void validate_and_erase_originals(oir::Function &function) {
        for (auto *instruction : erase_order_) {
            for (const auto &use : instruction->uses()) {
                auto *user = dynamic_cast<oir::Instruction *>(use.user);
                if (user == nullptr || erase_set_.find(user) == erase_set_.end()) {
                    reject(Code::UnsupportedOperation,
                           "PORTABLE_VECTOR_OPERATION_UNSUPPORTED: transformed vector value "
                           "still has a non-scalarized user");
                }
            }
        }
        for (auto *instruction : erase_order_)
            instruction->drop_all_operands();
        for (auto &block : function.blocks()) {
            for (auto position = block->instructions().begin();
                 position != block->instructions().end();) {
                if (erase_set_.find(position->get()) == erase_set_.end()) {
                    ++position;
                    continue;
                }
                position = block->instructions().erase(position);
            }
        }
    }

    void verify_scalar_function(const oir::Function &function) const {
        for (const auto &block : function.blocks()) {
            for (const auto &instruction : block->instructions()) {
                if (instruction_involves_direct_vector(*instruction) &&
                    !is_fixed_abi_boundary_op(instruction->op()) &&
                    !is_normalized_boundary_consumer(*instruction)) {
                    reject(Code::MutationFailed,
                           "PORTABLE_MUTATION_FAILED: non-boundary vector instruction remains "
                           "after scalarization");
                }
            }
        }
    }

    void seed_boundary_arguments(oir::Function &function) {
        auto *entry = function.entry_block();
        if (entry == nullptr)
            reject(Code::MutationFailed,
                   "PORTABLE_MUTATION_FAILED: function has no entry block");
        auto &instructions = entry->instructions();
        auto position = std::find_if(
            instructions.begin(), instructions.end(),
            [](const std::unique_ptr<oir::Instruction> &instruction) {
                return instruction->op() != oir::Instruction::OpID::Phi;
            });
        InsertionPoint point{entry,
                             position == instructions.end() ? nullptr : position->get()};

        for (const auto &argument : function.args()) {
            auto *type = as_vector_type(argument.get());
            if (type == nullptr)
                continue;
            Lanes values;
            values.reserve(static_cast<std::size_t>(type->element_count().min_lanes));
            std::string base = argument->name().empty()
                                   ? "portable.arg" + std::to_string(argument->index())
                                   : "portable.arg." + argument->name();
            for (std::uint64_t lane = 0; lane < type->element_count().min_lanes; ++lane) {
                values.push_back(emit_boundary_extract(
                    point, argument.get(), lane,
                    fresh_name(base + ".lane" + std::to_string(lane))));
            }
            set_lanes(*argument, std::move(values));
        }
    }

    void transform_function(oir::Function &function) {
        used_names_.clear();
        lane_values_.clear();
        vector_phis_.clear();
        erase_order_.clear();
        erase_set_.clear();
        for (const auto &argument : function.args())
            used_names_.insert(argument->name());
        for (const auto &block : function.blocks()) {
            used_names_.insert(block->name());
            for (const auto &instruction : block->instructions()) {
                if (!instruction->name().empty())
                    used_names_.insert(instruction->name());
            }
        }

        seed_boundary_arguments(function);

        std::vector<oir::Instruction *> worklist;
        for (auto &block : function.blocks()) {
            for (auto &instruction : block->instructions())
                worklist.push_back(instruction.get());
        }
        for (auto *instruction : worklist) {
            if (instruction->op() == oir::Instruction::OpID::Phi &&
                instruction->type()->is_vector())
                transform_phi(dynamic_cast<oir::PhiInst &>(*instruction));
        }
        for (auto *instruction : worklist)
            transform_instruction(*instruction);
        finish_phis();
        validate_and_erase_originals(function);
        verify_scalar_function(function);
    }

    oir::Module &module_;
    PortableVectorScalarizerResult &result_;
    std::unordered_set<std::string> used_names_;
    std::unordered_map<oir::Value *, Lanes> lane_values_;
    std::vector<oir::PhiInst *> vector_phis_;
    std::vector<oir::Instruction *> erase_order_;
    std::unordered_set<oir::Instruction *> erase_set_;
};

PortableVectorScalarizerResult failed_result(Code code, std::string message) {
    PortableVectorScalarizerResult result;
    result.success = false;
    result.changed = false;
    result.code = code;
    result.message = std::move(message);
    return result;
}

std::string snapshot_with_unique_ssa_names(oir::Module &module) {
    struct Rename final {
        oir::Value *value = nullptr;
        std::string original;
    };

    std::vector<Rename> renamed;
    const auto restore = [&]() {
        for (auto iterator = renamed.rbegin(); iterator != renamed.rend();
             ++iterator) {
            iterator->value->set_name(iterator->original);
        }
    };

    try {
        for (const auto &function : module.functions()) {
            std::unordered_set<std::string> all_names;
            for (const auto &argument : function->args()) {
                if (!argument->name().empty())
                    all_names.insert(argument->name());
            }
            for (const auto &block : function->blocks()) {
                for (const auto &instruction : block->instructions()) {
                    if (!instruction->name().empty())
                        all_names.insert(instruction->name());
                }
            }

            std::unordered_set<std::string> emitted_names;
            std::uint64_t suffix = 0;
            const auto make_unique = [&](oir::Value &value) {
                const auto &name = value.name();
                if (name.empty() || emitted_names.insert(name).second)
                    return;
                std::string candidate;
                do {
                    candidate = name + ".portable.snapshot." +
                                std::to_string(suffix++);
                } while (all_names.find(candidate) != all_names.end());
                all_names.insert(candidate);
                emitted_names.insert(candidate);
                renamed.push_back({&value, name});
                value.set_name(candidate);
            };

            for (const auto &argument : function->args())
                make_unique(*argument);
            for (const auto &block : function->blocks()) {
                for (const auto &instruction : block->instructions())
                    make_unique(*instruction);
            }
        }
        auto snapshot = module.print();
        restore();
        return snapshot;
    } catch (...) {
        restore();
        throw;
    }
}

} // namespace

std::string_view scalarization_reason_code_name(ScalarizationReasonCode code) {
    switch (code) {
    case Code::Scalarized:
        return "PORTABLE_SCALARIZED";
    case Code::NoFixedVector:
        return "PORTABLE_NO_FIXED_VECTOR";
    case Code::InputVerificationFailed:
        return "PORTABLE_INPUT_VERIFICATION_FAILED";
    case Code::SnapshotFailed:
        return "PORTABLE_SNAPSHOT_FAILED";
    case Code::ScalableVectorUnsupported:
        return "PORTABLE_SCALABLE_VECTOR_UNSUPPORTED";
    case Code::AggregateABIUnavailable:
        return "PORTABLE_VECTOR_AGGREGATE_ABI_UNAVAILABLE";
    case Code::FixedEVLRequired:
        return "PORTABLE_FIXED_EVL_REQUIRED";
    case Code::UnsupportedOperation:
        return "PORTABLE_VECTOR_OPERATION_UNSUPPORTED";
    case Code::MutationFailed:
        return "PORTABLE_MUTATION_FAILED";
    case Code::OutputVerificationFailed:
        return "PORTABLE_OUTPUT_VERIFICATION_FAILED";
    case Code::PostVerificationFailed:
        return "PORTABLE_POST_VERIFICATION_FAILED";
    }
    return "PORTABLE_UNKNOWN";
}

PortableVectorScalarizer::PortableVectorScalarizer(PortableVectorScalarizerOptions options)
    : options_(std::move(options)) {
}

PortableVectorScalarizerResult PortableVectorScalarizer::run(oir::Module &module) const {
    PortableVectorScalarizerResult result;
    std::string verification_error;
    if (!module.verify(&verification_error)) {
        return failed_result(Code::InputVerificationFailed,
                             "PORTABLE_INPUT_VERIFICATION_FAILED: " + verification_error);
    }

    try {
        if (!preflight(module)) {
            result.code = Code::NoFixedVector;
            result.message = "PORTABLE_NO_FIXED_VECTOR: module has no fixed vector operations";
            return result;
        }
    } catch (const ScalarizationFailure &failure) {
        return failed_result(failure.code, failure.message);
    }

    std::string snapshot;
    try {
        snapshot = snapshot_with_unique_ssa_names(module);
    } catch (const std::exception &error) {
        return failed_result(
            Code::SnapshotFailed,
            "PORTABLE_SNAPSHOT_FAILED: cannot create a uniquely named private "
            "transaction clone: " +
                std::string(error.what()));
    }
    auto parsed = oir::OIRParser::parse(snapshot, "<portable-scalarizer-clone>");
    if (!parsed.ok() || parsed.module == nullptr) {
        std::string detail =
            parsed.errors.empty() ? "unknown parser failure" : parsed.errors.front().message;
        return failed_result(Code::SnapshotFailed, "PORTABLE_SNAPSHOT_FAILED: " + detail);
    }

    try {
        Transformer(*parsed.module, result).run();
    } catch (const ScalarizationFailure &failure) {
        return failed_result(failure.code, failure.message);
    } catch (const std::exception &error) {
        return failed_result(Code::MutationFailed,
                             "PORTABLE_MUTATION_FAILED: " + std::string(error.what()));
    }

    verification_error.clear();
    if (!parsed.module->verify(&verification_error)) {
        return failed_result(Code::OutputVerificationFailed,
                             "PORTABLE_OUTPUT_VERIFICATION_FAILED: " + verification_error);
    }
    if (options_.post_transform_verifier) {
        std::string detail;
        if (!options_.post_transform_verifier(*parsed.module, detail)) {
            if (detail.empty())
                detail = "post-transform verifier rejected scalar OIR";
            return failed_result(Code::PostVerificationFailed,
                                 "PORTABLE_POST_VERIFICATION_FAILED: " + detail);
        }
    }

    try {
        module.replace_with(std::move(*parsed.module));
    } catch (const std::exception &error) {
        return failed_result(Code::MutationFailed, "PORTABLE_MUTATION_FAILED: commit failed: " +
                                                       std::string(error.what()));
    }
    (void)options_.force;
    result.success = true;
    result.changed = true;
    result.code = Code::Scalarized;
    result.message = "PORTABLE_SCALARIZED: vector_instructions=" +
                     std::to_string(result.vector_instructions_scalarized) +
                     ", scalar_instructions=" + std::to_string(result.scalar_instructions_created) +
                     ", boundary_instructions=" +
                     std::to_string(result.boundary_instructions_created) +
                     ", cfg_blocks=" + std::to_string(result.cfg_blocks_created) +
                     ", memory_operations=" + std::to_string(result.memory_operations_scalarized);
    return result;
}

} // namespace pass::oir_portable

namespace pass {

OIRPortableVectorScalarizerPass::OIRPortableVectorScalarizerPass(
    oir_portable::PortableVectorScalarizerOptions options)
    : options_(std::move(options)) {
}

std::string_view OIRPortableVectorScalarizerPass::name() const {
    return "OIRPortableVectorScalarizerPass";
}

PassKind OIRPortableVectorScalarizerPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRPortableVectorScalarizerPass::run(PassContext &context) {
    auto *module = context.ssa_module();
    if (module == nullptr) {
        return PassResult::fail(
            "PORTABLE_MISSING_MODULE: OIRPortableVectorScalarizerPass requires OIR module");
    }
    auto result = oir_portable::PortableVectorScalarizer(options_).run(*module);
    if (!result.success)
        return PassResult::fail(std::move(result.message));
    if (result.changed)
        context.invalidate_oir_analyses();
    return PassResult::ok(result.changed, std::move(result.message));
}

} // namespace pass
