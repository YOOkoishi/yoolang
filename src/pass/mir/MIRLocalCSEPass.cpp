#include "pass/mir/MIRLocalCSEPass.h"

#include "mir/MachineInstrDesc.h"
#include "pass/mir/MIRCostModel.h"
#include "pass/mir/MIRPeepholeCommon.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <set>
#include <sstream>
#include <vector>

namespace pass::mir_peephole {
namespace {

using Block = mir::MachineBasicBlock;
using BlockSet = std::set<Block *>;
using AvailableMap = std::map<std::string, mir::Register>;
using RegisterVersions = std::map<VRegId, std::uint64_t>;

enum class CSEScope {
    Local,
    Global,
};

std::string reg_key(const mir::Register &reg) {
    if (reg.is_virtual()) {
        return "v" + std::to_string(reg.id);
    }
    return "p" + reg.name;
}

bool stable_operand_for_cse(const mir::MachineOperand &operand,
                            const std::map<VRegId, RegCounts> *counts) {
    if (!operand.is_reg()) {
        return true;
    }
    const auto &reg = operand.reg_value();
    if (is_zero_reg(reg)) {
        return true;
    }
    if (!reg.is_virtual()) {
        return false;
    }
    return counts == nullptr || def_count(*counts, reg) == 1;
}

std::uint64_t register_version(const RegisterVersions &versions, const mir::Register &reg) {
    if (!reg.is_virtual()) {
        return 0;
    }
    auto found = versions.find(reg.id);
    return found == versions.end() ? 0 : found->second;
}

std::string operand_key(const mir::MachineOperand &operand, const RegisterVersions *versions) {
    switch (operand.kind()) {
    case mir::OperandKind::Reg:
    case mir::OperandKind::FReg: {
        auto key = reg_key(operand.reg_value());
        if (versions != nullptr && operand.reg_value().is_virtual()) {
            key += "@" + std::to_string(register_version(*versions, operand.reg_value()));
        }
        return key;
    }
    case mir::OperandKind::Imm:
        return "i" + std::to_string(operand.int_value());
    case mir::OperandKind::FloatImm: {
        std::uint32_t bits = 0;
        const float value = operand.float_value();
        static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
        std::memcpy(&bits, &value, sizeof(bits));
        return "f" + std::to_string(bits);
    }
    case mir::OperandKind::Slot:
        return "s" + std::to_string(operand.slot_id());
    case mir::OperandKind::Global:
        return "g" + operand.string_value();
    case mir::OperandKind::Block:
        return "b" + operand.string_value();
    case mir::OperandKind::Symbol:
        return "y" + operand.string_value();
    case mir::OperandKind::Type:
        return "t" + std::to_string(static_cast<int>(operand.type_value()));
    case mir::OperandKind::Text:
        return "x" + operand.string_value();
    }
    return "";
}

bool global_cse_opcode(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::LoadImm:
    case mir::Opcode::LoadFloatImm:
    case mir::Opcode::LoadGlobalAddr:
    case mir::Opcode::LoadStackAddr:
    case mir::Opcode::Add:
    case mir::Opcode::AddW:
    case mir::Opcode::AddI:
    case mir::Opcode::AddIW:
    case mir::Opcode::SubW:
    case mir::Opcode::Mul:
    case mir::Opcode::MulW:
    case mir::Opcode::And:
    case mir::Opcode::AndI:
    case mir::Opcode::SllI:
    case mir::Opcode::SllIW:
    case mir::Opcode::SraI:
    case mir::Opcode::SraIW:
    case mir::Opcode::SrliW:
    case mir::Opcode::Xor:
    case mir::Opcode::XorI:
    case mir::Opcode::Slt:
    case mir::Opcode::SeqZ:
    case mir::Opcode::Snez:
        return true;
    default:
        return false;
    }
}

bool cse_opcode(mir::Opcode opcode, CSEScope scope) {
    if (is_move(opcode)) {
        return false;
    }
    if (scope == CSEScope::Global) {
        return global_cse_opcode(opcode);
    }
    return is_pure_def(opcode);
}

bool commutative_opcode(mir::Opcode opcode) {
    switch (opcode) {
    case mir::Opcode::Add:
    case mir::Opcode::AddW:
    case mir::Opcode::Mul:
    case mir::Opcode::MulW:
    case mir::Opcode::And:
    case mir::Opcode::Xor:
        return true;
    default:
        return false;
    }
}

std::optional<std::string> cse_key(const mir::MachineInstr &instr,
                                   const std::map<VRegId, RegCounts> *counts, CSEScope scope,
                                   const RegisterVersions *versions = nullptr) {
    if (!cse_opcode(instr.opcode(), scope)) {
        return std::nullopt;
    }
    const auto defs = instr.defs();
    if (defs.size() != 1 || !defs[0].is_virtual()) {
        return std::nullopt;
    }
    if (counts != nullptr && def_count(*counts, defs[0]) != 1) {
        return std::nullopt;
    }

    std::vector<std::string> pieces;
    const auto &ops = instr.operands();
    for (std::size_t i = 1; i < ops.size(); ++i) {
        if (ops[i].is_implicit() || !stable_operand_for_cse(ops[i], counts)) {
            return std::nullopt;
        }
        pieces.push_back(operand_key(ops[i], versions));
    }
    if (commutative_opcode(instr.opcode()) && pieces.size() == 2) {
        std::sort(pieces.begin(), pieces.end());
    }

    std::ostringstream oss;
    oss << static_cast<int>(instr.opcode());
    oss << "|" << static_cast<int>(defs[0].reg_class);
    oss << "|" << static_cast<int>(defs[0].value_type);
    for (const auto &piece : pieces) {
        oss << "|" << piece;
    }
    return oss.str();
}

bool memory_or_call_barrier(mir::Opcode opcode) {
    const auto &desc = mir::instruction_desc(opcode);
    return desc.has_flag(mir::MIF_Call) || desc.has_flag(mir::MIF_Barrier) ||
           desc.may_store();
}

BlockSet all_blocks(mir::MachineFunction &function) {
    BlockSet out;
    for (auto &block : function.blocks()) {
        out.insert(block.get());
    }
    return out;
}

bool contains(const BlockSet &blocks, Block *block) {
    return blocks.find(block) != blocks.end();
}

BlockSet intersect_sets(const BlockSet &lhs, const BlockSet &rhs) {
    BlockSet out;
    std::set_intersection(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                          std::inserter(out, out.begin()));
    return out;
}

std::map<Block *, BlockSet> compute_dominators(mir::MachineFunction &function) {
    std::map<Block *, BlockSet> dom;
    if (function.blocks().empty()) {
        return dom;
    }

    auto universe = all_blocks(function);
    auto *entry = function.blocks().front().get();
    for (auto &block : function.blocks()) {
        dom[block.get()] = block.get() == entry ? BlockSet{entry} : universe;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto &block_ptr : function.blocks()) {
            auto *block = block_ptr.get();
            if (block == entry) {
                continue;
            }

            BlockSet next = universe;
            if (block->predecessors().empty()) {
                next.clear();
            } else {
                bool first = true;
                for (auto *pred : block->predecessors()) {
                    if (first) {
                        next = dom[pred];
                        first = false;
                    } else {
                        next = intersect_sets(next, dom[pred]);
                    }
                }
            }
            next.insert(block);
            if (next != dom[block]) {
                dom[block] = std::move(next);
                changed = true;
            }
        }
    }

