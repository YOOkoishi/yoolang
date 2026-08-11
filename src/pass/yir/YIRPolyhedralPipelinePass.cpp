#include "pass/yir/YIRPolyhedralPipelinePass.h"

#include "pass/ast/ASTToYIRPass.h"
#include "pass/yir/YIRPolyhedralCanonicalizePass.h"
#include "pass/yir/YIRPolyhedralDependenceAnalysisPass.h"
#include "pass/yir/YIRPolyhedralModelBuildPass.h"
#include "pass/yir/YIRPolyhedralTransformPass.h"
#include "pass/yir/YIRSCoPDetectPass.h"
#include "yir/YIR.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pass {
namespace {

const yir::ConstI32Op *const_i32_def(const yir::Value *value) {
    return value == nullptr ? nullptr : dynamic_cast<const yir::ConstI32Op *>(value->defining_op());
}

bool const_i32_value(const yir::Value *value, std::int64_t &out) {
    auto *constant = const_i32_def(value);
    if (constant == nullptr) {
        return false;
    }
    out = constant->value();
    return true;
}

struct AffineDimOffset {
    const yir::Value *dim = nullptr;
    std::int64_t offset = 0;
};

bool parse_dim_offset(const yir::Value *value, AffineDimOffset &out) {
    std::int64_t constant = 0;
    if (const_i32_value(value, constant)) {
        out = {nullptr, constant};
        return true;
    }

    if (auto *add = value == nullptr ? nullptr : dynamic_cast<const yir::AddIOp *>(value->defining_op())) {
        std::int64_t rhs = 0;
        if (const_i32_value(add->rhs(), rhs)) {
            out = {add->lhs(), rhs};
            return true;
        }
        std::int64_t lhs = 0;
        if (const_i32_value(add->lhs(), lhs)) {
            out = {add->rhs(), lhs};
            return true;
        }
    }

    if (auto *sub = value == nullptr ? nullptr : dynamic_cast<const yir::SubIOp *>(value->defining_op())) {
        std::int64_t rhs = 0;
        if (const_i32_value(sub->rhs(), rhs)) {
            out = {sub->lhs(), -rhs};
            return true;
        }
    }

    out = {value, 0};
    return value != nullptr;
}

std::size_t array_rank_of_value(const yir::Value *value) {
    if (value == nullptr || value->type() == nullptr) {
        return 0;
    }
    auto type = value->type();
    if (type->kind() == yir::Type::Kind::Ptr) {
        type = type->pointee();
    }
    return yir::array_rank(type);
}

std::uint64_t bounded_array_element_count(const yir::Value *value) {
    if (value == nullptr || value->type() == nullptr) {
        return 0;
    }

    auto type = value->type();
    if (type->kind() == yir::Type::Kind::Ptr) {
        type = type->pointee();
    }

    std::uint64_t count = 1;
    while (type != nullptr && type->kind() == yir::Type::Kind::Array) {
        if (type->count() != 0 &&
            count > std::numeric_limits<std::uint64_t>::max() / type->count()) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        count *= type->count();
        type = type->element();
    }
    return count;
}

bool vector_contains_value(const std::vector<const yir::Value *> &values,
                           const yir::Value *value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void push_unique_value(std::vector<const yir::Value *> &values, const yir::Value *value) {
    if (value != nullptr && !vector_contains_value(values, value)) {
        values.push_back(value);
    }
}

bool same_offset_vector(const std::vector<std::int64_t> &lhs,
                        const std::vector<std::int64_t> &rhs) {
    return lhs == rhs;
}

void push_unique_offset(std::vector<std::vector<std::int64_t>> &offsets,
                        std::vector<std::int64_t> offset) {
    const auto found =
        std::find_if(offsets.begin(), offsets.end(), [&](const auto &existing) {
            return same_offset_vector(existing, offset);
        });
    if (found == offsets.end()) {
        offsets.push_back(std::move(offset));
    }
}

bool parse_access_relative_to_store(const std::vector<yir::Value *> &indices,
                                    const std::vector<AffineDimOffset> &store_dims,
                                    std::vector<std::int64_t> &offsets) {
    if (indices.size() != store_dims.size()) {
        return false;
    }

    offsets.clear();
    offsets.reserve(indices.size());
    for (std::size_t i = 0; i < indices.size(); ++i) {
        AffineDimOffset access;
        if (!parse_dim_offset(indices[i], access) || access.dim != store_dims[i].dim) {
            return false;
        }
        offsets.push_back(access.offset - store_dims[i].offset);
    }
    return true;
}

bool all_offsets_unit_bounded(const std::vector<std::int64_t> &offsets) {
    return std::all_of(offsets.begin(), offsets.end(), [](std::int64_t offset) {
        return offset >= -1 && offset <= 1;
    });
}

bool any_nonzero_offset(const std::vector<std::int64_t> &offsets) {
    return std::any_of(offsets.begin(), offsets.end(), [](std::int64_t offset) {
        return offset != 0;
    });
}

bool any_positive_offset(const std::vector<std::int64_t> &offsets) {
    return std::any_of(offsets.begin(), offsets.end(), [](std::int64_t offset) {
        return offset > 0;
    });
}

bool any_negative_offset(const std::vector<std::int64_t> &offsets) {
    return std::any_of(offsets.begin(), offsets.end(), [](std::int64_t offset) {
        return offset < 0;
    });
}

bool is_innermost_previous_offset(const std::vector<std::int64_t> &offsets) {
    if (offsets.empty() || offsets.back() != -1) {
        return false;
    }
    for (std::size_t i = 0; i + 1 < offsets.size(); ++i) {
        if (offsets[i] != 0) {
            return false;
        }
    }
    return true;
}

struct PolyhedralProfitabilityEstimate {
    int score = 0;
    std::size_t best_rank = 0;
    std::size_t best_affine_loads = 0;
    std::size_t best_unit_neighbor_loads = 0;
    std::size_t best_distinct_offsets = 0;
};

struct LoopBandAccessStats {
    std::size_t affine_loads = 0;
    std::size_t distinct_load_memories = 0;
    std::size_t deeper_loop_loads = 0;
    bool has_extra_loop_depth = false;
    std::vector<const yir::Value *> load_memories;
    std::vector<const yir::Value *> covered_band_dims;
};

bool region_has_nested_loop(const yir::Region &region) {
    for (const auto &op : region.operations()) {
        if (dynamic_cast<const yir::WhileOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ForOp *>(op.get()) != nullptr) {
            return true;
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            if (region_has_nested_loop(if_op->then_region()) ||
                (if_op->has_else() && region_has_nested_loop(if_op->else_region()))) {
                return true;
            }
        }
    }
    return false;
}

bool is_unsupported_poly_loop_op(const yir::Operation &op) {
    return dynamic_cast<const yir::CallOp *>(&op) != nullptr ||
           dynamic_cast<const yir::LoadOp *>(&op) != nullptr ||
           dynamic_cast<const yir::StoreOp *>(&op) != nullptr ||
           dynamic_cast<const yir::ElemAddrOp *>(&op) != nullptr ||
           dynamic_cast<const yir::DecayOp *>(&op) != nullptr ||
           dynamic_cast<const yir::AllocaOp *>(&op) != nullptr ||
           dynamic_cast<const yir::ArrayInitOp *>(&op) != nullptr ||
           dynamic_cast<const yir::BreakOp *>(&op) != nullptr ||
           dynamic_cast<const yir::ContinueOp *>(&op) != nullptr ||
           dynamic_cast<const yir::ReturnOp *>(&op) != nullptr ||
           dynamic_cast<const yir::CondOp *>(&op) != nullptr;
}

bool loop_body_has_affine_memory_candidate(const yir::Region &region,
                                            bool &has_array_access) {
    for (const auto &op : region.operations()) {
        if (is_unsupported_poly_loop_op(*op)) {
            return false;
        }
        if (dynamic_cast<const yir::ArrayLoadOp *>(op.get()) != nullptr ||
            dynamic_cast<const yir::ArrayStoreOp *>(op.get()) != nullptr) {
            has_array_access = true;
            continue;
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            if (!loop_body_has_affine_memory_candidate(if_op->then_region(), has_array_access) ||
                (if_op->has_else() &&
                 !loop_body_has_affine_memory_candidate(if_op->else_region(), has_array_access))) {
                return false;
            }
            continue;
        }
        if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
            if (!loop_body_has_affine_memory_candidate(while_op->cond_region(), has_array_access) ||
                !loop_body_has_affine_memory_candidate(while_op->body_region(), has_array_access)) {
                return false;
            }
            continue;
        }
        if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
            if (!loop_body_has_affine_memory_candidate(for_op->body_region(), has_array_access)) {
                return false;
            }
        }
    }
    return true;
}

