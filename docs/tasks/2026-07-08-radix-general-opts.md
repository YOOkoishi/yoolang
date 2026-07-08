# Task: Radix General Optimizations

Status: blocked
Created: 2026-07-08
Last update: 2026-07-08
Owner: implementation subagent
Branch: task/oir-digit-extraction-cse
Base commit: 45759a2

## Goal

Implement a set of general, semantics-preserving optimizer improvements that remove the remaining
hot-path waste observed in fixed-bucket recursive partitioning workloads: dead zero initialization
of stack arrays, loop-invariant condition checks inside hot loops, small exact-trip-count bucket
loops, incomplete reuse of repeated scalar/load/digit computations, and simple backend arithmetic
idioms exposed by those shapes. The final hard gate is the three `03_sort` performance rows: each
row must reach at least `1.0x` GCC `-O3` and at least `1.0x` Clang `-O3` speedup while preserving
correctness. In the current `perf-report.json` schema there are no explicit speedup fields, so the
gate is computed as `gcc_time / compiler_time` and `clang_time / compiler_time` from the row's
`gcc`, `clang`, and `compiler` fields unless the script later writes explicit
`gcc_o3_speedup`/`clang_o3_speedup` fields.

## Non-goals

- Do not special-case `sort`, `radixSort`, helper names, variable names such as `head`/`tail`/`cnt`,
  filenames, testcase identities, input sizes, input values, argument counts, or expected outputs.
- Do not hardcode fixed bucket count `16` as a benchmark signature. Any unrolling or small-loop
  transform must be driven by proven exact trip count, cost model limits, and semantic safety.
- Do not speculate `sdiv`, `srem`, calls, loads, or memory writes onto paths where they did not
  previously execute unless legality is proven for every newly executed path.
- Do not weaken FileCheck, stage, e2e, or performance tests; do not bias `compare_perf.py` inputs.
- Do not claim final performance success unless all three `03_sort` performance rows individually
  satisfy both GCC and Clang speedup thresholds. Full broad perf remains useful regression evidence
  and may be reported as an observation, but broad geomean is no longer the hard termination gate.
- Do not replace the already completed `OIR Digit Extraction CSE` work; coordinate with it and only
  extend or repair it if evidence shows a remaining generic CSE/load-reuse gap.

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

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 2, this task and `docs/tasks/2026-07-08-oir-digit-extraction-cse.md`
- Source/script anchors: max 8 keep=yes anchors before editing
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- Parser, AST, YIR, runtime, and unrelated MIR register allocation or scheduler internals.
- Benchmark source files beyond the focused performance reproducer set used for measurement.
- Generated `build/perf-ci` per-case assembly artifacts except for cases whose focused sort perf row
  regresses or remains below GCC/Clang after the hard gate.
- Any task files unrelated to OIR scalar optimization, loop transforms, or performance audit.

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `/home/yoo/.codex/skills/yoolang-optimization/SKILL.md` | full | contest compliance and perf workflow | yes | required for optimization legality and perf gates |
| `docs/tasks/README.md` | full | active task registry | yes | updated for this task |
| `docs/tasks/TEMPLATE.md` | full | task shape | no | used once to create this record |
| `docs/tasks/2026-07-08-oir-digit-extraction-cse.md` | full | existing same-day CSE task and current dirty worktree context | yes | coordinate CSE patch and avoid duplicate implementation |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | full | OIR pass ordering around SROA, mem2reg, GVN, DSE, unswitch, and cleanup cadence | yes | use to place new cleanup/unroll windows |
| `include/oir/OIRScalarOpt.h` | full | shared OIR scalar optimization API and stats fields | yes | add declarations/stats only if needed |
| `src/pass/oir/OIRDeadStoreEliminationPass.cpp` | full | existing alloca write-only DSE and local/dom memory DSE behavior | yes | primary anchor for dead initializer removal |
| `src/pass/oir/OIRSROAPass.cpp` | full | existing aggregate scalar replacement and aggregate zero-store handling | yes | primary anchor for alloca/init splitting if DSE alone is insufficient |
| `src/pass/oir/OIRLoopTransforms.cpp` | `1-240, 820-1080, 1800-1890` | existing clone helpers, loop unswitch legality, and unswitch driver | yes | extend unswitch and/or add OIR exact small-loop unroll here if appropriate |
| `src/pass/oir/OIRGVNPass.cpp` | full | existing value numbering and the current digit-extraction CSE changes | yes | read after the CSE task to identify remaining generic reuse gaps |
| `src/oir/OIRAnalysis.cpp` | `840-930, 1030-1195, 1340-1455, 1670-1905` | SCEV trip counts, alias/modref, and MemorySSA clobber behavior | yes | needed for loop exact-count and memory legality |
| `include/oir/OIRAnalysis.h` | `68-166, 191-212, 264-290` | LoopInfo/SCEV, alias, modref, MemorySSA API surface | yes | declaration anchor for any analysis extension |
| `test/ir/oir_sroa_memory.sy` | full | existing SROA/memory FileCheck style | yes | candidate focused test for dead aggregate initialization |
| `test/ir/oir_loop_transforms.sy` | full | existing OIR loop-transform FileCheck style | yes | candidate focused test for unswitch and small-loop unroll |
| `test/ir/oir_digit_extraction_cse.sy` | full | current focused CSE test from the existing task | yes | extend only for remaining generic CSE miss |
| `rg -n "unswitch\|unroll\|constant_trip_count\|eliminate_dead_stores\|scalar_replacement_of_aggregates\|pre_inline_load_call_cse" src include test/ir` | query | locate candidate anchors | no | broad output discarded after selecting keep files |

## Branch

Decision: continue from the current dirty `task/oir-digit-extraction-cse` worktree; do not create or
switch a new branch in the task-generation step.

Reason:

```text
This is a performance-sensitive compiler optimization task with multiple implementation patches.
The user explicitly decided that implementation continues from the current dirty
task/oir-digit-extraction-cse worktree. The implementation subagent must preserve and coordinate the
existing CSE changes and must not revert, overwrite, or discard them.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
# no checkout in task-generation; implementation continues on task/oir-digit-extraction-cse
```

Observed during task generation:

```text
git status --short:
 M docs/tasks/README.md
 M include/oir/OIRScalarOpt.h
 M src/oir/OIRAnalysis.cpp
 M src/pass/oir/OIRGVNPass.cpp
 M src/pass/oir/OIROptimizationPipelinePass.cpp
?? docs/tasks/2026-07-08-oir-digit-extraction-cse.md
?? test/ir/oir_digit_extraction_cse.sy

git rev-parse --short HEAD: 45759a2
git branch --show-current: task/oir-digit-extraction-cse
```

