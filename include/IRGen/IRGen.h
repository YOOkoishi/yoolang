#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../IR/SSA_IR.h"
#include "../ast/ast.h"

namespace irgen {

struct LoweringOptions {
    bool declare_runtime_builtins = true;
};

class ASTToIRLowering {
  public:
    explicit ASTToIRLowering(LoweringOptions options = {});

    std::unique_ptr<ir::Module> lower(CompUnit &unit, const std::string &module_name = "sysy");

  private:
    ir::Type *map_builtin_type(ir::TypeContext &types, BuiltinType type) const;
    std::vector<ir::Type *> map_param_types(ir::TypeContext &types,
                                            const std::vector<FuncParam> &params) const;

    void declare_runtime_builtins(ir::Module &module);
    void lower_global_decls(ir::Module &module, CompUnit &unit);
    void lower_function_signatures(ir::Module &module, CompUnit &unit);
    void lower_function_bodies(ir::Module &module, CompUnit &unit);

    LoweringOptions options_;
};

} // namespace irgen

// Backward compatibility for old includes/usages.
namespace codegen {
using LoweringOptions = irgen::LoweringOptions;
using ASTToSSA = irgen::ASTToIRLowering;
} // namespace codegen
