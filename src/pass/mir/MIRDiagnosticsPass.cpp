#include "pass/mir/MIRDiagnosticsPass.h"

#include "mir/MachineInstrDesc.h"
#include "mir/MIRPrinter.h"

#include <sstream>
#include <utility>
#include <vector>

namespace pass {
namespace {

bool is_move(mir::Opcode opcode) {
    return opcode == mir::Opcode::Move || opcode == mir::Opcode::FMove ||
           opcode == mir::Opcode::FmvWX || opcode == mir::Opcode::FmvXW;
}

MIRStageMetrics collect_metrics(const std::string &stage, const mir::Module &module) {
    MIRStageMetrics metrics;
    metrics.stage = stage;

    for (const auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }

        ++metrics.functions;
        metrics.stack_slots += static_cast<std::int64_t>(function->stack_slots().size());
        for (const auto &slot : function->stack_slots()) {
            if (slot.kind == mir::StackSlotKind::Spill) {
                ++metrics.spills;
            }
        }

        for (const auto &block : function->blocks()) {
            ++metrics.basic_blocks;
            for (const auto &instr : block->instructions()) {
                const auto opcode = instr.opcode();
                const auto &desc = mir::instruction_desc(opcode);
                ++metrics.instructions;
                if (is_move(opcode)) {
                    ++metrics.moves;
                }
                if (opcode == mir::Opcode::Jump) {
                    ++metrics.jumps;
                }
                if (desc.has_flag(mir::MIF_Branch) && opcode != mir::Opcode::Jump) {
                    ++metrics.branches;
                }
                if (desc.memory == mir::MachineMemoryEffect::Read ||
                    desc.memory == mir::MachineMemoryEffect::ReadWrite) {
                    ++metrics.loads;
                }
                if (desc.memory == mir::MachineMemoryEffect::Write ||
                    desc.memory == mir::MachineMemoryEffect::ReadWrite) {
                    ++metrics.stores;
                }
                if (opcode == mir::Opcode::LoadSlot) {
                    ++metrics.load_slots;
                }
                if (opcode == mir::Opcode::StoreSlot) {
                    ++metrics.store_slots;
                }
                if (mir::machine_instr_may_call(instr)) {
                    ++metrics.calls;
                }
            }
        }
    }

    return metrics;
}

} // namespace

MIRDiagnosticsPass::MIRDiagnosticsPass(std::string stage,
                                       mir::MIRVerificationStage verification_stage)
    : stage_(std::move(stage)), verification_stage_(verification_stage) {
}

std::string MIRDiagnosticsPass::dump_artifact_key(std::string_view stage) {
    std::string key = "mir.stage.";
    key.append(stage.data(), stage.size());
    key += ".dump";
    return key;
}

std::string_view MIRDiagnosticsPass::name() const {
    return "MIRDiagnosticsPass";
}

PassKind MIRDiagnosticsPass::kind() const {
    return PassKind::Analysis;
}

PassResult MIRDiagnosticsPass::run(PassContext &context) {
    auto *module = context.machine_module();
    if (module == nullptr) {
        return PassResult::fail("MIRDiagnosticsPass requires MIR module in pass context");
    }

    auto verify = mir::verify_module(*module, verification_stage_);
    if (!verify.ok) {
        return PassResult::fail(verify.message);
    }

    std::ostringstream dump;
    mir::MIRPrinter printer(dump);
    printer.print(*module);
    context.set_artifact<std::string>(dump_artifact_key(stage_), dump.str());

    auto metrics = collect_metrics(stage_, *module);
    auto *all_metrics =
        context.get_artifact<std::vector<MIRStageMetrics>>(kMetricsArtifactKey);
    if (all_metrics == nullptr) {
        std::vector<MIRStageMetrics> initial;
        initial.push_back(std::move(metrics));
        context.set_artifact<std::vector<MIRStageMetrics>>(kMetricsArtifactKey,
                                                           std::move(initial));
    } else {
        all_metrics->push_back(std::move(metrics));
    }

    return PassResult::ok(false);
}

} // namespace pass
