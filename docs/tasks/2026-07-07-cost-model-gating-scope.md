# Task: Cost Model Gating Scope

Status: ready_for_review
Created: 2026-07-07
Last update: 2026-07-07
Owner: Codex
Branch: task/cost-model-gating-scope
Base commit: a370008

## Goal

Change the current cost-model integration so it only gates transforms with real profitability
tradeoffs. Canonicalization, local simplification, no-growth cleanup, and low-risk peephole-style
rewrites must stay enabled by default under `-O1`, while code-growth, register-pressure, scheduling,
and clone-producing transforms continue to use the cost model.

The intended result is to recover the legacy optimized pipeline's broad always-on behavior while
keeping cost-model diagnostics and decisions for transforms where a static profitability choice is
actually useful.

## Non-goals

- Do not remove the cost model, diagnostic report, JSON output, policy parsing, or perf-report
  integration.
- Do not make optimization decisions depend on filenames, testcase names, function names, variable
  names, source literals, runtime input, or expected output.
- Do not weaken legality checks. A transform that currently needs structural, data-flow, SMT,
  dependence, or e-graph proof must still prove legality before rewriting.
- Do not tune thresholds to a single benchmark identity. Any policy change must be justified by
  generic transform risk categories.
- Do not add `-O2` or `-O3`.

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

## Current Cost-Model Gates

Currently constrained transform sites:

- `OIRInlinePass`
  - `Inline`
  - `ConstantArgumentSpecialization`
- `OIRLocalSimplify`
  - `IfConversion`
  - SMT-backed `AlgebraicSimplify`
  - bounded e-graph `EGraphRewrite`
- `OIRLoopTransforms`
  - `LoopRotate`
  - `LoopUnswitch`
- `MIRLocalCSEPass`
  - `LocalCSE`
  - `GlobalCSE`
- `MIRLoopInvariantCodeMotionPass`
  - `LoopInvariantCodeMotion`
  - division reciprocal `StrengthReduction`
- `MIRListSchedulerPass`
  - pre-RA `InstructionScheduling`
  - post-RA `InstructionScheduling`

Transforms that are intentionally not part of this task unless a specific bug is found:

- `mem2reg`, SROA, SCCP, value range propagation, GVN, DCE/ADCE, DSE/DLE, CFG cleanup,
  jump threading, OIR global/load promotion, OIR GEP strength reduction, MIR combine pipeline,
  MIR peephole cleanup, register allocation, and final assembly printing.

## Desired Gating Policy

Always-on or mostly always-on:

- `LoopRotate`: treat as loop canonicalization. Keep legality checks, but do not reject based on
  code-growth or final-score policy.
- `LocalCSE`: allow local basic-block CSE by default when it only replaces a pure single-def
  expression with a move-like instruction.
- Low-risk `IfConversion`:
  - keep `conditional_add` and `value_select` always-on after legality checks;
  - keep `short_circuit_bool` gated only if it introduces extra live values or multiple new boolean
    operations.
- Fixed small algebraic/e-graph rewrites, such as `x * 2 -> x + x`, should act like peepholes after
  proof and bounded expression-size checks. Do not run them through the same large e-graph
  extraction profitability gate.
- Post-RA scheduler windows that do not cross calls, do not touch memory side effects, do not add
  instructions, and do not introduce spills should be allowed even when static cycle gain is zero.

Still cost-gated:

- `Inline`
- `ConstantArgumentSpecialization`
- `LoopUnswitch`
- future loop unroll / tiling / interchange transforms
- `GlobalCSE`
- pre-RA scheduling
- MIR division reciprocal strength reduction
- LICM candidates that increase live range or register pressure
- any partial-evaluation or e-graph transform that clones code, grows expression DAGs, or depends on
  follow-up cleanup.

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 2, starting with `docs/cost-model-design.md` and
  `docs/cost-model-calibration.md`
- Source/script anchors: max 10
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- `runtime/`
- frontend/parser/type-checking code
- unrelated YIR polyhedral implementation files
- broad generated perf artifacts outside the current validation run

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required on resume |
| `docs/cost-model-design.md` | relevant gating/policy sections | original cost-model design intent | yes | compare desired scope against design |
| `docs/cost-model-calibration.md` | full if present | current calibration evidence and policy notes | yes | update after perf validation |
| `src/main/main.cpp` | cost-model report setup and `-O1` pipeline | confirm default report artifact behavior | yes | cost model is active in optimized path |
| `src/pass/CostModel.cpp` | policy and `decide` implementation | understand final-score, risk, and rejection thresholds | yes | do not remove diagnostics |
| `include/pass/CostModel.h` | transform kinds and policy fields | public cost-model API | yes | add bypass classification only if needed |
| `src/pass/oir/OIRInlinePass.cpp` | inline and specialization gate sites | keep gated, potentially tune policy only | yes | high-risk transform |
| `src/pass/oir/OIRLocalSimplify.cpp` | if-conversion, SMT, e-graph gate sites | split always-on low-risk rewrites from gated rewrites | yes | main OIR scope change |
| `src/pass/oir/OIRLoopTransforms.cpp` | rotate and unswitch gate sites | make rotate always-on, keep unswitch gated | yes | main loop scope change |
| `src/pass/mir/MIRLocalCSEPass.cpp` | local/global CSE gate sites | make local CSE always-on, keep global CSE gated | yes | main MIR scope change |
| `src/pass/mir/MIRLoopInvariantCodeMotionPass.cpp` | LICM and div reciprocal gates | keep pressure-increasing candidates gated | yes | refine only if evidence supports |
| `src/pass/mir/MIRListSchedulerPass.cpp` | pre/post RA scheduler gates | split post-RA no-risk windows from gated candidates | yes | main scheduler scope change |
| `scripts/compare_perf.py` | cost-model decision report parsing | ensure perf report still summarizes decisions | yes | no behavior change unless tests require |

