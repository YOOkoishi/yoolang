# Task: OIR Loop Bound Tightening

Status: ready_for_review
Created: 2026-06-12
Last update: 2026-06-12
Owner: Codex
Branch: `task/oir-loop-bound-tightening`
Base commit: 24b9739

## Goal

Implement the two highest-priority optimizations from the transpose2/T03-61 attribution:

- OIR monotonic continue / guard loop-bound tightening.
- OIR min/max/effective-bound canonicalization needed to materialize tightened loop bounds safely.

The target general pattern is a counted loop whose body starts with a monotonic guard such as `outer_iv < inner_iv`, where the skipped arm only reaches the common latch and therefore can be converted into a tighter trip-count bound.

## Non-goals

- Do not implement YIR/polyhedral triangular-domain recovery in this task.
- Do not implement MIR/ASM branch-layout cleanup in this task except what naturally follows from cleaner OIR.
- Do not special-case `transpose2`, `2025-T03-61`, function names, variable names, input sizes, filenames, or expected outputs.
- Do not rely on undefined signed overflow when forming `limit + 1` or `upper - 1`.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [ ] YIR
- [x] OIR
- [x] MIR
- [x] ASM
- [ ] Runtime
- [ ] Test infrastructure
- [x] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: 1, `docs/tasks/2026-06-12-transpose2-t03-61-perf-gap.md`
- Source/script anchors: max 8
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- parser, AST, runtime, unrelated MIR RA files
- YIR polyhedral transform internals; they are a follow-up, not this task
- benchmark directories beyond focused validation cases until the implementation is ready for broad perf

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/2026-06-12-transpose2-t03-61-perf-gap.md` | full | source attribution and pass requirements | yes | predecessor task |
| `docs/tasks/README.md` | full | active task registry | yes | keep in sync |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | full | hook the new OIR transform into the existing optimization pipeline | yes | place before/around CFG cleanup, VRP, GVN, DCE |
| `src/pass/oir/OIRLoopTransforms.cpp` | focused helper and driver ranges | implement monotonic guarded loop-bound tightening | yes | pass lives here; reuses local CFG/loop helpers |
| `src/pass/oir/OIRLoopStrengthReductionPass.cpp` | `1-220` | induction, preheader, latch, and pointer recurrence helper patterns | no | reference only; final patch has local matcher |
| `include/oir/OIRScalarOpt.h` | full | add pass declaration and stats field | yes | expose new transform through OIR pipeline |
| `src/pass/oir/OIRScalarOptUtils.cpp` | `Stats::{changed,message}` | stats counter plumbing | yes | reports `looptighten` |
| `include/oir/OIR.h` | `285-405`, `560-580` | confirm compare, branch, phi, and builder APIs | no | API confirmed |
| `src/oir/OIRAnalysis.cpp` | `81-150`, `748-900` | predicate helpers and SCEV/trip-count handling | no | reference only |
| `test/ir/oir_loop_bound_tightening.sy` | full | positive and live-out negative FileCheck coverage | yes | added in this task |
| `test/performance/transpose2.sy` | full | focused perf target and representative source pattern | yes | validation only; do not special-case |

## Branch

Decision: created and used

Reason:

```text
Implementation used task/oir-loop-bound-tightening. Pre-existing uncommitted docs task files from the predecessor attribution were preserved and not reverted.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
```

Observed:

```text
git status --short:
 M docs/tasks/README.md
 M include/oir/OIRScalarOpt.h
 M src/pass/oir/OIRLoopTransforms.cpp
 M src/pass/oir/OIROptimizationPipelinePass.cpp
 M src/pass/oir/OIRScalarOptUtils.cpp
