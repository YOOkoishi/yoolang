#include "pass/oir/RVVTargetCostModel.h"

#include <algorithm>
#include <array>
#include <limits>
#include <ostream>
#include <utility>

namespace pass::oir_vectorize {
namespace {

constexpr std::uint64_t kCostMax = std::numeric_limits<std::uint64_t>::max();

std::uint64_t add_sat(std::uint64_t lhs, std::uint64_t rhs) {
    return rhs > kCostMax - lhs ? kCostMax : lhs + rhs;
}

std::uint64_t mul_sat(std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs == 0 || rhs == 0) {
        return 0;
    }
    return lhs > kCostMax / rhs ? kCostMax : lhs * rhs;
}

std::uint64_t ceil_div(std::uint64_t numerator, std::uint64_t denominator) {
    if (denominator == 0) {
        return kCostMax;
    }
    return numerator / denominator + (numerator % denominator != 0);
}

std::uint64_t nonnegative(int value) {
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

unsigned add_unsigned_sat(unsigned lhs, unsigned rhs) {
    return rhs > std::numeric_limits<unsigned>::max() - lhs ? std::numeric_limits<unsigned>::max()
                                                            : lhs + rhs;
}

unsigned mul_unsigned_sat(unsigned lhs, unsigned rhs) {
    if (lhs == 0 || rhs == 0) {
        return 0;
    }
    return lhs > std::numeric_limits<unsigned>::max() / rhs ? std::numeric_limits<unsigned>::max()
                                                            : lhs * rhs;
}

void print_json_string(std::ostream &out, std::string_view value) {
    out << '"';
    for (char ch : value) {
        switch (ch) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            out << ch;
            break;
        }
    }
    out << '"';
}

struct LMULChoice final {
    const char *name;
    unsigned numerator;
    unsigned denominator;
};

constexpr std::array<LMULChoice, 5> kLMULChoices = {
    LMULChoice{"mf2", 1, 2}, LMULChoice{"m1", 1, 1}, LMULChoice{"m2", 2, 1},
    LMULChoice{"m4", 4, 1},  LMULChoice{"m8", 8, 1},
};

bool is_power_of_two(unsigned value) {
    return value != 0 && (value & (value - 1U)) == 0;
}

unsigned physical_registers_for_lmul(const LMULChoice &choice) {
    return choice.numerator < choice.denominator ? 1U : choice.numerator / choice.denominator;
}

std::uint64_t memory_vector_cost(const RVVMemoryCostOp &operation,
                                 const target::TargetTuning &tuning) {
    switch (operation.kind) {
    case RVVMemoryAccessKind::UnitStride:
        return nonnegative(operation.is_store ? tuning.vector_store_cost : tuning.vector_load_cost);
    case RVVMemoryAccessKind::Strided:
        return nonnegative(operation.is_store ? tuning.vector_strided_store_cost
                                              : tuning.vector_strided_load_cost);
    case RVVMemoryAccessKind::Indexed:
        return nonnegative(operation.is_store ? tuning.vector_indexed_store_cost
                                              : tuning.vector_indexed_load_cost);
    case RVVMemoryAccessKind::Segment:
        return add_sat(
            nonnegative(tuning.vector_segment_base_cost),
            mul_sat(operation.segment_fields, nonnegative(tuning.vector_segment_field_cost)));
    }
    return kCostMax;
}

std::uint64_t scalar_lane_cost(const RVVLoopCostInput &input, const target::TargetTuning &tuning) {
    std::uint64_t cost = mul_sat(input.scalar_alu_operations, nonnegative(tuning.scalar_alu_cost));
    cost = add_sat(cost, mul_sat(input.reduction_operations, nonnegative(tuning.scalar_alu_cost)));
    // A vector mask operation corresponds to a scalar condition or select on
    // the original path.  Branch unpredictability is intentionally not
    // assumed without profile data.
    cost = add_sat(cost, mul_sat(input.mask_operations, nonnegative(tuning.scalar_alu_cost)));
    for (const auto &operation : input.memory_operations) {
        const unsigned fields =
            operation.kind == RVVMemoryAccessKind::Segment ? operation.segment_fields : 1U;
        const auto access_cost =
            nonnegative(operation.is_store ? tuning.scalar_store_cost : tuning.scalar_load_cost);
        cost = add_sat(cost, mul_sat(fields, access_cost));
    }
    cost = add_sat(cost, nonnegative(tuning.scalar_alu_cost));
    cost = add_sat(cost, nonnegative(tuning.scalar_branch_cost));
    return cost;
}

std::uint64_t runtime_alias_setup_cost(const RVVLoopCostInput &input,
                                       const target::TargetTuning &tuning) {
    // Each checked pair forms two ranges, calls/combines the pure predicate,
    // and contributes to one fast/slow branch.  This is a conservative local
    // proxy; legality remains responsible for constructing the ranges.
    const auto per_check = add_sat(mul_sat(6, nonnegative(tuning.scalar_alu_cost)),
                                   nonnegative(tuning.scalar_branch_cost));
    return mul_sat(input.runtime_alias_checks, per_check);
}

std::uint64_t estimated_scalar_loop_bytes(const RVVLoopCostInput &input) {
    if (input.scalar_loop_code_bytes != 0) {
        return input.scalar_loop_code_bytes;
    }
    std::uint64_t instructions = input.scalar_alu_operations;
    instructions = add_sat(instructions, input.mask_operations);
    instructions = add_sat(instructions, input.reduction_operations);
    for (const auto &operation : input.memory_operations) {
        instructions =
            add_sat(instructions,
                    operation.kind == RVVMemoryAccessKind::Segment ? operation.segment_fields : 1U);
    }
    instructions = add_sat(instructions, 3U); // induction, compare, branch
    return mul_sat(instructions, 4U);
}

std::uint64_t estimate_vector_code_bytes(const RVVLoopCostInput &input, unsigned interleave) {
    std::uint64_t chunk_instructions = input.vector_alu_operations;
    chunk_instructions = add_sat(chunk_instructions, input.memory_operations.size());
    chunk_instructions = add_sat(chunk_instructions, input.mask_operations);
    chunk_instructions = add_sat(chunk_instructions, input.reduction_operations);
    chunk_instructions = add_sat(chunk_instructions, mul_sat(input.distinct_index_vectors, 2U));
    chunk_instructions = add_sat(chunk_instructions, input.needs_tail_mask ? 1U : 0U);
    chunk_instructions = add_sat(chunk_instructions, 1U); // one setvl per chunk
    auto instructions = mul_sat(chunk_instructions, interleave);
    instructions = add_sat(instructions, 3U); // induction/remaining update and branch
    if (interleave > 1) {
        // remaining1, combined VL and the second scalar IV/address base.  This
        // is intentionally conservative for loops with several addresses.
        instructions = add_sat(instructions, mul_sat(interleave - 1U, 3U));
    }
    instructions = add_sat(instructions, mul_sat(input.runtime_alias_checks, 7U));
    auto bytes = mul_sat(instructions, 4U);
    if (input.runtime_alias_checks != 0) {
        bytes = add_sat(bytes, estimated_scalar_loop_bytes(input));
    }
    return bytes;
}

std::uint64_t code_growth_cost(const RVVLoopCostInput &input, const target::TargetTuning &tuning,
                               std::uint64_t vector_code_bytes) {
    const auto scalar_bytes = estimated_scalar_loop_bytes(input);
    const auto growth = vector_code_bytes > scalar_bytes ? vector_code_bytes - scalar_bytes : 0U;
    return mul_sat(ceil_div(growth, 4U), nonnegative(tuning.code_size_cost));
}

std::uint64_t break_even_trip_count(std::uint64_t scalar_per_lane,
                                    std::uint64_t lanes_per_iteration,
                                    std::uint64_t vector_body_cost,
                                    std::uint64_t fixed_vector_cost) {
    if (scalar_per_lane == 0 || lanes_per_iteration == 0) {
        return 0;
    }
    const auto scalar_per_full_vector = mul_sat(scalar_per_lane, lanes_per_iteration);
    if (scalar_per_full_vector <= vector_body_cost) {
        return 0;
    }
    const auto full_vector_gain = scalar_per_full_vector - vector_body_cost;
    auto vector_iterations = add_sat(fixed_vector_cost / full_vector_gain, 1U);
    if (vector_iterations == kCostMax) {
        return 0;
    }
    const auto vector_total =
        add_sat(fixed_vector_cost, mul_sat(vector_iterations, vector_body_cost));
    const auto chunk_low = add_sat(mul_sat(vector_iterations - 1U, lanes_per_iteration), 1U);
    const auto first_profitable = add_sat(vector_total / scalar_per_lane, 1U);
    const auto result = std::max(chunk_low, first_profitable);
    return result <= mul_sat(vector_iterations, lanes_per_iteration) ? result : 0U;
}

bool better_candidate(const RVVCostCandidate &candidate, const RVVCostCandidate &best) {
    if (candidate.total_cost != best.total_cost) {
        return candidate.total_cost < best.total_cost;
    }
    if (candidate.predicted_spill_registers != best.predicted_spill_registers) {
        return candidate.predicted_spill_registers < best.predicted_spill_registers;
    }
    if (candidate.estimated_vector_registers != best.estimated_vector_registers) {
        return candidate.estimated_vector_registers < best.estimated_vector_registers;
    }
    if (candidate.interleave != best.interleave) {
        // Equal cost is not enough evidence to duplicate the body.
        return candidate.interleave < best.interleave;
    }
    return candidate.minimum_lanes < best.minimum_lanes;
}

} // namespace

