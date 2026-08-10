# Standard fixed-vector aggregate ABI contract and interoperability probe

The default public ABI for source `vector<T,N>` and `mask<N>` values is the
ordinary RISC-V LP64D aggregate convention. It is deliberately separate from
the staged, currently fail-closed `-mvector-abi=psabi-vector` convention:
public values described here use integer argument registers and the stack,
never vector registers.

The shared `target::RISCVCallingConvention` classifier is the single source of
truth for function entry, call sites, and returns. The executable probe is
[`scripts/standard_vector_aggregate_abi_e2e.py`](../scripts/standard_vector_aggregate_abi_e2e.py).
Its auto-discovered required wrapper is
[`scripts/standard_vector_aggregate_abi_e2e_infra_tests.py`](../scripts/standard_vector_aggregate_abi_e2e_infra_tests.py).
The gate first runs `scripts/calling_convention_tests.py`, then validates the
same layout against GCC and Clang in both caller/callee directions, and finally
executes the yoolang interoperability matrix.

## Fixed contract exercised by the probe

| Source value or position | Size / alignment | LP64D locations |
| --- | --- | --- |
| `vector<int,3>` | 12 / 4 | first 8 bytes in `a0`, last 4 bytes in `a1` |
| `vector<float,3>` | 12 / 4 | identical integer aggregate locations; no `fa*` |
| either 12-byte vector return | 12 / 4 | first 8 bytes in `a0`, last 4 bytes in `a1` |
| `mask<31>` | 4 / 1 | packed bytes in `a0`; logical bits 0–30, tail bit 31 clear |
| 12-byte vector after seven integer arguments | 12 / 4 | first 8 bytes in `a7`, last 4 bytes at incoming stack offset 0 |
| scalar following that split vector | 4 / 4 | incoming stack offset 8; total argument area rounded to 16 |
| `vector<int,5>` | 20 / 4 | caller-owned temporary passed by address |
| `vector<int,5>` return | 20 / 4 | hidden sret pointer in `a0`; no aggregate return register |

The C representation of `vector<float,3>` is intentionally a union containing
`float[3]`, `unsigned int[3]`, and 12 raw bytes. A struct made only from scalar
float fields can be flattened into the LP64D hardware floating-point aggregate
convention and would therefore be the wrong oracle for a yoolang vector. The
union retains size 12 and alignment 4 while forcing ordinary integer aggregate
classification. `mask<31>` is a four-byte struct with no integer union member,
so its C alignment remains one.

## Directions and link technique

The probe contains these production-facing cases:

- GCC and Clang compile the C-only provider and consumer independently. The
  runner links GCC-caller → Clang-callee and Clang-caller → GCC-callee binaries,
  executes both under QEMU, and inspects both providers' assembly. The assembly
  audit requires `a0/a1` for the float union, rejects `fa0`–`fa7`, and proves the
  seven-GPR split consumes `a7`, incoming stack+0, and incoming stack+8.
- C callers pass direct integer vectors, a raw float vector, a split vector, and
  a packed mask into yoolang functions; C also consumes direct vector and mask
  returns from yoolang. A 20-byte vector additionally proves indirect parameter
  passing and hidden-sret return in both directions.
- For yoolang-caller → C-callee tests, the source uses a real declaration-only
  `extern` prototype. The assembled yoolang object must retain the C callee as
  an undefined `U` symbol. Aggregate cases must carry an `R_RISCV_CALL_PLT`
  relocation; the scalar/float/array smoke case may instead use the existing
  medany `R_RISCV_PCREL_HI20`/`R_RISCV_PCREL_LO12_I` call sequence. The gate
  then links the GCC- or Clang-built definition. No local stub, symbol weakening,
  or `objcopy` rewrite participates in this path.
- A yoolang caller also emits the seven-GPR plus 12-byte split layout, including
  `a7`, outgoing stack+0, and the following scalar at stack+8. Recursive and
  nested fixed-vector calls exercise entry staging, call-live values, and
  aggregate returns across multiple internal frames.

