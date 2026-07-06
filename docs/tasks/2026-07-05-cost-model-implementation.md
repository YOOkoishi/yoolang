# Task: Cost Model Implementation

Status: needs_revision
Created: 2026-07-05
Last update: 2026-07-06
Owner: Codex
Branch: task/cost-model-implementation
Base commit: 6bc9662

## Goal

Fully implement the cost model described in `docs/cost-model-design.md`: shared cost data
types, OIR/MIR static estimators, diagnostic output, selected OIR/MIR pass gates, SMT proof
integration, partial-evaluation candidates, e-graph extraction, and perf-report calibration.

The task must be executable by Codex `/goal` as a sequence of bounded checkpoints. Each
checkpoint must leave enough context in this file for the next run to continue without
rescanning the repository.

## Non-goals

- Do not add `-O2` or `-O3`; the cost model serves the existing `-O1` competition pipeline.
- Do not use source filename, testcase name, function name, variable name, literal string, input
  size, expected output, or runtime input data as an optimization trigger.
- Do not let cost model profitability override legality. Unknown, timeout, or failed proof means
  reject or defer.
- Do not gate canonicalization, DCE/ADCE, verifier-only, diagnostics-only, or no-tradeoff CFG
  cleanup passes unless a real profitability tradeoff appears.
- Do not tune weights from a single benchmark identity. Tune only generic risk/benefit categories.

## Affected Pipeline

- [x] Docs only
- [ ] Parser / AST
- [x] YIR
- [x] OIR
- [x] MIR
- [x] ASM
- [ ] Runtime
- [x] Test infrastructure
- [x] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 2, starting with `docs/cost-model-design.md`
- Source/script anchors: max 8 promoted anchors per checkpoint
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- `runtime/`
- unrelated frontend/parser/type-system files
- unrelated archived perf history
- broad full-directory scans outside `include/pass`, `src/pass`, `src/main`, `scripts`, and focused
  tests

Checkpoint expansion rule:

- Each checkpoint may add at most 4 new `keep=yes` anchors.
- If more anchors are needed, finish the current checkpoint with `Status: blocked` or split the
  next checkpoint into a smaller patch.
