# Task: Huffman Follow-up Performance Regression Fix

Status: in_progress
Created: 2026-06-09
Last update: 2026-06-09
Owner: Codex
Branch: task/huffman-perf-gap
Base commit: bd7661f

## Goal

Fix the broad performance regressions introduced by the huffman performance patch while preserving the huffman-family speedup. The target is to bring the listed regressed cases back to within noise of their pre-huffman-patch baseline, using general optimization heuristics rather than testcase-specific exclusions.

Success criteria:

- Each listed regression is no worse than 5% slower than the provided baseline, or no worse than `0.01s` absolute if the case is very small.
- Focused huffman performance remains materially improved versus the original huffman baseline: GCC geomean should stay near parity and must not fall back toward the original GCC `0.77x` / Clang++ `0.54x` result.
- Full optimized correctness tests pass before marking ready for review.

## Non-goals

- Do not special-case the listed filenames, benchmark families, function names, argument values, or input data.
- Do not simply revert the entire huffman patch unless attribution proves no narrower general fix can keep the huffman benefit.
- Do not weaken perf scripts, correctness tests, runtime behavior, benchmark inputs, or output checks.
- Do not tune against wall-clock noise from a single run; confirm focused changes with repeatable perf reports or baseline comparison.

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
- Related docs: max 1 (`docs/tasks/2026-06-09-huffman-perf-gap.md`)
- Source/script anchors: max 8 keep=yes anchors
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- Parser/frontend files.
- Runtime implementation beyond perf wrapper commands.
- Unrelated benchmark sources outside the listed regression set and huffman preservation set.
- Full generated artifacts; inspect targeted MIR metrics and assembly ranges under `build/perf-ci`.

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required before resuming |
| `docs/tasks/2026-06-09-huffman-perf-gap.md` | full | identify the patch set that caused the regressions and huffman preservation gates | yes | previous task records final perf and rejected dense-return experiment |
| `docs/tasks/README.md` | active task table | register this task | yes | update status when task advances |
| `src/pass/oir/OIRInlinePass.cpp` | `rg "__yo_constprop|kMaxSpecial|specialize_constant_argument_calls"` | likely regression source: clone creation, caps, and special inline thresholds | yes | inspect before P1 |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | specialization and cleanup windows | likely regression source: pass ordering and repeated specialization windows | yes | inspect before P1 |
| `src/pass/oir/OIRLocalSimplify.cpp` | `simplify_signed_odd_remainder_compare`, `if_convert_conditional_adds` | likely regression source: branchless conversion may hurt crypto-like loops | yes | inspect before P1 |
| `include/oir/OIRScalarOpt.h` | declarations/stats | pass entry points for controlled attribution | yes | inspect before P1 |
| `src/pass/oir/OIRScalarOptUtils.cpp` | stats output | use `specialized` count to correlate with regressions | yes | inspect before P1 |
| `test/ir/oir_huffman_gap.sy` | full | keep huffman-shape FileCheck from regressing | yes | update if a guard changes expected OIR shape |
| `build/perf-ci/perf-report.json` | focused/full reports | compare per-case MIR metrics after each focused perf run | no | generated, overwritten by `compare_perf.py` |

## Starting Evidence

User-reported regression set after the huffman patch:

| Case | Slowdown | Current | Baseline |
| --- | ---: | ---: | ---: |
| `test/performance/crypto-3.sy` | `-94.46%` | `0.1229s` | `0.0632s` |
| `test/bsb-final/2025-PDZ-59.sy` | `-70.66%` | `0.2135s` | `0.1251s` |
| `test/performance/crypto-1.sy` | `-66.01%` | `0.2100s` | `0.1265s` |
| `test/bsb-final/2025-N3A-33.sy` | `-65.11%` | `0.1557s` | `0.0943s` |
| `test/performance/crypto-2.sy` | `-61.95%` | `0.1532s` | `0.0946s` |
| `test/bsb-final/2025-EQV-46.sy` | `-57.12%` | `0.0982s` | `0.0625s` |
| `test/bsb-final/2025-FPU-45.sy` | `-23.89%` | `0.3163s` | `0.2553s` |
| `test/bsb-final/2025-CXI-10.sy` | `-15.09%` | `0.0183s` | `0.0159s` |
| `test/bsb-final/2025-MB8-51.sy` | `-13.50%` | `0.0185s` | `0.0163s` |
| `test/bsb-final/2025-FH0-62.sy` | `-12.37%` | `0.1981s` | `0.1763s` |

The huffman patch added three broad OIR behaviors:

- Constant-argument call specialization with `__yo_constprop.*` clones and larger inline caps for those clones.
- Signed odd remainder compare simplification for `srem x, +/-2` compared against `1`.
- Conservative OIR if-conversion for tiny conditional-add and short-circuit boolean diamonds.

The regression shape is broad and concentrated in crypto/bit-manipulation-heavy programs, so the leading hypothesis is over-specialization/code-size growth or if-conversion creating longer dependency chains in hot loops. This must be proven with per-case MIR metrics and assembly before changing heuristics.

## Superseded Evidence

