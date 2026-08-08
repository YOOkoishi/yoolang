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
#include "target/RVVFixedVectorLegalization.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw std::runtime_error(std::string(__FILE__) + ":" +                           \
                                     std::to_string(__LINE__) +                                \
                                     ": requirement failed: " #condition);                   \
        }                                                                                       \
    } while (false)

using target::RVVFixedLMUL;
using target::RVVFixedStorageOffsetUnit;
using target::RVVFixedVectorElementKind;
using target::RVVFixedVectorLegalizationPlan;
using target::RVVFixedVectorShape;

target::TargetProfile vector_profile(const std::string &march) {
    target::TargetProfile profile;
    profile.march = march;
    std::string error;
    if (!target::finalize_target_profile(profile, error)) {
        throw std::runtime_error("cannot construct test profile: " + error);
    }
    return profile;
}

target::RVVFixedVectorLegalizationResult
plan(const target::TargetProfile &profile, RVVFixedVectorElementKind kind,
     std::uint64_t lanes, unsigned sew = 32) {
    return target::plan_rvv_fixed_vector_legalization({kind, sew, lanes}, profile);
}

std::vector<std::uint64_t> lane_counts(const RVVFixedVectorLegalizationPlan &plan) {
    std::vector<std::uint64_t> result;
    for (const auto &piece : plan.pieces) result.push_back(piece.lane_count);
    return result;
}

std::vector<RVVFixedLMUL> lmuls(const RVVFixedVectorLegalizationPlan &plan) {
    std::vector<RVVFixedLMUL> result;
    for (const auto &piece : plan.pieces) result.push_back(piece.lmul);
    return result;
}

void verify_reconstruction(const RVVFixedVectorLegalizationPlan &plan) {
    std::uint64_t expected_lane_base = 0;
    for (const auto &piece : plan.pieces) {
        REQUIRE(piece.lane_base == expected_lane_base);
        REQUIRE(piece.lane_count != 0);
        REQUIRE(piece.lane_count <= piece.minimum_capacity_lanes);
        REQUIRE(piece.register_group_width ==
                (piece.lmul == RVVFixedLMUL::M2 ? 2U
                 : piece.lmul == RVVFixedLMUL::M4 ? 4U
                 : piece.lmul == RVVFixedLMUL::M8 ? 8U
                                                  : 1U));
        const auto capacity = target::rvv_fixed_vector_minimum_capacity(
            plan.minimum_vlen_bits, plan.shape.sew_bits, piece.lmul);
        REQUIRE(capacity.valid);
        REQUIRE(capacity.minimum_capacity_lanes == piece.minimum_capacity_lanes);
        REQUIRE(capacity.register_group_width == piece.register_group_width);
        if (plan.shape.element_kind == RVVFixedVectorElementKind::Mask) {
            REQUIRE(piece.storage_offset_unit == RVVFixedStorageOffsetUnit::Bits);
            REQUIRE(piece.storage_offset == piece.lane_base);
        } else {
            REQUIRE(piece.storage_offset_unit == RVVFixedStorageOffsetUnit::Bytes);
            REQUIRE(piece.storage_offset ==
                    piece.lane_base * static_cast<std::uint64_t>(plan.shape.sew_bits / 8U));
        }
        expected_lane_base += piece.lane_count;
    }
    REQUIRE(expected_lane_base == plan.shape.logical_lanes);
}

