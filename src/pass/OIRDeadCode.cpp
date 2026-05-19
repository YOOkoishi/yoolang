#include "../../include/oir/OIRScalarOpt.h"

#include "../../include/oir/OIRAnalysis.h"

namespace pass::oir_opt {
namespace {

bool is_removable_instruction(const oir::Instruction &inst,
                              const oir::FunctionModRefAnalysis &modref) {
    if (auto *call = dynamic_cast<const oir::CallInst *>(&inst)) {
        return !modref.call_has_side_effect(*call);
    }
    return is_pure_instruction(inst);
}

} // namespace

bool eliminate_dead_code(oir::Module &module, Stats &stats) {
    bool changed = false;
    bool keep_going = true;
    while (keep_going) {
        keep_going = false;
        oir::FunctionModRefAnalysis modref(module);
        for (auto &function : module.functions()) {
            if (function->is_external()) {
                continue;
            }
            for (auto &block : function->blocks()) {
                for (auto it = block->instructions().begin(); it != block->instructions().end();) {
                    if (is_removable_instruction(**it, modref) && !(*it)->has_uses()) {
                        (*it)->drop_all_operands();
                        it = block->instructions().erase(it);
                        ++stats.dce;
                        changed = true;
                        keep_going = true;
                        continue;
                    }
                    ++it;
                }
            }
        }
    }
    return changed;
}

bool verify_oir(oir::Module &module, std::string &message) {
    message.clear();
    return module.verify(&message);
}

} // namespace pass::oir_opt
