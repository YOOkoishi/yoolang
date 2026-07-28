#!/usr/bin/env python3

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]


SOURCE = r"""
#include "oir/OIR.h"
#include "oir/OIRAnalysis.h"
#include "oir/OIRCFGUtils.h"
#include "yir/Presburger.h"

#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final : public std::exception {
    explicit TestFailure(std::string message) : message(std::move(message)) {}

    const char *what() const noexcept override {
        return message.c_str();
    }

    std::string message;
};

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw TestFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +        \
                              ": requirement failed: " #condition);                           \
        }                                                                                       \
    } while (false)

void require_verify(oir::Module &module) {
    std::string message;
    if (!module.verify(&message)) {
        throw TestFailure("expected verifier success, got: " + message);
    }
}

void require_verify_fails(oir::Module &module, const std::string &needle) {
    std::string message;
    if (module.verify(&message)) {
        throw TestFailure("expected verifier failure containing '" + needle + "'");
    }
    if (message.find(needle) == std::string::npos) {
        throw TestFailure("expected verifier failure containing '" + needle + "', got: " +
                          message);
    }
}

oir::Function *create_function(oir::Module &module, const std::string &name,
                               oir::Type *return_type,
                               const std::vector<oir::Type *> &param_types = {}) {
    return module.create_function(name, module.types().func_ty(return_type, param_types));
}

void test_replace_all_uses_with_updates_use_lists() {
    oir::Module module("replace");
    auto *function =
        create_function(module, "f", module.types().int32_ty(), {module.types().int32_ty()});
    auto *arg = function->add_argument(module.types().int32_ty(), "x");
    auto *entry = function->create_block("entry");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *one = builder.i32(1);
    auto *two = builder.i32(2);
    auto *sum = builder.create_binary(oir::Instruction::OpID::Add, arg, one, "sum");
    auto *twice = builder.create_binary(oir::Instruction::OpID::Mul, sum, two, "twice");
    builder.create_ret(twice);

    require_verify(module);
    REQUIRE(sum->use_count() == 1);
    REQUIRE(twice->lhs() == sum);

    sum->replace_all_uses_with(arg);

    REQUIRE(sum->use_count() == 0);
    REQUIRE(twice->lhs() == arg);
    require_verify(module);
}

void test_erasing_instruction_drops_operand_uses() {
    oir::Module module("erase_inst");
    auto *function =
        create_function(module, "f", module.types().int32_ty(), {module.types().int32_ty()});
    auto *arg = function->add_argument(module.types().int32_ty(), "x");
    auto *entry = function->create_block("entry");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *one = builder.i32(1);
    auto *dead = builder.create_binary(oir::Instruction::OpID::Add, arg, one, "dead");
    builder.create_ret(arg);

    require_verify(module);
    REQUIRE(arg->use_count() == 2);
    REQUIRE(one->use_count() == 1);

    for (auto it = entry->instructions().begin(); it != entry->instructions().end(); ++it) {
        if (it->get() == dead) {
            (*it)->drop_all_operands();
            entry->instructions().erase(it);
            break;
        }
    }

    REQUIRE(arg->use_count() == 1);
    REQUIRE(one->use_count() == 0);
    require_verify(module);
}

void test_phi_incoming_removal_reindexes_use_lists() {
    oir::Module module("phi_remove");
    auto *function =
        create_function(module, "f", module.types().int32_ty(), {module.types().int1_ty()});
    auto *cond = function->add_argument(module.types().int1_ty(), "c");
    auto *entry = function->create_block("entry");
    auto *then_block = function->create_block("then");
    auto *else_block = function->create_block("else");
    auto *merge_block = function->create_block("merge");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_cond_br(cond, then_block, else_block);
    builder.set_insert_point(then_block);
    builder.create_br(merge_block);
    builder.set_insert_point(else_block);
    builder.create_br(merge_block);
    builder.set_insert_point(merge_block);
    auto *from_then = builder.i32(10);
    auto *from_else = builder.i32(20);
    auto *else_return = builder.i32(30);
    auto *phi = builder.create_phi(module.types().int32_ty(), "p");
    phi->add_incoming(from_then, then_block);
    phi->add_incoming(from_else, else_block);
    builder.create_ret(phi);

    require_verify(module);
    REQUIRE(from_else->use_count() == 1);

    oir::cfg::remove_edge(else_block, merge_block);
    else_block->terminator()->drop_all_operands();
    else_block->instructions().pop_back();
    builder.set_insert_point(else_block);
    builder.create_ret(else_return);

    REQUIRE(phi->incoming().size() == 1);
    REQUIRE(phi->operand_count() == 2);
    REQUIRE(from_else->use_count() == 0);
    require_verify(module);
}

void test_erasing_block_drops_instruction_operand_uses() {
    oir::Module module("erase_block");
    auto *function = create_function(module, "f", module.types().int32_ty());
    auto *entry = function->create_block("entry");
    auto *dead_block = function->create_block("dead");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *zero = builder.i32(0);
    builder.create_ret(zero);

    builder.set_insert_point(dead_block);
    auto *four = builder.i32(4);
    auto *five = builder.i32(5);
    auto *sum = builder.create_binary(oir::Instruction::OpID::Add, four, five, "sum");
    builder.create_ret(sum);

    require_verify(module);
    REQUIRE(four->use_count() == 1);
    REQUIRE(five->use_count() == 1);

    function->erase_block(dead_block);

    REQUIRE(four->use_count() == 0);
    REQUIRE(five->use_count() == 0);
    require_verify(module);
}

class RawOperandInst final : public oir::Instruction {
  public:
    RawOperandInst(oir::Type *type, oir::Value *value, oir::BasicBlock *parent)
        : oir::Instruction(type, oir::Instruction::OpID::Alloca, parent, "bad") {
        add_operand(value);
    }

    void corrupt_by_clearing_operands_without_use_update() {
        operands_.clear();
    }

    std::string print() const override {
        return "%bad = alloca";
    }
};

void test_verifier_catches_stale_constant_use_list() {
    oir::Module module("stale_constant");
    auto *function = create_function(module, "f", module.types().int32_ty());
    auto *entry = function->create_block("entry");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *constant = builder.i32(42);
    auto bad = std::make_unique<RawOperandInst>(module.types().ptr_ty(module.types().int32_ty()),
                                                constant, entry);
    auto *raw_bad = bad.get();
    entry->append_instruction(std::move(bad));
    raw_bad->corrupt_by_clearing_operands_without_use_update();
    builder.create_ret(constant);

    require_verify_fails(module, "stale use-list");
}

void test_memory_ssa_skips_noalias_defs() {
    oir::Module module("memory_ssa_noalias");
    auto *g = module.create_global("g", module.types().int32_ty(), false);
    auto *h = module.create_global("h", module.types().int32_ty(), false);
    auto *function = create_function(module, "f", module.types().int32_ty());
    auto *entry = function->create_block("entry");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *store_g = builder.create_store(builder.i32(1), g);
    auto *store_h = builder.create_store(builder.i32(2), h);
    auto *load_g = builder.create_load(g, module.types().int32_ty(), "lg");
    builder.create_ret(load_g);

    require_verify(module);

    oir::OIRAliasAnalysis aa;
    oir::FunctionModRefAnalysis modref(module);
    oir::MemorySSA memory_ssa(*function, aa, modref);

    auto *clobber = memory_ssa.clobbering_access(*load_g);
    REQUIRE(clobber != nullptr);
    REQUIRE(clobber->instruction() == store_g);
    REQUIRE(memory_ssa.access_for(store_h) != nullptr);
    REQUIRE(memory_ssa.access_for(store_h)->is_def());
}

void test_memory_ssa_uses_phi_at_cfg_join() {
    oir::Module module("memory_ssa_phi");
    auto *g = module.create_global("g", module.types().int32_ty(), false);
    auto *function =
        create_function(module, "f", module.types().int32_ty(), {module.types().int1_ty()});
    auto *cond = function->add_argument(module.types().int1_ty(), "cond");
    auto *entry = function->create_block("entry");
    auto *then_block = function->create_block("then");
    auto *else_block = function->create_block("else");
    auto *merge_block = function->create_block("merge");

    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_cond_br(cond, then_block, else_block);
    builder.set_insert_point(then_block);
    builder.create_store(builder.i32(1), g);
    builder.create_br(merge_block);
    builder.set_insert_point(else_block);
    builder.create_br(merge_block);
    builder.set_insert_point(merge_block);
    auto *load_g = builder.create_load(g, module.types().int32_ty(), "lg");
    builder.create_ret(load_g);

    require_verify(module);

    oir::OIRAliasAnalysis aa;
    oir::FunctionModRefAnalysis modref(module);
    oir::MemorySSA memory_ssa(*function, aa, modref);

    auto *phi = memory_ssa.memory_phi(merge_block);
    REQUIRE(phi != nullptr);
    REQUIRE(phi->is_phi());
    REQUIRE(phi->incoming().size() == 2);
    REQUIRE(memory_ssa.clobbering_access(*load_g) == phi);
}

void test_presburger_uses_explicit_bounds_outside_fallback_window() {
    yir::presburger::IntegerRelation relation(1);
    relation.add_inequality({1}, -100);
    relation.add_inequality({-1}, 128);

    auto sample = relation.find_integer_sample();
    REQUIRE(sample.has_value());
    REQUIRE(sample->size() == 1);
    REQUIRE((*sample)[0] >= 100);
    REQUIRE((*sample)[0] <= 128);

    auto lexmax = relation.find_lexicographic_maximum();
    REQUIRE(lexmax.has_value());
    REQUIRE((*lexmax)[0] == 128);

    yir::presburger::IntegerRelation empty(1);
    empty.add_inequality({1}, -100);
    empty.add_inequality({-1}, 99);
    REQUIRE(empty.is_integer_empty());
}

void test_presburger_prunes_infeasible_multivariable_box() {
    constexpr unsigned kVariables = 8;
    yir::presburger::IntegerRelation relation(kVariables);
    for (unsigned variable = 0; variable < kVariables; ++variable) {
        std::vector<std::int64_t> lower(kVariables, 0);
        std::vector<std::int64_t> upper(kVariables, 0);
        lower[variable] = 1;
        upper[variable] = -1;
        relation.add_inequality(std::move(lower), 0);
        relation.add_inequality(std::move(upper), 32);
    }
    std::vector<std::int64_t> sum(kVariables, 1);
    relation.add_equality(std::move(sum), -1000);
    REQUIRE(relation.is_integer_empty());
}

void test_presburger_local_floor_div_constraints() {
    yir::presburger::IntegerRelation relation(1);
    relation.add_vars(yir::presburger::VarKind::Local, 1);
    relation.add_equality({1, 0}, -9);
    relation.add_inequality({1, -4}, 0);
    relation.add_inequality({-1, 4}, 3);
    auto sample = relation.find_integer_sample();
    REQUIRE(sample.has_value());
    REQUIRE(sample->size() == 2);
    REQUIRE((*sample)[0] == 9);
    REQUIRE((*sample)[1] == 2);
}

void test_presburger_budgeted_feasibility_is_tristate() {
    yir::presburger::IntegerRelation empty(2);
    empty.add_inequality({1, 0}, 0);
    empty.add_inequality({-1, 0}, 4);
    empty.add_inequality({0, 1}, 0);
    empty.add_inequality({0, -1}, 4);
    empty.add_equality({1, 1}, -20);
    REQUIRE(empty.check_integer_feasibility(1000) ==
            yir::presburger::IntegerFeasibility::Empty);

    yir::presburger::IntegerRelation sample(1);
    sample.add_inequality({1}, 0);
    sample.add_inequality({-1}, 4);
    REQUIRE(sample.check_integer_feasibility(1000) ==
            yir::presburger::IntegerFeasibility::NonEmpty);
    REQUIRE(sample.check_integer_feasibility(1) ==
            yir::presburger::IntegerFeasibility::Unknown);

    yir::presburger::IntegerRelation unbounded(1);
    unbounded.add_inequality({1}, 0);
    REQUIRE(unbounded.check_integer_feasibility(1000) ==
            yir::presburger::IntegerFeasibility::Unknown);
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, void (*)()>> tests = {
        {"replace_all_uses_with_updates_use_lists", test_replace_all_uses_with_updates_use_lists},
        {"erasing_instruction_drops_operand_uses", test_erasing_instruction_drops_operand_uses},
        {"phi_incoming_removal_reindexes_use_lists",
         test_phi_incoming_removal_reindexes_use_lists},
        {"erasing_block_drops_instruction_operand_uses",
         test_erasing_block_drops_instruction_operand_uses},
        {"verifier_catches_stale_constant_use_list",
         test_verifier_catches_stale_constant_use_list},
        {"memory_ssa_skips_noalias_defs", test_memory_ssa_skips_noalias_defs},
        {"memory_ssa_uses_phi_at_cfg_join", test_memory_ssa_uses_phi_at_cfg_join},
        {"presburger_uses_explicit_bounds_outside_fallback_window",
         test_presburger_uses_explicit_bounds_outside_fallback_window},
        {"presburger_prunes_infeasible_multivariable_box",
         test_presburger_prunes_infeasible_multivariable_box},
        {"presburger_local_floor_div_constraints",
         test_presburger_local_floor_div_constraints},
        {"presburger_budgeted_feasibility_is_tristate",
         test_presburger_budgeted_feasibility_is_tristate},
    };

    for (const auto &[name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &ex) {
            std::cerr << "FAIL " << name << ": " << ex.what() << '\n';
            return 1;
        }
    }
    return 0;
}
"""


