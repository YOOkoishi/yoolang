#include "pass/oir/OIRSLPVectorizer.h"

#include "oir/OIRAnalysis.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pass::oir_vectorize {
namespace {

using InstructionList = std::list<std::unique_ptr<oir::Instruction>>;

struct AddressInfo final {
    const oir::Value *base = nullptr;
    std::int64_t offset = 0;
    std::uint64_t size = 0;
};

struct PendingRemark final {
    SLPDiagnostic diagnostic;
    PlanChoice plan;
};

struct BlockStats final {
    oir::Function *function = nullptr;
    oir::BasicBlock *block = nullptr;
    unsigned packs = 0;
    unsigned replaced = 0;
    unsigned maximum_lanes = 0;
    unsigned scalar_cost = 0;
    unsigned vector_cost = 0;
};

class NameGenerator final {
  public:
    explicit NameGenerator(const oir::Function &function) {
        for (const auto &argument : function.args())
            names_.insert(argument->name());
        for (const auto &block : function.blocks()) {
            names_.insert(block->name());
            for (const auto &instruction : block->instructions()) {
                if (!instruction->name().empty())
                    names_.insert(instruction->name());
            }
        }
    }

    std::string next(std::string stem) {
        stem = "slp." + stem;
        if (names_.insert(stem).second)
            return stem;
        for (;;) {
            auto candidate = stem + "." + std::to_string(next_id_++);
            if (names_.insert(candidate).second)
                return candidate;
        }
    }

  private:
    std::unordered_set<std::string> names_;
    unsigned next_id_ = 0;
};

bool is_i32(const oir::Type *type) {
    const auto *integer = dynamic_cast<const oir::IntegerType *>(type);
    return integer != nullptr && integer->bit_width() == 32;
}

bool is_supported_scalar_type(const oir::Type *type) {
    return is_i32(type) || (type != nullptr && type->is_scalar_float());
}

bool is_supported_binary(const oir::BinaryInst &binary) {
    if (!is_supported_scalar_type(binary.type()))
        return false;
    if (binary.type()->is_scalar_float()) {
        return binary.op() == oir::Instruction::OpID::FAdd ||
               binary.op() == oir::Instruction::OpID::FSub ||
               binary.op() == oir::Instruction::OpID::FMul;
    }
    return binary.op() == oir::Instruction::OpID::Add ||
           binary.op() == oir::Instruction::OpID::Sub ||
           binary.op() == oir::Instruction::OpID::Mul ||
           binary.op() == oir::Instruction::OpID::And ||
           binary.op() == oir::Instruction::OpID::Or || binary.op() == oir::Instruction::OpID::Xor;
}

bool is_supported_compare(const oir::CmpInst &compare) {
    return is_supported_scalar_type(compare.lhs()->type()) &&
           compare.lhs()->type() == compare.rhs()->type() &&
           ((compare.op() == oir::Instruction::OpID::ICmp && is_i32(compare.lhs()->type())) ||
            (compare.op() == oir::Instruction::OpID::FCmp &&
             compare.lhs()->type()->is_scalar_float()));
}

bool is_supported_mask_binary(const oir::BinaryInst &binary) {
    const auto *integer = dynamic_cast<const oir::IntegerType *>(binary.type());
    return integer != nullptr && integer->bit_width() == 1 &&
           (binary.op() == oir::Instruction::OpID::And ||
            binary.op() == oir::Instruction::OpID::Or ||
            binary.op() == oir::Instruction::OpID::Xor);
}

bool is_slp_arithmetic_opcode(oir::Instruction::OpID op) {
    switch (op) {
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::Mul:
    case oir::Instruction::OpID::And:
    case oir::Instruction::OpID::Or:
    case oir::Instruction::OpID::Xor:
    case oir::Instruction::OpID::FAdd:
    case oir::Instruction::OpID::FSub:
    case oir::Instruction::OpID::FMul:
        return true;
    default:
        return false;
    }
}

unsigned unsupported_candidate_count(const oir::BasicBlock &block) {
    unsigned count = 0;
    for (const auto &owned : block.instructions()) {
        const auto &instruction = *owned;
        if (const auto *binary = dynamic_cast<const oir::BinaryInst *>(&instruction)) {
            count += is_slp_arithmetic_opcode(binary->op()) &&
                     !is_supported_scalar_type(binary->type()) &&
                     !is_supported_mask_binary(*binary);
        } else if (const auto *compare = dynamic_cast<const oir::CmpInst *>(&instruction)) {
            count += !is_supported_compare(*compare);
        } else if (const auto *load = dynamic_cast<const oir::LoadInst *>(&instruction)) {
            count += !is_supported_scalar_type(load->type());
        } else if (const auto *store = dynamic_cast<const oir::StoreInst *>(&instruction)) {
            count += !is_supported_scalar_type(store->value()->type());
        }
    }
    return count;
}

bool is_potentially_trapping(const oir::Instruction &instruction) {
    return instruction.op() == oir::Instruction::OpID::SDiv ||
           instruction.op() == oir::Instruction::OpID::SRem ||
           instruction.op() == oir::Instruction::OpID::FDiv;
}

bool is_plain_memory(const oir::Instruction &instruction) {
    return dynamic_cast<const oir::LoadInst *>(&instruction) != nullptr ||
           dynamic_cast<const oir::StoreInst *>(&instruction) != nullptr;
}

bool target_supports(const target::TargetProfile &target, const oir::Type *type) {
    return type != nullptr && target.supports_vector_element(type->is_scalar_float(), 32);
}

bool target_supports_mask(const target::TargetProfile &target) {
    return target.supports_vector_element(false, 32);
}

const oir::Value *pointer_chain_root(const oir::Value *pointer) {
    const auto *gep = dynamic_cast<const oir::GetElementPtrInst *>(pointer);
    return gep == nullptr ? pointer : pointer_chain_root(gep->base_ptr());
}

std::optional<AddressInfo> address_info(const oir::Value *pointer,
                                        const oir::OIRAliasAnalysis &alias_analysis) {
    const auto location = alias_analysis.memory_location(pointer);
    const auto *base = location.base != nullptr ? location.base : pointer_chain_root(pointer);
    if (base == nullptr || !location.offset || !location.size || *location.size == 0 ||
        *location.size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return AddressInfo{base, *location.offset, *location.size};
}

bool addresses_are_contiguous(const std::vector<oir::Value *> &pointers,
                              const oir::OIRAliasAnalysis &alias_analysis) {
    if (pointers.empty())
        return false;
    auto first = address_info(pointers.front(), alias_analysis);
    if (!first)
        return false;
    for (std::size_t lane = 0; lane < pointers.size(); ++lane) {
        auto current = address_info(pointers[lane], alias_analysis);
        if (!current || current->base != first->base || current->size != first->size) {
            return false;
        }
        if (lane >
            static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() / first->size)) {
            return false;
        }
        const auto delta = static_cast<std::int64_t>(lane * first->size);
        if (first->offset > std::numeric_limits<std::int64_t>::max() - delta ||
            current->offset != first->offset + delta) {
            return false;
        }
    }
    return true;
}

template <typename InstructionT, typename PointerFn>
bool instructions_are_contiguous(const std::vector<InstructionT *> &instructions,
                                 const oir::OIRAliasAnalysis &alias_analysis, PointerFn pointer) {
    std::vector<oir::Value *> pointers;
    pointers.reserve(instructions.size());
    for (auto *instruction : instructions)
        pointers.push_back(pointer(*instruction));
    return addresses_are_contiguous(pointers, alias_analysis);
}

InstructionList::iterator find_instruction(oir::BasicBlock &block,
                                           const oir::Instruction *instruction) {
    return std::find_if(block.instructions().begin(), block.instructions().end(),
                        [&](const auto &owned) { return owned.get() == instruction; });
}

template <typename InstructionT, typename... Args>
InstructionT *insert_instruction_before(oir::BasicBlock &block, const oir::Instruction *anchor,
                                        Args &&...args) {
    auto position = find_instruction(block, anchor);
    if (position == block.instructions().end())
        return nullptr;
    auto owned = std::make_unique<InstructionT>(std::forward<Args>(args)...);
    auto *raw = owned.get();
    block.instructions().insert(position, std::move(owned));
    return raw;
}

class MutationFailure final : public std::runtime_error {
  public:
    explicit MutationFailure(std::string detail)
        : std::runtime_error("SLP_MUTATION_FAILED: " + detail) {
    }

