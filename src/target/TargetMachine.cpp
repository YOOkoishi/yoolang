#include "target/TargetMachine.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>

namespace target {
namespace {

std::string lower_copy(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

bool is_power_of_two(unsigned value) {
    return value != 0 && (value & (value - 1U)) == 0;
}

bool is_valid_component(std::string_view value, bool allow_plus = false) {
    if (value.empty()) {
        return false;
    }
    for (char ch : value) {
        const auto uch = static_cast<unsigned char>(ch);
        if (!std::isalnum(uch) && ch != '-' && ch != '_' && ch != '.' &&
            (!allow_plus || ch != '+')) {
            return false;
        }
    }
    return true;
}

bool is_valid_isa_string(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (char ch : value) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
            return false;
        }
    }
    return true;
}

bool is_known_single_letter_extension(char extension) {
    constexpr std::string_view known = "imafdqlcbkjtpvnh";
    return known.find(extension) != std::string_view::npos;
}

bool consume_version(std::string_view value, std::size_t &cursor) {
    if (cursor >= value.size() || !std::isdigit(static_cast<unsigned char>(value[cursor]))) {
        return true;
    }
    while (cursor < value.size() && std::isdigit(static_cast<unsigned char>(value[cursor]))) {
        ++cursor;
    }
    if (cursor < value.size() && value[cursor] == 'p') {
        ++cursor;
        const auto minor_begin = cursor;
        while (cursor < value.size() && std::isdigit(static_cast<unsigned char>(value[cursor]))) {
            ++cursor;
        }
        if (cursor == minor_begin) {
            return false;
        }
    }
    return true;
}

std::string extension_without_version(std::string_view extension) {
    const auto p = extension.rfind('p');
    if (p != std::string_view::npos && p + 1 < extension.size()) {
        bool minor_is_numeric = true;
        for (std::size_t i = p + 1; i < extension.size(); ++i) {
            minor_is_numeric &= std::isdigit(static_cast<unsigned char>(extension[i])) != 0;
        }
        std::size_t major_begin = p;
        while (major_begin > 0 &&
               std::isdigit(static_cast<unsigned char>(extension[major_begin - 1]))) {
            --major_begin;
        }
        if (minor_is_numeric && major_begin != p) {
            return std::string(extension.substr(0, major_begin));
        }
    }

    std::size_t version_begin = extension.size();
    while (version_begin > 0 &&
           std::isdigit(static_cast<unsigned char>(extension[version_begin - 1]))) {
        --version_begin;
    }
    if (version_begin != extension.size() && version_begin > 0 &&
        std::isalpha(static_cast<unsigned char>(extension[version_begin - 1]))) {
        return std::string(extension.substr(0, version_begin));
    }
    return std::string(extension);
}

void add_extension(TargetFeatures &features, std::string extension) {
    if (!extension.empty()) {
        features.extensions.insert(std::move(extension));
    }
}

unsigned parse_zvl(const std::string &extension) {
    if (extension.size() < 5 || extension.rfind("zvl", 0) != 0 || extension.back() != 'b') {
        return 0;
    }
    unsigned value = 0;
    for (std::size_t i = 3; i + 1 < extension.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(extension[i]))) {
            return 0;
        }
        unsigned digit = static_cast<unsigned>(extension[i] - '0');
        if (value > (std::numeric_limits<unsigned>::max() - digit) / 10U) {
            return 0;
        }
        value = value * 10U + digit;
    }
    return value;
}

void enable_standard_extension(TargetFeatures &features, char extension) {
    switch (extension) {
    case 'i':
        features.i = true;
        break;
    case 'm':
        features.m = true;
        break;
    case 'a':
        features.a = true;
        break;
    case 'f':
        features.f = true;
        break;
    case 'd':
        features.d = true;
        break;
    case 'c':
        features.c = true;
        break;
    case 'v':
        features.v = true;
        features.zve32x = true;
        features.zve32f = true;
        features.zve64x = true;
        features.zve64f = true;
        features.zve64d = true;
        add_extension(features, "zve32x");
        add_extension(features, "zve32f");
        add_extension(features, "zve64x");
        add_extension(features, "zve64f");
        add_extension(features, "zve64d");
        break;
    default:
        break;
    }
    add_extension(features, std::string(1, extension));
}

