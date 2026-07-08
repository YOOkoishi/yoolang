#include "pass/oir/OIRLoopStrengthReductionPass.h"

#include "oir/OIRAnalysis.h"
#include "oir/OIRScalarOpt.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

constexpr std::size_t kMaxLSRGEPIndices = 4;

struct InductionInfo {
    oir::PhiInst *phi = nullptr;
    oir::Value *start = nullptr;
    std::int64_t step = 0;
};

struct GEPCandidate {
    oir::GetElementPtrInst *gep = nullptr;
    InductionInfo induction;
    std::size_t index_pos = 0;
    std::int64_t pointer_step = 0;
    oir::Value *pointer_step_value = nullptr;
    std::int64_t index_scale = 1;
    oir::Value *index_scale_value = nullptr;
    std::int64_t index_offset = 0;
    oir::Value *index_offset_value = nullptr;
    int index_offset_value_sign = 1;
};

struct GEPCandidateGroup {
    oir::AllocaInst *alloca = nullptr;
    oir::PhiInst *induction_phi = nullptr;
    oir::Value *base_ptr = nullptr;
    std::size_t index_pos = 0;
    std::int64_t pointer_step = 0;
    std::vector<GEPCandidate> candidates;
};

bool contains_block(const oir::Loop &loop, const oir::BasicBlock *block) {
    return std::find(loop.blocks.begin(), loop.blocks.end(), block) != loop.blocks.end();
}

oir::BasicBlock *mutable_block(const oir::BasicBlock *block) {
    return const_cast<oir::BasicBlock *>(block);
}

oir::BasicBlock *find_preheader(const oir::Loop &loop) {
    oir::BasicBlock *preheader = nullptr;
    for (auto *pred : loop.header->predecessors()) {
        if (contains_block(loop, pred)) {
            continue;
        }
        if (preheader != nullptr) {
            return nullptr;
        }
        preheader = pred;
    }
    return preheader;
}

oir::BasicBlock *single_latch(const oir::Loop &loop) {
    if (loop.latches.size() != 1) {
        return nullptr;
    }
    return mutable_block(loop.latches.front());
}

bool value_defined_in_loop(oir::Value *value, const oir::Loop &loop) {
    auto *inst = dynamic_cast<oir::Instruction *>(value);
    return inst != nullptr && contains_block(loop, inst->parent());
}

oir::AllocaInst *underlying_alloca(oir::Value *value) {
    std::unordered_set<oir::Value *> seen;
    auto *current = value;
    while (current != nullptr && seen.insert(current).second) {
        if (auto *alloca = dynamic_cast<oir::AllocaInst *>(current)) {
            return alloca;
        }
        auto *gep = dynamic_cast<oir::GetElementPtrInst *>(current);
        if (gep == nullptr) {
            return nullptr;
        }
        current = gep->base_ptr();
    }
    return nullptr;
}

bool stack_object_is_non_escaping_impl(oir::Value *value, std::unordered_set<oir::Value *> &seen) {
    if (value == nullptr || !seen.insert(value).second) {
        return true;
    }
    for (auto *user : value->users()) {
        auto *inst = dynamic_cast<oir::Instruction *>(user);
        if (inst == nullptr) {
            return false;
        }
        if (auto *gep = dynamic_cast<oir::GetElementPtrInst *>(inst)) {
            if (gep->base_ptr() != value || !stack_object_is_non_escaping_impl(gep, seen)) {
                return false;
            }
            continue;
        }
        if (dynamic_cast<oir::PhiInst *>(inst) != nullptr) {
            if (!stack_object_is_non_escaping_impl(inst, seen)) {
                return false;
            }
            continue;
        }
        if (auto *load = dynamic_cast<oir::LoadInst *>(inst)) {
            if (load->ptr() == value) {
                continue;
            }
            return false;
        }
        if (auto *store = dynamic_cast<oir::StoreInst *>(inst)) {
            if (store->ptr() == value && store->value() != value) {
                continue;
            }
            return false;
        }
        if (auto *memzero = dynamic_cast<oir::MemZeroInst *>(inst)) {
            if (memzero->ptr() == value) {
                continue;
            }
            return false;
        }
        return false;
    }
    return true;
}

