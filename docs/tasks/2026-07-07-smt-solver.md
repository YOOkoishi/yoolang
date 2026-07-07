# Task: SMT Solver

Status: ready_for_review
Created: 2026-07-07
Last update: 2026-07-07
Owner: implementation subagent
Branch: task/smt-solver
Base commit: d63f08b

## Goal

Build a real in-process SMT solver for yoolang, with public headers under
`include/smt/` and implementation files under `src/smt/`. The deliverable must
replace the current fake/static SMT proof adapter with a deterministic,
tested solver-backed proof path, include focused tests, and add durable
documentation describing the solver architecture, supported theory, API,
limitations, and validation evidence.

The solver must be more than a demo: it should provide a reusable Boolean and
quantifier-free bit-vector proof engine suitable for compiler legality checks,
including model-producing SAT results, UNSAT proof status at the API level,
timeouts/resource limits, cacheable obligations, and integration with the
existing cost-model proof reporting.

## Non-goals

- Do not add an external solver dependency such as Z3, Boolector, CVC5, or an
  online service.
- Do not implement full SMT-LIB, quantifiers, arrays, floating-point theory, or
  unbounded mathematical integers.
- Do not change SysY/yoolang language semantics, parser syntax, runtime ABI, or
  RISC-V calling convention.
- Do not weaken, delete, xfail, or special-case existing tests to make the
  solver appear correct.
- Do not add testcase-specific behavior keyed to file names, function names,
  variable names, benchmark identity, input sizes, literal strings, or expected
  outputs.
- Do not hide proof failures by treating `Unknown` or `Timeout` as `Proven`.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [ ] YIR
- [x] OIR
- [ ] MIR
- [ ] ASM
- [ ] Runtime
- [x] Test infrastructure
- [x] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 2, initially `docs/cost-model-design.md` and the new
  `docs/smt-solver.md` once it exists
