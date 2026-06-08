# Task: Many Mat Cal Loop Fixes

Status: ready_for_review
Created: 2026-06-08
Last update: 2026-06-08
Owner: Codex
Branch: task/many-mat-cal-loop-fixes
Base commit: f199536

## Goal

Implement general, semantics-preserving compiler optimizations that address the issues found in `Many Mat Cal Performance Gap Attribution`:

1. Remove the redundant integer induction update/compare from hot counted pointer loops after address strength reduction, so scalar pointer-walk loops can use pointer/end style exits when legal.
2. Lower counted scalar store loops with a repeated byte pattern, especially `int -1` and `int 0`, to a generic memset-like operation instead of scalar stores.

The expected measurable target is improved `many_mat_cal-1/2/3` performance versus Clang++ `-O3` without causing important regressions in the focused and full performance suites.

## Problem Inventory

Primary issue: counted pointer loops keep a redundant integer IV.

- In the matrix multiply inner loop, OIR already contains pointer phis for `C.1.addr.ptr` and `A.1.addr.ptr`.
- The loop exit still uses `%k.loop + 1 < %v0`.
- Final yoolang assembly uses 8 steady-state instructions: two loads, two pointer bumps, one integer IV bump, `mulw`, `addw`, and `blt`.
- GCC/Clang scalar RISC-V hot loops use pointer-end exits and have 7 steady-state instructions.
- Nearest stage from attribution: `OIROptimizationPipelinePass`, specifically `OIRLoopStrengthReductionPass::reduce_gep_strength`, though implementation may be safer in MIR PreRA because OIR currently disallows pointer `icmp`.

Secondary issue: repeated-byte store loops are not lowered.

- Timed `-1` fill loops remain scalar `sw -1` loops in yoolang.
- GCC/Clang lower those loops to `memset(..., 255, T * 4)`.
- Existing `lower_counted_zero_store_loops_to_memzero` only accepts zero stores through `is_zero_store_value` and emits `MemZeroInst`.
- Existing `MemZeroInst` lowering can already call `memset`, but `AsmPrinter::emit_memset_call` hardcodes `a1 = 0`.

Explicit non-issues from attribution:

- Do not treat this as a register allocator task. Metrics showed `0` spills, `0` stack slots, and identical `pre-ra` / `post-ra` counts.
- Do not treat this as vectorization. GCC/Clang emitted scalar RISC-V loops for the relevant hot path.
- Do not focus on division by 3. yoolang already uses a magic-multiply lowering; it was not the primary gap.
- Do not chase a GCC gap for this case. Current focused result is compiler geomean `1.03x` vs GCC and `0.76x` vs Clang++.

## Non-goals

- Do not recognize `many_mat_cal`, filenames, function names, global names, input sizes, input values, or expected outputs.
- Do not implement matrix-specific algebra, loop interchange, blocking, vectorization, or benchmark-specific runtime shortcuts.
- Do not weaken performance scripts, expected outputs, test filters, verifier rules, or runtime semantics.
- Do not introduce pointer `icmp` into OIR unless the OIR verifier, printer, lowering, and tests are deliberately extended in the same patch queue.
- Do not change register allocation to mask the issue unless later evidence contradicts the current no-spill attribution.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [ ] YIR
- [x] OIR
- [x] MIR
- [x] ASM
- [ ] Runtime
- [x] Test infrastructure
- [x] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Attribution source task: yes
- Source anchors: max 8 keep=yes anchors; read narrow ranges before whole files.
- Test anchors: max 3 focused FileCheck-style tests.

Do not read unless explicitly needed:

- Parser/frontend implementation files.
- Runtime implementation outside `memset` linkage behavior.
- Unrelated performance cases before focused `many_mat_cal` and a small perf smoke.
- Full generated perf artifacts before a focused run identifies the artifact.

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required before resuming |
| `docs/tasks/2026-06-08-many-mat-cal-perf-gap.md` | full | source attribution and measured evidence | yes | contains timing rows, MIR metrics, and assembly conclusions |
| `docs/tasks/README.md` | active task table | register this task | yes | update when status advances |
| `src/main/main.cpp` | `add_oir_pipeline`, `add_mir_pipeline` | pass order and diagnostic stage boundaries | yes | optimized pipeline is `-O1` |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | pass list | decide where OIR loop/memset transforms run | yes | existing `reduce_gep_strength` and memzero calls are here |
| `src/pass/oir/OIRLoopStrengthReductionPass.cpp` | LSR candidate/apply code | primary fix target or source of metadata for MIR fix | yes | currently creates pointer phis but not pointer-end exits |
| `src/pass/oir/OIRLoopTransforms.cpp` | zero-store loop matcher | repeated-byte store-loop lowering target | yes | currently matches only zero stores |
| `include/oir/OIR.h` | `MemZeroInst`, builder APIs | IR representation for generalized memset | yes | likely needs `MemSetInst` or generalized memory set API |
| `src/oir/OIR.cpp` | verifier/printer/IRBuilder | OIR legality and pointer compare restriction | yes | `icmp` operands must be integer |
| `src/pass/oir/OIRToMIRVRegLowerer.cpp` | `lower_memzero` | optimized lowering path for memory set operation | yes | update if adding generic memset |
| `src/pass/oir/OIRToMIRStackLowerer.cpp` | `lower_memzero` | non-vreg lowering path for memory set operation | yes | mirrored vreg lowering operand order |
| `src/mir/AsmPrinter.cpp` | `emit_memzero`, `emit_memset_call` | final assembly lowering | yes | currently hardcodes memset value `0` |
| `src/pass/mir/MIRPointerLoopExitPass.cpp` | full | MIR PreRA redundant-IV pointer-exit cleanup | yes | new narrow matcher; rejects bounds defined in loop blocks |
| `src/pass/mir/MIRPeepholePipelinePass.cpp` | pass order | schedule pointer-exit cleanup after jump cleanup | yes | PreRA only |
| `test/ir/oir_memzero_loop.sy` | full | existing memzero FileCheck pattern | yes | extend or add sibling test for `-1`/repeated-byte memset |
| `test/ir/mir_pointer_loop_exit.sy` | full | focused FileCheck for redundant IV compare cleanup | yes | checks pointer `BNE` and no loop `BLT` |

## Branch

Decision: create a task branch for implementation.

Reason:

```text
This follow-up will likely touch OIR IR classes, OIR loop transforms,
MIR/ASM lowering, and tests. Keep the implementation isolated on a named
task branch.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git checkout -b task/many-mat-cal-loop-fixes
```

## Invariants And Risks

Correctness invariants:

- All optimizations must be general and semantics-preserving for all matching SysY/OIR/MIR programs.
- For redundant-IV compare cleanup, the replacement branch must have exactly the same trip count as the original counted loop on every reachable path.
- Only remove an integer IV when all remaining uses are replaced or proven dead by existing DCE.
- Preserve zero-trip behavior. If the original loop has an entry guard, the transformed loop must preserve it.
- For memory set lowering, only use memset when every stored byte has the same value. Safe `i32` examples: `0x00000000`, `0xffffffff`, `0x01010101`, `0x7f7f7f7f`.
- Byte count must be non-negative and must equal element count times element size.
- Memory set lowering must preserve aliasing/modref semantics in OIR analyses.

Contest / compliance constraints:

- No testcase-name, function-name, global-name, filename, input-size, or input-value special casing.
- No hardcoded output or shortcut for `many_mat_cal`.
- No undefined-behavior assumptions beyond existing IR semantics.

Risk areas:

- OIR pointer comparison is currently illegal: `src/oir/OIR.cpp` verifies `icmp` operands are integer. Prefer a MIR PreRA branch cleanup for pointer-end compares unless deliberately extending OIR pointer comparison support.
- A generic memory set operation crosses multiple layers: OIR representation, verifier/printer, cloning/inline helpers, memory analyses, OIR-to-MIR lowering, MIR representation if needed, and ASM printing.
- Calling `memset` can clobber caller-saved registers. Ensure MIR call notes/liveness behavior matches existing `MemZero` behavior.
- Dynamic byte counts need the same entry-guard handling as current memzero lowering.
- Performance can regress if the induction cleanup materializes a costly end pointer in loops where the removed IV was not hot enough. Gate the transform tightly and measure.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P0 | Reproduce baseline on the implementation branch | `build/perf-ci/perf-report.md`, stage dumps | `xmake`; focused `compare_perf.py`; direct emits for `many_mat_cal-1` | done | Baseline focused report: GCC geomean `0.93x`, Clang++ `0.81x`; `many_mat_cal-1/2/3` compiler times `0.1709/0.1831/0.1546s`; final metrics total `696` instrs, `15` stores, `0` spills. |
| P1 | Add or extend tests for repeated-byte store-loop lowering | `test/ir/oir_memzero_loop.sy` | FileCheck OIR/MIR/ASM | done | Covers zero memzero, `-1` as byte `255`, dynamic byte count, and non-repeated `258` staying scalar. |
| P2 | Introduce a generic memory set representation/lowering | `include/oir/OIR.h`, `src/oir/OIR.cpp`, OIR clone/inline helpers, OIR-to-MIR lowering, MIR verifier/ASM/RA | OIR verifier, MIR stage, ASM FileCheck | done | Generalized existing `MemZeroInst` to carry constant byte value; printer keeps `memzero` for byte `0` and prints `memset` for non-zero. MIR `MEMZERO` operands are `addr, byte_value, byte_count`. |
| P3 | Generalize counted store-loop matcher from zero-only to repeated-byte constants | `src/pass/oir/OIRLoopTransforms.cpp` | FileCheck from P1; full e2e | done | Only repeated-byte i32 constants or zero float/aggregate are lowered; arbitrary constants are rejected. |
| P4 | Add focused redundant-IV loop-exit cleanup test | `test/ir/mir_pointer_loop_exit.sy` | FileCheck MIR/ASM | done | Checks a generic two-load pointer-walk loop branches with pointer `BNE` instead of counted `BLT`. |
| P5 | Implement redundant counted-IV compare cleanup | `src/pass/mir/MIRPointerLoopExitPass.cpp`, `src/pass/mir/MIRPeepholePipelinePass.cpp` | MIR verifier, ASM stage, focused/full e2e | done | Implemented in MIR PreRA. Matcher requires zero-start unit IV, single-use IV step, pointer progression by power-of-two stride, at least two memory accesses, and loop-invariant branch bound. It computes end pointer from `max(bound, 1)` to preserve do-while semantics. |
| P6 | Focused performance verification and regression check | reports under `build/perf-ci` | focused `many_mat_cal`; `PERF_MAX_CASES=3`; full `test/performance`; full optimized tests | done | Focused many_mat final: GCC geomean `1.07x`, Clang++ `0.92x`; full `test/performance` 60/60 PASS, GCC `0.90x`, Clang++ `0.98x`; full `--suite all --o1` 1423 PASS, 1 SKIP. |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | Final build ok. |
| FileCheck memory set | `python3 scripts/run_tests.py --suite filecheck --filter oir_memzero_loop --jobs 1` | yes | PASS | Covers OIR `memzero`/`memset`, MIR byte operand, ASM `memset` call and inline non-zero fill loop. |
| FileCheck LSR/loop exit | `python3 scripts/run_tests.py --suite filecheck --filter lsr --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter mir_pointer_loop_exit --jobs 1` | yes | PASS | Existing LSR test passed; new MIR pointer-exit test passed. |
| OIR/MIR/ASM direct focused emits | direct `--emit-oir`, `--emit-mir-stage=pre-ra`, and `-S` for focused tests | yes | PASS | `run_tests.py` stage discovery deliberately skips `/test/ir/`; direct compiler emits succeeded for the FileCheck tests. |
| Full FileCheck | `python3 scripts/run_tests.py --suite filecheck --jobs 1` | yes | PASS | 19 passed. |
| Full stage+e2e | `python3 scripts/run_tests.py --suite stage --suite e2e --jobs 1 --o1` | yes | PASS | 1388 passed, 0 failed, 1 skipped. Initial crypto-related failures exposed a bound-dominance bug in the MIR matcher; fixed by rejecting bounds defined in header/backedge and reran PASS. |
| Focused e2e | `python3 scripts/run_tests.py --suite e2e --filter many_mat_cal --jobs 1 --o1` | if supported by filter | PASS | Covered by full stage+e2e and focused performance; many_mat e2e rows passed in full run. |
| Focused performance | `PERF_TEST_DIRS=test/performance/many_mat_cal-1.sy,test/performance/many_mat_cal-2.sy,test/performance/many_mat_cal-3.sy python3 scripts/compare_perf.py` | yes | PASS | Final focused report generated `2026-06-08 13:58:42 UTC`: GCC geomean `1.07x`, Clang++ `0.92x`; compiler times `0.1428/0.1453/0.1450s`. |
| Perf smoke | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=3 python3 scripts/compare_perf.py` | yes | PASS | 3/3 PASS; smoke only. |
| Full performance | `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` | yes | PASS | 60/60 PASS; report generated `2026-06-08 14:03:41 UTC`; geomean speedup GCC `0.90x`, Clang++ `0.98x`; QEMU instruction count disabled. |
| Full optimized tests | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | yes | PASS | 1423 passed, 0 failed, 1 skipped. |

## Expected Evidence

Before claiming success, record:

- Focused timing rows for `many_mat_cal-1/2/3` before and after.
- MIR stage metrics before and after, especially final instructions, branches, jumps, loads/stores, spills, and stack slots.
- Assembly snippet for the matrix multiply inner loop showing whether the redundant integer IV update/compare was removed.
- Assembly or FileCheck evidence showing `-1` counted stores lower to a memset byte value of `255`.
- Any regressions in `test/performance` geomean or important individual cases.

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Implement pointer-end exits in OIR | Keeps the transform near LSR where the relation is known | risky unless OIR pointer `icmp` is deliberately added; current verifier rejects non-integer `icmp` |
| Implement redundant-IV cleanup in MIR PreRA | Avoids changing OIR pointer compare semantics and works on GPR pointer values | preferred initial route, but must pattern-match tightly to avoid extra setup cost |
| Only add `-1` memzero special case | Faster to implement | rejected; implement repeated-byte constant lowering generally |
| Leave `MemZeroInst` and add a separate `MemSetInst` | Clear semantics for non-zero byte value | likely best if code churn is acceptable |
| Generalize `MemZeroInst` to include byte value | Smaller instruction set | acceptable only if existing memzero tests/readability remain clear |
| Try register allocation changes | RA often affects loops | rejected by attribution metrics: no spills and no RA-stage degradation |

## Change Log

- 2026-06-08: created scoped follow-up task from `many_mat_cal` performance attribution, with primary redundant-IV cleanup and secondary repeated-byte memset lowering.
- 2026-06-08: implemented repeated-byte counted-store lowering through generalized memzero byte values, added MIR PreRA pointer-loop exit cleanup, fixed a crypto e2e miscompile by requiring loop-invariant branch bounds, completed correctness and performance gates, and marked ready for review.

## Open Questions

- Resolved: redundant-IV cleanup lives in MIR PreRA; OIR pointer comparisons were not introduced.
- Resolved: generic memory set is represented by generalized `MemZeroInst` with constant byte value. Zero still prints as `memzero`; non-zero prints as `memset`.
- Resolved for this patch: pointer-exit cleanup is gated to loops with at least two memory accesses, zero-start unit IV, single-use removable IV, power-of-two pointer stride, and branch bounds not defined in the header/backedge. End pointer uses `max(bound, 1)` to preserve do-while semantics.

## Handoff Note

Current state:

- Implementation complete on `task/many-mat-cal-loop-fixes`; ready for review.
- Repeated-byte store loops lower to byte-valued `MEMZERO`/`memset` when the stored i32 pattern repeats in all bytes.
- Hot `many_mat_cal` matrix multiply loop now uses pointer-end `bne` and no longer carries the redundant integer IV in the steady-state body.
- Full optimized correctness passed: `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` -> 1423 passed, 1 skipped.
- Full `test/performance` compare passed: 60 cases, GCC geomean `0.90x`, Clang++ `0.98x`; focused many_mat improved to GCC `1.07x`, Clang++ `0.92x`.

Next action:

- Review the implementation diff, especially `src/pass/mir/MIRPointerLoopExitPass.cpp` safety predicates and the generalized `MemZeroInst` operand contract.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-08-many-mat-cal-loop-fixes.md`
- `include/oir/OIR.h`
- `src/oir/OIR.cpp`
- `src/pass/oir/OIRLoopTransforms.cpp`
- `src/pass/oir/OIRToMIRVRegLowerer.cpp`
- `src/pass/oir/OIRToMIRStackLowerer.cpp`
- `src/mir/AsmPrinter.cpp`
- `src/pass/mir/MIRPointerLoopExitPass.cpp`
- `test/ir/oir_memzero_loop.sy`
- `test/ir/mir_pointer_loop_exit.sy`