    return dom;
}

bool dominates(const std::map<Block *, BlockSet> &dom, Block *dominator, Block *block) {
    auto found = dom.find(block);
    return found != dom.end() && contains(found->second, dominator);
}

std::map<Block *, Block *> compute_idoms(mir::MachineFunction &function,
                                         const std::map<Block *, BlockSet> &dom) {
    std::map<Block *, Block *> idom;
    if (function.blocks().empty()) {
        return idom;
    }

    auto *entry = function.blocks().front().get();
    for (auto &block_ptr : function.blocks()) {
        auto *block = block_ptr.get();
        if (block == entry) {
            continue;
        }
        auto found_dom = dom.find(block);
        if (found_dom == dom.end()) {
            continue;
        }

        Block *best = nullptr;
        for (auto *candidate : found_dom->second) {
            if (candidate == block) {
                continue;
            }

            bool closest = true;
            for (auto *other : found_dom->second) {
                if (other == block || other == candidate) {
                    continue;
                }
                if (!dominates(dom, other, candidate)) {
                    closest = false;
                    break;
                }
            }
            if (closest) {
                best = candidate;
                break;
            }
        }

        if (best != nullptr) {
            idom[block] = best;
        }
    }

    return idom;
}

bool allows_cse_rewrite(Stats &stats, CSEScope scope);

bool local_cse_blocks(mir::MachineFunction &function, Stats &stats) {
    bool changed = false;
    for (auto &block_ptr : function.blocks()) {
        AvailableMap available;
        RegisterVersions versions;
        std::map<std::string, std::uint64_t> available_result_versions;
        for (auto &instr : block_ptr->instructions()) {
            if (memory_or_call_barrier(instr.opcode())) {
                available.clear();
                available_result_versions.clear();
            }

            auto key = cse_key(instr, nullptr, CSEScope::Local, &versions);
            const auto defs = instr.defs();
            bool replaced = false;
            if (key && defs.size() == 1) {
                auto found = available.find(*key);
                if (found != available.end()) {
                    auto version_found = available_result_versions.find(*key);
                    const bool result_still_available =
                        version_found != available_result_versions.end() &&
                        version_found->second == register_version(versions, found->second);
                    if (!result_still_available) {
                        available.erase(found);
                        available_result_versions.erase(*key);
                    } else if (allows_cse_rewrite(stats, CSEScope::Local)) {
                        instr = make_move_like(defs[0], found->second);
                        ++stats.cse;
                        changed = true;
                        replaced = true;
                    }
                }
            }

            bool physical_def = false;
            for (const auto &def : defs) {
                if (def.is_physical() && !is_zero_reg(def)) {
                    physical_def = true;
                } else if (def.is_virtual()) {
                    ++versions[def.id];
                }
            }
            if (physical_def) {
                available.clear();
                available_result_versions.clear();
                continue;
            }

            // Record a newly computed expression only after advancing its
            // destination version.  A CSE replacement keeps the older leader;
            // if that leader is also redefined here, the lazy version check will
            // invalidate it before the next lookup.
            if (key && defs.size() == 1 && !replaced) {
                available[*key] = defs[0];
                available_result_versions[*key] = register_version(versions, defs[0]);
            }
        }
    }
    return changed;
}

