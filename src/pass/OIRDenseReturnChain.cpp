#include "../../include/oir/OIRScalarOpt.h"

#include "../../include/oir/OIR.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

struct DenseReturnChain {
    oir::Value *selector = nullptr;
    oir::Value *payload = nullptr;
    oir::Instruction::OpID op = oir::Instruction::OpID::Mul;
    std::map<std::int64_t, std::int64_t> factors_by_key;
};

oir::Instruction *only_instruction(oir::BasicBlock *block) {
    if (block == nullptr || block->instructions().size() != 1) {
        return nullptr;
    }
    return block->instructions().front().get();
}

bool get_return_value(oir::BasicBlock *block, oir::Value *&value) {
    auto *ret = dynamic_cast<oir::ReturnInst *>(only_instruction(block));
    if (ret == nullptr || !ret->has_value()) {
        return false;
    }
    value = ret->value();
    return true;
}

bool match_case_return(oir::BasicBlock *block, oir::Value *&payload,
                       oir::Instruction::OpID &op, std::int64_t &factor) {
    if (block == nullptr || block->instructions().size() != 2) {
        return false;
    }
    auto inst_it = block->instructions().begin();
    auto *binary = dynamic_cast<oir::BinaryInst *>(inst_it->get());
    ++inst_it;
    auto *ret = dynamic_cast<oir::ReturnInst *>(inst_it->get());
    if (binary == nullptr || ret == nullptr || !ret->has_value() || ret->value() != binary) {
        return false;
    }

    auto rhs_const = int_constant(binary->rhs());
    auto lhs_const = int_constant(binary->lhs());
    if (binary->op() == oir::Instruction::OpID::Mul) {
        if (rhs_const) {
            if (payload != nullptr && payload != binary->lhs()) {
                return false;
            }
            payload = binary->lhs();
            op = binary->op();
            factor = *rhs_const;
            return true;
        }
        if (lhs_const) {
            if (payload != nullptr && payload != binary->rhs()) {
                return false;
            }
            payload = binary->rhs();
            op = binary->op();
            factor = *lhs_const;
            return true;
        }
        return false;
    }

    if (binary->op() == oir::Instruction::OpID::SDiv && rhs_const) {
        if (payload != nullptr && payload != binary->lhs()) {
            return false;
        }
        payload = binary->lhs();
        op = binary->op();
        factor = *rhs_const;
        return true;
    }

    return false;
}

bool match_chain_block(oir::BasicBlock *block, oir::Value *&selector, oir::BasicBlock *&case_bb,
                       oir::BasicBlock *&next_bb, std::int64_t &key) {
    if (block == nullptr || block->instructions().size() != 2) {
        return false;
    }
    auto inst_it = block->instructions().begin();
    auto *cmp = dynamic_cast<oir::CmpInst *>(inst_it->get());
    ++inst_it;
    auto *branch = dynamic_cast<oir::BranchInst *>(inst_it->get());
    if (cmp == nullptr || branch == nullptr || !branch->is_conditional() ||
        branch->cond() != cmp || cmp->op() != oir::Instruction::OpID::ICmp ||
        cmp->pred() != oir::CmpPred::EQ) {
        return false;
    }

    oir::Value *candidate_selector = nullptr;
    auto rhs_const = int_constant(cmp->rhs());
    auto lhs_const = int_constant(cmp->lhs());
    if (rhs_const) {
        candidate_selector = cmp->lhs();
        key = *rhs_const;
    } else if (lhs_const) {
        candidate_selector = cmp->rhs();
        key = *lhs_const;
    } else {
        return false;
    }

    if (selector != nullptr && selector != candidate_selector) {
        return false;
    }
    selector = candidate_selector;
    case_bb = branch->true_bb();
    next_bb = branch->false_bb();
    return case_bb != nullptr && next_bb != nullptr;
}

bool is_dense(const std::map<std::int64_t, std::int64_t> &items) {
    if (items.size() < 4) {
        return false;
    }
    const auto min = items.begin()->first;
    const auto max = items.rbegin()->first;
    return max >= min &&
           static_cast<std::uint64_t>(max - min + 1) == static_cast<std::uint64_t>(items.size());
}

