#pragma once

#include "mir/MIR.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pass {

inline constexpr const char *kMIRVectorRelegalizeArtifactKey =
    "mir.vector-register-relegalize";

enum class MIRVectorRelegalizeReason {
    TiedLiveRangeConflict,
    SpillScratchPressure,
};

struct MIRVectorRelegalizeRequest {
    MIRVectorRelegalizeRequest(std::string function, std::uint32_t virtual_register,
                               mir::MachineVectorType vector_type,
                               MIRVectorRelegalizeReason reason, std::string detail);

    std::string function;
    std::uint32_t virtual_register;
    mir::MachineVectorType vector_type;
    MIRVectorRelegalizeReason reason;
    std::string detail;
};

using MIRVectorRelegalizeRequests = std::vector<MIRVectorRelegalizeRequest>;

struct MIRVectorRegAllocResult {
    bool success = true;
    bool changed = false;
    std::string message;
    MIRVectorRelegalizeRequests relegalize_requests;
};

// Dedicated RVV register-group allocator.  It colors complete LMUL groups,
// reserves v0 for masks, honors descriptor ties, and inserts scalable
// whole-register spill/reload pseudos.  Cases that cannot yet be represented
// safely are returned through the explicit relegalization request interface.
class MIRVectorRegAllocator final {
  public:
    MIRVectorRegAllocResult run(mir::MachineFunction &function);
};

const char *mir_vector_relegalize_reason_name(MIRVectorRelegalizeReason reason);

} // namespace pass
