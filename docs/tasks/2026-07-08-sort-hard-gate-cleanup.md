# Task: Sort Hard Gate Cleanup

Status: blocked
Created: 2026-07-08
Last update: 2026-07-08
Owner: implementation subagent
Branch: task/oir-digit-extraction-cse
Base commit: 46486e9

## Goal

Optimize only the three sort-related performance rows, `test/performance/03_sort1.sy`,
`03_sort2.sy`, and `03_sort3.sy`, until each row reaches at least `1.0x` speedup versus both
GCC `-O3` and Clang `-O3`. Prioritize general late OIR cleanup after loop transforms,
bounded constant-argument or recursion-layer specialization, digit extraction strength reduction
after specialization, and exact-trip loop canonicalization/full unroll for call-free small loops.

## Non-goals

- Do not optimize or validate unrelated performance cases as acceptance criteria for this task.
- Do not special-case filenames, function names, variable names, testcase identity, input sizes,
  input values, argument counts, literal strings, or expected outputs.
- Do not add `sort`, `radixSort`, `getNumPos`, `03_sort`, or bucket-count keyed logic. Any win must
  follow from a general IR pattern and a legality proof.
- Do not weaken FileCheck, stage, e2e, or performance tests.
- Do not speculate `sdiv`, `srem`, calls, loads, or stores onto paths where they did not previously
  execute unless the transform proves the operation is safe on every newly executed path.
- Do not pursue broad backend cleanup before the OIR priorities have been investigated. Backend work
  is secondary and must be justified by focused sort evidence.

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

## Gap Analysis Evidence

Fresh focused measurement from task generation:

```bash
PERF_TEST_DIRS=test/performance/03_sort1.sy,test/performance/03_sort2.sy,test/performance/03_sort3.sy python3 scripts/compare_perf.py
```

Report artifacts:

- `build/perf-ci/perf-report.md`
- `build/perf-ci/perf-report.json`
- `build/perf-ci/test/performance/03_sort*/03_sort*.compiler.s`
- `build/perf-ci/test/performance/03_sort*/03_sort*.gcc.s`
- `build/perf-ci/test/performance/03_sort*/03_sort*.clang.s`

Measured current gap, generated `2026-07-08 01:51:39 UTC`:

| Case | GCC | Clang++ | yoolang | yoolang/GCC | yoolang/Clang | Status |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `test/performance/03_sort1.sy` | 0.0276s | 0.0336s | 0.0343s | 0.804665x | 0.979592x | below both gates |
| `test/performance/03_sort2.sy` | 0.0287s | 0.0344s | 0.0345s | 0.831884x | 0.997101x | below both gates |
| `test/performance/03_sort3.sy` | 0.0320s | 0.0360s | 0.0329s | 0.972644x | 1.094225x | below GCC gate |

Report summary:

- Focused geomean speedup: GCC `0.87x`, Clang++ `1.02x`.
- Faster cases: GCC `0 / 3`, Clang++ `1 / 3`.
- QEMU dynamic instruction count: disabled.
- MIR stage metrics: OK.
- Final per-row MIR metrics are identical for all three rows: 4 functions, 82 basic blocks,
  474 instructions, 72 moves, 41 jumps, 28 branches, 52 loads, 46 stores, 0 spills, 3 stack slots,
  8 calls.

Representative `03_sort1` inspection:

- Generated OIR snapshot command:
  `build/linux/x86_64/release/compiler -O1 --emit-oir test/performance/03_sort1.sy > /tmp/03_sort1.oir`
- Generated MIR snapshots:
  `--emit-mir-stage=pre-ra` and `--emit-mir-stage=final` to `/tmp/03_sort1.pre-ra.mir` and
  `/tmp/03_sort1.final.mir`.
- `radixSort` OIR still contains several inlined counted `getNumPos` chains after loop transforms:
  repeated `sdiv i32 ..., 16` and `srem i32 ..., 16` remain in the hot partition loops.
