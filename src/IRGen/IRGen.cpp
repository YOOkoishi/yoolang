#include "../../include/IRGen/IRGen.h"

#include <algorithm>

namespace irgen {

namespace {

std::size_t eval_dimension(const std::unique_ptr<Expr> &expr) {
    if (expr == nullptr) {
        return 1;
    }

    auto *int_lit = dynamic_cast<IntLiteral *>(expr.get());
    if (int_lit == nullptr || int_lit->value <= 0) {
        return 1;
    }

    return static_cast<std::size_t>(int_lit->value);
}

ir::Value *try_lower_global_scalar_init(ir::Module &module, const VarDecl &decl,
                                        ir::Type *value_type) {
    if (decl.init == nullptr || decl.init->expr == nullptr) {
        return nullptr;
    }

    if (value_type->is_integer()) {
        auto *int_lit = dynamic_cast<IntLiteral *>(decl.init->expr.get());
        if (int_lit != nullptr) {
            return module.create_i32(int_lit->value);
        }
    }

    if (value_type->is_float()) {
        auto *float_lit = dynamic_cast<FloatLiteral *>(decl.init->expr.get());
        if (float_lit != nullptr) {
            return module.create_f32(float_lit->value);
        }
    }

    return nullptr;
}

ir::Type *build_decl_type(ir::TypeContext &types, ir::Type *base_type,
                          const std::vector<std::unique_ptr<Expr>> &dimensions) {
    ir::Type *current = base_type;
    for (auto it = dimensions.rbegin(); it != dimensions.rend(); ++it) {
        current = types.array_ty(current, eval_dimension(*it));
    }
    return current;
}

} // namespace

ASTToIRLowering::ASTToIRLowering(LoweringOptions options) : options_(options) {
}

std::unique_ptr<ir::Module> ASTToIRLowering::lower(CompUnit &unit, const std::string &module_name) {
    auto module = std::make_unique<ir::Module>(module_name);

    if (options_.declare_runtime_builtins) {
        declare_runtime_builtins(*module);
    }

    lower_global_decls(*module, unit);
    lower_function_signatures(*module, unit);
    lower_function_bodies(*module, unit);

    return module;
}

ir::Type *ASTToIRLowering::map_builtin_type(ir::TypeContext &types, BuiltinType type) const {
    switch (type) {
    case BuiltinType::Void:
        return types.void_ty();
    case BuiltinType::Int:
        return types.int32_ty();
    case BuiltinType::Float:
        return types.float_ty();
    }

    return types.int32_ty();
}

std::vector<ir::Type *>
ASTToIRLowering::map_param_types(ir::TypeContext &types,
                                 const std::vector<FuncParam> &params) const {
    std::vector<ir::Type *> param_types;
    param_types.reserve(params.size());

    for (const auto &param : params) {
        ir::Type *base = map_builtin_type(types, param.type);
        if (!param.dimensions.empty()) {
            param_types.push_back(types.ptr_ty(base));
            continue;
        }
        param_types.push_back(base);
    }

    return param_types;
}

void ASTToIRLowering::declare_runtime_builtins(ir::Module &module) {
    auto &types = module.types();

    auto declare_fn = [&](const std::string &name, ir::Type *ret,
                          const std::vector<ir::Type *> &params) {
        auto *fn_type = types.func_ty(ret, params);
        module.create_function(name, fn_type, true);
    };

    auto *i32 = types.int32_ty();
    auto *f32 = types.float_ty();
    auto *void_ty = types.void_ty();
    auto *pi32 = types.ptr_ty(i32);
    auto *pf32 = types.ptr_ty(f32);

    declare_fn("getint", i32, {});
    declare_fn("getch", i32, {});
    declare_fn("getfloat", f32, {});
    declare_fn("getarray", i32, {pi32});
    declare_fn("getfarray", i32, {pf32});
    declare_fn("putint", void_ty, {i32});
    declare_fn("putch", void_ty, {i32});
    declare_fn("putfloat", void_ty, {f32});
    declare_fn("putarray", void_ty, {i32, pi32});
    declare_fn("putfarray", void_ty, {i32, pf32});
    declare_fn("starttime", void_ty, {});
    declare_fn("stoptime", void_ty, {});
}

void ASTToIRLowering::lower_global_decls(ir::Module &module, CompUnit &unit) {
    auto &types = module.types();

    for (auto &decl_stmt : unit.global_decls) {
        if (decl_stmt == nullptr) {
            continue;
        }

        for (auto &decl : decl_stmt->decls) {
            if (decl == nullptr) {
                continue;
            }

            ir::Type *base_type = map_builtin_type(types, decl->base_type);
            ir::Type *value_type = build_decl_type(types, base_type, decl->dimensions);
            ir::Value *init_value = try_lower_global_scalar_init(module, *decl, value_type);
            module.create_global(decl->name, value_type, decl->is_const, init_value);
        }
    }
}

void ASTToIRLowering::lower_function_signatures(ir::Module &module, CompUnit &unit) {
    auto &types = module.types();

    for (auto &func : unit.functions) {
        if (func == nullptr) {
            continue;
        }

        ir::Type *ret_type = map_builtin_type(types, func->return_type);
        auto param_types = map_param_types(types, func->params);
        auto *fn_type = types.func_ty(ret_type, param_types);
        auto *fn = module.create_function(func->name, fn_type, false);
        fn->set_external(false);

        const auto param_count = std::min(fn->args().size(), func->params.size());
        for (std::size_t i = 0; i < param_count; ++i) {
            fn->args()[i]->set_name(func->params[i].name);
        }
    }
}

void ASTToIRLowering::lower_function_bodies(ir::Module &module, CompUnit &unit) {
    ir::IRBuilder builder(&module);

    for (auto &func : unit.functions) {
        if (func == nullptr) {
            continue;
        }

        auto *ir_func = module.get_function(func->name);
        if (ir_func == nullptr || ir_func->is_external()) {
            continue;
        }

        auto *entry = ir_func->entry_block();
        if (entry == nullptr) {
            entry = ir_func->create_block("entry");
        }

        builder.set_insert_point(entry);

        // 当前阶段先确保每个函数体具备合法终结指令，AST 语句 lowering 在下一步补齐。
        if (ir_func->return_type()->is_void()) {
            builder.create_ret(nullptr);
        } else if (ir_func->return_type()->is_float()) {
            builder.create_ret(builder.f32(0.0f));
        } else {
            builder.create_ret(builder.i32(0));
        }
    }
}

} // namespace irgen
