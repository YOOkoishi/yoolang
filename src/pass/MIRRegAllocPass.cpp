#include "../../include/pass/MIRRegAllocPass.h"

#include "../../include/mir/MIR.h"
#include "../../include/mir/MIRVerifier.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pass {
namespace {

using VRegId = std::uint32_t;
using VRegSet = std::set<VRegId>;

struct BlockLiveInfo {
    VRegSet use;
    VRegSet def;
    VRegSet in;
    VRegSet out;
};

struct AllocationAttempt {
    std::map<VRegId, mir::Register> colors;
    std::set<VRegId> spills;
};

bool is_virtual_of_class(const mir::Register &reg, mir::RegisterClass reg_class) {
    return reg.is_virtual() && reg.reg_class == reg_class;
}

void set_union_into(VRegSet &dst, const VRegSet &src) {
    dst.insert(src.begin(), src.end());
}

VRegSet set_difference(const VRegSet &lhs, const VRegSet &rhs) {
    VRegSet out;
    for (auto value : lhs) {
        if (rhs.find(value) == rhs.end()) {
            out.insert(value);
        }
    }
    return out;
}

std::vector<mir::Register> set_to_regs(const mir::MachineFunction &function, const VRegSet &set) {
    std::vector<mir::Register> out;
    for (auto id : set) {
        if (auto *reg = function.regs().virtual_register(id)) {
            out.push_back(*reg);
        }
    }
    return out;
}

mir::TypeInfo type_info_for(mir::ValueType type) {
    mir::TypeInfo out;
    out.value_type = type;
    out.ir = mir::value_type_name(type);
    switch (type) {
    case mir::ValueType::F32:
    case mir::ValueType::I1:
    case mir::ValueType::I32:
        out.size = 4;
        out.align = 4;
        break;
    case mir::ValueType::Ptr:
        out.size = 8;
        out.align = 8;
        break;
    case mir::ValueType::Void:
    case mir::ValueType::Aggregate:
        out.size = 0;
        out.align = 1;
        break;
    }
    return out;
}

std::vector<mir::Register> caller_saved(mir::RegisterClass reg_class) {
    std::vector<mir::Register> out;
    if (reg_class == mir::RegisterClass::GPR) {
        for (const std::string &name :
             {"t0", "t1", "t2", "t3", "t4", "t5", "a0", "a1", "a2", "a3",
              "a4", "a5", "a6", "a7"}) {
            out.push_back(mir::Register::physical(name, reg_class));
        }
    } else {
        for (const std::string &name :
             {"ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7", "ft8",
              "ft9", "ft10", "ft11", "fa0", "fa1", "fa2", "fa3", "fa4", "fa5",
              "fa6", "fa7"}) {
            out.push_back(mir::Register::physical(name, reg_class));
        }
    }
    return out;
}

std::vector<mir::Register> allocatable(mir::RegisterClass reg_class) {
    auto out = caller_saved(reg_class);
    if (reg_class == mir::RegisterClass::GPR) {
        for (const std::string &name :
             {"s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10",
              "s11"}) {
            out.push_back(mir::Register::physical(name, reg_class));
        }
    }
    return out;
}

bool same_phys(const mir::Register &lhs, const mir::Register &rhs) {
    return lhs.is_physical() && rhs.is_physical() && lhs.name == rhs.name &&
           lhs.reg_class == rhs.reg_class;
}

bool phys_in_set(const std::set<std::string> &set, const mir::Register &reg) {
    return reg.is_physical() && set.find(reg.name) != set.end();
}

std::vector<mir::Register> physical_defs_for_special_instr(const mir::MachineInstr &instr,
                                                           mir::RegisterClass reg_class) {
    std::vector<mir::Register> out;
    if (instr.opcode() == mir::Opcode::MemZero && reg_class == mir::RegisterClass::GPR) {
        out.push_back(mir::Register::physical("t4", reg_class));
        out.push_back(mir::Register::physical("t5", reg_class));
    }
    return out;
}

class RegAllocator {
  public:
    void run(mir::MachineFunction &function) {
        function.regs().clear_allocations();
        constexpr int kMaxRewriteIterations = 12;
        for (int iteration = 0; iteration < kMaxRewriteIterations; ++iteration) {
            function.rebuild_cfg();
            auto live = compute_liveness(function);
            std::map<VRegId, mir::Register> colors;
            std::set<VRegId> spills;

            for (auto reg_class : {mir::RegisterClass::GPR, mir::RegisterClass::FPR32}) {
                auto attempt = allocate_class(function, live, reg_class);
                colors.insert(attempt.colors.begin(), attempt.colors.end());
                spills.insert(attempt.spills.begin(), attempt.spills.end());
            }

            if (spills.empty()) {
                apply_colors(function, colors);
                function.layout_frame();
                verify_no_virtual_regs(function);
                return;
            }
            rewrite_spills(function, spills);
        }
        throw std::runtime_error("register allocation did not converge after spill rewriting");
    }