- The OIR has call-free exact-trip unrolled setup (`.unr` blocks), but the recursive bucket loop is
  still residual and call-containing: bucket `0` is peeled, then `%i.1.loop` starts at `1`, and there
  are still two `call void @radixSort` sites in the function.
- Final MIR for `radixSort` has a 256-byte frame, no spills, repeated signed power-of-two digit
  extraction sequences (`SRAIW`/`SRLIW`/`SRAIW`/`ANDI`) in several inlined paths, and 2 recursive
  `CALL radixSort` instructions.
- Assembly comparison: yoolang assembly is compact at 638 lines versus GCC 2080 and Clang 758, so
  raw code size is not the primary failure. GCC/Clang expose more specialized digit positions and
  recursive bucket calls; yoolang retains looped digit extraction and residual counted call-loop
  overhead.

## Affected Priorities

1. Late OIR cleanup after loop transforms: run or improve `global_value_numbering`,
   `eliminate_dead_loads`, and DCE after loop transforms. Replacements must dominate all uses.
   Cleanup must not speculate division or remainder.
2. General constant-argument specialization and bounded recursion-layer specialization. This must be
   general, have a strict code growth limit, and must not use testcase/function-name hacks.
3. Digit extraction strength reduction after specialization for counted power-of-two division chains,
   especially when a constant recursion layer or constant digit position makes a chain reducible to
   shifts/masks.
4. Exact-trip loop canonicalization and full unroll for call-free small loops. Loops with calls must
   remain conservative unless a bounded specialization/peel is proven profitable and does not raise
   call pressure or frame pressure.
5. Backend cleanup only after the OIR items above are exhausted or a concrete residual ASM idiom is
   shown to block the sort rows.

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 3, including the current blocked sort/radix task and the digit CSE task
- Source/script anchors: max 8 keep=yes source anchors before editing
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- Parser, AST, YIR, runtime, and unrelated MIR register allocation internals.
- Non-sort benchmark sources except to guard against broad regression if a candidate transform
  touches shared behavior.
- Full `build/perf-ci` generated directories beyond `03_sort*` unless performance evidence points
  to a specific comparison need.

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `/home/yoo/.codex/skills/yoolang-optimization/SKILL.md` | full | contest constraints, perf workflow, assembly comparison rules | yes | required by user and optimization skill |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/README.md` | full | active task registry | yes | updated for this task |
| `docs/tasks/TEMPLATE.md` | full | task shape | no | used once to create this record |
| `docs/tasks/2026-07-08-radix-general-opts.md` | `1-120, 160-260` | prior blocked sort/radix attempts and current risk evidence | yes | avoid repeating rejected broad experiments |
| `docs/tasks/2026-07-08-oir-digit-extraction-cse.md` | search/read relevant CSE notes | existing GVN/CSE work and constraints | yes | coordinate rather than overwrite |
| `build/perf-ci/perf-report.md` | full focused report | current measured row gap | yes | generated during task creation |
| `build/perf-ci/perf-report.json` | focused row summary via `jq` | current timings and MIR metrics | yes | row speedups computed from string times |
| `test/performance/03_sort1.sy` | full | benchmark source shape for evidence only | no | do not use as transform trigger |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | full | pass ordering and cleanup windows | yes | primary pipeline anchor |
| `src/pass/oir/OIRInlinePass.cpp` | `540-740, 1030-1170` | current constant specialization limits and recursion exclusions | yes | likely first implementation anchor |
| `src/pass/oir/OIRGVNPass.cpp` | `451-470` plus needed internals | existing GVN entry points | yes | late cleanup priority |
| `src/pass/oir/OIRValueRange.cpp` | `560-640` | current nonnegative pow2 `srem` to `and` rewrite | yes | strength reduction anchor |
| `src/pass/oir/OIRLoopTransforms.cpp` | `1490-1620` plus needed unroll helpers | current multi-block unroll legality and call detection | yes | exact-trip/call-free unroll anchor |
| `/tmp/03_sort1.oir` | generated OIR inspection | evidence only | no | temporary, do not rely on it after resume; regenerate if needed |
| `/tmp/03_sort1.pre-ra.mir`, `/tmp/03_sort1.final.mir` | generated MIR inspection | evidence only | no | temporary, regenerate if needed |

## Branch

Decision: use current branch.

Reason:

```text
The repository is already on task/oir-digit-extraction-cse and the request is a scoped follow-up to
the current sort/radix optimization work. Task generation must not switch branches or implement
production changes. The implementation subagent may create a new branch only if the controller
requests it, but must preserve existing user/subagent work.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
```

Observed during task generation:

```text
git status --short:
 M docs/tasks/README.md