void test_vlen128_required_boundaries() {
    const auto profile = vector_profile("rv64gcv_zvl128b");
    struct Case {
        std::uint64_t lanes;
        std::vector<std::uint64_t> counts;
        std::vector<RVVFixedLMUL> lmuls;
    };
    const std::array<Case, 9> cases = {{
        {1, {1}, {RVVFixedLMUL::MF2}},
        {3, {3}, {RVVFixedLMUL::M1}},
        {7, {7}, {RVVFixedLMUL::M2}},
        {31, {31}, {RVVFixedLMUL::M8}},
        {32, {32}, {RVVFixedLMUL::M8}},
        {33, {32, 1}, {RVVFixedLMUL::M8, RVVFixedLMUL::MF2}},
        {63, {32, 31}, {RVVFixedLMUL::M8, RVVFixedLMUL::M8}},
        {65, {32, 32, 1},
         {RVVFixedLMUL::M8, RVVFixedLMUL::M8, RVVFixedLMUL::MF2}},
        {97, {32, 32, 32, 1},
         {RVVFixedLMUL::M8, RVVFixedLMUL::M8, RVVFixedLMUL::M8,
          RVVFixedLMUL::MF2}},
    }};
    for (const auto &entry : cases) {
        const auto result = plan(profile, RVVFixedVectorElementKind::Integer, entry.lanes);
        REQUIRE(result.valid);
        REQUIRE(result.error.empty());
        REQUIRE(lane_counts(result.plan) == entry.counts);
        REQUIRE(lmuls(result.plan) == entry.lmuls);
        REQUIRE(result.plan.storage_size_bits == entry.lanes * 32U);
        REQUIRE(result.plan.storage_size_bytes == entry.lanes * 4U);
        REQUIRE(!result.plan.zero_fill_unused_high_bits);
        verify_reconstruction(result.plan);
    }
    std::cout << "PASS fixed_chunk_vlen128_required_boundaries\n";
}

void test_vlen256_boundaries_and_minimal_lmul() {
    const auto profile = vector_profile("rv64gcv_zvl256b");
    const std::array<std::pair<std::uint64_t, std::vector<std::uint64_t>>, 9> cases = {{
        {1, {1}}, {3, {3}}, {7, {7}}, {31, {31}}, {32, {32}},
        {33, {33}}, {63, {63}}, {65, {64, 1}}, {129, {64, 64, 1}},
    }};
    const std::array<RVVFixedLMUL, 5> ordered_lmuls = {
        RVVFixedLMUL::MF2, RVVFixedLMUL::M1, RVVFixedLMUL::M2,
        RVVFixedLMUL::M4, RVVFixedLMUL::M8,
    };
    for (const auto &entry : cases) {
        const auto result = plan(profile, RVVFixedVectorElementKind::Integer, entry.first);
        REQUIRE(result.valid);
        REQUIRE(lane_counts(result.plan) == entry.second);
        verify_reconstruction(result.plan);
        for (const auto &piece : result.plan.pieces) {
            for (auto lmul : ordered_lmuls) {
                if (lmul == piece.lmul) break;
                const auto smaller = target::rvv_fixed_vector_minimum_capacity(
                    profile.minimum_vlen_bits, 32, lmul);
                REQUIRE(!smaller.valid || smaller.minimum_capacity_lanes < piece.lane_count);
            }
        }
    }
    std::cout << "PASS fixed_chunk_vlen256_minimal_lmul\n";
}

void test_mask_packed_layout_matches_data_chunks() {
    const auto profile = vector_profile("rv64gcv_zvl128b");
    for (std::uint64_t lanes : {1U, 3U, 7U, 31U, 32U, 33U, 63U, 65U}) {
        const auto data = plan(profile, RVVFixedVectorElementKind::Integer, lanes);
        const auto mask = plan(profile, RVVFixedVectorElementKind::Mask, lanes);
        REQUIRE(data.valid && mask.valid);
        REQUIRE(data.plan.pieces.size() == mask.plan.pieces.size());
        REQUIRE(mask.plan.storage_size_bits == lanes);
        REQUIRE(mask.plan.storage_size_bytes == lanes / 8U + (lanes % 8U != 0 ? 1U : 0U));
        REQUIRE(mask.plan.final_byte_unused_high_bits == (8U - lanes % 8U) % 8U);
        REQUIRE(mask.plan.zero_fill_unused_high_bits);
        for (std::size_t index = 0; index < data.plan.pieces.size(); ++index) {
            REQUIRE(data.plan.pieces[index].lane_base == mask.plan.pieces[index].lane_base);
            REQUIRE(data.plan.pieces[index].lane_count == mask.plan.pieces[index].lane_count);
            REQUIRE(data.plan.pieces[index].lmul == mask.plan.pieces[index].lmul);
            REQUIRE(data.plan.pieces[index].minimum_capacity_lanes ==
                    mask.plan.pieces[index].minimum_capacity_lanes);
        }
        verify_reconstruction(mask.plan);
    }
    const auto mask65 = plan(profile, RVVFixedVectorElementKind::Mask, 65);
    REQUIRE(mask65.plan.storage_size_bytes == 9);
    REQUIRE(mask65.plan.final_byte_unused_high_bits == 7);
    REQUIRE(mask65.plan.pieces[1].storage_offset == 32);
    REQUIRE(mask65.plan.pieces[2].storage_offset == 64);
    std::cout << "PASS fixed_chunk_mask_stable_packed_layout\n";
}