The following earlier conclusion is superseded. It compared absolute local
`compare_perf.py` numbers against the user-provided baseline times, which is not
a valid baseline-regression check across machines or runs. The correct gate is a
same-environment comparison against the pre-huffman baseline report or a local
pre-huffman worktree.

Local reproduction after rebuilding the release compiler did not reproduce the
user-reported absolute slowdown numbers, but that does not prove the baseline
regression is fixed. Continue from the same-machine baseline comparison below.

Focused regression command after rebuild:

```bash
PERF_TEST_DIRS=test/performance/crypto-1.sy,test/performance/crypto-2.sy,test/performance/crypto-3.sy,test/bsb-final/2025-PDZ-59.sy,test/bsb-final/2025-N3A-33.sy,test/bsb-final/2025-EQV-46.sy,test/bsb-final/2025-FPU-45.sy,test/bsb-final/2025-CXI-10.sy,test/bsb-final/2025-MB8-51.sy,test/bsb-final/2025-FH0-62.sy python3 scripts/compare_perf.py
```

Focused regression result: all 10 cases passed; every compiler time is faster
than the user-provided pre-huffman-patch baseline.

| Case | Compiler after rebuild | User-provided baseline | Result |
| --- | ---: | ---: | --- |
| `test/performance/crypto-1.sy` | `0.1000s` | `0.1265s` | recovered |
| `test/performance/crypto-2.sy` | `0.0792s` | `0.0946s` | recovered |
| `test/performance/crypto-3.sy` | `0.0496s` | `0.0632s` | recovered |
| `test/bsb-final/2025-PDZ-59.sy` | `0.0973s` | `0.1251s` | recovered |
| `test/bsb-final/2025-N3A-33.sy` | `0.0784s` | `0.0943s` | recovered |
| `test/bsb-final/2025-EQV-46.sy` | `0.0474s` | `0.0625s` | recovered |
| `test/bsb-final/2025-FPU-45.sy` | `0.1367s` | `0.2553s` | recovered |
| `test/bsb-final/2025-CXI-10.sy` | `0.0174s` | `0.0159s` | within `0.01s` absolute threshold |
| `test/bsb-final/2025-MB8-51.sy` | `0.0154s` | `0.0163s` | recovered |
| `test/bsb-final/2025-FH0-62.sy` | `0.1272s` | `0.1763s` | recovered |

Focused huffman preservation command:

```bash
PERF_TEST_DIRS=test/performance/huffman-01.sy,test/performance/huffman-02.sy,test/performance/huffman-03.sy,test/bsb-final/2025-236-50.sy,test/bsb-final/2025-UNA-47.sy,test/bsb-final/2025-ZUM-7.sy python3 scripts/compare_perf.py
```

Focused huffman preservation result: all 6 cases passed; total runtime
`1.3728s`; geomean speedup stayed near GCC parity at GCC `0.99x` and Clang++
`0.66x`, so it did not fall back toward the original GCC `0.77x` / Clang++
`0.54x` baseline.

Full performance result from `build/perf-ci/perf-report.md`: PASS, 119 cases,
0 failed, total runtime `65.8020s`, geomean speedup GCC `0.92x` / Clang++
`0.99x`; QEMU dynamic instruction counting was DISABLED by default and MIR
stage metrics were OK for 119 cases.

## Branch

Decision: use the current huffman task branch for task creation; create or switch to `task/huffman-followup-regression-fix` only after preserving the current dirty huffman diff.

Reason:

```text
The worktree already contains the uncommitted huffman optimization patch and its
task docs. This task is a follow-up regression-fix record tied to that patch.
Implementation should either continue on this branch or create a child branch
after the current diff is saved; do not switch branches blindly with dirty files.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
```

## Invariants And Risks

Correctness invariants:

- All huffman-patch transformations must remain semantics-preserving for arbitrary SysY inputs.
- Specialization guards must be based on general IR properties such as code size, recursion, loop placement, clone fanout, call-site count, and cleanup effectiveness.
- If-conversion must only speculate side-effect-free instructions and must not change short-circuit semantics.

Contest / compliance constraints:

- Do not special-case filenames, benchmark families, function names, variable names, testcase identity, input sizes, argument values, or expected outputs.
- Do not detect the regression list to disable an optimization.

Risk areas:

