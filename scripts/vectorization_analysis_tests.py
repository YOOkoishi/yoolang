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
#include "pass/oir/OIRVectorization.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw std::runtime_error(std::string("requirement failed: ") + #condition);        \
        }                                                                                       \
    } while (false)

struct LoopFixture {
    oir::Module module{"loop_fixture"};
    oir::Function *function = nullptr;
    oir::BasicBlock *header = nullptr;
    oir::PhiInst *iv = nullptr;
    const oir::Loop *loop = nullptr;
    std::unique_ptr<oir::DominatorTree> dom;
    std::unique_ptr<oir::LoopInfo> loops;
    std::unique_ptr<oir::ScalarEvolution> scev;

    LoopFixture(bool pointer_arguments, bool unsafe_division = false,
                std::int32_t trip_count = 32) {
        auto &types = module.types();
        std::vector<oir::Type *> params;
        if (pointer_arguments) {
            params = {types.ptr_ty(types.int32_ty()), types.ptr_ty(types.int32_ty())};
        }
        function = module.create_function("kernel", types.func_ty(types.void_ty(), params));
        auto *entry = function->create_block("entry");
        header = function->create_block("loop.header");
        auto *body = function->create_block("loop.body");
        auto *exit = function->create_block("exit");
        oir::IRBuilder builder(&module);

        const oir::Value *base_a = nullptr;
        const oir::Value *base_b = nullptr;
        if (pointer_arguments) {
            base_a = function->args()[0].get();
            base_b = function->args()[1].get();
        } else {
            base_a = module.create_global("a", types.array_ty(types.int32_ty(), 4096), false);
            base_b = module.create_global("b", types.array_ty(types.int32_ty(), 4096), false);
        }

        builder.set_insert_point(entry);
        builder.create_br(header);
        builder.set_insert_point(header);
        iv = builder.create_phi(types.int32_ty(), "iv");
        auto *condition =
            builder.create_icmp(oir::CmpPred::LT, iv, builder.i32(trip_count), "cond");
        builder.create_cond_br(condition, body, exit);
        builder.set_insert_point(body);
        auto *pa = builder.create_gep(const_cast<oir::Value *>(base_a),
                                      types.ptr_ty(types.int32_ty()), {builder.i32(0), iv}, "pa");
        auto *pb = builder.create_gep(const_cast<oir::Value *>(base_b),
                                      types.ptr_ty(types.int32_ty()), {builder.i32(0), iv}, "pb");
        auto *value = builder.create_load(pa, types.int32_ty(), "value");
        oir::Value *computed = builder.create_binary(oir::Instruction::OpID::Add, value,
                                                     builder.i32(1), "inc");
        if (unsafe_division) {
            computed = builder.create_binary(oir::Instruction::OpID::SDiv, computed, iv, "div");
        }
        builder.create_store(computed, pb);
        auto *next = builder.create_binary(oir::Instruction::OpID::Add, iv, builder.i32(1), "next");
        builder.create_br(header);
        builder.set_insert_point(exit);
        builder.create_ret();
        iv->add_incoming(builder.i32(0), entry);
        iv->add_incoming(next, body);

        dom = std::make_unique<oir::DominatorTree>(*function);
        loops = std::make_unique<oir::LoopInfo>(*function, *dom);
        REQUIRE(loops->loops().size() == 1);
        loop = &loops->loops().front();
        scev = std::make_unique<oir::ScalarEvolution>(*function, *loops);
    }
};

target::TargetProfile rvv_profile() {
    target::TargetProfile profile;
    profile.march = "rv64gcv";
    std::string error;
    REQUIRE(target::finalize_target_profile(profile, error));
    return profile;
}