void test_float_layout_matches_integer() {
    const auto profile = vector_profile("rv64gcv_zvl128b");
    for (std::uint64_t lanes : {1U, 3U, 7U, 31U, 33U, 63U, 65U}) {
        const auto integer = plan(profile, RVVFixedVectorElementKind::Integer, lanes);
        const auto floating = plan(profile, RVVFixedVectorElementKind::FloatingPoint, lanes);
        REQUIRE(integer.valid && floating.valid);
        REQUIRE(lane_counts(integer.plan) == lane_counts(floating.plan));
        REQUIRE(lmuls(integer.plan) == lmuls(floating.plan));
        REQUIRE(integer.plan.storage_size_bits == floating.plan.storage_size_bits);
        REQUIRE(integer.plan.storage_size_bytes == floating.plan.storage_size_bytes);
        verify_reconstruction(floating.plan);
    }
    std::cout << "PASS fixed_chunk_float_e32_layout\n";
}

bool same_plan(const RVVFixedVectorLegalizationPlan &lhs,
               const RVVFixedVectorLegalizationPlan &rhs) {
    if (lhs.shape.element_kind != rhs.shape.element_kind ||
        lhs.shape.sew_bits != rhs.shape.sew_bits ||
        lhs.shape.logical_lanes != rhs.shape.logical_lanes ||
        lhs.minimum_vlen_bits != rhs.minimum_vlen_bits ||
        lhs.storage_size_bits != rhs.storage_size_bits ||
        lhs.storage_size_bytes != rhs.storage_size_bytes ||
        lhs.final_byte_unused_high_bits != rhs.final_byte_unused_high_bits ||
        lhs.zero_fill_unused_high_bits != rhs.zero_fill_unused_high_bits ||
        lhs.pieces.size() != rhs.pieces.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.pieces.size(); ++index) {
        const auto &a = lhs.pieces[index];
        const auto &b = rhs.pieces[index];
        if (a.lane_base != b.lane_base || a.lane_count != b.lane_count ||
            a.storage_offset != b.storage_offset ||
            a.storage_offset_unit != b.storage_offset_unit || a.lmul != b.lmul ||
            a.register_group_width != b.register_group_width ||
            a.minimum_capacity_lanes != b.minimum_capacity_lanes) {
            return false;
        }
    }
    return true;
}

void test_large_and_deterministic() {
    const auto profile = vector_profile("rv64gcv_zvl128b");
    constexpr std::uint64_t lanes = 1000003;
    const auto first = plan(profile, RVVFixedVectorElementKind::Integer, lanes);
    const auto second = plan(profile, RVVFixedVectorElementKind::Integer, lanes);
    REQUIRE(first.valid && second.valid);
    REQUIRE(first.plan.pieces.size() == 31251);
    REQUIRE(first.plan.pieces.front().lane_count == 32);
    REQUIRE(first.plan.pieces.back().lane_count == 3);
    REQUIRE(first.plan.pieces.back().lmul == RVVFixedLMUL::M1);
    REQUIRE(same_plan(first.plan, second.plan));
    verify_reconstruction(first.plan);
    std::cout << "PASS fixed_chunk_large_deterministic\n";
}

