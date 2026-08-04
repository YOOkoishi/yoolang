# Task: General Semantics-Preserving Performance Optimizations

Status: ready_for_review
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

The figures above remain valid after the soundness repair.  A fresh same-machine comparison against
the preserved pre-fix `cd047e6` compiler classifies all 60 cases as `CODE_NO_CHANGE`: the repaired
compiler emits the same target-code fingerprints for the complete performance corpus while no
longer reusing pointer-parameter reads without a reachable-memory proof.

## Soundness Follow-up

### Correctness Invariant

A direct readonly call may reuse an earlier result only when equal guarded arguments imply the
same scalar inputs and every memory location the callee may read has the same value.  A summary
that says a pointer parameter is read describes memory reachable through that pointer, not merely
the single element addressed by the actual pointer value.  Unknown provenance or access range
must therefore fail closed.

### Confirmed Regression

For a recursive readonly callee that returns `a[index] + depth`, the sequence
`call(a, index)`, `a[1] = 99`, `call(a, index)` was guarded and reused when `index == 1`.
The preserved `58d15c1 -O1` compiler and current unoptimized compiler return `135`; the pre-fix
`cd047e6 -O1` compiler returns `56`.  The regression is attributable to
`OIRGuardedCallCSE` interpreting exact pointer-location NoAlias as a proof about the callee's
unbounded pointer-parameter read footprint.

### Context Ledger

| File | Lines / query | Why | Keep? |
| --- | --- | --- | --- |
| `src/oir/OIRAnalysis.cpp` | pointer paths, ModRef projection, `call_param_may_alias`, MemorySSA call clobbers | Root correctness fix | yes |
| `include/oir/OIRAnalysis.h` | memory summary and MemorySSA API | Confirm public legality facts | yes |
| `src/pass/oir/OIRGuardedCallCSE.cpp` | candidate legality and proof metadata | Pass-local fail-closed gate | yes |
| `src/pass/oir/OIRBitDigitIdiomPass.cpp` | fixed-width matcher/proof metadata | Derive the full-width constant from the IR type | yes |
| `test/ir/oir_guarded_call_cse.sy` | positive and negative transform shapes | Focused structural regression | yes |
| `test/easy/oir_guarded_call_cse*.sy` | optimized executable behavior | End-to-end regression | yes |
| `build/task-evidence/2026-08-04-general-opts/full-ab/` | current 60-case report | Pre-fix performance and hit attribution | yes |

### Patch Queue

- [x] Make pointer-parameter ModRef effects compare reachable pointer roots rather than exact GEP
  locations.
- [x] Restrict guarded-call reuse to memory-free or precisely tracked global-read callees until
  range-aware pointer footprints exist.
- [x] Add a negative IR check and executable regression for an intervening in-object store while
  retaining existing positive guarded-call checks.
- [x] Express the bit-digit full-width trip count through the matched integer type without changing
  i32 behavior.
- [x] Re-run focused, full correctness, and same-machine performance gates; update the handoff.

### Follow-up Verification Matrix

| Gate | Status | Evidence |
| --- | --- | --- |
| Preserved pre-fix compiler | PASS | `build/perf-baselines/cd047e6/compiler`, SHA-256 `6e75094b...f183` |
| Miscompile reproducer before fix | FAIL as expected | optimized result `56`, reference result `135` |
| Focused GuardedCallCSE FileCheck/e2e | PASS | IR check 1/1; guarded e2e 2/2; pointer-alias result `135` |
| YIR/OIR/MIR/ASM stage verification | PASS | pointer-alias 4/4; deep ModRef 3/3; bit-digit 6/6 |
| Full optimized test suite | PASS | 1573 passed, 0 failed, 1 existing expected skip |
| Same-machine performance preservation | PASS | 60/60 `CODE_NO_CHANGE` versus preserved `cd047e6`; 0 regressions |
| AI-use disclosure | REVIEW_REQUIRED | `docs/AI_USAGE.md`; team member must confirm human modification/review fields |

## Delivered Optimizations