std::string_view rvv_cost_reject_reason_name(RVVCostRejectReason reason) {
    switch (reason) {
    case RVVCostRejectReason::None:
        return "NONE";
    case RVVCostRejectReason::TargetUnsupported:
        return "TARGET_UNSUPPORTED";
    case RVVCostRejectReason::InvalidInput:
        return "INVALID_INPUT";
    case RVVCostRejectReason::NoLegalLMUL:
        return "NO_LEGAL_LMUL";
    case RVVCostRejectReason::ShortTrip:
        return "SHORT_TRIP";
    case RVVCostRejectReason::NotProfitable:
        return "NOT_PROFITABLE";
    }
    return "INVALID_INPUT";
}

std::string_view rvv_memory_access_kind_name(RVVMemoryAccessKind kind) {
    switch (kind) {
    case RVVMemoryAccessKind::UnitStride:
        return "unit";
    case RVVMemoryAccessKind::Strided:
        return "strided";
    case RVVMemoryAccessKind::Indexed:
        return "indexed";
    case RVVMemoryAccessKind::Segment:
        return "segment";
    }
    return "unit";
}

RVVTargetCostModel::RVVTargetCostModel(target::TargetProfile target,
                                       RVVTargetCostModelOptions options)
    : target_(std::move(target)), options_(options) {
}

