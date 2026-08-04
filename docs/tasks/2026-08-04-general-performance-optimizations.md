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

## Dynamic GEP Alias Soundness Follow-up

Fix base: `5f97e68` on `task/general-performance-optimizations`; the working tree was clean at
task start.

### Confirmed Counterexample

For a local `int values[2][4]`, a dynamic read through the decomposed GEP path
`[0, 0, row, col]` and a later write through `[0, 1, col]` overlap when `row == 1`.  With input
`1 2`, the unoptimized compiler returned `7099`, while the pre-fix `5f97e68 -O1` compiler
returned `7007` by reusing the load from before the store.

### Root Cause

`collect_pointer_path` flattened every GEP segment into one index vector, discarding each
segment's base type, boundary, and stride.  `memory_location()` correctly gave up its byte offset
when a dynamic index was present, but both same-root branches in `OIRAliasAnalysis::alias()` then
used `same_index_path` and `has_disjoint_constant_index` to recover `MustAlias` or `NoAlias` from
that lossy vector.  Neither conclusion follows from the retained information.

### Correctness Invariant

Alias analysis may return `MustAlias` or `NoAlias` only from a representation that preserves the
addressing semantics needed for that proof.  A concatenated sequence of indices from multiple
GEPs does not preserve each segment's base type or stride, so an unknown dynamic byte position
must fail closed to `MayAlias`.  Pointer identity remains `MustAlias`, and complete byte
offset-and-size intervals may retain their proven `MustAlias`/`NoAlias` result.

### Context Budget And Ledger

This follow-up keeps the existing task protocol and performance-workflow references, and limits
new source/script anchors to the alias implementation, focused regressions, test runner, task
record, and AI disclosure.

| File | Lines / query | Why | Keep? |
| --- | --- | --- | --- |
| `src/oir/OIRAnalysis.cpp` | `collect_pointer_path`, `alias`, `memory_location` | Remove unsound dynamic path proofs in both same-root branches | yes |
| `include/oir/OIRAnalysis.h` | `AliasResult`, `MemoryLocation`, AA API | Confirm public API and reliable byte-location facts | yes |
| `test/ir/oir_dynamic_gep_alias.sy` | full | Focused store/load structural regression | yes |
| `test/easy/oir_dynamic_gep_alias.{sy,in,out}` | full | Independent executable counterexample | yes |
| `test/ir/oir_guarded_call_cse.sy` | pointer checks | Preserve the prior pointer-alias soundness regression | yes |
| `scripts/oir_infra_tests.py` | direct AA construction | Cover both lossy-path `MustAlias` and `NoAlias` results | yes |
| `scripts/run_tests.py` | e2e discovery/output handling | Confirm `.out` includes stdout and exit code | no |
| `docs/AI_USAGE.md` | modification-scope section | Disclose the follow-up without filling human fields | yes |

### Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| GEP-P1 | Add independent semantic, OIR-shape, and direct-AA regressions | `test/easy/oir_dynamic_gep_alias.*`, `test/ir/oir_dynamic_gep_alias.sy`, `scripts/oir_infra_tests.py` | infra, focused FileCheck and e2e | complete | Pre-fix O1 was `7007`; FileCheck lacked the second load; direct AA returned old `MustAlias` |
| GEP-P2 | Delete concatenated-index fallback proofs when byte locations are unknown | `src/oir/OIRAnalysis.cpp` | OIR/MIR/ASM stages and e2e | complete | Both `same_index_path` and constant-index separation fallbacks removed; root stripping retained |
| GEP-P3 | Record verification, performance effect, and AI-modified scope | this file, `docs/AI_USAGE.md`, `docs/tasks/README.md` | `git diff --check` | complete | Human reviewer/date fields remain untouched |

