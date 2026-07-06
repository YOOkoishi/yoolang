# Task: Cost Model Pass Activation

Status: ready_for_review
Created: 2026-07-07
Last update: 2026-07-07
Owner: implementation subagent
Branch: task/cost-model-gating-scope
Base commit: 16f4984

## Goal

Recalibrate the current cost-model parameters and per-pass estimates so cost-gated transforms
actually activate under the default `-O1` pipeline when they are legal and generally profitable.
The implementation must compare current branch activation against the legacy `master` behavior,
increase real use of currently under-used gated passes, and preserve cost-model diagnostics.

## Non-goals

- Do not remove or globally disable the cost model, `--emit-cost-model`, policy parsing, JSON
  output, or perf-report cost-model summaries.
- Do not re-open the previous scope decision that canonicalization/local-cleanup transforms may
  use `BypassProfitability`; this task is about calibrating the remaining profitability-gated
  transforms.
- Do not make `-O2` or `-O3` supported.
- Do not weaken legality/proof checks to increase activation counts.
- Do not special-case benchmarks, filenames, function names, variable names, input sizes, source
  literals, runtime input, or expected output.
- Do not hide regressions by weakening tests, changing expected output, or filtering perf cases.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [ ] YIR
- [x] OIR
- [x] MIR
- [x] ASM
- [ ] Runtime
- [x] Test infrastructure
- [x] Performance

## Evidence From Current Vs Master

Master branch behavior:

- `master:src/main/main.cpp` adds `OIROptimizationPipelinePass`, `MIRCombinePipelinePass`,
  `MIRPeepholePipelinePass(false)`, `MIRListSchedulerPass(false)`, `MIRRegAllocPass`,
  `MIRPeepholePipelinePass(true)`, and `MIRListSchedulerPass(true)` under optimized `-O1`.
- `master:src/pass/oir/OIROptimizationPipelinePass.cpp` runs the same broad OIR sequence:
  call specialization windows, inlining, repeated aggressive OIR iterations, unswitch/rotate,
  if-conversion, cleanup, GVN/SCCP/DCE, memory promotion, and loop transforms.
- `master:src/pass/mir/MIRPeepholePipelinePass.cpp` runs local CSE, LICM, pointer loop exits,
  cleanup, and dead-def removal in repeated PreRA peephole rounds.
- `master` has no shared `CostModel` gate, so these passes are present and attempt candidates
  according to each pass's own legality/resource checks.

Current branch behavior:

- `src/main/main.cpp` initializes a `CostModelReport` for `-O1` and emits OIR/final-MIR diagnostics
  when requested; the optimized pass order is still present.
- `include/pass/CostModel.h` and `src/pass/CostModel.cpp` add global policy gates. Default
  `Balanced` uses `min_final_score=1`, `min_confidence=0.55`, `max_function_code_growth=200`,
  `max_module_code_growth_percent=15`, `max_register_pressure_growth=8`,
  `max_live_range_growth=16`, and risk weights including `register_pressure_growth * 6` and
  `cleanup_dependency * 5`.
- The previous task already added `DecisionAction::BypassProfitability` for loop rotate,
  low-risk if-conversion, fixed small e-graph peepholes, local CSE, and PostRA no-growth
  scheduling. Those remain out of scope except for validation.
- Remaining profitability-gated sites include inline, constant-argument specialization, loop
  unswitch, global CSE, PreRA scheduling, LICM, and division reciprocal strength reduction.
- A dirty working-tree change currently lowers `TargetCostProfile::call` from `18` to `12` in
  `include/pass/CostModel.h`. Treat this as existing user/subagent work: read it, decide whether
  it belongs in this calibration, and either keep and validate it or replace it with a better
  generic model. Do not revert it blindly.

Current activation evidence:

- Existing local subset report `build/perf-ci/perf-report.md`, generated 2026-07-06, covers 6
  cases only and is not final evidence. It reports GCC geomean `0.6165`, Clang geomean `0.9594`,
  and cost-model decisions `294 accepted / 66 bypassed / 3222 rejected`.