RVVCostDecision RVVTargetCostModel::choose(const RVVLoopCostInput &input) const {
    RVVCostDecision decision;
    decision.evaluated_trip_count =
        input.trip_count_known ? input.trip_count : options_.expected_trip_count;

    if (!target_.has_vector() ||
        !target_.supports_vector_element(input.element_is_float, input.element_bits)) {
        decision.reject_reason = RVVCostRejectReason::TargetUnsupported;
        decision.explanation = "target does not support the requested RVV element type";
        return decision;
    }
    if (!is_power_of_two(input.element_bits) || input.element_bits == 0 ||
        input.element_bits > 64 || options_.expected_trip_count == 0) {
        decision.reject_reason = RVVCostRejectReason::InvalidInput;
        decision.explanation = "invalid element width or expected trip count";
        return decision;
    }
    for (const auto &operation : input.memory_operations) {
        const bool valid_fields = operation.kind == RVVMemoryAccessKind::Segment
                                      ? operation.segment_fields >= 2
                                      : operation.segment_fields == 1;
        if (!valid_fields) {
            decision.reject_reason = RVVCostRejectReason::InvalidInput;
            decision.explanation = "segment fields must be >= 2 and non-segment fields must be 1";
            return decision;
        }
    }

    const auto &tuning = target_.tuning;
    const bool factor_two_requested =
        options_.explore_interleave && options_.requested_max_interleave >= 2;
    const bool factor_two_target = tuning.maximum_interleave_factor >= 2;
    const bool evaluate_factor_two =
        factor_two_requested && factor_two_target && input.interleave_factor_two_legal;
    if (factor_two_requested && !factor_two_target) {
        decision.interleave_capability_gate =
            "INTERLEAVE_FACTOR_2_TARGET_UNAVAILABLE: target tuning permits factor 1 only";
    } else if (factor_two_requested && !input.interleave_factor_two_legal) {
        decision.interleave_capability_gate =
            "INTERLEAVE_FACTOR_2_RECIPE_UNAVAILABLE: " +
            (input.interleave_factor_two_rejection.empty()
                 ? std::string("OIR planner did not prove two independent VLA chunks")
                 : input.interleave_factor_two_rejection);
    }

    const auto scalar_per_lane = scalar_lane_cost(input, tuning);
    decision.estimated_scalar_cost = mul_sat(decision.evaluated_trip_count, scalar_per_lane);
    const auto alias_setup = runtime_alias_setup_cost(input, tuning);

    bool found = false;
    for (const auto &lmul : kLMULChoices) {
        for (unsigned interleave : {1U, 2U}) {
            if (interleave == 2 && !evaluate_factor_two) {
                continue;
            }
            RVVCostCandidate candidate;
            candidate.lmul = lmul.name;
            candidate.lmul_numerator = lmul.numerator;
            candidate.lmul_denominator = lmul.denominator;
            candidate.interleave = interleave;
            const auto vector_code_bytes = estimate_vector_code_bytes(input, interleave);
            candidate.estimated_code_bytes = vector_code_bytes;
            candidate.code_size_cost = code_growth_cost(input, tuning, vector_code_bytes);

            const unsigned integral_lmul = lmul.numerator / lmul.denominator;
            if (integral_lmul > tuning.maximum_lmul) {
                candidate.rejection = "LMUL exceeds target tuning maximum";
                decision.candidates.push_back(std::move(candidate));
                continue;
            }
            // Mask ratio is SEW/LMUL and RVV 1.0 admits ratios only through 64.
            if (mul_sat(input.element_bits, lmul.denominator) > mul_sat(64U, lmul.numerator)) {
                candidate.rejection = "LMUL would require an unsupported mask ratio";
                decision.candidates.push_back(std::move(candidate));
                continue;
            }
            const auto base_lanes = target_.minimum_vlen_bits / input.element_bits;
            const auto lanes = mul_sat(base_lanes, lmul.numerator) / lmul.denominator;
            if (lanes == 0 || lanes > std::numeric_limits<unsigned>::max()) {
                candidate.rejection = "minimum VLEN cannot hold one element at this LMUL";
                decision.candidates.push_back(std::move(candidate));
                continue;
            }
            candidate.minimum_lanes = static_cast<unsigned>(lanes);

            const auto group_registers = physical_registers_for_lmul(lmul);
            unsigned required_registers =
                mul_unsigned_sat(std::max(2U, input.live_vector_values), group_registers);
            required_registers = add_unsigned_sat(
                required_registers, mul_unsigned_sat(input.reduction_operations, group_registers));
            required_registers =
                add_unsigned_sat(required_registers,
                                 mul_unsigned_sat(input.distinct_index_vectors, group_registers));
            unsigned maximum_segment_fields = 0;
            for (const auto &operation : input.memory_operations) {
                if (operation.kind == RVVMemoryAccessKind::Segment) {
                    maximum_segment_fields =
                        std::max(maximum_segment_fields, operation.segment_fields);
                }
            }
            if (maximum_segment_fields > 1) {
                required_registers = add_unsigned_sat(
                    required_registers,
                    mul_unsigned_sat(maximum_segment_fields - 1U, group_registers));
            }
            if (input.needs_tail_mask || input.mask_operations != 0) {
                required_registers = add_unsigned_sat(required_registers, 1U);
            }
            // Two chunks are emitted in program order, but the scheduler may
            // overlap the tail of group zero with address/setup values for
            // group one.  Charge one conservative register group per extra
            // chunk instead of pretending pressure is unchanged or doubled.
            required_registers = add_unsigned_sat(
                required_registers, mul_unsigned_sat(interleave - 1U, group_registers));
            candidate.estimated_vector_registers = required_registers;
            const auto minimum_instruction_registers =
                add_unsigned_sat(mul_unsigned_sat(2U, group_registers),
                                 input.needs_tail_mask || input.mask_operations != 0 ? 1U : 0U);
            if (minimum_instruction_registers > tuning.available_vector_registers) {
                candidate.rejection =
                    "LMUL leaves no allocatable register group for an instruction";
                decision.candidates.push_back(std::move(candidate));
                continue;
            }
            candidate.predicted_spill_registers =
                required_registers > tuning.available_vector_registers
                    ? required_registers - tuning.available_vector_registers
                    : 0U;
            candidate.estimated_code_bytes =
                add_sat(vector_code_bytes,
                        mul_sat(mul_sat(candidate.predicted_spill_registers, interleave), 8U));
            candidate.code_size_cost =
                code_growth_cost(input, tuning, candidate.estimated_code_bytes);

            std::uint64_t chunk_cost =
                mul_sat(input.vector_alu_operations, nonnegative(tuning.vector_alu_cost));
            chunk_cost = add_sat(
                chunk_cost, mul_sat(input.mask_operations, nonnegative(tuning.vector_mask_cost)));
            if (input.needs_tail_mask) {
                chunk_cost = add_sat(chunk_cost, nonnegative(tuning.vector_mask_cost));
            }
            chunk_cost = add_sat(chunk_cost, mul_sat(input.reduction_operations,
                                                     nonnegative(tuning.vector_reduction_cost)));
            std::uint64_t interleave_overlap_basis = 0;
            for (const auto &operation : input.memory_operations) {
                const auto operation_cost = memory_vector_cost(operation, tuning);
                chunk_cost = add_sat(chunk_cost, operation_cost);
                // Independent unit/indexed memory operations from the two
                // chunks may occupy the load/store pipelines concurrently.
                // Sum the charged memory issue costs, then cap the credit
                // below by the complete charged chunk cost.
                interleave_overlap_basis = add_sat(interleave_overlap_basis, operation_cost);
            }
            chunk_cost = add_sat(chunk_cost, mul_sat(input.distinct_index_vectors,
                                                     nonnegative(tuning.vector_index_setup_cost)));
            chunk_cost = add_sat(chunk_cost, nonnegative(tuning.vsetvl_cost));
            std::uint64_t vector_body_cost = mul_sat(chunk_cost, interleave);
            vector_body_cost = add_sat(vector_body_cost, nonnegative(tuning.scalar_alu_cost));
            vector_body_cost = add_sat(vector_body_cost, nonnegative(tuning.scalar_branch_cost));
            if (interleave > 1) {
                if (interleave_overlap_basis == 0) {
                    interleave_overlap_basis = nonnegative(tuning.vector_alu_cost);
                }
                // The two chunks are independent and issued in program order,
                // allowing the charged vector memory pipeline work (or one
                // ALU-only latency) to overlap.  Charge every operation first;
                // this bounded credit is the only throughput benefit assigned
                // to factor two.
                candidate.interleave_overlap_credit =
                    std::min(chunk_cost, interleave_overlap_basis);
                vector_body_cost -= std::min(vector_body_cost, candidate.interleave_overlap_credit);
            }
            const auto spill_cost = add_sat(nonnegative(tuning.vector_spill_load_cost),
                                            nonnegative(tuning.vector_spill_store_cost));
            vector_body_cost = add_sat(
                vector_body_cost,
                mul_sat(mul_sat(candidate.predicted_spill_registers, spill_cost), interleave));

            const auto lanes_per_outer = mul_sat(candidate.minimum_lanes, interleave);
            candidate.vector_iterations = ceil_div(decision.evaluated_trip_count, lanes_per_outer);
            candidate.dynamic_cost =
                add_sat(alias_setup, mul_sat(candidate.vector_iterations, vector_body_cost));
            candidate.total_cost = add_sat(candidate.dynamic_cost, candidate.code_size_cost);
            candidate.break_even_trip_count =
                break_even_trip_count(scalar_per_lane, lanes_per_outer, vector_body_cost,
                                      add_sat(alias_setup, candidate.code_size_cost));
            candidate.legal = true;

            if (!found || better_candidate(candidate, decision.selected)) {
                found = true;
                decision.selected = candidate;
            }
            decision.candidates.push_back(std::move(candidate));
        }
    }

    if (!found) {
        decision.reject_reason = RVVCostRejectReason::NoLegalLMUL;
        decision.explanation = "no LMUL satisfies target mask/register constraints";
        return decision;
    }

    const bool naturally_profitable = decision.selected.total_cost < decision.estimated_scalar_cost;
    if (options_.force) {
        decision.profitable = true;
        decision.profitability_bypassed = !naturally_profitable;
        decision.reject_reason = RVVCostRejectReason::None;
        decision.explanation = naturally_profitable
                                   ? "selected profitable RVV target plan"
                                   : "forced after legality; profitability bypassed";
        return decision;
    }
    if (naturally_profitable) {
        decision.profitable = true;
        decision.reject_reason = RVVCostRejectReason::None;
        decision.explanation = "selected profitable RVV target plan";
        return decision;
    }

    const auto break_even = decision.selected.break_even_trip_count;
    if (break_even != 0 && decision.evaluated_trip_count < break_even) {
        decision.reject_reason = RVVCostRejectReason::ShortTrip;
        decision.explanation = "trip count is below the RVV break-even threshold";
    } else {
        decision.reject_reason = RVVCostRejectReason::NotProfitable;
        decision.explanation = "estimated RVV cost is not lower than scalar cost";
    }
    return decision;
}

