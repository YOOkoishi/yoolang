# Cost Model Calibration Notes

Date: 2026-07-05

## Scope

This document records the initial calibration contract for the yoolang cost model. The model is
used by the existing `-O1` competition pipeline and must not introduce `-O2`/`-O3` behavior or
testcase-identity decisions.

## Evidence Sources

- `--emit-cost-model[=json] -O1`: per-candidate decisions, proof status, estimated gain, setup
  cost, risk penalty, and final score.
- `--emit-mir-metrics -O1`: lowered, post-combine, pre-ra, post-ra, and final MIR totals.
- `scripts/compare_perf.py`: timing, MIR stage metrics, and cost-model decision summaries in
  `build/perf-ci/perf-report.md` and `build/perf-ci/perf-report.json`.

## Initial Calibration Rules

- Legal/proven status is mandatory. `Refuted`, `Timeout`, and `Unknown` proof states reject before
  profitability can commit a transform.
- Default `-O1` uses the same non-printing cost-model report and decision engine as
  `--emit-cost-model`; diagnostic filtering only filters recorded output and must not change
  transform decisions.
- Canonicalization, local cleanup, and fixed proof-bounded peepholes may use
  `BypassProfitability` after legality is proven. These decisions are still recorded separately
  from profitable accepts and must not be used for clone-producing or register-pressure-growing
  transforms.
- PreRA MIR transforms carry register-pressure risk unless the transform is known to be PostRA-only
  or not to increase live ranges.
- Partial evaluation must charge clone/setup cost and cleanup dependency. Specialization budgets
  reject excess clones through the decision trace.
- E-graph extraction uses target cost plus code-size/register-pressure/proof cost; it is not
  selected by minimum node count.
- QEMU dynamic instruction count is optional. If disabled, use MIR metrics and wall time as the
  available calibration evidence.

## Current Smoke Evidence

Focused smoke command:

```bash
PERF_TEST_DIRS=test/performance PERF_MAX_CASES=1 python3 scripts/compare_perf.py
```

This is not final performance evidence. It is used during C3-C8 to confirm that the report pipeline,
MIR metrics, and cost-model decision summaries still generate. Finalization must run the full
required performance command unless the environment blocks it.

## Current Full Evidence

Final command:

```bash
PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py
```

Result:

- Cases: 119
- Failed: 0
- Total runtime: 41.7533s
- Geomean speedup: GCC 0.9702487919411927, Clang++ 1.0135988680507693
- MIR stage metrics: OK
- QEMU dynamic instruction count: attempted but failed because the local QEMU plugin API is
  incompatible (`plugin requires API version 1, but this QEMU supports only a minimum version of
  2`); use MIR final metrics as fallback evidence in this run.
- Cost-model decisions: 7882 total, 2527 accepted, 2067 bypassed profitability, 3288 rejected
- Targeted transform/action totals: Inline 616 accepted / 54 rejected; constant-argument
  specialization 6 accepted / 72 rejected; if-conversion 162 accepted / 792 bypassed
  profitability; algebraic simplify 210 accepted / 1740 rejected; GlobalCSE 261 accepted; PreRA
  instruction scheduling 519 accepted / 708 rejected; PostRA instruction scheduling 297 bypassed
  profitability; LICM 747 accepted; strength reduction
  6 accepted; loop unswitch 714 rejected for code growth.
- Proof status totals: 6142 proven, 780 refuted, 960 timeout
- MIR final totals: 33126 instructions, 7628 moves, 2437 branches, 3405 jumps, 1782 loads,
  1170 stores, 168 spills, 222 stack slots

The 2026-07-07 rejection-recovery task changed OIR `short_circuit_bool` if-conversion metadata:
single-result boolean diamonds no longer claim register/live-range growth. These candidates remain
profitability-gated; under `Balanced` they need positive score, while `Conservative` still rejects
the same candidate shape for low confidence. The same task also changed exact add/sub cancellation
`(x - y) + y` and `y + (x - y)` to use a structural proof before SMT budgeting; the previous model
incorrectly rejected complex but exact cancellations as `ProofTimeout`. This recovered the
crypto-equivalent regressions while preserving SMT timeout/unknown handling for non-cancelling or
unsupported expressions.

The generated reports are `build/perf-ci/perf-report.md` and
`build/perf-ci/perf-report.json`.

## Adjustment Policy

When estimates disagree with measured behavior, adjust only generic weights or risk categories:

- Raise `register_pressure_growth` penalties when PreRA wins become PostRA spills.
- Raise `cleanup_dependency` penalties when SCCP/GVN/DCE do not realize expected PE or e-graph
  benefits.
- Raise proof/setup costs when SMT or e-graph budget overhead dominates small local wins.
- Do not add file, function, variable, input, or benchmark identity checks.

## Provider Scope

The production gates in this checkpoint are the existing OIR/MIR transforms wired through the
shared decision engine, with OIR inline and constant-argument specialization carrying the most
structured estimates.

The current SMT, PE, and e-graph hooks are bounded scaffolding:

- SMT records proof metadata, cache/timeout status, and proof cost for a deterministic i32 rewrite
  adapter; it is not a general bitvector solver integration.
- PE is constant-argument specialization plus clone/residual/cleanup accounting; it is not a
  general residual-program provider.
- E-graph is a small OIR expression-slice provider for registered integer rewrites; it is not full
  equality saturation.

Future providers should populate `CandidateProviderRequest`, `CandidateProviderBudget`,
`TransformCandidate`, and `AlternativeCost` so proof metadata, setup cost, cleanup dependency,
extraction/proof budgets, and alternative costs enter the same `decide()` path.
