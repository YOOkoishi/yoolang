#include "../../include/oir/OIR.h"
#include "../../include/oir/OIRCFGUtils.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

bool contains(const std::string &text, const std::string &needle) {
    return text.find(needle) != std::string::npos;
}

bool contains_block_ptr_for_test(const std::vector<oir::BasicBlock *> &blocks,
                                 const oir::BasicBlock *needle) {
    for (auto *block : blocks) {
        if (block == needle) {
            return true;
        }
    }
    return false;
}

bool test_rauw_use_list() {
    oir::Module module("rauw");
    auto *i32 = module.types().int32_ty();
    auto *fn_ty = module.types().func_ty(i32, {i32, i32});
    auto *fn = module.create_function("f", fn_ty);
    auto *entry = fn->create_block("entry");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *a = fn->args()[0].get();
    auto *b = fn->args()[1].get();
    auto *x = builder.create_binary(oir::Instruction::OpID::Add, a, b, "x");
    auto *y = builder.create_binary(oir::Instruction::OpID::Add, x, b, "y");
    builder.create_ret(y);

    bool ok = true;
    ok &= expect(x->use_count() == 1, "instruction use_count tracks ordinary operands");
    x->replace_all_uses_with(a);
    ok &= expect(!x->has_uses(), "RAUW clears old value uses");
    ok &= expect(y->lhs() == a, "RAUW rewrites user operand");
    ok &= expect(module.verify(), "module verifies after RAUW");
    return ok;
}

bool test_phi_use_update() {
    oir::Module module("phi");
    auto *i1 = module.types().int1_ty();
    auto *i32 = module.types().int32_ty();
    auto *fn_ty = module.types().func_ty(i32, {i1, i32, i32});
    auto *fn = module.create_function("f", fn_ty);
    auto *entry = fn->create_block("entry");
    auto *left = fn->create_block("left");
    auto *right = fn->create_block("right");
    auto *merge = fn->create_block("merge");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_cond_br(fn->args()[0].get(), left, right);
    builder.set_insert_point(left);
    builder.create_br(merge);
    builder.set_insert_point(right);
    builder.create_br(merge);
    builder.set_insert_point(merge);
    auto *phi = builder.create_phi(i32, "p");
    phi->add_incoming(fn->args()[1].get(), left);
    phi->add_incoming(fn->args()[2].get(), right);
    builder.create_ret(phi);

    const auto left_uses = left->use_count();
    phi->remove_incoming_from(left);

    bool ok = true;
    ok &= expect(phi->incoming().size() == 1, "phi incoming removal updates incoming list");
    ok &= expect(phi->operand_count() == 2, "phi incoming removal updates operands");
    ok &= expect(left->use_count() + 1 == left_uses, "phi incoming removal updates block use-list");
    ok &= expect(fn->args()[1]->use_count() == 0, "phi incoming removal updates value use-list");
    return ok;
}

bool test_cfg_helper_redirect() {
    oir::Module module("cfg");
    auto *i1 = module.types().int1_ty();
    auto *i32 = module.types().int32_ty();
    auto *fn_ty = module.types().func_ty(i32, {i1, i32, i32});
    auto *fn = module.create_function("f", fn_ty);
    auto *entry = fn->create_block("entry");
    auto *left = fn->create_block("left");
    auto *right = fn->create_block("right");
    auto *merge = fn->create_block("merge");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_cond_br(fn->args()[0].get(), left, right);
    builder.set_insert_point(left);
    builder.create_br(merge);
    builder.set_insert_point(right);
    builder.create_br(merge);
    builder.set_insert_point(merge);
    auto *phi = builder.create_phi(i32, "p");
    phi->add_incoming(fn->args()[1].get(), left);
    phi->add_incoming(fn->args()[2].get(), right);
    builder.create_ret(phi);

    bool ok = true;
    ok &= expect(oir::cfg::replace_successor(left, merge, right),
                 "replace_successor rewrites branch target");
    ok &= expect(static_cast<oir::BranchInst *>(left->terminator())->target_bb() == right,
                 "replace_successor updates terminator operand");
    ok &= expect(!contains_block_ptr_for_test(merge->predecessors(), left),
                 "replace_successor removes old predecessor");
    ok &= expect(contains_block_ptr_for_test(right->predecessors(), left),
                 "replace_successor adds new predecessor");
    ok &= expect(phi->incoming().size() == 1 && phi->incoming().front().second == right,
                 "replace_successor removes stale phi incoming");
    ok &= expect(module.verify(), "module verifies after CFG helper redirect");
    return ok;
}

