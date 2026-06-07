# Task: MIR Perf Integration

Status: proposed
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: task/mir-perf-integration
Worktree: ../yoolang-mir-perf-integration
Base commit: d206fd7

## Goal

Maintain shared correctness and performance tracking across the MIR optimization tasks so individual wins do not hide regressions elsewhere.

## Non-goals

- Do not change compiler optimization behavior in this task.
- Do not weaken or exclude performance cases to make reports look better.

## Affected Pipeline

- [x] Docs only
- [ ] Parser / AST
- [ ] YIR
- [ ] OIR
- [ ] MIR
- [ ] ASM
- [ ] Runtime
- [x] Test infrastructure
- [x] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 0
- Source/script anchors: max 4
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- compiler source directories except when a regression report names a concrete pass
- `runtime/`

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `scripts/compare_perf.py` | report and row generation | baseline/perf report integration | yes | large file, read relevant ranges |
| `scripts/compare_perf_baseline.py` | full | regression threshold behavior | yes | primary anchor |
| `scripts/run_tests.py` | gate commands and filters | test orchestration | yes | read relevant ranges |
| `docs/tasks/README.md` | full | active task tracking | yes | primary anchor |
| `build/perf-ci/perf-report.md` | generated summary | compare current report conclusion | yes | generated, read when available |

## Worktree

Decision: used

Reason:

```text
This task may update scripts and shared docs. Use an isolated worktree before code edits.
The worktree is planned here but not created by this task-record split.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git worktree add ../yoolang-mir-perf-integration -b task/mir-perf-integration
```

## Invariants And Risks

Correctness invariants:

- Reports must reflect actual command results and must not hide failures or regressions.
- Any skipped gate must record an explicit reason.
- Baseline comparison thresholds must remain conservative enough to catch meaningful regressions.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- Report schema changes, generated file churn, accidental exclusion of cases, stale baseline data.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Record shared baseline and slow-case table for MIR tasks | task docs, perf report notes | no build required | pending | Use measured report data |
| P2 | Add focused perf command presets or documentation | `scripts/compare_perf.py` or task docs | focused perf dry run | pending | Do not exclude cases by default |
| P3 | Run full perf and attribute regressions to task records | `docs/tasks/*`, `build/perf-ci/perf-report.md` | full perf gate | pending | Generated report may stay untracked |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | if scripts change | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir --jobs 1` | if script behavior changes | NOT_RUN | |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter mir --jobs 1 --o1` | if backend tasks are integrated | NOT_RUN | |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter mir --jobs 1 --o1` | if backend tasks are integrated | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter mir --jobs 1 --o1` | if backend tasks are integrated | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` | yes | NOT_RUN | primary integration gate |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Leave perf tracking to each task | Less coordination | rejected: optimizations can interact and regress each other |
| Store generated perf artifacts as source docs | Easy handoff | rejected initially: avoid generated churn unless user asks |

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
- `docs/tasks/2026-06-07-mir-perf-integration.md`
- `scripts/compare_perf.py`
- `scripts/compare_perf_baseline.py`
- `scripts/run_tests.py`
- `docs/tasks/README.md`
- `build/perf-ci/perf-report.md`