bool is_positive_power_of_two(std::int64_t value) {
    if (value <= 0) {
        return false;
    }
    auto bits = static_cast<std::uint64_t>(value);
    return (bits & (bits - 1U)) == 0;
}

unsigned log2_u64(std::uint64_t value) {
    unsigned out = 0;
    while (value > 1) {
        value >>= 1U;
        ++out;
    }
    return out;
}

bool factors_are_selector_powers(const std::map<std::int64_t, std::int64_t> &items) {
    for (const auto &[key, factor] : items) {
        if (key < 0 || key >= 31 || !is_positive_power_of_two(factor) ||
            log2_u64(static_cast<std::uint64_t>(factor)) != static_cast<unsigned>(key)) {
            return false;
        }
    }
    return true;
}

bool should_keep_power_of_two_cases(const DenseReturnChain &chain) {
    return (chain.op == oir::Instruction::OpID::Mul ||
            chain.op == oir::Instruction::OpID::SDiv) &&
           factors_are_selector_powers(chain.factors_by_key);
}

bool match_dense_return_chain(oir::Function &function, DenseReturnChain &out) {
    if (function.is_external() || function.entry_block() == nullptr ||
        !function.return_type()->is_integer()) {
        return false;
    }

    std::unordered_set<oir::BasicBlock *> seen;
    oir::BasicBlock *block = function.entry_block();
    while (true) {
        if (block == nullptr || !seen.insert(block).second) {
            return false;
        }

        oir::BasicBlock *case_bb = nullptr;
        oir::BasicBlock *next_bb = nullptr;
        std::int64_t key = 0;
        if (!match_chain_block(block, out.selector, case_bb, next_bb, key)) {
            oir::Value *default_value = nullptr;
            if (!get_return_value(block, default_value) || default_value != out.payload) {
                return false;
            }
            break;
        }

        if (!seen.insert(case_bb).second) {
            return false;
        }
        std::int64_t factor = 0;
        auto case_op = out.op;
        if (!match_case_return(case_bb, out.payload, case_op, factor)) {
            return false;
        }
        if (out.factors_by_key.empty()) {
            out.op = case_op;
        } else if (out.op != case_op) {
            return false;
        }
        if (!out.factors_by_key.emplace(key, factor).second) {
            return false;
        }
        block = next_bb;
    }

    if (out.selector == nullptr || out.payload == nullptr ||
        out.selector->type() != function.parent()->types().int32_ty() ||
        out.payload->type() != function.parent()->types().int32_ty() ||
        dynamic_cast<oir::Instruction *>(out.selector) != nullptr ||
        dynamic_cast<oir::Instruction *>(out.payload) != nullptr) {
        return false;
    }
    return is_dense(out.factors_by_key);
}

std::string initializer_for(const std::map<std::int64_t, std::int64_t> &items) {
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto &[_, factor] : items) {
        if (!first) {
            oss << ", ";
        }
        first = false;
        oss << factor;
    }
    oss << "}";
    return oss.str();
}

void clear_body(oir::Function &function) {
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            inst->drop_all_operands();
        }
    }
    function.blocks().clear();
}

std::string next_table_name(oir::Module &module, unsigned &next_id) {
    while (true) {
        std::string name = "__yo_dense_factor." + std::to_string(next_id++);
        if (module.get_global(name) == nullptr) {
            return name;
        }
    }
}