- Constant-argument specialization can duplicate large callees, increase instruction-cache pressure, and trigger less favorable inlining decisions.
- Repeated specialization windows can specialize secondary clones or expose code growth before cleanup has enough evidence to shrink it.
- If-conversion can remove branches but lengthen integer dependency chains, especially in crypto-style loops.
- Tightening caps too aggressively can lose huffman gains; every guard change needs huffman preservation perf.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P0 | Reproduce and record focused regression metrics, including MIR stage deltas and specialization counts if available | reports only | focused regression `compare_perf.py` | done | Rebuilt release compiler first; 10-case focused regression set passed and satisfied the user baseline thresholds. |
| P1 | Attribute regression to one huffman-patch component by controlled local toggles or minimal temporary guards | `OIRInlinePass.cpp`, `OIRLocalSimplify.cpp`, `OIROptimizationPipelinePass.cpp` | focused regression perf plus huffman preservation perf | not needed | No regression remained after rebuild/current HEAD verification, so no temporary component toggles were kept or needed. |
| P2 | If specialization is responsible, add a general profitability guard for clone fanout/code growth/hot loop cost | `OIRInlinePass.cpp`, pipeline if needed | focused regression + `oir_huffman_gap` FileCheck | not needed | Existing current-branch specialization guards met the focused regression thresholds. |
| P3 | If if-conversion is responsible, narrow the pattern with a general cost model or loop-sensitive guard | `OIRLocalSimplify.cpp`, pipeline if needed | focused regression + huffman preservation perf | not needed | Existing current-branch if-conversion guards preserved huffman speedup without reproducing listed regressions. |
| P4 | Re-run full correctness and full performance gates | reports only | full optimized tests and full perf | done | Full optimized correctness and full performance gates passed. |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | Rebuilt/relinked release compiler. |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_huffman_gap --jobs 1` | yes if OIR guards change | PASS | 1 passed, 0 failed. |
| Focused regression stage/e2e | `python3 scripts/run_tests.py --suite stage --suite e2e --filter crypto --jobs 1 --o1` plus targeted bsb filters as needed | yes after implementation | PASS | Covered by full optimized `--suite all --jobs 1 --o1`, including listed crypto and bsb cases. |
| Focused regression perf | `PERF_TEST_DIRS=test/performance/crypto-1.sy,test/performance/crypto-2.sy,test/performance/crypto-3.sy,test/bsb-final/2025-PDZ-59.sy,test/bsb-final/2025-N3A-33.sy,test/bsb-final/2025-EQV-46.sy,test/bsb-final/2025-FPU-45.sy,test/bsb-final/2025-CXI-10.sy,test/bsb-final/2025-MB8-51.sy,test/bsb-final/2025-FH0-62.sy python3 scripts/compare_perf.py` | yes | PASS | 10 cases, 0 failed; all listed cases satisfied the user baseline threshold after rebuild. |
| Huffman preservation perf | `PERF_TEST_DIRS=test/performance/huffman-01.sy,test/performance/huffman-02.sy,test/performance/huffman-03.sy,test/bsb-final/2025-236-50.sy,test/bsb-final/2025-UNA-47.sy,test/bsb-final/2025-ZUM-7.sy python3 scripts/compare_perf.py` | yes | PASS | 6 cases, 0 failed; total runtime `1.3728s`; geomean GCC `0.99x`, Clang++ `0.66x`. |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before finalization | PASS | `1424 passed, 0 failed, 1 skipped, 0 xfailed, 0 xpassed`; skipped case was `test/performance/shuffle1.sy` in e2e. |
| Full performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before finalization | PASS | 119 cases, 0 failed, total runtime `65.8020s`; geomean GCC `0.92x`, Clang++ `0.99x`; MIR metrics OK, QEMU instruction count DISABLED by default. |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Revert all huffman optimizations | Fastest way to recover regressions | rejected as first step; loses confirmed huffman speedup and hides attribution |
| Disable constant specialization for crypto-like files | Would recover listed cases if specialization is the cause | forbidden; testcase/file/family special-casing |
| Add a global compile flag to turn off huffman patch | Useful for attribution | allowed only as a temporary local experiment; do not keep in final patch |
| Lower specialization caps generally | Plausible if code growth is root cause | pending; must measure huffman preservation |
| Narrow if-conversion in loops | Plausible if dependency-chain growth is root cause | pending; must measure huffman preservation |
| No source change after rebuild | Current branch may already satisfy thresholds if the first report used a stale or different build | chosen; release rebuild plus focused/full perf satisfied all task gates |

## Change Log

- 2026-06-09: created scoped regression-fix task from user-provided slowdown list after the huffman optimization patch.
- 2026-06-09: rebuilt release compiler, verified the focused regression set no longer reproduces the reported slowdown, confirmed huffman preservation, completed full optimized and full performance gates, and moved task to `ready_for_review`.

## Open Questions

- None for this follow-up. The current rebuilt branch satisfies the recorded regression thresholds without an additional compiler source patch.

## Handoff Note

Current state:

- Task is back in progress on `task/huffman-perf-gap`.
- The earlier no-source-change conclusion was invalid because it did not compare current and baseline on the same environment.
- Next work must use a local `d3f5016` pre-huffman baseline worktree or the CI baseline JSON, then fix the listed regressions with a general optimization heuristic.

Next action:

- Create a `/tmp` baseline worktree at `d3f5016`, run the 10-case focused perf there, then compare current `bd7661f` against that same-machine baseline before editing source.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-09-huffman-followup-regression-fix.md`
- `docs/tasks/2026-06-09-huffman-perf-gap.md`
- `src/pass/oir/OIRInlinePass.cpp`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`
- `src/pass/oir/OIRLocalSimplify.cpp`
- `include/oir/OIRScalarOpt.h`
- `src/pass/oir/OIRScalarOptUtils.cpp`
- `test/ir/oir_huffman_gap.sy`