void test_fail_closed_inputs_and_overflow() {
    const auto profile = vector_profile("rv64gcv_zvl128b");
    const auto scalar = vector_profile("rv64gc");
    auto failure = plan(profile, RVVFixedVectorElementKind::Integer, 0);
    REQUIRE(!failure.valid && failure.error.find("at least one") != std::string::npos);
    failure = plan(profile, RVVFixedVectorElementKind::Integer, 4, 24);
    REQUIRE(!failure.valid && failure.error.find("SEW") != std::string::npos);
    failure = target::plan_rvv_fixed_vector_legalization(
        {static_cast<RVVFixedVectorElementKind>(99), 32, 4}, profile);
    REQUIRE(!failure.valid && failure.error.find("element kind") != std::string::npos);
    failure = plan(scalar, RVVFixedVectorElementKind::Integer, 4);
    REQUIRE(!failure.valid && failure.error.find("vector-enabled") != std::string::npos);

    for (unsigned invalid_vlen : {0U, 16U, 96U, 131072U}) {
        auto corrupt = profile;
        corrupt.minimum_vlen_bits = invalid_vlen;
        failure = plan(corrupt, RVVFixedVectorElementKind::Integer, 4);
        REQUIRE(!failure.valid && failure.error.find("minimum VLEN") != std::string::npos);
    }
    auto inconsistent = profile;
    inconsistent.minimum_vlen_bits = 64;
    failure = plan(inconsistent, RVVFixedVectorElementKind::Integer, 4);
    REQUIRE(!failure.valid && failure.error.find("incompatible") != std::string::npos);

    failure = plan(profile, RVVFixedVectorElementKind::Integer,
                   std::numeric_limits<std::uint64_t>::max());
    REQUIRE(!failure.valid && failure.error.find("overflow") != std::string::npos);
    failure = plan(profile, RVVFixedVectorElementKind::Mask,
                   std::numeric_limits<std::uint64_t>::max());
    REQUIRE(!failure.valid && failure.error.find("planner capacity") != std::string::npos);

    auto capacity = target::rvv_fixed_vector_minimum_capacity(
        128, 32, static_cast<RVVFixedLMUL>(99));
    REQUIRE(!capacity.valid && capacity.error.find("LMUL") != std::string::npos);
    capacity = target::rvv_fixed_vector_minimum_capacity(96, 32, RVVFixedLMUL::M1);
    REQUIRE(!capacity.valid && capacity.error.find("minimum VLEN") != std::string::npos);
    capacity = target::rvv_fixed_vector_minimum_capacity(32, 64, RVVFixedLMUL::MF2);
    REQUIRE(!capacity.valid && capacity.error.find("zero lane capacity") != std::string::npos);
    REQUIRE(std::string(target::rvv_fixed_lmul_name(static_cast<RVVFixedLMUL>(99))) ==
            "unknown");

    const auto zve32 = vector_profile("rv64gc_zve32x");
    failure = plan(zve32, RVVFixedVectorElementKind::Integer, 4, 64);
    REQUIRE(!failure.valid && failure.error.find("unsupported by the target") !=
                                  std::string::npos);
    failure = plan(zve32, RVVFixedVectorElementKind::FloatingPoint, 4, 32);
    REQUIRE(!failure.valid && failure.error.find("unsupported by the target") !=
                                  std::string::npos);
    std::cout << "PASS fixed_chunk_fail_closed_inputs\n";
}

void test_all_legal_sews_have_exact_offsets() {
    const auto profile = vector_profile("rv64gcv_zvl128b");
    for (unsigned sew : {8U, 16U, 32U, 64U}) {
        const auto result = plan(profile, RVVFixedVectorElementKind::Integer, 257, sew);
        REQUIRE(result.valid);
        REQUIRE(result.plan.storage_size_bits == 257U * sew);
        REQUIRE(result.plan.storage_size_bytes == 257U * (sew / 8U));
        verify_reconstruction(result.plan);
    }
    std::cout << "PASS fixed_chunk_legal_sew_offsets\n";
}

} // namespace

int main() {
    try {
        test_vlen128_required_boundaries();
        test_vlen256_boundaries_and_minimal_lmul();
        test_mask_packed_layout_matches_data_chunks();
        test_float_layout_matches_integer();
        test_large_and_deterministic();
        test_fail_closed_inputs_and_overflow();
        test_all_legal_sews_have_exact_offsets();
    } catch (const std::exception &error) {
        std::cerr << "FAIL rvv_fixed_vector_legalization: " << error.what() << '\n';
        return 1;
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
        print("FAIL rvv_fixed_vector_legalization: no C++ compiler found", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="rvv-fixed-chunk-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "rvv_fixed_vector_legalization_tests.cpp"
        binary = tmp_dir / "rvv_fixed_vector_legalization_tests"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        compile_proc = subprocess.run(
            [
                cxx,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include"),
                str(source),
                str(ROOT / "src/target/RVVFixedVectorLegalization.cpp"),
                str(ROOT / "src/target/TargetMachine.cpp"),
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
