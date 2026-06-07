# Task: MIR List Scheduler

Status: ready_for_review
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: mir++
Worktree: /home/yoo/Documents/Compliers/yoolang
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
| `docs/mir-design.md` | scheduling/latency notes | target model context | no | used for target context; not required for review |
| `src/pass/mir/MIRPeepholePipelinePass.cpp` | full | PreRA/PostRA scheduling placement | no | placement pattern identified |
| `src/pass/mir/MIRCombinePipelinePass.cpp` | full | possible PreRA placement | no | combine placement identified |
| `include/mir/MIR.h` | opcode and operand model | dependency extraction | yes | primary anchor |
| `src/mir/MIR.cpp` | defs/uses implementation | dependency extraction | yes | read relevant ranges |
| `src/pass/mir/MIRPeepholeCommon.cpp` | helper patterns | side-effect classification | no | used for local style and opcode patterns |
| `src/mir/MIRVerifier.cpp` | verifier checks | ensure scheduled MIR remains valid | no | verifier is called from the pass |
| `include/pass/mir/MIRListSchedulerPass.h` | full | new pass API | yes | review anchor |
| `src/pass/mir/MIRListSchedulerPass.cpp` | full | scheduler implementation | yes | review anchor |
| `src/main/main.cpp` | MIR pipeline setup | pass wiring | yes | review anchor |
| `test/ir/mir_list_scheduler.sy` | full | focused scheduling shape test | yes | review anchor |
| `test/ir/mir_vregs.sy` | pressure ASM checks | updated stable ASM shape | yes | review anchor |

## Worktree

Decision: current workspace used

Reason:

```text
The original task split planned ../yoolang-mir-list-scheduler, but final edits
were made in the current workspace on branch mir++.
```

Commands:

```bash
git branch --show-current
git rev-parse --short HEAD
git worktree list
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
| P1 | Define opcode latency and dependency model | `src/pass/mir/MIRListSchedulerPass.cpp` | FileCheck shape test | done | RAW/WAR/WAW DAG over `defs()`/`uses()`; side-effect, call, terminator, and comment barriers |
| P2 | Add PreRA list scheduling within basic blocks | `include/pass/mir/MIRListSchedulerPass.h`, `src/pass/mir/MIRListSchedulerPass.cpp`, `src/main/main.cpp` | MIR/ASM/e2e focused tests | done | PreRA runs after MIR peephole and before regalloc; fixed non-zero physical operands are barriers |
| P3 | Add PostRA short-window safe reorder | `src/pass/mir/MIRListSchedulerPass.cpp`, `src/main/main.cpp` | ASM/e2e/full tests | done | PostRA uses 8-instruction windows and skips functions with calls or frame size > 2047 |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | final build completed |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir_list_scheduler --jobs 1` | yes | PASS | added stable MIR/ASM scheduling check |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter optimization_scheduling --jobs 1 --o1` | yes | PASS | 3 passed |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter optimization_scheduling --jobs 1 --o1` | yes | PASS | 3 passed |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter optimization_scheduling --jobs 1 --o1` | yes | PASS | 3 passed |
| Vreg FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir_vregs --jobs 1` | yes | PASS | updated ASM pressure check to stable incoming/outgoing stack traffic |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | PASS | 1420 passed, 0 failed, 1 skipped |
| Performance | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=6 python3 scripts/compare_perf.py` | yes | PASS | 6 cases, 0 failed; reports in `build/perf-ci/perf-report.md` and `.json` |
| Extended performance | `python3 scripts/compare_perf.py --help` | no | PASS | script ran default `test/bsb-final`; 59 cases, 0 failed, total 30.8770s; reports overwritten |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Schedule only PostRA | Avoid RA pressure changes | rejected: PreRA can improve allocation and latency hiding |
| Schedule across blocks | Higher potential | rejected: too broad for first scheduler task |

## Change Log

- 2026-06-07: created task file.
- 2026-06-07: implemented conservative MIR list scheduler and wired PreRA/PostRA pipeline passes.
- 2026-06-07: added focused scheduler FileCheck and updated `mir_vregs` ASM pressure expectations.
- 2026-06-07: completed focused, full optimized, and performance verification; status set to `ready_for_review`.

## Open Questions

- None.

## Handoff Note

Current state:

- Conservative block-local MIR list scheduling is implemented.
- PreRA schedules contiguous side-effect-free windows with a 64-instruction cap; any fixed non-zero physical operand is a barrier.
- PostRA schedules short 8-instruction windows and skips functions with calls or frame size > 2047.
- The pass verifies MIR after scheduling and reports `scheduled block=... region=... instr=...`.
- Pipeline wiring is `MIRCombinePipelinePass`, PreRA peephole, PreRA scheduler, register allocation, PostRA peephole, PostRA scheduler.
- Focused tests, full optimized suite, and performance smoke runs passed as recorded above.

Next action:

- Review the diff and decide whether to keep the conservative PostRA skip or extend MIR modeling for call clobbers and large-frame scratch registers in a later task.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-07-mir-list-scheduler.md`
- `include/pass/mir/MIRListSchedulerPass.h`
- `src/pass/mir/MIRListSchedulerPass.cpp`
- `src/main/main.cpp`
- `test/ir/mir_list_scheduler.sy`
- `test/ir/mir_vregs.sy`
- `include/mir/MIR.h`
- `src/mir/MIR.cpp`