bool stack_object_is_non_escaping(oir::AllocaInst *alloca) {
    std::unordered_set<oir::Value *> seen;
    return stack_object_is_non_escaping_impl(alloca, seen);
}

std::optional<std::int64_t> constant_int(oir::Value *value) {
    return int_constant(value);
}

std::optional<std::int64_t>
induction_step_value_impl(const oir::PhiInst &phi, oir::Value *back_value,
                          std::unordered_set<oir::Value *> &active) {
    if (!active.insert(back_value).second) {
        return std::nullopt;
    }

    auto *binary = dynamic_cast<oir::BinaryInst *>(back_value);
    if (binary == nullptr) {
        if (auto *back_phi = dynamic_cast<oir::PhiInst *>(back_value)) {
            std::optional<std::int64_t> step;
            for (const auto &[incoming_value, pred] : back_phi->incoming()) {
                (void)pred;
                auto incoming_step = induction_step_value_impl(phi, incoming_value, active);
                if (!incoming_step) {
                    active.erase(back_value);
                    return std::nullopt;
                }
                if (!step) {
                    step = *incoming_step;
                } else if (*step != *incoming_step) {
                    active.erase(back_value);
                    return std::nullopt;
                }
            }
            active.erase(back_value);
            return step;
        }
        active.erase(back_value);
        return std::nullopt;
    }

    if (binary->op() == oir::Instruction::OpID::Add) {
        if (binary->lhs() == &phi) {
            auto step = constant_int(binary->rhs());
            active.erase(back_value);
            return step;
        }
        if (binary->rhs() == &phi) {
            auto step = constant_int(binary->lhs());
            active.erase(back_value);
            return step;
        }
    }

    if (binary->op() == oir::Instruction::OpID::Sub && binary->lhs() == &phi) {
        auto step = constant_int(binary->rhs());
        if (step) {
            active.erase(back_value);
            return -*step;
        }
    }

    active.erase(back_value);
    return std::nullopt;
}

std::optional<std::int64_t> induction_step_value(const oir::PhiInst &phi, oir::Value *back_value) {
    std::unordered_set<oir::Value *> active;
    return induction_step_value_impl(phi, back_value, active);
}

std::vector<InductionInfo> collect_inductions(const oir::Loop &loop, oir::BasicBlock *preheader,
                                              oir::BasicBlock *latch) {
    std::vector<InductionInfo> inductions;
    for (auto &inst : loop.header->instructions()) {
        auto *phi = dynamic_cast<oir::PhiInst *>(inst.get());
        if (phi == nullptr) {
            break;
        }

        oir::Value *start = nullptr;
        oir::Value *back = nullptr;
        for (const auto &[value, pred] : phi->incoming()) {
            if (pred == preheader) {
                start = value;
            } else if (pred == latch) {
                back = value;
            }
        }

        if (start == nullptr || back == nullptr || value_defined_in_loop(start, loop)) {
            continue;
        }

        auto step = induction_step_value(*phi, back);
        if (!step || *step == 0) {
            continue;
        }

        inductions.push_back({phi, start, *step});
    }
    return inductions;
}

std::uint64_t type_size(oir::Type *type) {
    if (type == nullptr || type->is_void() || type->is_label() || type->is_function()) {
        return 0;
    }
    if (auto *int_ty = dynamic_cast<oir::IntegerType *>(type)) {
        return (int_ty->bit_width() + 7) / 8;
    }
    if (type->is_float()) {
        return 4;
    }
    if (type->is_pointer()) {
        return 8;
    }
    if (auto *array = dynamic_cast<oir::ArrayType *>(type)) {
        return type_size(array->element_type()) * array->element_count();
    }
    return 0;
}

std::optional<std::vector<std::uint64_t>>
gep_index_strides(const oir::GetElementPtrInst &gep) {
    auto *ptr_type = dynamic_cast<oir::PointerType *>(gep.base_ptr()->type());
    if (ptr_type == nullptr) {
        return std::nullopt;
    }

    std::vector<std::uint64_t> strides;
    auto *cursor = ptr_type->element_type();
    auto indices = gep.indices();
    strides.reserve(indices.size());
    for (std::size_t i = 0; i < indices.size(); ++i) {
        std::uint64_t stride = 0;
        if (i == 0) {
            stride = type_size(cursor);
        } else if (auto *array = dynamic_cast<oir::ArrayType *>(cursor)) {
            stride = type_size(array->element_type());
            cursor = array->element_type();
        } else {
            stride = type_size(cursor);
        }

        if (stride == 0) {
            return std::nullopt;
        }
        strides.push_back(stride);
    }
    return strides;
}

