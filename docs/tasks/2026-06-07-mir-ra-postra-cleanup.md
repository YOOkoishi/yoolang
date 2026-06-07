# Task: MIR RA PostRA Cleanup

Status: proposed
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: task/mir-ra-postra-cleanup
Worktree: ../yoolang-mir-ra-postra-cleanup
Base commit: d206fd7

## Goal

Improve MIR register allocation and PostRA cleanup to reduce spills, reloads, redundant moves, and unnecessary callee-saved pressure.

## Non-goals

- Do not replace the whole register allocator in one patch.
- Do not change ABI register preservation rules.

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
- `src/pass/oir/`
- parser/frontend files

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/mir-design.md` | RA/spill sections | intended allocator behavior | yes | read relevant ranges |
| `src/pass/mir/MIRRegAllocPass.cpp` | allocator phases | primary implementation | yes | large file, read targeted ranges |
| `src/pass/mir/MIRPeepholeDeadDefEliminationPass.cpp` | PostRA liveness cleanup | cleanup interactions | yes | primary anchor |
| `src/pass/mir/MIRBlockSimplifyPass.cpp` | PostRA local cleanup | remove redundant moves/stores | yes | primary anchor |
| `src/mir/MIR.cpp` | frame/register data model | frame and callee-saved state | yes | read relevant ranges |
| `src/mir/MIRVerifier.cpp` | PostRA verifier | preserve register/frame invariants | yes | primary anchor |

## Worktree

Decision: used

Reason:

```text
This task changes register allocation and PostRA behavior. Use an isolated worktree before code edits.
The worktree is planned here but not created by this task-record split.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git worktree add ../yoolang-mir-ra-postra-cleanup -b task/mir-ra-postra-cleanup
```

## Invariants And Risks

Correctness invariants:

- Every virtual register must be colored, spilled, or rematerialized before PostRA verification.
- Callee-saved registers used by allocation must be saved and restored exactly once.
- PostRA cleanup must not remove defs live across blocks or calls.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- Live range splitting, spill slot reuse, call clobbers, scratch register reservation, frame layout.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add call-aware/callee-saved cost model | `MIRRegAllocPass.cpp` | MIR/ASM stage focused tests | pending | Prefer callee-saved for values live across calls |
| P2 | Add limited live range splitting around high-pressure regions | `MIRRegAllocPass.cpp` | MIR stage and e2e focused tests | pending | Keep patch small and measurable |
| P3 | Add PostRA redundant load/store/move cleanup | `MIRPeepholeDeadDefEliminationPass.cpp`, `MIRBlockSimplifyPass.cpp` | ASM/e2e focused tests | pending | Recompute physical liveness |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir_vregs --jobs 1` | yes | NOT_RUN | add RA checks if stable |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter mir_vregs --jobs 1 --o1` | yes | NOT_RUN | |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter mir_vregs --jobs 1 --o1` | yes | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter crypto --jobs 1 --o1` | yes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=9 python3 scripts/compare_perf.py` | yes | NOT_RUN | include crypto/sort/conv cases |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Replace graph coloring with linear scan | Simpler splitting | rejected: larger allocator rewrite |
| Only add PostRA cleanup | Lower risk | rejected: does not address allocation pressure |

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
- `docs/tasks/2026-06-07-mir-ra-postra-cleanup.md`
- `docs/mir-design.md`
- `src/pass/mir/MIRRegAllocPass.cpp`
- `src/pass/mir/MIRPeepholeDeadDefEliminationPass.cpp`
- `src/pass/mir/MIRBlockSimplifyPass.cpp`
- `src/mir/MIR.cpp`
- `src/mir/MIRVerifier.cpp`