void print_rvv_cost_decision_json(const RVVCostDecision &decision, std::ostream &out) {
    out << "{\n  \"profitable\": " << (decision.profitable ? "true" : "false")
        << ",\n  \"profitability_bypassed\": "
        << (decision.profitability_bypassed ? "true" : "false") << ",\n  \"reject_reason\": ";
    print_json_string(out, rvv_cost_reject_reason_name(decision.reject_reason));
    out << ",\n  \"explanation\": ";
    print_json_string(out, decision.explanation);
    out << ",\n  \"interleave_capability_gate\": ";
    print_json_string(out, decision.interleave_capability_gate);
    out << ",\n  \"evaluated_trip_count\": " << decision.evaluated_trip_count
        << ",\n  \"estimated_scalar_cost\": " << decision.estimated_scalar_cost
        << ",\n  \"selected_lmul\": ";
    print_json_string(out, decision.selected.lmul);
    out << ",\n  \"selected_interleave\": " << decision.selected.interleave
        << ",\n  \"selected_break_even_trip_count\": " << decision.selected.break_even_trip_count
        << ",\n  \"candidates\": [\n";
    for (std::size_t index = 0; index < decision.candidates.size(); ++index) {
        const auto &candidate = decision.candidates[index];
        out << "    {\n      \"lmul\": ";
        print_json_string(out, candidate.lmul);
        out << ",\n      \"minimum_lanes\": " << candidate.minimum_lanes
            << ",\n      \"interleave\": " << candidate.interleave
            << ",\n      \"legal\": " << (candidate.legal ? "true" : "false")
            << ",\n      \"rejection\": ";
        print_json_string(out, candidate.rejection);
        out << ",\n      \"vector_iterations\": " << candidate.vector_iterations
            << ",\n      \"estimated_vector_registers\": " << candidate.estimated_vector_registers
            << ",\n      \"predicted_spill_registers\": " << candidate.predicted_spill_registers
            << ",\n      \"interleave_overlap_credit\": " << candidate.interleave_overlap_credit
            << ",\n      \"dynamic_cost\": " << candidate.dynamic_cost
            << ",\n      \"code_size_cost\": " << candidate.code_size_cost
            << ",\n      \"total_cost\": " << candidate.total_cost
            << ",\n      \"estimated_code_bytes\": " << candidate.estimated_code_bytes
            << ",\n      \"break_even_trip_count\": " << candidate.break_even_trip_count
            << "\n    }";
        if (index + 1 != decision.candidates.size()) {
            out << ',';
        }
        out << '\n';
    }
    out << "  ]\n}\n";
}

} // namespace pass::oir_vectorize