- Every promoted source anchor must state the exact pass, API, or verifier reason that makes it
  necessary.

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol, branch and checkpoint expectations | yes | required on every resume |
| `docs/cost-model-design.md` | headings plus `1-140`, `112-714`, `714-940` | source design for architecture, data types, pass order, diagnostics, perf loop, compliance, milestones | yes | primary requirements document |
| `docs/tasks/TEMPLATE.md` | full | required task record shape | no | used only to create this file |
| `docs/tasks/README.md` | full | active task registration format | yes | update status/branch/date as task advances |
| `src/main/main.cpp` | `49-234`, `261-660` | CLI options, emit-mode restrictions, OIR/MIR pipeline wiring, diagnostics artifact printing | yes | `--emit-cost-model[=json]`, policy/filter parsing, artifact printing |
| `include/pass/CostModel.h` | full | shared data types, policies, decision engine API, report printer API | yes | C1 public cost-model surface |
| `src/pass/CostModel.cpp` | full | policy defaults, weighted cost/risk, decision formula, JSON/text report printers | yes | C1 implementation |
| `include/pass/CostModelDiagnosticsPass.h` | full | artifact key and pass API for read-only cost-model reports | yes | C2 diagnostics pass surface |
| `src/pass/CostModelDiagnosticsPass.cpp` | full | read-only OIR/MIR collectors and report artifact merge | yes | C2 collector implementation |
| `include/oir/OIRScalarOpt.h` | `1-130` | OIR `Stats` carrier and `run_oir_transform` context bridge for decision recording | yes | C3 report pointer wiring |
| `src/pass/oir/OIRInlinePass.cpp` | `1-120`, `220-270`, `520-580`, `860-1030` | inline/specialization legality, thresholds, transform sites, decision trace hook | yes | C3 first pass integration |
| `include/pass/oir/OIRCostModel.h` | full | shared OIR decision helper input shape | yes | C3/C4 helper API |
| `src/pass/oir/OIRCostModel.cpp` | full | filter matching, candidate construction, decision recording, gate result | yes | reused by inline, if-conversion, loop transforms |
| `src/pass/oir/OIRLocalSimplify.cpp` | `1-80`, `928-1048` | OIR if-conversion match/rewrite sites and cost gate hook | yes | C4 if-conversion integration |
| `src/pass/oir/OIRLoopTransforms.cpp` | `1-80`, `560-660`, `960-1030`, `1720-1855` | loop rotate/unswitch legality and rewrite sites | yes | C4 loop transform integration |
| `include/pass/mir/MIRCostModel.h`, `src/pass/mir/MIRCostModel.cpp` | full | shared MIR decision helper, filter matching, candidate construction, decision recording | yes | C5 helper API/implementation |
| `include/pass/mir/MIRPeepholeCommon.h`, `src/pass/mir/MIRPeepholeCommon.cpp`, `src/pass/mir/MIRPeepholePipelinePass.cpp` | stats bridge and pass-context artifact wiring | carry cost-model report/policy/filter into MIR peephole subpasses | yes | C5 context plumbing |
| `src/pass/mir/MIRLocalCSEPass.cpp`, `src/pass/mir/MIRLoopInvariantCodeMotionPass.cpp` | CSE rewrite sites, LICM/strength-reduction move sites | gate MIR CSE, LICM, and division strength-reduction candidates with structural proof and risk | yes | C5 transform gates |
| `src/pass/mir/MIRListSchedulerPass.cpp` | scheduling DAG and `schedule_window` | gate PreRA/PostRA instruction scheduling with latency estimate and register-pressure risk | yes | C5 scheduler gate |
| `include/pass/SMTProof.h`, `src/pass/SMTProof.cpp` | full | SMT obligation, deterministic proof result adapter, timeout and cache support | yes | C6 proof-gate API |
| `include/pass/oir/OIRCostModel.h`, `src/pass/oir/OIRCostModel.cpp` | proof fields and setup cost wiring | carry SMT proof status/time/obligation metadata into `TransformCandidate` | yes | C6 proof metadata bridge |
| `src/pass/oir/OIRLocalSimplify.cpp` | add/sub algebraic simplify helper and local simplify loop | guarded i32 SMT rewrite provider for `bvadd(bvsub(x,y),y) == x` | yes | C6 provider integration |
| `test/ir/cost_model_smt.sy` | full | FileCheck coverage for SMT proven/refuted/timeout/unknown and JSON proof trace | yes | C6 focused test |
| `include/pass/CostModel.h` | `PartialEvalCost` | shared PE clone/residual/cleanup cost data shape | yes | C7 PE cost fields |
| `src/pass/oir/OIRInlinePass.cpp` | constant specialization cost gate | PE clone/code-growth/specialization budget integration | yes | C7 PE provider |
| `test/ir/cost_model_pe.sy` | full | FileCheck coverage for PE accept and specialization-budget reject traces | yes | C7 focused test |
| `src/pass/oir/OIRLocalSimplify.cpp` | e-graph mul-by-two provider | bounded OIR expression-slice provider and extraction budget gate | yes | C8 e-graph provider |
| `test/ir/cost_model_egraph.sy` | full | FileCheck coverage for e-graph accept and budget/proof reject traces | yes | C8 focused test |
| `scripts/compare_perf.py` | cost-model summary collection and report rendering | collect per-case `--emit-cost-model=json` decisions and aggregate accepted/rejected counts by kind, reason, and proof status | yes | C9 perf-report integration |
| `docs/cost-model-calibration.md` | full | calibration contract and final evidence summary | yes | C9 final documentation |
| `build/perf-ci/perf-report.md` | summary sections | full performance evidence, MIR metrics, and cost-model decision summary | no | generated artifact; inspect after perf runs |
| `build/perf-ci/perf-report.json` | top-level summary keys | machine-readable perf and cost-model decision evidence | no | generated artifact; inspect after perf runs |
| `rg --files include src scripts test \| rg "(main\\.cpp\|OIRInlinePass\|OIR.*Pass\|MIR.*Pass\|compare_perf\|Diagnostics\|Metrics)"` | query | locate candidate pass/script anchors without broad reading | no | promote exact files per checkpoint |
| `/home/yoo/.codex/skills/yoolang-optimization/SKILL.md` | full | contest compliance, validation, perf-report workflow | yes | external instruction, not a repo file |

## Branch

Decision: implementation branch created

Reason:

```text
Implementation started on a dedicated task branch. The previous attempt to create the branch inside
the sandbox failed because `.git` writes required escalation; the branch was then created with
approval from clean branch `cost-model` at `6bc9662`.
```

Commands already run:

```bash
git status --short
# clean before source edits
git rev-parse --short HEAD
# 6bc9662
git branch --show-current
# cost-model
git checkout -b task/cost-model-implementation
# Switched to a new branch 'task/cost-model-implementation'
```

## Invariants And Risks

Correctness invariants:

- Every accepted transform must have `legal == true` and a proven or structurally justified
  semantic invariant before profitability is evaluated.
- `TransformCandidate::scope` describes IR scope such as function, loop, block, instruction slice,
  or e-class; it must never encode testcase identity.
- Plain `-S -O1` remains quiet unless a cost-model diagnostic flag is passed.
- `--emit-cost-model` and `--emit-cost-model=json` reproduce decisions without changing generated
  code beyond decisions already enabled by the selected policy.
