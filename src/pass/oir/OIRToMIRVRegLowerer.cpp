#include "builtin/BuiltinRegistry.h"
#include "pass/oir/OIRToMIRCommon.h"
#include "target/RISCVCallingConvention.h"
#include "target/RVVFixedVectorLegalization.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pass::oir_to_mir {
namespace {

target::TargetProfile standard_abi_profile(target::TargetProfile profile) {
    profile.vector_abi = target::VectorABI::Standard;
    return profile;
}

class VRegLowerer final {
  public:
    explicit VRegLowerer(const target::TargetProfile &profile)
        : profile_(profile), calling_convention_(standard_abi_profile(profile)) {
        if (profile_.vector_abi == target::VectorABI::PsABIVector) {
            if (!profile_.vector_bits_explicit ||
                profile_.vector_bits_kind != target::VectorBitsKind::Fixed ||
                !profile_.fixed_vector_bits.has_value()) {
                throw std::runtime_error(
                    "psabi-vector lowering requires explicit numeric ABI_VLEN from "
                    "-mrvv-vector-bits");
            }
            psabi_vector_calling_convention_.emplace(profile_, *profile_.fixed_vector_bits);
        }
    }

    std::unique_ptr<mir::Module> lower(const oir::Module &module) {
        module_ = std::make_unique<mir::Module>(module.name() + ".mir");

        for (const auto &global : module.globals()) {
            mir::Global lowered;
            lowered.name = global->name();
            lowered.type = type_info(global->value_type());
            lowered.is_const = global->is_const();
            lowered.initializer_bytes = lower_global_initializer(*global);
            module_->add_global(std::move(lowered));
        }

        for (const auto &function : module.functions()) {
            const bool standard_runtime = is_standard_runtime_function(*function);
            if (psabi_vector_enabled() && function->is_external() && !standard_runtime) {
                fail_calling_convention(
                    "external function @" + function->name(),
                    "PSABI_VECTOR_EXTERNAL_CC_UNSPECIFIED: source/OIR has no "
                    "structured calling-convention ownership for this declaration");
            }
            if (psabi_vector_enabled() && !standard_runtime) {
                auto assignment = psabi_vector_calling_convention_->assign(
                    psabi_function_type(*function->function_type()));
                validate_psabi_signature_assignment(assignment, "function @" + function->name());
                if (function->function_type()->is_variadic()) {
                    fail_calling_convention(
                        "function @" + function->name(),
                        "variadic signatures are forbidden by psabi-vector compiler lowering");
                }
            }
            std::vector<mir::TypeInfo> params;
            params.reserve(function->function_type()->param_types().size());
            for (auto *param : function->function_type()->param_types()) {
                params.push_back(type_info(param));
            }
            auto *out =
                module_->create_function(function->name(), type_info(function->return_type()),
                                         std::move(params), function->is_external());
            out->set_variant_cc(psabi_vector_enabled() && !standard_runtime);
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
    enum class ActiveVLKind {
        FixedFull,
        Operand,
        VLMAX,
    };

    struct ActiveVectorConfiguration {
        mir::MachineVectorType type;
        mir::VectorTailPolicy tail_policy = mir::VectorTailPolicy::Agnostic;
        mir::VectorMaskPolicy mask_policy = mir::VectorMaskPolicy::Agnostic;
        ActiveVLKind vl_kind = ActiveVLKind::FixedFull;
        const oir::Value *avl = nullptr;
    };

    struct DominatingScalableConfiguration {
        mir::MachineVectorType type;
        oir::Value *avl = nullptr;
    };

    // An oversized source fixed vector is deliberately a lowering-only
    // bundle.  Every piece is an ordinary typed vector vreg, so MIR register
    // allocation and spilling keep operating on single RVV register groups.
    // The planner records the stable logical lane and packed-storage mapping.
    struct FixedVectorBundle final {
        const oir::VectorType *logical_type = nullptr;
        target::RVVFixedVectorLegalizationPlan plan;
        std::vector<mir::Register> pieces;
    };

    // A bundle piece never consumes the logical VP EVL directly.  Its AVL is
    // clamp(global_evl - lane_base, 0, lane_count), so a value larger than the
    // logical vector cannot expose the padding in the final RVV container.
    // Dynamic AVLs are materialized before any synthetic fallback CFG is
    // created, which also gives every piece a stable, dominating SSA identity.
    struct FixedPieceEVL final {
        std::optional<std::int64_t> constant;
        std::optional<mir::Register> reg;
        std::uint64_t lane_base = 0;
        std::uint64_t lane_count = 0;
    };

    [[noreturn]] void fail_vector_legalization(const std::string &message) const {
        throw std::runtime_error("RVV legalization: " + message);
    }

    mir::ValueType vector_element_type(const oir::VectorType &type) const {
        if (type.is_mask()) {
            return mir::ValueType::I1;
        }
        if (type.is_integer_vector()) {
            return mir::ValueType::I32;
        }
        if (type.is_float_vector()) {
            return mir::ValueType::F32;
        }
        fail_vector_legalization("unsupported vector element type in " + type.print());
    }

    target::RVVFixedVectorLegalizationPlan fixed_vector_plan(const oir::VectorType &type) const {
        if (!type.element_count().is_fixed()) {
            fail_vector_legalization("chunk planner received a scalable vector " + type.print());
        }
        target::RVVFixedVectorShape shape;
        shape.logical_lanes = type.element_count().min_lanes;
        shape.sew_bits = 32;
        shape.element_kind =
            type.is_mask()
                ? target::RVVFixedVectorElementKind::Mask
                : (type.is_float_vector() ? target::RVVFixedVectorElementKind::FloatingPoint
                                          : target::RVVFixedVectorElementKind::Integer);
        auto result = target::plan_rvv_fixed_vector_legalization(shape, profile_);
        if (!result.valid) {
            fail_vector_legalization("cannot plan " + type.print() + ": " + result.error);
        }
        return std::move(result.plan);
    }

    mir::VectorLMUL lower_fixed_lmul(target::RVVFixedLMUL lmul) const {
        switch (lmul) {
        case target::RVVFixedLMUL::MF2:
            return mir::VectorLMUL::MF2;
        case target::RVVFixedLMUL::M1:
            return mir::VectorLMUL::M1;
        case target::RVVFixedLMUL::M2:
            return mir::VectorLMUL::M2;
        case target::RVVFixedLMUL::M4:
            return mir::VectorLMUL::M4;
        case target::RVVFixedLMUL::M8:
            return mir::VectorLMUL::M8;
        }
        fail_vector_legalization("planner produced an unknown fixed LMUL");
    }

    mir::MachineVectorType machine_type_for_piece(const oir::VectorType &logical_type,
                                                  const target::RVVFixedVectorPiece &piece) const {
        if (piece.lane_count == 0 || piece.lane_count > std::numeric_limits<unsigned>::max()) {
            fail_vector_legalization("fixed chunk lane count is outside MIR range");
        }
        const auto element =
            logical_type.is_float_vector() ? mir::ValueType::F32 : mir::ValueType::I32;
        auto data = mir::MachineVectorType::fixed(element, 32, lower_fixed_lmul(piece.lmul),
                                                  static_cast<unsigned>(piece.lane_count));
        return logical_type.is_mask() ? mir::MachineVectorType::mask_for(data) : data;
    }

    bool is_oversized_fixed_vector(const oir::Type *type) const {
        const auto *vector = dynamic_cast<const oir::VectorType *>(type);
        return profile_.has_vector() && vector != nullptr && vector->element_count().is_fixed() &&
               fixed_vector_plan(*vector).pieces.size() > 1;
    }

    FixedVectorBundle allocate_fixed_bundle(const oir::VectorType &type) {
        FixedVectorBundle bundle;
        bundle.logical_type = &type;
        bundle.plan = fixed_vector_plan(type);
        if (bundle.plan.pieces.size() <= 1) {
            fail_vector_legalization("internal bundle allocation requested for single-piece " +
                                     type.print());
        }
        bundle.pieces.reserve(bundle.plan.pieces.size());
        for (const auto &piece : bundle.plan.pieces) {
            bundle.pieces.push_back(create_vector_vreg(machine_type_for_piece(type, piece)));
        }
        return bundle;
    }

    mir::VectorLMUL select_fixed_lmul(const oir::VectorType &type) const {
        const auto lanes = type.element_count().min_lanes;
        const auto minimum_vlen = profile_.minimum_vlen_bits;
        if (minimum_vlen == 0) {
            fail_vector_legalization(
                "fixed vector type reached a target without a guaranteed VLEN");
        }
        // e32,mf4/mf8 would require vbool128/vbool256 for the mask SSA
        // values produced by comparisons and selects.  RVV 1.0 only defines
        // mask ratios through vbool64, so even a logical one-lane vector uses
        // an mf2 physical container.
        const std::pair<mir::VectorLMUL, unsigned> candidates[] = {
            {mir::VectorLMUL::MF2, 4}, {mir::VectorLMUL::M1, 8},  {mir::VectorLMUL::M2, 16},
            {mir::VectorLMUL::M4, 32}, {mir::VectorLMUL::M8, 64},
        };
        for (const auto &[lmul, eighths] : candidates) {
            const auto capacity = (static_cast<std::uint64_t>(minimum_vlen) * eighths) / (32U * 8U);
            if (capacity >= lanes) {
                return lmul;
            }
        }
        fail_vector_legalization(
            "fixed vector " + type.print() +
            " exceeds one portable RVV register group at the target minimum VLEN; "
            "chunking/scalarization is required");
    }

    mir::VectorLMUL select_scalable_lmul(const oir::VectorType &type) const {
        if (!type.element_count().is_scalable()) {
            fail_vector_legalization("scalable LMUL selection received a fixed vector");
        }
        const struct {
            target::RVVFixedLMUL planner_lmul;
            mir::VectorLMUL machine_lmul;
        } candidates[] = {
            {target::RVVFixedLMUL::MF2, mir::VectorLMUL::MF2},
            {target::RVVFixedLMUL::M1, mir::VectorLMUL::M1},
            {target::RVVFixedLMUL::M2, mir::VectorLMUL::M2},
            {target::RVVFixedLMUL::M4, mir::VectorLMUL::M4},
            {target::RVVFixedLMUL::M8, mir::VectorLMUL::M8},
        };
        for (const auto &candidate : candidates) {
            const auto capacity = target::rvv_fixed_vector_minimum_capacity(
                profile_.minimum_vlen_bits, 32, candidate.planner_lmul);
            if (capacity.valid &&
                capacity.minimum_capacity_lanes >= type.element_count().min_lanes) {
                return candidate.machine_lmul;
            }
        }
        fail_vector_legalization("scalable vector " + type.print() +
                                 " has a minimum lane shape larger than RVV LMUL=m8 can represent");
    }

    mir::MachineVectorType legalize_vector_type(const oir::VectorType &type) const {
        if (!profile_.has_vector()) {
            fail_vector_legalization("vector OIR type " + type.print() +
                                     " reached a target without V or Zve; portable "
                                     "scalarization has not run");
        }
        const bool is_float = type.is_float_vector();
        if (!type.is_mask() && !profile_.supports_vector_element(is_float, 32)) {
            fail_vector_legalization("target " + profile_.march + " does not support " +
                                     type.print());
        }

        // The scalable type's minimum lane count is the vectorizer/legalizer
        // contract for LMUL.  Runtime VL remains VLA, while SEW/LMUL must
        // agree with that plan so capacity, pressure and final vsetvli are not
        // silently different from the selected cost-model candidate.
        const auto lmul = type.element_count().is_scalable() ? select_scalable_lmul(type)
                                                             : select_fixed_lmul(type);
        const auto data_element =
            type.is_float_vector() ? mir::ValueType::F32 : mir::ValueType::I32;
        mir::MachineVectorType data =
            type.element_count().is_scalable()
                ? mir::MachineVectorType::scalable(data_element, 32, lmul)
                : mir::MachineVectorType::fixed(
                      data_element, 32, lmul,
                      static_cast<unsigned>(type.element_count().min_lanes));
        return type.is_mask() ? mir::MachineVectorType::mask_for(data) : data;
    }

    mir::MachineVectorType data_config_for(const mir::MachineVectorType &type) const {
        if (!type.is_mask()) {
            return type;
        }
        if (type.is_scalable()) {
            return mir::MachineVectorType::scalable(mir::ValueType::I32, type.sew_bits(),
                                                    type.lmul());
        }
        return mir::MachineVectorType::fixed(mir::ValueType::I32, type.sew_bits(), type.lmul(),
                                             type.fixed_lanes());
    }

    bool has_equivalent_vtype(const mir::MachineVectorType &lhs,
                              const mir::MachineVectorType &rhs) const {
        // VTYPE encodes SEW, LMUL and the container shape; it does not encode
        // whether e32 lanes subsequently feed integer or floating-point
        // instructions.  This matters for a scalable mask splat used by an
        // f32 VP operation: the mask's canonical data configuration is i32,
        // while its dominating SetVL naturally carries f32.
        return lhs.sew_bits() == rhs.sew_bits() && lhs.lmul() == rhs.lmul() &&
               lhs.container_kind() == rhs.container_kind() &&
               lhs.fixed_lanes() == rhs.fixed_lanes();
    }

    mir::TypeInfo type_info(const oir::Type *type) const {
        mir::TypeInfo out;
        out.ir = type == nullptr ? "void" : type->print();
        out.align = 1;

        if (type == nullptr || type->is_void()) {
            out.value_type = mir::ValueType::Void;
            out.size = 0;
            return out;
        }
        if (auto *integer = dynamic_cast<const oir::IntegerType *>(type)) {
            out.value_type = integer->bit_width() == 1 ? mir::ValueType::I1 : mir::ValueType::I32;
            out.size = integer->bit_width() == 1 ? 1 : 4;
            out.align = integer->bit_width() == 1 ? 1 : 4;
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
        if (auto *array = dynamic_cast<const oir::ArrayType *>(type)) {
            auto element = type_info(array->element_type());
            out.value_type = mir::ValueType::Aggregate;
            out.size = element.size * array->element_count();
            out.align = element.align;
            return out;
        }
        if (type->is_vector()) {
            const auto *vector = dynamic_cast<const oir::VectorType *>(type);
            if (vector == nullptr) {
                fail_vector_legalization("malformed OIR vector type " + type->print());
            }
            if (!profile_.has_vector() && !vector->element_count().is_fixed()) {
                fail_vector_legalization("scalable vector type " + type->print() +
                                         " reached the fixed-only portable ABI boundary");
            }
            const auto size = calling_convention_.data_layout().fixed_alloc_size(type);
            out.value_type = mir::ValueType::Aggregate;
            out.size = size.value_or(0);
            out.align =
                size.has_value() ? calling_convention_.data_layout().abi_alignment(type) : 1;
            if (profile_.has_vector() && (!vector->element_count().is_fixed() ||
                                          fixed_vector_plan(*vector).pieces.size() == 1)) {
                out.vector_type = legalize_vector_type(*vector);
            }
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

    mir::Register create_vector_vreg(const mir::MachineVectorType &type) {
        const auto reg_class =
            type.is_mask() ? mir::RegisterClass::VMASK : mir::RegisterClass::VRNoV0;
        return current_function_->regs().create_virtual_vector(reg_class, type);
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

    IntRange union_ranges(const IntRange &lhs, const IntRange &rhs, const oir::Type *type) const {
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
        return range.valid ? range
                           : full_range_for_type(value == nullptr ? nullptr : value->type());
    }

    bool value_is_nonnegative(const oir::Value *value) const {
        auto range = range_for_lowering(value);
        return range.valid && range.lower >= 0;
    }

    struct VectorIndexRange {
        bool valid = false;
        std::int64_t lower = 0;
        std::int64_t upper = 0;
    };

    struct AffineVectorIndex {
        bool valid = false;
        // Element index for lane zero and the signed element-index delta
        // between consecutive lanes.  Both describe mathematical integers;
        // the recognizer rejects every recipe that can wrap its OIR i32
        // semantics over the architectural lane range.
        const oir::Value *dynamic_origin = nullptr;
        std::int64_t dynamic_scale = 0;
        std::int64_t origin = 0;
        std::int64_t stride = 0;
    };

    bool checked_i32(__int128 value, std::int64_t &out) const {
        if (value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }
        out = static_cast<std::int64_t>(value);
        return true;
    }

    AffineVectorIndex combine_affine_indices(oir::Instruction::OpID operation,
                                             const AffineVectorIndex &lhs,
                                             const AffineVectorIndex &rhs) const {
        if (!lhs.valid || !rhs.valid) {
            return {};
        }
        AffineVectorIndex result;
        result.valid = true;
        const auto combine_dynamic = [&](std::int64_t rhs_sign) -> bool {
            if (lhs.dynamic_origin != nullptr && rhs.dynamic_origin != nullptr &&
                lhs.dynamic_origin != rhs.dynamic_origin) {
                return false;
            }
            result.dynamic_origin = lhs.dynamic_origin != nullptr ? lhs.dynamic_origin
                                                                  : rhs.dynamic_origin;
            const auto lhs_scale = lhs.dynamic_origin == nullptr ? 0 : lhs.dynamic_scale;
            const auto rhs_scale = rhs.dynamic_origin == nullptr ? 0 : rhs.dynamic_scale;
            if (!checked_i32(static_cast<__int128>(lhs_scale) +
                                 static_cast<__int128>(rhs_sign) * rhs_scale,
                             result.dynamic_scale)) {
                return false;
            }
            if (result.dynamic_scale == 0) {
                result.dynamic_origin = nullptr;
            }
            return true;
        };
        switch (operation) {
        case oir::Instruction::OpID::Add:
            if (!combine_dynamic(1) ||
                !checked_i32(static_cast<__int128>(lhs.origin) + rhs.origin,
                             result.origin) ||
                !checked_i32(static_cast<__int128>(lhs.stride) + rhs.stride,
                             result.stride)) {
                return {};
            }
            return result;
        case oir::Instruction::OpID::Sub:
            if (!combine_dynamic(-1) ||
                !checked_i32(static_cast<__int128>(lhs.origin) - rhs.origin,
                             result.origin) ||
                !checked_i32(static_cast<__int128>(lhs.stride) - rhs.stride,
                             result.stride)) {
                return {};
            }
            return result;
        case oir::Instruction::OpID::Mul: {
            // The product of two lane-varying affine expressions is not an
            // arithmetic progression.  A uniform operand is sufficient and
            // covers stepvector times a positive or negative constant splat.
            const auto is_constant_uniform = [](const AffineVectorIndex &index) {
                return index.stride == 0 && index.dynamic_origin == nullptr;
            };
            const auto *uniform = is_constant_uniform(lhs)
                                      ? &lhs
                                      : is_constant_uniform(rhs) ? &rhs : nullptr;
            const auto *varying = uniform == &lhs ? &rhs : &lhs;
            if (uniform == nullptr ||
                !checked_i32(static_cast<__int128>(varying->origin) * uniform->origin,
                             result.origin) ||
                !checked_i32(static_cast<__int128>(varying->dynamic_scale) *
                                 uniform->origin,
                             result.dynamic_scale) ||
                !checked_i32(static_cast<__int128>(varying->stride) * uniform->origin,
                             result.stride)) {
                return {};
            }
            result.dynamic_origin = result.dynamic_scale == 0 ? nullptr
                                                               : varying->dynamic_origin;
            return result;
        }
        default:
            return {};
        }
    }

    AffineVectorIndex vector_index_affine(const oir::Value *value,
                                          const oir::Value *active_mask,
                                          const oir::Value *evl,
                                          unsigned depth = 0) const {
        if (value == nullptr || depth > 32 || value->type() == nullptr ||
            !value->type()->is_vector()) {
            return {};
        }
        if (dynamic_cast<const oir::ConstantZero *>(value) != nullptr) {
            return {true, nullptr, 0, 0, 0};
        }
        if (const auto *constant = dynamic_cast<const oir::ConstantVector *>(value)) {
            const auto &elements = constant->elements();
            if (elements.empty()) {
                return {};
            }
            const auto *first = dynamic_cast<const oir::ConstantInt *>(elements.front());
            if (first == nullptr) {
                return {};
            }
            std::int64_t stride = 0;
            if (elements.size() > 1) {
                const auto *second = dynamic_cast<const oir::ConstantInt *>(elements[1]);
                if (second == nullptr ||
                    !checked_i32(static_cast<__int128>(second->value()) - first->value(),
                                 stride)) {
                    return {};
                }
            }
            for (std::size_t lane = 0; lane < elements.size(); ++lane) {
                const auto *element = dynamic_cast<const oir::ConstantInt *>(elements[lane]);
                std::int64_t expected = 0;
                if (element == nullptr ||
                    !checked_i32(static_cast<__int128>(first->value()) +
                                     static_cast<__int128>(stride) * lane,
                                 expected) ||
                    element->value() != expected) {
                    return {};
                }
            }
            return {true, nullptr, 0, first->value(), stride};
        }
        if (const auto *splat = dynamic_cast<const oir::SplatInst *>(value)) {
            const auto range = range_for_lowering(splat->scalar());
            if (range.valid && range.lower == range.upper) {
                return {true, nullptr, 0, range.lower, 0};
            }
            return {true, splat->scalar(), 1, 0, 0};
        }
        if (dynamic_cast<const oir::StepVectorInst *>(value) != nullptr) {
            return {true, nullptr, 0, 0, 1};
        }
        if (const auto *binary = dynamic_cast<const oir::VPBinaryInst *>(value)) {
            if (binary->active_mask() != active_mask || binary->evl() != evl) {
                return {};
            }
            return combine_affine_indices(
                binary->binary_op(),
                vector_index_affine(binary->lhs(), active_mask, evl, depth + 1),
                vector_index_affine(binary->rhs(), active_mask, evl, depth + 1));
        }
        if (const auto *binary = dynamic_cast<const oir::BinaryInst *>(value)) {
            return combine_affine_indices(
                binary->op(),
                vector_index_affine(binary->lhs(), active_mask, evl, depth + 1),
                vector_index_affine(binary->rhs(), active_mask, evl, depth + 1));
        }
        return {};
    }

    std::optional<AffineVectorIndex>
    strided_index_plan(const oir::Value *indices, const oir::Value *active_mask,
                       const oir::Value *evl,
                       bool allow_zero_stride = false) const {
        const auto affine = vector_index_affine(indices, active_mask, evl);
        const auto range = vector_index_range(indices, active_mask, evl);
        if (!affine.valid || (!allow_zero_stride && affine.stride == 0) ||
            !range.valid) {
            return std::nullopt;
        }
        // Requiring range analysis to agree makes the transform fail closed
        // on any intermediate i32 wrap.  RVV signed strided memory can then
        // implement negative as well as positive element indexes directly.
        const auto byte_origin = static_cast<__int128>(affine.origin) * 4;
        const auto byte_stride = static_cast<__int128>(affine.stride) * 4;
        if (byte_origin < std::numeric_limits<std::int64_t>::min() ||
            byte_origin > std::numeric_limits<std::int64_t>::max() ||
            byte_stride < std::numeric_limits<std::int32_t>::min() ||
            byte_stride > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        return affine;
    }

    std::int64_t strided_piece_origin(const AffineVectorIndex &plan,
                                      std::uint64_t lane_base) const {
        const auto origin = static_cast<__int128>(plan.origin) +
                            static_cast<__int128>(plan.stride) * lane_base;
        const auto byte_origin = origin * 4;
        if (origin < std::numeric_limits<std::int32_t>::min() ||
            origin > std::numeric_limits<std::int32_t>::max() ||
            byte_origin < std::numeric_limits<std::int64_t>::min() ||
            byte_origin > std::numeric_limits<std::int64_t>::max()) {
            fail_vector_legalization("strided vector piece origin overflows address semantics");
        }
        return static_cast<std::int64_t>(origin);
    }

    mir::Register materialize_strided_address(mir::Register base,
                                              const AffineVectorIndex &plan,
                                              std::uint64_t lane_base = 0) {
        const auto constant_origin = strided_piece_origin(plan, lane_base);
        if (plan.dynamic_origin == nullptr) {
            return address_with_offset(std::move(base), constant_origin * 4);
        }

        auto element_origin = value_reg(const_cast<oir::Value *>(plan.dynamic_origin));
        if (plan.dynamic_scale != 1) {
            auto scale = materialize_internal_i32(plan.dynamic_scale);
            auto scaled = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::MulW,
                 {mir::MachineOperand::reg_def(scaled),
                  mir::MachineOperand::reg_use(element_origin),
                  mir::MachineOperand::reg_use(scale)});
            element_origin = scaled;
        }
        if (constant_origin != 0) {
            auto constant = materialize_internal_i32(constant_origin);
            auto adjusted = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::AddW,
                 {mir::MachineOperand::reg_def(adjusted),
                  mir::MachineOperand::reg_use(element_origin),
                  mir::MachineOperand::reg_use(constant)});
            element_origin = adjusted;
        }
        auto byte_offset = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::SllI,
             {mir::MachineOperand::reg_def(byte_offset),
              mir::MachineOperand::reg_use(element_origin), mir::MachineOperand::imm(2)});
        auto address = create_vreg(mir::ValueType::Ptr);
        emit(mir::Opcode::Add,
             {mir::MachineOperand::reg_def(address), mir::MachineOperand::reg_use(base),
              mir::MachineOperand::reg_use(byte_offset)});
        return address;
    }

    mir::Register materialize_strided_byte_stride(const AffineVectorIndex &plan) {
        return materialize_internal_i32(plan.stride * 4);
    }

    VectorIndexRange checked_vector_index_binary(oir::Instruction::OpID operation,
                                                 const VectorIndexRange &lhs,
                                                 const VectorIndexRange &rhs) const {
        if (!lhs.valid || !rhs.valid) {
            return {};
        }
        const auto checked = [](const __int128 value, std::int64_t &out) -> bool {
            if (value < std::numeric_limits<std::int32_t>::min() ||
                value > std::numeric_limits<std::int32_t>::max()) {
                return false;
            }
            out = static_cast<std::int64_t>(value);
            return true;
        };
        __int128 candidates[4] = {};
        std::size_t candidate_count = 0;
        switch (operation) {
        case oir::Instruction::OpID::Add:
            candidates[0] = static_cast<__int128>(lhs.lower) + rhs.lower;
            candidates[1] = static_cast<__int128>(lhs.upper) + rhs.upper;
            candidate_count = 2;
            break;
        case oir::Instruction::OpID::Sub:
            candidates[0] = static_cast<__int128>(lhs.lower) - rhs.upper;
            candidates[1] = static_cast<__int128>(lhs.upper) - rhs.lower;
            candidate_count = 2;
            break;
        case oir::Instruction::OpID::Mul:
            candidates[0] = static_cast<__int128>(lhs.lower) * rhs.lower;
            candidates[1] = static_cast<__int128>(lhs.lower) * rhs.upper;
            candidates[2] = static_cast<__int128>(lhs.upper) * rhs.lower;
            candidates[3] = static_cast<__int128>(lhs.upper) * rhs.upper;
            candidate_count = 4;
            break;
        default:
            return {};
        }
        auto minimum = candidates[0];
        auto maximum = candidates[0];
        for (std::size_t index = 1; index < candidate_count; ++index) {
            minimum = std::min(minimum, candidates[index]);
            maximum = std::max(maximum, candidates[index]);
        }
        VectorIndexRange result{true, 0, 0};
        if (!checked(minimum, result.lower) || !checked(maximum, result.upper)) {
            return {};
        }
        return result;
    }

    VectorIndexRange vector_index_range(const oir::Value *value, const oir::Value *active_mask,
                                        const oir::Value *evl, unsigned depth = 0) const {
        if (value == nullptr || depth > 32 || value->type() == nullptr ||
            !value->type()->is_vector()) {
            return {};
        }
        if (dynamic_cast<const oir::ConstantZero *>(value) != nullptr) {
            return {true, 0, 0};
        }
        if (auto *constant = dynamic_cast<const oir::ConstantVector *>(value)) {
            VectorIndexRange result;
            for (auto *element : constant->elements()) {
                const auto *integer = dynamic_cast<const oir::ConstantInt *>(element);
                if (integer == nullptr) {
                    return {};
                }
                if (!result.valid) {
                    result = {true, integer->value(), integer->value()};
                } else {
                    result.lower = std::min(result.lower, integer->value());
                    result.upper = std::max(result.upper, integer->value());
                }
            }
            return result;
        }
        if (auto *splat = dynamic_cast<const oir::SplatInst *>(value)) {
            const auto range = range_for_lowering(splat->scalar());
            return range.valid ? VectorIndexRange{true, range.lower, range.upper}
                               : VectorIndexRange{};
        }
        if (auto *step = dynamic_cast<const oir::StepVectorInst *>(value)) {
            const auto *type = dynamic_cast<const oir::VectorType *>(step->type());
            if (type == nullptr) {
                return {};
            }
            // RVV 1.0 bounds VLEN at 65536 bits.  With SEW=32 and LMUL=8,
            // vid.v can therefore produce at most lane 16383.
            const auto upper =
                type->element_count().is_scalable()
                    ? std::int64_t{16383}
                    : static_cast<std::int64_t>(type->element_count().min_lanes - 1U);
            return {true, 0, upper};
        }
        if (auto *binary = dynamic_cast<const oir::VPBinaryInst *>(value)) {
            if (binary->active_mask() != active_mask || binary->evl() != evl) {
                return {};
            }
            if (binary->binary_op() == oir::Instruction::OpID::Sub) {
                // The loop vectorizer represents a reverse chunk as
                // splat(actual_vl - 1) - stepvector.  Scalar range analysis
                // quite correctly gives (actual_vl - 1) a possible lower
                // bound of -1 when VL is zero, but no lane executes in that
                // case.  For every executing lane k, 0 <= k < actual_vl, so
                // the VP result is provably in [0, VLMAX - 1].  Recognizing
                // this exact recipe lets the adjusted-low-base loop use
                // zero-extended ordered ei32 offsets safely.
                const auto *last_splat = dynamic_cast<const oir::SplatInst *>(binary->lhs());
                const auto *step = dynamic_cast<const oir::StepVectorInst *>(binary->rhs());
                const auto *last =
                    last_splat == nullptr
                        ? nullptr
                        : dynamic_cast<const oir::BinaryInst *>(last_splat->scalar());
                const auto *one =
                    last == nullptr ? nullptr : dynamic_cast<const oir::ConstantInt *>(last->rhs());
                if (step != nullptr && last != nullptr &&
                    last->op() == oir::Instruction::OpID::Sub && last->lhs() == evl &&
                    dynamic_cast<const oir::SetVLInst *>(evl) != nullptr && one != nullptr &&
                    one->value() == 1) {
                    const auto *type = dynamic_cast<const oir::VectorType *>(binary->type());
                    if (type != nullptr) {
                        const auto upper =
                            type->element_count().is_scalable()
                                ? std::int64_t{16383}
                                : static_cast<std::int64_t>(type->element_count().min_lanes - 1U);
                        return {true, 0, upper};
                    }
                }
            }
            return checked_vector_index_binary(
                binary->binary_op(), vector_index_range(binary->lhs(), active_mask, evl, depth + 1),
                vector_index_range(binary->rhs(), active_mask, evl, depth + 1));
        }
        if (auto *binary = dynamic_cast<const oir::BinaryInst *>(value)) {
            return checked_vector_index_binary(
                binary->op(), vector_index_range(binary->lhs(), active_mask, evl, depth + 1),
                vector_index_range(binary->rhs(), active_mask, evl, depth + 1));
        }
        return {};
    }

    bool indices_are_safe_e32_byte_offsets(const oir::Value *indices, const oir::Value *active_mask,
                                           const oir::Value *evl) const {
        const auto range = vector_index_range(indices, active_mask, evl);
        return range.valid && range.lower >= 0 &&
               static_cast<std::uint64_t>(range.upper) <=
                   std::numeric_limits<std::uint32_t>::max() / 4U;
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
            return make_range_for_type(inst.type(), lhs.lower + rhs.lower, lhs.upper + rhs.upper);
        case oir::Instruction::OpID::Sub:
            return make_range_for_type(inst.type(), lhs.lower - rhs.upper, lhs.upper - rhs.lower);
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
        case oir::Instruction::OpID::Or:
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
        case oir::Instruction::OpID::Or:
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

    void apply_icmp_constraint(RangeMap &map, const oir::CmpInst &cmp, bool branch_taken) const {
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

    void merge_successor_entry(const oir::BasicBlock *succ, const RangeMap &candidate,
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

    [[noreturn]] void fail_calling_convention(const std::string &context,
                                              const std::string &message) const {
        throw std::runtime_error("RISC-V calling convention for " + context + ": " + message);
    }

    bool psabi_vector_enabled() const {
        return profile_.vector_abi == target::VectorABI::PsABIVector;
    }

    bool is_standard_runtime_function(const oir::Function &function) const {
        const auto *descriptor = builtin::BuiltinRegistry::instance().find(function.name());
        return descriptor != nullptr && descriptor->lowering == builtin::LoweringKind::RuntimeCall;
    }

    target::CCPSABIVectorFunctionType psabi_function_type(const oir::FunctionType &type) const {
        target::CCPSABIVectorFunctionType result;
        result.return_value = {type.return_type(), 1};
        result.variadic = type.is_variadic();
        result.prototyped = true;
        result.parameters.reserve(type.param_types().size());
        for (auto *parameter : type.param_types()) {
            result.parameters.push_back({parameter, 1});
        }
        return result;
    }

    bool psabi_direct_vector(const target::CCPSABIVectorValueAssignment &assignment) const {
        return assignment.kind != target::CCPSABIVectorValueKind::NonVector &&
               !assignment.value.indirect && assignment.vector_group.has_value();
    }

    void validate_psabi_value_assignment(const target::CCPSABIVectorValueAssignment &assignment,
                                         const std::string &context, bool allow_stack) const {
        const auto *type = assignment.value.type;
        if (type == nullptr || type->is_void()) {
            fail_calling_convention(context, "missing non-void value type");
        }
        if (assignment.tuple_fields != 1 ||
            assignment.kind == target::CCPSABIVectorValueKind::Tuple) {
            fail_calling_convention(context, "vector tuples are not source-level OIR ABI values");
        }
        if (psabi_direct_vector(assignment)) {
            const auto *vector = dynamic_cast<const oir::VectorType *>(type);
            if (vector == nullptr || !vector->element_count().is_fixed() ||
                !assignment.value.locations.empty()) {
                fail_calling_convention(context, "malformed direct vector-register assignment");
            }
            if (is_oversized_fixed_vector(type)) {
                fail_calling_convention(context,
                                        "an oversized fixed vector must use indirect ABI fallback");
            }
            const auto machine_type = legalize_vector_type(*vector);
            const auto &group = *assignment.vector_group;
            if (group.register_count != machine_type.register_group_width() ||
                group.first_register + group.register_count > 32U ||
                (group.first_register % machine_type.register_group_alignment()) != 0U) {
                fail_calling_convention(
                    context, "vector ABI register group disagrees with MIR legalization");
            }
            if (vector->is_mask() && group.register_count != 1U) {
                fail_calling_convention(context,
                                        "fixed mask ABI assignment must occupy one register");
            }
            return;
        }
        validate_value_assignment(assignment.value, context, allow_stack);
        if (assignment.kind != target::CCPSABIVectorValueKind::NonVector &&
            !assignment.value.indirect) {
            fail_calling_convention(
                context, "vector assignment has neither a register group nor indirect storage");
        }
    }

    void
    validate_psabi_signature_assignment(const target::CCPSABIVectorSignatureAssignment &assignment,
                                        const std::string &context) const {
        if (!assignment.valid) {
            fail_calling_convention(context, assignment.error);
        }
        if (assignment.return_value.value.type != nullptr &&
            !assignment.return_value.value.type->is_void()) {
            validate_psabi_value_assignment(assignment.return_value, context + " return value",
                                            false);
            if (assignment.has_sret &&
                (!assignment.return_value.value.indirect ||
                 !is_fixed_vector_abi_value(assignment.return_value.value.type))) {
                fail_calling_convention(
                    context, "psabi-vector sret is only legal for a fixed vector or mask");
            }
        }
        for (std::size_t index = 0; index < assignment.parameters.size(); ++index) {
            validate_psabi_value_assignment(assignment.parameters[index],
                                            context + " parameter " + std::to_string(index), true);
        }
    }

    mir::Register psabi_vector_register(const target::CCPSABIVectorValueAssignment &assignment,
                                        const std::string &context) const {
        if (!psabi_direct_vector(assignment)) {
            fail_calling_convention(context, "expected a direct vector-register assignment");
        }
        const auto *vector = dynamic_cast<const oir::VectorType *>(assignment.value.type);
        if (vector == nullptr) {
            fail_calling_convention(context, "vector assignment has a scalar type");
        }
        const auto machine_type = legalize_vector_type(*vector);
        const auto &group = *assignment.vector_group;
        const auto reg_class =
            vector->is_mask() ? mir::RegisterClass::VMASK : mir::RegisterClass::VRNoV0;
        return mir::Register::physical_vector("v" + std::to_string(group.first_register), reg_class,
                                              machine_type);
    }

    bool is_fixed_vector_abi_value(const oir::Type *type) const {
        const auto *vector = dynamic_cast<const oir::VectorType *>(type);
        return vector != nullptr && vector->element_count().is_fixed();
    }

    bool portable_fixed_abi_enabled() const {
        return !profile_.has_vector() && profile_.vector_abi == target::VectorABI::Standard;
    }

    [[noreturn]] void fail_missing_portable_boundary(const oir::Function &function,
                                                     const oir::Instruction &instruction) const {
        fail_vector_legalization(
            "fixed RVV legalization requires a vector-enabled target; function @" +
            function.name() + " contains unscalarized operation '" + instruction.print() +
            "'. Run OIRPortableVectorScalarizerPass before lower_with_vregs");
    }

    void validate_portable_boundary_function(const oir::Function &function) const {
        if (!portable_fixed_abi_enabled()) {
            return;
        }
        for (const auto &block : function.blocks()) {
            for (const auto &instruction : block->instructions()) {
                const auto result_is_fixed = is_fixed_vector_abi_value(instruction->type());
                if (result_is_fixed && instruction->op() != oir::Instruction::OpID::FixedABIPack &&
                    instruction->op() != oir::Instruction::OpID::Call &&
                    instruction->op() != oir::Instruction::OpID::Load) {
                    fail_missing_portable_boundary(function, *instruction);
                }

                for (auto *operand : instruction->operands()) {
                    if (!is_fixed_vector_abi_value(operand == nullptr ? nullptr
                                                                      : operand->type())) {
                        continue;
                    }
                    bool allowed = false;
                    if (instruction->op() == oir::Instruction::OpID::FixedABIExtractLane) {
                        const auto *extract =
                            dynamic_cast<const oir::FixedABIExtractLaneInst *>(instruction.get());
                        allowed = extract != nullptr && extract->aggregate() == operand;
                    } else if (instruction->op() == oir::Instruction::OpID::Call ||
                               instruction->op() == oir::Instruction::OpID::Ret ||
                               instruction->op() == oir::Instruction::OpID::Store) {
                        allowed = dynamic_cast<const oir::FixedABIPackInst *>(operand) != nullptr;
                    }
                    if (!allowed) {
                        fail_missing_portable_boundary(function, *instruction);
                    }
                }
            }
        }
    }

    mir::ValueType validate_value_assignment(const target::CCValueAssignment &assignment,
                                             const std::string &context, bool allow_stack) const {
        if (assignment.type == nullptr || assignment.type->is_void()) {
            fail_calling_convention(context, "missing non-void value type");
        }

        const auto value_type = type_info(assignment.type).value_type;
        if (value_type == mir::ValueType::Aggregate) {
            if (!is_fixed_vector_abi_value(assignment.type)) {
                fail_calling_convention(
                    context, "only fixed vector and mask aggregate values are implemented by "
                             "the MIR lowerer");
            }
            if (assignment.locations.empty() ||
                (!assignment.indirect && assignment.locations.size() > 2) ||
                (assignment.indirect && assignment.locations.size() != 1)) {
                fail_calling_convention(context, "malformed fixed-vector aggregate assignment");
            }
            for (const auto &location : assignment.locations) {
                if (location.kind == target::CCLocationKind::FPR32) {
                    fail_calling_convention(
                        context, "fixed vectors and masks must use integer aggregate locations");
                }
                if (location.kind == target::CCLocationKind::Stack && !allow_stack) {
                    fail_calling_convention(context,
                                            "a return value cannot be assigned to the stack");
                }
            }
            return value_type;
        }
        if (assignment.indirect) {
            fail_calling_convention(context, "non-aggregate value was assigned indirectly");
        }
        if (assignment.locations.size() != 1) {
            fail_calling_convention(context, "scalar value does not have exactly one ABI location");
        }

        const auto &location = assignment.locations.front();
        if (location.kind == target::CCLocationKind::Stack && !allow_stack) {
            fail_calling_convention(context, "a return value cannot be assigned to the stack");
        }
        if (location.kind == target::CCLocationKind::FPR32 && value_type != mir::ValueType::F32) {
            fail_calling_convention(context, "non-float value was assigned to an FPR");
        }
        return value_type;
    }

    void validate_signature_assignment(const target::CCSignatureAssignment &assignment,
                                       const std::string &context) const {
        if (!assignment.valid) {
            fail_calling_convention(context, assignment.error);
        }
        if (assignment.return_value.type != nullptr && !assignment.return_value.type->is_void()) {
            validate_value_assignment(assignment.return_value, context + " return value", false);
            if (assignment.has_sret && (!assignment.return_value.indirect ||
                                        !is_fixed_vector_abi_value(assignment.return_value.type))) {
                fail_calling_convention(context,
                                        "sret is only implemented for fixed vectors and masks");
            }
        }
        for (std::size_t index = 0; index < assignment.parameters.size(); ++index) {
            validate_value_assignment(assignment.parameters[index],
                                      context + " parameter " + std::to_string(index), true);
        }
    }

    const target::CCLocation &single_location(const target::CCValueAssignment &assignment) const {
        if (assignment.locations.size() != 1) {
            fail_calling_convention("internal lowering", "expected a single scalar ABI location");
        }
        return assignment.locations.front();
    }

    mir::Register physical_register(const target::CCLocation &location,
                                    const std::string &context) const {
        switch (location.kind) {
        case target::CCLocationKind::GPR:
            return phys_gpr(location.register_name);
        case target::CCLocationKind::FPR32:
            return phys_fpr(location.register_name);
        case target::CCLocationKind::Stack:
            fail_calling_convention(context, "stack location has no physical register");
        }
        fail_calling_convention(context, "unknown location kind");
    }

    std::int64_t stack_offset(const target::CCLocation &location,
                              const std::string &context) const {
        if (location.stack_offset >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            fail_calling_convention(context, "stack argument offset exceeds MIR range");
        }
        return static_cast<std::int64_t>(location.stack_offset);
    }

    std::int64_t value_offset(const target::CCLocation &location,
                              const std::string &context) const {
        if (location.value_offset >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            fail_calling_convention(context, "aggregate value offset exceeds MIR range");
        }
        return static_cast<std::int64_t>(location.value_offset);
    }

    int create_abi_bridge_slot(const oir::Type *type, std::uint64_t abi_size,
                               const std::string &name) {
        if (!is_fixed_vector_abi_value(type) || abi_size == 0 ||
            abi_size > std::numeric_limits<std::uint64_t>::max() - 15U) {
            fail_calling_convention(name, "invalid fixed-vector ABI bridge size");
        }
        auto info = type_info(type);
        info.value_type = mir::ValueType::Aggregate;
        info.ir = "abi.bridge<" + type->print() + ">";
        info.size = std::max<std::uint64_t>(16U, (abi_size + 15U) & ~std::uint64_t{15U});
        info.align = 16;
        info.vector_type.reset();
        return current_function_->add_stack_slot(name, std::move(info), mir::StackSlotKind::Value);
    }

    mir::Register bridge_address(int slot) {
        auto address = create_vreg(mir::ValueType::Ptr);
        emit(mir::Opcode::LoadStackAddr,
             {mir::MachineOperand::reg_def(address), mir::MachineOperand::slot(slot)});
        return address;
    }

    const oir::VectorType &portable_fixed_vector_type(const oir::Type *type,
                                                      const std::string &context) const {
        const auto *vector = dynamic_cast<const oir::VectorType *>(type);
        if (!portable_fixed_abi_enabled() || vector == nullptr ||
            !vector->element_count().is_fixed()) {
            fail_calling_convention(
                context, "portable fixed-ABI boundary requires a fixed vector or mask on an "
                         "rv64 target without V/Zve");
        }
        return *vector;
    }

    std::uint64_t portable_aggregate_size(const oir::VectorType &type,
                                          const std::string &context) const {
        const auto size = calling_convention_.data_layout().fixed_alloc_size(&type);
        if (!size.has_value() || *size == 0) {
            fail_calling_convention(context,
                                    "portable fixed-ABI aggregate has no fixed nonzero size");
        }
        return *size;
    }

    std::int64_t portable_memory_offset(std::uint64_t offset, const std::string &context) const {
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            fail_calling_convention(context, "portable fixed-ABI memory offset exceeds MIR range");
        }
        return static_cast<std::int64_t>(offset);
    }

    std::int64_t portable_lane_byte_offset(const oir::VectorType &type, std::uint64_t lane,
                                           const std::string &context) const {
        if (lane >= type.element_count().min_lanes) {
            fail_calling_convention(context, "portable fixed-ABI lane index is out of range");
        }
        if (type.is_mask()) {
            return portable_memory_offset(lane / 8U, context);
        }
        const auto stride = calling_convention_.data_layout().fixed_alloc_size(type.element_type());
        if (!stride.has_value() || *stride == 0 ||
            lane > std::numeric_limits<std::uint64_t>::max() / *stride) {
            fail_calling_convention(context, "portable fixed-ABI lane stride is not representable");
        }
        return portable_memory_offset(lane * *stride, context);
    }

    mir::Register portable_aggregate_address(const oir::Value *value,
                                             const std::string &context) const {
        const auto found = portable_aggregate_addresses_.find(value);
        if (found == portable_aggregate_addresses_.end()) {
            fail_calling_convention(
                context, "portable fixed-ABI aggregate has no staged boundary storage; "
                         "the scalarizer must pack every outbound value and preserve every "
                         "incoming boundary source");
        }
        return found->second;
    }

    void bind_portable_aggregate_address(const oir::Value *value, mir::Register address,
                                         const std::string &context) {
        if (value == nullptr || !is_fixed_vector_abi_value(value->type()) ||
            address.reg_class != mir::RegisterClass::GPR) {
            fail_calling_convention(context,
                                    "malformed portable fixed-ABI aggregate address binding");
        }
        portable_aggregate_addresses_.insert_or_assign(value, std::move(address));
    }

    void emit_portable_aggregate_copy(const oir::VectorType &type, mir::Register destination,
                                      mir::Register source, const std::string &context) {
        const auto size = portable_aggregate_size(type, context);
        if (type.is_mask()) {
            for (std::uint64_t offset = 0; offset < size; ++offset) {
                auto byte = create_vreg(mir::ValueType::I1);
                const auto displacement = portable_memory_offset(offset, context);
                emit(mir::Opcode::LoadMemOffset,
                     {mir::MachineOperand::reg_def(byte), mir::MachineOperand::reg_use(source),
                      mir::MachineOperand::imm(displacement),
                      mir::MachineOperand::type(mir::ValueType::I1)});
                emit(mir::Opcode::StoreMemOffset,
                     {mir::MachineOperand::reg_use(destination), mir::MachineOperand::reg_use(byte),
                      mir::MachineOperand::imm(displacement),
                      mir::MachineOperand::type(mir::ValueType::I1)});
            }
            return;
        }

        const auto stride = calling_convention_.data_layout().fixed_alloc_size(type.element_type());
        if (!stride.has_value() || *stride != 4U || size % *stride != 0U) {
            fail_calling_convention(context,
                                    "portable numeric vector has an unsupported lane layout");
        }
        for (std::uint64_t offset = 0; offset < size; offset += *stride) {
            auto word = create_vreg(mir::ValueType::I32);
            const auto displacement = portable_memory_offset(offset, context);
            emit(mir::Opcode::LoadMemOffset,
                 {mir::MachineOperand::reg_def(word), mir::MachineOperand::reg_use(source),
                  mir::MachineOperand::imm(displacement),
                  mir::MachineOperand::type(mir::ValueType::I32)});
            emit(mir::Opcode::StoreMemOffset,
                 {mir::MachineOperand::reg_use(destination), mir::MachineOperand::reg_use(word),
                  mir::MachineOperand::imm(displacement),
                  mir::MachineOperand::type(mir::ValueType::I32)});
        }
    }

    void emit_portable_mask_zero(const oir::VectorType &type, mir::Register address,
                                 const std::string &context) {
        if (!type.is_mask()) {
            fail_calling_convention(context, "portable mask zeroing received a numeric vector");
        }
        auto zero = create_vreg(mir::ValueType::I1);
        emit(mir::Opcode::LoadImm,
             {mir::MachineOperand::reg_def(zero), mir::MachineOperand::imm(0)});
        const auto size = portable_aggregate_size(type, context);
        for (std::uint64_t offset = 0; offset < size; ++offset) {
            emit(mir::Opcode::StoreMemOffset,
                 {mir::MachineOperand::reg_use(address), mir::MachineOperand::reg_use(zero),
                  mir::MachineOperand::imm(portable_memory_offset(offset, context)),
                  mir::MachineOperand::type(mir::ValueType::I1)});
        }
    }

    void emit_portable_lane_load(const oir::VectorType &type, std::uint64_t lane,
                                 mir::Register address, mir::Register destination,
                                 const std::string &context) {
        const auto offset = portable_lane_byte_offset(type, lane, context);
        if (!type.is_mask()) {
            const auto lane_type =
                type.is_float_vector() ? mir::ValueType::F32 : mir::ValueType::I32;
            emit(mir::Opcode::LoadMemOffset,
                 {mir::MachineOperand::reg_def(destination), mir::MachineOperand::reg_use(address),
                  mir::MachineOperand::imm(offset), mir::MachineOperand::type(lane_type)});
            return;
        }

        auto byte = create_vreg(mir::ValueType::I1);
        emit(mir::Opcode::LoadMemOffset,
             {mir::MachineOperand::reg_def(byte), mir::MachineOperand::reg_use(address),
              mir::MachineOperand::imm(offset), mir::MachineOperand::type(mir::ValueType::I1)});
        const auto bit = static_cast<unsigned>(lane % 8U);
        auto shifted = byte;
        if (bit != 0U) {
            shifted = create_vreg(mir::ValueType::I1);
            emit(mir::Opcode::Srli,
                 {mir::MachineOperand::reg_def(shifted), mir::MachineOperand::reg_use(byte),
                  mir::MachineOperand::imm(bit)});
        }
        emit(mir::Opcode::AndI,
             {mir::MachineOperand::reg_def(destination), mir::MachineOperand::reg_use(shifted),
              mir::MachineOperand::imm(1)});
    }

    void emit_portable_lane_store(const oir::VectorType &type, std::uint64_t lane,
                                  mir::Register value, mir::Register address,
                                  const std::string &context) {
        const auto offset = portable_lane_byte_offset(type, lane, context);
        if (!type.is_mask()) {
            const auto lane_type =
                type.is_float_vector() ? mir::ValueType::F32 : mir::ValueType::I32;
            emit(mir::Opcode::StoreMemOffset,
                 {mir::MachineOperand::reg_use(address), mir::MachineOperand::reg_use(value),
                  mir::MachineOperand::imm(offset), mir::MachineOperand::type(lane_type)});
            return;
        }

        auto byte = create_vreg(mir::ValueType::I1);
        emit(mir::Opcode::LoadMemOffset,
             {mir::MachineOperand::reg_def(byte), mir::MachineOperand::reg_use(address),
              mir::MachineOperand::imm(offset), mir::MachineOperand::type(mir::ValueType::I1)});
        const auto bit = static_cast<unsigned>(lane % 8U);
        const auto final_byte = lane / 8U + 1U == portable_aggregate_size(type, context);
        const auto valid_bits = static_cast<unsigned>(type.element_count().min_lanes % 8U);
        const auto valid_mask = final_byte && valid_bits != 0U ? (1U << valid_bits) - 1U : 0xffU;
        const auto preserve_mask = valid_mask & ~(1U << bit);
        auto preserved = create_vreg(mir::ValueType::I1);
        emit(mir::Opcode::AndI,
             {mir::MachineOperand::reg_def(preserved), mir::MachineOperand::reg_use(byte),
              mir::MachineOperand::imm(static_cast<std::int64_t>(preserve_mask))});
        auto lane_bit = create_vreg(mir::ValueType::I1);
        emit(mir::Opcode::AndI, {mir::MachineOperand::reg_def(lane_bit),
                                 mir::MachineOperand::reg_use(value), mir::MachineOperand::imm(1)});
        auto positioned = lane_bit;
        if (bit != 0U) {
            positioned = create_vreg(mir::ValueType::I1);
            emit(mir::Opcode::SllI,
                 {mir::MachineOperand::reg_def(positioned), mir::MachineOperand::reg_use(lane_bit),
                  mir::MachineOperand::imm(bit)});
        }
        auto combined = create_vreg(mir::ValueType::I1);
        // The destination bit was cleared above, so XOR is exactly OR while
        // avoiding a target opcode that the scalar MIR deliberately omits.
        emit(mir::Opcode::Xor,
             {mir::MachineOperand::reg_def(combined), mir::MachineOperand::reg_use(preserved),
              mir::MachineOperand::reg_use(positioned)});
        emit(mir::Opcode::StoreMemOffset,
             {mir::MachineOperand::reg_use(address), mir::MachineOperand::reg_use(combined),
              mir::MachineOperand::imm(offset), mir::MachineOperand::type(mir::ValueType::I1)});
    }

    mir::Register portable_outbound_aggregate_address(const oir::Value *value,
                                                      const oir::VectorType &type,
                                                      const std::string &context) {
        auto address = portable_aggregate_address(value, context);
        if (!type.is_mask() || type.element_count().min_lanes % 8U == 0U) {
            return address;
        }
        const auto size = portable_aggregate_size(type, context);
        const auto slot = create_abi_bridge_slot(
            &type, size, "abi.portable.mask.out." + std::to_string(temp_index_++));
        auto canonical = bridge_address(slot);
        emit_portable_aggregate_copy(type, canonical, address, context);
        clear_mask_padding_bits(type, canonical, size);
        return canonical;
    }

    void emit_raw_piece_store(mir::Register address, std::int64_t offset, mir::Register value) {
        emit(mir::Opcode::StoreMemOffset,
             {mir::MachineOperand::reg_use(std::move(address)),
              mir::MachineOperand::reg_use(std::move(value)), mir::MachineOperand::imm(offset),
              mir::MachineOperand::type(mir::ValueType::Ptr)});
    }

    mir::Register emit_raw_piece_load(mir::Register address, std::int64_t offset) {
        auto value = create_vreg(mir::ValueType::Ptr);
        emit(mir::Opcode::LoadMemOffset,
             {mir::MachineOperand::reg_def(value), mir::MachineOperand::reg_use(std::move(address)),
              mir::MachineOperand::imm(offset), mir::MachineOperand::type(mir::ValueType::Ptr)});
        return value;
    }

    void clear_mask_padding_bits(const oir::VectorType &type, mir::Register address,
                                 std::uint64_t abi_size) {
        if (!type.is_mask() || type.element_count().min_lanes % 8U == 0U) {
            return;
        }
        if (abi_size == 0 ||
            abi_size - 1U > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            fail_calling_convention("mask ABI bridge", "invalid packed mask byte offset");
        }
        const auto last_byte = static_cast<std::int64_t>(abi_size - 1U);
        auto byte = create_vreg(mir::ValueType::I1);
        emit(mir::Opcode::LoadMemOffset,
             {mir::MachineOperand::reg_def(byte), mir::MachineOperand::reg_use(address),
              mir::MachineOperand::imm(last_byte), mir::MachineOperand::type(mir::ValueType::I1)});
        auto clean = create_vreg(mir::ValueType::I1);
        const auto valid_bits = static_cast<unsigned>(type.element_count().min_lanes % 8U);
        const auto valid_mask = static_cast<std::int64_t>((1U << valid_bits) - 1U);
        emit(mir::Opcode::AndI,
             {mir::MachineOperand::reg_def(clean), mir::MachineOperand::reg_use(byte),
              mir::MachineOperand::imm(valid_mask)});
        emit(mir::Opcode::StoreMemOffset,
             {mir::MachineOperand::reg_use(address), mir::MachineOperand::reg_use(clean),
              mir::MachineOperand::imm(last_byte), mir::MachineOperand::type(mir::ValueType::I1)});
    }

    std::int64_t bundle_piece_byte_offset(const target::RVVFixedVectorPiece &piece) const {
        std::uint64_t byte_offset = piece.storage_offset;
        if (piece.storage_offset_unit == target::RVVFixedStorageOffsetUnit::Bits) {
            if (piece.storage_offset % 8U != 0U) {
                fail_vector_legalization(
                    "packed mask chunk begins at a non-byte-aligned bit offset");
            }
            byte_offset /= 8U;
        }
        if (byte_offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            fail_vector_legalization("fixed chunk byte offset exceeds MIR range");
        }
        return static_cast<std::int64_t>(byte_offset);
    }

    mir::Register address_with_offset(mir::Register base, std::int64_t offset) {
        if (offset == 0) {
            return base;
        }
        auto address = create_vreg(mir::ValueType::Ptr);
        if (fits_simm12(offset)) {
            emit(mir::Opcode::AddI,
                 {mir::MachineOperand::reg_def(address), mir::MachineOperand::reg_use(base),
                  mir::MachineOperand::imm(offset)});
            return address;
        }
        auto displacement = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::LoadImm,
             {mir::MachineOperand::reg_def(displacement), mir::MachineOperand::imm(offset)});
        emit(mir::Opcode::Add,
             {mir::MachineOperand::reg_def(address), mir::MachineOperand::reg_use(base),
              mir::MachineOperand::reg_use(displacement)});
        return address;
    }

    void emit_fixed_bundle_store(const FixedVectorBundle &bundle, mir::Register base_address) {
        if (bundle.logical_type == nullptr || bundle.pieces.size() != bundle.plan.pieces.size()) {
            fail_vector_legalization("malformed fixed-vector store bundle");
        }
        for (std::size_t index = 0; index < bundle.pieces.size(); ++index) {
            const auto type = *bundle.pieces[index].vector_type;
            const auto config = data_config_for(type);
            ensure_vector_configuration(config);
            mir::MachineVectorInfo info(config);
            info.operation = mir::RVVOperation::Store;
            info.avl = mir::MachineVectorAVL::current_vl();
            info.tail_policy = mir::VectorTailPolicy::Agnostic;
            info.mask_policy = mir::VectorMaskPolicy::Agnostic;
            auto address = address_with_offset(base_address,
                                               bundle_piece_byte_offset(bundle.plan.pieces[index]));
            emit_vector_instruction(type.is_mask() ? mir::Opcode::RVVMaskStore
                                                   : mir::Opcode::RVVStoreUnit,
                                    {mir::MachineOperand::reg_use(bundle.pieces[index]),
                                     mir::MachineOperand::reg_use(address)},
                                    std::move(info));
        }
    }

    void emit_fixed_bundle_load(FixedVectorBundle &bundle, mir::Register base_address) {
        if (bundle.logical_type == nullptr || bundle.pieces.size() != bundle.plan.pieces.size()) {
            fail_vector_legalization("malformed fixed-vector load bundle");
        }
        for (std::size_t index = 0; index < bundle.pieces.size(); ++index) {
            const auto type = *bundle.pieces[index].vector_type;
            const auto config = data_config_for(type);
            ensure_vector_configuration(config);
            mir::MachineVectorInfo info(config);
            info.operation = mir::RVVOperation::Load;
            info.avl = mir::MachineVectorAVL::current_vl();
            info.tail_policy = mir::VectorTailPolicy::Agnostic;
            info.mask_policy = mir::VectorMaskPolicy::Agnostic;
            auto address = address_with_offset(base_address,
                                               bundle_piece_byte_offset(bundle.plan.pieces[index]));
            emit_vector_instruction(type.is_mask() ? mir::Opcode::RVVMaskLoad
                                                   : mir::Opcode::RVVLoadUnitTA,
                                    {mir::MachineOperand::reg_def(bundle.pieces[index]),
                                     mir::MachineOperand::reg_use(address)},
                                    std::move(info));
        }
    }

    void emit_abi_vector_store(const oir::Type *type, mir::Register value, mir::Register address,
                               std::uint64_t abi_size) {
        const auto *vector = dynamic_cast<const oir::VectorType *>(type);
        if (vector == nullptr || !vector->element_count().is_fixed() ||
            !value.vector_type.has_value()) {
            fail_calling_convention("fixed-vector ABI store", "malformed vector value");
        }
        const auto machine_type = *value.vector_type;
        const auto config = data_config_for(machine_type);
        ensure_vector_configuration(config);
        mir::MachineVectorInfo info(config);
        info.operation = mir::RVVOperation::Store;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(
            machine_type.is_mask() ? mir::Opcode::RVVMaskStore : mir::Opcode::RVVStoreUnit,
            {mir::MachineOperand::reg_use(std::move(value)), mir::MachineOperand::reg_use(address)},
            std::move(info));
        clear_mask_padding_bits(*vector, std::move(address), abi_size);
    }

    void emit_abi_vector_load(const oir::Type *type, mir::Register destination,
                              mir::Register address) {
        const auto *vector = dynamic_cast<const oir::VectorType *>(type);
        if (vector == nullptr || !vector->element_count().is_fixed() ||
            !destination.vector_type.has_value()) {
            fail_calling_convention("fixed-vector ABI load", "malformed vector value");
        }
        const auto machine_type = *destination.vector_type;
        const auto config = data_config_for(machine_type);
        ensure_vector_configuration(config);
        mir::MachineVectorInfo info(config);
        info.operation = mir::RVVOperation::Load;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(machine_type.is_mask() ? mir::Opcode::RVVMaskLoad
                                                       : mir::Opcode::RVVLoadUnitTA,
                                {mir::MachineOperand::reg_def(std::move(destination)),
                                 mir::MachineOperand::reg_use(std::move(address))},
                                std::move(info));
    }

    void emit_abi_vector_store(const oir::Type *type, const FixedVectorBundle &value,
                               mir::Register address, std::uint64_t abi_size) {
        const auto *vector = dynamic_cast<const oir::VectorType *>(type);
        if (vector == nullptr || value.logical_type != vector ||
            value.plan.storage_size_bytes != abi_size) {
            fail_calling_convention("fixed-vector ABI bundle store",
                                    "malformed oversized vector value");
        }
        emit_fixed_bundle_store(value, address);
        // RVVMaskStore expansion clears each partial piece byte.  This
        // explicit logical-object cleanup is retained as an ABI invariant and
        // protects the final packed byte if a future planner changes pieces.
        clear_mask_padding_bits(*vector, address, abi_size);
    }

    void emit_abi_vector_load(const oir::Type *type, FixedVectorBundle &destination,
                              mir::Register address) {
        const auto *vector = dynamic_cast<const oir::VectorType *>(type);
        if (vector == nullptr || destination.logical_type != vector) {
            fail_calling_convention("fixed-vector ABI bundle load",
                                    "malformed oversized vector value");
        }
        emit_fixed_bundle_load(destination, address);
    }

    std::vector<mir::Register>
    future_parameter_registers(std::size_t parameter_index,
                               mir::RegisterClass destination_class) const {
        std::vector<mir::Register> future;
        for (std::size_t index = parameter_index + 1;
             index < current_cc_assignment_.parameters.size(); ++index) {
            const auto &location = single_location(current_cc_assignment_.parameters[index]);
            if (location.kind == target::CCLocationKind::Stack) {
                continue;
            }
            auto reg = physical_register(location, "function entry");
            if (reg.reg_class == destination_class) {
                future.push_back(std::move(reg));
            }
        }
        return future;
    }

    bool adjacent_segment_field_pointer(const oir::Value *field0,
                                        const oir::Value *field1) const {
        const auto *first_type =
            field0 == nullptr ? nullptr : dynamic_cast<const oir::PointerType *>(field0->type());
        const auto *second_type =
            field1 == nullptr ? nullptr : dynamic_cast<const oir::PointerType *>(field1->type());
        const auto *gep = dynamic_cast<const oir::GetElementPtrInst *>(field1);
        if (first_type == nullptr || second_type == nullptr ||
            first_type->element_type() != second_type->element_type() || gep == nullptr ||
            gep->base_ptr() != field0) {
            return false;
        }
        const auto indices = gep->indices();
        const auto *one = indices.size() == 1
                              ? dynamic_cast<const oir::ConstantInt *>(indices.front())
                              : nullptr;
        return one != nullptr && one->value() == 1;
    }

    bool potential_segment2_pair(const oir::VPGatherInst &field0,
                                 const oir::VPGatherInst &field1) const {
        const auto *type0 = dynamic_cast<const oir::VectorType *>(field0.type());
        const auto *type1 = dynamic_cast<const oir::VectorType *>(field1.type());
        return type0 != nullptr && type1 == type0 && !type0->is_mask() &&
               !is_oversized_fixed_vector(type0) && field0.indices() == field1.indices() &&
               field0.active_mask() == field1.active_mask() && field0.evl() == field1.evl() &&
               field0.alignment() >= 4 && field1.alignment() >= 4 &&
               field0.tail_policy() == oir::TailPolicy::Agnostic &&
               field1.tail_policy() == oir::TailPolicy::Agnostic &&
               field0.mask_policy() == oir::MaskPolicy::Agnostic &&
               field1.mask_policy() == oir::MaskPolicy::Agnostic &&
               vp_passthrough_is_discardable(field0) &&
               vp_passthrough_is_discardable(field1) &&
               adjacent_segment_field_pointer(field0.base_ptr(), field1.base_ptr());
    }

    bool potential_segment2_pair(const oir::VPScatterInst &field0,
                                 const oir::VPScatterInst &field1) const {
        const auto *type0 = dynamic_cast<const oir::VectorType *>(field0.value()->type());
        const auto *type1 = dynamic_cast<const oir::VectorType *>(field1.value()->type());
        return type0 != nullptr && type1 == type0 && !type0->is_mask() &&
               !is_oversized_fixed_vector(type0) && field0.indices() == field1.indices() &&
               field0.active_mask() == field1.active_mask() && field0.evl() == field1.evl() &&
               field0.alignment() >= 4 && field1.alignment() >= 4 &&
               field0.tail_policy() == oir::TailPolicy::Agnostic &&
               field1.tail_policy() == oir::TailPolicy::Agnostic &&
               field0.mask_policy() == oir::MaskPolicy::Agnostic &&
               field1.mask_policy() == oir::MaskPolicy::Agnostic &&
               adjacent_segment_field_pointer(field0.base_ptr(), field1.base_ptr());
    }

    void discover_segment2_candidates(const oir::Function &function) {
        for (const auto &block : function.blocks()) {
            std::vector<const oir::Instruction *> instructions;
            instructions.reserve(block->instructions().size());
            for (const auto &instruction : block->instructions()) {
                instructions.push_back(instruction.get());
            }
            for (std::size_t index = 0; index + 1 < instructions.size(); ++index) {
                const auto *first = instructions[index];
                const auto *second = instructions[index + 1];
                const auto *load0 = dynamic_cast<const oir::VPGatherInst *>(first);
                const auto *load1 = dynamic_cast<const oir::VPGatherInst *>(second);
                if (load0 != nullptr && load1 != nullptr &&
                    potential_segment2_pair(*load0, *load1)) {
                    segment2_candidates_[first] = second;
                    ++index;
                    continue;
                }
                const auto *store0 = dynamic_cast<const oir::VPScatterInst *>(first);
                const auto *store1 = dynamic_cast<const oir::VPScatterInst *>(second);
                if (store0 != nullptr && store1 != nullptr &&
                    potential_segment2_pair(*store0, *store1)) {
                    segment2_candidates_[first] = second;
                    ++index;
                }
            }
        }
    }

    void lower_function(const oir::Function &function, mir::MachineFunction &out) {
        current_function_ = &out;
        validate_portable_boundary_function(function);
        if (psabi_vector_enabled()) {
            current_psabi_cc_assignment_ = psabi_vector_calling_convention_->assign(
                psabi_function_type(*function.function_type()));
            validate_psabi_signature_assignment(current_psabi_cc_assignment_,
                                                "function @" + function.name());
        } else {
            current_cc_assignment_ = calling_convention_.assign(*function.function_type());
            validate_signature_assignment(current_cc_assignment_, "function @" + function.name());
        }
        value_regs_.clear();
        value_bundles_.clear();
        portable_aggregate_addresses_.clear();
        alloca_slots_.clear();
        segment2_candidates_.clear();
        consumed_segment2_fields_.clear();
        blocks_.clear();
        edge_blocks_.clear();
        pending_edge_blocks_.clear();
        value_ranges_.clear();
        block_entry_ranges_.clear();
        current_ranges_.clear();
        current_vector_config_.reset();
        dominating_scalable_config_.reset();
        current_execution_mask_source_.reset();
        block_mask_constant_regs_.clear();
        current_sret_pointer_.reset();
        temp_index_ = 0;

        analyze_value_ranges(function);
        discover_segment2_candidates(function);

        for (const auto &block : function.blocks()) {
            blocks_[block.get()] = out.create_block(block->name());
        }

        for (const auto &arg : function.args()) {
            if (portable_fixed_abi_enabled() && is_fixed_vector_abi_value(arg->type())) {
                // The public aggregate stays in byte-addressable ABI storage.
                // Boundary extracts materialize only its scalar lanes.
                continue;
            }
            if (is_oversized_fixed_vector(arg->type())) {
                auto *vector = dynamic_cast<const oir::VectorType *>(arg->type());
                value_bundles_.emplace(arg.get(), allocate_fixed_bundle(*vector));
                continue;
            }
            auto info = type_info(arg->type());
            value_regs_[arg.get()] = info.vector_type.has_value()
                                         ? create_vector_vreg(*info.vector_type)
                                         : create_vreg(info.value_type);
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
            current_vector_config_.reset();
            dominating_scalable_config_.reset();
            current_execution_mask_source_.reset();
            block_mask_constant_regs_.clear();
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
        current_vector_config_.reset();
        dominating_scalable_config_.reset();
        current_execution_mask_source_.reset();
        block_mask_constant_regs_.clear();
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
        if (portable_fixed_abi_enabled() && is_fixed_vector_abi_value(inst.type())) {
            // Calls, aggregate loads, and abi.fixed.pack bind an address when
            // they execute.  No vector vreg is legal on rv64gc.
            return;
        }
        if (is_oversized_fixed_vector(inst.type())) {
            auto *vector = dynamic_cast<const oir::VectorType *>(inst.type());
            value_bundles_.emplace(&inst, allocate_fixed_bundle(*vector));
            return;
        }
        auto info = type_info(inst.type());
        if (info.vector_type.has_value()) {
            value_regs_[&inst] = create_vector_vreg(*info.vector_type);
        } else if (info.value_type != mir::ValueType::Aggregate) {
            value_regs_[&inst] = create_vreg(info.value_type);
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

    std::vector<mir::Register>
    future_entry_registers(const std::vector<const target::CCLocation *> &physical_locations,
                           std::size_t next, mir::RegisterClass reg_class) const {
        std::vector<mir::Register> future;
        for (std::size_t index = next; index < physical_locations.size(); ++index) {
            const auto &location = *physical_locations[index];
            if (location.kind == target::CCLocationKind::Stack) {
                continue;
            }
            auto reg = physical_register(location, "function entry staging");
            if (reg.reg_class == reg_class) {
                future.push_back(std::move(reg));
            }
        }
        return future;
    }

    mir::Register
    stage_entry_location(const target::CCLocation &location, mir::ValueType type,
                         const std::vector<const target::CCLocation *> &physical_locations,
                         std::size_t next_physical, const std::string &context) {
        auto staged = create_vreg(type);
        if (location.kind == target::CCLocationKind::Stack) {
            emit(mir::Opcode::LoadIncomingArg,
                 {mir::MachineOperand::reg_def(staged),
                  mir::MachineOperand::imm(stack_offset(location, context)),
                  mir::MachineOperand::type(type)});
            return staged;
        }
        auto source = physical_register(location, context);
        emit_entry_move(
            staged, std::move(source),
            future_entry_registers(physical_locations, next_physical, staged.reg_class));
        return staged;
    }

    void emit_entry_vector_move(mir::Register destination, mir::Register source,
                                const std::vector<mir::Register> &future_vector_arguments) {
        if (!destination.is_vector() || !source.is_vector() ||
            destination.vector_type != source.vector_type) {
            fail_calling_convention("function entry",
                                    "direct vector ABI copy has incompatible register views");
        }
        const auto config = data_config_for(*destination.vector_type);
        ensure_vector_configuration(config);
        mir::MachineVectorInfo info(config);
        info.operation = mir::RVVOperation::Copy;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        std::vector<mir::MachineOperand> operands;
        operands.push_back(mir::MachineOperand::reg_def(destination));
        operands.push_back(mir::MachineOperand::reg_use(std::move(source)));
        for (const auto &future : future_vector_arguments) {
            operands.push_back(mir::MachineOperand::implicit_reg_use(future));
        }
        emit_vector_instruction(destination.vector_type->is_mask() ? mir::Opcode::RVVMaskCopy
                                                                   : mir::Opcode::RVVVectorCopy,
                                std::move(operands), std::move(info));
    }

    void emit_psabi_parameter_copies(const oir::Function &function) {
        current_block_ = blocks_.at(function.entry_block());
        const auto &signature = current_psabi_cc_assignment_;
        if (signature.parameters.size() != function.args().size()) {
            fail_calling_convention("function @" + function.name(),
                                    "parameter assignment count does not match the function");
        }

        std::vector<const target::CCLocation *> scalar_locations;
        if (signature.has_sret) {
            scalar_locations.push_back(&single_location(signature.return_value.value));
        }
        std::vector<mir::Register> direct_vector_sources;
        for (const auto &assignment : signature.parameters) {
            if (psabi_direct_vector(assignment)) {
                direct_vector_sources.push_back(psabi_vector_register(
                    assignment, "function @" + function.name() + " parameter"));
                continue;
            }
            for (const auto &location : assignment.value.locations) {
                if (location.kind != target::CCLocationKind::Stack) {
                    scalar_locations.push_back(&location);
                }
            }
        }

        std::size_t next_scalar = 0;
        if (signature.has_sret) {
            const auto &location = single_location(signature.return_value.value);
            current_sret_pointer_ =
                stage_entry_location(location, mir::ValueType::Ptr, scalar_locations, ++next_scalar,
                                     "function @" + function.name() + " hidden sret pointer");
        }

        std::vector<std::optional<mir::Register>> staged(function.args().size());
        std::size_t next_vector = 0;
        for (std::size_t index = 0; index < function.args().size(); ++index) {
            const auto &arg = function.args()[index];
            const auto &assignment = signature.parameters[index];
            const auto context =
                "function @" + function.name() + " parameter " + std::to_string(index);
            if (psabi_direct_vector(assignment)) {
                std::vector<mir::Register> future;
                future.reserve(direct_vector_sources.size() - next_vector - 1U);
                for (std::size_t future_index = next_vector + 1U;
                     future_index < direct_vector_sources.size(); ++future_index) {
                    future.push_back(direct_vector_sources[future_index]);
                }
                emit_entry_vector_move(value_regs_.at(arg.get()),
                                       direct_vector_sources[next_vector], future);
                ++next_vector;
                continue;
            }

            const auto &location = single_location(assignment.value);
            const auto is_physical = location.kind != target::CCLocationKind::Stack;
            const auto future_index = next_scalar + (is_physical ? 1U : 0U);
            const auto stage_type =
                assignment.value.indirect ? mir::ValueType::Ptr : type_info(arg->type()).value_type;
            staged[index] =
                stage_entry_location(location, stage_type, scalar_locations, future_index, context);
            next_scalar = future_index;
        }

        for (std::size_t index = 0; index < function.args().size(); ++index) {
            const auto &arg = function.args()[index];
            const auto &assignment = signature.parameters[index];
            if (psabi_direct_vector(assignment)) {
                continue;
            }
            if (!staged[index].has_value()) {
                fail_calling_convention("function @" + function.name(),
                                        "parameter did not receive an entry staging value");
            }
            if (!is_fixed_vector_abi_value(arg->type())) {
                emit_move(value_regs_.at(arg.get()), *staged[index]);
                continue;
            }
            if (!assignment.value.indirect) {
                fail_calling_convention("function @" + function.name(),
                                        "non-direct vector parameter is missing indirect fallback");
            }
            if (is_oversized_fixed_vector(arg->type())) {
                emit_abi_vector_load(arg->type(), bundle_result(*arg), *staged[index]);
            } else {
                emit_abi_vector_load(arg->type(), value_regs_.at(arg.get()), *staged[index]);
            }
        }
    }

    void emit_parameter_copies(const oir::Function &function) {
        if (psabi_vector_enabled()) {
            emit_psabi_parameter_copies(function);
            return;
        }
        current_block_ = blocks_.at(function.entry_block());
        if (current_cc_assignment_.parameters.size() != function.args().size()) {
            fail_calling_convention("function @" + function.name(),
                                    "parameter assignment count does not match the function");
        }

        const bool has_aggregate_parameter = std::any_of(
            current_cc_assignment_.parameters.begin(), current_cc_assignment_.parameters.end(),
            [&](const target::CCValueAssignment &assignment) {
                return is_fixed_vector_abi_value(assignment.type);
            });
        if (!current_cc_assignment_.has_sret && !has_aggregate_parameter) {
            // Preserve the scalar-only lowering byte for byte: in particular,
            // do not create staging vregs that perturb established RA choices.
            for (std::size_t index = 0; index < function.args().size(); ++index) {
                const auto &arg = function.args()[index];
                const auto &assignment = current_cc_assignment_.parameters[index];
                const auto &location = single_location(assignment);
                const auto type = type_info(arg->type()).value_type;
                const auto destination = value_regs_.at(arg.get());
                if (location.kind == target::CCLocationKind::Stack) {
                    emit(mir::Opcode::LoadIncomingArg,
                         {mir::MachineOperand::reg_def(destination),
                          mir::MachineOperand::imm(
                              stack_offset(location, "function @" + function.name() +
                                                         " parameter " + std::to_string(index))),
                          mir::MachineOperand::type(type)});
                    continue;
                }
                auto source =
                    physical_register(location, "function @" + function.name() + " parameter " +
                                                    std::to_string(index));
                emit_entry_move(destination, std::move(source),
                                future_parameter_registers(index, destination.reg_class));
            }
            return;
        }

        std::vector<const target::CCLocation *> physical_locations;
        if (current_cc_assignment_.has_sret) {
            physical_locations.push_back(&single_location(current_cc_assignment_.return_value));
        }
        for (const auto &assignment : current_cc_assignment_.parameters) {
            for (const auto &location : assignment.locations) {
                if (location.kind != target::CCLocationKind::Stack) {
                    physical_locations.push_back(&location);
                }
            }
        }

        std::size_t next_physical = 0;
        if (current_cc_assignment_.has_sret) {
            const auto &location = single_location(current_cc_assignment_.return_value);
            current_sret_pointer_ = stage_entry_location(
                location, mir::ValueType::Ptr, physical_locations, ++next_physical,
                "function @" + function.name() + " hidden sret pointer");
        }

        std::vector<std::vector<mir::Register>> staged_parameters(function.args().size());
        for (std::size_t index = 0; index < function.args().size(); ++index) {
            const auto &arg = function.args()[index];
            const auto &assignment = current_cc_assignment_.parameters[index];
            const auto aggregate = is_fixed_vector_abi_value(arg->type());
            const auto stage_type =
                aggregate ? mir::ValueType::Ptr : type_info(arg->type()).value_type;
            for (const auto &location : assignment.locations) {
                const auto is_physical = location.kind != target::CCLocationKind::Stack;
                const auto future_index = next_physical + (is_physical ? 1U : 0U);
                staged_parameters[index].push_back(stage_entry_location(
                    location, stage_type, physical_locations, future_index,
                    "function @" + function.name() + " parameter " + std::to_string(index)));
                next_physical = future_index;
            }
        }

        for (std::size_t index = 0; index < function.args().size(); ++index) {
            const auto &arg = function.args()[index];
            const auto &assignment = current_cc_assignment_.parameters[index];
            if (!is_fixed_vector_abi_value(arg->type())) {
                const auto destination = value_regs_.at(arg.get());
                emit_move(destination, staged_parameters[index].front());
                continue;
            }
            if (portable_fixed_abi_enabled()) {
                const auto context =
                    "function @" + function.name() + " parameter " + std::to_string(index);
                if (assignment.indirect) {
                    bind_portable_aggregate_address(arg.get(), staged_parameters[index].front(),
                                                    context);
                    continue;
                }
                const auto slot = create_abi_bridge_slot(arg->type(), assignment.size,
                                                         "abi.entry." + function.name() + ".arg" +
                                                             std::to_string(index));
                const auto address = bridge_address(slot);
                for (std::size_t piece = 0; piece < assignment.locations.size(); ++piece) {
                    emit_raw_piece_store(address,
                                         value_offset(assignment.locations[piece], context),
                                         staged_parameters[index][piece]);
                }
                bind_portable_aggregate_address(arg.get(), address, context);
                continue;
            }
            if (is_oversized_fixed_vector(arg->type())) {
                auto &destination = bundle_result(*arg);
                if (assignment.indirect) {
                    emit_abi_vector_load(arg->type(), destination,
                                         staged_parameters[index].front());
                    continue;
                }
                const auto slot = create_abi_bridge_slot(arg->type(), assignment.size,
                                                         "abi.entry." + function.name() + ".arg" +
                                                             std::to_string(index));
                const auto address = bridge_address(slot);
                for (std::size_t piece = 0; piece < assignment.locations.size(); ++piece) {
                    emit_raw_piece_store(address,
                                         value_offset(assignment.locations[piece],
                                                      "function @" + function.name() +
                                                          " parameter " + std::to_string(index)),
                                         staged_parameters[index][piece]);
                }
                emit_abi_vector_load(arg->type(), destination, address);
                continue;
            }
            const auto destination = value_regs_.at(arg.get());
            if (assignment.indirect) {
                emit_abi_vector_load(arg->type(), destination, staged_parameters[index].front());
                continue;
            }
            const auto slot = create_abi_bridge_slot(arg->type(), assignment.size,
                                                     "abi.entry." + function.name() + ".arg" +
                                                         std::to_string(index));
            const auto address = bridge_address(slot);
            for (std::size_t piece = 0; piece < assignment.locations.size(); ++piece) {
                emit_raw_piece_store(address,
                                     value_offset(assignment.locations[piece],
                                                  "function @" + function.name() + " parameter " +
                                                      std::to_string(index)),
                                     staged_parameters[index][piece]);
            }
            emit_abi_vector_load(arg->type(), destination, address);
        }
    }

    mir::Register vector_state_register(const char *name) const {
        return mir::Register::physical(name, mir::RegisterClass::VSTATE);
    }

    mir::VectorTailPolicy lower_tail_policy(oir::TailPolicy policy) const {
        return policy == oir::TailPolicy::Agnostic ? mir::VectorTailPolicy::Agnostic
                                                   : mir::VectorTailPolicy::Undisturbed;
    }

    mir::VectorMaskPolicy lower_mask_policy(oir::MaskPolicy policy) const {
        return policy == oir::MaskPolicy::Agnostic ? mir::VectorMaskPolicy::Agnostic
                                                   : mir::VectorMaskPolicy::Undisturbed;
    }

    std::vector<std::uint8_t>
    encode_fixed_vector_constant(const oir::ConstantVector &constant) const {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(constant.elements().size() * sizeof(std::uint32_t));
        for (auto *element : constant.elements()) {
            std::uint32_t bits = 0;
            if (auto *integer = dynamic_cast<const oir::ConstantInt *>(element)) {
                bits = static_cast<std::uint32_t>(integer->value());
            } else if (auto *floating = dynamic_cast<const oir::ConstantFloat *>(element)) {
                const float value = floating->value();
                static_assert(sizeof(bits) == sizeof(value), "f32 constant must be 32-bit");
                std::memcpy(&bits, &value, sizeof(bits));
            } else if (dynamic_cast<const oir::ConstantZero *>(element) == nullptr) {
                fail_vector_legalization("fixed vector constant contains an unsupported lane: " +
                                         element->print());
            }
            for (unsigned byte = 0; byte < sizeof(bits); ++byte) {
                bytes.push_back(static_cast<std::uint8_t>((bits >> (byte * 8U)) & 0xffU));
            }
        }
        return bytes;
    }

    std::string next_constant_pool_symbol() {
        for (;;) {
            auto candidate = "__yoo_vec_const_" + std::to_string(constant_pool_index_++);
            const auto collision =
                std::any_of(module_->globals().begin(), module_->globals().end(),
                            [&](const mir::Global &global) { return global.name == candidate; });
            if (!collision) {
                return candidate;
            }
        }
    }

    mir::Register materialize_constant_mask(const oir::ConstantMask &constant) {
        const auto cache_key = constant.type()->print() + "=" + constant.print();
        if (auto found = block_mask_constant_regs_.find(cache_key);
            found != block_mask_constant_regs_.end()) {
            return found->second;
        }
        bool any_set = false;
        bool any_clear = false;
        for (std::uint64_t lane = 0; lane < constant.lane_count(); ++lane) {
            if (constant.lane(lane)) {
                any_set = true;
            } else {
                any_clear = true;
            }
        }
        const auto mask_type = legalize_vector_type(*constant.mask_type());
        auto config = data_config_for(mask_type);
        if (current_vector_config_.has_value() &&
            mir::MachineVectorType::mask_for(current_vector_config_->type) == mask_type) {
            // An i1 mask does not encode whether the configured data operation
            // is integer or floating-point.  Preserve a compatible dominating
            // VTYPE instead of spuriously changing an f32 operation to i32.
            config = current_vector_config_->type;
        }
        ensure_vector_configuration(config);
        auto destination = create_vector_vreg(mask_type);
        mir::MachineVectorInfo info(config);
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        if (any_set && any_clear) {
            mir::Global pool;
            pool.name = next_constant_pool_symbol();
            pool.type = type_info(constant.type());
            pool.is_const = true;
            pool.initializer_bytes = constant.packed_bits();
            if (pool.initializer_bytes.size() != pool.type.size) {
                fail_vector_legalization(
                    "fixed mask constant byte count disagrees with packed layout");
            }
            const auto symbol = pool.name;
            module_->add_global(std::move(pool));
            auto address = create_vreg(mir::ValueType::Ptr);
            emit(mir::Opcode::LoadGlobalAddr,
                 {mir::MachineOperand::reg_def(address), mir::MachineOperand::global(symbol)});
            info.operation = mir::RVVOperation::Load;
            emit_vector_instruction(
                mir::Opcode::RVVMaskLoad,
                {mir::MachineOperand::reg_def(destination), mir::MachineOperand::reg_use(address)},
                std::move(info));
            block_mask_constant_regs_.emplace(cache_key, destination);
            return destination;
        }

        info.operation = any_set ? mir::RVVOperation::MaskSet : mir::RVVOperation::MaskClear;
        emit_vector_instruction(any_set ? mir::Opcode::RVVMaskSet : mir::Opcode::RVVMaskClear,
                                {mir::MachineOperand::reg_def(destination)}, std::move(info));
        block_mask_constant_regs_.emplace(cache_key, destination);
        return destination;
    }

    mir::Register materialize_zero_vector(oir::Value &constant) {
        auto *source_type = dynamic_cast<oir::VectorType *>(constant.type());
        if (source_type == nullptr) {
            fail_vector_legalization("malformed zero vector constant");
        }
        const auto type = legalize_vector_type(*source_type);
        auto config = data_config_for(type);
        if (type.is_mask() && current_vector_config_.has_value() &&
            mir::MachineVectorType::mask_for(current_vector_config_->type) == type) {
            config = current_vector_config_->type;
        }
        ensure_vector_configuration(config);
        auto destination = create_vector_vreg(type);
        mir::MachineVectorInfo info(config);
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        if (type.is_mask()) {
            info.operation = mir::RVVOperation::MaskClear;
            emit_vector_instruction(mir::Opcode::RVVMaskClear,
                                    {mir::MachineOperand::reg_def(destination)}, std::move(info));
        } else {
            info.operation = mir::RVVOperation::Splat;
            if (type.element_type() == mir::ValueType::F32) {
                auto scalar_zero = create_vreg(mir::ValueType::F32);
                emit(mir::Opcode::LoadFloatImm, {mir::MachineOperand::reg_def(scalar_zero),
                                                 mir::MachineOperand::float_imm(0.0F)});
                emit_vector_instruction(mir::Opcode::RVVSplatVFTA,
                                        {mir::MachineOperand::reg_def(destination),
                                         mir::MachineOperand::reg_use(scalar_zero)},
                                        std::move(info));
            } else {
                emit_vector_instruction(
                    mir::Opcode::RVVSplatVITA,
                    {mir::MachineOperand::reg_def(destination), mir::MachineOperand::imm(0)},
                    std::move(info));
            }
        }
        return destination;
    }

    mir::Register materialize_constant_vector(const oir::ConstantVector &constant) {
        const auto type = legalize_vector_type(*constant.vector_type());
        ensure_vector_configuration(type);
        auto destination = create_vector_vreg(type);

        // Arbitrary fixed literals are loaded from a typed constant-pool object.
        // This is exact for every lane and avoids pretending an InsertElement
        // family has been expanded before that legalization is implemented.
        mir::Global pool;
        pool.name = next_constant_pool_symbol();
        pool.type = type_info(constant.type());
        pool.is_const = true;
        pool.initializer_bytes = encode_fixed_vector_constant(constant);
        if (pool.initializer_bytes.size() != pool.type.size) {
            fail_vector_legalization("fixed vector constant byte count disagrees with layout");
        }
        const auto symbol = pool.name;
        module_->add_global(std::move(pool));

        auto address = create_vreg(mir::ValueType::Ptr);
        emit(mir::Opcode::LoadGlobalAddr,
             {mir::MachineOperand::reg_def(address), mir::MachineOperand::global(symbol)});
        mir::MachineVectorInfo info(type);
        info.operation = mir::RVVOperation::Load;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(
            mir::Opcode::RVVLoadUnitTA,
            {mir::MachineOperand::reg_def(destination), mir::MachineOperand::reg_use(address)},
            std::move(info));
        return destination;
    }

    void emit_vector_instruction(mir::Opcode opcode,
                                 std::vector<mir::MachineOperand> explicit_operands,
                                 mir::MachineVectorInfo info, bool uses_frm = false) {
        explicit_operands.push_back(
            mir::MachineOperand::implicit_reg_use(vector_state_register("vl")));
        explicit_operands.push_back(
            mir::MachineOperand::implicit_reg_use(vector_state_register("vtype")));
        if (uses_frm) {
            explicit_operands.push_back(
                mir::MachineOperand::implicit_reg_use(vector_state_register("frm")));
        }
        current_block_->add_instr(
            mir::MachineInstr(opcode, std::move(explicit_operands), std::move(info)));
    }

    mir::Register prepare_execution_mask(mir::Register source,
                                         const mir::MachineVectorType &config) {
        const auto mask_type = mir::MachineVectorType::mask_for(config);
        if (source.reg_class != mir::RegisterClass::VMASK || !source.vector_type.has_value() ||
            *source.vector_type != mask_type) {
            fail_vector_legalization(
                "execution mask type does not match the active vector configuration");
        }
        if (!current_vector_config_.has_value() || current_vector_config_->type != config) {
            fail_vector_legalization("execution mask copy requires an active matching VL/VTYPE");
        }
        if (source.is_physical() && source.name == "v0") {
            fail_vector_legalization("ordinary mask SSA value unexpectedly occupies physical v0");
        }

        auto execution_mask =
            mir::Register::physical_vector("v0", mir::RegisterClass::VMASK, mask_type);
        if (current_execution_mask_source_.has_value() &&
            *current_execution_mask_source_ == source) {
            return execution_mask;
        }
        const auto cached_source = source;
        mir::MachineVectorInfo info(config);
        info.operation = mir::RVVOperation::Copy;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = current_vector_config_->tail_policy;
        info.mask_policy = current_vector_config_->mask_policy;
        emit_vector_instruction(mir::Opcode::RVVMaskCopy,
                                {mir::MachineOperand::reg_def(execution_mask),
                                 mir::MachineOperand::reg_use(std::move(source))},
                                std::move(info));
        current_execution_mask_source_ = cached_source;
        return execution_mask;
    }

    void emit_setvli(const mir::MachineVectorType &type, mir::Register result, oir::Value *avl,
                     mir::VectorTailPolicy tail_policy = mir::VectorTailPolicy::Agnostic,
                     mir::VectorMaskPolicy mask_policy = mir::VectorMaskPolicy::Agnostic) {
        current_execution_mask_source_.reset();
        std::vector<mir::MachineOperand> operands;
        operands.push_back(mir::MachineOperand::reg_def(std::move(result)));
        if (auto *constant = dynamic_cast<oir::ConstantInt *>(avl)) {
            if (constant->value() < 0) {
                fail_vector_legalization("vector EVL must be non-negative");
            }
            auto requested = constant->value();
            if (type.is_fixed()) {
                requested = std::min<std::int64_t>(requested,
                                                   static_cast<std::int64_t>(type.fixed_lanes()));
            }
            if (requested <= 31) {
                operands.push_back(mir::MachineOperand::imm(requested));
            } else {
                auto requested_reg = materialize_internal_i32(requested);
                operands.push_back(mir::MachineOperand::reg_use(std::move(requested_reg)));
            }
        } else {
            auto requested = value_reg(avl);
            if (type.is_fixed()) {
                auto is_negative = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::Slt, {mir::MachineOperand::reg_def(is_negative),
                                        mir::MachineOperand::reg_use(requested),
                                        mir::MachineOperand::reg_use(zero_reg())});
                auto nonnegative_mask = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::AddIW,
                     {mir::MachineOperand::reg_def(nonnegative_mask),
                      mir::MachineOperand::reg_use(is_negative), mir::MachineOperand::imm(-1)});
                auto nonnegative = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::And, {mir::MachineOperand::reg_def(nonnegative),
                                        mir::MachineOperand::reg_use(requested),
                                        mir::MachineOperand::reg_use(nonnegative_mask)});
                auto lanes = materialize_internal_i32(type.fixed_lanes());
                auto below_limit = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::Sltu, {mir::MachineOperand::reg_def(below_limit),
                                         mir::MachineOperand::reg_use(nonnegative),
                                         mir::MachineOperand::reg_use(lanes)});
                auto select_mask = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::SubW, {mir::MachineOperand::reg_def(select_mask),
                                         mir::MachineOperand::reg_use(zero_reg()),
                                         mir::MachineOperand::reg_use(below_limit)});
                auto difference = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::SubW, {mir::MachineOperand::reg_def(difference),
                                         mir::MachineOperand::reg_use(nonnegative),
                                         mir::MachineOperand::reg_use(lanes)});
                auto selected = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::And, {mir::MachineOperand::reg_def(selected),
                                        mir::MachineOperand::reg_use(difference),
                                        mir::MachineOperand::reg_use(select_mask)});
                auto clamped = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::AddW,
                     {mir::MachineOperand::reg_def(clamped), mir::MachineOperand::reg_use(lanes),
                      mir::MachineOperand::reg_use(selected)});
                requested = clamped;
            }
            operands.push_back(mir::MachineOperand::reg_use(std::move(requested)));
        }
        operands.push_back(mir::MachineOperand::implicit_reg_def(vector_state_register("vl")));
        operands.push_back(mir::MachineOperand::implicit_reg_def(vector_state_register("vtype")));

        mir::MachineVectorInfo info(type);
        info.operation = mir::RVVOperation::SetVL;
        info.avl = mir::MachineVectorAVL::operand(1);
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        current_block_->add_instr(
            mir::MachineInstr(mir::Opcode::RVVSetVLI, std::move(operands), std::move(info)));
        current_vector_config_ =
            ActiveVectorConfiguration{type, tail_policy, mask_policy, ActiveVLKind::Operand, avl};
    }