    MutationFailure(const oir::BasicBlock &block, std::string detail)
        : std::runtime_error("SLP_MUTATION_FAILED: " + detail),
          function_(block.parent() == nullptr ? std::string{} : block.parent()->name()),
          block_(block.name()) {
    }

    const std::string &function() const {
        return function_;
    }

    const std::string &block() const {
        return block_;
    }

  private:
    std::string function_;
    std::string block_;
};

class MutationJournal final {
  public:
    MutationJournal() = default;
    MutationJournal(const MutationJournal &) = delete;
    MutationJournal &operator=(const MutationJournal &) = delete;

    ~MutationJournal() {
        if (active_)
            rollback();
    }

    template <typename InstructionT, typename... Args>
    InstructionT *insert_before(oir::BasicBlock &block, const oir::Instruction *anchor,
                                Args &&...args) {
        auto *instruction =
            insert_instruction_before<InstructionT>(block, anchor, std::forward<Args>(args)...);
        if (instruction == nullptr) {
            throw MutationFailure(block, "insertion anchor is no longer in its basic block");
        }
        inserted_.push_back(instruction);
        return instruction;
    }

    void replace_all_uses(oir::Value &from, oir::Value &to) {
        const auto uses = from.uses();
        for (const auto &use : uses) {
            operand_changes_.push_back({use.user, use.operand_index, &from});
        }
        from.replace_all_uses_with(&to);
    }

    void detach(oir::BasicBlock &block, const std::vector<oir::Instruction *> &instructions) {
        for (auto *instruction : instructions) {
            auto position = find_instruction(block, instruction);
            if (position == block.instructions().end()) {
                throw MutationFailure(block, "scalar instruction disappeared before replacement");
            }
            auto next = std::next(position);
            RemovedInstruction removed;
            removed.block = &block;
            removed.next = next == block.instructions().end() ? nullptr : next->get();
            removed.operands = instruction->operands();
            instruction->drop_all_operands();
            removed.instruction = std::move(*position);
            block.instructions().erase(position);
            removed_.push_back(std::move(removed));
        }
    }

    void discard_dead_inserted_extracts() {
        for (auto *instruction : inserted_) {
            if (dynamic_cast<oir::ExtractElementInst *>(instruction) == nullptr ||
                instruction->has_uses()) {
                continue;
            }
            auto *block = instruction->parent();
            if (block == nullptr) {
                throw MutationFailure("inserted extract has no parent block");
            }
            auto position = find_instruction(*block, instruction);
            if (position == block->instructions().end()) {
                throw MutationFailure(*block, "inserted extract disappeared before cleanup");
            }
            instruction->drop_all_operands();
            discarded_inserted_.push_back(std::move(*position));
            block->instructions().erase(position);
        }
    }

    void rollback() noexcept {
        if (!active_)
            return;

        // Recreate original instructions and their then-current operands while
        // all inserted replacement values are still alive.  Reverse order
        // makes a saved `next` instruction available for adjacent removals.
        for (auto it = removed_.rbegin(); it != removed_.rend(); ++it) {
            auto position = it->next == nullptr ? it->block->instructions().end()
                                                : find_instruction(*it->block, it->next);
            auto *restored = it->instruction.get();
            it->block->instructions().insert(position, std::move(it->instruction));
            for (auto *operand : it->operands)
                restored->add_operand(operand);
        }

        for (auto it = operand_changes_.rbegin(); it != operand_changes_.rend(); ++it) {
            it->user->set_operand(it->operand_index, it->original);
        }

        for (auto it = inserted_.rbegin(); it != inserted_.rend(); ++it) {
            auto *instruction = *it;
            auto *block = instruction->parent();
            if (block == nullptr)
                continue;
            auto position = find_instruction(*block, instruction);
            if (position == block->instructions().end())
                continue;
            instruction->drop_all_operands();
            block->instructions().erase(position);
        }
        removed_.clear();
        operand_changes_.clear();
        inserted_.clear();
        discarded_inserted_.clear();
        active_ = false;
    }

    void commit() noexcept {
        if (!active_)
            return;
        removed_.clear();
        operand_changes_.clear();
        inserted_.clear();
        discarded_inserted_.clear();
        active_ = false;
    }

  private:
    struct OperandChange final {
        oir::User *user = nullptr;
        std::size_t operand_index = 0;
        oir::Value *original = nullptr;
    };

    struct RemovedInstruction final {
        oir::BasicBlock *block = nullptr;
        oir::Instruction *next = nullptr;
        std::vector<oir::Value *> operands;
        std::unique_ptr<oir::Instruction> instruction;
    };