## Invariants And Risks

Correctness invariants:

- All transforms must be justified by SSA dominance, control-flow equivalence, exact loop-trip
  proof, memory alias/modref facts, and/or existing verifier-checked IR invariants.
- A removed store or memzero must be proven dead on every path to any read, escape, call that may
  read/clobber it, return, or externally visible memory use.
- SROA or alloca-init DSE must preserve aggregate element values, address identity constraints for
  non-escaped stack objects, volatile-equivalent behavior if ever represented, and all pointer uses.
- Loop unswitch may only hoist or materialize loop-invariant conditions and operands that are safe to
  execute in the preheader. Division/remainder and memory/call operations remain non-speculatable
  unless a stronger proof is added and tested.
- Small-loop unrolling must require a proven finite exact trip count, a structurally safe body,
  bounded code growth, correct phi/exit value repair, and no changes to recursion semantics.
- CSE/reuse must replace only equivalent computations whose replacement value dominates the rewritten
  use or whose materialization is control-flow safe and non-speculative.
- The optimized `-O1` pipeline must keep OIR, MIR, ASM, and e2e correctness for the full test suite.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or
  expected outputs.
- Do not hardcode known testcase results or replace real computation with expected output.
- Do not exploit undefined behavior or assumptions that make other valid inputs incorrect.
- Do not weaken tests, exclude cases, alter expected outputs, or bias performance scripts.

Risk areas:

- Removing array zero initialization is easy to get wrong when an alloca escapes, is partially read,
  is read through a GEP slice, or is passed to a call. Prefer conservative no-change over unsound DSE.
- Unswitching can duplicate loops and raise register pressure or code size. Use the cost model and
  inspect broad perf/MIR stage metrics before accepting.
- Unrolling loops that contain recursive calls can improve branch overhead but also increase code
  size and instruction-cache pressure. Gate by exact trip count and measured full perf.
- Existing `OIR Digit Extraction CSE` changes are already dirty in this worktree. The implementation
  subagent must not revert or overwrite them unless the controller explicitly rolls that task into
  this one.
- Reaching the three `03_sort` row speedups `>= 1.0x` against both GCC and Clang is the hard
  acceptance gate. If correctness passes but any row is below either compiler, keep the task open or
  mark it blocked with row-level perf evidence. Broad geomean is non-blocking observation only.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P0 | Capture baseline evidence and confirm remaining generic misses after the existing CSE task | task file, `build/perf-ci/perf-report.md`, `build/perf-ci/perf-report.json`, optional emitted OIR/MIR for focused cases | `xmake`; focused `--emit-oir` / `--emit-mir-stage=pre-ra` commands as needed; `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` for baseline only | complete | Baseline on current dirty CSE worktree: `test/performance` 60/60, GCC 0.96032, Clang 1.00686. |
