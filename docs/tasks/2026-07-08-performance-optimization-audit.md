# Task: Performance Optimization Opportunity Audit

Status: ready_for_review
Created: 2026-07-08
Last update: 2026-07-08
Owner: task-generation subagent
Branch: not used
Base commit: d63f08b

## Goal

Audit the 60 preliminary contest cases under `test/performance` to identify high-payoff,
general compiler optimization opportunities that yoolang `-O1` either does not implement or
implements too conservatively. The audit must be evidence-backed: rank cases from fresh perf data,
inspect optimized OIR and final generated output for selected high-payoff cases, compare against
GCC/Clang artifacts, and record durable findings.

## Non-goals

- Do not implement compiler optimizations in this task.
- Do not change production compiler code, runtime code, benchmark inputs/outputs, or expected output.
- Do not special-case benchmark names, function names, filenames, literal strings, input sizes, or
  expected results.
- Do not claim an optimization is missing or conservative from source shape alone; verify against
  optimized OIR and final assembly/MIR.
- Do not treat `PERF_MAX_CASES` runs as final performance evidence.

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
- Related docs: max 2, `docs/tasks/2026-06-12-transpose2-t03-61-perf-gap.md` and
  `docs/cost-model-calibration.md` only if cost-model report interpretation is needed
- Source/script anchors: max 8 before selecting cases
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- Parser, AST, frontend, and runtime directories
- All OIR/MIR pass implementation files before a ranked case points to a specific missed or
  conservative pattern
- `test/bsb-final` except as an optional cross-check after `test/performance` findings are stable
- Generated artifacts outside `build/perf-ci/perf-report.md`,
  `build/perf-ci/perf-report.json`, and selected per-case directories under `build/perf-ci`

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `/home/yoo/.codex/skills/yoolang-optimization/SKILL.md` | full | contest compliance and perf workflow | yes | required for yoolang optimization work |
| `docs/tasks/README.md` | full | active task registry and shared perf gates | yes | updated for this task |
| `docs/tasks/TEMPLATE.md` | full | task shape | no | used once to create this record |
| `docs/tasks/2026-06-12-transpose2-t03-61-perf-gap.md` | full | prior evidence-backed OIR/MIR/ASM audit pattern | yes | use as style and rigor reference, not as current evidence |
| `test/performance/*.sy` | `find test/performance -maxdepth 1 -name '*.sy'` | preliminary contest corpus; 60 source cases | yes | read individual cases only after ranking |
| `scripts/compare_perf.py` | `215-255`, `638-700`, `907-1397`, `1424-1516` | case discovery, report schema, MIR metrics, slow-case ranking, generated artifacts | yes | perf reports are overwritten by each run |
| `scripts/run_tests.py` | `1-110` | stage/e2e command syntax and `--o1` behavior | yes | use `--jobs 1` |
| `src/main/main.cpp` | `80-340`, `360-455`, `560-660` | `-O1`, emit flags, cost-model diagnostics, backend pipeline | yes | read exact pipeline ranges before attributing |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | full | current OIR pass order and already-enabled optimizations | yes | includes LICM, GVN, SCCP, LSR, loop bound tightening, unswitch, jump threading |
| `include/oir/OIRScalarOpt.h` | `1-130` | current OIR pass API and stats names | yes | use stats/pass names for classification |
| selected `test/performance/*.sy` | full for `shuffle1`, `conv2d-1`, `knapsack_naive-2`, `huffman-02`, `03_sort1`, `01_mm2`, `crypto-3`, `transpose2` | source shape for representative high-payoff cases | yes | ranked from fresh perf report |
| `/tmp/yoolang-audit/*.{oir,mir,cost.json}` | selected generated evidence | optimized OIR, MIR stage metrics, stage dumps, cost-model summaries | yes | temporary evidence artifacts generated on 2026-07-08 |
| `build/perf-ci/test/performance/<case>/*.s` | selected compiler/GCC/Clang assembly | final-output comparison for selected cases | yes | current full perf run artifacts |