?? docs/tasks/2026-06-12-oir-loop-bound-tightening.md
?? docs/tasks/2026-06-12-transpose2-t03-61-perf-gap.md
?? test/ir/oir_loop_bound_tightening.sy
git rev-parse --short HEAD: 24b9739
git branch --show-current: task/oir-loop-bound-tightening
```

## Invariants And Risks

Correctness invariants:

- Only tighten a loop when the removed/skipped iterations are proven side-effect-free except for the same latch recurrences that would have executed anyway.
- The guard must be monotonic with respect to the loop induction variable and loop step.
- The replacement bound must preserve zero-trip behavior and exit values used outside the loop.
- If materializing `limit + 1`, do it only on a path where `limit < upper`, so signed i32 overflow is impossible for the value actually computed.
- If materializing inclusive bounds, preserve signed comparison semantics exactly.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- PHI repair after changing predecessors or loop header conditions.
- Loops with live-out induction or pointer recurrences.
- Loops where the skipped arm has stores, calls, volatile-like effects, or memory operations.
- Non-unit or negative induction steps.
- Signed overflow when changing `< upper` plus `iv <= limit` into an effective bound.
- Register pressure if the min-bound diamond is unnecessarily inserted into hot code.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add effective-bound materialization helper for signed integer min-style bounds | `src/pass/oir/OIRLoopTransforms.cpp`, `include/oir/OIRScalarOpt.h` | focused OIR FileCheck | done | emits preheader-edge diamond/phi for `min(upper, limit + 1)` |
| P2 | Match monotonic continue/guard loops and tighten the counted-loop bound | `src/pass/oir/OIRLoopTransforms.cpp` | OIR stage + FileCheck | done | handles increasing i32 unit-step `iv < upper` plus `limit < iv` skipped arm |
| P3 | Hook pass into `OIROptimizationPipelinePass` with cleanup passes after it | `src/pass/oir/OIROptimizationPipelinePass.cpp`, `src/pass/oir/OIRScalarOptUtils.cpp` | `xmake`, OIR verifier | done | cleanup/simplify removes dead guard blocks after rewrite |
| P4 | Add regression tests and focused perf validation | `test/ir/oir_loop_bound_tightening.sy`, focused perf cases | stage/e2e/perf commands below | done | includes positive transform and live-out no-transform case |

## Implementation Sketch

Start with a narrow, legal matcher:

- Loop has one preheader, one header, one latch, and one increasing i32 induction PHI with step `+1`.
- Header condition is `iv < upper` or `iv <= upper`, with signed integer comparison.
- Body contains a conditional branch whose skipped arm reaches the common latch without stores, calls, memzero/memset, or other side effects.
- The skipped predicate is equivalent to `limit < iv` / `iv > limit`, where `limit` is loop-invariant for the inner loop. `limit` may be an outer-loop induction value.
- The kept arm is the only arm containing side effects.
- Latch recurrences for pointer PHIs and induction PHIs are identical in both semantic paths, or their final values are not used outside the loop.

For `iv < upper` plus active range `iv <= limit`, materialize:

```text
if (limit < upper) {
  effective_upper = limit + 1;
} else {
  effective_upper = upper;
}
while (iv < effective_upper) {
  kept_body;
}
```

The `limit + 1` add is only executed on the `limit < upper` edge, so for signed i32 values it cannot overflow. If the existing loop is `iv <= upper`, use an inclusive `effective_limit = min(limit, upper)` form instead.

If OIR later gains a `select` instruction, this can be canonicalized as a select; for this task, a preheader diamond plus PHI is acceptable and should be simplified by existing CFG/GVN/DCE where possible.

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | build ok |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_loop_bound_tightening --jobs 1` | yes | PASS | 1 passed |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter transpose2 --jobs 1 --o1` | yes | PASS | 1 passed |
| MIR/ASM stage | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter transpose2 --jobs 1 --o1` | yes | PASS | 2 passed |
| E2E focused | `python3 scripts/run_tests.py --suite e2e --filter transpose2 --jobs 1 --o1` | yes | PASS | 1 passed |
| Focused performance | `PERF_TEST_DIRS=test/performance/transpose2.sy,test/bsb-final/2025-T03-61.sy python3 scripts/compare_perf.py` | yes | PASS | latest focused run: `transpose2` compiler `0.1035s`, `T03-61` compiler `0.1086s` |
| Broader performance smoke | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=3 python3 scripts/compare_perf.py` | before handoff | PASS | 3 cases OK |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before ready_for_review | PASS | 1425 passed, 0 failed, 1 skipped |
| Full performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before ready_for_review if runtime allows | PASS | 119 cases, 0 failed, GCC geomean `0.92x`, Clang++ geomean `1.01x` |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| YIR triangular-domain recovery first | Could expose the domain before OIR lowering | rejected for this task; broader and already identified as follow-up |
| MIR branch cleanup first | Easier local peephole | rejected as primary fix; does not remove the wasted iterations |
| Add a first-class OIR `select` instruction | Cleaner long-term min/max representation | deferred; preheader diamond is enough to implement this task safely |
| Rewrite all guarded loops aggressively | More coverage | rejected initially; start with monotonic unit-step counted loops |

## Change Log

- 2026-06-12: created scoped implementation task for optimizations 1 and 2 from the transpose2/T03-61 attribution.
- 2026-06-12: implemented conservative OIR guarded-loop-bound tightening with preheader diamond effective bounds and pipeline cleanup.
- 2026-06-12: added OIR FileCheck coverage, completed focused/full correctness gates, and completed focused/full performance validation.

## Open Questions

- None.

## Results

The transform is intentionally narrow and generic:

- counted loop header `iv < upper`
- increasing i32 unit-step induction
- guard in the first body block matching `limit < iv` / `iv > limit`
- skipped arm directly reaches the common latch and contains no stores, calls, loads, memzero/memset, or other side effects
- latch contains only side-effect-free recurrence progress
- no loop-local definitions are used outside the loop

For `iv < upper` plus skipped predicate `limit < iv`, the pass materializes:

```text
if (limit < upper) {
  effective_upper = limit + 1;
} else {
  effective_upper = upper;
}
while (iv < effective_upper) {
  active_body;
}
```

The `limit + 1` add is only emitted on the `limit < upper` edge, preserving the signed i32 no-overflow invariant for the materialized value.

Current OIR for `transpose2` and the inlined copy now contains `loop.bound.tight`, `loop.bound.next`, and `loop.bound` phi blocks. The original hot `i < j` continue branch is gone from the inner loop. Final assembly for `transpose2` now computes the narrowed bound before the hot inner loop; the inner loop body has only the copy load/store and a single loop-back compare against the effective bound.

Focused performance improved materially:

| Case | GCC | Clang++ | Compiler | Notes |
| --- | ---: | ---: | ---: | --- |
| focused `test/performance/transpose2.sy` | `0.0915s` | `0.1848s` | `0.1035s` | before attribution run had compiler about `0.2078s` |
| focused `test/bsb-final/2025-T03-61.sy` | `0.0847s` | `0.1891s` | `0.1086s` | before attribution run had compiler about `0.1918s` |
| full-run `test/performance/transpose2.sy` | `0.0710s` | `0.1751s` | `0.0952s` | full 119-case report |
| full-run `test/bsb-final/2025-T03-61.sy` | `0.0783s` | `0.1946s` | `0.1041s` | full 119-case report |

Full performance report:

- Status: PASS
- Cases: 119
- Failed: 0
- Geomean speedup: GCC `0.92x` / Clang++ `1.01x`
- QEMU dynamic instruction count: DISABLED
- MIR metrics: OK
- `build/perf-ci/perf-report.md` and `.json` regenerated by the full run

## Handoff Note

Current state:

- Implementation and required verification are complete. Task is ready for review on `task/oir-loop-bound-tightening`.

Next action:

- Review the diff, especially `src/pass/oir/OIRLoopTransforms.cpp` matcher legality and CFG/phi edge rewriting.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-12-oir-loop-bound-tightening.md`
- `docs/tasks/2026-06-12-transpose2-t03-61-perf-gap.md`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`
- `src/pass/oir/OIRLoopTransforms.cpp`
- `include/oir/OIRScalarOpt.h`
- `src/pass/oir/OIRScalarOptUtils.cpp`
- `test/ir/oir_loop_bound_tightening.sy`
