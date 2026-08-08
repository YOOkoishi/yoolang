#pragma once

#include "MIR.h"

#include <cstdint>
#include <iosfwd>
#include <string>

namespace mir {

class AsmPrinter final {
  public:
    explicit AsmPrinter(std::ostream &out);

    void print(const Module &module);

  private:
    void print_global_sections(const Module &module);
    void print_global(const Global &global);
    void print_function(const MachineFunction &function);
    void print_instr(const MachineFunction &function, const MachineInstr &instr);
    void print_epilogue(const MachineFunction &function);

    void emit_adjust_sp(std::int64_t amount);
    void emit_adjust_scalable_sp(MachineScalableSize size, bool allocate);
    void emit_vector_callee_saved_address(const MachineFunction &function,
                                          const StackSlot &slot);

    void emit_int_slot_access(const std::string &mnemonic, const std::string &reg,
                              const std::string &base, std::int64_t offset);
    void emit_float_slot_access(const std::string &mnemonic, const std::string &reg,
                                const std::string &base, std::int64_t offset);

    bool fits_simm12(std::int64_t value) const;
    std::string label_for(const std::string &function, const std::string &block) const;
    std::string symbol_name(const std::string &name) const;
    bool is_zero_initializer(const Global &global) const;
    void print_initializer_bytes(const Global &global) const;

    std::ostream &out_;
};

} // namespace mir