std::optional<GEPCandidate>
analyze_gep(oir::GetElementPtrInst &gep, const oir::Loop &loop,
            const std::vector<InductionInfo> &inductions) {
    if (!gep.has_uses() || value_defined_in_loop(gep.base_ptr(), loop)) {
        return std::nullopt;
    }

    for (auto *user : gep.users()) {
        auto *inst_user = dynamic_cast<oir::Instruction *>(user);
        if (inst_user == nullptr || !contains_block(loop, inst_user->parent())) {
            return std::nullopt;
        }
    }

    auto strides = gep_index_strides(gep);
    auto *result_ptr = dynamic_cast<oir::PointerType *>(gep.type());
    if (!strides || result_ptr == nullptr) {
        return std::nullopt;
    }

    const auto result_elem_size = static_cast<std::int64_t>(type_size(result_ptr->element_type()));
    if (result_elem_size == 0) {
        return std::nullopt;
    }

    auto indices = gep.indices();
    if (indices.size() > kMaxLSRGEPIndices) {
        return std::nullopt;
    }

    struct AffineIndex {
        InductionInfo induction;
        std::int64_t scale = 1;
        oir::Value *scale_value = nullptr;
        std::int64_t offset = 0;
        oir::Value *offset_value = nullptr;
        int offset_value_sign = 1;
    };
    auto add_offset_value = [](AffineIndex &index, oir::Value *value, int sign) {
        if (value == nullptr) {
            return true;
        }
        if (index.offset_value != nullptr || value->type() != index.induction.start->type()) {
            return false;
        }
        index.offset_value = value;
        index.offset_value_sign = sign < 0 ? -1 : 1;
        return true;
    };
    std::function<std::optional<AffineIndex>(oir::Value *)> match_affine =
        [&](oir::Value *value) -> std::optional<AffineIndex> {
        for (const auto &info : inductions) {
            if (value == info.phi) {
                return AffineIndex{info, 1, nullptr, 0, nullptr, 1};
            }
        }
        if (auto *binary = dynamic_cast<oir::BinaryInst *>(value)) {
            if (binary->op() == oir::Instruction::OpID::Add ||
                binary->op() == oir::Instruction::OpID::Sub) {
                if (auto lhs = match_affine(binary->lhs())) {
                    auto rhs_const = constant_int(binary->rhs());
                    if (rhs_const && binary->op() == oir::Instruction::OpID::Add) {
                        lhs->offset += *rhs_const;
                        return lhs;
                    }
                    if (rhs_const && binary->op() == oir::Instruction::OpID::Sub) {
                        lhs->offset -= *rhs_const;
                        return lhs;
                    }
                    if (!rhs_const && !value_defined_in_loop(binary->rhs(), loop) &&
                        add_offset_value(*lhs, binary->rhs(),
                                         binary->op() == oir::Instruction::OpID::Sub ? -1 : 1)) {
                        return lhs;
                    }
                }
                if (binary->op() == oir::Instruction::OpID::Add) {
                    if (auto rhs = match_affine(binary->rhs())) {
                        auto lhs_const = constant_int(binary->lhs());
                        if (lhs_const) {
                            rhs->offset += *lhs_const;
                            return rhs;
                        }
                        if (!value_defined_in_loop(binary->lhs(), loop) &&
                            add_offset_value(*rhs, binary->lhs(), 1)) {
                            return rhs;
                        }
                    }
                }
            }
            if (binary->op() == oir::Instruction::OpID::Mul) {
                if (auto lhs = match_affine(binary->lhs())) {
                    auto rhs_const = constant_int(binary->rhs());
                    if (rhs_const && lhs->offset_value == nullptr) {
                        lhs->scale *= *rhs_const;
                        lhs->offset *= *rhs_const;
                        return lhs;
                    }
                    if (!rhs_const && lhs->scale_value == nullptr && lhs->offset == 0 &&
                        lhs->offset_value == nullptr &&
                        !value_defined_in_loop(binary->rhs(), loop) &&
                        binary->rhs()->type() == lhs->induction.start->type()) {
                        lhs->scale_value = binary->rhs();
                        return lhs;
                    }
                }
                if (auto rhs = match_affine(binary->rhs())) {
                    auto lhs_const = constant_int(binary->lhs());
                    if (lhs_const && rhs->offset_value == nullptr) {
                        rhs->scale *= *lhs_const;
                        rhs->offset *= *lhs_const;
                        return rhs;
                    }
                    if (!lhs_const && rhs->scale_value == nullptr && rhs->offset == 0 &&
                        rhs->offset_value == nullptr &&
                        !value_defined_in_loop(binary->lhs(), loop) &&
                        binary->lhs()->type() == rhs->induction.start->type()) {
                        rhs->scale_value = binary->lhs();
                        return rhs;
                    }
                }
            }
        }
        return std::nullopt;
        };

    std::optional<AffineIndex> induction;
    std::size_t index_pos = 0;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        auto found = match_affine(indices[i]);
        if (found) {
            if (induction) {
                return std::nullopt;
            }
            induction = *found;
            index_pos = i;
            continue;
        }

        if (value_defined_in_loop(indices[i], loop)) {
            return std::nullopt;
        }
    }

    if (!induction) {
        return std::nullopt;
    }

    const auto step_bytes =
        induction->induction.step * induction->scale *
        static_cast<std::int64_t>((*strides)[index_pos]);
    if (step_bytes == 0 || step_bytes % result_elem_size != 0) {
        return std::nullopt;
    }

    const auto pointer_step = step_bytes / result_elem_size;
    if (pointer_step == 0) {
        return std::nullopt;
    }

    return GEPCandidate{&gep,
                        induction->induction,
                        index_pos,
                        pointer_step,
                        induction->scale_value,
                        induction->scale,
                        induction->scale_value,
                        induction->offset,
                        induction->offset_value,
                        induction->offset_value_sign};
}

