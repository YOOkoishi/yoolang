# Task: Conv2D Loop Kernel Optimization

Status: ready_for_review
Created: 2026-06-12
Last update: 2026-06-12
Owner: Codex
Branch: task/conv2d-loop-kernel-optimization
Base commit: df20afe

## Goal

Implement general loop optimizations that improve the fixed-small-kernel guarded
2D convolution shape represented by `conv2d`, without recognizing function names,
filenames, variable names, testcase identity, or specific input values.

The first target is to reduce redundant hot-loop work in the `repeat -> r -> c ->
kr -> kc` nest: small constant kernel loops, repeated affine address computation,
and per-tap boundary guards.

## Non-goals

- Do not special-case `conv2d`, `idx`, `KSIZE`, `N_eff`, filenames, bsb IDs, input sizes, or expected outputs.
- Do not replace the convolution algorithm with a benchmark-specific shortcut.
- Do not assume pointer arguments are disjoint unless alias/call-site evidence proves it.
- Do not start with register allocation changes unless new evidence contradicts the current 0-spill measurements.
- Do not weaken tests, performance scripts, runtime behavior, or benchmark inputs.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [x] YIR
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
- Related docs: max 1, only `docs/polyhedral.md` if YIR polyhedral legality is needed
- Source/script anchors: max 8 keep=yes anchors
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- frontend/parser/runtime implementation
- unrelated performance cases outside `conv2d-*` and the identical bsb-final copies
- full generated perf artifacts before focused rows identify the relevant sections

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required before resuming |
| `docs/tasks/README.md` | active task table | register this task | yes | keep in sync |
| `docs/tasks/TEMPLATE.md` | full | task shape | no | used once to create this record |
| `test/performance/conv2d-1.sy` | full | representative source shape | yes | identical to `conv2d-2/3` and bsb-final copies by sha256 |
| `test/performance/conv2d-2.sy` | sha256 + perf row | input/perf variation | no | same source as `conv2d-1` |
| `test/performance/conv2d-3.sy` | sha256 + perf row | input/perf variation | no | same source as `conv2d-1` |
| `test/bsb-final/2025-OKA-1.sy` | sha256 | duplicate bsb-final regression case | no | same source as `conv2d-1` |
| `test/bsb-final/2025-1NE-37.sy` | sha256 | duplicate bsb-final regression case | no | same source as `conv2d-1` |
| `test/bsb-final/2025-MGB-26.sy` | sha256 | duplicate bsb-final regression case | no | same source as `conv2d-1` |
| `build/perf-ci/perf-report.{md,json}` | focused conv2d run | current timing and MIR metrics | yes | regenerated on 2026-06-12 |
| `public/external/hy/fcc36336197a/perf-report.json` | `conv2d-*` rows | historical HY gap and instruction-count evidence | yes | not current baseline; use as opportunity signal |
| `/tmp/conv2d-1.{yir,oir,pre-ra.mir}` | direct emits | YIR/OIR/MIR shape evidence | no | regenerate with commands in Verification Matrix |
| `src/main/main.cpp` | `add_*_pipeline` ranges | optimized `-O1` pipeline order | yes | YIR polyhedral, OIR, MIR stage order |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | full | existing OIR pass order and cleanup windows | yes | loop transforms already run, but no unroll/peel |
| `src/pass/oir/OIRLoopTransforms.cpp` | loop transform ranges | current rotate/unswitch/bound-tighten/memset scope | yes | added capped small constant-trip full unroll here |
| `src/pass/oir/OIRLoopStrengthReductionPass.cpp` | relevant candidate/apply ranges | address recurrence and pointer-phi helpers | yes | secondary anchor for avoiding address recomputation |
| `include/oir/OIRScalarOpt.h` | pass declarations/stats | add pass hook/stats if OIR-based | yes | added `unroll_small_constant_loops` declaration and stats |
| `src/pass/yir/YIRPolyhedralTransformPass.cpp` | `try_interchange_or_tile`, stencil ranges | YIR polyhedral opportunity and current gaps | yes | SCoP is recognized, but interchange/tile is TODO |
| `src/pass/oir/OIRScalarOptUtils.cpp` | stats message | report loop unroll stats | yes | added `loopunroll=` |
| `test/ir/oir_loop_transforms.sy` | focused FileCheck | generic small-loop unroll coverage | yes | no conv2d/testcase-name trigger |