bool function_has_polyhedral_candidate(const yir::Region &region) {
    for (const auto &op : region.operations()) {
        if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
            bool has_array_access = false;
            if (loop_body_has_affine_memory_candidate(for_op->body_region(), has_array_access) &&
                has_array_access) {
                return true;
            }
            if (function_has_polyhedral_candidate(for_op->body_region())) {
                return true;
            }
            continue;
        }
        if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
            bool has_array_access = false;
            if (loop_body_has_affine_memory_candidate(while_op->body_region(), has_array_access) &&
                has_array_access) {
                return true;
            }
            if (function_has_polyhedral_candidate(while_op->body_region())) {
                return true;
            }
            continue;
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            if (function_has_polyhedral_candidate(if_op->then_region()) ||
                (if_op->has_else() && function_has_polyhedral_candidate(if_op->else_region()))) {
                return true;
            }
        }
    }
    return false;
}

bool access_uses_dim(const std::vector<yir::Value *> &indices, const yir::Value *dim) {
    return std::any_of(indices.begin(), indices.end(), [dim](const yir::Value *index) {
        AffineDimOffset parsed;
        return parse_dim_offset(index, parsed) && parsed.dim == dim;
    });
}

bool affine_multidim_access(const yir::ArrayLoadOp &load) {
    const auto rank = array_rank_of_value(load.array());
    const auto indices = load.indices();
    if (rank < 2 || indices.size() != rank) {
        return false;
    }
    return std::all_of(indices.begin(), indices.end(), [](const yir::Value *index) {
        AffineDimOffset parsed;
        return parse_dim_offset(index, parsed) && parsed.dim != nullptr;
    });
}

