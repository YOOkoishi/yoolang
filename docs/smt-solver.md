# yoolang SMT Solver

This document describes the in-process SMT solver used for compiler proof
obligations. The solver is intentionally scoped to deterministic Boolean logic
and quantifier-free bit-vectors (QF_BV), with no external dependency on Z3,
Boolector, CVC5, or an online service.

## Goals

- Prove or refute local compiler legality obligations with fixed-width
  two's-complement bit-vector semantics.
- Produce models for satisfiable counterexample queries.
- Fail closed: unsupported expressions return `Unknown`, and exhausted budgets
  return `Timeout`; neither status may legalize a transform.
- Provide a reusable C++ API under `include/smt/` and implementation under
  `src/smt/`.
- Preserve cost-model proof reporting for proof kind, status, solver id, rule
  id, time, and obligation count.

## Public API

The public API is split into expression construction and solving:

- `include/smt/Expr.h`
  - `smt::Sort` represents `Bool` or `(_ BitVec width)`.
  - `smt::ExprBuilder` constructs typed Boolean and bit-vector expressions.
  - Expressions validate sorts at construction time and throw
    `std::invalid_argument` for invalid operations.
- `include/smt/Solver.h`
  - `smt::Solver::check` checks one or more Boolean assertions.
  - `smt::SolverOptions` carries timeout and resource limits.
  - `smt::SolverResult` returns `Sat`, `Unsat`, `Unknown`, or `Timeout`,
    diagnostics, and a model for `Sat`.

Compiler proof gates normally assert the counterexample condition. For an
equivalence proof, `Unsat` means the transform is proven; `Sat` means the solver
found a counterexample and the transform is refuted.

Example:

```cpp
smt::ExprBuilder b;
smt::Solver solver;
auto x = b.bv_var(32, "x");
auto y = b.bv_var(32, "y");
auto counterexample = b.distinct(b.bv_add(b.bv_sub(x, y), y), x);
auto result = solver.check(counterexample);
// result.status == smt::CheckStatus::Unsat, so the equivalence is proven.
```

## Supported Theory

The solver supports:

- Boolean constants and variables.
- Boolean `not`, `and`, `or`, `xor`, implication, equality, and disequality.
- Bit-vector constants and variables for widths 1 through 64.
- Bit-vector `not`, `and`, `or`, `xor`.
- Bit-vector add, subtract, and negate with modulo semantics.
- Equality and disequality over Booleans and bit-vectors.
- Unsigned comparisons `ult` and `ule`.
- Signed comparisons `slt` and `sle` using two's-complement sign bits.
- Constant shifts: logical left, logical right, and arithmetic right.
- `concat` and `extract`, as long as the resulting width is at most 64 bits.

Unsupported theories include quantifiers, arrays, memory, floating point,
unbounded integers, variable shifts, multiplication/division/remainder as
native bit-vector operators, and SMT-LIB parsing. Callers that cannot translate
an IR expression into the supported API must report `Unknown`.

## Architecture

The implementation has three layers:

1. Typed expression DAG.
   `ExprBuilder` creates immutable expression nodes with explicit result sorts.
   Invalid sort combinations are rejected at construction time.

2. QF_BV encoder.
   `Solver` bit-blasts supported bit-vector expressions into SAT literals.
   Addition and subtraction use ripple-carry full adders. Signed comparisons
   distinguish sign-bit cases from unsigned ordering. A small linear
   normalizer discharges generic add/sub/neg equalities before SAT search; this
   is a theory simplification, not a testcase-specific shortcut.

3. SAT core.
   The SAT engine uses deterministic variable allocation, CNF clauses,
   watched-literal propagation, unit propagation, chronological backtracking,
   assumptions at the API boundary, and model extraction. Resource counters and
   optional monotonic time budgets map to `Timeout`.

## Proof Adapter

`include/pass/SMTProof.h` keeps the existing cost-model-facing adapter:

- `pass::smt::SMTObligation` now carries typed `smt::Expr` assertions in
  addition to the existing human-readable fields.
- `pass::smt::prove_obligation` checks the typed assertions with
  `smt::Solver`.
- `Unsat` maps to `ProofStatus::Proven`.
- `Sat` maps to `ProofStatus::Refuted` and includes model metadata in the
  summary.
- `Timeout` maps to `ProofStatus::Timeout`.
- `Unknown` maps to `ProofStatus::Unknown`.
- `SMTProofCache` caches complete proof objects by stage, formula, assumptions,
  guarantees, timeout, estimated cost, and typed assertion string.

The OIR local simplifier uses this adapter for the add/sub cancellation
obligation. Structural identity remains a fast path, but non-identical i32
expressions are translated into a typed QF_BV counterexample query. The rewrite
is committed only when the resulting proof status is `Proven`.

## Resource Limits

`SolverOptions` exposes:

- `timeout_us`
- `max_decisions`
- `max_conflicts`
- `max_propagations`
- `max_sat_variables`
- `max_clauses`

The pass-level adapter also applies admission control with
`SMTObligation::estimated_cost_us` and `timeout_us`. This prevents known-large
obligations from entering SAT search after the cost-model policy has already
declared the proof budget exhausted.

## Validation

Focused coverage lives in `test/smt/smt_solver_tests.cpp` and exercises:

- Boolean SAT and UNSAT.
- QF_BV add/sub proof by UNSAT counterexample.
- Refuted QF_BV obligation with model extraction.
- Signed versus unsigned comparison behavior.
- Modular arithmetic model extraction.
- `concat`, `extract`, and constant shift behavior.
- Deterministic resource-limit timeout.
- Pass-level `SMTProof` status mapping and cache behavior.

Cost-model integration coverage lives in `test/ir/cost_model_smt.sy` and checks
that report output includes structural proof, solver-backed `Proven`,
`Refuted`, `Timeout`, `Unknown`, solver id, and obligation count.

Validation commands used for this implementation:

```bash
xmake
xmake run smt_solver_tests
python3 scripts/run_tests.py --suite filecheck --filter cost_model_smt --jobs 1
python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1
```

Broader stage, e2e, full FileCheck, full optimized, and performance gates are
tracked in `docs/tasks/2026-07-07-smt-solver.md`.

## Limitations

- The SAT core is deterministic and complete for the generated CNF within the
  configured resource budgets, but it does not yet implement clause learning.
- The public bit-vector value type stores model values up to 64 bits.
- Multiplication, division, remainder, memory, and floating-point expressions
  must be rejected by translators unless another independent proof handles
  them.
- `Timeout` and `Unknown` are never proof successes.
