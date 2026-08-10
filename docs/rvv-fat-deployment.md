# RVV fat deployment

`-mrvv-deployment=fat` emits one assembly file containing a baseline `rv64gc`
variant, an RVV variant, and a public scalar-ABI dispatcher for every defined
source function.  This first deployable slice supports source-local fixed
vector operations, O2/O3 auto-vectorized kernels, and public
`vector<int|float,N>` / `mask<N>` values through the ordinary LP64D aggregate
ABI.  Fixed vectors and masks keep their source `FunctionType`: they use GPR,
stack, by-reference, and hidden-sret locations exactly as in
[standard-vector-aggregate-abi.md](standard-vector-aggregate-abi.md), never
vector argument registers.

For a source function `kernel`, the emitted symbols are:

```text
kernel                    public dispatcher
__yoolang_scalar_kernel  hidden rv64gc implementation
__yoolang_rvv_kernel     hidden RVV implementation
```

All direct calls are bound to the corresponding private variant before branch
optimization.  This includes recursion: the scalar implementation never
re-enters the dispatcher or reaches RVV, and the RVV implementation recursively
calls the RVV implementation.  The dispatcher preserves all eight integer and
all eight floating-point argument registers around the detector call and leaves
incoming stack arguments in place before tail-calling the selected variant.
This also preserves an aggregate split between `a7` and incoming stack+0, a
following scalar at stack+8, a large by-reference argument pointer, and the
hidden sret pointer in `a0`.  No vector register is live across the dispatcher.
OIR optimization preserves every source definition's complete function
signature in this mode; dead-argument elimination cannot silently change the
public dispatcher ABI.

The dispatcher references the weak symbol
`__yoolang_rvv_available`.  A missing symbol or a zero result selects scalar;
only a non-zero result selects RVV.  The production implementation and its
Linux fail-closed policy are documented in
[rvv-runtime-dispatch.md](rvv-runtime-dispatch.md).
Because an undefined weak reference does not normally extract an archive
member, applications that want the production detector must link its object
directly or force extraction of that member.  Ordinary archive linkage is
intentionally equivalent to a missing detector and therefore selects scalar.

The file-level architecture attribute remains `rv64gc`.  RVV functions are
assembled inside a balanced GNU assembler scope:

```asm
.option push
.option arch, +v
  # hidden RVV variants
.option pop
```

Consequently an ELF consumer does not infer that every entry point may execute
RVV.  The scalar variants and dispatchers contain no vector instruction.

Direct declaration-only calls are supported when their OIR `FunctionType`
uses the same standard ABI.  External declarations are not cloned or renamed:
both private variants call the same source symbol, and the combined object
retains that symbol as `UND` with ordinary RISC-V call relocations.  Builtin
runtime calls such as `putint` follow the same rule.  Direct recursion and
calls to definitions in the same source module are instead rebound to the
corresponding hidden variant so they do not re-enter a dispatcher.

Declaration-only standard-ABI scalar variadic callees are supported when every
variadic tail argument is scalar; both variants call the same external symbol.
Variadic source definitions and vector/mask variadic tail arguments remain
fail-closed with `FAT_VARIADIC_UNSUPPORTED`.

The current implementation deliberately rejects, with stable `FAT_*`
diagnostics, scalable or otherwise unsupported public ABI types, indirect or
address-taken functions, reserved `__yoolang_*` definitions, nonstandard fat
architecture strings, and emit modes other than assembly.
`-mvector-abi=psabi-vector` remains rejected.  A numeric
`-mrvv-vector-bits=N` is also rejected because the current detector proves V
availability, not a particular VLEN; source-level fixed vectors remain
supported through VLA lowering.  Those cases are not silently compiled as one
target.

Reproduce the assembler, ELF attribute, recursion/call rewrite, weak/strong
detector, QEMU `v=false`, and VLEN 128/256/512/1024 checks with:

```bash
xmake build compiler
python3 scripts/rvv_fat_multiversion_infra_tests.py \
  --compiler build/linux/x86_64/release/compiler
python3 scripts/rvv_fat_standard_abi_infra_tests.py \
  --compiler build/linux/x86_64/release/compiler
```

The second gate covers N=1/3/7/31/33, large by-reference and sret traffic,
`mask<31/33>` packed tail bits, float-union integer classification, `a7`/stack
splits, globals/arrays, direct C externs (including scalar variadic calls),
builtin runtime calls, recursion,
missing/zero/nonzero detectors, GCC and Clang peers, O0-O3, QEMU `v=false`, and
VLEN 128/256/512/1024.  It also audits ELF attributes, symbol visibility,
non-overlapping function ranges, undefined symbols, relocations, and decoded
instructions.