void collect_loop_band_access_stats(const yir::Region &region,
                                    const std::vector<const yir::Value *> &band_dims,
                                    int loop_depth,
                                    LoopBandAccessStats &stats) {
    for (const auto &op : region.operations()) {
        if (auto *load = dynamic_cast<const yir::ArrayLoadOp *>(op.get())) {
            if (affine_multidim_access(*load)) {
                const auto indices = load->indices();
                bool covers_band_dim = false;
                for (const auto *dim : band_dims) {
                    if (access_uses_dim(indices, dim)) {
                        covers_band_dim = true;
                        push_unique_value(stats.covered_band_dims, dim);
                    }
                }
                if (!covers_band_dim) {
                    continue;
                }
                ++stats.affine_loads;
                push_unique_value(stats.load_memories, load->array());
                stats.distinct_load_memories = stats.load_memories.size();
                if (loop_depth > 0) {
                    ++stats.deeper_loop_loads;
                }
            }
            continue;
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            collect_loop_band_access_stats(if_op->then_region(), band_dims, loop_depth, stats);
            if (if_op->has_else()) {
                collect_loop_band_access_stats(if_op->else_region(), band_dims, loop_depth, stats);
            }
            continue;
        }
        if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
            stats.has_extra_loop_depth = true;
            collect_loop_band_access_stats(while_op->body_region(), band_dims, loop_depth + 1,
                                           stats);
            continue;
        }
        if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
            stats.has_extra_loop_depth = true;
            collect_loop_band_access_stats(for_op->body_region(), band_dims, loop_depth + 1,
                                           stats);
        }
    }
}

