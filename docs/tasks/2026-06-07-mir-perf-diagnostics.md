# Task: MIR Perf Diagnostics

Status: proposed
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: task/mir-perf-diagnostics
Worktree: ../yoolang-mir-perf-diagnostics
Base commit: d206fd7

## Goal

Add MIR stage-aware diagnostics so backend performance gaps can be attributed to lowering, combine, PreRA peephole, register allocation, PostRA peephole, or final assembly.

## Non-goals

- Do not change optimization behavior or generated code in this task.
- Do not weaken existing verifier or test behavior.

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
- Related docs: max 1, `docs/mir-design.md`
- Source/script anchors: max 6
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- `src/pass/yir/`
- `src/pass/oir/` except handoff-selected lowering interactions
- `runtime/`

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/mir-design.md` | diagnostics/stage sections | MIR stage model | yes | read relevant ranges only |
| `src/main/main.cpp` | MIR pipeline and CLI | add `--emit-mir-stage` or stage artifact output | yes | primary entry |
| `src/pass/mir/MIRCombinePipelinePass.cpp` | full | stage boundary for post-combine metrics | yes | primary anchor |
| `src/pass/mir/MIRPeepholePipelinePass.cpp` | full | PreRA/PostRA stage boundary | yes | primary anchor |
| `src/pass/mir/MIRRegAllocPass.cpp` | run/apply colors area | RA boundary and spill metrics | yes | large file, read narrow ranges |
| `src/mir/MIRPrinter.cpp` | printer entry | stage dump formatting | yes | primary anchor |
| `scripts/compare_perf.py` | report metric collection | expose stage metrics in perf report | yes | large file, read relevant functions |

## Worktree

Decision: used

Reason:

```text
This task touches compiler CLI and perf scripts. Use an isolated worktree before code edits.
The worktree is planned here but not created by this task-record split.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git worktree add ../yoolang-mir-perf-diagnostics -b task/mir-perf-diagnostics
```

## Invariants And Risks

Correctness invariants:

- Diagnostics must not alter pass order, generated MIR, generated ASM, or optimization decisions.
- All stage dumps must come from verified MIR at the appropriate stage.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- CLI compatibility, pass artifact ownership, perf report schema, MIR verifier stage selection.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add `--emit-mir-stage=lowered/post-combine/pre-ra/post-ra/final` without changing default output | `src/main/main.cpp`, MIR pass boundaries | `python3 scripts/run_tests.py --suite stage --stage mir --filter mir --jobs 1 --o1` | pending | Keep `--emit-mir` behavior compatible |
| P2 | Count per-stage MIR/ASM metrics: moves, jumps, branches, loads, stores, spills | MIR metric helper, pass artifacts | focused MIR stage tests | pending | No optimization behavior change |
| P3 | Include metrics in `compare_perf.py` reports | `scripts/compare_perf.py` | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=3 python3 scripts/compare_perf.py` | pending | Preserve existing JSON fields |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir --jobs 1` | if IR shape changes | NOT_RUN | |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter mir --jobs 1 --o1` | yes | NOT_RUN | |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter mir --jobs 1 --o1` | yes | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter mir --jobs 1 --o1` | if behavior affected | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=3 python3 scripts/compare_perf.py` | yes | NOT_RUN | |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Only post-final metrics | Simpler report | rejected: cannot locate stage responsible for regressions |
| Always dump all stages | Complete data | rejected: too noisy for normal compiler use |

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
- `docs/tasks/2026-06-07-mir-perf-diagnostics.md`
- `docs/mir-design.md`
- `src/main/main.cpp`
- `src/pass/mir/MIRCombinePipelinePass.cpp`
- `src/pass/mir/MIRPeepholePipelinePass.cpp`
- `src/pass/mir/MIRRegAllocPass.cpp`
- `src/mir/MIRPrinter.cpp`
- `scripts/compare_perf.py`
