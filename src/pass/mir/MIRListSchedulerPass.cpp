#include "pass/mir/MIRListSchedulerPass.h"

#include "mir/MIRVerifier.h"
#include "pass/mir/MIRCostModel.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace pass {
namespace {

struct Stats {
    unsigned blocks = 0;
    unsigned regions = 0;
    unsigned instructions = 0;
    cost_model::CostModelReport *cost_model_report = nullptr;
    cost_model::CostModelPolicyKind cost_model_policy =
        cost_model::CostModelPolicyKind::Balanced;
    std::string cost_model_filter;

    std::string message() const {
        std::ostringstream oss;
        oss << "scheduled block=" << blocks << " region=" << regions
            << " instr=" << instructions;
        return oss.str();
    }
};

struct SUnit {
    std::size_t original_index = 0;
    unsigned latency = 1;
    unsigned height = 1;
    unsigned ready_cycle = 0;
    unsigned scheduled_cycle = 0;
    unsigned unscheduled_preds = 0;
    std::vector<std::size_t> succs;
};

struct SchedulePlan {
    std::vector<std::size_t> order;
    unsigned serial_cycles = 0;
    unsigned scheduled_cycles = 0;
};

bool is_branch(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::BranchNonZero:
    case mir::Opcode::BranchZero:
    case mir::Opcode::BranchEq:
    case mir::Opcode::BranchNe:
    case mir::Opcode::BranchLT:
    case mir::Opcode::BranchGE:
    case mir::Opcode::Jump:
        return true;
    default:
        return false;
    }
}

bool is_store_or_side_effect(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::StoreSlot:
    case mir::Opcode::StoreMem:
    case mir::Opcode::StoreMemOffset:
    case mir::Opcode::MemZero:
    case mir::Opcode::StoreOutgoingArg:
    case mir::Opcode::Call:
        return true;
    default:
        return false;
    }
}

bool is_zero_reg(const mir::Register &reg) {
    return reg.is_physical() && reg.name == "zero";
}

bool has_prera_fixed_physical_operand(const mir::MachineInstr &instr) {
    for (const auto &operand : instr.operands()) {
        if (operand.is_reg() && operand.reg_value().is_physical() &&
            !is_zero_reg(operand.reg_value())) {
            return true;
        }
    }
    return false;
}

bool is_schedulable_instr(const mir::MachineInstr &instr, bool post_ra) {
    const auto opcode = instr.opcode();
    if (is_branch(opcode) || is_store_or_side_effect(opcode) || opcode == mir::Opcode::Comment) {
        return false;
    }
    if (!post_ra && has_prera_fixed_physical_operand(instr)) {
        return false;
    }

    switch (opcode) {
    case mir::Opcode::LoadFloatImm:
        return false;
    case mir::Opcode::LoadSlot:
    case mir::Opcode::LoadIncomingArg:
    case mir::Opcode::LoadStackAddr:
        return !post_ra;
    default:
        return true;
    }
}

unsigned opcode_latency(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::LoadMem:
    case mir::Opcode::LoadMemOffset:
    case mir::Opcode::LoadSlot:
    case mir::Opcode::LoadIncomingArg:
        return 4;
    case mir::Opcode::Mul:
    case mir::Opcode::MulW:
    case mir::Opcode::FMulS:
        return 4;
    case mir::Opcode::FAddS:
    case mir::Opcode::FSubS:
    case mir::Opcode::FeqS:
    case mir::Opcode::FltS:
    case mir::Opcode::FleS:
    case mir::Opcode::FcvtSW:
    case mir::Opcode::FcvtWS:
    case mir::Opcode::FmvWX:
        return 3;
    case mir::Opcode::DivW:
    case mir::Opcode::DivU:
    case mir::Opcode::RemW:
        return 12;
    case mir::Opcode::FDivS:
        return 16;
    default:
        return 1;
    }
}

bool same_reg(const mir::Register &lhs, const mir::Register &rhs) {
    return lhs == rhs;
}

bool intersects(const std::vector<mir::Register> &lhs,
                const std::vector<mir::Register> &rhs) {
    for (const auto &left : lhs) {
        for (const auto &right : rhs) {
            if (same_reg(left, right)) {
                return true;
            }
        }
    }
    return false;
}

bool has_register_dependency(const mir::MachineInstr &before,
                             const mir::MachineInstr &after) {
    const auto before_defs = before.defs();
    const auto before_uses = before.uses();
    const auto after_defs = after.defs();
    const auto after_uses = after.uses();

    return intersects(before_defs, after_uses) || intersects(before_uses, after_defs) ||
           intersects(before_defs, after_defs);
}