    bool active_ = true;
    std::vector<oir::Instruction *> inserted_;
    std::vector<OperandChange> operand_changes_;
    std::vector<RemovedInstruction> removed_;
    std::vector<std::unique_ptr<oir::Instruction>> discarded_inserted_;
};

std::optional<std::uint64_t> constant_lane(const oir::Value *value) {
    const auto *constant = dynamic_cast<const oir::ConstantInt *>(value);
    if (constant == nullptr || constant->value() < 0)
        return std::nullopt;
    return static_cast<std::uint64_t>(constant->value());
}

oir::Value *reusable_vector(const std::vector<oir::Value *> &values,
                            const oir::VectorType *expected_type = nullptr) {
    if (values.empty())
        return nullptr;
    oir::Value *vector = nullptr;
    for (std::size_t lane = 0; lane < values.size(); ++lane) {
        const auto *extract = dynamic_cast<const oir::ExtractElementInst *>(values[lane]);
        if (extract == nullptr || constant_lane(extract->index()) != lane)
            return nullptr;
        if (vector == nullptr)
            vector = extract->vector();
        if (extract->vector() != vector)
            return nullptr;
    }
    const auto *vector_type =
        vector == nullptr ? nullptr : dynamic_cast<const oir::VectorType *>(vector->type());
    if (vector_type == nullptr || vector_type->element_count().is_scalable() ||
        vector_type->element_count().min_lanes != values.size() ||
        vector_type->element_type() != values.front()->type() ||
        (expected_type != nullptr && vector_type != expected_type)) {
        return nullptr;
    }
    return vector;
}

std::uint32_t float_bits(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "f32 must have a 32-bit representation");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool equivalent_splat_value(const oir::Value *lhs, const oir::Value *rhs) {
    if (lhs == rhs)
        return true;
    if (lhs == nullptr || rhs == nullptr || lhs->type() != rhs->type())
        return false;
    const auto *lhs_integer = dynamic_cast<const oir::ConstantInt *>(lhs);
    const auto *rhs_integer = dynamic_cast<const oir::ConstantInt *>(rhs);
    if (lhs_integer != nullptr || rhs_integer != nullptr) {
        return lhs_integer != nullptr && rhs_integer != nullptr &&
               lhs_integer->value() == rhs_integer->value();
    }
    const auto *lhs_float = dynamic_cast<const oir::ConstantFloat *>(lhs);
    const auto *rhs_float = dynamic_cast<const oir::ConstantFloat *>(rhs);
    if (lhs_float != nullptr || rhs_float != nullptr) {
        return lhs_float != nullptr && rhs_float != nullptr &&
               float_bits(lhs_float->value()) == float_bits(rhs_float->value());
    }
    return dynamic_cast<const oir::ConstantZero *>(lhs) != nullptr &&
           dynamic_cast<const oir::ConstantZero *>(rhs) != nullptr;
}

bool all_same_value(const std::vector<oir::Value *> &values) {
    return !values.empty() && std::all_of(values.begin() + 1, values.end(), [&](const auto *value) {
        return equivalent_splat_value(value, values.front());
    });
}

unsigned estimated_pack_cost(const std::vector<oir::Value *> &values) {
    if (reusable_vector(values) != nullptr)
        return 0;
    return all_same_value(values) ? 1U : static_cast<unsigned>(values.size());
}

std::vector<std::uint8_t> all_true_mask_bytes(std::uint64_t lanes) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>((lanes + 7U) / 8U), 0xffU);
    if (!bytes.empty() && lanes % 8U != 0) {
        bytes.back() = static_cast<std::uint8_t>((1U << (lanes % 8U)) - 1U);
    }
    return bytes;
}

oir::VPMetadata full_lane_metadata(oir::Module &module, oir::VectorType *vector_type,
                                   bool needs_passthrough) {
    auto *mask_type = module.types().fixed_vector_ty(module.types().int1_ty(),
                                                     vector_type->element_count().min_lanes);
    auto *mask = module.create_constant_mask(
        mask_type, all_true_mask_bytes(vector_type->element_count().min_lanes));
    auto *evl =
        module.create_i32(static_cast<std::int64_t>(vector_type->element_count().min_lanes));
    return {mask, evl, needs_passthrough ? module.create_undef(vector_type) : nullptr,
            oir::TailPolicy::Agnostic, oir::MaskPolicy::Agnostic};
}

oir::Value *pack_values(oir::Module &module, oir::BasicBlock &block, const oir::Instruction *anchor,
                        oir::VectorType *vector_type, const std::vector<oir::Value *> &values,
                        NameGenerator &names, MutationJournal &journal) {
    if (auto *existing = reusable_vector(values, vector_type))
        return existing;
    if (all_same_value(values)) {
        return journal.insert_before<oir::SplatInst>(block, anchor, vector_type, values.front(),
                                                     &block, names.next("splat"));
    }
    oir::Value *packed = module.create_undef(vector_type);
    for (std::size_t lane = 0; lane < values.size(); ++lane) {
        packed = journal.insert_before<oir::InsertElementInst>(
            block, anchor, vector_type, packed, values[lane],
            module.create_i32(static_cast<std::int64_t>(lane)), &block, names.next("pack"));
    }
    return packed;
}

void replace_scalar_results(oir::Module &module, oir::BasicBlock &block,
                            const oir::Instruction *anchor, oir::Value *vector,
                            const std::vector<oir::Instruction *> &scalars, NameGenerator &names,
                            MutationJournal &journal) {
    for (std::size_t lane = 0; lane < scalars.size(); ++lane) {
        auto *extract = journal.insert_before<oir::ExtractElementInst>(
            block, anchor, scalars[lane]->type(), vector,
            module.create_i32(static_cast<std::int64_t>(lane)), &block, names.next("lane"));
        journal.replace_all_uses(*scalars[lane], *extract);
    }
}

bool instruction_is_between_memory_barrier(const oir::Instruction &instruction,
                                           const oir::OIRAliasAnalysis &alias_analysis) {
    return dynamic_cast<const oir::CallInst *>(&instruction) != nullptr ||
           alias_analysis.may_read_memory(instruction) ||
           alias_analysis.may_write_memory(instruction);
}

bool stores_form_pack(const std::vector<oir::BinaryInst *> &binaries,
                      const oir::OIRAliasAnalysis &alias_analysis) {
    std::vector<oir::StoreInst *> stores;
    stores.reserve(binaries.size());
    for (auto *binary : binaries) {
        if (binary->uses().size() != 1)
            return false;
        auto *store = dynamic_cast<oir::StoreInst *>(binary->uses().front().user);
        if (store == nullptr || store->value() != binary)
            return false;
        stores.push_back(store);
    }
    if (!instructions_are_contiguous(stores, alias_analysis,
                                     [](const auto &store) { return store.ptr(); })) {
        return false;
    }
    auto &instructions = stores.front()->parent()->instructions();
    auto first = std::find_if(instructions.begin(), instructions.end(),
                              [&](const auto &owned) { return owned.get() == stores.front(); });
    auto last = std::find_if(instructions.begin(), instructions.end(),
                             [&](const auto &owned) { return owned.get() == stores.back(); });
    if (first == instructions.end() || last == instructions.end())
        return false;
    std::unordered_set<const oir::Instruction *> allowed(stores.begin(), stores.end());
    for (auto it = first; it != instructions.end(); ++it) {
        if (instruction_is_between_memory_barrier(**it, alias_analysis) &&
            allowed.find(it->get()) == allowed.end()) {
            return false;
        }
        if (it == last)
            break;
        if (std::next(it) == instructions.end())
            return false;
    }
    return true;
}

template <typename InstructionT>
void split_run(const std::vector<InstructionT *> &run, unsigned minimum_lanes,
               unsigned maximum_lanes, std::vector<std::vector<InstructionT *>> &packs) {
    std::size_t offset = 0;
    while (run.size() - offset >= minimum_lanes) {
        const auto lanes = std::min<std::size_t>(maximum_lanes, run.size() - offset);
        packs.emplace_back(run.begin() + static_cast<std::ptrdiff_t>(offset),
                           run.begin() + static_cast<std::ptrdiff_t>(offset + lanes));
        offset += lanes;
    }
}