## Branch

Decision: implementation branch created and used

Reason:

```text
Implementation is on:

task/conv2d-loop-kernel-optimization
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
```

Observed:

```text
git status --short: docs task files modified/untracked before implementation; source/test edits added in task branch
git rev-parse --short HEAD: df20afe
git branch --show-current: task/conv2d-loop-kernel-optimization
```

## Invariants And Risks

Correctness invariants:

- Transform only general counted/affine loops whose bounds, steps, side effects, and memory behavior are proven.
- Preserve tap accumulation order for signed integer arithmetic unless the IR explicitly permits reassociation.
- Preserve boundary semantics for small dynamic extents; if `N <= 2 * pad`, fall back to the original guarded region or emit a guarded fast path.
- Preserve pointer-alias semantics. Loop interchange or accumulation into `Out` is only legal when reads/writes cannot alias in a way that changes observed values.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, argument values, or expected outputs.
- Any optimization must be expressible as a semantics-preserving transform over a generic IR pattern such as affine guarded stencil loops or small constant-trip counted loops.

Risk areas:

- Code-size growth and register pressure after unrolling 5x5 kernels.
- OIR phi/CFG rewriting for nested loops with reductions.
- Boundary peeling legality when loop bounds are dynamic.
- Alias/modref precision for `In`, `Out`, and `K` pointer arguments.
- Interaction with existing YIR polyhedral transforms and OIR loop unswitch/bound-tightening.
- Performance regressions in non-convolution small loops if unroll profitability is too broad.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add focused generic IR tests for small constant-trip loop unrolling and guarded affine stencil shape | `test/ir/oir_loop_transforms.sy` | FileCheck + OIR stage | done | Generic `small_guarded_kernel`; asserts `unr.` clones and removal of `i < 5` loop test |
| P2 | Add a capped small constant-trip loop unroll transform for side-effect-safe counted loops | `src/pass/oir/OIRLoopTransforms.cpp`, `include/oir/OIRScalarOpt.h`, `src/pass/oir/OIROptimizationPipelinePass.cpp`, `src/pass/oir/OIRScalarOptUtils.cpp` | focused FileCheck, OIR verifier | done | Full-unrolls small proven constant-trip multi-block loops; cap `trip_count <= 5`, loop blocks/instruction budget; runs late to avoid LICM-induced spill pressure |
| P3 | Add guarded affine-stencil interior/border splitting or equivalent bound peeling | YIR polyhedral transform or OIR loop transforms | stage/e2e + focused perf | deferred | Larger follow-up; current patch keeps guarded edge semantics and does not attempt interior/border splitting |
| P4 | Reuse affine row-base and kernel-address recurrences after shape changes | `OIRLoopStrengthReductionPass.cpp` or MIR peephole if evidence points lower | MIR stage + ASM inspection | deferred | LSR not changed; unroll exposes fixed lanes but residual address/guard work remains |
| P5 | Run focused and broad performance gates, then tune profitability | perf scripts and generated reports | focused conv2d + full performance | done | Focused conv2d and full 119-case perf pass; same-machine baseline diff could not run because worktree creation was blocked by environment approval |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Source identity | `sha256sum test/performance/conv2d-1.sy test/performance/conv2d-2.sy test/performance/conv2d-3.sy test/bsb-final/2025-OKA-1.sy test/bsb-final/2025-1NE-37.sy test/bsb-final/2025-MGB-26.sy` | yes | PASS | all six files hash to `89b3115d489b4735adcecc5881356f763f1a4d6d1e20c6c3fba7b18b41bcf851` |
| Pre-implementation focused performance | `PERF_TEST_DIRS=test/performance/conv2d-1.sy,test/performance/conv2d-2.sy,test/performance/conv2d-3.sy python3 scripts/compare_perf.py` | yes | PASS | 3 cases PASS; generated `2026-06-12 12:42:35 UTC` |
| Pre-implementation report readback | `sed -n '1,120p' build/perf-ci/perf-report.md` | yes | PASS | yoolang geomean `0.92x` vs GCC, `0.73x` vs Clang++; no faster cases |
| Pre-implementation YIR emit | `build/linux/x86_64/release/compiler -O1 --emit-yir test/performance/conv2d-1.sy -o /tmp/conv2d-1.yir` | yes | PASS | YIR keeps five nested `while` loops and per-tap boundary guards |
| Pre-implementation OIR emit | `build/linux/x86_64/release/compiler -O1 --emit-oir test/performance/conv2d-1.sy -o /tmp/conv2d-1.oir` | yes | PASS | OIR keeps `kr/kc` loops; only partial unswitching/LSR happened |
| Pre-implementation MIR emit | `build/linux/x86_64/release/compiler -O1 --emit-mir-stage=pre-ra test/performance/conv2d-1.sy -o /tmp/conv2d-1.pre-ra.mir` | yes | PASS | pre-RA still has loop branches and address arithmetic in tap loops |
| Pre-implementation MIR metrics | `build/linux/x86_64/release/compiler -O1 --emit-mir-metrics test/performance/conv2d-1.sy` | yes | PASS | per-case final `535` MIR instrs, `0` spills, `0` stack slots |
| Polyhedral dump | `build/linux/x86_64/release/compiler -O1 --emit-poly test/performance/conv2d-1.sy` | yes | PASS | conv2d SCoP recognized, but no useful transform is applied |
| Build after implementation | `xmake` | yes | PASS | final post-format rebuild passed |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_loop_transforms --jobs 1` | yes | PASS | 1 passed |
| Full FileCheck | `python3 scripts/run_tests.py --suite filecheck --jobs 1` | yes | PASS | 21 passed |
| Focused stage/e2e | `python3 scripts/run_tests.py --suite stage --suite e2e --filter conv2d --jobs 1 --o1` | yes | PASS | 12 stage + 3 e2e passed |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | yes | PASS | 1425 passed, 0 failed, 1 skipped |
| Focused performance | `PERF_TEST_DIRS=test/performance/conv2d-1.sy,test/performance/conv2d-2.sy,test/performance/conv2d-3.sy python3 scripts/compare_perf.py` | yes | PASS | 3 cases; generated `2026-06-12 13:05:40 UTC`; geomean GCC `0.94x`, Clang++ `0.76x`; compiler times `0.6474s`, `0.1869s`, `0.0709s` |
| Full performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | yes | PASS | 119 cases, 0 failed; generated `2026-06-12 13:12:41 UTC`; geomean GCC `0.91x`, Clang++ `0.99x`; faster cases GCC `38`, Clang++ `39` |
| Same-machine baseline diff | `git worktree add /tmp/yoolang-baseline-conv2d df20afe` then baseline perf + `scripts/compare_perf_baseline.py` | desired | SKIP | environment rejected `.git` write approval for `git worktree add`; no baseline report available without bypassing approval |

## Current Evidence

The six known conv2d files are identical source programs. Treat them as one
source shape with multiple inputs/performance rows.

Focused current performance:

| Case | GCC `-O3` | Clang++ `-O3` | yoolang `-O1` |
| --- | ---: | ---: | ---: |
| `test/performance/conv2d-1.sy` | `0.5999s` | `0.4708s` | `0.6658s` |
| `test/performance/conv2d-2.sy` | `0.1741s` | `0.1392s` | `0.1896s` |
| `test/performance/conv2d-3.sy` | `0.0680s` | `0.0555s` | `0.0734s` |

Focused geomean: yoolang is `0.92x` vs GCC and `0.73x` vs Clang++; yoolang is
slower on all three current rows.

MIR stage metrics for the focused run:

| Stage | Instrs | Moves | Branches | Jumps | Loads | Stores | Spills | Stack slots |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| lowered | 2187 | 525 | 117 | 360 | 87 | 48 | 0 | 0 |
| post-combine | 2070 | 525 | 117 | 360 | 87 | 48 | 0 | 0 |
| pre-ra | 1818 | 474 | 117 | 204 | 87 | 48 | 0 | 0 |
| post-ra | 1818 | 474 | 117 | 204 | 87 | 48 | 0 | 0 |
| final | 1605 | 279 | 117 | 186 | 87 | 48 | 0 | 0 |

This is not currently a register allocator failure: `pre-ra == post-ra`,
`spills=0`, and `stack_slots=0`.

Historical HY artifact:

| Case | yoolang time | HY time | yoolang insns | HY insns | Instr ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| `conv2d-1` | `0.7920s` | `0.0560s` | `1,658,871,155` | `144,206,440` | `11.5x` |
| `conv2d-2` | `0.2311s` | `0.0284s` | `452,785,970` | `39,418,623` | `11.5x` |
| `conv2d-3` | `0.0909s` | `0.0202s` | `149,855,473` | `13,918,436` | `10.8x` |

Treat this as evidence that large improvements are plausible, not as a current
baseline requirement: the current focused run did not configure an HY compiler.

Representative source hot loop:

```c
while (kr < KSIZE) {
    int kc = 0;
    int rr = r + kr - pad;
    while (kc < KSIZE) {
        int cc = c + kc - pad;
        if (rr >= 0 && rr < N_eff && cc >= 0 && cc < N_eff) {
            sum = sum + In[idx(rr, cc, N_eff)] * K[idx(kr, kc, KSIZE)];
        }
        kc = kc + 1;
    }
    kr = kr + 1;
}
```

The optimized OIR still has:

- `kr.loop` and `kc.loop` with constant upper bound `5`.
- Per-`kc` checks for `cc >= 0` and `cc < N_eff`.
- Only partial unswitching for `rr >= 0`.
- `K.addr.start.ptr += 5` and `K.addr.ptr += 1` recurrences, but no full tap unroll.
- Output address recomputation after the tap loops.

The compiler assembly still has nested tap-loop branches and address arithmetic
inside the hot convolution path. GCC/Clang assembly for the same baseline C++
shows much more aggressive 5x5 tap expansion and fixed-offset `K`/input loads.

Polyhedral evidence:

- `--emit-poly` recognizes a conv2d SCoP with schedule `[%repeat, %r, %c, %kr, %kc]`.
- `src/pass/yir/YIRPolyhedralTransformPass.cpp::try_interchange_or_tile` is currently a TODO returning `false`.
- Existing stencil carry logic is for loop-carried RAW reuse, not read-only 5x5 convolution taps.

## Implementation Result

Implemented a generic OIR full-unroll pass for small proven constant-trip
multi-block loops. The matcher is structural:

- unique outside predecessor, single latch, one header-controlled loop exit
- `ScalarEvolution::constant_trip_count` proves `2..5` iterations
- no nested loop headers
- loop block and cloned-instruction caps
- header phi live-outs are repaired through the loop exit
- no testcase, function, variable, filename, or input-size recognition

The pass runs late in `OIROptimizationPipelinePass`, after loop cleanup and
before final dead-code cleanup. An earlier experiment running it inside the
aggressive iteration window caused subsequent LICM to extend live ranges and
introduced 31 spills in `conv2d-1`; the final placement avoids that spill
regression.

Focused post-implementation performance:

| Case | Old yoolang | New yoolang focused | New yoolang full-run row |
| --- | ---: | ---: | ---: |
| `test/performance/conv2d-1.sy` | `0.6658s` | `0.6474s` | `0.6578s` |
| `test/performance/conv2d-2.sy` | `0.1896s` | `0.1869s` | `0.1884s` |
| `test/performance/conv2d-3.sy` | `0.0734s` | `0.0709s` | `0.0728s` |

Focused geomean improved from GCC `0.92x` / Clang++ `0.73x` to GCC `0.94x`
/ Clang++ `0.76x`.

Focused post-implementation MIR totals for the three conv2d rows:

| Stage | Instrs | Moves | Branches | Jumps | Loads | Stores | Spills | Stack slots |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| lowered | 2454 | 543 | 123 | 432 | 111 | 48 | 0 | 0 |
| post-combine | 2343 | 546 | 123 | 432 | 111 | 48 | 0 | 0 |
| pre-ra | 2016 | 483 | 123 | 207 | 111 | 48 | 0 | 0 |
| post-ra | 2016 | 483 | 123 | 207 | 111 | 48 | 0 | 0 |
| final | 1788 | 273 | 123 | 189 | 111 | 48 | 0 | 0 |

This is a modest runtime win, not a full solution to the historical HY gap.
The `kr` loop, per-tap boundary guards, and interior/border split opportunity
remain as follow-up work.

## Recommended Implementation Direction

1. Start with a general capped small-loop unroll pass.

   The `kr < 5` and `kc < 5` loops are constant-trip loops after OIR
   simplification. A general unroll pass with a tight cost cap can remove hot
   branch overhead, expose fixed `K` offsets, and make boundary predicates easier
   for GVN/VRP/SCCP to simplify. Preserve accumulation order by cloning lanes in
   original iteration order.

2. Add guarded affine-stencil interior peeling.

   For loops whose guarded memory accesses are affine in surrounding IVs, split
   the `r/c` domain into a fast interior where `0 <= r + kr - pad < N` and
   `0 <= c + kc - pad < N` are proven for every unrolled tap, plus conservative
   edge regions that keep the original guard. This is the most likely path toward
   the historical 10x instruction-count gap because it removes per-tap branches
   over the dominant interior area.

3. Re-run address recurrence cleanup after the shape transform.

   After unroll/peel, rerun or extend LSR/GVN so row bases and kernel offsets are
   reused. The desired shape is pointer increments or fixed offsets, not repeated
   `mul r, N` and `mul kr, 5` style address rebuilds in the hot path.

4. Treat loop interchange as a later, alias-gated option.

   Moving `kr/kc` outside `r/c` or accumulating into `Out` can be profitable, but
   it changes memory order and needs stronger no-alias proof between input,
   output, and kernel pointers. Do not make this the first patch.

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Special-case `conv2d` or `KSIZE == 5` | Would be simple and could match the known benchmark | rejected; violates contest/compliance constraints |
| Start in register allocation | Backend quality is often suspicious for hot loops | rejected for now; focused metrics show 0 spills and no pre/post-RA growth |
| Implement only MIR peepholes | Could clean static branch/move count | insufficient; IR still executes guarded 5x5 loops |
| Full loop interchange first | Could explain the historical 10x gap | deferred; requires alias-safe reduction transformation |
| YIR polyhedral-only solution | SCoP is visible at high level | possible, but current transform hook is TODO and OIR already has useful loop/CFG utilities |

## Change Log

- 2026-06-12: created scoped task from conv2d performance investigation, focused perf run, direct YIR/OIR/MIR emits, polyhedral dump, and historical HY evidence.
- 2026-06-12: implemented generic capped OIR small constant-trip full unroll, added FileCheck coverage, ran focused/full correctness and performance gates, and marked task ready for review with P3/P4 deferred.

## Open Questions

- Should the next optimization be a YIR/OIR guarded-stencil interior split or a smaller OIR boundary-guard peeling transform?
- Is there an existing alias analysis strong enough to prove `In`, `Out`, and `K` disjoint at the call site for more aggressive loop interchange? If not, keep interchange out of the initial task.
- Should the unroll profitability cap remain `trip_count <= 5` and `cloned_insts <= 220`, or be tuned with a same-machine baseline comparison once baseline worktree creation is available?

## Handoff Note

Current state:

- Task is ready for review on `task/conv2d-loop-kernel-optimization`.
- Implemented P1/P2/P5: generic small constant-trip multi-block loop full unroll, focused FileCheck, focused/full correctness, focused/full perf.
- Focused conv2d runtime improved modestly; full 119-case perf passed with GCC geomean `0.91x`, Clang++ geomean `0.99x`.
- Same-machine baseline diff was not run because the environment rejected the `.git` write approval for `git worktree add /tmp/yoolang-baseline-conv2d df20afe`.
- P3/P4 remain deferred: interior/border splitting and deeper affine address recurrence cleanup are still needed for a large conv2d gap closure.

Next action:

- Review the OIR full-unroll profitability limits and decide whether to open a separate guarded-stencil interior/border split task.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-12-conv2d-loop-kernel-optimization.md`
- `docs/tasks/README.md`
- `test/performance/conv2d-1.sy`
- `build/perf-ci/perf-report.md`
- `build/perf-ci/perf-report.json`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`
- `src/pass/oir/OIRLoopTransforms.cpp`
- `src/pass/oir/OIRLoopStrengthReductionPass.cpp`
- `include/oir/OIRScalarOpt.h`
- `src/pass/oir/OIRScalarOptUtils.cpp`
- `src/pass/yir/YIRPolyhedralTransformPass.cpp`
- `test/ir/oir_loop_transforms.sy`
