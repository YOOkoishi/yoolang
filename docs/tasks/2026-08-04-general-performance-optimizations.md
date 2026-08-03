# Task: General Semantics-Preserving Performance Optimizations

Status: complete
Created: 2026-08-04
Last update: 2026-08-04
Owner: Codex
Branch: `task/general-performance-optimizations`
Base commit: `58d15c1`

## Outcome

The selected production-quality OIR/MIR optimizations are implemented and verified.  Every
transformation is selected by IR structure, data flow, memory effects, and cost-model evidence;
none dispatches on a testcase, source path, user function/symbol name, known input, or expected
output.

On the final compiler binary, all 60 public performance cases pass.  Against the preserved
same-machine `58d15c1` compiler baseline, the generated programs' adjudicated QEMU runtime total
improves from 6.3272s to 4.8752s: 1.30x overall, or 22.95%, with 9 evidence-backed wins, no
significant regressions, and 51 neutral cases.

## Delivered Optimizations

| Area | Mature implementation | Legality / fallback |
| --- | --- | --- |
| Interprocedural ModRef | Replace the unsound 64-round cap with a reverse-caller worklist that converges to a fixed point | External/unresolved calls remain conservative |
| Affine signed-remainder recurrence | Replace a proven `x = (x + c) % m` unit-trip loop with `(x0 + n*c) % m` | Runtime guards require `n >= 3`, nonnegative initial state, positive constants, and no signed overflow; otherwise execute the original loop |
| Exact small-loop unroll | Extend constant-trip unrolling to repair PHI-latch/multi-block loops and both true/false backedges | Exact i128 trip proof, i32 wrap rejection, side-exit/call/live-out validation, and profitability gates |
| Nested remainder simplification | Fold `(x % d) % d` to `x % d` for the same nonzero signed divisor | Exact signed-remainder identity only |
| Signed bit-digit reconstruction | Recognize exact 32-step `/2`, `%2`, power-of-two reconstruction of AND/OR/XOR and emit constant-time bitwise closed forms | Proven for all i32 values, including negative values and `INT_MIN`, with modulo-2^32 wrap preserved; strict structural matcher only |
| Guarded readonly-call reuse | Reuse an earlier direct call when exact integer argument equality holds | Same direct internal nonwriting callee, identical MemorySSA clobber, bounded equality guards, slow call path retained, cost-model gate |
| MIR expression CSE | Version virtual-register operands/results so a redefinition cannot leave a stale available expression | Physical definitions and call/memory barriers invalidate state; global CSE retains its single-def restriction |
| Signed power-of-two div/rem lowering | Use distinct `sign` and `bias` virtual registers | Prevents Local CSE from observing an overwritten temporary and preserves negative i32 semantics |
| Exact float identities | Key MIR immediates, OIR GVN, inline specialization, and constant equivalence by IEEE-754 f32 bits | Distinguishes `+0/-0`, adjacent f32 values, and NaN payloads; memzero accepts only all-zero `+0` bits |

The accepted closed forms are the theoretical optimum for their matched semantic idioms: a
linear recurrence becomes one multiply/add/remainder, a 32-iteration digit reconstruction becomes
a bounded set of comparisons and bitwise instructions, and duplicate readonly work becomes one
call plus a guard.  The original computation remains available whenever a proof obligation fails.

## Pipeline Placement

- Signed bit-digit recognition runs while the loop is still structurally intact, before the main
  inlining cleanup windows.
- Guarded call reuse runs after inlining/specialization so later recursive expansion cannot break
  the guard/merge dominance relation.
- Affine recurrence recognition runs after loop rotation and again after final loop cleanup.
- All accepted transformations emit normal cost-model diagnostics; legality never depends on
  benchmark timing or identity.

## Correctness Work Found During Differential Testing

The new negative-input differential cases exposed two pre-existing backend bugs.  Both are fixed
because leaving them in place would make an otherwise legal OIR optimization appear to miscompile:

1. signed power-of-two div/rem reused one MIR virtual register for both sign and bias;
2. MIR Local CSE keyed non-SSA virtual registers without definition versions.

The float-immediate collision found during the same independent review was also a correctness
issue: decimal strings rounded distinct f32 constants to the same CSE/GVN/specialization key.  All
optimization identities now use the exact 32-bit representation.  A related memzero check now
rejects `-0.0`, whose byte representation is not zero.

## Verification

