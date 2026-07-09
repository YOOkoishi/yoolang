#include "pass/oir/OIRToMIRCommon.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pass::oir_to_mir {
namespace {

class VRegLowerer final {
  public:
    std::unique_ptr<mir::Module> lower(const oir::Module &module) {
        module_ = std::make_unique<mir::Module>(module.name() + ".mir");

        for (const auto &global : module.globals()) {
            mir::Global lowered;
            lowered.name = global->name();
            lowered.type = type_info(global->value_type());
            lowered.is_const = global->is_const();
            lowered.initializer = global->initializer_literal();
            module_->add_global(std::move(lowered));
        }

        for (const auto &function : module.functions()) {
            std::vector<mir::TypeInfo> params;
            params.reserve(function->function_type()->param_types().size());
            for (auto *param : function->function_type()->param_types()) {
                params.push_back(type_info(param));
            }
            auto *out =
                module_->create_function(function->name(), type_info(function->return_type()),
                                         std::move(params), function->is_external());
            functions_[function.get()] = out;
        }

        for (const auto &function : module.functions()) {
            if (!function->is_external()) {
                lower_function(*function, *functions_.at(function.get()));
            }
        }

        return std::move(module_);
    }

  private:
    mir::TypeInfo type_info(oir::Type *type) const {
        mir::TypeInfo out;
        out.ir = type == nullptr ? "void" : type->print();
        out.align = 1;

        if (type == nullptr || type->is_void()) {
            out.value_type = mir::ValueType::Void;
            out.size = 0;
            return out;
        }
        if (auto *integer = dynamic_cast<oir::IntegerType *>(type)) {
            out.value_type = integer->bit_width() == 1 ? mir::ValueType::I1 : mir::ValueType::I32;
            out.size = 4;
            out.align = 4;
            return out;
        }
        if (type->is_float()) {
            out.value_type = mir::ValueType::F32;
            out.size = 4;
            out.align = 4;
            return out;
        }
        if (type->is_pointer()) {
            out.value_type = mir::ValueType::Ptr;
            out.size = 8;
            out.align = 8;
            return out;
        }
        if (type->is_function()) {
            out.value_type = mir::ValueType::Ptr;
            out.size = 8;
            out.align = 8;
            return out;
        }
        if (auto *array = dynamic_cast<oir::ArrayType *>(type)) {
            auto element = type_info(array->element_type());
            out.value_type = mir::ValueType::Aggregate;
            out.size = element.size * array->element_count();
            out.align = element.align;
            return out;
        }
        throw std::runtime_error("unsupported OIR type for MIR: " + type->print());
    }

    mir::RegisterClass reg_class_for(mir::ValueType type) const {
        return type == mir::ValueType::F32 ? mir::RegisterClass::FPR32 : mir::RegisterClass::GPR;
    }

    mir::Register create_vreg(mir::ValueType type) {
        return current_function_->regs().create_virtual(reg_class_for(type), type);
    }

    bool fits_simm12(std::int64_t value) const {
        return value >= -2048 && value <= 2047;
    }

    bool neg_fits_simm12(std::int64_t value) const {
        return value >= -2047 && value <= 2048;
    }

    std::uint64_t abs_u64(std::int64_t value) const {
        if (value >= 0) {
            return static_cast<std::uint64_t>(value);
        }
        return static_cast<std::uint64_t>(-(value + 1)) + 1;
    }

    const oir::ConstantInt *constant_int(const oir::Value *value) const {
        return dynamic_cast<const oir::ConstantInt *>(value);
    }

    struct IntRange {
        bool valid = false;
        std::int64_t lower = 0;
        std::int64_t upper = 0;

        bool operator==(const IntRange &other) const {
            return valid == other.valid &&
                   (!valid || (lower == other.lower && upper == other.upper));
        }

        bool operator!=(const IntRange &other) const {
            return !(*this == other);
        }
    };

    using RangeMap = std::unordered_map<const oir::Value *, IntRange>;

    static constexpr std::int64_t kI32Min = std::numeric_limits<std::int32_t>::min();
    static constexpr std::int64_t kI32Max = std::numeric_limits<std::int32_t>::max();

    bool is_integer_value(const oir::Value *value) const {
        return value != nullptr && value->type() != nullptr && value->type()->is_integer();
    }

    std::pair<std::int64_t, std::int64_t> integer_bounds(const oir::Type *type) const {
        if (auto *integer = dynamic_cast<const oir::IntegerType *>(type)) {
            if (integer->bit_width() == 1) {
                return {0, 1};
            }
        }
        return {kI32Min, kI32Max};
    }

    IntRange full_range_for_type(const oir::Type *type) const {
        if (type == nullptr || !type->is_integer()) {
            return {};
        }
        auto [lower, upper] = integer_bounds(type);
        return {true, lower, upper};
    }

    IntRange make_range_for_type(const oir::Type *type, std::int64_t lower,
                                 std::int64_t upper) const {
        if (type == nullptr || !type->is_integer()) {
            return {};
        }
        auto [type_min, type_max] = integer_bounds(type);
        lower = std::max(lower, type_min);
        upper = std::min(upper, type_max);
        if (lower > upper) {
            return {};
        }
        return {true, lower, upper};
    }

    bool is_full_range_for_type(const IntRange &range, const oir::Type *type) const {
        if (!range.valid || type == nullptr || !type->is_integer()) {
            return false;
        }
        auto [lower, upper] = integer_bounds(type);
        return range.lower == lower && range.upper == upper;
    }

    IntRange constant_range(const oir::ConstantInt &constant) const {
        return make_range_for_type(constant.type(), constant.value(), constant.value());
    }

    IntRange union_ranges(const IntRange &lhs, const IntRange &rhs,
                          const oir::Type *type) const {
        if (!lhs.valid) {
            return rhs;
        }
        if (!rhs.valid) {
            return lhs;
        }
        return make_range_for_type(type, std::min(lhs.lower, rhs.lower),
                                   std::max(lhs.upper, rhs.upper));
    }

    IntRange intersect_ranges(const IntRange &lhs, const IntRange &rhs,
                              const oir::Type *type) const {
        if (!lhs.valid || !rhs.valid) {
            return {};
        }
        return make_range_for_type(type, std::max(lhs.lower, rhs.lower),
                                   std::min(lhs.upper, rhs.upper));
    }

    IntRange range_for_value(const oir::Value *value, const RangeMap *local,
                             bool bottom_for_unseen_inst) const {
        if (!is_integer_value(value)) {
            return {};
        }
        if (auto *constant = dynamic_cast<const oir::ConstantInt *>(value)) {
            return constant_range(*constant);
        }
        if (dynamic_cast<const oir::ConstantZero *>(value) != nullptr) {
            return make_range_for_type(value->type(), 0, 0);
        }
        if (local != nullptr) {
            auto found = local->find(value);
            if (found != local->end()) {
                return found->second;
            }
        }
        if (auto *inst = dynamic_cast<const oir::Instruction *>(value)) {
            auto found = value_ranges_.find(inst);
            if (found != value_ranges_.end()) {
                return found->second;
            }
            if (bottom_for_unseen_inst) {
                return {};
            }
        }
        return full_range_for_type(value->type());
    }

    IntRange range_for_lowering(const oir::Value *value) const {
        auto range = range_for_value(value, &current_ranges_, false);
        return range.valid ? range : full_range_for_type(value == nullptr ? nullptr : value->type());
    }

    bool value_is_nonnegative(const oir::Value *value) const {
        auto range = range_for_lowering(value);
        return range.valid && range.lower >= 0;
    }

    bool exact_constant_range(const IntRange &range, std::int64_t *value = nullptr) const {
        if (!range.valid || range.lower != range.upper) {
            return false;
        }
        if (value != nullptr) {
            *value = range.lower;
        }
        return true;
    }

