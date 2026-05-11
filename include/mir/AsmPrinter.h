#pragma once

#include "MIR.h"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

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

    void emit_load_slot(const MachineFunction &function, const std::string &reg, int slot,
                        ValueType type);
    void emit_store_slot(const MachineFunction &function, const std::string &reg, int slot,
                         ValueType type);
    void emit_load_mem(const std::string &reg, const std::string &addr_reg, std::int64_t offset,
                       ValueType type);
    void emit_store_mem(const std::string &reg, const std::string &addr_reg, std::int64_t offset,
                        ValueType type);
    void emit_store_outgoing_arg(const std::string &reg, std::int64_t offset, ValueType type);
    void emit_load_incoming_arg(const MachineFunction &function, const std::string &reg,
                                std::int64_t offset, ValueType type);
    void emit_add_sp_offset(const std::string &dst, std::int64_t offset);
    void emit_adjust_sp(std::int64_t amount);
    void emit_memzero_loop(const std::string &addr_reg, std::uint64_t size);

    void emit_int_slot_access(const std::string &mnemonic, const std::string &reg,
                              const std::string &base, std::int64_t offset);
    void emit_float_slot_access(const std::string &mnemonic, const std::string &reg,
                                const std::string &base, std::int64_t offset);

    bool is_int_type(ValueType type) const;
    bool is_float_type(ValueType type) const;
    bool fits_simm12(std::int64_t value) const;
    std::string label_for(const std::string &function, const std::string &block) const;
    std::string symbol_name(const std::string &name) const;
    std::vector<std::uint32_t> initializer_words(const Global &global) const;

    std::ostream &out_;
    const MachineFunction *current_function_ = nullptr;
    unsigned unique_label_id_ = 0;
};

} // namespace mir
