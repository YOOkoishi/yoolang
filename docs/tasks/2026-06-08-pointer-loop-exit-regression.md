# Task: Pointer Loop Exit Regression Fix

Status: ready_for_review
Created: 2026-06-08
Last update: 2026-06-08
Owner: Codex
Branch: task/pointer-loop-exit-regression
Base commit: ffd20d0

## Goal

Fix performance regressions introduced by the MIR PreRA pointer-loop-exit cleanup while keeping the `many_mat_cal` steady-state loop improvement. The fix must make the transformation profitable and avoid adding costly end-pointer setup or register pressure in short or high-pressure loops.

## Investigation Evidence

User-reported regressions after `ffd20d0`:

- `test/bsb-final/2025-PDZ-59.sy`: current `0.2174s`, baseline `0.1270s`
- `test/performance/crypto-1.sy`: current `0.2163s`, baseline `0.1267s`
- `test/performance/crypto-2.sy`: current `0.1571s`, baseline `0.0950s`
- `test/bsb-final/2025-N3A-33.sy`: current `0.1588s`, baseline `0.0967s`
- `test/performance/crypto-3.sy`: current `0.1008s`, baseline `0.0623s`
- `test/bsb-final/2025-EQV-46.sy`: current `0.0993s`, baseline `0.0629s`
- Smaller regressions: `2025-A62-49`, `crc3`, `2025-OK9-56`, `2025-EAR-48`

Local reproduction on 2026-06-08 did not reproduce the same timing deltas, but did reproduce the codegen regression shape:

- Compared `HEAD^` (`f199536`) against `HEAD` (`ffd20d0`) using the same 10-case focused `compare_perf.py` set.
- `crypto-1`/`crypto-2`/`crypto-3`/`2025-PDZ-59`/`2025-N3A-33`/`2025-EQV-46` all share the same generated crypto program shape.
- For `crypto-1`, final metrics changed from `699` to `711` MIR instructions, jumps `57 -> 58`, loads `22 -> 23`, stores `50 -> 51`, spills `2 -> 3`, stack slots `8 -> 9`.
- For `2025-OK9-56`, final metrics changed from `277` to `309` MIR instructions and jumps `30 -> 32`.
- Assembly diff shows the new `MIRPointerLoopExitPass` replacing counted exits such as `addiw iv, iv, 1; blt iv, bound` with pointer-end `bne`, but inserting a hot preheader end-pointer sequence:

```asm
li t0, 1
slt t1, bound, t0
subw t2, t0, bound
subw t1, zero, t1
and t1, t2, t1
addw t1, bound, t1
slliw t1, t1, 2
add end, start, t1
```

Root cause: `materialize_end_pointer` always computes `start + max(bound, 1) * stride` to preserve do-while semantics. Many affected loops are normal guarded `while` bodies with an entry guard proving `bound >= 1` before the body is reached, so the max sequence is unnecessary. The extra end-pointer vreg also stays live across the loop and can increase GPR pressure enough to add spills.

Secondary finding: the generalized non-zero memset path is present in the same commit, but these regressions are better explained by pointer-loop-exit codegen. `HEAD^` already emitted `call memset` for the large zero initialization in crypto; the large diff is the pointer-exit rewrite and its setup/spill fallout.

## Non-goals

- Do not special-case `crypto`, `many_mat_cal`, `2025-*`, filenames, function names, array names, input sizes, or expected outputs.
- Do not revert repeated-byte `memset` support unless new evidence shows it is independently incorrect or broadly unprofitable.
- Do not weaken correctness tests, performance scripts, verifier checks, or runtime semantics.
- Do not hide regressions by excluding cases.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [ ] YIR
- [ ] OIR
- [x] MIR
- [x] ASM
- [ ] Runtime
- [x] Test infrastructure
- [x] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: `docs/tasks/2026-06-08-many-mat-cal-loop-fixes.md`
- Source/script anchors: max 6
- Large-file rule: read only named functions/ranges unless promoted below

Do not read unless explicitly needed:

- Parser/frontend files.
- Runtime implementation outside `memset` ABI behavior.
- Unrelated OIR/YIR optimization passes.

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/2026-06-08-many-mat-cal-loop-fixes.md` | `Goal`, `Patch Queue`, `Handoff Note` | source task that introduced the pass | yes | contains original invariants and verification gates |
| `src/pass/mir/MIRPointerLoopExitPass.cpp` | full | primary fix target | yes | `materialize_end_pointer` always emits max-bound setup |
| `src/pass/mir/MIRPeepholePipelinePass.cpp` | pass order | know where cleanup runs and iteration effects | yes | PreRA only |
| `src/pass/mir/MIRRegAllocPass.cpp` | liveness helpers around `compute_liveness` | possible pressure/profitability helper source | maybe | private today; extract only if needed |
| `test/ir/mir_pointer_loop_exit.sy` | full | existing positive FileCheck | yes | must keep many_mat-style benefit covered |
| `scripts/compare_perf.py` | case collection/report fields | focused verification | yes | use `PERF_TEST_DIRS` focused sets |

## Branch

Decision: create a follow-up task branch before code changes.

Reason:

```text
The fix changes a shared MIR peephole transform and profitability policy.
Keep it isolated from the already ready-for-review many-mat task branch.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git checkout -b task/pointer-loop-exit-regression
```

## Invariants And Risks

Correctness invariants:

- Pointer-end branch replacement must preserve the original loop trip count on every reachable path.
- If a cheap end pointer uses `start + bound * stride`, the pass must prove the loop body is only reached when `bound >= 1` or equivalent.
- If the pass cannot prove guarded entry, it must either keep the conservative `max(bound, 1)` formula and pass profitability checks, or skip the transform.
- Removing the integer IV is legal only when all remaining uses are replaced or proven dead by existing DCE.
- The transform must not increase live GPR pressure enough to introduce spills in common hot loops without a strong expected steady-state win.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.
- Only use general CFG, def-use, dominance/guard, instruction-cost, and liveness facts.

Risk areas:

- Guard proof can be wrong if it does not account for edge blocks and fallthrough.
- Profitability estimates can accidentally disable the `many_mat_cal` win.
- New liveness/pressure checks can become expensive if recomputed too often inside the peephole fixed point.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P0 | Reproduce and preserve evidence | `build/perf-ci` reports, task notes | focused 10-case `compare_perf.py`; direct assembly diff | done | Local timing did not reproduce user deltas, but codegen regression was reproduced and recorded above. |
| P1 | Add guarded-entry proof and cheap end-pointer materialization | `src/pass/mir/MIRPointerLoopExitPass.cpp` | MIR stage/FileCheck | done | Added generic positive-bound facts from constants, direct compare edges, and `slt zero,bound` condition edges. The pass now emits `start + bound * stride` only when the positive edge target dominates the loop preheader. |
| P2 | Add profitability and pressure gates | `src/pass/mir/MIRPointerLoopExitPass.cpp` | MIR stage/FileCheck | done | Disabled the conservative `max(bound,1)` path; skip small positive constant bounds `<= 8`, bounds with multiple defs, and loops with more than 6 loop-carried GPR moves. |
| P3 | Add regression and preservation tests | `test/ir/mir_pointer_loop_exit.sy` | FileCheck | done | Extended the test with a guarded dynamic dot loop that must use pointer `BNE`, and a small constant loop that must retain counted `BLT`. |
| P4 | Focused performance verification | reports under `build/perf-ci` | focused listed regressions plus `many_mat_cal-1/2/3` | done | Focused regression and preservation perf sets passed. Crypto/PDZ/N3A/EQV shape improved from bad `711` instrs / `58` jumps to `700` instrs / `57` jumps; many_mat stayed at `214` instrs and `0` spills per case. |
| P5 | Broad correctness/performance gate | no source beyond test updates | full optimized tests; `test/performance,test/bsb-final` perf | done | Full `stage+e2e`, full `all`, and full performance gates passed. QEMU dynamic instruction counting was disabled by default in the perf script. |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | Build succeeded after the MIR pass change. `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` also rebuilt successfully. |
| FileCheck pointer loop | `python3 scripts/run_tests.py --suite filecheck --filter mir_pointer_loop_exit --jobs 1` | yes | PASS | Guarded dynamic loop emits cheap pointer `BNE`; small constant loop keeps counted `BLT`. |
| MIR stage focused | `python3 scripts/run_tests.py --suite stage --stage mir --filter crypto --jobs 1 --o1` | yes | PASS | 3/3 passed. |
| E2E focused | `python3 scripts/run_tests.py --suite e2e --filter crypto --jobs 1 --o1` | yes | PASS | 3/3 passed. |
| Focused perf regressions | `PERF_TEST_DIRS=test/bsb-final/2025-PDZ-59.sy,test/performance/crypto-1.sy,test/performance/crypto-2.sy,test/bsb-final/2025-N3A-33.sy,test/performance/crypto-3.sy,test/bsb-final/2025-EQV-46.sy,test/bsb-final/2025-A62-49.sy,test/performance/crc3.sy,test/bsb-final/2025-OK9-56.sy,test/bsb-final/2025-EAR-48.sy python3 scripts/compare_perf.py` | yes | PASS | 10/10 passed. Crypto/PDZ/N3A/EQV final metrics: `700` MIR instrs, `57` jumps, `23` loads, `51` stores, `3` spills, `9` stack slots. `2025-OK9-56`: `278` instrs, `32` jumps, `0` spills. Residual crypto spill remains versus the older baseline noted above (`2` spills, `8` stack slots). |
| Focused perf preservation | `PERF_TEST_DIRS=test/performance/many_mat_cal-1.sy,test/performance/many_mat_cal-2.sy,test/performance/many_mat_cal-3.sy python3 scripts/compare_perf.py` | yes | PASS | 3/3 passed; each many_mat case ended at `214` MIR instrs and `0` spills/stack slots. |
| Full stage+e2e | `python3 scripts/run_tests.py --suite stage --suite e2e --jobs 1 --o1` | yes | PASS | 1388 passed, 0 failed, 1 skipped. `test/h_functional/29_long_line.sy` passed in 8.71s after replacing the earlier full dominator-set implementation with lazy reachability. |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | yes | PASS | 1423 passed, 0 failed, 1 skipped. Includes infra, FileCheck, poly, stage, and e2e. |
| Full performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | yes | PASS | 119 cases, 0 failed, total runtime 64.6859s. Geomean speedup: GCC `0.90x`, Clang++ `0.97x`. MIR metrics OK for 119 cases; final totals `35117` instrs, `3710` jumps, `180` spills, `234` stack slots. QEMU dynamic instruction count: DISABLED. |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Disable `optimize_pointer_loop_exits` entirely | Fastest safety rollback | acceptable emergency mitigation, but loses many_mat improvement |
| Raise `memory_access_count` threshold | Simple gate | rejected as primary fix; many_mat has a small memory-access count and would likely lose the target win |
| Only use cheap guarded formula, skip conservative max formula | Removes the worst setup sequence | likely good first implementation; keep conservative path only if profitability is proven |
| Add full RA-aware cost model | Most accurate | too broad for first fix; start with bounded pressure/savings checks |

## Change Log

- 2026-06-08: created scoped follow-up task from regression investigation.
- 2026-06-08: implemented guarded positive-bound proof, removed unconditional `max(bound,1)` end-pointer setup, and added small-constant / loop-carried-GPR profitability gates.
- 2026-06-08: replaced an initially expensive full dominator-set implementation with lazy reachability after `test/h_functional/29_long_line.sy` exposed a compiler-time timeout; reran full optimized correctness gates successfully.
- 2026-06-08: completed focused and full performance verification; task is ready for review with a documented residual crypto spill delta versus the older baseline.

## Open Questions

- Resolved for this patch: positive-bound proofs cover direct `BranchLT zero,bound`, false edge of `BranchGE zero,bound`, and branch edges using a single-def `SLT zero,bound` condition.
- Resolved for this patch: the conservative `max(bound,1)` path is disabled; pointer-loop-exit now requires a positive-bound proof before materializing the end pointer.
- Residual: crypto/PDZ-style cases still have `3` spills / `9` stack slots versus the older baseline note of `2` / `8`. The bad pointer-loop-exit instruction and jump growth is fixed, but fully recovering that spill likely needs a separate RA/pressure task.

## Handoff Note

Current state:

- Compiler fix is implemented in `src/pass/mir/MIRPointerLoopExitPass.cpp`.
- Regression/preservation coverage is implemented in `test/ir/mir_pointer_loop_exit.sy`.
- Root cause is attributed to `MIRPointerLoopExitPass` profitability and end-pointer materialization, not testcase-specific behavior.
- Local focused timing did not reproduce the user's large deltas, but final-code metrics and assembly diff showed the problematic codegen pattern. The updated pass fixes the bad `max(bound,1)` setup and short-loop rewrite shape while preserving the many_mat pointer-exit win.
- The task is `ready_for_review` on `task/pointer-loop-exit-regression`.

Next action:

- Review the MIR positive-bound proof and profitability thresholds.
- If the remaining crypto spill delta matters, open a follow-up RA/pressure task rather than expanding this pointer-loop-exit patch.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-08-pointer-loop-exit-regression.md`
- `docs/tasks/2026-06-08-many-mat-cal-loop-fixes.md`
- `src/pass/mir/MIRPointerLoopExitPass.cpp`
- `src/pass/mir/MIRPeepholePipelinePass.cpp`
- `test/ir/mir_pointer_loop_exit.sy`