PolyhedralProfitabilityEstimate estimate_loop_band_profitability(
    const yir::ArrayStoreOp &store) {
    PolyhedralProfitabilityEstimate estimate;
    if (store.parent() == nullptr) {
        return estimate;
    }

    const auto store_indices = store.indices();
    const auto rank = array_rank_of_value(store.array());
    if (rank < 2 || store_indices.size() != rank) {
        return estimate;
    }

    std::vector<const yir::Value *> band_dims;
    band_dims.reserve(2);
    for (std::size_t i = 0; i < 2; ++i) {
        AffineDimOffset parsed;
        if (!parse_dim_offset(store_indices[i], parsed) || parsed.dim == nullptr ||
            vector_contains_value(band_dims, parsed.dim)) {
            return estimate;
        }
        band_dims.push_back(parsed.dim);
    }

    if (!region_has_nested_loop(*store.parent())) {
        return estimate;
    }

    LoopBandAccessStats stats;
    collect_loop_band_access_stats(*store.parent(), band_dims, 0, stats);
    if (!stats.has_extra_loop_depth || stats.affine_loads < 2 ||
        stats.covered_band_dims.size() < band_dims.size() ||
        stats.distinct_load_memories < 2 || stats.deeper_loop_loads == 0) {
        return estimate;
    }

    int score = 96;
    score += static_cast<int>(std::min<std::size_t>(stats.affine_loads, 8) * 8);
    score += static_cast<int>(std::min<std::size_t>(stats.distinct_load_memories, 4) * 12);
    score += static_cast<int>(std::min<std::size_t>(stats.deeper_loop_loads, 4) * 10);

    const auto elements = bounded_array_element_count(store.array());
    if (elements >= 1'000'000) {
        score += 24;
    } else if (elements >= 65'536) {
        score += 12;
    }

    estimate.score = score;
    estimate.best_rank = rank;
    estimate.best_affine_loads = stats.affine_loads;
    estimate.best_unit_neighbor_loads = 0;
    estimate.best_distinct_offsets = stats.distinct_load_memories;
    return estimate;
}

