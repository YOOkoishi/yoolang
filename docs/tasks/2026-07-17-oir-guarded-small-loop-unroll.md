# Task: OIR Guarded Small-Loop Unroll And Address Recurrence Optimization

Status: scoped
Created: 2026-07-17
Last update: 2026-07-17
Owner: Codex implementation subagent
Branch: `task/oir-guarded-small-loop-unroll` (planned; not created during task generation)
Base commit: de93e24

## Goal

Implement a general OIR optimization sequence that (1) makes exact innermost loops with
2--5 iterations eligible for full unrolling even when the pre-tested multi-block loop has a
PHI-bearing merge latch, (2) versions affine guarded loop bodies into a proof-checked fast path
and an unchanged fallback, (3) unrolls only the proven fast path, and (4) groups the exposed
address recurrences so repeated GEP arithmetic is replaced by a shared base recurrence plus
constant offsets.

The transform must be selected entirely from ordinary IR facts and must benefit every program
that satisfies the same structural, range, side-effect, alias, proof, and profitability
conditions. The three `conv2d-*` files are discovery evidence and focused performance acceptance
cases, not matcher inputs.

## Non-goals

- Do not recognize convolution, stencils, `conv2d`, `idx`, `KSIZE`, `K`, or any other source,
  filename, function, variable, parameter, testcase, input, or benchmark identity.
- Do not add a `KSIZE == 5`, exact source-layout, argument-count, known-input-size, or expected
  output special case. Constants 2--5 are a general bounded unroll policy applied uniformly.
- Do not remove the original guarded path. A versioned transform must retain an exact fallback
  for every input for which the fast-path precondition is false.
- Do not reorder memory operations or outer iteration points. In particular, do not assume array
  parameters are `noalias` and do not process all interior points before all border points.
- Do not expand the SMT solver to arrays, memory, multiplication, division, remainder, or variable
  shifts in this task. Unsupported proof expressions must reject the candidate.
- Do not implement YIR/polyhedral recognition, vectorization, MIR-specific stencil combines, or
  ASM peepholes. MIR/ASM changes may only be downstream consequences of the OIR rewrite.
- Do not tune the transformation from GCC/Clang names or timings. External compilers are context;
  the same-Yoolang baseline is the attribution gate.
- Do not modify the pre-existing unrelated `docs/README.md` or `docs/egraph-design.md` changes.

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
- Related docs: max 2: `docs/performance-optimization-opportunity-audit.md` and
  `docs/smt-solver.md`
- Source/script anchors: max 8
- Large-file rule: read only the named ranges/queries below; promote a file to a full read only
  after recording the reason in the Context Ledger

Do not read unless explicitly needed:

- parser, AST, runtime, YIR/polyhedral implementation, MIR register allocation, or ASM emitter
  internals
- Huffman sources/tasks or other benchmark-specific optimization records
- performance inputs other than the three focused files before broad validation
- `docs/README.md` and `docs/egraph-design.md`, which already contain unrelated user changes

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/README.md` | Active Tasks | register this task | yes | update status with the task |
| `docs/tasks/TEMPLATE.md` | full | task schema | no | task is now instantiated |
| `docs/performance-optimization-opportunity-audit.md` | `94-118`, `224-226` | original rolled-loop/guard evidence | yes | discovery evidence only |
| `docs/smt-solver.md` | `1-110`, `158-166` | supported QF_BV theory and fail-closed contract | yes | no memory/array proof |
| `src/pass/oir/OIRLoopTransforms.cpp` | `550-700`, `1410-1590`, `3169-3265` | rotation rejection, exact trip matching, multi-block unroll, loop order | yes | primary P1/P2 anchor |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | `1-55`, `145-175` | current cleanup/unswitch/rotate/unroll/LSR order | yes | pipeline placement anchor |
| `include/oir/OIRScalarOpt.h` | loop transform declarations and `Stats` | declare/version the generic pass | yes | keep API changes narrow |
| `src/pass/oir/OIRLoopStrengthReductionPass.cpp` | `20-60`, `268-480`, `571-820` | existing GEP candidates and stack-call grouping | yes | generalize grouping, do not duplicate it |
| `include/pass/SMTProof.h`, `src/pass/SMTProof.cpp` | public obligation; `168-238` | build and consume typed fail-closed proof obligations | yes | empty assertions are not an SMT proof |
| `test/ir/oir_loop_transforms.sy` | full | existing rotate/unroll/cost-model FileCheck style | yes | extend or split into a focused test |
| `test/performance/conv2d-1.sy` | `51-80` | representative exact nested loops and affine guards | yes | validation only; `-2/-3` have identical source |
| `build/perf-ci/perf-report.md`, `.json` | metadata and `conv2d-*` rows | durable historical timing/MIR/cost context | yes | archive before any new perf run; never use as the same-machine or CI-parity baseline |

If cost-model enum/report plumbing or a new test file becomes necessary in P2, add those exact
files to this ledger before reading them; do not compensate by scanning the pass tree.

## Branch

Decision: use a dedicated branch during implementation; task generation remains on the current
`master` checkout.

Reason:

```text
The implementation changes CFG/PHI rewriting, proof gates, loop unrolling, and address induction,
so it needs an isolated task branch. At task creation HEAD is master@de93e24 and the worktree has
two unrelated user changes that must remain byte-for-byte untouched:

 M docs/README.md
?? docs/egraph-design.md
```

Commands for the implementation subagent:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
git checkout -b task/oir-guarded-small-loop-unroll
git status --short
```

Before editing, verify that the checkout still reports base `de93e24` and that the two unrelated
paths above are unchanged. Do not stash, reset, commit, or edit them.

## Worktree Preservation Gates

The implementation subagent must protect the two pre-existing user paths independently of the
task-generated records. At task-generation handoff the complete expected `git status --short` set
is exactly:

```text
 M docs/README.md
 M docs/tasks/README.md
?? docs/egraph-design.md
?? docs/tasks/2026-07-17-context-inline-residual-pe.md
?? docs/tasks/2026-07-17-oir-guarded-small-loop-unroll.md
```

The `docs/README.md` and `docs/egraph-design.md` entries are the user's pre-existing bytes. The
remaining three entries are task-generation output. A different status set is a stop condition:
record it in this task and ask the coordinator to reconcile it; do not stash, reset, clean, or
overwrite anything.

Before branch creation, source edits, builds, or tests, run this initial snapshot gate from the
current repository root. This command block is for the future implementation subagent; task-document
repair must not execute it or create its evidence files.