    void emit_fixed_setvli(const mir::MachineVectorType &type,
                           mir::VectorTailPolicy tail_policy = mir::VectorTailPolicy::Agnostic,
                           mir::VectorMaskPolicy mask_policy = mir::VectorMaskPolicy::Agnostic) {
        current_execution_mask_source_.reset();
        const auto lanes = static_cast<std::int64_t>(type.fixed_lanes());
        auto actual_vl = create_vreg(mir::ValueType::I32);
        if (lanes <= 31) {
            std::vector<mir::MachineOperand> operands = {
                mir::MachineOperand::reg_def(std::move(actual_vl)),
                mir::MachineOperand::imm(lanes),
                mir::MachineOperand::implicit_reg_def(vector_state_register("vl")),
                mir::MachineOperand::implicit_reg_def(vector_state_register("vtype")),
            };
            mir::MachineVectorInfo info(type);
            info.operation = mir::RVVOperation::SetVL;
            info.avl = mir::MachineVectorAVL::operand(1);
            info.tail_policy = tail_policy;
            info.mask_policy = mask_policy;
            current_block_->add_instr(
                mir::MachineInstr(mir::Opcode::RVVSetVLI, std::move(operands), std::move(info)));
            current_vector_config_ = ActiveVectorConfiguration{type, tail_policy, mask_policy,
                                                               ActiveVLKind::FixedFull, nullptr};
            return;
        }
        auto requested = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::LoadImm,
             {mir::MachineOperand::reg_def(requested), mir::MachineOperand::imm(lanes)});
        std::vector<mir::MachineOperand> operands = {
            mir::MachineOperand::reg_def(std::move(actual_vl)),
            mir::MachineOperand::reg_use(std::move(requested)),
            mir::MachineOperand::implicit_reg_def(vector_state_register("vl")),
            mir::MachineOperand::implicit_reg_def(vector_state_register("vtype")),
        };
        mir::MachineVectorInfo info(type);
        info.operation = mir::RVVOperation::SetVL;
        info.avl = mir::MachineVectorAVL::operand(1);
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        current_block_->add_instr(
            mir::MachineInstr(mir::Opcode::RVVSetVLI, std::move(operands), std::move(info)));
        current_vector_config_ = ActiveVectorConfiguration{type, tail_policy, mask_policy,
                                                           ActiveVLKind::FixedFull, nullptr};
    }