PolyhedralProfitabilityEstimate estimate_store_profitability(
    const yir::ArrayStoreOp &store,
    const std::vector<const yir::Value *> &constant_initialized_arrays) {
    PolyhedralProfitabilityEstimate estimate;
    const auto store_indices = store.indices();
    const auto rank = array_rank_of_value(store.array());
    if (rank < 2 || store_indices.size() != rank || store.parent() == nullptr) {
        return estimate;
    }

    std::vector<AffineDimOffset> store_dims;
    store_dims.reserve(store_indices.size());
    std::vector<const yir::Value *> unique_dims;
    for (auto *index : store_indices) {
        AffineDimOffset dim;
        if (!parse_dim_offset(index, dim) || dim.dim == nullptr ||
            vector_contains_value(unique_dims, dim.dim)) {
            return estimate;
        }
        store_dims.push_back(dim);
        unique_dims.push_back(dim.dim);
    }

    std::size_t same_memory_loads = 0;
    std::size_t affine_loads = 0;
    std::size_t unit_neighbor_loads = 0;
    std::size_t positive_unit_loads = 0;
    std::size_t negative_unit_loads = 0;
    bool has_innermost_previous_load = false;
    std::vector<std::vector<std::int64_t>> distinct_offsets;

    for (const auto &op : store.parent()->operations()) {
        auto *load = dynamic_cast<const yir::ArrayLoadOp *>(op.get());
        if (load == nullptr || load->array() != store.array()) {
            continue;
        }
        ++same_memory_loads;

        std::vector<std::int64_t> offsets;
        if (!parse_access_relative_to_store(load->indices(), store_dims, offsets)) {
            continue;
        }
        ++affine_loads;
        push_unique_offset(distinct_offsets, offsets);

        if (!all_offsets_unit_bounded(offsets) || !any_nonzero_offset(offsets)) {
            continue;
        }
        ++unit_neighbor_loads;
        if (any_positive_offset(offsets)) {
            ++positive_unit_loads;
        }
        if (any_negative_offset(offsets)) {
            ++negative_unit_loads;
        }
        has_innermost_previous_load =
            has_innermost_previous_load || is_innermost_previous_offset(offsets);
    }

    if (same_memory_loads < 2 || affine_loads < 2 || unit_neighbor_loads < 2) {
        return estimate;
    }

    int score = 0;
    score += static_cast<int>(std::min<std::size_t>(affine_loads, 10) * 8);
    score += static_cast<int>(std::min<std::size_t>(unit_neighbor_loads, 10) * 10);
    score += static_cast<int>(std::min<std::size_t>(distinct_offsets.size(), 10) * 6);
    score += rank >= 3 ? 24 : 8;

    if (positive_unit_loads != 0 && negative_unit_loads != 0) {
        score += 36;
    }
    if (positive_unit_loads >= 2 && negative_unit_loads >= 2) {
        score += 24;
    }
    if (has_innermost_previous_load) {
        score += 28;
    }
    if (vector_contains_value(constant_initialized_arrays, store.array()) &&
        positive_unit_loads != 0) {
        score += 36;
    }

    const auto elements = bounded_array_element_count(store.array());
    if (elements >= 1'000'000) {
        score += 28;
    } else if (elements >= 65'536) {
        score += 12;
    }

    estimate.score = score;
    estimate.best_rank = rank;
    estimate.best_affine_loads = affine_loads;
    estimate.best_unit_neighbor_loads = unit_neighbor_loads;
    estimate.best_distinct_offsets = distinct_offsets.size();
    return estimate;
}

void collect_constant_initialized_arrays(const yir::Region &region,
                                         std::vector<const yir::Value *> &arrays) {
    for (const auto &op : region.operations()) {
        if (auto *store = dynamic_cast<const yir::ArrayStoreOp *>(op.get())) {
            std::int64_t constant = 0;
            if (store->indices().size() >= 2 && const_i32_value(store->value(), constant)) {
                push_unique_value(arrays, store->array());
            }
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            collect_constant_initialized_arrays(if_op->then_region(), arrays);
            if (if_op->has_else()) {
                collect_constant_initialized_arrays(if_op->else_region(), arrays);
            }
            continue;
        }
        if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
            collect_constant_initialized_arrays(while_op->cond_region(), arrays);
            collect_constant_initialized_arrays(while_op->body_region(), arrays);
            continue;
        }
        if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
            collect_constant_initialized_arrays(for_op->body_region(), arrays);
        }
    }
}

void estimate_region_profitability(
    const yir::Region &region, const std::vector<const yir::Value *> &constant_initialized_arrays,
    PolyhedralProfitabilityEstimate &estimate) {
    for (const auto &op : region.operations()) {
        if (auto *store = dynamic_cast<const yir::ArrayStoreOp *>(op.get())) {
            auto candidate = estimate_store_profitability(*store, constant_initialized_arrays);
            if (candidate.score > estimate.score) {
                estimate = candidate;
            }
            auto band_candidate = estimate_loop_band_profitability(*store);
            if (band_candidate.score > estimate.score) {
                estimate = band_candidate;
            }
        }
        if (auto *if_op = dynamic_cast<const yir::IfOp *>(op.get())) {
            estimate_region_profitability(if_op->then_region(), constant_initialized_arrays,
                                          estimate);
            if (if_op->has_else()) {
                estimate_region_profitability(if_op->else_region(), constant_initialized_arrays,
                                              estimate);
            }
            continue;
        }
        if (auto *while_op = dynamic_cast<const yir::WhileOp *>(op.get())) {
            estimate_region_profitability(while_op->cond_region(), constant_initialized_arrays,
                                          estimate);
            estimate_region_profitability(while_op->body_region(), constant_initialized_arrays,
                                          estimate);
            continue;
        }
        if (auto *for_op = dynamic_cast<const yir::ForOp *>(op.get())) {
            estimate_region_profitability(for_op->body_region(), constant_initialized_arrays,
                                          estimate);
        }
    }
}

