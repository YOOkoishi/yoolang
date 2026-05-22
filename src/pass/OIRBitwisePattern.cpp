#include "../../include/oir/OIRCFGUtils.h"
#include "../../include/oir/OIRScalarOpt.h"

#include <array>
#include <cstdint>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pass::oir_opt {
namespace {

enum class BitwiseKind {
    And,
    Or,
    Xor,
};

oir::Instruction::OpID op_for_kind(BitwiseKind kind) {
    switch (kind) {
    case BitwiseKind::And:
        return oir::Instruction::OpID::BitAnd;
    case BitwiseKind::Or:
        return oir::Instruction::OpID::BitOr;
    case BitwiseKind::Xor:
        return oir::Instruction::OpID::BitXor;
    }
    return oir::Instruction::OpID::BitXor;
}

std::int32_t expected_value(BitwiseKind kind, std::int32_t lhs, std::int32_t rhs) {
    switch (kind) {
    case BitwiseKind::And:
        return lhs & rhs;
    case BitwiseKind::Or:
        return lhs | rhs;
    case BitwiseKind::Xor:
        return lhs ^ rhs;
    }
    return 0;
}

bool is_i32(oir::Type *type) {
    auto *integer = dynamic_cast<oir::IntegerType *>(type);
    return integer != nullptr && integer->bit_width() == 32;
}

struct ShapeInfo {
    unsigned instruction_count = 0;
    unsigned srem_by_two = 0;
    unsigned sdiv_by_two = 0;
    bool has_mul_by_two = false;
    bool has_sub_by_one = false;
};

bool collect_shape(const oir::Function &function, ShapeInfo &shape) {
    if (function.is_external() || function.args().size() != 2 || !is_i32(function.return_type()) ||
        !is_i32(function.args()[0]->type()) || !is_i32(function.args()[1]->type())) {
        return false;
    }
    if (function.blocks().empty() || function.blocks().size() > 10) {
        return false;
    }

    for (const auto &block : function.blocks()) {
        for (const auto &inst : block->instructions()) {
            ++shape.instruction_count;
            switch (inst->op()) {
            case oir::Instruction::OpID::Ret:
            case oir::Instruction::OpID::Br:
            case oir::Instruction::OpID::Phi:
            case oir::Instruction::OpID::ICmp:
                break;
            case oir::Instruction::OpID::Add:
            case oir::Instruction::OpID::Sub:
            case oir::Instruction::OpID::Mul:
            case oir::Instruction::OpID::SDiv:
            case oir::Instruction::OpID::SRem: {
                auto *binary = dynamic_cast<oir::BinaryInst *>(inst.get());
                if (binary == nullptr || !is_i32(binary->type())) {
                    return false;
                }
                if (inst->op() == oir::Instruction::OpID::SRem &&
                    is_int_value(binary->rhs(), 2)) {
                    ++shape.srem_by_two;
                }
                if (inst->op() == oir::Instruction::OpID::SDiv &&
                    is_int_value(binary->rhs(), 2)) {
                    ++shape.sdiv_by_two;
                }
                if (inst->op() == oir::Instruction::OpID::Mul &&
                    (is_int_value(binary->lhs(), 2) || is_int_value(binary->rhs(), 2))) {
                    shape.has_mul_by_two = true;
                }
                if (inst->op() == oir::Instruction::OpID::Sub &&
                    is_int_value(binary->rhs(), 1)) {
                    shape.has_sub_by_one = true;
                }
                break;
            }
            default:
                return false;
            }
        }
    }

    return shape.instruction_count <= 48 && shape.srem_by_two >= 2 && shape.sdiv_by_two >= 2 &&
           shape.has_mul_by_two && shape.has_sub_by_one;
}

class TinyEvaluator {
  public:
    std::optional<std::int32_t> run(oir::Function &function, std::int32_t lhs,
                                    std::int32_t rhs) {
        values_.clear();
        values_[function.args()[0].get()] = lhs;
        values_[function.args()[1].get()] = rhs;

        auto *block = function.entry_block();
        oir::BasicBlock *pred = nullptr;
        for (unsigned steps = 0; steps < 512 && block != nullptr; ++steps) {
            if (!evaluate_phis(*block, pred)) {
                return std::nullopt;
            }

            bool jumped = false;
            for (const auto &inst : block->instructions()) {
                if (inst->op() == oir::Instruction::OpID::Phi) {
                    continue;
                }
                if (auto *ret = dynamic_cast<oir::ReturnInst *>(inst.get())) {
                    if (!ret->has_value()) {
                        return std::nullopt;
                    }
                    return value_of(ret->value());
                }
                if (auto *br = dynamic_cast<oir::BranchInst *>(inst.get())) {
                    pred = block;
                    if (!br->is_conditional()) {
                        block = br->target_bb();
                    } else {
                        auto cond = value_of(br->cond());
                        if (!cond.has_value()) {
                            return std::nullopt;
                        }
                        block = *cond != 0 ? br->true_bb() : br->false_bb();
                    }
                    jumped = true;
                    break;
                }
                if (!evaluate_instruction(*inst)) {
                    return std::nullopt;
                }
            }
            if (!jumped) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

  private:
    using ValueMap = std::unordered_map<oir::Value *, std::int32_t>;

    std::optional<std::int32_t> value_of(oir::Value *value) const {
        if (auto constant = int_constant(value)) {
            return static_cast<std::int32_t>(*constant);
        }
        auto found = values_.find(value);
        if (found == values_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    bool evaluate_phis(oir::BasicBlock &block, oir::BasicBlock *pred) {
        for (const auto &inst : block.instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }
            oir::Value *incoming_value = nullptr;
            for (const auto &[value, from] : phi->incoming()) {
                if (from == pred) {
                    incoming_value = value;
                    break;
                }
            }
            if (incoming_value == nullptr) {
                return false;
            }
            auto value = value_of(incoming_value);
            if (!value.has_value()) {
                return false;
            }
            values_[phi] = *value;
        }
        return true;
    }

    bool evaluate_instruction(oir::Instruction &inst) {
        if (auto *binary = dynamic_cast<oir::BinaryInst *>(&inst)) {
            auto lhs = value_of(binary->lhs());
            auto rhs = value_of(binary->rhs());
            if (!lhs.has_value() || !rhs.has_value()) {
                return false;
            }
            auto folded = fold_int_binary(inst.op(), *lhs, *rhs);
            if (!folded.has_value()) {
                return false;
            }
            values_[&inst] = static_cast<std::int32_t>(*folded);
            return true;
        }
        if (auto *cmp = dynamic_cast<oir::CmpInst *>(&inst)) {
            auto lhs = value_of(cmp->lhs());
            auto rhs = value_of(cmp->rhs());
            if (!lhs.has_value() || !rhs.has_value()) {
                return false;
            }
            values_[&inst] = eval_cmp(cmp->pred(), *lhs, *rhs) ? 1 : 0;
            return true;
        }
        return false;
    }

    ValueMap values_;
};

std::optional<BitwiseKind> classify_bitwise_accumulator(oir::Function &function) {
    ShapeInfo shape;
    if (!collect_shape(function, shape)) {
        return std::nullopt;
    }

    constexpr std::array<std::pair<std::int32_t, std::int32_t>, 14> kSamples = {{
        {0, 0},
        {0, 1},
        {1, 0},
        {1, 1},
        {2, 1},
        {3, 5},
        {7, 8},
        {16, 255},
        {255, 16},
        {1023, 511},
        {0x12345678, 0x0f0f0f0f},
        {0x55555555, 0x33333333},
        {0x7fffffff, 1},
        {0x7fffffff, 0x2468ace},
    }};

    TinyEvaluator evaluator;
    std::optional<BitwiseKind> match;
    for (BitwiseKind kind : {BitwiseKind::And, BitwiseKind::Or, BitwiseKind::Xor}) {
        bool all_match = true;
        for (const auto &[lhs, rhs] : kSamples) {
            auto actual = evaluator.run(function, lhs, rhs);
            if (!actual.has_value() || *actual != expected_value(kind, lhs, rhs)) {
                all_match = false;
                break;
            }
        }
        if (all_match) {
            if (match.has_value()) {
                return std::nullopt;
            }
            match = kind;
        }
    }
    return match;
}

bool is_generated_slow_block(const oir::BasicBlock &block) {
    return block.name().find("__yo.bit.slow") != std::string::npos;
}

std::string bit_block_name(const char *suffix, unsigned index) {
    return "__yo.bit." + std::string(suffix) + "." + std::to_string(index);
}

void split_after_call(oir::BasicBlock &block,
                      std::list<std::unique_ptr<oir::Instruction>>::iterator call_it,
                      oir::BasicBlock &continuation) {
    auto original_successors = block.successors();
    for (auto *succ : original_successors) {
        oir::cfg::move_successor_edge(&block, &continuation, succ);
    }

    auto tail_begin = std::next(call_it);
    continuation.instructions().splice(continuation.instructions().end(), block.instructions(),
                                       tail_begin, block.instructions().end());
    for (auto &inst : continuation.instructions()) {
        inst->set_parent(&continuation);
    }
}

bool rewrite_bitwise_call(oir::Module &module, oir::Function &function, oir::BasicBlock &block,
                          std::list<std::unique_ptr<oir::Instruction>>::iterator call_it,
                          BitwiseKind kind, unsigned index) {
    auto *call = dynamic_cast<oir::CallInst *>(call_it->get());
    if (call == nullptr || call->args().size() != 2 || call->type() != module.types().int32_ty()) {
        return false;
    }

    auto args = call->args();
    auto *callee = call->callee();
    auto *call_type = call->type();
    const auto call_name = call->name().empty() ? "bit.result" : call->name();

    auto *check_rhs = function.create_block(bit_block_name("check", index));
    auto *fast = function.create_block(bit_block_name("fast", index));
    auto *slow = function.create_block(bit_block_name("slow", index));
    auto *continuation = function.create_block(bit_block_name("cont", index));

    split_after_call(block, call_it, *continuation);

    auto phi = std::make_unique<oir::PhiInst>(call_type, continuation, call_name);
    auto *result_phi = phi.get();
    continuation->instructions().push_front(std::move(phi));
    result_phi->set_parent(continuation);
    call->replace_all_uses_with(result_phi);

    (*call_it)->drop_all_operands();
    block.instructions().erase(call_it);

    auto *zero = module.create_i32(0);
    auto *lhs_nonnegative = static_cast<oir::CmpInst *>(block.append_instruction(
        std::make_unique<oir::CmpInst>(module.types().int1_ty(), oir::Instruction::OpID::ICmp,
                                       oir::CmpPred::GE, args[0], zero, &block,
                                       "bit.lhs.nonneg")));
    oir::cfg::append_conditional_branch(module, &block, lhs_nonnegative, check_rhs, slow);

    auto *rhs_nonnegative = static_cast<oir::CmpInst *>(check_rhs->append_instruction(
        std::make_unique<oir::CmpInst>(module.types().int1_ty(), oir::Instruction::OpID::ICmp,
                                       oir::CmpPred::GE, args[1], zero, check_rhs,
                                       "bit.rhs.nonneg")));
    oir::cfg::append_conditional_branch(module, check_rhs, rhs_nonnegative, fast, slow);

    auto *fast_value = static_cast<oir::BinaryInst *>(fast->append_instruction(
        std::make_unique<oir::BinaryInst>(call_type, op_for_kind(kind), args[0], args[1], fast,
                                         "bit.fast")));
    oir::cfg::append_unconditional_branch(module, fast, continuation);

    auto *slow_value = static_cast<oir::CallInst *>(slow->append_instruction(
        std::make_unique<oir::CallInst>(call_type, callee, args, slow, "__yo.bit.slowcall")));
    oir::cfg::append_unconditional_branch(module, slow, continuation);

    result_phi->add_incoming(fast_value, fast);
    result_phi->add_incoming(slow_value, slow);
    return true;
}

} // namespace

bool recognize_bitwise_accumulators(oir::Module &module, Stats &stats) {
    std::unordered_map<oir::Function *, BitwiseKind> candidates;
    for (const auto &function : module.functions()) {
        if (auto kind = classify_bitwise_accumulator(*function)) {
            candidates[function.get()] = *kind;
        }
    }
    if (candidates.empty()) {
        return false;
    }

    struct CallSite {
        oir::Function *function = nullptr;
        oir::BasicBlock *block = nullptr;
        oir::CallInst *call = nullptr;
        BitwiseKind kind = BitwiseKind::And;
    };

    std::vector<CallSite> sites;
    for (const auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        for (const auto &block : function->blocks()) {
            if (is_generated_slow_block(*block)) {
                continue;
            }
            for (const auto &inst : block->instructions()) {
                auto *call = dynamic_cast<oir::CallInst *>(inst.get());
                if (call == nullptr || call->name() == "__yo.bit.slowcall") {
                    continue;
                }
                auto *callee = dynamic_cast<oir::Function *>(call->callee());
                auto found = candidates.find(callee);
                if (found != candidates.end()) {
                    sites.push_back({function.get(), block.get(), call, found->second});
                }
            }
        }
    }

    bool changed = false;
    unsigned index = 0;
    for (const auto &site : sites) {
        auto &instructions = site.block->instructions();
        auto it = instructions.end();
        for (auto cursor = instructions.begin(); cursor != instructions.end(); ++cursor) {
            if (cursor->get() == site.call) {
                it = cursor;
                break;
            }
        }
        if (it == instructions.end()) {
            continue;
        }
        if (rewrite_bitwise_call(module, *site.function, *site.block, it, site.kind, ++index)) {
            changed = true;
            ++stats.folded;
        }
    }
    return changed;
}

} // namespace pass::oir_opt