std::vector<GEPCandidate>
collect_candidates(const oir::Loop &loop, const std::vector<InductionInfo> &inductions) {
    std::vector<GEPCandidate> candidates;
    for (auto *const_block : loop.blocks) {
        auto *block = mutable_block(const_block);
        for (auto &inst : block->instructions()) {
            auto *gep = dynamic_cast<oir::GetElementPtrInst *>(inst.get());
            if (gep == nullptr) {
                continue;
            }
            auto candidate = analyze_gep(*gep, loop, inductions);
            if (candidate) {
                candidates.push_back(*candidate);
            }
        }
    }
    return candidates;
}

bool latch_has_call_before_terminator(const oir::BasicBlock &latch) {
    for (const auto &inst : latch.instructions()) {
        if (inst->is_terminator()) {
            return false;
        }
        if (inst->op() == oir::Instruction::OpID::Call) {
            return true;
        }
    }
    return false;
}

oir::CallInst *first_call_before_terminator(oir::BasicBlock *block) {
    for (const auto &inst : block->instructions()) {
        if (inst->is_terminator()) {
            return nullptr;
        }
        if (auto *call = dynamic_cast<oir::CallInst *>(inst.get())) {
            return call;
        }
    }
    return nullptr;
}

bool instruction_precedes_in_block(const oir::Instruction *candidate,
                                   const oir::Instruction *limit) {
    if (candidate == nullptr || limit == nullptr || candidate->parent() != limit->parent()) {
        return false;
    }
    for (const auto &inst : candidate->parent()->instructions()) {
        if (inst.get() == limit) {
            return false;
        }
        if (inst.get() == candidate) {
            return true;
        }
    }
    return false;
}

bool gep_loop_uses_precede_call(const GEPCandidate &candidate, const oir::Loop &loop,
                                const oir::CallInst *call) {
    if (!instruction_precedes_in_block(candidate.gep, call)) {
        return false;
    }
    for (auto *user : candidate.gep->users()) {
        auto *inst = dynamic_cast<oir::Instruction *>(user);
        if (inst == nullptr || !contains_block(loop, inst->parent())) {
            return false;
        }
        if (!instruction_precedes_in_block(inst, call)) {
            return false;
        }
    }
    return true;
}