void enable_named_extension(TargetFeatures &features, const std::string &extension) {
    add_extension(features, extension);
    if (extension == "zve32x") {
        features.zve32x = true;
    } else if (extension == "zve32f") {
        features.zve32x = true;
        features.zve32f = true;
        add_extension(features, "zve32x");
    } else if (extension == "zve64x") {
        features.zve32x = true;
        features.zve64x = true;
        add_extension(features, "zve32x");
    } else if (extension == "zve64f") {
        features.zve32x = true;
        features.zve32f = true;
        features.zve64x = true;
        features.zve64f = true;
        add_extension(features, "zve32x");
        add_extension(features, "zve32f");
        add_extension(features, "zve64x");
    } else if (extension == "zve64d") {
        features.zve32x = true;
        features.zve32f = true;
        features.zve64x = true;
        features.zve64f = true;
        features.zve64d = true;
        add_extension(features, "zve32x");
        add_extension(features, "zve32f");
        add_extension(features, "zve64x");
        add_extension(features, "zve64f");
    }
}

bool parse_architecture(TargetProfile &profile, std::string &error) {
    profile.march = lower_copy(profile.march);
    if (profile.march.rfind("rv64", 0) != 0) {
        error = "unsupported -march '" + profile.march + "': yoolang currently requires rv64";
        return false;
    }
    if (!is_valid_isa_string(profile.march)) {
        error = "invalid -march '" + profile.march + "'";
        return false;
    }
    if (profile.march.back() == '_' || profile.march.find("__") != std::string::npos) {
        error = "invalid -march '" + profile.march + "': empty extension name";
        return false;
    }
    if (profile.march.size() == 4) {
        error = "invalid -march '" + profile.march + "': missing base ISA";
        return false;
    }

    TargetFeatures features;
    const auto first_separator = profile.march.find('_', 4);
    const auto base_end =
        first_separator == std::string::npos ? profile.march.size() : first_separator;
    std::size_t cursor = 4;
    bool first_extension = true;
    while (cursor < base_end) {
        const char extension = profile.march[cursor++];
        if (!std::isalpha(static_cast<unsigned char>(extension))) {
            error = "invalid -march '" + profile.march + "': malformed base ISA";
            return false;
        }
        if (first_extension && extension != 'i' && extension != 'g') {
            error = "invalid -march '" + profile.march + "': rv64 base ISA must be I or G";
            return false;
        }
        first_extension = false;
        if (extension != 'g' && !is_known_single_letter_extension(extension)) {
            error = "unsupported -march '" + profile.march +
                    "': unknown single-letter extension '" + std::string(1, extension) + "'";
            return false;
        }
        if (extension == 'g') {
            for (char implied : std::string("imafd")) {
                enable_standard_extension(features, implied);
            }
            add_extension(features, "g");
        } else {
            enable_standard_extension(features, extension);
        }
        if (!consume_version(profile.march, cursor) || cursor > base_end) {
            error = "invalid -march '" + profile.march + "': malformed extension version";
            return false;
        }
    }

    unsigned explicit_minimum_vlen = 0;
    if (first_separator != std::string::npos && first_separator + 1 == profile.march.size()) {
        error = "invalid -march '" + profile.march + "': empty extension name";
        return false;
    }
    cursor = first_separator == std::string::npos ? profile.march.size() : first_separator + 1;
    while (cursor < profile.march.size()) {
        if (profile.march[cursor] == '_') {
            error = "invalid -march '" + profile.march + "': empty extension name";
            return false;
        }
        const auto end = profile.march.find('_', cursor);
        const auto raw_extension = profile.march.substr(cursor, end - cursor);
        const auto extension = extension_without_version(raw_extension);
        if (extension.empty() || !std::isalpha(static_cast<unsigned char>(extension.front()))) {
            error = "invalid -march '" + profile.march + "': malformed extension name";
            return false;
        }
        if (extension.size() == 1) {
            if (extension == "g") {
                for (char implied : std::string("imafd")) {
                    enable_standard_extension(features, implied);
                }
                add_extension(features, "g");
            } else {
                if (!is_known_single_letter_extension(extension.front())) {
                    error = "unsupported -march '" + profile.march +
                            "': unknown single-letter extension '" + extension + "'";
                    return false;
                }
                enable_standard_extension(features, extension.front());
            }
        }
        enable_named_extension(features, extension);
        const auto zvl = parse_zvl(extension);
        if (extension.rfind("zvl", 0) == 0 &&
            (zvl < 32U || zvl > 65536U || !is_power_of_two(zvl))) {
            error = "invalid -march '" + profile.march + "': malformed Zvl extension '" +
                    extension + "'";
            return false;
        }
        explicit_minimum_vlen = std::max(explicit_minimum_vlen, zvl);
        cursor = end == std::string::npos ? profile.march.size() : end + 1;
    }

    if (!features.i) {
        error = "invalid -march '" + profile.march + "': the base I extension is required";
        return false;
    }
    if (features.d && !features.f) {
        error = "invalid -march '" + profile.march + "': D requires F";
        return false;
    }
    if (!features.m || !features.a || !features.f || !features.d) {
        error = "unsupported -march '" + profile.march +
                "': the current backend requires the rv64g baseline";
        return false;
    }
    if (!features.has_any_vector()) {
        for (const auto &extension : features.extensions) {
            if (extension.rfind("zv", 0) == 0) {
                error =
                    "invalid -march '" + profile.march + "': " + extension + " requires V or Zve";
                return false;
            }
        }
    }

    profile.features = std::move(features);
    profile.flen_bits = profile.features.d ? 64U : (profile.features.f ? 32U : 0U);
    if (profile.features.v) {
        profile.minimum_vlen_bits = std::max(128U, explicit_minimum_vlen);
    } else if (profile.features.has_any_vector()) {
        unsigned zve_minimum =
            (profile.features.zve64x || profile.features.zve64f || profile.features.zve64d) ? 64U
                                                                                            : 32U;
        profile.minimum_vlen_bits = std::max(zve_minimum, explicit_minimum_vlen);
    } else {
        profile.minimum_vlen_bits = 0;
    }
    if (!profile.deployment_explicit) {
        profile.deployment = profile.features.has_any_vector() ? DeploymentMode::CompileTimeVector
                                                               : DeploymentMode::Scalar;
    }
    return true;
}