- Cost model decisions must remain deterministic for the same IR, target profile, and policy.
- SMT timeout/unknown, e-graph budget exhaustion with incomplete proof, or PE proof uncertainty must
  reject or defer the candidate.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes,
  input values, or expected outputs.
- Do not use unproved undefined-behavior assumptions.
- Use only IR structure, types, SSA use-def, dominance, loop structure, value ranges,
  alias/dependence proof, target profile, and generic pass metrics.

Risk areas:

- OIR inlining/specialization can increase code size and register pressure before MIR cleanup.
- If-conversion and LICM can reduce branches/instructions while increasing live ranges and spills.
- MIR PreRA improvements can be erased by PostRA spills; calibration must compare `pre-ra`,
  `post-ra`, and `final`.
- SMT, PE, and e-graph can increase compile time; all must have budgets and timeout accounting.
- JSON diagnostics can become unstable if candidate IDs depend on pointer values or iteration over
  unordered containers.

## Review Findings

Review date: 2026-07-06

Current verdict:

- The branch does not yet satisfy the full `docs/cost-model-design.md` target. It is best described
  as a cost-model diagnostics layer plus prototype gates for selected OIR/MIR transforms.
- Do not commit or present this branch as a complete cost-model implementation until the gaps below
  are resolved or explicitly downgraded in the design/task scope.

Blocking gaps before returning to `ready_for_review`:

- Default `-O1` is not driven by the cost model. `--cost-model-policy` currently requires
  `--emit-cost-model`, and OIR/MIR helpers return allow when the report artifact is absent. The
  optimized assembly path therefore still mostly follows legacy thresholds, while perf collection
  runs a separate diagnostics compile.
- Pass-local thresholds still dominate important decisions. Inline/specialization retain
  `kMax*` magic numbers, and the cost estimates use coarse fixed discounts such as `info.cost / 2`
  or `info.cost / 3` rather than structured before/after cost vectors.
- SMT is a deterministic proof-result adapter, not an actual bitvector/guard solver integration.
  The caller computes `Proven`/`Refuted`/`Unknown` structurally, and the helper only wraps timeout
  and cache metadata.
- E-graph support is a single `x * 2 -> x + x` expression rule and is disabled in normal `-O1`
  because it requires a cost-model report. It does not build/extract from a candidate set or
  implement equality saturation budgets at the level described in the design.
- PE support is constant-argument specialization with added accounting. It is not a general
  residual-program provider and is not actually linked to a cleanup window that verifies
  SCCP/GVN/DCE realized the expected benefit.
- MIR calibration is not closed over real backend risk. Reports aggregate decision counts, but do
  not map accepted decisions to MIR stage deltas, spills, frame/callee-saved costs, or estimated
  gain totals.
- Several policy fields are not enforced by `decide()`, including module code growth percentage and
  feature allow flags. `memory_pressure_growth` is compared against the register-pressure threshold.
- The inline path still checks `callee->name() == "main"`. This should be replaced with an
  entry-point/ABI property or explicitly justified, because the task forbids function-name keyed
  optimization triggers.
- MIR small-if conversion remains unimplemented/not connected; the task should not claim that part
  of the design is done.

Recommended follow-up checkpoints:

| Checkpoint | Status | Deliverable | Main anchors | Required result |
| --- | --- | --- | --- | --- |
| C10 | pending | Split cost-model reporting from cost-model policy activation so default `-O1` can use decisions without printing diagnostics | `src/main/main.cpp`, `include/oir/OIRScalarOpt.h`, MIR peephole/list-scheduler context plumbing, `include/pass/CostModel.h` | ordinary `-S -O1` stays quiet but uses the same policy path as `--emit-cost-model`; tests compare accepted/rejected behavior under explicit policies |
| C11 | pending | Move pass-local inline/specialization/loop thresholds into structured cost estimates or clearly keep them as legality/resource prefilters | `src/pass/oir/OIRInlinePass.cpp`, `src/pass/oir/OIRLoopTransforms.cpp`, `src/pass/oir/OIRCostModel.cpp` | code growth/register pressure decisions are visible in cost-model decisions rather than hidden in pass-local magic numbers |
| C12 | pending | Replace the SMT adapter with a real bounded proof implementation or downgrade C6 scope in the docs | `include/pass/SMTProof.h`, `src/pass/SMTProof.cpp`, `src/pass/oir/OIRLocalSimplify.cpp` | i32 bitvector proof result is produced by the proof layer; timeout/unknown still reject |
| C13 | pending | Decide whether e-graph is a real saturation/extraction implementation or a scoped peephole rule; update implementation/docs accordingly | `src/pass/oir/OIRLocalSimplify.cpp`, `include/pass/CostModel.h`, `test/ir/cost_model_egraph.sy` | extraction uses candidate alternatives and budgets, or task status explicitly records this as a future non-goal |
| C14 | pending | Extend perf calibration to connect decisions to MIR stage metric deltas and estimated gain totals | `scripts/compare_perf.py`, `src/pass/CostModel.cpp`, `docs/cost-model-calibration.md` | report can identify accepted high-risk decision kinds when post-RA/final metrics regress |
| C15 | pending | Clean up contest-compliance naming risks and missing MIR small-if claim | `src/pass/oir/OIRInlinePass.cpp`, task docs, optional MIR small-if task | no function-name keyed optimization trigger remains unless justified as ABI/entry semantics |