## Branch

Decision: not used for the audit task

Reason:

```text
This scoped task is an evidence-backed optimization audit and should only create/update task
documentation and, if useful, one durable findings document. No production compiler changes are
allowed. The task-generation environment is currently on branch task/smt-solver with unrelated
dirty changes; the implementation subagent must not revert them or switch branches unless the
controller/user provides a clean worktree or explicitly approves branch cleanup.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
```

Observed:

```text
git status --short:
 M docs/tasks/README.md
 M include/pass/SMTProof.h
 M src/pass/SMTProof.cpp
 M src/pass/oir/OIRLocalSimplify.cpp
 M test/ir/cost_model_smt.sy
 M xmake.lua
?? docs/smt-solver-review-zh.md
?? docs/smt-solver.md
?? docs/tasks/2026-07-07-smt-solver.md
?? include/smt/
?? src/smt/
?? test/smt/

git rev-parse --short HEAD: d63f08b
git branch --show-current: task/smt-solver
```

## Invariants And Risks

Correctness invariants:

- Every proposed optimization opportunity must be general and semantics-preserving for the IR/MIR
  pattern, not for a named benchmark.
- A case is "already optimized" only if the optimized OIR and final generated output show the
  expected transformation or equivalent code shape.
- A case is "implemented too conservatively" only if a current pass exists for the general pattern
  but OIR/cost-model/MIR/ASM evidence shows the candidate was not applied, was rejected, or did not
  survive to final code.
- Performance conclusions must come from fresh `test/performance` measurements, not stale reports.
- Generated perf reports are overwritten by each run; copy key numbers and artifact paths into the
  task file or durable findings document before running another perf command.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or
  expected outputs.
- Do not hardcode known testcase results or replace real computation with expected output.
- Do not exploit undefined behavior or assumptions that make other valid inputs incorrect.
- Do not weaken tests, skip cases, alter expected outputs, or bias performance scripts.

Risk areas:

- Ranking by wall-clock time can be noisy; use geomean ratios, repeated medians from
  `compare_perf.py`, MIR stage metrics, and assembly shape together.
- QEMU dynamic instruction counts may be disabled or unsupported; if unavailable, treat MIR metrics
  and assembly comparison as the fallback evidence.
- Current OIR already has several relevant passes, including loop-bound tightening, strength
  reduction, LICM, GVN, SCCP, if-conversion, unswitching, jump threading, inlining, and cost-model
  diagnostics. The audit must avoid recommending an already-effective pass without proving a gap.
- Cost-model decisions may explain conservative behavior; inspect `--emit-cost-model=json -O1` or
  `perf-report.json` summaries before labeling profitability gating as the root cause.
- Broad case-family claims can be wrong if only one benchmark is inspected; use family
  representatives and record evidence limits.

## Audit Method

1. Build the current compiler and run a fresh full preliminary performance report:

   ```bash
   xmake
   PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py
   ```

2. Parse `build/perf-ci/perf-report.json` and rank candidates by:
   - largest yoolang runtime among OK rows,
   - lowest yoolang/GCC and yoolang/Clang speedup ratios,
   - high final MIR instruction count, loads/stores, branches/jumps, spills, or stack slots,
   - family coverage across matrix, convolution, FFT, sort, crypto/CRC, scheduling, transpose,
     huffman, and loop-heavy cases.

3. Select 6-10 representative high-payoff cases. For each selected case, capture:
   - source pattern summary from `test/performance/<case>.sy`,
   - optimized OIR with `build/linux/x86_64/release/compiler <case>.sy --emit-oir -O1`,
   - MIR metrics with `--emit-mir-metrics -O1`,
   - at least `lowered`, `pre-ra`, and `final` MIR if backend attribution is needed,
   - final yoolang assembly and GCC/Clang assembly from `build/perf-ci/<case>/`,
   - cost-model JSON if a current pass appears to reject or skip a candidate.

