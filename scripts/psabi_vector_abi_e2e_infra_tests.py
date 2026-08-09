#!/usr/bin/env python3

"""End-to-end gate for the opt-in RISC-V fixed-length vector ABI variant."""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]
COMPILER = Path(
    os.environ.get(
        "YOOLANG_COMPILER", str(ROOT / "build/linux/x86_64/release/compiler")
    )
)
MARCH = "rv64gcv_zvl128b"
MABI = "lp64d"
OPT_LEVELS = (0, 1, 2, 3)
VLENS = (128, 256, 512, 1024)
PSABI_FLAGS = (
    f"-march={MARCH}",
    f"-mabi={MABI}",
    "-mrvv-deployment=compile-time",
    "-mrvv-vector-bits=128",
    "-mvector-abi=psabi-vector",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError(f"required tool not found in PATH: {name}")
    return path


def invoke(
    command: list[str], *, timeout: float = 90.0
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
        timeout=timeout,
    )


def checked(command: list[str], description: str, *, timeout: float = 90.0) -> str:
    result = invoke(command, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(
            f"{description} failed ({result.returncode})\n"
            f"command: {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result.stdout


def expect_failure(command: list[str], needle: str, description: str) -> None:
    result = invoke(command)
    require(result.returncode != 0, f"{description} unexpectedly succeeded")
    diagnostic = result.stdout + result.stderr
    require(
        needle in diagnostic,
        f"{description} lost diagnostic {needle!r}\n{diagnostic}",
    )


def qemu_cpu(vlen: int) -> str:
    return f"rv64,v=true,vlen={vlen},elen=64,vext_spec=v1.0"


def run_all_vlens(qemu: str, executable: Path, description: str) -> None:
    for vlen in VLENS:
        checked(
            [qemu, "-cpu", qemu_cpu(vlen), str(executable)],
            f"{description} at VLEN={vlen}",
        )


def symbol_line(symbols: str, name: str) -> str:
    matches = [
        line
        for line in symbols.splitlines()
        if re.search(rf"\b{re.escape(name)}$", line.strip())
    ]
    require(len(matches) == 1, f"expected exactly one ELF symbol {name}, got {matches}")
    return matches[0]


def require_variant_symbol(symbols: str, name: str, binding: str | None = None) -> None:
    line = symbol_line(symbols, name)
    require("[VARIANT_CC]" in line, f"ELF symbol {name} lacks STO_RISCV_VARIANT_CC: {line}")
    if binding is not None:
        require(binding in line, f"ELF symbol {name} lacks {binding} binding: {line}")


def function_body(assembly: str, name: str) -> str:
    match = re.search(
        rf"(?ms)^{re.escape(name)}:\n(.*?)^\s*\.size\s+{re.escape(name)},",
        assembly,
    )
    require(match is not None, f"assembly lacks function body {name}")
    return match.group(1)


def internal_source() -> str:
    alternating = ",".join("1" if lane % 2 == 0 else "0" for lane in range(31))
    return textwrap.dedent(
        f"""
        vector<int,4> add4(vector<int,4> a, vector<int,4> b) {{
          return a + b;
        }}

        vector<float,4> addf4(vector<float,4> a, vector<float,4> b) {{
          return (a + b) * vector<float,4>(2.0);
        }}

        vector<int,8> add8(vector<int,8> a, vector<int,8> b) {{
          return a + b;
        }}

        vector<int,16> add16(vector<int,16> a, vector<int,16> b) {{
          return a + b;
        }}

        vector<int,32> exhausted32(vector<int,32> a, vector<int,32> b,
                                   vector<int,32> indirect) {{
          return a + b + indirect;
        }}

        mask<31> mix_mask31(mask<31> a, mask<31> b, int choose) {{
          mask<31> all = (a ^ b) & (a | b);
          mask<31> result;
          if (choose) result = all;
          else result = a;
          return result;
        }}

        vector<int,33> add33(vector<int,33> a, vector<int,33> b, int bias) {{
          vector<int,33> sum = a + b + vector<int,33>(bias);
          vector<int,33> result;
          if (bias & 1) result = sum;
          else result = a;
          return result;
        }}

        mask<129> invert_mask129(mask<129> input) {{
          return ~input;
        }}

        vector<int,4> pressure_across_call(vector<int,4> a,
                                           vector<int,4> b, int choose) {{
          vector<int,4> k0 = a + vector<int,4>(10);
          vector<int,4> k1 = b + vector<int,4>(20);
          vector<int,4> k2 = a + vector<int,4>(30);
          vector<int,4> k3 = b + vector<int,4>(40);
          vector<int,4> k4 = a + vector<int,4>(50);
          vector<int,4> k5 = b + vector<int,4>(60);
          vector<int,4> k6 = a + vector<int,4>(70);
          vector<int,4> k7 = b + vector<int,4>(80);
          vector<int,4> called = add4(vector<int,4>(1), vector<int,4>(2));
          vector<int,4> merged;
          if (choose)
            merged = k0 + k1 + k2 + k3 + k4 + k5 + k6 + k7;
          else
            merged = a;
          return merged + called;
        }}

        int main() {{
          vector<int,4> pressure = pressure_across_call(
              vector<int,4>(1), vector<int,4>(2), 1);
          if (pressure[0] != 375 || pressure[3] != 375) return 1;

          vector<float,4> floats = addf4(
              vector<float,4>(1.5), vector<float,4>(2.5));
          vector<int,4> float_bits = vector<int,4>(floats);
          if (float_bits[0] != 8 || float_bits[3] != 8) return 2;

          vector<int,8> m2 = add8(vector<int,8>(3), vector<int,8>(4));
          vector<int,16> m4 = add16(vector<int,16>(5), vector<int,16>(6));
          vector<int,32> m8 = exhausted32(
              vector<int,32>(1), vector<int,32>(2), vector<int,32>(3));
          if (m2[7] != 7 || m4[15] != 11 || m8[31] != 6) return 5;

          mask<31> a = mask<31>{{{alternating}}};
          mask<31> masks = mix_mask31(a, ~a, 1);
          if (!masks[0] || !masks[1] || !masks[30]) return 3;

          mask<129> wide_mask = invert_mask129(mask<129>{{}});
          if (!wide_mask[0] || !wide_mask[64] || !wide_mask[128]) return 6;

          vector<int,33> wide = add33(
              vector<int,33>(5), vector<int,33>(7), 3);
          if (wide[0] != 15 || wide[16] != 15 || wide[32] != 15) return 4;
          return 0;
        }}
        """
    ).strip() + "\n"


Y_PROVIDER = r"""
vector<int,4> y_add4(vector<int,4> a, vector<int,4> b) {
  return a + b;
}

vector<float,4> y_addf4(vector<float,4> a, vector<float,4> b) {
  return a + b;
}
"""


CLANG_CALLER = r"""
typedef int v4i __attribute__((vector_size(16)));

extern __attribute__((riscv_vls_cc)) v4i y_add4(v4i, v4i);

int main(void) {
  v4i ia = {1, 2, 3, 4};
  v4i ib = {10, 20, 30, 40};
  v4i ir = y_add4(ia, ib);
  if (ir[0] != 11 || ir[1] != 22 || ir[2] != 33 || ir[3] != 44) return 1;
  return 0;
}
"""


CLANG_PROVIDER = r"""
typedef int v4i __attribute__((vector_size(16)));
typedef float v4f __attribute__((vector_size(16)));

__attribute__((riscv_vls_cc)) v4i c_add4(v4i a, v4i b) {
  return a + b;
}

__attribute__((riscv_vls_cc)) v4f c_addf4(v4f a, v4f b) {
  return a + b;
}
"""


Y_CALLER = r"""
extern vector<int,4> c_add4(vector<int,4> a, vector<int,4> b);

int main() {
  vector<int,4> ints = c_add4(
      vector<int,4>{1,2,3,4}, vector<int,4>{10,20,30,40});
  if (ints[0] != 11 || ints[1] != 22 ||
      ints[2] != 33 || ints[3] != 44) return 1;
  return 0;
}
"""


def compile_yoolang(source: Path, assembly: Path, opt: int) -> None:
    checked(
        [
            str(COMPILER),
            str(source),
            "-S",
            f"-O{opt}",
            *PSABI_FLAGS,
            "-o",
            str(assembly),
        ],
        f"compile {source.name} at O{opt}",
    )


def assemble(assembler: str, assembly: Path, output: Path) -> None:
    checked(
        [assembler, f"-march={MARCH}", f"-mabi={MABI}", str(assembly), "-o", str(output)],
        f"GNU as {assembly.name}",
    )


def clang_compile(clang: str, source: Path, output: Path) -> None:
    checked(
        [
            clang,
            "--target=riscv64-linux-gnu",
            "--sysroot=/usr/riscv64-linux-gnu",
            "-c",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-march={MARCH}",
            f"-mabi={MABI}",
            "-mrvv-vector-bits=128",
            str(source),
            "-o",
            str(output),
        ],
        f"Clang riscv_vls_cc compile {source.name}",
    )


def link(gcc: str, output: Path, *objects: Path) -> None:
    checked(
        [
            gcc,
            "-static",
            f"-march={MARCH}",
            f"-mabi={MABI}",
            *(str(obj) for obj in objects),
            "-o",
            str(output),
        ],
        f"GNU link {output.name}",
    )


def validate_internal_assembly(assembly: str) -> None:
    for name in (
        "add4",
        "addf4",
        "add8",
        "add16",
        "exhausted32",
        "mix_mask31",
        "add33",
        "invert_mask129",
        "pressure_across_call",
        "main",
    ):
        require(f"\t.variant_cc {name}\n" in assembly, f"assembly does not mark {name}")

    add_body = function_body(assembly, "add4")
    require(re.search(r"\bvmv\.v\.v\s+v\d+,\s*v8\b", add_body) is not None,
            "direct data parameter 0 was not received from v8")
    require(re.search(r"\bvmv\.v\.v\s+v\d+,\s*v9\b", add_body) is not None,
            "direct data parameter 1 was not received from v9")
    require(re.search(r"\bvmv\.v\.v\s+v8,\s*v\d+\b", add_body) is not None,
            "direct data return was not written to v8")

    mask_body = function_body(assembly, "mix_mask31")
    require(re.search(r"\bvmand\.mm\s+v\d+,\s*v0,\s*v0\b", mask_body) is not None,
            "first mask parameter was not received from v0")
    require(re.search(r"\bvmand\.mm\s+v\d+,\s*v8,\s*v8\b", mask_body) is not None,
            "later mask parameter was not received from v8")
    require(re.search(r"\bvmand\.mm\s+v0,\s*(v\d+),\s*\1\b", mask_body) is not None,
            "mask return was not written to v0")

    pressure_body = function_body(assembly, "pressure_across_call")
    call = re.search(
        r"(?ms)\bauipc\s+ra,\s*%pcrel_hi\(add4\).*?\n\s*jalr\s+ra,.*?\n",
        pressure_body,
    )
    require(call is not None, "O0 pressure fixture lost its vector call")
    saves = re.findall(r"(?m)^\s*vs1r\.v\s+(v(?:[1-7]|2[4-9]|3[01]))\b", pressure_body)
    restores = re.findall(r"(?m)^\s*vl1re8\.v\s+(v(?:[1-7]|2[4-9]|3[01]))\b", pressure_body)
    require(saves, "variant caller did not save any used callee-saved vector register")
    require(sorted(saves) == sorted(restores),
            f"vector callee-save/restore sets disagree: {saves} vs {restores}")
    after_call = pressure_body[call.end():]
    require(re.search(r"\bvset(?:i)?vli\b", after_call) is not None,
            "vector configuration was reused across a variant call")

    wide_body = function_body(assembly, "add33")
    require("\tvle32.v " in wide_body and "\tvse32.v " in wide_body,
            "N=33 indirect parameter/sret path lacks bundle loads/stores")
    exhausted_body = function_body(assembly, "exhausted32")
    require(re.search(r"\bvmv\.v\.v\s+v\d+,\s*v8\b", exhausted_body) is not None and
            re.search(r"\bvmv\.v\.v\s+v\d+,\s*v16\b", exhausted_body) is not None and
            "\tvle32.v " in exhausted_body,
            "M8 register exhaustion did not use v8/v16 plus indirect fallback")
    for pseudo in ("RVVMask", "RVVVector", "Pseudo", "%v"):
        require(pseudo not in assembly, f"final assembly leaked pseudo text {pseudo}")


def validate_negative_profiles(work: Path) -> None:
    scalar = work / "negative_scalar.sy"
    scalar.write_text("int main() { return 0; }\n", encoding="utf-8")
    float_vector = work / "negative_float_zve.sy"
    float_vector.write_text(
        "vector<float,1> identity(vector<float,1> x) { return x; }\n",
        encoding="utf-8",
    )
    variadic = work / "negative_variadic.sy"
    variadic.write_text(
        "extern int variadic_api(int fixed, ...);\n"
        "int main() { return variadic_api(1); }\n",
        encoding="utf-8",
    )

    expect_failure(
        [str(COMPILER), str(scalar), "-S", "-march=rv64gcv",
         "-mvector-abi=psabi-vector"],
        "explicit numeric -mrvv-vector-bits=ABI_VLEN",
        "missing ABI_VLEN",
    )
    expect_failure(
        [str(COMPILER), str(scalar), "-S", "-march=rv64gcv",
         "-mrvv-vector-bits=scalable", "-mvector-abi=psabi-vector"],
        "explicit numeric -mrvv-vector-bits=ABI_VLEN",
        "scalable ABI_VLEN",
    )
    expect_failure(
        [str(COMPILER), str(scalar), "-S", "-march=rv64gcv_zvl128b",
         "-mrvv-vector-bits=256", "-mvector-abi=psabi-vector"],
        "must match the VLEN guaranteed by -march",
        "mismatched ABI_VLEN",
    )
    expect_failure(
        [str(COMPILER), str(scalar), "-S", "-march=rv64gc",
         "-mrvv-deployment=compile-time", "-mrvv-vector-bits=128",
         "-mvector-abi=psabi-vector"],
        "compile-time requires V or Zve",
        "psabi-vector without V/Zve",
    )
    expect_failure(
        [str(COMPILER), str(float_vector), "-S", "-march=rv64gc_zve32x",
         "-mrvv-vector-bits=32", "-mvector-abi=psabi-vector"],
        "f32 vector support",
        "float signature on Zve32x",
    )
    expect_failure(
        [str(COMPILER), str(variadic), "-S", *PSABI_FLAGS],
        "variadic signatures are forbidden",
        "psabi-vector variadic signature",
    )


def run_staged_matrix() -> int:
    """Bring-up matrix retained for reactivation once the public blockers close."""
    require(COMPILER.is_file(), f"compiler not found: {COMPILER}")
    assembler = require_tool("riscv64-linux-gnu-as")
    readelf = require_tool("riscv64-linux-gnu-readelf")
    objdump = require_tool("riscv64-linux-gnu-objdump")
    gcc = require_tool("riscv64-linux-gnu-gcc")
    clang = require_tool("clang")
    qemu = require_tool("qemu-riscv64")

    with tempfile.TemporaryDirectory(prefix="yoolang-psabi-vector-") as directory:
        work = Path(directory)
        validate_negative_profiles(work)

        standard_source = work / "standard.sy"
        standard_source.write_text("int main() { return 0; }\n", encoding="utf-8")
        standard_assembly = work / "standard.s"
        standard_object = work / "standard.o"
        checked(
            [str(COMPILER), str(standard_source), "-S", "-O0", f"-march={MARCH}",
             "-o", str(standard_assembly)],
            "compile standard ABI control",
        )
        assemble(assembler, standard_assembly, standard_object)
        standard_symbols = checked([readelf, "-Ws", str(standard_object)],
                                   "read standard control symbols")
        require(".variant_cc" not in standard_assembly.read_text(encoding="utf-8") and
                "[VARIANT_CC]" not in standard_symbols,
                "standard ABI control was contaminated by variant CC metadata")

        internal = work / "internal.sy"
        internal.write_text(internal_source(), encoding="utf-8")
        for opt in OPT_LEVELS:
            assembly = work / f"internal.O{opt}.s"
            object_file = work / f"internal.O{opt}.o"
            executable = work / f"internal.O{opt}"
            compile_yoolang(internal, assembly, opt)
            assembly_text = assembly.read_text(encoding="utf-8")
            if opt == 0:
                validate_internal_assembly(assembly_text)
            assemble(assembler, assembly, object_file)
            symbols = checked([readelf, "-Ws", str(object_file)],
                              f"read internal O{opt} symbols")
            for name in ("add4", "addf4", "add8", "add16", "exhausted32",
                         "mix_mask31", "add33", "invert_mask129",
                         "pressure_across_call", "main"):
                require_variant_symbol(symbols, name)
            disassembly = checked([objdump, "-dr", str(object_file)],
                                  f"objdump internal O{opt}")
            require("vset" in disassembly and "vadd.vv" in disassembly,
                    f"internal O{opt} object lacks decoded RVV instructions")
            link(gcc, executable, object_file)
            run_all_vlens(qemu, executable, f"internal O{opt}")

            final_mir = checked(
                [str(COMPILER), str(internal), "--emit-mir-stage=final", f"-O{opt}",
                 *PSABI_FLAGS],
                f"final MIR internal O{opt}",
            )
            require(re.search(r"(?m)^\s+RVV[A-Z]", final_mir) is None,
                    f"internal O{opt} final MIR contains an RVV pseudo")
            virtual_lines = [
                line for line in final_mir.splitlines()
                if re.match(r"^\s+[A-Z][A-Z0-9_.]* ", line) and "%v" in line
            ]
            require(not virtual_lines,
                    f"internal O{opt} final MIR contains a virtual register: "
                    f"{virtual_lines[:8]}")
            print(f"PASS psabi_internal_O{opt}_vlen128_256_512_1024")

        y_provider = work / "y_provider.sy"
        clang_caller = work / "clang_caller.c"
        clang_provider = work / "clang_provider.c"
        y_caller = work / "y_caller.sy"
        y_provider.write_text(textwrap.dedent(Y_PROVIDER), encoding="utf-8")
        clang_caller.write_text(textwrap.dedent(CLANG_CALLER), encoding="utf-8")
        clang_provider.write_text(textwrap.dedent(CLANG_PROVIDER), encoding="utf-8")
        y_caller.write_text(textwrap.dedent(Y_CALLER), encoding="utf-8")

        clang_caller_object = work / "clang_caller.o"
        clang_provider_object = work / "clang_provider.o"
        clang_compile(clang, clang_caller, clang_caller_object)
        clang_compile(clang, clang_provider, clang_provider_object)
        clang_caller_symbols = checked([readelf, "-Ws", str(clang_caller_object)],
                                       "read Clang caller symbols")
        clang_provider_symbols = checked([readelf, "-Ws", str(clang_provider_object)],
                                         "read Clang provider symbols")
        for name in ("y_add4",):
            # Clang 22 selects the VLS register convention at the call site but
            # does not set STO_RISCV_VARIANT_CC on an undefined declaration.
            # The linked definition below carries the bit; yoolang references
            # are checked strictly in the opposite direction.
            require("UND" in symbol_line(clang_caller_symbols, name),
                    f"Clang caller does not retain undefined symbol {name}")
        for name in ("c_add4", "c_addf4"):
            require_variant_symbol(clang_provider_symbols, name)

        for opt in OPT_LEVELS:
            provider_assembly = work / f"y_provider.O{opt}.s"
            provider_object = work / f"y_provider.O{opt}.o"
            provider_executable = work / f"clang_to_y.O{opt}"
            compile_yoolang(y_provider, provider_assembly, opt)
            assemble(assembler, provider_assembly, provider_object)
            provider_symbols = checked([readelf, "-Ws", str(provider_object)],
                                       f"read yoolang provider O{opt} symbols")
            for name in ("y_add4", "y_addf4"):
                require_variant_symbol(provider_symbols, name)
            link(gcc, provider_executable, clang_caller_object, provider_object)
            run_all_vlens(qemu, provider_executable, f"Clang caller -> yoolang O{opt}")

            caller_assembly = work / f"y_caller.O{opt}.s"
            caller_object = work / f"y_caller.O{opt}.o"
            caller_executable = work / f"y_to_clang.O{opt}"
            compile_yoolang(y_caller, caller_assembly, opt)
            assemble(assembler, caller_assembly, caller_object)
            caller_symbols = checked([readelf, "-Ws", str(caller_object)],
                                     f"read yoolang caller O{opt} symbols")
            for name in ("c_add4",):
                require_variant_symbol(caller_symbols, name, "UND")
            link(gcc, caller_executable, caller_object, clang_provider_object)
            run_all_vlens(qemu, caller_executable, f"yoolang O{opt} -> Clang callee")
            print(f"PASS psabi_clang_both_directions_O{opt}")

        # GCC 15 currently diagnoses riscv_vls_cc as an ignored attribute.  If
        # a newer configured GCC implements it, promote the probe to a hard ELF
        # symbol check; otherwise GNU as/readelf/objdump/link above remain the
        # expressible GNU-toolchain direction and the unsupported frontend is
        # recorded explicitly rather than silently treated as interoperable.
        gcc_probe = work / "gcc_vls_probe.o"
        probe = invoke(
            [gcc, "-c", "-O2", "-Wall", "-Wextra", "-Werror=attributes",
             f"-march={MARCH}", f"-mabi={MABI}", str(clang_provider),
             "-o", str(gcc_probe)]
        )
        gcc_has_vls_cc = False
        if probe.returncode == 0:
            probe_symbols = checked([readelf, "-Ws", str(gcc_probe)],
                                    "read GCC VLS probe symbols")
            gcc_has_vls_cc = all(
                "[VARIANT_CC]" in symbol_line(probe_symbols, name)
                for name in ("c_add4", "c_addf4")
            )
        if gcc_has_vls_cc:
            print("PASS gcc_riscv_vls_cc_frontend_and_variant_symbols")
        else:
            print("PASS gcc_current_toolchain_has_no_expressible_riscv_vls_cc_frontend")

    print("PASS psabi_numeric_abi_vlen_negative_profiles")
    print("PASS psabi_gnu_as_readelf_objdump_and_standard_zero_variant")
    print("PASS psabi_vector_abi_e2e")
    return 0


def main() -> int:
    require(COMPILER.is_file(), f"compiler not found: {COMPILER}")
    with tempfile.TemporaryDirectory(prefix="yoolang-psabi-vector-gate-") as directory:
        source = Path(directory) / "gate.sy"
        source.write_text("int main() { return 0; }\n", encoding="utf-8")
        expect_failure(
            [str(COMPILER), str(source), "-S", *PSABI_FLAGS],
            "PSABI_VECTOR_ABI_UNAVAILABLE",
            "complete psabi-vector profile remains publicly gated",
        )
        rejected = invoke([str(COMPILER), str(source), "-S", *PSABI_FLAGS])
        require("vector tuple" in rejected.stderr and "GCC/Clang" in rejected.stderr,
                "psabi-vector public gate does not name both release blockers")
    print("PASS psabi_vector_public_gate_tuple_and_gcc_clang")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL psabi_vector_abi_e2e: {error}", file=sys.stderr)
        raise SystemExit(1)