void test_distinct_globals_are_legal_and_costed() {
    LoopFixture fixture(false, false, 4096);
    oir::OIRAliasAnalysis aa;
    pass::oir_vectorize::LoopVectorizationLegality legality;
    auto result = legality.analyze(*fixture.function, *fixture.loop, *fixture.loops,
                                   *fixture.scev, aa);
    REQUIRE(result.legal);
    REQUIRE(result.code == pass::oir_vectorize::RemarkCode::Candidate);
    REQUIRE(result.canonical_induction == fixture.iv);
    REQUIRE(result.induction_step == 1);
    REQUIRE(!result.memory.requires_runtime_alias_check);

    pass::oir_vectorize::RVVCostModel cost(rvv_profile());
    auto plan = cost.choose(result);
    REQUIRE(plan.profitable);
    REQUIRE(plan.choice.scalable);
    REQUIRE(plan.choice.minimum_lanes > 0);
    REQUIRE(plan.choice.interleave == 1);
    REQUIRE(plan.choice.tuning == "generic-rv64");
    REQUIRE(plan.choice.estimated_code_bytes > 0);
    REQUIRE(plan.choice.break_even_trip_count > 0);

    pass::oir_vectorize::CostModelOptions o3_options;
    o3_options.explore_interleave = true;
    o3_options.expected_trip_count = 4096;
    auto gated_o3_plan =
        pass::oir_vectorize::RVVCostModel(rvv_profile(), o3_options).choose(result);
    REQUIRE(gated_o3_plan.profitable);
    REQUIRE(gated_o3_plan.choice.interleave == 1);
    REQUIRE(gated_o3_plan.choice.interleave_capability_gate.find(
                "INTERLEAVE_FACTOR_2_RECIPE_UNAVAILABLE") != std::string::npos);

    auto o3_plan = pass::oir_vectorize::RVVCostModel(rvv_profile(), o3_options)
                       .choose(result, true);
    REQUIRE(o3_plan.profitable);
    if (o3_plan.choice.interleave != 2) {
        throw std::runtime_error("eligible factor-two cost not selected: gate=" +
                                 o3_plan.choice.interleave_capability_gate +
                                 ", cost=" +
                                 std::to_string(o3_plan.choice.estimated_vector_cost));
    }
    REQUIRE(o3_plan.choice.interleave_capability_gate.empty());
    REQUIRE(o3_plan.choice.interleave_overlap_credit > 0);
}

void test_unknown_pointer_alias_is_never_assumed_away() {
    LoopFixture fixture(true);
    oir::OIRAliasAnalysis aa;
    pass::oir_vectorize::LoopVectorizationLegality legality;
    auto result = legality.analyze(*fixture.function, *fixture.loop, *fixture.loops,
                                   *fixture.scev, aa);
    REQUIRE(!result.legal);
    REQUIRE(result.code == pass::oir_vectorize::RemarkCode::RejectAlias);
    REQUIRE(result.memory.requires_runtime_alias_check);
}

void test_potential_integer_trap_is_rejected() {
    LoopFixture fixture(false, true);
    oir::OIRAliasAnalysis aa;
    pass::oir_vectorize::LoopVectorizationLegality legality;
    auto result = legality.analyze(*fixture.function, *fixture.loop, *fixture.loops,
                                   *fixture.scev, aa);
    REQUIRE(!result.legal);
    REQUIRE(result.code == pass::oir_vectorize::RemarkCode::RejectPotentialTrap);
}

void test_no_vector_target_is_rejected_by_cost_model() {
    LoopFixture fixture(false);
    oir::OIRAliasAnalysis aa;
    pass::oir_vectorize::LoopVectorizationLegality legality;
    auto result = legality.analyze(*fixture.function, *fixture.loop, *fixture.loops,
                                   *fixture.scev, aa);
    REQUIRE(result.legal);
    target::TargetProfile scalar;
    std::string error;
    REQUIRE(target::finalize_target_profile(scalar, error));
    pass::oir_vectorize::RVVCostModel cost(scalar);
    auto plan = cost.choose(result);
    REQUIRE(!plan.profitable);
    REQUIRE(plan.code == pass::oir_vectorize::RemarkCode::RejectTargetFeature);
}