4. Classify each opportunity as one of:
   - `missing pass`: no current general pass appears to cover the observed pattern,
   - `conservative pass`: a current pass exists but rejects, misses, or fails to preserve the shape,
   - `backend lowering/codegen gap`: OIR is good but MIR/final assembly leaves avoidable overhead,
   - `not currently high payoff`: evidence does not justify implementation priority.

5. Produce a durable findings artifact if the findings exceed a short task note. Preferred path:
   `docs/performance-optimization-opportunity-audit.md`.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Establish fresh `test/performance` baseline and ranking from current compiler | `build/perf-ci/perf-report.md`, `build/perf-ci/perf-report.json`, task file | `xmake`; `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` | done | 60/60 OK; geomean speedup GCC 0.972x / Clang++ 1.018x; selected 8 representative cases |
| P2 | Inspect selected high-payoff cases from source through optimized OIR and final output | selected `test/performance/*.sy`, generated OIR/MIR/ASM artifacts, task file | direct `--emit-oir`, `--emit-mir-metrics`, `--emit-mir-stage=<stage>`, and assembly comparison commands | done | Generated OIR, MIR metrics, cost-model JSON, and lowered/pre-ra/final MIR under `/tmp/yoolang-audit`; inspected compiler/GCC/Clang assembly |
| P3 | Determine whether each opportunity is missing, conservative, backend-only, or low priority | `src/main/main.cpp`, `src/pass/oir/OIROptimizationPipelinePass.cpp`, `include/oir/OIRScalarOpt.h`, selected pass files only as needed | evidence review plus optional `--emit-cost-model=json -O1` | done | Classified sort, huffman, conv2d, mm, knapsack, crypto, shuffle, and transpose in durable findings |
| P4 | Write durable findings and prioritized next-task recommendations | `docs/performance-optimization-opportunity-audit.md`, task file, `docs/tasks/README.md` if status changes | markdown review; optional command snippets rerun for top cases | done | Findings document added; task and README status updated |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | Build completed in 0.053s on 2026-07-08 local time |
| Full preliminary performance | `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` | yes | PASS | 60/60 OK; report generated at `build/perf-ci/perf-report.md` and `.json` |
| Perf report readback | inspect `build/perf-ci/perf-report.md` and parse `build/perf-ci/perf-report.json` | yes | PASS | Geomean speedup GCC 0.972x / Clang++ 1.018x; QEMU instruction count disabled; MIR metrics OK for 60 cases; selected candidates: `shuffle1`, `conv2d-1`, `knapsack_naive-2`, `huffman-02`, `03_sort1`, `01_mm2`, `crypto-3`, `transpose2` |
| Focused performance rerun | `PERF_TEST_DIRS=<selected-case-list> python3 scripts/compare_perf.py` | optional | SKIP | Full 60-case run is the final ranking evidence; no compiler behavior changed, so narrowed rerun would not add stronger evidence |
| OIR stage evidence | `build/linux/x86_64/release/compiler <case>.sy --emit-oir -O1 -o /tmp/yoolang-audit/<case>.oir` | yes for selected cases | PASS | Generated for `shuffle1`, `conv2d-1`, `knapsack_naive-2`, `huffman-02`, `03_sort1`, `01_mm2`, `crypto-3`, `transpose2` |
| MIR metrics evidence | `build/linux/x86_64/release/compiler <case>.sy --emit-mir-metrics -O1 -o /tmp/yoolang-audit/<case>.mir-metrics.json` | yes for selected cases | PASS | Generated for all selected cases; used final metrics and stage deltas in findings |
| MIR stage evidence | `build/linux/x86_64/release/compiler <case>.sy --emit-mir-stage=<lowered|pre-ra|final> -O1 -o /tmp/yoolang-audit/<case>.<stage>.mir` | if backend attribution needed | PASS | Generated lowered, pre-ra, and final MIR for all selected cases |
| Cost-model evidence | `build/linux/x86_64/release/compiler <case>.sy --emit-cost-model=json -O1 -o /tmp/yoolang-audit/<case>.cost.json` | if conservative gating suspected | PASS | Generated for all selected cases; inspected OIR/final cost summaries and action/reject counts |
| Assembly comparison | inspect `build/perf-ci/test/performance/<case>/<case>.compiler.s`, `.gcc.s`, and `.clang.s` | yes for selected cases | PASS | Compared loop bodies, bit-op loops, fixed kernel unrolling, recursive spill pressure, and transpose loop-bound labels |
| Focused stage/e2e smoke | `python3 scripts/run_tests.py --suite stage --suite e2e --filter <case-or-family> --jobs 1 --o1` | optional | SKIP | Docs-only audit; full perf run already compiled/executed all 60 selected corpus cases successfully |
| Full optimized tests | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | no | SKIP | Not required because this task did not change compiler behavior |
| Broad final performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | no | SKIP | Scope was preliminary `test/performance`; no final-contest cross-check required |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Implement top optimization immediately | User wants high-payoff optimization ideas, but asked to follow task workflow and inspect evidence first | rejected for this task |
| Only inspect source benchmark shapes | Fast but cannot tell whether OIR or final output already optimized the pattern | rejected |
| Rank only by yoolang/GCC wall-time ratio | Highlights gaps but can overfit noisy or tiny cases | rejected; combine ratio, absolute runtime, MIR metrics, and family coverage |
| Use `PERF_MAX_CASES` for final ranking | Faster | rejected; only acceptable for local smoke before the required full 60-case run |