The default probe uses matching `-march=rv64gcv -mabi=lp64d` compiler,
assembler, linker, and C commands. Its executables run unchanged at VLEN 128,
256, 512, and 1024. Passing `--march rv64gc` selects the portable scalar
implementation instead: every linked binary runs with `qemu-riscv64 -cpu
rv64,v=false`, and both each yoolang object and the final static ELF must have a
scalar `Tag_RISCV_arch` and no decoded V/Zve instruction. Both modes compile
every yoolang case at O0, O1, O2, and O3 with GCC- and Clang-built peers, use GNU
`as`, inspect with `readelf`/`objdump`, and reject `.variant_cc`.

## Running and interpreting status

Build the release compiler, then run:

```sh
python3 scripts/standard_vector_aggregate_abi_e2e.py

# The same public LP64D ABI with scalar lane computation and no V/Zve ISA:
python3 scripts/standard_vector_aggregate_abi_e2e.py --march rv64gc
```

To retain every generated fixture and exact link input:

```sh
python3 scripts/standard_vector_aggregate_abi_e2e.py \
  --artifacts /tmp/yoolang-standard-vector-abi
```

Status is fail closed:

- Exit 0 means both C cross-toolchain oracle binaries and every yoolang/C case
  assembled, linked, and executed successfully.
- Exit 2 with `EXPECTED_UNIMPLEMENTED` means the yoolang compiler rejected a
  case using one of the exact, audited aggregate-lowering diagnostics. Such a
  case is never printed as `PASS`.
- Exit 2 with `BLOCKED` means a required external compiler, binutils tool, QEMU,
  or the yoolang compiler binary is unavailable.
- Exit 1 means a malformed/unrecognized compiler failure, partial assembly on a
  failed compile, incorrect C layout, wrong register class, link error, or
  runtime mismatch.

The production lowerer consumes the shared classifier assignments without
reclassifying values. Direct aggregates use padded, 16-byte-aligned bridge
slots between raw GPR/stack pieces and the implementation value. All incoming
and outgoing physical pieces are staged before conversion, so split values
cannot overwrite later arguments. Values larger than 2×XLEN use caller-owned
by-reference temporaries and a staged hidden sret pointer.

On RVV targets the implementation value is loaded/stored through RVV. On
`rv64gc`, `OIRPortableVectorScalarizerPass` keeps the public `FunctionType`,
global type, array stride, and `ptr<vector>` object identity, while replacing
body computation with scalar lanes. Four fixed-only boundary operations carry
the remaining ownership explicitly: `abi.fixed.extract`, `abi.fixed.pack`,
`abi.fixed.load_lane`, and `abi.fixed.store_lane`. Their parser, printer,
verifier, use-list, and private-transaction round trips are required gates.
Numeric lane offsets use `DataLayout`; mask lane `i` is packed bit `i`.
Outbound mask packs zero their storage before bit RMW, and final-byte stores
preserve other valid lanes while clearing every unused high bit.

The same contract is used by `-mrvv-deployment=fat`.  Its public dispatcher is
ABI-transparent: it saves all integer and floating-point argument registers,
leaves incoming stack arguments in place, restores a hidden sret pointer, and
tail-calls either a portable rv64gc implementation or an RVV implementation.
Direct declaration-only externs retain one shared standard-ABI symbol in both
variants.  The dedicated cross-target gate is
[`scripts/rvv_fat_standard_abi_infra_tests.py`](../scripts/rvv_fat_standard_abi_infra_tests.py).

The original boundary cases plus the extended by-reference, split, recursive,
nested, declaration-only extern, and nonzero-prefilled `mask<31>` global-object
RMW cases pass the full GCC/Clang O0–O3 matrix. The RVV mode covers VLEN
128/256/512/1024; the portable mode covers `rv64,v=false` and globally rejects
V/Zve ELF attributes and opcodes. The wrapper is part of the required toolchain
infra profile, so any future `EXPECTED_UNIMPLEMENTED` result is a gate failure,
not an allowed green status. Fixed/scalable vector varargs remain explicitly
unsupported and fail before MIR lowering.
