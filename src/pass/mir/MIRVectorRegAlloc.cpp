#include "pass/mir/MIRVectorRegAlloc.h"

#include "mir/MachineInstrDesc.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace pass {
namespace {

using VRegId = std::uint32_t;
using VRegSet = std::set<VRegId>;

constexpr unsigned kVectorRegisterCount = 32;
constexpr unsigned kSpillScratchBegin = 24;
constexpr std::uint32_t kVariantCallerClobberedMask =
    (std::uint32_t{1} << 0U) | (((std::uint32_t{1} << 16U) - 1U) << 8U);

bool is_virtual_vector(const mir::Register &reg) {
    return reg.is_virtual() && reg.is_vector();
}

std::optional<unsigned> vector_register_index(const std::string &name) {
    if (name.size() < 2 || name[0] != 'v' ||
        !std::all_of(name.begin() + 1, name.end(), [](char ch) {
            return std::isdigit(static_cast<unsigned char>(ch));
        })) {
        return std::nullopt;
    }
    try {
        const auto index = std::stoul(name.substr(1));
        if (index >= kVectorRegisterCount) {
            return std::nullopt;
        }
        return static_cast<unsigned>(index);
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

bool is_variant_callee_saved_index(unsigned index) {
    return (index >= 1U && index <= 7U) ||
           (index >= 24U && index <= 31U);
}

void collect_variant_callee_saves(mir::MachineFunction &function) {
    if (!function.is_variant_cc()) {
        return;
    }
    std::set<unsigned> used;
    for (const auto &block : function.blocks()) {
        for (const auto &instr : block->instructions()) {
            for (const auto &operand : instr.operands()) {
                if (!operand.is_reg() || !operand.reg_value().is_physical() ||
                    !operand.reg_value().is_vector()) {
                    continue;
                }
                const auto base =
                    vector_register_index(operand.reg_value().name);
                if (!base.has_value()) {
                    continue;
                }
                for (unsigned offset = 0;
                     offset < operand.reg_value().vector_group_width &&
                     *base + offset < kVectorRegisterCount;
                     ++offset) {
                    if (is_variant_callee_saved_index(*base + offset)) {
                        used.insert(*base + offset);
                    }
                }
            }
        }
    }
    for (auto index : used) {
        function.note_used_vector_callee_saved(mir::Register::physical(
            "v" + std::to_string(index), mir::RegisterClass::VR));
    }
}

bool ranges_overlap(unsigned lhs_base, unsigned lhs_width, unsigned rhs_base,
                    unsigned rhs_width) {
    return lhs_base < rhs_base + rhs_width && rhs_base < lhs_base + lhs_width;
}

std::uint32_t register_range_mask(unsigned base, unsigned width) {
    std::uint32_t mask = 0;
    for (unsigned index = base; index < base + width && index < kVectorRegisterCount; ++index) {
        mask |= std::uint32_t{1} << index;
    }
    return mask;
}

class DisjointSet final {
  public:
    void add(VRegId id) {
        parent_.emplace(id, id);
    }

    VRegId find(VRegId id) {
        auto found = parent_.find(id);
        if (found == parent_.end()) {
            throw std::runtime_error("vector RA saw an unknown virtual register");
        }
        if (found->second != id) {
            found->second = find(found->second);
        }
        return found->second;
    }

    void unite(VRegId lhs, VRegId rhs) {
        lhs = find(lhs);
        rhs = find(rhs);
        if (lhs == rhs) {
            return;
        }
        if (rhs < lhs) {
            std::swap(lhs, rhs);
        }
        parent_[rhs] = lhs;
    }

  private:
    std::map<VRegId, VRegId> parent_;
};

struct BlockLiveInfo {
    VRegSet use;
    VRegSet def;
    VRegSet in;
    VRegSet out;
};

struct InstrLiveInfo {
    VRegSet before;
    VRegSet after;
};

struct FunctionLiveInfo {
    std::map<mir::MachineBasicBlock *, BlockLiveInfo> blocks;
    std::map<mir::MachineBasicBlock *, std::vector<InstrLiveInfo>> instructions;
};

void set_union_into(VRegSet &destination, const VRegSet &source) {
    destination.insert(source.begin(), source.end());
}

VRegSet set_difference(const VRegSet &lhs, const VRegSet &rhs) {
    VRegSet result;
    for (auto id : lhs) {
        if (rhs.find(id) == rhs.end()) {
            result.insert(id);
        }
    }
    return result;
}

FunctionLiveInfo compute_liveness(mir::MachineFunction &function) {
    function.rebuild_cfg();
    FunctionLiveInfo result;
    for (auto &block_ptr : function.blocks()) {
        auto *block = block_ptr.get();
        auto &info = result.blocks[block];
        for (const auto &instr : block->instructions()) {
            for (const auto &use : instr.uses()) {
                if (!is_virtual_vector(use)) {
                    continue;
                }
                if (info.def.find(use.id) == info.def.end()) {
                    info.use.insert(use.id);
                }
            }
            for (const auto &def : instr.defs()) {
                if (is_virtual_vector(def)) {
                    info.def.insert(def.id);
                }
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto block_it = function.blocks().rbegin(); block_it != function.blocks().rend();
             ++block_it) {
            auto *block = block_it->get();
            auto &info = result.blocks[block];
            VRegSet out;
            for (auto *successor : block->successors()) {
                set_union_into(out, result.blocks[successor].in);
            }
            auto in = info.use;
            set_union_into(in, set_difference(out, info.def));
            if (in != info.in || out != info.out) {
                info.in = std::move(in);
                info.out = std::move(out);
                changed = true;
            }
        }
    }

    for (auto &block_ptr : function.blocks()) {
        auto *block = block_ptr.get();
        auto &per_instr = result.instructions[block];
        per_instr.resize(block->instructions().size());
        VRegSet live = result.blocks[block].out;
        for (std::size_t index = block->instructions().size(); index > 0; --index) {
            const auto &instr = block->instructions()[index - 1];
            auto &entry = per_instr[index - 1];
            entry.after = live;
            for (const auto &def : instr.defs()) {
                if (is_virtual_vector(def)) {
                    live.erase(def.id);
                }
            }
            for (const auto &use : instr.uses()) {
                if (is_virtual_vector(use)) {
                    live.insert(use.id);
                }
            }
            entry.before = live;
        }
    }
    return result;
}

std::vector<std::size_t> explicit_operand_indices(const mir::MachineInstr &instr) {
    std::vector<std::size_t> indices;
    for (std::size_t index = 0; index < instr.operands().size(); ++index) {
        if (!instr.operands()[index].is_implicit()) {
            indices.push_back(index);
        }
    }
    return indices;
}

struct TiedVirtualPair {
    mir::MachineBasicBlock *block = nullptr;
    std::size_t instr_index = 0;
    VRegId def = 0;
    VRegId use = 0;
};

struct VirtualPhysicalTie {
    VRegId virtual_register = 0;
    mir::Register physical;
};

void collect_ties(mir::MachineFunction &function, DisjointSet &sets,
                  std::vector<TiedVirtualPair> &virtual_pairs,
                  std::vector<VirtualPhysicalTie> &physical_ties) {
    for (auto &block_ptr : function.blocks()) {
        auto *block = block_ptr.get();
        for (std::size_t instr_index = 0; instr_index < block->instructions().size();
             ++instr_index) {
            const auto &instr = block->instructions()[instr_index];
            const auto &desc = mir::instruction_desc(instr.opcode());
            const auto explicit_indices = explicit_operand_indices(instr);
            if (desc.operand_constraints == nullptr) {
                continue;
            }
            const auto count =
                std::min(explicit_indices.size(),
                         static_cast<std::size_t>(desc.operand_constraint_count));
            for (std::size_t ordinal = 0; ordinal < count; ++ordinal) {
                const auto tied_to = desc.operand_constraints[ordinal].tied_to;
                if (tied_to < 0 || ordinal >= static_cast<std::size_t>(tied_to) ||
                    static_cast<std::size_t>(tied_to) >= explicit_indices.size()) {
                    continue;
                }
                const auto &lhs = instr.operands()[explicit_indices[ordinal]];
                const auto &rhs =
                    instr.operands()[explicit_indices[static_cast<std::size_t>(tied_to)]];
                if (!lhs.is_reg() || !rhs.is_reg() || !lhs.reg_value().is_vector() ||
                    !rhs.reg_value().is_vector()) {
                    continue;
                }
                const auto lhs_reg = lhs.reg_value();
                const auto rhs_reg = rhs.reg_value();
                if (lhs_reg.is_virtual() && rhs_reg.is_virtual()) {
                    sets.unite(lhs_reg.id, rhs_reg.id);
                    if (lhs_reg.id != rhs_reg.id) {
                        const auto def = lhs.is_def() ? lhs_reg.id : rhs_reg.id;
                        const auto use = lhs.is_use() ? lhs_reg.id : rhs_reg.id;
                        virtual_pairs.push_back({block, instr_index, def, use});
                    }
                } else if (lhs_reg.is_virtual() && rhs_reg.is_physical()) {
                    physical_ties.push_back({lhs_reg.id, rhs_reg});
                } else if (rhs_reg.is_virtual() && lhs_reg.is_physical()) {
                    physical_ties.push_back({rhs_reg.id, lhs_reg});
                }
            }
        }
    }
}

struct VectorNode {
    VRegId root = 0;
    std::vector<VRegId> members;
    mir::RegisterClass reg_class = mir::RegisterClass::VR;
    mir::MachineVectorType type =
        mir::MachineVectorType::scalable(mir::ValueType::I32, 32, mir::VectorLMUL::M1);
    unsigned width = 1;
    unsigned alignment = 1;
    double spill_cost = 1.0;
    bool forced_spill = false;
    std::optional<unsigned> precolored_base;
};

using NodeMap = std::map<VRegId, VectorNode>;
using InterferenceGraph = std::map<VRegId, std::set<VRegId>>;

MIRVectorRelegalizeRequest make_request(const mir::MachineFunction &function,
                                        const VectorNode &node,
                                        MIRVectorRelegalizeReason reason,
                                        std::string detail) {
    return {function.name(), node.members.empty() ? node.root : node.members.front(),
            node.type, reason, std::move(detail)};
}

std::string requests_message(const MIRVectorRelegalizeRequests &requests) {
    if (requests.empty()) {
        return "";
    }
    const auto &request = requests.front();
    std::ostringstream oss;
    oss << "RVV relegalization required in @" << request.function << " for %v"
        << request.virtual_register << " ("
        << mir_vector_relegalize_reason_name(request.reason) << "): " << request.detail;
    if (requests.size() > 1) {
        oss << " [and " << requests.size() - 1 << " more request(s)]";
    }
    return oss.str();
}

void add_edge(InterferenceGraph &graph, VRegId lhs, VRegId rhs) {
    if (lhs == rhs) {
        return;
    }
    graph[lhs].insert(rhs);
    graph[rhs].insert(lhs);
}

VRegSet collapse_set(const VRegSet &values, DisjointSet &sets) {
    VRegSet result;
    for (auto id : values) {
        result.insert(sets.find(id));
    }
    return result;
}

void add_clique(InterferenceGraph &graph, const VRegSet &values) {
    for (auto lhs = values.begin(); lhs != values.end(); ++lhs) {
        for (auto rhs = std::next(lhs); rhs != values.end(); ++rhs) {
            add_edge(graph, *lhs, *rhs);
        }
    }
}

struct ColorAttempt {
    std::map<VRegId, unsigned> colors;
    VRegSet spills;
};

bool candidate_is_available(VRegId root, unsigned base, const VectorNode &node,
                            const InterferenceGraph &graph,
                            const std::map<VRegId, std::uint32_t> &forbidden,
                            const std::map<VRegId, unsigned> &colors,
                            const NodeMap &nodes) {
    if (base + node.width > kVectorRegisterCount || base % node.alignment != 0) {
        return false;
    }
    const auto candidate_mask = register_range_mask(base, node.width);
    if (auto found = forbidden.find(root);
        found != forbidden.end() && (found->second & candidate_mask) != 0) {
        return false;
    }
    auto found_neighbors = graph.find(root);
    if (found_neighbors == graph.end()) {
        return true;
    }
    for (auto neighbor : found_neighbors->second) {
        auto found_color = colors.find(neighbor);
        if (found_color == colors.end()) {
            continue;
        }
        const auto &neighbor_node = nodes.at(neighbor);
        if (ranges_overlap(base, node.width, found_color->second, neighbor_node.width)) {
            return false;
        }
    }
    return true;
}

ColorAttempt color_nodes(const NodeMap &nodes, const InterferenceGraph &graph,
                         const std::map<VRegId, std::uint32_t> &forbidden,
                         bool reserve_spill_scratch) {
    std::vector<VRegId> order;
    for (const auto &[root, node] : nodes) {
        if (!node.forced_spill) {
            order.push_back(root);
        }
    }
    std::sort(order.begin(), order.end(), [&](VRegId lhs, VRegId rhs) {
        const auto &lhs_node = nodes.at(lhs);
        const auto &rhs_node = nodes.at(rhs);
        if (lhs_node.precolored_base.has_value() != rhs_node.precolored_base.has_value()) {
            return lhs_node.precolored_base.has_value();
        }
        const bool lhs_mask = lhs_node.reg_class == mir::RegisterClass::VMASK;
        const bool rhs_mask = rhs_node.reg_class == mir::RegisterClass::VMASK;
        if (lhs_mask != rhs_mask) {
            return lhs_mask;
        }
        if (lhs_node.width != rhs_node.width) {
            return lhs_node.width > rhs_node.width;
        }
        const auto lhs_degree = graph.at(lhs).size();
        const auto rhs_degree = graph.at(rhs).size();
        if (lhs_degree != rhs_degree) {
            return lhs_degree > rhs_degree;
        }
        if (lhs_node.spill_cost != rhs_node.spill_cost) {
            return lhs_node.spill_cost > rhs_node.spill_cost;
        }
        return lhs < rhs;
    });

    ColorAttempt result;
    for (const auto &[root, node] : nodes) {
        if (node.forced_spill) {
            result.spills.insert(root);
        }
    }
    for (auto root : order) {
        const auto &node = nodes.at(root);
        std::vector<unsigned> candidates;
        if (node.precolored_base.has_value()) {
            candidates.push_back(*node.precolored_base);
        } else {
            const unsigned end = reserve_spill_scratch ? kSpillScratchBegin
                                                       : kVectorRegisterCount;
            for (unsigned base = 1; base + node.width <= end; ++base) {
                if (base % node.alignment == 0) {
                    candidates.push_back(base);
                }
            }
        }

        const auto found = std::find_if(candidates.begin(), candidates.end(), [&](unsigned base) {
            return candidate_is_available(root, base, node, graph, forbidden, result.colors,
                                          nodes);
        });
        if (found == candidates.end()) {
            result.spills.insert(root);
        } else {
            result.colors[root] = *found;
        }
    }
    return result;
}

mir::Register physical_register(const VectorNode &node, unsigned base) {
    return mir::Register::physical_vector("v" + std::to_string(base), node.reg_class,
                                          node.type);
}

mir::Register vector_state(const char *name) {
    return mir::Register::physical(name, mir::RegisterClass::VSTATE);
}

mir::Register spill_address_scratch(const char *name) {
    return mir::Register::physical(name, mir::RegisterClass::GPR,
                                   mir::ValueType::Ptr);
}

mir::MachineVectorInfo whole_register_info(const mir::MachineVectorType &type,
                                           mir::RVVOperation operation) {
    mir::MachineVectorInfo info(type);
    info.operation = operation;
    info.avl = mir::MachineVectorAVL::whole_register();
    info.tail_policy = mir::VectorTailPolicy::Undisturbed;
    info.mask_policy = mir::VectorMaskPolicy::Undisturbed;
    return info;
}

mir::MachineInstr make_reload(const VectorNode &node, int slot, unsigned base) {
    return mir::MachineInstr(
        mir::Opcode::RVVWholeRegReload,
        {mir::MachineOperand::reg_def(physical_register(node, base)),
         mir::MachineOperand::slot(slot),
         mir::MachineOperand::implicit_reg_use(vector_state("vlenb")),
         mir::MachineOperand::implicit_reg_def(spill_address_scratch("t5")),
         mir::MachineOperand::implicit_reg_def(spill_address_scratch("t6"))},
        whole_register_info(node.type, mir::RVVOperation::Reload));
}

mir::MachineInstr make_spill(const VectorNode &node, int slot, unsigned base) {
    return mir::MachineInstr(
        mir::Opcode::RVVWholeRegSpill,
        {mir::MachineOperand::slot(slot),
         mir::MachineOperand::reg_use(physical_register(node, base)),
         mir::MachineOperand::implicit_reg_use(vector_state("vlenb")),
         mir::MachineOperand::implicit_reg_def(spill_address_scratch("t5")),
         mir::MachineOperand::implicit_reg_def(spill_address_scratch("t6"))},
        whole_register_info(node.type, mir::RVVOperation::Spill));
}

struct ScratchUse {
    unsigned base = 0;
    bool use = false;
    bool def = false;
};

using InstrScratchPlan = std::map<VRegId, ScratchUse>;
using BlockScratchPlan = std::vector<InstrScratchPlan>;
using InstrEvictionPlan = std::map<VRegId, unsigned>;
using BlockEvictionPlan = std::vector<InstrEvictionPlan>;

std::optional<unsigned> choose_scratch_base(const VectorNode &node, std::uint32_t occupied) {
    for (unsigned base = kSpillScratchBegin;
         base + node.width <= kVectorRegisterCount; ++base) {
        if (base % node.alignment != 0) {
            continue;
        }
        if ((occupied & register_range_mask(base, node.width)) == 0) {
            return base;
        }
    }
    return std::nullopt;
}

struct ScavengedScratchBase final {
    unsigned base = 0;
    std::vector<VRegId> evicted_roots;
};

std::optional<ScavengedScratchBase> choose_scavenged_scratch_base(
    const VectorNode &node, std::uint32_t occupied,
    const std::map<VRegId, unsigned> &evictable_colors,
    const NodeMap &nodes) {
    for (unsigned base = 1; base + node.width <= kVectorRegisterCount; ++base) {
        if (base % node.alignment != 0 ||
            (occupied & register_range_mask(base, node.width)) != 0) {
            continue;
        }
        ScavengedScratchBase candidate;
        candidate.base = base;
        for (const auto &[root, color] : evictable_colors) {
            const auto &resident = nodes.at(root);
            if (ranges_overlap(base, node.width, color, resident.width)) {
                candidate.evicted_roots.push_back(root);
            }
        }
        return candidate;
    }
    return std::nullopt;
}

} // namespace

MIRVectorRelegalizeRequest::MIRVectorRelegalizeRequest(
    std::string function_value, std::uint32_t virtual_register_value,
    mir::MachineVectorType vector_type_value, MIRVectorRelegalizeReason reason_value,
    std::string detail_value)
    : function(std::move(function_value)), virtual_register(virtual_register_value),
      vector_type(std::move(vector_type_value)), reason(reason_value),
      detail(std::move(detail_value)) {
}

const char *mir_vector_relegalize_reason_name(MIRVectorRelegalizeReason reason) {
    switch (reason) {
    case MIRVectorRelegalizeReason::TiedLiveRangeConflict:
        return "tied-live-range-conflict";
    case MIRVectorRelegalizeReason::SpillScratchPressure:
        return "spill-scratch-pressure";
    }
    return "unknown";
}

MIRVectorRegAllocResult MIRVectorRegAllocator::run(mir::MachineFunction &function) {
    MIRVectorRegAllocResult result;
    try {
        DisjointSet sets;
        bool has_virtual_vectors = false;
        for (const auto &reg : function.regs().virtual_registers()) {
            if (reg.is_vector()) {
                sets.add(reg.id);
                has_virtual_vectors = true;
            }
        }
        if (!has_virtual_vectors) {
            return result;
        }

        std::vector<TiedVirtualPair> virtual_ties;
        std::vector<VirtualPhysicalTie> physical_ties;
        collect_ties(function, sets, virtual_ties, physical_ties);
        const auto live = compute_liveness(function);

        NodeMap nodes;
        for (const auto &reg : function.regs().virtual_registers()) {
            if (!reg.is_vector()) {
                continue;
            }
            if (!reg.vector_type.has_value()) {
                throw std::runtime_error("vector virtual register lacks a machine vector type");
            }
            const auto root = sets.find(reg.id);
            auto found = nodes.find(root);
            if (found == nodes.end()) {
                VectorNode node;
                node.root = root;
                node.reg_class = reg.reg_class;
                node.type = *reg.vector_type;
                node.width = reg.vector_group_width;
                node.alignment = reg.vector_type->register_group_alignment();
                found = nodes.emplace(root, std::move(node)).first;
            } else if (found->second.reg_class != reg.reg_class ||
                       found->second.type != *reg.vector_type ||
                       found->second.width != reg.vector_group_width) {
                throw std::runtime_error(
                    "descriptor-tied vector registers have incompatible register views");
            }
            found->second.members.push_back(reg.id);
        }

        for (const auto &tie : physical_ties) {
            const auto root = sets.find(tie.virtual_register);
            const auto base = vector_register_index(tie.physical.name);
            if (!base.has_value()) {
                throw std::runtime_error("vector tie names an invalid physical register");
            }
            auto &node = nodes.at(root);
            if (node.precolored_base.has_value() && *node.precolored_base != *base) {
                throw std::runtime_error("vector tie requires incompatible physical groups");
            }
            node.precolored_base = *base;
        }

        for (const auto &block_ptr : function.blocks()) {
            for (const auto &instr : block_ptr->instructions()) {
                for (const auto &operand : instr.operands()) {
                    if (operand.is_reg() && is_virtual_vector(operand.reg_value())) {
                        auto &node = nodes.at(sets.find(operand.reg_value().id));
                        node.spill_cost += operand.is_use() ? 2.0 : 1.0;
                    }
                }
            }
        }

        for (const auto &tie : virtual_ties) {
            const auto &after = live.instructions.at(tie.block)[tie.instr_index].after;
            if (after.find(tie.use) == after.end()) {
                continue;
            }
            const auto &node = nodes.at(sets.find(tie.use));
            result.relegalize_requests.push_back(make_request(
                function, node, MIRVectorRelegalizeReason::TiedLiveRangeConflict,
                "a destructive passthrough source remains live after its tied definition"));
        }
        if (!result.relegalize_requests.empty()) {
            result.success = false;
            result.message = requests_message(result.relegalize_requests);
            return result;
        }

        InterferenceGraph graph;
        std::map<VRegId, std::uint32_t> forbidden;
        for (const auto &[root, node] : nodes) {
            (void)node;
            graph[root];
        }

        for (const auto &block_ptr : function.blocks()) {
            auto *block = block_ptr.get();
            const auto &per_instr = live.instructions.at(block);
            add_clique(graph, collapse_set(live.blocks.at(block).in, sets));
            add_clique(graph, collapse_set(live.blocks.at(block).out, sets));
            for (std::size_t instr_index = 0; instr_index < block->instructions().size();
                 ++instr_index) {
                const auto &instr = block->instructions()[instr_index];
                const auto before = collapse_set(per_instr[instr_index].before, sets);
                const auto after = collapse_set(per_instr[instr_index].after, sets);
                add_clique(graph, before);
                add_clique(graph, after);

                VRegSet current_defs;
                VRegSet current_uses;
                for (const auto &def : instr.defs()) {
                    if (is_virtual_vector(def)) {
                        current_defs.insert(sets.find(def.id));
                    }
                }
                for (const auto &use : instr.uses()) {
                    if (is_virtual_vector(use)) {
                        current_uses.insert(sets.find(use.id));
                    }
                }
                for (auto def : current_defs) {
                    for (auto use : current_uses) {
                        add_edge(graph, def, use);
                    }
                    for (auto live_through : after) {
                        add_edge(graph, def, live_through);
                    }
                }
                add_clique(graph, current_defs);

                if (mir::machine_instr_may_call(instr)) {
                    VRegSet call_defs;
                    for (const auto &def : instr.defs()) {
                        if (is_virtual_vector(def)) {
                            call_defs.insert(def.id);
                        }
                    }
                    for (auto id : per_instr[instr_index].after) {
                        if (call_defs.find(id) != call_defs.end()) {
                            continue;
                        }
                        auto &node = nodes.at(sets.find(id));
                        if (function.is_variant_cc() &&
                            instr.is_variant_cc_call()) {
                            forbidden[node.root] |=
                                kVariantCallerClobberedMask;
                        } else {
                            node.forced_spill = true;
                        }
                    }
                }

                VRegSet affected = before;
                set_union_into(affected, after);
                set_union_into(affected, current_defs);
                set_union_into(affected, current_uses);
                for (const auto &operand : instr.operands()) {
                    if (!operand.is_reg() || !operand.reg_value().is_physical() ||
                        !operand.reg_value().is_vector()) {
                        continue;
                    }
                    const auto base = vector_register_index(operand.reg_value().name);
                    if (!base.has_value()) {
                        throw std::runtime_error("invalid physical vector register in pre-RA MIR");
                    }
                    const auto mask = register_range_mask(*base,
                                                          operand.reg_value().vector_group_width);
                    for (auto root : affected) {
                        forbidden[root] |= mask;
                    }
                }
            }
        }

        bool has_forced_spills = std::any_of(nodes.begin(), nodes.end(), [](const auto &entry) {
            return entry.second.forced_spill;
        });
        auto coloring = color_nodes(nodes, graph, forbidden, has_forced_spills);
        if (!has_forced_spills && !coloring.spills.empty()) {
            coloring = color_nodes(nodes, graph, forbidden, true);
        }

        std::map<mir::MachineBasicBlock *, BlockScratchPlan> scratch_plans;
        std::map<mir::MachineBasicBlock *, BlockEvictionPlan> eviction_plans;
        for (const auto &block_ptr : function.blocks()) {
            auto *block = block_ptr.get();
            auto &plans = scratch_plans[block];
            auto &evictions = eviction_plans[block];
            plans.resize(block->instructions().size());
            evictions.resize(block->instructions().size());
            const auto &per_instr = live.instructions.at(block);
            for (std::size_t instr_index = 0; instr_index < block->instructions().size();
                 ++instr_index) {
                const auto &instr = block->instructions()[instr_index];
                std::uint32_t occupied = 1U; // v0 is exclusively the mask register.
                VRegSet active = collapse_set(per_instr[instr_index].before, sets);
                set_union_into(active, collapse_set(per_instr[instr_index].after, sets));

                VRegSet operand_roots;
                for (const auto &operand : instr.operands()) {
                    if (operand.is_reg() && is_virtual_vector(operand.reg_value())) {
                        const auto root = sets.find(operand.reg_value().id);
                        operand_roots.insert(root);
                        if (auto color = coloring.colors.find(root);
                            color != coloring.colors.end()) {
                            occupied |=
                                register_range_mask(color->second, nodes.at(root).width);
                        }
                    }
                    if (!operand.is_reg() || !operand.reg_value().is_physical() ||
                        !operand.reg_value().is_vector()) {
                        continue;
                    }
                    const auto base = vector_register_index(operand.reg_value().name);
                    if (base.has_value()) {
                        occupied |= register_range_mask(*base,
                                                        operand.reg_value().vector_group_width);
                    }
                }

                // A colored value which is live through this instruction but
                // is not one of its operands may be saved, temporarily
                // evicted, and restored.  This is required for M8: the only
                // legal groups are v8/v16/v24, so permanently reserving v24
                // as scratch otherwise leaves no way to execute a three-group
                // instruction when two operands were selected for spilling.
                std::map<VRegId, unsigned> evictable_colors;
                for (auto root : active) {
                    auto color = coloring.colors.find(root);
                    if (color == coloring.colors.end() ||
                        operand_roots.find(root) != operand_roots.end()) {
                        continue;
                    }
                    evictable_colors.emplace(root, color->second);
                }

                std::map<VRegId, std::pair<bool, bool>> occurrences;
                for (const auto &operand : instr.operands()) {
                    if (!operand.is_reg() || !is_virtual_vector(operand.reg_value())) {
                        continue;
                    }
                    const auto root = sets.find(operand.reg_value().id);
                    if (coloring.spills.find(root) == coloring.spills.end()) {
                        continue;
                    }
                    auto &flags = occurrences[root];
                    flags.first = flags.first || operand.is_use();
                    flags.second = flags.second || operand.is_def();
                }
                std::vector<VRegId> scratch_order;
                for (const auto &[root, flags] : occurrences) {
                    (void)flags;
                    scratch_order.push_back(root);
                }
                std::sort(scratch_order.begin(), scratch_order.end(), [&](VRegId lhs, VRegId rhs) {
                    if (nodes.at(lhs).width != nodes.at(rhs).width) {
                        return nodes.at(lhs).width > nodes.at(rhs).width;
                    }
                    return lhs < rhs;
                });
                for (auto root : scratch_order) {
                    const auto &node = nodes.at(root);
                    auto base = choose_scratch_base(node, occupied);
                    if (!base.has_value()) {
                        const auto scavenged = choose_scavenged_scratch_base(
                            node, occupied, evictable_colors, nodes);
                        if (scavenged.has_value()) {
                            base = scavenged->base;
                            for (auto evicted_root : scavenged->evicted_roots) {
                                evictions[instr_index].emplace(
                                    evicted_root,
                                    coloring.colors.at(evicted_root));
                            }
                        }
                    }
                    if (!base.has_value()) {
                        result.relegalize_requests.push_back(make_request(
                            function, node, MIRVectorRelegalizeReason::SpillScratchPressure,
                            "one instruction needs more simultaneous vector groups than the "
                            "architectural register file can provide after safe live-through "
                            "eviction"));
                        continue;
                    }
                    occupied |= register_range_mask(*base, node.width);
                    plans[instr_index][root] =
                        {*base, occurrences[root].first, occurrences[root].second};
                }
            }
        }
        if (!result.relegalize_requests.empty()) {
            result.success = false;
            result.message = requests_message(result.relegalize_requests);
            return result;
        }

        std::map<VRegId, int> spill_slots;
        for (auto root : coloring.spills) {
            const auto &node = nodes.at(root);
            spill_slots[root] = function.add_scalable_stack_slot(
                "rvv.spill.v" + std::to_string(node.members.front()), node.type,
                mir::StackSlotKind::Spill);
        }
        std::map<VRegId, int> eviction_slots;
        for (const auto &[block, plans] : eviction_plans) {
            (void)block;
            for (const auto &plan : plans) {
                for (const auto &[root, base] : plan) {
                    (void)base;
                    if (eviction_slots.find(root) != eviction_slots.end()) {
                        continue;
                    }
                    const auto &node = nodes.at(root);
                    eviction_slots[root] = function.add_scalable_stack_slot(
                        "rvv.evict.v" + std::to_string(node.members.front()),
                        node.type, mir::StackSlotKind::Spill);
                }
            }
        }

        for (const auto &[root, base] : coloring.colors) {
            const auto &node = nodes.at(root);
            const auto physical = physical_register(node, base);
            for (auto id : node.members) {
                const auto *virtual_reg = function.regs().virtual_register(id);
                if (virtual_reg != nullptr) {
                    function.regs().set_allocation(*virtual_reg, physical);
                }
            }
        }

        for (auto &block_ptr : function.blocks()) {
            auto *block = block_ptr.get();
            std::vector<mir::MachineInstr> rewritten;
            auto &plans = scratch_plans.at(block);
            auto &evictions = eviction_plans.at(block);
            for (std::size_t instr_index = 0; instr_index < block->instructions().size();
                 ++instr_index) {
                auto instr = std::move(block->instructions()[instr_index]);
                const auto &plan = plans[instr_index];
                const auto &eviction = evictions[instr_index];
                for (const auto &[root, base] : eviction) {
                    rewritten.push_back(
                        make_spill(nodes.at(root), eviction_slots.at(root), base));
                }
                for (const auto &[root, scratch] : plan) {
                    if (scratch.use) {
                        rewritten.push_back(
                            make_reload(nodes.at(root), spill_slots.at(root), scratch.base));
                    }
                }
                for (auto &operand : instr.operands()) {
                    if (!operand.is_reg() || !is_virtual_vector(operand.reg_value())) {
                        continue;
                    }
                    const auto root = sets.find(operand.reg_value().id);
                    if (auto color = coloring.colors.find(root); color != coloring.colors.end()) {
                        operand.set_reg(physical_register(nodes.at(root), color->second));
                    } else {
                        operand.set_reg(
                            physical_register(nodes.at(root), plan.at(root).base));
                    }
                }
                rewritten.push_back(std::move(instr));
                for (const auto &[root, scratch] : plan) {
                    if (scratch.def) {
                        rewritten.push_back(
                            make_spill(nodes.at(root), spill_slots.at(root), scratch.base));
                    }
                }
                for (const auto &[root, base] : eviction) {
                    rewritten.push_back(
                        make_reload(nodes.at(root), eviction_slots.at(root), base));
                }
            }
            block->instructions() = std::move(rewritten);
        }

        for (const auto &block_ptr : function.blocks()) {
            for (const auto &instr : block_ptr->instructions()) {
                for (const auto &operand : instr.operands()) {
                    if (operand.is_reg() && is_virtual_vector(operand.reg_value())) {
                        throw std::runtime_error(
                            "virtual vector register survived dedicated RVV allocation");
                    }
                }
            }
        }
        collect_variant_callee_saves(function);
        function.layout_frame();
        result.changed = true;
        return result;
    } catch (const std::exception &error) {
        result.success = false;
        result.message = error.what();
        return result;
    }
}

} // namespace pass