std::vector<std::vector<oir::LoadInst *>>
collect_load_packs(oir::BasicBlock &block, const oir::OIRAliasAnalysis &alias_analysis,
                   const SLPVectorizerOptions &options, unsigned &load_candidates,
                   bool &saw_memory_order_barrier) {
    std::vector<std::vector<oir::LoadInst *>> packs;
    std::vector<oir::LoadInst *> run;
    auto flush = [&] {
        split_run(run, options.minimum_lanes, options.maximum_lanes, packs);
        run.clear();
    };
    for (auto &owned : block.instructions()) {
        auto *instruction = owned.get();
        if (auto *load = dynamic_cast<oir::LoadInst *>(instruction);
            load != nullptr && is_supported_scalar_type(load->type())) {
            ++load_candidates;
            if (!run.empty()) {
                auto extended = run;
                extended.push_back(load);
                if (load->type() != run.front()->type() ||
                    !instructions_are_contiguous(
                        extended, alias_analysis,
                        [](const auto &candidate) { return candidate.ptr(); })) {
                    flush();
                }
            }
            run.push_back(load);
            continue;
        }
        if (alias_analysis.may_write_memory(*instruction) ||
            (alias_analysis.may_read_memory(*instruction) &&
             dynamic_cast<oir::LoadInst *>(instruction) == nullptr)) {
            saw_memory_order_barrier |= !run.empty();
            flush();
        }
    }
    flush();
    return packs;
}

std::vector<std::vector<oir::StoreInst *>>
collect_store_packs(oir::BasicBlock &block, const oir::OIRAliasAnalysis &alias_analysis,
                    const SLPVectorizerOptions &options, unsigned &store_candidates,
                    bool &saw_memory_order_barrier) {
    std::vector<std::vector<oir::StoreInst *>> packs;
    std::vector<oir::StoreInst *> run;
    auto flush = [&] {
        split_run(run, options.minimum_lanes, options.maximum_lanes, packs);
        run.clear();
    };
    for (auto &owned : block.instructions()) {
        auto *instruction = owned.get();
        if (auto *store = dynamic_cast<oir::StoreInst *>(instruction);
            store != nullptr && is_supported_scalar_type(store->value()->type())) {
            ++store_candidates;
            if (!run.empty()) {
                auto extended = run;
                extended.push_back(store);
                if (store->value()->type() != run.front()->value()->type() ||
                    !instructions_are_contiguous(
                        extended, alias_analysis,
                        [](const auto &candidate) { return candidate.ptr(); })) {
                    flush();
                }
            }
            run.push_back(store);
            continue;
        }
        if (instruction_is_between_memory_barrier(*instruction, alias_analysis)) {
            saw_memory_order_barrier |= !run.empty();
            flush();
        }
    }
    flush();
    return packs;
}

std::vector<std::vector<oir::BinaryInst *>>
collect_binary_packs(oir::BasicBlock &block, const SLPVectorizerOptions &options,
                     unsigned &binary_candidates, bool &saw_dependence) {
    std::vector<std::vector<oir::BinaryInst *>> packs;
    std::vector<oir::BinaryInst *> run;
    auto flush = [&] {
        split_run(run, options.minimum_lanes, options.maximum_lanes, packs);
        run.clear();
    };
    for (auto &owned : block.instructions()) {
        auto *binary = dynamic_cast<oir::BinaryInst *>(owned.get());
        if (binary == nullptr || !is_supported_binary(*binary)) {
            flush();
            continue;
        }
        ++binary_candidates;
        bool depends_on_run = false;
        for (auto *prior : run) {
            depends_on_run |= binary->lhs() == prior || binary->rhs() == prior;
        }
        if (!run.empty() && (binary->op() != run.front()->op() ||
                             binary->type() != run.front()->type() || depends_on_run)) {
            saw_dependence |= depends_on_run;
            flush();
        }
        run.push_back(binary);
    }
    flush();
    return packs;
}

std::vector<std::vector<oir::CmpInst *>> collect_compare_packs(oir::BasicBlock &block,
                                                               const SLPVectorizerOptions &options,
                                                               unsigned &compare_candidates,
                                                               bool &saw_dependence) {
    std::vector<std::vector<oir::CmpInst *>> packs;
    std::vector<oir::CmpInst *> run;
    auto flush = [&] {
        split_run(run, options.minimum_lanes, options.maximum_lanes, packs);
        run.clear();
    };
    for (auto &owned : block.instructions()) {
        auto *compare = dynamic_cast<oir::CmpInst *>(owned.get());
        if (compare == nullptr || !is_supported_compare(*compare)) {
            flush();
            continue;
        }
        ++compare_candidates;
        bool depends_on_run = false;
        for (auto *prior : run)
            depends_on_run |= compare->lhs() == prior || compare->rhs() == prior;
        if (!run.empty() &&
            (compare->op() != run.front()->op() || compare->pred() != run.front()->pred() ||
             compare->lhs()->type() != run.front()->lhs()->type() || depends_on_run)) {
            saw_dependence |= depends_on_run;
            flush();
        }
        run.push_back(compare);
    }
    flush();
    return packs;
}

std::vector<std::vector<oir::BinaryInst *>>
collect_mask_binary_packs(oir::BasicBlock &block, const SLPVectorizerOptions &options,
                          unsigned &mask_candidates, bool &saw_dependence) {
    std::vector<std::vector<oir::BinaryInst *>> packs;
    std::vector<oir::BinaryInst *> run;
    auto flush = [&] {
        split_run(run, options.minimum_lanes, options.maximum_lanes, packs);
        run.clear();
    };
    for (auto &owned : block.instructions()) {
        auto *binary = dynamic_cast<oir::BinaryInst *>(owned.get());
        if (binary == nullptr || !is_supported_mask_binary(*binary)) {
            flush();
            continue;
        }
        ++mask_candidates;
        bool depends_on_run = false;
        for (auto *prior : run)
            depends_on_run |= binary->lhs() == prior || binary->rhs() == prior;
        if (!run.empty() && (binary->op() != run.front()->op() || depends_on_run)) {
            saw_dependence |= depends_on_run;
            flush();
        }
        run.push_back(binary);
    }
    flush();
    return packs;
}

