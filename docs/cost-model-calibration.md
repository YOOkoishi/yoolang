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
- Total runtime: 40.7021s
- Geomean speedup: GCC 0.9779787144448093, Clang++ 1.0094285572486272
- MIR stage metrics: OK
- QEMU dynamic instruction count: disabled
- Cost-model decisions: 11625 total, 3191 accepted, 8434 rejected
- Proof status totals: 8343 proven, 108 refuted, 3174 timeout

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