```bash
set -eu
TASK_ROOT="$(git rev-parse --show-toplevel)"
test "$(pwd -P)" = "$(cd "$TASK_ROOT" && pwd -P)"
TASK_GUARD_DIR="$TASK_ROOT/build/task-evidence/oir-guarded-small-loop-unroll/worktree-guard"
case "$TASK_GUARD_DIR" in
  "$TASK_ROOT"/build/task-evidence/oir-guarded-small-loop-unroll/worktree-guard) ;;
  *) echo "unsafe guard directory: $TASK_GUARD_DIR" >&2; exit 1 ;;
esac
mkdir -p "$TASK_GUARD_DIR"
git status --short > "$TASK_GUARD_DIR/initial-status.txt"
python3 - "$TASK_GUARD_DIR/initial-status.txt" <<'PY'
from pathlib import Path
import sys

expected = {
    " M docs/README.md",
    " M docs/tasks/README.md",
    "?? docs/egraph-design.md",
    "?? docs/tasks/2026-07-17-context-inline-residual-pe.md",
    "?? docs/tasks/2026-07-17-oir-guarded-small-loop-unroll.md",
}
actual = set(Path(sys.argv[1]).read_text().splitlines())
if actual != expected:
    raise SystemExit(f"initial status mismatch: expected={sorted(expected)!r}, actual={sorted(actual)!r}")
PY
sha256sum docs/README.md docs/egraph-design.md > "$TASK_GUARD_DIR/initial-user-files.sha256"
git diff --binary HEAD -- docs/README.md > "$TASK_GUARD_DIR/initial-docs-README.diff"
git status --short -- docs/README.md docs/egraph-design.md \
  > "$TASK_GUARD_DIR/initial-protected-status.txt"
```

Run the following final gate after implementation, task updates, correctness checks, and performance
closure, immediately before handoff to review. It records the complete final status, then proves
that both protected files have the same hashes, that the tracked `docs/README.md` diff is identical,
and that both protected status entries are identical. New task-scoped source/test entries are allowed
in the complete final status; changes to either protected entry are not.

```bash
set -eu
TASK_ROOT="$(git rev-parse --show-toplevel)"
test "$(pwd -P)" = "$(cd "$TASK_ROOT" && pwd -P)"
TASK_GUARD_DIR="$TASK_ROOT/build/task-evidence/oir-guarded-small-loop-unroll/worktree-guard"
test -f "$TASK_GUARD_DIR/initial-status.txt"
test -f "$TASK_GUARD_DIR/initial-user-files.sha256"
test -f "$TASK_GUARD_DIR/initial-docs-README.diff"
test -f "$TASK_GUARD_DIR/initial-protected-status.txt"
git status --short > "$TASK_GUARD_DIR/final-status.txt"
sha256sum -c "$TASK_GUARD_DIR/initial-user-files.sha256"
git diff --binary HEAD -- docs/README.md > "$TASK_GUARD_DIR/final-docs-README.diff"
cmp "$TASK_GUARD_DIR/initial-docs-README.diff" "$TASK_GUARD_DIR/final-docs-README.diff"
git status --short -- docs/README.md docs/egraph-design.md \
  > "$TASK_GUARD_DIR/final-protected-status.txt"
cmp "$TASK_GUARD_DIR/initial-protected-status.txt" "$TASK_GUARD_DIR/final-protected-status.txt"
python3 - "$TASK_GUARD_DIR/initial-status.txt" "$TASK_GUARD_DIR/final-status.txt" <<'PY'
from pathlib import Path
import sys

protected = {"docs/README.md", "docs/egraph-design.md"}

def protected_lines(path: str) -> set[str]:
    lines = Path(path).read_text().splitlines()
    return {line for line in lines if len(line) >= 4 and line[3:] in protected}

initial = protected_lines(sys.argv[1])
final = protected_lines(sys.argv[2])
if initial != final:
    raise SystemExit(f"protected status changed: initial={sorted(initial)!r}, final={sorted(final)!r}")
PY
```

Any failure in either gate blocks the task. Preserve the evidence and report the mismatch; never
attempt an automatic cleanup or restoration.

## Diagnostic Evidence And Derived Fix Sequence

The current report was generated at `2026-07-16 01:08:44 UTC` from
`build/linux/x86_64/release/compiler`, selected `test/performance`, and passed correctness:

| Case | GCC -O3 | Clang -O3 | Yoolang -O1 | Yoolang gap vs GCC | Gap vs Clang | Final MIR | Spills | `LoopUnroll` decisions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `conv2d-1.sy` | 0.4212s | 0.3256s | 0.4583s | +8.81% | +40.76% | 446 | 0 | 0 |
| `conv2d-2.sy` | 0.1215s | 0.0968s | 0.1331s | +9.55% | +37.50% | 446 | 0 | 0 |
| `conv2d-3.sy` | 0.0459s | 0.0383s | 0.0503s | +9.59% | +31.33% | 446 | 0 | 0 |

The three sources are identical and differ only in input. The hot `kr` and `kc` trip counts are
ordinary compile-time constants: start 0, signed condition `< 5`, unit step, hence exactly five
iterations on entry. Fresh Clang 22 optimization remarks report both loops as completely unrolled
with five iterations; this is normal loop optimization, not convolution recognition.

The missed Yoolang transform is earlier than code generation:

1. `rotate_loop` rejects any latch whose first instruction is a PHI via
   `block_starts_with_phi(*latch)`.
2. The guarded source lowers to a pre-tested multi-block loop with a PHI-bearing merge latch and
   an unconditional latch-to-header edge.
3. `match_multi_block_unroll_loop` accepts only a conditional latch whose other successor is the
   loop exit.
4. Consequently the exact trip count is never presented to profitability and the current report
   contains zero `LoopUnroll` candidates. Raising the unroll limit or cost threshold cannot fix
   this missing-candidate problem.

The required repair order is therefore:

```text
generic loop canonicalization for PHI-bearing latches
  -> innermost-first exact 2..5 full-unroll coverage
  -> affine guard clustering + fast/fallback versioning
  -> finite-PE + structural + QF_BV proof, fail closed
  -> full unroll only on the proven fast path
  -> SCCP/GVN/CFG/DCE cleanup
  -> grouped GEP/address recurrence reduction
  -> optional lexicographic boundary peeling only after bound/alias proof
```

## Invariants And Risks

Correctness invariants:

- Preserve the exact observable behavior, including zero-trip entry behavior, PHI live-outs,
  branch/path-specific updates, call/store order, and the original lexicographic iteration order.
- Treat all i32 arithmetic and comparisons with the language/OIR's actual fixed-width signed and
  wrapping semantics. Do not replace bit-vector arithmetic with unbounded mathematical integers
  or assume signed overflow cannot occur.
- The initial unroll coverage applies only when SCEV/structural analysis proves a positive exact
  trip count in 2--5. Existing independently legal coverage outside that range must not regress.
- A rotation/canonicalization with a PHI-bearing latch is legal only when all incoming edges and
  values are explicit, the cloned condition is available after latch PHIs, exit PHIs can be
  repaired, and no side exit or external use is lost.
- Guard versioning requires a natural single-entry region, loop-invariant versioning operands, an
  affine conjunction of guards, and an inactive/skipped arm with no call, store, volatile-like
  operation, trap, early exit, or other observable side effect.
- The fast path may delete per-iteration guards only after proving that its entry condition implies
  every original guard for every finitely instantiated iteration. The slow path must remain the
  original loop, not a reconstructed approximation.
- Versioning/unrolling must not reorder loads or stores. Alias uncertainty is acceptable only when
  the exact original memory order is retained; transformations that change outer-point order need
  a dependence/noalias proof and otherwise must reject.
- Grouped LSR may replace address computations only when each replacement denotes the identical
  address at the same program point. It must not merge bases that may differ, change access order,
  or lengthen live ranges past calls without the existing pressure/safety checks.
- If optional boundary peeling is enabled, execute top/interior/bottom rows in original row order
  and prefix/interior/suffix columns within each row in original column order. A layout such as
  "all interior, then all border" is forbidden without a complete dependence proof.

Proof contract:

- Candidate discovery reads only opcode, type, SSA use-def, CFG, dominance, LoopInfo/SCEV, ranges,
  side effects, alias/dependence facts, target data layout, and shared cost-model data.
- Finite PE substitutes the statically proven 2--5 induction values into the original affine guard
  expressions; it is not allowed to substitute runtime benchmark inputs.
- The structural proof certifies CFG/PHI mapping, exact iteration enumeration, fallback identity,
  memory-operation order, and absence of effects on the skipped arm.
- The QF_BV obligation asserts a counterexample such as
  `EntryDomain && FastGuard && OR(!OriginalGuard[i])`. Commit only when this formula is `UNSAT` and
  the returned proof status is `Proven`.
- `SAT/Refuted`, `Unknown`, `Timeout`, unsupported operators, missing typed assertions, proof-budget
  exhaustion, or an incomplete structural certificate reject the candidate. A caller-supplied
  fallback status for an empty SMT obligation is not an SMT proof.
- SMT proves integer guard implication only. It must not be presented as proving memory aliasing,
  side effects, dominance, or CFG correctness.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes,
  parameter tuples, benchmark families, or expected outputs.
- Do not use names even as a secondary profitability feature. Names may appear only in diagnostics
  emitted after the IR-only decision.
- Do not use SMT or PE to legitimize an identity-based selector. Proof validates semantic
  equivalence; it does not make an illegal trigger compliant.
- Add alpha-renaming and structural metamorphic tests demonstrating that renamed/general variants
  receive the same decision and same-named near misses do not.

Profitability and compile-time constraints:

- Legality and proof complete before the shared cost model is queried.
- Static cost accounts for original and residual instructions/blocks, cloned fast/slow setup,
  branch count, code growth, generated pointer PHIs, live-range/pressure risk, and cumulative
  per-function growth. Do not hide the retained fallback from the size estimate.
- Dynamic benefit is a structural estimate based on exact trip count, loop nesting depth, removed
  backedges, removed dynamic guard evaluations, removed address operations, and expected setup
  executions. Do not use benchmark-specific frequencies or known input distributions.
- Preserve the existing generic 2--16 unroll behavior. The new PHI-latch/pre-tested coverage starts
  at 2--5 and remains behind the shared policy's code-growth and pressure caps.
- Versioning must be rejected when the fast path does not eliminate enough repeated work to pay for
  its entry check and retained fallback, or when cumulative code growth/compile-time proof budgets
  are exceeded.

Risk areas:

- CFG edge and PHI repair during rotation, cloning, and fallback construction.
- Header-tested loops, zero iterations, live-out induction values, nested loops, and side exits.
- Signed compare normalization and wraparound while materializing min/max affine guard bounds.
- Loads/calls hidden inside guard expressions and operations that may trap.
- Alias between input, output, and coefficient pointers.
- Code growth, I-cache cost, virtual-register pressure, and new post-RA spills after full unrolling.
- Repeated fixed-point execution re-versioning or re-unrolling already transformed loops; use
  structural state/metadata, not block/function name substrings, to prevent duplication.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P0 | Capture the initial dirty-file guard and archive the 2026-07-16 report as legacy context | task evidence under `build/task-evidence/`; no production edit | `Initial worktree protection` and `Legacy report context archive` | pending | Initial protection precedes branch creation; archive precedes any `compare_perf.py` run; the legacy report is never a delta baseline |
| P1 | Permit legal PHI-bearing-latch canonicalization and make exact 2--5 header-tested multi-block loops reach existing full unroll, innermost first | `src/pass/oir/OIRLoopTransforms.cpp`, focused OIR FileCheck | `Build`, `Focused loop/versioning FileCheck`, and `Generic OIR stage` | pending | Prefer generalizing rotation and reusing existing unroll; if direct pre-tested unroll is safer, record the branch and identical invariants here before editing |
| P2 | Add generic affine guard collection/versioning with an unchanged fallback and explicit structural certificate | new/focused OIR versioning implementation, `include/oir/OIRScalarOpt.h`, focused FileCheck | `Focused loop/versioning FileCheck`, `Generic OIR stage`, and `Focused semantic e2e` | pending | No benchmark vocabulary; inactive arm must be effect-free |
| P3 | Add finite 2--5 PE guard instantiation, typed QF_BV counterexample proof, proof diagnostics, and shared static/dynamic cost gating | versioning implementation plus narrowly required cost-model plumbing and proof test | `SMT/cost integration`, `Focused loop/versioning FileCheck`, and `Generic OIR stage` | pending | Missing assertions and every non-Proven result reject; do not modify the solver theory unless separately scoped |
| P4 | Place versioning before fast-path-only unroll and run SCCP/GVN/branch/CFG/DCE cleanup after it | `src/pass/oir/OIROptimizationPipelinePass.cpp`, versioning implementation, focused test | `Generic OIR stage`, `Generic downstream MIR/ASM`, and `Focused semantic e2e` | pending | Slow fallback stays guarded; prevent fixed-point duplication structurally |
| P5 | Generalize existing GEP grouping so unrolled same-base/same-IV/same-stride addresses share one recurrence plus constant offsets | `src/pass/oir/OIRLoopStrengthReductionPass.cpp`, `test/ir/oir_lsr_dynamic.sy` or a focused new test | `Focused LSR FileCheck`, `Focused LSR OIR/MIR stages`, and `Focused semantic e2e` | pending | Preserve address and memory-op order; include pressure/call negatives |
| P6 | Add semantic, negative, metamorphic, alias, wraparound, and zero-trip tests | focused `test/ir` case plus a deterministic functional/e2e case | `Focused loop/versioning FileCheck`, `Generic OIR stage`, `Generic downstream MIR/ASM`, and `Focused semantic e2e` | pending | Cover 2, 3, 4, 5 trips; renamed variants; unsupported proof; near misses |
| P7 | Decide whether lexicographic boundary peeling has the required stable-bound and alias/dependence proof; implement only if the focused same-machine delta wins | versioning/loop transform implementation plus focused tests when implemented; otherwise this task record only | `P7 disposition`; if implemented, also `P7 boundary FileCheck`, `P7 alias/order e2e`, and `Focused delta adjudication` | pending | Record exactly one outcome: implemented with all three conditional gates passing, or `deferred` with the missing proof/no-win evidence; never reorder outer points |
| P8 | Run broad correctness plus all three same-machine baseline/current scopes and deltas, then run the final dirty-file guard and update this task | task file and generated `build/task-evidence`/`build/perf-ci` artifacts | `Full FileCheck/poly`, `Full optimized correctness`, `Baseline path/isolation preflight`, `Baseline/current release builds`, `Focused baseline/current raw pair`, `Focused delta`, `Focused delta adjudication`, `Affected baseline/current raw pair`, `Affected delta`, `Affected delta adjudication`, `CI-parity baseline/current raw pair`, `CI-parity delta`, `CI-parity delta adjudication`, and `Final worktree protection` | pending | All three baseline/current pairs run sequentially with no concurrent build/test activity |

Each implementation patch must remain one behavior point, touch at most three production files by
default, and update this task's status, ledger, result, and next action. Split P2/P3 further before
editing if new cost-model plumbing would exceed that limit.

## Implementation Sketch

### 1. Canonicalization and exact unroll

Select innermost loops by containment/depth, not merely by block count. For a pre-tested loop with a
single preheader and latch, exact trip count, no non-condition side exit, cloneable acyclic body,
and a PHI-bearing merge latch:

- retain or reconstruct the preheader entry condition so zero-trip behavior is unchanged;
- map header induction PHIs to their preheader value for the first check and latch/update value for
  the backedge check;
- keep latch PHIs at the beginning of the block and insert any cloned condition after all PHIs and
  before the terminator;
- repair exit PHIs and direct live-out uses before deleting/replacing the old header;
- expose the canonical conditional latch to the existing multi-block full-unroll implementation;
- apply the new path uniformly for exact counts 2, 3, 4, and 5, then run cleanup before considering
  an enclosing loop.

Do not key "already transformed" checks on `.unr` or another textual name for the new path. Add
explicit IR metadata/transform state or recognize the resulting CFG structurally.

### 2. Guard clustering and versioning

Start narrowly with a nested or single natural loop whose exact inner induction domain is known and
whose side-effecting body is controlled by a conjunction of signed affine comparisons. Normalize
only expressions supported by structural affine analysis and QF_BV add/sub/compare encoding.

For each exact induction value, finite PE produces the corresponding original guard expression.
Construct a sufficient, loop-invariant `FastGuard` from extrema/clustered bounds. For a representative
affine row guard with `iv in [0, T-1]`, the mathematical shape may be:

```text
base >= lower && base + (T - 1) < upper
```

but the emitted i32 expression is accepted only if the bit-vector counterexample formula proves it
implies every original per-iteration guard under actual wrapping semantics. A stronger guard that
falls back more often is legal; a weaker unproven guard is not.

Clone as:

```text
if (FastGuard) {
  exact original iterations in original order, with only the proven-redundant guards removed
} else {
  exact original guarded loop
}
```

Run full unroll only in the fast clone. Join every live-out through repaired PHIs. If the fallback
cannot remain byte-for-byte/structure-equivalent before ordinary cleanup, reject.

### 3. Grouped address induction

After unroll and cleanup, group GEP candidates only when they share the same proven base pointer,
induction recurrence, index position, element type/data-layout stride, and pointer step. Choose one
base pointer PHI/update and materialize other addresses as constant offsets at their original use
sites. Reuse/generalize the existing `GEPCandidateGroup` machinery rather than adding a
benchmark-shaped address matcher.

### 4. Optional boundary peeling

Only after P1--P6 are measured, consider deriving prefix/interior/suffix domains from proven stable
bounds. Preserve lexicographic execution by keeping row order and, within each row, column order.
If stores/calls can modify a bound, or pointer aliasing/dependence can make reordered observation
possible, retain per-point versioning and record P7 as safely deferred.

## Test Requirements

Focused IR and e2e coverage must include:

- positive exact-trip loops for 2, 3, 4, and 5 iterations, including nested loops and a
  PHI-bearing merge latch;
- an alpha-renamed/structurally equivalent copy that receives the same decision;
- an equivalent guard with swapped compare operands/conjunction order where canonicalization
  permits it;
- zero-trip entry, PHI live-out, empty body, and branch-specific recurrence cases;
- signed boundary values near `INT_MIN`/`INT_MAX` that exercise i32 wraparound and force fallback
  when the fast condition is not proven;
- two pointer parameters that alias at runtime, with observable output confirming original memory
  order;
- a skipped arm containing a call/store, a side exit, an unknown trip count, a non-affine or
  unsupported SMT expression, and an incomplete/timeout proof; all must remain unversioned;
- cost-model diagnostics showing a `Proven` structural+SMT candidate, a `Refuted` candidate, and
  `Unknown/Timeout` rejection without transformation;
- grouped LSR positives plus different-base, different-stride, live-across-call/pressure, and
  non-constant-offset negatives;
- a deterministic differential functional case comparing optimized results against a direct
  reference implementation across interior, border, small-domain, alias, negative, and extreme
  inputs.

No test may assert the presence of `conv2d`, `KSIZE`, or another benchmark name in a decision.

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Initial worktree protection | Run the exact initial snapshot block in `Worktree Preservation Gates` | before branch/build/edit/test | NOT_RUN | Must match the five-entry task-generation status exactly and save both user-file hashes plus the tracked diff |
| Legacy report context archive | Run `Legacy report archive` below | before any new perf run | NOT_RUN | Historical context only; forbidden as a same-machine or CI-parity baseline |
| Baseline path/isolation preflight | Run the validation and `git worktree add` portion of `Same-machine de93e24 performance closure` below | at P8, before release builds | NOT_RUN | Executor must replace and validate the absolute empty-path placeholder before the state-changing command |
| Build | `xmake` | yes | NOT_RUN | Use release configuration before final perf |
| Focused loop/versioning FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_guarded_small_loop_unroll --jobs 1` | yes | NOT_RUN | Include proof and rejection checks |
| Focused LSR FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_lsr --jobs 1` | yes | NOT_RUN | Existing plus grouped recurrence coverage |
| SMT/cost integration | `python3 scripts/run_tests.py --suite filecheck --filter cost_model_smt --jobs 1` | yes | NOT_RUN | Do not count `xmake run smt_solver_tests` as executed unit coverage |
| Generic OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_guarded_small_loop_unroll --jobs 1 --o1` | yes | NOT_RUN | Module verifier must pass |
| Generic downstream MIR/ASM | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter oir_guarded_small_loop_unroll --jobs 1 --o1` | yes | NOT_RUN | Verify downstream lowering of the generic transform tests |
| Focused LSR OIR/MIR stages | `python3 scripts/run_tests.py --suite stage --stage oir --stage mir --filter oir_lsr --jobs 1 --o1` | yes | NOT_RUN | Module/MIR verifiers must pass for grouped recurrence positives and negatives |
| Conv2d OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter conv2d --jobs 1 --o1` | yes | NOT_RUN | Confirm exact loops/candidates structurally, without matcher names |
| Conv2d downstream MIR/ASM | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter conv2d --jobs 1 --o1` | yes | NOT_RUN | Inspect calls, branches, loads/stores, vregs and spills |
| Focused semantic e2e | `python3 scripts/run_tests.py --suite e2e --filter guarded_small_loop --jobs 1 --o1` | yes | NOT_RUN | Include alias, wraparound, border, and zero-trip data |
| Conv2d e2e | `python3 scripts/run_tests.py --suite e2e --filter conv2d --jobs 1 --o1` | yes | NOT_RUN | All three inputs must match |
| P7 disposition | Record P7 as either `implemented` after all conditional P7 gates pass or `deferred` with the missing-proof/no-win evidence in P7 Notes, Change Log, and Handoff | yes | NOT_RUN | No third state and no identity-based or order-changing fallback |
| P7 boundary FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_guarded_boundary_peel --jobs 1` | only if P7 implemented | NOT_RUN | Check stable-bound/dependence acceptance and rejection |
| P7 alias/order e2e | `python3 scripts/run_tests.py --suite e2e --filter guarded_boundary_peel --jobs 1 --o1` | only if P7 implemented | NOT_RUN | Must cover aliasing and lexicographic order |
| Full FileCheck/poly | `python3 scripts/run_tests.py --suite filecheck --suite poly --jobs 1` | before review | NOT_RUN | Detect unrelated IR-shape regressions |
| Full optimized correctness | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before review | NOT_RUN | Record pass/fail/skip counts |
| Baseline/current release builds | Run the two sequential release-build commands in `Same-machine de93e24 performance closure` | before the six perf runs | NOT_RUN | Isolated de93e24 compiler and current task compiler; one shared runtime archive |
| Focused baseline/current raw pair | Run the `focused` baseline call immediately followed by its current call | yes | NOT_RUN | Identical explicit three-file scope; archive raw MD/JSON and per-run provenance before continuing |
| Focused delta | Run the exact focused `compare_perf_baseline.py` command below | yes | NOT_RUN | Outputs `delta/focused/perf-delta.{md,json}` and `instruction-count-compare.json` |
| Focused delta adjudication | Run the exact focused adjudicator below | yes; also required if P7 implemented | NOT_RUN | PASS requires all three comparable rows, at least 1.05x geomean, no row below -3%, and no new spill/stack-slot growth |
| Affected baseline/current raw pair | Run the `affected` baseline call immediately followed by its current call | yes | NOT_RUN | Both select exactly `test/performance`, with no exclusions |
| Affected delta | Run the exact affected `compare_perf_baseline.py` command below | yes | NOT_RUN | Outputs `delta/affected/perf-delta.{md,json}` and `instruction-count-compare.json` |
| Affected delta adjudication | Run the exact broad-scope adjudicator for `affected` below | yes | NOT_RUN | Script heuristics plus spill/stack checks are hard gates; every smaller negative needs recorded attribution |
| CI-parity baseline/current raw pair | Re-read `.github/workflows/test.yml`, then run the `ci-parity` baseline call immediately followed by its current call | before `ready_for_review` | NOT_RUN | Identical current CI source selection/exclusions; this means CI-parity selection, not the runner-only enrichment steps |
| CI-parity delta | Run the exact CI-parity `compare_perf_baseline.py` command below | before `ready_for_review` | NOT_RUN | Outputs `delta/ci-parity/perf-delta.{md,json}` and `instruction-count-compare.json` |
| CI-parity delta adjudication | Run the exact broad-scope adjudicator for `ci-parity` below | before `ready_for_review` | NOT_RUN | Any unaccepted same-Yoolang regression blocks review |
| Final worktree protection | Run the exact final block in `Worktree Preservation Gates` | last gate before review handoff | NOT_RUN | Hash, tracked diff, protected status, and complete final status evidence must all agree |

### Legacy report archive

The report generated on 2026-07-16 predates the controlled pair below. Preserve it because it is
useful durable diagnostic context, but put it under `legacy-context`, never under `baseline`, and
never pass it to `compare_perf_baseline.py` for this task.

```bash
set -eu
TASK_ROOT="$(git rev-parse --show-toplevel)"
TASK_LEGACY_DIR="$TASK_ROOT/build/task-evidence/oir-guarded-small-loop-unroll/perf/legacy-context/2026-07-16-de93e24"
mkdir -p "$TASK_LEGACY_DIR"
test -f "$TASK_ROOT/build/perf-ci/perf-report.md"
test -f "$TASK_ROOT/build/perf-ci/perf-report.json"
cp "$TASK_ROOT/build/perf-ci/perf-report.md" "$TASK_LEGACY_DIR/perf-report.md"
cp "$TASK_ROOT/build/perf-ci/perf-report.json" "$TASK_LEGACY_DIR/perf-report.json"
python3 - "$TASK_LEGACY_DIR/perf-report.json" <<'PY'
from pathlib import Path
import json
import sys

