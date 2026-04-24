#pragma once

#include "YIR.h"

#include <iosfwd>
#include <string>
#include <unordered_map>

namespace yir {

class YIRPrinter {
  public:
    explicit YIRPrinter(std::ostream &out);

    void print(const Module &module);

  private:
    void print_global(const Global &global);
    void print_function(const Function &function);
    void print_region(const Region &region);
    void print_operation(const Operation &op);

    std::string value_name(const Value *value);
    std::string init_value(const Value *value);
    void write_indent();
    void write_result(const Operation &op);
    void with_indent(int delta);

    std::ostream &out_;
    int indent_ = 0;
    unsigned next_value_id_ = 0;
    std::unordered_map<const Value *, std::string> names_;
};

std::string print_yir_to_string(const Module &module);

} // namespace yir