- The same subset report shows transform decision totals: `AlgebraicSimplify=1911`,
  `IfConversion=1080`, `InstructionScheduling=219`, `Inline=186`, `GlobalCSE=84`,
  `LoopInvariantCodeMotion=75`, `LoopRotate=18`, `LocalCSE=6`, `EGraphRewrite=3`.
- Reject reasons in the subset are dominated by `ProofTimeout=1539` and `NegativeGain=1344`, with
  `Illegal=339`.
- Historical pre-task full-run evidence after the bypass-scope task reported 119 cases, GCC
  geomean `0.7821686424741846`, Clang geomean `0.8109606792364373`, and decisions
  `559 accepted / 1470 bypassed / 3676 rejected`. Use this as historical context, but rerun full
  performance before claiming the new calibration works.

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 3, starting with `docs/cost-model-calibration.md`,
  `docs/tasks/2026-07-07-cost-model-gating-scope.md`, and `docs/cost-model-design.md` if needed
- Source/script anchors: max 10
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- `runtime/`
- frontend parser/type-checking code
- unrelated YIR polyhedral implementation files
- broad generated artifacts outside `build/perf-ci/perf-report.md` and
  `build/perf-ci/perf-report.json`
- benchmark source files unless a measured regression requires source-level attribution

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required on resume |
| `docs/tasks/README.md` | Active Tasks | task index update rules | yes | updated for this task |
| `docs/tasks/TEMPLATE.md` | full | task shape | yes | used to create this file |
| `docs/cost-model-calibration.md` | full | prior full-run cost-model evidence and calibration contract | yes | historical baseline, not final proof |
| `docs/tasks/2026-07-07-cost-model-gating-scope.md` | full/relevant sections | previous bypass-scope decisions and final perf evidence | yes | avoid repeating previous task |
| `src/main/main.cpp` | `rg add_*pipeline`, cost-model setup | compare pass insertion with master and cost-model report activation | yes | optimized pass order still present |
| `include/pass/CostModel.h` | policy fields and dirty `call` change | default target/policy parameters to calibrate | yes | dirty change: `call 18 -> 12` |
| `src/pass/CostModel.cpp` | `policy_for_kind`, `weighted_risk`, `decide` | central thresholds, risk weights, reject reasons | yes | main calibration target |
| `src/pass/oir/OIRCostModel.cpp` | `cost_model_allows_transform` | OIR candidate conversion to shared model | yes | update if report detail/action matrix is needed |
| `src/pass/mir/MIRCostModel.cpp` | `allows_transform` | MIR candidate conversion to shared model | yes | update if report detail/action matrix is needed |
| `src/pass/oir/OIRInlinePass.cpp` | inline and specialization gates | major master-vs-current activation gap | yes | tune generic estimate/resource limits |
| `src/pass/oir/OIRLoopTransforms.cpp` | loop rotate/unswitch gates | keep rotate bypassed; calibrate unswitch only if evidence supports | yes | unswitch remains high risk |
| `src/pass/mir/MIRLoopInvariantCodeMotionPass.cpp` | LICM and div reciprocal gates | currently cost-gated PreRA loop transforms | yes | watch register pressure/spills |
| `src/pass/mir/MIRListSchedulerPass.cpp` | PreRA/PostRA scheduling gates | PreRA scheduling under-use likely from low confidence/risk | yes | PostRA bypass should remain |
| `scripts/compare_perf.py` | cost-model aggregation/reporting | verify per-transform activation evidence is visible | yes | add reporting only if needed |
| `build/perf-ci/perf-report.md` | summary only | existing subset activation clue | no | stale subset; rerun for real evidence |
| `build/perf-ci/perf-report.json` | `cost_model_decision_summary` ranges | existing subset transform/reject counts | no | use as initial clue only |

## Branch

Decision: current branch, no new branch

Reason:

```text
The repository is already on task/cost-model-gating-scope with cost-model work in progress and an
uncommitted implementation change in include/pass/CostModel.h. Creating a second branch during task
generation would risk hiding or misattributing that existing work. Implementation should continue
on this branch unless the controller explicitly creates a follow-up branch.

Current HEAD: 16f4984
Merge-base with master for comparison: 6bc9662b0dc4205e2b56b98eb5bd58debb3fbdcf
Current git status --short: M include/pass/CostModel.h
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git merge-base HEAD master
```