| Area | Mature implementation | Legality / fallback |
| --- | --- | --- |
| Interprocedural ModRef | Replace the unsound 64-round cap with a reverse-caller worklist that converges to a fixed point | External/unresolved calls remain conservative |
| Affine signed-remainder recurrence | Replace a proven `x = (x + c) % m` unit-trip loop with `(x0 + n*c) % m` | Runtime guards require `n >= 3`, nonnegative initial state, positive constants, and no signed overflow; otherwise execute the original loop |
| Exact small-loop unroll | Extend constant-trip unrolling to repair PHI-latch/multi-block loops and both true/false backedges | Exact i128 trip proof, i32 wrap rejection, side-exit/call/live-out validation, and profitability gates |
| Nested remainder simplification | Fold `(x % d) % d` to `x % d` for the same nonzero signed divisor | Exact signed-remainder identity only |
| Signed bit-digit reconstruction | Recognize an exact full-width `/2`, `%2`, power-of-two reconstruction of AND/OR/XOR and emit constant-time bitwise closed forms | Trip count is derived from the matched integer type (currently i32); proven for negative values and `INT_MIN`, with modulo wrap preserved; strict structural matcher only |
| Guarded readonly-call reuse | Reuse an earlier direct call when exact integer argument equality holds | Same direct internal nonwriting callee with no pointer/unknown reads, identical MemorySSA clobber for tracked globals, bounded equality guards, slow call path retained, cost-model gate |
| MIR expression CSE | Version virtual-register operands/results so a redefinition cannot leave a stale available expression | Physical definitions and call/memory barriers invalidate state; global CSE retains its single-def restriction |
| Signed power-of-two div/rem lowering | Use distinct `sign` and `bias` virtual registers | Prevents Local CSE from observing an overwritten temporary and preserves negative i32 semantics |
| Exact float identities | Key MIR immediates, OIR GVN, inline specialization, and constant equivalence by IEEE-754 f32 bits | Distinguishes `+0/-0`, adjacent f32 values, and NaN payloads; memzero accepts only all-zero `+0` bits |

The accepted closed forms are the theoretical optimum for their matched semantic idioms: a
linear recurrence becomes one multiply/add/remainder, a full-width digit reconstruction becomes a
bounded set of comparisons and bitwise instructions, and duplicate readonly work becomes one call
plus a guard.  The original computation remains available whenever a proof obligation fails.

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
| Release build | PASS; repaired compiler SHA-256 `be2220b1a4e85c186434d72b1e714df11b55aa118a8776d349fceca600f31f40` |
| Focused IR tests | PASS for affine recurrence, bit-digit idiom, guarded call CSE, PHI-latch unroll, and memzero `-0.0` rejection |
| Differential/e2e tests | PASS for signed recurrence, all-sign bit reconstruction, deep ModRef call chain, signed div/rem powers of two, adjacent f32 immediates, and negative zero |
| Full optimized suite | PASS: 1573 passed, 0 failed, 1 expected skip |
| Suite breakdown | 1 infra + 40 FileCheck + 49 polyhedral + 1188 YIR/OIR/MIR/ASM stage + 295 e2e passes |
| Full performance corpus | PASS: 60 cases, 0 failed |
| Same-machine A/B | IMPROVEMENT: 6.3272s -> 4.8752s, 1.30x, +22.95%; 9 faster / 0 slower / 51 neutral |
| Formatting | PASS for all new files and all changed lines |
| Diff integrity | `git diff --check` PASS |
| Soundness repair A/B | PASS: all 60 performance cases are `CODE_NO_CHANGE` versus `cd047e6`; raw timing is diagnostic only |
| Contest technical compliance | PASS: semantic regression fixed; identity audit still passes; no benchmark-specific dispatch added |
| AI disclosure / human review | PENDING: complete disclosure is recorded in `docs/AI_USAGE.md`; 参赛队人工复核与签名不得由 Codex 代填 |
| Independent code review | PASS for automated cross-review; no high/medium issue found in ModRef and guarded-call repair |

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
- Soundness-repair comparison:
  `build/task-evidence/2026-08-04-general-opts/soundness-fix/fresh-ab/`
- AI-use disclosure and team review checklist:
  `docs/AI_USAGE.md`

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
- 2026-08-04: reopened the task after reproducing a guarded readonly-call pointer-footprint
  miscompile; froze the `cd047e6` compiler/report as the performance-preservation baseline.
- 2026-08-04: repaired reachable-pointer ModRef reasoning, made guarded-call reuse fail closed for
  pointer/unknown reads, corrected equivalent-GEP byte-range aliasing, and added regressions.
- 2026-08-04: passed 1573 optimized checks and a fresh 60-case comparison with every case
  `CODE_NO_CHANGE`; added a truthful AI-use disclosure and left team sign-off explicit.

## Handoff

The implementation is ready for team review on `task/general-performance-optimizations`.  Technical
correctness and performance-preservation gates pass.  Before competition submission, a team member
must review the production patch and complete the human-modification/sign-off fields in
`docs/AI_USAGE.md`; that human attestation cannot be supplied by Codex.