    IntRange eval_binary_range(const oir::BinaryInst &inst, const RangeMap *local,
                               bool bottom_for_unseen_inst) const {
        auto lhs = range_for_value(inst.lhs(), local, bottom_for_unseen_inst);
        auto rhs = range_for_value(inst.rhs(), local, bottom_for_unseen_inst);
        if (!lhs.valid || !rhs.valid) {
            return {};
        }

        switch (inst.op()) {
        case oir::Instruction::OpID::Add:
            return make_range_for_type(inst.type(), lhs.lower + rhs.lower,
                                       lhs.upper + rhs.upper);
        case oir::Instruction::OpID::Sub:
            return make_range_for_type(inst.type(), lhs.lower - rhs.upper,
                                       lhs.upper - rhs.lower);
        case oir::Instruction::OpID::Mul: {
            std::int64_t products[] = {lhs.lower * rhs.lower, lhs.lower * rhs.upper,
                                       lhs.upper * rhs.lower, lhs.upper * rhs.upper};
            auto [min_it, max_it] = std::minmax_element(products, products + 4);
            return make_range_for_type(inst.type(), *min_it, *max_it);
        }
        case oir::Instruction::OpID::And: {
            std::int64_t mask = 0;
            if (exact_constant_range(rhs, &mask) && mask >= 0) {
                return make_range_for_type(inst.type(), 0, mask);
            }
            if (exact_constant_range(lhs, &mask) && mask >= 0) {
                return make_range_for_type(inst.type(), 0, mask);
            }
            return full_range_for_type(inst.type());
        }
        case oir::Instruction::OpID::Xor:
            return full_range_for_type(inst.type());
        case oir::Instruction::OpID::SDiv: {
            std::int64_t divisor = 0;
            if (!exact_constant_range(rhs, &divisor) || divisor == 0) {
                return full_range_for_type(inst.type());
            }
            if (lhs.lower == kI32Min && divisor == -1) {
                return full_range_for_type(inst.type());
            }
            auto first = lhs.lower / divisor;
            auto second = lhs.upper / divisor;
            return make_range_for_type(inst.type(), std::min(first, second),
                                       std::max(first, second));
        }
        case oir::Instruction::OpID::SRem: {
            std::int64_t divisor = 0;
            if (!exact_constant_range(rhs, &divisor) || divisor == 0) {
                return full_range_for_type(inst.type());
            }
            auto magnitude = static_cast<std::int64_t>(abs_u64(divisor));
            if (magnitude <= 1) {
                return make_range_for_type(inst.type(), 0, 0);
            }
            if (lhs.lower >= 0) {
                return make_range_for_type(inst.type(), 0, magnitude - 1);
            }
            if (lhs.upper <= 0) {
                return make_range_for_type(inst.type(), 1 - magnitude, 0);
            }
            return make_range_for_type(inst.type(), 1 - magnitude, magnitude - 1);
        }
        default:
            return full_range_for_type(inst.type());
        }
    }

    IntRange evaluate_instruction_range(const oir::Instruction &inst, const RangeMap *local,
                                        bool bottom_for_unseen_inst) const {
        if (!is_integer_value(&inst)) {
            return {};
        }

        switch (inst.op()) {
        case oir::Instruction::OpID::Phi: {
            auto &phi = static_cast<const oir::PhiInst &>(inst);
            IntRange out;
            for (const auto &[value, pred] : phi.incoming()) {
                (void)pred;
                out = union_ranges(out, range_for_value(value, local, bottom_for_unseen_inst),
                                   inst.type());
            }
            return out.valid ? out : full_range_for_type(inst.type());
        }
        case oir::Instruction::OpID::Add:
        case oir::Instruction::OpID::Sub:
        case oir::Instruction::OpID::Mul:
        case oir::Instruction::OpID::And:
        case oir::Instruction::OpID::Xor:
        case oir::Instruction::OpID::SDiv:
        case oir::Instruction::OpID::SRem:
            return eval_binary_range(static_cast<const oir::BinaryInst &>(inst), local,
                                     bottom_for_unseen_inst);
        case oir::Instruction::OpID::ICmp:
            return make_range_for_type(inst.type(), 0, 1);
        case oir::Instruction::OpID::ZExt: {
            auto &cast = static_cast<const oir::CastInst &>(inst);
            auto src = range_for_value(cast.src(), local, bottom_for_unseen_inst);
            if (!src.valid) {
                return full_range_for_type(inst.type());
            }
            return make_range_for_type(inst.type(), std::max<std::int64_t>(0, src.lower),
                                       std::max<std::int64_t>(0, src.upper));
        }
        case oir::Instruction::OpID::Load:
        case oir::Instruction::OpID::Call:
        case oir::Instruction::OpID::MemZero:
        case oir::Instruction::OpID::FPToSI:
            return full_range_for_type(inst.type());
        default:
            return full_range_for_type(inst.type());
        }
    }

    void store_range_override(RangeMap &map, const oir::Value *value, const IntRange &range) const {
        if (!is_integer_value(value) || !range.valid) {
            map.erase(value);
            return;
        }
        auto global = range_for_value(value, nullptr, false);
        if (range == global || is_full_range_for_type(range, value->type())) {
            map.erase(value);
            return;
        }
        map[value] = range;
    }

    void analyze_global_ranges(const oir::Function &function) {
        value_ranges_.clear();
        for (const auto &block : function.blocks()) {
            for (const auto &inst : block->instructions()) {
                if (is_integer_value(inst.get())) {
                    value_ranges_[inst.get()] = {};
                }
            }
        }

        constexpr unsigned kMaxIterations = 128;
        for (unsigned iteration = 0; iteration < kMaxIterations; ++iteration) {
            bool changed = false;
            for (const auto &block : function.blocks()) {
                for (const auto &inst : block->instructions()) {
                    if (!is_integer_value(inst.get())) {
                        continue;
                    }
                    auto next = evaluate_instruction_range(*inst, nullptr, true);
                    auto &slot = value_ranges_[inst.get()];
                    if (next != slot) {
                        slot = next;
                        changed = true;
                    }
                }
            }
            if (!changed) {
                break;
            }
        }
    }

    oir::CmpPred invert_predicate(oir::CmpPred pred) const {
        switch (pred) {
        case oir::CmpPred::EQ:
            return oir::CmpPred::NE;
        case oir::CmpPred::NE:
            return oir::CmpPred::EQ;
        case oir::CmpPred::LT:
            return oir::CmpPred::GE;
        case oir::CmpPred::LE:
            return oir::CmpPred::GT;
        case oir::CmpPred::GT:
            return oir::CmpPred::LE;
        case oir::CmpPred::GE:
            return oir::CmpPred::LT;
        }
        return pred;
    }

    oir::CmpPred swap_predicate(oir::CmpPred pred) const {
        switch (pred) {
        case oir::CmpPred::LT:
            return oir::CmpPred::GT;
        case oir::CmpPred::LE:
            return oir::CmpPred::GE;
        case oir::CmpPred::GT:
            return oir::CmpPred::LT;
        case oir::CmpPred::GE:
            return oir::CmpPred::LE;
        case oir::CmpPred::EQ:
        case oir::CmpPred::NE:
            return pred;
        }
        return pred;
    }

    void refine_value_range(RangeMap &map, const oir::Value *value,
                            const IntRange &constraint) const {
        if (!is_integer_value(value) || !constraint.valid) {
            return;
        }
        auto current = range_for_value(value, &map, false);
        auto refined = intersect_ranges(current, constraint, value->type());
        store_range_override(map, value, refined);
    }

    void apply_icmp_constraint(RangeMap &map, const oir::CmpInst &cmp,
                               bool branch_taken) const {
        auto pred = cmp.pred();
        if (!branch_taken) {
            pred = invert_predicate(pred);
        }

        const oir::Value *value = cmp.lhs();
        auto *constant = constant_int(cmp.rhs());
        if (constant == nullptr) {
            constant = constant_int(cmp.lhs());
            value = cmp.rhs();
            pred = swap_predicate(pred);
        }
        if (constant == nullptr || !is_integer_value(value)) {
            return;
        }

        auto [type_min, type_max] = integer_bounds(value->type());
        const auto c = constant->value();
        IntRange constraint;
        switch (pred) {
        case oir::CmpPred::EQ:
            if (c < type_min || c > type_max) {
                return;
            }
            constraint = make_range_for_type(value->type(), c, c);
            break;
        case oir::CmpPred::LT:
            if (c <= type_min) {
                return;
            }
            constraint = make_range_for_type(value->type(), type_min, c - 1);
            break;
        case oir::CmpPred::LE:
            if (c < type_min) {
                return;
            }
            constraint = make_range_for_type(value->type(), type_min, c);
            break;
        case oir::CmpPred::GT:
            if (c >= type_max) {
                return;
            }
            constraint = make_range_for_type(value->type(), c + 1, type_max);
            break;
        case oir::CmpPred::GE:
            if (c > type_max) {
                return;
            }
            constraint = make_range_for_type(value->type(), c, type_max);
            break;
        case oir::CmpPred::NE:
            return;
        }
        refine_value_range(map, value, constraint);
    }

