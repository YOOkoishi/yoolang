# Task: MIR Performance Review Fixes

Status: ready_for_review
Created: 2026-07-11
Last update: 2026-07-11
Owner: Codex
Branch: ipsccp
Base commit: 776a374

## Goal

Repair the two performance regressions introduced by `7c7bc1a`: keep register-sized
`MemZero` operations inline unless a future range/cost proof selects `memset`, and make the
four-way pretested pointer-loop unroll use the existing MIR cost model instead of running
unconditionally. Keep call/clobber metadata and final assembly lowering driven by one shared
MemZero policy.

## Non-goals

- Do not change the MIR bit-idiom combine behavior from `7c7bc1a`.
- Do not add benchmark-, function-, variable-, input-, or expected-output-specific conditions.
- Do not add a new dynamic-MemZero value-range analysis in this repair; unknown dynamic lengths
  return to the previously designed inline loop.
- Do not redesign general loop unrolling or add a new optimization level. This task only gates the
  existing factor-four pretested pointer-loop transform.

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

## Review Evidence

The review compared the parent of `7c7bc1a` with the introduced behavior:

| Case | Before | Regressed | Impact |
| --- | --- | --- | --- |
| Two dynamic 12-byte clears | 161288 QEMU instructions | 161462 | about 87 extra instructions per clear |
| `dynamic_clear` assembly | 512-byte leaf frame, no saved registers | 528-byte frame, saves `ra+s0` | small dynamic clear became a full call site |
| Complex pointer kernel | 16-byte frame, saves `s0-s1` | 64-byte frame, saves `ra+s0-s6` | dynamic memset plus unroll pressure |
| Isolated unknown-bound pointer loop, runtime `n=1`, 1000 repetitions | 267049 QEMU instructions | 269228 | final MIR 117 to 187; saved registers `s0-s1` to `s0-s3` |

The bug-introducing commit changed 11 files by `+546/-45`, including six source files, and mixed
bit combine, dynamic memset, and loop unroll behavior without a matching task record.

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 2,
  `docs/tasks/2026-06-07-oir-memzero-mir-lowering.md` and
  `docs/tasks/2026-07-07-cost-model-gating-scope.md`
- Source/script anchors: max 8; tests are read only with their corresponding patch
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- frontend, parser, YIR, runtime, and unrelated OIR optimization files
- unrelated MIR bit-combine implementation
- broad generated performance artifacts before focused gates pass

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol and patch-size rules | yes | required |
| `docs/tasks/2026-06-07-oir-memzero-mir-lowering.md` | full | authoritative dynamic-inline design decision | yes | dynamic byte counts must stay inline |
| `docs/tasks/2026-07-07-cost-model-gating-scope.md` | full | unroll profitability policy | yes | future unroll must be cost-gated |
| `include/mir/MIR.h` | MemZero threshold and operand helpers | canonical MemZero call classification | yes | shared by lowering, RA, and ASM |
| `src/mir/AsmPrinter.cpp` | `emit_memzero`, `emit_memset_call`, stack-address facts | final inline/call choice | yes | no second hand-written memset ABI sequence |
| `src/pass/mir/MIRRegAllocPass.cpp` | special clobbers and call-like classification | keep RA metadata equal to emitted assembly | yes | dynamic MemZero is not a call |
| `src/pass/oir/OIRToMIRVRegLowerer.cpp` | `lower_memzero` and aggregate zero store | VReg function call metadata | yes | mirrored with stack lowerer |
| `src/pass/oir/OIRToMIRStackLowerer.cpp` | `lower_memzero` and aggregate zero store | stack-lowering call metadata | yes | mirrored with VReg lowerer |
| `src/pass/mir/MIRPointerLoopExitPass.cpp` | pretested rewrite and factor-four unroll | profitability decision site | yes | use existing `Stats` cost-model context |
| `include/pass/mir/MIRPeepholeCommon.h` | shared peephole statistics | module static-size baseline | yes | no cumulative, order-dependent budget state |
| `src/pass/mir/MIRPeepholePipelinePass.cpp` | module pass entry | initialize the stable module baseline once | yes | external functions excluded |
| `include/pass/mir/MIRCostModel.h` | `MIRTransformCostEstimate`, `allows_transform` | existing MIR gate API | no | API shape confirmed; no change expected |
| `test/ir/oir_memzero_loop.sy` | full | OIR/MIR/ASM MemZero regression | yes | assert leaf frame and no dynamic call |
| `test/ir/mir_pointer_loop_exit.sy` | full | unroll shape and policy diagnostics | yes | compare policy decisions/output |
| `test/ir/mir_pointer_loop_unroll_small.sy` | full | durable small-module regression | yes | aggressive policy rejects both unknown-bound candidates |
| `test/easy/pointer_unroll.sy` | full | focused stage/e2e coverage | yes | existing generic runtime case |
| `scripts/compare_perf.py` | test selection and QEMU count options | focused performance command | no | use `PERF_TEST_DIRS` and optional instruction counter |