bool candidate_is_stack_call_lsr_safe(const GEPCandidate &candidate, const oir::Loop &loop,
                                      const oir::CallInst *call,
                                      std::unordered_map<oir::AllocaInst *, bool> &escape_cache,
                                      oir::AllocaInst *&alloca) {
    alloca = underlying_alloca(candidate.gep->base_ptr());
    if (alloca == nullptr) {
        return false;
    }
    auto [escape_it, inserted] = escape_cache.emplace(alloca, false);
    if (inserted) {
        escape_it->second = stack_object_is_non_escaping(alloca);
    }
    if (!escape_it->second) {
        return false;
    }
    if (candidate.pointer_step_value != nullptr || candidate.index_scale_value != nullptr ||
        candidate.index_offset_value != nullptr || candidate.index_scale != 1) {
        return false;
    }
    constexpr std::int64_t kMaxStackPointerStep = 8;
    if (candidate.pointer_step < -kMaxStackPointerStep ||
        candidate.pointer_step > kMaxStackPointerStep) {
        return false;
    }
    return gep_loop_uses_precede_call(candidate, loop, call);
}

std::vector<GEPCandidateGroup>
group_stack_call_lsr_candidates(const oir::Loop &loop, oir::BasicBlock *latch,
                                const std::vector<GEPCandidate> &candidates) {
    std::vector<GEPCandidateGroup> groups;
    auto *call = first_call_before_terminator(latch);
    if (call == nullptr) {
        return groups;
    }

    std::unordered_map<oir::AllocaInst *, bool> escape_cache;
    for (const auto &candidate : candidates) {
        oir::AllocaInst *alloca = nullptr;
        if (!candidate_is_stack_call_lsr_safe(candidate, loop, call, escape_cache, alloca)) {
            continue;
        }

        auto found = std::find_if(groups.begin(), groups.end(), [&](const GEPCandidateGroup &g) {
            return g.alloca == alloca && g.induction_phi == candidate.induction.phi &&
                   g.base_ptr == candidate.gep->base_ptr() && g.index_pos == candidate.index_pos &&
                   g.pointer_step == candidate.pointer_step;
        });
        if (found == groups.end()) {
            groups.push_back(
                GEPCandidateGroup{alloca, candidate.induction.phi, candidate.gep->base_ptr(),
                                  candidate.index_pos, candidate.pointer_step, {candidate}});
        } else {
            found->candidates.push_back(candidate);
        }
    }

    constexpr std::size_t kMaxStackCallPointerPhis = 4;
    if (groups.size() > kMaxStackCallPointerPhis) {
        groups.clear();
    }
    return groups;
}

oir::Instruction *insert_before_instruction(oir::BasicBlock *block, oir::Instruction *before,
                                            std::unique_ptr<oir::Instruction> inst) {
    auto *raw = inst.get();
    raw->set_parent(block);
    for (auto it = block->instructions().begin(); it != block->instructions().end(); ++it) {
        if (it->get() == before) {
            block->instructions().insert(it, std::move(inst));
            return raw;
        }
    }
    return block->insert_before_terminator(std::move(inst));
}

std::string lsr_name(const oir::GetElementPtrInst &gep, const char *suffix) {
    const std::string base = gep.name().empty() ? "gep" : gep.name();
    return base + suffix;
}

oir::PhiInst *insert_pointer_phi(oir::BasicBlock *header, oir::Type *type,
                                 const std::string &name) {
    auto phi = std::make_unique<oir::PhiInst>(type, header, name);
    auto *raw = phi.get();
    auto insert_pos = header->instructions().begin();
    while (insert_pos != header->instructions().end() &&
           dynamic_cast<oir::PhiInst *>(insert_pos->get()) != nullptr) {
        ++insert_pos;
    }
    header->instructions().insert(insert_pos, std::move(phi));
    return raw;
}

