#include "pass/mir/MIRVectorStatePass.h"

#include "mir/MIRVectorState.h"
#include "mir/MachineInstrDesc.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pass {
namespace {

mir::Register state_register(const char *name) {
    return mir::Register::physical(name, mir::RegisterClass::VSTATE, mir::ValueType::Void);
}

bool zero_register(const mir::MachineOperand &operand) {
    return operand.is_reg() && operand.reg_value().is_physical() &&
           operand.reg_value().name == "zero";
}

std::optional<mir::MIRVLRequest> request_for(const mir::MachineInstr &instruction) {
    if (instruction.operands().size() < 2) {
        return std::nullopt;
    }
    const auto &operand = instruction.operands()[1];
    mir::MIRVLRequest request;
    if (operand.kind() == mir::OperandKind::Imm) {
        request.kind = mir::MIRVLRequestKind::Immediate;
        request.immediate = operand.int_value();
        return request;
    }
    if (!operand.is_reg()) {
        return std::nullopt;
    }
    if (zero_register(operand)) {
        request.kind = mir::MIRVLRequestKind::VLMAX;
    } else {
        request.kind = mir::MIRVLRequestKind::Register;
        request.reg = operand.reg_value();
    }
    return request;
}

std::string canonical_request_key(const mir::MIRVLRequest &request) {
    std::ostringstream out;
    switch (request.kind) {
    case mir::MIRVLRequestKind::Immediate:
        out << "imm:" << request.immediate;
        break;
    case mir::MIRVLRequestKind::VLMAX:
        out << "vlmax";
        break;
    case mir::MIRVLRequestKind::Register:
        if (!request.reg.is_virtual()) {
            return {};
        }
        out << "vreg:" << static_cast<unsigned>(request.reg.reg_class) << ':' << request.reg.id;
        break;
    }
    return out.str();
}

void assign_vl_identities(mir::MachineFunction &function) {
    std::uint64_t next = 1;
    for (const auto &block : function.blocks()) {
        for (const auto &instruction : block->instructions()) {
            if (instruction.has_vector_info()) {
                next = std::max(next, instruction.vector_info().vl_identity + 1);
            }
        }
    }

    std::map<std::string, std::uint64_t> canonical;
    for (auto &block : function.blocks()) {
        for (auto &instruction : block->instructions()) {
            if (!mir::is_mir_vector_state_set(instruction) ||
                mir::is_mir_policy_only_state_set(instruction)) {
                continue;
            }
            auto &info = instruction.vector_info();
            if (info.vl_identity != 0) {
                continue;
            }
            const auto request = request_for(instruction);
            if (!request.has_value()) {
                info.vl_identity = next++;
                continue;
            }
            const auto key = canonical_request_key(*request);
            if (key.empty()) {
                info.vl_identity = next++;
                continue;
            }
            auto [found, inserted] = canonical.emplace(key, next);
            if (inserted) {
                ++next;
            }
            info.vl_identity = found->second;
        }
    }
}

bool state_matches(const mir::MIRVectorState &state, const mir::MachineVectorInfo &info) {
    return state.fully_known() && *state.vtype == mir::hardware_vtype(info.vector_type) &&
           *state.tail_policy == info.tail_policy && *state.mask_policy == info.mask_policy &&
           (info.vl_identity == 0 || *state.vl_identity == info.vl_identity);
}

mir::MachineInstr make_state_set(mir::MachineFunction &function, const mir::MIRVectorState &state,
                                 const mir::MachineVectorInfo &consumer_info) {
    if (!state.vl_identity.has_value() || !state.vl_request.has_value()) {
        throw std::runtime_error("MIR vector-state insertion lacks a reaching AVL identity");
    }

    const auto desired_vtype = mir::hardware_vtype(consumer_info.vector_type);
    const bool policy_only = state.vtype.has_value() && *state.vtype == desired_vtype;
    std::vector<mir::MachineOperand> operands;
    if (policy_only) {
        operands.push_back(mir::MachineOperand::reg_def(
            mir::Register::physical("zero", mir::RegisterClass::GPR, mir::ValueType::I32)));
        operands.push_back(mir::MachineOperand::reg_use(
            mir::Register::physical("zero", mir::RegisterClass::GPR, mir::ValueType::I32)));
    } else {
        auto result = function.regs().create_virtual(mir::RegisterClass::GPR, mir::ValueType::I32);
        operands.push_back(mir::MachineOperand::reg_def(result));
        switch (state.vl_request->kind) {
        case mir::MIRVLRequestKind::Immediate:
            if (state.vl_request->immediate < 0 || state.vl_request->immediate > 31) {
                throw std::runtime_error(
                    "MIR vector-state insertion cannot encode a non-uimm5 AVL");
            }
            operands.push_back(mir::MachineOperand::imm(state.vl_request->immediate));
            break;
        case mir::MIRVLRequestKind::Register:
            operands.push_back(mir::MachineOperand::reg_use(state.vl_request->reg));
            break;
        case mir::MIRVLRequestKind::VLMAX:
            operands.push_back(mir::MachineOperand::reg_use(
                mir::Register::physical("zero", mir::RegisterClass::GPR, mir::ValueType::I32)));
            break;
        }
    }
    operands.push_back(mir::MachineOperand::implicit_reg_def(state_register("vl")));
    operands.push_back(mir::MachineOperand::implicit_reg_def(state_register("vtype")));

    mir::MachineVectorInfo info(consumer_info.vector_type);
    info.operation = mir::RVVOperation::SetVL;
    info.avl = mir::MachineVectorAVL::operand(1);
    info.tail_policy = consumer_info.tail_policy;
    info.mask_policy = consumer_info.mask_policy;
    info.vl_identity = *state.vl_identity;
    return mir::MachineInstr(mir::Opcode::RVVSetVLI, std::move(operands), std::move(info));
}

bool insert_required_sets(mir::MachineFunction &function, MIRVectorStateMetrics &metrics,
                          std::string &error) {
    auto analysis = mir::analyze_mir_vector_state(function, false);
    if (!analysis.ok) {
        error = std::move(analysis.message);
        return false;
    }

    for (auto &owned_block : function.blocks()) {
        auto &block = *owned_block;
        auto state = analysis.block_in.at(&block);
        if (!state.reachable) {
            state = mir::MIRVectorState::unknown();
        }
        auto &instructions = block.instructions();
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            const auto &desc = mir::instruction_desc(instructions[index].opcode());
            const bool consumes_state =
                (desc.implicitly_uses(mir::MVS_VL) || desc.implicitly_uses(mir::MVS_VTYPE)) &&
                !mir::is_mir_vector_state_set(instructions[index]);
            if (consumes_state && instructions[index].has_vector_info() &&
                !state_matches(state, instructions[index].vector_info()) &&
                state.vl_identity.has_value() && state.vl_request.has_value()) {
                try {
                    auto inserted =
                        make_state_set(function, state, instructions[index].vector_info());
                    instructions.insert(instructions.begin() + static_cast<std::ptrdiff_t>(index),
                                        std::move(inserted));
                    ++metrics.inserted_vset;
                    state = mir::advance_mir_vector_state(std::move(state), instructions[index]);
                    ++index;
                } catch (const std::exception &exception) {
                    error = exception.what();
                    return false;
                }
            }
            auto &current = instructions[index];
            if (mir::is_mir_policy_only_state_set(current) && current.has_vector_info() &&
                current.vector_info().vl_identity == 0 && state.vl_identity.has_value()) {
                current.vector_info().vl_identity = *state.vl_identity;
            }
            const auto &current_desc = mir::instruction_desc(current.opcode());
            const bool current_consumes_state = (current_desc.implicitly_uses(mir::MVS_VL) ||
                                                 current_desc.implicitly_uses(mir::MVS_VTYPE)) &&
                                                !mir::is_mir_vector_state_set(current);
            if (current_consumes_state && current.has_vector_info() &&
                current.vector_info().vl_identity == 0 && state.vl_identity.has_value()) {
                current.vector_info().vl_identity = *state.vl_identity;
            }
            state = mir::advance_mir_vector_state(std::move(state), current);
        }
    }
    return true;
}

