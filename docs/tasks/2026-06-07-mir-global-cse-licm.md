# Task: MIR Global CSE LICM

Status: proposed
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: task/mir-global-cse-licm
Worktree: ../yoolang-mir-global-cse-licm
Base commit: d206fd7

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

Decision: used

Reason:

```text
This task changes global machine optimization behavior and can affect many hot loops. Use an isolated worktree before code edits.
The worktree is planned here but not created by this task-record split.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git worktree add ../yoolang-mir-global-cse-licm -b task/mir-global-cse-licm
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
| P1 | Add dominator-based available-value table for pure MIR defs | `MIRLocalCSEPass.cpp`, `MIRPeepholeCommon.*` | focused FileCheck | pending | Do not CSE physical-def instructions |
| P2 | Expand LICM candidates to `LoadImm`, `LoadGlobalAddr`, `LoadStackAddr`, `AddI`, `SllI`, and safe arithmetic | `MIRLoopInvariantCodeMotionPass.cpp` | MIR stage focused tests | pending | Watch register pressure |
| P3 | Reuse loop stride/address expressions such as constant row stride | `MIRAddressModeCombinePass.cpp`, `MIRAddressOffsetFoldPass.cpp` | ASM/e2e focused tests | pending | Target fewer repeated `li/add/slli` in loops |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir_loop_licm --jobs 1` | yes | NOT_RUN | add CSE/LICM checks |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter mir_loop_licm --jobs 1 --o1` | yes | NOT_RUN | |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter mir_loop_licm --jobs 1 --o1` | yes | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter 01_mm --jobs 1 --o1` | yes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=9 python3 scripts/compare_perf.py` | yes | NOT_RUN | include matrix/conv cases |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Keep CSE local only | Lower risk | rejected: observed gaps are across loop blocks |
| Hoist all pure defs | Simple rule | rejected: register pressure can regress hot loops |

## Change Log

- 2026-06-07: created task file.

## Open Questions

- None.

## Handoff Note

Current state:

- Task record created from the MIR optimization split; no code edits made.

Next action:

- Create the recorded worktree, read keep=yes anchors, then implement P1.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-07-mir-global-cse-licm.md`
- `docs/mir-design.md`
- `src/pass/mir/MIRLocalCSEPass.cpp`
- `src/pass/mir/MIRLoopInvariantCodeMotionPass.cpp`
- `src/pass/mir/MIRAddressModeCombinePass.cpp`
- `src/pass/mir/MIRAddressOffsetFoldPass.cpp`
- `src/pass/mir/MIRCombinePipelinePass.cpp`