## Change Log

- 2026-07-08: created scoped audit task file and registered it in `docs/tasks/README.md`.
- 2026-07-08: implementation subagent started audit, built compiler, ran full `test/performance` perf report, and selected representative cases from fresh ranking.
- 2026-07-08: completed selected OIR/MIR/cost/ASM inspection, wrote durable findings document, and marked task ready for review.

## Open Questions

- None.

## Findings Summary

Durable findings are recorded in `docs/performance-optimization-opportunity-audit.md`.

Priority order:

1. OIR repeated digit-extraction CSE/specialization for `03_sort*`.
2. OIR/MIR bit-operation idiom recognition for loop-defined `_and`/`_or`/`_xor` and rotate-like arithmetic, covering `huffman-*` first and `crypto-*` second.
3. Fixed-small-loop unroll plus stencil boundary guard clustering for `conv2d-*`.
4. Strided scalar loop unroll/peeling for `01_mm*`.
5. Recursive inlining cost-model guardrails using post-RA spill-risk evidence from `knapsack_naive-*`.

Low-priority or already-covered cases:

- `shuffle1`: high absolute runtime but only 1.02x slower than GCC and no obvious compiler gap.
- `transpose2`: current OIR/ASM shows `loop.bound.tight` and loop-bound labels, so the old triangular-loop gap is fixed in this tree.

## Handoff Note

Current state:

- Audit is complete and ready for review.
- No production compiler/runtime/test source was modified.
- Added durable findings at `docs/performance-optimization-opportunity-audit.md`.
- Updated this task file and `docs/tasks/README.md` status only.
- Current branch remained `task/smt-solver`; unrelated SMT/cost-model worktree changes were not reverted or cleaned.

Next action:

- Review the findings document against the requested audit scope and the verification matrix.

Read next:

- `docs/task-system.md`
- `/home/yoo/.codex/skills/yoolang-optimization/SKILL.md`
- `docs/tasks/2026-07-08-performance-optimization-audit.md`
- `docs/performance-optimization-opportunity-audit.md`
- `scripts/compare_perf.py`
- `scripts/run_tests.py`
- `src/main/main.cpp`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`
- `include/oir/OIRScalarOpt.h`
