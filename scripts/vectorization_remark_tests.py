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
#include "pass/oir/OIRVectorizationRemark.h"

#include <iostream>
#include <sstream>
#include <string>

int main() {
    using namespace pass::oir_vectorize;
    RemarkLog log;
    Remark success;
    success.code = RemarkCode::Vectorized;
    success.function = "kernel";
    success.region = "loop.header";
    success.explanation = "unit-stride integer loop";
    success.plan.minimum_lanes = 4;
    success.plan.lmul = "m2";
    success.plan.interleave = 2;
    success.plan.estimated_scalar_cost = 32;
    success.plan.estimated_vector_cost = 9;
    success.plan.estimated_vector_registers = 6;
    success.plan.predicted_spill_registers = 1;
    success.plan.interleave_overlap_credit = 5;
    success.plan.estimated_code_bytes = 48;
    success.plan.break_even_trip_count = 7;
    success.plan.tuning = "generic-rvv";
    success.plan.uses_mask = true;
    log.add(success);

    Remark rejected;
    rejected.code = RemarkCode::RejectAlias;
    rejected.function = "overlap";
    rejected.region = "loop.header";
    rejected.explanation = "cannot prove disjoint address ranges";
    rejected.plan.interleave = 1;
    rejected.plan.interleave_capability_gate =
        "INTERLEAVE_FACTOR_2_RECIPE_UNAVAILABLE: test rejection";
    log.add(rejected);

    std::ostringstream text;
    print_remarks_text(log, text);
    if (text.str().find("VECTORIZED") == std::string::npos ||
        text.str().find("REJECT_ALIAS") == std::string::npos ||
        text.str().find("vscale x 4") == std::string::npos) {
        std::cerr << text.str();
        return 1;
    }

    std::ostringstream json;
    print_vector_plan_json(log, json);
    if (json.str().find("\"code\": \"VECTORIZED\"") == std::string::npos ||
        json.str().find("\"predicted_spill_registers\": 1") == std::string::npos ||
        json.str().find("\"interleave_overlap_credit\": 5") == std::string::npos ||
        json.str().find("\"break_even_trip_count\": 7") == std::string::npos ||
        json.str().find("\"tuning\": \"generic-rvv\"") == std::string::npos ||
        json.str().find("INTERLEAVE_FACTOR_2_RECIPE_UNAVAILABLE: test rejection") ==
            std::string::npos ||
        json.str().find("\"runtime_alias_check\": false") == std::string::npos ||
        json.str().find("\"function\": \"overlap\"") == std::string::npos) {
        std::cerr << json.str();
        return 1;
    }

    std::ostringstream filtered;
    print_remarks_text(log, filtered, false, true, "overlap");
    if (filtered.str().find("REJECT_ALIAS") == std::string::npos ||
        filtered.str().find("VECTORIZED") != std::string::npos) {
        return 1;
    }
    std::cout << "PASS stable_vectorization_remarks\n";
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
    with tempfile.TemporaryDirectory(prefix="vector-remark-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "vectorization_remark_tests.cpp"
        binary = tmp_dir / "vectorization_remark_tests"
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
                str(ROOT / "src/pass/oir/OIRVectorizationRemark.cpp"),
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
