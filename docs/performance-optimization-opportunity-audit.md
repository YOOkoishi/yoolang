# Performance Optimization Opportunity Audit

Date: 2026-07-08

## Baseline

Fresh command sequence:

```bash
xmake
PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py
```

Result:

- 60/60 `test/performance` cases passed.
- Geomean speedup: GCC `0.972x`, Clang++ `1.018x`.
- Faster cases: GCC 21/60, Clang++ 22/60.
- QEMU dynamic instruction count: disabled, because `ENABLE_QEMU_INSN_COUNT` was not set.
- MIR stage metrics: OK for all 60 cases.
- Cost-model decisions: OK, 1272 accepted / 1039 bypassed / 1649 rejected.

The audit used `build/perf-ci/perf-report.md`, `build/perf-ci/perf-report.json`,
selected generated assembly under `build/perf-ci/test/performance/<case>/`, and selected
temporary OIR/MIR/cost-model dumps under `/tmp/yoolang-audit`.

## Selected Cases

| Case | yoolang/GCC | yoolang/Clang | yoolang time | Final MIR | Loads | Stores | Branches | Jumps | Spills | Classification |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `shuffle1` | 1.02x | 1.15x | 0.6377s | 184 | 18 | 18 | 11 | 17 | 0 | low priority |
| `conv2d-1` | 1.08x | 1.46x | 0.4814s | 446 | 27 | 14 | 32 | 50 | 0 | missing fixed-small-loop specialization |
| `knapsack_naive-2` | 1.14x | 0.42x | 0.3100s | 700 | 93 | 21 | 60 | 56 | 17 | conservative recursive inlining / backend pressure |
| `huffman-02` | 1.25x | 1.91x | 0.0664s | 479 | 20 | 12 | 37 | 55 | 0 | missing arithmetic bit-op idiom recognition |
| `03_sort1` | 1.85x | 1.55x | 0.0525s | 366 | 22 | 15 | 29 | 36 | 0 | missing repeated digit-extraction CSE/specialization |
| `01_mm2` | 1.38x | 1.51x | 0.0700s | 168 | 5 | 3 | 16 | 22 | 0 | missing inner-loop unrolling / peeling |
| `crypto-3` | 1.19x | 1.38x | 0.0304s | 595 | 18 | 50 | 28 | 52 | 2 | lower priority bit idioms / pressure |
| `transpose2` | 1.17x | 0.38x | 0.0412s | 132 | 4 | 4 | 14 | 19 | 0 | not currently high priority |

## Findings

### 1. `03_sort*`: repeated digit extraction after inlining

Payoff: high ratio gap in the sort family; `03_sort1` is the worst yoolang/GCC ratio in the
fresh report at `1.85x`.

Evidence:

- Source uses `getNumPos(num, bitround)` repeatedly in `radixSort`.
- OIR inlines `getNumPos` many times, but leaves repeated loops that divide by 16 up to
  `bitround` and then compute `% 16`.
- Cost model for `03_sort1`: OIR calls drop from 14 to 7 after inlining, but OIR `int_div_rem`
  rises from 3 to 17 and branches from 41 to 44.
- Final assembly lowers signed `/16` and `%16` without hardware `div`, but repeats the same
  digit-extraction loop bodies many times inside the hot permutation loop.
- GCC still performs signed-correct division-by-16 sequences, but reuses the digit result more
  aggressively in the hot loop.

Classification: missing OIR cleanup/specialization after inlining.

Suggested next task:

- Add value-numbering or local CSE that can reuse equivalent inlined `getNumPos(x, bitround)`
  results across the same basic-block region and across simple diamonds.
- Consider a range-proven digit extraction idiom: for values proven nonnegative, canonicalize
  repeated `x / 16^k % 16` into a shift/mask-like representation or at least compute it once.
  Keep the legality invariant range-based; do not special-case radix sort.

### 2. `huffman-*`: arithmetic implementations of bit operations stay as loops

Payoff: high against Clang; all three huffman cases are about `1.85x-1.91x` slower than Clang.

Evidence:

- Source implements `_and`, `_or`, and `_xor` by 32-iteration loops using `% 2`, `/ 2`, and
  accumulating powers of two.
- OIR inlines `_and`/`_or` into `read_bits`, but keeps the 32-iteration arithmetic loops.
- `huffman-02` final MIR has 479 instructions, 37 branches, 55 jumps, and 21 calls.
- Compiler assembly for `read_bits` contains explicit inlined `_or` and `_and` loop bodies.
- GCC produces much more compact const-propagated read-bit paths and optimized modulo tests for
  output filtering.

Classification: missing general arithmetic bit-op idiom recognition.

Suggested next task:

- Add a semantics-preserving OIR idiom recognizer for loop-defined bitwise operations over fixed
  32-bit iteration counts: `and`, `or`, `xor`, and logical masks built from `% 2`, `/ 2`, and
  `power *= 2`.
- If direct bitwise OIR operations do not exist, introduce or lower to target-specific MIR idioms
  only after a proof that the arithmetic loop is equivalent for 32-bit signed values.
- Also add modulo-by-small-constant compare lowering for output filters such as `idx % 5 == 0`.

### 3. `conv2d-*`: fixed 5x5 kernel loops remain rolled with per-tap guards

Payoff: high absolute time. `conv2d-1` is the second slowest yoolang case at `0.4814s` and is
`1.46x` slower than Clang.

Evidence:

- Source has nested `kr < 5` and `kc < 5` loops with boundary checks.
- OIR keeps `kr` and `kc` loops, the four boundary predicates, and the 5x5 tap traversal.
- Final compiler assembly is still a rolled `kr/kc` loop with per-tap guard evaluation.
- GCC assembly expands the 5x5 stencil into straight-line tap code with boundary checks grouped
  around rows/columns.