| Gate | Final result |
| --- | --- |
| Release build | PASS; final compiler SHA-256 `6e75094be51b5eedb265439ed902eb58d5fb3c26640b26f7d3fd2ef4b5a7f183` |
| Focused IR tests | PASS for affine recurrence, bit-digit idiom, guarded call CSE, PHI-latch unroll, and memzero `-0.0` rejection |
| Differential/e2e tests | PASS for signed recurrence, all-sign bit reconstruction, deep ModRef call chain, signed div/rem powers of two, adjacent f32 immediates, and negative zero |
| Full optimized suite | PASS: 1568 passed, 0 failed, 1 expected skip |
| Suite breakdown | 1 infra + 40 FileCheck + 49 polyhedral + 1184 YIR/OIR/MIR/ASM stage + 295 e2e items |
| Full performance corpus | PASS: 60 cases, 0 failed |
| Same-machine A/B | IMPROVEMENT: 6.3272s -> 4.8752s, 1.30x, +22.95%; 9 faster / 0 slower / 51 neutral |
| Formatting | PASS for all new files and all changed lines |
| Diff integrity | `git diff --check` PASS |
| Contest compliance | PASS: no testcase/path/output identities in changed production code; all triggers are structural |
| Independent review | PASS: PHI/unroll, guarded call CSE, bit-digit proof/compliance, MIR CSE/versioning, and exact float keys |

The sole full-suite skip is the repository's existing `test/performance/shuffle1.sy` e2e exclusion;
its four compiler stages pass, and the standalone 60-case performance runner also executes it
successfully.

## Final Same-Machine Performance Changes

| Case | Baseline | Final | Speedup | Improvement |
| --- | ---: | ---: | ---: | ---: |
| `conv2d-1` | 0.4561s | 0.0311s | 14.67x | 93.18% |
| `conv2d-2` | 0.1313s | 0.0152s | 8.64x | 88.42% |
| `conv2d-3` | 0.0486s | 0.0113s | 4.30x | 76.75% |
| `huffman-01` | 0.0508s | 0.0100s | 5.08x | 80.31% |
| `huffman-02` | 0.0517s | 0.0104s | 4.97x | 79.88% |
| `huffman-03` | 0.0516s | 0.0097s | 5.32x | 81.20% |
| `knapsack_naive-1` | 0.2875s | 0.0455s | 6.32x | 84.17% |
| `knapsack_naive-2` | 0.2855s | 0.0188s | 15.19x | 93.42% |
| `knapsack_naive-3` | 0.2866s | 0.0457s | 6.27x | 84.05% |

`many_mat_cal-*` remains neutral under evidence adjudication.  No unsafe benchmark-shaped rewrite
was added to force a win.

## Evidence

- Final 60-case reports:
  `build/task-evidence/2026-08-04-general-opts/full-ab/{baseline-58d15c1,current,delta}.{json,md}`
- Focused 12-case attribution:
  `build/task-evidence/2026-08-04-general-opts/focused-ab/{baseline-58d15c1,current,delta}.{json,md}`
- Bit-digit-specific attribution and MIR metrics:
  `build/task-evidence/2026-08-04-general-opts/bitdigit/`
- Preserved base compiler/report:
  `build/perf-baselines/58d15c1/`

QEMU dynamic-instruction counting was disabled because no compatible plugin evidence was
available.  The hard performance decision therefore uses assembly/executable identity plus
multi-sample wall-time evidence floors; inconclusive timing is classified as neutral.

## Deliberately Deferred Work

Repeated full-overwrite iteration elimination is still deferred.  A mature generic version needs
dynamic rectangular may/must write regions, direct-call memory footprints, alias separation, and
scalar definite-definition/live-out proofs.  A narrow matcher for the visible `many_mat_cal`
shape would be unsound or benchmark-specific and is prohibited by this task's compliance gate.

One unrelated baseline semantic gap remains recorded for follow-up: AST lowering implements float
unary minus as `+0.0 - x`, so source `-0.0` does not preserve the negative-zero sign.  The optimizer
itself now handles a real `-0.0` constant correctly, and the regression constructs it as
`(-1.0) * 0.0`.

## Change Log

- 2026-08-04: established the clean `58d15c1` baseline and compliance gates.
- 2026-08-04: implemented and independently reviewed all selected OIR/MIR transformations.
- 2026-08-04: fixed backend correctness blockers exposed by negative and float differential tests.
- 2026-08-04: passed the final 1568-item optimized suite and final 60/60 same-machine A/B gate.

## Handoff

The selected patch set is complete and ready for normal code review.  No commit or remote publish
was performed.  Further work on `many_mat_cal-*` should begin with the deferred general memory
footprint analysis, not a testcase-shaped optimizer.