?? docs/tasks/2026-07-08-sort-hard-gate-cleanup.md

git rev-parse --short HEAD:
46486e9

git branch --show-current:
task/oir-digit-extraction-cse
```

## Invariants And Risks

Correctness invariants:

- Any GVN replacement must dominate every use in the final CFG, including after loop cloning,
  peeling, unswitching, and cleanup.
- No transform may introduce a new execution of `sdiv`, `srem`, a load, a store, or a call on a path
  where that operation did not previously execute unless the transform proves the operation is safe
  and non-trapping on all newly exposed paths.
- Signed division/remainder strength reduction must preserve SysY/C signed integer semantics.
  `% 2^k -> and` is legal only with a nonnegative dividend proof; general signed `% +/-2^k` needs
  the bias/mask/sub style already used in lowering or an equivalent proof.
- Constant-argument specialization must remove only arguments proven constant at a callsite and must
  update all call ABI/parameter uses consistently.
- Bounded recursion-layer specialization must be general and must use structural recursion/constant
  argument evidence, not names. It must have a strict per-function and whole-module code growth cap.
- Exact-trip full unroll must require a proven finite trip count, repairable PHIs/exit uses, no side
  exits, and a bounded clone-size limit.
- Loops containing calls are conservative by default. Any call-loop peel/specialization must prove it
  does not increase recursive call count, frame pressure, or harmful code growth.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes,
  input values, argument counts, literal strings, or expected outputs.
- Do not bias `scripts/compare_perf.py`, exclude hard-gate sort rows, or weaken expected outputs.

Risk areas:

- GVN across transformed CFGs can produce use-before-def or non-dominating replacements if dominance
  is stale or block-local availability is over-approximated.
- Late cleanup after loop transforms can accidentally remove needed memory operations if alias or
  MemorySSA facts are stale.
- Constant recursion specialization can grow code, frame size, and callee-save pressure enough to
  lose the focused row even if instruction count drops.
- Digit strength reduction can be illegal for negative values without range proof.
- Full unroll of multi-block loops can break PHIs or side exits; call-loop unrolls previously
  increased frame/callee-save pressure in the blocked radix task.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P0 | Reproduce the focused sort gap and inspect OIR/MIR/ASM before editing | task file, `build/perf-ci/perf-report.md`, `build/perf-ci/perf-report.json`, temporary `/tmp/03_sort1.*` | `PERF_TEST_DIRS=test/performance/03_sort1.sy,test/performance/03_sort2.sy,test/performance/03_sort3.sy python3 scripts/compare_perf.py`; emit OIR/MIR for `03_sort1` | complete | Task-generation evidence captured above; no production code modified. |
| P1 | Add or extend generic FileCheck coverage for late post-loop-transform cleanup, including dominance-safe GVN/load cleanup and a negative case for non-dominating/speculative division | `test/ir/oir_cfg_gvn.sy`, `test/ir/oir_digit_extraction_cse.sy`, or focused new test | `python3 scripts/run_tests.py --suite filecheck --filter oir_cfg_gvn --jobs 1` and focused OIR stage | complete | Added `no_nondom_div_replacement` to existing `oir_cfg_gvn.sy`; focused FileCheck passed. |
| P2 | Run/improve late OIR cleanup after loop transforms: `global_value_numbering`, `eliminate_dead_loads`, DCE/ADCE, and CFG cleanup in the right order | `src/pass/oir/OIROptimizationPipelinePass.cpp`, `src/pass/oir/OIRGVNPass.cpp`, maybe `include/oir/OIRScalarOpt.h` | P1 FileCheck; `python3 scripts/run_tests.py --suite stage --stage oir --filter 03_sort --jobs 1 --o1` | complete | Added final SCCP/VRP/GVN/load-store cleanup after late memory promotion and before final DCE; focused correctness passed, but hard gate still fails. |
| P3 | Add generic tests for constant-argument specialization and bounded recursion-layer specialization with strict code growth limits | `test/ir/oir_recursive_const_specialization.sy` | focused FileCheck plus OIR stage | complete | Added direct-recursive constant-layer coverage; avoids recursive-inline interference and checks constant-parameter branch removal. |
| P4 | Implement or tighten general specialization so constant recursion layers expose fixed digit positions without testcase/function-name logic | `src/pass/oir/OIRInlinePass.cpp` | P3 tests; focused `03_sort` OIR/MIR/ASM/e2e | complete | Added masked direct-recursive constant-argument specialization. Recursive layers specialize only constant args that do not feed PHIs and have structural arithmetic/control use. Budget is bounded by existing policy `max_specializations_per_function` plus static-instruction caps. |
| P5 | Add generic tests for counted power-of-two division-chain strength reduction after specialization | `test/ir/oir_div_chain_simplify.sy` | FileCheck; OIR stage | complete | Covers positive nested signed-divisor composition and negative-divisor non-composition. |
| P6 | Implement digit extraction strength reduction for counted `/ 2^k` then `% 2^k` chains when constants/ranges prove legality | `src/pass/oir/OIRLocalSimplify.cpp` | P5 tests; focused `03_sort` OIR/MIR/ASM/e2e | complete | Added general nested positive signed-division composition, e.g. `(x / 16) / 16 -> x / 256`, without speculating division or changing signed semantics. |
| P7 | Add or repair exact-trip loop canonicalization/full unroll for call-free small loops only; keep call-containing loops conservative | `src/pass/oir/OIRLoopTransforms.cpp`, `test/ir/oir_loop_transforms.sy` | `python3 scripts/run_tests.py --suite filecheck --filter oir_loop_transforms --jobs 1`; focused stage/e2e | skipped | Existing call-free exact-trip unroll coverage passes in full FileCheck. Remaining hot residual loops contain recursive calls; current metrics show call/code pressure is the limiting risk. |
| P8 | Only if OIR priorities leave a concrete residual ASM idiom, add a narrow backend cleanup with FileCheck | MIR/ASM files as evidence dictates | backend FileCheck; focused `03_sort` perf | skipped | No narrow backend-only idiom was isolated that can close the remaining GCC gap without broader call/code-growth work. |
| P9 | Final focused acceptance gate and report inspection | task file, perf artifacts | focused `03_sort` perf command, plus `build/perf-ci/perf-report.md` and `.json` inspection | blocked | Refreshed focused report generated 2026-07-08 02:30:45 UTC: all rows beat Clang but remain below GCC, so strict acceptance is not met. |
| R1 | Repair attempt: reduce direct-recursive specialization/code pressure by lowering the effective recursive layer cap | `src/pass/oir/OIRInlinePass.cpp` | `xmake`; focused sort perf | reverted | Lowering the cap to 3 reduced final MIR to 10 functions, 1163 instructions, and 14 calls, but focused GCC speedups were only 0.9329/0.9579/0.9552. Reverted because it worsened the blocker. |
| R2 | Repair attempt: extend nested positive signed-division cleanup for quotient-proven-zero chains | `src/pass/oir/OIRLocalSimplify.cpp`, `test/ir/oir_div_chain_simplify.sy` | focused FileCheck/stage/e2e/perf | reverted | Correct in isolation and reduced final MIR to 1340 instructions, but focused GCC speedups remained below gate and were unstable/worse on `03_sort3` (observed 0.9404/0.9892/0.9106). Reverted. |
| R3 | Repair attempt: add broad dead-function pruning after clone cleanup | `include/oir/OIR.h`, `src/oir/OIR.cpp`, `src/pass/oir/OIRDAEPass.cpp`, pipeline, focused test | focused FileCheck/perf | reverted | Reduced final MIR to 6 functions and 1281 instructions but broke existing OIR FileCheck expectations by removing inlined helper definitions; hard gate also still failed (0.9724/0.9648/0.9542). Reverted. |
| R4 | Repair attempt: enable existing final-iteration peel for void self-recursive call loops | `src/pass/oir/OIRLoopTransforms.cpp` | focused stage/e2e/perf; OIR inspection | reverted | The transform did not match the sort recursion loop; final MIR metrics stayed 12 functions, 1416 instructions, 16 calls, and hard gate still failed. Reverted to avoid unrelated loop behavior changes. |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | Refreshed final build passed after reverting unsuccessful repair experiments. |
| Focused GVN/CSE FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_cfg_gvn --jobs 1` | if GVN touched | PASS | 1 passed; new non-dominating division negative case included. |
| Focused digit CSE FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_digit_extraction_cse --jobs 1` | if CSE/digit cleanup touched | PASS | 1 passed after P2. |
| Focused recursive specialization FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_recursive_const_specialization --jobs 1` | if specialization touched | PASS | 1 passed. |
| Focused division-chain FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_div_chain_simplify --jobs 1` | if local div simplification touched | PASS | 1 passed. |
| Focused loop transform FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_loop_transforms --jobs 1` | if loop transforms touched | PASS | 1 passed after reverting the final-peel experiment. |
| Focused OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter 03_sort --jobs 1 --o1` | yes | PASS | 3 passed in repair verification. |
| Focused MIR/ASM stage | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter 03_sort --jobs 1 --o1` | yes | PASS | 6 passed in repair verification. |
| Focused E2E | `python3 scripts/run_tests.py --suite e2e --filter 03_sort --jobs 1 --o1` | yes | PASS | 3 passed in repair verification. |
| Full FileCheck | `python3 scripts/run_tests.py --suite filecheck --jobs 1` | yes | PASS | 32 passed. |
| Full optimized stage/e2e | `python3 scripts/run_tests.py --suite stage --suite e2e --jobs 1 --o1` | yes | PASS | 1393 passed, 1 skipped, 0 failed. |
| Full all-suite optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | Recommended if patch touches shared specialization/GVN behavior. |
| Focused sort performance | `PERF_TEST_DIRS=test/performance/03_sort1.sy,test/performance/03_sort2.sy,test/performance/03_sort3.sy python3 scripts/compare_perf.py` | yes | FAIL final hard gate | Report generated 2026-07-08 02:30:45 UTC: geomean GCC 0.95x / Clang 1.13x; rows 0.9239/1.1349, 0.9441/1.1399, 0.9692/1.1199 vs GCC/Clang. |
| Optional broad perf guard | `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` | optional guard | NOT_RUN | Use to catch severe shared regressions; not the acceptance gate. |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Continue broad radix-general optimization task | Already contains useful evidence | rejected as the task target; this task is sort-only with the user's new priority order |
| Backend-only cleanup | Might remove a few final instructions | deferred; OIR still has looped digit extraction and residual specialization gaps |
| Special-case the three sort rows | Would directly satisfy measurements | rejected; violates contest/compliance constraints |
| Fully unroll recursive call loops | Clang emits many direct recursive calls | rejected as default; previous broad task saw frame/callee-save pressure and worse timing |
| Bounded recursion-layer specialization | Exposes constant digit positions without name hacks | preferred if guarded by structural recursion proof and strict growth limits |