  private:
    std::map<mir::MachineBasicBlock *, BlockLiveInfo>
    compute_liveness(mir::MachineFunction &function) {
        std::map<mir::MachineBasicBlock *, BlockLiveInfo> info;
        for (auto &block_ptr : function.blocks()) {
            auto *block = block_ptr.get();
            auto &entry = info[block];
            for (const auto &instr : block->instructions()) {
                for (const auto &use : instr.uses()) {
                    if (!use.is_virtual()) {
                        continue;
                    }
                    if (entry.def.find(use.id) == entry.def.end()) {
                        entry.use.insert(use.id);
                    }
                }
                for (const auto &def : instr.defs()) {
                    if (def.is_virtual()) {
                        entry.def.insert(def.id);
                    }
                }
            }
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (auto it = function.blocks().rbegin(); it != function.blocks().rend(); ++it) {
                auto *block = it->get();
                auto &entry = info[block];
                VRegSet out;
                for (auto *succ : block->successors()) {
                    set_union_into(out, info[succ].in);
                }
                VRegSet in = entry.use;
                auto out_minus_def = set_difference(out, entry.def);
                set_union_into(in, out_minus_def);
                if (in != entry.in || out != entry.out) {
                    entry.in = std::move(in);
                    entry.out = std::move(out);
                    changed = true;
                }
            }
        }

        for (auto &block_ptr : function.blocks()) {
            auto *block = block_ptr.get();
            block->set_live_in(set_to_regs(function, info[block].in));
            block->set_live_out(set_to_regs(function, info[block].out));
        }
        return info;
    }

    AllocationAttempt allocate_class(
        mir::MachineFunction &function,
        const std::map<mir::MachineBasicBlock *, BlockLiveInfo> &live_info,
        mir::RegisterClass reg_class) {
        std::set<VRegId> nodes;
        std::map<VRegId, std::set<VRegId>> graph;
        std::map<VRegId, std::set<std::string>> forbidden;

        for (const auto &reg : function.regs().virtual_registers()) {
            if (reg.reg_class == reg_class) {
                nodes.insert(reg.id);
                graph[reg.id];
            }
        }

        auto add_edge = [&](VRegId lhs, VRegId rhs) {
            if (lhs == rhs) {
                return;
            }
            graph[lhs].insert(rhs);
            graph[rhs].insert(lhs);
        };
        auto forbid = [&](VRegId id, const mir::Register &phys) {
            if (phys.reg_class == reg_class) {
                forbidden[id].insert(phys.name);
            }
        };
        auto forbid_live = [&](const VRegSet &live, const mir::Register &phys) {
            if (phys.reg_class != reg_class) {
                return;
            }
            for (auto id : live) {
                forbid(id, phys);
            }
        };

        for (const auto &block_ptr : function.blocks()) {
            auto *block = block_ptr.get();
            VRegSet live = live_info.at(block).out;
            for (auto instr_it = block->instructions().rbegin();
                 instr_it != block->instructions().rend(); ++instr_it) {
                const auto &instr = *instr_it;

                if (instr.opcode() == mir::Opcode::Call) {
                    for (const auto &phys : caller_saved(reg_class)) {
                        forbid_live(live, phys);
                    }
                }
                for (const auto &phys : physical_defs_for_special_instr(instr, reg_class)) {
                    forbid_live(live, phys);
                }

                for (const auto &def : instr.defs()) {
                    if (def.is_physical()) {
                        forbid_live(live, def);
                    }
                }

                for (const auto &def : instr.defs()) {
                    if (!is_virtual_of_class(def, reg_class)) {
                        continue;
                    }
                    for (const auto &operand : instr.operands()) {
                        if (operand.is_implicit() && operand.is_reg() &&
                            operand.reg_value().is_physical() && operand.is_use()) {
                            forbid(def.id, operand.reg_value());
                        }
                    }
                    for (auto live_id : live) {
                        add_edge(def.id, live_id);
                    }
                }

                for (const auto &def : instr.defs()) {
                    if (is_virtual_of_class(def, reg_class)) {
                        live.erase(def.id);
                    }
                }
                for (const auto &use : instr.uses()) {
                    if (is_virtual_of_class(use, reg_class)) {
                        live.insert(use.id);
                    }
                }
            }
        }

        return color_graph(nodes, graph, forbidden, reg_class);
    }

