#include "pass/oir/OIRVectorizationRemark.h"

#include <ostream>
#include <utility>

namespace pass::oir_vectorize {
namespace {

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20U) {
                // Source identifiers and explanations are UTF-8; only ASCII
                // control bytes need a deterministic replacement here.
                out += "?";
            } else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

} // namespace

std::string_view remark_code_name(RemarkCode code) {
    switch (code) {
    case RemarkCode::Vectorized:
        return "VECTORIZED";
    case RemarkCode::Candidate:
        return "CANDIDATE";
    case RemarkCode::RejectDependence:
        return "REJECT_DEPENDENCE";
    case RemarkCode::RejectAlias:
        return "REJECT_ALIAS";
    case RemarkCode::RejectFPOrder:
        return "REJECT_FP_ORDER";
    case RemarkCode::RejectCall:
        return "REJECT_CALL";
    case RemarkCode::RejectCost:
        return "REJECT_COST";
    case RemarkCode::RejectRegisterPressure:
        return "REJECT_REGISTER_PRESSURE";
    case RemarkCode::RejectUnsupportedType:
        return "REJECT_UNSUPPORTED_TYPE";
    case RemarkCode::RejectNonCanonicalLoop:
        return "REJECT_NON_CANONICAL_LOOP";
    case RemarkCode::RejectEarlyExit:
        return "REJECT_EARLY_EXIT";
    case RemarkCode::RejectPotentialTrap:
        return "REJECT_POTENTIAL_TRAP";
    case RemarkCode::RejectReduction:
        return "REJECT_REDUCTION";
    case RemarkCode::RejectStride:
        return "REJECT_STRIDE";
    case RemarkCode::RejectVolatileOrAtomic:
        return "REJECT_VOLATILE_OR_ATOMIC";
    case RemarkCode::RejectScalableStorage:
        return "REJECT_SCALABLE_STORAGE";
    case RemarkCode::RejectTargetFeature:
        return "REJECT_TARGET_FEATURE";
    case RemarkCode::Disabled:
        return "DISABLED";
    }
    return "REJECT_UNSUPPORTED_TYPE";
}

std::string_view vectorizer_kind_name(VectorizerKind kind) {
    switch (kind) {
    case VectorizerKind::Loop:
        return "loop";
    case VectorizerKind::SLP:
        return "slp";
    }
    return "loop";
}

bool Remark::succeeded() const {
    return code == RemarkCode::Vectorized;
}

void RemarkLog::add(Remark remark) {
    remarks_.push_back(std::move(remark));
}

const std::vector<Remark> &RemarkLog::remarks() const {
    return remarks_;
}

bool RemarkLog::empty() const {
    return remarks_.empty();
}

void RemarkLog::clear() {
    remarks_.clear();
}

void print_remarks_text(const RemarkLog &log, std::ostream &out, bool include_successes,
                        bool include_misses, std::string_view function_filter) {
    for (const auto &remark : log.remarks()) {
        const auto matches_filter =
            function_filter.empty() || remark.function.find(function_filter) != std::string::npos ||
            remark.region.find(function_filter) != std::string::npos ||
            vectorizer_kind_name(remark.vectorizer).find(function_filter) !=
                std::string_view::npos ||
            remark_code_name(remark.code).find(function_filter) != std::string_view::npos;
        if ((remark.succeeded() && !include_successes) ||
            (!remark.succeeded() && !include_misses) || !matches_filter) {
            continue;
        }
        out << "remark: vectorize(" << vectorizer_kind_name(remark.vectorizer)
            << "): " << remark_code_name(remark.code) << ": " << remark.function;
        if (!remark.region.empty()) {
            out << ":" << remark.region;
        }
        if (!remark.explanation.empty()) {
            out << ": " << remark.explanation;
        }
        if (remark.succeeded()) {
            out << " [vf=" << (remark.plan.scalable ? "vscale x " : "") << remark.plan.minimum_lanes
                << ", lmul=" << remark.plan.lmul << ", interleave=" << remark.plan.interleave
                << "]";
        }
        out << '\n';
    }
}

void print_vector_plan_json(const RemarkLog &log, std::ostream &out) {
    out << "{\n  \"vectorization_plans\": [\n";
    for (std::size_t index = 0; index < log.remarks().size(); ++index) {
        const auto &remark = log.remarks()[index];
        out << "    {\n"
            << "      \"vectorizer\": \"" << vectorizer_kind_name(remark.vectorizer) << "\",\n"
            << "      \"code\": \"" << remark_code_name(remark.code) << "\",\n"
            << "      \"function\": \"" << json_escape(remark.function) << "\",\n"
            << "      \"region\": \"" << json_escape(remark.region) << "\",\n"
            << "      \"explanation\": \"" << json_escape(remark.explanation) << "\",\n"
            << "      \"plan\": {\n"
            << "        \"scalable\": " << (remark.plan.scalable ? "true" : "false") << ",\n"
            << "        \"minimum_lanes\": " << remark.plan.minimum_lanes << ",\n"
            << "        \"lmul\": \"" << json_escape(remark.plan.lmul) << "\",\n"
            << "        \"interleave\": " << remark.plan.interleave << ",\n"
            << "        \"estimated_scalar_cost\": " << remark.plan.estimated_scalar_cost << ",\n"
            << "        \"estimated_vector_cost\": " << remark.plan.estimated_vector_cost << ",\n"
            << "        \"estimated_vector_registers\": " << remark.plan.estimated_vector_registers
            << ",\n"
            << "        \"predicted_spill_registers\": " << remark.plan.predicted_spill_registers
            << ",\n"
            << "        \"interleave_overlap_credit\": " << remark.plan.interleave_overlap_credit
            << ",\n"
            << "        \"estimated_code_bytes\": " << remark.plan.estimated_code_bytes << ",\n"
            << "        \"break_even_trip_count\": " << remark.plan.break_even_trip_count << ",\n"
            << "        \"tuning\": \"" << json_escape(remark.plan.tuning) << "\",\n"
            << "        \"interleave_capability_gate\": \""
            << json_escape(remark.plan.interleave_capability_gate) << "\",\n"
            << "        \"runtime_alias_check\": "
            << (remark.plan.requires_runtime_alias_check ? "true" : "false") << ",\n"
            << "        \"uses_mask\": " << (remark.plan.uses_mask ? "true" : "false") << "\n"
            << "      }\n"
            << "    }";
        if (index + 1 != log.remarks().size()) {
            out << ',';
        }
        out << '\n';
    }
    out << "  ]\n}\n";
}

} // namespace pass::oir_vectorize