## Change Log

- 2026-07-08: created scoped task file from the user request; captured fresh focused sort perf,
  representative OIR/MIR/ASM evidence, branch state, risks, patch queue, and verification gates.
- 2026-07-08: implementation subagent started on existing `task/oir-digit-extraction-cse`
  branch; observed uncommitted task doc/README changes and preserved them.
- 2026-07-08: P1 added generic GVN FileCheck coverage for a non-dominating division replacement
  hazard; focused `oir_cfg_gvn` FileCheck passed.
- 2026-07-08: P2 added final SCCP/VRP/GVN/load-store cleanup after late memory promotion.
  Focused FileCheck/stage/e2e passed, but focused sort perf still failed the strict hard gate.
- 2026-07-08: P3/P4 added generic masked direct-recursive constant-argument specialization with
  focused FileCheck coverage. A 10-layer experiment caused excessive code growth and worse perf;
  final cap respects the existing balanced per-function specialization budget.
- 2026-07-08: P5/P6 added nested positive signed-division composition and FileCheck coverage.
  This improved the focused sort rows materially versus baseline, but the strict GCC gate still
  failed.
- 2026-07-08: final verification completed: `xmake`, focused FileChecks, focused sort OIR/MIR/ASM
  and e2e, full FileCheck, and full optimized stage/e2e all passed. Final focused perf remains
  below GCC on all three rows; task marked blocked rather than weakening acceptance.