    void emit_policy_setvli(const mir::MachineVectorType &type, mir::VectorTailPolicy tail_policy,
                            mir::VectorMaskPolicy mask_policy) {
        current_execution_mask_source_.reset();
        std::vector<mir::MachineOperand> operands = {
            mir::MachineOperand::reg_def(
                mir::Register::physical("zero", mir::RegisterClass::GPR, mir::ValueType::I32)),
            mir::MachineOperand::reg_use(
                mir::Register::physical("zero", mir::RegisterClass::GPR, mir::ValueType::I32)),
            mir::MachineOperand::implicit_reg_def(vector_state_register("vl")),
            mir::MachineOperand::implicit_reg_def(vector_state_register("vtype")),
        };
        mir::MachineVectorInfo info(type);
        info.operation = mir::RVVOperation::SetVL;
        info.avl = mir::MachineVectorAVL::operand(1);
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        current_block_->add_instr(
            mir::MachineInstr(mir::Opcode::RVVSetVLI, std::move(operands), std::move(info)));
        auto active = *current_vector_config_;
        active.tail_policy = tail_policy;
        active.mask_policy = mask_policy;
        current_vector_config_ = std::move(active);
    }

    void emit_vlmax_setvli(const mir::MachineVectorType &type,
                           mir::VectorTailPolicy tail_policy = mir::VectorTailPolicy::Agnostic,
                           mir::VectorMaskPolicy mask_policy = mir::VectorMaskPolicy::Agnostic) {
        current_execution_mask_source_.reset();
        auto actual_vl = create_vreg(mir::ValueType::I32);
        std::vector<mir::MachineOperand> operands = {
            mir::MachineOperand::reg_def(std::move(actual_vl)),
            mir::MachineOperand::reg_use(
                mir::Register::physical("zero", mir::RegisterClass::GPR, mir::ValueType::I32)),
            mir::MachineOperand::implicit_reg_def(vector_state_register("vl")),
            mir::MachineOperand::implicit_reg_def(vector_state_register("vtype")),
        };
        mir::MachineVectorInfo info(type);
        info.operation = mir::RVVOperation::SetVL;
        info.avl = mir::MachineVectorAVL::operand(1);
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        current_block_->add_instr(
            mir::MachineInstr(mir::Opcode::RVVSetVLI, std::move(operands), std::move(info)));
        current_vector_config_ =
            ActiveVectorConfiguration{type, tail_policy, mask_policy, ActiveVLKind::VLMAX, nullptr};
    }

    void ensure_vector_configuration(
        const mir::MachineVectorType &type, oir::Value *evl = nullptr,
        mir::VectorTailPolicy tail_policy = mir::VectorTailPolicy::Agnostic,
        mir::VectorMaskPolicy mask_policy = mir::VectorMaskPolicy::Agnostic) {
        const auto config = data_config_for(type);
        if (config.is_scalable() && evl == nullptr) {
            if (current_vector_config_.has_value() && current_vector_config_->type == config &&
                current_vector_config_->vl_kind == ActiveVLKind::VLMAX &&
                current_vector_config_->tail_policy == tail_policy &&
                current_vector_config_->mask_policy == mask_policy) {
                return;
            }
            if (!dominating_scalable_config_.has_value() ||
                !has_equivalent_vtype(dominating_scalable_config_->type, config) ||
                dominating_scalable_config_->avl == nullptr) {
                fail_vector_legalization("scalable operation requires a dominating SetVL with " +
                                         machine_vector_type_name(config));
            }
            // A plain scalable SSA operation denotes every lane in its
            // register group.  It must not inherit a smaller VP EVL merely
            // because that instruction happened to execute immediately
            // before it.
            emit_vlmax_setvli(config, tail_policy, mask_policy);
            return;
        }
        const auto vl_matches = [&]() {
            if (!current_vector_config_.has_value() || current_vector_config_->type != config) {
                return false;
            }
            if (evl != nullptr) {
                return current_vector_config_->vl_kind == ActiveVLKind::Operand &&
                       current_vector_config_->avl == evl;
            }
            if (config.is_fixed()) {
                return current_vector_config_->vl_kind == ActiveVLKind::FixedFull;
            }
            return false;
        }();
        if (vl_matches) {
            if (current_vector_config_->tail_policy == tail_policy &&
                current_vector_config_->mask_policy == mask_policy) {
                return;
            }
            // With unchanged SEW/LMUL, vsetvli x0,x0 changes only the
            // policy bits and preserves the current VL, including a fixed
            // VP operation whose runtime EVL is smaller than its lane count.
            emit_policy_setvli(config, tail_policy, mask_policy);
            return;
        }
        if (config.is_fixed()) {
            if (evl != nullptr) {
                emit_setvli(config, create_vreg(mir::ValueType::I32), evl, tail_policy,
                            mask_policy);
            } else {
                emit_fixed_setvli(config, tail_policy, mask_policy);
            }
            return;
        }
        emit_setvli(config, create_vreg(mir::ValueType::I32), evl, tail_policy, mask_policy);
    }

    bool vp_passthrough_is_discardable(const oir::VPInstruction &inst) const {
        if (!inst.has_passthrough()) {
            fail_vector_legalization("vector-producing VP operation lacks passthrough");
        }
        auto *passthrough = inst.passthrough();
        if (dynamic_cast<oir::UndefValue *>(passthrough) != nullptr ||
            dynamic_cast<oir::ConstantZero *>(passthrough) != nullptr) {
            return inst.tail_policy() == oir::TailPolicy::Agnostic &&
                   inst.mask_policy() == oir::MaskPolicy::Agnostic;
        }
        return false;
    }

    bool vp_mask_is_all_true(const oir::VPInstruction &inst) const {
        // VL already excludes tail lanes.  With agnostic policies, an all-true
        // active mask is therefore identical to an unmasked RVV instruction.
        if (inst.tail_policy() != oir::TailPolicy::Agnostic ||
            inst.mask_policy() != oir::MaskPolicy::Agnostic ||
            !is_all_true_mask(inst.active_mask())) {
            return false;
        }
        return true;
    }

    bool all_true_mask_splat_is_lowering_dead(const oir::SplatInst &inst) const {
        if (!is_all_true_mask(&inst) || inst.uses().empty())
            return false;
        return std::all_of(inst.uses().begin(), inst.uses().end(), [&](const auto &use) {
            const auto *vp = dynamic_cast<const oir::VPInstruction *>(use.user);
            if (vp == nullptr || vp->active_mask() != &inst ||
                !vp_mask_is_all_true(*vp)) {
                return false;
            }
            if (const auto *compare = dynamic_cast<const oir::VPCmpInst *>(vp)) {
                return !is_oversized_fixed_vector(
                    dynamic_cast<const oir::VectorType *>(compare->lhs()->type()));
            }
            if (const auto *load = dynamic_cast<const oir::VPLoadInst *>(vp)) {
                return !is_oversized_fixed_vector(
                    dynamic_cast<const oir::VectorType *>(load->type()));
            }
            if (const auto *store = dynamic_cast<const oir::VPStoreInst *>(vp)) {
                return !is_oversized_fixed_vector(
                    dynamic_cast<const oir::VectorType *>(store->value()->type()));
            }
            if (const auto *binary = dynamic_cast<const oir::VPBinaryInst *>(vp)) {
                const auto *type = dynamic_cast<const oir::VectorType *>(binary->type());
                return type != nullptr && !type->is_mask() &&
                       !is_oversized_fixed_vector(type);
            }
            return false;
        });
    }

    bool strided_gather_index_splat_is_lowering_dead(
        const oir::SplatInst &inst) const {
        if (inst.uses().empty())
            return false;
        return std::all_of(inst.uses().begin(), inst.uses().end(),
                           [&](const auto &use) {
            const auto *gather =
                dynamic_cast<const oir::VPGatherInst *>(use.user);
            return gather != nullptr && gather->indices() == &inst &&
                   !is_oversized_fixed_vector(gather->type()) &&
                   strided_index_plan(gather->indices(),
                                      gather->active_mask(), gather->evl(),
                                      true)
                       .has_value();
        });
    }

    mir::Register make_fresh_tied_passthrough(mir::Register source) {
        if (!source.is_vector() || !source.vector_type.has_value()) {
            fail_vector_legalization("VP passthrough is not a typed vector value");
        }
        if (source.vector_type->is_scalable()) {
            emit_vlmax_setvli(data_config_for(*source.vector_type));
        } else {
            // A destructive TU/MU instruction needs a private copy of every
            // logical fixed lane, not merely the lanes selected by the
            // operation's current EVL.  The caller restores that EVL and its
            // policy immediately after this full-width copy.
            emit_fixed_setvli(data_config_for(*source.vector_type));
        }
        auto fresh = create_vector_vreg(*source.vector_type);
        emit_move(fresh, std::move(source));
        return fresh;
    }

    mir::RVVOperation lower_binary_operation(oir::Instruction::OpID op) const {
        switch (op) {
        case oir::Instruction::OpID::Add:
        case oir::Instruction::OpID::FAdd:
            return mir::RVVOperation::Add;
        case oir::Instruction::OpID::Sub:
        case oir::Instruction::OpID::FSub:
            return mir::RVVOperation::Sub;
        case oir::Instruction::OpID::Mul:
        case oir::Instruction::OpID::FMul:
            return mir::RVVOperation::Mul;
        case oir::Instruction::OpID::SDiv:
        case oir::Instruction::OpID::FDiv:
            return mir::RVVOperation::Div;
        case oir::Instruction::OpID::SRem:
            return mir::RVVOperation::Rem;
        case oir::Instruction::OpID::And:
            return mir::RVVOperation::And;
        case oir::Instruction::OpID::Or:
            return mir::RVVOperation::Or;
        case oir::Instruction::OpID::Xor:
            return mir::RVVOperation::Xor;
        default:
            fail_vector_legalization("unsupported VP binary opcode");
        }
    }

    void lower_setvl(const oir::SetVLInst &inst) {
        if (is_oversized_fixed_vector(inst.vector_type())) {
            fail_vector_legalization(
                "oversized fixed vectors do not expose a single SetVL; full-EVL "
                "operations are lowered piece by piece");
        }
        auto type = data_config_for(legalize_vector_type(*inst.vector_type()));
        emit_setvli(type, value_regs_.at(&inst), inst.avl());
        if (type.is_scalable()) {
            dominating_scalable_config_ = DominatingScalableConfiguration{type, inst.avl()};
        }
    }

    void lower_splat(const oir::SplatInst &inst) {
        if (is_oversized_fixed_vector(inst.type())) {
            auto &bundle = bundle_result(inst);
            auto *mask_constant = dynamic_cast<oir::ConstantInt *>(inst.scalar());
            std::optional<mir::Register> scalar;
            for (const auto &destination : bundle.pieces) {
                const auto type = *destination.vector_type;
                const auto config = data_config_for(type);
                ensure_vector_configuration(config);
                mir::MachineVectorInfo info(config);
                info.avl = mir::MachineVectorAVL::current_vl();
                info.tail_policy = mir::VectorTailPolicy::Agnostic;
                info.mask_policy = mir::VectorMaskPolicy::Agnostic;
                if (type.is_mask()) {
                    if (mask_constant == nullptr ||
                        (mask_constant->value() != 0 && mask_constant->value() != 1)) {
                        fail_vector_legalization(
                            "oversized mask splat requires constant i1 0 or 1");
                    }
                    info.operation = mask_constant->value() == 0 ? mir::RVVOperation::MaskClear
                                                                 : mir::RVVOperation::MaskSet;
                    emit_vector_instruction(mask_constant->value() == 0 ? mir::Opcode::RVVMaskClear
                                                                        : mir::Opcode::RVVMaskSet,
                                            {mir::MachineOperand::reg_def(destination)},
                                            std::move(info));
                    continue;
                }
                info.operation = mir::RVVOperation::Splat;
                std::vector<mir::MachineOperand> operands = {
                    mir::MachineOperand::reg_def(destination)};
                auto opcode = mir::Opcode::RVVSplatVXTA;
                if (auto *constant = dynamic_cast<oir::ConstantInt *>(inst.scalar());
                    constant != nullptr && constant->value() >= -16 && constant->value() <= 15) {
                    opcode = mir::Opcode::RVVSplatVITA;
                    operands.push_back(mir::MachineOperand::imm(constant->value()));
                } else {
                    if (!scalar.has_value()) {
                        scalar = value_reg(inst.scalar());
                    }
                    opcode = scalar->reg_class == mir::RegisterClass::FPR32
                                 ? mir::Opcode::RVVSplatVFTA
                                 : mir::Opcode::RVVSplatVXTA;
                    operands.push_back(mir::MachineOperand::reg_use(*scalar));
                }
                emit_vector_instruction(opcode, std::move(operands), std::move(info));
            }
            return;
        }
        auto type = legalize_vector_type(*static_cast<oir::VectorType *>(inst.type()));
        const auto config = data_config_for(type);
        ensure_vector_configuration(config);
        auto dst = value_regs_.at(&inst);

        mir::MachineVectorInfo info(config);
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        if (type.is_mask()) {
            auto *constant = dynamic_cast<oir::ConstantInt *>(inst.scalar());
            if (constant == nullptr || (constant->value() != 0 && constant->value() != 1)) {
                fail_vector_legalization("mask splat currently requires constant i1 0 or 1: " +
                                         inst.print());
            }
            info.operation =
                constant->value() == 0 ? mir::RVVOperation::MaskClear : mir::RVVOperation::MaskSet;
            emit_vector_instruction(constant->value() == 0 ? mir::Opcode::RVVMaskClear
                                                           : mir::Opcode::RVVMaskSet,
                                    {mir::MachineOperand::reg_def(dst)}, std::move(info));
            return;
        }

        info.operation = mir::RVVOperation::Splat;
        std::vector<mir::MachineOperand> operands = {mir::MachineOperand::reg_def(dst)};
        mir::Opcode opcode = mir::Opcode::RVVSplatVXTA;
        if (auto *constant = dynamic_cast<oir::ConstantInt *>(inst.scalar());
            constant != nullptr && constant->value() >= -16 && constant->value() <= 15) {
            opcode = mir::Opcode::RVVSplatVITA;
            operands.push_back(mir::MachineOperand::imm(constant->value()));
        } else {
            auto scalar = value_reg(inst.scalar());
            opcode = scalar.reg_class == mir::RegisterClass::FPR32 ? mir::Opcode::RVVSplatVFTA
                                                                   : mir::Opcode::RVVSplatVXTA;
            operands.push_back(mir::MachineOperand::reg_use(std::move(scalar)));
        }
        emit_vector_instruction(opcode, std::move(operands), std::move(info));
    }

