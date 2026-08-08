#include "mir/MIRVectorState.h"

#include "mir/MachineInstrDesc.h"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace mir {
namespace {

bool is_zero_register(const MachineOperand &operand) {
    return operand.is_reg() && operand.reg_value().is_physical() &&
           operand.reg_value().name == "zero";
}

bool is_state_setting_instruction_impl(const MachineInstr &instruction) {
    const auto &desc = instruction_desc(instruction.opcode());
    return desc.implicitly_defines(MVS_VL) && desc.implicitly_defines(MVS_VTYPE) &&
           instruction.has_vector_info();
}

bool is_policy_only_set(const MachineInstr &instruction) {
    return is_state_setting_instruction_impl(instruction) && instruction.operands().size() >= 2 &&
           is_zero_register(instruction.operands()[0]) &&
           is_zero_register(instruction.operands()[1]);
}

std::optional<MIRVLRequest> request_for(const MachineInstr &instruction) {
    if (instruction.operands().size() < 2) {
        return std::nullopt;
    }
    const auto &operand = instruction.operands()[1];
    if (operand.kind() == OperandKind::Imm) {
        MIRVLRequest request;
        request.kind = MIRVLRequestKind::Immediate;
        request.immediate = operand.int_value();
        return request;
    }
    if (!operand.is_reg()) {
        return std::nullopt;
    }
    MIRVLRequest request;
    if (is_zero_register(operand)) {
        request.kind = MIRVLRequestKind::VLMAX;
    } else {
        request.kind = MIRVLRequestKind::Register;
        request.reg = operand.reg_value();
    }
    return request;
}

template <typename T>
std::optional<T> meet_component(const std::optional<T> &lhs, const std::optional<T> &rhs) {
    if (lhs.has_value() && rhs.has_value() && *lhs == *rhs) {
        return lhs;
    }
    return std::nullopt;
}

MIRVectorState meet_states(const MIRVectorState &lhs, const MIRVectorState &rhs) {
    if (!lhs.reachable) {
        return rhs;
    }
    if (!rhs.reachable) {
        return lhs;
    }
    MIRVectorState result;
    result.reachable = true;
    result.vtype = meet_component(lhs.vtype, rhs.vtype);
    result.tail_policy = meet_component(lhs.tail_policy, rhs.tail_policy);
    result.mask_policy = meet_component(lhs.mask_policy, rhs.mask_policy);
    result.vl_identity = meet_component(lhs.vl_identity, rhs.vl_identity);
    result.vl_request = meet_component(lhs.vl_request, rhs.vl_request);
    return result;
}

MIRVectorState
transfer_instruction(MIRVectorState state, const MachineInstr &instruction,
                     const std::unordered_map<const MachineInstr *, std::uint64_t> &fallback_ids) {
    if (!state.reachable) {
        // Unreachable is the dataflow bottom.  Turning a block that has not
        // been reached *yet* into architectural unknown during the fixed
        // point iteration permanently poisons an earlier join when its
        // predecessors are stored later in the function.  Disconnected
        // blocks are checked from unknown in the verification walk below;
        // they must not participate in the reachable CFG solution.
        return state;
    }
    if (machine_instr_may_call(instruction)) {
        return MIRVectorState::unknown();
    }

    const auto &desc = instruction_desc(instruction.opcode());
    if (is_state_setting_instruction_impl(instruction)) {
        const auto &info = instruction.vector_info();
        const auto desired_vtype = hardware_vtype(info.vector_type);
        if (is_policy_only_set(instruction)) {
            const bool can_preserve = state.vtype.has_value() && *state.vtype == desired_vtype &&
                                      state.vl_identity.has_value();
            state.vtype = desired_vtype;
            state.tail_policy = info.tail_policy;
            state.mask_policy = info.mask_policy;
            if (!can_preserve) {
                state.vl_identity.reset();
                state.vl_request.reset();
            }
            return state;
        }

        state.vtype = desired_vtype;
        state.tail_policy = info.tail_policy;
        state.mask_policy = info.mask_policy;
        auto fallback = fallback_ids.find(&instruction);
        if (info.vl_identity != 0) {
            state.vl_identity = info.vl_identity;
        } else if (fallback != fallback_ids.end()) {
            state.vl_identity = fallback->second;
        } else {
            state.vl_identity.reset();
        }
        state.vl_request = request_for(instruction);
        return state;
    }

    if (desc.implicitly_defines(MVS_VL) || desc.implicitly_defines(MVS_VTYPE)) {
        state = MIRVectorState::unknown();
    }
    return state;
}

std::string state_error(const MachineFunction &function, const MachineBasicBlock &block,
                        const MachineInstr &instruction, const std::string &detail) {
    std::ostringstream out;
    out << "MIR vector state @" << function.name() << ':' << block.name() << ' '
        << opcode_name(instruction.opcode()) << ": " << detail;
    return out.str();
}

std::unordered_map<const MachineInstr *, std::uint64_t>
fallback_identities(const MachineFunction &function) {
    std::unordered_map<const MachineInstr *, std::uint64_t> result;
    std::uint64_t next = UINT64_C(0x8000000000000000);
    for (const auto &block : function.blocks()) {
        for (const auto &instruction : block->instructions()) {
            if (!is_state_setting_instruction_impl(instruction) ||
                is_policy_only_set(instruction) || instruction.vector_info().vl_identity != 0) {
                continue;
            }
            result.emplace(&instruction, next++);
        }
    }
    return result;
}

MIRVectorState
block_input(const MachineFunction &function, const MachineBasicBlock &block,
            const std::unordered_map<const MachineBasicBlock *, MIRVectorState> &out) {
    if (!function.blocks().empty() && function.blocks().front().get() == &block) {
        // The architectural state on function entry is unspecified even if a
        // malformed CFG happens to contain an edge back to the entry block.
        return MIRVectorState::unknown();
    }
    MIRVectorState result = MIRVectorState::unreachable();
    for (const auto *predecessor : block.predecessors()) {
        const auto found = out.find(predecessor);
        if (found != out.end()) {
            result = meet_states(result, found->second);
        }
    }
    return result;
}

MIRVectorState
transfer_block(MIRVectorState state, const MachineBasicBlock &block,
               const std::unordered_map<const MachineInstr *, std::uint64_t> &fallback_ids) {
    for (const auto &instruction : block.instructions()) {
        state = transfer_instruction(std::move(state), instruction, fallback_ids);
    }
    return state;
}

} // namespace

