# Task: MIR List Scheduler

Status: proposed
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: task/mir-list-scheduler
Worktree: ../yoolang-mir-list-scheduler
Base commit: d206fd7

## Goal

Add a conservative MIR list scheduler for PreRA and short-window PostRA scheduling to reduce visible latency in scheduling-sensitive cases.

## Non-goals

- Do not perform global trace scheduling.
- Do not reorder across side effects, calls, volatile-like operations, or terminators.

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
| `docs/mir-design.md` | scheduling/latency notes | target model context | yes | read relevant ranges |
| `src/pass/mir/MIRPeepholePipelinePass.cpp` | full | PreRA/PostRA scheduling placement | yes | primary anchor |
| `src/pass/mir/MIRCombinePipelinePass.cpp` | full | possible PreRA placement | yes | primary anchor |
| `include/mir/MIR.h` | opcode and operand model | dependency extraction | yes | primary anchor |
| `src/mir/MIR.cpp` | defs/uses implementation | dependency extraction | yes | read relevant ranges |
| `src/pass/mir/MIRPeepholeCommon.cpp` | helper patterns | side-effect classification | yes | primary anchor |
| `src/mir/MIRVerifier.cpp` | verifier checks | ensure scheduled MIR remains valid | yes | primary anchor |

## Worktree

Decision: used

Reason:

```text
This task changes instruction order and latency-sensitive behavior. Use an isolated worktree before code edits.
The worktree is planned here but not created by this task-record split.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git worktree add ../yoolang-mir-list-scheduler -b task/mir-list-scheduler
```

## Invariants And Risks

Correctness invariants:

- Reordering must preserve register defs/uses, memory dependencies, call clobbers, and terminator order.
- Scheduler must not move instructions across basic block boundaries in this task.
- PostRA scheduling must respect physical register liveness.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- Memory ordering, implicit operands, physical register clobbers, register pressure, verifier blind spots.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Define opcode latency and dependency model | `include/mir/MIR.h`, `MIRPeepholeCommon.cpp` | unit/FileCheck shape tests | pending | Start conservative |
| P2 | Add PreRA list scheduling within basic blocks | new/nearby MIR pass, pipeline file | MIR stage focused tests | pending | Do not cross memory barriers |
| P3 | Add PostRA short-window safe reorder | scheduler pass, `MIRVerifier.cpp` if needed | ASM/e2e focused tests | pending | Avoid increasing spills |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter optimization_scheduling --jobs 1` | yes | NOT_RUN | add scheduling checks if stable |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter optimization_scheduling --jobs 1 --o1` | yes | NOT_RUN | |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter optimization_scheduling --jobs 1 --o1` | yes | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter optimization_scheduling --jobs 1 --o1` | yes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=6 python3 scripts/compare_perf.py` | yes | NOT_RUN | include scheduling cases |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Schedule only PostRA | Avoid RA pressure changes | rejected: PreRA can improve allocation and latency hiding |
| Schedule across blocks | Higher potential | rejected: too broad for first scheduler task |

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
- `docs/tasks/2026-06-07-mir-list-scheduler.md`
- `docs/mir-design.md`
- `src/pass/mir/MIRPeepholePipelinePass.cpp`
- `src/pass/mir/MIRCombinePipelinePass.cpp`
- `include/mir/MIR.h`
- `src/mir/MIR.cpp`
- `src/pass/mir/MIRPeepholeCommon.cpp`
- `src/mir/MIRVerifier.cpp`