bool function_contains_call(const mir::MachineFunction &function) {
    for (const auto &block_ptr : function.blocks()) {
        for (const auto &instr : block_ptr->instructions()) {
            if (instr.opcode() == mir::Opcode::Call) {
                return true;
            }
        }
    }
    return false;
}

bool global_cse_dominator_tree(mir::MachineFunction &function, Stats &stats) {
    if (function.blocks().empty() || function_contains_call(function)) {
        return false;
    }

    const auto counts = count_vregs(function);
    const auto dom = compute_dominators(function);
    const auto idom = compute_idoms(function, dom);

    std::map<Block *, std::vector<Block *>> children;
    for (auto &block_ptr : function.blocks()) {
        children[block_ptr.get()];
    }
    for (const auto &[block, parent] : idom) {
        children[parent].push_back(block);
    }

    bool changed = false;
    std::set<Block *> visited;
    std::function<void(Block *, AvailableMap)> visit = [&](Block *block, AvailableMap available) {
        if (block == nullptr || !visited.insert(block).second) {
            return;
        }

        for (auto &instr : block->instructions()) {
            auto key = cse_key(instr, &counts, CSEScope::Global);
            const auto defs = instr.defs();
            if (!key || defs.size() != 1) {
                continue;
            }

            auto found = available.find(*key);
            if (found != available.end() && found->second != defs[0]) {
                if (!allows_cse_rewrite(stats, CSEScope::Global)) {
                    continue;
                }
                instr = make_move_like(defs[0], found->second);
                ++stats.cse;
                changed = true;
                continue;
            }
            available[*key] = defs[0];
        }

        for (auto *child : children[block]) {
            visit(child, available);
        }
    };

    visit(function.blocks().front().get(), {});
    for (auto &block_ptr : function.blocks()) {
        if (!contains(visited, block_ptr.get())) {
            visit(block_ptr.get(), {});
        }
    }
    return changed;
}

bool allows_cse_rewrite(Stats &stats, CSEScope scope) {
    pass::mir_cost_model::MIRTransformCostEstimate estimate;
    estimate.kind =
        scope == CSEScope::Global ? pass::cost_model::TransformKind::GlobalCSE
                                  : pass::cost_model::TransformKind::LocalCSE;
    estimate.stage = pass::cost_model::CostIRStage::PreRAMIR;
    estimate.pass_name = "MIRLocalCSEPass";
    estimate.candidate_id =
        std::string(scope == CSEScope::Global ? "global-cse." : "local-cse.") +
        std::to_string(stats.cse + 1);
    estimate.scope = scope == CSEScope::Global ? "dominator_tree" : "basic_block";
    estimate.proof_summary = "single-def pure expression availability";
    estimate.confidence = scope == CSEScope::Global ? 0.66 : 0.72;
    estimate.before_cycles = 3;
    estimate.after_cycles = 1;
    estimate.before_instrs = 1;
    estimate.after_instrs = 1;
    estimate.after_moves = 1;
    if (scope == CSEScope::Local) {
        estimate.bypass_profitability = true;
        estimate.bypass_reason = "AlwaysOnLocalCleanup";
    }
    return pass::mir_cost_model::allows_transform(stats.cost_model_report,
                                                  stats.cost_model_policy,
                                                  stats.cost_model_filter, estimate);
}

} // namespace

bool local_cse(mir::MachineFunction &function, bool post_ra, Stats &stats) {
    (void)post_ra;

    bool changed = false;
    changed |= local_cse_blocks(function, stats);
    changed |= global_cse_dominator_tree(function, stats);
    return changed;
}

} // namespace pass::mir_peephole

namespace pass {

std::string_view MIRLocalCSEPass::name() const {
    return "MIRLocalCSEPass";
}

PassKind MIRLocalCSEPass::kind() const {
    return PassKind::Transform;
}

PassResult MIRLocalCSEPass::run(PassContext &context) {
    return mir_peephole::run_transform(context, name(), false, mir_peephole::local_cse);
}

} // namespace pass