bool rewrite_as_factor_table(oir::Function &function, const DenseReturnChain &chain,
                             unsigned &next_table_id) {
    auto &module = *function.parent();
    const auto min_key = chain.factors_by_key.begin()->first;
    const auto max_key = chain.factors_by_key.rbegin()->first;
    const auto count = static_cast<std::size_t>(max_key - min_key + 1);

    auto *array_type = module.types().array_ty(module.types().int32_ty(), count);
    const std::string table_name = next_table_name(module, next_table_id);
    auto *table = module.create_global(table_name, array_type, true);
    table->set_initializer_literal(initializer_for(chain.factors_by_key));

    auto *selector = chain.selector;
    auto *payload = chain.payload;
    clear_body(function);

    auto *entry = function.create_block("dense.entry");
    auto *range_hi = function.create_block("dense.range_hi");
    auto *table_block = function.create_block("dense.table");
    auto *default_block = function.create_block("dense.default");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *below_min = builder.create_icmp(oir::CmpPred::LT, selector, builder.i32(min_key),
                                          "dense.below");
    builder.create_cond_br(below_min, default_block, range_hi);

    builder.set_insert_point(range_hi);
    auto *above_max = builder.create_icmp(oir::CmpPred::GT, selector, builder.i32(max_key),
                                          "dense.above");
    builder.create_cond_br(above_max, default_block, table_block);

    builder.set_insert_point(table_block);
    auto *zero = builder.i32(0);
    auto *idx = min_key == 0
                    ? static_cast<oir::Value *>(selector)
                    : builder.create_binary(oir::Instruction::OpID::Sub, selector,
                                            builder.i32(min_key), "dense.index");
    auto *factor_addr =
        builder.create_gep(table, module.types().ptr_ty(module.types().int32_ty()),
                           std::vector<oir::Value *>{zero, idx}, "dense.factor.addr");
    auto *factor = builder.create_load(factor_addr, module.types().int32_ty(), "dense.factor");
    auto *result = builder.create_binary(chain.op, payload, factor, "dense.result");
    builder.create_ret(result);

    builder.set_insert_point(default_block);
    builder.create_ret(payload);
    return true;
}

bool rewrite_as_power_shift(oir::Function &function, const DenseReturnChain &chain) {
    if (!should_keep_power_of_two_cases(chain)) {
        return false;
    }

    const auto min_key = chain.factors_by_key.begin()->first;
    const auto max_key = chain.factors_by_key.rbegin()->first;
    if (min_key < 0 || max_key >= 31) {
        return false;
    }

    auto &module = *function.parent();
    auto *selector = chain.selector;
    auto *payload = chain.payload;
    clear_body(function);

    auto *entry = function.create_block("pow2.entry");
    auto *range_hi = function.create_block("pow2.range_hi");
    auto *shift_block = function.create_block("pow2.shift");
    auto *default_block = function.create_block("pow2.default");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *below_min =
        builder.create_icmp(oir::CmpPred::LT, selector, builder.i32(min_key), "pow2.below");
    builder.create_cond_br(below_min, default_block, range_hi);

    builder.set_insert_point(range_hi);
    auto *above_max =
        builder.create_icmp(oir::CmpPred::GT, selector, builder.i32(max_key), "pow2.above");
    builder.create_cond_br(above_max, default_block, shift_block);

    builder.set_insert_point(shift_block);
    oir::Value *result = nullptr;
    if (chain.op == oir::Instruction::OpID::Mul) {
        result = builder.create_binary(oir::Instruction::OpID::Shl, payload, selector,
                                       "pow2.shifted");
    } else {
        auto *sign = builder.create_binary(oir::Instruction::OpID::AShr, payload, builder.i32(31),
                                           "pow2.sign");
        auto *divisor =
            builder.create_binary(oir::Instruction::OpID::Shl, builder.i32(1), selector,
                                  "pow2.divisor");
        auto *mask =
            builder.create_binary(oir::Instruction::OpID::Sub, divisor, builder.i32(1),
                                  "pow2.mask");
        auto *bias =
            builder.create_binary(oir::Instruction::OpID::And, sign, mask, "pow2.bias");
        auto *adjusted =
            builder.create_binary(oir::Instruction::OpID::Add, payload, bias, "pow2.adjusted");
        result = builder.create_binary(oir::Instruction::OpID::AShr, adjusted, selector,
                                       "pow2.quotient");
    }
    builder.create_ret(result);

    builder.set_insert_point(default_block);
    builder.create_ret(payload);
    return true;
}

} // namespace

bool lower_dense_return_chains(oir::Module &module, Stats &stats) {
    bool changed = false;
    unsigned next_table_id = 0;
    for (auto &function : module.functions()) {
        DenseReturnChain chain;
        if (!match_dense_return_chain(*function, chain)) {
            continue;
        }
        if (rewrite_as_power_shift(*function, chain)) {
            changed = true;
            ++stats.branches;
            continue;
        }
        if (rewrite_as_factor_table(*function, chain, next_table_id)) {
            changed = true;
            ++stats.branches;
        }
    }
    return changed;
}

} // namespace pass::oir_opt
