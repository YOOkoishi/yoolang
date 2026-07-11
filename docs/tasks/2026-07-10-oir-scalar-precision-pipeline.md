# Task: OIR Scalar Precision Pipeline

Status: ready_for_review
Created: 2026-07-10
Last update: 2026-07-10
Owner: Codex
Branch: `master` (current branch)
Base commit: `7c7bc1a`

## Goal

Close the highest-value scalar-optimization precision gaps in yoolang by adding a conservative
OIR combination analogous to LLVM's IPSCCP/function attributes/ArgumentPromotion/Reassociate/
ConstraintElimination/BDCE/MemCpyOpt sequence, adapted to the operations and whole-module model
that OIR actually supports.

## Non-goals

- Do not add `-O2`/`-O3` command-line modes or clone LLVM's entire pass manager.
- Do not add unsupported LLVM IR concepts such as poison, arbitrary-width vectors, structs, or a
  general memcpy intrinsic merely to copy pass names.
- Do not change YIR, MIR, the SysY ABI, or benchmark-specific behavior.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [ ] YIR
- [x] OIR
- [ ] MIR
- [ ] ASM
- [ ] Runtime
- [x] Test infrastructure
- [x] Performance

## Context Budget

Initial read budget:

- Task file and `docs/task-system.md`: yes
- Related docs: `docs/llvm/riscv_o3_pipeline_report.md` and
  `docs/performance-optimization-opportunity-audit.md`
- Source/script anchors: at most 8 keep=yes anchors; related implementation files may be sampled
  by symbol before promotion
- Large-file rule: read only named ranges unless the whole implementation is the edit target

Do not read unless explicitly needed:

- MIR register allocation and assembly emission internals
- Polyhedral implementation
- Runtime implementation
- Benchmark expected outputs

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/llvm/riscv_o3_pipeline_report.md` | 60-180 | reference pass ordering | yes | module simplification and nested function cleanup |
| `docs/performance-optimization-opportunity-audit.md` | findings | existing evidence and anti-overfit constraints | no | priority context only |
| `include/oir/OIRScalarOpt.h` | full | shared transform API/stats | yes | edit anchor |
| `include/oir/OIRAnalysis.h` | function memory analysis | existing inferred effects | yes | extend only if needed |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | full | integration and cleanup windows | yes | edit anchor |
| `src/pass/oir/OIRSCCPPass.cpp` | full | function-local SCCP baseline | yes | IPSCCP integration anchor |
| `src/pass/oir/OIRValueRange.cpp` | range propagation | current path-constraint precision | no | retain API only |
| `src/pass/oir/OIRDeadStoreEliminationPass.cpp` | memory transforms | overlap with MemCpyOpt-like work | no | avoid duplicating existing DSE |
| `test/ir/oir_o3.sy` | full | OIR FileCheck style | yes | tests may use a new focused file |

## Branch

Decision: current branch; no branch switch.

Reason:

```text
The shared worktree already contains an unrelated untracked user document. Keep that state intact
and make a reviewable local diff without changing branches underneath other collaborators.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
```

## Invariants And Risks

Correctness invariants:

- Interprocedural constants are propagated only when every possible use is a well-typed direct
  call inside the closed module.
- Pointer argument promotion must not speculate a load that was conditional or preceded by a
  memory side effect in the callee.
- Integer reassociation follows OIR's existing 32-bit wrapping semantics; floating-point
  expressions are not reassociated.
- Interprocedural floating constants must have identical bit patterns; `+0.0f` and `-0.0f` are
  never treated as interchangeable.
- Constraint facts are used only on CFG edges proven by dominance.
- Demanded-bit rewrites may alter only bits not observable by any live use.
- Memory-intrinsic combination must have exact base/offset/size evidence and no intervening memory
  observer.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes,
  or expected outputs.

Risk areas:

- Function signature mutation and call-site ordering.
- Recursive/cyclic call graphs.
- Integer overflow and signed comparison implications.
- Alias and range precision around memory combination.
- Compile-time growth from repeated cleanup rounds.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add interprocedural constants and inferred return/effect use | SCCP/analysis/header | focused OIR FileCheck | done | multiple closed-world direct calls; leaf acyclic returned-value inference; existing iterative ModRef remains the function-effect engine |
| P2 | Add conservative scalar pointer argument promotion | new OIR scalar combination source/header | focused OIR FileCheck | done | unconditional entry-prefix fixed-offset loads only; call verifier added |
| P3 | Add integer reassociation, relational constraint elimination, and demanded bits | new OIR scalar combination source/header | focused OIR FileCheck | done | no FP reassociation; constraint facts require a conservatively edge-dominating unique-predecessor successor |
| P4 | Add exact-range memset/store combining and pipeline cleanup windows | new source/pipeline | focused OIR + e2e | done | OIR has generalized memset/memzero, not memcpy; combines exact adjacent/covered ranges and aligned word-sized repeated-byte scalar stores |
| P5 | Broad verification and durable documentation | tests/task docs | full optimized tests | done | 1398 stage/e2e checks passed; FileCheck binary unavailable |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Baseline build | `xmake -y` | yes | PASS | completed before edits |
| Build | `xmake f -m release -y && xmake -y` | yes | PASS | final release build passed |
| OIR infrastructure | `python3 scripts/run_tests.py --suite infra --jobs 1` | yes | PASS | nine C++ cases: use lists, MemorySSA, call-signature verifier, and signed-zero constant identity |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_scalar_precision --jobs 1` | yes | SKIP | compiler succeeded; shell failed because `FileCheck` is not installed |
| Focused direct OIR | `compiler --emit-oir -O1 test/ir/oir_scalar_precision.sy` | yes | PASS | inspected positive and conservative negative shapes for all six new transforms |
| All IR stage smoke | compile all 33 `test/ir/*.sy` at OIR, MIR, and ASM with `-O1` | yes | PASS | 99 compiler/verifier invocations passed |
| Full stage + E2E | `python3 scripts/run_tests.py --suite stage --suite e2e --jobs 4 --o1` | yes | PASS | 1398 passed, 0 failed, 1 skipped (`shuffle1` input-size gate) |
| Focused differential E2E | current vs base `7c7bc1a` on `oir_scalar_precision.sy`, 3 input sets | yes | PASS | stdout and process return status matched for all inputs under RISC-V/QEMU |
| Warning/syntax check | `c++ -std=c++17 -Wall -Wextra -Iinclude -fsyntax-only <six new sources>` | yes | PASS | no diagnostics |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | yes | SKIP | superseded by full stage/e2e plus infra; filecheck component cannot run without `FileCheck` |
| Performance smoke | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=3 python3 scripts/compare_perf.py` | optional | PASS | 3/3 OK; geomean 1.13x vs GCC, 1.06x vs Clang++; MIR metrics OK |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Copy LLVM pass names as empty wrappers | Superficially matches the report | rejected; no measurable precision gain |
| Add each LLVM pass as an independent large subsystem | Maximum architectural similarity | rejected; mismatched to OIR and too broad for one safe patch |
| Add one OIR scalar precision combination with separable functions | Fits current monolithic `-O1` pipeline and allows focused invariants | chosen |

## Change Log

- 2026-07-10: created and scoped the task; baseline build passed; implementation in progress.
- 2026-07-10: implemented the adapted scalar precision sequence, call-signature verifier, and
  focused positive/negative regression coverage.
- 2026-07-10: found a constraint-edge dominance bug during testing (a rotated self-loop could use
  its own branch result as a fact); fixed it with a unique-predecessor edge-dominance guard and an
  explicit same-block exclusion.
- 2026-07-10: final release build, infra, 99 IR-stage compiles, 1398 stage/e2e checks, focused base
  differential, and 3-case performance smoke passed; status advanced to `ready_for_review`.
- 2026-07-10: final read-only review found that floating `==` merged `+0.0f` and `-0.0f` in
  interprocedural facts; switched constant identity to bitwise comparison and added a regression.
  The same review prompted a word-size/alignment guard for store-to-memset combining. Release,
  infra, and the full 1398-check stage/e2e gate passed again after both fixes.

## Open Questions

- Whether fixed-offset pointer argument promotion occurs often enough in the current corpus to keep
  enabled by default; correctness gates decide inclusion, performance decides later tuning.

## Handoff Note

Current state:

- The OIR `-O1` pipeline now has two bounded interprocedural windows and the scalar sequence
  `Reassociate -> ConstraintElimination -> loop transforms -> GVN -> SCCP -> BDCE -> ADCE ->
  memory combine -> DSE/DLE`.
- Existing `FunctionModRefAnalysis` supplies bottom-up read/write/side-effect attributes; the new
  interprocedural pass adds shared constant-argument and leaf returned-value facts without cloning.
- Argument promotion is deliberately limited to direct calls and unconditional fixed-offset loads.
- Since OIR has no memcpy instruction, the MemCpyOpt analogue works on its generalized
  memset/memzero instruction and repeated-byte store ranges; adding copy-loop IR remains a separate
  cross-layer task.

Next action:

- Review the diff. If exact FileCheck execution is required locally, install LLVM `FileCheck` and
  run the focused command in the verification matrix.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-10-oir-scalar-precision-pipeline.md`
- `include/oir/OIRScalarOpt.h`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`
- `src/pass/oir/OIRSCCPPass.cpp`
- `test/ir/oir_scalar_precision.sy`
