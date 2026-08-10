#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs/vector-mask-language.md"
REGISTRY = ROOT / "src/builtin/BuiltinRegistry.cpp"

VECTOR_BUILTINS = (
    "select",
    "any",
    "all",
    "none",
    "extract_lane",
    "insert_lane",
    "iota",
    "reduce_add",
    "reduce_mul",
    "reduce_min",
    "reduce_max",
    "reduce_and",
    "reduce_or",
    "reduce_xor",
    "masked_load",
    "masked_store",
    "gather",
    "scatter",
    "shuffle",
)

EXAMPLES = {
    ROOT / "examples/vector_value_semantics.sy": {
        "--emit-yir": (
            "vector<1 x i32>",
            "vector<3 x f32>",
            "mask<7>",
            "vector<31 x i32>",
            "yir.vector.cast",
            "yir.vector.splat",
            "yir.vector.extract",
            "yir.vector.insert",
            "yir.mask.not",
            "ptr<vector<3 x f32>>",
        ),
        "--emit-oir": (
            "@one_lane = global <1 x i32>",
            "@alternating = global <7 x i1>",
            "define <3 x float> @convert_and_select(",
            "vector.sitofp <3 x i32>",
            "vector.fptosi <3 x float>",
            "splat float",
            "extractelement <3 x i1>",
            "define <3 x float> @first_row(<3 x float>*",
            "xor <31 x i32>",
        ),
    },
    ROOT / "examples/vector_intrinsics.sy": {
        "--emit-yir": (
            "yir.vector.select",
            "yir.mask.any",
            "yir.mask.all",
            "yir.mask.none",
            "yir.vector.extract",
            "yir.vector.insert",
            "yir.vector.step",
            "yir.vector.reduce_add",
            "yir.vector.reduce_mul",
            "yir.vector.reduce_min",
            "yir.vector.reduce_max",
            "yir.vector.reduce_and",
            "yir.vector.reduce_or",
            "yir.vector.reduce_xor",
            "yir.vector.masked_load",
            "yir.vector.masked_store",
            "yir.vector.gather",
            "yir.vector.scatter",
            "yir.vector.shuffle",
            "ordered",
        ),
        "--emit-oir": (
            "stepvector <7 x i32>",
            "select <7 x i1>",
            "masked.load <7 x float>",
            "masked.store <7 x float>",
            "vp.gather <7 x float>",
            "vp.scatter <7 x float>",
            "shufflevector <7 x float>",
            "[-1, 0, 6, 7, 8, 12, 13]",
            "vp.reduce.ordered.fadd",
            "vp.reduce.ordered.fmul",
            "vp.reduce.ordered.fmin",
            "vp.reduce.ordered.fmax",
            "vp.reduce.and <3 x i32>",
            "vp.reduce.or <3 x i32>",
            "vp.reduce.xor <3 x i32>",
        ),
    },
}

COMPILE_FAIL_CASES = (
    ("non_positive_lane", "vector<int,0> value; int main(){return 0;}", "SE0006"),
    (
        "overflow_lane",
        "vector<int,2147483647+1> value; int main(){return 0;}",
        "CE0003",
    ),
    ("division_by_zero_lane", "vector<int,4/0> value; int main(){return 0;}", "CE0004"),
    (
        "non_constant_lane",
        "int lanes=3; vector<int,lanes> value; int main(){return 0;}",
        "SE0005",
    ),
    (
        "mask_condition",
        "int test(mask<3> value){if(value)return 1;return 0;}",
        "SE0011",
    ),
    (
        "vector_does_not_decay",
        "int main(){vector<int,3> value={};putarray(3,value);return 0;}",
        "SE0008",
    ),
    (
        "constant_lane_out_of_range",
        "int main(){vector<int,3> value={};return value[3];}",
        "SE0014",
    ),
    (
        "invalid_mask_literal",
        "int main(){mask<3> value=mask<3>{0,2,1};return 0;}",
        "SE0015",
    ),
    (
        "wrong_literal_lane_count",
        "int main(){vector<int,3> value=vector<int,3>{1,2};return 0;}",
        "SE0015",
    ),
    (
        "vector_shape_mismatch",
        "int main(){vector<int,3>a={};vector<int,7>b={};a+b;return 0;}",
        "SE0007",
    ),
    (
        "mask_arithmetic",
        "int main(){mask<3>a={};a+a;return 0;}",
        "SE0007",
    ),
    (
        "runtime_shuffle_indices",
        "vector<int,3> f(vector<int,3>a,vector<int,3>b,vector<int,3>i){"
        "return shuffle(a,b,i);}",
        "SE0017",
    ),
    (
        "shuffle_index_out_of_range",
        "vector<int,3> f(vector<int,3>a,vector<int,3>b){"
        "return shuffle(a,b,vector<int,3>{0,1,6});}",
        "SE0017",
    ),
    (
        "vector_variadic_tail",
        "int main(){vector<int,3> value={};putf(value);return 0;}",
        "SE0016",
    ),
    (
        "float_vector_remainder",
        "vector<float,3> f(vector<float,3>a,vector<float,3>b){return a%b;}",
        "SE0007",
    ),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compile and validate the fixed-vector/mask language documentation examples."
    )
    parser.add_argument("--compiler", type=Path, help="compiler binary to test")
    return parser.parse_args()