payload = json.loads(Path(sys.argv[1]).read_text())
required = {
    "test/performance/conv2d-1.sy",
    "test/performance/conv2d-2.sy",
    "test/performance/conv2d-3.sy",
}
rows = {row["case"] for row in payload.get("rows", [])}
if not str(payload.get("generated_utc", "")).startswith("2026-07-16"):
    raise SystemExit(f"top-level report is no longer the 2026-07-16 context: {payload.get('generated_utc')!r}")
if payload.get("test_dirs") != ["test/performance"]:
    raise SystemExit(f"legacy scope changed: {payload.get('test_dirs')!r}")
if not required <= rows:
    raise SystemExit(f"legacy report is missing focused rows: {sorted(required - rows)}")
print(payload.get("generated_utc"), payload.get("compiler_binary"), payload.get("test_dirs"))
PY
```

### Same-machine de93e24 performance closure

Run this after P1--P6 and all required correctness gates for the current code pass. If P7 is
attempted, finish its edits plus the two conditional correctness gates first, keep its disposition
pending until the focused adjudication, and record `implemented` only if that adjudication passes;
if it fails, preserve the failed pair and leave P7 blocked for a scoped repair that removes only P7
edits and reruns the complete final pairs under a new run label (never use `git reset`,
`git checkout --`, stash, or cleanup). No build, test, editor indexing, or other benchmark may run
on the machine during the six timed invocations. The historical report above cannot satisfy any
gate in this section.

The baseline is a clean detached worktree at `de93e24`; the current compiler is built from the task
worktree. The future executor must replace `TASK_BASELINE_WT` with a new, empty, absolute path and
must inspect `git worktree list --porcelain` before allowing `git worktree add`. The placeholder
checks deliberately fail until it is replaced. Do not add an automatic worktree removal/cleanup
command: retain the isolated tree and evidence for review, and let the coordinator authorize any
later removal.

```bash
set -euo pipefail
TASK_CURRENT_WT="$(git rev-parse --show-toplevel)"
TASK_BASELINE_WT="/absolute/new/empty/path/to/yoolang-de93e24-baseline-REPLACE_ME"
test "$TASK_BASELINE_WT" != "/absolute/new/empty/path/to/yoolang-de93e24-baseline-REPLACE_ME"
case "$TASK_BASELINE_WT" in /*) ;; *) echo "baseline path must be absolute" >&2; exit 1 ;; esac
test "$TASK_BASELINE_WT" != "/"
test "$TASK_BASELINE_WT" != "$TASK_CURRENT_WT"
test -d "$(dirname "$TASK_BASELINE_WT")"
test ! -e "$TASK_BASELINE_WT"
git -C "$TASK_CURRENT_WT" worktree list --porcelain
test "$(git -C "$TASK_CURRENT_WT" rev-parse --short=7 'de93e24^{commit}')" = "de93e24"

# State-changing only after every path check above and the executor's inspection of the list.
git -C "$TASK_CURRENT_WT" worktree add --detach "$TASK_BASELINE_WT" de93e24
test "$(git -C "$TASK_BASELINE_WT" rev-parse --short=7 HEAD)" = "de93e24"
test -z "$(git -C "$TASK_BASELINE_WT" status --short)"

# Builds are sequential and occur before timing; do not rebuild between a paired baseline/current run.
(cd "$TASK_BASELINE_WT" && xmake f -m release && xmake)
(cd "$TASK_CURRENT_WT" && xmake f -m release && xmake)

TASK_BASELINE_COMPILER="$TASK_BASELINE_WT/build/linux/x86_64/release/compiler"
TASK_CURRENT_COMPILER="$TASK_CURRENT_WT/build/linux/x86_64/release/compiler"
TASK_RUNTIME_LIB="$TASK_BASELINE_WT/runtime/libsysy_riscv.a"
TASK_RISCV_GCC="$(command -v riscv64-linux-gnu-g++)"
TASK_RISCV_CLANGXX="$(command -v clang++)"
TASK_QEMU_BIN="$(command -v qemu-riscv64)"
TASK_PERF_ROOT="$TASK_CURRENT_WT/build/task-evidence/oir-guarded-small-loop-unroll/perf"
test -x "$TASK_BASELINE_COMPILER"
test -x "$TASK_CURRENT_COMPILER"
test -f "$TASK_RUNTIME_LIB"
mkdir -p "$TASK_PERF_ROOT"

run_task_perf() {
  local task_side="$1"
  local task_wt="$2"
  local task_compiler="$3"
  local task_label="$4"
  local task_scope="$5"
  local task_excludes="$6"
  local task_out="$TASK_PERF_ROOT/$task_side/$task_label"
  case "$task_side" in baseline|current) ;; *) return 2 ;; esac
  case "$task_label" in focused|affected|ci-parity) ;; *) return 2 ;; esac
  mkdir -p "$task_out"
  (
    cd "$task_wt"
    env \
      COMPILER_BIN="$task_compiler" \
      SYSY_RUNTIME_LIB="$TASK_RUNTIME_LIB" \
      PERF_TEST_DIRS="$task_scope" \
      PERF_EXCLUDE_CASES="$task_excludes" \
      PERF_MAX_CASES=0 \
      PERF_TIMEOUT_SEC=20 \
      RISCV_GCC="$TASK_RISCV_GCC" \
      RISCV_CLANGXX="$TASK_RISCV_CLANGXX" \
      QEMU_BIN="$TASK_QEMU_BIN" \
      QEMU_RISCV64="$TASK_QEMU_BIN" \
      ENABLE_QEMU_INSN_COUNT=0 \
      python3 scripts/compare_perf.py
  )
  cp "$task_wt/build/perf-ci/perf-report.md" "$task_out/perf-report.md"
  cp "$task_wt/build/perf-ci/perf-report.json" "$task_out/perf-report.json"
  printf '%s\n' \
    "PERF_TEST_DIRS=$task_scope" \
    "PERF_EXCLUDE_CASES=$task_excludes" \
    "PERF_MAX_CASES=0" \
    "PERF_TIMEOUT_SEC=20" \
    "ENABLE_QEMU_INSN_COUNT=0" \
    > "$task_out/measurement.env"
  sha256sum "$task_compiler" "$TASK_RUNTIME_LIB" > "$task_out/binaries.sha256"
  git -C "$task_wt" rev-parse HEAD > "$task_out/source-head.txt"
  git -C "$task_wt" branch --show-current > "$task_out/source-branch.txt"
  git -C "$task_wt" status --short > "$task_out/source-status.txt"
  {
    "$TASK_RISCV_GCC" --version
    "$TASK_RISCV_CLANGXX" --version
    "$TASK_QEMU_BIN" --version
    python3 --version
  } > "$task_out/toolchain.txt" 2>&1
  python3 - "$task_out/perf-report.json" "$task_wt" "$task_compiler" \
    "$TASK_RUNTIME_LIB" "$task_scope" "$task_label" <<'PY'
from pathlib import Path
import hashlib
import json
import sys

report_path, workspace, compiler, runtime, scope, label = sys.argv[1:]
payload = json.loads(Path(report_path).read_text())
rows = payload.get("rows", [])
cases = [row.get("case") for row in rows]
expected_dirs = scope.split(",")
checks = [
    payload.get("workspace") == workspace,
    payload.get("compiler_binary") == compiler,
    payload.get("runtime_lib") == runtime,
    payload.get("compiler_opt") == "-O1",
    payload.get("test_dirs") == expected_dirs,
    payload.get("cases") == len(rows) == len(set(cases)),
    payload.get("failures") == 0,
    bool(payload.get("generated_utc")),
]
if not all(checks):
    raise SystemExit(f"invalid {label} provenance/report: {payload}")
source_manifest = []
for case in sorted(cases):
    source_path = Path(workspace) / case
    source_manifest.append(f"{hashlib.sha256(source_path.read_bytes()).hexdigest()}  {case}")
Path(report_path).with_name("selected-sources.sha256").write_text("\n".join(source_manifest) + "\n")
if label == "focused":
    expected = {
        "test/performance/conv2d-1.sy",
        "test/performance/conv2d-2.sy",
        "test/performance/conv2d-3.sy",
    }
    if set(cases) != expected:
        raise SystemExit(f"focused row mismatch: {sorted(cases)!r}")
PY
}

check_task_perf_pair() {
  local task_label="$1"
  local task_base="$TASK_PERF_ROOT/baseline/$task_label"
  local task_current="$TASK_PERF_ROOT/current/$task_label"
  cmp "$task_base/measurement.env" "$task_current/measurement.env"
  cmp "$task_base/toolchain.txt" "$task_current/toolchain.txt"
  cmp "$task_base/selected-sources.sha256" "$task_current/selected-sources.sha256"
  python3 - "$task_base/perf-report.json" "$task_current/perf-report.json" "$task_label" <<'PY'
from pathlib import Path
import json
import sys

baseline = json.loads(Path(sys.argv[1]).read_text())
current = json.loads(Path(sys.argv[2]).read_text())
label = sys.argv[3]
baseline_cases = {row["case"] for row in baseline["rows"]}
current_cases = {row["case"] for row in current["rows"]}
if baseline["test_dirs"] != current["test_dirs"]:
    raise SystemExit(f"{label}: test_dirs differ")
if baseline_cases != current_cases or baseline["cases"] != current["cases"]:
    raise SystemExit(f"{label}: row membership differs")
if baseline["runtime_lib"] != current["runtime_lib"]:
    raise SystemExit(f"{label}: runtime differs")
if baseline["compiler_opt"] != current["compiler_opt"]:
    raise SystemExit(f"{label}: compiler mode differs")
if baseline["failures"] != 0 or current["failures"] != 0:
    raise SystemExit(f"{label}: correctness failure in perf pair")
print(f"{label}: provenance OK; {len(current_cases)} identical cases")
PY
}

TASK_FOCUSED_SCOPE="test/performance/conv2d-1.sy,test/performance/conv2d-2.sy,test/performance/conv2d-3.sy"
TASK_AFFECTED_SCOPE="test/performance"
TASK_CI_SCOPE="test/performance,test/bsb-final"
TASK_CI_EXCLUDES="test/performance/h-10-02.sy,test/performance/h-10-03.sy,test/bsb-final/2025-CPS-39.sy,test/bsb-final/2025-Z8N-28.sy"

# Re-read and lock the current CI selection now. A mismatch stops before either CI-parity run;
# update both calls and this task together rather than silently using the stale literals.
sed -n '155,175p' "$TASK_CURRENT_WT/.github/workflows/test.yml" \
  > "$TASK_PERF_ROOT/current-ci-selection.txt"
python3 - "$TASK_CURRENT_WT/.github/workflows/test.yml" "$TASK_CI_SCOPE" "$TASK_CI_EXCLUDES" <<'PY'
from pathlib import Path
import re
import sys

lines = Path(sys.argv[1]).read_text().splitlines()
expected_dirs = sys.argv[2]
expected_excludes = sys.argv[3].split(",")
dirs = []
excludes = []
for index, line in enumerate(lines):
    match = re.match(r"^\s*PERF_TEST_DIRS:\s*(.*?)\s*$", line)
    if match:
        dirs.append(match.group(1).strip("'\""))
    match = re.match(r"^(\s*)PERF_EXCLUDE_CASES:\s*\|\s*$", line)
    if not match:
        continue
    base_indent = len(match.group(1))
    for following in lines[index + 1:]:
        if not following.strip():
            continue
        indent = len(following) - len(following.lstrip())
        if indent <= base_indent:
            break
        excludes.append(following.strip())
if dirs != [expected_dirs] or excludes != expected_excludes:
    raise SystemExit(
        f"current CI selection changed: dirs={dirs!r}, excludes={excludes!r}; "
        "update both paired calls and the task"
    )
PY

# Each baseline/current pair is adjacent and sequential. Each call archives raw MD/JSON immediately.
run_task_perf baseline "$TASK_BASELINE_WT" "$TASK_BASELINE_COMPILER" focused "$TASK_FOCUSED_SCOPE" ""
run_task_perf current "$TASK_CURRENT_WT" "$TASK_CURRENT_COMPILER" focused "$TASK_FOCUSED_SCOPE" ""
check_task_perf_pair focused

run_task_perf baseline "$TASK_BASELINE_WT" "$TASK_BASELINE_COMPILER" affected "$TASK_AFFECTED_SCOPE" ""
run_task_perf current "$TASK_CURRENT_WT" "$TASK_CURRENT_COMPILER" affected "$TASK_AFFECTED_SCOPE" ""
check_task_perf_pair affected

run_task_perf baseline "$TASK_BASELINE_WT" "$TASK_BASELINE_COMPILER" ci-parity "$TASK_CI_SCOPE" "$TASK_CI_EXCLUDES"
run_task_perf current "$TASK_CURRENT_WT" "$TASK_CURRENT_COMPILER" ci-parity "$TASK_CI_SCOPE" "$TASK_CI_EXCLUDES"
check_task_perf_pair ci-parity
```

`ENABLE_QEMU_INSN_COUNT=0` makes local `DISABLED` informational and keeps the default flow usable.
If instruction counting is explicitly enabled, use one verified plugin-capable QEMU path for all
six invocations, archive that path/version, and keep the same pair ordering; never mix counted and
uncounted reports. CI-only heatmaps/static-diff/history remain enrichment, not part of this local
same-machine attribution gate.

### Same-machine deltas and adjudication

Only the six newly archived raw JSON files above may be inputs. Run all three delta commands; the
comparison script exits zero even for `REGRESSION`, so the explicit adjudicators are mandatory.

```bash
set -euo pipefail
TASK_ROOT="$(git rev-parse --show-toplevel)"
TASK_PERF_ROOT="$TASK_ROOT/build/task-evidence/oir-guarded-small-loop-unroll/perf"
mkdir -p "$TASK_PERF_ROOT/delta/focused" "$TASK_PERF_ROOT/delta/affected" "$TASK_PERF_ROOT/delta/ci-parity"

python3 scripts/compare_perf_baseline.py \
  --current "$TASK_PERF_ROOT/current/focused/perf-report.json" \
  --baseline "$TASK_PERF_ROOT/baseline/focused/perf-report.json" \
  --out-md "$TASK_PERF_ROOT/delta/focused/perf-delta.md" \
  --out-json "$TASK_PERF_ROOT/delta/focused/perf-delta.json" \
  --out-insn-json "$TASK_PERF_ROOT/delta/focused/instruction-count-compare.json" \
  --baseline-label "same-machine de93e24 focused" \
  --baseline-commit-sha de93e24

python3 scripts/compare_perf_baseline.py \
  --current "$TASK_PERF_ROOT/current/affected/perf-report.json" \
  --baseline "$TASK_PERF_ROOT/baseline/affected/perf-report.json" \
  --out-md "$TASK_PERF_ROOT/delta/affected/perf-delta.md" \
  --out-json "$TASK_PERF_ROOT/delta/affected/perf-delta.json" \
  --out-insn-json "$TASK_PERF_ROOT/delta/affected/instruction-count-compare.json" \
  --baseline-label "same-machine de93e24 affected-test-performance" \
  --baseline-commit-sha de93e24

python3 scripts/compare_perf_baseline.py \
  --current "$TASK_PERF_ROOT/current/ci-parity/perf-report.json" \
  --baseline "$TASK_PERF_ROOT/baseline/ci-parity/perf-report.json" \
  --out-md "$TASK_PERF_ROOT/delta/ci-parity/perf-delta.md" \
  --out-json "$TASK_PERF_ROOT/delta/ci-parity/perf-delta.json" \
  --out-insn-json "$TASK_PERF_ROOT/delta/ci-parity/instruction-count-compare.json" \
  --baseline-label "same-machine de93e24 current-CI-selection" \
  --baseline-commit-sha de93e24
```

Focused adjudication writes its own durable result and exits nonzero on a blocker:

```bash
set -eu
TASK_ROOT="$(git rev-parse --show-toplevel)"
TASK_PERF_ROOT="$TASK_ROOT/build/task-evidence/oir-guarded-small-loop-unroll/perf"
python3 - \
  "$TASK_PERF_ROOT/delta/focused/perf-delta.json" \
  "$TASK_PERF_ROOT/baseline/focused/perf-report.json" \
  "$TASK_PERF_ROOT/current/focused/perf-report.json" \
  > "$TASK_PERF_ROOT/delta/focused/adjudication.txt" <<'PY'
from pathlib import Path
import json
import sys

delta, baseline, current = [json.loads(Path(path).read_text()) for path in sys.argv[1:]]
expected = {
    "test/performance/conv2d-1.sy",
    "test/performance/conv2d-2.sy",
    "test/performance/conv2d-3.sy",
}
baseline_rows = {row["case"]: row for row in baseline["rows"]}
current_rows = {row["case"]: row for row in current["rows"]}
problems = []
if set(baseline_rows) != expected or set(current_rows) != expected or delta.get("comparable_cases") != 3:
    problems.append("focused case membership/comparable count is not exactly three")
if (delta.get("case_speedup_geomean") or 0.0) < 1.05:
    problems.append("focused same-Yoolang geomean is below 1.05x")
slow = [row["case"] for row in delta.get("rows", []) if row.get("delta_pct", -100.0) < -3.0]
if slow:
    problems.append(f"focused rows slower by more than 3%: {slow}")
for case in sorted(expected):
    before = baseline_rows[case].get("codegen_metrics", {})
    after = current_rows[case].get("codegen_metrics", {})
    for key in ("spills", "stack_slots"):
        if after.get(key, 0) > before.get(key, 0):
            problems.append(f"{case}: {key} grew from {before.get(key)} to {after.get(key)}")
if problems:
    print("BLOCKED")
    print("\n".join(f"- {item}" for item in problems))
    raise SystemExit(1)
print("PASS: exact three-case pair, >=1.05x geomean, every row >=-3%, no spill/stack-slot growth")
PY
```

Run this broad-scope adjudicator once with `TASK_SCOPE=affected` and once with
`TASK_SCOPE=ci-parity`. It records every sub-threshold negative for mandatory review even when the
comparison heuristic reports `OK`.

```bash
set -eu
TASK_ROOT="$(git rev-parse --show-toplevel)"
TASK_PERF_ROOT="$TASK_ROOT/build/task-evidence/oir-guarded-small-loop-unroll/perf"
for TASK_SCOPE in affected ci-parity; do
  python3 - \
    "$TASK_SCOPE" \
    "$TASK_PERF_ROOT/delta/$TASK_SCOPE/perf-delta.json" \
    "$TASK_PERF_ROOT/baseline/$TASK_SCOPE/perf-report.json" \
    "$TASK_PERF_ROOT/current/$TASK_SCOPE/perf-report.json" \
    > "$TASK_PERF_ROOT/delta/$TASK_SCOPE/adjudication.txt" <<'PY'
from pathlib import Path
import json
import sys

scope = sys.argv[1]
delta, baseline, current = [json.loads(Path(path).read_text()) for path in sys.argv[2:]]
baseline_rows = {row["case"]: row for row in baseline["rows"]}
current_rows = {row["case"]: row for row in current["rows"]}
problems = []
if set(baseline_rows) != set(current_rows):
    problems.append("baseline/current row sets differ")
if delta.get("comparable_cases") != len(current_rows):
    problems.append("delta comparable count does not cover the complete selected scope")
if delta.get("status") == "REGRESSION" or delta.get("regressions"):
    problems.append("compare_perf_baseline regression threshold reached")
for case in sorted(current_rows):
    before = baseline_rows[case].get("codegen_metrics", {})
    after = current_rows[case].get("codegen_metrics", {})
    for key in ("spills", "stack_slots"):
        if after.get(key, 0) > before.get(key, 0):
            problems.append(f"{case}: {key} grew from {before.get(key)} to {after.get(key)}")
negatives = [
    (row["case"], row["delta_sec"], row["delta_pct"])
    for row in delta.get("rows", [])
    if row.get("delta_pct", 0.0) < 0.0
]
print(f"scope={scope}; comparable={delta.get('comparable_cases')}; status={delta.get('status')}")
print(f"sub-threshold negative rows requiring task-record attribution: {negatives}")
if problems:
    print("BLOCKED")
    print("\n".join(f"- {item}" for item in problems))
    raise SystemExit(1)
print("AUTOMATED CHECK PASS; gate remains open until every listed negative is attributed as noise/non-regression")
PY
done
```

Performance acceptance:

- All selected cases in both sides of every pair must remain correct; no timeout or output mismatch
  may be reclassified or excluded to obtain a timing result.
- Focused OIR/cost output must contain real generic unroll/versioning candidates, proof metadata
  `Proven`, and fail-closed negative decisions. The focused delta passes only under the adjudicator's
  exact three-row, 1.05x geomean, per-row -3%, and spill/stack conditions.
- Affected and CI-parity deltas are mandatory hard same-Yoolang gates. Any
  threshold regression, new spill/stack-slot growth, or consistent smaller slowdown in related
  loop-heavy cases blocks review. Every negative row printed by the adjudicator must be investigated
  and attributed in this task; absent explicit user permission, a demonstrated regression is not
  accepted.
- The newly measured clean `de93e24` report for each identical scope is the only baseline for its
  delta. The 2026-07-16 report remains durable context and cannot satisfy same-machine, affected, or
  CI-parity comparison requirements.
- GCC/Clang ratios are external context only. Attribute the change primarily with same-Yoolang
  deltas, loop/backedge/guard/address counts, MIR stage metrics, spills, and assembly.
- Do not replace a bad pair with selectively chosen reruns. If machine noise invalidates a pair,
  retain it, record why, and rerun both baseline and current for that complete scope in the same
  order under a new archived run label.

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Raise the existing unroll trip/count or code-growth threshold | The loops are exact and small | rejected as a fix: no candidate reaches the cost model because CFG shape matching fails |
| Recognize convolution/stencil source structure | Could directly emit a specialized kernel | forbidden: identity/benchmark-shaped selector and unnecessary for a general compiler pass |
| Directly unroll the current pre-tested CFG | Avoids changing rotation | fallback implementation choice only if PHI-safe rotation cannot be certified; record the structural reason before choosing it |
| Generalize PHI-safe loop rotation, then reuse multi-block unroll | Reuses existing unroll/PHI cloning path and canonicalizes other programs | chosen first implementation route |
| Remove all boundary guards after unroll | Produces straight-line code | rejected without a fast-path implication proof and exact fallback |
| Runtime fast path plus original fallback | Preserves semantics and captures common interior regions | chosen when selected from generic affine IR facts and structurally/SMT proven |
| Explicit all-interior then all-border split | Can reduce guard frequency | rejected without dependence/noalias proof because it changes lexicographic memory order |
| Per-row prefix/interior/suffix peeling | Can remove version checks while preserving order | optional P7 after stable-bound and alias/dependence proof plus measurement |
| Add a convolution-aware MIR combine | Late and loses loop/guard structure | rejected; OIR is the earliest sound layer |

## Change Log

- 2026-07-17: created the scoped task from de93e24 evidence; recorded the missing-candidate chain,
  PHI-latch canonicalization, exact 2--5 innermost-first unroll, structural/finite-PE/QF_BV
  versioning, fast-path-only unroll, grouped LSR, optional order-preserving boundary peeling, and
  focused/broad validation contract. No implementation or test command was run.
- 2026-07-17: repaired the scoped task after independent task-document review. Added a five-entry
  initial-status gate and final hash/diff/status proof for the two user-owned files; made the
  2026-07-16 report context-only; specified an isolated clean `de93e24` release build, six sequential
  provenance-checked raw runs, three same-scope deltas, and explicit adjudication; and mapped every
  Patch Queue verifier to named matrix gates. No branch/worktree command, hash snapshot, build,
  implementation, test, or performance command was run during this documentation repair.
- 2026-07-17: fixed the raw-run provenance gate by importing `hashlib` in the same embedded Python
  block that hashes selected performance sources. No gate, test, or performance command was run.

## Open Questions

- None. P7 is explicitly conditional rather than an unresolved design requirement: defer it safely
  if the required stable-bound and alias/dependence proof is unavailable or it does not improve the
  measured result.

## Handoff Note

Current state:

- Task is scoped; no production code or tests have been changed or run.
- Base is `master@de93e24`.
- The complete task-generation status is the five-entry set under `Worktree Preservation Gates`:
  user-owned `M docs/README.md` and `?? docs/egraph-design.md`, task-generated
  `M docs/tasks/README.md` and the two untracked 2026-07-17 task files. The two user-owned paths
  must remain byte-for-byte untouched.
- The 2026-07-16 performance report is durable context only. Completion requires new isolated
  same-machine baseline/current raw pairs and deltas for focused, affected-directory, and current
  CI-parity scopes exactly as specified above.
- Current evidence proves the 5-iteration trip counts are available, but the PHI-bearing-latch
  rotation rejection and conditional-latch-only multi-block matcher prevent any `LoopUnroll`
  candidate from being emitted.

Next action:

- Execute P0 exactly: run `Initial worktree protection`, archive the legacy report, verify
  `master@de93e24`, then create `task/oir-guarded-small-loop-unroll`. Implement and verify only P1,
  update this file, and do not begin P2 until P1's three named gates pass. Create the validated
  isolated baseline worktree only when entering the final same-machine performance closure.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-17-oir-guarded-small-loop-unroll.md`
- `src/pass/oir/OIRLoopTransforms.cpp` at `550-700`, `1410-1590`, and `3169-3265`
- `src/pass/oir/OIROptimizationPipelinePass.cpp` at `1-55`
- `test/ir/oir_loop_transforms.sy`