    void apply_branch_constraint(RangeMap &map, const oir::BranchInst &branch,
                                 const oir::BasicBlock *succ) const {
        if (!branch.is_conditional()) {
            return;
        }
        auto *cmp = dynamic_cast<const oir::CmpInst *>(branch.cond());
        if (cmp == nullptr) {
            return;
        }
        if (succ == branch.true_bb()) {
            apply_icmp_constraint(map, *cmp, true);
        } else if (succ == branch.false_bb()) {
            apply_icmp_constraint(map, *cmp, false);
        }
    }

    void merge_successor_entry(
        const oir::BasicBlock *succ, const RangeMap &candidate,
        std::unordered_map<const oir::BasicBlock *, RangeMap> &next_entries,
        std::unordered_set<const oir::BasicBlock *> &seen) const {
        auto [seen_it, first] = seen.insert(succ);
        auto &entry = next_entries[succ];
        if (first) {
            for (const auto &[value, range] : candidate) {
                store_range_override(entry, value, range);
            }
            return;
        }

        std::unordered_set<const oir::Value *> keys;
        for (const auto &[value, range] : entry) {
            (void)range;
            keys.insert(value);
        }
        for (const auto &[value, range] : candidate) {
            (void)range;
            keys.insert(value);
        }

        RangeMap merged;
        for (auto *value : keys) {
            auto lhs = range_for_value(value, &entry, false);
            auto rhs = range_for_value(value, &candidate, false);
            auto joined = union_ranges(lhs, rhs, value->type());
            store_range_override(merged, value, joined);
        }
        entry = std::move(merged);
    }

    void analyze_block_ranges(const oir::Function &function) {
        block_entry_ranges_.clear();
        for (const auto &block : function.blocks()) {
            block_entry_ranges_[block.get()] = {};
        }

        constexpr unsigned kMaxIterations = 64;
        for (unsigned iteration = 0; iteration < kMaxIterations; ++iteration) {
            std::unordered_map<const oir::BasicBlock *, RangeMap> next_entries;
            std::unordered_set<const oir::BasicBlock *> seen;

            for (const auto &block_ptr : function.blocks()) {
                auto *block = block_ptr.get();
                RangeMap local = block_entry_ranges_[block];
                for (const auto &inst : block->instructions()) {
                    if (!is_integer_value(inst.get())) {
                        continue;
                    }
                    auto range = evaluate_instruction_range(*inst, &local, false);
                    store_range_override(local, inst.get(), range);
                }

                auto *term = block->terminator();
                for (auto *succ : block->successors()) {
                    RangeMap candidate = local;
                    if (auto *branch = dynamic_cast<const oir::BranchInst *>(term)) {
                        apply_branch_constraint(candidate, *branch, succ);
                    }
                    merge_successor_entry(succ, candidate, next_entries, seen);
                }
            }

            bool changed = false;
            for (const auto &block : function.blocks()) {
                auto *raw = block.get();
                auto found = next_entries.find(raw);
                RangeMap next = found == next_entries.end() ? RangeMap{} : std::move(found->second);
                if (block_entry_ranges_[raw] != next) {
                    block_entry_ranges_[raw] = std::move(next);
                    changed = true;
                }
            }
            if (!changed) {
                break;
            }
        }
    }

    void analyze_value_ranges(const oir::Function &function) {
        analyze_global_ranges(function);
        analyze_block_ranges(function);
    }

    mir::Register zero_reg() const {
        return phys_gpr("zero");
    }

    void lower_function(const oir::Function &function, mir::MachineFunction &out) {
        current_function_ = &out;
        value_regs_.clear();
        alloca_slots_.clear();
        blocks_.clear();
        edge_blocks_.clear();
        pending_edge_blocks_.clear();
        value_ranges_.clear();
        block_entry_ranges_.clear();
        current_ranges_.clear();
        temp_index_ = 0;

        analyze_value_ranges(function);

        for (const auto &block : function.blocks()) {
            blocks_[block.get()] = out.create_block(block->name());
        }

        for (const auto &arg : function.args()) {
            auto type = type_info(arg->type()).value_type;
            value_regs_[arg.get()] = create_vreg(type);
        }

        for (const auto &block : function.blocks()) {
            for (const auto &inst : block->instructions()) {
                preallocate_result(*inst);
            }
        }

        create_phi_edge_blocks(function, out);
        emit_parameter_copies(function);

        for (const auto &block : function.blocks()) {
            current_block_ = blocks_.at(block.get());
            current_ranges_ = block_entry_ranges_[block.get()];
            emit_block_entry_phi_copies(*block);
            for (const auto &inst : block->instructions()) {
                lower_instruction(*inst);
                if (is_integer_value(inst.get())) {
                    auto range = evaluate_instruction_range(*inst, &current_ranges_, false);
                    store_range_override(current_ranges_, inst.get(), range);
                }
            }
        }

        fill_phi_edge_blocks();
        out.rebuild_cfg();
        current_function_ = nullptr;
        current_block_ = nullptr;
        current_ranges_.clear();
    }

    void preallocate_result(const oir::Instruction &inst) {
        if (auto *alloca = dynamic_cast<const oir::AllocaInst *>(&inst)) {
            std::string object_name =
                inst.name().empty() ? slot_name(inst, "alloca.obj") : inst.name() + ".obj";
            alloca_slots_[&inst] = current_function_->add_stack_slot(
                std::move(object_name), type_info(alloca->allocated_type()),
                mir::StackSlotKind::Alloca);
        }

        if (inst.type() == nullptr || inst.type()->is_void()) {
            return;
        }
        auto type = type_info(inst.type()).value_type;
        if (type != mir::ValueType::Aggregate) {
            value_regs_[&inst] = create_vreg(type);
        }
    }

    std::string slot_name(const oir::Value &value, const std::string &fallback) const {
        if (!value.name().empty()) {
            return value.name();
        }
        return fallback + "." + std::to_string(temp_index_);
    }

    void create_phi_edge_blocks(const oir::Function &function, mir::MachineFunction &out) {
        for (const auto &target_ptr : function.blocks()) {
            const auto *target = target_ptr.get();
            if (!block_has_phi(*target)) {
                continue;
            }
            for (auto *pred : target->predecessors()) {
                if (!needs_dedicated_phi_edge(pred, target)) {
                    continue;
                }
                std::string name = "edge." + pred->name() + ".to." + target->name();
                auto *edge = out.create_block(name);
                edge_blocks_[edge_key(pred, target)] = edge;
                pending_edge_blocks_.push_back({pred, target, edge});
            }
        }
    }

    bool block_has_phi(const oir::BasicBlock &block) const {
        for (const auto &inst : block.instructions()) {
            if (inst->op() == oir::Instruction::OpID::Phi) {
                return true;
            }
            return false;
        }
        return false;
    }

    bool needs_dedicated_phi_edge(const oir::BasicBlock *pred,
                                  const oir::BasicBlock *target) const {
        const bool critical = pred->successors().size() > 1 && target->predecessors().size() > 1;
        const bool conditional_self_edge = pred == target && pred->successors().size() > 1;
        return critical || conditional_self_edge;
    }

    bool has_edge_block(const oir::BasicBlock *pred, const oir::BasicBlock *target) const {
        return edge_blocks_.find(edge_key(pred, target)) != edge_blocks_.end();
    }

    void emit_block_entry_phi_copies(const oir::BasicBlock &block) {
        if (!block_has_phi(block) || block.predecessors().size() != 1) {
            return;
        }

        auto *pred = block.predecessors().front();
        if (pred == &block || pred->successors().size() <= 1 || has_edge_block(pred, &block)) {
            return;
        }
        emit_phi_copies_for_edge(pred, &block);
    }