RemarkCode shared_remark_code(SLPReasonCode code) {
    switch (code) {
    case SLPReasonCode::Vectorized:
        return RemarkCode::Vectorized;
    case SLPReasonCode::RejectTooFewLanes:
    case SLPReasonCode::RejectCost:
        return RemarkCode::RejectCost;
    case SLPReasonCode::RejectUnsupportedType:
    case SLPReasonCode::RejectVerification:
    case SLPReasonCode::RejectMutation:
        return RemarkCode::RejectUnsupportedType;
    case SLPReasonCode::RejectDependence:
    case SLPReasonCode::RejectMemoryOrder:
        return RemarkCode::RejectDependence;
    case SLPReasonCode::RejectAlias:
        return RemarkCode::RejectAlias;
    case SLPReasonCode::RejectCall:
        return RemarkCode::RejectCall;
    case SLPReasonCode::RejectPotentialTrap:
        return RemarkCode::RejectPotentialTrap;
    case SLPReasonCode::RejectMemorySemantics:
        return RemarkCode::RejectVolatileOrAtomic;
    case SLPReasonCode::RejectTargetFeature:
        return RemarkCode::RejectTargetFeature;
    case SLPReasonCode::Disabled:
        return RemarkCode::Disabled;
    }
    return RemarkCode::RejectUnsupportedType;
}

void publish_remark(RemarkLog &remarks, const PendingRemark &pending) {
    Remark remark;
    remark.vectorizer = VectorizerKind::SLP;
    remark.code = shared_remark_code(pending.diagnostic.code);
    remark.function = pending.diagnostic.function;
    remark.region = pending.diagnostic.block;
    remark.explanation = std::string(slp_reason_code_name(pending.diagnostic.code)) + ": " +
                         pending.diagnostic.explanation;
    remark.plan = pending.plan;
    remarks.add(std::move(remark));
}

void record_rejection(SLPVectorizerResult &result, std::vector<PendingRemark> &pending,
                      const oir::Function &function, const oir::BasicBlock &block,
                      SLPReasonCode code, std::string explanation, PlanChoice plan = {}) {
    const auto duplicate = std::any_of(
        result.diagnostics.begin(), result.diagnostics.end(), [&](const auto &diagnostic) {
            return diagnostic.function == function.name() && diagnostic.block == block.name() &&
                   diagnostic.code == code;
        });
    if (duplicate)
        return;
    SLPDiagnostic diagnostic{code, function.name(), block.name(), std::move(explanation)};
    result.diagnostics.push_back(diagnostic);
    pending.push_back({std::move(diagnostic), std::move(plan)});
}

bool block_has_candidate(const oir::BasicBlock &block) {
    unsigned candidates = 0;
    for (const auto &owned : block.instructions()) {
        const auto &instruction = *owned;
        if (const auto *binary = dynamic_cast<const oir::BinaryInst *>(&instruction)) {
            candidates += is_supported_binary(*binary) || is_supported_mask_binary(*binary) ||
                          is_potentially_trapping(instruction);
        } else if (const auto *compare = dynamic_cast<const oir::CmpInst *>(&instruction)) {
            candidates += is_supported_compare(*compare);
        } else if (const auto *load = dynamic_cast<const oir::LoadInst *>(&instruction)) {
            candidates += is_supported_scalar_type(load->type());
        } else if (const auto *store = dynamic_cast<const oir::StoreInst *>(&instruction)) {
            candidates += is_supported_scalar_type(store->value()->type());
        }
    }
    return candidates >= 2;
}

std::optional<std::pair<SLPReasonCode, std::string>>
block_legality_failure(const oir::BasicBlock &block, const oir::OIRAliasAnalysis &alias_analysis) {
    if (!block_has_candidate(block))
        return std::nullopt;
    for (const auto &owned : block.instructions()) {
        const auto &instruction = *owned;
        if (dynamic_cast<const oir::CallInst *>(&instruction) != nullptr) {
            return std::pair{SLPReasonCode::RejectCall,
                             std::string("call in candidate block prevents instruction and "
                                         "memory reordering")};
        }
        if (is_potentially_trapping(instruction)) {
            return std::pair{SLPReasonCode::RejectPotentialTrap,
                             std::string("potentially trapping div/rem operation in candidate "
                                         "block")};
        }
        if ((alias_analysis.may_read_memory(instruction) ||
             alias_analysis.may_write_memory(instruction)) &&
            !is_plain_memory(instruction)) {
            return std::pair{
                SLPReasonCode::RejectMemorySemantics,
                std::string("OIR exposes no volatile/atomic qualifier on these scalar "
                            "operations; SLP accepts exact plain LoadInst/StoreInst only "
                            "and rejects every other memory instruction form")};
        }
    }
    return std::nullopt;
}

PlanChoice cost_plan(unsigned lanes, unsigned scalar_cost, unsigned vector_cost) {
    PlanChoice plan;
    plan.scalable = false;
    plan.minimum_lanes = lanes;
    plan.estimated_scalar_cost = scalar_cost;
    plan.estimated_vector_cost = vector_cost;
    plan.estimated_vector_registers = 4;
    plan.uses_mask = true;
    return plan;
}

bool transform_load_pack(oir::Module &module, oir::Function &function, oir::BasicBlock &block,
                         const std::vector<oir::LoadInst *> &pack,
                         const target::TargetProfile &target, const SLPVectorizerOptions &options,
                         NameGenerator &names, MutationJournal &journal, BlockStats &stats,
                         SLPVectorizerResult &result, std::vector<PendingRemark> &pending) {
    const auto lanes = static_cast<unsigned>(pack.size());
    auto *scalar_type = pack.front()->type();
    if (!target_supports(target, scalar_type)) {
        record_rejection(result, pending, function, block, SLPReasonCode::RejectTargetFeature,
                         "target does not support 32-bit vector element family");
        return false;
    }
    const auto scalar_cost = lanes * static_cast<unsigned>(target.tuning.scalar_load_cost);
    const auto vector_cost = static_cast<unsigned>(target.tuning.vector_load_cost) + lanes;
    const auto plan = cost_plan(lanes, scalar_cost, vector_cost);
    if (!options.force && vector_cost >= scalar_cost) {
        record_rejection(result, pending, function, block, SLPReasonCode::RejectCost,
                         "contiguous load pack is not profitable", plan);
        return false;
    }
    auto *vector_type = module.types().fixed_vector_ty(scalar_type, lanes);
    auto metadata = full_lane_metadata(module, vector_type, true);
    auto *vector_load = journal.insert_before<oir::VPLoadInst>(
        block, pack.front(), oir::Instruction::OpID::VPLoad, vector_type, pack.front()->ptr(), 1,
        metadata, &block, names.next("load"));
    std::vector<oir::Instruction *> scalars(pack.begin(), pack.end());
    replace_scalar_results(module, block, pack.front(), vector_load, scalars, names, journal);
    journal.detach(block, scalars);
    ++stats.packs;
    stats.replaced += lanes;
    stats.maximum_lanes = std::max(stats.maximum_lanes, lanes);
    stats.scalar_cost += scalar_cost;
    stats.vector_cost += vector_cost;
    return true;
}

