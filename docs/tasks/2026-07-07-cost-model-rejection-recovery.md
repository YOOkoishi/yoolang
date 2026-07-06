# Task: Cost Model Rejection Recovery

Status: ready_for_review
Created: 2026-07-07
Last update: 2026-07-07
Owner: implementation subagent
Branch: task/cost-model-rejection-recovery
Base commit: 0346fb7

## Goal

Identify which legal, generally profitable optimization passes or transform candidates are being
incorrectly rejected by the current cost model, then fix those generic cost estimates, risk
penalties, or pass-level candidate facts so performance and generated instruction counts recover
toward `master` without disabling the cost model.

The implementation must compare current behavior against `master` where practical, inspect
`--emit-cost-model=json`, perf reports, MIR metrics, and generated MIR/ASM for the regressing cases,
and record which rejected pass decisions were true cost-model mistakes versus legitimate legality,
code-growth, proof-timeout, or register-pressure rejections.

## Non-goals

- Do not remove, globally disable, or bypass the shared cost model to match `master`.
- Do not make the default `-O1` pipeline use `--cost-model-policy=aggressive` as a blanket fix.
- Do not reopen the prior decision that low-risk canonicalization/local-cleanup transforms may use
  `BypassProfitability`, unless new evidence shows that classification itself causes a regression.
- Do not blindly accept every transform that `master` used. `master` is an attribution baseline, not
  a legality or profitability proof.
- Do not special-case benchmark files, function names, variable names, literal strings, input sizes,
  testcase identities, runtime input, or expected outputs.
- Do not weaken legality/proof checks, verifier coverage, expected outputs, or perf scripts to hide
  regressions.
- Do not add `-O2` or `-O3` support.

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

## Current Evidence And Scope

The previous cost-model tasks recovered much of the regression but left measurable gaps relative to
`master`.

Current full perf evidence in `build/perf-ci/perf-report.md`, generated 2026-07-06:

- 119 cases, 0 failed, GCC geomean `0.936692009660409`, Clang++ geomean
  `0.9719614534957396`.
- MIR final totals: 33672 instructions, 7808 moves, 2587 branches, 3621 jumps, 1800 loads, 1164
  stores, 162 spills, 216 stack slots.
- QEMU dynamic instruction counting was disabled in this report, so instruction-count claims need a
  rerun with instruction counting enabled if the toolchain supports it.
- Cost-model decisions: 2113 accepted / 2085 bypassed / 8208 rejected.
- Remaining transform rejection totals include:
  - `OIRLocalSimplify/AlgebraicSimplify`: 3174 `ProofTimeout`, 780 `Illegal`.
  - `OIRLocalSimplify/IfConversion`: 2706 `NegativeGain`.
  - `OIRInlinePass/Inline`: 27 `CodeGrowthTooHigh`, 21 `NegativeGain`.
  - `OIRInlinePass/ConstantArgumentSpecialization`: 72 `CodeGrowthTooHigh`.
  - `MIRPreRAListSchedulerPass/InstructionScheduling`: 714 `NegativeGain`.
  - `OIRLoopTransforms/LoopUnswitch`: 714 `CodeGrowthTooHigh`.

Current `master` comparison in `build/perf-ci/master-baseline-compare.md`:

- Overall status OK, but current is still slower than `master`: 13.4045s vs 13.3267s, total delta
  +0.0778s (-0.58%), with 33 faster / 82 slower / 4 tied.
- Explicit per-case regression flags: `test/bsb-final/2025-PDZ-59.sy` (+0.0523s, -109.41%) and
  `test/performance/crypto-1.sy` (+0.0515s, -108.42%).
- QEMU dynamic instruction comparison was skipped due to missing comparable instruction counts.

The implementation should begin with these two flagged cases, then expand to the largest remaining
current-vs-master losses or instruction-count regressions if the first two do not explain the gap.

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 3, starting with `docs/cost-model-calibration.md`,
  `docs/tasks/2026-07-07-cost-model-pass-activation.md`, and
  `docs/tasks/2026-07-07-cost-model-gating-scope.md`