struct CPUModel final {
    std::string_view name;
    std::string_view default_arch;
    TargetTuning tuning;
};

TargetTuning generic_rv64_tuning() {
    TargetTuning tuning;
    tuning.scalar_alu_cost = 1;
    tuning.scalar_load_cost = 4;
    tuning.scalar_store_cost = 4;
    tuning.scalar_branch_cost = 2;
    tuning.vector_alu_cost = 1;
    tuning.vector_load_cost = 5;
    tuning.vector_store_cost = 5;
    tuning.vector_strided_load_cost = 8;
    tuning.vector_strided_store_cost = 9;
    tuning.vector_indexed_load_cost = 12;
    tuning.vector_indexed_store_cost = 13;
    tuning.vector_segment_base_cost = 3;
    tuning.vector_segment_field_cost = 4;
    tuning.vector_index_setup_cost = 3;
    tuning.vector_mask_cost = 2;
    tuning.vector_reduction_cost = 8;
    tuning.vsetvl_cost = 3;
    tuning.vector_spill_load_cost = 10;
    tuning.vector_spill_store_cost = 10;
    tuning.vector_spill_cost = 10;
    tuning.code_size_cost = 1;
    tuning.available_vector_registers = 31;
    tuning.maximum_lmul = 8;
    // Interleave is a property of the verified VLA recipe, not a promise that
    // the scalar default architecture contains V.  An explicit vector march
    // may therefore use factor two with the conservative generic-rv64 costs.
    tuning.maximum_interleave_factor = 2;
    return tuning;
}

TargetTuning generic_rvv_tuning() {
    TargetTuning tuning;
    tuning.scalar_alu_cost = 1;
    tuning.scalar_load_cost = 4;
    tuning.scalar_store_cost = 4;
    tuning.scalar_branch_cost = 2;
    tuning.vector_alu_cost = 1;
    tuning.vector_load_cost = 3;
    tuning.vector_store_cost = 3;
    tuning.vector_strided_load_cost = 5;
    tuning.vector_strided_store_cost = 6;
    tuning.vector_indexed_load_cost = 8;
    tuning.vector_indexed_store_cost = 9;
    tuning.vector_segment_base_cost = 2;
    tuning.vector_segment_field_cost = 2;
    tuning.vector_index_setup_cost = 2;
    tuning.vector_mask_cost = 1;
    tuning.vector_reduction_cost = 4;
    tuning.vsetvl_cost = 2;
    tuning.vector_spill_load_cost = 8;
    tuning.vector_spill_store_cost = 8;
    tuning.vector_spill_cost = 8;
    tuning.code_size_cost = 1;
    tuning.available_vector_registers = 31;
    tuning.maximum_lmul = 8;
    // The generic RVV profile models the verified two-chunk VLA recipe.  The
    // loop planner still rejects factor two for reductions, diamonds, runtime
    // versioning and unsupported live-outs.
    tuning.maximum_interleave_factor = 2;
    return tuning;
}

const std::array<CPUModel, 2> &cpu_models() {
    static const std::array<CPUModel, 2> models = {
        CPUModel{"generic-rv64", "rv64gc", generic_rv64_tuning()},
        CPUModel{"generic-rvv", "rv64gcv", generic_rvv_tuning()},
    };
    return models;
}

const CPUModel *find_cpu_model(std::string_view name) {
    for (const auto &model : cpu_models()) {
        if (model.name == name) {
            return &model;
        }
    }
    return nullptr;
}

} // namespace

bool lookup_target_tuning(std::string_view name, TargetTuning &tuning) {
    const auto *model = find_cpu_model(name);
    if (model == nullptr) {
        return false;
    }
    tuning = model->tuning;
    return true;
}

std::string_view target_cpu_default_arch(std::string_view name) {
    const auto *model = find_cpu_model(name);
    return model == nullptr ? std::string_view{} : model->default_arch;
}

std::string supported_target_cpu_names() {
    return "generic-rv64, generic-rvv";
}

bool TargetFeatures::has(std::string_view extension) const {
    return extensions.find(std::string(extension)) != extensions.end();
}

bool TargetFeatures::has_any_vector() const {
    return v || zve32x || zve32f || zve64x || zve64f || zve64d;
}

bool TargetFeatures::supports_i32_vectors() const {
    return has_any_vector();
}

bool TargetFeatures::supports_f32_vectors() const {
    return (v && f) || zve32f || zve64f || zve64d;
}

bool TargetProfile::has_vector() const {
    // The parsed ISA remains available through features/march even for an
    // explicitly scalar deployment.  Code generation must use this effective
    // capability gate so that -march=...v -mrvv-deployment=scalar cannot emit
    // RVV instructions or enable an automatic vectorizer.
    return deployment == DeploymentMode::CompileTimeVector && features.has_any_vector();
}

bool TargetProfile::supports_vector_element(bool is_float, unsigned bit_width) const {
    if (!has_vector() || bit_width != 32) {
        return false;
    }
    return is_float ? features.supports_f32_vectors() : features.supports_i32_vectors();
}

std::string TargetProfile::vector_bits_name() const {
    if (vector_bits_kind == VectorBitsKind::Scalable) {
        return "scalable";
    }
    return fixed_vector_bits ? std::to_string(*fixed_vector_bits) : "scalable";
}

std::string TargetProfile::vector_abi_name() const {
    return vector_abi == VectorABI::Standard ? "standard" : "psabi-vector";
}

std::string TargetProfile::deployment_name() const {
    switch (deployment) {
    case DeploymentMode::Scalar:
        return "scalar";
    case DeploymentMode::CompileTimeVector:
        return "compile-time";
    case DeploymentMode::Multiversion:
        return "fat";
    }
    return "scalar";
}

TargetMachine::TargetMachine() : TargetMachine(TargetProfile{}) {
}

TargetMachine::TargetMachine(TargetProfile profile) : profile_(std::move(profile)) {
    std::string error;
    if (!finalize_target_profile(profile_, error)) {
        throw std::invalid_argument(error);
    }
    data_layout_.pointer_size = profile_.xlen_bits / 8U;
    data_layout_.pointer_abi_alignment = data_layout_.pointer_size;
    data_layout_.pointer_preferred_alignment = data_layout_.pointer_size;
    data_layout_.stack_alignment = profile_.stack_alignment;
}

const TargetProfile &TargetMachine::profile() const {
    return profile_;
}

TargetProfile &TargetMachine::profile() {
    return profile_;
}

const DataLayoutSpec &TargetMachine::data_layout() const {
    return data_layout_;
}

bool finalize_target_profile(TargetProfile &profile, std::string &error) {
    profile.triple = lower_copy(profile.triple);
    profile.mabi = lower_copy(profile.mabi);
    profile.cpu = lower_copy(profile.cpu);
    profile.tune = lower_copy(profile.tune);
    const bool cpu_was_explicit = profile.cpu_explicit || profile.cpu != "generic-rv64";
    const bool tune_was_explicit = profile.tune_explicit || profile.tune != "generic-rv64";
    if (!is_valid_component(profile.triple) ||
        (profile.triple != "riscv64" && profile.triple.rfind("riscv64-", 0) != 0)) {
        error = "unsupported target triple '" + profile.triple + "'";
        return false;
    }
    if (!is_valid_component(profile.cpu, true)) {
        error = "invalid -mcpu value '" + profile.cpu + "'";
        return false;
    }
    if (!is_valid_component(profile.tune, true)) {
        error = "invalid -mtune value '" + profile.tune + "'";
        return false;
    }
    const auto cpu_default_arch = target_cpu_default_arch(profile.cpu);
    if (cpu_default_arch.empty()) {
        error = "unsupported -mcpu '" + profile.cpu + "': supported CPUs are " +
                supported_target_cpu_names();
        return false;
    }
    TargetTuning selected_tuning;
    if (!tune_was_explicit && cpu_was_explicit) {
        profile.tune = profile.cpu;
    }
    if (!lookup_target_tuning(profile.tune, selected_tuning)) {
        error = "unsupported -mtune '" + profile.tune + "': supported tuning CPUs are " +
                supported_target_cpu_names();
        return false;
    }
    if (cpu_was_explicit && !profile.march_explicit) {
        profile.march = std::string(cpu_default_arch);
    }
    profile.tuning = selected_tuning;
    if (profile.mabi != "lp64d") {
        error = "unsupported -mabi '" + profile.mabi + "': only lp64d is implemented";
        return false;
    }
    if (!parse_architecture(profile, error)) {
        return false;
    }
    if (!profile.features.d) {
        error = "-mabi=lp64d requires the D extension in -march";
        return false;
    }
    if (profile.deployment == DeploymentMode::CompileTimeVector &&
        !profile.features.has_any_vector()) {
        error = "-mrvv-deployment=compile-time requires V or Zve in -march";
        return false;
    }
    if (profile.vector_bits_explicit && !profile.features.has_any_vector() &&
        profile.deployment != DeploymentMode::Multiversion) {
        error = "-mrvv-vector-bits requires V or Zve in -march";
        return false;
    }
    if (profile.vector_bits_explicit && profile.deployment == DeploymentMode::Scalar) {
        error = "-mrvv-vector-bits is incompatible with -mrvv-deployment=scalar";
        return false;
    }
    if (profile.vector_bits_kind == VectorBitsKind::Fixed) {
        if (!profile.fixed_vector_bits) {
            error = "fixed RVV vector bits require a numeric width";
            return false;
        }
        if (*profile.fixed_vector_bits < profile.minimum_vlen_bits) {
            error = "fixed RVV vector bits are smaller than the target minimum VLEN";
            return false;
        }
    }
    if (profile.vector_abi == VectorABI::PsABIVector) {
        if (profile.deployment != DeploymentMode::CompileTimeVector ||
            !profile.features.has_any_vector()) {
            error = "-mvector-abi=psabi-vector requires compile-time V or Zve code generation";
            return false;
        }
        if (!profile.vector_bits_explicit || profile.vector_bits_kind != VectorBitsKind::Fixed ||
            !profile.fixed_vector_bits.has_value()) {
            error = "-mvector-abi=psabi-vector requires an explicit numeric "
                    "-mrvv-vector-bits=ABI_VLEN";
            return false;
        }
        if (*profile.fixed_vector_bits != profile.minimum_vlen_bits) {
            error = "psabi-vector ABI_VLEN from -mrvv-vector-bits must match the VLEN "
                    "guaranteed by -march (use the matching zvl<N>b extension)";
            return false;
        }
        error = "PSABI_VECTOR_ABI_UNAVAILABLE: -mvector-abi=psabi-vector remains "
                "fail-closed until fixed vector tuple lowering and GCC/Clang "
                "bidirectional interoperability are both implemented and verified";
        return false;
    }
    return true;
}

bool parse_vector_bits(std::string_view value, TargetProfile &profile, std::string &error) {
    const auto normalized = lower_copy(value);
    if (normalized == "scalable") {
        profile.vector_bits_kind = VectorBitsKind::Scalable;
        profile.fixed_vector_bits.reset();
        profile.vector_bits_explicit = true;
        return true;
    }
    if (normalized.empty()) {
        error = "-mrvv-vector-bits requires 'scalable' or a power-of-two width";
        return false;
    }
    unsigned bits = 0;
    for (char ch : normalized) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            error = "invalid -mrvv-vector-bits value '" + normalized + "'";
            return false;
        }
        unsigned digit = static_cast<unsigned>(ch - '0');
        if (bits > (std::numeric_limits<unsigned>::max() - digit) / 10U) {
            error = "-mrvv-vector-bits value is too large";
            return false;
        }
        bits = bits * 10U + digit;
    }
    if (!is_power_of_two(bits) || bits < 32U || bits > 65536U) {
        error = "-mrvv-vector-bits must be a power of two in [32, 65536]";
        return false;
    }
    profile.vector_bits_kind = VectorBitsKind::Fixed;
    profile.fixed_vector_bits = bits;
    profile.vector_bits_explicit = true;
    return true;
}

bool parse_vector_abi(std::string_view value, TargetProfile &profile, std::string &error) {
    const auto normalized = lower_copy(value);
    if (normalized == "standard") {
        profile.vector_abi = VectorABI::Standard;
        return true;
    }
    if (normalized == "psabi-vector") {
        profile.vector_abi = VectorABI::PsABIVector;
        return true;
    }
    error = "invalid -mvector-abi value '" + normalized + "'";
    return false;
}

bool parse_rvv_deployment(std::string_view value, TargetProfile &profile, std::string &error) {
    const auto normalized = lower_copy(value);
    if (normalized == "scalar") {
        profile.deployment = DeploymentMode::Scalar;
        profile.deployment_explicit = true;
        return true;
    }
    if (normalized == "compile-time") {
        profile.deployment = DeploymentMode::CompileTimeVector;
        profile.deployment_explicit = true;
        return true;
    }
    if (normalized == "fat") {
        profile.deployment = DeploymentMode::Multiversion;
        profile.deployment_explicit = true;
        return true;
    }
    error = "invalid -mrvv-deployment value '" + normalized +
            "': expected scalar, compile-time, or fat";
    return false;
}

bool make_rvv_multiversion_profiles(const TargetProfile &fat_profile, TargetProfile &scalar_profile,
                                    TargetProfile &vector_profile, std::string &error) {
    if (fat_profile.deployment != DeploymentMode::Multiversion) {
        error = "FAT_TARGET_REQUIRED: target deployment is not fat";
        return false;
    }
    if (fat_profile.march != "rv64gc" && fat_profile.march != "rv64gcv") {
        error = "FAT_TARGET_UNSUPPORTED: fat deployment currently supports only "
                "-march=rv64gc or rv64gcv";
        return false;
    }
    if (fat_profile.vector_abi != VectorABI::Standard) {
        error = "FAT_VECTOR_ABI_UNSUPPORTED: fat deployment requires -mvector-abi=standard";
        return false;
    }
    if (fat_profile.vector_bits_kind == VectorBitsKind::Fixed) {
        error = "FAT_FIXED_VLEN_UNSUPPORTED: fat deployment requires "
                "-mrvv-vector-bits=scalable";
        return false;
    }

    scalar_profile = fat_profile;
    scalar_profile.march = "rv64gc";
    scalar_profile.march_explicit = true;
    scalar_profile.deployment = DeploymentMode::Scalar;
    scalar_profile.deployment_explicit = true;
    scalar_profile.vector_bits_kind = VectorBitsKind::Scalable;
    scalar_profile.fixed_vector_bits.reset();
    scalar_profile.vector_bits_explicit = false;
    if (!finalize_target_profile(scalar_profile, error)) {
        error = "FAT_SCALAR_TARGET_FAILED: " + error;
        return false;
    }

    vector_profile = fat_profile;
    vector_profile.march = "rv64gcv";
    vector_profile.march_explicit = true;
    vector_profile.deployment = DeploymentMode::CompileTimeVector;
    vector_profile.deployment_explicit = true;
    if (!finalize_target_profile(vector_profile, error)) {
        error = "FAT_VECTOR_TARGET_FAILED: " + error;
        return false;
    }
    return true;
}

} // namespace target
