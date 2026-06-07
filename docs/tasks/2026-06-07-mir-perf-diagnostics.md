# Task: MIR Perf Diagnostics

Status: ready_for_review
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: mir++
Worktree: .
Base commit: 98a41f2

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
| `include/pass/mir/MIRDiagnosticsPass.h` | full | new diagnostics pass API and metric schema | yes | added |
| `src/pass/mir/MIRDiagnosticsPass.cpp` | full | stage verification, dumps, and counters | yes | added |
| `test/ir/mir_vregs.sy` | RUN lines and diagnostics checks | focused CLI coverage | yes | updated |

## Worktree

Decision: current worktree

Reason:

```text
The recorded ../ worktree path is outside the writable sandbox roots. The current worktree was clean,
on branch mir++, and the task was completed there instead of creating task/mir-perf-diagnostics.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
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
| P1 | Add `--emit-mir-stage=lowered/post-combine/pre-ra/post-ra/final` without changing default output | `src/main/main.cpp`, `include/pass/mir/MIRDiagnosticsPass.h`, `src/pass/mir/MIRDiagnosticsPass.cpp` | `python3 scripts/run_tests.py --suite filecheck --filter mir_vregs --jobs 1` | done | `--emit-mir` unchanged unless stage is selected |
| P2 | Count per-stage MIR/ASM metrics: moves, jumps, branches, loads, stores, spills | `MIRDiagnosticsPass`, `scripts/compare_perf.py` | focused MIR stage tests | done | MIR metrics come from verified stage snapshots; asm lines remain from generated `.s` |
| P3 | Include metrics in `compare_perf.py` reports | `scripts/compare_perf.py` | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=3 python3 scripts/compare_perf.py` | done | Existing JSON fields preserved; added `mir_stages` and `mir_stage_metric_summary` |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | build ok |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir_vregs --jobs 1` | if IR shape changes | PASS | 1 passed |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter mir --jobs 1 --o1` | yes | PASS | 0 matched; supplemented by focused stage gate |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter mir --jobs 1 --o1` | yes | PASS | 0 matched; supplemented by focused stage gate |
| Focused MIR/ASM stage | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter test/easy/basic.sy --jobs 1 --o1` | yes | PASS | 2 passed |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter mir --jobs 1 --o1` | if behavior affected | SKIP | diagnostics do not alter generated code |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | |
| Performance focused | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=3 python3 scripts/compare_perf.py` | yes | PASS | 3 cases, MIR stage metrics OK |
| Performance full test/performance | `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` | yes | PASS | 60 cases, report has MIR stage metrics OK |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Only post-final metrics | Simpler report | rejected: cannot locate stage responsible for regressions |
| Always dump all stages | Complete data | rejected: too noisy for normal compiler use |

## Change Log

- 2026-06-07: created task file.
- 2026-06-07: implemented MIR diagnostics pass, `--emit-mir-stage`, `--emit-mir-metrics`, perf report integration, and focused FileCheck coverage.
- 2026-06-07: marked ready_for_review after build, focused FileCheck, focused stage, and performance gates passed.

## Open Questions

- None.

## Handoff Note

Current state:

- Implementation is ready for review in the current `mir++` worktree.
- `--emit-mir-stage=lowered/post-combine/pre-ra/post-ra/final` dumps verified MIR snapshots when requested.
- `--emit-mir-metrics` emits JSON stage metrics, and `compare_perf.py` records per-case and aggregate MIR stage metrics.
- Final full `test/performance` report: 60 cases, 0 failures, GCC geomean 0.91x, Clang++ geomean 0.98x, MIR stage metrics OK.

Next action:

- Review the code and decide whether to run the broader `test/bsb-final` perf set.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-07-mir-perf-diagnostics.md`
- `docs/mir-design.md`
- `src/main/main.cpp`
- `src/pass/mir/MIRCombinePipelinePass.cpp`
- `src/pass/mir/MIRPeepholePipelinePass.cpp`
- `src/pass/mir/MIRRegAllocPass.cpp`
- `src/mir/MIRPrinter.cpp`
- `include/pass/mir/MIRDiagnosticsPass.h`
- `src/pass/mir/MIRDiagnosticsPass.cpp`
- `scripts/compare_perf.py`