void test_verified_vla_transformation_uses_actual_vl() {
    LoopFixture fixture(false);
    pass::oir_vectorize::RemarkLog remarks;
    pass::oir_vectorize::LoopVectorizerOptions options;
    options.force = true;
    auto transformed = pass::oir_vectorize::LoopVectorizer(options).run(
        fixture.module, rvv_profile(), remarks);
    REQUIRE(transformed.success);
    if (!transformed.changed) {
        std::string why = "LV1 did not transform";
        for (const auto &remark : remarks.remarks()) {
            why += ": " + std::string(pass::oir_vectorize::remark_code_name(remark.code)) +
                   " " + remark.explanation;
        }
        throw std::runtime_error(why);
    }
    REQUIRE(transformed.changed);
    REQUIRE(transformed.loops_vectorized == 1);
    REQUIRE(!remarks.empty());
    REQUIRE(remarks.remarks().back().succeeded());

    const oir::SetVLInst *setvl = nullptr;
    unsigned vp_memory = 0;
    for (const auto &block : fixture.function->blocks()) {
        for (const auto &instruction : block->instructions()) {
            if (const auto *candidate =
                    dynamic_cast<const oir::SetVLInst *>(instruction.get())) {
                setvl = candidate;
            }
            vp_memory += dynamic_cast<const oir::VPLoadInst *>(instruction.get()) != nullptr;
            vp_memory += dynamic_cast<const oir::VPStoreInst *>(instruction.get()) != nullptr;
        }
    }
    REQUIRE(setvl != nullptr);
    REQUIRE(setvl->vector_type()->element_count().is_scalable());
    REQUIRE(vp_memory == 2);
    bool used_as_evl = false;
    bool used_by_add = false;
    bool used_by_sub = false;
    for (const auto &use : setvl->uses()) {
        if (const auto *vp = dynamic_cast<const oir::VPInstruction *>(use.user)) {
            used_as_evl |= vp->evl() == setvl;
        }
        if (const auto *binary = dynamic_cast<const oir::BinaryInst *>(use.user)) {
            used_by_add |= binary->op() == oir::Instruction::OpID::Add;
            used_by_sub |= binary->op() == oir::Instruction::OpID::Sub;
        }
    }
    REQUIRE(used_as_evl);
    REQUIRE(used_by_add);
    REQUIRE(used_by_sub);
    std::string error;
    REQUIRE(fixture.module.verify(&error));
}

} // namespace

int main() {
    const std::pair<const char *, void (*)()> tests[] = {
        {"distinct_globals_are_legal_and_costed", test_distinct_globals_are_legal_and_costed},
        {"unknown_pointer_alias_is_never_assumed_away", test_unknown_pointer_alias_is_never_assumed_away},
        {"potential_integer_trap_is_rejected", test_potential_integer_trap_is_rejected},
        {"no_vector_target_is_rejected_by_cost_model", test_no_vector_target_is_rejected_by_cost_model},
        {"verified_vla_transformation_uses_actual_vl", test_verified_vla_transformation_uses_actual_vl},
    };
    for (const auto &[name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
"""


def find_cxx() -> str | None:
    configured = os.environ.get("CXX")
    if configured:
        return configured
    for candidate in ("c++", "g++", "clang++"):
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    return None


def main() -> int:
    cxx = find_cxx()
    if cxx is None:
        print("error: no C++ compiler found in PATH", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="vector-analysis-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "vectorization_analysis_tests.cpp"
        binary = tmp_dir / "vectorization_analysis_tests"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        sources = [
            source,
            ROOT / "src/oir/OIR.cpp",
            ROOT / "src/oir/OIRAnalysis.cpp",
            ROOT / "src/oir/OIRCFGUtils.cpp",
            ROOT / "src/oir/OIRDataLayout.cpp",
            ROOT / "src/oir/OIRParser.cpp",
            ROOT / "src/builtin/BuiltinRegistry.cpp",
            ROOT / "src/pass/oir/OIRVectorization.cpp",
            ROOT / "src/pass/oir/RVVTargetCostModel.cpp",
            ROOT / "src/pass/oir/OIRVectorizationRemark.cpp",
            ROOT / "src/target/TargetMachine.cpp",
        ]
        compile_proc = subprocess.run(
            [
                cxx,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include"),
                *map(str, sources),
                "-o",
                str(binary),
            ],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if compile_proc.returncode != 0:
            print(compile_proc.stdout, end="")
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
        print(run_proc.stdout, end="")
        print(run_proc.stderr, end="", file=sys.stderr)
        return run_proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