- Source/script anchors: max 8
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- `runtime/`
- frontend parser/type-checking code
- unrelated YIR polyhedral implementation files
- unrelated benchmark sources beyond the measured regression cases
- broad generated artifacts outside `build/perf-ci/perf-report.md`,
  `build/perf-ci/perf-report.json`, `build/perf-ci/master-baseline-compare.md`, and focused
  per-case artifacts under `build/perf-ci/<test-dir>/<case>/`

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required on resume |
| `docs/tasks/README.md` | Active Tasks | task index update rules | yes | updated for this task |
| `docs/tasks/TEMPLATE.md` | full | task shape | yes | used to create this file |
| `/home/yoo/.codex/skills/yoolang-optimization/SKILL.md` | full | contest legality and perf validation discipline | yes | required for optimization work |
| `docs/cost-model-calibration.md` | full | current calibration evidence and cost-model contract | yes | historical baseline, update if evidence changes |
| `docs/tasks/2026-07-07-cost-model-pass-activation.md` | full | previous activation changes and remaining master risk | yes | avoid repeating completed calibration |
| `docs/tasks/2026-07-07-cost-model-gating-scope.md` | full/relevant sections | prior always-on/bypass decisions | yes | confirms which transforms intentionally bypass profitability |
| `build/perf-ci/perf-report.md` | summary, MIR metrics, cost-model tables | current full-run decision and metrics evidence | yes | stale after new runs; replace with fresh evidence |
| `build/perf-ci/perf-report.json` | `rows`, `cost_model_decision_summary`, per-row `codegen_metrics` | per-case rejection and MIR metrics attribution | yes | use structured parsing instead of manual grep |
| `build/perf-ci/master-baseline-compare.md` | full | current-vs-master timing risk | yes | identifies first regression cases |
| `build/perf-ci/master-baseline-compare.json` | `regressions` and per-case deltas | sort losses and instruction-count comparison if present | yes | current instruction counts are skipped |
| `scripts/compare_perf.py` | cost-model/instruction-count reporting paths | rerun full/focused perf and ensure reports expose needed attribution | yes | change only if report lacks required evidence |
| `scripts/compare_perf_baseline.py` | baseline comparison behavior | compare current vs master/current baselines | yes | use existing thresholds; do not weaken them |
| `include/pass/CostModel.h` | policy fields and transform structs | public cost-model API and risk vectors | yes | edit only if generic model fields need adjustment |
| `src/pass/CostModel.cpp` | `policy_for_kind`, `weighted_risk`, `decide` | central thresholds, risk penalties, reject reasons | yes | avoid blanket policy relaxation |
| `src/pass/oir/OIRCostModel.cpp` | `cost_model_allows_transform` | OIR candidate conversion/reporting | no | promote only if reports lack OIR pass metadata |
| `src/pass/mir/MIRCostModel.cpp` | `allows_transform` | MIR candidate conversion/reporting | no | promote only if reports lack MIR pass metadata |
| `src/pass/oir/OIRInlinePass.cpp` | inline/specialization estimates | rejects may be stale or too conservative | yes | high priority if regressing cases show missed clone/inline wins |
| `src/pass/oir/OIRLocalSimplify.cpp` | if-conversion/algebraic simplify estimates | high rejection volume | yes | distinguish true proof failures from stale profitability |
| `src/pass/oir/OIRLoopTransforms.cpp` | loop unswitch estimates | code-growth rejections may be legitimate or overbroad | no | promote only if measured loop evidence points to unswitch |
| `src/pass/mir/MIRListSchedulerPass.cpp` | PreRA scheduling estimates | 714 negative-gain rejects and instruction-count risk | yes | high priority for instruction count/codegen deltas |
| `src/pass/mir/MIRLoopInvariantCodeMotionPass.cpp` | LICM/strength reduction estimates | accepted transforms may alter pressure; rejected ones may matter | no | promote only if MIR metrics point here |
| `src/pass/mir/MIRLocalCSEPass.cpp` | GlobalCSE estimates | accepted currently; check for over/under-activation side effects | no | promote only if master assembly shows missed CSE |

## Branch

Decision: create task branch at implementation start

Reason:

```text
This is a performance-sensitive multi-patch investigation touching cost-model estimates, OIR/MIR
passes, tests, and perf documentation. The current task-generation context is on
`cost-model-verified` at clean HEAD `0346fb7`; implementation should create
`task/cost-model-rejection-recovery` from this commit before editing code.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git checkout -b task/cost-model-rejection-recovery
git merge-base HEAD master
```

Result:

```text
Initial status: M docs/tasks/README.md; ?? docs/tasks/2026-07-07-cost-model-rejection-recovery.md
Initial HEAD: 0346fb7
Created branch: task/cost-model-rejection-recovery
Merge-base with master: 6bc9662b0dc4205e2b56b98eb5bd58debb3fbdcf
```

## Invariants And Risks

Correctness invariants:

- Every transform that becomes accepted must still satisfy its existing legality proof and IR/MIR
  verifier invariants.
- A cost-model rejection may be changed only after evidence shows a generic estimate, confidence,
  risk, policy threshold, or candidate metadata mistake.
- Fixes must be general across matching IR/MIR patterns and target-cost facts, not keyed to
  benchmark identity.
- `--emit-cost-model[=json]` must keep reporting accepted, rejected, bypassed, or filtered
  decisions with pass, transform, proof, score, risk, and reason fields.
- Default `-O1` must remain quiet unless a diagnostic emit option is requested.
- `Conservative`, `Balanced`, and `Aggressive` policies must remain meaningfully distinct.
- Instruction-count improvements must not come from deleting required work, changing semantics, or
  exploiting undefined behavior.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes,
  expected outputs, source literals, or runtime input data.
- Do not hardcode benchmark results or known outputs.
- Do not add assumptions that make valid SysY inputs incorrect.
- Do not weaken tests, exclude perf cases, or alter perf baselines to make results look better.

Risk areas:

- Relaxing `CodeGrowthTooHigh` for inline/specialization/unswitch may increase final instruction
  count and I-cache pressure even if a few hot cases improve.
- Relaxing PreRA scheduling or LICM risk can increase spills, loads, stores, and stack slots after
  register allocation. Attribute deltas across `pre-ra -> post-ra -> final`.
- Treating `ProofTimeout` as a profitability bug can be wrong. Timeouts are legality/proof-provider
  issues unless there is a cheaper structural proof or bounded prefilter.
- If-conversion negative-gain rejections may protect branch-heavy code from extra live ranges; use
  emitted OIR/MIR and final ASM before accepting more candidates.
- Focused case wins can regress the 119-case full suite. Full performance comparison against both
  prior current and `master` evidence is required before review.
- QEMU instruction counting may be unavailable. If so, report that explicitly and use MIR final
  instruction/move/load/store/spill metrics as the fallback.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Reproduce and attribute current-vs-master gaps for `test/bsb-final/2025-PDZ-59.sy` and `test/performance/crypto-1.sy`, including cost-model JSON, MIR metrics, and final ASM differences | `build/perf-ci/*`, focused emitted artifacts, task file | focused `--emit-cost-model=json -O1`; focused `--emit-mir-metrics -O1`; existing `build/perf-ci/*` current-vs-master reports | done | The two flagged cases are byte-identical in source/codegen profile. Initial attribution found only SMT `AlgebraicSimplify` proof timeouts, 1 illegal SMT proof, and PreRA scheduling `NegativeGain` windows, with no inline/specialization/if-conversion/unswitch rejection. P3-repair later identified the relevant subset of `ProofTimeout` rejections as exact add/sub cancellations that should have used a structural proof. |
