#!/usr/bin/env python3

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]


HARNESS = r"""
#include "pass/CostModel.h"
#include "pass/oir/RVVTargetCostModel.h"
#include "target/TargetMachine.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw std::runtime_error(std::string("requirement failed: ") + #condition);        \
        }                                                                                       \
    } while (false)

using pass::oir_vectorize::RVVCostRejectReason;
using pass::oir_vectorize::RVVLoopCostInput;
using pass::oir_vectorize::RVVMemoryAccessKind;
using pass::oir_vectorize::RVVMemoryCostOp;
using pass::oir_vectorize::RVVTargetCostModel;
using pass::oir_vectorize::RVVTargetCostModelOptions;

target::TargetProfile rvv_profile(std::string tune = "generic-rvv") {
    target::TargetProfile profile;
    profile.cpu = "generic-rvv";
    profile.cpu_explicit = true;
    profile.tune = std::move(tune);
    profile.tune_explicit = true;
    std::string error;
    REQUIRE(target::finalize_target_profile(profile, error));
    REQUIRE(profile.has_vector());
    return profile;
}

RVVLoopCostInput base_input(std::uint64_t trip_count = 1024) {
    RVVLoopCostInput input;
    input.trip_count_known = true;
    input.trip_count = trip_count;
    input.scalar_alu_operations = 2;
    input.vector_alu_operations = 2;
    input.live_vector_values = 4;
    input.memory_operations = {
        RVVMemoryCostOp{RVVMemoryAccessKind::UnitStride, false, 1},
        RVVMemoryCostOp{RVVMemoryAccessKind::UnitStride, true, 1},
    };
    return input;
}

void test_strict_cpu_and_tune_registry() {
    std::string error;
    target::TargetProfile generic_rvv;
    generic_rvv.cpu = "GeNeRiC-RvV";
    generic_rvv.cpu_explicit = true;
    REQUIRE(target::finalize_target_profile(generic_rvv, error));
    REQUIRE(generic_rvv.cpu == "generic-rvv");
    REQUIRE(generic_rvv.tune == "generic-rvv");
    REQUIRE(generic_rvv.march == "rv64gcv");
    REQUIRE(generic_rvv.has_vector());
    REQUIRE(generic_rvv.tuning.vector_load_cost == 3);

    target::TargetProfile explicit_arch;
    explicit_arch.cpu = "generic-rvv";
    explicit_arch.cpu_explicit = true;
    explicit_arch.march = "rv64gc";
    explicit_arch.march_explicit = true;
    explicit_arch.tune = "generic-rv64";
    explicit_arch.tune_explicit = true;
    error.clear();
    REQUIRE(target::finalize_target_profile(explicit_arch, error));
    REQUIRE(explicit_arch.march == "rv64gc");
    REQUIRE(!explicit_arch.has_vector());
    REQUIRE(explicit_arch.tuning.vector_load_cost == 5);

    target::TargetProfile bad_cpu;
    bad_cpu.cpu = "made-up-core";
    bad_cpu.cpu_explicit = true;
    error.clear();
    REQUIRE(!target::finalize_target_profile(bad_cpu, error));
    REQUIRE(error.find("unsupported -mcpu") != std::string::npos);
    REQUIRE(error.find("generic-rv64, generic-rvv") != std::string::npos);

    target::TargetProfile bad_tune;
    bad_tune.tune = "made-up-core";
    bad_tune.tune_explicit = true;
    error.clear();
    REQUIRE(!target::finalize_target_profile(bad_tune, error));
    REQUIRE(error.find("unsupported -mtune") != std::string::npos);
}

void test_target_profile_maps_to_shared_cost_report() {
    const auto profile = rvv_profile();
    const auto mapped = pass::cost_model::target_profile_for(profile);
    REQUIRE(mapped.arch == "rv64gcv");
    REQUIRE(mapped.cpu == "generic-rvv");
    REQUIRE(mapped.tune == "generic-rvv");
    REQUIRE(mapped.load == profile.tuning.scalar_load_cost);
    REQUIRE(mapped.rvv_unit_load == profile.tuning.vector_load_cost);
    REQUIRE(mapped.rvv_strided_load == profile.tuning.vector_strided_load_cost);
    REQUIRE(mapped.rvv_indexed_store == profile.tuning.vector_indexed_store_cost);
    REQUIRE(mapped.rvv_index_setup == profile.tuning.vector_index_setup_cost);
    REQUIRE(mapped.rvv_segment_field == profile.tuning.vector_segment_field_cost);
    REQUIRE(mapped.rvv_reduction == profile.tuning.vector_reduction_cost);
    REQUIRE(mapped.rvv_code_size == profile.tuning.code_size_cost);
    REQUIRE(mapped.rvv_available_registers ==
            static_cast<int>(profile.tuning.available_vector_registers));
}

std::uint64_t forced_cost(RVVLoopCostInput input) {
    RVVTargetCostModelOptions options;
    options.force = true;
    const auto decision = RVVTargetCostModel(rvv_profile(), options).choose(input);
    REQUIRE(decision.profitable);
    REQUIRE(decision.selected.legal);
    return decision.selected.total_cost;
}

void test_memory_kinds_have_distinct_costs() {
    auto unit = base_input();
    unit.memory_operations = {{RVVMemoryAccessKind::UnitStride, false, 1}};
    auto strided = unit;
    strided.memory_operations[0].kind = RVVMemoryAccessKind::Strided;
    auto indexed = unit;
    indexed.memory_operations[0].kind = RVVMemoryAccessKind::Indexed;
    indexed.distinct_index_vectors = 1;
    auto segment = unit;
    segment.memory_operations[0] = {RVVMemoryAccessKind::Segment, false, 4};

    const auto unit_cost = forced_cost(unit);
    const auto strided_cost = forced_cost(strided);
    const auto indexed_cost = forced_cost(indexed);
    const auto segment_cost = forced_cost(segment);
    REQUIRE(unit_cost < strided_cost);
    REQUIRE(strided_cost < indexed_cost);
    REQUIRE(unit_cost < segment_cost);
}

void test_mask_reduction_pressure_and_spills_are_charged() {
    auto plain = base_input();
    plain.needs_tail_mask = false;
    const auto plain_cost = forced_cost(plain);

    auto masked = plain;
    masked.needs_tail_mask = true;
    masked.mask_operations = 2;
    REQUIRE(plain_cost < forced_cost(masked));

    auto reduced = masked;
    reduced.reduction_operations = 2;
    REQUIRE(forced_cost(masked) < forced_cost(reduced));

    auto pressured_profile = rvv_profile();
    pressured_profile.tuning.available_vector_registers = 5;
    auto pressured = reduced;
    pressured.live_vector_values = 12;
    RVVTargetCostModelOptions forced;
    forced.force = true;
    const auto decision = RVVTargetCostModel(pressured_profile, forced).choose(pressured);
    REQUIRE(decision.profitable);
    REQUIRE(decision.selected.predicted_spill_registers > 0);
    REQUIRE(decision.selected.estimated_vector_registers >
            pressured_profile.tuning.available_vector_registers);
    REQUIRE(decision.selected.dynamic_cost > forced_cost(reduced));
}

void test_code_size_short_trip_break_even_and_force_boundary() {
    auto short_loop = base_input(1);
    const auto ordinary = RVVTargetCostModel(rvv_profile()).choose(short_loop);
    REQUIRE(!ordinary.profitable);
    REQUIRE(ordinary.reject_reason == RVVCostRejectReason::ShortTrip ||
            ordinary.reject_reason == RVVCostRejectReason::NotProfitable);
    REQUIRE(ordinary.selected.code_size_cost > 0);
    REQUIRE(ordinary.selected.break_even_trip_count == 0 ||
            ordinary.selected.break_even_trip_count > short_loop.trip_count);

    RVVTargetCostModelOptions o3;
    o3.explore_interleave = true;
    short_loop.interleave_factor_two_legal = true;
    const auto short_o3 = RVVTargetCostModel(rvv_profile(), o3).choose(short_loop);
    REQUIRE(!short_o3.profitable);
    REQUIRE(short_o3.selected.interleave == 1);

    RVVTargetCostModelOptions forced;
    forced.force = true;
    const auto forced_short = RVVTargetCostModel(rvv_profile(), forced).choose(short_loop);
    REQUIRE(forced_short.profitable);
    REQUIRE(forced_short.profitability_bypassed);

    target::TargetProfile scalar;
    std::string error;
    REQUIRE(target::finalize_target_profile(scalar, error));
    const auto forced_scalar = RVVTargetCostModel(scalar, forced).choose(short_loop);
    REQUIRE(!forced_scalar.profitable);
    REQUIRE(forced_scalar.reject_reason == RVVCostRejectReason::TargetUnsupported);

    auto malformed = short_loop;
    malformed.memory_operations = {{RVVMemoryAccessKind::Segment, false, 1}};
    const auto forced_malformed = RVVTargetCostModel(rvv_profile(), forced).choose(malformed);
    REQUIRE(!forced_malformed.profitable);
    REQUIRE(forced_malformed.reject_reason == RVVCostRejectReason::InvalidInput);
}

void test_interleave_factor_two_legality_and_cost_selection() {
    RVVTargetCostModelOptions options;
    options.force = true;
    options.explore_interleave = true;
    options.requested_max_interleave = 4;
    const auto gated = RVVTargetCostModel(rvv_profile(), options).choose(base_input());
    REQUIRE(gated.profitable);
    REQUIRE(gated.interleave_capability_gate.find("INTERLEAVE_FACTOR_2_RECIPE_UNAVAILABLE") !=
            std::string::npos);
    REQUIRE(gated.selected.interleave == 1);
    REQUIRE(std::all_of(gated.candidates.begin(), gated.candidates.end(),
                        [](const auto &candidate) { return candidate.interleave == 1; }));

    auto legal = base_input(4096);
    legal.interleave_factor_two_legal = true;
    const auto selected = RVVTargetCostModel(rvv_profile(), options).choose(legal);
    REQUIRE(selected.profitable);
    REQUIRE(selected.interleave_capability_gate.empty());
    REQUIRE(selected.selected.interleave == 2);
    const auto factor_one = std::find_if(
        selected.candidates.begin(), selected.candidates.end(), [&](const auto &candidate) {
            return candidate.legal && candidate.lmul == selected.selected.lmul &&
                   candidate.interleave == 1;
        });
    REQUIRE(factor_one != selected.candidates.end());
    REQUIRE(selected.selected.vector_iterations <= factor_one->vector_iterations);
    REQUIRE(selected.selected.estimated_code_bytes > factor_one->estimated_code_bytes);
    REQUIRE(selected.selected.estimated_vector_registers >
            factor_one->estimated_vector_registers);

    auto target_limited_profile = rvv_profile();
    target_limited_profile.tuning.maximum_interleave_factor = 1;
    const auto target_limited =
        RVVTargetCostModel(target_limited_profile, options).choose(legal);
    REQUIRE(target_limited.selected.interleave == 1);
    REQUIRE(target_limited.interleave_capability_gate.find(
                "INTERLEAVE_FACTOR_2_TARGET_UNAVAILABLE") != std::string::npos);

    auto default_o3 = base_input(64);
    default_o3.interleave_factor_two_legal = true;
    const auto conservative =
        RVVTargetCostModel(rvv_profile("generic-rv64"), options).choose(default_o3);
    REQUIRE(conservative.profitable);
    if (conservative.selected.interleave != 2) {
        const auto factor_two = std::min_element(
            conservative.candidates.begin(), conservative.candidates.end(),
            [](const auto &lhs, const auto &rhs) {
                const auto lhs_cost = lhs.legal && lhs.interleave == 2
                                          ? lhs.total_cost
                                          : std::numeric_limits<std::uint64_t>::max();
                const auto rhs_cost = rhs.legal && rhs.interleave == 2
                                          ? rhs.total_cost
                                          : std::numeric_limits<std::uint64_t>::max();
                return lhs_cost < rhs_cost;
            });
        throw std::runtime_error(
            "generic-rv64 expected factor2, selected cost=" +
            std::to_string(conservative.selected.total_cost) + ", factor2 cost=" +
            std::to_string(factor_two->total_cost));
    }

    auto pressured_profile = rvv_profile();
    pressured_profile.tuning.available_vector_registers = 4;
    legal.live_vector_values = 3;
    const auto pressured = RVVTargetCostModel(pressured_profile, options).choose(legal);
    REQUIRE(pressured.profitable);
    REQUIRE(pressured.selected.interleave == 1);
}

int main() {
    const std::pair<const char *, void (*)()> tests[] = {
        {"strict_cpu_and_tune_registry", test_strict_cpu_and_tune_registry},
        {"target_profile_maps_to_shared_cost_report",
         test_target_profile_maps_to_shared_cost_report},
        {"memory_kinds_have_distinct_costs", test_memory_kinds_have_distinct_costs},
        {"mask_reduction_pressure_and_spills_are_charged",
         test_mask_reduction_pressure_and_spills_are_charged},
        {"code_size_short_trip_break_even_and_force_boundary",
         test_code_size_short_trip_break_even_and_force_boundary},
        {"interleave_factor_two_legality_and_cost_selection",
         test_interleave_factor_two_legality_and_cost_selection},
    };
    try {
        for (const auto &[name, test] : tests) {
            test();
            std::cout << "PASS " << name << '\n';
        }
        RVVTargetCostModelOptions options;
        options.force = true;
        options.explore_interleave = true;
        auto input = base_input(4096);
        input.interleave_factor_two_legal = true;
        const auto decision = RVVTargetCostModel(rvv_profile(), options).choose(input);
        std::cout << "JSON_BEGIN\n";
        pass::oir_vectorize::print_rvv_cost_decision_json(decision, std::cout);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL rvv_cost_model: " << error.what() << '\n';
        return 1;
    }
}
"""