    void lower_step_vector(const oir::StepVectorInst &inst) {
        if (is_oversized_fixed_vector(inst.type())) {
            auto &bundle = bundle_result(inst);
            for (std::size_t index = 0; index < bundle.pieces.size(); ++index) {
                const auto type = *bundle.pieces[index].vector_type;
                ensure_vector_configuration(type);
                const auto lane_base = bundle.plan.pieces[index].lane_base;
                auto step = lane_base == 0 ? bundle.pieces[index] : create_vector_vreg(type);
                mir::MachineVectorInfo step_info(type);
                step_info.operation = mir::RVVOperation::Step;
                step_info.avl = mir::MachineVectorAVL::current_vl();
                step_info.tail_policy = mir::VectorTailPolicy::Agnostic;
                step_info.mask_policy = mir::VectorMaskPolicy::Agnostic;
                emit_vector_instruction(mir::Opcode::RVVStepTA,
                                        {mir::MachineOperand::reg_def(step)}, std::move(step_info));
                if (lane_base == 0) {
                    continue;
                }
                if (lane_base >
                    static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
                    fail_vector_legalization(
                        "oversized stepvector lane base exceeds i32 semantics");
                }
                auto base = emit_data_splat(
                    type, materialize_internal_i32(static_cast<std::int64_t>(lane_base)));
                mir::MachineVectorInfo add_info(type);
                add_info.operation = mir::RVVOperation::Add;
                add_info.avl = mir::MachineVectorAVL::current_vl();
                add_info.tail_policy = mir::VectorTailPolicy::Agnostic;
                add_info.mask_policy = mir::VectorMaskPolicy::Agnostic;
                emit_vector_instruction(mir::Opcode::RVVIntBinaryVVTA,
                                        {mir::MachineOperand::reg_def(bundle.pieces[index]),
                                         mir::MachineOperand::reg_use(step),
                                         mir::MachineOperand::reg_use(base)},
                                        std::move(add_info));
            }
            return;
        }
        auto type = legalize_vector_type(*static_cast<oir::VectorType *>(inst.type()));
        ensure_vector_configuration(type);
        auto dst = value_regs_.at(&inst);
        mir::MachineVectorInfo info(type);
        info.operation = mir::RVVOperation::Step;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(mir::Opcode::RVVStepTA, {mir::MachineOperand::reg_def(dst)},
                                std::move(info));
    }