- Cost model shows current passes did useful work, but loop unswitch rejected eight candidates for
  code growth and no unroll/specialize pass covers the fixed small loop shape.

Classification: missing fixed-small-loop unroll and boundary-peeling/specialization pass.

Suggested next task:

- Add an OIR pass for small constant trip-count loops with low side-effect complexity. Start with
  trip counts up to 5 or 8 and require a static code-growth cap.
- Follow with boundary peeling or guard clustering for stencil-like loops where the inner body is
  guarded by affine row/column range checks.
- Use the cost model to gate code growth, but record dynamic loop trip-count benefit rather than
  only static size.

### 4. `01_mm*`: inner matrix loop remains scalar and rolled

Payoff: medium. `01_mm2` is `1.38x` slower than GCC and `1.51x` slower than Clang.

Evidence:

- Source has a dense inner `j` loop over row elements guarded by `A[i][k] == 1`.
- OIR lowers zero initialization to a `memzero` pattern, so the obvious memset opportunity is
  already covered.
- Final compiler assembly still performs one element per iteration in the inner `j` loop.
- GCC emits a more specialized loop body that handles at least one peeled iteration and a tighter
  row traversal.

Classification: missing or conservative loop unrolling/peeling for simple strided array loops.

Suggested next task:

- Add OIR or MIR loop unrolling for small/medium simple memory loops where the body has one
  multiply-add recurrence and aligned unit-stride accesses.
- Keep the legality invariant alias-based and stride-based; do not depend on matrix benchmark
  identity.

### 5. `knapsack_naive-*`: aggressive recursive inlining creates register pressure

Payoff: medium versus GCC, but yoolang is much faster than Clang on this family. Treat carefully.

Evidence:

- Source is recursive and branch-heavy.
- OIR expands the function heavily: OIR static cost grows from 49 before optimization to 525 after
  OIR optimization; calls grow from 12 to 30 in the optimized OIR snapshot due to recursive inline
  expansion shape.
- MIR metrics show the issue appears at register allocation: pre-RA has 754 instructions, 52 loads,
  2 stores, 0 spills; post-RA has 815 instructions, 94 loads, 21 stores, 17 spills; final still has
  700 instructions, 93 loads, 21 stores, 17 spills.
- Compiler assembly has a 176-byte frame and many callee-saved registers/spill slots. GCC also
  partially specializes recursion but has a smaller frame in the inspected path.

Classification: conservative inlining / backend pressure gap, not a missing algorithmic pass.

Suggested next task:

- Tune recursive inlining cost to account for post-RA spill risk and live-value growth. The current
  cost model accepted 19 inlines and 40 LICM decisions for this case; use max-live growth and
  recursive depth as guardrails.
- Add an optimization-specific regression check comparing `knapsack_naive-*` final spills/stack
  slots before and after any inlining-cost change.

### 6. `crypto-*`: bit-rotation idioms and stack pressure are secondary

Payoff: lower absolute time but recurring pattern with huffman.

Evidence:

- Source expresses rotates as `x * C + x % C` and `_xor`/`_or` via arithmetic helper functions.
- OIR has many `srem` and multiply/add rotate idioms in `pseudo_md5` and `pseudo_sha1`.
- Final MIR for `crypto-3` has 595 instructions, 50 stores, 2 spills, and 8 stack slots.
- Cost model reports 144 `AlgebraicSimplify` proof timeouts, so algebraic simplification is trying
  but not proving many local rewrites.

Classification: lower-priority companion to bit-op/rotate idiom recognition.

Suggested next task:

- Reuse the huffman bit-op task for rotate idioms such as `x * 2 + x % 2`, `x * 32 + x % 32`, and
  fixed-width rotate-like arithmetic when a 32-bit equivalence proof exists.
- Add targeted proof shortcuts for known algebraic rotate masks instead of relying on generic SMT
  proofs for every occurrence.

### 7. `shuffle*`: high runtime but small compiler gap

Payoff: low priority.

Evidence:

- `shuffle1` is the slowest yoolang case at `0.6377s`, but only `1.02x` slower than GCC and
  `1.15x` slower than Clang.
- Final MIR has only 184 instructions, no spills, and no obvious backend pressure issue.
- The hot work is hash-table pointer chasing over large global arrays. GCC and yoolang assembly are
  broadly similar in memory-load/store shape.

Classification: not currently high priority.

Suggested next task: no immediate compiler task unless a future report shows a larger gap.

### 8. `transpose2`: old loop-bound gap is fixed in current tree

Payoff: low priority in this fresh run.

Evidence:

- Current optimized OIR contains `loop.bound.tight`, `loop.bound.plus`, and `loop.bound.merge` in
  both the standalone and inlined transpose bodies.
- Final compiler assembly contains matching `loop.bound` labels and no longer scans the full
  triangular skipped region in the old shape.
- Current time is `0.0412s` versus GCC `0.0353s` and Clang `0.1088s`.

Classification: not currently high priority; the loop-bound-tightening pass is effective here.

## Prioritized Next Tasks

1. OIR repeated digit-extraction CSE/specialization for `03_sort*`.
2. OIR/MIR bit-operation idiom recognition for loop-defined `_and`/`_or`/`_xor` and rotate-like
   arithmetic, covering `huffman-*` first and `crypto-*` second.
3. Fixed-small-loop unroll plus stencil boundary guard clustering for `conv2d-*`.
4. Strided scalar loop unroll/peeling for `01_mm*` and similar matrix loops.
5. Recursive inlining cost-model guardrails using post-RA spill-risk evidence from
   `knapsack_naive-*`.

All suggested tasks are general compiler optimizations. None requires recognizing benchmark names,
input sizes, function names, or expected outputs.