void add_edge(std::vector<SUnit> &nodes, std::size_t pred, std::size_t succ) {
    auto &succs = nodes[pred].succs;
    if (std::find(succs.begin(), succs.end(), succ) != succs.end()) {
        return;
    }
    succs.push_back(succ);
    ++nodes[succ].unscheduled_preds;
}

std::vector<SUnit> build_dag(const std::vector<mir::MachineInstr> &instrs, std::size_t begin,
                             std::size_t end) {
    const std::size_t count = end - begin;
    std::vector<SUnit> nodes(count);
    for (std::size_t i = 0; i < count; ++i) {
        nodes[i].original_index = i;
        nodes[i].latency = opcode_latency(instrs[begin + i].opcode());
        nodes[i].height = nodes[i].latency;
    }

    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            if (has_register_dependency(instrs[begin + i], instrs[begin + j])) {
                add_edge(nodes, i, j);
            }
        }
    }

    for (std::size_t i = count; i > 0; --i) {
        auto &node = nodes[i - 1];
        unsigned best_succ_height = 0;
        for (auto succ : node.succs) {
            best_succ_height = std::max(best_succ_height, nodes[succ].height);
        }
        node.height = node.latency + best_succ_height;
    }

    return nodes;
}

bool better_candidate(const std::vector<SUnit> &nodes, std::size_t lhs, std::size_t rhs) {
    const auto &left = nodes[lhs];
    const auto &right = nodes[rhs];
    if (left.height != right.height) {
        return left.height > right.height;
    }
    if (left.latency != right.latency) {
        return left.latency > right.latency;
    }
    return left.original_index < right.original_index;
}

std::size_t pick_ready_node(const std::vector<SUnit> &nodes,
                            const std::vector<std::size_t> &ready, unsigned &cycle) {
    bool has_cycle_ready = false;
    unsigned next_ready_cycle = std::numeric_limits<unsigned>::max();
    for (auto node_index : ready) {
        if (nodes[node_index].ready_cycle <= cycle) {
            has_cycle_ready = true;
        }
        next_ready_cycle = std::min(next_ready_cycle, nodes[node_index].ready_cycle);
    }

    if (!has_cycle_ready) {
        cycle = next_ready_cycle;
    }

    std::size_t best = ready.front();
    for (auto node_index : ready) {
        if (nodes[node_index].ready_cycle > cycle) {
            continue;
        }
        if (nodes[best].ready_cycle > cycle || better_candidate(nodes, node_index, best)) {
            best = node_index;
        }
    }
    return best;
}

SchedulePlan schedule_order(const std::vector<mir::MachineInstr> &instrs,
                            std::size_t begin, std::size_t end) {
    auto nodes = build_dag(instrs, begin, end);
    SchedulePlan plan;
    plan.order.reserve(nodes.size());
    for (const auto &node : nodes) {
        plan.serial_cycles += node.latency;
    }

    std::vector<std::size_t> ready;
    ready.reserve(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].unscheduled_preds == 0) {
            ready.push_back(i);
        }
    }

    unsigned cycle = 0;
    while (!ready.empty()) {
        const auto picked = pick_ready_node(nodes, ready, cycle);
        ready.erase(std::remove(ready.begin(), ready.end(), picked), ready.end());

        nodes[picked].scheduled_cycle = cycle;
        plan.order.push_back(picked);
        plan.scheduled_cycles =
            std::max(plan.scheduled_cycles, cycle + nodes[picked].latency);

        const unsigned result_ready_cycle = cycle + nodes[picked].latency;
        for (auto succ : nodes[picked].succs) {
            nodes[succ].ready_cycle = std::max(nodes[succ].ready_cycle, result_ready_cycle);
            --nodes[succ].unscheduled_preds;
            if (nodes[succ].unscheduled_preds == 0) {
                ready.push_back(succ);
            }
        }
        ++cycle;
    }
    return plan;
}