def run(argv: list[str], *, timeout: float = 30.0) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        argv,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
        check=False,
    )


def main() -> int:
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++")
    if cxx is None:
        print("error: no host C++ compiler found", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="yoolang-rvv-cost-") as tmp:
        source = Path(tmp) / "rvv_cost_model_tests.cpp"
        binary = Path(tmp) / "rvv_cost_model_tests"
        source.write_text(textwrap.dedent(HARNESS), encoding="utf-8")
        compile_result = run(
            [
                cxx,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include"),
                str(source),
                str(ROOT / "src/target/TargetMachine.cpp"),
                str(ROOT / "src/pass/CostModel.cpp"),
                str(ROOT / "src/pass/oir/RVVTargetCostModel.cpp"),
                "-o",
                str(binary),
            ]
        )
        if compile_result.returncode != 0:
            print(compile_result.stdout, end="")
            print(compile_result.stderr, end="", file=sys.stderr)
            return 1
        result = run([str(binary)])
        if result.returncode != 0:
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            return result.returncode
        marker = "JSON_BEGIN\n"
        if marker not in result.stdout:
            print("FAIL rvv_cost_model: missing JSON marker", file=sys.stderr)
            return 1
        pass_output, json_output = result.stdout.split(marker, 1)
        document = json.loads(json_output)
        if document.get("selected_interleave") != 2:
            print("FAIL rvv_cost_model: JSON did not select factor two", file=sys.stderr)
            return 1
        if document.get("interleave_capability_gate"):
            print("FAIL rvv_cost_model: selected factor two retained a capability gate", file=sys.stderr)
            return 1
        candidates = document.get("candidates")
        if not isinstance(candidates, list) or len(candidates) != 10:
            print("FAIL rvv_cost_model: JSON candidate matrix is incomplete", file=sys.stderr)
            return 1
        if not any(
            candidate.get("interleave") == 2
            and candidate.get("interleave_overlap_credit", 0) > 0
            for candidate in candidates
        ):
            print("FAIL rvv_cost_model: JSON lost factor-two overlap evidence", file=sys.stderr)
            return 1
        print(pass_output, end="")
        print("PASS rvv_cost_model_json_schema")

        compiler = Path(
            os.environ.get(
                "YOOLANG_COMPILER",
                ROOT / "build/linux/x86_64/release/compiler",
            )
        )
        if not compiler.exists():
            print(f"FAIL rvv_cost_model: compiler not found: {compiler}", file=sys.stderr)
            return 1
        sysy_source = Path(tmp) / "cost_model_cli.sy"
        sysy_source.write_text("int main() { return 0; }\n", encoding="utf-8")
        cli_result = run(
            [
                str(compiler),
                "--emit-cost-model=json",
                "-O1",
                "-mcpu=generic-rvv",
                str(sysy_source),
            ],
            timeout=60.0,
        )
        if cli_result.returncode != 0:
            print(cli_result.stdout, end="")
            print(cli_result.stderr, end="", file=sys.stderr)
            return cli_result.returncode
        cli_document = json.loads(cli_result.stdout)
        if (
            cli_document.get("target") != "rv64gcv/lp64d"
            or cli_document.get("cpu") != "generic-rvv"
            or cli_document.get("tune") != "generic-rvv"
        ):
            print("FAIL rvv_cost_model: CLI target/tune mapping is stale", file=sys.stderr)
            return 1
        rvv_costs = cli_document.get("rvv_costs", {})
        if (
            rvv_costs.get("unit_load") != 3
            or rvv_costs.get("strided_load") != 5
            or rvv_costs.get("indexed_load") != 8
            or rvv_costs.get("index_setup") != 2
            or rvv_costs.get("reduction") != 4
            or rvv_costs.get("maximum_lmul") != 8
            or rvv_costs.get("maximum_interleave") != 2
        ):
            print("FAIL rvv_cost_model: CLI RVV tuning costs are stale", file=sys.stderr)
            return 1
        print("PASS rvv_cost_model_cli_target_mapping")
    return 0


if __name__ == "__main__":
    sys.exit(main())