### Follow-up Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Pre-fix evidence | frozen `5f97e68` compiler | yes | PASS | unoptimized `7099/0`; O1 `7007/0`; focused FileCheck and direct-AA assertion failed as expected |
| Direct AA infra | `python3 scripts/run_tests.py --suite infra --jobs 1` | yes | PASS | identity/constant intervals remain precise; both dynamic cases are `MayAlias` |
| Build | `xmake` | yes | PASS | release compiler rebuilt |
| Dynamic GEP FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter test/ir/oir_dynamic_gep_alias.sy --jobs 1` | yes | PASS | store followed by a fresh load |
| Dynamic GEP YIR/OIR/MIR/ASM | `python3 scripts/run_tests.py --suite stage --stage yir --stage oir --stage mir --stage asm --filter test/easy/oir_dynamic_gep_alias.sy --jobs 1 --o1` | yes | PASS | 4/4 |
| Dynamic GEP unoptimized e2e | `python3 scripts/run_tests.py --suite e2e --filter test/easy/oir_dynamic_gep_alias.sy --jobs 1` | yes | PASS | stdout `7099`, exit `0` |
| Dynamic GEP optimized e2e | `python3 scripts/run_tests.py --suite e2e --filter test/easy/oir_dynamic_gep_alias.sy --jobs 1 --o1` | yes | PASS | stdout `7099`, exit `0` |
| Guarded-call FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter test/ir/oir_guarded_call_cse.sy --jobs 1` | yes | PASS | 1/1 |
| Guarded-call pointer stage/e2e | `python3 scripts/run_tests.py --suite stage --suite e2e --filter test/easy/oir_guarded_call_cse_pointer_alias.sy --jobs 1 --o1` | yes | PASS | 5/5; stdout `135`, exit `0` |
| Guarded-call stage/e2e | `python3 scripts/run_tests.py --suite stage --suite e2e --filter test/easy/oir_guarded_call_cse.sy --jobs 1 --o1` | yes | PASS | 5/5 |
| Bit-digit FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter test/ir/oir_bit_digit_idiom.sy --jobs 1` | yes | PASS | 1/1 |
| Bit-digit easy stage/e2e | `python3 scripts/run_tests.py --suite stage --suite e2e --filter test/easy/oir_bit_digit_idiom.sy --jobs 1 --o1` | yes | PASS | 5/5 |
| Bit-digit differential stage/e2e | `python3 scripts/run_tests.py --suite stage --suite e2e --filter test/functional/oir_bit_digit_idiom_differential.sy --jobs 1 --o1` | yes | PASS | 5/5 |
| YIR view FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter test/ir/yir_view.sy --jobs 1` | yes | PASS | 1/1 |
| YIR view stage/e2e | `python3 scripts/run_tests.py --suite stage --suite e2e --filter test/easy/yir_view.sy --jobs 1 --o1` | yes | PASS | 5/5 |
| Full optimized suite | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | yes | PASS | 1579 passed, 0 failed, 1 existing `shuffle1` e2e skip |
| Diff integrity | `git diff --check` | yes | PASS | no whitespace errors |
| Current `test/performance` run | `COMPILER_BIN=build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` | yes | PASS | 60/60 |
| `test/performance` adjudication | `python3 scripts/compare_perf_baseline.py --current build/task-evidence/2026-08-04-general-opts/dynamic-gep-alias/current.json --baseline build/task-evidence/2026-08-04-general-opts/soundness-fix/fresh-ab/fixed.json --out-md build/task-evidence/2026-08-04-general-opts/dynamic-gep-alias/delta.md --out-json build/task-evidence/2026-08-04-general-opts/dynamic-gep-alias/delta.json --out-insn-json build/task-evidence/2026-08-04-general-opts/dynamic-gep-alias/instruction-count-compare.json --baseline-label 5f97e68` | yes | PASS | 60/60 `CODE_NO_CHANGE`; 0 wins, 0 losses, 60 neutral |
| CI-parity 115-case performance timing | `COMPILER_BIN=build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance,test/bsb-final PERF_EXCLUDE_CASES=test/performance/h-10-02.sy,test/performance/h-10-03.sy,test/bsb-final/2025-CPS-39.sy,test/bsb-final/2025-Z8N-28.sy python3 scripts/compare_perf.py` | no | NOT_RUN | no target-code change in the complete 60-case performance corpus; full optimized correctness covers both directories |
| Dynamic instruction comparison | same 60-case command with `ENABLE_QEMU_INSN_COUNT=1 QEMU_RISCV64=/usr/bin/qemu-riscv64 QEMU_INSN_API_VERSION=6` on both compilers | no | NOT_RUN | baseline counting was disabled, so the matching current run also left it disabled |