bool schedule_window(std::vector<mir::MachineInstr> &instrs, std::size_t begin,
                     std::size_t end, bool post_ra, Stats &stats) {
    if (end - begin < 3) {
        return false;
    }

    auto plan = schedule_order(instrs, begin, end);
    bool changed = false;
    unsigned moved = 0;
    for (std::size_t i = 0; i < plan.order.size(); ++i) {
        if (plan.order[i] != i) {
            changed = true;
            ++moved;
        }
    }
    if (!changed) {
        return false;
    }
    pass::mir_cost_model::MIRTransformCostEstimate estimate;
    estimate.kind = pass::cost_model::TransformKind::InstructionScheduling;
    estimate.stage =
        post_ra ? pass::cost_model::CostIRStage::PostRAMIR
                : pass::cost_model::CostIRStage::PreRAMIR;
    estimate.pass_name =
        post_ra ? "MIRPostRAListSchedulerPass" : "MIRPreRAListSchedulerPass";
    estimate.candidate_id = "schedule." + std::to_string(stats.regions + 1);
    estimate.scope = post_ra ? "post_ra_block_window" : "pre_ra_block_window";
    estimate.proof_summary = "register dependency DAG preserves side-effect order";
    estimate.confidence = post_ra ? 0.60 : 0.64;
    estimate.before_cycles = plan.serial_cycles;
    estimate.after_cycles = plan.scheduled_cycles;
    estimate.risk.register_pressure_growth = post_ra ? 0 : 1;
    if (!pass::mir_cost_model::allows_transform(stats.cost_model_report,
                                                stats.cost_model_policy,
                                                stats.cost_model_filter, estimate)) {
        return false;
    }

    std::vector<mir::MachineInstr> scheduled;
    scheduled.reserve(plan.order.size());
    for (auto local_index : plan.order) {
        scheduled.push_back(std::move(instrs[begin + local_index]));
    }
    for (std::size_t i = 0; i < scheduled.size(); ++i) {
        instrs[begin + i] = std::move(scheduled[i]);
    }

    ++stats.regions;
    stats.instructions += moved;
    return true;
}

bool schedule_block(mir::MachineBasicBlock &block, bool post_ra, Stats &stats) {
    auto &instrs = block.instructions();
    const std::size_t max_window = post_ra ? 8 : 64;
    bool changed = false;

    for (std::size_t index = 0; index < instrs.size();) {
        while (index < instrs.size() &&
               !is_schedulable_instr(instrs[index], post_ra)) {
            ++index;
        }
        const std::size_t region_begin = index;
        while (index < instrs.size() &&
               is_schedulable_instr(instrs[index], post_ra)) {
            ++index;
        }
        const std::size_t region_end = index;

        for (std::size_t chunk_begin = region_begin; chunk_begin < region_end;
             chunk_begin += max_window) {
            const auto chunk_end = std::min(region_end, chunk_begin + max_window);
            changed |= schedule_window(instrs, chunk_begin, chunk_end, post_ra, stats);
        }
    }

    if (changed) {
        ++stats.blocks;
    }
    return changed;
}

bool schedule_function(mir::MachineFunction &function, bool post_ra, Stats &stats) {
    if (post_ra && (function.has_call() || function.frame_size() > 2047)) {
        return false;
    }

    bool changed = false;
    for (auto &block : function.blocks()) {
        changed |= schedule_block(*block, post_ra, stats);
    }
    return changed;
}

} // namespace

MIRListSchedulerPass::MIRListSchedulerPass(bool post_ra) : post_ra_(post_ra) {
}

std::string_view MIRListSchedulerPass::name() const {
    return post_ra_ ? "MIRPostRAListSchedulerPass" : "MIRPreRAListSchedulerPass";
}

PassKind MIRListSchedulerPass::kind() const {
    return PassKind::Transform;
}

PassResult MIRListSchedulerPass::run(PassContext &context) {
    auto *module = context.machine_module();
    if (module == nullptr) {
        return PassResult::fail("MIRListSchedulerPass requires MIR module in pass context");
    }

    Stats total;
    auto *cost_model_report =
        context.get_artifact<cost_model::CostModelReport>(cost_model::kReportArtifactKey);
    if (cost_model_report != nullptr) {
        total.cost_model_report = cost_model_report;
        total.cost_model_policy = cost_model_report->policy;
        total.cost_model_filter = cost_model_report->filter;
    }
    bool changed = false;
    for (auto &function : module->functions()) {
        if (function->is_external()) {
            continue;
        }
        function->rebuild_cfg();
        changed |= schedule_function(*function, post_ra_, total);
        function->rebuild_cfg();
    }

    auto verify = mir::verify_module(
        *module, post_ra_ ? mir::MIRVerificationStage::PostRA : mir::MIRVerificationStage::PreRA);
    if (!verify.ok) {
        return PassResult::fail(verify.message);
    }

    context.set_artifact(std::string(name()), total.message());
    return PassResult::ok(changed, total.message());
}

} // namespace pass