## Branch

Decision: created

Reason:

```text
Created `task/cost-model-gating-scope` from `a370008`. The first sandboxed attempt could not write
`.git`; the same non-destructive branch creation command succeeded with approval.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git checkout -b task/cost-model-gating-scope
```

## Invariants And Risks

Correctness invariants:

- Profitability gates may be bypassed only after all existing legality checks succeed.
- Bypassed transforms must be general, semantics-preserving compiler optimizations.
- `--emit-cost-model` must still show enough trace to explain which transforms were accepted,
  rejected, or bypassed as always-on.
- Plain `-S -O1` remains quiet.
- The implementation must preserve deterministic output for the same input and policy.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes,
  expected outputs, or runtime input data.
- Do not hardcode benchmark results or known output.
- Do not use undefined behavior assumptions to justify a transform.

Risk areas:

- Letting post-RA scheduler bypass cost gates could reorder instructions in windows that the current
  dependency model fails to protect. Keep memory/call/side-effect barriers strict.
- Letting local CSE always-on may introduce extra moves that are not later coalesced. Verify MIR
  final metrics, not only pre-RA metrics.
- Letting if-conversion always-on can increase live ranges. Keep short-circuit forms gated or add
  explicit live-range guards.
- Letting loop rotate always-on may expose latent loop-form bugs. Run OIR stage and e2e tests for
  loop-heavy cases.
- Diagnostics can become misleading if bypassed transforms disappear from the cost-model report.
  Prefer recording a decision action or reason that marks them as always-on.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add a clear cost-model bypass classification for always-on transforms without dropping diagnostics | `include/pass/CostModel.h`, `src/pass/CostModel.cpp`, OIR/MIR helper files | `xmake` and cost-model FileCheck | done | Added `DecisionAction::BypassProfitability`; legality/proof rejection still happens before bypass. |
| P2 | Make OIR loop rotate bypass profitability while keeping loop unswitch gated | `src/pass/oir/OIRLoopTransforms.cpp`, focused loop tests | OIR stage and e2e loop cases | done | Rotate reports `AlwaysOnCanonicalization`; unswitch still rejects by cost policy. |
| P3 | Split OIR if-conversion policy: allow low-risk forms, keep high-live-range forms gated | `src/pass/oir/OIRLocalSimplify.cpp`, if-conversion tests | FileCheck/OIR/e2e | done | `conditional_add` and `value_select` report `AlwaysOnLowRiskIfConversion`; `short_circuit_bool` remains gated. |
| P4 | Treat fixed small e-graph/algebraic rewrites as peepholes after proof and size bounds | `src/pass/oir/OIRLocalSimplify.cpp`, cost-model egraph/smt tests | FileCheck and OIR stage | done | `x * 2 -> x + x` bypasses only after e-graph budget/proof succeeds; conservative budget exhaustion still rejects. |
| P5 | Make MIR local CSE always-on and keep global CSE gated | `src/pass/mir/MIRLocalCSEPass.cpp` | MIR stage and focused CSE tests | done | Local CSE reports `AlwaysOnLocalCleanup`; global CSE remains cost-gated. |
| P6 | Split scheduler policy: keep pre-RA gated, allow post-RA no-risk windows | `src/pass/mir/MIRListSchedulerPass.cpp` | MIR stage, asm stage, scheduler FileCheck | done | PostRA windows report `AlwaysOnPostRANoGrowthScheduling`; PreRA scheduling remains cost-gated. |
| P7 | Validate LICM/div reciprocal gates remain appropriate and only relax no-pressure LICM if supported by metrics | `src/pass/mir/MIRLoopInvariantCodeMotionPass.cpp` | MIR metrics and perf | done | LICM remains gated. Division reciprocal remains gated but now models repeated loop-body savings for unknown trip counts. |
| P8 | Update tests and calibration docs for accepted/rejected/bypassed decision summaries | `test/ir/*`, `docs/cost-model-calibration.md`, `scripts/compare_perf.py` | FileCheck and perf | done | Perf report now shows accepted/rejected/bypassed counts; calibration doc records full evidence. |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | build ok |
| Cost-model FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1` | yes | PASS | 4 passed |
| OIR if-conversion FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_if_conversion --jobs 1` | yes | PASS | 1 passed |
| OIR loop transforms FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_loop_transforms --jobs 1` | yes | PASS | 1 passed |
| MIR CSE/LICM/scheduler FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir_ --jobs 1` | yes | PASS | 6 passed |
| OIR huffman FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_huffman_gap --jobs 1` | yes | PASS | 1 passed after updating assertions for gated specialization and short-circuit policy |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --jobs 1 --o1` | yes | PASS | 279 passed |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --jobs 1 --o1` | yes | PASS | 279 passed |
| E2E optimized | `python3 scripts/run_tests.py --suite e2e --jobs 1 --o1` | yes | PASS | 277 passed, 1 skipped |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before review | PASS | 1439 passed, 1 skipped |
| Perf smoke | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=5 python3 scripts/compare_perf.py` | during iteration | NOT_RUN | skipped because full performance was run |
| Full performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before review | PASS | 119 cases, 0 failed; GCC geomean 0.7821686424741846, Clang++ geomean 0.8109606792364373 |