### Performance Impact

The pre-fix compiler is frozen at `build/perf-baselines/5f97e68/compiler`, SHA-256
`be2220b1a4e85c186434d72b1e714df11b55aa118a8776d349fceca600f31f40`.  A fresh current
`test/performance` run passed all 60 cases.  Comparison against the preserved `5f97e68` report
classified all 60 as `CODE_NO_CHANGE`, so the patch changes no target code in that complete
performance scope: adjudicated delta `0.00%`, 0 wins, 0 losses, 60 neutral.  Raw QEMU totals
(`5.2882s` current versus `5.2833s` baseline) are diagnostic noise because the binaries are
identical.  Evidence is under
`build/task-evidence/2026-08-04-general-opts/dynamic-gep-alias/`.

- Baseline report: `build/task-evidence/2026-08-04-general-opts/soundness-fix/fresh-ab/fixed.{json,md}`.
- Current report: `build/task-evidence/2026-08-04-general-opts/dynamic-gep-alias/current.{json,md}`.
- Adjudicated comparison: `build/task-evidence/2026-08-04-general-opts/dynamic-gep-alias/delta.{json,md}`.

### Residual Risk And Unrun Gates

The conservative result may suppress alias-dependent optimization in unseen programs with
unknown dynamic same-root GEPs; it cannot enable an illegal transformation.  Existing constant
byte-offset arithmetic (`type_size`, GEP offset accumulation, and interval endpoint addition)
does not have explicit overflow checks.  This patch does not add symbolic offsets or expand that
pre-existing boundary; every newly affected dynamic case fails closed.

The 115-case CI-parity performance timing scope and dynamic QEMU instruction counts were not run.
They are not needed to adjudicate this patch's 60-case `CODE_NO_CHANGE` result, but remain explicit
unrun performance diagnostics rather than claimed coverage.

### Handoff State

Current conclusion: both lossy syntactic fallbacks have been removed.  Pointer identity and
complete constant byte intervals retain their strong results; every tested unknown dynamic
same-root case now returns `MayAlias`.

Next action: team review.  All required correctness gates and the complete 60-case target-code
comparison pass; only the explicitly optional diagnostics above remain unrun.

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
- 2026-08-04: reopened at `5f97e68` to repair unsound dynamic-GEP `MustAlias`/`NoAlias`
  fallbacks; reproduced `7007` versus `7099`, added direct AA/FileCheck/e2e regressions, and made
  unknown dynamic same-root locations fail closed.
- 2026-08-04: passed the 1579-item optimized suite and classified all 60 performance cases as
  `CODE_NO_CHANGE` against the frozen `5f97e68` compiler; recorded optional unrun diagnostics.
- 2026-08-04: completed independent code/test/documentation review and moved the task back to
  `ready_for_review`; human competition sign-off remains intentionally pending.

## Handoff

The implementation is ready for team review on `task/general-performance-optimizations`.  The
dynamic-GEP fix makes unknown same-root locations fail closed, the new `7099/0` and existing
`135/0` regressions pass, the full optimized suite is 1579/0/1, and all 60 performance cases are
target-code-identical to `5f97e68`.  Before competition submission, a team member must review the
production patch and complete the human-modification/sign-off fields in `docs/AI_USAGE.md`; that
human attestation cannot be supplied by Codex.
