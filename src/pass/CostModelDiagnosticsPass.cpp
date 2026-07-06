#include "pass/CostModelDiagnosticsPass.h"

#include "mir/MIR.h"
#include "oir/OIR.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace pass {
namespace {

using cost_model::CostVector;
using cost_model::ModuleCostSummary;

bool is_oir_int_alu(oir::Instruction::OpID op) {
    switch (op) {
    case oir::Instruction::OpID::Add:
    case oir::Instruction::OpID::Sub:
    case oir::Instruction::OpID::And:
    case oir::Instruction::OpID::Xor:
    case oir::Instruction::OpID::ICmp:
    case oir::Instruction::OpID::ZExt:
    case oir::Instruction::OpID::SIToFP:
    case oir::Instruction::OpID::FPToSI:
        return true;
    default:
        return false;
    }
}

bool is_mir_move(mir::Opcode opcode) {
    return opcode == mir::Opcode::Move || opcode == mir::Opcode::FMove ||
           opcode == mir::Opcode::FmvWX;
}

bool is_mir_branch(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::BranchNonZero:
    case mir::Opcode::BranchZero:
    case mir::Opcode::BranchEq:
    case mir::Opcode::BranchNe:
    case mir::Opcode::BranchLT:
    case mir::Opcode::BranchGE:
        return true;
    default:
        return false;
    }
}

bool is_mir_load(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::LoadSlot:
    case mir::Opcode::LoadMem:
    case mir::Opcode::LoadMemOffset:
    case mir::Opcode::LoadIncomingArg:
        return true;
    default:
        return false;
    }
}

bool is_mir_store(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::StoreSlot:
    case mir::Opcode::StoreMem:
    case mir::Opcode::StoreMemOffset:
    case mir::Opcode::StoreOutgoingArg:
        return true;
    default:
        return false;
    }
}

bool is_mir_int_alu(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::LoadImm:
    case mir::Opcode::LoadGlobalAddr:
    case mir::Opcode::LoadStackAddr:
    case mir::Opcode::Add:
    case mir::Opcode::AddW:
    case mir::Opcode::AddI:
    case mir::Opcode::AddIW:
    case mir::Opcode::Sub:
    case mir::Opcode::SubW:
    case mir::Opcode::And:
    case mir::Opcode::AndI:
    case mir::Opcode::SllI:
    case mir::Opcode::SllIW:
    case mir::Opcode::SraI:
    case mir::Opcode::SraIW:
    case mir::Opcode::Srli:
    case mir::Opcode::SrliW:
    case mir::Opcode::Xor:
    case mir::Opcode::XorI:
    case mir::Opcode::Slt:
    case mir::Opcode::Sltu:
    case mir::Opcode::SeqZ:
    case mir::Opcode::Snez:
        return true;
    default:
        return false;
    }
}

bool is_mir_fp_alu(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::LoadFloatImm:
    case mir::Opcode::FAddS:
    case mir::Opcode::FSubS:
    case mir::Opcode::FMulS:
    case mir::Opcode::FeqS:
    case mir::Opcode::FltS:
    case mir::Opcode::FleS:
    case mir::Opcode::FcvtSW:
    case mir::Opcode::FcvtWS:
        return true;
    default:
        return false;
    }
}

void add_oir_instruction_cost(const oir::Instruction &inst, CostVector &cost) {
    ++cost.static_instrs;
    cost.code_bytes += 4;
    switch (inst.op()) {
    case oir::Instruction::OpID::Mul:
        ++cost.int_mul;
        break;
    case oir::Instruction::OpID::SDiv:
    case oir::Instruction::OpID::SRem:
        ++cost.int_div_rem;
        break;
    case oir::Instruction::OpID::FDiv:
        ++cost.fp_div;
        break;
    case oir::Instruction::OpID::FAdd:
    case oir::Instruction::OpID::FSub:
    case oir::Instruction::OpID::FMul:
    case oir::Instruction::OpID::FCmp:
        ++cost.fp_alu;
        break;
    case oir::Instruction::OpID::Load:
        ++cost.loads;
        break;
    case oir::Instruction::OpID::Store:
        ++cost.stores;
        break;
    case oir::Instruction::OpID::GetElementPtr:
        ++cost.pointer_arith;
        break;
    case oir::Instruction::OpID::Call:
        ++cost.calls;
        break;
    case oir::Instruction::OpID::Br:
        ++cost.branches;
        break;
    case oir::Instruction::OpID::Phi:
        ++cost.phis;
        ++cost.live_values;
        break;
    case oir::Instruction::OpID::MemZero:
        ++cost.stores;
        ++cost.memzero_bytes;
        break;
    default:
        if (is_oir_int_alu(inst.op())) {
            ++cost.int_alu;
        }
        break;
    }
    if (!inst.type()->is_void()) {
        ++cost.live_values;
    }
}

