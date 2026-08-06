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
#include "yir/YIR.h"
#include "yir/YIRPrinter.h"
#include "yir/YIRVerifier.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::exception {
  public:
    explicit TestFailure(std::string message) : message_(std::move(message)) {}
    const char *what() const noexcept override { return message_.c_str(); }

  private:
    std::string message_;
};

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw TestFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +        \
                              ": requirement failed: " #condition);                           \
        }                                                                                       \
    } while (false)

template <typename Fn> void require_invalid_argument(Fn &&fn) {
    try {
        fn();
    } catch (const std::invalid_argument &) {
        return;
    }
    throw TestFailure("expected std::invalid_argument");
}

void test_fixed_vector_and_mask_identity() {
    for (std::uint64_t lanes : {1ULL, 3ULL, 7ULL, 31ULL}) {
        auto integers = yir::Type::get_vector(lanes, yir::Type::get_i32());
        auto same_integers = yir::Type::get_vector(lanes, yir::Type::get_i32());
        auto floats = yir::Type::get_vector(lanes, yir::Type::get_f32());
        auto mask = yir::Type::get_mask(lanes);
        REQUIRE(integers == same_integers);
        REQUIRE(integers != floats);
        REQUIRE(integers != mask);
        REQUIRE(integers->is_vector());
        REQUIRE(!integers->is_integer());
        REQUIRE(!integers->is_float());
        REQUIRE(!mask->is_vector());
        REQUIRE(mask->is_mask());
        REQUIRE(mask->element() == yir::Type::get_i1());
        REQUIRE(integers->count() == lanes);
        REQUIRE(mask->count() == lanes);
    }
    REQUIRE(yir::Type::get_vector(3, yir::Type::get_i32())->str() == "vector<3 x i32>");
    REQUIRE(yir::Type::get_mask(7)->str() == "mask<7>");
}

void test_composite_types_are_interned_structurally() {
    auto vector = yir::Type::get_vector(3, yir::Type::get_f32());
    REQUIRE(yir::Type::get_ptr(vector) == yir::Type::get_ptr(vector));
    REQUIRE(yir::Type::get_array(5, vector) == yir::Type::get_array(5, vector));
    REQUIRE(yir::Type::get_func({vector, yir::Type::get_mask(3)}, vector) ==
            yir::Type::get_func({vector, yir::Type::get_mask(3)}, vector));
    auto fixed = yir::Type::get_func({vector}, vector, false);
    auto variadic = yir::Type::get_func({vector}, vector, true);
    REQUIRE(fixed != variadic);
    REQUIRE(!fixed->is_variadic());
    REQUIRE(variadic->is_variadic());
    REQUIRE(variadic->str().find("...") != std::string::npos);
}

void test_invalid_vector_shapes_are_rejected() {
    require_invalid_argument([] { yir::Type::get_vector(0, yir::Type::get_i32()); });
    require_invalid_argument([] { yir::Type::get_mask(0); });
    require_invalid_argument([] { yir::Type::get_vector(4, yir::Type::get_i1()); });
    require_invalid_argument([] { yir::Type::get_vector(4, yir::Type::get_void()); });
    require_invalid_argument([] {
        yir::Type::get_vector(4, yir::Type::get_vector(4, yir::Type::get_i32()));
    });
}

void test_external_declaration_printing_and_verification() {
    yir::Module module;
    auto vector = yir::Type::get_vector(3, yir::Type::get_i32());
    auto mask = yir::Type::get_mask(3);
    auto *external = module.add_function("foreign", vector, {vector, mask}, false, true);
    external->add_param(vector, "value");
    external->add_param(mask, "active");
    REQUIRE(external->is_external());
    REQUIRE(!external->is_variadic());
    REQUIRE(external->body().operations().empty());
    auto verified = yir::verify_high_level_yir(module);
    REQUIRE(verified.success);
    const std::string printed = yir::print_yir_to_string(module);
    REQUIRE(printed.find("yir.declare @foreign(%value : vector<3 x i32>, "
                         "%active : mask<3>) -> vector<3 x i32>") != std::string::npos);
    REQUIRE(printed.find("yir.func @foreign") == std::string::npos);

    yir::Module invalid;
    auto *bad = invalid.add_function("bad", yir::Type::get_void(), {}, false, true);
    bad->body().append<yir::ReturnOp>();
    auto rejected = yir::verify_high_level_yir(invalid);
    REQUIRE(!rejected.success);
    REQUIRE(!rejected.errors.empty());
    REQUIRE(rejected.errors.front().find("yir.declare") != std::string::npos);
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, void (*)()>> tests = {
        {"fixed_vector_and_mask_identity", test_fixed_vector_and_mask_identity},
        {"composite_types_are_interned_structurally", test_composite_types_are_interned_structurally},
        {"invalid_vector_shapes_are_rejected", test_invalid_vector_shapes_are_rejected},
        {"external_declaration_printing_and_verification",
         test_external_declaration_printing_and_verification},
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

    with tempfile.TemporaryDirectory(prefix="yir-infra-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "yir_infra_tests.cpp"
        binary = tmp_dir / "yir_infra_tests"
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
            str(ROOT / "src/yir/YIR.cpp"),
            str(ROOT / "src/yir/YIRPrinter.cpp"),
            str(ROOT / "src/yir/YIRVerifier.cpp"),
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