- Source/script anchors: max 8
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- `test/bsb-final/` case bodies except during perf attribution
- unrelated YIR, MIR, ASM pass implementations
- runtime sources
- archived or unrelated task files
- broad whole-repo scans after the initial anchor search

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/TEMPLATE.md` | full | task file shape | no | used only to create this file |
| `docs/tasks/README.md` | Active Tasks section | task index update rule | no | updated for this task |
| `docs/cost-model-design.md` | `1-180`, `rg "SMTObligation"` | proof-gate and SMT design context | yes | read relevant SMT sections before API integration |
| `xmake.lua` | full | build currently globs `src/pass/**.cpp` but not `src/smt/**.cpp` | yes | must add `src/smt/**.cpp` and likely test target |
| `include/pass/SMTProof.h` | full | existing SMT obligation/cache adapter API | yes | replace or wrap with solver-backed API while preserving callers |
| `src/pass/SMTProof.cpp` | full | current fake/static proof adapter | yes | should become adapter over `smt::Solver`, not a hardcoded result mapper |
| `src/pass/oir/OIRLocalSimplify.cpp` | `1-190`, `420-465`, `780-1040`, `1260-1280`, `rg cost_model_allows_smt_add_sub_cancel` | current SMT-backed algebraic simplification caller and cache lifetime | yes | expanded to inspect includes/helpers and the exact add/sub rewrite; `780-1040` was read but discarded as unrelated if-conversion matching context |
| `include/pass/CostModel.h` | `1-220` | `EquivalenceProof`, proof statuses, cost vectors, SMT query accounting | yes | API integration must preserve report schema or update tests/docs |
| `test/ir/cost_model_smt.sy` | full | current FileCheck coverage for Proven/Refuted/Timeout/Unknown | yes | extend to prove solver integration and avoid fake statuses |
| `scripts/run_tests.py` | `1-130`, `260-620` | focused test command behavior and infra/e2e gates | yes | add or reuse focused solver test command without parallel scripts |
| `include/oir/OIR.h` | `rg "enum class OpID\|class BinaryInst\|CmpPred"` | confirm OIR op names while translating supported i32 expressions | no | query only; no implementation dependency retained |
| `src/pass/oir/OIRCostModel.cpp` | `1-125` | debug why rejected solver results were not recorded | no | found report hook records rejects; implementation bug was short-circuit before calling hook |
| `src/pass/oir/OIRScalarOptUtils.cpp` | `1-70` | confirm `int_constant` semantics used by structural fast path | no | verified non-constants are not treated as constants |
| `docs/smt-solver.md` | full | durable solver architecture/API documentation | yes | new document created by P6 |
| `build/perf-ci/perf-report.md` | `1-120` | final performance evidence | no | generated artifact; summary recorded below |
| `build/perf-ci/perf-report.json` | summary fields | final performance evidence | no | generated artifact; summary recorded below |

## Branch

Decision: used

Reason:

```text
This is an invasive, multi-file implementation that adds new public APIs, new
source directories, test infrastructure, optimizer proof integration, and
documentation. The implementation subagent created `task/smt-solver` before
editing production or test files.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git checkout -b task/smt-solver
```

Observed at task creation:

```text
branch: master
base commit: d63f08b
git status --short: clean
```

Observed at implementation start:

```text
branch: task/smt-solver
base commit: d63f08b
git status --short: M docs/tasks/README.md; ?? docs/tasks/2026-07-07-smt-solver.md
```

## Invariants And Risks

Correctness invariants:

- `Proven` must mean the queried negated equivalence or implication is UNSAT
  under the solver's supported theory and active assumptions.
- `Refuted` must expose a model or enough model metadata to show the obligation
  is satisfiable.
- `Unknown` and `Timeout` must reject legality-dependent transforms unless an
  existing structural proof independently proves the transform.
- Bit-vector semantics must be fixed-width two's-complement modulo semantics,
  matching OIR integer behavior for supported widths.
- Signed and unsigned comparisons must be distinct and tested.
- Resource limits must be deterministic and based on solver work counters or
  monotonic time budgets; they must not depend on testcase identity.
- The solver API must make supported theory explicit and fail closed for
  unsupported operations.
- Existing cost-model JSON/text fields for proof kind, status, rule id, solver
  id, proof time, and obligation count must remain stable unless tests and docs
  are updated deliberately.
- The `-O1` compiler pipeline must remain contest-compliant and
  semantics-preserving for all valid inputs.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase
  identity, input sizes, or expected outputs.
- Do not hardcode known benchmark results or solver answers for specific
  source snippets.
- Do not weaken tests, skip existing checks without a documented legitimate
  toolchain reason, or bias performance scripts.
- Any performance change must come from general proof and optimization behavior,
  not benchmark recognition.

Risk areas:

- Bit-blasting bugs for signed comparison, overflow, shifts, extraction,
  extension, and mixed-width expressions.
- SAT solver completeness bugs, especially watched-literal propagation,
  conflict analysis/backtracking, assumptions, and model reconstruction.
- Treating an unsupported expression as proven instead of unknown.
- Excess compile-time from solver queries in hot optimization passes.
- Cost-model report drift that breaks downstream perf calibration.
- OIR simplification accepting more rewrites than the solver actually proves.
- Build-system omissions because `src/smt/**.cpp` is not currently compiled.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add the solver module skeleton, public API, build integration, and test target | `include/smt/`, `src/smt/`, `xmake.lua`, `test/smt/` | `xmake`; `xmake run smt_solver_tests` | completed | Added `Expr.h`, `Solver.h`, `Expr.cpp`, `Solver.cpp`, test target, and `src/smt/**.cpp` compiler build integration. |
| P2 | Implement reusable SAT core for Boolean formulas | `include/smt/`, `src/smt/`, `test/smt/` | `xmake run smt_solver_tests` | completed | Implemented deterministic CNF generation, watched literals, unit propagation, chronological backtracking, resource limits, diagnostics, and model extraction. |
| P3 | Implement QF_BV bit-vector theory by bit-blasting into the SAT core | `include/smt/`, `src/smt/`, `test/smt/` | `xmake run smt_solver_tests` | completed | Supports 1..64-bit constants/vars; bool ops; bv not/and/or/xor/add/sub/neg; eq/distinct; ult/ule/slt/sle; constant shifts; concat/extract; and add/sub/neg linear equality normalization. |
| P4 | Replace the current fake SMT proof adapter with typed solver-backed obligations | `include/pass/SMTProof.h`, `src/pass/SMTProof.cpp`, `src/pass/oir/OIRLocalSimplify.cpp`, `include/pass/CostModel.h` if needed | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter cost_model_smt --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1` | completed | Adapter maps solver UNSAT/SAT/TIMEOUT/UNKNOWN to Proven/Refuted/Timeout/Unknown, records models for refutations, keeps cache support, and OIR add/sub cancellation now commits only on Proven. |
| P5 | Add focused integration tests for solver-backed optimization proof behavior | `test/ir/cost_model_smt.sy`, optional `test/ir/smt_solver.sy`, `test/smt/` | `python3 scripts/run_tests.py --suite filecheck --filter cost_model_smt --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --filter cost_model_smt --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e --filter basic --jobs 1 --o1` | completed | `smt_solver_tests` covers Proven/Refuted/Timeout/Unknown, cache hits, signed/unsigned distinction, models, shifts/extract/concat. `cost_model_smt.sy` covers structural and solver-backed report statuses. |
| P6 | Write durable solver documentation and update task status | `docs/smt-solver.md`, this task file, `docs/tasks/README.md` | `git diff --check` | completed | Documented architecture, supported theory, API examples, limitations, failure modes, validation commands, and known non-goals. |
| P7 | Run final correctness and performance gates, then record evidence | this task file | commands in Verification Matrix | completed | Full correctness and performance gates passed; full perf report status PASS with 119 cases and MIR metrics OK. |
| R1 | Repair review findings for normalizer soundness and option-sensitive proof caching | `src/smt/Solver.cpp`, `src/pass/SMTProof.cpp`, `test/smt/smt_solver_tests.cpp`, this task file | `xmake`; `xmake run smt_solver_tests`; focused cost-model FileCheck filters; `git diff --check` | completed | Linear normalization now fails closed for nonlinear BV atoms instead of using rendered text identity; SMT proof cache keys include all `SolverOptions` fields. Added regressions for textual atom collisions and stale cache reuse across resource options. |
| R2 | Repair second-review finding for render-colliding typed assertion cache keys | `src/pass/SMTProof.cpp`, `test/smt/smt_solver_tests.cpp`, this task file | `xmake`; `xmake run smt_solver_tests`; focused cost-model FileCheck filters; `git diff --check` | completed | SMT proof cache now uses a structural assertion encoding with expression kind, sort kind/width, scalar payloads, child counts, child boundaries, and length-prefixed string payloads instead of `smt::to_string(assertion)`. Added a regression where two same-rendered obligations require different proof statuses. |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | Second repair rerun passed; rebuilt `src/pass/SMTProof.cpp` and `test/smt/smt_solver_tests.cpp`. |
| Solver focused tests | `xmake run smt_solver_tests` | yes | PASS | Second repair rerun passed; includes regressions for textual nonlinear BV atom collisions, option-sensitive cache keys, and structural typed assertion cache identity. |
| Focused SMT FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter cost_model_smt --jobs 1` | yes | PASS | Second repair rerun passed: 1 passed, 0 failed. |
| Cost-model regression FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1` | yes | PASS | Second repair rerun passed: 4 passed, 0 failed. |
| Focused OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter cost_model_smt --jobs 1 --o1` | yes | PASS | Command exited 0 with 0 discovered items under default stage discovery; additionally ran `build/linux/x86_64/release/compiler --emit-oir -O1 test/ir/cost_model_smt.sy` successfully. |
| Focused e2e smoke | `python3 scripts/run_tests.py --suite stage --suite e2e --filter basic --jobs 1 --o1` | yes | PASS | 5 passed, 0 failed. |
| Full FileCheck | `python3 scripts/run_tests.py --suite filecheck --jobs 1` | yes | PASS | 29 passed, 0 failed. |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | yes | PASS | 1439 passed, 0 failed, 1 skipped by default 10 MiB input cap (`shuffle1.in` is 16 MiB). Follow-up `python3 scripts/run_tests.py --suite e2e --filter shuffle1 --jobs 1 --o1 --max-input-bytes 0` passed 1/1. |
| Perf smoke | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=3 python3 scripts/compare_perf.py` | during iteration | PASS | 3 cases, 0 failed; smoke only. |
| Full performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | yes | PASS | 119 cases, 0 failed; `perf-report.md` status PASS; geomean speedup GCC 0.97x / Clang++ 1.01x; MIR stage metrics OK; QEMU instruction count disabled by config. |
| Diff hygiene | `git diff --check` | yes | PASS | Second repair rerun passed with no whitespace/errors. |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Keep `pass::smt::prove_obligation` as a static status mapper | Minimal change to existing cost model | rejected; user explicitly requested an effective SMT solver, not an MVP or fake adapter |
| Shell out to Z3 or another external solver | Faster to implement broad SMT functionality | rejected; adds dependency and contest-environment uncertainty |
| Implement only structural algebraic rewrites | Cheap and useful for current add/sub cases | rejected as the main deliverable; structural fast paths may remain but do not satisfy the SMT solver requirement |
| Build a focused in-process QF_BV and Boolean solver | Matches compiler proof needs, deterministic, testable, no external dependency | chosen |
| Add general SMT-LIB parser frontend | Useful for interoperability | deferred; task should expose a typed C++ API first and may document SMT-LIB as future work |

## Change Log

- 2026-07-07: created scoped task file for dedicated in-process SMT solver and
  README Active Tasks entry.
- 2026-07-07: implementation subagent created `task/smt-solver`, confirmed
  only task-system changes were present, and moved task status to
  `in_progress`.
- 2026-07-07: read allowed SMT/cost-model/build/test anchors, expanded the
  OIRLocalSimplify range to include the exact add/sub rewrite site, and started
  P1-P4 implementation.
- 2026-07-07: added reusable in-process SMT API and implementation under
  `include/smt/` and `src/smt/`, plus `smt_solver_tests`.
- 2026-07-07: replaced the static proof adapter with typed solver-backed
  obligations and updated OIR add/sub cancellation to require a Proven solver
  result for non-structural rewrites.
- 2026-07-07: fixed rejected solver-result reporting so non-Proven proofs still
  reach the cost-model report hook, while only Proven results can commit a
  transform.
- 2026-07-07: added `docs/smt-solver.md`, extended `cost_model_smt.sy`, ran all
  verification gates, and moved status to `ready_for_review`.
- 2026-07-07: repair subagent fixed review findings: removed textual identity
  from nonlinear BV linear normalization, added `SMTObligation::options` to the
  proof cache key, added focused regressions, and reran requested validation.
- 2026-07-07: second repair subagent replaced typed assertion cache-key
  rendering with structural expression serialization, added a same-rendered
  Proven-vs-Refuted cache regression, and reran requested validation.

## Open Questions

- None blocking. The task intentionally scopes "effective SMT solver" to a
  production-quality in-process Boolean plus QF_BV engine for compiler proof
  obligations, with unsupported theories failing closed as `Unknown`.

## Handoff Note

Current state:

- Implementation is complete on `task/smt-solver` and ready for review.
- Current branch at task creation was `master`, base commit `d63f08b`, with a
  clean `git status --short`; implementation start carried only the generated
  task file and task README changes.
- Added dedicated `include/smt/` and `src/smt/` folders with a reusable Boolean
  plus QF_BV in-process solver.
- `pass::smt::prove_obligation` now calls the solver for typed assertions and
  maps SAT/UNSAT/UNKNOWN/TIMEOUT to cost-model proof statuses.
- OIR local add/sub cancellation keeps the structural fast path and uses typed
  QF_BV counterexample queries for non-structural cases; only Proven rewrites
  are committed.
- Full correctness and performance validation passed as recorded in the
  Verification Matrix.
- Review repairs are complete: nonlinear BV atom collisions no longer prove
  through `to_string(expr)`, cache entries are separated by solver resource
  options, and typed assertion cache keys are structural instead of rendered.

Next action:

- Review the diff against this task file and the original user request.
- No implementation blocker remains.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-07-smt-solver.md`
- `docs/cost-model-design.md`
- `xmake.lua`
- `include/pass/SMTProof.h`
- `src/pass/SMTProof.cpp`
- `src/pass/oir/OIRLocalSimplify.cpp`
- `include/pass/CostModel.h`
- `test/ir/cost_model_smt.sy`
- `scripts/run_tests.py`
- `include/smt/Expr.h`
- `include/smt/Solver.h`
- `src/smt/Expr.cpp`
- `src/smt/Solver.cpp`
- `test/smt/smt_solver_tests.cpp`
- `docs/smt-solver.md`