| P2 | Add or improve analysis/reporting only if current reports do not expose rejected pass/candidate attribution by case | none | existing report JSON and focused emits | done | No script change needed; existing `perf-report.json` per-row `codegen_metrics.cost_model_summary` exposed pass/transform/action/reject counts. |
| P3 | Fix the highest-confidence incorrect OIR rejection from the attribution, likely inline, constant-argument specialization, if-conversion, or loop-unswitch estimate metadata if evidence supports it | `src/pass/oir/OIRLocalSimplify.cpp` | `xmake`; cost-model FileCheck; focused OIR/e2e/perf | done | `short_circuit_bool` if-conversion always reported one unit of register/live-range growth even for the single-result boolean expression form. On huffman-style loss cases this turned 57 proven branch-removal candidates from final score `1` into `NegativeGain` score `-7`. Changed metadata to pressure growth `0`; transform remains cost-gated and must still score positive. |
| P3-repair | Fix crypto/PDZ attribution: exact `(x - y) + y` add/sub cancellation was incorrectly routed through SMT timeout when `x` was complex, despite being a structural SSA/bitvector identity | `src/pass/oir/OIRLocalSimplify.cpp`, `test/ir/cost_model_smt.sy`, this task file | `xmake`; cost-model FileCheck; focused crypto/PDZ e2e/perf; full perf | done | Added a structural proof path for exact RHS cancellation before the SMT budget check. Crypto/PDZ now have 33 structural accepted algebraic simplifications, OIR line count matches fresh master (586 lines), and final MIR improves from 661 instructions / 57 jumps to 595 / 52. Remaining SMT rejections on crypto are 112 `ProofTimeout` and 1 `Illegal`. This is generic algebraic simplification, not keyed to crypto/PDZ. |
| P4 | Fix the highest-confidence incorrect MIR/backend rejection from the attribution, likely PreRA scheduling, LICM/strength reduction, or GlobalCSE estimate metadata if evidence supports it | none | MIR metrics attribution and final perf | done | No MIR/backend rejection fix was supported. PreRA scheduling rejections on the crypto-equivalent cases had zero or negative modeled cycle gain; accepting zero-gain windows was not justified. |
| P5 | Update focused tests and calibration docs for any accepted model change | `test/ir/oir_if_conversion.sy`, `test/ir/oir_huffman_gap.sy`, `docs/cost-model-calibration.md`, this task file | focused FileCheck and full all-suite gate | done | Added/updated FileCheck coverage for profitable short-circuit if-conversion under `Balanced` and conservative low-confidence rejection. |
| P6 | Run final correctness and performance gates, compare current vs pre-task current and `master`, and record instruction-count/MIR metric outcome | this task file, generated perf reports | full commands in Verification Matrix | done | Full perf used `PERF_TEST_DIRS=test/performance,test/bsb-final` with no `PERF_MAX_CASES`. QEMU instruction count was attempted but failed due plugin API mismatch; MIR metrics used as fallback. |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | Build ok after `src/pass/oir/OIRLocalSimplify.cpp` change. |
| Repair build | `xmake` | yes | PASS | Rebuilt after adding structural add/sub cancellation proof path. |
| Cost-model FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1` | yes | PASS | 4 passed. |
| Repair SMT FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter cost_model_smt --jobs 1` | yes | PASS | 1 passed after updating fixtures: exact complex cancellation is structural/proven; non-cancelling complex/unsupported cases still cover SMT `ProofTimeout` and `ProofUnknown`. |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_if_conversion --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter oir_huffman_gap --jobs 1` | if IR/MIR/ASM shape changes | PASS | `oir_if_conversion` 1 passed; `oir_huffman_gap` 1 passed after updating expected if-converted shape. |
| OIR stage focused | `python3 scripts/run_tests.py --suite stage --stage oir --filter huffman-01 --jobs 1 --o1` | if OIR affected | PASS | 1 passed. |
| MIR stage focused | `python3 scripts/run_tests.py --suite stage --stage mir --filter <case> --jobs 1 --o1` | if MIR affected | SKIP | No MIR/backend source change. Full stage gate covered MIR. |
| ASM stage focused | `python3 scripts/run_tests.py --suite stage --stage asm --filter <case> --jobs 1 --o1` | if backend/codegen affected | SKIP | No MIR/backend source change. Full stage gate covered ASM. |
| E2E focused | `python3 scripts/run_tests.py --suite e2e --filter huffman-01 --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e --filter crypto-1 --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e --filter 2025-PDZ-59 --jobs 1 --o1` | yes | PASS | Each focused command passed 1 case. |
| Repair targeted e2e | `python3 scripts/run_tests.py --suite e2e --filter crypto-1 --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e --filter 2025-PDZ-59 --jobs 1 --o1` | yes | PASS | Both originally flagged cases passed after structural add/sub cancellation repair. |
| Full optimized stage/e2e | `python3 scripts/run_tests.py --suite stage --suite e2e --jobs 1 --o1` | yes | PASS | 1393 passed, 1 skipped. |
| Full optimized all | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before review | PASS | Repair rerun after structural add/sub cancellation passed: 1439 passed, 1 skipped. |
| Focused perf attribution | `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` | yes | PASS | 60 cases, 0 failed; focused report showed `IfConversion/Accept=81` and `huffman-01` final MIR 479 instrs / 78 moves / 55 jumps / 37 branches / 0 spills. |
| Repair targeted perf | `PERF_TEST_DIRS=test/performance/crypto-1.sy,test/bsb-final/2025-PDZ-59.sy python3 scripts/compare_perf.py` | yes | PASS | 2 cases, 0 failed. Crypto-1 compiler time recovered to 0.0494s; PDZ recovered to 0.0487s. These match fresh master within timing noise and are no longer above the +0.05s per-case regression threshold. This focused run temporarily overwrote `build/perf-ci/perf-report.*`; full perf must be rerun before review. |
| Cost-model JSON attribution | `build/linux/x86_64/release/compiler test/performance/crypto-1.sy --emit-cost-model=json -O1`; `build/linux/x86_64/release/compiler test/bsb-final/2025-PDZ-59.sy --emit-cost-model=json -O1`; `build/linux/x86_64/release/compiler test/performance/huffman-01.sy --emit-cost-model=json -O1` | yes | PASS | Crypto and PDZ JSON are identical. After repair, crypto has 33 `AlgebraicSimplify/Accept` decisions with `Structural/Proven` exact add/sub cancellation, plus 112 remaining SMT `ProofTimeout` and 1 `Illegal` rejection. Huffman before/after showed 57 `IfConversion/NegativeGain` rejections removed and 5 profitable if-conversions accepted. |
| MIR metrics attribution | `build/linux/x86_64/release/compiler test/performance/crypto-1.sy --emit-mir-metrics -O1`; `build/linux/x86_64/release/compiler test/bsb-final/2025-PDZ-59.sy --emit-mir-metrics -O1`; `build/linux/x86_64/release/compiler test/performance/huffman-01.sy --emit-mir-metrics -O1` | yes | PASS | Crypto and PDZ repair metrics are identical: final MIR 595 instrs / 98 moves / 52 jumps / 28 branches / 18 loads / 50 stores / 2 spills / 8 stack slots, improved from the failed 661-instruction profile. Huffman final MIR improved from 481 instrs / 81 moves / 57 jumps / 40 branches to 479 / 78 / 55 / 37 with 0 spills. |
| Assembly attribution | Current and fresh-master focused OIR/MIR/ASM artifacts for crypto/PDZ | yes | PASS | After structural add/sub repair, crypto OIR line count matches fresh master (586 lines) except the intended generic `x * 2 -> x + x` e-graph rewrite; final MIR is smaller than fresh master for the targeted cases (595 vs 604 instructions). |
| Full performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | yes | PASS | Final repair run passed 119 cases, 0 failed, total runtime 41.7533s, compiler total 13.3101s, GCC geomean 0.9702487919411927, Clang++ geomean 1.0135988680507693. Final MIR totals: 33126 instructions, 7628 moves, 2437 branches, 3405 jumps, 1782 loads, 1170 stores, 168 spills, 222 stack slots. |
| Instruction count perf | `ENABLE_QEMU_INSN_COUNT=1 PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py`; direct repair check `python3 tools/qemu-insn-count/count_insn.py qemu-riscv64 -L /usr/riscv64-linux-gnu build/perf-ci/test/performance/crypto-1/crypto-1.compiler.riscv` | if QEMU supports it | FAIL / FALLBACK | Prior counted perf passed 119 cases but instruction counts failed for all cases. Direct post-repair helper invocation reproduced the environment failure without overwriting the final report: `plugin requires API version 1, but this QEMU supports only a minimum version of 2`. MIR metrics are the fallback. |
| Saved master baseline comparison | `python3 scripts/compare_perf_baseline.py --current build/perf-ci/perf-report.json --baseline /tmp/yoolang-master-baseline-from-compare.json --out-md build/perf-ci/master-baseline-compare.md --out-json build/perf-ci/master-baseline-compare.json --baseline-label master --baseline-branch master` | no; historical continuity | FAIL | Repair rerun against the saved master baseline failed before P3-repair: current 13.6163s vs saved master 13.3267s, delta +0.2896s, with crypto-1 +0.0548s and PDZ +0.0526s. This was superseded by the fresh-master final comparison below after P3-repair. |
| Fresh master baseline comparison | temp clone `/tmp/yoolang-master-fresh` at `6bc9662`; `xmake`; `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py`; then `python3 scripts/compare_perf_baseline.py --current build/perf-ci/perf-report.json --baseline /tmp/yoolang-master-fresh/build/perf-ci/perf-report.json --out-md build/perf-ci/master-baseline-compare.md --out-json build/perf-ci/master-baseline-compare.json --baseline-label master-fresh --baseline-branch master --baseline-commit-sha 6bc9662b0dc4205e2b56b98eb5bd58debb3fbdcf` | yes | PASS | Final current-vs-fresh-master: current 13.3103s vs master 13.6068s, delta -0.2965s (+2.18%), 45 faster / 72 slower / 2 tied, and no significant per-case regressions. Crypto-1 is 0.0483s; PDZ is 0.0493s. Both targeted regressions are recovered and below the +0.05s threshold. |

## Acceptance Criteria

- The task records a concrete attribution table for at least the two current regression flags:
  `test/bsb-final/2025-PDZ-59.sy` and `test/performance/crypto-1.sy`.
- For every changed threshold or estimate, the task explains the generic compiler pattern and why
  the previous rejection was wrong.
- At least one incorrectly rejected legal/profitable transform is fixed, or the task records
  evidence that remaining rejections are legitimate and the regression comes from accepted/bypassed
  transforms instead.
- Full optimized correctness tests pass, or any skipped gate has an explicit environment reason.
- Full performance on `test/performance,test/bsb-final` does not regress relative to the pre-task
  current evidence and improves current-vs-master total delta or the targeted per-case regressions.
- Instruction-count comparison is performed with QEMU if available; otherwise MIR final instruction,
  move, load, store, spill, and stack-slot metrics are used and recorded as fallback evidence.
- Contest-compliance constraints remain satisfied.

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Disable the cost model for passes that are slower than `master` | Would recover old behavior quickly | rejected: violates the task goal and hides model mistakes |
| Lower `Balanced::min_final_score` globally | Might accept many rejected candidates | rejected initially: too broad and risks code-size/spill regressions |
| Switch default policy to `Aggressive` | Simple activation increase | rejected: erases policy separation and may overfit |
| Fix per-pass estimate metadata based on current-vs-master attribution | More work but preserves cost-model architecture | chosen |
| Treat proof timeouts as accepted if runtime improves | Could recover algebraic simplify opportunities | rejected: proof timeout is not legality proof |

## Change Log

- 2026-07-07: created task file from user request, current perf reports, prior cost-model tasks, and
  yoolang optimization rules; status set to `scoped`.
- 2026-07-07: implementation started; created `task/cost-model-rejection-recovery` from `0346fb7`;
  status set to `in_progress`.
- 2026-07-07: completed focused attribution for the two flagged crypto-equivalent cases; found no
  incorrect OIR profitability rejection there, then fixed a generic OIR `short_circuit_bool`
  if-conversion risk metadata issue exposed by the next largest huffman-style losses.
- 2026-07-07: updated focused OIR FileCheck coverage and huffman expected OIR shape for the newly
  accepted short-circuit if-conversion.
- 2026-07-07: ran full optimized correctness and performance gates; status set to
  `ready_for_review`. Perf passed, QEMU instruction counting failed due local plugin API mismatch,
  and MIR metrics were recorded as fallback evidence.
- 2026-07-07: repair review found the final master-baseline comparison failed the task acceptance
  gate: current-vs-master delta worsened from the pre-task +0.0778s to +0.3941s, with crypto-1 and
  PDZ still regressed. Status reset to `in_progress`; the master-baseline gate is recorded as
  failing until fresh comparable perf evidence and/or a repair satisfies the acceptance criterion.
- 2026-07-07: repair reran full current perf and the saved master-baseline comparison. The refreshed
  run reduced the measured delta to +0.2896s, but at that point the acceptance gate failed:
  crypto-1 and PDZ remained above the per-case regression threshold and the total delta was worse
  than pre-task current.
- 2026-07-07: repair built a fresh `/tmp` clone of `master` at `6bc9662` and reran the same full
  perf methodology. The overall total delta is only +0.0095s against fresh master, but the targeted
  crypto-1/PDZ regressions are confirmed at +0.0531s/+0.0513s, so repair continues on those cases.
- 2026-07-07: repair identified the targeted crypto/PDZ gap as exact add/sub cancellation rejected
  by the cost model as `ProofTimeout` when the cancelled expression was complex. Began P3-repair by
  adding a structural proof path for exact `(x - y) + y` / `y + (x - y)` cancellation.
- 2026-07-07: focused targeted e2e and perf passed after P3-repair. Crypto-1 recovered to 0.0494s
  and PDZ to 0.0487s in a 2-case perf run, eliminating the targeted >0.05s regression against
  fresh master before the final full gates.
- 2026-07-07: full optimized all-suite correctness gate passed after P3-repair: 1439 passed, 1
  skipped.
- 2026-07-07: final full perf and fresh-master comparison passed after P3-repair. Full perf passed
  119 cases with compiler total 13.3101s. Fresh-master comparison improved to -0.2965s total delta
  with no significant per-case regressions; crypto-1 and PDZ recovered to 0.0483s and 0.0493s.
  Status set to `ready_for_review`.

## Open Questions

- QEMU dynamic instruction counting is unavailable in this environment because the generated plugin
  requires API version 1 while local `qemu-riscv64` supports minimum API version 2.

## Handoff Note

Current state:

- Branch: `task/cost-model-rejection-recovery` from base commit `0346fb7`.
- Code change: `src/pass/oir/OIRLocalSimplify.cpp` changes `short_circuit_bool` if-conversion
  pressure growth from `1` to `0`, matching the single-result boolean expression form, and treats
  exact `(x - y) + y` / `y + (x - y)` cancellation as a structural proof before SMT budgeting.
- Test/docs changes: `test/ir/oir_if_conversion.sy`, `test/ir/oir_huffman_gap.sy`,
  `test/ir/cost_model_smt.sy`, `docs/cost-model-calibration.md`, `docs/tasks/README.md`, and this
  task file.
- Generated report updates: `build/perf-ci/perf-report.md`, `build/perf-ci/perf-report.json`,
  `build/perf-ci/master-baseline-compare.md`, and `build/perf-ci/master-baseline-compare.json`.
- The existing `docs/tasks/README.md` task-index edit was preserved and its status was updated to
  `ready_for_review` after repair.
- Final correctness gates passed: `xmake`, focused FileCheck/stage/e2e commands, full
  `stage`+`e2e`, and full `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1`
  with 1439 passed and 1 skipped.
- Latest repair full perf passed 119 cases with GCC geomean `0.9702487919411927` and Clang++ geomean
  `1.0135988680507693`, total runtime `41.7533s`, and compiler total `13.3101s`. Final MIR totals
  are 33126 instructions, 7628 moves, 2437 branches, 3405 jumps, 1782 loads, 1170 stores, 168
  spills, and 222 stack slots.
- Cost-model decisions moved from 2113 accepted / 2085 bypassed / 8208 rejected in the pre-task
  report to 2527 accepted / 2067 bypassed / 3288 rejected in the final report.
- Fresh-master comparison passes the acceptance gate: current 13.3103s vs master 13.6068s, delta
  -0.2965s (+2.18%), with no significant per-case regressions. Crypto-1 is 0.0483s and PDZ is
  0.0493s.
- Remaining risk: QEMU dynamic instruction counts are unavailable due local plugin API mismatch;
  MIR metrics are recorded as fallback evidence.