bool test_verifier_def_dominates_use() {
    oir::Module module("bad_dom");
    auto *i1 = module.types().int1_ty();
    auto *i32 = module.types().int32_ty();
    auto *fn_ty = module.types().func_ty(i32, {i1, i32, i32});
    auto *fn = module.create_function("f", fn_ty);
    auto *entry = fn->create_block("entry");
    auto *left = fn->create_block("left");
    auto *right = fn->create_block("right");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_cond_br(fn->args()[0].get(), left, right);
    builder.set_insert_point(left);
    auto *only_left = builder.create_binary(oir::Instruction::OpID::Add, fn->args()[1].get(),
                                            fn->args()[2].get(), "only.left");
    builder.create_ret(fn->args()[1].get());
    builder.set_insert_point(right);
    builder.create_ret(only_left);

    std::string message;
    bool ok = expect(!module.verify(&message), "verifier rejects non-dominating definition");
    ok &= expect(contains(message, "does not dominate"), "verifier reports dominance context");
    return ok;
}

bool test_verifier_phi_duplicate_pred() {
    oir::Module module("bad_phi");
    auto *i1 = module.types().int1_ty();
    auto *i32 = module.types().int32_ty();
    auto *fn_ty = module.types().func_ty(i32, {i1, i32, i32});
    auto *fn = module.create_function("f", fn_ty);
    auto *entry = fn->create_block("entry");
    auto *left = fn->create_block("left");
    auto *right = fn->create_block("right");
    auto *merge = fn->create_block("merge");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_cond_br(fn->args()[0].get(), left, right);
    builder.set_insert_point(left);
    builder.create_br(merge);
    builder.set_insert_point(right);
    builder.create_br(merge);
    builder.set_insert_point(merge);
    auto *phi = builder.create_phi(i32, "p");
    phi->add_incoming(fn->args()[1].get(), left);
    phi->add_incoming(fn->args()[2].get(), left);
    builder.create_ret(phi);

    std::string message;
    bool ok = expect(!module.verify(&message), "verifier rejects duplicate phi predecessor");
    ok &= expect(contains(message, "duplicate incoming predecessor"),
                 "verifier reports duplicate phi predecessor");
    return ok;
}

bool test_verifier_cfg_mismatch() {
    oir::Module module("bad_cfg");
    auto *i32 = module.types().int32_ty();
    auto *fn_ty = module.types().func_ty(i32, {});
    auto *fn = module.create_function("f", fn_ty);
    auto *entry = fn->create_block("entry");
    auto *extra = fn->create_block("extra");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_ret(module.create_i32(0));
    builder.set_insert_point(extra);
    builder.create_ret(module.create_i32(1));
    entry->add_successor(extra);
    extra->add_predecessor(entry);

    std::string message;
    bool ok = expect(!module.verify(&message), "verifier rejects return block successor");
    ok &= expect(contains(message, "return block"), "verifier reports CFG mismatch context");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_rauw_use_list();
    ok &= test_phi_use_update();
    ok &= test_cfg_helper_redirect();
    ok &= test_verifier_def_dominates_use();
    ok &= test_verifier_phi_duplicate_pred();
    ok &= test_verifier_cfg_mismatch();
    if (!ok) {
        return 1;
    }
    std::cout << "OIR infra tests passed\n";
    return 0;
}
