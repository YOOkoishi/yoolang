#include "pass/oir/OIRVectorization.h"

#include "oir/OIRCFGUtils.h"
#include "oir/OIRParser.h"
#include "pass/oir/RVVTargetCostModel.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pass::oir_vectorize {
namespace {

bool loop_contains(const oir::Loop &loop, const oir::BasicBlock *block) {
    return std::find(loop.blocks.begin(), loop.blocks.end(), block) != loop.blocks.end();
}

std::optional<std::int64_t> addrec_step(const oir::SCEVExpr &expression) {
    if (expression.kind() != oir::SCEVKind::AddRec || expression.rhs() == nullptr) {
        return std::nullopt;
    }
    return expression.rhs()->constant_value();
}

const oir::Value *underlying_object(const oir::Value *pointer, const oir::Loop &loop,
                                    const oir::OIRAliasAnalysis &alias_analysis) {
    if (const auto *phi = dynamic_cast<const oir::PhiInst *>(pointer)) {
        const oir::Value *outside_object = nullptr;
        for (const auto &[incoming, block] : phi->incoming()) {
            if (loop_contains(loop, block)) {
                continue;
            }
            const auto *candidate = underlying_object(incoming, loop, alias_analysis);
            if (outside_object != nullptr && outside_object != candidate) {
                return nullptr;
            }
            outside_object = candidate;
        }
        return outside_object;
    }
    return alias_analysis.memory_location(pointer).base;
}

std::optional<std::int64_t> pointer_stride(const oir::Value *pointer, const oir::Loop &loop,
                                           const oir::ScalarEvolution &scev) {
    if (const auto *phi = dynamic_cast<const oir::PhiInst *>(pointer)) {
        if (!loop_contains(loop, phi->parent())) {
            return 0;
        }
        for (const auto &[incoming, block] : phi->incoming()) {
            if (!loop_contains(loop, block)) {
                continue;
            }
            const auto *update = dynamic_cast<const oir::GetElementPtrInst *>(incoming);
            if (update == nullptr || update->base_ptr() != phi) {
                return std::nullopt;
            }
            const auto indices = update->indices();
            if (indices.size() != 1) {
                return std::nullopt;
            }
            if (const auto *constant = dynamic_cast<const oir::ConstantInt *>(indices.front())) {
                return constant->value();
            }
            if (dynamic_cast<const oir::ConstantZero *>(indices.front()) != nullptr) {
                return 0;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }
    const auto *gep = dynamic_cast<const oir::GetElementPtrInst *>(pointer);
    if (gep == nullptr) {
        return 0;
    }
    if (!loop_contains(loop, gep->parent())) {
        return 0;
    }
    auto indices = gep->indices();
    const auto *base_pointer_type = dynamic_cast<const oir::PointerType *>(gep->base_ptr()->type());
    const auto *result_pointer_type = dynamic_cast<const oir::PointerType *>(gep->type());
    const bool preserves_pointee_type =
        base_pointer_type != nullptr && result_pointer_type != nullptr &&
        base_pointer_type->element_type() == result_pointer_type->element_type();
    auto inherited_base_stride = [&]() -> std::optional<std::int64_t> {
        auto base_stride = pointer_stride(gep->base_ptr(), loop, scev);
        if (!base_stride || (*base_stride != 0 && !preserves_pointee_type)) {
            return std::nullopt;
        }
        return base_stride;
    };
    if (indices.empty()) {
        return inherited_base_stride();
    }
    if (auto direct_stride = addrec_step(scev.expression_for(indices.back(), &loop))) {
        // A dynamic final index is an element stride only when the GEP base
        // itself is loop invariant.  Combining two recurrences would require
        // scaling each one by its pointee layout, which this analysis does not
        // guess.
        auto base_stride = pointer_stride(gep->base_ptr(), loop, scev);
        return base_stride && *base_stride == 0 ? direct_stride : std::nullopt;
    }
    // Strength reduction commonly represents an unrolled access as a
    // constant-offset GEP from a pointer induction.  The offset changes the
    // lane-zero address, not the per-iteration stride, so inherit the base
    // recurrence only when every added index is loop invariant.
    if (std::all_of(indices.begin(), indices.end(),
                    [&](const oir::Value *index) { return scev.is_loop_invariant(index, loop); })) {
        return inherited_base_stride();
    }
    return std::nullopt;
}

bool same_lane_address(const MemoryAccess &lhs, const MemoryAccess &rhs) {
    if (lhs.pointer == rhs.pointer) {
        return true;
    }
    const auto *lhs_gep = dynamic_cast<const oir::GetElementPtrInst *>(lhs.pointer);
    const auto *rhs_gep = dynamic_cast<const oir::GetElementPtrInst *>(rhs.pointer);
    if (lhs_gep == nullptr || rhs_gep == nullptr || lhs_gep->base_ptr() != rhs_gep->base_ptr()) {
        return false;
    }
    const auto lhs_indices = lhs_gep->indices();
    const auto rhs_indices = rhs_gep->indices();
    if (lhs_indices.size() != rhs_indices.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs_indices.size(); ++index) {
        if (lhs_indices[index] == rhs_indices[index]) {
            continue;
        }
        const auto *lhs_integer = dynamic_cast<const oir::ConstantInt *>(lhs_indices[index]);
        const auto *rhs_integer = dynamic_cast<const oir::ConstantInt *>(rhs_indices[index]);
        if (lhs_integer == nullptr || rhs_integer == nullptr ||
            lhs_integer->type() != rhs_integer->type() ||
            lhs_integer->value() != rhs_integer->value()) {
            return false;
        }
    }
    return true;
}

bool is_supported_numeric_type(const oir::Type *type) {
    if (type == nullptr) {
        return false;
    }
    if (const auto *integer = dynamic_cast<const oir::IntegerType *>(type)) {
        return integer->bit_width() == 1 || integer->bit_width() == 32;
    }
    return type->is_scalar_float();
}

bool instruction_has_float_value(const oir::Instruction &instruction) {
    if (instruction.type() != nullptr && instruction.type()->is_scalar_float()) {
        return true;
    }
    for (auto *operand : instruction.operands()) {
        if (operand != nullptr && operand->type() != nullptr &&
            operand->type()->is_scalar_float()) {
            return true;
        }
    }
    return false;
}

bool is_float_reduction(const oir::PhiInst &phi, const oir::Loop &loop) {
    if (!phi.type()->is_scalar_float()) {
        return false;
    }
    for (const auto &[value, from] : phi.incoming()) {
        if (!loop_contains(loop, from)) {
            continue;
        }
        const auto *binary = dynamic_cast<const oir::BinaryInst *>(value);
        if (binary == nullptr) {
            continue;
        }
        switch (binary->op()) {
        case oir::Instruction::OpID::FAdd:
        case oir::Instruction::OpID::FSub:
        case oir::Instruction::OpID::FMul:
        case oir::Instruction::OpID::FDiv:
            if (binary->lhs() == &phi || binary->rhs() == &phi) {
                return true;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

bool is_supported_integer_reduction_op(oir::Instruction::OpID op) {
    switch (op) {
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Mul:
    case oir::Instruction::OpID::And:
    case oir::Instruction::OpID::Or:
    case oir::Instruction::OpID::Xor:
        return true;
    default:
        return false;
    }
}

struct IntegerReductionMatch final {
    const oir::BinaryInst *update = nullptr;
    oir::Instruction::OpID operation = oir::Instruction::OpID::Add;
    std::vector<const oir::BinaryInst *> chain_updates;
    std::vector<const oir::Value *> lane_values;
};

std::optional<IntegerReductionMatch> match_integer_reduction(const oir::PhiInst &phi,
                                                             const oir::Loop &loop) {
    const auto *integer = dynamic_cast<const oir::IntegerType *>(phi.type());
    if (integer == nullptr || integer->bit_width() != 32 || phi.incoming().size() != 2) {
        return std::nullopt;
    }
    const oir::Value *latch_value = nullptr;
    unsigned loop_incoming = 0;
    unsigned outside_incoming = 0;
    for (const auto &[value, block] : phi.incoming()) {
        if (loop_contains(loop, block)) {
            latch_value = value;
            ++loop_incoming;
        } else {
            ++outside_incoming;
        }
    }
    if (loop_incoming != 1 || outside_incoming != 1) {
        return std::nullopt;
    }
    const auto *update = dynamic_cast<const oir::BinaryInst *>(latch_value);
    if (update == nullptr || !loop_contains(loop, update->parent()) ||
        !is_supported_integer_reduction_op(update->op()) || update->type() != phi.type()) {
        return std::nullopt;
    }

    std::unordered_set<const oir::Value *> active;
    std::function<bool(const oir::Value *)> depends_on_accumulator;
    depends_on_accumulator = [&](const oir::Value *value) {
        if (value == &phi) {
            return true;
        }
        const auto *instruction = dynamic_cast<const oir::Instruction *>(value);
        if (instruction == nullptr || !loop_contains(loop, instruction->parent()) ||
            dynamic_cast<const oir::PhiInst *>(instruction) != nullptr ||
            !active.insert(value).second) {
            return false;
        }
        const bool dependent =
            std::any_of(instruction->operands().begin(), instruction->operands().end(),
                        [&](const oir::Value *operand) { return depends_on_accumulator(operand); });
        active.erase(value);
        return dependent;
    };

    IntegerReductionMatch match;
    match.update = update;
    match.operation = update->op();
    std::function<bool(const oir::BinaryInst *)> collect_chain;
    collect_chain = [&](const oir::BinaryInst *node) {
        if (node == nullptr || node->op() != match.operation || node->type() != phi.type() ||
            !loop_contains(loop, node->parent())) {
            return false;
        }
        const bool lhs_dependent = depends_on_accumulator(node->lhs());
        const bool rhs_dependent = depends_on_accumulator(node->rhs());
        if (lhs_dependent == rhs_dependent) {
            return false;
        }
        const auto *dependent = lhs_dependent ? node->lhs() : node->rhs();
        const auto *lane_value = lhs_dependent ? node->rhs() : node->lhs();
        if (depends_on_accumulator(lane_value)) {
            return false;
        }
        if (dependent != &phi) {
            const auto *nested = dynamic_cast<const oir::BinaryInst *>(dependent);
            if (!collect_chain(nested)) {
                return false;
            }
        }
        match.chain_updates.push_back(node);
        match.lane_values.push_back(lane_value);
        return true;
    };
    if (!collect_chain(update) || match.chain_updates.empty() ||
        match.chain_updates.size() != match.lane_values.size()) {
        return std::nullopt;
    }
    return match;
}

oir::ReductionKind reduction_kind_for(oir::Instruction::OpID op) {
    switch (op) {
    case oir::Instruction::OpID::Add:
        return oir::ReductionKind::Add;
    case oir::Instruction::OpID::Mul:
        return oir::ReductionKind::Mul;
    case oir::Instruction::OpID::And:
        return oir::ReductionKind::And;
    case oir::Instruction::OpID::Or:
        return oir::ReductionKind::Or;
    case oir::Instruction::OpID::Xor:
        return oir::ReductionKind::Xor;
    default:
        throw std::logic_error("unsupported integer reduction opcode");
    }
}

bool divisor_is_proven_nonzero(const oir::Instruction &instruction) {
    if (instruction.operand_count() != 2) {
        return false;
    }
    const auto *constant = dynamic_cast<const oir::ConstantInt *>(instruction.operand(1));
    return constant != nullptr && constant->value() != 0;
}

std::optional<std::int64_t> integer_constant(const oir::Value *value) {
    if (const auto *constant = dynamic_cast<const oir::ConstantInt *>(value)) {
        return constant->value();
    }
    if (dynamic_cast<const oir::ConstantZero *>(value) != nullptr && value != nullptr &&
        value->type()->is_scalar_integer()) {
        return 0;
    }
    return std::nullopt;
}

bool is_supported_constant_stride(std::int64_t stride) {
    switch (stride) {
    case -4:
    case -2:
    case -1:
    case 1:
    case 2:
    case 4:
        return true;
    default:
        return false;
    }
}

bool is_supported_memory_stride(std::int64_t stride, bool is_write) {
    // Repeated-address loads are lane-invariant and may be represented by an
    // indexed gather whose offsets are all zero.  A repeated-address store is
    // not legal: RVV does not define an ordering among duplicate scatter
    // destinations that preserves the scalar last-writer semantics.
    return (stride == 0 && !is_write) || is_supported_constant_stride(stride);
}

// RVV constrains VLEN to at most 65536 bits and permits a minimum VLEN of
// 32 bits for the Zve profiles accepted by this vectorizer.  A scalable OIR
// minimum-lane count can therefore grow by at most 65536 / 32 at run time.
// Use that deliberately conservative bound (it can double-count a target's
// already-larger minimum VLEN) before constructing the i32 arithmetic used by
// the reverse indexed-memory recipe.  Failing to prove the bound is a hard
// legality failure: wrapping an e32 index would be zero-extended by RV64 RVV
// indexed loads/stores and could access a completely different address.
bool reverse_index_offset_fits_i32(std::int64_t stride, unsigned minimum_lanes) {
    constexpr std::uint64_t maximum_vlen_bits = 65536U;
    constexpr std::uint64_t minimum_vector_bits = 32U;
    constexpr std::uint64_t element_bytes = 4U;
    constexpr std::uint64_t maximum_vscale = maximum_vlen_bits / minimum_vector_bits;
    static_assert(maximum_vscale == 2048U);

    if (stride >= 0 || stride == std::numeric_limits<std::int64_t>::min()) {
        return stride >= 0;
    }
    const auto magnitude = static_cast<std::uint64_t>(-stride);
    const auto lanes = static_cast<std::uint64_t>(std::max(1U, minimum_lanes));
    if (lanes > std::numeric_limits<std::uint64_t>::max() / maximum_vscale) {
        return false;
    }
    const auto maximum_active_lanes = lanes * maximum_vscale;
    if (maximum_active_lanes <= 1U) {
        return true;
    }
    const auto maximum_lane = maximum_active_lanes - 1U;
    if (magnitude > std::numeric_limits<std::uint64_t>::max() / element_bytes) {
        return false;
    }
    // Prove both the OIR element offset and the eventual e32 byte offset.
    const auto byte_stride = magnitude * element_bytes;
    return byte_stride <=
           static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) / maximum_lane;
}

oir::Value *phi_incoming_for(const oir::PhiInst &phi, const oir::BasicBlock *block) {
    for (const auto &[value, from] : phi.incoming()) {
        if (from == block) {
            return value;
        }
    }
    return nullptr;
}

bool replace_phi_incoming_value(oir::PhiInst &phi, const oir::BasicBlock *block,
                                oir::Value *expected, oir::Value *replacement) {
    bool replaced = false;
    for (std::size_t index = 0; index < phi.incoming().size(); ++index) {
        const auto &[value, from] = phi.incoming()[index];
        if (from != block) {
            continue;
        }
        if (replaced || value != expected) {
            return false;
        }
        phi.set_operand(index * 2, replacement);
        replaced = true;
    }
    return replaced;
}

bool value_is_defined_in_loop(const oir::Value *value, const oir::Loop &loop) {
    const auto *instruction = dynamic_cast<const oir::Instruction *>(value);
    return instruction != nullptr && loop_contains(loop, instruction->parent());
}

bool is_supported_widen_binary(oir::Instruction::OpID op) {
    switch (op) {
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::Mul:
    case oir::Instruction::OpID::SDiv:
    case oir::Instruction::OpID::SRem:
    case oir::Instruction::OpID::FAdd:
    case oir::Instruction::OpID::FSub:
    case oir::Instruction::OpID::FMul:
    case oir::Instruction::OpID::FDiv:
    case oir::Instruction::OpID::And:
    case oir::Instruction::OpID::Or:
    case oir::Instruction::OpID::Xor:
        return true;
    default:
        return false;
    }
}

bool is_widenable_lane_type(const oir::Type *type) {
    const auto *integer = dynamic_cast<const oir::IntegerType *>(type);
    return type != nullptr &&
           (type->is_scalar_float() || (integer != nullptr && integer->bit_width() == 32));
}

std::string unique_value_name(const oir::Function &function, std::string base) {
    std::unordered_set<std::string> names;
    for (const auto &argument : function.args()) {
        names.insert(argument->name());
    }
    for (const auto &block : function.blocks()) {
        names.insert(block->name());
        for (const auto &instruction : block->instructions()) {
            if (!instruction->name().empty()) {
                names.insert(instruction->name());
            }
        }
    }
    if (names.find(base) == names.end()) {
        return base;
    }
    for (unsigned suffix = 0;; ++suffix) {
        auto candidate = base + "." + std::to_string(suffix);
        if (names.find(candidate) == names.end()) {
            return candidate;
        }
    }
}

oir::PhiInst *insert_header_phi(oir::BasicBlock &header, oir::Type *type, const std::string &name) {
    auto phi = std::make_unique<oir::PhiInst>(type, &header, name);
    auto *raw = phi.get();
    auto &instructions = header.instructions();
    auto position = instructions.begin();
    while (position != instructions.end() &&
           dynamic_cast<oir::PhiInst *>(position->get()) != nullptr) {
        ++position;
    }
    instructions.insert(position, std::move(phi));
    return raw;
}

bool instruction_has_use_outside(const oir::Instruction &instruction, const oir::Loop &loop) {
    for (const auto &use : instruction.uses()) {
        const auto *user = dynamic_cast<const oir::Instruction *>(use.user);
        if (user == nullptr || !loop_contains(loop, user->parent())) {
            return true;
        }
    }
    return false;
}

using CloneValueMap = std::unordered_map<const oir::Value *, oir::Value *>;
using CloneBlockMap = std::unordered_map<const oir::BasicBlock *, oir::BasicBlock *>;

oir::Value *clone_mapped_value(const oir::Value *value, const CloneValueMap &map) {
    const auto found = map.find(value);
    return found == map.end() ? const_cast<oir::Value *>(value) : found->second;
}

std::vector<oir::Value *> clone_mapped_values(const std::vector<oir::Value *> &values,
                                              const CloneValueMap &map) {
    std::vector<oir::Value *> result;
    result.reserve(values.size());
    for (auto *value : values) {
        result.push_back(clone_mapped_value(value, map));
    }
    return result;
}

std::string cloned_value_name(const oir::Value &value) {
    return value.name().empty() ? std::string() : value.name() + ".lv.slow";
}

std::unique_ptr<oir::Instruction> clone_scalar_instruction(const oir::Instruction &instruction,
                                                           const CloneValueMap &map,
                                                           oir::BasicBlock *parent) {
    if (const auto *binary = dynamic_cast<const oir::BinaryInst *>(&instruction)) {
        return std::make_unique<oir::BinaryInst>(
            binary->type(), binary->op(), clone_mapped_value(binary->lhs(), map),
            clone_mapped_value(binary->rhs(), map), parent, cloned_value_name(instruction));
    }
    if (const auto *compare = dynamic_cast<const oir::CmpInst *>(&instruction)) {
        return std::make_unique<oir::CmpInst>(compare->type(), compare->op(), compare->pred(),
                                              clone_mapped_value(compare->lhs(), map),
                                              clone_mapped_value(compare->rhs(), map), parent,
                                              cloned_value_name(instruction));
    }
    if (const auto *cast = dynamic_cast<const oir::CastInst *>(&instruction)) {
        return std::make_unique<oir::CastInst>(cast->type(), cast->op(),
                                               clone_mapped_value(cast->src(), map), parent,
                                               cloned_value_name(instruction));
    }
    if (const auto *gep = dynamic_cast<const oir::GetElementPtrInst *>(&instruction)) {
        return std::make_unique<oir::GetElementPtrInst>(
            gep->type(), clone_mapped_value(gep->base_ptr(), map),
            clone_mapped_values(gep->indices(), map), parent, cloned_value_name(instruction));
    }
    if (const auto *load = dynamic_cast<const oir::LoadInst *>(&instruction)) {
        return std::make_unique<oir::LoadInst>(load->type(), clone_mapped_value(load->ptr(), map),
                                               parent, cloned_value_name(instruction));
    }
    if (const auto *store = dynamic_cast<const oir::StoreInst *>(&instruction)) {
        return std::make_unique<oir::StoreInst>(store->type(),
                                                clone_mapped_value(store->value(), map),
                                                clone_mapped_value(store->ptr(), map), parent);
    }
    if (const auto *branch = dynamic_cast<const oir::BranchInst *>(&instruction)) {
        if (branch->is_conditional()) {
            return std::make_unique<oir::BranchInst>(
                branch->type(), clone_mapped_value(branch->cond(), map),
                static_cast<oir::BasicBlock *>(clone_mapped_value(branch->true_bb(), map)),
                static_cast<oir::BasicBlock *>(clone_mapped_value(branch->false_bb(), map)),
                parent);
        }
        return std::make_unique<oir::BranchInst>(
            branch->type(),
            static_cast<oir::BasicBlock *>(clone_mapped_value(branch->target_bb(), map)), parent);
    }
    if (const auto *phi = dynamic_cast<const oir::PhiInst *>(&instruction)) {
        auto clone =
            std::make_unique<oir::PhiInst>(phi->type(), parent, cloned_value_name(instruction));
        for (const auto &[value, block] : phi->incoming()) {
            clone->add_incoming(clone_mapped_value(value, map),
                                static_cast<oir::BasicBlock *>(clone_mapped_value(block, map)));
        }
        return clone;
    }
    return nullptr;
}

struct ScalarLoopClone final {
    oir::BasicBlock *header = nullptr;
    CloneValueMap values;
    CloneBlockMap blocks;
};

bool loop_liveouts_are_cloneable(const oir::Loop &loop) {
    for (const auto *block : loop.blocks) {
        for (const auto &owned : block->instructions()) {
            for (const auto &use : owned->uses()) {
                const auto *user = dynamic_cast<const oir::Instruction *>(use.user);
                if (user != nullptr && loop_contains(loop, user->parent())) {
                    continue;
                }
                const auto *phi = dynamic_cast<const oir::PhiInst *>(user);
                if (phi == nullptr || phi->parent() == nullptr ||
                    loop_contains(loop, phi->parent())) {
                    return false;
                }
                bool edge_matched = false;
                for (const auto &[value, incoming_block] : phi->incoming()) {
                    if (value == owned.get() && incoming_block == block) {
                        edge_matched = true;
                        break;
                    }
                }
                if (!edge_matched) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool clone_scalar_loop(oir::Function &function, const oir::Loop &loop, ScalarLoopClone &result) {
    std::vector<oir::BasicBlock *> originals;
    for (const auto &owned_block : function.blocks()) {
        if (loop_contains(loop, owned_block.get())) {
            originals.push_back(owned_block.get());
        }
    }
    if (originals.size() != loop.blocks.size()) {
        return false;
    }

    for (auto *block : originals) {
        auto *clone = function.create_block("lv.slow." + block->name());
        result.blocks.emplace(block, clone);
        result.values.emplace(block, clone);
        if (block == loop.header) {
            result.header = clone;
        }
    }
    for (auto *block : originals) {
        auto *clone_block = result.blocks.at(block);
        for (const auto &instruction : block->instructions()) {
            auto clone = clone_scalar_instruction(*instruction, result.values, clone_block);
            if (clone == nullptr) {
                return false;
            }
            auto *raw = clone.get();
            clone_block->append_instruction(std::move(clone));
            result.values.emplace(instruction.get(), raw);
        }
    }
    for (auto *original : originals) {
        auto *clone = result.blocks.at(original);
        for (auto &instruction : clone->instructions()) {
            for (std::size_t index = 0; index < instruction->operand_count(); ++index) {
                auto *mapped = clone_mapped_value(instruction->operand(index), result.values);
                if (mapped != instruction->operand(index)) {
                    instruction->set_operand(index, mapped);
                }
            }
        }
    }

    for (auto *original : originals) {
        auto *clone = result.blocks.at(original);
        for (auto *successor : original->successors()) {
            const auto mapped_successor = result.blocks.find(successor);
            auto *clone_successor =
                mapped_successor == result.blocks.end() ? successor : mapped_successor->second;
            oir::cfg::add_edge(clone, clone_successor);
            if (mapped_successor != result.blocks.end()) {
                continue;
            }
            for (auto &owned : successor->instructions()) {
                auto *phi = dynamic_cast<oir::PhiInst *>(owned.get());
                if (phi == nullptr) {
                    break;
                }
                for (const auto &[value, incoming_block] : phi->incoming()) {
                    if (incoming_block == original) {
                        phi->add_incoming(clone_mapped_value(value, result.values), clone);
                        break;
                    }
                }
            }
        }
    }
    return result.header != nullptr;
}

oir::Function *get_or_create_range_helper(oir::Module &module) {
    constexpr const char *helper_name = "__yoolang_ranges_disjoint";
    auto &types = module.types();
    auto *i32 = types.int32_ty();
    auto *pointer = types.ptr_ty(i32);
    std::vector<oir::Type *> arguments{pointer, i32, i32, i32, pointer, i32, i32, i32};
    auto *expected_type = types.func_ty(i32, arguments);
    if (auto *existing = module.get_function(helper_name)) {
        return existing->is_external() && existing->function_type() == expected_type ? existing
                                                                                     : nullptr;
    }
    return module.create_function(helper_name, expected_type, true);
}

struct RuntimeVersioningResult final {
    bool installed = false;
    oir::BasicBlock *slow_header = nullptr;
    std::string error;
};

RuntimeVersioningResult install_runtime_alias_versioning(oir::Module &module, const oir::Loop &loop,
                                                         VectorizationPlan &plan) {
    RuntimeVersioningResult result;
    if (plan.runtime_alias_checks.empty() || plan.preheader == nullptr || plan.header == nullptr ||
        plan.trip_count == nullptr) {
        result.error = "runtime alias versioning plan is incomplete";
        return result;
    }
    if (!loop_liveouts_are_cloneable(loop)) {
        result.error = "runtime alias versioning cannot merge a non-phi loop live-out";
        return result;
    }

    auto &function = *plan.function;
    auto *original_preheader = plan.preheader;
    auto *original_branch = dynamic_cast<oir::BranchInst *>(original_preheader->terminator());
    if (original_branch == nullptr ||
        (original_branch->is_conditional() ? original_branch->true_bb() != plan.header &&
                                                 original_branch->false_bb() != plan.header
                                           : original_branch->target_bb() != plan.header)) {
        result.error = "runtime alias versioning lost the unique incoming loop edge";
        return result;
    }

    ScalarLoopClone slow;
    if (!clone_scalar_loop(function, loop, slow)) {
        result.error = "runtime alias versioning could not clone the scalar fallback";
        return result;
    }
    auto *helper = get_or_create_range_helper(module);
    if (helper == nullptr) {
        result.error = "runtime alias helper declaration has an incompatible type";
        return result;
    }

    auto *check_block = function.create_block("lv.alias.check");
    auto *fast_preheader = function.create_block("lv.alias.fast");
    auto *slow_preheader = function.create_block("lv.alias.slow");
    auto *i32 = module.types().int32_ty();
    oir::Value *all_disjoint = nullptr;
    for (std::size_t index = 0; index < plan.runtime_alias_checks.size(); ++index) {
        const auto &check = plan.runtime_alias_checks[index];
        std::vector<oir::Value *> arguments{
            check.lhs.base,
            plan.trip_count,
            module.create_i32(check.lhs.stride_elements),
            module.create_i32(check.lhs.element_bytes),
            check.rhs.base,
            plan.trip_count,
            module.create_i32(check.rhs.stride_elements),
            module.create_i32(check.rhs.element_bytes),
        };
        auto *call = static_cast<oir::CallInst *>(
            check_block->append_instruction(std::make_unique<oir::CallInst>(
                i32, helper, arguments, check_block,
                unique_value_name(function, "lv.alias.pair." + std::to_string(index)))));
        if (all_disjoint == nullptr) {
            all_disjoint = call;
        } else {
            all_disjoint = check_block->append_instruction(std::make_unique<oir::BinaryInst>(
                i32, oir::Instruction::OpID::And, all_disjoint, call, check_block,
                unique_value_name(function, "lv.alias.all.disjoint")));
        }
    }
    auto *guard =
        static_cast<oir::CmpInst *>(check_block->append_instruction(std::make_unique<oir::CmpInst>(
            module.types().int1_ty(), oir::Instruction::OpID::ICmp, oir::CmpPred::NE, all_disjoint,
            module.create_i32(0), check_block,
            unique_value_name(function, "lv.alias.fast.guard"))));

    if (!oir::cfg::replace_branch_target(*original_branch, plan.header, check_block)) {
        result.error = "runtime alias versioning could not split the incoming loop edge";
        return result;
    }
    oir::cfg::remove_edge_no_phi_update(original_preheader, plan.header);
    oir::cfg::add_edge(original_preheader, check_block);
    oir::cfg::replace_phi_incoming_block(plan.header, original_preheader, fast_preheader);
    oir::cfg::replace_phi_incoming_block(slow.header, original_preheader, slow_preheader);
    oir::cfg::append_conditional_branch(module, check_block, guard, fast_preheader, slow_preheader);
    oir::cfg::append_unconditional_branch(module, fast_preheader, plan.header);
    oir::cfg::append_unconditional_branch(module, slow_preheader, slow.header);

    plan.preheader = fast_preheader;
    result.installed = true;
    result.slow_header = slow.header;
    return result;
}

class ModuleTextTransaction final {
  public:
    explicit ModuleTextTransaction(oir::Module &module)
        : module_(&module), snapshot_(module.print()) {
    }

    ModuleTextTransaction(const ModuleTextTransaction &) = delete;
    ModuleTextTransaction &operator=(const ModuleTextTransaction &) = delete;

    ~ModuleTextTransaction() {
        if (committed_) {
            return;
        }
        auto restored = oir::OIRParser::parse(snapshot_, "<lv1-rollback>");
        if (!restored.ok() || restored.module == nullptr) {
            std::terminate();
        }
        try {
            module_->replace_with(std::move(*restored.module));
        } catch (...) {
            std::terminate();
        }
    }

    void commit() {
        committed_ = true;
    }

  private:
    oir::Module *module_ = nullptr;
    std::string snapshot_;
    bool committed_ = false;
};

std::optional<std::string> interleave_factor_two_recipe_rejection(const VectorizationPlan &plan) {
    if (!plan.runtime_alias_checks.empty() || plan.choice.requires_runtime_alias_check) {
        return "runtime alias versioning has a scalar fallback and is not a two-chunk recipe";
    }
    if (!plan.integer_reductions.empty()) {
        return "loop-carried reductions are not proven for two independent chunks";
    }
    if (plan.if_conversion.enabled()) {
        return "diamond if-conversion is not proven for two independent chunks";
    }
    if (plan.induction_exit_phi != nullptr) {
        return "the scalar induction has a loop live-out not proven for factor two";
    }
    const auto non_active_predicate = [](const PredicatedScalarInstruction &step) {
        return step.predicate != LanePredicate::Active;
    };
    if (std::any_of(plan.scalar_instructions_to_widen.begin(),
                    plan.scalar_instructions_to_widen.end(), non_active_predicate) ||
        std::any_of(plan.post_merge_instructions_to_widen.begin(),
                    plan.post_merge_instructions_to_widen.end(), non_active_predicate)) {
        return "predicated lane regions are not proven for factor two";
    }
    if (!plan.post_merge_instructions_to_widen.empty()) {
        return "post-merge lane operations are not a simple two-chunk recipe";
    }
    if (plan.scalar_instructions_to_widen.empty()) {
        return "the loop has no independent lane operations to duplicate";
    }
    return std::nullopt;
}

} // namespace

LoopAccessInfo LoopAccessAnalysis::analyze(const oir::Loop &loop, const oir::ScalarEvolution &scev,
                                           const oir::OIRAliasAnalysis &alias_analysis) const {
    LoopAccessInfo result;
    for (const auto *block : loop.blocks) {
        for (const auto &owned_instruction : block->instructions()) {
            const auto *instruction = owned_instruction.get();
            const oir::Value *pointer = nullptr;
            bool is_write = false;
            if (const auto *load = dynamic_cast<const oir::LoadInst *>(instruction)) {
                pointer = load->ptr();
            } else if (const auto *store = dynamic_cast<const oir::StoreInst *>(instruction)) {
                pointer = store->ptr();
                is_write = true;
            } else if (dynamic_cast<const oir::MemZeroInst *>(instruction) != nullptr) {
                result.has_loop_carried_dependence = true;
                result.explanation = "memzero in loop is not a lane-wise memory operation";
                return result;
            } else {
                continue;
            }
            result.accesses.push_back({instruction, pointer,
                                       underlying_object(pointer, loop, alias_analysis), is_write,
                                       pointer_stride(pointer, loop, scev)});
        }
    }

    for (std::size_t lhs_index = 0; lhs_index < result.accesses.size(); ++lhs_index) {
        for (std::size_t rhs_index = lhs_index + 1; rhs_index < result.accesses.size();
             ++rhs_index) {
            const auto &lhs = result.accesses[lhs_index];
            const auto &rhs = result.accesses[rhs_index];
            if (!lhs.is_write && !rhs.is_write) {
                continue;
            }
            if (alias_analysis.alias(lhs.pointer, rhs.pointer) == oir::AliasResult::NoAlias ||
                (lhs.underlying_object != nullptr && rhs.underlying_object != nullptr &&
                 alias_analysis.alias(lhs.underlying_object, rhs.underlying_object) ==
                     oir::AliasResult::NoAlias)) {
                continue;
            }

            // A load/update/store to the exact same affine lane address is not
            // loop-carried when the address advances each iteration.  The
            // vector plan preserves instruction order for that lane.
            if (same_lane_address(lhs, rhs) && lhs.stride_elements && rhs.stride_elements &&
                *lhs.stride_elements == *rhs.stride_elements && *lhs.stride_elements != 0) {
                continue;
            }

            if (lhs.underlying_object != nullptr &&
                lhs.underlying_object == rhs.underlying_object) {
                result.has_loop_carried_dependence = true;
                result.explanation = "potential loop-carried dependence within one object";
                return result;
            }

            result.may_alias_pairs.emplace_back(lhs_index, rhs_index);
            result.requires_runtime_alias_check = true;
        }
    }
    return result;
}

LoopVectorizationLegality::LoopVectorizationLegality(LegalityOptions options) : options_(options) {
}

LegalityResult
LoopVectorizationLegality::analyze(const oir::Function &function, const oir::Loop &loop,
                                   const oir::LoopInfo &loop_info, const oir::ScalarEvolution &scev,
                                   const oir::OIRAliasAnalysis &alias_analysis) const {
    (void)function;
    (void)loop_info;
    LegalityResult result;
    result.constant_trip_count = scev.constant_trip_count(loop);

    if (loop.header == nullptr || loop.latches.empty()) {
        result.code = RemarkCode::RejectNonCanonicalLoop;
        result.explanation = "loop has no header or latch";
        return result;
    }

    unsigned exiting_blocks = 0;
    for (const auto *block : loop.blocks) {
        bool exits = false;
        for (const auto *successor : block->successors()) {
            exits |= !loop_contains(loop, successor);
        }
        exiting_blocks += exits ? 1U : 0U;
    }
    if (exiting_blocks != 1) {
        result.code = RemarkCode::RejectEarlyExit;
        result.explanation = "loop must have exactly one exiting block";
        return result;
    }

    for (const auto &owned_instruction : loop.header->instructions()) {
        const auto *phi = dynamic_cast<const oir::PhiInst *>(owned_instruction.get());
        if (phi == nullptr || !phi->type()->is_scalar_integer()) {
            continue;
        }
        const auto step = addrec_step(scev.expression_for(phi, &loop));
        if (step && *step != 0) {
            result.canonical_induction = phi;
            result.induction_step = *step;
            break;
        }
    }
    if (result.canonical_induction == nullptr) {
        result.code = RemarkCode::RejectNonCanonicalLoop;
        result.explanation = "loop has no canonical integer induction variable";
        return result;
    }
    if (!is_supported_constant_stride(result.induction_step)) {
        result.code = RemarkCode::RejectStride;
        result.explanation = "LV3 supports only compile-time induction strides +/-1, +/-2, or +/-4";
        return result;
    }

    for (const auto *block : loop.blocks) {
        for (const auto &owned_instruction : block->instructions()) {
            const auto &instruction = *owned_instruction;
            ++result.scalar_instruction_count;
            result.contains_float |= instruction_has_float_value(instruction);

            if (dynamic_cast<const oir::CallInst *>(&instruction) != nullptr) {
                result.code = RemarkCode::RejectCall;
                result.explanation = "loop contains a call without a vector mapping";
                return result;
            }
            if ((instruction.op() == oir::Instruction::OpID::SDiv ||
                 instruction.op() == oir::Instruction::OpID::SRem) &&
                !divisor_is_proven_nonzero(instruction)) {
                result.code = RemarkCode::RejectPotentialTrap;
                result.explanation = "integer division or remainder may divide by zero";
                return result;
            }
            if (const auto *phi = dynamic_cast<const oir::PhiInst *>(&instruction)) {
                const bool loop_carried = phi->parent() == loop.header;
                if (loop_carried && phi != result.canonical_induction &&
                    options_.strict_floating_point && is_float_reduction(*phi, loop)) {
                    result.code = RemarkCode::RejectFPOrder;
                    result.explanation = "strict floating-point reduction cannot be reassociated";
                    return result;
                }
                if (loop_carried && phi != result.canonical_induction &&
                    phi->type()->is_scalar_numeric()) {
                    if (auto reduction = match_integer_reduction(*phi, loop);
                        reduction.has_value()) {
                        result.reduction_operation_count +=
                            static_cast<unsigned>(reduction->chain_updates.size());
                        continue;
                    }
                    result.code = RemarkCode::RejectReduction;
                    result.explanation =
                        "loop-carried value is not a supported associative i32 reduction";
                    return result;
                }
            }

            if (instruction.type() != nullptr && !instruction.type()->is_void() &&
                !instruction.type()->is_pointer() && !instruction.type()->is_label() &&
                !is_supported_numeric_type(instruction.type())) {
                result.code = RemarkCode::RejectUnsupportedType;
                result.explanation = "loop contains an unsupported scalar value type";
                return result;
            }
        }
    }

    result.memory = LoopAccessAnalysis{}.analyze(loop, scev, alias_analysis);
    if (result.memory.has_loop_carried_dependence) {
        result.code = RemarkCode::RejectDependence;
        result.explanation = result.memory.explanation;
        return result;
    }
    if (result.memory.requires_runtime_alias_check) {
        if (!options_.allow_runtime_alias_checks) {
            result.code = RemarkCode::RejectAlias;
            result.explanation =
                "unknown alias requires overflow-safe loop versioning, which LV1 does not claim";
            return result;
        }
        if (std::any_of(result.memory.accesses.begin(), result.memory.accesses.end(),
                        [](const MemoryAccess &access) {
                            return !access.stride_elements ||
                                   !is_supported_memory_stride(*access.stride_elements,
                                                               access.is_write);
                        })) {
            // Preserve the stable alias refusal for loops for which no
            // complete range description can even be formed.  Enabling the
            // versioning feature is not permission to reclassify an unknown
            // pair as an ordinary stride miss.
            result.code = RemarkCode::RejectAlias;
            result.explanation =
                "unknown alias requires overflow-safe loop versioning, which LV1 does not claim";
            return result;
        }
    }
    for (const auto &access : result.memory.accesses) {
        if (!access.stride_elements ||
            !is_supported_memory_stride(*access.stride_elements, access.is_write)) {
            result.code = RemarkCode::RejectStride;
            result.explanation =
                "LV3 memory access needs a proven load stride 0, +/-1, +/-2, or +/-4; stores "
                "require a nonzero injective stride";
            return result;
        }
    }

    result.legal = true;
    result.code = RemarkCode::Candidate;
    result.explanation = "loop is legal for a signed constant-stride scalable VLA plan";
    return result;
}

RVVCostModel::RVVCostModel(target::TargetProfile target, CostModelOptions options)
    : target_(std::move(target)), options_(options) {
}

PlanResult RVVCostModel::choose(const LegalityResult &legality, bool interleave_factor_two_legal,
                                std::string interleave_factor_two_rejection) const {
    PlanResult result;
    if (!legality.legal) {
        result.code = legality.code;
        result.explanation = legality.explanation;
        return result;
    }

    RVVLoopCostInput input;
    input.element_bits = 32;
    input.element_is_float = legality.contains_float;
    if (legality.constant_trip_count && *legality.constant_trip_count >= 0) {
        input.trip_count_known = true;
        input.trip_count = static_cast<std::uint64_t>(*legality.constant_trip_count);
    }
    input.reduction_operations = legality.reduction_operation_count;
    const auto memory_count = static_cast<unsigned>(std::min<std::size_t>(
        legality.memory.accesses.size(), std::numeric_limits<unsigned>::max()));
    auto ordinary_operations = legality.scalar_instruction_count;
    ordinary_operations -= std::min(memory_count, ordinary_operations);
    ordinary_operations -= std::min(legality.reduction_operation_count, ordinary_operations);
    input.scalar_alu_operations = ordinary_operations;
    input.vector_alu_operations = input.scalar_alu_operations;
    input.live_vector_values = std::max(2U, legality.scalar_instruction_count / 2U);
    input.needs_tail_mask = legality.needs_predication;
    input.runtime_alias_checks = static_cast<unsigned>(std::min<std::size_t>(
        legality.memory.may_alias_pairs.size(), std::numeric_limits<unsigned>::max()));
    input.scalar_loop_code_bytes =
        static_cast<std::uint64_t>(legality.scalar_instruction_count) * 4U;
    input.interleave_factor_two_legal = interleave_factor_two_legal;
    input.interleave_factor_two_rejection = std::move(interleave_factor_two_rejection);

    std::unordered_set<std::int64_t> indexed_strides;
    for (const auto &access : legality.memory.accesses) {
        RVVMemoryCostOp operation;
        operation.is_store = access.is_write;
        operation.kind = access.stride_elements && *access.stride_elements == 1
                             ? RVVMemoryAccessKind::UnitStride
                             : RVVMemoryAccessKind::Indexed;
        input.memory_operations.push_back(operation);
        if (operation.kind == RVVMemoryAccessKind::Indexed && access.stride_elements) {
            indexed_strides.insert(*access.stride_elements);
        }
    }
    input.distinct_index_vectors = static_cast<unsigned>(
        std::min<std::size_t>(indexed_strides.size(), std::numeric_limits<unsigned>::max()));

    RVVTargetCostModelOptions model_options;
    model_options.force = options_.force;
    model_options.expected_trip_count = options_.expected_trip_count;
    model_options.explore_interleave = options_.explore_interleave;
    model_options.requested_max_interleave = 4;
    const auto decision = RVVTargetCostModel(target_, model_options).choose(input);

    result.choice.scalable = true;
    result.choice.minimum_lanes = decision.selected.minimum_lanes;
    result.choice.lmul = decision.selected.lmul;
    result.choice.interleave = decision.selected.interleave;
    result.choice.estimated_scalar_cost = decision.estimated_scalar_cost;
    result.choice.estimated_vector_cost = decision.selected.total_cost;
    result.choice.estimated_vector_registers = decision.selected.estimated_vector_registers;
    result.choice.predicted_spill_registers = decision.selected.predicted_spill_registers;
    result.choice.interleave_overlap_credit = decision.selected.interleave_overlap_credit;
    result.choice.estimated_code_bytes = decision.selected.estimated_code_bytes;
    result.choice.break_even_trip_count = decision.selected.break_even_trip_count;
    result.choice.tuning = target_.tune;
    result.choice.interleave_capability_gate = decision.interleave_capability_gate;
    result.choice.requires_runtime_alias_check = legality.memory.requires_runtime_alias_check;
    result.choice.uses_mask = legality.needs_predication;

    result.explanation = decision.explanation;
    if (!decision.interleave_capability_gate.empty()) {
        result.explanation += "; " + decision.interleave_capability_gate;
    }
    if (!decision.profitable) {
        switch (decision.reject_reason) {
        case RVVCostRejectReason::TargetUnsupported:
            result.code = RemarkCode::RejectTargetFeature;
            break;
        case RVVCostRejectReason::NoLegalLMUL:
            result.code = RemarkCode::RejectRegisterPressure;
            break;
        case RVVCostRejectReason::InvalidInput:
            result.code = RemarkCode::RejectUnsupportedType;
            break;
        case RVVCostRejectReason::ShortTrip:
        case RVVCostRejectReason::NotProfitable:
        case RVVCostRejectReason::None:
            result.code = RemarkCode::RejectCost;
            break;
        }
        return result;
    }
    result.profitable = true;
    result.code = RemarkCode::Candidate;
    return result;
}

PlanBuildResult VectorizationPlanner::build(oir::Function &function, const oir::Loop &loop,
                                            const LegalityResult &legality,
                                            const PlanChoice &choice) const {
    PlanBuildResult result;
    result.plan.function = &function;
    result.plan.choice = choice;
    result.plan.element_count = oir::ElementCount::get_scalable(std::max(1U, choice.minimum_lanes));
    result.plan.configuration_element_type = function.parent()->types().int32_ty();

    auto reject = [&](RemarkCode code, std::string explanation) {
        result.code = code;
        result.explanation = std::move(explanation);
        return result;
    };
    if (!legality.legal || legality.canonical_induction == nullptr) {
        return reject(legality.code, legality.explanation);
    }
    if (choice.interleave != 1 && choice.interleave != 2) {
        return reject(RemarkCode::RejectCost,
                      "OIR VLA transform supports only interleave factors one and two");
    }
    for (const auto &access : legality.memory.accesses) {
        if (access.stride_elements && *access.stride_elements < 0 &&
            !reverse_index_offset_fits_i32(*access.stride_elements, choice.minimum_lanes)) {
            return reject(RemarkCode::RejectStride,
                          "LV4 cannot prove that the reverse indexed-memory offset fits i32 "
                          "for every architectural RVV VLEN");
        }
    }
    if (loop.header == nullptr || loop.latches.size() != 1) {
        return reject(RemarkCode::RejectNonCanonicalLoop, "LV1 requires one canonical loop latch");
    }

    auto *header = const_cast<oir::BasicBlock *>(loop.header);
    const bool rotated_single_block =
        loop.blocks.size() == 1 && loop.latches.front() == loop.header;
    const bool two_block = loop.blocks.size() == 2 && loop.latches.front() != loop.header;
    std::vector<oir::BasicBlock *> outside_predecessors;
    for (auto *predecessor : header->predecessors()) {
        if (!loop_contains(loop, predecessor)) {
            outside_predecessors.push_back(predecessor);
        }
    }
    if (outside_predecessors.size() != 1) {
        return reject(RemarkCode::RejectNonCanonicalLoop,
                      "LV1 requires one dedicated loop preheader");
    }
    auto *preheader = outside_predecessors.front();
    const auto *preheader_branch = dynamic_cast<const oir::BranchInst *>(preheader->terminator());

    const auto *header_branch = dynamic_cast<const oir::BranchInst *>(header->terminator());
    if (preheader_branch == nullptr || header_branch == nullptr ||
        !header_branch->is_conditional()) {
        return reject(RemarkCode::RejectNonCanonicalLoop,
                      "LV1 requires canonical preheader and loop branches");
    }
    auto *latch = const_cast<oir::BasicBlock *>(loop.latches.front());
    const auto *latch_branch = dynamic_cast<const oir::BranchInst *>(latch->terminator());
    const bool rotated_guarded_diamond =
        !rotated_single_block && loop.blocks.size() >= 3 && loop.blocks.size() <= 4 &&
        preheader_branch->is_conditional() && preheader_branch->true_bb() == header &&
        !loop_contains(loop, preheader_branch->false_bb()) && latch_branch != nullptr &&
        latch_branch->is_conditional() && latch_branch->true_bb() == header &&
        latch_branch->false_bb() == preheader_branch->false_bb();
    const bool rotated_loop = rotated_single_block || rotated_guarded_diamond;
    const auto *loop_branch = rotated_guarded_diamond ? latch_branch : header_branch;
    auto *body = rotated_loop ? header : latch;
    IfConversionRegion if_conversion;
    if (rotated_single_block) {
        if (!preheader_branch->is_conditional() || preheader_branch->true_bb() != header ||
            loop_contains(loop, preheader_branch->false_bb()) ||
            header_branch->true_bb() != header || loop_contains(loop, header_branch->false_bb())) {
            return reject(RemarkCode::RejectEarlyExit,
                          "LV1 rotated loop requires true backedges and one shared exit");
        }
    } else if (rotated_guarded_diamond) {
        auto *lane_condition = dynamic_cast<oir::CmpInst *>(header_branch->cond());
        if (lane_condition == nullptr || lane_condition->parent() != body ||
            header_branch->true_bb() == header_branch->false_bb()) {
            return reject(RemarkCode::RejectNonCanonicalLoop,
                          "LV2 rotated diamond requires one scalar cmp lane branch");
        }
        for (const auto &use : lane_condition->uses()) {
            if (use.user != header_branch) {
                return reject(RemarkCode::RejectNonCanonicalLoop,
                              "LV2 rotated diamond condition may only feed its lane branch");
            }
        }
        if (body->predecessors().size() != 2 ||
            std::find(body->predecessors().begin(), body->predecessors().end(), preheader) ==
                body->predecessors().end() ||
            std::find(body->predecessors().begin(), body->predecessors().end(), latch) ==
                body->predecessors().end()) {
            return reject(RemarkCode::RejectNonCanonicalLoop,
                          "LV2 rotated diamond header requires only guard and latch predecessors");
        }

        auto resolve_arm = [&](oir::BasicBlock *successor, oir::BasicBlock *&arm,
                               oir::BasicBlock *&merge_predecessor) {
            if (successor == latch) {
                arm = nullptr;
                merge_predecessor = body;
                return true;
            }
            if (!loop_contains(loop, successor) || successor == header || successor == latch ||
                successor->predecessors().size() != 1 ||
                successor->predecessors().front() != body) {
                return false;
            }
            const auto *branch = dynamic_cast<const oir::BranchInst *>(successor->terminator());
            if (branch == nullptr || branch->is_conditional() || branch->target_bb() != latch) {
                return false;
            }
            arm = successor;
            merge_predecessor = successor;
            return true;
        };

        oir::BasicBlock *then_block = nullptr;
        oir::BasicBlock *else_block = nullptr;
        oir::BasicBlock *true_predecessor = nullptr;
        oir::BasicBlock *false_predecessor = nullptr;
        if (!resolve_arm(header_branch->true_bb(), then_block, true_predecessor) ||
            !resolve_arm(header_branch->false_bb(), else_block, false_predecessor) ||
            true_predecessor == false_predecessor ||
            (then_block != nullptr && then_block == else_block)) {
            return reject(RemarkCode::RejectNonCanonicalLoop,
                          "LV2 rotated diamond arms must converge directly at its latch");
        }
        if (latch->predecessors().size() != 2 ||
            std::find(latch->predecessors().begin(), latch->predecessors().end(),
                      true_predecessor) == latch->predecessors().end() ||
            std::find(latch->predecessors().begin(), latch->predecessors().end(),
                      false_predecessor) == latch->predecessors().end()) {
            return reject(RemarkCode::RejectNonCanonicalLoop,
                          "LV2 rotated diamond requires exactly two merge predecessors");
        }
        std::unordered_set<const oir::BasicBlock *> expected_blocks{header, latch};
        if (then_block != nullptr) {
            expected_blocks.insert(then_block);
        }
        if (else_block != nullptr) {
            expected_blocks.insert(else_block);
        }
        if (expected_blocks.size() != loop.blocks.size() ||
            std::any_of(loop.blocks.begin(), loop.blocks.end(), [&](const oir::BasicBlock *block) {
                return expected_blocks.find(block) == expected_blocks.end();
            })) {
            return reject(RemarkCode::RejectEarlyExit,
                          "LV2 accepts only one guarded unnested rotated diamond in the loop");
        }

        if_conversion.condition_block = body;
        if_conversion.then_block = then_block;
        if_conversion.else_block = else_block;
        if_conversion.merge_block = latch;
        if_conversion.true_predecessor = true_predecessor;
        if_conversion.false_predecessor = false_predecessor;
        if_conversion.condition = lane_condition;
    } else if (two_block) {
        const auto *body_branch = dynamic_cast<const oir::BranchInst *>(body->terminator());
        const bool unconditional_preheader =
            !preheader_branch->is_conditional() && preheader_branch->target_bb() == header;
        const bool guarded_preheader =
            preheader_branch->is_conditional() &&
            ((preheader_branch->true_bb() == header) != (preheader_branch->false_bb() == header)) &&
            !loop_contains(loop, preheader_branch->true_bb() == header
                                     ? preheader_branch->false_bb()
                                     : preheader_branch->true_bb());
        if ((!unconditional_preheader && !guarded_preheader) || body_branch == nullptr ||
            body_branch->is_conditional() || body_branch->target_bb() != header ||
            header_branch->true_bb() != body || loop_contains(loop, header_branch->false_bb())) {
            return reject(RemarkCode::RejectEarlyExit,
                          "LV1 requires header true-edge to one body/latch and one exit edge");
        }
    } else {
        if (preheader_branch->is_conditional() || preheader_branch->target_bb() != header ||
            loop_contains(loop, header_branch->false_bb()) ||
            !loop_contains(loop, header_branch->true_bb())) {
            return reject(RemarkCode::RejectEarlyExit,
                          "LV2 diamond requires one header exit and one in-loop body edge");
        }
        body = header_branch->true_bb();
        if (body == header || body == latch) {
            return reject(RemarkCode::RejectNonCanonicalLoop,
                          "LV2 diamond requires a distinct condition block and latch");
        }
        const auto *body_branch = dynamic_cast<const oir::BranchInst *>(body->terminator());
        auto *lane_condition =
            body_branch == nullptr ? nullptr : dynamic_cast<oir::CmpInst *>(body_branch->cond());
        if (body_branch == nullptr || !body_branch->is_conditional() || lane_condition == nullptr ||
            lane_condition->parent() != body || body_branch->true_bb() == body_branch->false_bb()) {
            return reject(RemarkCode::RejectNonCanonicalLoop,
                          "LV2 diamond requires one scalar cmp conditional branch");
        }
        for (const auto &use : lane_condition->uses()) {
            if (use.user != body_branch) {
                return reject(RemarkCode::RejectNonCanonicalLoop,
                              "LV2 diamond condition may only feed its branch");
            }
        }
        if (body->predecessors().size() != 1 || body->predecessors().front() != header) {
            return reject(RemarkCode::RejectNonCanonicalLoop,
                          "LV2 diamond condition block must have only the loop header predecessor");
        }

        auto resolve_arm = [&](oir::BasicBlock *successor, oir::BasicBlock *&arm,
                               oir::BasicBlock *&merge_predecessor) {
            if (successor == latch) {
                arm = nullptr;
                merge_predecessor = body;
                return true;
            }
            if (!loop_contains(loop, successor) || successor == header || successor == body ||
                successor == latch || successor->predecessors().size() != 1 ||
                successor->predecessors().front() != body) {
                return false;
            }
            const auto *branch = dynamic_cast<const oir::BranchInst *>(successor->terminator());
            if (branch == nullptr || branch->is_conditional() || branch->target_bb() != latch) {
                return false;
            }
            arm = successor;
            merge_predecessor = successor;
            return true;
        };

        oir::BasicBlock *then_block = nullptr;
        oir::BasicBlock *else_block = nullptr;
        oir::BasicBlock *true_predecessor = nullptr;
        oir::BasicBlock *false_predecessor = nullptr;
        if (!resolve_arm(body_branch->true_bb(), then_block, true_predecessor) ||
            !resolve_arm(body_branch->false_bb(), else_block, false_predecessor) ||
            true_predecessor == false_predecessor ||
            (then_block != nullptr && then_block == else_block)) {
            return reject(RemarkCode::RejectNonCanonicalLoop,
                          "LV2 diamond arms must converge directly at one latch");
        }
        const auto *latch_branch = dynamic_cast<const oir::BranchInst *>(latch->terminator());
        if (latch_branch == nullptr || latch_branch->is_conditional() ||
            latch_branch->target_bb() != header || latch->predecessors().size() != 2 ||
            std::find(latch->predecessors().begin(), latch->predecessors().end(),
                      true_predecessor) == latch->predecessors().end() ||
            std::find(latch->predecessors().begin(), latch->predecessors().end(),
                      false_predecessor) == latch->predecessors().end()) {
            return reject(RemarkCode::RejectNonCanonicalLoop,
                          "LV2 diamond requires exactly two merge predecessors and one backedge");
        }
        std::unordered_set<const oir::BasicBlock *> expected_blocks{header, body, latch};
        if (then_block != nullptr) {
            expected_blocks.insert(then_block);
        }
        if (else_block != nullptr) {
            expected_blocks.insert(else_block);
        }
        if (expected_blocks.size() != loop.blocks.size() ||
            std::any_of(loop.blocks.begin(), loop.blocks.end(), [&](const oir::BasicBlock *block) {
                return expected_blocks.find(block) == expected_blocks.end();
            })) {
            return reject(RemarkCode::RejectEarlyExit,
                          "LV2 accepts only one unnested canonical diamond in the loop");
        }

        if_conversion.condition_block = body;
        if_conversion.then_block = then_block;
        if_conversion.else_block = else_block;
        if_conversion.merge_block = latch;
        if_conversion.true_predecessor = true_predecessor;
        if_conversion.false_predecessor = false_predecessor;
        if_conversion.condition = lane_condition;
    }

    auto *induction = const_cast<oir::PhiInst *>(legality.canonical_induction);
    auto *start = phi_incoming_for(*induction, preheader);
    auto *latch_value = phi_incoming_for(*induction, latch);
    auto *induction_update = dynamic_cast<oir::BinaryInst *>(latch_value);
    if (induction_update == nullptr || induction_update->parent() != latch) {
        return reject(RemarkCode::RejectNonCanonicalLoop,
                      "LV3 induction latch must be a constant add/sub recurrence");
    }
    std::optional<std::int64_t> update_step;
    if (induction_update->op() == oir::Instruction::OpID::Add) {
        if (induction_update->lhs() == induction) {
            update_step = integer_constant(induction_update->rhs());
        } else if (induction_update->rhs() == induction) {
            update_step = integer_constant(induction_update->lhs());
        }
    } else if (induction_update->op() == oir::Instruction::OpID::Sub &&
               induction_update->lhs() == induction) {
        if (auto amount = integer_constant(induction_update->rhs())) {
            update_step = -*amount;
        }
    }
    if (!update_step || *update_step != legality.induction_step ||
        !is_supported_constant_stride(*update_step)) {
        return reject(RemarkCode::RejectStride,
                      "LV3 induction update must have the proven stride +/-1, +/-2, or +/-4");
    }

    auto *condition = dynamic_cast<oir::CmpInst *>(loop_branch->cond());
    if (condition == nullptr || condition->op() != oir::Instruction::OpID::ICmp) {
        return reject(RemarkCode::RejectNonCanonicalLoop,
                      "LV3 requires an integer induction exit comparison");
    }
    oir::Value *trip_count = nullptr;
    if (*update_step > 0) {
        const auto start_constant = integer_constant(start);
        if (!start_constant || *start_constant != 0 || condition->pred() != oir::CmpPred::LT ||
            condition->lhs() != (rotated_loop ? static_cast<oir::Value *>(induction_update)
                                              : static_cast<oir::Value *>(induction)) ||
            condition->rhs()->type() != function.parent()->types().int32_ty() ||
            value_is_defined_in_loop(condition->rhs(), loop)) {
            return reject(
                RemarkCode::RejectNonCanonicalLoop,
                rotated_loop
                    ? "LV3 requires rotated forward condition 'icmp lt %iv.next, %trip_count'"
                    : "LV3 requires zero-based forward condition 'icmp lt %iv, %trip_count'");
        }
        trip_count = condition->rhs();
    } else {
        const auto *reverse_start = dynamic_cast<const oir::BinaryInst *>(start);
        if (reverse_start == nullptr || reverse_start->op() != oir::Instruction::OpID::Sub ||
            reverse_start->parent() != preheader || !integer_constant(reverse_start->rhs()) ||
            *integer_constant(reverse_start->rhs()) != 1 ||
            reverse_start->lhs()->type() != function.parent()->types().int32_ty() ||
            value_is_defined_in_loop(reverse_start->lhs(), loop) ||
            condition->pred() != oir::CmpPred::GE ||
            condition->lhs() != (rotated_loop ? static_cast<oir::Value *>(induction_update)
                                              : static_cast<oir::Value *>(induction)) ||
            !integer_constant(condition->rhs()) || *integer_constant(condition->rhs()) != 0) {
            return reject(RemarkCode::RejectNonCanonicalLoop,
                          rotated_loop ? "LV3 requires rotated reverse start '%trip_count - 1' and "
                                         "condition 'icmp ge %iv.next, 0'"
                                       : "LV3 reverse loop requires start '%trip_count - 1' and "
                                         "condition 'icmp ge %iv, 0'");
        }
        trip_count = reverse_start->lhs();
    }
    for (const auto &use : condition->uses()) {
        if (use.user != loop_branch) {
            return reject(RemarkCode::RejectNonCanonicalLoop,
                          "LV1 loop exit condition may only feed its loop branch");
        }
    }
    oir::PhiInst *induction_exit_phi = nullptr;
    for (const auto &use : induction_update->uses()) {
        if (use.user == induction || use.user == condition) {
            continue;
        }
        auto *candidate = dynamic_cast<oir::PhiInst *>(use.user);
        auto *exit_seed = candidate == nullptr ? nullptr : phi_incoming_for(*candidate, preheader);
        const auto start_constant = integer_constant(start);
        const auto exit_seed_constant = integer_constant(exit_seed);
        const bool same_start = exit_seed == start || (start_constant && exit_seed_constant &&
                                                       *start_constant == *exit_seed_constant &&
                                                       exit_seed->type() == start->type());
        const bool canonical_rotated_exit =
            rotated_loop && candidate != nullptr &&
            candidate->parent() == loop_branch->false_bb() &&
            candidate->type() == induction->type() && candidate->incoming().size() == 2 &&
            phi_incoming_for(*candidate, latch) == induction_update && same_start &&
            induction_exit_phi == nullptr;
        if (!canonical_rotated_exit) {
            return reject(RemarkCode::RejectNonCanonicalLoop,
                          "LV1 scalar induction update has an unsupported use");
        }
        induction_exit_phi = candidate;
    }
    if (rotated_loop) {
        const auto *guard = dynamic_cast<const oir::CmpInst *>(preheader_branch->cond());
        bool canonical_guard = guard != nullptr && guard->op() == oir::Instruction::OpID::ICmp &&
                               preheader_branch->false_bb() == loop_branch->false_bb();
        if (*update_step > 0) {
            canonical_guard = canonical_guard && guard->pred() == oir::CmpPred::LT &&
                              integer_constant(guard->lhs()).has_value() &&
                              *integer_constant(guard->lhs()) == 0 && guard->rhs() == trip_count;
        } else {
            canonical_guard = canonical_guard && guard->pred() == oir::CmpPred::GE &&
                              guard->lhs() == start && integer_constant(guard->rhs()).has_value() &&
                              *integer_constant(guard->rhs()) == 0;
        }
        if (!canonical_guard) {
            return reject(RemarkCode::RejectNonCanonicalLoop,
                          *update_step > 0 ? "LV1 rotated forward loop requires guard 'icmp lt 0, "
                                             "%trip_count'"
                                           : "LV1 rotated reverse loop requires guard 'icmp ge "
                                             "%trip_count - 1, 0'");
        }
    }

    result.plan.preheader = preheader;
    result.plan.header = header;
    result.plan.body = body;
    result.plan.latch = latch;
    result.plan.exit = loop_branch->false_bb();
    result.plan.induction = induction;
    result.plan.induction_update = induction_update;
    result.plan.induction_exit_phi = induction_exit_phi;
    result.plan.loop_condition = condition;
    result.plan.trip_count = trip_count;
    result.plan.induction_step = *update_step;
    result.plan.rotated_loop = rotated_loop;
    result.plan.if_conversion = if_conversion;
    result.plan.memory_accesses = legality.memory.accesses;

    if (choice.requires_runtime_alias_check) {
        if (!legality.memory.requires_runtime_alias_check ||
            (!two_block && !rotated_single_block) || *update_step != 1) {
            return reject(RemarkCode::RejectAlias,
                          "LV1 runtime alias versioning requires a zero-based forward two-block "
                          "or guarded rotated single-block loop with unit induction stride");
        }
        if (!loop_liveouts_are_cloneable(loop)) {
            return reject(RemarkCode::RejectAlias,
                          "LV1 runtime alias versioning cannot merge a non-phi loop live-out");
        }
        auto make_range = [&](const MemoryAccess &access) -> std::optional<RuntimeAliasRange> {
            if (!access.stride_elements ||
                !is_supported_memory_stride(*access.stride_elements, access.is_write)) {
                return std::nullopt;
            }
            const auto *pointer_type =
                dynamic_cast<const oir::PointerType *>(access.pointer->type());
            if (pointer_type == nullptr ||
                pointer_type->element_type() != function.parent()->types().int32_ty()) {
                return std::nullopt;
            }
            oir::Value *base = nullptr;
            if (const auto *phi = dynamic_cast<const oir::PhiInst *>(access.pointer);
                phi != nullptr && phi->parent() == header) {
                base = phi_incoming_for(*phi, preheader);
            } else if (const auto *gep =
                           dynamic_cast<const oir::GetElementPtrInst *>(access.pointer);
                       gep != nullptr && loop_contains(loop, gep->parent())) {
                const auto indices = gep->indices();
                if (indices.size() == 1 && indices.front() == induction &&
                    !value_is_defined_in_loop(gep->base_ptr(), loop) &&
                    gep->base_ptr()->type() == access.pointer->type()) {
                    base = gep->base_ptr();
                }
            } else if (!value_is_defined_in_loop(access.pointer, loop)) {
                base = const_cast<oir::Value *>(access.pointer);
            }
            if (base == nullptr || base->type() != access.pointer->type()) {
                return std::nullopt;
            }
            return RuntimeAliasRange{base, *access.stride_elements, 4U};
        };

        for (const auto &[lhs_index, rhs_index] : legality.memory.may_alias_pairs) {
            if (lhs_index >= legality.memory.accesses.size() ||
                rhs_index >= legality.memory.accesses.size()) {
                return reject(RemarkCode::RejectAlias,
                              "LV1 runtime alias pair references an invalid memory access");
            }
            auto lhs = make_range(legality.memory.accesses[lhs_index]);
            auto rhs = make_range(legality.memory.accesses[rhs_index]);
            if (!lhs || !rhs) {
                return reject(
                    RemarkCode::RejectAlias,
                    "LV1 runtime alias versioning remains scalar: a complete overflow-safe "
                    "affine i32 byte range cannot be constructed for every pair");
            }
            result.plan.runtime_alias_checks.push_back({*lhs, *rhs});
        }
        if (result.plan.runtime_alias_checks.empty()) {
            return reject(RemarkCode::RejectAlias,
                          "LV1 runtime alias versioning has no complete unknown pair");
        }
    }
    result.plan.recipes.push_back(
        {RecipeKind::SetVectorLength, nullptr, "select runtime VL from remaining AVL"});
    result.plan.recipes.push_back(
        {RecipeKind::ActiveLaneMask, nullptr, "materialize all-true scalable active mask"});

    std::unordered_set<const oir::Instruction *> pointer_updates;
    std::unordered_set<const oir::Instruction *> reduction_updates;
    std::unordered_set<const oir::PhiInst *> supported_header_phis{induction};
    for (const auto &owned : header->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(owned.get());
        if (phi == nullptr || phi == induction) {
            continue;
        }
        if (auto reduction = match_integer_reduction(*phi, loop); reduction.has_value()) {
            auto *update = const_cast<oir::BinaryInst *>(reduction->update);
            std::vector<oir::BinaryInst *> chain_updates;
            chain_updates.reserve(reduction->chain_updates.size());
            for (auto *chain_update : reduction->chain_updates) {
                chain_updates.push_back(const_cast<oir::BinaryInst *>(chain_update));
            }
            std::vector<oir::Value *> lane_values;
            lane_values.reserve(reduction->lane_values.size());
            for (auto *lane_value : reduction->lane_values) {
                lane_values.push_back(const_cast<oir::Value *>(lane_value));
            }
            if (chain_updates.empty() || chain_updates.back() != update ||
                chain_updates.size() != lane_values.size()) {
                return reject(RemarkCode::RejectReduction,
                              "LV1 associative reduction chain changed before planning");
            }
            auto *seed = phi_incoming_for(*phi, preheader);
            for (const auto &use : phi->uses()) {
                const auto *user = dynamic_cast<const oir::Instruction *>(use.user);
                if (user != chain_updates.front() && user != nullptr &&
                    loop_contains(loop, user->parent())) {
                    return reject(
                        RemarkCode::RejectReduction,
                        "LV1 reduction phi may only feed its associative update chain in-loop");
                }
            }
            for (std::size_t index = 0; index + 1 < chain_updates.size(); ++index) {
                unsigned chain_uses = 0;
                for (const auto &use : chain_updates[index]->uses()) {
                    if (use.user == chain_updates[index + 1]) {
                        ++chain_uses;
                    } else {
                        return reject(RemarkCode::RejectReduction,
                                      "LV1 intermediate reduction update escapes its linear chain");
                    }
                }
                if (chain_uses != 1) {
                    return reject(RemarkCode::RejectReduction,
                                  "LV1 reduction chain has a missing or duplicate update edge");
                }
            }
            oir::PhiInst *rotated_exit_phi = nullptr;
            unsigned carried_uses = 0;
            for (const auto &use : update->uses()) {
                if (use.user == phi) {
                    ++carried_uses;
                    continue;
                }
                auto *candidate = dynamic_cast<oir::PhiInst *>(use.user);
                const bool canonical_rotated_exit =
                    rotated_loop && candidate != nullptr &&
                    candidate->parent() == loop_branch->false_bb() &&
                    candidate->type() == phi->type() && candidate->incoming().size() == 2 &&
                    phi_incoming_for(*candidate, latch) == update &&
                    phi_incoming_for(*candidate, preheader) == seed && rotated_exit_phi == nullptr;
                if (!canonical_rotated_exit) {
                    return reject(RemarkCode::RejectReduction,
                                  "LV1 reduction update may only feed its loop-carried phi and one "
                                  "canonical rotated exit phi");
                }
                rotated_exit_phi = candidate;
            }
            if (seed == nullptr || carried_uses != 1 || phi_incoming_for(*phi, latch) != update) {
                return reject(
                    RemarkCode::RejectReduction,
                    "LV1 reduction update must be the unique loop-carried accumulator value");
            }
            result.plan.integer_reductions.push_back({phi, update, reduction->operation,
                                                      std::move(chain_updates),
                                                      std::move(lane_values), rotated_exit_phi});
            for (auto *chain_update : result.plan.integer_reductions.back().chain_updates) {
                reduction_updates.insert(chain_update);
            }
            supported_header_phis.insert(phi);
            continue;
        }
        if (!phi->type()->is_pointer()) {
            return reject(RemarkCode::RejectReduction,
                          "LV1 rejects unsupported non-induction loop-carried phi values");
        }
        auto *initial_pointer = phi_incoming_for(*phi, preheader);
        auto *updated_pointer = phi_incoming_for(*phi, latch);
        auto *update = dynamic_cast<oir::GetElementPtrInst *>(updated_pointer);
        if (initial_pointer == nullptr || update == nullptr || update->parent() != latch ||
            update->base_ptr() != phi || update->type() != phi->type()) {
            return reject(RemarkCode::RejectStride,
                          "LV3 pointer induction must be a constant-stride GEP recurrence");
        }
        const auto indices = update->indices();
        const auto pointer_step =
            indices.size() == 1 ? integer_constant(indices.front()) : std::nullopt;
        if (!pointer_step || !is_supported_constant_stride(*pointer_step)) {
            return reject(RemarkCode::RejectStride,
                          "LV3 pointer induction GEP stride must be +/-1, +/-2, or +/-4");
        }
        for (const auto &use : update->uses()) {
            if (use.user != phi) {
                return reject(RemarkCode::RejectStride,
                              "LV1 pointer update may only feed its header phi");
            }
        }
        result.plan.pointer_inductions.push_back({phi, update, *pointer_step});
        pointer_updates.insert(update);
        supported_header_phis.insert(phi);
    }

    if (!rotated_loop) {
        for (const auto &owned : header->instructions()) {
            const auto *instruction = owned.get();
            if (const auto *phi = dynamic_cast<const oir::PhiInst *>(instruction)) {
                if (supported_header_phis.find(phi) == supported_header_phis.end()) {
                    return reject(RemarkCode::RejectReduction,
                                  "LV1 rejects unsupported loop-carried phi values");
                }
                continue;
            }
            if (instruction != condition && instruction != header->terminator()) {
                return reject(RemarkCode::RejectNonCanonicalLoop,
                              "LV1 header may contain only induction phis and its exit test");
            }
        }
    }

    std::unordered_set<const oir::PhiInst *> merge_phi_set;
    if (if_conversion.enabled()) {
        for (const auto &owned : latch->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(owned.get());
            if (phi == nullptr) {
                break;
            }
            if (!is_widenable_lane_type(phi->type()) || phi->incoming().size() != 2 ||
                phi_incoming_for(*phi, if_conversion.true_predecessor) == nullptr ||
                phi_incoming_for(*phi, if_conversion.false_predecessor) == nullptr ||
                instruction_has_use_outside(*phi, loop)) {
                return reject(RemarkCode::RejectUnsupportedType,
                              "LV2 merge phi must be a local i32/float two-arm value");
            }
            result.plan.if_conversion.merge_phis.push_back(phi);
            merge_phi_set.insert(phi);
            result.plan.recipes.push_back(
                {RecipeKind::MergeSelect, phi,
                 "replace the scalar merge phi with a same-shaped vector select"});
        }
    }

    std::vector<oir::BasicBlock *> pre_merge_blocks{body};
    if (if_conversion.then_block != nullptr) {
        pre_merge_blocks.push_back(if_conversion.then_block);
    }
    if (if_conversion.else_block != nullptr) {
        pre_merge_blocks.push_back(if_conversion.else_block);
    }
    std::vector<oir::BasicBlock *> post_merge_blocks;
    if (if_conversion.enabled()) {
        post_merge_blocks.push_back(latch);
    }
    std::vector<oir::BasicBlock *> execution_blocks = pre_merge_blocks;
    execution_blocks.insert(execution_blocks.end(), post_merge_blocks.begin(),
                            post_merge_blocks.end());

    std::unordered_set<const oir::Instruction *> scalar_addresses;
    std::unordered_set<const oir::Instruction *> scalar_address_computations;
    std::unordered_set<const oir::Instruction *> visiting_address_computations;
    std::function<bool(const oir::Value *)> prove_scalar_address_value;
    prove_scalar_address_value = [&](const oir::Value *value) {
        if (value == induction || !value_is_defined_in_loop(value, loop)) {
            return value != nullptr && value->type() == function.parent()->types().int32_ty();
        }
        const auto *binary = dynamic_cast<const oir::BinaryInst *>(value);
        if (binary == nullptr || binary->type() != function.parent()->types().int32_ty() ||
            (binary->op() != oir::Instruction::OpID::Add &&
             binary->op() != oir::Instruction::OpID::Sub &&
             binary->op() != oir::Instruction::OpID::Mul)) {
            return false;
        }
        if (scalar_address_computations.find(binary) != scalar_address_computations.end()) {
            return true;
        }
        if (!visiting_address_computations.insert(binary).second) {
            return false;
        }
        const bool proven =
            prove_scalar_address_value(binary->lhs()) && prove_scalar_address_value(binary->rhs());
        visiting_address_computations.erase(binary);
        if (proven) {
            scalar_address_computations.insert(binary);
        }
        return proven;
    };

    auto memory_stride_for = [&](const oir::Value *pointer) -> std::optional<std::int64_t> {
        std::optional<std::int64_t> stride;
        for (const auto &access : result.plan.memory_accesses) {
            if (access.pointer != pointer) {
                continue;
            }
            if (!access.stride_elements || (stride && *stride != *access.stride_elements)) {
                return std::nullopt;
            }
            stride = access.stride_elements;
        }
        return stride;
    };
    auto pointer_has_write = [&](const oir::Value *pointer) {
        return std::any_of(result.plan.memory_accesses.begin(), result.plan.memory_accesses.end(),
                           [&](const MemoryAccess &access) {
                               return access.pointer == pointer && access.is_write;
                           });
    };
    for (const auto *execution_block : execution_blocks) {
        for (const auto &owned : execution_block->instructions()) {
            const auto *gep = dynamic_cast<const oir::GetElementPtrInst *>(owned.get());
            if (gep == nullptr || pointer_updates.find(gep) != pointer_updates.end()) {
                continue;
            }
            const auto indices = gep->indices();
            const auto stride = memory_stride_for(gep);
            if (indices.empty() || !stride ||
                !is_supported_memory_stride(*stride, pointer_has_write(gep)) ||
                !prove_scalar_address_value(indices.back())) {
                return reject(RemarkCode::RejectStride,
                              "LV3 memory address needs a proven affine i32 final index");
            }
            for (std::size_t index = 0; index + 1 < indices.size(); ++index) {
                if (value_is_defined_in_loop(indices[index], loop)) {
                    return reject(RemarkCode::RejectStride,
                                  "LV1 non-lane GEP indices must be loop invariant");
                }
            }
            const auto *pointer_type = dynamic_cast<const oir::PointerType *>(gep->type());
            if (pointer_type == nullptr || !is_widenable_lane_type(pointer_type->element_type())) {
                return reject(RemarkCode::RejectUnsupportedType,
                              "LV1 memory GEP must address i32 or float elements");
            }
            for (const auto &use : gep->uses()) {
                if (dynamic_cast<const oir::LoadInst *>(use.user) == nullptr &&
                    dynamic_cast<const oir::StoreInst *>(use.user) == nullptr) {
                    return reject(RemarkCode::RejectStride,
                                  "LV1 lane address escapes a load/store recipe");
                }
            }
            scalar_addresses.insert(gep);
            result.plan.recipes.push_back(
                {RecipeKind::ScalarAddress, gep,
                 "form the scalar lane-zero base address for signed-stride VP memory"});
        }
    }

    for (const auto *computation : scalar_address_computations) {
        for (const auto &use : computation->uses()) {
            const auto *user = dynamic_cast<const oir::Instruction *>(use.user);
            if (user == nullptr ||
                (scalar_addresses.find(user) == scalar_addresses.end() &&
                 scalar_address_computations.find(user) == scalar_address_computations.end())) {
                return reject(RemarkCode::RejectStride,
                              "LV3 affine address computation has a non-address lane use");
            }
        }
    }

    auto supported_memory_pointer = [&](const oir::Value *pointer) {
        if (const auto *instruction = dynamic_cast<const oir::Instruction *>(pointer)) {
            if (scalar_addresses.find(instruction) != scalar_addresses.end()) {
                return true;
            }
        }
        const auto *phi = dynamic_cast<const oir::PhiInst *>(pointer);
        if (phi != nullptr && supported_header_phis.find(phi) != supported_header_phis.end() &&
            phi != induction) {
            return true;
        }
        const auto *pointer_type =
            pointer == nullptr ? nullptr : dynamic_cast<const oir::PointerType *>(pointer->type());
        return pointer_type != nullptr &&
               pointer_type->element_type() == function.parent()->types().int32_ty() &&
               !value_is_defined_in_loop(pointer, loop);
    };

    auto predicate_for_block = [&](const oir::BasicBlock *block) {
        if (block == if_conversion.then_block) {
            return LanePredicate::Then;
        }
        if (block == if_conversion.else_block) {
            return LanePredicate::Else;
        }
        return LanePredicate::Active;
    };

    auto instruction_memory_stride =
        [&](const oir::Instruction *instruction) -> std::optional<std::int64_t> {
        for (const auto &access : result.plan.memory_accesses) {
            if (access.instruction == instruction) {
                return access.stride_elements;
            }
        }
        return std::nullopt;
    };

    std::unordered_set<const oir::Instruction *> widen_set;
    auto collect_block = [&](oir::BasicBlock *execution_block,
                             std::vector<PredicatedScalarInstruction> &destination)
        -> std::optional<PlanBuildResult> {
        for (const auto &owned : execution_block->instructions()) {
            auto *instruction = owned.get();
            if (instruction == execution_block->terminator() || instruction == condition ||
                instruction == induction_update ||
                pointer_updates.find(instruction) != pointer_updates.end() ||
                reduction_updates.find(instruction) != reduction_updates.end() ||
                scalar_addresses.find(instruction) != scalar_addresses.end() ||
                scalar_address_computations.find(instruction) !=
                    scalar_address_computations.end()) {
                continue;
            }
            if (const auto *phi = dynamic_cast<const oir::PhiInst *>(instruction)) {
                if ((execution_block == header &&
                     supported_header_phis.find(phi) != supported_header_phis.end()) ||
                    merge_phi_set.find(phi) != merge_phi_set.end()) {
                    continue;
                }
                return reject(RemarkCode::RejectNonCanonicalLoop,
                              "LV2 arm/condition blocks may not contain additional phis");
            }
            VectorizationRecipe recipe;
            recipe.scalar_instruction = instruction;
            if (const auto *load = dynamic_cast<const oir::LoadInst *>(instruction)) {
                const auto stride = instruction_memory_stride(load);
                if (!is_widenable_lane_type(load->type()) ||
                    !supported_memory_pointer(load->ptr()) || !stride ||
                    !is_supported_memory_stride(*stride, false)) {
                    return reject(RemarkCode::RejectStride,
                                  "LV3 load lacks a supported signed constant stride");
                }
                recipe.kind = *stride == 1 ? RecipeKind::WidenLoad : RecipeKind::WidenGather;
                recipe.explanation =
                    *stride == 1
                        ? "widen forward unit-stride load to vp.load"
                        : (*stride == 0
                               ? "widen invariant-address load to indexed vp.gather with zero "
                                 "offsets"
                               : (*stride < 0 ? "widen reverse load from a chunk-low base with "
                                                "nonnegative indexed vp.gather offsets"
                                              : "widen positive-stride load to indexed vp.gather"));
                if (*stride != 1) {
                    result.plan.recipes.push_back(
                        {RecipeKind::StridedIndex, load,
                         *stride < 0 ? "bias the lane-zero pointer to the chunk-low base and "
                                       "reverse an i32-safe stepvector into injective nonnegative "
                                       "element offsets"
                                     : "scale stepvector by the positive element stride"});
                }
            } else if (const auto *store = dynamic_cast<const oir::StoreInst *>(instruction)) {
                const auto stride = instruction_memory_stride(store);
                if (!is_widenable_lane_type(store->value()->type()) ||
                    !supported_memory_pointer(store->ptr()) || !stride ||
                    !is_supported_memory_stride(*stride, true)) {
                    return reject(RemarkCode::RejectStride,
                                  "LV3 store lacks a supported signed constant stride");
                }
                recipe.kind = *stride == 1 ? RecipeKind::WidenStore : RecipeKind::WidenScatter;
                recipe.explanation =
                    *stride == 1
                        ? "widen forward unit-stride store to vp.store"
                        : (*stride < 0 ? "widen reverse store from a chunk-low base with injective "
                                         "nonnegative vp.scatter offsets"
                                       : "widen positive-stride store to indexed vp.scatter");
                if (*stride != 1) {
                    result.plan.recipes.push_back(
                        {RecipeKind::StridedIndex, store,
                         *stride < 0 ? "bias the lane-zero pointer to the chunk-low base and "
                                       "reverse an i32-safe stepvector into injective nonnegative "
                                       "element offsets"
                                     : "scale stepvector by the positive element stride"});
                }
            } else if (const auto *binary = dynamic_cast<const oir::BinaryInst *>(instruction)) {
                if (!is_supported_widen_binary(binary->op()) ||
                    !is_widenable_lane_type(binary->type())) {
                    return reject(RemarkCode::RejectUnsupportedType,
                                  "LV1 encountered an unsupported scalar binary operation");
                }
                recipe.kind = RecipeKind::WidenBinary;
                recipe.explanation = "map lane-wise scalar binary to predicated VP binary";
            } else if (dynamic_cast<const oir::CmpInst *>(instruction) != nullptr) {
                recipe.kind = RecipeKind::WidenCompare;
                recipe.explanation = "map lane-wise scalar comparison to VP mask comparison";
            } else if (dynamic_cast<const oir::CastInst *>(instruction) != nullptr) {
                recipe.kind = RecipeKind::WidenCast;
                recipe.explanation = "map lane-wise scalar conversion to vector conversion";
            } else if (dynamic_cast<const oir::CallInst *>(instruction) != nullptr) {
                return reject(RemarkCode::RejectCall, "LV1 has no vector mapping for calls");
            } else if (instruction->op() == oir::Instruction::OpID::SetVL ||
                       instruction->type()->is_vector()) {
                return reject(RemarkCode::RejectNonCanonicalLoop,
                              "loop is already vector or contains an existing setvl");
            } else {
                return reject(RemarkCode::RejectUnsupportedType,
                              "LV1 has no recipe for an instruction in the loop body");
            }
            if (!instruction->type()->is_void() &&
                instruction_has_use_outside(*instruction, loop)) {
                return reject(RemarkCode::RejectUnsupportedType,
                              "LV1 scalar lane value has an unsupported live-out use");
            }
            widen_set.insert(instruction);
            destination.push_back({instruction, predicate_for_block(execution_block)});
            result.plan.recipes.push_back(std::move(recipe));
        }
        return std::nullopt;
    };
    for (auto *execution_block : pre_merge_blocks) {
        if (auto failure =
                collect_block(execution_block, result.plan.scalar_instructions_to_widen)) {
            return *failure;
        }
    }
    for (auto *execution_block : post_merge_blocks) {
        if (auto failure =
                collect_block(execution_block, result.plan.post_merge_instructions_to_widen)) {
            return *failure;
        }
    }

    if (result.plan.scalar_instructions_to_widen.empty() &&
        result.plan.post_merge_instructions_to_widen.empty() &&
        result.plan.if_conversion.merge_phis.empty() && result.plan.integer_reductions.empty()) {
        return reject(RemarkCode::RejectUnsupportedType,
                      "LV1 loop body has no lane-wise operations to widen");
    }
    auto check_dependencies =
        [&](const PredicatedScalarInstruction &step) -> std::optional<PlanBuildResult> {
        auto *instruction = step.instruction;
        for (auto *operand : instruction->operands()) {
            const auto *definition = dynamic_cast<const oir::Instruction *>(operand);
            if (definition == nullptr || !value_is_defined_in_loop(definition, loop) ||
                definition == induction) {
                continue;
            }
            if (scalar_addresses.find(definition) != scalar_addresses.end() ||
                scalar_address_computations.find(definition) != scalar_address_computations.end() ||
                pointer_updates.find(definition) != pointer_updates.end() ||
                reduction_updates.find(definition) != reduction_updates.end()) {
                continue;
            }
            if (const auto *phi = dynamic_cast<const oir::PhiInst *>(definition);
                phi != nullptr && supported_header_phis.find(phi) != supported_header_phis.end()) {
                continue;
            }
            if (const auto *phi = dynamic_cast<const oir::PhiInst *>(definition);
                phi != nullptr && merge_phi_set.find(phi) != merge_phi_set.end()) {
                continue;
            }
            if (widen_set.find(definition) == widen_set.end()) {
                return reject(RemarkCode::RejectUnsupportedType,
                              "LV1 lane operation depends on an unsupported loop-local value");
            }
        }
        return std::nullopt;
    };
    for (const auto &step : result.plan.scalar_instructions_to_widen) {
        if (auto failure = check_dependencies(step)) {
            return *failure;
        }
    }
    for (const auto &step : result.plan.post_merge_instructions_to_widen) {
        if (auto failure = check_dependencies(step)) {
            return *failure;
        }
    }
    for (auto *phi : result.plan.if_conversion.merge_phis) {
        for (auto *predecessor :
             {if_conversion.true_predecessor, if_conversion.false_predecessor}) {
            const auto *definition =
                dynamic_cast<const oir::Instruction *>(phi_incoming_for(*phi, predecessor));
            if (definition != nullptr && value_is_defined_in_loop(definition, loop) &&
                definition != induction && widen_set.find(definition) == widen_set.end()) {
                return reject(RemarkCode::RejectUnsupportedType,
                              "LV2 merge phi incoming value has no widening recipe");
            }
        }
    }

    for (const auto &reduction : result.plan.integer_reductions) {
        for (auto *lane_value : reduction.lane_values) {
            const auto *definition = dynamic_cast<const oir::Instruction *>(lane_value);
            if (definition != nullptr && value_is_defined_in_loop(definition, loop) &&
                definition != induction && widen_set.find(definition) == widen_set.end() &&
                merge_phi_set.find(dynamic_cast<const oir::PhiInst *>(definition)) ==
                    merge_phi_set.end()) {
                return reject(RemarkCode::RejectReduction,
                              "LV1 reduction lane value has no widening recipe");
            }
        }
        result.plan.recipes.push_back(
            {RecipeKind::WidenReduction, reduction.update,
             reduction.lane_values.size() == 1
                 ? "reduce active integer lanes with the scalar accumulator as passthrough"
                 : "reduce each linear unrolled associative lane group into the scalar "
                   "accumulator"});
    }

    result.plan.recipes.push_back({RecipeKind::InductionUpdate, induction_update,
                                   "advance scalar IV by actual VL times its signed stride"});
    for (const auto &pointer : result.plan.pointer_inductions) {
        result.plan.recipes.push_back(
            {RecipeKind::PointerUpdate, pointer.update,
             "advance pointer by actual VL times its signed element stride"});
    }
    result.plan.recipes.push_back(
        {RecipeKind::RemainingUpdate, nullptr, "decrement remaining AVL by actual VL"});
    if (choice.interleave == 2) {
        if (auto rejection = interleave_factor_two_recipe_rejection(result.plan)) {
            return reject(RemarkCode::RejectCost, "factor-two VLA recipe rejected: " + *rejection);
        }
    }
    result.valid = true;
    result.code = RemarkCode::Candidate;
    const bool has_reverse_indexed_memory =
        std::any_of(result.plan.memory_accesses.begin(), result.plan.memory_accesses.end(),
                    [](const MemoryAccess &access) {
                        return access.stride_elements && *access.stride_elements < 0;
                    });
    if (!result.plan.runtime_alias_checks.empty()) {
        result.explanation =
            "constructed scalable VLA plan with overflow-safe runtime alias versioning for " +
            std::to_string(result.plan.runtime_alias_checks.size()) +
            " complete byte-range pair(s)";
    } else {
        result.explanation =
            has_reverse_indexed_memory
                ? "constructed explicit scalable VLA plan with i32-proven nonnegative "
                  "reverse-memory indices"
                : "constructed explicit scalable VLA vectorization plan";
    }
    return result;
}

LoopVectorizer::LoopVectorizer(LoopVectorizerOptions options) : options_(options) {
}

LoopVectorizerResult LoopVectorizer::run(oir::Module &module, const target::TargetProfile &target,
                                         RemarkLog &remarks) const {
    LoopVectorizerResult run_result;
    if (!options_.enabled) {
        return run_result;
    }

    std::string preverify_error;
    if (!module.verify(&preverify_error)) {
        run_result.success = false;
        run_result.message = "LV1 input module failed verification: " + preverify_error;
        return run_result;
    }

    auto add_remark = [&](const oir::Function &function, const oir::BasicBlock *header,
                          RemarkCode code, std::string explanation, const PlanChoice &choice = {}) {
        Remark remark;
        remark.vectorizer = VectorizerKind::Loop;
        remark.code = code;
        remark.function = function.name();
        remark.region = header == nullptr ? std::string() : header->name();
        remark.explanation = std::move(explanation);
        remark.plan = choice;
        remarks.add(std::move(remark));
    };

    for (std::size_t function_index = 0; function_index < module.functions().size();
         ++function_index) {
        // Runtime versioning may append the pure helper declaration and thus
        // reallocate the owning vector.  Function objects themselves remain
        // stable, so retain only the pointee across the transformation.
        auto *function_pointer = module.functions()[function_index].get();
        auto &function = *function_pointer;
        if (function.is_external()) {
            continue;
        }
        std::unordered_set<const oir::BasicBlock *> attempted_headers;
        while (true) {
            oir::DominatorTree dominators(function);
            oir::LoopInfo loop_info(function, dominators);
            const oir::Loop *next_loop = nullptr;
            for (const auto &loop : loop_info.loops()) {
                if (attempted_headers.insert(loop.header).second) {
                    next_loop = &loop;
                    break;
                }
            }
            if (next_loop == nullptr) {
                break;
            }

            oir::ScalarEvolution scev(function, loop_info);
            oir::OIRAliasAnalysis alias_analysis;
            LegalityOptions legality_options;
            legality_options.strict_floating_point = options_.strict_floating_point;
            legality_options.allow_runtime_alias_checks = options_.enable_runtime_alias_versioning;
            auto legality = LoopVectorizationLegality(legality_options)
                                .analyze(function, *next_loop, loop_info, scev, alias_analysis);
            if (!legality.legal) {
                add_remark(function, next_loop->header, legality.code, legality.explanation);
                continue;
            }

            CostModelOptions cost_options;
            cost_options.force = options_.force;
            cost_options.expected_trip_count = options_.expected_trip_count;
            cost_options.explore_interleave = options_.explore_interleave;
            VectorizationPlanner planner;
            PlanResult cost;
            PlanBuildResult plan_result;
            if (!cost_options.explore_interleave) {
                cost = RVVCostModel(target, cost_options).choose(legality);
                if (!cost.profitable) {
                    add_remark(function, next_loop->header, cost.code, cost.explanation,
                               cost.choice);
                    continue;
                }
                plan_result = planner.build(function, *next_loop, legality, cost.choice);
            } else {
                // Construct a factor-one plan before granting factor-two
                // legality.  This keeps the target model independent of OIR
                // objects and prevents force/profitability from inventing a
                // transform recipe.
                auto baseline_options = cost_options;
                baseline_options.explore_interleave = false;
                auto baseline_cost = RVVCostModel(target, baseline_options).choose(legality);
                if (baseline_cost.choice.minimum_lanes == 0) {
                    add_remark(function, next_loop->header, baseline_cost.code,
                               baseline_cost.explanation, baseline_cost.choice);
                    continue;
                }
                auto baseline_plan =
                    planner.build(function, *next_loop, legality, baseline_cost.choice);
                if (!baseline_plan.valid) {
                    if (baseline_cost.profitable) {
                        add_remark(function, next_loop->header, baseline_plan.code,
                                   baseline_plan.explanation, baseline_cost.choice);
                    } else {
                        add_remark(function, next_loop->header, baseline_cost.code,
                                   baseline_cost.explanation, baseline_cost.choice);
                    }
                    continue;
                }
                auto factor_two_rejection =
                    interleave_factor_two_recipe_rejection(baseline_plan.plan);
                cost = RVVCostModel(target, cost_options)
                           .choose(legality, !factor_two_rejection.has_value(),
                                   factor_two_rejection.value_or(std::string()));
                if (!cost.profitable) {
                    add_remark(function, next_loop->header, cost.code, cost.explanation,
                               cost.choice);
                    continue;
                }
                plan_result = planner.build(function, *next_loop, legality, cost.choice);
                if (!plan_result.valid && cost.choice.interleave == 2) {
                    // A different LMUL can tighten a reverse-index proof.  If
                    // that rare rebuild fails, recost factor one explicitly;
                    // never advertise plan2 while emitting plan1.
                    cost = RVVCostModel(target, cost_options)
                               .choose(legality, false,
                                       "the selected factor-two LMUL could not rebuild its "
                                       "validated OIR recipe");
                    if (!cost.profitable) {
                        add_remark(function, next_loop->header, cost.code, cost.explanation,
                                   cost.choice);
                        continue;
                    }
                    plan_result = planner.build(function, *next_loop, legality, cost.choice);
                }
            }
            if (!plan_result.valid) {
                add_remark(function, next_loop->header, plan_result.code, plan_result.explanation,
                           cost.choice);
                continue;
            }
            ModuleTextTransaction transaction(module);
            auto &plan = plan_result.plan;
            oir::BasicBlock *versioned_slow_header = nullptr;
            if (!plan.runtime_alias_checks.empty()) {
                auto versioning = install_runtime_alias_versioning(module, *next_loop, plan);
                if (!versioning.installed) {
                    add_remark(function, next_loop->header, RemarkCode::RejectAlias,
                               "runtime alias versioning rolled back: " + versioning.error,
                               plan.choice);
                    run_result.success = false;
                    run_result.message =
                        "LV1 runtime alias versioning failed closed and rolled back: " +
                        versioning.error;
                    return run_result;
                }
                versioned_slow_header = versioning.slow_header;
            }
            auto &types = module.types();
            const auto lanes = plan.element_count;
            auto *i32 = types.int32_ty();
            auto *mask_type = types.vector_ty(types.int1_ty(), lanes);
            auto *configuration = types.vector_ty(plan.configuration_element_type, lanes);

            const auto base_name =
                plan.induction->name().empty() ? std::string("lv") : plan.induction->name() + ".lv";
            auto append_preheader = [&](std::unique_ptr<oir::Instruction> instruction) {
                return plan.preheader->insert_before_terminator(std::move(instruction));
            };
            auto *trip_is_positive =
                static_cast<oir::CmpInst *>(append_preheader(std::make_unique<oir::CmpInst>(
                    types.int1_ty(), oir::Instruction::OpID::ICmp, oir::CmpPred::GT,
                    plan.trip_count, module.create_i32(0), plan.preheader,
                    unique_value_name(function, base_name + ".trip.positive"))));
            auto *trip_mask =
                static_cast<oir::CastInst *>(append_preheader(std::make_unique<oir::CastInst>(
                    i32, oir::Instruction::OpID::ZExt, trip_is_positive, plan.preheader,
                    unique_value_name(function, base_name + ".trip.mask"))));
            auto *nonnegative_trip =
                static_cast<oir::BinaryInst *>(append_preheader(std::make_unique<oir::BinaryInst>(
                    i32, oir::Instruction::OpID::Mul, plan.trip_count, trip_mask, plan.preheader,
                    unique_value_name(function, base_name + ".trip.nonnegative"))));
            oir::Value *initial_remaining = nonnegative_trip;
            const auto absolute_step = static_cast<std::int64_t>(
                plan.induction_step < 0 ? -plan.induction_step : plan.induction_step);
            if (absolute_step > 1) {
                auto *divisor = module.create_i32(absolute_step);
                auto *whole = static_cast<oir::BinaryInst *>(
                    append_preheader(std::make_unique<oir::BinaryInst>(
                        i32, oir::Instruction::OpID::SDiv, nonnegative_trip, divisor,
                        plan.preheader, unique_value_name(function, base_name + ".trip.whole"))));
                auto *remainder = static_cast<oir::BinaryInst *>(
                    append_preheader(std::make_unique<oir::BinaryInst>(
                        i32, oir::Instruction::OpID::SRem, nonnegative_trip, divisor,
                        plan.preheader,
                        unique_value_name(function, base_name + ".trip.remainder"))));
                auto *has_remainder =
                    static_cast<oir::CmpInst *>(append_preheader(std::make_unique<oir::CmpInst>(
                        types.int1_ty(), oir::Instruction::OpID::ICmp, oir::CmpPred::NE, remainder,
                        module.create_i32(0), plan.preheader,
                        unique_value_name(function, base_name + ".trip.has_remainder"))));
                auto *round_up =
                    static_cast<oir::CastInst *>(append_preheader(std::make_unique<oir::CastInst>(
                        i32, oir::Instruction::OpID::ZExt, has_remainder, plan.preheader,
                        unique_value_name(function, base_name + ".trip.round_up"))));
                initial_remaining = append_preheader(std::make_unique<oir::BinaryInst>(
                    i32, oir::Instruction::OpID::Add, whole, round_up, plan.preheader,
                    unique_value_name(function, base_name + ".trip.iterations")));
            }
            auto *remaining = insert_header_phi(
                *plan.header, i32, unique_value_name(function, base_name + ".remaining"));
            remaining->add_incoming(initial_remaining, plan.preheader);
            auto *loop_branch =
                dynamic_cast<oir::BranchInst *>(plan.loop_condition->parent()->terminator());
            if (loop_branch == nullptr || !loop_branch->is_conditional() ||
                loop_branch->cond() != plan.loop_condition) {
                throw std::logic_error("LV1 planner lost its canonical conditional loop branch");
            }
            if (!plan.rotated_loop) {
                auto *remaining_nonzero = static_cast<oir::CmpInst *>(
                    plan.header->insert_before_terminator(std::make_unique<oir::CmpInst>(
                        types.int1_ty(), oir::Instruction::OpID::ICmp, oir::CmpPred::NE, remaining,
                        module.create_i32(0), plan.header,
                        unique_value_name(function, base_name + ".remaining.nonzero"))));
                loop_branch->set_operand(0, remaining_nonzero);
            }

            auto append = [&](std::unique_ptr<oir::Instruction> instruction) {
                return plan.body->insert_before_terminator(std::move(instruction));
            };
            struct ChunkEmission final {
                oir::SetVLInst *actual_vl = nullptr;
                std::unordered_map<oir::BinaryInst *, oir::Value *> widened_reductions;
            };
            auto emit_chunk =
                [&](oir::Value *chunk_remaining, oir::Value *chunk_induction_base,
                    const std::unordered_map<const oir::Value *, oir::Value *> &chunk_pointer_bases)
                -> ChunkEmission {
                auto *actual_vl =
                    static_cast<oir::SetVLInst *>(append(std::make_unique<oir::SetVLInst>(
                        i32, configuration, chunk_remaining, plan.body,
                        unique_value_name(function, base_name + ".vl"))));
                auto *active_mask =
                    static_cast<oir::SplatInst *>(append(std::make_unique<oir::SplatInst>(
                        mask_type, module.create_i1(true), plan.body,
                        unique_value_name(function, base_name + ".active"))));

                std::unordered_map<const oir::Value *, oir::Value *> widened;
                std::unordered_map<const oir::GetElementPtrInst *, oir::Value *> widened_addresses;
                std::unordered_map<const oir::Value *, oir::Value *> cloned_scalar_addresses;
                std::unordered_map<std::int64_t, oir::Value *> strided_indices;
                std::unordered_map<std::int64_t, oir::Value *> reverse_base_deltas;
                std::unordered_map<const oir::Value *,
                                   std::unordered_map<std::int64_t, oir::Value *>>
                    reverse_indexed_bases;
                std::unordered_map<oir::BinaryInst *, oir::Value *> widened_reductions;
                unsigned temporary_id = 0;
                oir::Value *then_mask = nullptr;
                oir::Value *else_mask = nullptr;
                oir::Value *last_active_lane = nullptr;
                oir::Value *reversed_memory_lane = nullptr;

                auto vector_type_for = [&](oir::Type *scalar) -> oir::VectorType * {
                    if (scalar == types.int1_ty()) {
                        return mask_type;
                    }
                    if (scalar == i32 || scalar == types.float_ty()) {
                        return types.vector_ty(scalar, lanes);
                    }
                    throw std::logic_error("LV1 planner admitted an unsupported scalar lane type");
                };
                std::function<oir::Value *(oir::Value *)> widen_value;
                widen_value = [&](oir::Value *scalar) -> oir::Value * {
                    auto found = widened.find(scalar);
                    if (found != widened.end()) {
                        return found->second;
                    }
                    if (scalar == nullptr) {
                        throw std::logic_error("LV1 planner admitted a null lane value");
                    }
                    auto *vector_type = vector_type_for(scalar->type());
                    if (scalar == plan.induction) {
                        auto *base =
                            static_cast<oir::SplatInst *>(append(std::make_unique<oir::SplatInst>(
                                vector_type, chunk_induction_base, plan.body,
                                unique_value_name(function, base_name + ".iv.base"))));
                        auto *step = static_cast<oir::StepVectorInst *>(
                            append(std::make_unique<oir::StepVectorInst>(
                                vector_type, plan.body,
                                unique_value_name(function, base_name + ".lane"))));
                        oir::VPMetadata metadata{
                            active_mask, actual_vl, module.create_undef(vector_type),
                            oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic};
                        oir::Value *lane_offset = step;
                        if (plan.induction_step != 1) {
                            auto *stride_splat = static_cast<oir::SplatInst *>(
                                append(std::make_unique<oir::SplatInst>(
                                    vector_type, module.create_i32(plan.induction_step), plan.body,
                                    unique_value_name(function, base_name + ".iv.stride"))));
                            lane_offset = append(std::make_unique<oir::VPBinaryInst>(
                                vector_type, oir::Instruction::OpID::Mul, step, stride_splat,
                                metadata, plan.body,
                                unique_value_name(function, base_name + ".iv.offset")));
                        }
                        auto *lane_iv = static_cast<oir::VPBinaryInst *>(
                            append(std::make_unique<oir::VPBinaryInst>(
                                vector_type, oir::Instruction::OpID::Add, base, lane_offset,
                                metadata, plan.body,
                                unique_value_name(function, base_name + ".iv"))));
                        widened.emplace(scalar, lane_iv);
                        return lane_iv;
                    }
                    if (const auto *definition = dynamic_cast<const oir::Instruction *>(scalar)) {
                        if (loop_contains(*next_loop, definition->parent())) {
                            throw std::logic_error(
                                "LV1 planner admitted an unmapped loop-local definition");
                        }
                    }
                    auto *splat =
                        static_cast<oir::SplatInst *>(append(std::make_unique<oir::SplatInst>(
                            vector_type, scalar, plan.body,
                            unique_value_name(function, base_name + ".splat." +
                                                            std::to_string(temporary_id++)))));
                    widened.emplace(scalar, splat);
                    return splat;
                };

                std::function<oir::Value *(oir::Value *)> clone_scalar_address_value;
                clone_scalar_address_value = [&](oir::Value *value) -> oir::Value * {
                    auto found = cloned_scalar_addresses.find(value);
                    if (found != cloned_scalar_addresses.end()) {
                        return found->second;
                    }
                    if (value == plan.induction) {
                        return chunk_induction_base;
                    }
                    auto *binary = dynamic_cast<oir::BinaryInst *>(value);
                    if (binary == nullptr || !loop_contains(*next_loop, binary->parent()) ||
                        (binary->parent() == plan.body && chunk_induction_base == plan.induction)) {
                        return value;
                    }
                    auto *cloned =
                        static_cast<oir::BinaryInst *>(append(std::make_unique<oir::BinaryInst>(
                            binary->type(), binary->op(), clone_scalar_address_value(binary->lhs()),
                            clone_scalar_address_value(binary->rhs()), plan.body,
                            unique_value_name(function, binary->name() + ".ifc.addr"))));
                    cloned_scalar_addresses.emplace(value, cloned);
                    return cloned;
                };

                auto memory_pointer = [&](oir::Value *pointer) -> oir::Value * {
                    if (const auto found = chunk_pointer_bases.find(pointer);
                        found != chunk_pointer_bases.end()) {
                        return found->second;
                    }
                    auto *gep = dynamic_cast<oir::GetElementPtrInst *>(pointer);
                    if (gep == nullptr ||
                        (gep->parent() == plan.body && chunk_induction_base == plan.induction)) {
                        return pointer;
                    }
                    auto found = widened_addresses.find(gep);
                    if (found != widened_addresses.end()) {
                        return found->second;
                    }
                    auto cloned_indices = gep->indices();
                    for (auto *&index : cloned_indices) {
                        index = clone_scalar_address_value(index);
                    }
                    auto *cloned_base = gep->base_ptr();
                    if (const auto found = chunk_pointer_bases.find(cloned_base);
                        found != chunk_pointer_bases.end()) {
                        cloned_base = found->second;
                    }
                    auto *cloned = static_cast<oir::GetElementPtrInst *>(
                        append(std::make_unique<oir::GetElementPtrInst>(
                            gep->type(), cloned_base, std::move(cloned_indices), plan.body,
                            unique_value_name(function, gep->name() + ".ifc.addr"))));
                    widened_addresses.emplace(gep, cloned);
                    return cloned;
                };

                auto memory_stride = [&](const oir::Instruction *instruction) {
                    for (const auto &access : plan.memory_accesses) {
                        if (access.instruction == instruction && access.stride_elements) {
                            return *access.stride_elements;
                        }
                    }
                    throw std::logic_error("LV3 planner omitted a proven memory-access stride");
                };

                auto last_active_lane_value = [&]() -> oir::Value * {
                    if (last_active_lane == nullptr) {
                        last_active_lane = append(std::make_unique<oir::BinaryInst>(
                            i32, oir::Instruction::OpID::Sub, actual_vl, module.create_i32(1),
                            plan.body,
                            unique_value_name(function, base_name + ".memory.last.lane")));
                    }
                    return last_active_lane;
                };

                auto indexed_memory_base = [&](oir::Value *pointer,
                                               std::int64_t stride) -> oir::Value * {
                    auto *lane_zero_pointer = memory_pointer(pointer);
                    if (stride >= 0) {
                        return lane_zero_pointer;
                    }
                    auto &pointer_bases = reverse_indexed_bases[pointer];
                    if (const auto found = pointer_bases.find(stride);
                        found != pointer_bases.end()) {
                        return found->second;
                    }

                    oir::Value *delta = nullptr;
                    if (const auto found = reverse_base_deltas.find(stride);
                        found != reverse_base_deltas.end()) {
                        delta = found->second;
                    } else {
                        // Move the current scalar lane-zero address to the lowest
                        // address touched by this chunk.  For EVL zero this may
                        // form a non-dereferenced pointer, but the VP operation's
                        // zero EVL performs no memory access.
                        delta = append(std::make_unique<oir::BinaryInst>(
                            i32, oir::Instruction::OpID::Mul, last_active_lane_value(),
                            module.create_i32(stride), plan.body,
                            unique_value_name(function, base_name + ".memory.reverse.bias")));
                        reverse_base_deltas.emplace(stride, delta);
                    }
                    auto *biased = static_cast<oir::GetElementPtrInst *>(
                        append(std::make_unique<oir::GetElementPtrInst>(
                            lane_zero_pointer->type(), lane_zero_pointer,
                            std::vector<oir::Value *>{delta}, plan.body,
                            unique_value_name(function, base_name + ".memory.chunk.low.base"))));
                    pointer_bases.emplace(stride, biased);
                    return biased;
                };

                auto index_vector = [&](std::int64_t stride) -> oir::Value * {
                    auto found = strided_indices.find(stride);
                    if (found != strided_indices.end()) {
                        return found->second;
                    }
                    auto *index_type = types.vector_ty(i32, lanes);
                    oir::Value *lane = nullptr;
                    std::int64_t index_scale = stride;
                    if (stride < 0) {
                        if (reversed_memory_lane == nullptr) {
                            // With low = lane0 + (VL - 1) * stride, lane k uses
                            // ((VL - 1) - k) * abs(stride).  Every active offset
                            // is nonnegative and injective, while the lane-to-
                            // address mapping remains lane0, lane1, ... in the
                            // original scalar decreasing-address order.  Thus a
                            // scatter gains no duplicate-address ordering case.
                            auto *step = static_cast<oir::StepVectorInst *>(
                                append(std::make_unique<oir::StepVectorInst>(
                                    index_type, plan.body,
                                    unique_value_name(function, base_name + ".memory.lane"))));
                            auto *last = static_cast<oir::SplatInst *>(
                                append(std::make_unique<oir::SplatInst>(
                                    index_type, last_active_lane_value(), plan.body,
                                    unique_value_name(function,
                                                      base_name + ".memory.last.lane.splat"))));
                            oir::VPMetadata reverse_metadata{
                                active_mask, actual_vl, module.create_undef(index_type),
                                oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic};
                            reversed_memory_lane = append(std::make_unique<oir::VPBinaryInst>(
                                index_type, oir::Instruction::OpID::Sub, last, step,
                                reverse_metadata, plan.body,
                                unique_value_name(function, base_name + ".memory.reverse.lane")));
                        }
                        lane = reversed_memory_lane;
                        index_scale = -stride;
                    } else {
                        lane = append(std::make_unique<oir::StepVectorInst>(
                            index_type, plan.body,
                            unique_value_name(function, base_name + ".memory.lane")));
                    }
                    auto *stride_splat =
                        static_cast<oir::SplatInst *>(append(std::make_unique<oir::SplatInst>(
                            index_type, module.create_i32(index_scale), plan.body,
                            unique_value_name(function, base_name + ".memory.stride"))));
                    oir::VPMetadata metadata{active_mask, actual_vl,
                                             module.create_undef(index_type),
                                             oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic};
                    auto *scaled = append(std::make_unique<oir::VPBinaryInst>(
                        index_type, oir::Instruction::OpID::Mul, lane, stride_splat, metadata,
                        plan.body, unique_value_name(function, base_name + ".memory.index")));
                    strided_indices.emplace(stride, scaled);
                    return scaled;
                };

                auto ensure_if_masks = [&]() {
                    if (!plan.if_conversion.enabled() ||
                        (then_mask != nullptr && else_mask != nullptr)) {
                        return;
                    }
                    auto condition_it = widened.find(plan.if_conversion.condition);
                    if (condition_it == widened.end() ||
                        condition_it->second->type() != mask_type) {
                        throw std::logic_error(
                            "LV2 planner did not widen its scalar diamond condition first");
                    }
                    oir::VPMetadata metadata{active_mask, actual_vl, module.create_undef(mask_type),
                                             oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic};
                    then_mask = append(std::make_unique<oir::VPBinaryInst>(
                        mask_type, oir::Instruction::OpID::And, active_mask, condition_it->second,
                        metadata, plan.body,
                        unique_value_name(function, base_name + ".if.then.mask")));
                    else_mask = append(std::make_unique<oir::VPBinaryInst>(
                        mask_type, oir::Instruction::OpID::Xor, active_mask, then_mask, metadata,
                        plan.body, unique_value_name(function, base_name + ".if.else.mask")));
                };

                auto mask_for = [&](LanePredicate predicate) -> oir::Value * {
                    switch (predicate) {
                    case LanePredicate::Active:
                        return active_mask;
                    case LanePredicate::Then:
                        ensure_if_masks();
                        return then_mask;
                    case LanePredicate::Else:
                        ensure_if_masks();
                        return else_mask;
                    }
                    throw std::logic_error("LV2 encountered an invalid lane predicate");
                };

                auto widen_instruction = [&](const PredicatedScalarInstruction &step) {
                    auto *scalar_instruction = step.instruction;
                    auto *instruction_mask = mask_for(step.predicate);
                    if (auto *load = dynamic_cast<oir::LoadInst *>(scalar_instruction)) {
                        auto *result_type = vector_type_for(load->type());
                        const auto stride = memory_stride(load);
                        oir::VPMetadata metadata{
                            instruction_mask, actual_vl, module.create_undef(result_type),
                            oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic};
                        oir::Value *vector_load = nullptr;
                        if (stride == 1) {
                            vector_load = append(std::make_unique<oir::VPLoadInst>(
                                oir::Instruction::OpID::VPLoad, result_type,
                                memory_pointer(load->ptr()), 4, metadata, plan.body,
                                unique_value_name(function, load->name() + ".vp")));
                        } else {
                            vector_load = append(std::make_unique<oir::VPGatherInst>(
                                result_type, indexed_memory_base(load->ptr(), stride),
                                index_vector(stride), 4, metadata, plan.body,
                                unique_value_name(function, load->name() + ".gather")));
                        }
                        widened.emplace(load, vector_load);
                        return;
                    }
                    if (auto *binary = dynamic_cast<oir::BinaryInst *>(scalar_instruction)) {
                        auto *lhs = widen_value(binary->lhs());
                        auto *rhs = widen_value(binary->rhs());
                        auto *result_type = vector_type_for(binary->type());
                        oir::VPMetadata metadata{
                            instruction_mask, actual_vl, module.create_undef(result_type),
                            oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic};
                        auto *vector_binary = static_cast<oir::VPBinaryInst *>(
                            append(std::make_unique<oir::VPBinaryInst>(
                                result_type, binary->op(), lhs, rhs, metadata, plan.body,
                                unique_value_name(function, binary->name() + ".vp"))));
                        widened.emplace(binary, vector_binary);
                        return;
                    }
                    if (auto *compare = dynamic_cast<oir::CmpInst *>(scalar_instruction)) {
                        auto *lhs = widen_value(compare->lhs());
                        auto *rhs = widen_value(compare->rhs());
                        oir::VPMetadata metadata{
                            instruction_mask, actual_vl, module.create_undef(mask_type),
                            oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic};
                        auto *vector_compare =
                            static_cast<oir::VPCmpInst *>(append(std::make_unique<oir::VPCmpInst>(
                                mask_type, compare->op(), compare->pred(), lhs, rhs, metadata,
                                plan.body, unique_value_name(function, compare->name() + ".vp"))));
                        widened.emplace(compare, vector_compare);
                        return;
                    }
                    if (auto *cast = dynamic_cast<oir::CastInst *>(scalar_instruction)) {
                        auto *source = widen_value(cast->src());
                        auto *result_type = vector_type_for(cast->type());
                        oir::VectorCastKind kind = oir::VectorCastKind::ZExt;
                        if (cast->op() == oir::Instruction::OpID::SIToFP) {
                            kind = oir::VectorCastKind::SIToFP;
                        } else if (cast->op() == oir::Instruction::OpID::FPToSI) {
                            kind = oir::VectorCastKind::FPToSI;
                        }
                        auto *vector_cast = static_cast<oir::VectorCastInst *>(
                            append(std::make_unique<oir::VectorCastInst>(
                                result_type, kind, source, plan.body,
                                unique_value_name(function, cast->name() + ".vp"))));
                        widened.emplace(cast, vector_cast);
                        return;
                    }
                    if (auto *store = dynamic_cast<oir::StoreInst *>(scalar_instruction)) {
                        auto *value = widen_value(store->value());
                        const auto stride = memory_stride(store);
                        oir::VPMetadata metadata{instruction_mask, actual_vl, nullptr,
                                                 oir::TailPolicy::Agnostic,
                                                 oir::MaskPolicy::Agnostic};
                        if (stride == 1) {
                            append(std::make_unique<oir::VPStoreInst>(
                                oir::Instruction::OpID::VPStore, types.void_ty(), value,
                                memory_pointer(store->ptr()), 4, metadata, plan.body));
                        } else {
                            append(std::make_unique<oir::VPScatterInst>(
                                types.void_ty(), value, indexed_memory_base(store->ptr(), stride),
                                index_vector(stride), 4, metadata, plan.body));
                        }
                        return;
                    }
                    throw std::logic_error("LV1 planner emitted an unsupported widening recipe");
                };

                for (const auto &step : plan.scalar_instructions_to_widen) {
                    widen_instruction(step);
                }
                if (plan.if_conversion.enabled()) {
                    ensure_if_masks();
                    for (auto *phi : plan.if_conversion.merge_phis) {
                        auto *true_value = widen_value(
                            phi_incoming_for(*phi, plan.if_conversion.true_predecessor));
                        auto *false_value = widen_value(
                            phi_incoming_for(*phi, plan.if_conversion.false_predecessor));
                        auto *result_type = vector_type_for(phi->type());
                        auto *select = static_cast<oir::VectorSelectInst *>(
                            append(std::make_unique<oir::VectorSelectInst>(
                                result_type, then_mask, true_value, false_value, plan.body,
                                unique_value_name(function, phi->name() + ".ifc"))));
                        widened.emplace(phi, select);
                    }
                }
                for (const auto &step : plan.post_merge_instructions_to_widen) {
                    widen_instruction(step);
                }
                for (const auto &reduction : plan.integer_reductions) {
                    oir::Value *accumulator = reduction.phi;
                    for (std::size_t index = 0; index < reduction.lane_values.size(); ++index) {
                        auto *lane_value = widen_value(reduction.lane_values[index]);
                        oir::VPMetadata metadata{active_mask, actual_vl, accumulator,
                                                 oir::TailPolicy::Agnostic,
                                                 oir::MaskPolicy::Agnostic};
                        accumulator = append(std::make_unique<oir::VPReductionInst>(
                            i32, reduction_kind_for(reduction.operation), false, lane_value,
                            metadata, plan.body,
                            unique_value_name(function, reduction.update->name() + ".vp." +
                                                            std::to_string(index))));
                    }
                    widened_reductions.emplace(reduction.update, accumulator);
                }

                return {actual_vl, std::move(widened_reductions)};
            };

            std::unordered_map<const oir::Value *, oir::Value *> first_pointer_bases;
            for (const auto &pointer : plan.pointer_inductions) {
                first_pointer_bases.emplace(pointer.phi, pointer.phi);
            }
            auto first_chunk = emit_chunk(remaining, plan.induction, first_pointer_bases);
            std::vector<oir::SetVLInst *> emitted_vls{first_chunk.actual_vl};
            oir::Value *actual_vl = first_chunk.actual_vl;
            auto widened_reductions = std::move(first_chunk.widened_reductions);

            if (plan.choice.interleave == 2) {
                auto *remaining_after_first = append(std::make_unique<oir::BinaryInst>(
                    i32, oir::Instruction::OpID::Sub, remaining, first_chunk.actual_vl, plan.body,
                    unique_value_name(function, base_name + ".remaining.after.chunk0")));

                const auto induction_magnitude = static_cast<std::int64_t>(
                    plan.induction_step < 0 ? -plan.induction_step : plan.induction_step);
                oir::Value *first_induction_delta = first_chunk.actual_vl;
                if (induction_magnitude != 1) {
                    first_induction_delta = append(std::make_unique<oir::BinaryInst>(
                        i32, oir::Instruction::OpID::Mul, first_chunk.actual_vl,
                        module.create_i32(induction_magnitude), plan.body,
                        unique_value_name(function, base_name + ".chunk1.iv.delta")));
                }
                auto *second_induction_base = append(std::make_unique<oir::BinaryInst>(
                    i32,
                    plan.induction_step > 0 ? oir::Instruction::OpID::Add
                                            : oir::Instruction::OpID::Sub,
                    plan.induction, first_induction_delta, plan.body,
                    unique_value_name(function, base_name + ".chunk1.iv.base")));

                std::unordered_map<const oir::Value *, oir::Value *> second_pointer_bases;
                for (const auto &pointer : plan.pointer_inductions) {
                    oir::Value *pointer_delta = first_chunk.actual_vl;
                    if (pointer.stride_elements != 1) {
                        pointer_delta = append(std::make_unique<oir::BinaryInst>(
                            i32, oir::Instruction::OpID::Mul, first_chunk.actual_vl,
                            module.create_i32(pointer.stride_elements), plan.body,
                            unique_value_name(function, pointer.phi->name() + ".chunk1.delta")));
                    }
                    auto *second_pointer_base = append(std::make_unique<oir::GetElementPtrInst>(
                        pointer.phi->type(), pointer.phi, std::vector<oir::Value *>{pointer_delta},
                        plan.body,
                        unique_value_name(function, pointer.phi->name() + ".chunk1.base")));
                    second_pointer_bases.emplace(pointer.phi, second_pointer_base);
                }

                auto second_chunk =
                    emit_chunk(remaining_after_first, second_induction_base, second_pointer_bases);
                emitted_vls.push_back(second_chunk.actual_vl);
                if (!widened_reductions.empty() || !second_chunk.widened_reductions.empty()) {
                    throw std::logic_error(
                        "factor-two VLA recipe admitted a loop-carried reduction");
                }
                actual_vl = append(std::make_unique<oir::BinaryInst>(
                    i32, oir::Instruction::OpID::Add, first_chunk.actual_vl, second_chunk.actual_vl,
                    plan.body, unique_value_name(function, base_name + ".vl.total")));
            }

            const auto induction_magnitude = static_cast<std::int64_t>(
                plan.induction_step < 0 ? -plan.induction_step : plan.induction_step);
            oir::Value *induction_delta = actual_vl;
            oir::Instruction *induction_vl_user = nullptr;
            if (induction_magnitude != 1) {
                induction_delta = append(std::make_unique<oir::BinaryInst>(
                    i32, oir::Instruction::OpID::Mul, actual_vl,
                    module.create_i32(induction_magnitude), plan.body,
                    unique_value_name(function, base_name + ".iv.delta")));
                induction_vl_user = dynamic_cast<oir::Instruction *>(induction_delta);
            }
            auto *new_induction_update =
                static_cast<oir::BinaryInst *>(append(std::make_unique<oir::BinaryInst>(
                    i32,
                    plan.induction_step > 0 ? oir::Instruction::OpID::Add
                                            : oir::Instruction::OpID::Sub,
                    plan.induction, induction_delta, plan.body, plan.induction_update->name())));
            if (induction_vl_user == nullptr) {
                induction_vl_user = new_induction_update;
            }
            plan.induction_update->replace_all_uses_with(new_induction_update);
            if (plan.induction_exit_phi != nullptr &&
                phi_incoming_for(*plan.induction_exit_phi, plan.latch) != new_induction_update) {
                run_result.success = false;
                run_result.message =
                    "LV1 internal error: canonical induction live-out changed before "
                    "transformation";
                return run_result;
            }

            for (const auto &pointer : plan.pointer_inductions) {
                oir::Value *pointer_delta = actual_vl;
                if (pointer.stride_elements != 1) {
                    pointer_delta = append(std::make_unique<oir::BinaryInst>(
                        i32, oir::Instruction::OpID::Mul, actual_vl,
                        module.create_i32(pointer.stride_elements), plan.body,
                        unique_value_name(function, pointer.phi->name() + ".delta")));
                }
                auto *replacement = static_cast<oir::GetElementPtrInst *>(
                    append(std::make_unique<oir::GetElementPtrInst>(
                        pointer.update->type(), pointer.phi,
                        std::vector<oir::Value *>{pointer_delta}, plan.body,
                        pointer.update->name())));
                pointer.update->replace_all_uses_with(replacement);
            }
            for (const auto &reduction : plan.integer_reductions) {
                auto found = widened_reductions.find(reduction.update);
                if (found == widened_reductions.end()) {
                    throw std::logic_error("LV1 planner omitted a proven integer reduction recipe");
                }
                const bool carried_rewired = replace_phi_incoming_value(
                    *reduction.phi, plan.latch, reduction.update, found->second);
                const bool exit_rewired =
                    reduction.rotated_exit_phi == nullptr ||
                    replace_phi_incoming_value(*reduction.rotated_exit_phi, plan.latch,
                                               reduction.update, found->second);
                if (!carried_rewired || !exit_rewired || reduction.update->has_uses()) {
                    run_result.success = false;
                    run_result.message =
                        "LV1 internal error: proven reduction incoming edges changed "
                        "before transformation";
                    return run_result;
                }
            }
            auto *remaining_next =
                static_cast<oir::BinaryInst *>(append(std::make_unique<oir::BinaryInst>(
                    i32, oir::Instruction::OpID::Sub, remaining, actual_vl, plan.body,
                    unique_value_name(function, base_name + ".remaining.next"))));
            auto *remaining_predecessor =
                plan.rotated_loop && plan.if_conversion.enabled() ? plan.latch : plan.body;
            remaining->add_incoming(remaining_next, remaining_predecessor);
            if (plan.rotated_loop) {
                auto *remaining_nonzero =
                    static_cast<oir::CmpInst *>(append(std::make_unique<oir::CmpInst>(
                        types.int1_ty(), oir::Instruction::OpID::ICmp, oir::CmpPred::NE,
                        remaining_next, module.create_i32(0), plan.body,
                        unique_value_name(function, base_name + ".remaining.nonzero"))));
                loop_branch->set_operand(0, remaining_nonzero);
            }

            std::unordered_set<oir::Instruction *> erase_set;
            for (const auto &step : plan.scalar_instructions_to_widen) {
                erase_set.insert(step.instruction);
            }
            for (const auto &step : plan.post_merge_instructions_to_widen) {
                erase_set.insert(step.instruction);
            }
            for (auto *phi : plan.if_conversion.merge_phis) {
                erase_set.insert(phi);
            }
            erase_set.insert(plan.induction_update);
            for (const auto &pointer : plan.pointer_inductions) {
                erase_set.insert(pointer.update);
            }
            for (const auto &reduction : plan.integer_reductions) {
                erase_set.insert(reduction.chain_updates.begin(), reduction.chain_updates.end());
            }

            if (plan.if_conversion.enabled()) {
                auto *diamond_branch = dynamic_cast<oir::BranchInst *>(
                    plan.if_conversion.condition_block->terminator());
                if (diamond_branch == nullptr || !diamond_branch->is_conditional()) {
                    throw std::logic_error("LV2 planner lost the canonical diamond branch");
                }
                auto *true_target = diamond_branch->true_bb();
                auto *false_target = diamond_branch->false_bb();
                oir::cfg::remove_edge(plan.body, true_target);
                if (false_target != true_target) {
                    oir::cfg::remove_edge(plan.body, false_target);
                }
                if (plan.if_conversion.then_block != nullptr) {
                    oir::cfg::remove_edge(plan.if_conversion.then_block, plan.latch);
                }
                if (plan.if_conversion.else_block != nullptr) {
                    oir::cfg::remove_edge(plan.if_conversion.else_block, plan.latch);
                }
                diamond_branch->drop_all_operands();
                auto &body_instructions = plan.body->instructions();
                for (auto it = body_instructions.begin(); it != body_instructions.end(); ++it) {
                    if (it->get() == diamond_branch) {
                        body_instructions.erase(it);
                        break;
                    }
                }
                if (plan.rotated_loop) {
                    // Keep the latch as the unique loop-control block.  This
                    // preserves the guarded zero-trip edge, all exit phis and
                    // the header's IV/pointer/remaining backedge provenance.
                    oir::cfg::append_unconditional_branch(module, plan.body, plan.latch);
                } else {
                    oir::cfg::remove_edge_no_phi_update(plan.latch, plan.header);
                    oir::cfg::replace_phi_incoming_block(plan.header, plan.latch, plan.body);
                    oir::cfg::append_unconditional_branch(module, plan.body, plan.header);
                }
            }

            for (auto *instruction : erase_set) {
                instruction->drop_all_operands();
            }
            auto &body_instructions = plan.body->instructions();
            for (auto it = body_instructions.begin(); it != body_instructions.end();) {
                if (erase_set.find(it->get()) != erase_set.end()) {
                    it = body_instructions.erase(it);
                } else {
                    ++it;
                }
            }
            if (plan.rotated_loop && plan.if_conversion.enabled() && plan.latch != plan.body) {
                auto &latch_instructions = plan.latch->instructions();
                for (auto it = latch_instructions.begin(); it != latch_instructions.end();) {
                    if (erase_set.find(it->get()) != erase_set.end()) {
                        it = latch_instructions.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (plan.if_conversion.enabled()) {
                if (plan.if_conversion.then_block != nullptr) {
                    function.erase_block(plan.if_conversion.then_block);
                }
                if (plan.if_conversion.else_block != nullptr) {
                    function.erase_block(plan.if_conversion.else_block);
                }
                if (!plan.rotated_loop) {
                    function.erase_block(plan.latch);
                }
            }
            if (plan.loop_condition->has_uses()) {
                throw std::logic_error("LV1 canonical loop condition retained an unexpected use");
            }
            plan.loop_condition->drop_all_operands();
            auto &condition_instructions = plan.loop_condition->parent()->instructions();
            for (auto it = condition_instructions.begin(); it != condition_instructions.end();
                 ++it) {
                if (it->get() == plan.loop_condition) {
                    condition_instructions.erase(it);
                    break;
                }
            }

            bool every_chunk_used_as_evl = true;
            for (auto *chunk_vl : emitted_vls) {
                bool chunk_used_as_evl = false;
                for (const auto &use : chunk_vl->uses()) {
                    const auto *vp = dynamic_cast<const oir::VPInstruction *>(use.user);
                    chunk_used_as_evl |= vp != nullptr && vp->evl() == chunk_vl;
                }
                every_chunk_used_as_evl &= chunk_used_as_evl;
            }
            bool used_by_induction = false;
            bool used_by_remaining = false;
            for (const auto &use : actual_vl->uses()) {
                used_by_induction |= use.user == induction_vl_user;
                used_by_remaining |= use.user == remaining_next;
            }
            if (!every_chunk_used_as_evl || !used_by_induction || !used_by_remaining) {
                run_result.success = false;
                run_result.message =
                    "LV1 internal error: chunk VL is not wired to its EVL or total "
                    "VL is not wired to IV/remaining";
                return run_result;
            }

            std::string verification_error;
            if (!module.verify(&verification_error)) {
                run_result.success = false;
                run_result.message =
                    "LV1 produced invalid OIR and withheld VECTORIZED: " + verification_error;
                return run_result;
            }
            if (options_.post_transform_validation) {
                std::string validation_error;
                bool accepted = false;
                try {
                    accepted = options_.post_transform_validation(module, validation_error);
                } catch (const std::exception &error) {
                    validation_error = error.what();
                } catch (...) {
                    validation_error = "unknown post-transform validation exception";
                }
                if (!accepted) {
                    run_result.success = false;
                    run_result.message =
                        "LV3 post-transform validation rejected the plan and withheld "
                        "VECTORIZED: " +
                        (validation_error.empty() ? std::string("unspecified rejection")
                                                  : validation_error);
                    return run_result;
                }
            }
            transaction.commit();
            if (versioned_slow_header != nullptr) {
                attempted_headers.insert(versioned_slow_header);
            }
            const bool has_reverse_indexed_memory =
                std::any_of(plan.memory_accesses.begin(), plan.memory_accesses.end(),
                            [](const MemoryAccess &access) {
                                return access.stride_elements && *access.stride_elements < 0;
                            });
            std::string success_explanation;
            if (!plan.runtime_alias_checks.empty()) {
                success_explanation =
                    "transformed verified scalable VLA fast path with overflow-safe runtime "
                    "alias versioning for " +
                    std::to_string(plan.runtime_alias_checks.size()) +
                    " complete byte-range pair(s); preserved scalar slow path";
            } else {
                success_explanation =
                    has_reverse_indexed_memory
                        ? "transformed verified scalable VLA loop with nonnegative "
                          "reverse-memory indices"
                        : "transformed verified scalable VLA loop";
            }
            add_remark(function, plan.header, RemarkCode::Vectorized,
                       std::move(success_explanation), plan.choice);
            run_result.changed = true;
            ++run_result.loops_vectorized;
        }
    }
    return run_result;
}

} // namespace pass::oir_vectorize
