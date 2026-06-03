#include "pass/mir/MIRPeepholeDeadDefEliminationPass.h"

#include "pass/mir/MIRPeepholeCommon.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pass::mir_peephole {
namespace {

struct PhysRegKey {
    mir::RegisterClass reg_class = mir::RegisterClass::GPR;
    std::string name;

    bool operator==(const PhysRegKey &other) const {
        return reg_class == other.reg_class && name == other.name;
    }

    bool operator<(const PhysRegKey &other) const {
        if (reg_class != other.reg_class) {
            return reg_class < other.reg_class;
        }
        return name < other.name;
    }
};

using PhysRegSet = std::set<PhysRegKey>;

struct PhysBlockLiveInfo {
    PhysRegSet use;
    PhysRegSet def;
    PhysRegSet in;
    PhysRegSet out;
};

PhysRegKey phys_key(std::string name, mir::RegisterClass reg_class) {
    PhysRegKey key;
    key.reg_class = reg_class;
    key.name = std::move(name);
    return key;
}

PhysRegKey phys_key(const mir::Register &reg) {
    return phys_key(reg.name, reg.reg_class);
}

bool is_zero_phys_reg(const mir::Register &reg) {
    return reg.is_physical() && reg.reg_class == mir::RegisterClass::GPR && reg.name == "zero";
}

void insert_use(PhysRegSet &set, const mir::Register &reg) {
    if (reg.is_physical() && !is_zero_phys_reg(reg)) {
        set.insert(phys_key(reg));
    }
}

void insert_def(PhysRegSet &set, const mir::Register &reg) {
    if (reg.is_physical()) {
        set.insert(phys_key(reg));
    }
}

void insert_named_regs(PhysRegSet &set, mir::RegisterClass reg_class,
                       const std::vector<const char *> &names) {
    for (const char *name : names) {
        set.insert(phys_key(name, reg_class));
    }
}

void set_union_into(PhysRegSet &dst, const PhysRegSet &src) {
    dst.insert(src.begin(), src.end());
}

PhysRegSet set_difference(const PhysRegSet &lhs, const PhysRegSet &rhs) {
    PhysRegSet out;
    for (const auto &value : lhs) {
        if (rhs.find(value) == rhs.end()) {
            out.insert(value);
        }
    }
    return out;
}

bool has_live_def(const PhysRegSet &defs, const PhysRegSet &live) {
    for (const auto &def : defs) {
        if (live.find(def) != live.end()) {
            return true;
        }
    }
    return false;
}

PhysRegSet explicit_physical_defs(const mir::MachineInstr &instr) {
    PhysRegSet defs;
    for (const auto &operand : instr.operands()) {
        if (operand.is_reg() && operand.is_def()) {
            insert_def(defs, operand.reg_value());
        }
    }
    return defs;
}

PhysRegSet physical_uses(const mir::MachineInstr &instr) {
    PhysRegSet uses;
    for (const auto &operand : instr.operands()) {
        if (operand.is_reg() && operand.is_use()) {
            insert_use(uses, operand.reg_value());
        }
    }

    if (instr.opcode() == mir::Opcode::Call) {
        insert_named_regs(uses, mir::RegisterClass::GPR,
                          {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"});
        insert_named_regs(uses, mir::RegisterClass::FPR32,
                          {"fa0", "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7"});
    }
    return uses;
}

PhysRegSet physical_defs(const mir::MachineInstr &instr) {
    PhysRegSet defs = explicit_physical_defs(instr);

    if (instr.opcode() == mir::Opcode::Call) {
        insert_named_regs(defs, mir::RegisterClass::GPR,
                          {"ra", "t0", "t1", "t2", "t3", "t4", "t5", "t6",
                           "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"});
        insert_named_regs(defs, mir::RegisterClass::FPR32,
                          {"ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6",
                           "ft7", "ft8", "ft9", "ft10", "ft11", "fa0", "fa1",
                           "fa2", "fa3", "fa4", "fa5", "fa6", "fa7"});
    }

    return defs;
}

PhysRegSet return_live_out(const mir::MachineFunction &function) {
    switch (function.return_type().value_type) {
    case mir::ValueType::I1:
    case mir::ValueType::I32:
    case mir::ValueType::Ptr:
        return {phys_key("a0", mir::RegisterClass::GPR)};
    case mir::ValueType::F32:
        return {phys_key("fa0", mir::RegisterClass::FPR32)};
    case mir::ValueType::Void:
    case mir::ValueType::Aggregate:
        return {};
    }
    return {};
}

bool jumps_to_epilogue(const mir::MachineBasicBlock &block) {
    if (block.instructions().empty()) {
        return false;
    }
    const auto &instr = block.instructions().back();
    if (instr.opcode() != mir::Opcode::Jump || instr.operands().empty()) {
        return false;
    }
    const auto &target = instr.operands()[0];
    return target.kind() == mir::OperandKind::Block && target.string_value() == "epilogue";
}

std::map<mir::MachineBasicBlock *, PhysBlockLiveInfo>
compute_physical_liveness(mir::MachineFunction &function) {
    std::map<mir::MachineBasicBlock *, PhysBlockLiveInfo> info;
    for (auto &block_ptr : function.blocks()) {
        auto *block = block_ptr.get();
        auto &entry = info[block];
        for (const auto &instr : block->instructions()) {
            const auto uses = physical_uses(instr);
            const auto defs = physical_defs(instr);
            for (const auto &use : uses) {
                if (entry.def.find(use) == entry.def.end()) {
                    entry.use.insert(use);
                }
            }
            set_union_into(entry.def, defs);
        }
    }

    const auto epilogue_live = return_live_out(function);
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = function.blocks().rbegin(); it != function.blocks().rend(); ++it) {
            auto *block = it->get();
            auto &entry = info[block];
            PhysRegSet out;
            for (auto *succ : block->successors()) {
                set_union_into(out, info[succ].in);
            }
            if (jumps_to_epilogue(*block)) {
                set_union_into(out, epilogue_live);
            }

            PhysRegSet in = entry.use;
            auto out_minus_def = set_difference(out, entry.def);
            set_union_into(in, out_minus_def);
            if (in != entry.in || out != entry.out) {
                entry.in = std::move(in);
                entry.out = std::move(out);
                changed = true;
            }
        }
    }
    return info;
}

bool remove_dead_defs_once(mir::MachineFunction &function, Stats &stats) {
    auto counts = count_vregs(function);
    bool changed = false;
    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        for (std::size_t i = 0; i < instrs.size();) {
            const auto defs = instrs[i].defs();
            if (defs.size() == 1 && defs[0].is_virtual() && is_pure_def(instrs[i].opcode()) &&
                use_count(counts, defs[0]) == 0) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(i));
                ++stats.dead;
                changed = true;
                continue;
            }
            ++i;
        }
    }
    return changed;
}