bool transform_binary_pack(oir::Module &module, oir::Function &function, oir::BasicBlock &block,
                           const std::vector<oir::BinaryInst *> &pack,
                           const oir::OIRAliasAnalysis &alias_analysis,
                           const target::TargetProfile &target, const SLPVectorizerOptions &options,
                           NameGenerator &names, MutationJournal &journal, BlockStats &stats,
                           SLPVectorizerResult &result, std::vector<PendingRemark> &pending) {
    const auto lanes = static_cast<unsigned>(pack.size());
    auto *scalar_type = pack.front()->type();
    if (!target_supports(target, scalar_type)) {
        record_rejection(result, pending, function, block, SLPReasonCode::RejectTargetFeature,
                         "target does not support 32-bit vector element family");
        return false;
    }
    std::vector<oir::Value *> lhs;
    std::vector<oir::Value *> rhs;
    lhs.reserve(pack.size());
    rhs.reserve(pack.size());
    for (auto *binary : pack) {
        lhs.push_back(binary->lhs());
        rhs.push_back(binary->rhs());
    }
    const auto scalar_cost = lanes * static_cast<unsigned>(target.tuning.scalar_alu_cost);
    const auto unpack_cost = stores_form_pack(pack, alias_analysis) ? 0U : lanes;
    const auto vector_cost = static_cast<unsigned>(target.tuning.vector_alu_cost) +
                             estimated_pack_cost(lhs) + estimated_pack_cost(rhs) + unpack_cost;
    const auto plan = cost_plan(lanes, scalar_cost, vector_cost);
    if (!options.force && vector_cost >= scalar_cost) {
        record_rejection(result, pending, function, block, SLPReasonCode::RejectCost,
                         "arithmetic pack cost includes required pack/unpack operations", plan);
        return false;
    }
    auto *vector_type = module.types().fixed_vector_ty(scalar_type, lanes);
    auto *packed_lhs = pack_values(module, block, pack.front(), vector_type, lhs, names, journal);
    auto *packed_rhs = pack_values(module, block, pack.front(), vector_type, rhs, names, journal);
    auto metadata = full_lane_metadata(module, vector_type, true);
    auto *vector_binary = journal.insert_before<oir::VPBinaryInst>(
        block, pack.front(), vector_type, pack.front()->op(), packed_lhs, packed_rhs, metadata,
        &block, names.next("binary"));
    std::vector<oir::Instruction *> scalars(pack.begin(), pack.end());
    replace_scalar_results(module, block, pack.front(), vector_binary, scalars, names, journal);
    journal.detach(block, scalars);
    ++stats.packs;
    stats.replaced += lanes;
    stats.maximum_lanes = std::max(stats.maximum_lanes, lanes);
    stats.scalar_cost += scalar_cost;
    stats.vector_cost += vector_cost;
    return true;
}

bool transform_compare_pack(oir::Module &module, oir::Function &function, oir::BasicBlock &block,
                            const std::vector<oir::CmpInst *> &pack,
                            const target::TargetProfile &target,
                            const SLPVectorizerOptions &options, NameGenerator &names,
                            MutationJournal &journal, BlockStats &stats,
                            SLPVectorizerResult &result, std::vector<PendingRemark> &pending) {
    const auto lanes = static_cast<unsigned>(pack.size());
    auto *scalar_type = pack.front()->lhs()->type();
    if (!target_supports(target, scalar_type)) {
        record_rejection(result, pending, function, block, SLPReasonCode::RejectTargetFeature,
                         "target does not support the comparison vector element family");
        return false;
    }
    std::vector<oir::Value *> lhs;
    std::vector<oir::Value *> rhs;
    lhs.reserve(pack.size());
    rhs.reserve(pack.size());
    for (auto *compare : pack) {
        lhs.push_back(compare->lhs());
        rhs.push_back(compare->rhs());
    }
    const auto scalar_cost = lanes * static_cast<unsigned>(target.tuning.scalar_alu_cost);
    const auto vector_cost = static_cast<unsigned>(target.tuning.vector_alu_cost) +
                             estimated_pack_cost(lhs) + estimated_pack_cost(rhs) + lanes;
    const auto plan = cost_plan(lanes, scalar_cost, vector_cost);
    if (!options.force && vector_cost >= scalar_cost) {
        record_rejection(result, pending, function, block, SLPReasonCode::RejectCost,
                         "comparison pack cost includes required pack/unpack operations", plan);
        return false;
    }

    auto *operand_type = module.types().fixed_vector_ty(scalar_type, lanes);
    auto *mask_type = module.types().fixed_vector_ty(module.types().int1_ty(), lanes);
    auto *packed_lhs = pack_values(module, block, pack.front(), operand_type, lhs, names, journal);
    auto *packed_rhs = pack_values(module, block, pack.front(), operand_type, rhs, names, journal);
    auto metadata = full_lane_metadata(module, mask_type, true);
    auto *vector_compare = journal.insert_before<oir::VPCmpInst>(
        block, pack.front(), mask_type, pack.front()->op(), pack.front()->pred(), packed_lhs,
        packed_rhs, metadata, &block, names.next("compare"));
    std::vector<oir::Instruction *> scalars(pack.begin(), pack.end());
    replace_scalar_results(module, block, pack.front(), vector_compare, scalars, names, journal);
    journal.detach(block, scalars);
    ++stats.packs;
    stats.replaced += lanes;
    stats.maximum_lanes = std::max(stats.maximum_lanes, lanes);
    stats.scalar_cost += scalar_cost;
    stats.vector_cost += vector_cost;
    return true;
}

bool transform_mask_binary_pack(oir::Module &module, oir::Function &function,
                                oir::BasicBlock &block, const std::vector<oir::BinaryInst *> &pack,
                                const target::TargetProfile &target,
                                const SLPVectorizerOptions &options, NameGenerator &names,
                                MutationJournal &journal, BlockStats &stats,
                                SLPVectorizerResult &result, std::vector<PendingRemark> &pending) {
    const auto lanes = static_cast<unsigned>(pack.size());
    if (!target_supports_mask(target)) {
        record_rejection(result, pending, function, block, SLPReasonCode::RejectTargetFeature,
                         "target does not support fixed mask vectors");
        return false;
    }
    std::vector<oir::Value *> lhs;
    std::vector<oir::Value *> rhs;
    lhs.reserve(pack.size());
    rhs.reserve(pack.size());
    for (auto *binary : pack) {
        lhs.push_back(binary->lhs());
        rhs.push_back(binary->rhs());
    }
    const auto scalar_cost = lanes * static_cast<unsigned>(target.tuning.scalar_alu_cost);
    const auto vector_cost = static_cast<unsigned>(target.tuning.vector_mask_cost) +
                             estimated_pack_cost(lhs) + estimated_pack_cost(rhs) + lanes;
    const auto plan = cost_plan(lanes, scalar_cost, vector_cost);
    if (!options.force && vector_cost >= scalar_cost) {
        record_rejection(result, pending, function, block, SLPReasonCode::RejectCost,
                         "mask pack cost includes required pack/unpack operations", plan);
        return false;
    }

    auto *mask_type = module.types().fixed_vector_ty(module.types().int1_ty(), lanes);
    auto *packed_lhs = pack_values(module, block, pack.front(), mask_type, lhs, names, journal);
    auto *packed_rhs = pack_values(module, block, pack.front(), mask_type, rhs, names, journal);
    auto metadata = full_lane_metadata(module, mask_type, true);
    auto *vector_binary = journal.insert_before<oir::VPBinaryInst>(
        block, pack.front(), mask_type, pack.front()->op(), packed_lhs, packed_rhs, metadata,
        &block, names.next("mask"));
    std::vector<oir::Instruction *> scalars(pack.begin(), pack.end());
    replace_scalar_results(module, block, pack.front(), vector_binary, scalars, names, journal);
    journal.detach(block, scalars);
    ++stats.packs;
    stats.replaced += lanes;
    stats.maximum_lanes = std::max(stats.maximum_lanes, lanes);
    stats.scalar_cost += scalar_cost;
    stats.vector_cost += vector_cost;
    return true;
}