bool MIRHardwareVType::operator==(const MIRHardwareVType &other) const {
    return sew_bits == other.sew_bits && lmul == other.lmul;
}

bool MIRHardwareVType::operator!=(const MIRHardwareVType &other) const {
    return !(*this == other);
}

bool MIRVLRequest::operator==(const MIRVLRequest &other) const {
    if (kind != other.kind) {
        return false;
    }
    switch (kind) {
    case MIRVLRequestKind::Immediate:
        return immediate == other.immediate;
    case MIRVLRequestKind::Register:
        return reg == other.reg;
    case MIRVLRequestKind::VLMAX:
        return true;
    }
    return false;
}

bool MIRVLRequest::operator!=(const MIRVLRequest &other) const {
    return !(*this == other);
}

MIRVectorState MIRVectorState::unreachable() {
    return {};
}

MIRVectorState MIRVectorState::unknown() {
    MIRVectorState state;
    state.reachable = true;
    return state;
}

bool MIRVectorState::fully_known() const {
    return reachable && vtype.has_value() && tail_policy.has_value() && mask_policy.has_value() &&
           vl_identity.has_value();
}

bool MIRVectorState::operator==(const MIRVectorState &other) const {
    return reachable == other.reachable && vtype == other.vtype &&
           tail_policy == other.tail_policy && mask_policy == other.mask_policy &&
           vl_identity == other.vl_identity && vl_request == other.vl_request;
}

bool MIRVectorState::operator!=(const MIRVectorState &other) const {
    return !(*this == other);
}

MIRHardwareVType hardware_vtype(const MachineVectorType &type) {
    return {type.sew_bits(), type.lmul()};
}

