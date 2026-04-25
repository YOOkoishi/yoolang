#pragma once

#include "MIR.h"

#include <iosfwd>
#include <string>

namespace mir {

class MIRPrinter final {
  public:
    explicit MIRPrinter(std::ostream &out);

    void print(const Module &module);

  private:
    void print_global(const Global &global);
    void print_function(const MachineFunction &function);
    void print_block(const MachineBasicBlock &block);
    std::string operand_string(const MachineFunction *function, const MachineOperand &operand);

    std::ostream &out_;
};

} // namespace mir