    void emit_parameter_copies(const oir::Function &function) {
        current_block_ = blocks_.at(function.entry_block());
        std::size_t int_reg = 0;
        std::size_t float_reg = 0;
        std::int64_t stack_offset = 0;

        for (const auto &arg : function.args()) {
            auto type = type_info(arg->type()).value_type;
            auto dst = value_regs_.at(arg.get());
            if (type == mir::ValueType::F32) {
                if (float_reg < kFArgRegs.size()) {
                    std::vector<mir::Register> future_regs;
                    for (std::size_t i = float_reg + 1; i < kFArgRegs.size(); ++i) {
                        future_regs.push_back(phys_fpr(kFArgRegs[i]));
                    }
                    emit_entry_move(dst, phys_fpr(kFArgRegs[float_reg++]), future_regs);
                } else {
                    emit(mir::Opcode::LoadIncomingArg,
                         {mir::MachineOperand::reg_def(dst), mir::MachineOperand::imm(stack_offset),
                          mir::MachineOperand::type(type)});
                    stack_offset += 8;
                }
            } else {
                if (int_reg < kArgRegs.size()) {
                    std::vector<mir::Register> future_regs;
                    for (std::size_t i = int_reg + 1; i < kArgRegs.size(); ++i) {
                        future_regs.push_back(phys_gpr(kArgRegs[i]));
                    }
                    emit_entry_move(dst, phys_gpr(kArgRegs[int_reg++]), future_regs);
                } else {
                    emit(mir::Opcode::LoadIncomingArg,
                         {mir::MachineOperand::reg_def(dst), mir::MachineOperand::imm(stack_offset),
                          mir::MachineOperand::type(type)});
                    stack_offset += 8;
                }
            }
        }
    }

    void lower_instruction(const oir::Instruction &inst) {
        if (inst.op() == oir::Instruction::OpID::Phi) {
            return;
        }

        switch (inst.op()) {
        case oir::Instruction::OpID::Alloca:
            lower_alloca(static_cast<const oir::AllocaInst &>(inst));
            break;
        case oir::Instruction::OpID::Load:
            lower_load(static_cast<const oir::LoadInst &>(inst));
            break;
        case oir::Instruction::OpID::Store:
            lower_store(static_cast<const oir::StoreInst &>(inst));
            break;
        case oir::Instruction::OpID::MemZero:
            lower_memzero(static_cast<const oir::MemZeroInst &>(inst));
            break;
        case oir::Instruction::OpID::GetElementPtr:
            lower_gep(static_cast<const oir::GetElementPtrInst &>(inst));
            break;
        case oir::Instruction::OpID::Add:
        case oir::Instruction::OpID::Sub:
        case oir::Instruction::OpID::Mul:
        case oir::Instruction::OpID::And:
        case oir::Instruction::OpID::Xor:
        case oir::Instruction::OpID::SDiv:
        case oir::Instruction::OpID::SRem:
            lower_int_binary(static_cast<const oir::BinaryInst &>(inst));
            break;
        case oir::Instruction::OpID::FAdd:
        case oir::Instruction::OpID::FSub:
        case oir::Instruction::OpID::FMul:
        case oir::Instruction::OpID::FDiv:
            lower_float_binary(static_cast<const oir::BinaryInst &>(inst));
            break;
        case oir::Instruction::OpID::ICmp:
            lower_icmp(static_cast<const oir::CmpInst &>(inst));
            break;
        case oir::Instruction::OpID::FCmp:
            lower_fcmp(static_cast<const oir::CmpInst &>(inst));
            break;
        case oir::Instruction::OpID::ZExt:
            lower_zext(static_cast<const oir::CastInst &>(inst));
            break;
        case oir::Instruction::OpID::SIToFP:
            lower_sitofp(static_cast<const oir::CastInst &>(inst));
            break;
        case oir::Instruction::OpID::FPToSI:
            lower_fptosi(static_cast<const oir::CastInst &>(inst));
            break;
        case oir::Instruction::OpID::Call:
            lower_call(static_cast<const oir::CallInst &>(inst));
            break;
        case oir::Instruction::OpID::Ret:
            lower_return(static_cast<const oir::ReturnInst &>(inst));
            break;
        case oir::Instruction::OpID::Br:
            lower_branch(static_cast<const oir::BranchInst &>(inst));
            break;
        case oir::Instruction::OpID::Phi:
            break;
        }
    }

    void lower_alloca(const oir::AllocaInst &inst) {
        auto dst = value_regs_.at(&inst);
        int object_slot = alloca_slots_.at(&inst);
        emit(mir::Opcode::LoadStackAddr,
             {mir::MachineOperand::reg_def(dst), mir::MachineOperand::slot(object_slot)});
    }

    void lower_load(const oir::LoadInst &inst) {
        auto loaded = type_info(inst.type());
        auto dst = value_regs_.at(&inst);
        auto addr = value_reg(inst.ptr());
        emit(mir::Opcode::LoadMem,
             {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(addr),
              mir::MachineOperand::type(loaded.value_type)});
    }

    void lower_store(const oir::StoreInst &inst) {
        auto stored = type_info(inst.value()->type());
        auto addr = value_reg(inst.ptr());
        if (stored.value_type == mir::ValueType::Aggregate) {
            if (dynamic_cast<const oir::ConstantZero *>(inst.value()) == nullptr) {
                throw std::runtime_error("only zero aggregate stores are supported in MIR vreg");
            }
            emit(mir::Opcode::MemZero,
                 {mir::MachineOperand::reg_use(addr),
                  mir::MachineOperand::imm(0),
                  mir::MachineOperand::imm(static_cast<std::int64_t>(stored.size))});
            if (static_cast<std::int64_t>(stored.size) >= mir::kMemZeroMemsetThresholdBytes) {
                current_function_->note_call();
            }
            return;
        }

        auto value = value_reg(inst.value());
        emit(mir::Opcode::StoreMem,
             {mir::MachineOperand::reg_use(addr), mir::MachineOperand::reg_use(value),
              mir::MachineOperand::type(stored.value_type)});
    }

    void lower_memzero(const oir::MemZeroInst &inst) {
        auto addr = value_reg(inst.ptr());
        auto *byte_value = constant_int(inst.byte_value());
        if (byte_value == nullptr || byte_value->value() < 0 || byte_value->value() > 255) {
            throw std::runtime_error("memzero byte value must be a constant in [0, 255]");
        }
        if (auto *constant = constant_int(inst.byte_count())) {
            if (constant->value() < 0) {
                throw std::runtime_error("memzero byte count must be non-negative");
            }
            emit(mir::Opcode::MemZero,
                 {mir::MachineOperand::reg_use(addr), mir::MachineOperand::imm(byte_value->value()),
                  mir::MachineOperand::imm(constant->value())});
            if (constant->value() >= mir::kMemZeroMemsetThresholdBytes) {
                current_function_->note_call();
            }
            return;
        }

        auto byte_count = value_reg(inst.byte_count());
        emit(mir::Opcode::MemZero,
             {mir::MachineOperand::reg_use(addr), mir::MachineOperand::imm(byte_value->value()),
              mir::MachineOperand::reg_use(byte_count)});
        current_function_->note_call();
    }