    AllocationAttempt color_graph(const std::set<VRegId> &nodes,
                                  const std::map<VRegId, std::set<VRegId>> &graph,
                                  const std::map<VRegId, std::set<std::string>> &forbidden,
                                  mir::RegisterClass reg_class) {
        AllocationAttempt out;
        auto colors = allocatable(reg_class);
        std::set<VRegId> remaining = nodes;
        std::vector<VRegId> stack;
        const auto k = colors.size();

        while (!remaining.empty()) {
            auto picked = remaining.end();
            for (auto it = remaining.begin(); it != remaining.end(); ++it) {
                std::size_t degree = 0;
                for (auto neighbor : graph.at(*it)) {
                    if (remaining.find(neighbor) != remaining.end()) {
                        ++degree;
                    }
                }
                if (degree < k) {
                    picked = it;
                    break;
                }
            }
            if (picked == remaining.end()) {
                picked = std::max_element(remaining.begin(), remaining.end(), [&](auto lhs, auto rhs) {
                    return graph.at(lhs).size() < graph.at(rhs).size();
                });
            }
            stack.push_back(*picked);
            remaining.erase(picked);
        }

        while (!stack.empty()) {
            auto id = stack.back();
            stack.pop_back();
            std::vector<mir::Register> available;
            for (const auto &color : colors) {
                auto found_forbidden = forbidden.find(id);
                if (found_forbidden != forbidden.end() && phys_in_set(found_forbidden->second, color)) {
                    continue;
                }
                bool used_by_neighbor = false;
                for (auto neighbor : graph.at(id)) {
                    auto found_color = out.colors.find(neighbor);
                    if (found_color != out.colors.end() && same_phys(found_color->second, color)) {
                        used_by_neighbor = true;
                        break;
                    }
                }
                if (!used_by_neighbor) {
                    available.push_back(color);
                }
            }

            if (available.empty()) {
                out.spills.insert(id);
            } else {
                out.colors[id] = available.front();
            }
        }

        return out;
    }