def find_cxx() -> str | None:
    env_cxx = os.environ.get("CXX")
    if env_cxx:
        return env_cxx
    for candidate in ("c++", "g++", "clang++"):
        path = shutil.which(candidate)
        if path:
            return path
    return None


def main() -> int:
    cxx = find_cxx()
    if cxx is None:
        print("error: no C++ compiler found in PATH", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="oir-infra-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "oir_infra_tests.cpp"
        binary = tmp_dir / "oir_infra_tests"
        source.write_text(textwrap.dedent(SOURCE))

        compile_cmd = [
            cxx,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-I",
            str(ROOT / "include"),
            str(source),
            str(ROOT / "src/oir/OIR.cpp"),
            str(ROOT / "src/oir/OIRAnalysis.cpp"),
            str(ROOT / "src/oir/OIRCFGUtils.cpp"),
            str(ROOT / "src/yir/Presburger.cpp"),
            "-o",
            str(binary),
        ]
        compile_proc = subprocess.run(
            compile_cmd,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if compile_proc.returncode != 0:
            if compile_proc.stdout:
                print(compile_proc.stdout, end="")
            if compile_proc.stderr:
                print(compile_proc.stderr, end="", file=sys.stderr)
            return compile_proc.returncode

        run_proc = subprocess.run(
            [str(binary)],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if run_proc.stdout:
            print(run_proc.stdout, end="")
        if run_proc.stderr:
            print(run_proc.stderr, end="", file=sys.stderr)
        return run_proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