oir::Value *materialize_start_index(oir::Module &module, oir::BasicBlock *preheader,
                                    const GEPCandidate &candidate) {
    oir::Value *value = candidate.induction.start;
    if (candidate.index_scale != 1) {
        value = preheader->insert_before_terminator(std::make_unique<oir::BinaryInst>(
            value->type(), oir::Instruction::OpID::Mul, value,
            make_int_constant(module, value->type(), candidate.index_scale), preheader,
            "lsr.idx.scale"));
    }
    if (candidate.index_scale_value != nullptr) {
        value = preheader->insert_before_terminator(std::make_unique<oir::BinaryInst>(
            value->type(), oir::Instruction::OpID::Mul, value, candidate.index_scale_value,
            preheader, "lsr.idx.dynamic_scale"));
    }
    if (candidate.index_offset_value != nullptr) {
        value = preheader->insert_before_terminator(std::make_unique<oir::BinaryInst>(
            value->type(),
            candidate.index_offset_value_sign < 0 ? oir::Instruction::OpID::Sub
                                                  : oir::Instruction::OpID::Add,
            value, candidate.index_offset_value, preheader, "lsr.idx.base"));
    }
    if (candidate.index_offset != 0) {
        value = preheader->insert_before_terminator(std::make_unique<oir::BinaryInst>(
            value->type(), oir::Instruction::OpID::Add, value,
            make_int_constant(module, value->type(), candidate.index_offset), preheader,
            "lsr.idx.offset"));
    }
    return value;
}

oir::Value *materialize_pointer_step(oir::Module &module, oir::BasicBlock *preheader,
                                     const GEPCandidate &candidate) {
    if (candidate.pointer_step_value == nullptr) {
        return make_int_constant(module, module.types().int32_ty(), candidate.pointer_step);
    }

    auto *value = candidate.pointer_step_value;
    if (candidate.pointer_step == 1) {
        return value;
    }
    if (candidate.pointer_step == -1) {
        return preheader->insert_before_terminator(std::make_unique<oir::BinaryInst>(
            value->type(), oir::Instruction::OpID::Sub,
            make_int_constant(module, value->type(), 0), value, preheader,
            "lsr.step.neg"));
    }
    return preheader->insert_before_terminator(std::make_unique<oir::BinaryInst>(
        value->type(), oir::Instruction::OpID::Mul, value,
        make_int_constant(module, value->type(), candidate.pointer_step), preheader,
        "lsr.step.scale"));
}

bool apply_lsr(oir::Module &module, const oir::Loop &loop, oir::BasicBlock *preheader,
               oir::BasicBlock *latch, const std::vector<GEPCandidate> &candidates,
               Stats &stats) {
    if (candidates.empty()) {
        return false;
    }

    ReplacementMap replacements;
    auto *header = mutable_block(loop.header);
    for (const auto &candidate : candidates) {
        auto *gep = candidate.gep;
        auto start_indices = gep->indices();
        start_indices[candidate.index_pos] = materialize_start_index(module, preheader, candidate);

        auto *start_ptr = static_cast<oir::GetElementPtrInst *>(
            preheader->insert_before_terminator(std::make_unique<oir::GetElementPtrInst>(
                gep->type(), gep->base_ptr(), start_indices, preheader, lsr_name(*gep, ".start"))));

        auto *phi = insert_pointer_phi(header, gep->type(), lsr_name(*gep, ".ptr"));
        phi->add_incoming(start_ptr, preheader);

        auto *step = materialize_pointer_step(module, preheader, candidate);
        auto *next = static_cast<oir::GetElementPtrInst *>(
            latch->insert_before_terminator(std::make_unique<oir::GetElementPtrInst>(
                gep->type(), phi, std::vector<oir::Value *>{step}, latch,
                lsr_name(*gep, ".next"))));
        phi->add_incoming(next, latch);

        replacements[gep] = phi;
        ++stats.lsr;
    }

    return apply_replacements(module, replacements) != 0;
}