    void lower_gep(const oir::GetElementPtrInst &inst) {
        auto dst = value_regs_.at(&inst);
        auto acc = value_reg(inst.base_ptr());

        auto *ptr_type = dynamic_cast<oir::PointerType *>(inst.base_ptr()->type());
        if (ptr_type == nullptr) {
            throw std::runtime_error("gep base is not a pointer");
        }

        oir::Type *cursor = ptr_type->element_type();
        std::int64_t constant_offset = 0;
        auto indices = inst.indices();
        for (std::size_t i = 0; i < indices.size(); ++i) {
            std::uint64_t stride = 0;
            if (i == 0) {
                stride = type_info(cursor).size;
            } else if (auto *array = dynamic_cast<oir::ArrayType *>(cursor)) {
                stride = type_info(array->element_type()).size;
                cursor = array->element_type();
            } else {
                stride = type_info(cursor).size;
            }

            if (stride == 0) {
                continue;
            }

            if (auto *constant = dynamic_cast<oir::ConstantInt *>(indices[i])) {
                constant_offset += constant->value() * static_cast<std::int64_t>(stride);
                continue;
            }

            auto index = value_reg(indices[i]);
            mir::Register scaled = index;
            if (stride != 1) {
                scaled = create_vreg(mir::ValueType::I32);
                if (is_power_of_two(stride)) {
                    emit(mir::Opcode::SllI,
                         {mir::MachineOperand::reg_def(scaled), mir::MachineOperand::reg_use(index),
                          mir::MachineOperand::imm(log2_u64(stride))});
                } else {
                    auto stride_reg = create_vreg(mir::ValueType::I32);
                    emit(mir::Opcode::LoadImm,
                         {mir::MachineOperand::reg_def(stride_reg),
                          mir::MachineOperand::imm(static_cast<std::int64_t>(stride))});
                    emit(mir::Opcode::Mul,
                         {mir::MachineOperand::reg_def(scaled), mir::MachineOperand::reg_use(index),
                          mir::MachineOperand::reg_use(stride_reg)});
                }
            }

            auto next = create_vreg(mir::ValueType::Ptr);
            emit(mir::Opcode::Add,
                 {mir::MachineOperand::reg_def(next), mir::MachineOperand::reg_use(acc),
                  mir::MachineOperand::reg_use(scaled)});
            acc = next;
        }

        if (constant_offset != 0) {
            auto next = create_vreg(mir::ValueType::Ptr);
            if (fits_simm12(constant_offset)) {
                emit(mir::Opcode::AddI,
                     {mir::MachineOperand::reg_def(next), mir::MachineOperand::reg_use(acc),
                      mir::MachineOperand::imm(constant_offset)});
            } else {
                auto offset = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::LoadImm,
                     {mir::MachineOperand::reg_def(offset),
                      mir::MachineOperand::imm(constant_offset)});
                emit(mir::Opcode::Add,
                     {mir::MachineOperand::reg_def(next), mir::MachineOperand::reg_use(acc),
                      mir::MachineOperand::reg_use(offset)});
            }
            acc = next;
        }

        emit_move(dst, acc);
    }

    bool try_lower_const_mul(mir::Register dst, mir::Register value, std::int64_t constant) {
        if (constant == 0) {
            emit_move(dst, zero_reg());
            return true;
        }
        if (constant == 1) {
            emit_move(dst, value);
            return true;
        }
        if (constant == -1) {
            emit(mir::Opcode::SubW,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(zero_reg()),
                  mir::MachineOperand::reg_use(value)});
            return true;
        }

        auto magnitude = abs_u64(constant);
        if (is_power_of_two(magnitude) && log2_u64(magnitude) < 32) {
            auto shifted = constant < 0 ? create_vreg(mir::ValueType::I32) : dst;
            emit(mir::Opcode::SllIW,
                 {mir::MachineOperand::reg_def(shifted), mir::MachineOperand::reg_use(value),
                  mir::MachineOperand::imm(log2_u64(magnitude))});
            if (constant < 0) {
                emit(mir::Opcode::SubW,
                     {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(zero_reg()),
                      mir::MachineOperand::reg_use(shifted)});
            }
            return true;
        }

