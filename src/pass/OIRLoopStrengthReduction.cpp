#include "../../include/oir/OIRAnalysis.h"
#include "../../include/oir/OIRScalarOpt.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <functional>
#include <unordered_map>
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
    std::int64_t index_scale = 1;
    std::int64_t index_offset = 0;
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

std::optional<std::int64_t> constant_int(oir::Value *value) {
    return int_constant(value);
}

std::optional<std::int64_t> induction_step_value(const oir::PhiInst &phi, oir::Value *back_value) {
    auto *binary = dynamic_cast<oir::BinaryInst *>(back_value);
    if (binary == nullptr) {
        return std::nullopt;
    }

    if (binary->op() == oir::Instruction::OpID::Add) {
        if (binary->lhs() == &phi) {
            return constant_int(binary->rhs());
        }
        if (binary->rhs() == &phi) {
            return constant_int(binary->lhs());
        }
    }

    if (binary->op() == oir::Instruction::OpID::Sub && binary->lhs() == &phi) {
        auto step = constant_int(binary->rhs());
        if (step) {
            return -*step;
        }
    }

    return std::nullopt;
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
        std::int64_t offset = 0;
    };
    std::function<std::optional<AffineIndex>(oir::Value *)> match_affine =
        [&](oir::Value *value) -> std::optional<AffineIndex> {
        for (const auto &info : inductions) {
            if (value == info.phi) {
                return AffineIndex{info, 1, 0};
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
                }
                if (binary->op() == oir::Instruction::OpID::Add) {
                    if (auto rhs = match_affine(binary->rhs())) {
                        auto lhs_const = constant_int(binary->lhs());
                        if (lhs_const) {
                            rhs->offset += *lhs_const;
                            return rhs;
                        }
                    }
                }
            }
            if (binary->op() == oir::Instruction::OpID::Mul) {
                if (auto lhs = match_affine(binary->lhs())) {
                    auto rhs_const = constant_int(binary->rhs());
                    if (rhs_const) {
                        lhs->scale *= *rhs_const;
                        lhs->offset *= *rhs_const;
                        return lhs;
                    }
                }
                if (auto rhs = match_affine(binary->rhs())) {
                    auto lhs_const = constant_int(binary->lhs());
                    if (lhs_const) {
                        rhs->scale *= *lhs_const;
                        rhs->offset *= *lhs_const;
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

    return GEPCandidate{&gep, induction->induction, index_pos, pointer_step,
                        induction->scale, induction->offset};
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
    if (candidate.index_offset != 0) {
        value = preheader->insert_before_terminator(std::make_unique<oir::BinaryInst>(
            value->type(), oir::Instruction::OpID::Add, value,
            make_int_constant(module, value->type(), candidate.index_offset), preheader,
            "lsr.idx.offset"));
    }
    return value;
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

        auto *step = make_int_constant(module, module.types().int32_ty(), candidate.pointer_step);
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