bool transform_store_pack(oir::Module &module, oir::Function &function, oir::BasicBlock &block,
                          const std::vector<oir::StoreInst *> &pack,
                          const target::TargetProfile &target, const SLPVectorizerOptions &options,
                          NameGenerator &names, MutationJournal &journal, BlockStats &stats,
                          SLPVectorizerResult &result, std::vector<PendingRemark> &pending) {
    const auto lanes = static_cast<unsigned>(pack.size());
    auto *scalar_type = pack.front()->value()->type();
    if (!target_supports(target, scalar_type)) {
        record_rejection(result, pending, function, block, SLPReasonCode::RejectTargetFeature,
                         "target does not support 32-bit vector element family");
        return false;
    }
    std::vector<oir::Value *> values;
    values.reserve(pack.size());
    for (auto *store : pack)
        values.push_back(store->value());
    const auto scalar_cost = lanes * static_cast<unsigned>(target.tuning.scalar_store_cost);
    const auto vector_cost =
        static_cast<unsigned>(target.tuning.vector_store_cost) + estimated_pack_cost(values);
    const auto plan = cost_plan(lanes, scalar_cost, vector_cost);
    if (!options.force && vector_cost >= scalar_cost) {
        record_rejection(result, pending, function, block, SLPReasonCode::RejectCost,
                         "contiguous store pack is not profitable", plan);
        return false;
    }
    auto *vector_type = module.types().fixed_vector_ty(scalar_type, lanes);
    auto *anchor = pack.back();
    auto *packed = pack_values(module, block, anchor, vector_type, values, names, journal);
    auto metadata = full_lane_metadata(module, vector_type, false);
    journal.insert_before<oir::VPStoreInst>(block, anchor, oir::Instruction::OpID::VPStore,
                                            module.types().void_ty(), packed, pack.front()->ptr(),
                                            1, metadata, &block);
    std::vector<oir::Instruction *> scalars(pack.begin(), pack.end());
    journal.detach(block, scalars);
    ++stats.packs;
    stats.replaced += lanes;
    stats.maximum_lanes = std::max(stats.maximum_lanes, lanes);
    stats.scalar_cost += scalar_cost;
    stats.vector_cost += vector_cost;
    return true;
}

} // namespace

std::string_view slp_reason_code_name(SLPReasonCode code) {
    switch (code) {
    case SLPReasonCode::Vectorized:
        return "SLP_VECTORIZED";
    case SLPReasonCode::RejectTooFewLanes:
        return "SLP_REJECT_TOO_FEW_LANES";
    case SLPReasonCode::RejectUnsupportedType:
        return "SLP_REJECT_UNSUPPORTED_TYPE";
    case SLPReasonCode::RejectDependence:
        return "SLP_REJECT_DEPENDENCE";
    case SLPReasonCode::RejectAlias:
        return "SLP_REJECT_ALIAS";
    case SLPReasonCode::RejectMemoryOrder:
        return "SLP_REJECT_MEMORY_ORDER";
    case SLPReasonCode::RejectCall:
        return "SLP_REJECT_CALL";
    case SLPReasonCode::RejectPotentialTrap:
        return "SLP_REJECT_POTENTIAL_TRAP";
    case SLPReasonCode::RejectMemorySemantics:
        return "SLP_REJECT_MEMORY_SEMANTICS";
    case SLPReasonCode::RejectCost:
        return "SLP_REJECT_COST";
    case SLPReasonCode::RejectTargetFeature:
        return "SLP_REJECT_TARGET_FEATURE";
    case SLPReasonCode::RejectVerification:
        return "SLP_REJECT_VERIFICATION";
    case SLPReasonCode::RejectMutation:
        return "SLP_REJECT_MUTATION";
    case SLPReasonCode::Disabled:
        return "SLP_DISABLED";
    }
    return "SLP_REJECT_UNSUPPORTED_TYPE";
}

SLPVectorizer::SLPVectorizer(SLPVectorizerOptions options) : options_(options) {
}