bool register_is_used(const mir::MachineFunction &function, const mir::Register &reg,
                      const mir::MachineInstr *definition) {
    for (const auto &block : function.blocks()) {
        for (const auto &instruction : block->instructions()) {
            for (const auto &operand : instruction.operands()) {
                if (&instruction == definition && operand.is_def() && operand.reg_value() == reg) {
                    continue;
                }
                if (operand.is_reg() && operand.is_use() && operand.reg_value() == reg) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool disposable_set_result(const mir::MachineFunction &function,
                           const mir::MachineInstr &instruction) {
    if (instruction.operands().empty() || !instruction.operands()[0].is_reg() ||
        !instruction.operands()[0].is_def()) {
        return false;
    }
    const auto &reg = instruction.operands()[0].reg_value();
    if (reg.is_physical()) {
        return reg.name == "zero";
    }
    return !register_is_used(function, reg, &instruction);
}

bool eliminate_redundant_sets(mir::MachineFunction &function, MIRVectorStateMetrics &metrics) {
    const auto analysis = mir::analyze_mir_vector_state(function, false);
    if (!analysis.ok) {
        return false;
    }
    bool changed = false;
    for (auto &owned_block : function.blocks()) {
        auto &block = *owned_block;
        auto state = analysis.block_in.at(&block);
        if (!state.reachable) {
            state = mir::MIRVectorState::unknown();
        }
        auto &instructions = block.instructions();
        for (auto iter = instructions.begin(); iter != instructions.end();) {
            auto &instruction = *iter;
            if (mir::is_mir_policy_only_state_set(instruction) && instruction.has_vector_info() &&
                instruction.vector_info().vl_identity == 0 && state.vl_identity.has_value()) {
                instruction.vector_info().vl_identity = *state.vl_identity;
            }
            auto after = mir::advance_mir_vector_state(state, instruction);
            if (mir::is_mir_vector_state_set(instruction) && state == after &&
                disposable_set_result(function, instruction)) {
                iter = instructions.erase(iter);
                ++metrics.removed_vset;
                changed = true;
                continue;
            }
            state = std::move(after);
            ++iter;
        }
    }
    return changed;
}

} // namespace

std::string_view MIRVectorStatePass::name() const {
    return "MIRVectorStatePass";
}

PassKind MIRVectorStatePass::kind() const {
    return PassKind::Transform;
}

PassResult MIRVectorStatePass::run(PassContext &context) {
    auto *module = context.machine_module();
    if (module == nullptr) {
        return PassResult::fail("MIRVectorStatePass requires MIR module in pass context");
    }

    MIRVectorStateMetrics metrics;
    try {
        for (auto &function : module->functions()) {
            if (function->is_external()) {
                continue;
            }
            function->rebuild_cfg();
            assign_vl_identities(*function);
            std::string error;
            if (!insert_required_sets(*function, metrics, error)) {
                return PassResult::fail(std::move(error));
            }
            while (eliminate_redundant_sets(*function, metrics)) {
                function->rebuild_cfg();
            }
            const auto analysis = mir::analyze_mir_vector_state(*function, true);
            if (!analysis.ok) {
                return PassResult::fail(analysis.message);
            }
        }
        context.set_artifact<MIRVectorStateMetrics>(kMetricsArtifactKey, metrics);
        return PassResult::ok(metrics.inserted_vset != 0 || metrics.removed_vset != 0);
    } catch (const std::exception &exception) {
        return PassResult::fail(exception.what());
    }
}

} // namespace pass