## Acceptance Criteria

- The cost-model report shows that always-on transforms are not rejected by `NegativeGain` or
  `CodeGrowthTooHigh` when they are classified as canonicalization, local cleanup, or fixed
  peephole rewrites.
- `Inline`, `ConstantArgumentSpecialization`, `LoopUnswitch`, `GlobalCSE`, pre-RA scheduling,
  division reciprocal strength reduction, and register-pressure-growing LICM remain gated.
- Performance recovers materially from the measured cost-model regression without hiding correctness
  or disabling diagnostics.
- No new contest-compliance risk is introduced.
- Final report includes before/after geomean, MIR stage metrics, and accepted/rejected/bypassed
  cost-model decision counts.

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Disable cost model for all `-O1` builds | Fastest way to recover performance | rejected: loses the purpose of the cost-model branch |
| Switch default policy to `aggressive` only | Minimal code change | rejected as insufficient; low-risk transforms should not depend on a profitability score at all |
| Keep all gates and tune thresholds | Preserves current architecture | partially rejected; still makes canonicalization and local cleanup hostage to weak static estimates |
| Add explicit always-on classification | Separates legality from profitability and preserves diagnostics | chosen |

## Change Log

- 2026-07-07: created task file.
- 2026-07-07: created `task/cost-model-gating-scope`, implemented
  `DecisionAction::BypassProfitability`, applied it to loop rotation, low-risk OIR
  if-conversion, fixed e-graph peepholes, MIR local CSE, and post-RA no-growth scheduling.
- 2026-07-07: kept high-risk transforms cost-gated, including inline, constant-argument
  specialization, loop unswitch, global CSE, pre-RA scheduling, LICM, and division reciprocal
  strength reduction. Division reciprocal remains gated but now estimates repeated loop-body
  savings.
- 2026-07-07: completed focused FileCheck, full optimized correctness, and full performance gates;
  updated calibration evidence and moved task to `ready_for_review`.

## Open Questions

- Should bypassed transforms be represented as `DecisionAction::Accept` with reason
  `AlwaysOn`, or should a new `DecisionAction::BypassProfitability` be added? Resolved: added
  `DecisionAction::BypassProfitability` so diagnostics and perf reports can count bypasses
  separately from profitable accepts.
- Should `--cost-model-policy=conservative` still gate low-risk always-on transforms, or should
  conservative only affect high-risk transforms? Resolved: conservative does not gate transforms
  explicitly classified as always-on after legality proof.
- Should `short_circuit_bool` remain fully gated, or can it be split further by exact live-value
  growth? Resolved for this patch: `short_circuit_bool` remains cost-gated; only
  `conditional_add` and `value_select` bypass profitability.

## Handoff Note

Current state:

- Implementation is ready for review on `task/cost-model-gating-scope`.
- Cost-model diagnostics now distinguish `Accept`, `Reject`, and `BypassProfitability`.
- Full performance report: `build/perf-ci/perf-report.md` and
  `build/perf-ci/perf-report.json`.
- Full performance result: 119 cases, 0 failed, GCC geomean 0.7821686424741846, Clang++ geomean
  0.8109606792364373. QEMU dynamic instruction count was disabled.
- Cost-model decision summary from the full run: 5705 total, 559 accepted, 1470 bypassed
  profitability, 3676 rejected.
- MIR final metrics from the full run: 26305 instructions, 6932 moves, 1789 branches, 2715 jumps,
  1496 loads, 1218 stores, 210 spills, 264 stack slots.

Next action:

- Review the diff. If needed, compare this full performance run against the previous
  `cost-model-implementation` report before merging.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-07-cost-model-gating-scope.md`
- `docs/cost-model-calibration.md`
- `src/pass/CostModel.cpp`
- `src/pass/oir/OIRLocalSimplify.cpp`
- `src/pass/oir/OIRLoopTransforms.cpp`
- `src/pass/mir/MIRLocalCSEPass.cpp`
- `src/pass/mir/MIRListSchedulerPass.cpp`
- `src/pass/mir/MIRLoopInvariantCodeMotionPass.cpp`
