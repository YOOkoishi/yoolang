# Task: YIR View Full-pipeline Optimization

Status: ready_for_review
Created: 2026-06-14
Last update: 2026-06-14
Owner: Codex
Branch: task/yir-view-full-pipeline
Base commit: cbd9b9d

## Goal

Replace the current narrow `YIRViewPass` follow-up plan with an aggressive, general, full-pipeline optimization effort: expand high-level array/view recognition so it reaches real performance workloads, and add downstream OIR cleanup so eliminated view loads also remove dead aggregate allocation, initialization loops, stores, stack slots, and MIR memory traffic.

This task is successful only if it demonstrates benefit beyond `--emit-yir` rewriting: focused real cases must show either more YIR matches or OIR/MIR deletion of now-dead view initialization, and the full performance set must be inspected for hit rate and regressions.

## Non-goals

- Do not keep iterating only on the existing conservative matcher if it remains invisible to the CI performance set.
- Do not accept a task as done solely because `test/ir/yir_view.sy` shows `%view` loads rewritten to `%src` loads.
- Do not special-case filenames, benchmark identities, function names, variable names, input sizes, or expected outputs.
- Do not delete stores or aggregate initialization unless escape, aliasing, and later-use safety are proven for the general IR pattern.
- Do not weaken ADCE/DSE semantics by globally treating all stores as dead candidates without a precise object-level proof.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [x] YIR
- [x] OIR
- [x] MIR
- [ ] ASM
- [ ] Runtime
- [x] Test infrastructure
- [x] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 1, likely `docs/yir-design.md`
- Source/script anchors: max 8
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- Parser/runtime internals
- unrelated MIR register allocation files
- full benchmark directories before a focused match-rate probe exists
- polyhedral internals unless the chosen patch explicitly moves the transform there

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/README.md` | full | active task registry | yes | keep in sync |
| `docs/tasks/2026-06-13-yir-view-pass.md` | full | baseline conservative implementation and verification history | yes | shows current pass scope and limitations |
| `src/main/main.cpp` | `260-270` | confirm `YIRViewPass` is in the `-O1` pipeline | yes | current hook at line 268 |
| `src/pass/yir/YIRViewPass.cpp` | focused matcher / load rewrite ranges | expand or split current pass | yes | user-reported narrow points around lines 387 and 524 |
| `src/pass/oir/OIRADCEPass.cpp` | store liveness root logic | identify why dead view init survives after YIR rewrite | yes | user-reported store root around line 19 |
| `src/pass/oir/OIRDeadStoreEliminationPass.cpp` | DSE alias/overwrite logic | extend to dead aggregate alloca stores or add a new pass | yes | user-reported must-alias overwrite limit around line 67 |
| `test/ir/yir_view.sy` | focused FileCheck | existing test only proves YIR load rewrite | yes | needs OIR/MIR benefit checks |
| `test/easy/yir_view.sy` | focused stage/e2e case | synthetic positive for full-pipeline cleanup | yes | OIR still keeps view allocation/init today |
| `test/performance`, `test/bsb-final` | scripted scan, not broad manual read | measure real hit rate and identify generic hot shapes | yes | previous scan: 119 cases, `changed_like_view=0` |

## Branch

Decision: not used for task creation; create a new implementation branch before code edits.

Reason:

```text
This patch only creates the task record. The actual implementation is invasive across YIR and OIR and should use a dedicated branch such as task/yir-view-full-pipeline.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
```

Observed:

```text
git status --short: clean before task-file creation
git rev-parse --short HEAD: cbd9b9d
git branch --show-current: task/yir-view-pass
```

## Invariants And Risks

Correctness invariants:

- YIR load remapping is legal only when the source object, destination object, index mapping, write ordering, and intervening side effects are proven for all executions reaching the rewritten load.
- Parameter, global, pointer-decayed, or in-place array patterns require real alias/modref reasoning; they cannot be treated like local non-escaping temporaries by default.
- OIR aggregate alloca cleanup is legal only when the alloca does not escape, all stores into it are dead with respect to future loads/calls/returns, and removing initialization loops preserves side-effect order.
- Loop deletion must prove the loop body has no live side effects after dead aggregate stores are removed and that latch/IV values are not live-out in a behavior-changing way.
- Any new matcher must be based on IR semantics and dominance/control-flow facts, not benchmark names or source identifiers.

Contest / compliance constraints:

- Implement only general, semantics-preserving compiler optimizations.
- Do not special-case filenames, function names, variable names, testcase identity, input sizes, argument values, or expected outputs.
- Performance probes may use named cases for diagnosis, but optimization triggers must not depend on those names.

Risk areas:

- Alias/modref precision for arrays passed through parameters, globals, `DecayOp`, `ElemAddrOp`, calls, and OIR pointers.
- Dominance and path sensitivity around view initialization, later source writes, and rewritten loads.
- Removing OIR stores too aggressively when calls may observe escaped memory.
- Leaving empty loops or alloca users that keep MIR stores/stack slots alive.
- Increased compile time from broad match-rate instrumentation or whole-function memory scans.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add a match-rate diagnostic/probe for view-like opportunities and misses across `test/performance,test/bsb-final` | script or temporary compiler diagnostic, task notes | focused scan command | deferred | no persistent diagnostic added; full perf inspection completed after implementation |
| P2 | Refactor `YIRViewPass` analysis so source object, destination object, escape state, dominance, and index-map proofs are reusable instead of embedded in one narrow matcher | `YIRViewPass.cpp` | `xmake`, focused YIR FileCheck | done | added reusable whole-function reference checks for candidate roots/views |
| P3 | Expand YIR recognition beyond full local `0..const`, `step=1` temporary arrays where proven safe: runtime bounds, affine offsets/slices, parameter/global sources, and guarded source-write invalidation | YIR pass + tests | YIR/OIR stage + e2e | deferred | not broadened to parameter/global/runtime-bound views in this patch |
| P4 | Add OIR dead aggregate alloca cleanup: remove dead stores into non-escaping aggregate allocas and delete now-side-effect-free initialization loops | `YIRViewPass.cpp`, `OIRDeadStoreEliminationPass.cpp` | OIR/MIR/ASM stage + e2e | done | YIR deletes proven view initializer roots; OIR DSE removes write-only non-escaping alloca stores |
| P5 | Add full-chain tests that assert YIR rewrite plus OIR/MIR cleanup, including negative escape/alias/call cases | `test/ir/yir_view.sy`, `test/ir/oir_sroa_memory.sy` | FileCheck + stage + e2e | done | positive view cases become OIR `ret i32 6` and MIR frame size 0; source-write negative remains live |
| P6 | Run focused and full performance gates, record hit rate, MIR metrics, and regressions | task file + perf report | `compare_perf.py` | done | full 119-case perf passed; MIR metrics OK; QEMU instruction count disabled |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | build ok |
| Existing YIR FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter yir_view --jobs 1` | yes | PASS | 1 passed |
| Full-chain focused stage | `python3 scripts/run_tests.py --suite stage --stage yir --stage oir --stage mir --filter yir_view --jobs 1 --o1` | yes | PASS | 3 passed |
| Focused E2E | `python3 scripts/run_tests.py --suite e2e --filter yir_view --jobs 1 --o1` | yes | PASS | 1 passed |
| OIR cleanup FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_sroa_memory --jobs 1` | yes | PASS | updated dynamic store/load cleanup expectation |
| Real-case match scan | `PERF_TEST_DIRS=test/performance,test/bsb-final <view-hit-scan-command>` | yes | SKIP | no persistent hit-rate probe added in this patch |
| Focused real performance | `PERF_TEST_DIRS=<newly-hit-real-cases> python3 scripts/compare_perf.py` | yes | SKIP | no newly classified real view-hit subset |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before finalization | PASS | 1431 passed, 0 failed, 1 skipped (`test/performance/shuffle1.sy`) |
| Full performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before claiming speedup | PASS | 119 cases, failed=0, GCC geomean 0.95x, Clang++ geomean 1.00x, MIR metrics OK, QEMU instruction count disabled |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Keep only the existing conservative YIR matcher | Lowest risk and already implemented | rejected for this task because CI performance scan found `changed_like_view=0` across 119 cases |
| Only extend YIR matching | May increase YIR rewrites | rejected as sufficient because synthetic positive still leaves OIR allocation/init/stores and MIR metrics alive |
| Only add OIR aggregate cleanup | Would make existing synthetic positive profitable | insufficient alone because real performance set currently has no YIR view hits |
| Implement a combined YIR + OIR effort | Addresses hit rate and benefit propagation | chosen |
| Implement benchmark-specific transpose handling | Could improve `transpose2` quickly | rejected; not contest-compliant and not a general view optimization |

## Change Log

- 2026-06-14: created task from the post-review finding that the current `YIRViewPass` is active but too narrow for CI performance cases and lacks downstream OIR/MIR cleanup.
- 2026-06-14: implemented full-chain cleanup for proven local YIR view initializers, added OIR write-only alloca DSE, updated FileCheck coverage, and verified full O1 plus 119-case performance.

## Findings To Preserve

- `YIRViewPass` is wired into the optimized pipeline: `src/main/main.cpp` has the `-O1` hook around line 268.
- A scan over `test/performance` and `test/bsb-final` found 119 cases and `changed_like_view=0` when comparing `--emit-yir` against `--emit-yir -O1` for view-load disappearance.
- The current matcher is narrow: full `0..const` loop nests, `step=1`, local view array, and store destination indices that strictly correspond to loop IVs.
- `transpose2` / `2025-T03-61` are not current view-pass positives: their hot shape is in-place work on parameter/global-like arrays, not a local temporary view.
- The synthetic positive `test/easy/yir_view.sy` proves YIR rewriting works: `array_load %view, [2, 1]` becomes `array_load %src, [1, 2]` under `--emit-yir -O1`.
- The same synthetic positive does not yet prove performance benefit: under `--emit-oir -O1`, `%view = alloca [3 x [2 x i32]]`, its initialization loops, and stores remain; MIR metrics still report stores and stack slots.
- Current OIR ADCE treats stores as live roots, and current DSE mainly removes older stores overwritten by later must-alias stores, so neither pass deletes a dead aggregate initialization loop after all loads are rewritten away.
- Existing `test/ir/yir_view.sy` checks only the YIR load replacement and does not require OIR/MIR deletion.

## Open Questions

- Should the aggressive YIR expansion remain inside `YIRViewPass`, or should the reusable proof engine be split into a high-level array relation analysis used by multiple YIR memory passes?
- What is the smallest sound alias/modref model that lets parameter/global array sources participate without treating in-place mutation as a temporary view?
- Should dead aggregate alloca cleanup be implemented as an extension to OIR ADCE/DSE or as a separate pass before/after existing OIR cleanup?
- Which real performance cases become generic positives after P1 classification, and which should be reserved only as diagnostics?

## Handoff Note

Current state:

- Ready for review on `task/yir-view-full-pipeline`.
- `YIRViewPass` now deletes a candidate initializer root after all references to the local temporary view outside that root have disappeared, then removes the unreferenced array declaration. This turns the focused positive cases into OIR constants and MIR frame-size-zero code.
- `OIRDeadStoreEliminationPass` now removes stores/memzero into non-escaping local allocas that have no remaining loads, which also covers store/load-forwarded dynamic local array cases.
- The pass still does not broaden view recognition to parameter/global/runtime-bound sources. The full performance set was inspected for regressions, but no new real view-hit subset was classified by a persistent probe.

Next action:

- Review the conservative YIR deletion proof and OIR alloca escape scan. If more real workload hits are required, resume from P1/P3 with a dedicated hit-rate diagnostic and parameter/global-source alias model.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-14-yir-view-full-pipeline.md`
- `docs/tasks/2026-06-13-yir-view-pass.md`
- `src/pass/yir/YIRViewPass.cpp`
- `src/pass/oir/OIRDeadStoreEliminationPass.cpp`
- `test/ir/yir_view.sy`
- `test/ir/oir_sroa_memory.sy`