bool module_is_profitable_for_auto_polyhedral(const yir::Module &module,
                                              PolyhedralProfitabilityEstimate &estimate,
                                              YIRPolyhedralFunctionSelection &selection) {
    constexpr int kAutoProfitabilityThreshold = 150;
    std::vector<const yir::Value *> constant_initialized_arrays;
    for (const auto &function : module.functions()) {
        if (function != nullptr) {
            collect_constant_initialized_arrays(function->body(), constant_initialized_arrays);
        }
    }

    estimate = {};
    selection.functions.clear();
    for (const auto &function : module.functions()) {
        if (function == nullptr) {
            continue;
        }
        PolyhedralProfitabilityEstimate function_estimate;
        estimate_region_profitability(function->body(), constant_initialized_arrays,
                                       function_estimate);
        if (function_estimate.score >= kAutoProfitabilityThreshold &&
            function_has_polyhedral_candidate(function->body())) {
            selection.functions.insert(function.get());
        }
        if (function_estimate.score > estimate.score) {
            estimate = function_estimate;
        }
    }
    return !selection.functions.empty();
}

} // namespace

YIRPolyhedralPipelinePass::YIRPolyhedralPipelinePass(bool run_transform,
                                                     bool enable_rvv_preparation)
    : YIRPolyhedralPipelinePass(YIRPolyhedralPipelineMode::Force, run_transform,
                                enable_rvv_preparation) {}

YIRPolyhedralPipelinePass::YIRPolyhedralPipelinePass(YIRPolyhedralPipelineMode mode,
                                                     bool run_transform,
                                                     bool enable_rvv_preparation)
    : mode_(mode), run_transform_(run_transform),
      enable_rvv_preparation_(enable_rvv_preparation) {}

std::string_view YIRPolyhedralPipelinePass::name() const {
    return "YIRPolyhedralPipelinePass";
}

PassKind YIRPolyhedralPipelinePass::kind() const {
    return PassKind::Transform;
}

