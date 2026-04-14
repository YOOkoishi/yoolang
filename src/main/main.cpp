#include "../../include/IR/SSA_IR.h"
#include "../../include/include.h"

int main(int argc, char **argv) {
    std::cout << "compiler starting..." << std::endl;

    ir::Module module("ssa_smoke");
    auto *i32 = module.types().int32_ty();
    auto *fn_ty = module.types().func_ty(i32, {});
    auto *fn = module.create_function("smoke_main", fn_ty, false);
    auto *entry = fn->create_block("entry");

    ir::IRBuilder builder(&module);
    builder.set_insert_point(entry);

    auto *sum =
        builder.create_binary(ir::Instruction::OpID::Add, builder.i32(1), builder.i32(2), "sum");
    builder.create_ret(sum);

    std::string verify_message;
    if (!module.verify(&verify_message)) {
        std::cerr << "SSA verify failed: " << verify_message << std::endl;
        return 1;
    }

    std::cout << module.print() << std::endl;

    return 0;
}