    void rewrite_spills(mir::MachineFunction &function, const std::set<VRegId> &spills) {
        std::map<VRegId, int> slots;
        for (auto id : spills) {
            const auto *reg = function.regs().virtual_register(id);
            if (reg == nullptr) {
                continue;
            }
            slots[id] = function.add_stack_slot("spill.v" + std::to_string(id),
                                                type_info_for(reg->value_type),
                                                mir::StackSlotKind::Spill);
        }

        for (auto &block_ptr : function.blocks()) {
            std::vector<mir::MachineInstr> rewritten;
            for (auto instr : block_ptr->instructions()) {
                std::vector<mir::MachineInstr> before;
                std::vector<mir::MachineInstr> after;
                for (auto &operand : instr.operands()) {
                    if (!operand.is_reg() || !operand.reg_value().is_virtual()) {
                        continue;
                    }
                    auto id = operand.reg_value().id;
                    auto found_slot = slots.find(id);
                    if (found_slot == slots.end()) {
                        continue;
                    }
                    const auto *old_reg = function.regs().virtual_register(id);
                    if (old_reg == nullptr) {
                        continue;
                    }
                    auto temp = function.regs().create_virtual(old_reg->reg_class,
                                                               old_reg->value_type);
                    if (operand.is_use()) {
                        before.emplace_back(
                            mir::Opcode::LoadSlot,
                            std::vector<mir::MachineOperand>{
                                mir::MachineOperand::reg_def(temp),
                                mir::MachineOperand::slot(found_slot->second),
                                mir::MachineOperand::type(old_reg->value_type)});
                    }
                    operand.set_reg(temp);
                    if (operand.is_def()) {
                        after.emplace_back(
                            mir::Opcode::StoreSlot,
                            std::vector<mir::MachineOperand>{
                                mir::MachineOperand::slot(found_slot->second),
                                mir::MachineOperand::reg_use(temp),
                                mir::MachineOperand::type(old_reg->value_type)});
                    }
                }
                rewritten.insert(rewritten.end(), std::make_move_iterator(before.begin()),
                                 std::make_move_iterator(before.end()));
                rewritten.push_back(std::move(instr));
                rewritten.insert(rewritten.end(), std::make_move_iterator(after.begin()),
                                 std::make_move_iterator(after.end()));
            }
            block_ptr->instructions() = std::move(rewritten);
        }
    }

    void apply_colors(mir::MachineFunction &function,
                      const std::map<VRegId, mir::Register> &colors) {
        for (const auto &[id, phys] : colors) {
            const auto *vreg = function.regs().virtual_register(id);
            if (vreg != nullptr) {
                function.regs().set_allocation(*vreg, phys);
                function.note_used_callee_saved(phys);
            }
        }

        for (auto &block : function.blocks()) {
            for (auto &instr : block->instructions()) {
                for (auto &operand : instr.operands()) {
                    if (!operand.is_reg() || !operand.reg_value().is_virtual()) {
                        continue;
                    }
                    auto found = colors.find(operand.reg_value().id);
                    if (found == colors.end()) {
                        throw std::runtime_error("uncolored virtual register in MIR RA");
                    }
                    operand.set_reg(found->second);
                }
            }
        }
    }

    void verify_no_virtual_regs(const mir::MachineFunction &function) const {
        for (const auto &block : function.blocks()) {
            for (const auto &instr : block->instructions()) {
                for (const auto &operand : instr.operands()) {
                    if (operand.is_reg() && operand.reg_value().is_virtual()) {
                        std::ostringstream oss;
                        oss << "virtual register survived RA in @" << function.name();
                        throw std::runtime_error(oss.str());
                    }
                }
            }
        }
    }
};

} // namespace

std::string_view MIRRegAllocPass::name() const {
    return "MIRRegAllocPass";
}

PassKind MIRRegAllocPass::kind() const {
    return PassKind::Transform;
}

PassResult MIRRegAllocPass::run(PassContext &context) {
    auto *module = context.machine_module();
    if (module == nullptr) {
        return PassResult::fail("MIRRegAllocPass requires MIR module in pass context");
    }

    try {
        RegAllocator allocator;
        for (auto &function : module->functions()) {
            if (!function->is_external()) {
                allocator.run(*function);
            }
        }
        auto verify = mir::verify_module(*module, mir::MIRVerificationStage::PostRA);
        if (!verify.ok) {
            return PassResult::fail(verify.message);
        }
        return PassResult::ok(true);
    } catch (const std::exception &ex) {
        return PassResult::fail(ex.what());
    }
}

} // namespace pass
