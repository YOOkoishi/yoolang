#pragma once

#include "MIR.h"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_map>
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
    void compute_stack_addr_facts(const MachineFunction &function);
    void update_stack_addr_facts(
        const MachineFunction &function, const MachineInstr &instr,
        std::unordered_map<std::string, std::int64_t> &facts) const;
    void invalidate_memzero_stack_addr_facts(
        const MachineInstr &instr, std::unordered_map<std::string, std::int64_t> &facts) const;
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
    void emit_memzero(const MachineOperand &addr, const MachineOperand &byte_value,
                      const MachineOperand &byte_count);
    bool emit_inline_memzero_stores(const std::string &addr_reg,
                                    const MachineOperand &byte_value,
                                    std::uint64_t size, bool prefer_wide_zero_stores);
    void emit_memzero_loop(const std::string &addr_reg, const MachineOperand &byte_value,
                           const MachineOperand &byte_count);
    void emit_memset_call(const std::string &addr_reg, const MachineOperand &byte_value,
                          std::uint64_t size);

    void emit_int_slot_access(const std::string &mnemonic, const std::string &reg,
                              const std::string &base, std::int64_t offset);
    void emit_float_slot_access(const std::string &mnemonic, const std::string &reg,
                                const std::string &base, std::int64_t offset);

    bool is_int_type(ValueType type) const;
    bool is_float_type(ValueType type) const;
    bool fits_simm12(std::int64_t value) const;
    std::string label_for(const std::string &function, const std::string &block) const;
    std::string symbol_name(const std::string &name) const;
    bool is_zero_initializer(const Global &global) const;
    void print_initializer_words(const Global &global,
                                 const std::vector<std::uint32_t> &words) const;
    std::vector<std::uint32_t> initializer_words(const Global &global) const;

    std::ostream &out_;
    const MachineFunction *current_function_ = nullptr;
    std::unordered_map<const MachineBasicBlock *,
                       std::unordered_map<std::string, std::int64_t>>
        stack_addr_block_in_;
    std::unordered_map<std::string, std::int64_t> known_stack_addr_offsets_;
    unsigned unique_label_id_ = 0;
};

} // namespace mir