bool is_mir_vector_state_set(const MachineInstr &instruction) {
    return is_state_setting_instruction_impl(instruction);
}

bool is_mir_policy_only_state_set(const MachineInstr &instruction) {
    return is_policy_only_set(instruction);
}

MIRVectorState advance_mir_vector_state(MIRVectorState state, const MachineInstr &instruction) {
    static const std::unordered_map<const MachineInstr *, std::uint64_t> no_fallback;
    return transfer_instruction(std::move(state), instruction, no_fallback);
}

MIRVectorStateAnalysis analyze_mir_vector_state(const MachineFunction &function, bool verify_uses) {
    MIRVectorStateAnalysis result;
    const auto fallback_ids = fallback_identities(function);

    for (const auto &block : function.blocks()) {
        result.block_in.emplace(block.get(), MIRVectorState::unreachable());
        result.block_out.emplace(block.get(), MIRVectorState::unreachable());
    }

    bool changed = true;
    std::size_t iterations = 0;
    const auto limit = std::max<std::size_t>(8, function.blocks().size() * 8);
    while (changed && iterations++ < limit) {
        changed = false;
        for (const auto &owned_block : function.blocks()) {
            const auto *block = owned_block.get();
            const auto incoming = block_input(function, *block, result.block_out);
            const auto outgoing = transfer_block(incoming, *block, fallback_ids);
            if (result.block_in.at(block) != incoming) {
                result.block_in[block] = incoming;
                changed = true;
            }
            if (result.block_out.at(block) != outgoing) {
                result.block_out[block] = outgoing;
                changed = true;
            }
        }
    }
    if (changed) {
        result.ok = false;
        result.message = "MIR vector-state dataflow did not converge in @" + function.name();
        return result;
    }

    if (!verify_uses) {
        return result;
    }

    for (const auto &owned_block : function.blocks()) {
        const auto &block = *owned_block;
        auto state = result.block_in.at(&block);
        if (!state.reachable) {
            state = MIRVectorState::unknown();
        }
        for (const auto &instruction : block.instructions()) {
            const auto &desc = instruction_desc(instruction.opcode());
            if (is_policy_only_set(instruction)) {
                const auto desired = hardware_vtype(instruction.vector_info().vector_type);
                if (!state.vtype.has_value() || *state.vtype != desired ||
                    !state.vl_identity.has_value()) {
                    result.ok = false;
                    result.message = state_error(
                        function, block, instruction,
                        "policy-only vsetvli cannot preserve an unknown or mismatched VL");
                    return result;
                }
            } else if ((desc.implicitly_uses(MVS_VL) || desc.implicitly_uses(MVS_VTYPE)) &&
                       !is_state_setting_instruction_impl(instruction)) {
                if (!state.fully_known()) {
                    result.ok = false;
                    result.message =
                        state_error(function, block, instruction,
                                    "vector instruction has no exact reaching VL/VTYPE state");
                    return result;
                }
                if (!instruction.has_vector_info()) {
                    result.ok = false;
                    result.message =
                        state_error(function, block, instruction,
                                    "vector instruction lacks structured state metadata");
                    return result;
                }
                const auto &info = instruction.vector_info();
                if (*state.vtype != hardware_vtype(info.vector_type)) {
                    result.ok = false;
                    result.message = state_error(function, block, instruction,
                                                 "reaching VTYPE does not match SEW/LMUL");
                    return result;
                }
                if (*state.tail_policy != info.tail_policy ||
                    *state.mask_policy != info.mask_policy) {
                    result.ok = false;
                    result.message = state_error(function, block, instruction,
                                                 "reaching VTYPE does not match tail/mask policy");
                    return result;
                }
                if (info.vl_identity != 0 && *state.vl_identity != info.vl_identity) {
                    result.ok = false;
                    result.message = state_error(
                        function, block, instruction,
                        "reaching VL identity does not match the instruction requirement");
                    return result;
                }
            }
            state = transfer_instruction(std::move(state), instruction, fallback_ids);
        }
    }
    return result;
}

} // namespace mir