    mir::Register materialize_internal_i32(std::int64_t value) {
        if (value == 0) {
            return zero_reg();
        }
        auto result = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::LoadImm,
             {mir::MachineOperand::reg_def(result), mir::MachineOperand::imm(value)});
        return result;
    }

    mir::Register emit_data_splat(const mir::MachineVectorType &type, mir::Register scalar) {
        if (type.is_mask()) {
            fail_vector_legalization("data splat cannot use a mask vector type");
        }
        ensure_vector_configuration(type);
        auto result = create_vector_vreg(type);
        mir::MachineVectorInfo info(type);
        info.operation = mir::RVVOperation::Splat;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        const auto opcode = type.element_type() == mir::ValueType::F32 ? mir::Opcode::RVVSplatVFTA
                                                                       : mir::Opcode::RVVSplatVXTA;
        emit_vector_instruction(
            opcode,
            {mir::MachineOperand::reg_def(result), mir::MachineOperand::reg_use(std::move(scalar))},
            std::move(info));
        return result;
    }

    mir::Register emit_zero_data_vector(const mir::MachineVectorType &type) {
        if (type.element_type() == mir::ValueType::F32) {
            auto scalar = create_vreg(mir::ValueType::F32);
            emit(mir::Opcode::LoadFloatImm,
                 {mir::MachineOperand::reg_def(scalar), mir::MachineOperand::float_imm(0.0F)});
            return emit_data_splat(type, scalar);
        }
        return emit_data_splat(type, zero_reg());
    }

    mir::Register emit_compare_mask(const mir::MachineVectorType &type, mir::RVVOperation operation,
                                    mir::Register lhs, mir::Register rhs) {
        ensure_vector_configuration(type);
        auto result = create_vector_vreg(mir::MachineVectorType::mask_for(type));
        mir::MachineVectorInfo info(type);
        info.operation = operation;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(mir::Opcode::RVVCompareVVTA,
                                {mir::MachineOperand::reg_def(result),
                                 mir::MachineOperand::reg_use(std::move(lhs)),
                                 mir::MachineOperand::reg_use(std::move(rhs))},
                                std::move(info));
        return result;
    }

    mir::Register emit_mask_logical(const mir::MachineVectorType &config,
                                    mir::RVVOperation operation, mir::Register lhs,
                                    mir::Register rhs,
                                    std::optional<mir::Register> destination = std::nullopt) {
        if (config.is_mask()) {
            fail_vector_legalization("mask logical operation requires a data VTYPE configuration");
        }
        ensure_vector_configuration(config);
        auto result = destination.has_value()
                          ? *destination
                          : create_vector_vreg(mir::MachineVectorType::mask_for(config));
        mir::MachineVectorInfo info(config);
        info.operation = operation;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(mir::Opcode::RVVMaskLogical,
                                {mir::MachineOperand::reg_def(result),
                                 mir::MachineOperand::reg_use(std::move(lhs)),
                                 mir::MachineOperand::reg_use(std::move(rhs))},
                                std::move(info));
        return result;
    }

    mir::Register emit_mask_constant(const mir::MachineVectorType &config, bool value) {
        ensure_vector_configuration(config);
        auto result = create_vector_vreg(mir::MachineVectorType::mask_for(config));
        mir::MachineVectorInfo info(config);
        info.operation = value ? mir::RVVOperation::MaskSet : mir::RVVOperation::MaskClear;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(value ? mir::Opcode::RVVMaskSet : mir::Opcode::RVVMaskClear,
                                {mir::MachineOperand::reg_def(result)}, std::move(info));
        return result;
    }

    mir::Register emit_evl_prefix_mask(const mir::MachineVectorType &config, oir::Value *evl) {
        if (evl == nullptr) {
            fail_vector_legalization("VP mask binary is missing EVL");
        }
        if (auto *constant = dynamic_cast<oir::ConstantInt *>(evl)) {
            if (constant->value() < 0) {
                fail_vector_legalization("vector EVL must be non-negative");
            }
            if (constant->value() == 0) {
                return emit_mask_constant(config, false);
            }
            if (config.is_fixed() &&
                static_cast<std::uint64_t>(constant->value()) >= config.fixed_lanes()) {
                return emit_mask_constant(config, true);
            }
        }

        auto requested = value_reg(evl);
        if (dynamic_cast<oir::ConstantInt *>(evl) == nullptr) {
            auto is_negative = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::Slt, {mir::MachineOperand::reg_def(is_negative),
                                    mir::MachineOperand::reg_use(requested),
                                    mir::MachineOperand::reg_use(zero_reg())});
            auto nonnegative_mask = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::AddIW,
                 {mir::MachineOperand::reg_def(nonnegative_mask),
                  mir::MachineOperand::reg_use(is_negative), mir::MachineOperand::imm(-1)});
            auto nonnegative = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::And, {mir::MachineOperand::reg_def(nonnegative),
                                    mir::MachineOperand::reg_use(requested),
                                    mir::MachineOperand::reg_use(nonnegative_mask)});
            requested = nonnegative;
        }

        ensure_vector_configuration(config);
        auto lane_ids = create_vector_vreg(config);
        mir::MachineVectorInfo step_info(config);
        step_info.operation = mir::RVVOperation::Step;
        step_info.avl = mir::MachineVectorAVL::current_vl();
        step_info.tail_policy = mir::VectorTailPolicy::Agnostic;
        step_info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(mir::Opcode::RVVStepTA, {mir::MachineOperand::reg_def(lane_ids)},
                                std::move(step_info));
        auto limit = emit_data_splat(config, std::move(requested));
        return emit_compare_mask(config, mir::RVVOperation::Lt, std::move(lane_ids),
                                 std::move(limit));
    }

    void lower_vp_mask_binary(const oir::VPBinaryInst &inst,
                              const mir::MachineVectorType &mask_type) {
        const auto config = data_config_for(mask_type);
        if (config.is_fixed()) {
            ensure_vector_configuration(config);
        } else {
            emit_vlmax_setvli(config);
        }

        mir::RVVOperation operation;
        switch (inst.binary_op()) {
        case oir::Instruction::OpID::And:
            operation = mir::RVVOperation::MaskAnd;
            break;
        case oir::Instruction::OpID::Or:
            operation = mir::RVVOperation::MaskOr;
            break;
        case oir::Instruction::OpID::Xor:
            operation = mir::RVVOperation::MaskXor;
            break;
        default:
            fail_vector_legalization("VP mask binary supports only And/Or/Xor");
        }

        const bool tail_agnostic = inst.tail_policy() == oir::TailPolicy::Agnostic;
        const bool mask_agnostic = inst.mask_policy() == oir::MaskPolicy::Agnostic;
        if (inst.binary_op() == oir::Instruction::OpID::Xor &&
            tail_agnostic && mask_agnostic) {
            const bool lhs_true = is_all_true_mask(inst.lhs());
            const bool rhs_true = is_all_true_mask(inst.rhs());
            if (lhs_true != rhs_true) {
                auto source = value_reg(lhs_true ? inst.rhs() : inst.lhs());
                emit_mask_logical(config, mir::RVVOperation::MaskNot, source,
                                  source, value_regs_.at(&inst));
                return;
            }
        }

        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        if (tail_agnostic && mask_agnostic) {
            emit_mask_logical(config, operation, std::move(lhs), std::move(rhs),
                              value_regs_.at(&inst));
            return;
        }

        auto computed = emit_mask_logical(config, operation, std::move(lhs), std::move(rhs));
        auto active = value_reg(inst.active_mask());
        auto prefix = emit_evl_prefix_mask(config, inst.evl());
        mir::Register update;
        if (!tail_agnostic && !mask_agnostic) {
            update = emit_mask_logical(config, mir::RVVOperation::MaskAnd, std::move(active),
                                       std::move(prefix));
        } else if (!tail_agnostic) {
            update = std::move(prefix);
        } else {
            auto outside_prefix =
                emit_mask_logical(config, mir::RVVOperation::MaskNot, prefix, prefix);
            update = emit_mask_logical(config, mir::RVVOperation::MaskOr, std::move(active),
                                       std::move(outside_prefix));
        }

        auto kept_computed =
            emit_mask_logical(config, mir::RVVOperation::MaskAnd, update, std::move(computed));
        auto not_update = emit_mask_logical(config, mir::RVVOperation::MaskNot, update, update);
        auto kept_passthrough =
            emit_mask_logical(config, mir::RVVOperation::MaskAnd, std::move(not_update),
                              value_reg(inst.passthrough()));
        emit_mask_logical(config, mir::RVVOperation::MaskOr, std::move(kept_computed),
                          std::move(kept_passthrough), value_regs_.at(&inst));
    }

    mir::Register emit_mask_zext(const mir::MachineVectorType &mask_type, mir::Register source) {
        if (!mask_type.is_mask()) {
            fail_vector_legalization("mask zext requires a mask source type");
        }
        const auto data_type = data_config_for(mask_type);
        auto zero = emit_zero_data_vector(data_type);
        auto one = emit_data_splat(data_type, materialize_internal_i32(1));
        ensure_vector_configuration(data_type);
        auto selector = prepare_execution_mask(std::move(source), data_type);
        auto result = create_vector_vreg(data_type);
        mir::MachineVectorInfo info(data_type);
        info.operation = mir::RVVOperation::Merge;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        info.mask_operand = 3;
        emit_vector_instruction(
            mir::Opcode::RVVMergeVVM,
            {mir::MachineOperand::reg_def(result), mir::MachineOperand::reg_use(zero),
             mir::MachineOperand::reg_use(one), mir::MachineOperand::reg_use(selector)},
            std::move(info));
        return result;
    }

    void emit_extract_data(mir::Register destination, const mir::MachineVectorType &type,
                           mir::Register source, mir::Register dynamic_index,
                           std::optional<std::int64_t> constant_index) {
        ensure_vector_configuration(type);
        if (!constant_index.has_value() || *constant_index != 0) {
            auto passthrough = emit_zero_data_vector(type);
            auto shifted = create_vector_vreg(type);
            mir::MachineVectorInfo slide_info(type);
            slide_info.operation = mir::RVVOperation::SlideDown;
            slide_info.avl = mir::MachineVectorAVL::current_vl();
            slide_info.tail_policy = mir::VectorTailPolicy::Agnostic;
            slide_info.mask_policy = mir::VectorMaskPolicy::Agnostic;
            slide_info.passthrough_operand = 1;
            std::vector<mir::MachineOperand> operands = {
                mir::MachineOperand::reg_def(shifted),
                mir::MachineOperand::reg_use(passthrough),
                mir::MachineOperand::reg_use(source),
            };
            auto opcode = mir::Opcode::RVVSlideDownVX;
            if (constant_index.has_value() && *constant_index >= 0 && *constant_index <= 31) {
                opcode = mir::Opcode::RVVSlideDownVI;
                operands.push_back(mir::MachineOperand::imm(*constant_index));
            } else {
                operands.push_back(mir::MachineOperand::reg_use(std::move(dynamic_index)));
            }
            emit_vector_instruction(opcode, std::move(operands), std::move(slide_info));
            source = shifted;
        }

        mir::MachineVectorInfo extract_info(type);
        extract_info.operation = mir::RVVOperation::Extract;
        extract_info.avl = mir::MachineVectorAVL::current_vl();
        extract_info.tail_policy = mir::VectorTailPolicy::Agnostic;
        extract_info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(mir::Opcode::RVVExtractElement,
                                {mir::MachineOperand::reg_def(destination),
                                 mir::MachineOperand::reg_use(source), mir::MachineOperand::imm(0)},
                                std::move(extract_info));
    }

    void emit_insert_data(mir::Register destination, const mir::MachineVectorType &type,
                          mir::Register source, mir::Register element, mir::Register index) {
        auto index_type = type.is_fixed()
                              ? mir::MachineVectorType::fixed(mir::ValueType::I32, type.sew_bits(),
                                                              type.lmul(), type.fixed_lanes())
                              : mir::MachineVectorType::scalable(mir::ValueType::I32,
                                                                 type.sew_bits(), type.lmul());
        ensure_vector_configuration(index_type);
        auto lane_ids = create_vector_vreg(index_type);
        mir::MachineVectorInfo step_info(index_type);
        step_info.operation = mir::RVVOperation::Step;
        step_info.avl = mir::MachineVectorAVL::current_vl();
        step_info.tail_policy = mir::VectorTailPolicy::Agnostic;
        step_info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(mir::Opcode::RVVStepTA, {mir::MachineOperand::reg_def(lane_ids)},
                                std::move(step_info));
        auto splat_index = emit_data_splat(index_type, std::move(index));
        auto lane_mask =
            emit_compare_mask(index_type, mir::RVVOperation::Eq, lane_ids, splat_index);

        ensure_vector_configuration(type);
        auto splat_element = emit_data_splat(type, std::move(element));
        auto execution_mask = prepare_execution_mask(std::move(lane_mask), type);
        mir::MachineVectorInfo merge_info(type);
        merge_info.operation = mir::RVVOperation::Merge;
        merge_info.avl = mir::MachineVectorAVL::current_vl();
        merge_info.tail_policy = mir::VectorTailPolicy::Agnostic;
        merge_info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        merge_info.mask_operand = 3;
        emit_vector_instruction(mir::Opcode::RVVMergeVVM,
                                {mir::MachineOperand::reg_def(destination),
                                 mir::MachineOperand::reg_use(source),
                                 mir::MachineOperand::reg_use(splat_element),
                                 mir::MachineOperand::reg_use(execution_mask)},
                                std::move(merge_info));
    }

    std::optional<std::int64_t> checked_fixed_index(const oir::Value *index,
                                                    const mir::MachineVectorType &type,
                                                    const std::string &context) const {
        const auto *constant = dynamic_cast<const oir::ConstantInt *>(index);
        if (constant == nullptr) {
            return std::nullopt;
        }
        if (!type.is_fixed() || constant->value() < 0 ||
            static_cast<std::uint64_t>(constant->value()) >= type.fixed_lanes()) {
            fail_vector_legalization(context + " constant lane index is out of range");
        }
        return constant->value();
    }

    mir::Register chunk_local_index(oir::Value *global_index, std::uint64_t lane_base) {
        auto index = value_reg(global_index);
        if (lane_base == 0) {
            return index;
        }
        if (lane_base > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            fail_vector_legalization("fixed chunk lane base exceeds i32 index semantics");
        }
        auto local = create_vreg(mir::ValueType::I32);
        const auto signed_base = static_cast<std::int64_t>(lane_base);
        if (neg_fits_simm12(-signed_base)) {
            emit(mir::Opcode::AddIW,
                 {mir::MachineOperand::reg_def(local), mir::MachineOperand::reg_use(index),
                  mir::MachineOperand::imm(-signed_base)});
            return local;
        }
        auto base = materialize_internal_i32(signed_base);
        emit(mir::Opcode::SubW,
             {mir::MachineOperand::reg_def(local), mir::MachineOperand::reg_use(index),
              mir::MachineOperand::reg_use(base)});
        return local;
    }

    mir::Register fixed_piece_lane_select_mask(oir::Value *global_index,
                                               const target::RVVFixedVectorPiece &piece) {
        const auto lane_end = piece.lane_base + piece.lane_count;
        if (lane_end < piece.lane_base || lane_end > std::numeric_limits<std::uint32_t>::max()) {
            fail_vector_legalization("fixed chunk dynamic lane range exceeds i32 index semantics");
        }
        auto index = value_reg(global_index);
        auto base = materialize_internal_i32(piece.lane_base);
        auto below_base = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::Sltu,
             {mir::MachineOperand::reg_def(below_base), mir::MachineOperand::reg_use(index),
              mir::MachineOperand::reg_use(base)});
        auto at_or_above_base = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::XorI,
             {mir::MachineOperand::reg_def(at_or_above_base),
              mir::MachineOperand::reg_use(below_base), mir::MachineOperand::imm(1)});
        auto end = materialize_internal_i32(lane_end);
        auto below_end = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::Sltu,
             {mir::MachineOperand::reg_def(below_end), mir::MachineOperand::reg_use(index),
              mir::MachineOperand::reg_use(end)});
        auto selected = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::And, {mir::MachineOperand::reg_def(selected),
                                mir::MachineOperand::reg_use(at_or_above_base),
                                mir::MachineOperand::reg_use(below_end)});
        auto select_mask = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::SubW,
             {mir::MachineOperand::reg_def(select_mask), mir::MachineOperand::reg_use(zero_reg()),
              mir::MachineOperand::reg_use(selected)});
        return select_mask;
    }

    std::size_t fixed_bundle_piece_for_lane(const FixedVectorBundle &bundle,
                                            std::uint64_t lane) const {
        for (std::size_t index = 0; index < bundle.plan.pieces.size(); ++index) {
            const auto &piece = bundle.plan.pieces[index];
            if (lane >= piece.lane_base && lane - piece.lane_base < piece.lane_count) {
                return index;
            }
        }
        fail_vector_legalization("logical lane is absent from fixed chunk plan");
    }

    void lower_extract_element(const oir::ExtractElementInst &inst) {
        const auto *oir_type = dynamic_cast<const oir::VectorType *>(inst.vector()->type());
        if (oir_type == nullptr) {
            fail_vector_legalization("extractelement source is not a vector");
        }
        if (is_oversized_fixed_vector(oir_type)) {
            auto source = value_bundle(inst.vector());
            auto destination = value_regs_.at(&inst);
            if (auto *constant = dynamic_cast<oir::ConstantInt *>(inst.index())) {
                if (constant->value() < 0 || static_cast<std::uint64_t>(constant->value()) >=
                                                 source.plan.shape.logical_lanes) {
                    fail_vector_legalization("extractelement constant lane index is out of range");
                }
                const auto piece_index = fixed_bundle_piece_for_lane(
                    source, static_cast<std::uint64_t>(constant->value()));
                const auto local =
                    static_cast<std::int64_t>(static_cast<std::uint64_t>(constant->value()) -
                                              source.plan.pieces[piece_index].lane_base);
                auto piece = source.pieces[piece_index];
                auto work_type = *piece.vector_type;
                if (work_type.is_mask()) {
                    piece = emit_mask_zext(work_type, piece);
                    work_type = data_config_for(work_type);
                }
                emit_extract_data(destination, work_type, piece, materialize_internal_i32(local),
                                  local);
                return;
            }

            const bool is_float = oir_type->is_float_vector();
            const bool is_mask = oir_type->is_mask();
            auto accumulated = create_vreg(mir::ValueType::I32);
            emit_move(accumulated, zero_reg());
            for (std::size_t index = 0; index < source.pieces.size(); ++index) {
                auto piece = source.pieces[index];
                auto work_type = *piece.vector_type;
                if (work_type.is_mask()) {
                    piece = emit_mask_zext(work_type, piece);
                    work_type = data_config_for(work_type);
                }
                auto extracted = create_vreg(work_type.element_type());
                emit_extract_data(
                    extracted, work_type, piece,
                    chunk_local_index(inst.index(), source.plan.pieces[index].lane_base),
                    std::nullopt);
                mir::Register bits = extracted;
                if (is_float) {
                    bits = create_vreg(mir::ValueType::I32);
                    emit(mir::Opcode::FmvXW, {mir::MachineOperand::reg_def(bits),
                                              mir::MachineOperand::reg_use(extracted)});
                }
                // An out-of-piece vslidedown lane is architecturally
                // unobservable, but its tail value is not a portable zero at
                // larger runtime VLENs.  Select the one logical piece
                // explicitly before OR-combining all candidates.
                auto selected_bits = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::And, {mir::MachineOperand::reg_def(selected_bits),
                                        mir::MachineOperand::reg_use(bits),
                                        mir::MachineOperand::reg_use(fixed_piece_lane_select_mask(
                                            inst.index(), source.plan.pieces[index]))});
                bits = selected_bits;
                auto disjoint = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::Xor, {mir::MachineOperand::reg_def(disjoint),
                                        mir::MachineOperand::reg_use(accumulated),
                                        mir::MachineOperand::reg_use(bits)});
                auto overlap = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::And, {mir::MachineOperand::reg_def(overlap),
                                        mir::MachineOperand::reg_use(accumulated),
                                        mir::MachineOperand::reg_use(bits)});
                auto next = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::Xor,
                     {mir::MachineOperand::reg_def(next), mir::MachineOperand::reg_use(overlap),
                      mir::MachineOperand::reg_use(disjoint)});
                accumulated = next;
            }
            if (is_float) {
                emit(mir::Opcode::FmvWX, {mir::MachineOperand::reg_def(destination),
                                          mir::MachineOperand::reg_use(accumulated)});
            } else if (is_mask) {
                emit(mir::Opcode::AndI,
                     {mir::MachineOperand::reg_def(destination),
                      mir::MachineOperand::reg_use(accumulated), mir::MachineOperand::imm(1)});
            } else {
                emit_move(destination, accumulated);
            }
            return;
        }
        const auto source_type = legalize_vector_type(*oir_type);
        auto source = value_reg(inst.vector());
        auto work_type = source_type;
        if (source_type.is_mask()) {
            source = emit_mask_zext(source_type, std::move(source));
            work_type = data_config_for(source_type);
        }
        const auto constant_index = checked_fixed_index(inst.index(), work_type, "extractelement");
        auto index = value_reg(inst.index());
        emit_extract_data(value_regs_.at(&inst), work_type, std::move(source), std::move(index),
                          constant_index);
    }

    void lower_insert_element(const oir::InsertElementInst &inst) {
        const auto *oir_type = dynamic_cast<const oir::VectorType *>(inst.type());
        if (oir_type == nullptr) {
            fail_vector_legalization("insertelement result is not a vector");
        }
        if (is_oversized_fixed_vector(oir_type)) {
            auto source = value_bundle(inst.vector());
            auto &destination = bundle_result(inst);
            auto element = value_reg(inst.element());
            const auto *constant = dynamic_cast<oir::ConstantInt *>(inst.index());
            std::optional<std::size_t> selected_piece;
            if (constant != nullptr) {
                if (constant->value() < 0 || static_cast<std::uint64_t>(constant->value()) >=
                                                 source.plan.shape.logical_lanes) {
                    fail_vector_legalization("insertelement constant lane index is out of range");
                }
                selected_piece = fixed_bundle_piece_for_lane(
                    source, static_cast<std::uint64_t>(constant->value()));
            }
            for (std::size_t index = 0; index < source.pieces.size(); ++index) {
                if (selected_piece.has_value() && *selected_piece != index) {
                    emit_move(destination.pieces[index], source.pieces[index]);
                    continue;
                }
                auto type = *source.pieces[index].vector_type;
                auto local_index =
                    constant != nullptr
                        ? materialize_internal_i32(
                              constant->value() -
                              static_cast<std::int64_t>(source.plan.pieces[index].lane_base))
                        : chunk_local_index(inst.index(), source.plan.pieces[index].lane_base);
                if (!type.is_mask()) {
                    emit_insert_data(destination.pieces[index], type, source.pieces[index], element,
                                     local_index);
                    continue;
                }
                const auto data_type = data_config_for(type);
                auto data_source = emit_mask_zext(type, source.pieces[index]);
                auto inserted = create_vector_vreg(data_type);
                emit_insert_data(inserted, data_type, data_source, element, local_index);
                auto zero = emit_zero_data_vector(data_type);
                auto result = emit_compare_mask(data_type, mir::RVVOperation::Ne, inserted, zero);
                emit_move(destination.pieces[index], result);
            }
            return;
        }
        const auto type = legalize_vector_type(*oir_type);
        const auto constant_index = checked_fixed_index(inst.index(), type, "insertelement");
        (void)constant_index;
        auto source = value_reg(inst.vector());
        auto element = value_reg(inst.element());
        auto index = value_reg(inst.index());
        if (!type.is_mask()) {
            emit_insert_data(value_regs_.at(&inst), type, std::move(source), std::move(element),
                             std::move(index));
            return;
        }

        const auto data_type = data_config_for(type);
        auto data_source = emit_mask_zext(type, std::move(source));
        auto inserted = create_vector_vreg(data_type);
        emit_insert_data(inserted, data_type, std::move(data_source), std::move(element),
                         std::move(index));
        auto zero = emit_zero_data_vector(data_type);
        auto result = emit_compare_mask(data_type, mir::RVVOperation::Ne, std::move(inserted),
                                        std::move(zero));
        emit_move(value_regs_.at(&inst), std::move(result));
    }

    void lower_shuffle_vector(const oir::ShuffleVectorInst &inst) {
        const auto *oir_type = dynamic_cast<const oir::VectorType *>(inst.type());
        const auto *source_oir_type = dynamic_cast<const oir::VectorType *>(inst.lhs()->type());
        if (oir_type == nullptr || source_oir_type == nullptr) {
            fail_vector_legalization("shufflevector result is not a vector");
        }
        if (is_oversized_fixed_vector(oir_type) || is_oversized_fixed_vector(source_oir_type)) {
            if (!is_oversized_fixed_vector(oir_type) ||
                !is_oversized_fixed_vector(source_oir_type)) {
                fail_vector_legalization("shufflevector cannot cross the fixed bundle boundary");
            }
            auto lhs = value_bundle(inst.lhs());
            auto rhs = value_bundle(inst.rhs());
            auto &destination = bundle_result(inst);
            if (inst.shuffle_mask().size() != destination.plan.shape.logical_lanes) {
                fail_vector_legalization("oversized shufflevector mask does not cover the result");
            }
            if (lhs.plan.shape.logical_lanes != rhs.plan.shape.logical_lanes ||
                lhs.pieces.size() != rhs.pieces.size()) {
                fail_vector_legalization("oversized shufflevector source plans disagree");
            }

            std::vector<mir::Register> lhs_work = lhs.pieces;
            std::vector<mir::Register> rhs_work = rhs.pieces;
            std::vector<mir::MachineVectorType> source_work_types;
            source_work_types.reserve(lhs.pieces.size());
            for (std::size_t index = 0; index < lhs.pieces.size(); ++index) {
                auto type = *lhs.pieces[index].vector_type;
                if (type.is_mask()) {
                    lhs_work[index] = emit_mask_zext(type, lhs_work[index]);
                    rhs_work[index] = emit_mask_zext(type, rhs_work[index]);
                    type = data_config_for(type);
                }
                source_work_types.push_back(type);
            }

            std::vector<mir::Register> lanes;
            lanes.reserve(inst.shuffle_mask().size());
            const auto source_lanes = lhs.plan.shape.logical_lanes;
            for (const auto selected : inst.shuffle_mask()) {
                const auto element_type = source_work_types.front().element_type();
                auto scalar = create_vreg(element_type);
                if (selected < 0) {
                    if (element_type == mir::ValueType::F32) {
                        emit(mir::Opcode::LoadFloatImm, {mir::MachineOperand::reg_def(scalar),
                                                         mir::MachineOperand::float_imm(0.0F)});
                    } else {
                        emit_move(scalar, zero_reg());
                    }
                    lanes.push_back(scalar);
                    continue;
                }
                const auto selected_u = static_cast<std::uint64_t>(selected);
                if (selected_u >= source_lanes * 2U) {
                    fail_vector_legalization("oversized shufflevector lane exceeds both sources");
                }
                const bool from_rhs = selected_u >= source_lanes;
                const auto source_lane = from_rhs ? selected_u - source_lanes : selected_u;
                const auto &source_bundle = from_rhs ? rhs : lhs;
                const auto piece_index = fixed_bundle_piece_for_lane(source_bundle, source_lane);
                const auto local = static_cast<std::int64_t>(
                    source_lane - source_bundle.plan.pieces[piece_index].lane_base);
                emit_extract_data(scalar, source_work_types[piece_index],
                                  from_rhs ? rhs_work[piece_index] : lhs_work[piece_index],
                                  materialize_internal_i32(local), local);
                lanes.push_back(scalar);
            }

            for (std::size_t piece_index = 0; piece_index < destination.pieces.size();
                 ++piece_index) {
                const auto result_type = *destination.pieces[piece_index].vector_type;
                const auto construction_type = data_config_for(result_type);
                auto current = emit_zero_data_vector(construction_type);
                const auto &piece = destination.plan.pieces[piece_index];
                for (std::uint64_t local = 0; local < piece.lane_count; ++local) {
                    const auto logical_lane = piece.lane_base + local;
                    const bool direct = !result_type.is_mask() && local + 1U == piece.lane_count;
                    auto next = direct ? destination.pieces[piece_index]
                                       : create_vector_vreg(construction_type);
                    emit_insert_data(next, construction_type, current,
                                     lanes[static_cast<std::size_t>(logical_lane)],
                                     materialize_internal_i32(static_cast<std::int64_t>(local)));
                    current = next;
                }
                if (!result_type.is_mask()) {
                    continue;
                }
                auto zero = emit_zero_data_vector(construction_type);
                auto result =
                    emit_compare_mask(construction_type, mir::RVVOperation::Ne, current, zero);
                emit_move(destination.pieces[piece_index], result);
            }
            return;
        }
        const auto result_type = legalize_vector_type(*oir_type);
        const auto source_type = legalize_vector_type(*source_oir_type);
        if (!result_type.is_fixed() || inst.shuffle_mask().size() != result_type.fixed_lanes()) {
            fail_vector_legalization("shufflevector requires an exact fixed-lane mask");
        }
        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        auto extraction_type = source_type;
        auto construction_type = result_type;
        if (result_type.is_mask()) {
            if (!source_type.is_mask()) {
                fail_vector_legalization("mask shuffle requires mask source vectors");
            }
            lhs = emit_mask_zext(source_type, std::move(lhs));
            rhs = emit_mask_zext(source_type, std::move(rhs));
            extraction_type = data_config_for(source_type);
            construction_type = data_config_for(result_type);
        } else if (source_type.element_type() != result_type.element_type() ||
                   source_type.is_mask()) {
            fail_vector_legalization("shufflevector source and result element types disagree");
        }

        // Extract every source lane before constructing the result.  This
        // keeps the two M8 sources plus one slide temporary within the three
        // allocatable M8 groups required by N=31 at VLEN=128.
        std::vector<mir::Register> lanes;
        lanes.reserve(inst.shuffle_mask().size());
        for (const auto lane : inst.shuffle_mask()) {
            auto scalar = create_vreg(extraction_type.element_type());
            if (lane < 0) {
                if (extraction_type.element_type() == mir::ValueType::F32) {
                    emit(mir::Opcode::LoadFloatImm, {mir::MachineOperand::reg_def(scalar),
                                                     mir::MachineOperand::float_imm(0.0F)});
                } else {
                    emit_move(scalar, zero_reg());
                }
                lanes.push_back(std::move(scalar));
                continue;
            }
            const auto width = static_cast<std::int64_t>(extraction_type.fixed_lanes());
            if (lane >= width * 2) {
                fail_vector_legalization("shufflevector lane index exceeds both source vectors");
            }
            const auto source_lane = lane < width ? lane : lane - width;
            auto source = lane < width ? lhs : rhs;
            emit_extract_data(scalar, extraction_type, std::move(source),
                              materialize_internal_i32(source_lane), source_lane);
            lanes.push_back(std::move(scalar));
        }

        auto current = emit_zero_data_vector(construction_type);
        for (std::size_t lane = 0; lane < lanes.size(); ++lane) {
            const bool direct_result = !result_type.is_mask() && lane + 1 == lanes.size();
            auto next =
                direct_result ? value_regs_.at(&inst) : create_vector_vreg(construction_type);
            emit_insert_data(next, construction_type, current, lanes[lane],
                             materialize_internal_i32(static_cast<std::int64_t>(lane)));
            current = next;
        }
        if (!result_type.is_mask()) {
            return;
        }
        auto zero = emit_zero_data_vector(construction_type);
        auto result = emit_compare_mask(construction_type, mir::RVVOperation::Ne,
                                        std::move(current), std::move(zero));
        emit_move(value_regs_.at(&inst), std::move(result));
    }

    void lower_vector_select(const oir::VectorSelectInst &inst) {
        const auto *oir_type = dynamic_cast<const oir::VectorType *>(inst.type());
        if (oir_type == nullptr) {
            fail_vector_legalization("vector select result is not a vector");
        }
        if (is_oversized_fixed_vector(oir_type)) {
            auto condition = value_bundle(inst.condition());
            auto true_value = value_bundle(inst.true_value());
            auto false_value = value_bundle(inst.false_value());
            auto &destination = bundle_result(inst);
            if (condition.pieces.size() != destination.pieces.size() ||
                true_value.pieces.size() != destination.pieces.size() ||
                false_value.pieces.size() != destination.pieces.size()) {
                fail_vector_legalization(
                    "oversized vector select operands have different chunk plans");
            }
            for (std::size_t index = 0; index < destination.pieces.size(); ++index) {
                const auto result_type = *destination.pieces[index].vector_type;
                auto true_piece = true_value.pieces[index];
                auto false_piece = false_value.pieces[index];
                auto work_type = result_type;
                if (result_type.is_mask()) {
                    true_piece = emit_mask_zext(result_type, true_piece);
                    false_piece = emit_mask_zext(result_type, false_piece);
                    work_type = data_config_for(result_type);
                }
                ensure_vector_configuration(work_type);
                auto selector = prepare_execution_mask(condition.pieces[index], work_type);
                auto merged = result_type.is_mask() ? create_vector_vreg(work_type)
                                                    : destination.pieces[index];
                mir::MachineVectorInfo info(work_type);
                info.operation = mir::RVVOperation::Merge;
                info.avl = mir::MachineVectorAVL::current_vl();
                info.tail_policy = mir::VectorTailPolicy::Agnostic;
                info.mask_policy = mir::VectorMaskPolicy::Agnostic;
                info.mask_operand = 3;
                emit_vector_instruction(mir::Opcode::RVVMergeVVM,
                                        {mir::MachineOperand::reg_def(merged),
                                         mir::MachineOperand::reg_use(false_piece),
                                         mir::MachineOperand::reg_use(true_piece),
                                         mir::MachineOperand::reg_use(selector)},
                                        std::move(info));
                if (!result_type.is_mask()) {
                    continue;
                }
                auto zero = emit_zero_data_vector(work_type);
                auto result = emit_compare_mask(work_type, mir::RVVOperation::Ne, merged, zero);
                emit_move(destination.pieces[index], result);
            }
            return;
        }
        const auto result_type = legalize_vector_type(*oir_type);
        auto condition = value_reg(inst.condition());
        auto true_value = value_reg(inst.true_value());
        auto false_value = value_reg(inst.false_value());
        auto work_type = result_type;
        if (result_type.is_mask()) {
            true_value = emit_mask_zext(result_type, std::move(true_value));
            false_value = emit_mask_zext(result_type, std::move(false_value));
            work_type = data_config_for(result_type);
        }
        ensure_vector_configuration(work_type);
        auto selector = prepare_execution_mask(std::move(condition), work_type);
        auto merged = result_type.is_mask() ? create_vector_vreg(work_type) : value_regs_.at(&inst);
        mir::MachineVectorInfo info(work_type);
        info.operation = mir::RVVOperation::Merge;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        info.mask_operand = 3;
        emit_vector_instruction(
            mir::Opcode::RVVMergeVVM,
            {mir::MachineOperand::reg_def(merged), mir::MachineOperand::reg_use(false_value),
             mir::MachineOperand::reg_use(true_value), mir::MachineOperand::reg_use(selector)},
            std::move(info));
        if (!result_type.is_mask()) {
            return;
        }
        auto zero = emit_zero_data_vector(work_type);
        auto result =
            emit_compare_mask(work_type, mir::RVVOperation::Ne, std::move(merged), std::move(zero));
        emit_move(value_regs_.at(&inst), std::move(result));
    }

    void lower_vector_cast(const oir::VectorCastInst &inst) {
        const auto *result_oir_type = dynamic_cast<const oir::VectorType *>(inst.type());
        const auto *source_oir_type = dynamic_cast<const oir::VectorType *>(inst.source()->type());
        if (result_oir_type == nullptr || source_oir_type == nullptr) {
            fail_vector_legalization("vector cast has malformed vector types");
        }
        if (is_oversized_fixed_vector(result_oir_type) ||
            is_oversized_fixed_vector(source_oir_type)) {
            if (!is_oversized_fixed_vector(result_oir_type) ||
                !is_oversized_fixed_vector(source_oir_type)) {
                fail_vector_legalization(
                    "vector cast cannot change between bundled and single-piece shapes");
            }
            auto source = value_bundle(inst.source());
            auto &destination = bundle_result(inst);
            if (source.pieces.size() != destination.pieces.size()) {
                fail_vector_legalization("oversized vector cast source and result plans disagree");
            }
            for (std::size_t index = 0; index < destination.pieces.size(); ++index) {
                const auto result_type = *destination.pieces[index].vector_type;
                const auto source_type = *source.pieces[index].vector_type;
                if (inst.kind() == oir::VectorCastKind::Bitcast) {
                    if (result_type != source_type) {
                        fail_vector_legalization(
                            "oversized vector bitcast requires identical piece types");
                    }
                    emit_move(destination.pieces[index], source.pieces[index]);
                    continue;
                }
                if (inst.kind() == oir::VectorCastKind::ZExt) {
                    if (!source_type.is_mask() || result_type != data_config_for(source_type)) {
                        fail_vector_legalization("oversized vector zext supports mask to i32 only");
                    }
                    emit_move(destination.pieces[index],
                              emit_mask_zext(source_type, source.pieces[index]));
                    continue;
                }
                ensure_vector_configuration(result_type);
                mir::MachineVectorInfo info(result_type);
                info.avl = mir::MachineVectorAVL::current_vl();
                info.tail_policy = mir::VectorTailPolicy::Agnostic;
                info.mask_policy = mir::VectorMaskPolicy::Agnostic;
                bool uses_frm = false;
                mir::Opcode opcode;
                if (inst.kind() == oir::VectorCastKind::SIToFP) {
                    info.operation = mir::RVVOperation::ConvertSIToFP;
                    info.rounding = mir::VectorRoundingMode::Dynamic;
                    opcode = mir::Opcode::RVVSIToFP;
                    uses_frm = true;
                } else if (inst.kind() == oir::VectorCastKind::FPToSI) {
                    info.operation = mir::RVVOperation::ConvertFPToSI;
                    info.rounding = mir::VectorRoundingMode::RTZ;
                    opcode = mir::Opcode::RVVFPToSI;
                } else {
                    fail_vector_legalization("unsupported oversized fixed vector cast");
                }
                emit_vector_instruction(opcode,
                                        {mir::MachineOperand::reg_def(destination.pieces[index]),
                                         mir::MachineOperand::reg_use(source.pieces[index])},
                                        std::move(info), uses_frm);
            }
            return;
        }
        const auto result_type = legalize_vector_type(*result_oir_type);
        const auto source_type = legalize_vector_type(*source_oir_type);
        auto source = value_reg(inst.source());
        auto destination = value_regs_.at(&inst);
        if (inst.kind() == oir::VectorCastKind::Bitcast) {
            if (result_type != source_type) {
                fail_vector_legalization(
                    "vector bitcast requires identical machine representation");
            }
            emit_move(destination, std::move(source));
            return;
        }
        if (inst.kind() == oir::VectorCastKind::ZExt) {
            if (!source_type.is_mask() || result_type != data_config_for(source_type)) {
                fail_vector_legalization("vector zext currently supports fixed mask to i32 only");
            }
            emit_move(destination, emit_mask_zext(source_type, std::move(source)));
            return;
        }

        ensure_vector_configuration(result_type);
        mir::MachineVectorInfo info(result_type);
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        bool uses_frm = false;
        mir::Opcode opcode;
        if (inst.kind() == oir::VectorCastKind::SIToFP) {
            info.operation = mir::RVVOperation::ConvertSIToFP;
            info.rounding = mir::VectorRoundingMode::Dynamic;
            opcode = mir::Opcode::RVVSIToFP;
            uses_frm = true;
        } else if (inst.kind() == oir::VectorCastKind::FPToSI) {
            info.operation = mir::RVVOperation::ConvertFPToSI;
            info.rounding = mir::VectorRoundingMode::RTZ;
            opcode = mir::Opcode::RVVFPToSI;
        } else {
            fail_vector_legalization("unsupported fixed vector cast");
        }
        emit_vector_instruction(opcode,
                                {mir::MachineOperand::reg_def(destination),
                                 mir::MachineOperand::reg_use(std::move(source))},
                                std::move(info), uses_frm);
    }

    FixedPieceEVL lower_fixed_piece_evl(oir::Value *global_evl,
                                        const target::RVVFixedVectorPiece &piece) {
        if (global_evl == nullptr) {
            fail_vector_legalization("oversized fixed VP operation is missing EVL");
        }
        if (piece.lane_count == 0 ||
            piece.lane_base >
                static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
            piece.lane_count >
                static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            fail_vector_legalization("fixed piece EVL is outside the supported i32 lane domain");
        }

        FixedPieceEVL result;
        result.lane_base = piece.lane_base;
        result.lane_count = piece.lane_count;
        if (const auto *constant = dynamic_cast<const oir::ConstantInt *>(global_evl)) {
            if (constant->value() < 0) {
                fail_vector_legalization("vector EVL must be non-negative");
            }
            const auto global = static_cast<std::uint64_t>(constant->value());
            const auto remaining = global > piece.lane_base ? global - piece.lane_base : 0U;
            result.constant =
                static_cast<std::int64_t>(std::min<std::uint64_t>(remaining, piece.lane_count));
            return result;
        }

        // Clamp a signed runtime EVL to non-negative first.  The public OIR
        // type is i32, and the existing single-piece lowering defines a
        // negative runtime EVL as zero as well.
        auto requested = value_reg(global_evl);
        auto is_negative = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::Slt,
             {mir::MachineOperand::reg_def(is_negative), mir::MachineOperand::reg_use(requested),
              mir::MachineOperand::reg_use(zero_reg())});
        auto nonnegative_mask = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::AddIW,
             {mir::MachineOperand::reg_def(nonnegative_mask),
              mir::MachineOperand::reg_use(is_negative), mir::MachineOperand::imm(-1)});
        auto nonnegative = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::And,
             {mir::MachineOperand::reg_def(nonnegative), mir::MachineOperand::reg_use(requested),
              mir::MachineOperand::reg_use(nonnegative_mask)});

        auto lane_base = materialize_internal_i32(piece.lane_base);
        auto has_remaining = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::Sltu,
             {mir::MachineOperand::reg_def(has_remaining), mir::MachineOperand::reg_use(lane_base),
              mir::MachineOperand::reg_use(nonnegative)});
        auto remaining_mask = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::SubW, {mir::MachineOperand::reg_def(remaining_mask),
                                 mir::MachineOperand::reg_use(zero_reg()),
                                 mir::MachineOperand::reg_use(has_remaining)});
        auto difference = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::SubW,
             {mir::MachineOperand::reg_def(difference), mir::MachineOperand::reg_use(nonnegative),
              mir::MachineOperand::reg_use(lane_base)});
        auto remaining = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::And,
             {mir::MachineOperand::reg_def(remaining), mir::MachineOperand::reg_use(difference),
              mir::MachineOperand::reg_use(remaining_mask)});

        auto lane_count = materialize_internal_i32(piece.lane_count);
        auto below_limit = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::Sltu,
             {mir::MachineOperand::reg_def(below_limit), mir::MachineOperand::reg_use(remaining),
              mir::MachineOperand::reg_use(lane_count)});
        auto select_mask = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::SubW,
             {mir::MachineOperand::reg_def(select_mask), mir::MachineOperand::reg_use(zero_reg()),
              mir::MachineOperand::reg_use(below_limit)});
        auto excess = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::SubW,
             {mir::MachineOperand::reg_def(excess), mir::MachineOperand::reg_use(remaining),
              mir::MachineOperand::reg_use(lane_count)});
        auto selected = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::And,
             {mir::MachineOperand::reg_def(selected), mir::MachineOperand::reg_use(excess),
              mir::MachineOperand::reg_use(select_mask)});
        auto clamped = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::AddW,
             {mir::MachineOperand::reg_def(clamped), mir::MachineOperand::reg_use(lane_count),
              mir::MachineOperand::reg_use(selected)});
        result.reg = std::move(clamped);
        return result;
    }

    void
    emit_fixed_piece_setvli(const mir::MachineVectorType &piece_type, const FixedPieceEVL &evl,
                            mir::VectorTailPolicy tail_policy = mir::VectorTailPolicy::Agnostic,
                            mir::VectorMaskPolicy mask_policy = mir::VectorMaskPolicy::Agnostic) {
        const auto config = data_config_for(piece_type);
        if (!config.is_fixed() || config.fixed_lanes() != evl.lane_count) {
            fail_vector_legalization("piece EVL does not match its fixed machine-vector type");
        }
        current_execution_mask_source_.reset();
        std::vector<mir::MachineOperand> operands = {
            mir::MachineOperand::reg_def(create_vreg(mir::ValueType::I32))};
        if (evl.constant.has_value()) {
            if (*evl.constant <= 31) {
                operands.push_back(mir::MachineOperand::imm(*evl.constant));
            } else {
                operands.push_back(
                    mir::MachineOperand::reg_use(materialize_internal_i32(*evl.constant)));
            }
        } else if (evl.reg.has_value()) {
            operands.push_back(mir::MachineOperand::reg_use(*evl.reg));
        } else {
            fail_vector_legalization("piece EVL has no constant or SSA value");
        }
        operands.push_back(mir::MachineOperand::implicit_reg_def(vector_state_register("vl")));
        operands.push_back(mir::MachineOperand::implicit_reg_def(vector_state_register("vtype")));
        mir::MachineVectorInfo info(config);
        info.operation = mir::RVVOperation::SetVL;
        info.avl = mir::MachineVectorAVL::operand(1);
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        current_block_->add_instr(
            mir::MachineInstr(mir::Opcode::RVVSetVLI, std::move(operands), std::move(info)));
        // A PieceEVL is not represented by an OIR Value*.  Keep the active
        // state usable for masked emission, but deliberately make it miss the
        // ordinary OIR-AVL reuse test so another piece cannot alias this VL.
        current_vector_config_ = ActiveVectorConfiguration{config, tail_policy, mask_policy,
                                                           ActiveVLKind::Operand, nullptr};
    }

    mir::Register emit_fixed_piece_evl_prefix_mask(const mir::MachineVectorType &piece_type,
                                                   const FixedPieceEVL &evl) {
        const auto config = data_config_for(piece_type);
        if (evl.constant.has_value() && *evl.constant == 0) {
            return emit_mask_constant(config, false);
        }
        if (evl.constant.has_value() &&
            static_cast<std::uint64_t>(*evl.constant) >= evl.lane_count) {
            return emit_mask_constant(config, true);
        }
        ensure_vector_configuration(config);
        auto lane_ids = create_vector_vreg(config);
        mir::MachineVectorInfo step_info(config);
        step_info.operation = mir::RVVOperation::Step;
        step_info.avl = mir::MachineVectorAVL::current_vl();
        step_info.tail_policy = mir::VectorTailPolicy::Agnostic;
        step_info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(mir::Opcode::RVVStepTA, {mir::MachineOperand::reg_def(lane_ids)},
                                std::move(step_info));
        auto limit = emit_data_splat(
            config, evl.constant.has_value() ? materialize_internal_i32(*evl.constant) : *evl.reg);
        return emit_compare_mask(config, mir::RVVOperation::Lt, std::move(lane_ids),
                                 std::move(limit));
    }

    void lower_vp_compare(const oir::VPCmpInst &inst) {
        const auto *source_oir_type = dynamic_cast<const oir::VectorType *>(inst.lhs()->type());
        if (source_oir_type == nullptr) {
            fail_vector_legalization("VP compare operands are not vectors");
        }
        if (is_oversized_fixed_vector(source_oir_type)) {
            auto lhs = value_bundle(inst.lhs());
            auto rhs = value_bundle(inst.rhs());
            auto active = value_bundle(inst.active_mask());
            auto &destination = bundle_result(inst);
            const bool discardable = vp_passthrough_is_discardable(inst);
            std::optional<FixedVectorBundle> passthrough;
            if (!discardable) {
                passthrough = value_bundle(inst.passthrough());
            }
            if (lhs.pieces.size() != rhs.pieces.size() ||
                lhs.pieces.size() != active.pieces.size() ||
                lhs.pieces.size() != destination.pieces.size() ||
                (passthrough.has_value() && passthrough->pieces.size() != lhs.pieces.size())) {
                fail_vector_legalization(
                    "oversized VP compare operands have different chunk plans");
            }
            bool swap_operands = false;
            const auto operation = lower_compare_operation(inst.pred(), swap_operands);
            const auto tail_policy = lower_tail_policy(inst.tail_policy());
            const auto mask_policy = lower_mask_policy(inst.mask_policy());
            for (std::size_t index = 0; index < lhs.pieces.size(); ++index) {
                const auto piece_evl = lower_fixed_piece_evl(inst.evl(), lhs.plan.pieces[index]);
                auto left = lhs.pieces[index];
                auto right = rhs.pieces[index];
                if (swap_operands) {
                    std::swap(left, right);
                }
                const auto type = *left.vector_type;
                std::optional<mir::Register> tied;
                if (!discardable) {
                    tied = make_fresh_tied_passthrough(passthrough->pieces[index]);
                }
                emit_fixed_piece_setvli(type, piece_evl, tail_policy, mask_policy);
                auto execution_mask = prepare_execution_mask(active.pieces[index], type);
                mir::MachineVectorInfo info(type);
                info.operation = operation;
                info.avl = mir::MachineVectorAVL::current_vl();
                info.tail_policy = tail_policy;
                info.mask_policy = mask_policy;
                if (discardable) {
                    info.mask_operand = 3;
                    emit_vector_instruction(
                        mir::Opcode::RVVCompareVVTA,
                        {mir::MachineOperand::reg_def(destination.pieces[index]),
                         mir::MachineOperand::reg_use(left), mir::MachineOperand::reg_use(right),
                         mir::MachineOperand::reg_use(execution_mask)},
                        std::move(info));
                } else {
                    info.passthrough_operand = 1;
                    info.mask_operand = 4;
                    emit_vector_instruction(
                        mir::Opcode::RVVCompareVV,
                        {mir::MachineOperand::reg_def(destination.pieces[index]),
                         mir::MachineOperand::reg_use(*tied), mir::MachineOperand::reg_use(left),
                         mir::MachineOperand::reg_use(right),
                         mir::MachineOperand::reg_use(execution_mask)},
                        std::move(info));
                }
            }
            return;
        }
        const auto type = legalize_vector_type(*source_oir_type);
        if (type.is_mask()) {
            fail_vector_legalization(
                "VP mask comparison is unsupported; use mask logical operations");
        }
        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        const bool unmasked = vp_mask_is_all_true(inst);
        std::optional<mir::Register> ordinary_mask;
        if (!unmasked) {
            ordinary_mask = value_reg(inst.active_mask());
        }
        bool swap_operands = false;
        const auto operation = lower_compare_operation(inst.pred(), swap_operands);
        if (swap_operands) {
            std::swap(lhs, rhs);
        }
        const auto tail_policy = lower_tail_policy(inst.tail_policy());
        const auto mask_policy = lower_mask_policy(inst.mask_policy());
        const bool discardable = vp_passthrough_is_discardable(inst);
        std::optional<mir::Register> passthrough;
        if (!discardable) {
            passthrough = make_fresh_tied_passthrough(value_reg(inst.passthrough()));
        }
        ensure_vector_configuration(type, inst.evl(), tail_policy, mask_policy);
        mir::MachineVectorInfo info(type);
        info.operation = operation;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        if (discardable) {
            std::vector<mir::MachineOperand> operands = {
                mir::MachineOperand::reg_def(value_regs_.at(&inst)),
                mir::MachineOperand::reg_use(lhs), mir::MachineOperand::reg_use(rhs)};
            if (!unmasked) {
                auto execution_mask = prepare_execution_mask(*ordinary_mask, type);
                info.mask_operand = operands.size();
                operands.push_back(mir::MachineOperand::reg_use(execution_mask));
            }
            emit_vector_instruction(mir::Opcode::RVVCompareVVTA,
                                    std::move(operands), std::move(info));
            return;
        }
        info.passthrough_operand = 1;
        std::vector<mir::MachineOperand> operands = {
            mir::MachineOperand::reg_def(value_regs_.at(&inst)),
            mir::MachineOperand::reg_use(*passthrough),
            mir::MachineOperand::reg_use(lhs), mir::MachineOperand::reg_use(rhs)};
        if (!unmasked) {
            auto execution_mask = prepare_execution_mask(*ordinary_mask, type);
            info.mask_operand = operands.size();
            operands.push_back(mir::MachineOperand::reg_use(execution_mask));
        }
        emit_vector_instruction(mir::Opcode::RVVCompareVV, std::move(operands),
                                std::move(info));
    }

    void lower_vp_load(const oir::VPLoadInst &inst) {
        if (is_oversized_fixed_vector(inst.type())) {
            auto &destination = bundle_result(inst);
            auto active = value_bundle(inst.active_mask());
            const bool discardable = vp_passthrough_is_discardable(inst);
            std::optional<FixedVectorBundle> passthrough;
            if (!discardable) {
                passthrough = value_bundle(inst.passthrough());
            }
            if (active.pieces.size() != destination.pieces.size() ||
                (passthrough.has_value() &&
                 passthrough->pieces.size() != destination.pieces.size())) {
                fail_vector_legalization("oversized VP load mask/passthrough chunk plan mismatch");
            }
            const auto tail_policy = lower_tail_policy(inst.tail_policy());
            const auto mask_policy = lower_mask_policy(inst.mask_policy());
            auto base = value_reg(inst.ptr());
            for (std::size_t index = 0; index < destination.pieces.size(); ++index) {
                const auto piece_evl =
                    lower_fixed_piece_evl(inst.evl(), destination.plan.pieces[index]);
                const auto type = *destination.pieces[index].vector_type;
                std::optional<mir::Register> tied;
                if (!discardable) {
                    tied = make_fresh_tied_passthrough(passthrough->pieces[index]);
                }
                emit_fixed_piece_setvli(type, piece_evl, tail_policy, mask_policy);
                auto mask = prepare_execution_mask(active.pieces[index], type);
                auto address = address_with_offset(
                    base, bundle_piece_byte_offset(destination.plan.pieces[index]));
                mir::MachineVectorInfo info(data_config_for(type));
                info.operation = mir::RVVOperation::Load;
                info.avl = mir::MachineVectorAVL::current_vl();
                info.tail_policy = tail_policy;
                info.mask_policy = mask_policy;
                if (discardable) {
                    info.mask_operand = 2;
                    emit_vector_instruction(
                        type.is_mask() ? mir::Opcode::RVVMaskLoad : mir::Opcode::RVVLoadUnitTA,
                        {mir::MachineOperand::reg_def(destination.pieces[index]),
                         mir::MachineOperand::reg_use(address), mir::MachineOperand::reg_use(mask)},
                        std::move(info));
                } else {
                    if (type.is_mask()) {
                        fail_vector_legalization("oversized masked mask-load with undisturbed "
                                                 "passthrough is not representable by RVV vlm.v");
                    }
                    info.passthrough_operand = 1;
                    info.mask_operand = 3;
                    emit_vector_instruction(
                        mir::Opcode::RVVLoadUnit,
                        {mir::MachineOperand::reg_def(destination.pieces[index]),
                         mir::MachineOperand::reg_use(*tied), mir::MachineOperand::reg_use(address),
                         mir::MachineOperand::reg_use(mask)},
                        std::move(info));
                }
            }
            return;
        }
        auto type = legalize_vector_type(*static_cast<oir::VectorType *>(inst.type()));
        auto dst = value_regs_.at(&inst);
        auto address = value_reg(inst.ptr());
        const bool unmasked = vp_mask_is_all_true(inst);
        std::optional<mir::Register> ordinary_mask;
        if (!unmasked) {
            ordinary_mask = value_reg(inst.active_mask());
        }
        const auto tail_policy = lower_tail_policy(inst.tail_policy());
        const auto mask_policy = lower_mask_policy(inst.mask_policy());
        std::optional<mir::Register> passthrough;
        const bool discardable = vp_passthrough_is_discardable(inst);
        if (!discardable) {
            passthrough = make_fresh_tied_passthrough(value_reg(inst.passthrough()));
        }
        ensure_vector_configuration(type, inst.evl(), tail_policy, mask_policy);
        mir::MachineVectorInfo info(type);
        info.operation = mir::RVVOperation::Load;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        if (discardable) {
            if (unmasked) {
                emit_vector_instruction(mir::Opcode::RVVLoadUnitTA,
                                        {mir::MachineOperand::reg_def(dst),
                                         mir::MachineOperand::reg_use(address)},
                                        std::move(info));
                return;
            }
            auto mask = prepare_execution_mask(*ordinary_mask, type);
            info.mask_operand = 2;
            emit_vector_instruction(mir::Opcode::RVVLoadUnitTA,
                                    {mir::MachineOperand::reg_def(dst),
                                     mir::MachineOperand::reg_use(address),
                                     mir::MachineOperand::reg_use(mask)},
                                    std::move(info));
        } else {
            std::optional<mir::Register> mask;
            if (!unmasked) {
                mask = prepare_execution_mask(*ordinary_mask, type);
            }
            info.passthrough_operand = 1;
            std::vector<mir::MachineOperand> operands = {
                mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(*passthrough),
                mir::MachineOperand::reg_use(address)};
            if (mask.has_value()) {
                info.mask_operand = operands.size();
                operands.push_back(mir::MachineOperand::reg_use(*mask));
            }
            emit_vector_instruction(mir::Opcode::RVVLoadUnit, std::move(operands),
                                    std::move(info));
        }
    }

    void lower_vp_store(const oir::VPStoreInst &inst) {
        auto *vector_type = dynamic_cast<oir::VectorType *>(inst.value()->type());
        if (vector_type == nullptr) {
            fail_vector_legalization("VP store value is not a vector");
        }
        if (is_oversized_fixed_vector(vector_type)) {
            auto value = value_bundle(inst.value());
            auto active = value_bundle(inst.active_mask());
            if (value.pieces.size() != active.pieces.size()) {
                fail_vector_legalization("oversized VP store mask chunk plan mismatch");
            }
            const auto tail_policy = lower_tail_policy(inst.tail_policy());
            const auto mask_policy = lower_mask_policy(inst.mask_policy());
            auto base = value_reg(inst.ptr());
            for (std::size_t index = 0; index < value.pieces.size(); ++index) {
                const auto piece_evl = lower_fixed_piece_evl(inst.evl(), value.plan.pieces[index]);
                const auto type = *value.pieces[index].vector_type;
                emit_fixed_piece_setvli(type, piece_evl, tail_policy, mask_policy);
                auto mask = prepare_execution_mask(active.pieces[index], type);
                auto address =
                    address_with_offset(base, bundle_piece_byte_offset(value.plan.pieces[index]));
                mir::MachineVectorInfo info(data_config_for(type));
                info.operation = mir::RVVOperation::Store;
                info.avl = mir::MachineVectorAVL::current_vl();
                info.tail_policy = tail_policy;
                info.mask_policy = mask_policy;
                info.mask_operand = 2;
                if (type.is_mask()) {
                    fail_vector_legalization("oversized masked mask-store is unsupported; use an "
                                             "ordinary packed mask store");
                }
                emit_vector_instruction(mir::Opcode::RVVStoreUnit,
                                        {mir::MachineOperand::reg_use(value.pieces[index]),
                                         mir::MachineOperand::reg_use(address),
                                         mir::MachineOperand::reg_use(mask)},
                                        std::move(info));
            }
            return;
        }
        auto type = legalize_vector_type(*vector_type);
        auto value = value_reg(inst.value());
        auto address = value_reg(inst.ptr());
        const bool unmasked = vp_mask_is_all_true(inst);
        std::optional<mir::Register> ordinary_mask;
        if (!unmasked) {
            ordinary_mask = value_reg(inst.active_mask());
        }
        const auto tail_policy = lower_tail_policy(inst.tail_policy());
        const auto mask_policy = lower_mask_policy(inst.mask_policy());
        ensure_vector_configuration(type, inst.evl(), tail_policy, mask_policy);
        mir::MachineVectorInfo info(type);
        info.operation = mir::RVVOperation::Store;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        std::vector<mir::MachineOperand> operands = {mir::MachineOperand::reg_use(value),
                                                     mir::MachineOperand::reg_use(address)};
        if (!unmasked) {
            auto mask = prepare_execution_mask(*ordinary_mask, type);
            info.mask_operand = operands.size();
            operands.push_back(mir::MachineOperand::reg_use(mask));
        }
        emit_vector_instruction(mir::Opcode::RVVStoreUnit, std::move(operands), std::move(info));
    }

    mir::Register emit_e32_byte_offsets(const mir::MachineVectorType &index_type,
                                        mir::Register indices, oir::Value *evl) {
        if (index_type.is_mask() || index_type.element_type() != mir::ValueType::I32 ||
            index_type.sew_bits() != 32) {
            fail_vector_legalization("indexed memory requires an e32 integer index vector");
        }
        // OIR indexes elements; RVV indexed memory consumes unsigned byte
        // offsets.  The caller has proved non-negativity and no u32 overflow.
        // Build the scale at full width, then restore the exact VP EVL before
        // multiplying so lanes outside EVL remain unobserved.
        auto scale = emit_data_splat(index_type, materialize_internal_i32(4));
        ensure_vector_configuration(index_type, evl);
        auto result = create_vector_vreg(index_type);
        mir::MachineVectorInfo info(index_type);
        info.operation = mir::RVVOperation::Mul;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(mir::Opcode::RVVIntBinaryVVTA,
                                {mir::MachineOperand::reg_def(result),
                                 mir::MachineOperand::reg_use(std::move(indices)),
                                 mir::MachineOperand::reg_use(std::move(scale))},
                                std::move(info));
        return result;
    }

    mir::Register emit_e32_byte_offsets(const mir::MachineVectorType &index_type,
                                        mir::Register indices, const FixedPieceEVL &evl) {
        if (index_type.is_mask() || index_type.element_type() != mir::ValueType::I32 ||
            index_type.sew_bits() != 32) {
            fail_vector_legalization("indexed memory requires an e32 integer index vector");
        }
        auto scale = emit_data_splat(index_type, materialize_internal_i32(4));
        emit_fixed_piece_setvli(index_type, evl);
        auto result = create_vector_vreg(index_type);
        mir::MachineVectorInfo info(index_type);
        info.operation = mir::RVVOperation::Mul;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(mir::Opcode::RVVIntBinaryVVTA,
                                {mir::MachineOperand::reg_def(result),
                                 mir::MachineOperand::reg_use(std::move(indices)),
                                 mir::MachineOperand::reg_use(std::move(scale))},
                                std::move(info));
        return result;
    }

    void emit_clear_enumerated_lane(const mir::MachineVectorType &index_type,
                                    mir::Register work_mask, mir::Register lane_ids,
                                    mir::Register lane_index,
                                    const mir::MachineBasicBlock &header) {
        auto index_splat = emit_data_splat(index_type, lane_index);
        auto one_hot =
            emit_compare_mask(index_type, mir::RVVOperation::Eq, lane_ids, std::move(index_splat));
        auto remaining = emit_mask_logical(index_type, mir::RVVOperation::MaskXor, work_mask,
                                           std::move(one_hot));
        emit_move(work_mask, std::move(remaining));
        emit(mir::Opcode::Jump, {mir::MachineOperand::block(header.name())});
    }

    void emit_ordinary_mask_copy(const mir::MachineVectorType &config, mir::Register destination,
                                 mir::Register source) {
        ensure_vector_configuration(config);
        mir::MachineVectorInfo info(config);
        info.operation = mir::RVVOperation::Copy;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(mir::Opcode::RVVMaskCopy,
                                {mir::MachineOperand::reg_def(std::move(destination)),
                                 mir::MachineOperand::reg_use(std::move(source))},
                                std::move(info));
    }

    void lower_vp_gather_scalar_fallback(const oir::VPGatherInst &inst,
                                         const mir::MachineVectorType &type,
                                         const mir::MachineVectorType &index_type,
                                         mir::Register destination, mir::Register passthrough,
                                         mir::Register indices, mir::Register address,
                                         mir::Register active,
                                         const FixedPieceEVL *piece_evl = nullptr) {
        emit_move(destination, std::move(passthrough));
        auto work_mask = create_vector_vreg(mir::MachineVectorType::mask_for(type));
        emit_ordinary_mask_copy(type, work_mask, std::move(active));
        ensure_vector_configuration(index_type);
        auto lane_ids = create_vector_vreg(index_type);
        mir::MachineVectorInfo step_info(index_type);
        step_info.operation = mir::RVVOperation::Step;
        step_info.avl = mir::MachineVectorAVL::current_vl();
        step_info.tail_policy = mir::VectorTailPolicy::Agnostic;
        step_info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(mir::Opcode::RVVStepTA, {mir::MachineOperand::reg_def(lane_ids)},
                                std::move(step_info));

        const auto suffix = std::to_string(temp_index_++);
        auto *header = current_function_->create_block("rvv.gather.fallback.header." + suffix);
        auto *body = current_function_->create_block("rvv.gather.fallback.body." + suffix);
        auto *clear = current_function_->create_block("rvv.gather.fallback.clear." + suffix);
        auto *done = current_function_->create_block("rvv.gather.fallback.done." + suffix);
        emit(mir::Opcode::Jump, {mir::MachineOperand::block(header->name())});

        const auto tail_policy = lower_tail_policy(inst.tail_policy());
        const auto mask_policy = lower_mask_policy(inst.mask_policy());
        enter_synthetic_vector_block(header, type, inst.evl());
        if (piece_evl != nullptr) {
            emit_fixed_piece_setvli(type, *piece_evl, tail_policy, mask_policy);
        } else {
            auto ignored_vl = create_vreg(mir::ValueType::I32);
            emit_setvli(type, ignored_vl, inst.evl(), tail_policy, mask_policy);
        }
        auto lane_index = create_vreg(mir::ValueType::I32);
        mir::MachineVectorInfo first_info(type);
        first_info.operation = mir::RVVOperation::MaskFirst;
        first_info.avl = mir::MachineVectorAVL::current_vl();
        first_info.tail_policy = tail_policy;
        first_info.mask_policy = mask_policy;
        emit_vector_instruction(
            mir::Opcode::RVVMaskFirst,
            {mir::MachineOperand::reg_def(lane_index), mir::MachineOperand::reg_use(work_mask)},
            std::move(first_info));
        emit(mir::Opcode::BranchLT,
             {mir::MachineOperand::reg_use(lane_index), mir::MachineOperand::reg_use(zero_reg()),
              mir::MachineOperand::block(done->name())});
        emit(mir::Opcode::Jump, {mir::MachineOperand::block(body->name())});

        enter_synthetic_vector_block(body, index_type, inst.evl());
        auto element_index = create_vreg(mir::ValueType::I32);
        emit_extract_data(element_index, index_type, indices, lane_index, std::nullopt);
        auto byte_offset = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::SllI,
             {mir::MachineOperand::reg_def(byte_offset),
              mir::MachineOperand::reg_use(element_index), mir::MachineOperand::imm(2)});
        auto lane_address = create_vreg(mir::ValueType::Ptr);
        emit(mir::Opcode::Add,
             {mir::MachineOperand::reg_def(lane_address), mir::MachineOperand::reg_use(address),
              mir::MachineOperand::reg_use(byte_offset)});
        enter_synthetic_vector_block(body, type, inst.evl());
        auto lane_value = create_vreg(type.element_type());
        emit(mir::Opcode::LoadMem,
             {mir::MachineOperand::reg_def(lane_value), mir::MachineOperand::reg_use(lane_address),
              mir::MachineOperand::type(type.element_type())});
        auto updated = create_vector_vreg(type);
        emit_insert_data(updated, type, destination, lane_value, lane_index);
        emit_move(destination, std::move(updated));
        emit(mir::Opcode::Jump, {mir::MachineOperand::block(clear->name())});

        enter_synthetic_vector_block(clear, index_type, inst.evl());
        emit_clear_enumerated_lane(index_type, work_mask, lane_ids, lane_index, *header);
        enter_synthetic_vector_block(done, type, inst.evl());
    }

    void lower_vp_scatter_scalar_fallback(const oir::VPScatterInst &inst,
                                          const mir::MachineVectorType &type,
                                          const mir::MachineVectorType &index_type,
                                          mir::Register values, mir::Register indices,
                                          mir::Register address, mir::Register active,
                                          const FixedPieceEVL *piece_evl = nullptr) {
        auto work_mask = create_vector_vreg(mir::MachineVectorType::mask_for(type));
        emit_ordinary_mask_copy(type, work_mask, std::move(active));
        ensure_vector_configuration(index_type);
        auto lane_ids = create_vector_vreg(index_type);
        mir::MachineVectorInfo step_info(index_type);
        step_info.operation = mir::RVVOperation::Step;
        step_info.avl = mir::MachineVectorAVL::current_vl();
        step_info.tail_policy = mir::VectorTailPolicy::Agnostic;
        step_info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(mir::Opcode::RVVStepTA, {mir::MachineOperand::reg_def(lane_ids)},
                                std::move(step_info));

        const auto suffix = std::to_string(temp_index_++);
        auto *header = current_function_->create_block("rvv.scatter.fallback.header." + suffix);
        auto *body = current_function_->create_block("rvv.scatter.fallback.body." + suffix);
        auto *clear = current_function_->create_block("rvv.scatter.fallback.clear." + suffix);
        auto *done = current_function_->create_block("rvv.scatter.fallback.done." + suffix);
        emit(mir::Opcode::Jump, {mir::MachineOperand::block(header->name())});

        const auto tail_policy = lower_tail_policy(inst.tail_policy());
        const auto mask_policy = lower_mask_policy(inst.mask_policy());
        enter_synthetic_vector_block(header, type, inst.evl());
        if (piece_evl != nullptr) {
            emit_fixed_piece_setvli(type, *piece_evl, tail_policy, mask_policy);
        } else {
            auto ignored_vl = create_vreg(mir::ValueType::I32);
            emit_setvli(type, ignored_vl, inst.evl(), tail_policy, mask_policy);
        }
        auto lane_index = create_vreg(mir::ValueType::I32);
        mir::MachineVectorInfo first_info(type);
        first_info.operation = mir::RVVOperation::MaskFirst;
        first_info.avl = mir::MachineVectorAVL::current_vl();
        first_info.tail_policy = tail_policy;
        first_info.mask_policy = mask_policy;
        emit_vector_instruction(
            mir::Opcode::RVVMaskFirst,
            {mir::MachineOperand::reg_def(lane_index), mir::MachineOperand::reg_use(work_mask)},
            std::move(first_info));
        emit(mir::Opcode::BranchLT,
             {mir::MachineOperand::reg_use(lane_index), mir::MachineOperand::reg_use(zero_reg()),
              mir::MachineOperand::block(done->name())});
        emit(mir::Opcode::Jump, {mir::MachineOperand::block(body->name())});

        enter_synthetic_vector_block(body, index_type, inst.evl());
        auto element_index = create_vreg(mir::ValueType::I32);
        emit_extract_data(element_index, index_type, indices, lane_index, std::nullopt);
        enter_synthetic_vector_block(body, type, inst.evl());
        auto lane_value = create_vreg(type.element_type());
        emit_extract_data(lane_value, type, values, lane_index, std::nullopt);
        auto byte_offset = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::SllI,
             {mir::MachineOperand::reg_def(byte_offset),
              mir::MachineOperand::reg_use(element_index), mir::MachineOperand::imm(2)});
        auto lane_address = create_vreg(mir::ValueType::Ptr);
        emit(mir::Opcode::Add,
             {mir::MachineOperand::reg_def(lane_address), mir::MachineOperand::reg_use(address),
              mir::MachineOperand::reg_use(byte_offset)});
        emit(mir::Opcode::StoreMem,
             {mir::MachineOperand::reg_use(lane_address), mir::MachineOperand::reg_use(lane_value),
              mir::MachineOperand::type(type.element_type())});
        emit(mir::Opcode::Jump, {mir::MachineOperand::block(clear->name())});

        enter_synthetic_vector_block(clear, index_type, inst.evl());
        emit_clear_enumerated_lane(index_type, work_mask, lane_ids, lane_index, *header);
        enter_synthetic_vector_block(done, type, inst.evl());
    }

    std::optional<std::pair<mir::Register, mir::Register>>
    segment2_tuple_registers(const mir::MachineVectorType &type) const {
        const auto width = type.register_group_width();
        if (type.is_mask() || type.lmul_eighths() * 2U > 64U || width * 2U > 8U) {
            return std::nullopt;
        }
        // v24-v31 are the allocator's dedicated high-register scratch bank.
        // Keeping the complete tuple there makes field adjacency explicit to
        // MIR and leaves ordinary SSA results independently allocatable.
        auto field0 = mir::Register::physical_vector("v24", mir::RegisterClass::VR, type);
        auto field1 = mir::Register::physical_vector(
            "v" + std::to_string(24U + width), mir::RegisterClass::VR, type);
        return std::make_pair(std::move(field0), std::move(field1));
    }

    void emit_segment2_guarded_copy(mir::Register destination, mir::Register source,
                                    mir::Register other_tuple_field,
                                    const mir::MachineVectorType &type) {
        mir::MachineVectorInfo info(type);
        info.operation = mir::RVVOperation::Copy;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(
            mir::Opcode::RVVVectorCopy,
            {mir::MachineOperand::reg_def(std::move(destination)),
             mir::MachineOperand::reg_use(std::move(source)),
             mir::MachineOperand::implicit_reg_use(std::move(other_tuple_field))},
            std::move(info));
    }

    bool try_lower_segment2_load(const oir::VPGatherInst &field0) {
        const auto candidate = segment2_candidates_.find(&field0);
        if (candidate == segment2_candidates_.end()) {
            return false;
        }
        const auto *field1 = dynamic_cast<const oir::VPGatherInst *>(candidate->second);
        const auto plan = strided_index_plan(field0.indices(), field0.active_mask(), field0.evl());
        if (field1 == nullptr || !plan.has_value() || plan->stride != 2) {
            return false;
        }
        const auto *oir_type = dynamic_cast<const oir::VectorType *>(field0.type());
        if (oir_type == nullptr) {
            return false;
        }
        const auto type = legalize_vector_type(*oir_type);
        const auto tuple = segment2_tuple_registers(type);
        if (!tuple.has_value()) {
            return false;
        }

        auto address = materialize_strided_address(value_reg(field0.base_ptr()), *plan);
        auto ordinary_mask = value_reg(field0.active_mask());
        ensure_vector_configuration(type, field0.evl(), mir::VectorTailPolicy::Agnostic,
                                    mir::VectorMaskPolicy::Agnostic);
        auto execution_mask = prepare_execution_mask(std::move(ordinary_mask), type);
        mir::MachineVectorInfo info(type);
        info.operation = mir::RVVOperation::Load;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        info.mask_operand = 3;
        emit_vector_instruction(
            mir::Opcode::RVVLoadSegment2,
            {mir::MachineOperand::reg_def(tuple->first),
             mir::MachineOperand::reg_def(tuple->second),
             mir::MachineOperand::reg_use(std::move(address)),
             mir::MachineOperand::reg_use(std::move(execution_mask))},
            std::move(info));
        emit_segment2_guarded_copy(value_regs_.at(&field0), tuple->first, tuple->second, type);
        emit_segment2_guarded_copy(value_regs_.at(field1), tuple->second, tuple->first, type);
        consumed_segment2_fields_.insert(field1);
        return true;
    }

    bool try_lower_segment2_store(const oir::VPScatterInst &field0) {
        const auto candidate = segment2_candidates_.find(&field0);
        if (candidate == segment2_candidates_.end()) {
            return false;
        }
        const auto *field1 = dynamic_cast<const oir::VPScatterInst *>(candidate->second);
        const auto plan = strided_index_plan(field0.indices(), field0.active_mask(), field0.evl());
        if (field1 == nullptr || !plan.has_value() || plan->stride != 2) {
            return false;
        }
        const auto *oir_type = dynamic_cast<const oir::VectorType *>(field0.value()->type());
        if (oir_type == nullptr) {
            return false;
        }
        const auto type = legalize_vector_type(*oir_type);
        const auto tuple = segment2_tuple_registers(type);
        if (!tuple.has_value()) {
            return false;
        }

        // Materialize both values and the ordinary mask before establishing
        // the segment EVL; constants are permitted to change VL while loaded.
        auto value0 = value_reg(field0.value());
        auto value1 = value_reg(field1->value());
        auto address = materialize_strided_address(value_reg(field0.base_ptr()), *plan);
        auto ordinary_mask = value_reg(field0.active_mask());
        ensure_vector_configuration(type, field0.evl(), mir::VectorTailPolicy::Agnostic,
                                    mir::VectorMaskPolicy::Agnostic);
        emit_segment2_guarded_copy(tuple->first, std::move(value0), tuple->second, type);
        emit_segment2_guarded_copy(tuple->second, std::move(value1), tuple->first, type);
        auto execution_mask = prepare_execution_mask(std::move(ordinary_mask), type);
        mir::MachineVectorInfo info(type);
        info.operation = mir::RVVOperation::Store;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        info.mask_operand = 3;
        emit_vector_instruction(
            mir::Opcode::RVVStoreSegment2,
            {mir::MachineOperand::reg_use(tuple->first),
             mir::MachineOperand::reg_use(tuple->second),
             mir::MachineOperand::reg_use(std::move(address)),
             mir::MachineOperand::reg_use(std::move(execution_mask))},
            std::move(info));
        consumed_segment2_fields_.insert(field1);
        return true;
    }

    void lower_vp_gather(const oir::VPGatherInst &inst) {
        const auto *result_oir_type = dynamic_cast<const oir::VectorType *>(inst.type());
        const auto *index_oir_type = dynamic_cast<const oir::VectorType *>(inst.indices()->type());
        if (result_oir_type == nullptr || index_oir_type == nullptr) {
            fail_vector_legalization("VP gather requires typed data and index vectors");
        }
        const bool oversized_data = is_oversized_fixed_vector(result_oir_type);
        const bool oversized_indices = is_oversized_fixed_vector(index_oir_type);
        if (oversized_data != oversized_indices) {
            fail_vector_legalization(
                "oversized VP gather data/index shapes have different chunk plans");
        }
        if (oversized_data) {
            if (result_oir_type->is_mask()) {
                fail_vector_legalization("VP gather of packed masks is unsupported");
            }
            auto &destination = bundle_result(inst);
            auto indices = value_bundle(inst.indices());
            auto active = value_bundle(inst.active_mask());
            auto passthrough = value_bundle(inst.passthrough());
            if (destination.pieces.size() != indices.pieces.size() ||
                destination.pieces.size() != active.pieces.size() ||
                destination.pieces.size() != passthrough.pieces.size()) {
                fail_vector_legalization("oversized VP gather operands have different chunk plans");
            }
            const bool native_offsets =
                indices_are_safe_e32_byte_offsets(inst.indices(), inst.active_mask(), inst.evl());
            const auto strided =
                strided_index_plan(inst.indices(), inst.active_mask(), inst.evl(),
                                   true);
            auto address = value_reg(inst.base_ptr());
            const auto strided_byte_stride =
                strided.has_value()
                    ? std::optional<mir::Register>{materialize_strided_byte_stride(*strided)}
                    : std::nullopt;
            const auto tail_policy = lower_tail_policy(inst.tail_policy());
            const auto mask_policy = lower_mask_policy(inst.mask_policy());
            for (std::size_t index = 0; index < destination.pieces.size(); ++index) {
                const auto &plan_piece = destination.plan.pieces[index];
                if (indices.plan.pieces[index].lane_base != plan_piece.lane_base ||
                    indices.plan.pieces[index].lane_count != plan_piece.lane_count ||
                    active.plan.pieces[index].lane_base != plan_piece.lane_base ||
                    active.plan.pieces[index].lane_count != plan_piece.lane_count) {
                    fail_vector_legalization(
                        "oversized VP gather data/index/mask lanes are not aligned");
                }
                const auto piece_evl = lower_fixed_piece_evl(inst.evl(), plan_piece);
                const auto type = *destination.pieces[index].vector_type;
                const auto index_type = *indices.pieces[index].vector_type;
                if (strided.has_value()) {
                    auto piece_address =
                        materialize_strided_address(address, *strided, plan_piece.lane_base);
                    auto tied = make_fresh_tied_passthrough(passthrough.pieces[index]);
                    emit_fixed_piece_setvli(type, piece_evl, tail_policy, mask_policy);
                    auto execution_mask = prepare_execution_mask(active.pieces[index], type);
                    mir::MachineVectorInfo info(type);
                    info.operation = mir::RVVOperation::Load;
                    info.avl = mir::MachineVectorAVL::current_vl();
                    info.tail_policy = tail_policy;
                    info.mask_policy = mask_policy;
                    info.passthrough_operand = 1;
                    info.mask_operand = 4;
                    emit_vector_instruction(
                        mir::Opcode::RVVLoadStrided,
                        {mir::MachineOperand::reg_def(destination.pieces[index]),
                         mir::MachineOperand::reg_use(std::move(tied)),
                         mir::MachineOperand::reg_use(std::move(piece_address)),
                         mir::MachineOperand::reg_use(*strided_byte_stride),
                         mir::MachineOperand::reg_use(std::move(execution_mask))},
                        std::move(info));
                    continue;
                }
                if (!native_offsets) {
                    lower_vp_gather_scalar_fallback(
                        inst, type, index_type, destination.pieces[index],
                        passthrough.pieces[index], indices.pieces[index], address,
                        active.pieces[index], &piece_evl);
                    continue;
                }
                auto byte_offsets =
                    emit_e32_byte_offsets(index_type, indices.pieces[index], piece_evl);
                auto tied = make_fresh_tied_passthrough(passthrough.pieces[index]);
                emit_fixed_piece_setvli(type, piece_evl, tail_policy, mask_policy);
                auto execution_mask = prepare_execution_mask(active.pieces[index], type);
                mir::MachineVectorInfo info(type);
                info.operation = mir::RVVOperation::Load;
                info.avl = mir::MachineVectorAVL::current_vl();
                info.tail_policy = tail_policy;
                info.mask_policy = mask_policy;
                info.index_vector_type = index_type;
                info.passthrough_operand = 1;
                info.mask_operand = 4;
                emit_vector_instruction(mir::Opcode::RVVLoadIndexed,
                                        {mir::MachineOperand::reg_def(destination.pieces[index]),
                                         mir::MachineOperand::reg_use(std::move(tied)),
                                         mir::MachineOperand::reg_use(address),
                                         mir::MachineOperand::reg_use(std::move(byte_offsets)),
                                         mir::MachineOperand::reg_use(std::move(execution_mask))},
                                        std::move(info));
            }
            return;
        }
        const auto type = legalize_vector_type(*result_oir_type);
        const auto index_type = legalize_vector_type(*index_oir_type);
        if (type.is_mask()) {
            fail_vector_legalization("VP gather of packed masks is unsupported");
        }
        const auto strided =
            strided_index_plan(inst.indices(), inst.active_mask(), inst.evl(), true);
        if (strided.has_value()) {
            auto address = materialize_strided_address(value_reg(inst.base_ptr()), *strided);
            auto byte_stride = materialize_strided_byte_stride(*strided);
            const auto tail_policy = lower_tail_policy(inst.tail_policy());
            const auto mask_policy = lower_mask_policy(inst.mask_policy());
            const bool discardable = vp_passthrough_is_discardable(inst);
            std::optional<mir::Register> passthrough;
            if (!discardable) {
                passthrough = make_fresh_tied_passthrough(
                    value_reg(inst.passthrough()));
            }
            ensure_vector_configuration(type, inst.evl(), tail_policy, mask_policy);
            auto execution_mask = prepare_execution_mask(value_reg(inst.active_mask()), type);
            mir::MachineVectorInfo info(type);
            info.operation = mir::RVVOperation::Load;
            info.avl = mir::MachineVectorAVL::current_vl();
            info.tail_policy = tail_policy;
            info.mask_policy = mask_policy;
            if (discardable) {
                info.mask_operand = 3;
                emit_vector_instruction(
                    mir::Opcode::RVVLoadStridedTA,
                    {mir::MachineOperand::reg_def(value_regs_.at(&inst)),
                     mir::MachineOperand::reg_use(std::move(address)),
                     mir::MachineOperand::reg_use(std::move(byte_stride)),
                     mir::MachineOperand::reg_use(std::move(execution_mask))},
                    std::move(info));
                return;
            }
            info.passthrough_operand = 1;
            info.mask_operand = 4;
            emit_vector_instruction(
                mir::Opcode::RVVLoadStrided,
                {mir::MachineOperand::reg_def(value_regs_.at(&inst)),
                 mir::MachineOperand::reg_use(*passthrough),
                 mir::MachineOperand::reg_use(std::move(address)),
                 mir::MachineOperand::reg_use(std::move(byte_stride)),
                 mir::MachineOperand::reg_use(std::move(execution_mask))},
                std::move(info));
            return;
        }
        if (!indices_are_safe_e32_byte_offsets(inst.indices(), inst.active_mask(), inst.evl())) {
            lower_vp_gather_scalar_fallback(inst, type, index_type, value_regs_.at(&inst),
                                            value_reg(inst.passthrough()),
                                            value_reg(inst.indices()), value_reg(inst.base_ptr()),
                                            value_reg(inst.active_mask()));
            return;
        }

        auto address = value_reg(inst.base_ptr());
        auto ordinary_mask = value_reg(inst.active_mask());
        auto byte_offsets =
            emit_e32_byte_offsets(index_type, value_reg(inst.indices()), inst.evl());
        // The indexed pseudo is destructive for TU/MU.  A private full-width
        // copy is also safe for agnostic policies and keeps one fail-closed
        // descriptor shape through RA and expansion.
        auto passthrough = make_fresh_tied_passthrough(value_reg(inst.passthrough()));
        const auto tail_policy = lower_tail_policy(inst.tail_policy());
        const auto mask_policy = lower_mask_policy(inst.mask_policy());
        ensure_vector_configuration(type, inst.evl(), tail_policy, mask_policy);
        auto execution_mask = prepare_execution_mask(std::move(ordinary_mask), type);
        mir::MachineVectorInfo info(type);
        info.operation = mir::RVVOperation::Load;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        info.index_vector_type = index_type;
        info.passthrough_operand = 1;
        info.mask_operand = 4;
        emit_vector_instruction(mir::Opcode::RVVLoadIndexed,
                                {mir::MachineOperand::reg_def(value_regs_.at(&inst)),
                                 mir::MachineOperand::reg_use(std::move(passthrough)),
                                 mir::MachineOperand::reg_use(std::move(address)),
                                 mir::MachineOperand::reg_use(std::move(byte_offsets)),
                                 mir::MachineOperand::reg_use(std::move(execution_mask))},
                                std::move(info));
    }

    void lower_vp_scatter(const oir::VPScatterInst &inst) {
        const auto *value_oir_type = dynamic_cast<const oir::VectorType *>(inst.value()->type());
        const auto *index_oir_type = dynamic_cast<const oir::VectorType *>(inst.indices()->type());
        if (value_oir_type == nullptr || index_oir_type == nullptr) {
            fail_vector_legalization("VP scatter requires typed data and index vectors");
        }
        const bool oversized_data = is_oversized_fixed_vector(value_oir_type);
        const bool oversized_indices = is_oversized_fixed_vector(index_oir_type);
        if (oversized_data != oversized_indices) {
            fail_vector_legalization(
                "oversized VP scatter data/index shapes have different chunk plans");
        }
        if (oversized_data) {
            if (value_oir_type->is_mask()) {
                fail_vector_legalization("VP scatter of packed masks is unsupported");
            }
            auto values = value_bundle(inst.value());
            auto indices = value_bundle(inst.indices());
            auto active = value_bundle(inst.active_mask());
            if (values.pieces.size() != indices.pieces.size() ||
                values.pieces.size() != active.pieces.size()) {
                fail_vector_legalization(
                    "oversized VP scatter operands have different chunk plans");
            }
            const bool native_offsets =
                indices_are_safe_e32_byte_offsets(inst.indices(), inst.active_mask(), inst.evl());
            const auto strided =
                strided_index_plan(inst.indices(), inst.active_mask(), inst.evl());
            auto address = value_reg(inst.base_ptr());
            const auto strided_byte_stride =
                strided.has_value()
                    ? std::optional<mir::Register>{materialize_strided_byte_stride(*strided)}
                    : std::nullopt;
            const auto tail_policy = lower_tail_policy(inst.tail_policy());
            const auto mask_policy = lower_mask_policy(inst.mask_policy());
            for (std::size_t index = 0; index < values.pieces.size(); ++index) {
                const auto &plan_piece = values.plan.pieces[index];
                if (indices.plan.pieces[index].lane_base != plan_piece.lane_base ||
                    indices.plan.pieces[index].lane_count != plan_piece.lane_count ||
                    active.plan.pieces[index].lane_base != plan_piece.lane_base ||
                    active.plan.pieces[index].lane_count != plan_piece.lane_count) {
                    fail_vector_legalization(
                        "oversized VP scatter data/index/mask lanes are not aligned");
                }
                const auto piece_evl = lower_fixed_piece_evl(inst.evl(), plan_piece);
                const auto type = *values.pieces[index].vector_type;
                const auto index_type = *indices.pieces[index].vector_type;
                if (strided.has_value()) {
                    auto piece_address =
                        materialize_strided_address(address, *strided, plan_piece.lane_base);
                    emit_fixed_piece_setvli(type, piece_evl, tail_policy, mask_policy);
                    auto execution_mask = prepare_execution_mask(active.pieces[index], type);
                    mir::MachineVectorInfo info(type);
                    info.operation = mir::RVVOperation::Store;
                    info.avl = mir::MachineVectorAVL::current_vl();
                    info.tail_policy = tail_policy;
                    info.mask_policy = mask_policy;
                    info.mask_operand = 3;
                    emit_vector_instruction(
                        mir::Opcode::RVVStoreStrided,
                        {mir::MachineOperand::reg_use(values.pieces[index]),
                         mir::MachineOperand::reg_use(std::move(piece_address)),
                         mir::MachineOperand::reg_use(*strided_byte_stride),
                         mir::MachineOperand::reg_use(std::move(execution_mask))},
                        std::move(info));
                    continue;
                }
                if (!native_offsets) {
                    lower_vp_scatter_scalar_fallback(inst, type, index_type, values.pieces[index],
                                                     indices.pieces[index], address,
                                                     active.pieces[index], &piece_evl);
                    continue;
                }
                auto byte_offsets =
                    emit_e32_byte_offsets(index_type, indices.pieces[index], piece_evl);
                emit_fixed_piece_setvli(type, piece_evl, tail_policy, mask_policy);
                auto execution_mask = prepare_execution_mask(active.pieces[index], type);
                mir::MachineVectorInfo info(type);
                info.operation = mir::RVVOperation::Store;
                info.avl = mir::MachineVectorAVL::current_vl();
                info.tail_policy = tail_policy;
                info.mask_policy = mask_policy;
                info.index_vector_type = index_type;
                info.mask_operand = 3;
                emit_vector_instruction(mir::Opcode::RVVStoreIndexed,
                                        {mir::MachineOperand::reg_use(values.pieces[index]),
                                         mir::MachineOperand::reg_use(address),
                                         mir::MachineOperand::reg_use(std::move(byte_offsets)),
                                         mir::MachineOperand::reg_use(std::move(execution_mask))},
                                        std::move(info));
            }
            return;
        }
        const auto type = legalize_vector_type(*value_oir_type);
        const auto index_type = legalize_vector_type(*index_oir_type);
        if (type.is_mask()) {
            fail_vector_legalization("VP scatter of packed masks is unsupported");
        }
        const auto strided = strided_index_plan(inst.indices(), inst.active_mask(), inst.evl());
        if (strided.has_value()) {
            // Materializing a constant vector may temporarily select its full
            // fixed width.  Do it before establishing the scatter EVL and
            // execution mask so the final VSSE cannot accidentally observe
            // that materialization VL.
            auto value = value_reg(inst.value());
            auto address = materialize_strided_address(value_reg(inst.base_ptr()), *strided);
            auto byte_stride = materialize_strided_byte_stride(*strided);
            const auto tail_policy = lower_tail_policy(inst.tail_policy());
            const auto mask_policy = lower_mask_policy(inst.mask_policy());
            ensure_vector_configuration(type, inst.evl(), tail_policy, mask_policy);
            auto execution_mask = prepare_execution_mask(value_reg(inst.active_mask()), type);
            mir::MachineVectorInfo info(type);
            info.operation = mir::RVVOperation::Store;
            info.avl = mir::MachineVectorAVL::current_vl();
            info.tail_policy = tail_policy;
            info.mask_policy = mask_policy;
            info.mask_operand = 3;
            emit_vector_instruction(
                mir::Opcode::RVVStoreStrided,
                {mir::MachineOperand::reg_use(std::move(value)),
                 mir::MachineOperand::reg_use(std::move(address)),
                 mir::MachineOperand::reg_use(std::move(byte_stride)),
                 mir::MachineOperand::reg_use(std::move(execution_mask))},
                std::move(info));
            return;
        }
        if (!indices_are_safe_e32_byte_offsets(inst.indices(), inst.active_mask(), inst.evl())) {
            lower_vp_scatter_scalar_fallback(inst, type, index_type, value_reg(inst.value()),
                                             value_reg(inst.indices()), value_reg(inst.base_ptr()),
                                             value_reg(inst.active_mask()));
            return;
        }

        auto value = value_reg(inst.value());
        auto address = value_reg(inst.base_ptr());
        auto ordinary_mask = value_reg(inst.active_mask());
        auto byte_offsets =
            emit_e32_byte_offsets(index_type, value_reg(inst.indices()), inst.evl());
        const auto tail_policy = lower_tail_policy(inst.tail_policy());
        const auto mask_policy = lower_mask_policy(inst.mask_policy());
        ensure_vector_configuration(type, inst.evl(), tail_policy, mask_policy);
        auto execution_mask = prepare_execution_mask(std::move(ordinary_mask), type);
        mir::MachineVectorInfo info(type);
        info.operation = mir::RVVOperation::Store;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        info.index_vector_type = index_type;
        info.mask_operand = 3;
        emit_vector_instruction(mir::Opcode::RVVStoreIndexed,
                                {mir::MachineOperand::reg_use(std::move(value)),
                                 mir::MachineOperand::reg_use(std::move(address)),
                                 mir::MachineOperand::reg_use(std::move(byte_offsets)),
                                 mir::MachineOperand::reg_use(std::move(execution_mask))},
                                std::move(info));
    }

    mir::RVVOperation lower_native_reduction_operation(oir::ReductionKind kind) const {
        switch (kind) {
        case oir::ReductionKind::Add:
            return mir::RVVOperation::ReduceSum;
        case oir::ReductionKind::Min:
            return mir::RVVOperation::ReduceMin;
        case oir::ReductionKind::Max:
            return mir::RVVOperation::ReduceMax;
        case oir::ReductionKind::And:
            return mir::RVVOperation::ReduceAnd;
        case oir::ReductionKind::Or:
            return mir::RVVOperation::ReduceOr;
        case oir::ReductionKind::Xor:
            return mir::RVVOperation::ReduceXor;
        case oir::ReductionKind::Mul:
            break;
        }
        fail_vector_legalization("reduction operation requires sequential scalar fallback");
    }

    mir::Register emit_mask_population_count(const mir::MachineVectorType &config,
                                             mir::Register source, mir::Register active_mask,
                                             oir::Value *evl, mir::VectorTailPolicy tail_policy,
                                             mir::VectorMaskPolicy mask_policy) {
        ensure_vector_configuration(config, evl, tail_policy, mask_policy);
        auto execution_mask = prepare_execution_mask(std::move(active_mask), config);
        auto count = create_vreg(mir::ValueType::I32);
        mir::MachineVectorInfo info(config);
        info.operation = mir::RVVOperation::MaskPopulationCount;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        info.mask_operand = 2;
        emit_vector_instruction(mir::Opcode::RVVMaskPopCount,
                                {mir::MachineOperand::reg_def(count),
                                 mir::MachineOperand::reg_use(std::move(source)),
                                 mir::MachineOperand::reg_use(std::move(execution_mask))},
                                std::move(info));
        return count;
    }

    mir::Register emit_mask_population_count(const mir::MachineVectorType &config,
                                             mir::Register source, mir::Register active_mask,
                                             const FixedPieceEVL &evl,
                                             mir::VectorTailPolicy tail_policy,
                                             mir::VectorMaskPolicy mask_policy) {
        emit_fixed_piece_setvli(config, evl, tail_policy, mask_policy);
        auto execution_mask = prepare_execution_mask(std::move(active_mask), config);
        auto count = create_vreg(mir::ValueType::I32);
        mir::MachineVectorInfo info(config);
        info.operation = mir::RVVOperation::MaskPopulationCount;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        info.mask_operand = 2;
        emit_vector_instruction(mir::Opcode::RVVMaskPopCount,
                                {mir::MachineOperand::reg_def(count),
                                 mir::MachineOperand::reg_use(std::move(source)),
                                 mir::MachineOperand::reg_use(std::move(execution_mask))},
                                std::move(info));
        return count;
    }

    void combine_mask_reduction(oir::ReductionKind kind, mir::Register count, mir::Register seed,
                                mir::Register destination) {
        if (kind == oir::ReductionKind::Or) {
            auto any = create_vreg(mir::ValueType::I1);
            emit(mir::Opcode::Snez,
                 {mir::MachineOperand::reg_def(any), mir::MachineOperand::reg_use(count)});
            auto disjoint = create_vreg(mir::ValueType::I1);
            emit(mir::Opcode::Xor,
                 {mir::MachineOperand::reg_def(disjoint), mir::MachineOperand::reg_use(seed),
                  mir::MachineOperand::reg_use(any)});
            emit(mir::Opcode::And,
                 {mir::MachineOperand::reg_def(destination), mir::MachineOperand::reg_use(seed),
                  mir::MachineOperand::reg_use(any)});
            emit(mir::Opcode::Xor, {mir::MachineOperand::reg_def(destination),
                                    mir::MachineOperand::reg_use(destination),
                                    mir::MachineOperand::reg_use(disjoint)});
            return;
        }
        if (kind == oir::ReductionKind::And) {
            auto all = create_vreg(mir::ValueType::I1);
            emit(mir::Opcode::SeqZ,
                 {mir::MachineOperand::reg_def(all), mir::MachineOperand::reg_use(count)});
            emit(mir::Opcode::And,
                 {mir::MachineOperand::reg_def(destination), mir::MachineOperand::reg_use(seed),
                  mir::MachineOperand::reg_use(all)});
            return;
        }
        if (kind != oir::ReductionKind::Xor) {
            fail_vector_legalization("mask reduction supports only And/Or/Xor");
        }
        auto parity = create_vreg(mir::ValueType::I1);
        emit(mir::Opcode::AndI, {mir::MachineOperand::reg_def(parity),
                                 mir::MachineOperand::reg_use(count), mir::MachineOperand::imm(1)});
        emit(mir::Opcode::Xor,
             {mir::MachineOperand::reg_def(destination), mir::MachineOperand::reg_use(seed),
              mir::MachineOperand::reg_use(parity)});
    }

    void lower_vp_mask_reduction(const oir::VPReductionInst &inst,
                                 const mir::MachineVectorType &mask_type) {
        const auto config = data_config_for(mask_type);
        auto source = value_reg(inst.vector());
        auto active = value_reg(inst.active_mask());
        auto seed = value_reg(inst.passthrough());
        const auto tail_policy = lower_tail_policy(inst.tail_policy());
        const auto mask_policy = lower_mask_policy(inst.mask_policy());

        if (inst.kind() == oir::ReductionKind::And) {
            source = emit_mask_logical(config, mir::RVVOperation::MaskNot, source, source);
        } else if (inst.kind() != oir::ReductionKind::Or &&
                   inst.kind() != oir::ReductionKind::Xor) {
            fail_vector_legalization("mask reduction supports only And/Or/Xor");
        }
        auto count = emit_mask_population_count(config, std::move(source), std::move(active),
                                                inst.evl(), tail_policy, mask_policy);
        combine_mask_reduction(inst.kind(), std::move(count), std::move(seed),
                               value_regs_.at(&inst));
    }

    void lower_vp_mask_reduction_piece(const oir::VPReductionInst &inst,
                                       const mir::MachineVectorType &mask_type,
                                       mir::Register source, mir::Register active,
                                       mir::Register destination, const FixedPieceEVL &evl) {
        const auto config = data_config_for(mask_type);
        const auto tail_policy = lower_tail_policy(inst.tail_policy());
        const auto mask_policy = lower_mask_policy(inst.mask_policy());
        if (inst.kind() == oir::ReductionKind::And) {
            source = emit_mask_logical(config, mir::RVVOperation::MaskNot, source, source);
        } else if (inst.kind() != oir::ReductionKind::Or &&
                   inst.kind() != oir::ReductionKind::Xor) {
            fail_vector_legalization("mask reduction supports only And/Or/Xor");
        }
        auto count = emit_mask_population_count(config, std::move(source), std::move(active), evl,
                                                tail_policy, mask_policy);
        combine_mask_reduction(inst.kind(), std::move(count), destination, destination);
    }

    void lower_vp_native_reduction(const oir::VPReductionInst &inst,
                                   const mir::MachineVectorType &type) {
        auto source = value_reg(inst.vector());
        auto active = value_reg(inst.active_mask());
        auto seed = value_reg(inst.passthrough());
        // Seed every lane before narrowing to the requested EVL.  In
        // particular, VL=0 leaves the tied destination unchanged at seed.
        auto seed_vector = emit_data_splat(type, std::move(seed));
        auto result_vector = create_vector_vreg(type);
        const auto tail_policy = lower_tail_policy(inst.tail_policy());
        const auto mask_policy = lower_mask_policy(inst.mask_policy());
        ensure_vector_configuration(type, inst.evl(), tail_policy, mask_policy);
        auto execution_mask = prepare_execution_mask(std::move(active), type);
        mir::MachineVectorInfo info(type);
        info.operation = lower_native_reduction_operation(inst.kind());
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        info.mask_operand = 3;
        const bool is_float = type.element_type() == mir::ValueType::F32;
        if (is_float) {
            if (inst.kind() != oir::ReductionKind::Add || !inst.ordered()) {
                fail_vector_legalization(
                    "non-add floating reduction requires sequential scalar fallback");
            }
            info.rounding = mir::VectorRoundingMode::Dynamic;
        }
        emit_vector_instruction(is_float ? mir::Opcode::RVVReductionFloat
                                         : mir::Opcode::RVVReductionInt,
                                {mir::MachineOperand::reg_def(result_vector),
                                 mir::MachineOperand::reg_use(std::move(source)),
                                 mir::MachineOperand::reg_use(std::move(seed_vector)),
                                 mir::MachineOperand::reg_use(std::move(execution_mask))},
                                std::move(info), is_float);
        emit_extract_data(value_regs_.at(&inst), type, std::move(result_vector), zero_reg(), 0);
    }

    void lower_vp_native_reduction_piece(const oir::VPReductionInst &inst,
                                         const mir::MachineVectorType &type, mir::Register source,
                                         mir::Register active, mir::Register destination,
                                         const FixedPieceEVL &evl) {
        // Chaining destination as the next piece's seed preserves the logical
        // bundle order.  Ordered floating reductions therefore never combine
        // independently reduced partials or reassociate across a boundary.
        auto seed_vector = emit_data_splat(type, destination);
        auto result_vector = create_vector_vreg(type);
        const auto tail_policy = lower_tail_policy(inst.tail_policy());
        const auto mask_policy = lower_mask_policy(inst.mask_policy());
        emit_fixed_piece_setvli(type, evl, tail_policy, mask_policy);
        auto execution_mask = prepare_execution_mask(std::move(active), type);
        mir::MachineVectorInfo info(type);
        info.operation = lower_native_reduction_operation(inst.kind());
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        info.mask_operand = 3;
        const bool is_float = type.element_type() == mir::ValueType::F32;
        if (is_float) {
            if (inst.kind() != oir::ReductionKind::Add || !inst.ordered()) {
                fail_vector_legalization(
                    "non-add floating reduction requires sequential scalar fallback");
            }
            info.rounding = mir::VectorRoundingMode::Dynamic;
        }
        emit_vector_instruction(is_float ? mir::Opcode::RVVReductionFloat
                                         : mir::Opcode::RVVReductionInt,
                                {mir::MachineOperand::reg_def(result_vector),
                                 mir::MachineOperand::reg_use(std::move(source)),
                                 mir::MachineOperand::reg_use(std::move(seed_vector)),
                                 mir::MachineOperand::reg_use(std::move(execution_mask))},
                                std::move(info), is_float);
        emit_extract_data(destination, type, std::move(result_vector), zero_reg(), 0);
    }

    mir::MachineVectorType integer_vector_view(const mir::MachineVectorType &type) const {
        if (type.is_fixed()) {
            return mir::MachineVectorType::fixed(mir::ValueType::I32, type.sew_bits(), type.lmul(),
                                                 type.fixed_lanes());
        }
        return mir::MachineVectorType::scalable(mir::ValueType::I32, type.sew_bits(), type.lmul());
    }

    void enter_synthetic_vector_block(mir::MachineBasicBlock *block,
                                      const mir::MachineVectorType &config, oir::Value *evl) {
        current_block_ = block;
        current_vector_config_.reset();
        current_execution_mask_source_.reset();
        block_mask_constant_regs_.clear();
        if (config.is_scalable()) {
            dominating_scalable_config_ = DominatingScalableConfiguration{config, evl};
        } else {
            dominating_scalable_config_.reset();
        }
    }

    void lower_vp_sequential_reduction(const oir::VPReductionInst &inst,
                                       const mir::MachineVectorType &type, mir::Register source,
                                       mir::Register active, mir::Register destination,
                                       mir::Register seed, bool initialize_seed,
                                       const FixedPieceEVL *piece_evl = nullptr) {
        const auto config = data_config_for(type);
        const auto index_type = integer_vector_view(config);
        if (initialize_seed) {
            emit_move(destination, std::move(seed));
        }

        // Preserve the ordinary mask away from v0.  vfirst enumerates the
        // lowest remaining active lane, so scalar operations observe exactly
        // source order even for sparse masks and arbitrary EVL.
        auto work_mask = create_vector_vreg(mir::MachineVectorType::mask_for(config));
        emit_ordinary_mask_copy(config, work_mask, std::move(active));
        ensure_vector_configuration(index_type);
        auto lane_ids = create_vector_vreg(index_type);
        mir::MachineVectorInfo step_info(index_type);
        step_info.operation = mir::RVVOperation::Step;
        step_info.avl = mir::MachineVectorAVL::current_vl();
        step_info.tail_policy = mir::VectorTailPolicy::Agnostic;
        step_info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(mir::Opcode::RVVStepTA, {mir::MachineOperand::reg_def(lane_ids)},
                                std::move(step_info));

        const auto suffix = std::to_string(temp_index_++);
        auto *header = current_function_->create_block("rvv.reduce.header." + suffix);
        auto *body = current_function_->create_block("rvv.reduce.body." + suffix);
        const bool needs_choose =
            type.element_type() == mir::ValueType::F32 &&
            (inst.kind() == oir::ReductionKind::Min || inst.kind() == oir::ReductionKind::Max);
        auto *choose =
            needs_choose ? current_function_->create_block("rvv.reduce.choose." + suffix) : nullptr;
        auto *clear = current_function_->create_block("rvv.reduce.clear." + suffix);
        auto *done = current_function_->create_block("rvv.reduce.done." + suffix);
        emit(mir::Opcode::Jump, {mir::MachineOperand::block(header->name())});

        const auto tail_policy = lower_tail_policy(inst.tail_policy());
        const auto mask_policy = lower_mask_policy(inst.mask_policy());
        enter_synthetic_vector_block(header, config, inst.evl());
        if (piece_evl != nullptr) {
            emit_fixed_piece_setvli(config, *piece_evl, tail_policy, mask_policy);
        } else {
            auto ignored_vl = create_vreg(mir::ValueType::I32);
            emit_setvli(config, ignored_vl, inst.evl(), tail_policy, mask_policy);
        }
        auto lane_index = create_vreg(mir::ValueType::I32);
        mir::MachineVectorInfo first_info(config);
        first_info.operation = mir::RVVOperation::MaskFirst;
        first_info.avl = mir::MachineVectorAVL::current_vl();
        first_info.tail_policy = tail_policy;
        first_info.mask_policy = mask_policy;
        emit_vector_instruction(
            mir::Opcode::RVVMaskFirst,
            {mir::MachineOperand::reg_def(lane_index), mir::MachineOperand::reg_use(work_mask)},
            std::move(first_info));
        emit(mir::Opcode::BranchLT,
             {mir::MachineOperand::reg_use(lane_index), mir::MachineOperand::reg_use(zero_reg()),
              mir::MachineOperand::block(done->name())});
        emit(mir::Opcode::Jump, {mir::MachineOperand::block(body->name())});

        enter_synthetic_vector_block(body, config, inst.evl());
        auto lane_value = create_vreg(type.element_type());
        emit_extract_data(lane_value, type, source, lane_index, std::nullopt);
        const bool floating = type.element_type() == mir::ValueType::F32;
        if (inst.kind() == oir::ReductionKind::Mul) {
            emit(floating ? mir::Opcode::FMulS : mir::Opcode::MulW,
                 {mir::MachineOperand::reg_def(destination),
                  mir::MachineOperand::reg_use(destination),
                  mir::MachineOperand::reg_use(lane_value)});
            emit(mir::Opcode::Jump, {mir::MachineOperand::block(clear->name())});
        } else {
            if (!floating || (inst.kind() != oir::ReductionKind::Min &&
                              inst.kind() != oir::ReductionKind::Max)) {
                fail_vector_legalization("invalid sequential reduction fallback operation");
            }
            auto take_lane = create_vreg(mir::ValueType::I1);
            emit(mir::Opcode::FltS,
                 {mir::MachineOperand::reg_def(take_lane),
                  mir::MachineOperand::reg_use(
                      inst.kind() == oir::ReductionKind::Min ? lane_value : destination),
                  mir::MachineOperand::reg_use(
                      inst.kind() == oir::ReductionKind::Min ? destination : lane_value)});
            emit(mir::Opcode::BranchNonZero, {mir::MachineOperand::reg_use(take_lane),
                                              mir::MachineOperand::block(choose->name())});
            emit(mir::Opcode::Jump, {mir::MachineOperand::block(clear->name())});
        }

        if (choose != nullptr) {
            enter_synthetic_vector_block(choose, config, inst.evl());
            emit_move(destination, lane_value);
            emit(mir::Opcode::Jump, {mir::MachineOperand::block(clear->name())});
        }

        enter_synthetic_vector_block(clear, index_type, inst.evl());
        auto index_splat = emit_data_splat(index_type, lane_index);
        auto one_hot =
            emit_compare_mask(index_type, mir::RVVOperation::Eq, lane_ids, std::move(index_splat));
        auto remaining = emit_mask_logical(index_type, mir::RVVOperation::MaskXor, work_mask,
                                           std::move(one_hot));
        emit_move(work_mask, std::move(remaining));
        emit(mir::Opcode::Jump, {mir::MachineOperand::block(header->name())});

        enter_synthetic_vector_block(done, config, inst.evl());
    }

    void lower_vp_reduction(const oir::VPReductionInst &inst) {
        const auto *oir_type = dynamic_cast<const oir::VectorType *>(inst.vector()->type());
        if (oir_type == nullptr) {
            fail_vector_legalization("VP reduction source is not a vector");
        }
        if (is_oversized_fixed_vector(oir_type)) {
            auto source = value_bundle(inst.vector());
            auto active = value_bundle(inst.active_mask());
            if (source.pieces.size() != active.pieces.size()) {
                fail_vector_legalization("oversized VP reduction source/mask chunk plan mismatch");
            }
            auto destination = value_regs_.at(&inst);
            emit_move(destination, value_reg(inst.passthrough()));
            for (std::size_t index = 0; index < source.pieces.size(); ++index) {
                const auto &plan_piece = source.plan.pieces[index];
                if (active.plan.pieces[index].lane_base != plan_piece.lane_base ||
                    active.plan.pieces[index].lane_count != plan_piece.lane_count) {
                    fail_vector_legalization(
                        "oversized VP reduction source/mask lanes are not aligned");
                }
                const auto piece_evl = lower_fixed_piece_evl(inst.evl(), plan_piece);
                const auto type = *source.pieces[index].vector_type;
                if (type.is_mask()) {
                    lower_vp_mask_reduction_piece(inst, type, source.pieces[index],
                                                  active.pieces[index], destination, piece_evl);
                    continue;
                }
                const bool needs_scalar_fallback = inst.kind() == oir::ReductionKind::Mul ||
                                                   (type.element_type() == mir::ValueType::F32 &&
                                                    inst.kind() != oir::ReductionKind::Add);
                if (needs_scalar_fallback) {
                    lower_vp_sequential_reduction(inst, type, source.pieces[index],
                                                  active.pieces[index], destination, destination,
                                                  false, &piece_evl);
                    continue;
                }
                lower_vp_native_reduction_piece(inst, type, source.pieces[index],
                                                active.pieces[index], destination, piece_evl);
            }
            return;
        }
        const auto type = legalize_vector_type(*oir_type);
        if (type.is_mask()) {
            lower_vp_mask_reduction(inst, type);
            return;
        }
        const bool needs_scalar_fallback =
            inst.kind() == oir::ReductionKind::Mul ||
            (type.element_type() == mir::ValueType::F32 && inst.kind() != oir::ReductionKind::Add);
        if (needs_scalar_fallback) {
            lower_vp_sequential_reduction(inst, type, value_reg(inst.vector()),
                                          value_reg(inst.active_mask()), value_regs_.at(&inst),
                                          value_reg(inst.passthrough()), true);
            return;
        }
        lower_vp_native_reduction(inst, type);
    }

    void lower_vp_binary(const oir::VPBinaryInst &inst) {
        if (is_oversized_fixed_vector(inst.type())) {
            auto lhs = value_bundle(inst.lhs());
            auto rhs = value_bundle(inst.rhs());
            auto active = value_bundle(inst.active_mask());
            auto &destination = bundle_result(inst);
            const bool discardable = vp_passthrough_is_discardable(inst);
            std::optional<FixedVectorBundle> passthrough;
            if (!discardable) {
                passthrough = value_bundle(inst.passthrough());
            }
            if (lhs.pieces.size() != destination.pieces.size() ||
                rhs.pieces.size() != destination.pieces.size() ||
                active.pieces.size() != destination.pieces.size() ||
                (passthrough.has_value() &&
                 passthrough->pieces.size() != destination.pieces.size())) {
                fail_vector_legalization("oversized VP binary operands have different chunk plans");
            }
            const auto tail_policy = lower_tail_policy(inst.tail_policy());
            const auto mask_policy = lower_mask_policy(inst.mask_policy());
            for (std::size_t index = 0; index < destination.pieces.size(); ++index) {
                const auto piece_evl =
                    lower_fixed_piece_evl(inst.evl(), destination.plan.pieces[index]);
                const auto type = *destination.pieces[index].vector_type;
                const auto config = data_config_for(type);
                if (type.is_mask()) {
                    mir::RVVOperation operation;
                    switch (inst.binary_op()) {
                    case oir::Instruction::OpID::And:
                        operation = mir::RVVOperation::MaskAnd;
                        break;
                    case oir::Instruction::OpID::Or:
                        operation = mir::RVVOperation::MaskOr;
                        break;
                    case oir::Instruction::OpID::Xor:
                        operation = mir::RVVOperation::MaskXor;
                        break;
                    default:
                        fail_vector_legalization(
                            "oversized VP mask binary supports only And/Or/Xor");
                    }
                    if (discardable) {
                        emit_mask_logical(config, operation, lhs.pieces[index], rhs.pieces[index],
                                          destination.pieces[index]);
                        continue;
                    }
                    auto computed =
                        emit_mask_logical(config, operation, lhs.pieces[index], rhs.pieces[index]);
                    auto prefix = emit_fixed_piece_evl_prefix_mask(config, piece_evl);
                    mir::Register update;
                    const bool tail_agnostic = inst.tail_policy() == oir::TailPolicy::Agnostic;
                    const bool mask_agnostic = inst.mask_policy() == oir::MaskPolicy::Agnostic;
                    if (!tail_agnostic && !mask_agnostic) {
                        update = emit_mask_logical(config, mir::RVVOperation::MaskAnd,
                                                   active.pieces[index], std::move(prefix));
                    } else if (!tail_agnostic) {
                        update = std::move(prefix);
                    } else {
                        auto outside_prefix =
                            emit_mask_logical(config, mir::RVVOperation::MaskNot, prefix, prefix);
                        update = emit_mask_logical(config, mir::RVVOperation::MaskOr,
                                                   active.pieces[index], std::move(outside_prefix));
                    }
                    auto kept_computed =
                        emit_mask_logical(config, mir::RVVOperation::MaskAnd, update, computed);
                    auto not_active =
                        emit_mask_logical(config, mir::RVVOperation::MaskNot, update, update);
                    auto kept_passthrough = emit_mask_logical(
                        config, mir::RVVOperation::MaskAnd, not_active, passthrough->pieces[index]);
                    emit_mask_logical(config, mir::RVVOperation::MaskOr, kept_computed,
                                      kept_passthrough, destination.pieces[index]);
                    continue;
                }

                std::optional<mir::Register> tied;
                if (!discardable) {
                    tied = make_fresh_tied_passthrough(passthrough->pieces[index]);
                }
                emit_fixed_piece_setvli(config, piece_evl, tail_policy, mask_policy);
                auto mask = prepare_execution_mask(active.pieces[index], config);
                mir::MachineVectorInfo info(config);
                info.operation = lower_binary_operation(inst.binary_op());
                info.avl = mir::MachineVectorAVL::current_vl();
                info.tail_policy = tail_policy;
                info.mask_policy = mask_policy;
                const bool is_float = type.element_type() == mir::ValueType::F32;
                info.rounding =
                    is_float ? mir::VectorRoundingMode::Dynamic : mir::VectorRoundingMode::None;
                if (discardable) {
                    info.mask_operand = 3;
                    emit_vector_instruction(
                        is_float ? mir::Opcode::RVVFloatBinaryVVTA : mir::Opcode::RVVIntBinaryVVTA,
                        {mir::MachineOperand::reg_def(destination.pieces[index]),
                         mir::MachineOperand::reg_use(lhs.pieces[index]),
                         mir::MachineOperand::reg_use(rhs.pieces[index]),
                         mir::MachineOperand::reg_use(mask)},
                        std::move(info), is_float);
                } else {
                    info.passthrough_operand = 1;
                    info.mask_operand = 4;
                    emit_vector_instruction(
                        is_float ? mir::Opcode::RVVFloatBinaryVV : mir::Opcode::RVVIntBinaryVV,
                        {mir::MachineOperand::reg_def(destination.pieces[index]),
                         mir::MachineOperand::reg_use(*tied),
                         mir::MachineOperand::reg_use(lhs.pieces[index]),
                         mir::MachineOperand::reg_use(rhs.pieces[index]),
                         mir::MachineOperand::reg_use(mask)},
                        std::move(info), is_float);
                }
            }
            return;
        }
        auto type = legalize_vector_type(*static_cast<oir::VectorType *>(inst.type()));
        if (type.is_mask()) {
            lower_vp_mask_binary(inst, type);
            return;
        }
        auto dst = value_regs_.at(&inst);
        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        const bool unmasked = vp_mask_is_all_true(inst);
        std::optional<mir::Register> ordinary_mask;
        if (!unmasked) {
            ordinary_mask = value_reg(inst.active_mask());
        }
        const auto tail_policy = lower_tail_policy(inst.tail_policy());
        const auto mask_policy = lower_mask_policy(inst.mask_policy());
        std::optional<mir::Register> passthrough;
        const bool discardable = vp_passthrough_is_discardable(inst);
        if (!discardable) {
            passthrough = make_fresh_tied_passthrough(value_reg(inst.passthrough()));
        }
        ensure_vector_configuration(type, inst.evl(), tail_policy, mask_policy);
        mir::MachineVectorInfo info(type);
        info.operation = lower_binary_operation(inst.binary_op());
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = tail_policy;
        info.mask_policy = mask_policy;
        const bool is_float = type.element_type() == mir::ValueType::F32;
        info.rounding = is_float ? mir::VectorRoundingMode::Dynamic : mir::VectorRoundingMode::None;
        if (discardable) {
            std::vector<mir::MachineOperand> operands = {
                mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(lhs),
                mir::MachineOperand::reg_use(rhs)};
            if (!unmasked) {
                auto mask = prepare_execution_mask(*ordinary_mask, type);
                info.mask_operand = operands.size();
                operands.push_back(mir::MachineOperand::reg_use(mask));
            }
            emit_vector_instruction(
                is_float ? mir::Opcode::RVVFloatBinaryVVTA : mir::Opcode::RVVIntBinaryVVTA,
                std::move(operands), std::move(info), is_float);
        } else {
            info.passthrough_operand = 1;
            std::vector<mir::MachineOperand> operands = {
                mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(*passthrough),
                mir::MachineOperand::reg_use(lhs), mir::MachineOperand::reg_use(rhs)};
            if (!unmasked) {
                auto mask = prepare_execution_mask(*ordinary_mask, type);
                info.mask_operand = operands.size();
                operands.push_back(mir::MachineOperand::reg_use(mask));
            }
            emit_vector_instruction(is_float ? mir::Opcode::RVVFloatBinaryVV
                                             : mir::Opcode::RVVIntBinaryVV,
                                    std::move(operands), std::move(info), is_float);
        }
    }

    void lower_instruction(const oir::Instruction &inst) {
        if (inst.op() == oir::Instruction::OpID::Phi ||
            consumed_segment2_fields_.find(&inst) != consumed_segment2_fields_.end()) {
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
        case oir::Instruction::OpID::Or:
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
        case oir::Instruction::OpID::Splat:
            if (!all_true_mask_splat_is_lowering_dead(
                    static_cast<const oir::SplatInst &>(inst)) &&
                !strided_gather_index_splat_is_lowering_dead(
                    static_cast<const oir::SplatInst &>(inst))) {
                lower_splat(static_cast<const oir::SplatInst &>(inst));
            }
            break;
        case oir::Instruction::OpID::StepVector:
            lower_step_vector(static_cast<const oir::StepVectorInst &>(inst));
            break;
        case oir::Instruction::OpID::ExtractElement:
            lower_extract_element(static_cast<const oir::ExtractElementInst &>(inst));
            break;
        case oir::Instruction::OpID::InsertElement:
            lower_insert_element(static_cast<const oir::InsertElementInst &>(inst));
            break;
        case oir::Instruction::OpID::ShuffleVector:
            lower_shuffle_vector(static_cast<const oir::ShuffleVectorInst &>(inst));
            break;
        case oir::Instruction::OpID::VectorSelect:
            lower_vector_select(static_cast<const oir::VectorSelectInst &>(inst));
            break;
        case oir::Instruction::OpID::VectorCast:
            lower_vector_cast(static_cast<const oir::VectorCastInst &>(inst));
            break;
        case oir::Instruction::OpID::VPCmp:
            lower_vp_compare(static_cast<const oir::VPCmpInst &>(inst));
            break;
        case oir::Instruction::OpID::VPGather:
            if (!try_lower_segment2_load(static_cast<const oir::VPGatherInst &>(inst))) {
                lower_vp_gather(static_cast<const oir::VPGatherInst &>(inst));
            }
            break;
        case oir::Instruction::OpID::VPScatter:
            if (!try_lower_segment2_store(static_cast<const oir::VPScatterInst &>(inst))) {
                lower_vp_scatter(static_cast<const oir::VPScatterInst &>(inst));
            }
            break;
        case oir::Instruction::OpID::VPReduction:
            lower_vp_reduction(static_cast<const oir::VPReductionInst &>(inst));
            break;
        case oir::Instruction::OpID::VPBinary:
            lower_vp_binary(static_cast<const oir::VPBinaryInst &>(inst));
            break;
        case oir::Instruction::OpID::VPLoad:
        case oir::Instruction::OpID::MaskedLoad:
            lower_vp_load(static_cast<const oir::VPLoadInst &>(inst));
            break;
        case oir::Instruction::OpID::VPStore:
        case oir::Instruction::OpID::MaskedStore:
            lower_vp_store(static_cast<const oir::VPStoreInst &>(inst));
            break;
        case oir::Instruction::OpID::SetVL:
            lower_setvl(static_cast<const oir::SetVLInst &>(inst));
            break;
        case oir::Instruction::OpID::FixedABIExtractLane:
            lower_fixed_abi_extract_lane(static_cast<const oir::FixedABIExtractLaneInst &>(inst));
            break;
        case oir::Instruction::OpID::FixedABIPack:
            lower_fixed_abi_pack(static_cast<const oir::FixedABIPackInst &>(inst));
            break;
        case oir::Instruction::OpID::FixedABIObjectLoadLane:
            lower_fixed_abi_object_load_lane(
                static_cast<const oir::FixedABIObjectLoadLaneInst &>(inst));
            break;
        case oir::Instruction::OpID::FixedABIObjectStoreLane:
            lower_fixed_abi_object_store_lane(
                static_cast<const oir::FixedABIObjectStoreLaneInst &>(inst));
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

    void lower_fixed_abi_extract_lane(const oir::FixedABIExtractLaneInst &inst) {
        const auto &type =
            portable_fixed_vector_type(inst.aggregate()->type(), "abi.fixed.extract");
        emit_portable_lane_load(type, inst.lane_index(),
                                portable_aggregate_address(inst.aggregate(), "abi.fixed.extract"),
                                value_regs_.at(&inst), "abi.fixed.extract");
    }

    void lower_fixed_abi_pack(const oir::FixedABIPackInst &inst) {
        const auto &type = portable_fixed_vector_type(inst.type(), "abi.fixed.pack");
        const auto size = portable_aggregate_size(type, "abi.fixed.pack");
        const auto slot = create_abi_bridge_slot(
            &type, size, "abi.portable.pack." + std::to_string(temp_index_++));
        const auto address = bridge_address(slot);
        if (type.is_mask()) {
            // A packed mask is outbound ABI storage.  Zero every byte first;
            // per-lane RMW then cannot preserve uninitialized high padding.
            emit_portable_mask_zero(type, address, "abi.fixed.pack");
        }
        const auto &lanes = inst.lane_values();
        if (lanes.size() != type.element_count().min_lanes) {
            fail_calling_convention("abi.fixed.pack", "lane count changed after OIR verification");
        }
        for (std::uint64_t lane = 0; lane < lanes.size(); ++lane) {
            emit_portable_lane_store(type, lane, value_reg(lanes[lane]), address, "abi.fixed.pack");
        }
        bind_portable_aggregate_address(&inst, address, "abi.fixed.pack");
    }

    void lower_fixed_abi_object_load_lane(const oir::FixedABIObjectLoadLaneInst &inst) {
        const auto *pointer = dynamic_cast<const oir::PointerType *>(inst.object_ptr()->type());
        const auto &type = portable_fixed_vector_type(
            pointer == nullptr ? nullptr : pointer->element_type(), "abi.fixed.load_lane");
        emit_portable_lane_load(type, inst.lane_index(), value_reg(inst.object_ptr()),
                                value_regs_.at(&inst), "abi.fixed.load_lane");
    }

    void lower_fixed_abi_object_store_lane(const oir::FixedABIObjectStoreLaneInst &inst) {
        const auto *pointer = dynamic_cast<const oir::PointerType *>(inst.object_ptr()->type());
        const auto &type = portable_fixed_vector_type(
            pointer == nullptr ? nullptr : pointer->element_type(), "abi.fixed.store_lane");
        emit_portable_lane_store(type, inst.lane_index(), value_reg(inst.lane_value()),
                                 value_reg(inst.object_ptr()), "abi.fixed.store_lane");
    }

    void lower_load(const oir::LoadInst &inst) {
        if (portable_fixed_abi_enabled() && is_fixed_vector_abi_value(inst.type())) {
            const auto &type = portable_fixed_vector_type(inst.type(), "portable aggregate load");
            const auto size = portable_aggregate_size(type, "portable aggregate load");
            const auto slot = create_abi_bridge_slot(
                &type, size, "abi.portable.load." + std::to_string(temp_index_++));
            const auto address = bridge_address(slot);
            emit_portable_aggregate_copy(type, address, value_reg(inst.ptr()),
                                         "portable aggregate load");
            bind_portable_aggregate_address(&inst, address, "portable aggregate load");
            return;
        }
        if (is_oversized_fixed_vector(inst.type())) {
            auto &destination = bundle_result(inst);
            emit_fixed_bundle_load(destination, value_reg(inst.ptr()));
            return;
        }
        auto loaded = type_info(inst.type());
        auto dst = value_regs_.at(&inst);
        auto addr = value_reg(inst.ptr());
        if (loaded.vector_type.has_value()) {
            const auto type = *loaded.vector_type;
            const auto config = data_config_for(type);
            ensure_vector_configuration(config);
            mir::MachineVectorInfo info(config);
            info.operation = mir::RVVOperation::Load;
            info.avl = mir::MachineVectorAVL::current_vl();
            info.tail_policy = mir::VectorTailPolicy::Agnostic;
            info.mask_policy = mir::VectorMaskPolicy::Agnostic;
            emit_vector_instruction(
                type.is_mask() ? mir::Opcode::RVVMaskLoad : mir::Opcode::RVVLoadUnitTA,
                {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(addr)},
                std::move(info));
            return;
        }
        emit(mir::Opcode::LoadMem,
             {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(addr),
              mir::MachineOperand::type(loaded.value_type)});
    }

    void lower_store(const oir::StoreInst &inst) {
        if (portable_fixed_abi_enabled() && is_fixed_vector_abi_value(inst.value()->type())) {
            const auto &type =
                portable_fixed_vector_type(inst.value()->type(), "portable aggregate store");
            const auto source =
                portable_outbound_aggregate_address(inst.value(), type, "portable aggregate store");
            emit_portable_aggregate_copy(type, value_reg(inst.ptr()), source,
                                         "portable aggregate store");
            return;
        }
        if (is_oversized_fixed_vector(inst.value()->type())) {
            auto value = value_bundle(inst.value());
            emit_fixed_bundle_store(value, value_reg(inst.ptr()));
            return;
        }
        auto stored = type_info(inst.value()->type());
        auto addr = value_reg(inst.ptr());
        if (stored.vector_type.has_value()) {
            const auto type = *stored.vector_type;
            const auto config = data_config_for(type);
            auto value = value_reg(inst.value());
            ensure_vector_configuration(config);
            mir::MachineVectorInfo info(config);
            info.operation = mir::RVVOperation::Store;
            info.avl = mir::MachineVectorAVL::current_vl();
            info.tail_policy = mir::VectorTailPolicy::Agnostic;
            info.mask_policy = mir::VectorMaskPolicy::Agnostic;
            emit_vector_instruction(
                type.is_mask() ? mir::Opcode::RVVMaskStore : mir::Opcode::RVVStoreUnit,
                {mir::MachineOperand::reg_use(value), mir::MachineOperand::reg_use(addr)},
                std::move(info));
            return;
        }
        if (stored.value_type == mir::ValueType::Aggregate) {
            if (dynamic_cast<const oir::ConstantZero *>(inst.value()) == nullptr) {
                throw std::runtime_error("only zero aggregate stores are supported in MIR vreg");
            }
            emit(mir::Opcode::MemZero,
                 {mir::MachineOperand::reg_use(addr), mir::MachineOperand::imm(0),
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

        std::int64_t constant_offset = 0;
        auto indices = inst.indices();
        const auto strides =
            calling_convention_.data_layout().fixed_gep_index_strides(ptr_type, indices.size());
        if (!strides.has_value() || strides->size() != indices.size()) {
            throw std::runtime_error("gep path has no exact fixed DataLayout stride");
        }
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const auto stride = (*strides)[i];

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
                emit(mir::Opcode::LoadImm, {mir::MachineOperand::reg_def(offset),
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
        auto bias = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::SrliW,
             {mir::MachineOperand::reg_def(bias), mir::MachineOperand::reg_use(sign),
              mir::MachineOperand::imm(32 - shift)});

        auto adjusted = create_vreg(mir::ValueType::I32);
        emit(mir::Opcode::AddW,
             {mir::MachineOperand::reg_def(adjusted), mir::MachineOperand::reg_use(value),
              mir::MachineOperand::reg_use(bias)});

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

    bool try_lower_magic_const_div(mir::Register dst, mir::Register value, std::int64_t constant) {
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
                 {mir::MachineOperand::reg_def(adjusted), mir::MachineOperand::reg_use(quotient),
                  mir::MachineOperand::reg_use(value)});
            quotient = adjusted;
        } else if (divisor < 0 && magic.magic > 0) {
            auto adjusted = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::SubW,
                 {mir::MachineOperand::reg_def(adjusted), mir::MachineOperand::reg_use(quotient),
                  mir::MachineOperand::reg_use(value)});
            quotient = adjusted;
        }

        if (magic.shift != 0) {
            auto shifted = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::SraIW,
                 {mir::MachineOperand::reg_def(shifted), mir::MachineOperand::reg_use(quotient),
                  mir::MachineOperand::imm(magic.shift)});
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
                auto sign = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::SraIW,
                     {mir::MachineOperand::reg_def(sign), mir::MachineOperand::reg_use(value),
                      mir::MachineOperand::imm(31)});
                auto bias = create_vreg(mir::ValueType::I32);
                emit(mir::Opcode::SrliW,
                     {mir::MachineOperand::reg_def(bias), mir::MachineOperand::reg_use(sign),
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
                    emit(mir::Opcode::And, {mir::MachineOperand::reg_def(masked),
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
                 {mir::MachineOperand::reg_def(product), mir::MachineOperand::reg_use(quotient),
                  mir::MachineOperand::reg_use(constant_reg)});
            emit(mir::Opcode::SubW,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(value),
                  mir::MachineOperand::reg_use(product)});
            return true;
        }

        return false;
    }

    bool is_all_true_mask(const oir::Value *value) const {
        if (const auto *splat = dynamic_cast<const oir::SplatInst *>(value)) {
            const auto *lane = dynamic_cast<const oir::ConstantInt *>(splat->scalar());
            const auto *lane_type =
                lane == nullptr
                    ? nullptr
                    : dynamic_cast<const oir::IntegerType *>(lane->type());
            return lane_type != nullptr && lane_type->bit_width() == 1 &&
                   lane->value() != 0;
        }
        const auto *mask = dynamic_cast<const oir::ConstantMask *>(value);
        if (mask == nullptr)
            return false;
        for (std::uint64_t lane = 0; lane < mask->lane_count(); ++lane) {
            if (!mask->lane(lane)) {
                return false;
            }
        }
        return true;
    }

    void lower_oversized_vector_binary(const oir::BinaryInst &inst) {
        auto lhs = value_bundle(inst.lhs());
        auto rhs = value_bundle(inst.rhs());
        auto &destination = bundle_result(inst);
        if (lhs.pieces.size() != rhs.pieces.size() ||
            lhs.pieces.size() != destination.pieces.size()) {
            fail_vector_legalization("oversized vector binary operands have different chunk plans");
        }
        for (std::size_t index = 0; index < destination.pieces.size(); ++index) {
            const auto type = *destination.pieces[index].vector_type;
            const auto config = data_config_for(type);
            ensure_vector_configuration(config);
            mir::MachineVectorInfo info(config);
            info.avl = mir::MachineVectorAVL::current_vl();
            info.tail_policy = mir::VectorTailPolicy::Agnostic;
            info.mask_policy = mir::VectorMaskPolicy::Agnostic;
            if (type.is_mask()) {
                switch (inst.op()) {
                case oir::Instruction::OpID::And:
                    info.operation = mir::RVVOperation::MaskAnd;
                    break;
                case oir::Instruction::OpID::Or:
                    info.operation = mir::RVVOperation::MaskOr;
                    break;
                case oir::Instruction::OpID::Xor:
                    info.operation = mir::RVVOperation::MaskXor;
                    break;
                default:
                    fail_vector_legalization("oversized mask binary supports only And/Or/Xor");
                }
                emit_vector_instruction(mir::Opcode::RVVMaskLogical,
                                        {mir::MachineOperand::reg_def(destination.pieces[index]),
                                         mir::MachineOperand::reg_use(lhs.pieces[index]),
                                         mir::MachineOperand::reg_use(rhs.pieces[index])},
                                        std::move(info));
                continue;
            }
            info.operation = lower_binary_operation(inst.op());
            const bool is_float = type.element_type() == mir::ValueType::F32;
            info.rounding =
                is_float ? mir::VectorRoundingMode::Dynamic : mir::VectorRoundingMode::None;
            emit_vector_instruction(is_float ? mir::Opcode::RVVFloatBinaryVVTA
                                             : mir::Opcode::RVVIntBinaryVVTA,
                                    {mir::MachineOperand::reg_def(destination.pieces[index]),
                                     mir::MachineOperand::reg_use(lhs.pieces[index]),
                                     mir::MachineOperand::reg_use(rhs.pieces[index])},
                                    std::move(info), is_float);
        }
    }

    void lower_vector_int_binary(const oir::BinaryInst &inst, const oir::VectorType &source_type) {
        if (is_oversized_fixed_vector(&source_type)) {
            lower_oversized_vector_binary(inst);
            return;
        }
        const auto type = legalize_vector_type(source_type);
        const auto config = data_config_for(type);
        auto dst = value_regs_.at(&inst);
        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        ensure_vector_configuration(config);

        mir::MachineVectorInfo info(config);
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        if (type.is_mask()) {
            switch (inst.op()) {
            case oir::Instruction::OpID::And:
                info.operation = mir::RVVOperation::MaskAnd;
                break;
            case oir::Instruction::OpID::Or:
                info.operation = mir::RVVOperation::MaskOr;
                break;
            case oir::Instruction::OpID::Xor:
                if (is_all_true_mask(inst.rhs())) {
                    info.operation = mir::RVVOperation::MaskNot;
                    rhs = lhs;
                } else if (is_all_true_mask(inst.lhs())) {
                    info.operation = mir::RVVOperation::MaskNot;
                    lhs = rhs;
                } else {
                    info.operation = mir::RVVOperation::MaskXor;
                }
                break;
            default:
                fail_vector_legalization("unsupported fixed mask binary operation: " +
                                         inst.print());
            }
            emit_vector_instruction(mir::Opcode::RVVMaskLogical,
                                    {mir::MachineOperand::reg_def(dst),
                                     mir::MachineOperand::reg_use(lhs),
                                     mir::MachineOperand::reg_use(rhs)},
                                    std::move(info));
            return;
        }

        info.operation = lower_binary_operation(inst.op());
        emit_vector_instruction(mir::Opcode::RVVIntBinaryVVTA,
                                {mir::MachineOperand::reg_def(dst),
                                 mir::MachineOperand::reg_use(lhs),
                                 mir::MachineOperand::reg_use(rhs)},
                                std::move(info));
    }

    void lower_vector_float_binary(const oir::BinaryInst &inst,
                                   const oir::VectorType &source_type) {
        if (is_oversized_fixed_vector(&source_type)) {
            lower_oversized_vector_binary(inst);
            return;
        }
        const auto type = legalize_vector_type(source_type);
        auto dst = value_regs_.at(&inst);
        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        ensure_vector_configuration(type);
        mir::MachineVectorInfo info(type);
        info.operation = lower_binary_operation(inst.op());
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        info.rounding = mir::VectorRoundingMode::Dynamic;
        emit_vector_instruction(mir::Opcode::RVVFloatBinaryVVTA,
                                {mir::MachineOperand::reg_def(dst),
                                 mir::MachineOperand::reg_use(lhs),
                                 mir::MachineOperand::reg_use(rhs)},
                                std::move(info), true);
    }

    mir::RVVOperation lower_compare_operation(oir::CmpPred pred, bool &swap_operands) const {
        swap_operands = false;
        switch (pred) {
        case oir::CmpPred::EQ:
            return mir::RVVOperation::Eq;
        case oir::CmpPred::NE:
            return mir::RVVOperation::Ne;
        case oir::CmpPred::LT:
            return mir::RVVOperation::Lt;
        case oir::CmpPred::LE:
            return mir::RVVOperation::Le;
        case oir::CmpPred::GT:
            swap_operands = true;
            return mir::RVVOperation::Lt;
        case oir::CmpPred::GE:
            swap_operands = true;
            return mir::RVVOperation::Le;
        }
        fail_vector_legalization("unknown vector comparison predicate");
    }

    void lower_vector_compare(const oir::CmpInst &inst) {
        const auto *source_type = dynamic_cast<const oir::VectorType *>(inst.lhs()->type());
        const auto *result_type = dynamic_cast<const oir::VectorType *>(inst.type());
        if (source_type == nullptr || result_type == nullptr || !result_type->is_mask()) {
            fail_vector_legalization("malformed fixed vector comparison: " + inst.print());
        }
        if (is_oversized_fixed_vector(source_type)) {
            auto lhs = value_bundle(inst.lhs());
            auto rhs = value_bundle(inst.rhs());
            auto &destination = bundle_result(inst);
            if (lhs.pieces.size() != rhs.pieces.size() ||
                lhs.pieces.size() != destination.pieces.size()) {
                fail_vector_legalization(
                    "oversized vector comparison operands have different chunk plans");
            }
            bool swap_operands = false;
            const auto operation = lower_compare_operation(inst.pred(), swap_operands);
            for (std::size_t index = 0; index < lhs.pieces.size(); ++index) {
                auto left = lhs.pieces[index];
                auto right = rhs.pieces[index];
                if (swap_operands) {
                    std::swap(left, right);
                }
                const auto type = *left.vector_type;
                ensure_vector_configuration(type);
                mir::MachineVectorInfo info(type);
                info.operation = operation;
                info.avl = mir::MachineVectorAVL::current_vl();
                info.tail_policy = mir::VectorTailPolicy::Agnostic;
                info.mask_policy = mir::VectorMaskPolicy::Agnostic;
                emit_vector_instruction(mir::Opcode::RVVCompareVVTA,
                                        {mir::MachineOperand::reg_def(destination.pieces[index]),
                                         mir::MachineOperand::reg_use(left),
                                         mir::MachineOperand::reg_use(right)},
                                        std::move(info));
            }
            return;
        }
        const auto type = legalize_vector_type(*source_type);
        if (type.is_mask()) {
            fail_vector_legalization("mask comparison is unsupported; use mask logical operations");
        }
        auto lhs = value_reg(inst.lhs());
        auto rhs = value_reg(inst.rhs());
        bool swap_operands = false;
        const auto operation = lower_compare_operation(inst.pred(), swap_operands);
        if (swap_operands) {
            std::swap(lhs, rhs);
        }
        ensure_vector_configuration(type);
        mir::MachineVectorInfo info(type);
        info.operation = operation;
        info.avl = mir::MachineVectorAVL::current_vl();
        info.tail_policy = mir::VectorTailPolicy::Agnostic;
        info.mask_policy = mir::VectorMaskPolicy::Agnostic;
        emit_vector_instruction(mir::Opcode::RVVCompareVVTA,
                                {mir::MachineOperand::reg_def(value_regs_.at(&inst)),
                                 mir::MachineOperand::reg_use(lhs),
                                 mir::MachineOperand::reg_use(rhs)},
                                std::move(info));
    }

    void lower_int_binary(const oir::BinaryInst &inst) {
        if (const auto *vector_type = dynamic_cast<const oir::VectorType *>(inst.type())) {
            lower_vector_int_binary(inst, *vector_type);
            return;
        }
        auto dst = value_regs_.at(&inst);

        auto *lhs_const = constant_int(inst.lhs());
        auto *rhs_const = constant_int(inst.rhs());

        if (inst.op() == oir::Instruction::OpID::Add) {
            if (rhs_const != nullptr && fits_simm12(rhs_const->value())) {
                emit(mir::Opcode::AddIW, {mir::MachineOperand::reg_def(dst),
                                          mir::MachineOperand::reg_use(value_reg(inst.lhs())),
                                          mir::MachineOperand::imm(rhs_const->value())});
                return;
            }
            if (lhs_const != nullptr && fits_simm12(lhs_const->value())) {
                emit(mir::Opcode::AddIW, {mir::MachineOperand::reg_def(dst),
                                          mir::MachineOperand::reg_use(value_reg(inst.rhs())),
                                          mir::MachineOperand::imm(lhs_const->value())});
                return;
            }
        } else if (inst.op() == oir::Instruction::OpID::Sub) {
            if (rhs_const != nullptr && neg_fits_simm12(rhs_const->value())) {
                emit(mir::Opcode::AddIW, {mir::MachineOperand::reg_def(dst),
                                          mir::MachineOperand::reg_use(value_reg(inst.lhs())),
                                          mir::MachineOperand::imm(0 - rhs_const->value())});
                return;
            }
            if (lhs_const != nullptr && lhs_const->value() == 0) {
                emit(mir::Opcode::SubW,
                     {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(zero_reg()),
                      mir::MachineOperand::reg_use(value_reg(inst.rhs()))});
                return;
            }
        } else if (inst.op() == oir::Instruction::OpID::Mul) {
            if (rhs_const != nullptr &&
                try_lower_const_mul(dst, value_reg(inst.lhs()), rhs_const->value())) {
                return;
            }
            if (lhs_const != nullptr &&
                try_lower_const_mul(dst, value_reg(inst.rhs()), lhs_const->value())) {
                return;
            }
        } else if (inst.op() == oir::Instruction::OpID::And) {
            if (rhs_const != nullptr && fits_simm12(rhs_const->value())) {
                emit(mir::Opcode::AndI, {mir::MachineOperand::reg_def(dst),
                                         mir::MachineOperand::reg_use(value_reg(inst.lhs())),
                                         mir::MachineOperand::imm(rhs_const->value())});
                return;
            }
            if (lhs_const != nullptr && fits_simm12(lhs_const->value())) {
                emit(mir::Opcode::AndI, {mir::MachineOperand::reg_def(dst),
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
        if (inst.op() == oir::Instruction::OpID::Or) {
            auto disjoint_bits = create_vreg(dst.value_type);
            emit(mir::Opcode::Xor,
                 {mir::MachineOperand::reg_def(disjoint_bits), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            emit(mir::Opcode::And,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(lhs),
                  mir::MachineOperand::reg_use(rhs)});
            emit(mir::Opcode::Xor,
                 {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(dst),
                  mir::MachineOperand::reg_use(disjoint_bits)});
            return;
        }
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
        if (const auto *vector_type = dynamic_cast<const oir::VectorType *>(inst.type())) {
            lower_vector_float_binary(inst, *vector_type);
            return;
        }
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
        if (inst.lhs()->type() != nullptr && inst.lhs()->type()->is_vector()) {
            lower_vector_compare(inst);
            return;
        }
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
        if (inst.lhs()->type() != nullptr && inst.lhs()->type()->is_vector()) {
            lower_vector_compare(inst);
            return;
        }
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

    void emit_psabi_call_instruction(const std::string &symbol, bool variant_cc,
                                     const std::vector<mir::Register> &argument_registers,
                                     const std::vector<mir::Register> &result_registers) {
        std::vector<mir::MachineOperand> operands;
        operands.push_back(mir::MachineOperand::symbol(symbol));
        for (const auto &reg : argument_registers) {
            operands.push_back(mir::MachineOperand::implicit_reg_use(reg));
        }
        for (const auto &reg : result_registers) {
            operands.push_back(mir::MachineOperand::implicit_reg_def(reg));
        }
        mir::MachineInstr call(mir::Opcode::Call, std::move(operands));
        call.set_variant_cc_call(variant_cc);
        current_block_->add_instr(std::move(call));
    }

    void lower_psabi_call(const oir::CallInst &inst, oir::Function &callee) {
        if (is_standard_runtime_function(callee)) {
            fail_calling_convention(
                "call @" + callee.name(),
                "internal routing error: a standard runtime call reached variant lowering");
        }
        if (callee.function_type()->is_variadic()) {
            fail_calling_convention(
                "call @" + callee.name(),
                "variadic calls are forbidden by psabi-vector compiler lowering");
        }

        const std::string symbol = callee.name();
        current_function_->note_call();
        const auto args = inst.args();
        std::vector<target::CCPSABIVectorValue> actual_values;
        actual_values.reserve(args.size());
        for (auto *arg : args) {
            actual_values.push_back({arg->type(), 1});
        }
        auto assignment = psabi_vector_calling_convention_->assign_call(
            psabi_function_type(*callee.function_type()), actual_values);
        validate_psabi_signature_assignment(assignment, "call @" + callee.name());
        if (assignment.parameters.size() != args.size()) {
            fail_calling_convention("call @" + callee.name(),
                                    "argument assignment count does not match the call");
        }

        struct StagedScalarRegister final {
            mir::Register destination;
            mir::Register value;
        };
        struct StagedVectorRegister final {
            mir::Register destination;
            mir::Register value;
        };
        struct StagedStackArgument final {
            const target::CCLocation *location = nullptr;
            mir::Register value;
            std::string context;
        };
        std::vector<StagedScalarRegister> scalar_registers;
        std::vector<StagedVectorRegister> vector_registers;
        std::vector<StagedStackArgument> stack_arguments;
        std::optional<mir::Register> sret_result_address;

        const auto stage_scalar_location = [&](const target::CCLocation &location,
                                               mir::Register value, std::string context) {
            if (location.kind == target::CCLocationKind::Stack) {
                stack_arguments.push_back({&location, std::move(value), std::move(context)});
                return;
            }
            scalar_registers.push_back({physical_register(location, context), std::move(value)});
        };

        if (assignment.has_sret) {
            const auto &return_value = assignment.return_value.value;
            const auto slot = create_abi_bridge_slot(return_value.type, return_value.size,
                                                     "abi.call." + callee.name() + ".sret." +
                                                         std::to_string(temp_index_++));
            sret_result_address = bridge_address(slot);
            auto staged_pointer = create_vreg(mir::ValueType::Ptr);
            emit_move(staged_pointer, *sret_result_address);
            stage_scalar_location(single_location(return_value), staged_pointer,
                                  "call @" + callee.name() + " hidden sret pointer");
        }

        for (std::size_t index = 0; index < args.size(); ++index) {
            auto *arg = args[index];
            const auto &value_assignment = assignment.parameters[index];
            const auto context = "call @" + callee.name() + " argument " + std::to_string(index);
            if (psabi_direct_vector(value_assignment)) {
                if (is_oversized_fixed_vector(arg->type())) {
                    fail_calling_convention(
                        context, "oversized fixed vector cannot have a direct register assignment");
                }
                vector_registers.push_back(
                    {psabi_vector_register(value_assignment, context), value_reg(arg)});
                continue;
            }
            if (is_fixed_vector_abi_value(arg->type())) {
                if (!value_assignment.value.indirect) {
                    fail_calling_convention(context, "fixed vector fallback is not indirect");
                }
                const auto slot = create_abi_bridge_slot(arg->type(), value_assignment.value.size,
                                                         "abi.call." + callee.name() + ".arg" +
                                                             std::to_string(index) + "." +
                                                             std::to_string(temp_index_++));
                auto address = bridge_address(slot);
                if (is_oversized_fixed_vector(arg->type())) {
                    emit_abi_vector_store(arg->type(), value_bundle(arg), address,
                                          value_assignment.value.size);
                } else {
                    emit_abi_vector_store(arg->type(), value_reg(arg), address,
                                          value_assignment.value.size);
                }
                auto staged_pointer = create_vreg(mir::ValueType::Ptr);
                emit_move(staged_pointer, address);
                stage_scalar_location(single_location(value_assignment.value), staged_pointer,
                                      context);
                continue;
            }

            auto staged = create_vreg(type_info(arg->type()).value_type);
            emit_move(staged, value_reg(arg));
            stage_scalar_location(single_location(value_assignment.value), staged, context);
        }

        current_function_->reserve_outgoing_arg_bytes(assignment.stack_argument_size);
        for (const auto &argument : stack_arguments) {
            emit(mir::Opcode::StoreOutgoingArg,
                 {mir::MachineOperand::reg_use(argument.value),
                  mir::MachineOperand::imm(stack_offset(*argument.location, argument.context)),
                  mir::MachineOperand::type(argument.value.value_type)});
        }

        std::vector<mir::Register> call_argument_registers;
        call_argument_registers.reserve(scalar_registers.size() + vector_registers.size());
        for (const auto &argument : scalar_registers) {
            emit_move(argument.destination, argument.value);
            call_argument_registers.push_back(argument.destination);
        }
        for (const auto &argument : vector_registers) {
            emit_move(argument.destination, argument.value);
            call_argument_registers.push_back(argument.destination);
        }

        std::vector<mir::Register> call_result_registers;
        if (!inst.type()->is_void() && !assignment.has_sret) {
            if (psabi_direct_vector(assignment.return_value)) {
                call_result_registers.push_back(psabi_vector_register(
                    assignment.return_value, "call @" + callee.name() + " return value"));
            } else {
                const auto &return_location = single_location(assignment.return_value.value);
                if (return_location.kind != target::CCLocationKind::Stack) {
                    call_result_registers.push_back(physical_register(
                        return_location, "call @" + callee.name() + " return value"));
                }
            }
        }
        emit_psabi_call_instruction(symbol, true, call_argument_registers, call_result_registers);
        current_vector_config_.reset();
        dominating_scalable_config_.reset();
        current_execution_mask_source_.reset();

        if (inst.type()->is_void()) {
            return;
        }
        const auto &return_assignment = assignment.return_value;
        if (psabi_direct_vector(return_assignment)) {
            emit_move(value_regs_.at(&inst),
                      psabi_vector_register(return_assignment,
                                            "call @" + callee.name() + " return value"));
            return;
        }
        if (is_fixed_vector_abi_value(inst.type())) {
            if (!return_assignment.value.indirect || !sret_result_address.has_value()) {
                fail_calling_convention("call @" + callee.name(),
                                        "fixed vector return lacks caller-owned sret storage");
            }
            if (is_oversized_fixed_vector(inst.type())) {
                emit_abi_vector_load(inst.type(), bundle_result(inst), *sret_result_address);
            } else {
                emit_abi_vector_load(inst.type(), value_regs_.at(&inst), *sret_result_address);
            }
            return;
        }
        emit_move(value_regs_.at(&inst),
                  physical_register(single_location(return_assignment.value),
                                    "call @" + callee.name() + " return value"));
    }

    void lower_call(const oir::CallInst &inst) {
        auto *callee = dynamic_cast<oir::Function *>(inst.callee());
        if (callee == nullptr) {
            throw std::runtime_error("MIR lowering only supports direct calls");
        }

        if (psabi_vector_enabled() && !is_standard_runtime_function(*callee)) {
            lower_psabi_call(inst, *callee);
            return;
        }

        std::string symbol = callee->name();
        current_function_->note_call();

        const auto args = inst.args();
        std::vector<oir::Type *> actual_types;
        actual_types.reserve(args.size());
        for (auto *arg : args) {
            actual_types.push_back(arg->type());
        }
        auto call_assignment =
            calling_convention_.assign_call(*callee->function_type(), actual_types);
        validate_signature_assignment(call_assignment, "call @" + callee->name());
        const bool has_standard_aggregate =
            is_fixed_vector_abi_value(call_assignment.return_value.type) ||
            std::any_of(call_assignment.parameters.begin(), call_assignment.parameters.end(),
                        [&](const target::CCValueAssignment &assignment) {
                            return is_fixed_vector_abi_value(assignment.type);
                        });
        if (has_standard_aggregate) {
            current_function_->note_standard_aggregate_call();
        }
        if (call_assignment.parameters.size() != args.size()) {
            fail_calling_convention("call @" + callee->name(),
                                    "argument assignment count does not match the call");
        }

        struct StagedRegisterArgument final {
            mir::Register destination;
            mir::Register value;
        };
        struct StagedStackArgument final {
            const target::CCLocation *location = nullptr;
            mir::Register value;
            std::string context;
        };
        std::vector<StagedRegisterArgument> staged_register_args;
        std::vector<StagedStackArgument> staged_stack_args;
        std::optional<mir::Register> sret_result_address;

        const auto stage_location = [&](const target::CCLocation &location, mir::Register value,
                                        std::string context) {
            if (location.kind == target::CCLocationKind::Stack) {
                staged_stack_args.push_back({&location, std::move(value), std::move(context)});
                return;
            }
            staged_register_args.push_back(
                {physical_register(location, context), std::move(value)});
        };

        if (symbol == "starttime" || symbol == "stoptime") {
            auto zero = create_vreg(mir::ValueType::I32);
            emit(mir::Opcode::LoadImm,
                 {mir::MachineOperand::reg_def(zero), mir::MachineOperand::imm(0)});
            staged_register_args.push_back({phys_gpr("a0"), zero});
            symbol = symbol == "starttime" ? "_sysy_starttime" : "_sysy_stoptime";
        }

        if (call_assignment.has_sret) {
            const auto &return_assignment = call_assignment.return_value;
            const auto slot = create_abi_bridge_slot(return_assignment.type, return_assignment.size,
                                                     "abi.call." + callee->name() + ".sret." +
                                                         std::to_string(temp_index_++));
            sret_result_address = bridge_address(slot);
            auto staged_pointer = create_vreg(mir::ValueType::Ptr);
            emit_move(staged_pointer, *sret_result_address);
            stage_location(single_location(return_assignment), staged_pointer,
                           "call @" + callee->name() + " hidden sret pointer");
        }

        for (std::size_t index = 0; index < args.size(); ++index) {
            auto *arg = args[index];
            const auto &assignment = call_assignment.parameters[index];
            const auto context = "call @" + callee->name() + " argument " + std::to_string(index);
            if (is_fixed_vector_abi_value(arg->type())) {
                if (portable_fixed_abi_enabled()) {
                    const auto &type = portable_fixed_vector_type(arg->type(), context);
                    const auto address = portable_outbound_aggregate_address(arg, type, context);
                    if (assignment.indirect) {
                        auto staged_pointer = create_vreg(mir::ValueType::Ptr);
                        emit_move(staged_pointer, address);
                        stage_location(single_location(assignment), staged_pointer, context);
                        continue;
                    }
                    for (const auto &location : assignment.locations) {
                        auto piece = emit_raw_piece_load(address, value_offset(location, context));
                        stage_location(location, std::move(piece), context);
                    }
                    continue;
                }
                if (is_oversized_fixed_vector(arg->type())) {
                    auto src = value_bundle(arg);
                    const auto slot = create_abi_bridge_slot(arg->type(), assignment.size,
                                                             "abi.call." + callee->name() + ".arg" +
                                                                 std::to_string(index) + "." +
                                                                 std::to_string(temp_index_++));
                    auto address = bridge_address(slot);
                    emit_abi_vector_store(arg->type(), src, address, assignment.size);
                    if (!assignment.indirect) {
                        for (const auto &location : assignment.locations) {
                            auto piece =
                                emit_raw_piece_load(address, value_offset(location, context));
                            stage_location(location, std::move(piece), context);
                        }
                        continue;
                    }
                    auto staged_pointer = create_vreg(mir::ValueType::Ptr);
                    emit_move(staged_pointer, address);
                    stage_location(single_location(assignment), staged_pointer, context);
                    continue;
                }
                const auto src = value_reg(arg);
                if (assignment.indirect) {
                    const auto slot = create_abi_bridge_slot(arg->type(), assignment.size,
                                                             "abi.call." + callee->name() + ".arg" +
                                                                 std::to_string(index) + "." +
                                                                 std::to_string(temp_index_++));
                    auto address = bridge_address(slot);
                    emit_abi_vector_store(arg->type(), src, address, assignment.size);
                    auto staged_pointer = create_vreg(mir::ValueType::Ptr);
                    emit_move(staged_pointer, address);
                    stage_location(single_location(assignment), staged_pointer, context);
                    continue;
                }

                const auto slot = create_abi_bridge_slot(arg->type(), assignment.size,
                                                         "abi.call." + callee->name() + ".arg" +
                                                             std::to_string(index) + "." +
                                                             std::to_string(temp_index_++));
                const auto address = bridge_address(slot);
                emit_abi_vector_store(arg->type(), src, address, assignment.size);
                for (const auto &location : assignment.locations) {
                    auto piece = emit_raw_piece_load(address, value_offset(location, context));
                    stage_location(location, std::move(piece), context);
                }
                continue;
            }

            const auto src = value_reg(arg);
            const auto type = type_info(arg->type()).value_type;
            auto staged = create_vreg(type);
            emit_move(staged, src);
            stage_location(single_location(assignment), staged, context);
        }

        current_function_->reserve_outgoing_arg_bytes(call_assignment.stack_argument_size);
        for (const auto &argument : staged_stack_args) {
            emit(mir::Opcode::StoreOutgoingArg,
                 {mir::MachineOperand::reg_use(argument.value),
                  mir::MachineOperand::imm(stack_offset(*argument.location, argument.context)),
                  mir::MachineOperand::type(argument.value.value_type)});
        }
        for (const auto &argument : staged_register_args) {
            emit_move(argument.destination, argument.value);
        }
        emit_psabi_call_instruction(symbol, false, {}, {});
        current_vector_config_.reset();
        dominating_scalable_config_.reset();
        current_execution_mask_source_.reset();

        if (!inst.type()->is_void()) {
            const auto &return_assignment = call_assignment.return_value;
            if (is_fixed_vector_abi_value(inst.type())) {
                if (portable_fixed_abi_enabled()) {
                    const auto &type = portable_fixed_vector_type(
                        inst.type(), "call @" + callee->name() + " return value");
                    if (return_assignment.indirect) {
                        if (!sret_result_address.has_value()) {
                            fail_calling_convention(
                                "call @" + callee->name(),
                                "portable fixed-vector return lacks caller-owned "
                                "sret storage");
                        }
                        bind_portable_aggregate_address(&inst, *sret_result_address,
                                                        "call @" + callee->name() +
                                                            " return value");
                        return;
                    }

                    std::vector<mir::Register> pieces;
                    pieces.reserve(return_assignment.locations.size());
                    for (const auto &location : return_assignment.locations) {
                        auto staged = create_vreg(mir::ValueType::Ptr);
                        emit_move(staged, physical_register(location, "call @" + callee->name() +
                                                                          " return value"));
                        pieces.push_back(std::move(staged));
                    }
                    const auto slot =
                        create_abi_bridge_slot(&type, return_assignment.size,
                                               "abi.call." + callee->name() + ".portable.result." +
                                                   std::to_string(temp_index_++));
                    const auto address = bridge_address(slot);
                    for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
                        emit_raw_piece_store(
                            address,
                            value_offset(return_assignment.locations[piece],
                                         "call @" + callee->name() + " return value"),
                            pieces[piece]);
                    }
                    bind_portable_aggregate_address(&inst, address,
                                                    "call @" + callee->name() + " return value");
                    return;
                }
                if (is_oversized_fixed_vector(inst.type())) {
                    auto &destination = bundle_result(inst);
                    if (return_assignment.indirect) {
                        if (!sret_result_address.has_value()) {
                            fail_calling_convention(
                                "call @" + callee->name(),
                                "oversized fixed-vector return lacks caller-owned "
                                "sret storage");
                        }
                        emit_abi_vector_load(inst.type(), destination, *sret_result_address);
                        return;
                    }
                    const auto slot = create_abi_bridge_slot(
                        inst.type(), return_assignment.size,
                        "abi.call." + callee->name() + ".result." + std::to_string(temp_index_++));
                    const auto address = bridge_address(slot);
                    for (const auto &location : return_assignment.locations) {
                        auto staged = create_vreg(mir::ValueType::Ptr);
                        emit_move(staged, physical_register(location, "call @" + callee->name() +
                                                                          " return value"));
                        emit_raw_piece_store(
                            address,
                            value_offset(location, "call @" + callee->name() + " return value"),
                            staged);
                    }
                    emit_abi_vector_load(inst.type(), destination, address);
                    return;
                }
                const auto destination = value_regs_.at(&inst);
                if (return_assignment.indirect) {
                    if (!sret_result_address.has_value()) {
                        fail_calling_convention("call @" + callee->name(),
                                                "missing caller-owned sret temporary");
                    }
                    emit_abi_vector_load(inst.type(), destination, *sret_result_address);
                    return;
                }

                std::vector<mir::Register> pieces;
                pieces.reserve(return_assignment.locations.size());
                for (const auto &location : return_assignment.locations) {
                    auto staged = create_vreg(mir::ValueType::Ptr);
                    emit_move(staged, physical_register(location, "call @" + callee->name() +
                                                                      " return value"));
                    pieces.push_back(std::move(staged));
                }
                const auto slot = create_abi_bridge_slot(inst.type(), return_assignment.size,
                                                         "abi.call." + callee->name() + ".result." +
                                                             std::to_string(temp_index_++));
                const auto address = bridge_address(slot);
                for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
                    emit_raw_piece_store(address,
                                         value_offset(return_assignment.locations[piece],
                                                      "call @" + callee->name() + " return value"),
                                         pieces[piece]);
                }
                emit_abi_vector_load(inst.type(), destination, address);
                return;
            }
            const auto &location = single_location(return_assignment);
            emit_move(value_regs_.at(&inst),
                      physical_register(location, "call @" + callee->name() + " return value"));
        }
    }

    void lower_psabi_return(const oir::ReturnInst &inst) {
        if (inst.has_value()) {
            const auto &assignment = current_psabi_cc_assignment_.return_value;
            if (psabi_direct_vector(assignment)) {
                emit_move(psabi_vector_register(assignment, "function return value"),
                          value_reg(inst.value()));
            } else if (is_fixed_vector_abi_value(inst.value()->type())) {
                if (!assignment.value.indirect || !current_sret_pointer_.has_value()) {
                    fail_calling_convention(
                        "function return value",
                        "fixed vector indirect return lacks hidden sret pointer");
                }
                if (is_oversized_fixed_vector(inst.value()->type())) {
                    emit_abi_vector_store(inst.value()->type(), value_bundle(inst.value()),
                                          *current_sret_pointer_, assignment.value.size);
                } else {
                    emit_abi_vector_store(inst.value()->type(), value_reg(inst.value()),
                                          *current_sret_pointer_, assignment.value.size);
                }
            } else {
                emit_move(
                    physical_register(single_location(assignment.value), "function return value"),
                    value_reg(inst.value()));
            }
        }
        emit(mir::Opcode::Jump, {mir::MachineOperand::block("epilogue")});
    }

    void lower_return(const oir::ReturnInst &inst) {
        if (psabi_vector_enabled()) {
            lower_psabi_return(inst);
            return;
        }
        if (inst.has_value()) {
            const auto &assignment = current_cc_assignment_.return_value;
            if (is_fixed_vector_abi_value(inst.value()->type())) {
                if (portable_fixed_abi_enabled()) {
                    const auto &type =
                        portable_fixed_vector_type(inst.value()->type(), "function return value");
                    const auto address = portable_outbound_aggregate_address(
                        inst.value(), type, "function return value");
                    if (assignment.indirect) {
                        if (!current_sret_pointer_.has_value()) {
                            fail_calling_convention("function return value",
                                                    "portable fixed-vector return lacks the staged "
                                                    "hidden sret pointer");
                        }
                        emit_portable_aggregate_copy(type, *current_sret_pointer_, address,
                                                     "function return value");
                    } else {
                        std::vector<mir::Register> pieces;
                        pieces.reserve(assignment.locations.size());
                        for (const auto &location : assignment.locations) {
                            pieces.push_back(emit_raw_piece_load(
                                address, value_offset(location, "function return value")));
                        }
                        for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
                            emit_move(physical_register(assignment.locations[piece],
                                                        "function return value"),
                                      pieces[piece]);
                        }
                    }
                    emit(mir::Opcode::Jump, {mir::MachineOperand::block("epilogue")});
                    return;
                }
                if (is_oversized_fixed_vector(inst.value()->type())) {
                    auto value = value_bundle(inst.value());
                    if (assignment.indirect) {
                        if (!current_sret_pointer_.has_value()) {
                            fail_calling_convention(
                                "function return value",
                                "oversized fixed-vector return lacks hidden sret "
                                "pointer");
                        }
                        emit_abi_vector_store(inst.value()->type(), value, *current_sret_pointer_,
                                              assignment.size);
                    } else {
                        const auto slot =
                            create_abi_bridge_slot(inst.value()->type(), assignment.size,
                                                   "abi.return." + current_function_->name() + "." +
                                                       std::to_string(temp_index_++));
                        const auto address = bridge_address(slot);
                        emit_abi_vector_store(inst.value()->type(), value, address,
                                              assignment.size);
                        for (const auto &location : assignment.locations) {
                            emit_move(
                                physical_register(location, "function return value"),
                                emit_raw_piece_load(
                                    address, value_offset(location, "function return value")));
                        }
                    }
                    emit(mir::Opcode::Jump, {mir::MachineOperand::block("epilogue")});
                    return;
                }
                auto value = value_reg(inst.value());
                if (assignment.indirect) {
                    if (!current_sret_pointer_.has_value()) {
                        fail_calling_convention("function return value",
                                                "missing staged hidden sret pointer");
                    }
                    emit_abi_vector_store(inst.value()->type(), value, *current_sret_pointer_,
                                          assignment.size);
                } else {
                    const auto slot =
                        create_abi_bridge_slot(inst.value()->type(), assignment.size,
                                               "abi.return." + current_function_->name() + "." +
                                                   std::to_string(temp_index_++));
                    const auto address = bridge_address(slot);
                    emit_abi_vector_store(inst.value()->type(), value, address, assignment.size);
                    std::vector<mir::Register> pieces;
                    pieces.reserve(assignment.locations.size());
                    for (const auto &location : assignment.locations) {
                        pieces.push_back(emit_raw_piece_load(
                            address, value_offset(location, "function return value")));
                    }
                    for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
                        emit_move(
                            physical_register(assignment.locations[piece], "function return value"),
                            pieces[piece]);
                    }
                }
            } else {
                auto value = value_reg(inst.value());
                const auto &location = single_location(assignment);
                emit_move(physical_register(location, "function return value"), value);
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
            current_vector_config_.reset();
            dominating_scalable_config_.reset();
            current_execution_mask_source_.reset();
            block_mask_constant_regs_.clear();
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
            if (is_oversized_fixed_vector(phi->type())) {
                auto &destination = bundle_result(*phi);
                auto source = value_bundle(incoming);
                if (destination.pieces.size() != source.pieces.size()) {
                    fail_vector_legalization("oversized phi incoming has a different chunk plan");
                }
                for (std::size_t index = 0; index < destination.pieces.size(); ++index) {
                    if (destination.pieces[index] != source.pieces[index]) {
                        copies.push_back({destination.pieces[index], source.pieces[index]});
                    }
                }
                continue;
            }
            auto dst = value_regs_.at(phi);
            auto src = value_reg(incoming);
            if (dst != src) {
                copies.push_back({dst, src});
            }
        }

        while (!copies.empty()) {
            auto acyclic = std::find_if(copies.begin(), copies.end(), [&](const auto &copy) {
                return std::none_of(copies.begin(), copies.end(),
                                    [&](const auto &other) { return other.src == copy.dst; });
            });

            if (acyclic != copies.end()) {
                emit_move(acyclic->dst, acyclic->src);
                copies.erase(acyclic);
                continue;
            }

            auto &cycle = copies.front();
            auto temp = cycle.dst.is_vector() ? create_vector_vreg(*cycle.dst.vector_type)
                                              : create_vreg(cycle.dst.value_type);
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

    FixedVectorBundle &bundle_result(const oir::Value &value) {
        auto found = value_bundles_.find(&value);
        if (found == value_bundles_.end()) {
            fail_vector_legalization("missing oversized fixed-vector result bundle for " +
                                     value.print());
        }
        return found->second;
    }

    FixedVectorBundle materialize_fixed_bundle_constant(oir::Value &value) {
        auto *logical_type = dynamic_cast<oir::VectorType *>(value.type());
        if (logical_type == nullptr || !is_oversized_fixed_vector(logical_type)) {
            fail_vector_legalization(
                "fixed bundle constant does not have an oversized vector type");
        }
        auto bundle = allocate_fixed_bundle(*logical_type);

        if (dynamic_cast<oir::ConstantZero *>(&value) != nullptr ||
            dynamic_cast<oir::UndefValue *>(&value) != nullptr) {
            for (const auto &destination : bundle.pieces) {
                const auto type = *destination.vector_type;
                const auto config = data_config_for(type);
                ensure_vector_configuration(config);
                mir::MachineVectorInfo info(config);
                info.avl = mir::MachineVectorAVL::current_vl();
                info.tail_policy = mir::VectorTailPolicy::Agnostic;
                info.mask_policy = mir::VectorMaskPolicy::Agnostic;
                if (type.is_mask()) {
                    info.operation = mir::RVVOperation::MaskClear;
                    emit_vector_instruction(mir::Opcode::RVVMaskClear,
                                            {mir::MachineOperand::reg_def(destination)},
                                            std::move(info));
                    continue;
                }
                info.operation = mir::RVVOperation::Splat;
                if (type.element_type() == mir::ValueType::F32) {
                    auto scalar = create_vreg(mir::ValueType::F32);
                    emit(mir::Opcode::LoadFloatImm, {mir::MachineOperand::reg_def(scalar),
                                                     mir::MachineOperand::float_imm(0.0F)});
                    emit_vector_instruction(mir::Opcode::RVVSplatVFTA,
                                            {mir::MachineOperand::reg_def(destination),
                                             mir::MachineOperand::reg_use(scalar)},
                                            std::move(info));
                } else {
                    emit_vector_instruction(
                        mir::Opcode::RVVSplatVITA,
                        {mir::MachineOperand::reg_def(destination), mir::MachineOperand::imm(0)},
                        std::move(info));
                }
            }
            return bundle;
        }

        mir::Global pool;
        pool.name = next_constant_pool_symbol();
        pool.type = type_info(logical_type);
        pool.is_const = true;
        if (auto *constant = dynamic_cast<oir::ConstantVector *>(&value)) {
            pool.initializer_bytes = encode_fixed_vector_constant(*constant);
        } else if (auto *constant = dynamic_cast<oir::ConstantMask *>(&value)) {
            pool.initializer_bytes = constant->packed_bits();
        } else {
            fail_vector_legalization("unsupported oversized fixed-vector constant " +
                                     value.print());
        }
        if (pool.initializer_bytes.size() != pool.type.size) {
            fail_vector_legalization(
                "oversized fixed-vector constant byte count disagrees with layout");
        }
        const auto symbol = pool.name;
        module_->add_global(std::move(pool));
        auto address = create_vreg(mir::ValueType::Ptr);
        emit(mir::Opcode::LoadGlobalAddr,
             {mir::MachineOperand::reg_def(address), mir::MachineOperand::global(symbol)});
        emit_fixed_bundle_load(bundle, address);
        return bundle;
    }

    FixedVectorBundle value_bundle(oir::Value *value) {
        if (dynamic_cast<oir::ConstantVector *>(value) != nullptr ||
            dynamic_cast<oir::ConstantMask *>(value) != nullptr ||
            dynamic_cast<oir::ConstantZero *>(value) != nullptr ||
            dynamic_cast<oir::UndefValue *>(value) != nullptr) {
            return materialize_fixed_bundle_constant(*value);
        }
        auto found = value_bundles_.find(value);
        if (found == value_bundles_.end()) {
            fail_vector_legalization("MIR lowering cannot find fixed bundle for value: " +
                                     value->print());
        }
        return found->second;
    }

    mir::Register value_reg(oir::Value *value) {
        if (is_oversized_fixed_vector(value->type())) {
            fail_vector_legalization(
                "oversized fixed vector was used through a single-register lowering path: " +
                value->print());
        }
        if (auto *constant = dynamic_cast<oir::ConstantMask *>(value)) {
            return materialize_constant_mask(*constant);
        }
        if (auto *constant = dynamic_cast<oir::ConstantVector *>(value)) {
            return materialize_constant_vector(*constant);
        }
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
            if (value->type() != nullptr && value->type()->is_vector()) {
                return materialize_zero_vector(*value);
            }
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
            emit(mir::Opcode::LoadGlobalAddr, {mir::MachineOperand::reg_def(reg),
                                               mir::MachineOperand::global(function->name())});
            return reg;
        }

        auto found = value_regs_.find(value);
        if (found == value_regs_.end()) {
            throw std::runtime_error("MIR lowering cannot find vreg for value: " + value->print());
        }
        return found->second;
    }

    void emit_move(mir::Register dst, mir::Register src) {
        if (dst.is_vector() || src.is_vector()) {
            if (!dst.is_vector() || !src.is_vector() || dst.vector_type != src.vector_type) {
                fail_vector_legalization(
                    "vector copy requires identical source and destination types");
            }
            const auto config = data_config_for(*dst.vector_type);
            if (config.is_scalable() && !dominating_scalable_config_.has_value()) {
                // Phi copies are emitted in synthetic edge blocks, outside the
                // lexical block containing the SetVL that defines the value.
                // A full-register copy is independent of that predecessor's
                // active EVL, so establish VLMAX locally before copying it.
                emit_vlmax_setvli(config);
            }
            ensure_vector_configuration(config);
            mir::MachineVectorInfo info(config);
            info.operation = mir::RVVOperation::Copy;
            info.avl = mir::MachineVectorAVL::current_vl();
            info.tail_policy = mir::VectorTailPolicy::Agnostic;
            info.mask_policy = mir::VectorMaskPolicy::Agnostic;
            emit_vector_instruction(
                dst.vector_type->is_mask() ? mir::Opcode::RVVMaskCopy : mir::Opcode::RVVVectorCopy,
                {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(std::move(src))},
                std::move(info));
            return;
        }
        mir::Opcode opcode;
        if (dst.reg_class == src.reg_class) {
            opcode =
                dst.reg_class == mir::RegisterClass::FPR32 ? mir::Opcode::FMove : mir::Opcode::Move;
        } else if (dst.reg_class == mir::RegisterClass::FPR32) {
            opcode = mir::Opcode::FmvWX;
        } else {
            opcode = mir::Opcode::FmvXW;
        }
        emit(opcode,
             {mir::MachineOperand::reg_def(dst), mir::MachineOperand::reg_use(std::move(src))});
    }

    void emit_entry_move(mir::Register dst, mir::Register src,
                         const std::vector<mir::Register> &future_arg_regs) {
        mir::Opcode opcode;
        if (dst.reg_class == src.reg_class) {
            opcode =
                dst.reg_class == mir::RegisterClass::FPR32 ? mir::Opcode::FMove : mir::Opcode::Move;
        } else if (dst.reg_class == mir::RegisterClass::FPR32) {
            opcode = mir::Opcode::FmvWX;
        } else {
            opcode = mir::Opcode::FmvXW;
        }
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
    target::TargetProfile profile_;
    target::RISCVCallingConvention calling_convention_;
    std::optional<target::RISCVPSABIVectorCallingConvention> psabi_vector_calling_convention_;
    target::CCSignatureAssignment current_cc_assignment_;
    target::CCPSABIVectorSignatureAssignment current_psabi_cc_assignment_;
    mir::MachineFunction *current_function_ = nullptr;
    mir::MachineBasicBlock *current_block_ = nullptr;
    unsigned temp_index_ = 0;
    unsigned constant_pool_index_ = 0;
    std::unordered_map<const oir::Function *, mir::MachineFunction *> functions_;
    std::unordered_map<const oir::BasicBlock *, mir::MachineBasicBlock *> blocks_;
    std::unordered_map<std::string, mir::MachineBasicBlock *> edge_blocks_;
    std::vector<PendingEdgeBlock> pending_edge_blocks_;
    std::unordered_map<const oir::Instruction *, IntRange> value_ranges_;
    std::unordered_map<const oir::BasicBlock *, RangeMap> block_entry_ranges_;
    RangeMap current_ranges_;
    std::optional<ActiveVectorConfiguration> current_vector_config_;
    std::optional<DominatingScalableConfiguration> dominating_scalable_config_;
    std::optional<mir::Register> current_execution_mask_source_;
    std::optional<mir::Register> current_sret_pointer_;
    std::unordered_map<std::string, mir::Register> block_mask_constant_regs_;
    std::unordered_map<const oir::Value *, mir::Register> value_regs_;
    std::unordered_map<const oir::Value *, FixedVectorBundle> value_bundles_;
    std::unordered_map<const oir::Value *, mir::Register> portable_aggregate_addresses_;
    std::unordered_map<const oir::Instruction *, int> alloca_slots_;
    std::unordered_map<const oir::Instruction *, const oir::Instruction *>
        segment2_candidates_;
    std::unordered_set<const oir::Instruction *> consumed_segment2_fields_;
};

} // namespace

std::unique_ptr<mir::Module> lower_with_vregs(const oir::Module &module,
                                              const target::TargetProfile &profile) {
    VRegLowerer lowerer(profile);
    return lowerer.lower(module);
}

} // namespace pass::oir_to_mir
