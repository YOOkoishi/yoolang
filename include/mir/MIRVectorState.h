#pragma once

#include "mir/MIR.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace mir {

struct MIRHardwareVType final {
    unsigned sew_bits = 0;
    VectorLMUL lmul = VectorLMUL::M1;

    bool operator==(const MIRHardwareVType &other) const;
    bool operator!=(const MIRHardwareVType &other) const;
};

enum class MIRVLRequestKind {
    Immediate,
    Register,
    VLMAX,
};

struct MIRVLRequest final {
    MIRVLRequestKind kind = MIRVLRequestKind::Immediate;
    std::int64_t immediate = 0;
    Register reg;

    bool operator==(const MIRVLRequest &other) const;
    bool operator!=(const MIRVLRequest &other) const;
};

// Component-wise meet state.  A reachable join may retain the actual-VL
// identity and hardware shape even when predecessor policy bits differ; this
// lets the transform insert a safe policy-only vsetvli at the join.
struct MIRVectorState final {
    bool reachable = false;
    std::optional<MIRHardwareVType> vtype;
    std::optional<VectorTailPolicy> tail_policy;
    std::optional<VectorMaskPolicy> mask_policy;
    std::optional<std::uint64_t> vl_identity;
    std::optional<MIRVLRequest> vl_request;

    static MIRVectorState unreachable();
    static MIRVectorState unknown();
    bool fully_known() const;
    bool operator==(const MIRVectorState &other) const;
    bool operator!=(const MIRVectorState &other) const;
};

struct MIRVectorStateAnalysis final {
    bool ok = true;
    std::string message;
    std::unordered_map<const MachineBasicBlock *, MIRVectorState> block_in;
    std::unordered_map<const MachineBasicBlock *, MIRVectorState> block_out;
};

MIRHardwareVType hardware_vtype(const MachineVectorType &type);
bool is_mir_vector_state_set(const MachineInstr &instruction);
bool is_mir_policy_only_state_set(const MachineInstr &instruction);
MIRVectorState advance_mir_vector_state(MIRVectorState state, const MachineInstr &instruction);

// Analyze the state at every CFG boundary and, when requested, fail closed on
// a vector instruction that can execute without an exact VL/VTYPE/policy.
MIRVectorStateAnalysis analyze_mir_vector_state(const MachineFunction &function,
                                                bool verify_uses = true);

} // namespace mir