- 2026-07-08: repair subagent investigated four scoped generic repairs and reverted all unsuccessful
  experiments: tighter recursive specialization cap, oversized nested-division zero fold, broad
  dead-function pruning, and enabling final-iteration peel. Focused correctness gates still pass,
  but the refreshed focused perf report remains below the GCC hard gate.

## Open Questions

- Should the implementation subagent continue on `task/oir-digit-extraction-cse` or create a new
  branch from `46486e9` before editing? Task generation did not switch branches.
- What exact code growth cap should bounded recursion-layer specialization use? Suggested starting
  constraint: reuse or tighten the existing cost model caps, with an additional per-callee layer cap
  and a module-level cloned-instruction cap.
- If the focused sort rows pass but broad `test/performance` regresses, should review block on broad
  regression even though the user scoped acceptance to sort-only cases? Default recommendation:
  treat severe correctness or large shared perf regressions as blockers, but do not require broad
  geomean `>= 1.0x` for acceptance.

## Handoff Note

Current state:

- Implementation is blocked on the strict GCC hard gate. The final patch set improves the focused
  rows and beats Clang, but refreshed final focused sort speedups versus GCC are only `0.9239x`,
  `0.9441x`, and `0.9692x`.
- Fresh focused perf evidence is in `build/perf-ci/perf-report.md` and
  `build/perf-ci/perf-report.json`.