def find_compiler(explicit: Path | None) -> Path | None:
    candidates: list[Path] = []
    if explicit is not None:
        candidates.append(explicit)
    configured = os.environ.get("YOOLANG_COMPILER")
    if configured:
        candidates.append(Path(configured))
    candidates.extend(
        (
            ROOT / "build/linux/x86_64/release/compiler",
            ROOT / "build/linux/x86_64/debug/compiler",
        )
    )
    for candidate in candidates:
        resolved = candidate if candidate.is_absolute() else ROOT / candidate
        if resolved.is_file() and os.access(resolved, os.X_OK):
            return resolved.resolve()
    return None


def fail(message: str, output: str = "") -> int:
    print(f"FAIL vector_docs: {message}", file=sys.stderr)
    if output:
        print(output, file=sys.stderr)
    return 1


def validate_registry_and_document() -> int:
    registry_text = REGISTRY.read_text(encoding="utf-8")
    registered = tuple(
        re.findall(
            r'intrinsic\(\s*BuiltinID::Vector[A-Za-z0-9_]+,\s*"([^"]+)"',
            registry_text,
        )
    )
    if set(registered) != set(VECTOR_BUILTINS) or len(registered) != len(VECTOR_BUILTINS):
        return fail(
            "documented intrinsic set differs from BuiltinRegistry",
            f"registered={registered}\ndocumented={VECTOR_BUILTINS}",
        )

    doc_text = DOC.read_text(encoding="utf-8")
    missing_doc = [name for name in VECTOR_BUILTINS if f"`{name}`" not in doc_text]
    if missing_doc:
        return fail("intrinsic names missing from documentation", ", ".join(missing_doc))

    example_text = (ROOT / "examples/vector_intrinsics.sy").read_text(encoding="utf-8")
    missing_example = [
        name
        for name in VECTOR_BUILTINS
        if re.search(rf"\b{re.escape(name)}\s*\(", example_text) is None
    ]
    if missing_example:
        return fail("intrinsics missing from compilable example", ", ".join(missing_example))

    print(f"PASS registry_document_contract ({len(VECTOR_BUILTINS)} intrinsics)")
    return 0


def compile_examples(compiler: Path) -> int:
    for source, modes in EXAMPLES.items():
        if not source.is_file():
            return fail(f"missing example {source.relative_to(ROOT)}")
        for mode, expected in modes.items():
            proc = subprocess.run(
                [str(compiler), str(source), mode],
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=30,
                check=False,
            )
            if proc.returncode != 0:
                return fail(
                    f"{source.relative_to(ROOT)} {mode} exited {proc.returncode}", proc.stdout
                )
            missing = [needle for needle in expected if needle not in proc.stdout]
            if missing:
                return fail(
                    f"{source.relative_to(ROOT)} {mode} omitted expected typed IR",
                    f"missing={missing}\n{proc.stdout}",
                )
            print(f"PASS {source.stem} {mode[2:]}")
    return 0


def compile_fail_cases(compiler: Path) -> int:
    with tempfile.TemporaryDirectory(prefix="vector-docs-") as tmp:
        directory = Path(tmp)
        for name, source, diagnostic in COMPILE_FAIL_CASES:
            source_path = directory / f"{name}.sy"
            source_path.write_text(source, encoding="utf-8")
            proc = subprocess.run(
                [str(compiler), str(source_path), "--emit-yir"],
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=30,
                check=False,
            )
            if proc.returncode == 0 or diagnostic not in proc.stdout:
                return fail(
                    f"compile-fail case {name} did not report {diagnostic}",
                    proc.stdout,
                )
            print(f"PASS compile_fail_{name} ({diagnostic})")
    return 0


def main() -> int:
    args = parse_args()
    compiler = find_compiler(args.compiler)
    if compiler is None:
        return fail("compiler binary not found; run xmake or pass --compiler")
    if validate_registry_and_document() != 0:
        return 1
    if compile_examples(compiler) != 0:
        return 1
    if compile_fail_cases(compiler) != 0:
        return 1
    print(
        f"PASS vector_docs ({len(EXAMPLES)} examples x YIR/OIR, "
        f"{len(COMPILE_FAIL_CASES)} compile-fail cases)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
