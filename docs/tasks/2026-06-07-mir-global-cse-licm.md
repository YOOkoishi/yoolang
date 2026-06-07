# Task: MIR Global CSE LICM

Status: ready_for_review
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: tasksys
Worktree: .
Base commit: e74b36d

## Goal

Extend MIR CSE and LICM beyond single-block and `SllI/SllIW` cases so loop-invariant constants, addresses, and stride expressions are reused across hot loops.

## Non-goals

- Do not perform memory CSE without alias guarantees.
- Do not replace OIR scalar optimizations; this task is machine-expression focused.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [ ] YIR
- [ ] OIR
- [x] MIR
- [x] ASM
- [ ] Runtime
- [ ] Test infrastructure
- [x] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 1, `docs/mir-design.md`
- Source/script anchors: max 5
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- `src/pass/yir/`
- OIR loop transforms
- `runtime/`

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/mir-design.md` | MIR optimization/loop sections | stage and legality context | yes | read relevant ranges |
| `src/pass/mir/MIRLocalCSEPass.cpp` | full | existing CSE keying | yes | primary anchor |
| `src/pass/mir/MIRLoopInvariantCodeMotionPass.cpp` | full | existing dominator/loop logic | yes | primary anchor |
| `src/pass/mir/MIRAddressModeCombinePass.cpp` | full | address folding interactions | yes | primary anchor |
| `src/pass/mir/MIRAddressOffsetFoldPass.cpp` | full | local address fold interactions | yes | primary anchor |
| `src/pass/mir/MIRCombinePipelinePass.cpp` | full | pipeline placement | yes | primary anchor |

## Worktree

Decision: not used

Reason:

```text
The active workspace is the writable repository root and the requested task was completed in-place.
The sandbox did not allow creating the planned sibling worktree without escalation.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
```

## Invariants And Risks

Correctness invariants:

- Only pure machine definitions may participate in global CSE.
- LICM may hoist only values whose operands are available in the preheader and whose movement does not change observable side effects.
- Memory loads are excluded unless a later patch proves alias safety.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- Dominator correctness, loop preheader validity, register pressure growth, rematerializable values, address expression equivalence.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add dominator-based available-value table for pure MIR defs | `MIRLocalCSEPass.cpp` | focused FileCheck, full O1 | done | Global CSE is pre-RA only, single virtual def only, no memory loads, and skipped for functions containing calls to avoid known RA pressure failures |
| P2 | Expand LICM candidates to `LoadImm`, `LoadGlobalAddr`, `LoadStackAddr`, `AddI`, `SllI`, and safe arithmetic | `MIRLoopInvariantCodeMotionPass.cpp` | focused FileCheck, full O1 | done | Extended candidates are limited to no-call loops with <= 8 natural-loop blocks; original `SllI/SllIW` behavior remains for larger/call loops |
| P3 | Reuse loop stride/address expressions such as constant row stride | `MIRLocalCSEPass.cpp`, `MIRLoopInvariantCodeMotionPass.cpp` | ASM/e2e focused tests | done | Satisfied via MIR CSE/LICM; no address-mode pass edits were needed |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir_loop_licm --jobs 1` | yes | PASS | 1 passed |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter mir_loop_licm --jobs 1 --o1` | yes | PASS | 0 matched; `test/ir` FileCheck case is not selected by this stage filter |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter mir_loop_licm --jobs 1 --o1` | yes | PASS | 0 matched; `test/ir` FileCheck case is not selected by this stage filter |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter 01_mm --jobs 1 --o1` | yes | PASS | 3 passed |
| Regression E2E | `python3 scripts/run_tests.py --suite e2e --filter 69_expr_eval --jobs 1 --o1` | yes | PASS | Guarded against call-loop LICM pressure |
| Regression E2E | `python3 scripts/run_tests.py --suite e2e --filter 83_long_array --jobs 1 --o1` | yes | PASS | Guarded against call-function global CSE pressure |
| Regression E2E | `python3 scripts/run_tests.py --suite e2e --filter 30_many_dimensions --jobs 1 --o1` | yes | PASS | Guarded extended LICM to small loops |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | PASS | 1410 passed, 0 failed, 1 skipped |
| Performance | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=9 python3 scripts/compare_perf.py` | yes | PASS | 9 cases OK; geomean vs GCC 0.77x, vs Clang++ 0.74x |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Keep CSE local only | Lower risk | rejected: observed gaps are across loop blocks |
| Hoist all pure defs | Simple rule | rejected: register pressure can regress hot loops |

## Change Log

- 2026-06-07: created task file.
- 2026-06-07: implemented no-call-function MIR global CSE for pure single-def expressions, extended small-loop MIR LICM candidates, added focused `mir_loop_licm` coverage, and completed correctness/perf verification.

## Open Questions

- None.

## Handoff Note

Current state:

- Implementation ready for review.
- `MIRLocalCSEPass.cpp` keeps existing block-local CSE and adds dominator-tree CSE for pure single virtual-def expressions in functions without calls.
- `MIRLoopInvariantCodeMotionPass.cpp` hoists additional constants, addresses, and safe integer arithmetic in no-call loops with <= 8 natural-loop blocks; larger/call loops keep original `SllI/SllIW` behavior.
- `test/ir/mir_loop_licm.sy` now checks global constant CSE, large-constant LICM, and global-address LICM.

Next action:

- Review and merge, or broaden global CSE/LICM only after RA handles long live ranges more robustly.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-07-mir-global-cse-licm.md`
- `src/pass/mir/MIRLocalCSEPass.cpp`
- `src/pass/mir/MIRLoopInvariantCodeMotionPass.cpp`
- `test/ir/mir_loop_licm.sy`