ModuleCostSummary collect_oir_summary(const oir::Module &module,
                                      cost_model::CostIRStage stage,
                                      const std::string &label) {
    ModuleCostSummary summary;
    summary.stage = stage;
    summary.label = label;
    summary.globals = static_cast<std::int64_t>(module.globals().size());
    for (const auto &function : module.functions()) {
        if (function->is_external()) {
            ++summary.external_functions;
            continue;
        }
        ++summary.functions;
        std::int64_t function_live_values = 0;
        for (const auto &block : function->blocks()) {
            ++summary.basic_blocks;
            for (const auto &inst : block->instructions()) {
                const auto before_live = summary.cost.live_values;
                add_oir_instruction_cost(*inst, summary.cost);
                function_live_values += summary.cost.live_values - before_live;
            }
        }
        summary.cost.max_live_values =
            std::max(summary.cost.max_live_values, function_live_values);
    }
    summary.cost.dynamic_instrs = summary.cost.static_instrs;
    return summary;
}

void add_mir_instruction_cost(const mir::MachineInstr &instr, CostVector &cost) {
    ++cost.static_instrs;
    cost.code_bytes += 4;
    const auto opcode = instr.opcode();
    if (is_mir_int_alu(opcode)) {
        ++cost.int_alu;
    }
    if (opcode == mir::Opcode::Mul || opcode == mir::Opcode::MulW) {
        ++cost.int_mul;
    }
    if (opcode == mir::Opcode::DivU || opcode == mir::Opcode::DivW ||
        opcode == mir::Opcode::RemW) {
        ++cost.int_div_rem;
    }
    if (is_mir_fp_alu(opcode)) {
        ++cost.fp_alu;
    }
    if (opcode == mir::Opcode::FDivS) {
        ++cost.fp_div;
    }
    if (is_mir_load(opcode)) {
        ++cost.loads;
    }
    if (is_mir_store(opcode)) {
        ++cost.stores;
    }
    if (is_mir_branch(opcode)) {
        ++cost.branches;
    }
    if (opcode == mir::Opcode::Jump) {
        ++cost.jumps;
    }
    if (opcode == mir::Opcode::Call) {
        ++cost.calls;
    }
    if (is_mir_move(opcode)) {
        ++cost.moves;
    }
    if (opcode == mir::Opcode::MemZero) {
        ++cost.memzero_bytes;
    }
    for (const auto &operand : instr.operands()) {
        if (operand.is_reg() && operand.reg_value().is_virtual()) {
            ++cost.virtual_regs;
        }
    }
}

ModuleCostSummary collect_mir_summary(const mir::Module &module,
                                      cost_model::CostIRStage stage,
                                      const std::string &label) {
    ModuleCostSummary summary;
    summary.stage = stage;
    summary.label = label;
    summary.globals = static_cast<std::int64_t>(module.globals().size());
    for (const auto &function : module.functions()) {
        if (function->is_external()) {
            ++summary.external_functions;
            continue;
        }
        ++summary.functions;
        summary.cost.stack_slots += static_cast<std::int64_t>(function->stack_slots().size());
        summary.cost.virtual_regs +=
            static_cast<std::int64_t>(function->regs().virtual_registers().size());
        summary.cost.max_live_values = std::max(
            summary.cost.max_live_values,
            static_cast<std::int64_t>(function->regs().virtual_registers().size()));
        for (const auto &slot : function->stack_slots()) {
            if (slot.kind == mir::StackSlotKind::Spill) {
                ++summary.cost.estimated_spills;
            }
        }
        for (const auto &block : function->blocks()) {
            ++summary.basic_blocks;
            summary.cost.max_live_values =
                std::max(summary.cost.max_live_values,
                         static_cast<std::int64_t>(block->live_in().size()));
            summary.cost.max_live_values =
                std::max(summary.cost.max_live_values,
                         static_cast<std::int64_t>(block->live_out().size()));
            for (const auto &instr : block->instructions()) {
                add_mir_instruction_cost(instr, summary.cost);
            }
        }
    }
    summary.cost.dynamic_instrs = summary.cost.static_instrs;
    return summary;
}

} // namespace

CostModelDiagnosticsPass::CostModelDiagnosticsPass(cost_model::CostIRStage stage,
                                                   std::string label,
                                                   cost_model::CostModelPolicyKind policy,
                                                   std::string filter)
    : stage_(stage), label_(std::move(label)), policy_(policy), filter_(std::move(filter)) {
}

std::string_view CostModelDiagnosticsPass::name() const {
    return "CostModelDiagnosticsPass";
}

PassKind CostModelDiagnosticsPass::kind() const {
    return PassKind::Analysis;
}

PassResult CostModelDiagnosticsPass::run(PassContext &context) {
    cost_model::CostModelReport local;
    local.target = cost_model::default_target_profile();
    local.policy = policy_;
    local.filter = filter_;

    if (stage_ == cost_model::CostIRStage::OIR) {
        auto *module = context.ssa_module();
        if (module == nullptr) {
            return PassResult::fail("CostModelDiagnosticsPass requires OIR module");
        }
        local.summaries.push_back(collect_oir_summary(*module, stage_, label_));
    } else {
        auto *module = context.machine_module();
        if (module == nullptr) {
            return PassResult::fail("CostModelDiagnosticsPass requires MIR module");
        }
        local.summaries.push_back(collect_mir_summary(*module, stage_, label_));
    }

    auto *report = context.get_artifact<cost_model::CostModelReport>(kReportArtifactKey);
    if (report == nullptr) {
        context.set_artifact<cost_model::CostModelReport>(kReportArtifactKey, std::move(local));
    } else {
        cost_model::merge_report(*report, local);
    }
    return PassResult::ok(false);
}

} // namespace pass
