#include "../../include/oir/OIRScalarOpt.h"

#include "../../include/oir/OIRAnalysis.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

std::string ptr_key(const void *ptr) {
    std::ostringstream oss;
    oss << ptr;
    return oss.str();
}

oir::Value *resolve_value(const ReplacementMap &replacements, oir::Value *value) {
    std::unordered_set<oir::Value *> seen;
    auto *current = value;
    while (current != nullptr && seen.insert(current).second) {
        auto found = replacements.find(current);
        if (found == replacements.end()) {
            return current;
        }
        current = found->second;
    }
    return value;
}

std::string value_key(const ReplacementMap &replacements, oir::Value *value) {
    value = resolve_value(replacements, value);
    if (auto constant = int_constant(value)) {
        return "i:" + ptr_key(value->type()) + ":" + std::to_string(*constant);
    }
    if (auto constant = float_constant(value)) {
        return "f:" + ptr_key(value->type()) + ":" + std::to_string(*constant);
    }
    if (dynamic_cast<oir::ConstantZero *>(value) != nullptr) {
        return "z:" + ptr_key(value->type());
    }
    if (dynamic_cast<oir::UndefValue *>(value) != nullptr) {
        return "u:" + ptr_key(value);
    }
    return "v:" + ptr_key(value);
}

bool is_commutative_integer_op(oir::Instruction::OpID op) {
    return op == oir::Instruction::OpID::Add || op == oir::Instruction::OpID::Mul;
}

bool is_commutative_equality(oir::CmpPred pred) {
    return pred == oir::CmpPred::EQ || pred == oir::CmpPred::NE;
}

bool is_gvn_candidate(const oir::Instruction &inst) {
    switch (inst.op()) {
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::Mul:
    case oir::Instruction::OpID::SDiv:
    case oir::Instruction::OpID::SRem:
    case oir::Instruction::OpID::FAdd:
    case oir::Instruction::OpID::FSub:
    case oir::Instruction::OpID::FMul:
    case oir::Instruction::OpID::FDiv:
    case oir::Instruction::OpID::ICmp:
    case oir::Instruction::OpID::FCmp:
    case oir::Instruction::OpID::GetElementPtr:
    case oir::Instruction::OpID::ZExt:
    case oir::Instruction::OpID::SIToFP:
    case oir::Instruction::OpID::FPToSI:
        return true;
    case oir::Instruction::OpID::Ret:
    case oir::Instruction::OpID::Br:
    case oir::Instruction::OpID::Alloca:
    case oir::Instruction::OpID::Load:
    case oir::Instruction::OpID::Store:
    case oir::Instruction::OpID::Call:
    case oir::Instruction::OpID::Phi:
        return false;
    }
    return false;
}

std::string expression_key(const ReplacementMap &replacements, oir::Instruction &inst) {
    std::ostringstream key;
    key << static_cast<int>(inst.op()) << ":" << ptr_key(inst.type()) << ":";

    if (auto *binary = dynamic_cast<oir::BinaryInst *>(&inst)) {
        auto lhs = value_key(replacements, binary->lhs());
        auto rhs = value_key(replacements, binary->rhs());
        if (is_commutative_integer_op(inst.op()) && rhs < lhs) {
            std::swap(lhs, rhs);
        }
        key << lhs << ":" << rhs;
        return key.str();
    }

    if (auto *cmp = dynamic_cast<oir::CmpInst *>(&inst)) {
        auto lhs = value_key(replacements, cmp->lhs());
        auto rhs = value_key(replacements, cmp->rhs());
        if (inst.op() == oir::Instruction::OpID::ICmp && is_commutative_equality(cmp->pred()) &&
            rhs < lhs) {
            std::swap(lhs, rhs);
        }
        key << static_cast<int>(cmp->pred()) << ":" << lhs << ":" << rhs;
        return key.str();
    }

    if (auto *cast = dynamic_cast<oir::CastInst *>(&inst)) {
        key << value_key(replacements, cast->src());
        return key.str();
    }

    if (auto *gep = dynamic_cast<oir::GetElementPtrInst *>(&inst)) {
        key << value_key(replacements, gep->base_ptr());
        for (auto *index : gep->indices()) {
            key << ":" << value_key(replacements, index);
        }
        return key.str();
    }

    return "";
}

