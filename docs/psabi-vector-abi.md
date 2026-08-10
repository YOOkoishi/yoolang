# RISC-V fixed-length vector calling-convention variant (staged)

The classifier, MIR lowering, register allocation, callee-save handling, and
ELF emission for `-mvector-abi=psabi-vector` are staged in the compiler, but
the public CLI remains fail-closed. The original release contract requires
both fixed vector tuple lowering and real GCC/Clang caller/callee
interoperability. Source/OIR function types cannot yet represent tuple
NFIELDS, and the installed GCC 15 treats `riscv_vls_cc` as an ignored
attribute. A profile that otherwise satisfies every ABI_VLEN rule therefore
ends with `PSABI_VECTOR_ABI_UNAVAILABLE`; it must not silently use the standard
ABI or be described as GA.

Once those blockers are closed, the option will select the RISC-V fixed-length
vector calling-convention variant for every function definition and
declaration in one compilation. It is separate from the default `standard`
LP64D aggregate ABI.

## Target and ABI_VLEN contract

The staged mode first validates compile-time V or Zve code generation and an
ABI_VLEN supplied by an explicit numeric option:

```sh
compiler input.sy -S \
  -march=rv64gcv_zvl128b \
  -mrvv-deployment=compile-time \
  -mrvv-vector-bits=128 \
  -mvector-abi=psabi-vector
```

The numeric value is consumed by both target validation and the calling-
convention classifier. It must be a supported power of two and must equal the
minimum VLEN guaranteed by `-march`; a scalable value, an omitted value, or a
larger assumed VLEN is rejected. The signature is checked against the actual
Zve element families. For example, Zve32x accepts fixed `vector<int,N>` and
mask values but rejects a public `vector<float,N>` signature.

Public scalable vectors, unprototyped vector functions, compiler-level
variadic signatures, and tuple shapes that violate the classifier constraints
fail closed. Legal fixed vector tuples are classified, including consecutive
LMUL groups, but cannot yet be carried by source/OIR function types; this gap
is one of the public CLI blockers. These are ABI boundaries, not requests to
truncate a value or silently select the standard aggregate convention.

## Register and fallback mapping

For a legal direct fixed value, the classifier uses the following locations:

| Value | Parameter location | Return location |
| --- | --- | --- |
| first fixed mask | `v0` | `v0` |
| fixed data vector | aligned group in `v8`–`v23` | aligned group beginning at `v8` |
| later fixed mask | one register in `v8`–`v23` | n/a |
| scalar | ordinary LP64D GPR/FPR/stack rules | ordinary LP64D result register |

Data LMUL is the smallest of 1/2/4/8 that holds the fixed object at ABI_VLEN.
Every group obeys its LMUL alignment. A mask occupies one whole architectural
mask register. When a value is wider than the maximum direct group or no legal
argument group remains, the caller materializes the unchanged fixed object and
passes its address through the integer argument convention. An oversized
return uses a hidden sret pointer. The OIR value remains one logical fixed
vector or mask; oversized values reuse the backend's per-piece `VectorBundle`
lowering rather than exposing tuples to register allocation.

## Calls, preservation, and vector state

`v0` and `v8`–`v23` are call-clobbered. `v1`–`v7` and `v24`–`v31` are
callee-saved in variant functions. Register allocation keeps values live across
a variant call in the saved bank or spills them, and the prologue saves exactly
the touched saved registers with scalable whole-register slots. The frame size
therefore follows runtime `vlenb`; the same binary remains valid at larger VLEN.

SysY runtime functions are identified through `BuiltinRegistry` and remain
standard-ABI callees even inside a staged variant function. Their declarations
and undefined symbols carry no variant bit, and register allocation spills all
live vector values across such calls. An arbitrary source `extern` currently
has no structured per-declaration calling-convention ownership in OIR, so it
fails with `PSABI_VECTOR_EXTERNAL_CC_UNSPECIFIED` instead of being guessed as
variant or standard.

VL, VTYPE, VXRM, and VXSAT are not preserved across a call. Staged lowering
invalidates its cached vector configuration and emits a new `vset{i}vli` before
subsequent RVV work. Code generation never writes a nonzero `vstart`. The CALL
descriptor explicitly defines/clobbers VL, VTYPE, VXRM, VXSAT, and VSTART, and
the CFG vector-state pass requires a new exact reaching configuration before a
later RVV instruction.

## ELF marking and interoperability

The staged AsmPrinter emits `.variant_cc symbol` for every MIR function or
reference carrying structured variant metadata. GNU `as` maps this to
`STO_RISCV_VARIANT_CC`, which is displayed as `[VARIANT_CC]` by GNU
`readelf -Ws`. Standard runtime references carry no such metadata, and unknown
source extern declarations fail before MIR. The default standard ABI emits
neither the directive nor the symbol-other bit.

Bring-up covered direct integer/float vectors at LMUL M1/M2/M4/M8, `v0` and
later mask arguments, register-exhaustion fallback, N=33 indirect
parameter/sret bundles, N=129 indirect mask/sret bundles, phi and call
pressure, callee saves, Final-MIR pseudo elimination, GNU
`as`/`readelf`/`objdump`, and O0–O3 execution at VLEN 128/256/512/1024. Clang 22
fixed-integer `riscv_vls_cc` worked in both caller/callee directions, although
it did not mark undefined declarations with the ELF other bit. This evidence
does not replace the missing tuple path or GCC direction.

[`scripts/psabi_vector_abi_e2e_infra_tests.py`](../scripts/psabi_vector_abi_e2e_infra_tests.py)
now guards the public fail-closed contract. The full execution matrix must be
re-enabled as a release gate only after both named blockers are removed.
[`scripts/psabi_vector_staged_runtime_infra_tests.py`](../scripts/psabi_vector_staged_runtime_infra_tests.py)
uses the programmatic OIR→MIR interface behind that gate to verify live vector
spills across `putint`/`getint`, zero variant bits on both undefined runtime
symbols, and one unchanged executable at VLEN 128/256/512/1024.