SLPVectorizerResult SLPVectorizer::run(oir::Module &module, const target::TargetProfile &target,
                                       RemarkLog &remarks) const {
    SLPVectorizerResult result;
    if (options_.minimum_lanes < 2 || options_.maximum_lanes < options_.minimum_lanes ||
        options_.preferred_lanes < options_.minimum_lanes ||
        options_.preferred_lanes > options_.maximum_lanes) {
        result.success = false;
        result.message = "SLP_INVALID_OPTIONS: require 2 <= minimum <= preferred <= maximum";
        return result;
    }

    std::string input_error;
    if (!module.verify(&input_error)) {
        result.success = false;
        result.message = "SLP input module failed verification: " + input_error;
        result.diagnostics.push_back({SLPReasonCode::RejectVerification, {}, {}, input_error});
        return result;
    }

    std::vector<PendingRemark> pending_rejections;
    std::vector<BlockStats> changed_blocks;
    oir::OIRAliasAnalysis alias_analysis;
    MutationJournal journal;

    if (!options_.enabled) {
        for (const auto &owned_function : module.functions()) {
            if (owned_function->is_external())
                continue;
            for (const auto &owned_block : owned_function->blocks()) {
                if (!block_has_candidate(*owned_block))
                    continue;
                record_rejection(result, pending_rejections, *owned_function, *owned_block,
                                 SLPReasonCode::Disabled, "SLP vectorization is disabled");
            }
        }
        for (const auto &pending : pending_rejections)
            publish_remark(remarks, pending);
        return result;
    }

    try {
        for (auto &owned_function : module.functions()) {
            auto &function = *owned_function;
            if (function.is_external())
                continue;
            NameGenerator names(function);
            for (auto &owned_block : function.blocks()) {
                auto &block = *owned_block;
                if (unsupported_candidate_count(block) >= options_.minimum_lanes) {
                    record_rejection(result, pending_rejections, function, block,
                                     SLPReasonCode::RejectUnsupportedType,
                                     "candidate pack is outside the supported scalar i32/f32 "
                                     "element types");
                }
                if (auto failure = block_legality_failure(block, alias_analysis)) {
                    record_rejection(result, pending_rejections, function, block, failure->first,
                                     std::move(failure->second));
                    continue;
                }

                BlockStats stats;
                stats.function = &function;
                stats.block = &block;
                unsigned load_candidates = 0;
                bool saw_load_memory_order_barrier = false;
                auto load_packs =
                    collect_load_packs(block, alias_analysis, options_, load_candidates,
                                       saw_load_memory_order_barrier);
                for (const auto &pack : load_packs) {
                    transform_load_pack(module, function, block, pack, target, options_, names,
                                        journal, stats, result, pending_rejections);
                }
                if (load_candidates != 0 && load_packs.empty()) {
                    auto code = SLPReasonCode::RejectAlias;
                    std::string explanation =
                        "scalar loads are not a proven same-base contiguous byte range";
                    if (load_candidates < options_.minimum_lanes) {
                        code = SLPReasonCode::RejectTooFewLanes;
                        explanation = "scalar load run has fewer lanes than the configured minimum";
                    } else if (saw_load_memory_order_barrier) {
                        code = SLPReasonCode::RejectMemoryOrder;
                        explanation =
                            "a memory-writing operation separates otherwise eligible scalar loads";
                    }
                    record_rejection(result, pending_rejections, function, block, code,
                                     std::move(explanation));
                }

                unsigned binary_candidates = 0;
                bool saw_dependence = false;
                auto binary_packs =
                    collect_binary_packs(block, options_, binary_candidates, saw_dependence);
                for (const auto &pack : binary_packs) {
                    transform_binary_pack(module, function, block, pack, alias_analysis, target,
                                          options_, names, journal, stats, result,
                                          pending_rejections);
                }
                if (binary_candidates != 0 && binary_packs.empty()) {
                    record_rejection(result, pending_rejections, function, block,
                                     saw_dependence ? SLPReasonCode::RejectDependence
                                                    : SLPReasonCode::RejectTooFewLanes,
                                     saw_dependence
                                         ? "candidate arithmetic operations have an intra-pack SSA "
                                           "dependence"
                                         : "candidate arithmetic operations do not form a large "
                                           "enough same-op run");
                }

                unsigned compare_candidates = 0;
                bool saw_compare_dependence = false;
                auto compare_packs = collect_compare_packs(block, options_, compare_candidates,
                                                           saw_compare_dependence);
                for (const auto &pack : compare_packs) {
                    transform_compare_pack(module, function, block, pack, target, options_, names,
                                           journal, stats, result, pending_rejections);
                }
                if (compare_candidates != 0 && compare_packs.empty()) {
                    record_rejection(
                        result, pending_rejections, function, block,
                        saw_compare_dependence ? SLPReasonCode::RejectDependence
                                               : SLPReasonCode::RejectTooFewLanes,
                        saw_compare_dependence
                            ? "candidate comparisons have an intra-pack SSA dependence"
                            : "candidate comparisons do not form a large enough same-predicate "
                              "run");
                }

                unsigned mask_candidates = 0;
                bool saw_mask_dependence = false;
                auto mask_packs = collect_mask_binary_packs(block, options_, mask_candidates,
                                                            saw_mask_dependence);
                for (const auto &pack : mask_packs) {
                    transform_mask_binary_pack(module, function, block, pack, target, options_,
                                               names, journal, stats, result, pending_rejections);
                }
                if (mask_candidates != 0 && mask_packs.empty()) {
                    record_rejection(
                        result, pending_rejections, function, block,
                        saw_mask_dependence ? SLPReasonCode::RejectDependence
                                            : SLPReasonCode::RejectTooFewLanes,
                        saw_mask_dependence
                            ? "candidate mask operations have an intra-pack SSA dependence"
                            : "candidate mask operations do not form a large enough same-op run");
                }

                unsigned store_candidates = 0;
                bool saw_store_memory_order_barrier = false;
                auto store_packs =
                    collect_store_packs(block, alias_analysis, options_, store_candidates,
                                        saw_store_memory_order_barrier);
                for (const auto &pack : store_packs) {
                    transform_store_pack(module, function, block, pack, target, options_, names,
                                         journal, stats, result, pending_rejections);
                }
                if (store_candidates != 0 && store_packs.empty()) {
                    auto code = SLPReasonCode::RejectAlias;
                    std::string explanation =
                        "scalar stores are not a proven same-base contiguous byte range";
                    if (store_candidates < options_.minimum_lanes) {
                        code = SLPReasonCode::RejectTooFewLanes;
                        explanation =
                            "scalar store run has fewer lanes than the configured minimum";
                    } else if (saw_store_memory_order_barrier) {
                        code = SLPReasonCode::RejectMemoryOrder;
                        explanation = "a memory-reading or writing operation separates otherwise "
                                      "eligible scalar stores";
                    }
                    record_rejection(result, pending_rejections, function, block, code,
                                     std::move(explanation));
                }

                if (stats.packs != 0)
                    changed_blocks.push_back(stats);
            }
        }
        journal.discard_dead_inserted_extracts();
    } catch (const MutationFailure &failure) {
        journal.rollback();
        result.success = false;
        result.message = failure.what();
        SLPDiagnostic diagnostic{SLPReasonCode::RejectMutation, failure.function(), failure.block(),
                                 failure.what()};
        result.diagnostics.push_back(diagnostic);
        for (const auto &pending : pending_rejections)
            publish_remark(remarks, pending);
        publish_remark(remarks, {diagnostic, {}});
        return result;
    }

    if (!changed_blocks.empty()) {
        std::string verification_error;
        bool verified = module.verify(&verification_error);
        if (verified && options_.post_transform_verifier) {
            try {
                verified = options_.post_transform_verifier(module, verification_error);
                if (!verified && verification_error.empty()) {
                    verification_error = "post-transform verifier rejected the module";
                }
            } catch (const std::exception &error) {
                verified = false;
                verification_error = std::string("post-transform verifier threw: ") + error.what();
            } catch (...) {
                verified = false;
                verification_error = "post-transform verifier threw a non-standard exception";
            }
        }
        if (!verified) {
            journal.rollback();
            result.success = false;
            result.message =
                "SLP produced invalid OIR and withheld SLP_VECTORIZED: " + verification_error;
            for (const auto &stats : changed_blocks) {
                record_rejection(result, pending_rejections, *stats.function, *stats.block,
                                 SLPReasonCode::RejectVerification, verification_error);
            }
            for (const auto &pending : pending_rejections)
                publish_remark(remarks, pending);
            return result;
        }
    }

    journal.commit();

    for (const auto &pending : pending_rejections)
        publish_remark(remarks, pending);
    for (const auto &stats : changed_blocks) {
        PlanChoice plan = cost_plan(stats.maximum_lanes, stats.scalar_cost, stats.vector_cost);
        SLPDiagnostic diagnostic{
            SLPReasonCode::Vectorized, stats.function->name(), stats.block->name(),
            "transformed and verified " + std::to_string(stats.packs) + " fixed-width SLP pack(s)"};
        result.diagnostics.push_back(diagnostic);
        publish_remark(remarks, {diagnostic, plan});
        result.changed = true;
        result.packs_vectorized += stats.packs;
        result.scalar_instructions_replaced += stats.replaced;
    }
    return result;
}

} // namespace pass::oir_vectorize