## Goal Execution Contract

For `/goal`, execute checkpoints in order. Do not skip a checkpoint unless this file is updated
with an explicit reason and the skipped work is genuinely unnecessary.

Each checkpoint must end by updating:

- `Status`
- `Context Ledger`
- `Patch Queue`
- `Stage Checkpoints`
- `Verification Matrix`
- `Change Log`
- `Handoff Note`

Checkpoint completion criteria:

- Source compiles if source changed.
- Focused verifier/test gate for the touched layer has run or is explicitly marked not applicable.
- Any generated cost-model JSON is parsed by a test or a simple JSON consumer.
- Any transform-gating change includes an accept and reject path test when feasible.
- Performance-affecting checkpoints include a focused `compare_perf.py` run; final checkpoint
  includes the full performance set unless the environment blocks it.

## Stage Plan

| Checkpoint | Deliverable | Main anchors to promote | Required result |
| --- | --- | --- | --- |
| C0 | Branch setup and source survey | `src/main/main.cpp`, pass manager/context APIs | task branch exists, first 8 anchors selected |
| C1 | Shared cost model core | `include/pass/CostModel.h`, `src/pass/CostModel.cpp` | data types, policies, decision engine, unit/FileCheck-style coverage |
| C2 | Read-only collectors and CLI diagnostics | `src/main/main.cpp`, OIR/MIR collector files, diagnostics artifact path | `--emit-cost-model=json` works and does not change codegen |
| C3 | OIR pass integration batch 1 | `OIRInlinePass`, constant argument specialization, OIR pipeline | accept/reject trace for inline/specialization with policy gates |
| C4 | OIR pass integration batch 2 | OIR if-conversion/loop transform anchors | cost gates for high-growth loop/if transforms, no gate on no-tradeoff cleanup |
| C5 | MIR pass integration | MIR LICM/CSE, small-if, scheduler, diagnostics | PreRA decisions calibrated against PostRA/final metrics |
| C6 | SMT proof-gate integration | SMT obligation/proof API and one guarded i32 rewrite provider | timeout/cache/proven/refuted states appear in decision trace |
| C7 | Partial evaluation integration | specialization/clone machinery, SCCP/GVN/DCE cleanup window | clone/code-growth/specialization budgets gate PE candidates |
| C8 | E-graph extraction integration | OIR expression slice/e-graph provider | extraction uses cost vector and budgets, not minimum node count |
| C9 | Perf calibration and finalization | `scripts/compare_perf.py`, `docs/cost-model-calibration.md` | perf report includes decision summary and calibration notes |

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| C0 | Create implementation branch and promote exact anchors for C1/C2 | task file only | `git status --short` | done | Branch is `task/cost-model-implementation`; exact C1/C2 anchors promoted |
| C1.1 | Add shared public cost model definitions | `include/pass/CostModel.h`, `src/pass/CostModel.cpp`, build config if needed | `xmake` | done | No pass behavior change |
| C1.2 | Add policy defaults and decision scoring | same core files | focused cost model test or JSON smoke | done | Balanced is default for `-O1`; conservative/aggressive parseable |
| C2.1 | Add pass-context artifact and read-only OIR/MIR collectors | cost model core plus collector files | OIR/MIR stage focused tests | done | OIR and FinalMIR summaries emitted through artifact |
| C2.2 | Add CLI flags `--emit-cost-model`, `--emit-cost-model=json`, `--cost-model-filter`, `--cost-model-policy` | `src/main/main.cpp` plus diagnostic printer | CLI smoke, JSON parse | done | Emit mode is mutually exclusive with other emit modes |
| C3.1 | Gate OIR inline and constant-argument specialization through cost decisions | `src/pass/oir/OIRInlinePass.cpp`, OIR cost model | focused FileCheck/e2e | done | Cost-model active decisions now control transform execution; default `-O1` remains old behavior |
| C4.1 | Gate OIR if-conversion and loop transforms with code-growth/live-range risks | OIR transform anchors | OIR stage and e2e focused tests | done | If-conversion accepts/rejects by policy; rotate accepts; unswitch rejects on code-growth risk |
| C5.1 | Gate MIR LICM/CSE/scheduler with PreRA risk and PostRA metrics | MIR pass anchors, MIR cost model | MIR/ASM stage focused tests | done | No MIR small-if pass exists in this tree; PreRA scheduling rejects on risk while PostRA can accept |
| C6.1 | Add SMT obligation/proof result API and guarded i32 provider | SMT/proof anchors | FileCheck/e2e with timeout and unknown cases | done | SMT is ProofGate only; timeout/unknown/refuted reject before profitability |
| C7.1 | Add PE candidate and budget integration | PE/specialization anchors | OIR stage/e2e, JSON trace | done | Cleanup dependency affects risk; policy specialization budget produces reject trace |
| C8.1 | Add e-graph expression-slice provider and cost extraction | e-graph anchors | FileCheck/e2e, budget tests | done | Rejects incomplete/untrusted proof as `ProofUnknown` |
| C9.1 | Extend perf report with cost-model summaries | `scripts/compare_perf.py` | focused/full perf report | done | accepted/rejected counts by kind/reason/proof status are in Markdown and JSON |
| C9.2 | Full validation, calibration doc, final handoff | `docs/cost-model-calibration.md`, task file | full optimized tests and perf set | done | status moved to `ready_for_review` after full gates |