bool apply_stack_call_lsr(oir::Module &module, oir::BasicBlock *preheader,
                          oir::BasicBlock *latch,
                          const std::vector<GEPCandidateGroup> &groups, Stats &stats) {
    if (groups.empty()) {
        return false;
    }

    ReplacementMap replacements;
    auto *header = latch;
    for (const auto &group : groups) {
        if (group.candidates.empty()) {
            continue;
        }

        auto base_candidate = group.candidates.front();
        for (const auto &candidate : group.candidates) {
            if (candidate.index_offset < base_candidate.index_offset) {
                base_candidate = candidate;
            }
        }

        auto start_indices = base_candidate.gep->indices();
        start_indices[base_candidate.index_pos] =
            materialize_start_index(module, preheader, base_candidate);
        auto *start_ptr = static_cast<oir::GetElementPtrInst *>(
            preheader->insert_before_terminator(std::make_unique<oir::GetElementPtrInst>(
                base_candidate.gep->type(), base_candidate.gep->base_ptr(), start_indices,
                preheader, lsr_name(*base_candidate.gep, ".start"))));

        auto *phi =
            insert_pointer_phi(header, base_candidate.gep->type(),
                               lsr_name(*base_candidate.gep, ".stack.ptr"));
        phi->add_incoming(start_ptr, preheader);

        auto *step = materialize_pointer_step(module, preheader, base_candidate);
        auto *next = static_cast<oir::GetElementPtrInst *>(
            latch->insert_before_terminator(std::make_unique<oir::GetElementPtrInst>(
                base_candidate.gep->type(), phi, std::vector<oir::Value *>{step}, latch,
                lsr_name(*base_candidate.gep, ".stack.next"))));
        phi->add_incoming(next, latch);

        for (const auto &candidate : group.candidates) {
            const auto offset_delta = candidate.index_offset - base_candidate.index_offset;
            if (offset_delta == 0) {
                replacements[candidate.gep] = phi;
                continue;
            }
            auto *offset_ptr = insert_before_instruction(
                candidate.gep->parent(), candidate.gep,
                std::make_unique<oir::GetElementPtrInst>(
                    candidate.gep->type(), phi,
                    std::vector<oir::Value *>{
                        make_int_constant(module, module.types().int32_ty(), offset_delta)},
                    candidate.gep->parent(), lsr_name(*candidate.gep, ".stack.off")));
            replacements[candidate.gep] = offset_ptr;
        }

        ++stats.lsr;
    }

    return apply_replacements(module, replacements) != 0;
}

bool run_on_loop(oir::Module &module, const oir::Loop &loop, Stats &stats) {
    auto *preheader = find_preheader(loop);
    auto *latch = single_latch(loop);
    if (preheader == nullptr || latch == nullptr) {
        return false;
    }

    auto inductions = collect_inductions(loop, preheader, latch);
    if (inductions.empty()) {
        return false;
    }

    auto candidates = collect_candidates(loop, inductions);
    if (latch_has_call_before_terminator(*latch)) {
        if (mutable_block(loop.header) != latch) {
            return false;
        }
        auto groups = group_stack_call_lsr_candidates(loop, latch, candidates);
        return apply_stack_call_lsr(module, preheader, latch, groups, stats);
    }
    return apply_lsr(module, loop, preheader, latch, candidates, stats);
}

bool run_on_function(oir::Function &function, Stats &stats) {
    if (function.is_external() || function.entry_block() == nullptr) {
        return false;
    }

    oir::DominatorTree dom_tree(function);
    oir::LoopInfo loop_info(function, dom_tree);
    auto loops = loop_info.loops();
    std::sort(loops.begin(), loops.end(), [](const oir::Loop &lhs, const oir::Loop &rhs) {
        return lhs.blocks.size() < rhs.blocks.size();
    });

    bool changed = false;
    for (const auto &loop : loops) {
        changed |= run_on_loop(*function.parent(), loop, stats);
    }
    return changed;
}

} // namespace

bool reduce_gep_strength(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        changed |= run_on_function(*function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt

namespace pass {

std::string_view OIRLoopStrengthReductionPass::name() const {
    return "OIRLoopStrengthReductionPass";
}

PassKind OIRLoopStrengthReductionPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRLoopStrengthReductionPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(
        context, "OIRLoopStrengthReductionPass requires OIR module in pass context",
        [](oir::Module &module, oir_opt::Stats &stats) {
            bool changed = oir_opt::reduce_gep_strength(module, stats);
            changed |= oir_opt::global_value_numbering(module, stats);
            changed |= oir_opt::eliminate_dead_code(module, stats);
            return changed;
        });
}

} // namespace pass