bool remove_dead_physical_defs_once(mir::MachineFunction &function, Stats &stats) {
    auto live_info = compute_physical_liveness(function);
    bool changed = false;

    for (auto &block_ptr : function.blocks()) {
        auto &instrs = block_ptr->instructions();
        PhysRegSet live = live_info[block_ptr.get()].out;

        for (std::size_t index = instrs.size(); index > 0;) {
            --index;
            const auto explicit_defs = explicit_physical_defs(instrs[index]);
            if (!explicit_defs.empty() && is_pure_def(instrs[index].opcode()) &&
                !has_live_def(explicit_defs, live)) {
                instrs.erase(instrs.begin() + static_cast<std::ptrdiff_t>(index));
                ++stats.dead;
                changed = true;
                continue;
            }

            const auto defs = physical_defs(instrs[index]);
            for (const auto &def : defs) {
                live.erase(def);
            }
            set_union_into(live, physical_uses(instrs[index]));
        }
    }

    return changed;
}

} // namespace

bool remove_dead_defs(mir::MachineFunction &function, bool post_ra, Stats &stats) {
    bool changed = false;
    for (int iteration = 0; iteration < 8; ++iteration) {
        const bool iteration_changed =
            post_ra ? remove_dead_physical_defs_once(function, stats)
                    : remove_dead_defs_once(function, stats);
        if (!iteration_changed) {
            break;
        }
        changed = true;
    }
    return changed;
}

} // namespace pass::mir_peephole

namespace pass {

std::string_view MIRPeepholeDeadDefEliminationPass::name() const {
    return "MIRPeepholeDeadDefEliminationPass";
}

PassKind MIRPeepholeDeadDefEliminationPass::kind() const {
    return PassKind::Transform;
}

PassResult MIRPeepholeDeadDefEliminationPass::run(PassContext &context) {
    return mir_peephole::run_transform(context, name(), false, mir_peephole::remove_dead_defs);
}

} // namespace pass