class ScopedValueTable final {
  public:
    oir::Value *find(const std::string &key) const {
        auto found = table_.find(key);
        if (found == table_.end() || found->second.empty()) {
            return nullptr;
        }
        return found->second.back();
    }

    void push(const std::string &key, oir::Value *value) {
        table_[key].push_back(value);
        stack_.push_back(key);
    }

    std::size_t mark() const {
        return stack_.size();
    }

    void pop_to(std::size_t mark) {
        while (stack_.size() > mark) {
            auto key = stack_.back();
            stack_.pop_back();
            auto found = table_.find(key);
            if (found == table_.end()) {
                continue;
            }
            found->second.pop_back();
            if (found->second.empty()) {
                table_.erase(found);
            }
        }
    }

  private:
    std::unordered_map<std::string, std::vector<oir::Value *>> table_;
    std::vector<std::string> stack_;
};

struct AvailableMemoryValue {
    std::string key;
    oir::Value *ptr = nullptr;
    oir::Value *value = nullptr;
};

using MemoryTable = std::vector<AvailableMemoryValue>;

oir::Value *find_available_memory_value(const MemoryTable &memory, const std::string &key,
                                        oir::Type *type) {
    for (auto it = memory.rbegin(); it != memory.rend(); ++it) {
        if (it->key == key && it->value != nullptr && it->value->type() == type) {
            return it->value;
        }
    }
    return nullptr;
}

void invalidate_aliasing_memory(MemoryTable &memory, oir::Value *ptr,
                                const oir::OIRAliasAnalysis &alias_analysis) {
    memory.erase(std::remove_if(memory.begin(), memory.end(),
                                [ptr, &alias_analysis](const AvailableMemoryValue &entry) {
                                    return alias_analysis.alias(ptr, entry.ptr) !=
                                           oir::AliasResult::NoAlias;
                                }),
                 memory.end());
}

void invalidate_call_clobbered_memory(MemoryTable &memory, const oir::CallInst &call,
                                      const oir::OIRAliasAnalysis &alias_analysis,
                                      const oir::FunctionModRefAnalysis &modref) {
    memory.erase(std::remove_if(memory.begin(), memory.end(),
                                [&](const AvailableMemoryValue &entry) {
                                    return modref.call_may_clobber(call, entry.ptr,
                                                                   alias_analysis);
                                }),
                 memory.end());
}

void rewrite_operands(oir::Instruction &inst, const ReplacementMap &replacements) {
    for (std::size_t i = 0; i < inst.operand_count(); ++i) {
        auto *old_operand = inst.operand(i);
        auto *new_operand = resolve_value(replacements, old_operand);
        if (new_operand != old_operand && new_operand->type() == old_operand->type()) {
            inst.set_operand(i, new_operand);
        }
    }
}