PassResult YIRPolyhedralPipelinePass::run(PassContext &context) {
    context.erase_artifact(std::string(YIRPolyhedralFunctionSelection::kArtifactKey));
    if (mode_ == YIRPolyhedralPipelineMode::Auto) {
        auto *artifact =
            context.get_artifact<std::unique_ptr<yir::Module>>(ASTToYIRPass::kArtifactKey);
        if (artifact == nullptr || *artifact == nullptr) {
            return PassResult::fail("YIRPolyhedralPipelinePass requires YIR module in pass context");
        }
        PolyhedralProfitabilityEstimate estimate;
        YIRPolyhedralFunctionSelection selection;
        if (!module_is_profitable_for_auto_polyhedral(**artifact, estimate, selection)) {
            std::ostringstream message;
            message << "skipped auto polyhedral pipeline: score=" << estimate.score
                    << ", rank=" << estimate.best_rank
                    << ", affine_loads=" << estimate.best_affine_loads
                    << ", unit_neighbor_loads=" << estimate.best_unit_neighbor_loads
                    << ", distinct_offsets=" << estimate.best_distinct_offsets;
            return PassResult::ok(false, message.str());
        }
        context.set_artifact<YIRPolyhedralFunctionSelection>(
            std::string(YIRPolyhedralFunctionSelection::kArtifactKey), std::move(selection));
    }

    bool changed = false;
    std::size_t execution_count = 0;
    std::size_t analysis_rounds = 0;
    std::size_t structural_rounds = 0;
    bool structural_budget_exhausted = false;

    const auto run_manager = [&](PassManager &pm) -> std::optional<std::string> {
        auto result = pm.run(context);
        execution_count += result.executions.size();
        changed = changed || result.changed;
        if (result.success) {
            return std::nullopt;
        }
        if (result.executions.empty()) {
            return std::string("polyhedral pipeline failed before running any pass");
        }
        const auto &execution = result.executions.back();
        std::string error = execution.name;
        if (!execution.result.message.empty()) {
            error += ": ";
            error += execution.result.message;
        }
        return error;
    };

    const auto run_analysis = [&]() -> std::optional<std::string> {
        PassManager analysis;
        analysis.add_pass<YIRPolyhedralCanonicalizePass>();
        analysis.add_pass<YIRSCoPDetectPass>();
        analysis.add_pass<YIRPolyhedralModelBuildPass>();
        analysis.add_pass<YIRPolyhedralDependenceAnalysisPass>();
        ++analysis_rounds;
        return run_manager(analysis);
    };

    if (!run_transform_) {
        if (auto error = run_analysis()) {
            return PassResult::fail(std::move(*error));
        }
    } else {
        constexpr std::size_t kMaxStructuralRounds = 3;
        YIRPolyhedralStructuralChange last_change =
            YIRPolyhedralStructuralChange::None;
        for (std::size_t round = 0; round < kMaxStructuralRounds; ++round) {
            if (auto error = run_analysis()) {
                return PassResult::fail(std::move(*error));
            }

            PassManager structural;
            structural.add_pass<YIRPolyhedralTransformPass>(
                YIRPolyhedralTransformMode::Structural, enable_rvv_preparation_);
            ++structural_rounds;
            if (auto error = run_manager(structural)) {
                return PassResult::fail(std::move(*error));
            }

            const auto *summary = context.get_artifact<YIRPolyhedralTransformSummary>(
                std::string(YIRPolyhedralTransformSummary::kArtifactKey));
            last_change = summary == nullptr
                              ? YIRPolyhedralStructuralChange::None
                              : summary->structural_change;
            if (last_change != YIRPolyhedralStructuralChange::Fusion) {
                break;
            }
            if (round + 1 == kMaxStructuralRounds) {
                structural_budget_exhausted = true;
            }
        }

        // Any structural rewrite invalidates SCoP membership, schedule bands,
        // dependence relations, and stored operation pointers. Rebuild once
        // more before local model-driven rewrites. Schedule rewrites terminate
        // the structural loop, preventing a rebuilt tiled nest from being
        // tiled again.
        if (last_change != YIRPolyhedralStructuralChange::None) {
            if (auto error = run_analysis()) {
                return PassResult::fail(std::move(*error));
            }
        }

        PassManager local;
        local.add_pass<YIRPolyhedralTransformPass>(
            YIRPolyhedralTransformMode::Local, enable_rvv_preparation_);
        if (auto error = run_manager(local)) {
            return PassResult::fail(std::move(*error));
        }
    }

    std::ostringstream message;
    message << "ran " << execution_count << " polyhedral passes"
            << ", analysis_rounds=" << analysis_rounds
            << ", structural_rounds=" << structural_rounds;
    if (structural_budget_exhausted) {
        message << ", structural_budget_exhausted=true";
    }
    if (mode_ == YIRPolyhedralPipelineMode::Auto) {
        const auto *selection = context.get_artifact<YIRPolyhedralFunctionSelection>(
            std::string(YIRPolyhedralFunctionSelection::kArtifactKey));
        message << ", selected_functions="
                << (selection == nullptr ? 0 : selection->functions.size());
    }
    return PassResult::ok(changed, message.str());
}

} // namespace pass