## Stage Checkpoints

Use this table as the durable checkpoint log. Add one row after each completed checkpoint.

| Checkpoint | Status | Summary | Changed files | Verification | Perf / metrics evidence | Next read anchors |
| --- | --- | --- | --- | --- | --- | --- |
| C0 | done | Created `task/cost-model-implementation` and promoted exact C1/C2 anchors | task file, README | `git status --short`; branch/head checked | none | `include/pass/CostModel.h`, `src/pass/CostModel.cpp`, `include/pass/CostModelDiagnosticsPass.h`, `src/pass/CostModelDiagnosticsPass.cpp` |
| C1 | done | Added shared cost vectors, risk vectors, proof/decision types, policies, scoring, and report printers | `include/pass/CostModel.h`, `src/pass/CostModel.cpp` | `xmake`; focused FileCheck | JSON smoke parsed by `python3 -m json.tool` | C2 anchors |
| C2 | done | Added read-only OIR/MIR summary collectors and CLI diagnostics for text/JSON reports | `src/main/main.cpp`, `include/pass/CostModelDiagnosticsPass.h`, `src/pass/CostModelDiagnosticsPass.cpp`, `test/ir/cost_model.sy` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1`; JSON parse; quiet codegen smoke | no perf change expected; diagnostics only | `src/pass/oir/OIRInlinePass.cpp`, `include/oir/OIRScalarOpt.h`, `src/pass/oir/OIROptimizationPipelinePass.cpp` |
| C3 | done | Cost-model decisions gate OIR inline and constant-argument specialization when report/policy is active; report-inactive default `-O1` behavior is preserved | `include/oir/OIRScalarOpt.h`, `include/pass/oir/OIRCostModel.h`, `src/pass/oir/OIRCostModel.cpp`, `src/pass/oir/OIRInlinePass.cpp`, `test/ir/cost_model.sy` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1`; JSON parse; quiet codegen; `python3 scripts/run_tests.py --suite stage --suite e2e --filter basic --jobs 1 --o1` | balanced accepts specialization/inline; conservative filtered trace rejects inline and OIR summary keeps `calls=1` | C4 anchors |
| C4 | done | Added cost-model gates for OIR if-conversion, loop rotate, and loop unswitch; no-tradeoff cleanup passes remain ungated | `src/pass/oir/OIRLocalSimplify.cpp`, `src/pass/oir/OIRLoopTransforms.cpp`, `test/ir/oir_if_conversion.sy`, `test/ir/oir_loop_transforms.sy` | `xmake`; focused FileCheck for `oir_if_conversion` and `oir_loop_transforms`; JSON parse; default stage/e2e smoke | balanced accepts low-risk if-conversion and rotate; unswitch rejects with `NegativeGain` due code-growth risk; conservative if-conversion rejects with `LowConfidence` | `include/pass/mir/MIRDiagnosticsPass.h`, `src/pass/mir/MIRDiagnosticsPass.cpp`, MIR LICM/CSE/scheduler anchors |
| C5 | done | Added MIR cost-model helper and gates for MIR local/global CSE, LICM, division strength reduction, and PreRA/PostRA list scheduling; ordinary report-inactive `-O1` behavior remains unchanged | `include/pass/mir/MIRCostModel.h`, `src/pass/mir/MIRCostModel.cpp`, MIR peephole context bridge, `MIRLocalCSEPass`, `MIRLoopInvariantCodeMotionPass`, `MIRListSchedulerPass`, `test/ir/mir_loop_licm.sy`, `test/ir/mir_list_scheduler.sy` | `xmake`; focused FileCheck for `mir_loop_licm`, `mir_list_scheduler`, `cost_model`, `oir_if_conversion`, `oir_loop_transforms`; direct MIR/ASM emit; JSON parse; basic stage/e2e smoke; `git diff --check` | `--emit-mir-metrics -O1 test/ir/mir_loop_licm.sy`: pre-ra/post-ra/final `80/80/61` instrs, `0` spills; `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=1 python3 scripts/compare_perf.py` PASS, MIR metrics OK, QEMU insn count disabled | C6 SMT proof-gate anchors |
| C6 | done | Added a deterministic SMT proof-gate adapter with obligation/cache/timeout support and connected it to a guarded i32 add/sub algebraic rewrite in OIR local simplify | `include/pass/SMTProof.h`, `src/pass/SMTProof.cpp`, OIR cost-model proof metadata bridge, `src/pass/oir/OIRLocalSimplify.cpp`, `test/ir/cost_model_smt.sy` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter cost_model_smt --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1`; OIR/MIR focused FileCheck regression; JSON parse; basic stage/e2e smoke; `git diff --check` | JSON trace contains `Proven`, `Refuted`, `Timeout`, `Unknown`, `smt.bv32.add_sub_cancel`, and cache solver ids; `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=1 python3 scripts/compare_perf.py` PASS, MIR metrics OK | C7 partial-evaluation anchors |
| C7 | done | Extended constant-argument specialization as the PE provider with shared `PartialEvalCost`, setup/cleanup cost accounting, and policy specialization-budget rejection for excess clones | `include/pass/CostModel.h`, `include/pass/oir/OIRCostModel.h`, `src/pass/oir/OIRCostModel.cpp`, `src/pass/oir/OIRInlinePass.cpp`, `test/ir/cost_model_pe.sy` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter cost_model_pe --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1`; selected OIR/MIR regression; JSON parse; basic stage/e2e smoke; `git diff --check` | PE JSON trace has `PartialEvaluation`, `cleanup_dependency=1`, accepted specialization, and `CodeGrowthTooHigh` budget rejects; `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=1 python3 scripts/compare_perf.py` PASS | C8 e-graph expression-slice anchors |
| C8 | done | Added a bounded OIR integer expression-slice e-graph provider for `x * 2 -> x + x`, using cost/risk extraction and rejecting over-budget/untrusted extraction before profitability | `include/pass/CostModel.h`, `include/pass/oir/OIRCostModel.h`, `src/pass/oir/OIRCostModel.cpp`, `src/pass/oir/OIRLocalSimplify.cpp`, `test/ir/cost_model_egraph.sy` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter cost_model_egraph --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1`; selected OIR/MIR regression; JSON parse; basic stage/e2e smoke; `git diff --check` | E-graph JSON trace has `EGraphRewrite`, `EGraphEquality`, `egraph.bv32.mul2_to_add`; conservative budget path rejects with `ProofUnknown`; perf smoke PASS | C9 perf-report calibration anchors |
| C9 | done | Extended `compare_perf.py` to collect `--emit-cost-model=json -O1` per case, aggregate decision totals, and emit calibration notes in Markdown/JSON reports; added final calibration document | `scripts/compare_perf.py`, `docs/cost-model-calibration.md`, task file, README | `xmake`; focused FileCheck/regression gates; `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1`; `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py`; `git diff --check` | full perf PASS: 119 cases, 0 failed, 40.7021s, GCC geomean `0.9779787144448093`, Clang geomean `1.0094285572486272`, MIR metrics OK, QEMU insn count disabled, cost decisions `3191` accepted / `8434` rejected; later review found this is diagnostic/prototype evidence, not proof of complete design conformance | C10 default `-O1` activation and scope repair |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | every source checkpoint | PASS | build ok after C9 |
| Cost-model JSON smoke | `<compiler> <case.sy> --emit-cost-model=json -O1 | python3 -m json.tool >/tmp/cost-model.json` | after C2 and later diagnostic changes | PASS | ran on `test/bsb-final/2025-G46-5.sy`, `test/ir/cost_model.sy`, `test/ir/oir_if_conversion.sy`, `test/ir/oir_loop_transforms.sy`, `test/ir/mir_loop_licm.sy`, `test/ir/cost_model_smt.sy`, `test/ir/cost_model_pe.sy`, `test/ir/cost_model_egraph.sy`, and final perf-report collection; JSON parsed after C9 |
| Cost-model quiet codegen | compare `-S -O1` output before/after enabling diagnostics off | after C2, C3-C8 | PASS | `build/linux/x86_64/release/compiler test/bsb-final/2025-G46-5.sy -S -O1 -o /tmp/cost-model-quiet.s` produced no cost-model output |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1` | after adding tests | PASS | 4 passed after C9; covers JSON/text/quiet asm, C3 accept/reject, SMT proof-gate trace, PE budget trace, and e-graph extraction trace |
| OIR FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_if_conversion --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter oir_loop_transforms --jobs 1` | C4 | PASS | rerun after C6; covers IfConversion accept/reject, LoopRotate accept, LoopUnswitch reject |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter <case> --jobs 1 --o1` | OIR checkpoints | PASS | covered by `python3 scripts/run_tests.py --suite stage --suite e2e --filter basic --jobs 1 --o1` plus direct `--emit-oir -O1` smoke for `test/ir/cost_model.sy` |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter <case> --jobs 1 --o1` | MIR checkpoints | PASS | direct `build/linux/x86_64/release/compiler test/ir/mir_loop_licm.sy --emit-mir -O1 >/tmp/mir-loop-licm.mir` passed; scripted stage filter matched 0 `test/ir` items |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter <case> --jobs 1 --o1` | MIR/backend or codegen decision checkpoints | PASS | direct `build/linux/x86_64/release/compiler test/ir/mir_list_scheduler.sy -S -O1 -o /tmp/mir-list-scheduler.s` passed |
| E2E focused | `python3 scripts/run_tests.py --suite e2e --filter <case> --jobs 1 --o1` | behavior-affecting checkpoints | PASS | `python3 scripts/run_tests.py --suite stage --suite e2e --filter basic --jobs 1 --o1` passed 5 checks |
| MIR metrics | `<compiler> <case.sy> --emit-mir-metrics -O1` | C5 and C9 | PASS | `test/ir/mir_loop_licm.sy`: lowered/post-combine/pre-ra/post-ra/final instrs `101/94/80/80/61`, spills all `0`; JSON parsed |
| Focused performance | `PERF_TEST_DIRS=<focused-dir> python3 scripts/compare_perf.py` | C3-C9 if decisions affect generated code | PASS | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=1 python3 scripts/compare_perf.py` passed during C3-C8; final full perf supersedes the smoke evidence |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before finalization | PASS | `1439 passed, 0 failed, 1 skipped, 0 xfailed, 0 xpassed`; one skipped case is `test/performance/shuffle1.sy` |
| Full performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before finalization | PASS | 119 cases, 0 failed, total runtime `40.7021s`; GCC geomean `0.9779787144448093`, Clang geomean `1.0094285572486272`; MIR metrics OK; QEMU dynamic instruction count disabled; cost decisions `11625` total, `3191` accepted, `8434` rejected |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| One large implementation patch | Minimizes task-record overhead | rejected: too broad for `/goal` recovery and too hard to verify |
| Diagnostics-only cost model | Low-risk first step | chosen only for C1-C2; final task requires actual pass gating |
| Gate all passes immediately | Appears complete quickly | rejected: no-tradeoff passes should only record metrics, and high-risk gates need staged tests |
| Make SMT/e-graph mandatory before OIR/MIR pass gates | Strong proof story | rejected: handwritten structural proofs can gate early passes; SMT/e-graph are later checkpoints |
| Tune from slow benchmark names | Fast local improvement | rejected: contest-noncompliant and explicitly outside design |

## Checkpoint Handoff Template

Append this shape to `Stage Checkpoints` and update `Handoff Note` at the end of every checkpoint:

```text
Checkpoint:
Implemented:
Legality invariant:
Profitability/risk rule:
Files changed:
Tests run:
Perf evidence:
Decision trace examples:
Regressions or skipped gates:
Next exact anchors:
Next command:
```

## Change Log

- 2026-07-05: created scoped multi-checkpoint task from `docs/cost-model-design.md`.
- 2026-07-05: completed C0-C2 foundation: branch, shared cost model core, read-only OIR/MIR diagnostics, CLI JSON/text output, and focused FileCheck smoke.
- 2026-07-05: started C3 with observational `TransformDecision` recording for OIR inline and constant-argument specialization, including balanced accept and conservative reject trace coverage.
- 2026-07-05: completed C3/C4 OIR gates: inline/specialization, if-conversion, loop rotate, and loop unswitch now use cost-model decisions when diagnostics/policy are active; focused FileCheck and smoke gates pass.
- 2026-07-05: completed C5 MIR gates: local/global CSE, LICM, division strength reduction, and PreRA/PostRA list scheduling now record cost-model decisions when diagnostics/policy are active; focused MIR tests, JSON, metrics, and perf smoke pass.
- 2026-07-05: completed C6 SMT proof gate: added obligation/cache/timeout proof adapter and a guarded i32 add/sub rewrite provider with proven/refuted/timeout/unknown trace coverage.
- 2026-07-05: completed C7 PE integration: constant-argument specialization now records PE clone/residual/cleanup costs and rejects excess clones through the policy budget trace.
- 2026-07-05: completed C8 e-graph integration: added bounded OIR expression-slice extraction for a registered i32 rule and proof/budget trace coverage.
- 2026-07-05: completed C9 finalization: perf reports now aggregate cost-model decisions; calibration notes added; full optimized tests and full performance set pass; status moved to `ready_for_review`.
- 2026-07-06: code review found the branch is not yet design-complete. Status moved to `needs_revision`; added C10-C15 follow-up checkpoints for default `-O1` activation, pass-local threshold cleanup, real/downgraded SMT and e-graph scope, perf calibration linkage, and contest-compliance cleanup.

## Open Questions

- Should the cost model become active for default `-O1` immediately, or should the next checkpoint
  add a hidden/non-printing activation flag first and then flip the default after focused perf?
- Is real SMT/e-graph implementation required for this task, or should C6/C8 be explicitly
  downgraded to prototype diagnostics with separate follow-up tasks?
- Should `main` exclusion in inline be modeled as an entry-point ABI property, or removed if no
  semantic reason remains?

## Handoff Note

Current state:

- C0-C9 are implemented on `task/cost-model-implementation`, but 2026-07-06 review moved the task
  back to `needs_revision`.
- Treat the existing implementation as diagnostics plus prototype gates. It should not be committed
  as the complete `docs/cost-model-design.md` implementation without addressing or re-scoping the
  review findings above.
- `--emit-cost-model` and `--emit-cost-model=json` are mutually exclusive with other emit modes.
- Reports contain read-only OIR and FinalMIR summaries plus C3/C4 OIR and C5 MIR decisions.
- Cost-model-active OIR gates now affect inline, constant-argument specialization, if-conversion,
  loop rotate, and loop unswitch decisions. Report-inactive `-O1` behavior remains unchanged.
- Cost-model-active MIR gates now affect local/global CSE, LICM, division strength reduction, and
  PreRA/PostRA list scheduling decisions. Report-inactive `-O1` behavior remains unchanged.
- Cost-model-active SMT proof-gated OIR algebraic simplify now records `SMT` proof metadata for
  the generic i32 add/sub cancellation rule. Proven candidates may be accepted; refuted, timeout,
  and unknown proofs reject before profitability can commit the rewrite.
- Cost-model-active PE specialization now records clone/residual/cleanup costs. The same constant
  argument specialization machinery rejects excess new clones when the selected policy's
  specialization budget is exceeded.
- Cost-model-active e-graph extraction now handles a bounded OIR i32 `x * 2 -> x + x` expression
  slice. Proven extraction may be accepted; over-budget extraction is `ProofUnknown` and rejected.
- There is no MIR small-if conversion pass in the current source tree, so C5 did not add a
  small-if gate.
- Plain `-S -O1` remains quiet in the focused smoke and FileCheck test.
- `scripts/compare_perf.py` now records cost-model decision summaries in
  `build/perf-ci/perf-report.md` and `build/perf-ci/perf-report.json`.
- Final validation passed: full optimized tests reported `1439 passed, 0 failed, 1 skipped`; full
  perf reported 119 cases, 0 failed, GCC geomean `0.9779787144448093`, Clang geomean
  `1.0094285572486272`, MIR metrics OK, QEMU instruction count disabled, and cost-model decisions
  `3191` accepted / `8434` rejected. This evidence validates the current prototype pipeline, not
  full design conformance.

Next action:

- Start C10: split diagnostics/reporting from policy activation so ordinary quiet `-S -O1` can use
  the same cost-model decision path as `--emit-cost-model`, then re-run focused FileCheck/stage/e2e
  and performance smoke before deciding whether to flip default behavior.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-05-cost-model-implementation.md`
- `docs/cost-model-calibration.md`
- `scripts/compare_perf.py`
- `include/pass/CostModel.h`, `src/pass/CostModel.cpp`
- `include/pass/CostModelDiagnosticsPass.h`, `src/pass/CostModelDiagnosticsPass.cpp`
- `include/pass/oir/OIRCostModel.h`, `src/pass/oir/OIRCostModel.cpp`
- `src/pass/oir/OIRInlinePass.cpp`, `src/pass/oir/OIRLocalSimplify.cpp`,
  `src/pass/oir/OIRLoopTransforms.cpp`
- `include/pass/mir/MIRCostModel.h`, `src/pass/mir/MIRCostModel.cpp`
- `src/pass/mir/MIRLocalCSEPass.cpp`, `src/pass/mir/MIRLoopInvariantCodeMotionPass.cpp`,
  `src/pass/mir/MIRListSchedulerPass.cpp`