## Invariants And Risks

Correctness invariants:

- Profitability calibration must not make an illegal or unproven transform run.
- Increasing activation must come from generic target/profile/policy or candidate-estimate fixes,
  not testcase-specific checks.
- `--emit-cost-model[=json]` must still explain each candidate as accepted, bypassed, rejected, or
  filtered, with transform, pass, score, risk, proof, and reason information.
- Default `-O1` must remain quiet unless an emit/debug option is requested.
- Master comparison is used to identify activation gaps, not to blindly restore every old transform
  if cost-model evidence shows a generic regression risk.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes,
  expected outputs, source literals, or runtime input data.
- Do not hardcode benchmark results.
- Do not rely on undefined behavior or target/environment assumptions outside the existing RISC-V
  codegen contract.

Risk areas:

- Lowering call cost can make inline and specialization look less profitable; raising it can bloat
  code. Validate with final MIR metrics, not just acceptance counts.
- Relaxing register/live-pressure thresholds can improve LICM/CSE/scheduling activation while
  increasing spills after RA. Inspect `pre-ra->post-ra` spill, load, store, and stack-slot deltas.
- Reducing `ProofTimeout` rejections by raising SMT/e-graph budgets may increase compile time
  without runtime benefit. Prefer structural prefilters or cheaper proof routing over broad budget
  increases.
- Making `Balanced` equivalent to `Aggressive` would defeat the policy separation. Keep policy
  meanings distinct and document any threshold changes.
- Per-transform activation counts can rise while performance falls. Treat measurable full-suite
  geomean regression as a blocker unless the user explicitly accepts it.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add or improve activation diagnostics so reports expose action/reject counts per transform and pass, enough to show which gated passes remain unused | `scripts/compare_perf.py`, maybe `src/pass/CostModel.cpp` | `python3 -m py_compile scripts/compare_perf.py`; full perf report | done | Added `by_transform_action`, `by_transform_reject_reason`, and `by_pass_transform_action` to perf JSON/Markdown. |
| P2 | Calibrate default target/profile and `Balanced` thresholds from generic RISC-V costs, including deciding whether the existing `call 18 -> 12` change is correct | `include/pass/CostModel.h`, `src/pass/CostModel.cpp`, `docs/cost-model-calibration.md` | `xmake`; focused cost-model FileCheck | done | Replaced dirty `call=12` experiment with `call=18`; generic RISC-V call cost should include call/return sequence plus ABI and optimization-boundary overhead. Central policy thresholds unchanged to preserve policy separation. |
| P3 | Rework OIR inline and constant-argument specialization estimates/resource limits so legal small-call and high-cleanup-benefit candidates are not rejected mostly by stale static costs | `src/pass/oir/OIRInlinePass.cpp`, cost-model tests | FileCheck for cost model and OIR-focused tests | done | No OIR resource-budget change; restoring call cost increases generic call-removal profitability without weakening clone/specialization caps. |
| P4 | Rework MIR PreRA profitability estimates for global CSE, LICM/div reciprocal, and PreRA scheduling so accepted candidates correlate with final MIR improvements without spill growth | `src/pass/mir/MIRLocalCSEPass.cpp`, `src/pass/mir/MIRLoopInvariantCodeMotionPass.cpp`, `src/pass/mir/MIRListSchedulerPass.cpp` | MIR stage/ASM stage focused tests plus MIR metrics | done | Global CSE and PreRA scheduling no longer model no-new-register rewrites as register-pressure growth; they remain profitability-gated by positive estimated cycle gain. LICM/div reciprocal unchanged. |
| P5 | Address excessive `ProofTimeout`/`NegativeGain` rejection paths generically, either by cheaper prefilters, better confidence/frequency estimates, or documented non-action if timeouts are legitimate illegal/unknown cases | `src/pass/oir/OIRLocalSimplify.cpp`, `src/pass/SMTProof.cpp`, or docs only depending on evidence | cost-model FileCheck and OIR stage | done | No proof-budget increase. Final report shows remaining `ProofTimeout` only for SMT-backed `AlgebraicSimplify`; this is a provider follow-up, not a profitability calibration fix. |
| P6 | Run full correctness/perf validation, compare current-vs-master activation evidence, and update calibration/task docs with final accepted/bypassed/rejected counts by transform | `docs/cost-model-calibration.md`, this task file | full test/perf commands below | done | Full perf used `PERF_TEST_DIRS=test/performance,test/bsb-final` with no `PERF_MAX_CASES`; calibration doc updated. |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | build ok after source changes. |
| Cost-model FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1` | yes | PASS | 4 passed. |
| OIR focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_ --jobs 1` | yes | PASS | 10 passed. |
| MIR focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir_ --jobs 1` | yes | PASS | 6 passed after updating scheduler/global-CSE assertions. |
| Perf script syntax | `python3 -m py_compile scripts/compare_perf.py` | yes | PASS | Added report aggregation fields. |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --jobs 1 --o1` | yes | PASS | 279 passed. |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --jobs 1 --o1` | yes | PASS | 279 passed. |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --jobs 1 --o1` | yes | PASS | 279 passed. |
| E2E optimized | `python3 scripts/run_tests.py --suite e2e --jobs 1 --o1` | yes | PASS | 277 passed, 1 skipped. |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before review | PASS | 1439 passed, 1 skipped. |
| Perf focused | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=6 python3 scripts/compare_perf.py` | during iteration only | SKIP | Full performance was run twice; no narrowed perf result used as evidence. |
| Full performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before review | PASS | Final report: 119 cases, 0 failed, GCC geomean 0.936692009660409, Clang++ geomean 0.9719614534957396. |
| Final master-baseline comparison | `python3 scripts/compare_perf_baseline.py --current build/perf-ci/perf-report.json --baseline /tmp/yoolang-master-baseline-from-delta.json --out-md build/perf-ci/master-baseline-compare.md --out-json build/perf-ci/master-baseline-compare.json --baseline-label master --baseline-branch master` | before review | PASS / RISK | Recomputed from final full perf report and the prior master baseline rows: total status OK, 119 comparable cases, current 13.4045s vs master 13.3267s, delta +0.0778s (-0.58%), 33 faster / 82 slower / 4 tied. Per-case script flags remain for `test/bsb-final/2025-PDZ-59.sy` (+0.0523s, -109.41%) and `test/performance/crypto-1.sy` (+0.0515s, -108.42%). |
| Master activation comparison | `git show master:<file>` and current `--emit-cost-model=json -O1`/perf report comparison | yes | PASS | Inspected `master:src/pass/oir/OIRInlinePass.cpp` and `master:src/pass/mir/MIRListSchedulerPass.cpp`; final perf report shows current accepted/bypassed/rejected activation by transform/pass/action. |

## Acceptance Criteria

- The final full perf report records cost-model decisions and per-transform counts sufficient to
  show whether inline, specialization, global CSE, PreRA scheduling, LICM/div reciprocal, and loop
  unswitch are accepted, bypassed, or rejected.
- At least one targeted profitability-gated transform with proven candidates shows increased real
  activation versus the pre-task current-branch evidence, or the task records a measured reason why
  activation must remain low.
- Full optimized correctness tests pass.
- Full performance does not regress materially from the prior post-bypass full-run evidence
  (`GCC=0.7821686424741846`, `Clang=0.8109606792364373`) unless the task records a concrete
  user-approved exception.
- Any policy/weight change is justified by generic transform or target-cost reasoning, not by a
  single benchmark identity.
- `Conservative`, `Balanced`, and `Aggressive` remain meaningfully different.

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Make default `-O1` use `--cost-model-policy=aggressive` | Would immediately increase accepted candidates | rejected for now: loses policy separation and may hide bad estimates |
| Disable profitability gates for all transforms that existed in master | Restores master-like activation | rejected: clone/code-growth/register-pressure transforms still need cost control |
| Tune only `min_final_score` downward | Minimal central change | possible but insufficient if rejections are dominated by proof timeout, confidence, or risk weights |
| Calibrate estimates and thresholds by transform risk category | More work, but preserves cost-model purpose | chosen |

## Change Log

- 2026-07-07: created task file from current branch/master comparison and subset activation
  evidence; added Active Tasks entry.
- 2026-07-07: implementation started; restored generic call cost to 18 and recalibrated MIR
  GlobalCSE/PreRA scheduling risk estimates without weakening legality checks or bypassing
  profitability.
- 2026-07-07: enhanced perf report cost-model summaries with transform/action,
  transform/reject-reason, and pass/transform/action counts; completed full correctness and full
  performance validation; updated calibration evidence; status moved to `ready_for_review`.
- 2026-07-07: repair updated the final master-baseline comparison from the final full perf report
  using `scripts/compare_perf_baseline.py`; total delta is OK at +0.0778s (-0.58%), with remaining
  per-case regression flags for `test/bsb-final/2025-PDZ-59.sy` and
  `test/performance/crypto-1.sy`.
- 2026-07-07: repair staged this new task file with intent-to-add so `git diff` includes it while
  leaving the repository uncommitted.

## Open Questions

- Resolved: the dirty `include/pass/CostModel.h` change (`TargetCostProfile::call 18 -> 12`) was
  treated as an experimental candidate and replaced with the generic `call=18` model. The final
  diff no longer contains a `CostModel.h` change.
- Resolved: final acceptance is based on full-suite geomean improvement plus concrete nonzero
  targeted-transform activation in the final report, not a fixed arbitrary count target.
- Follow-up: `ProofTimeout` remains concentrated in SMT-backed `AlgebraicSimplify` (3174
  timeouts). This should be handled as a provider/proof-routing task, not by broad policy-threshold
  relaxation.

## Handoff Note

Current state:

- Implementation is ready for review on `task/cost-model-gating-scope`.
- Final diff is scoped to perf reporting, MIR cost estimates, focused MIR FileCheck expectations,
  task docs, and calibration docs.
- The pre-existing dirty `include/pass/CostModel.h` `call=12` experiment was evaluated and removed;
  final tree uses the generic `call=18` value with no remaining diff in that file.
- Final full performance report is `build/perf-ci/perf-report.md` and
  `build/perf-ci/perf-report.json`.
- Final full performance result: 119 cases, 0 failed, GCC geomean 0.936692009660409, Clang++
  geomean 0.9719614534957396. QEMU dynamic instruction counting was disabled.
- Final master-baseline comparison was regenerated from the final full perf report with
  `scripts/compare_perf_baseline.py`: status OK overall, 119 comparable cases, current total
  13.4045s vs master baseline 13.3267s, total delta +0.0778s (-0.58%). Remaining risk: the script
  still flags `test/bsb-final/2025-PDZ-59.sy` at 0.1001s vs 0.0478s (+0.0523s, -109.41%) and
  `test/performance/crypto-1.sy` at 0.0990s vs 0.0475s (+0.0515s, -108.42%). These are below the
  total-regression blocker threshold but should be reviewed as localized performance risk.
- Final targeted activation: Inline 616 accepted / 48 rejected; ConstantArgumentSpecialization 6
  accepted / 72 rejected; GlobalCSE 261 accepted; PreRA InstructionScheduling 519 accepted / 714
  rejected; PostRA InstructionScheduling 315 bypassed; LICM 627 accepted; StrengthReduction 6
  accepted; LoopUnswitch 714 rejected for code growth.
- MIR final metrics: 33672 instructions, 7808 moves, 2587 branches, 3621 jumps, 1800 loads, 1164
  stores, 162 spills, 216 stack slots.

Next action:

- Review current diff against this task file and the user request. Pay particular attention to
  whether accepting GlobalCSE and PreRA scheduling remains acceptable under full perf and spill
  metrics.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-07-cost-model-pass-activation.md`
- `docs/cost-model-calibration.md`
- `scripts/compare_perf.py`
- `src/pass/mir/MIRLocalCSEPass.cpp`
- `src/pass/mir/MIRListSchedulerPass.cpp`
- `test/ir/mir_loop_licm.sy`
- `test/ir/mir_list_scheduler.sy`