## Branch

Decision: current branch

Reason:

```text
The user requested the review fixes in the active managed workspace. The repair is recorded
separately from the bug-introducing commit even though it remains on the current ipsccp branch.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git rev-parse --abbrev-ref HEAD
```

## Invariants And Risks

Correctness invariants:

- A register byte count in MIR `MemZero` emits the inline loop, including its non-positive-count
  guard, and is not marked call-like by register allocation or function metadata.
- The inline-loop clobber model includes every scratch register the ASM path can use: `t4/t5`,
  `t3` for a non-zero fill value, and `t6` for the post-RA parallel-copy swap fallback.
- A constant byte count at or above `kMemZeroMemsetThresholdBytes` still emits `call memset` and
  retains caller-saved clobber/function-call metadata.
- Non-zero repeated-byte dynamic fills remain correct in the inline loop.
- Pointer-loop exit rewriting remains legal independently of whether factor-four unrolling is
  accepted; rejecting unroll must leave the single-lane rewritten loop valid.
- Every attempted unknown-trip-count unroll that reaches profitability evaluation records a
  `LoopUnroll` decision when the filter matches.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes,
  or expected outputs.

Risk areas:

- If ASM, RA, and OIR-to-MIR function call metadata use different MemZero predicates, leaf-frame
  regressions or caller-saved corruption can reappear.
- An unroll estimate that ignores dispatch/peel setup, static growth, and register-pressure growth
  can reproduce the `n=1` regression while appearing profitable.
- Cost-model rejection must occur before mutating the CFG; otherwise a rejected candidate may leave
  partial dispatch/peel blocks behind.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Restore dynamic-length `MemZero` to the inline loop and make RA use the same call classification as ASM | `include/mir/MIR.h`, `src/mir/AsmPrinter.cpp`, `src/pass/mir/MIRRegAllocPass.cpp`, `test/ir/oir_memzero_loop.sy` | focused MemZero output assertions, alias harness, frame inspection | complete | Dynamic form has no `call memset`/`sd ra` and keeps the 512-byte leaf frame; static 4000-byte forms still call `memset`. |