void number_block(const oir::DominatorTree &dom_tree, const oir::BasicBlock *block,
                  ScopedValueTable &table, MemoryTable memory, ReplacementMap &replacements,
                  const oir::OIRAliasAnalysis &alias_analysis,
                  const oir::FunctionModRefAnalysis &modref) {
    auto mark = table.mark();
    for (auto &inst_ptr : const_cast<oir::BasicBlock *>(block)->instructions()) {
        auto *inst = inst_ptr.get();
        rewrite_operands(*inst, replacements);

        if (auto *load = dynamic_cast<oir::LoadInst *>(inst)) {
            auto key = value_key(replacements, load->ptr());
            if (auto *available = find_available_memory_value(memory, key, load->type())) {
                replacements[load] = available;
                continue;
            }
            memory.push_back({key, load->ptr(), load});
            continue;
        }

        if (auto *store = dynamic_cast<oir::StoreInst *>(inst)) {
            invalidate_aliasing_memory(memory, store->ptr(), alias_analysis);
            memory.push_back({value_key(replacements, store->ptr()), store->ptr(), store->value()});
            continue;
        }

        if (auto *call = dynamic_cast<oir::CallInst *>(inst)) {
            invalidate_call_clobbered_memory(memory, *call, alias_analysis, modref);
        }

        if (!is_gvn_candidate(*inst) || inst->type() == nullptr || inst->type()->is_void()) {
            continue;
        }

        auto key = expression_key(replacements, *inst);
        if (key.empty()) {
            continue;
        }

        auto *leader = table.find(key);
        if (leader != nullptr && leader->type() == inst->type()) {
            replacements[inst] = leader;
            continue;
        }
        table.push(key, inst);
    }

    for (auto *child : dom_tree.children(block)) {
        MemoryTable child_memory;
        if (child->predecessors().size() == 1 && child->predecessors().front() == block) {
            child_memory = memory;
        }
        number_block(dom_tree, child, table, child_memory, replacements, alias_analysis, modref);
    }
    table.pop_to(mark);
}

bool run_on_function(oir::Module &module, oir::Function &function, Stats &stats) {
    if (function.is_external() || function.entry_block() == nullptr) {
        return false;
    }

    std::size_t block_count = 0;
    std::size_t instruction_count = 0;
    for (const auto &block : function.blocks()) {
        ++block_count;
        instruction_count += block->instructions().size();
    }

    if (block_count > 1000 || instruction_count > 8000) {
        ReplacementMap replacements;
        oir::OIRAliasAnalysis alias_analysis;
        oir::FunctionModRefAnalysis modref(module);
        for (auto &block : function.blocks()) {
            ScopedValueTable table;
            MemoryTable memory;
            for (auto &inst_ptr : block->instructions()) {
                auto *inst = inst_ptr.get();
                rewrite_operands(*inst, replacements);

                if (auto *load = dynamic_cast<oir::LoadInst *>(inst)) {
                    auto key = value_key(replacements, load->ptr());
                    if (auto *available = find_available_memory_value(memory, key, load->type())) {
                        replacements[load] = available;
                        continue;
                    }
                    memory.push_back({key, load->ptr(), load});
                    continue;
                }

                if (auto *store = dynamic_cast<oir::StoreInst *>(inst)) {
                    invalidate_aliasing_memory(memory, store->ptr(), alias_analysis);
                    memory.push_back(
                        {value_key(replacements, store->ptr()), store->ptr(), store->value()});
                    continue;
                }

                if (auto *call = dynamic_cast<oir::CallInst *>(inst)) {
                    invalidate_call_clobbered_memory(memory, *call, alias_analysis, modref);
                }

                if (!is_gvn_candidate(*inst) || inst->type() == nullptr ||
                    inst->type()->is_void()) {
                    continue;
                }

                auto key = expression_key(replacements, *inst);
                if (key.empty()) {
                    continue;
                }
                auto *leader = table.find(key);
                if (leader != nullptr && leader->type() == inst->type()) {
                    replacements[inst] = leader;
                    continue;
                }
                table.push(key, inst);
            }
        }
        if (apply_replacements(module, replacements) == 0) {
            return false;
        }
        stats.gvn += static_cast<unsigned>(replacements.size());
        return true;
    }

    oir::DominatorTree dom_tree(function);
    ScopedValueTable table;
    MemoryTable memory;
    ReplacementMap replacements;
    oir::OIRAliasAnalysis alias_analysis;
    oir::FunctionModRefAnalysis modref(module);
    number_block(dom_tree, function.entry_block(), table, memory, replacements, alias_analysis,
                 modref);
    if (apply_replacements(module, replacements) == 0) {
        return false;
    }
    stats.gvn += static_cast<unsigned>(replacements.size());
    return true;
}

} // namespace

bool global_value_numbering(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        changed |= run_on_function(module, *function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt
