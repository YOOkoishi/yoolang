#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace target {

inline constexpr const char *kTargetMachineArtifactKey = "target.machine";

enum class VectorABI {
    Standard,
    PsABIVector,
};

enum class VectorBitsKind {
    Scalable,
    Fixed,
};

enum class DeploymentMode {
    Scalar,
    CompileTimeVector,
    Multiversion, // Exposed as "fat" by the deployment CLI contract.
};

struct TargetFeatures {
    bool i = false;
    bool m = false;
    bool a = false;
    bool f = false;
    bool d = false;
    bool c = false;
    bool v = false;
    bool zve32x = false;
    bool zve32f = false;
    bool zve64x = false;
    bool zve64f = false;
    bool zve64d = false;
    std::unordered_set<std::string> extensions;

    bool has(std::string_view extension) const;
    bool has_any_vector() const;
    bool supports_i32_vectors() const;
    bool supports_f32_vectors() const;
};

struct TargetTuning {
    // Costs are abstract throughput/latency units.  They are intentionally
    // integral so that vectorization decisions and diagnostics are stable
    // across hosts.
    int scalar_alu_cost = 1;
    int scalar_load_cost = 4;
    int scalar_store_cost = 4;
    int scalar_branch_cost = 2;
    int vector_alu_cost = 1;
    // The legacy load/store names are the unit-stride RVV costs.
    int vector_load_cost = 4;
    int vector_store_cost = 4;
    int vector_strided_load_cost = 7;
    int vector_strided_store_cost = 7;
    int vector_indexed_load_cost = 10;
    int vector_indexed_store_cost = 11;
    int vector_segment_base_cost = 2;
    int vector_segment_field_cost = 3;
    int vector_index_setup_cost = 2;
    int vector_mask_cost = 1;
    int vector_reduction_cost = 6;
    int vsetvl_cost = 2;
    int vector_spill_load_cost = 8;
    int vector_spill_store_cost = 8;
    // Kept for callers that need one conservative spill-side estimate.
    int vector_spill_cost = 8;
    // Cost per emitted four-byte instruction.  This charges code growth once,
    // rather than once per dynamic vector iteration.
    int code_size_cost = 1;
    unsigned available_vector_registers = 31;
    unsigned maximum_lmul = 8;
    // Maximum VLA chunk groups that the target tuning permits one outer
    // vector-loop iteration to issue.  Transform legality remains a separate
    // fail-closed gate in the loop vectorizer.
    unsigned maximum_interleave_factor = 1;
};

struct TargetProfile {
    std::string triple = "riscv64-unknown-linux-gnu";
    std::string march = "rv64gc";
    std::string mabi = "lp64d";
    std::string cpu = "generic-rv64";
    std::string tune = "generic-rv64";
    bool march_explicit = false;
    bool cpu_explicit = false;
    bool tune_explicit = false;
    TargetFeatures features;
    TargetTuning tuning;
    unsigned xlen_bits = 64;
    unsigned flen_bits = 64;
    unsigned minimum_vlen_bits = 0;
    unsigned stack_alignment = 16;
    VectorBitsKind vector_bits_kind = VectorBitsKind::Scalable;
    std::optional<unsigned> fixed_vector_bits;
    bool vector_bits_explicit = false;
    VectorABI vector_abi = VectorABI::Standard;
    DeploymentMode deployment = DeploymentMode::Scalar;
    bool deployment_explicit = false;

    bool has_vector() const;
    bool supports_vector_element(bool is_float, unsigned bit_width) const;
    std::string vector_bits_name() const;
    std::string vector_abi_name() const;
    std::string deployment_name() const;
};

struct DataLayoutSpec {
    bool little_endian = true;
    unsigned pointer_size = 8;
    unsigned pointer_abi_alignment = 8;
    unsigned pointer_preferred_alignment = 8;
    unsigned stack_alignment = 16;
};

class TargetMachine final {
  public:
    TargetMachine();
    explicit TargetMachine(TargetProfile profile);

    const TargetProfile &profile() const;
    TargetProfile &profile();
    const DataLayoutSpec &data_layout() const;

  private:
    TargetProfile profile_;
    DataLayoutSpec data_layout_;
};

bool finalize_target_profile(TargetProfile &profile, std::string &error);
bool lookup_target_tuning(std::string_view name, TargetTuning &tuning);
std::string_view target_cpu_default_arch(std::string_view name);
std::string supported_target_cpu_names();
bool parse_vector_bits(std::string_view value, TargetProfile &profile, std::string &error);
bool parse_vector_abi(std::string_view value, TargetProfile &profile, std::string &error);
bool parse_rvv_deployment(std::string_view value, TargetProfile &profile, std::string &error);
bool make_rvv_multiversion_profiles(const TargetProfile &fat_profile, TargetProfile &scalar_profile,
                                    TargetProfile &vector_profile, std::string &error);

} // namespace target