- Final focused MIR metrics per row are 12 functions, 138 basic blocks, 1416 final instructions,
  146 moves, 73 jumps, 49 branches, 211 loads, 212 stores, 0 spills, 15 stack slots, and 16 calls.
- Repair attempts did not leave source changes beyond the existing P1-P6 implementation. Lowering
  recursive specialization reduced static code but hurt runtime; broad dead-function pruning broke
  existing FileCheck assumptions; the existing final-iteration peel did not match the sort recursion
  loop; and the quotient-zero division fold did not close the gate.

Next action:

- A follow-up should inspect why the residual bucket-recursion loop does not match the existing
  final-iteration peel/tail-recursion cleanup, or design a clone-pruning facility that preserves the
  repository's OIR FileCheck contract. Do not claim acceptance until all three focused rows are
  `>= 1.0x` versus both GCC and Clang in a full focused `compare_perf.py` run.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-08-sort-hard-gate-cleanup.md`
- `/home/yoo/.codex/skills/yoolang-optimization/SKILL.md`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`
- `src/pass/oir/OIRGVNPass.cpp`
- `src/pass/oir/OIRInlinePass.cpp`
- `src/pass/oir/OIRValueRange.cpp`
- `src/pass/oir/OIRLoopTransforms.cpp`
- `build/perf-ci/perf-report.md`
- `build/perf-ci/perf-report.json`
