#include "pass/oir/OIRADCEPass.h"

#include "oir/OIRAnalysis.h"
#include "oir/OIRScalarOpt.h"

#include <algorithm>
#include <deque>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

bool is_live_root(const oir::Instruction &inst, const oir::FunctionModRefAnalysis &modref) {
    if (auto *call = dynamic_cast<const oir::CallInst *>(&inst)) {
        return modref.call_has_side_effect(*call);
    }

    switch (inst.op()) {
    case oir::Instruction::OpID::Ret:
    case oir::Instruction::OpID::Br:
    case oir::Instruction::OpID::Store:
    case oir::Instruction::OpID::MemZero:
        return true;
    default:
        return false;
    }
}

bool is_removable_instruction(const oir::Instruction &inst,
                              const oir::FunctionModRefAnalysis &modref) {
    if (auto *call = dynamic_cast<const oir::CallInst *>(&inst)) {
        return !modref.call_has_side_effect(*call);
    }
    return is_pure_instruction(inst);
}

bool run_adce_on_function(oir::Function &function, const oir::FunctionModRefAnalysis &modref,
                          Stats &stats) {
    std::unordered_set<oir::Instruction *> live;
    std::deque<oir::Instruction *> worklist;
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            if (is_live_root(*inst, modref)) {
                live.insert(inst.get());
                worklist.push_back(inst.get());
            }
        }
    }

    while (!worklist.empty()) {
        auto *inst = worklist.front();
        worklist.pop_front();
        for (auto *operand : inst->operands()) {
            auto *operand_inst = dynamic_cast<oir::Instruction *>(operand);
            if (operand_inst != nullptr && live.insert(operand_inst).second) {
                worklist.push_back(operand_inst);
            }
        }
    }

    std::vector<oir::Instruction *> dead;
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            if (live.find(inst.get()) == live.end() && is_removable_instruction(*inst, modref)) {
                dead.push_back(inst.get());
            }
        }
    }
    if (dead.empty()) {
        return false;
    }

    for (auto *inst : dead) {
        inst->drop_all_operands();
    }
    for (auto &block : function.blocks()) {
        for (auto it = block->instructions().begin(); it != block->instructions().end();) {
            if (std::find(dead.begin(), dead.end(), it->get()) != dead.end()) {
                it = block->instructions().erase(it);
            } else {
                ++it;
            }
        }
    }
    stats.adce += static_cast<unsigned>(dead.size());
    return true;
}

} // namespace

bool aggressive_dead_code_elimination(oir::Module &module, Stats &stats) {
    bool changed = false;
    oir::FunctionModRefAnalysis modref(module);
    for (auto &function : module.functions()) {
        if (!function->is_external()) {
            changed |= run_adce_on_function(*function, modref, stats);
        }
    }
    return changed;
}

} // namespace pass::oir_opt

namespace pass {

std::string_view OIRADCEPass::name() const {
    return "OIRADCEPass";
}

PassKind OIRADCEPass::kind() const {
    return PassKind::Transform;
}

PassResult OIRADCEPass::run(PassContext &context) {
    return oir_opt::run_oir_transform(context, "OIRADCEPass requires OIR module in pass context",
                                      [](oir::Module &module, oir_opt::Stats &stats) {
                                          bool changed =
                                              oir_opt::aggressive_dead_code_elimination(module,
                                                                                        stats);
                                          changed |= oir_opt::cleanup_cfg(module, stats);
                                          return changed;
                                      });
}

} // namespace pass
