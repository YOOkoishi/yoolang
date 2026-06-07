# Task: MIR Perf Integration

Status: ready_for_review
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: mir++
Worktree: .
Base commit: 98a41f2

## Goal

Maintain shared correctness and performance tracking across the MIR optimization tasks so individual wins do not hide regressions elsewhere.

## Non-goals

- Do not change compiler optimization behavior in this task.
- Do not weaken or exclude performance cases to make reports look better.

## Affected Pipeline

- [ ] Docs only
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
| `build/perf-ci/perf-report.json` | summary and schema | verify machine-readable MIR metrics | yes | generated, read when available |

## Worktree

Decision: current worktree

Reason:

```text
The recorded ../ worktree path is outside the writable sandbox roots. The current worktree was clean,
on branch mir++, and the task was completed there instead of creating task/mir-perf-integration.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
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
| P1 | Record shared baseline and slow-case table for MIR tasks | `scripts/compare_perf.py`, perf report notes | no build required | done | Report now includes MIR stage totals and top slow compiler cases |
| P2 | Add focused perf command presets or documentation | `scripts/compare_perf.py`, task docs | focused perf dry run | done | `PERF_MAX_CASES` preset remains opt-in; default cases are not excluded |
| P3 | Run full perf and attribute regressions to task records | `docs/tasks/*`, `build/perf-ci/perf-report.md` | full perf gate | done | 60 `test/performance` cases passed; generated report remains untracked |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | if scripts change | PASS | build ok |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir_vregs --jobs 1` | if script behavior changes | PASS | 1 passed |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter mir --jobs 1 --o1` | if backend tasks are integrated | PASS | 0 matched; supplemented by focused stage gate |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter mir --jobs 1 --o1` | if backend tasks are integrated | PASS | 0 matched; supplemented by focused stage gate |
| Focused MIR/ASM stage | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter test/easy/basic.sy --jobs 1 --o1` | if backend tasks are integrated | PASS | 2 passed |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter mir --jobs 1 --o1` | if backend tasks are integrated | SKIP | diagnostics/reporting only; no generated-code behavior change |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | |
| Performance focused | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=3 python3 scripts/compare_perf.py` | yes | PASS | 3 cases, report schema smoke test |
| Performance | `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` | yes | PASS | 60 cases, 0 failures, MIR stage metrics OK |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Leave perf tracking to each task | Less coordination | rejected: optimizations can interact and regress each other |
| Store generated perf artifacts as source docs | Easy handoff | rejected initially: avoid generated churn unless user asks |

## Change Log

- 2026-06-07: created task file.
- 2026-06-07: integrated MIR stage metrics into `compare_perf.py` JSON and Markdown reports.
- 2026-06-07: recorded full `test/performance` result and marked ready_for_review.

## Open Questions

- None.

## Handoff Note

Current state:

- Shared perf integration is ready for review in the current `mir++` worktree.
- `build/perf-ci/perf-report.md` now contains MIR stage totals and a slow compiler cases table.
- `build/perf-ci/perf-report.json` now contains `mir_stage_metric_summary` and per-row `codegen_metrics.mir_stages`.
- Final `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py`: 60 cases, 0 failures, GCC geomean 0.91x, Clang++ geomean 0.98x, MIR stage metrics OK.
- QEMU dynamic instruction count remained DISABLED because `ENABLE_QEMU_INSN_COUNT` was not enabled.

Next action:

- Review the report schema and decide whether to add a baseline comparison run against main.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-07-mir-perf-integration.md`
- `scripts/compare_perf.py`
- `scripts/compare_perf_baseline.py`
- `scripts/run_tests.py`
- `docs/tasks/README.md`
- `build/perf-ci/perf-report.md`
- `build/perf-ci/perf-report.json`