| P1 | Add focused generic tests for dead aggregate/alloca initialization where later stores overwrite every read or the object is never read | `test/ir/oir_sroa_memory.sy` or new focused `test/ir/oir_alloca_init_dse.sy` | `python3 scripts/run_tests.py --suite filecheck --filter oir_alloca_init_dse --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_alloca_init_dse --jobs 1 --o1` | complete | Added `guarded_zero_init` coverage in `oir_sroa_memory.sy` for sinking local aggregate zero init past an early return guard. |
| P2 | Implement conservative alloca/init DSE or SROA enhancement for overwritten or write-only stack aggregate zero initialization | `src/pass/oir/OIRDeadStoreEliminationPass.cpp`, maybe `src/pass/oir/OIRSROAPass.cpp`, maybe `include/oir/OIRScalarOpt.h` | P1 tests; `python3 scripts/run_tests.py --suite filecheck --filter oir_sroa_memory --jobs 1`; focused OIR stage | complete | Extended aggregate-zero store sinking beyond the first entry move: non-escaped stack aggregate zero stores can now keep sinking through guard merge blocks to a proven dominator of all later alloca uses, still avoiding loop headers with backedges. |
| P3 | Add focused generic tests for loop-invariant condition unswitching of an inner hot loop without speculating unsafe division/call work | `test/ir/oir_loop_transforms.sy` or new focused `test/ir/oir_loop_unswitch_invariant.sy` | `python3 scripts/run_tests.py --suite filecheck --filter oir_loop_unswitch_invariant --jobs 1`; focused OIR/MIR stages | complete | Updated `oir_loop_transforms.sy` to assert an unswitched clone and cost-model `LoopInvariantBranch` bypass. |
| P4 | Extend OIR loop unswitch legality/profitability only for general loop-invariant conditions that current analysis proves safe | `src/pass/oir/OIRLoopTransforms.cpp`, maybe `src/oir/OIRAnalysis.cpp`, maybe `include/oir/OIRAnalysis.h` | P3 tests; `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_loop_unswitch --jobs 1 --o1` | complete | Existing unswitch legality already blocks unsafe materialization; changed profitability handling to bypass for proven loop-invariant branches. Focused and broad correctness passed. |
| P5 | Add focused generic tests for exact small constant-trip-count loop unrolling, including loops with calls when structurally safe | `test/ir/oir_loop_transforms.sy` or new focused `test/ir/oir_small_loop_unroll.sy` | `python3 scripts/run_tests.py --suite filecheck --filter oir_loop_transforms --jobs 1`; focused OIR/MIR/ASM stages | complete | Added focused coverage in `oir_loop_transforms.sy` for an exact constant-trip loop that must lower to `.unr` blocks and emit `ExactSmallTripCount` in the cost-model report. |
| P6 | Implement OIR exact small-loop unroll or adjust existing loop transform placement/costing to unroll safe fixed-trip loops | `src/pass/oir/OIRLoopTransforms.cpp`, maybe `include/oir/OIRScalarOpt.h`, maybe `src/pass/oir/OIROptimizationPipelinePass.cpp` | P5 tests; `python3 scripts/run_tests.py --suite stage --stage oir --filter 03_sort --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e --filter 03_sort --jobs 1 --o1` | complete | Repaired the single-block exact-trip unroll prototype: latch-aware trip counting is now preferred for rotated do-while shapes, fixing the prior one-iteration undercount that broke `03_sort*`; added a narrow `ExactSmallTripCount` cost-model bypass and exit-use repairability guard. |
| P7 | Finish remaining generic repeated load/digit CSE gaps after P2/P4/P6, coordinating with the existing CSE task | `src/pass/oir/OIRGVNPass.cpp`, maybe `src/oir/OIRAnalysis.cpp`, `test/ir/oir_digit_extraction_cse.sy` | `python3 scripts/run_tests.py --suite filecheck --filter oir_digit_extraction_cse --jobs 1`; focused OIR/MIR/ASM stages | complete | Existing dirty CSE task preserved; focused digit CSE FileCheck passed after DSE/unswitch changes. No additional CSE change beyond the existing task was needed. |
| P8 | Run correctness and final focused sort perf gate; inspect markdown and JSON reports before marking ready | task file, `build/perf-ci/perf-report.md`, `build/perf-ci/perf-report.json` | commands in Verification Matrix, especially focused `03_sort` stage/e2e and `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` row inspection | blocked | Correctness gates passed, but the new hard gate is not met: current `03_sort` row speedups are below `1.0x` for GCC on all three rows and below `1.0x` for Clang on rows 1 and 3. |
| P9 | Reduce small fixed-size memzero overhead in generated assembly without changing MIR semantics | `include/mir/AsmPrinter.h`, `src/mir/AsmPrinter.cpp` | `xmake`; focused `03_sort` asm/e2e; full correctness; full perf | complete | Added generic direct `sw` expansion for immediate memset/memzero sizes up to 64 bytes and multiples of 4; other sizes still use the existing loop or `memset` call path. |
| P10 | Repair scope after hard-gate correction and add remaining low-cost generic backend idiom for signed power-of-two remainder | `src/pass/oir/OIRToMIRVRegLowerer.cpp`, `test/ir/mir_backend_combine.sy`, task file | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter mir_backend_combine --jobs 1`; focused `03_sort` stage/e2e; `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` | blocked | Reverted this repair's tailrec experiment because it served the old broad target, retained the exact-unroll side-effect/early-exit negative test, and lowered signed `% +/-2^k` to bias/mask/sub. Correctness passes, but sort hard gate still fails. |
| P11 | Add generic stack-address-aware small memzero assembly expansion and test it | `include/mir/AsmPrinter.h`, `src/mir/AsmPrinter.cpp`, `test/ir/mir_backend_combine.sy`, task file | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter mir_backend_combine --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter oir_loop_transforms --jobs 1`; focused `03_sort` stage/e2e/perf | blocked | Added an AsmPrinter dataflow that tracks physical registers known to hold stack-slot addresses and emits `sd zero` for 8-byte-aligned immediate zero memzero. This safely reduced one 64-byte `03_sort` stack memzero from 16 `sw` to 8 `sd`; a broader multi-block recursive-call unroll experiment was reverted because it worsened sort perf. Correctness passes, but the hard gate still fails. |
| P12 | Delete fully overwritten fixed-size stack aggregate zero stores in OIR DSE | `src/pass/oir/OIRDeadStoreEliminationPass.cpp`, `test/ir/oir_sroa_memory.sy`, task file | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_sroa_memory --jobs 1`; focused `03_sort` OIR/MIR/ASM/e2e/perf | blocked | Added conservative same-block interval coverage DSE for aggregate zero stores killed by complete constant-offset scalar overwrites. It supports chained constant GEP offsets and reads from already-overwritten slices, rejects partial/dynamic coverage, one-sided branch coverage, and pre-coverage pointer/call escape. This removes the two head/tail zero bursts; only `cnt` zeroing remains, but the sort hard gate still fails. |
| P13 | Repair AsmPrinter stack-address facts across printed memzero scratch clobbers | `include/mir/AsmPrinter.h`, `src/mir/AsmPrinter.cpp`, `test/ir/mir_backend_combine.sy`, task file | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter mir_backend_combine --jobs 1`; focused `03_sort` stage/e2e/perf | complete | Moved AsmPrinter stack-address fact transfer to after instruction emission and invalidated `MemZero` scratch/call clobbers consistently with regalloc special defs. Added an ASM regression that rejects stale `t3` stack-address reuse after nonzero inline memset expansion. |
| P14 | Add structurally safe exact-trip multi-block unroll without increasing call-loop pressure | `src/pass/oir/OIRLoopTransforms.cpp`, `test/ir/oir_loop_transforms.sy`, task file | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_loop_transforms --jobs 1`; focused `03_sort` stage/e2e/perf | blocked | Added generic multi-block exact-trip full unroll for no-call loops with one latch/exit, no side exits, acyclic body except latch, trip count `2..16`, and bounded clone growth. A call-loop peel/full-unroll repair was tried but not kept: it grew `radixSort` frame/callee-saves and worsened sort timing, so call-containing multi-block loops now conservatively decline. Hard sort gate remains blocked. |
| P15 | Add exact-counted final-iteration peel for conservative call loops and expose self-tail recursion | `src/pass/oir/OIRLoopTransforms.cpp`, `src/pass/oir/OIROptimizationPipelinePass.cpp`, `test/ir/oir_loop_transforms.sy`, task file | `xmake`; focused OIR FileCheck; focused `03_sort` stage/e2e/perf | blocked | Added a bounded final-iteration peel for exact ascending `+1` multi-block call loops: one latch/exit, no side exits, exactly one unused call in the latch, pure post-call IV/address/latch updates, no loop-defined exit values, and void return-only exit. The peel clones only the final iteration, tightens the original latch bound by one, and lets a post-loop-transform tail-recursion cleanup convert the final self-call to `tailrec.header`. It appears in `03_sort` without growing the 272-byte frame or `s0-s8` saves, but hard sort perf still fails. |
| P16 | Add post-RA `addi tmp, src, imm; mv dst, tmp` cleanup when the temp is locally redefined before reuse | `src/pass/mir/MIRBlockSimplifyPass.cpp`, `test/ir/mir_backend_combine.sy`, task file | `xmake`; backend FileCheck; focused `03_sort` stage/e2e/perf | blocked | Added a post-RA-only peephole that rewrites an immediate add directly into the move destination when the add's temporary physical register is redefined later in the same block before any non-implicit use. This removes the recursive-loop latch `addi t0,...; mv arg,t0` chain in `03_sort`; correctness passes, but hard sort perf still fails. |
| P17 | Restrict/disable final peel and make OIR LSR call-pressure-aware | `src/pass/oir/OIRLoopTransforms.cpp`, `src/pass/oir/OIRLoopStrengthReductionPass.cpp`, `test/ir/oir_loop_transforms.sy`, `test/ir/oir_lsr_dynamic.sy`, task file | `xmake`; focused OIR FileChecks; focused `03_sort` stage/e2e/perf | blocked | Final peel matcher now requires a direct self-recursive call if re-enabled, and final peel is disabled after focused perf stayed below the hard gate. OIR LSR now skips pointer-IV phis for loops whose latch contains a call before the latch update, avoiding call-crossing pointer IVs when base+index recomputation is available. In regenerated `03_sort`, the recursive loop pointer phis are gone and `radixSort` drops from 272 bytes / `s0-s8` saved to 256 bytes / `s0-s5` saved, but GCC row speedups remain below `1.0x`. |
| P18 | Add exact first-iteration peel for small counted call loops whose first-iteration guard is provably constant after iteration zero | `src/pass/oir/OIRLoopTransforms.cpp`, `test/ir/oir_loop_transforms.sy`, task file | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_loop_transforms --jobs 1`; focused `03_sort` stage/e2e/perf | blocked | Added a conservative first-iteration peel using the existing exact multi-block loop matcher: exact trip `2..16`, one latch, one normal exit, no side exits, bounded clone size, call present, repairable loop exits, and an internal guard comparing a header IV against a constant whose iteration-0 outcome differs from the uniform residual outcome. Also blocked single-block full unroll of call loops. In `03_sort`, bucket 0 is executed before the residual loop, the residual loop starts at `i=1`, and the loop-internal `i > 0` branch is gone without growing the 256-byte frame / `s0-s5` saves. Hard sort perf still fails. |
| P19 | Gate first-iteration peel on coupled stack-only call-latch LSR and replace blanket call-latch LSR rejection with conservative stack grouping | `src/pass/oir/OIRLoopTransforms.cpp`, `src/pass/oir/OIRLoopStrengthReductionPass.cpp`, `test/ir/oir_loop_transforms.sy`, `test/ir/oir_lsr_dynamic.sy`, task file | `xmake`; focused OIR FileChecks; focused `03_sort` stage/e2e/perf | blocked | First-peel no longer has a standalone `ExactFirstIterationPeel` bypass: negative-cost first peels are rejected unless the residual path proves a coupled non-escaped stack-object LSR shape, reported as `ExactFirstIterationPeelWithStackLSR`. OIR LSR now keeps default call-latch rejection for general loops but allows single-block latch/header loops over non-escaped stack objects whose GEP uses precede the latch call, with same-base/same-induction grouping, constant-only offsets/scale, small constant stride, and a cap of 4 pointer phis. In `03_sort`, residual stack address recomputation becomes 3 compact stack pointer phis and final asm removes repeated `slli ..., s4, 2`; frame remains 256 bytes / `s0-s5`, spills 0. Hard sort perf still fails. |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | Final build passed after P19 repair. |
| Focused alloca/init DSE FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_alloca_init_dse --jobs 1` | yes for P1/P2 | SKIP | Implemented coverage in existing `oir_sroa_memory.sy`; no separate `oir_alloca_init_dse` file was created. |
| Focused SROA memory regression FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_sroa_memory --jobs 1` | yes for P2/P12 | PASS | Covers SROA regressions, prior zero-store sinking, full-overwrite aggregate zero DSE, and negative cases for partial overwrite, one-sided branch overwrite, and pre-coverage call escape. |
| Focused unswitch FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_loop_transforms --jobs 1` | yes for P3/P4 | PASS | Re-run after P18; updated existing loop transform FileCheck for unswitch clone and `LoopInvariantBranch` cost-model bypass. |
| Focused small-loop unroll / final-peel / first-peel FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_loop_transforms --jobs 1` | yes for P5/P6/P14/P15/P17/P18/P19 | PASS | Re-run after P19. Covers single-block exact unroll, no-call multi-block exact unroll, side-exit rejection, disabled final-iteration peel, call-loop no-full-unroll behavior, standalone first-peel negative behavior, negative side-exit/unknown-trip/exit-value/nested-control cases, `ExactSmallTripCount` bypass, and absence of the old standalone `ExactFirstIterationPeel` bypass. |
| Focused OIR LSR FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_lsr --jobs 1` | yes for P17/P19 | PASS | Re-run after P19. Covers non-call dynamic-stride LSR positive behavior, general call-latch negative behavior, stack-only call-latch positive behavior, and dynamic-offset rejection for the stack-only call-latch exception. |
| Focused digit CSE FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_digit_extraction_cse --jobs 1` | yes for P7 or if CSE files touched | PASS | Existing dirty CSE task coverage still passes. |
| Focused OIR/MIR/ASM stage | `python3 scripts/run_tests.py --suite stage --stage oir --stage mir --stage asm --filter 03_sort --jobs 1 --o1` | yes | PASS | 9/9 passed after P19. `03_sort*` emitted OIR still has only the required `%cnt` aggregate zero store; `%head` and `%tail` aggregate zero stores are gone. Final-iteration peel remains disabled. First-iteration peel is present only through the coupled stack-LSR bypass: bucket 0 is before the residual loop, residual `%i.1.loop` starts from `1`, there is no `icmp gt i32 %i.1.loop, 0`, and residual stack GEPs for `%head`/`%tail`/`%cnt` are represented by compact `.stack.ptr` phis. |
| Focused E2E | `python3 scripts/run_tests.py --suite e2e --filter 03_sort --jobs 1 --o1` | yes | PASS | 3/3 passed after P19. |
| Focused tailrec regression after discarded experiment | `python3 scripts/run_tests.py --suite e2e --filter h-1-03 --jobs 1 --o1` | yes for repair experiment cleanup | PASS | Passed after reverting the unsafe tailrec-base shortcut experiment that caused a timeout. |
| Backend signed power-of-two remainder / stack memzero / post-RA peephole FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir_backend_combine --jobs 1` | yes for P10/P11/P13/P16 | PASS | Re-run after P16. Covers signed `% 16` lowering, stack-slot-address-aware `sd zero` expansion, stale `t3` fact rejection after nonzero inline memset expansion, large-memset call invalidation, merge-disagreement negative coverage, and the post-RA addi/mv cleanup path. |
| Full FileCheck | `python3 scripts/run_tests.py --suite filecheck --jobs 1` | yes | PASS | 30/30 passed after P10. |
| Full optimized stage/e2e | `python3 scripts/run_tests.py --suite stage --suite e2e --jobs 1 --o1` | yes | PASS before P10 | 1393 passed, 0 failed, 1 script skip (`test/performance/shuffle1.sy` e2e). Not rerun after the signed-remainder lowering repair; focused sort correctness and full FileCheck were rerun after P10. |
| Full all-suite optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | yes | PASS before P10 | 1440 passed, 0 failed, 1 script skip. Not rerun after P10. |
| Focused sort hard-gate performance | `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` plus `03_sort` row inspection in `build/perf-ci/perf-report.json` | yes | FAIL | Latest P19 run: 60/60 correctness passed. Row speedups: `03_sort1` GCC 0.829787, Clang 1.012158; `03_sort2` GCC 0.870871, Clang 0.981982; `03_sort3` GCC 0.839394, Clang 0.978788. |
| Broad performance observation | `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` top-level geomean | no, observation only | OBSERVED | Latest P19 run reported broad `test/performance` geomean GCC 0.9840727097072963, Clang 1.0168261552666757. This is not the termination blocker. |
| Earlier full broad performance observation | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | no, observation only | OBSERVED | Before P10 and before the hard-gate correction, 119/119 correctness passed with GCC 0.9832168134957185 and Clang 1.0185428408077781. Do not use this as the current hard gate. |
| Perf report inspection | inspect `build/perf-ci/perf-report.md` and `build/perf-ci/perf-report.json`; inspect `build/perf-ci/test/performance/03_sort*/03_sort*.compiler.s` | yes | PASS | Current JSON has row time fields but no explicit speedup fields, so row speedups were computed from `gcc`, `clang`, and `compiler`. Latest asm has no 32-store head/tail `sw zero` burst; only the live `%cnt` clear remains as 8 `sd zero` stores. `radixSort` frame remains 256 bytes with `s0-s5` saved in all three regenerated sort artifacts, spills 0. Final peel and `tailrec.header` are absent. First iteration is peeled only with coupled stack LSR. Residual loop no longer has repeated `slli ..., s4, 2` stack address recomputation; it carries stack pointers, but still has phi-copy and loop-control overhead versus GCC's pointer-end loop. |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Only tune the existing CSE patch | CSE removes duplicate digit arithmetic and load reuse misses | rejected as sufficient by itself; user identified DSE, unswitch, and unroll gaps too |
| Add benchmark-specific radix lowering | Would directly attack the motivating workload | rejected; violates contest/compliance constraints |
| Implement DSE in MIR/ASM after lowering | Could remove some stores late | secondary only; source problem is OIR alloca/init and SROA visibility before lowering |
| Extend current OIR loop unswitch | Existing infrastructure already clones loops and repairs exits | preferred for P4 if current blocker is legality/costing rather than missing representation |
| Add a separate OIR small-loop unroll pass | Exact-trip-count small loops are not covered by existing OIR transforms | allowed if it stays generic, bounded, verifier-clean, and pipeline placement is justified |
| Use YIR unrolling only | Existing YIR has unroll support for structured loops | insufficient for OIR shapes introduced by lowering/inlining/cleanup or missed by YIR constraints |

## Change Log

- 2026-07-08: created scoped task file from user request; recorded existing dirty CSE task context
  and the original broad perf gate.
- 2026-07-08: resolved open questions at task creation: implementation continues from the current
  dirty `task/oir-digit-extraction-cse` worktree preserving existing CSE changes; the initial broad
  perf geomean gate was later superseded by the row-level `03_sort` hard gate below.
- 2026-07-08: implementation added conservative aggregate zero-init sinking for stack allocas,
  loop-invariant branch unswitch profitability bypass, SCEV exact-trip support, and a guarded
  single-block unroll prototype. Correctness gates passed, but final broad perf was blocked by GCC
  geomean 0.97575 < 1.0, so task status is `blocked` rather than `ready_for_review`.
- 2026-07-08: repair fixed exact small-loop unroll correctness by preferring latch-aware trip
  counts for rotated single-block loops, added focused `.unr`/`ExactSmallTripCount` FileCheck
  coverage, and verified `03_sort*` OIR/MIR/ASM/e2e. Full correctness still passes, but final broad
  perf remains blocked: 119/119 pass, GCC 0.96691, Clang 1.02998.
- 2026-07-08: second repair fixed the remaining dead alloca/init residue by allowing aggregate
  zero stores to continue sinking through guard merge blocks to the first common dominator of all
  later alloca uses, added `double_guard_zero_init` FileCheck coverage, and added generic inline
  assembly expansion for small immediate memzero/memset sizes up to 64 bytes. Correctness still
  passes and broad GCC improved, but hard gate remains blocked: 119/119 pass, GCC
  0.9832168134957185, Clang 1.0185428408077781.
- 2026-07-08: user corrected the hard gate from broad geomean to the three `03_sort` performance
  rows individually reaching GCC and Clang speedup `>= 1.0x`. This repair reverted its own tailrec
  experiment that was aimed at the old broad target, kept the low-risk exact-unroll side-effect /
  early-exit negative test, and added generic signed `% +/-2^k` MIR lowering. Correctness and
  FileCheck pass, but the new hard gate remains blocked: `03_sort1` 0.781768/0.936464,
  `03_sort2` 0.791176/1.005882, `03_sort3` 0.725067/0.902965 for GCC/Clang respectively.
- 2026-07-08: repair diagnosed the remaining `03_sort` gap against compiler/GCC/Clang assembly.
  A conservative multi-block exact-trip recursive-call unroll experiment did expose the expected 16
  direct recursive calls but grew the `radixSort` frame and worsened focused sort time, so that
  experiment was reverted. Kept a generic AsmPrinter improvement that tracks stack-address physical
  registers across MIR basic blocks and emits aligned `sd zero` stores for immediate 8-byte-aligned
  stack memzero. Focused correctness and FileCheck pass, but the hard gate remains blocked:
  `03_sort1` 0.800000/0.977465, `03_sort2` 0.782486/0.988701, `03_sort3` 0.809392/0.903315 for
  GCC/Clang respectively.
- 2026-07-08: repair added generic OIR DSE for fixed-size stack aggregate zero stores whose bytes
  are completely overwritten by constant-offset scalar stores in the same straight-line region
  before any uncovered read, escape, call-use, unknown alias access, or partial-path coverage.
  Focused tests cover full overwrite and negative partial/branch/escape cases. This removes the
  `03_sort` head/tail zero bursts from OIR and asm; only the live `cnt` zero clear remains. The hard
  gate still fails after P12: `03_sort1` 0.794521/1.030137, `03_sort2` 0.782369/0.980716,
  `03_sort3` 0.707196/0.848635 for GCC/Clang respectively.
- 2026-07-08: repair fixed `AsmPrinter` stack-address facts across printed `MemZero` expansion by
  applying transfer after emission and clearing facts for the scratch/call clobbers that regalloc
  models as special defs. Added a focused backend FileCheck regression for stale `t3` stack-address
  facts after nonzero inline memset expansion. Also added no-call multi-block exact-trip unroll for
  structurally safe loops. A call-containing recursive-loop peel/full-unroll experiment was tested
  and discarded because it grew `radixSort` from 272 bytes / `s0-s8` saved to 320-512 bytes with
  more callee-saves and worsened `03_sort` timing. Final P14 correctness gates pass, but the hard
  gate still fails: `03_sort1` 0.798295/0.968750, `03_sort2` 0.824047/0.988270, `03_sort3`
  0.797101/0.962319 for GCC/Clang respectively.
- 2026-07-08: repair added a conservative final-iteration peel for exact ascending multi-block
  call loops. The kept transform clones only the final iteration, tightens the original latch bound
  by one, requires one unused latch call plus pure post-call induction/address updates, and then
  lets a post-loop-transform tail-recursion cleanup rewrite the exposed self-tail call. `03_sort`
  artifacts now show `.peel` and `tailrec.header`, while `radixSort` remains 272 bytes and still
  saves only `s0-s8`. Also added a post-RA addi/mv peephole that folds `addi tmp, src, imm; mv dst,
  tmp` when `tmp` is redefined before any later use, plus OIR and ASM negative tests for call-loop
  full-unroll avoidance and AsmPrinter fact invalidation. Correctness gates pass, but the hard gate
  still fails after P16: `03_sort1` 0.778090/0.960674, `03_sort2` 0.799419/0.994186, `03_sort3`
  0.794721/0.982405 for GCC/Clang respectively.
- 2026-07-08: repair restricted final peel to direct self-recursive calls and then disabled the
  final-peel transform after focused perf remained below the row hard gate. Added call-pressure-aware
  OIR LSR that skips pointer-IV phis in loops whose latch contains a call before the latch update,
  preserving base+index recomputation instead. Focused correctness gates pass, `radixSort` drops to
  256 bytes with only `s0-s5` saved, and Clang parity is mostly recovered, but the hard gate remains
  blocked by GCC rows: `03_sort1` 0.788406/1.014493, `03_sort2` 0.808571/0.980000, `03_sort3`
  0.820588/1.008824 for GCC/Clang respectively.
- 2026-07-08: repair added generic exact first-iteration peel for small counted call loops where a
  first-iteration-only IV guard can be proven by enumerating the exact trip count. The transform
  clones only iteration 0, rewrites residual header phi starts to the peeled latch values, forces
  the residual guard only after proving its uniform outcome for iterations 1..N-1, preserves call
  order, and rejects side exits, unknown trip counts, oversized bodies, non-repairable exit uses,
  and nested/unsafe control. It also blocks single-block full unroll of call loops. Focused
  correctness gates pass and `03_sort` now has bucket 0 peeled before a rolled residual loop with
  no `i > 0` branch, while frame/saves remain 256 bytes / `s0-s5`; however the hard gate still
  fails: `03_sort1` 0.771429/0.928571, `03_sort2` 0.814706/1.035294, `03_sort3`
  0.814371/0.985030 for GCC/Clang respectively.
- 2026-07-08: repair narrowed first-iteration peel profitability so standalone negative-cost
  first peels no longer bypass the cost model; the bypass is now only
  `ExactFirstIterationPeelWithStackLSR`, requiring a proven residual path to non-escaped stack-only
  compact-pointer LSR. Replaced P17's blanket call-latch LSR rejection with a conservative
  stack-only single-block exception: non-escaped stack objects only, uses before the latch call,
  same-base/same-induction grouping, constant-only offsets/scale, small constant stride, and at
  most four pointer phis. Focused correctness passes and `03_sort` residual asm carries stack
  pointers without repeated `slli ..., s4, 2`; frame remains 256 bytes / `s0-s5`, spills 0. The hard
  gate still fails: `03_sort1` 0.829787/1.012158, `03_sort2` 0.870871/0.981982, `03_sort3`
  0.839394/0.978788 for GCC/Clang respectively.

## Open Questions

- None.

Resolved decisions:

- Implementation starts from the current dirty `task/oir-digit-extraction-cse` worktree. Preserve
  and coordinate the existing CSE changes; do not revert, overwrite, or discard them.
- The final hard gate is row-level sort performance: the three `03_sort` performance rows must each
  satisfy `gcc_o3_speedup >= 1.0` and `clang_o3_speedup >= 1.0` if those fields exist, or the same
  ratios computed from `gcc/compiler` and `clang/compiler` when the report only stores times. Broad
  GCC/Clang geomean is non-blocking observation and must not keep or clear the task by itself.

## Handoff Note

Current state:

- Implementation ran on the current dirty `task/oir-digit-extraction-cse` worktree and preserved the
  existing CSE changes.
- Added a P12 OIR DSE repair for fixed-size stack aggregate zero stores that are fully killed by
  later scalar stores. The legality invariant is local and conservative: in the same straight-line
  block after the aggregate zero store, constant-offset scalar stores must cover the whole object;
  loads are allowed only from slices already overwritten; dynamic/partial coverage, uncovered
  reads, pointer/call escape, unknown alias memory access, and branch-before-coverage all block the
  deletion. This is not keyed to array length, names, functions, files, cases, or inputs.
- Focused P12 FileCheck coverage was added to `test/ir/oir_sroa_memory.sy`: full aggregate
  overwrite is deleted; partial overwrite plus dynamic read, one-sided branch overwrite, and
  pre-coverage call escape keep the zero store.
- P12 verification passed: `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter
  oir_sroa_memory --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --filter
  03_sort --jobs 1 --o1`; `python3 scripts/run_tests.py --suite stage --stage mir --stage asm
  --filter 03_sort --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e --filter 03_sort
  --jobs 1 --o1`; `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py`.
- P12 artifact inspection: `build/linux/x86_64/release/compiler --emit-oir -O1
  test/performance/03_sort1.sy | rg "store \[16 x i32\] zero"` now prints only the `%cnt` zero
  store. The regenerated compiler asm for all three sort cases has no 32-store head/tail
  `sw zero` burst; only the live `%cnt` clear remains as 8 `sd zero` stores.
- Implemented generic stack aggregate zero-init sinking in OIR DSE. It only handles local alloca
  aggregate-zero stores whose later alloca uses are all dominated by the sink point, avoids sinking
  into loop headers with backedges, and now continues sinking through guard merge blocks when the
  first sink point still post-dominates an early return path. This removed the remaining `cnt`
  zero-init before the recursive sort guard in the focused artifact.
- Enabled existing OIR loop unswitch for proven loop-invariant branches by using a cost-model
  `LoopInvariantBranch` profitability bypass. Unsafe condition materialization rules remain intact.
- Added SCEV constant-expression folding and single-block exact-trip-count unroll support. Repair
  fixed the prior unsafe profitability-bypass trial: generic SCEV counted rotated latch-condition
  loops one iteration short, so the unroller now prefers latch-aware exact trip counts before falling
  back to SCEV. `ExactSmallTripCount` cost-model bypass is enabled for tiny exact loops that remove
  loop branches and stay within the code-growth cap.
- Correctness gates passed for the current P11 repair: `xmake`, focused FileChecks
  (`mir_backend_combine`, `oir_loop_transforms`), and focused `03_sort` OIR/MIR/ASM/e2e. Full
  optimized stage/e2e and full optimized all-suite passed earlier in this task before P10 and were
  not rerun after P11.
- Added generic small immediate memzero/memset expansion in `AsmPrinter`: sizes up to 64 bytes and
  divisible by 4 print direct `sw` stores; all other sizes keep the existing loop or `memset` call.
- A discarded repair experiment tried a small-positive inline cost-model bypass; focused perf showed
  worse sort behavior, so the OIRInlinePass change was reverted and is not part of the final diff.
- Discarded experiments in this repair: disabling exact small-loop unroll worsened broad GCC
  geomean to 0.94564; broad scalar constant-call specialization improved huffman locally but
  inflated MIR and failed broad GCC; a tail-recursive base-case shortcut targeted the old broad gate,
  was unsafe during experimentation, and was fully reverted after the user corrected the hard gate.
- Added generic signed power-of-two remainder lowering in OIR-to-MIR: signed `% +/-2^k` now lowers
  to sign-bias, mask, and subtract instead of quotient/product reconstruction. This preserves C
  signed remainder semantics without relying on case names or input sizes.
- Added generic stack-address-aware `AsmPrinter` dataflow: if every path into a MIR block agrees
  that a physical register holds a specific stack-slot address, immediate aligned zero memzero can
  print `sd zero` stores instead of `sw zero`. In the focused sort artifact this safely reduces the
  `cnt` 64-byte stack clear from 16 stores to 8 stores; the two other stack clears remain
  conservatively unchanged because their address registers are clobbered on loop paths.
- P13 repair fixed a stale `AsmPrinter` fact risk: stack-address facts are now transferred after
  printing each MIR instruction, and `MemZero` clears facts for its printed scratch/call clobbers
  consistently with regalloc special defs. The focused ASM regression in
  `test/ir/mir_backend_combine.sy` rejects stale `t3` stack-address reuse after nonzero inline
  memset expansion.
- P14 repair added generic exact-trip multi-block full unroll only for structurally safe no-call
  loops: one latch, one exit, no side exits/returns, acyclic body except the latch backedge, exact
  trip count `2..16`, bounded clone growth, and repairable exit uses. Calls remain rejected from
  full unroll.
- P15 repair added final-iteration peel for exact ascending multi-block call loops without full
  unrolling: one latch, one normal exit, no side exits/returns, exact trip count `2..16`, exactly
  one unused call in the latch, pure post-call induction/address/latch compare, no loop-defined exit
  values, and a void return-only exit. The final clone jumps directly to the exit, and the pipeline
  runs existing tail-recursion elimination after loop transforms so self-recursive final calls become
  `tailrec.header`.
- P16 repair added a post-RA-only `addi tmp, src, imm; mv dst, tmp` peephole in
  `MIRBlockSimplifyPass`; it fires only when the temporary physical register is redefined later in
  the same block before any non-implicit use. This removes the recursive-loop latch temporary move
  chain without relying on function/test names.
- Final P16 verification passed: `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter
  mir_backend_combine --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter
  oir_loop_transforms --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --filter
  03_sort --jobs 1 --o1`; `python3 scripts/run_tests.py --suite stage --stage mir --stage asm
  --filter 03_sort --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e --filter 03_sort
  --jobs 1 --o1`; `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py`.
- Final artifact inspection: `radixSort` frame is 272 bytes and saves `s0-s8` in all three
  regenerated `03_sort` compiler asm files. Final peel and `tailrec.header` are present. The
  head/tail zero bursts stayed gone: OIR has only `store [16 x i32] zero` for `%cnt`, `sw zero`
  count in `radixSort` is 0, and the remaining live `%cnt` clear is 8 `sd zero` stores.
- Current focused sort perf still fails the hard gate after P16: `03_sort1` GCC 0.778090 / Clang
  0.960674, `03_sort2` GCC 0.799419 / Clang 0.994186, `03_sort3` GCC 0.794721 / Clang 0.982405.
  Task status is `blocked`, not `ready_for_review`.
- P17 repair restricted final-iteration peel so the cloned call must be a direct self-recursive call
  if the transform is re-enabled, then disabled final peel entirely after focused perf stayed below
  the row hard gate. The generic `putint` call-loop FileCheck no longer expects `.peel`, and the
  cost-model FileCheck no longer expects `ExactFinalIterationPeel`.
- P17 repair added call-pressure-aware OIR LSR: when a loop latch contains a call before the latch
  update, LSR skips pointer-IV phis for that loop and leaves equivalent base+index GEP recomputation
  in place. This is a generic pressure rule, not keyed to function names, variable names, files,
  test identities, bucket counts, or input sizes.
- P17 verification passed: `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter
  oir_loop_transforms --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter oir_lsr
  --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --stage mir --stage asm
  --filter 03_sort --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e --filter 03_sort
  --jobs 1 --o1`; `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py`.
- P17 artifact inspection: the recursive `03_sort` loop no longer has pointer-IV phis for
  `%head`/`%tail`/`%cnt` stack-array walks crossing the recursive call. `radixSort` frame is now
  256 bytes and saves only `s0-s5` in all three regenerated sort artifacts. Final peel and
  `tailrec.header` are absent. The head/tail zero bursts stayed gone; the remaining live `%cnt`
  clear is still 8 `sd zero` stores.
- Current focused sort perf still fails the hard gate after P17: `03_sort1` GCC 0.788406 / Clang
  1.014493, `03_sort2` GCC 0.808571 / Clang 0.980000, `03_sort3` GCC 0.820588 / Clang 1.008824.
  Task status remains `blocked`, not `ready_for_review`.
- P18 repair added exact first-iteration peel for small counted call loops. Legality is deliberately
  narrow: exact trip count `2..16`, one latch and one normal exit, no side exits/returns, acyclic
  loop body except the latch, call present, cloned body under the size cap, loop exit uses locally
  repairable, and an internal branch whose `icmp` compares a header IV against an integer constant
  with a first-iteration outcome different from the uniform residual outcome. The first clone
  executes before the residual loop, residual header phi incoming values are rewritten to the
  first-iteration latch values, and the residual branch is forced only after the proof; calls,
  loads, stores, division, and remainder are not speculated.
- P18 also blocks single-block exact full unroll when the loop body contains a call, keeping the
  no-full-call-loop invariant aligned with the existing multi-block call-loop restriction.
- P18 verification passed: `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter
  oir_loop_transforms --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --stage
  mir --stage asm --filter 03_sort --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e
  --filter 03_sort --jobs 1 --o1`; `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py`.
- P18 artifact inspection: `03_sort*` OIR has a pre-residual bucket-0 `call void @radixSort(...)`,
  residual `%i.1.loop = phi [1, %while.end.15], ...`, and no `icmp gt i32 %i.1.loop, 0`. The
  regenerated asm likewise has a first `call radixSort`, initializes `s4` to `1`, and the rolled
  residual loop has no `bge zero, s4` first-iteration guard. `radixSort` frame remains 256 bytes
  with `s0-s5` saved; final peel and `tailrec.header` remain absent. Head/tail zero bursts remain
  gone; the only remaining zero burst is the live `%cnt` clear as 8 `sd zero` stores.
- Current focused sort perf still fails the hard gate after P18: `03_sort1` GCC 0.771429 / Clang
  0.928571, `03_sort2` GCC 0.814706 / Clang 1.035294, `03_sort3` GCC 0.814371 / Clang 0.985030.
  Task status remains `blocked`, not `ready_for_review`.
- P19 repair removed the standalone `ExactFirstIterationPeel` profitability bypass. First-peel can
  only bypass a negative cost model result when the residual path proves a coupled stack-only LSR
  shape; the cost-model reason is now `ExactFirstIterationPeelWithStackLSR`. The generic
  `no_full_unroll_call_loop` FileCheck remains unpeeled and rejects the old standalone bypass.
- P19 repair replaced the P17 blanket call-latch LSR rejection with a conservative stack-only
  exception. It fires only for single-block latch/header loops over non-escaped stack objects, only
  when all candidate GEP uses precede the latch call, rejects dynamic scale/offset, caps pointer
  phis at four, groups same-base/same-induction GEPs, and does not move loads/stores/calls.
- P19 verification passed: `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter
  oir_lsr --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter
  oir_loop_transforms --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --stage
  mir --stage asm --filter 03_sort --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e
  --filter 03_sort --jobs 1 --o1`; `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py`.
- P19 artifact inspection: `03_sort*` residual OIR has compact `.stack.ptr` phis for `%tail`,
  `%head`, and `%cnt`; the final asm residual loop no longer repeats `slli ..., s4, 2` for stack
  array addresses. `radixSort` remains 256 bytes with `s0-s5` saved and spills 0. Head/tail zero
  bursts remain gone; the only remaining zero burst is the live `%cnt` clear as 8 `sd zero` stores.
- Current focused sort perf still fails the hard gate after P19: `03_sort1` GCC 0.829787 / Clang
  1.012158, `03_sort2` GCC 0.870871 / Clang 0.981982, `03_sort3` GCC 0.839394 / Clang 0.978788.
  Task status remains `blocked`, not `ready_for_review`.

Next action:

- Next repair should ignore broad geomean as the hard target and focus only on generic optimizations
  that lift all three `03_sort` rows over both GCC and Clang. Current evidence says correctness is
  stable, head/tail zero bursts are fixed, final peel is disabled, standalone negative first-peel is
  blocked, first-peel only survives when coupled to stack-only LSR, and the residual recursive loop
  now carries compact stack pointers without frame/callee-save regression. The compiler is still too
  slow against GCC and slightly below Clang on rows 2/3: `03_sort1` 0.0329s vs GCC 0.0273s / Clang
  0.0333s, `03_sort2` 0.0333s vs GCC 0.0290s / Clang 0.0327s, and `03_sort3` 0.0330s vs GCC
  0.0277s / Clang 0.0323s. The next blocker is residual loop body quality versus GCC after compact
  stack LSR: final asm still has backedge phi-copy moves and integer loop-control overhead, while
  GCC's residual recursive loop is closer to a pointer-end loop with fewer loop-control operations.
  Any repair must stay generic and avoid frame/callee-save regression; do not reintroduce direct
  multi-block full unroll with calls or unconditional final peel. Do not mark ready until all three
  row-level speedups are at least `1.0x`.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-08-radix-general-opts.md`
- `docs/tasks/2026-07-08-oir-digit-extraction-cse.md`
- `/home/yoo/.codex/skills/yoolang-optimization/SKILL.md`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`
- `src/pass/oir/OIRDeadStoreEliminationPass.cpp`
- `src/pass/oir/OIRSROAPass.cpp`
- `src/pass/oir/OIRLoopTransforms.cpp`
- `src/pass/oir/OIRLoopStrengthReductionPass.cpp`
- `src/pass/oir/OIRGVNPass.cpp`
- `src/oir/OIRAnalysis.cpp`
- `include/oir/OIRScalarOpt.h`
- `src/pass/mir/MIRBlockSimplifyPass.cpp`
- `src/pass/mir/MIRRegAllocPass.cpp`
- `src/pass/oir/OIRToMIRVRegLowerer.cpp`
- `src/mir/AsmPrinter.cpp`
- `include/mir/AsmPrinter.h`
- `test/ir/oir_sroa_memory.sy`
- `test/ir/oir_loop_transforms.sy`
- `test/ir/oir_lsr_dynamic.sy`
- `test/ir/mir_backend_combine.sy`