        return false;
    }

    void emit_signed_div_pow2(mir::Register dst, mir::Register value, unsigned shift,
                              bool negate_result) {
        auto sign = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::SraIW,
             {mir::MachineOperand::reg_def(sign), mir::MachineOperand::reg_use(value),
              mir::MachineOperand::imm(31)});
        emit(mir::Opcode::SrliW,
             {mir::MachineOperand::reg_def(sign), mir::MachineOperand::reg_use(sign),
              mir::MachineOperand::imm(32 - shift)});

        auto adjusted = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::AddW,
             {mir::MachineOperand::reg_def(adjusted), mir::MachineOperand::reg_use(value),
              mir::MachineOperand::reg_use(sign)});

        auto quotient = negate_result ? create_vreg(mir::ValueType::I32) : dst;
        emit(mir::Opcode::SraIW,
             {mir::MachineOperand::reg_def(quotient), mir::MachineOperand::reg_use(adjusted),
              mir::MachineOperand::imm(shift)});
        if (negate_result) {
            emit(mir::Opcode::SubW,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(zero_reg()),
                  mir::MachineOperand::reg_use(quotient)});
        }
    }

    struct SignedMagic {
        std::int32_t magic = 0;
        unsigned shift = 0;
    };

    SignedMagic signed_magic_info(std::int32_t divisor) const {
        const std::uint32_t ad =
            divisor < 0 ? static_cast<std::uint32_t>(-static_cast<std::int64_t>(divisor))
                        : static_cast<std::uint32_t>(divisor);
        const std::uint32_t two31 = 0x80000000U;
        const std::uint32_t sign = static_cast<std::uint32_t>(divisor) >> 31U;
        const std::uint32_t t = two31 + sign;
        const std::uint32_t anc = t - 1U - (t % ad);

        unsigned p = 31;
        std::uint32_t q1 = two31 / anc;
        std::uint32_t r1 = two31 - q1 * anc;
        std::uint32_t q2 = two31 / ad;
        std::uint32_t r2 = two31 - q2 * ad;
        std::uint32_t delta = 0;
        do {
            ++p;
            q1 <<= 1U;
            r1 <<= 1U;
            if (r1 >= anc) {
                ++q1;
                r1 -= anc;
            }
            q2 <<= 1U;
            r2 <<= 1U;
            if (r2 >= ad) {
                ++q2;
                r2 -= ad;
            }
            delta = ad - r2;
        } while (q1 < delta || (q1 == delta && r1 == 0));

        std::int64_t magic = static_cast<std::int64_t>(q2) + 1;
        if (divisor < 0) {
            magic = -magic;
        }
        return {static_cast<std::int32_t>(magic), p - 32U};
    }

    bool try_lower_magic_const_div(mir::Register dst, mir::Register value,
                                   std::int64_t constant) {
        if (constant == 0 || constant < std::numeric_limits<std::int32_t>::min() ||
            constant > std::numeric_limits<std::int32_t>::max() ||
            constant == std::numeric_limits<std::int32_t>::min()) {
            return false;
        }

        const auto divisor = static_cast<std::int32_t>(constant);
        auto magic = signed_magic_info(divisor);
        auto magic_reg = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::LoadImm,
             {mir::MachineOperand::reg_def(magic_reg), mir::MachineOperand::imm(magic.magic)});

        auto product = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::Mul,
             {mir::MachineOperand::reg_def(product), mir::MachineOperand::reg_use(value),
              mir::MachineOperand::reg_use(magic_reg)});

        auto quotient = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::SraI,
             {mir::MachineOperand::reg_def(quotient), mir::MachineOperand::reg_use(product),
              mir::MachineOperand::imm(32)});

        if (divisor > 0 && magic.magic < 0) {
            auto adjusted = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::AddW,
                 {mir::MachineOperand::reg_def(adjusted),
                  mir::MachineOperand::reg_use(quotient), mir::MachineOperand::reg_use(value)});
            quotient = adjusted;
        } else if (divisor < 0 && magic.magic > 0) {
            auto adjusted = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::SubW,
                 {mir::MachineOperand::reg_def(adjusted),
                  mir::MachineOperand::reg_use(quotient), mir::MachineOperand::reg_use(value)});
            quotient = adjusted;
        }

        if (magic.shift != 0) {
            auto shifted = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::SraIW,
                 {mir::MachineOperand::reg_def(shifted),
                  mir::MachineOperand::reg_use(quotient), mir::MachineOperand::imm(magic.shift)});
            quotient = shifted;
        }

        auto sign = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::SrliW,
             {mir::MachineOperand::reg_def(sign), mir::MachineOperand::reg_use(quotient),
              mir::MachineOperand::imm(31)});
        emit(mir::Opcode::AddW,
             {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(quotient),
              mir::MachineOperand::reg_use(sign)});
        return true;
    }

    bool try_lower_nonnegative_const_div(mir::Register dst, mir::Register value,
                                         std::int64_t constant) {
        if (constant == 1) {
            emit_move(dst, value);
            return true;
        }
        if (constant == -1) {
            emit(mir::Opcode::SubW,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(zero_reg()),
                  mir::MachineOperand::reg_use(value)});
            return true;
        }

        auto magnitude = abs_u64(constant);
        if (!is_power_of_two(magnitude)) {
            return false;
        }

        auto shift = log2_u64(magnitude);
        if (shift == 0 || shift > 31) {
            return false;
        }

        auto quotient = constant < 0 ? create_vreg(mir::ValueType::I32) : dst;
        emit(mir::Opcode::SrliW,
             {mir::MachineOperand::reg_def(quotient), mir::MachineOperand::reg_use(value),
              mir::MachineOperand::imm(shift)});
        if (constant < 0) {
            emit(mir::Opcode::SubW,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(zero_reg()),
                  mir::MachineOperand::reg_use(quotient)});
        }
        return true;
    }

    bool try_lower_const_div(mir::Register dst, mir::Register value, std::int64_t constant) {
        if (constant == 1) {
            emit_move(dst, value);
            return true;
        }
        if (constant == -1) {
            emit(mir::Opcode::SubW,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(zero_reg()),
                  mir::MachineOperand::reg_use(value)});
            return true;
        }

        auto magnitude = abs_u64(constant);
        if (is_power_of_two(magnitude)) {
            auto shift = log2_u64(magnitude);
            if (shift > 0 && shift < 31) {
                emit_signed_div_pow2(dst, value, shift, constant < 0);
                return true;
            }
        }

        return try_lower_magic_const_div(dst, value, constant);
    }

    bool try_lower_nonnegative_const_rem(mir::Register dst, mir::Register value,
                                         std::int64_t constant) {
        if (constant == 1 || constant == -1) {
            emit_move(dst, zero_reg());
            return true;
        }

        auto magnitude = abs_u64(constant);
        if (!is_power_of_two(magnitude)) {
            return false;
        }

        auto shift = log2_u64(magnitude);
        if (shift == 0 || shift > 31) {
            return false;
        }

        if (shift == 31) {
            emit_move(dst, value);
            return true;
        }

        const auto mask = static_cast<std::int64_t>(magnitude - 1);
        if (fits_simm12(mask)) {
            emit(mir::Opcode::AndI,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(value),
                  mir::MachineOperand::imm(mask)});
        } else {
            auto mask_reg = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::LoadImm,
                 {mir::MachineOperand::reg_def(mask_reg), mir::MachineOperand::imm(mask)});
            emit(mir::Opcode::And,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(value),
                  mir::MachineOperand::reg_use(mask_reg)});
        }
        return true;
    }

    bool try_lower_const_rem(mir::Register dst, mir::Register value, std::int64_t constant) {
        if (constant == 1 || constant == -1) {
            emit_move(dst, zero_reg());
            return true;
        }

        auto magnitude = abs_u64(constant);
        if (is_power_of_two(magnitude)) {
            auto shift = log2_u64(magnitude);
            if (shift > 0 && shift < 31) {
                auto bias = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::SraIW,
                     {mir::MachineOperand::reg_def(bias), mir::MachineOperand::reg_use(value),
                      mir::MachineOperand::imm(31)});
                emit(mir::Opcode::SrliW,
                     {mir::MachineOperand::reg_def(bias), mir::MachineOperand::reg_use(bias),
                      mir::MachineOperand::imm(32 - shift)});

                auto adjusted = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::AddW,
                     {mir::MachineOperand::reg_def(adjusted), mir::MachineOperand::reg_use(value),
                      mir::MachineOperand::reg_use(bias)});
                auto masked = create_vreg(mir::ValueType::I32);
                const auto mask = static_cast<std::int64_t>(magnitude - 1);
                if (fits_simm12(mask)) {
                    emit(mir::Opcode::AndI,
                         {mir::MachineOperand::reg_def(masked),
                          mir::MachineOperand::reg_use(adjusted), mir::MachineOperand::imm(mask)});
                } else {
                    auto mask_reg = create_vreg(mir::ValueType::I32);
                    emit(mir::Opcode::LoadImm,
                         {mir::MachineOperand::reg_def(mask_reg), mir::MachineOperand::imm(mask)});
                    emit(mir::Opcode::And,
                         {mir::MachineOperand::reg_def(masked),
                          mir::MachineOperand::reg_use(adjusted),
                          mir::MachineOperand::reg_use(mask_reg)});
                }
                emit(mir::Opcode::SubW,
                     {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(masked),
                      mir::MachineOperand::reg_use(bias)});
                return true;
            }
        }

        if (constant != 0 && constant != std::numeric_limits<std::int32_t>::min()) {
            auto quotient = create_vreg(mir::ValueType::I32);
            if (!try_lower_magic_const_div(quotient, value, constant)) {
                return false;
            }

            auto constant_reg = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::LoadImm,
                 {mir::MachineOperand::reg_def(constant_reg),
                  mir::MachineOperand::imm(static_cast<std::int32_t>(constant))});
            auto product = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::MulW,
                 {mir::MachineOperand::reg_def(product),
                  mir::MachineOperand::reg_use(quotient),
                  mir::MachineOperand::reg_use(constant_reg)});
            emit(mir::Opcode::SubW,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(value),
                  mir::MachineOperand::reg_use(product)});
            return true;
        }

        return false;
    }

    void lower_int_binary(const oir::BinaryInst &inst) {
        auto dst = value_regs_.at(&inst);

        auto *lhs_const = constant_int(inst.lhs());
        auto *rhs_const = constant_int(inst.rhs());

        if (inst.op() == oir::Instruction::OpID::Add) {
            if (rhs_const != nullptr && fits_simm12(rhs_const->value())) {
                emit(mir::Opcode::AddIW,
                     {mir::MachineOperand::reg_def(dst),
                      mir::MachineOperand::reg_use(value_reg(inst.lhs())),
                      mir::MachineOperand::imm(rhs_const->value())});
                return;
            }
            if (lhs_const != nullptr && fits_simm12(lhs_const->value())) {
                emit(mir::Opcode::AddIW,
                     {mir::MachineOperand::reg_def(dst),
                      mir::MachineOperand::reg_use(value_reg(inst.rhs())),
                      mir::MachineOperand::imm(lhs_const->value())});
                return;
            }
        } else if (inst.op() == oir::Instruction::OpID::Sub) {
            if (rhs_const != nullptr && neg_fits_simm12(rhs_const->value())) {
                emit(mir::Opcode::AddIW,
                     {mir::MachineOperand::reg_def(dst),
                      mir::MachineOperand::reg_use(value_reg(inst.lhs())),
                      mir::MachineOperand::imm(0 - rhs_const->value())});
                return;
            }
            if (lhs_const != nullptr && lhs_const->value() == 0) {
                emit(mir::Opcode::SubW,
                     {mir::MachineOperand::reg_def(dst),
                      mir::MachineOperand::reg_use(zero_reg()),
                      mir::MachineOperand::reg_use(value_reg(inst.rhs()))});
                return;
            }
        } else if (inst.op() == oir::Instruction::OpID::Mul) {
            if (rhs_const != nullptr && try_lower_const_mul(dst, value_reg(inst.lhs()),
                                                            rhs_const->value())) {
                return;
            }
            if (lhs_const != nullptr && try_lower_const_mul(dst, value_reg(inst.rhs()),
                                                            lhs_const->value())) {
                return;
            }
        } else if (inst.op() == oir::Instruction::OpID::And) {
            if (rhs_const != nullptr && fits_simm12(rhs_const->value())) {
                emit(mir::Opcode::AndI,
                     {mir::MachineOperand::reg_def(dst),
                      mir::MachineOperand::reg_use(value_reg(inst.lhs())),
                      mir::MachineOperand::imm(rhs_const->value())});
                return;
            }
            if (lhs_const != nullptr && fits_simm12(lhs_const->value())) {
                emit(mir::Opcode::AndI,
                     {mir::MachineOperand::reg_def(dst),
                      mir::MachineOperand::reg_use(value_reg(inst.rhs())),
                      mir::MachineOperand::imm(lhs_const->value())});
                return;
            }
        } else if (inst.op() == oir::Instruction::OpID::SDiv) {
            if (rhs_const != nullptr) {
                auto lhs = value_reg(inst.lhs());
                if (value_is_nonnegative(inst.lhs()) &&
                    try_lower_nonnegative_const_div(dst, lhs, rhs_const->value())) {
                    return;
                }
                if (try_lower_const_div(dst, lhs, rhs_const->value())) {
                    return;
                }
            }
        } else if (inst.op() == oir::Instruction::OpID::SRem) {
            if (rhs_const != nullptr) {
                auto lhs = value_reg(inst.lhs());
                if (value_is_nonnegative(inst.lhs()) &&
                    try_lower_nonnegative_const_rem(dst, lhs, rhs_const->value())) {
                    return;
                }
                if (try_lower_const_rem(dst, lhs, rhs_const->value())) {
                    return;
                }
            }
        }

        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        mir::Opcode opcode = mir::Opcode::AddW;
        switch (inst.op()) {
        case oir::Instruction::OpID::Add:
            opcode = mir::Opcode::AddW;
            break;
        case oir::Instruction::OpID::Sub:
            opcode = mir::Opcode::SubW;
            break;
        case oir::Instruction::OpID::Mul:
            opcode = mir::Opcode::MulW;
            break;
        case oir::Instruction::OpID::And:
            opcode = mir::Opcode::And;
            break;
        case oir::Instruction::OpID::Xor:
            opcode = mir::Opcode::Xor;
            break;
        case oir::Instruction::OpID::SDiv:
            opcode = mir::Opcode::DivW;
            break;
        case oir::Instruction::OpID::SRem:
            opcode = mir::Opcode::RemW;
            break;
        default:
            break;
        }
        emit(opcode, {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(lhs),
                      mir::MachineOperand::reg_use(rhs)});
    }

    void lower_float_binary(const oir::BinaryInst &inst) {
        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        auto dst = value_regs_.at(&inst);
        mir::Opcode opcode = mir::Opcode::FAddS;
        switch (inst.op()) {
        case oir::Instruction::OpID::FAdd:
            opcode = mir::Opcode::FAddS;
            break;
        case oir::Instruction::OpID::FSub:
            opcode = mir::Opcode::FSubS;
            break;
        case oir::Instruction::OpID::FMul:
            opcode = mir::Opcode::FMulS;
            break;
        case oir::Instruction::OpID::FDiv:
            opcode = mir::Opcode::FDivS;
            break;
        default:
            break;
        }
        emit(opcode, {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(lhs),
                      mir::MachineOperand::reg_use(rhs)});
    }

    void lower_icmp(const oir::CmpInst &inst) {
        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        auto dst = value_regs_.at(&inst);
        switch (inst.pred()) {
        case oir::CmpPred::EQ: {
            auto tmp = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::Xor,
                 {mir::MachineOperand::reg_def(tmp), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            emit(mir::Opcode::SeqZ,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(tmp)});
            break;
        }
        case oir::CmpPred::NE: {
            auto tmp = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::Xor,
                 {mir::MachineOperand::reg_def(tmp), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            emit(mir::Opcode::Snez,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(tmp)});
            break;
        }
        case oir::CmpPred::LT:
            emit(mir::Opcode::Slt,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            break;
        case oir::CmpPred::LE: {
            auto tmp = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::Slt,
                 {mir::MachineOperand::reg_def(tmp), mir::MachineOperand::reg_use(rhs),
                  mir::MachineOperand::reg_use(lhs)});
            emit(mir::Opcode::XorI,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(tmp),
                  mir::MachineOperand::imm(1)});
            break;
        }
        case oir::CmpPred::GT:
            emit(mir::Opcode::Slt,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(rhs),
                  mir::MachineOperand::reg_use(lhs)});
            break;
        case oir::CmpPred::GE: {
            auto tmp = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::Slt,
                 {mir::MachineOperand::reg_def(tmp), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            emit(mir::Opcode::XorI,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(tmp),
                  mir::MachineOperand::imm(1)});
            break;
        }
        }
    }

    void lower_fcmp(const oir::CmpInst &inst) {
        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        auto dst = value_regs_.at(&inst);
        switch (inst.pred()) {
        case oir::CmpPred::EQ:
            emit(mir::Opcode::FeqS,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            break;
        case oir::CmpPred::NE: {
            auto tmp = create_vreg(mir::ValueType::I1);
            emit(mir::Opcode::FeqS,
                 {mir::MachineOperand::reg_def(tmp), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            emit(mir::Opcode::XorI,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(tmp),
                  mir::MachineOperand::imm(1)});
            break;
        }
        case oir::CmpPred::LT:
            emit(mir::Opcode::FltS,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            break;
        case oir::CmpPred::LE:
            emit(mir::Opcode::FleS,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            break;
        case oir::CmpPred::GT:
            emit(mir::Opcode::FltS,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(rhs),
                  mir::MachineOperand::reg_use(lhs)});
            break;
        case oir::CmpPred::GE:
            emit(mir::Opcode::FleS,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(rhs),
                  mir::MachineOperand::reg_use(lhs)});
            break;
        }
    }

    void lower_zext(const oir::CastInst &inst) {
        emit_move(value_regs_.at(&inst), value_reg(inst.src()));
    }

    void lower_sitofp(const oir::CastInst &inst) {
        auto src = value_reg(inst.src());
        auto dst = value_regs_.at(&inst);
        emit(mir::Opcode::FcvtSW,
             {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(src)});
    }

    void lower_fptosi(const oir::CastInst &inst) {
        auto src = value_reg(inst.src());
        auto dst = value_regs_.at(&inst);
        emit(mir::Opcode::FcvtWS,
             {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(src)});
    }

    void lower_call(const oir::CallInst &inst) {
        auto *callee = dynamic_cast<oir::Function *>(inst.callee());
        if (callee == nullptr) {
            throw std::runtime_error("MIR lowering only supports direct calls");
        }

        std::string symbol = callee->name();
        current_function_->note_call();

        std::size_t int_reg = 0;
        std::size_t float_reg = 0;
        std::int64_t stack_offset = 0;
        std::vector<mir::Register> staged_int_args;
        std::vector<mir::Register> staged_float_args;

        if (symbol == "starttime" || symbol == "stoptime") {
            auto zero = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::LoadImm,
                 {mir::MachineOperand::reg_def(zero), mir::MachineOperand::imm(0)});
            staged_int_args.push_back(zero);
            symbol = symbol == "starttime" ? "_sysy_starttime" : "_sysy_stoptime";
            int_reg = 1;
        }

        auto args = inst.args();
        for (auto *arg : args) {
            auto type = type_info(arg->type()).value_type;
            auto src = value_reg(arg);
            if (type == mir::ValueType::F32) {
                if (float_reg < kFArgRegs.size()) {
                    auto staged = create_vreg(type);
                    emit_move(staged, src);
                    staged_float_args.push_back(staged);
                    ++float_reg;
                } else {
                    emit(mir::Opcode::StoreOutgoingArg,
                         {mir::MachineOperand::reg_use(src), mir::MachineOperand::imm(stack_offset),
                          mir::MachineOperand::type(type)});
                    stack_offset += 8;
                }
            } else {
                if (int_reg < kArgRegs.size()) {
                    auto staged = create_vreg(type);
                    emit_move(staged, src);
                    staged_int_args.push_back(staged);
                    ++int_reg;
                } else {
                    emit(mir::Opcode::StoreOutgoingArg,
                         {mir::MachineOperand::reg_use(src), mir::MachineOperand::imm(stack_offset),
                          mir::MachineOperand::type(type)});
                    stack_offset += 8;
                }
            }
        }

        current_function_->reserve_outgoing_arg_bytes(static_cast<std::uint64_t>(stack_offset));
        for (std::size_t i = 0; i < staged_int_args.size(); ++i) {
            emit_move(phys_gpr(kArgRegs[i]), staged_int_args[i]);
        }
        for (std::size_t i = 0; i < staged_float_args.size(); ++i) {
            emit_move(phys_fpr(kFArgRegs[i]), staged_float_args[i]);
        }
        emit(mir::Opcode::Call, {mir::MachineOperand::symbol(symbol)});

        if (!inst.type()->is_void()) {
            auto result_type = type_info(inst.type()).value_type;
            if (result_type == mir::ValueType::F32) {
                emit_move(value_regs_.at(&inst), phys_fpr("fa0"));
            } else {
                emit_move(value_regs_.at(&inst), phys_gpr("a0"));
            }
        }
    }

    void lower_return(const oir::ReturnInst &inst) {
        if (inst.has_value()) {
            auto type = type_info(inst.value()->type()).value_type;
            if (type == mir::ValueType::F32) {
                emit_move(phys_fpr("fa0"), value_reg(inst.value()));
            } else {
                emit_move(phys_gpr("a0"), value_reg(inst.value()));
            }
        }
        emit(mir::Opcode::Jump, {mir::MachineOperand::block("epilogue")});
    }

    void lower_branch(const oir::BranchInst &inst) {
        if (!inst.is_conditional()) {
            if (!has_edge_block(inst.parent(), inst.target_bb()) &&
                inst.parent()->successors().size() == 1 && block_has_phi(*inst.target_bb())) {
                emit_phi_copies_for_edge(inst.parent(), inst.target_bb());
            }
            emit(mir::Opcode::Jump,
                 {mir::MachineOperand::block(branch_target(inst.parent(), inst.target_bb()))});
            return;
        }

        auto cond = value_reg(inst.cond());
        emit(mir::Opcode::BranchNonZero,
             {mir::MachineOperand::reg_use(cond),
              mir::MachineOperand::block(branch_target(inst.parent(), inst.true_bb()))});
        emit(mir::Opcode::Jump,
             {mir::MachineOperand::block(branch_target(inst.parent(), inst.false_bb()))});
    }

    std::string branch_target(const oir::BasicBlock *pred, const oir::BasicBlock *succ) const {
        auto found = edge_blocks_.find(edge_key(pred, succ));
        if (found != edge_blocks_.end()) {
            return found->second->name();
        }
        return succ->name();
    }

    void fill_phi_edge_blocks() {
        for (const auto &edge : pending_edge_blocks_) {
            current_block_ = edge.block;
            emit_phi_copies_for_edge(edge.pred, edge.target);
            emit(mir::Opcode::Jump, {mir::MachineOperand::block(edge.target->name())});
        }
    }

    void emit_phi_copies_for_edge(const oir::BasicBlock *pred, const oir::BasicBlock *target) {
        struct ParallelCopy {
            mir::Register dst;
            mir::Register src;
        };

        std::vector<ParallelCopy> copies;
        for (const auto &inst : target->instructions()) {
            auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
            if (phi == nullptr) {
                break;
            }

            auto *incoming = incoming_for(*phi, pred);
            auto dst = value_regs_.at(phi);
            auto src = value_reg(incoming);
            if (dst != src) {
                copies.push_back({dst, src});
            }
        }

        while (!copies.empty()) {
            auto acyclic = std::find_if(copies.begin(), copies.end(), [&](const auto &copy) {
                return std::none_of(copies.begin(), copies.end(), [&](const auto &other) {
                    return other.src == copy.dst;
                });
            });

            if (acyclic != copies.end()) {
                emit_move(acyclic->dst, acyclic->src);
                copies.erase(acyclic);
                continue;
            }

            auto &cycle = copies.front();
            auto temp = create_vreg(cycle.dst.value_type);
            emit_move(temp, cycle.src);
            cycle.src = temp;
        }
    }

    oir::Value *incoming_for(const oir::PhiInst &phi, const oir::BasicBlock *pred) const {
        for (const auto &incoming : phi.incoming()) {
            if (incoming.second == pred) {
                return incoming.first;
            }
        }
        throw std::runtime_error("phi missing incoming edge");
    }

    mir::Register value_reg(oir::Value *value) {
        if (auto *constant = dynamic_cast<oir::ConstantInt *>(value)) {
            if (constant->value() == 0) {
                return zero_reg();
            }
            auto reg = create_vreg(type_info(value->type()).value_type);
            emit(mir::Opcode::LoadImm,
                 {mir::MachineOperand::reg_def(reg), mir::MachineOperand::imm(constant->value())});
            return reg;
        }
        if (auto *constant = dynamic_cast<oir::ConstantFloat *>(value)) {
            auto reg = create_vreg(mir::ValueType::F32);
            emit(mir::Opcode::LoadFloatImm, {mir::MachineOperand::reg_def(reg),
                                             mir::MachineOperand::float_imm(constant->value())});
            return reg;
        }
        if (dynamic_cast<oir::ConstantZero *>(value) != nullptr ||
            dynamic_cast<oir::UndefValue *>(value) != nullptr) {
            auto type = type_info(value->type()).value_type;
            if (type != mir::ValueType::F32) {
                return zero_reg();
            }
            auto reg = create_vreg(type);
            emit(mir::Opcode::LoadFloatImm,
                 {mir::MachineOperand::reg_def(reg), mir::MachineOperand::float_imm(0.0F)});
            return reg;
        }
        if (auto *global = dynamic_cast<oir::GlobalVariable *>(value)) {
            auto reg = create_vreg(mir::ValueType::Ptr);
            emit(mir::Opcode::LoadGlobalAddr,
                 {mir::MachineOperand::reg_def(reg), mir::MachineOperand::global(global->name())});
            return reg;
        }
        if (auto *function = dynamic_cast<oir::Function *>(value)) {
            auto reg = create_vreg(mir::ValueType::Ptr);
            emit(mir::Opcode::LoadGlobalAddr,
                 {mir::MachineOperand::reg_def(reg), mir::MachineOperand::global(function->name())});
            return reg;
        }

        auto found = value_regs_.find(value);
        if (found == value_regs_.end()) {
            throw std::runtime_error("MIR lowering cannot find vreg for value: " + value->print());
        }
        return found->second;
    }

    void emit_move(mir::Register dst, mir::Register src) {
        mir::Opcode opcode =
            dst.reg_class == mir::RegisterClass::FPR32 ? mir::Opcode::FMove : mir::Opcode::Move;
        emit(opcode,
             {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(std::move(src))});
    }

    void emit_entry_move(mir::Register dst, mir::Register src,
                         const std::vector<mir::Register> &future_arg_regs) {
        mir::Opcode opcode =
            dst.reg_class == mir::RegisterClass::FPR32 ? mir::Opcode::FMove : mir::Opcode::Move;
        std::vector<mir::MachineOperand> operands;
        operands.push_back(mir::MachineOperand::reg_def(dst));
        operands.push_back(mir::MachineOperand::reg_use(std::move(src)));
        for (const auto &reg : future_arg_regs) {
            auto implicit_use = mir::MachineOperand::reg_use(reg);
            implicit_use.set_is_implicit(true);
            operands.push_back(implicit_use);
        }
        emit(opcode, std::move(operands));
    }

    void emit(mir::Opcode opcode, std::vector<mir::MachineOperand> operands) {
        current_block_->add_instr(opcode, std::move(operands));
    }

    struct PendingEdgeBlock {
        const oir::BasicBlock *pred = nullptr;
        const oir::BasicBlock *target = nullptr;
        mir::MachineBasicBlock *block = nullptr;
    };

    std::unique_ptr<mir::Module> module_;
    mir::MachineFunction *current_function_ = nullptr;
    mir::MachineBasicBlock *current_block_ = nullptr;
    unsigned temp_index_ = 0;
    std::unordered_map<const oir::Function *, mir::MachineFunction *> functions_;
    std::unordered_map<const oir::BasicBlock *, mir::MachineBasicBlock *> blocks_;
    std::unordered_map<std::string, mir::MachineBasicBlock *> edge_blocks_;
    std::vector<PendingEdgeBlock> pending_edge_blocks_;
    std::unordered_map<const oir::Instruction *, IntRange> value_ranges_;
    std::unordered_map<const oir::BasicBlock *, RangeMap> block_entry_ranges_;
    RangeMap current_ranges_;
    std::unordered_map<const oir::Value *, mir::Register> value_regs_;
    std::unordered_map<const oir::Instruction *, int> alloca_slots_;
};

} // namespace

std::unique_ptr<mir::Module> lower_with_vregs(const oir::Module &module) {
    VRegLowerer lowerer;
    return lowerer.lower(module);
}

} // namespace pass::oir_to_mir