| P2 | Gate factor-four pointer-loop unroll through `MIRCostModel` | `include/pass/mir/MIRPeepholeCommon.h`, `src/pass/mir/MIRPeepholePipelinePass.cpp`, `src/pass/mir/MIRPointerLoopExitPass.cpp`, `test/ir/mir_pointer_loop_exit.sy`, `test/ir/mir_pointer_loop_unroll_small.sy` | three-policy diagnostics, durable small-module output assertions, stage/e2e, QEMU A/B | complete | Uses a stable module footprint, dynamic control benefit, confidence, register-pressure risk, and projected post-RA growth. Matrix is conservative R/R, balanced R/A, aggressive A/A; the isolated small module is aggressive R/R. |
| P3 | Converge duplicated MemZero/call policy and record independently landable review fixes | `src/pass/oir/OIRToMIRVRegLowerer.cpp`, `src/pass/oir/OIRToMIRStackLowerer.cpp`, this task file, `docs/tasks/README.md` | direct MemZero assertions, diff inspection, `git diff --check` | complete | Both lowerers use the shared predicate for `note_call`; the duplicate dynamic ABI/call sequence is gone; no bit-combine source changed. |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | Release compiler rebuilt successfully. |
| Dynamic MemZero FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_memzero_loop --jobs 1` | yes | BLOCKED_ENV | `/bin/bash: FileCheck: command not found`; equivalent direct ASM assertions passed. |
| Pointer policy FileChecks | focused `mir_pointer_loop_exit` and `mir_pointer_loop_unroll_small` filecheck suites | yes | BLOCKED_ENV | Same missing `FileCheck`; all emitted MIR/ASM and cost-report checks were run directly and passed. |
| Direct policy diagnostics | emit `LoopUnroll` reports under all three policies | yes | PASS | Large fixture: conservative R/R, balanced R/A, aggressive A/A. Small module under aggressive: R/R with `CodeGrowthTooHigh`. |
| Inline scratch alias harness | compile/run temporary MIR-to-ASM harness over `t4/t5/t6` address/count combinations | yes | PASS | Parallel-copy swap and ordinary alias cases preserve both inputs. |
| Focused MIR/ASM stage | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter pointer_unroll --jobs 1 --o1` | yes | PASS | 2/2. |
| Focused E2E | `python3 scripts/run_tests.py --suite e2e --filter pointer_unroll --jobs 1 --o1` | yes | PASS | 1/1. |
| Dynamic clear stage/E2E | focused `01_mm1` MIR/ASM stage and E2E commands | yes | PASS | 2/2 stage and 1/1 E2E. |
| Infrastructure | `python3 scripts/run_tests.py --suite infra --jobs 1` | yes | PASS | 1/1. |
| Full MIR/ASM stage | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --jobs 1 --o1` | before review | PASS | 560/560. |
| Full E2E | `python3 scripts/run_tests.py --suite e2e --jobs 1 --o1` | before review | PASS | 278 passed, 0 failed, 1 input-size skip. |
| Focused MIR metrics | exact pre-fix compiler versus repaired compiler | yes | PASS | Isolated small fixture final MIR 187 to 113; `01_mm1` final MIR 195 to 191. |
| QEMU instruction A/B | `QEMU_INSN_API_VERSION=2 tools/qemu-insn-count/count_insn.py ...` | yes | PASS | Isolated `n=1` x1000: 269487 to 263219; `pointer_unroll`: 187029 to 186554; 12-byte clear incremental cost: 36 to 10 instructions. |
| Large-kernel guardrail | same QEMU helper on `01_mm1` | yes | PASS_WITH_NOTE | 97682212 to 98082230 (+0.41%) versus the call-based bug version, while retaining unroll; still about 36.7% below the prior inline/no-unroll 155017588 result. Frame improves 80B + `ra+s0-s7` to 48B + `s0-s4`. |
| Full performance corpus | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | only for a broad corpus claim | NOT_RUN | No broad corpus-performance claim is made; focused exact-compiler QEMU A/B covers the reviewed regressions. |
| Independent code review | read-only review of final source and generated output | yes | PASS | No remaining code-level blocker. |
| Diff hygiene | `git diff --check` | yes | PASS | |

## Acceptance Criteria

- `dynamic_clear` has a 512-byte leaf frame, does not save `ra`, and contains no `call memset`;
  dynamic non-zero repeated-byte fill also remains inline.
- Constant large clears/fills still call the existing `emit_memset_call` path and are modeled as
  call sites by lowerers and RA.
- `--cost-model-filter=LoopUnroll` reports every structurally eligible candidate with its stable
  module footprint, dynamic estimate, confidence, and projected post-RA growth. Decisions no
  longer depend on a hidden unconditional path or on prior-candidate traversal state.
- The durable small-module fixture rejects both unknown-bound candidates under the default
  aggressive policy, removing the reviewed runtime `n=1` regression. A larger module may retain
  the transform under aggressive policy when its module-growth budget covers the risk.
- A candidate dominated by an `n >= 32` guard is policy-sensitive: conservative rejects it,
  balanced and aggressive accept it. The unguarded candidate in the same large fixture is rejected
  by balanced on confidence and accepted by aggressive.
- Rejecting unroll leaves no `unroll4.dispatch` or `unroll4.peel` blocks; accepting it preserves the
  existing factor-four output and correctness.
- Focused performance evidence removes the reviewed small-clear and `n=1` regressions while
  retaining the large-loop benefit. Complex frames improve materially but are not claimed to
  return completely to the pre-unroll 16-byte baseline.
- The repair diff contains only the two performance behavior fixes, their shared-policy cleanup,
  tests, and task documentation.

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Always call `memset` for a register byte count | Simple final lowering | rejected: contradicts the original design and regresses small/zero dynamic clears at function scope |
| Keep dynamic `memset` but add a runtime size branch | Could help large dynamic clears | deferred: requires a range/cost design and still imposes call metadata on the whole function unless represented explicitly before RA |
| Delete factor-four unroll for every dynamic bound | Removes the `n=1` regression | rejected: `n >= 2` can benefit; retain it behind confidence, dynamic-benefit, and module-growth gates |
| Use only the candidate function as the growth denominator | Simple local calculation | rejected: measured `01_mm1` is 40/94 instructions before post-RA projection and would always reject despite its large runtime benefit |
| Accumulate accepted growth greedily while walking functions | Enforces an aggregate module cap | rejected for this repair: it made results source-order dependent and let a low-confidence candidate crowd out a later proven bound; proper collection/ranking needs separate cost-model work |
| Keep independent ASM, RA, and lowerer predicates | Minimal local edits | rejected: the policies already drifted; one shared classification is required |
| Repair bit combine in the same patch | It was part of `7c7bc1a` | rejected: independent semantic point with no cited performance regression in this review |

## Change Log

- 2026-07-11: restored register-count MemZero to the inline loop, centralized the constant-call
  predicate, and modeled all inline scratch clobbers including the `t6` parallel-copy fallback.
- 2026-07-11: gated factor-four pointer unroll using a stable module baseline, dynamic control
  estimate, confidence, and PreRA-to-post-RA growth projection. Final policy matrix is conservative
  R/R, balanced R/A, aggressive A/A; the separate small module is aggressive R/R.
- 2026-07-11: added durable MemZero, policy, and small-module regressions; completed focused QEMU
  A/B, 560 stage gates, 279 E2E cases, and independent code review.
- 2026-07-11: created a dedicated review-fix record, split dynamic MemZero and pointer-loop unroll
  into independently verifiable patches, and recorded shared-policy/scope cleanup as P3.

## Open Questions

- A future cost-model task may collect/rank multiple module candidates before applying transforms,
  allowing an aggregate module-growth budget without source-order dependence. This repair uses
  candidate-local decisions against a stable original module footprint.
- Install `FileCheck` in the developer image so the durable RUN lines execute in this environment;
  direct equivalent assertions already pass.

## Handoff Note

Current state:

- P1, P2, and P3 are complete in the working tree and independently reviewed.
- Dynamic register counts are inline and non-call-like; large constants retain `memset`.
- Pointer unroll is visible in cost diagnostics and controlled by policy/module risk. The reviewed
  small module rejects it, while `pointer_unroll` and `01_mm1` retain the profitable form.
- Build, infrastructure, full stage, full E2E, focused QEMU A/B, manual output assertions, alias
  coverage, and diff hygiene pass. Only FileCheck execution is unavailable locally.

Next action:

- Review and land P1, P2, and P3 as separate semantic commits if commit access is desired.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-11-mir-performance-review-fixes.md`
- `include/mir/MIR.h`
- `src/mir/AsmPrinter.cpp`
- `src/pass/mir/MIRRegAllocPass.cpp`
- `src/pass/oir/OIRToMIRVRegLowerer.cpp`
- `src/pass/oir/OIRToMIRStackLowerer.cpp`
- `src/pass/mir/MIRPointerLoopExitPass.cpp`
- `test/ir/oir_memzero_loop.sy`
- `test/ir/mir_pointer_loop_exit.sy`
- `test/ir/mir_pointer_loop_unroll_small.sy`
