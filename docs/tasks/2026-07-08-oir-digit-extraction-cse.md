# Task: OIR Digit Extraction CSE

Status: ready_for_review
Created: 2026-07-08
Last update: 2026-07-08
Owner: implementation subagent
Branch: task/oir-digit-extraction-cse
Base commit: 45759a2

## Goal

Implement a general OIR cleanup that reuses repeated inlined digit-extraction computations,
prioritizing pure `sdiv`/`srem` chains like `x / 16^k % 16` that appear multiple times after
inlining. The implementation must improve the `03_sort*` family by reducing duplicated
division/remainder work without recognizing benchmark names, helper names, input sizes, or expected
outputs.

## Non-goals

- Do not special-case `03_sort*`, `radixSort`, `getNumPos`, filenames, function names, variable
  names, testcase identities, input values, or expected outputs.
- Do not introduce speculative `sdiv`/`srem` execution across control flow unless the divisor and
  value-range legality are proven for all newly executed paths.
- Do not add new OIR operations, target opcodes, or frontend syntax solely for this task unless the
  existing OIR representation cannot express the chosen general transform and the task file is
  updated first.
- Do not weaken FileCheck, stage, e2e, or performance tests to hide regressions.
- Do not claim final performance success from a narrowed or `PERF_MAX_CASES` run.

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
- Related docs: max 2, `docs/performance-optimization-opportunity-audit.md` and
  `/home/yoo/.codex/skills/yoolang-optimization/SKILL.md`
- Source/script/test anchors: max 8 keep=yes anchors before editing
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- Parser, AST, YIR, runtime, and unrelated MIR/ASM backend pass implementations
- Other performance families from the audit (`huffman-*`, `conv2d-*`, `01_mm*`,
  `knapsack_naive-*`, `crypto-*`) except to check final perf regressions
- `test/bsb-final` sources before the implementation is ready for broad performance validation
- Generated `build/perf-ci` case artifacts other than the final `perf-report.md` and
  `perf-report.json`, unless a regression must be attributed

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `/home/yoo/.codex/skills/yoolang-optimization/SKILL.md` | full | contest compliance and perf workflow | yes | required for yoolang optimization work |
| `docs/tasks/README.md` | full | active task registry | yes | updated for this task |
| `docs/tasks/TEMPLATE.md` | full | task shape | no | used once to create this record |
| `docs/performance-optimization-opportunity-audit.md` | full | source audit and first prioritized opportunity | yes | maps this task to finding 1 and prioritized task 1 |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | full | current OIR pass order around inlining, value range, GVN, DCE, and cleanup | yes | implementation should preserve existing cleanup cadence |
| `include/oir/OIRScalarOpt.h` | full | OIR scalar pass API, stats, verifier wrapper | yes | declares pre-inline load/call CSE entry |
| `src/pass/oir/OIRGVNPass.cpp` | full | existing expression keying, dominance-scoped GVN, load/call memory handling | yes | adds load/call-only pre-inline CSE mode |
| `src/pass/oir/OIRValueRange.cpp` | `540-700` | existing nonnegative power-of-two `srem` to `and` rewrite | no | P3 not needed for current patch |
| `src/pass/oir/OIRScalarOptUtils.cpp` | `1-220` | shared constants, pure-instruction classification, stats message | no | no helper/stat change needed |
| `include/oir/OIRAnalysis.h` | `153-166` | alias API surface for no-alias load reuse diagnosis | no | no declaration change required |
| `src/oir/OIRAnalysis.cpp` | `145-205, 900-980, 1030-1195, 1340-1455` | existing alias/modref behavior around alloca, arguments, load/store clobbers | yes | P2 adds a general stack-object no-alias rule |
| `test/ir/oir_cfg_gvn.sy` | full | current GVN FileCheck style | yes | extend or mirror with a focused digit-extraction test |
| `test/ir/oir_opt.sy` | full | existing range-gated remainder tests | no | P3 not needed for current patch |
| `test/ir/oir_digit_extraction_cse.sy` | full | focused generic repeated digit extraction reproducer | yes | added in P1 |
| `test/performance/03_sort1.sy` | full | representative source shape for audit evidence | yes | use only as performance evidence, not as a transform trigger |
| `include/oir/OIR.h` | `360-490` | narrow API check for phi/basic-block insertion alternatives | no | no implementation change required in OIR data structures |
| `src/oir/OIR.cpp` | `888-930` | narrow API check for phi incoming mechanics | no | no implementation change required in OIR data structures |
| `rg -n "GVN\|Local\|CSE\|int_div_rem\|srem\|sdiv" src include test/ir test/performance` | query | locate candidate anchors | no | broad output discarded after selecting keep files |
| `src/pass/oir/OIRLocalSimplify.cpp` | `rg "srem"` | possible algebraic/range simplification anchor | no | read only if GVN/value-range hooks are insufficient |

## Branch

Decision: use a dedicated task branch, but do not create it in the task-generation step.

Reason:

```text
This is a performance-sensitive compiler implementation task and should not be developed on the
current task/smt-solver branch. The task-generation subagent is limited to task-file editing, so it
records the required branch instead of switching branches. The implementation subagent should create
or switch to task/oir-digit-extraction-cse before code edits, after confirming the controller's
intended base branch.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
git checkout -b task/oir-digit-extraction-cse
```

Observed during task generation:

```text
git status --short:

git rev-parse --short HEAD: 45759a2
git branch --show-current: task/smt-solver
```

Observed during implementation setup:

```text
git status --short:
 M docs/tasks/README.md
?? docs/tasks/2026-07-08-oir-digit-extraction-cse.md

git rev-parse --short HEAD: 45759a2
git branch --show-current before checkout: task/smt-solver
git checkout -b task/oir-digit-extraction-cse: initially blocked by sandboxed .git write, then
  succeeded with controller-approved escalation
```

## Invariants And Risks

Correctness invariants:

- CSE/reuse must be driven by SSA value equivalence, instruction opcode, type, operands, dominance,
  and memory side-effect information where relevant.
- A replacement value must dominate every rewritten use, or the implementation must insert
  non-speculative control-flow-safe materialization that is executed exactly where the original
  computation was valid.
- `sdiv` and `srem` rewrites must preserve signed integer semantics, including negative values and
  divisor sign. Power-of-two mask rewrites are legal only when the dividend is proven nonnegative.
- The transform may reuse pure arithmetic/cast/compare/GEP computations and side-effect-free calls
  only under the existing mod/ref and memory-epoch rules.
- Any new canonicalization for `x / 16^k % 16` must be range-based and type-based, not based on
  helper function names or benchmark source text.
- The final task must keep OIR verifier success and optimized `-O1` behavior for all stage/e2e
  tests.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or
  expected outputs.
- Do not hardcode known testcase results or replace real computation with expected output.
- Do not exploit undefined behavior or assumptions that make other valid inputs incorrect.
- Do not weaken tests, skip cases, alter expected outputs, or bias performance scripts.

Risk areas:

- Existing `OIRGVNPass` already handles many dominated pure expressions; the real miss may be loop
  shape, phis, cloned inlined regions, or conservative availability rather than a simple local CSE
  bug. Diagnose before adding a pass.
- Hoisting or merging `sdiv`/`srem` across diamonds can introduce new execution on paths that did
  not previously divide. Treat this as illegal unless nonzero/range preconditions are proven.
- Eliminating repeated computation can lengthen live ranges and hurt register allocation. Check MIR
  metrics and final perf, not only OIR instruction count.
- Range proving for recursive radix partitioning may be incomplete. If nonnegative proof is absent,
  keep the optimization to safe reuse of already-computed values.
- A generic FileCheck can become brittle if it asserts exact loop labels. Prefer stable assertions:
  absence/reduction of duplicate `sdiv`/`srem` in named generic functions and verifier success.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add a focused generic FileCheck reproducer for repeated inlined digit extraction and simple-diamond reuse | `test/ir/oir_digit_extraction_cse.sy` or `test/ir/oir_cfg_gvn.sy` | `python3 scripts/run_tests.py --suite filecheck --filter oir_digit_extraction_cse --jobs 1` | complete | Added generic `digit` reproducer. Same-SSA repeated call already passed; repeated array load separated by local bucket store failed before P2, matching the `03_sort` miss without naming that benchmark. |
| P2 | Diagnose and extend existing OIR GVN/local CSE so equivalent pure digit-extraction chains are reused without speculation | `src/pass/oir/OIRGVNPass.cpp`, maybe `include/oir/OIRScalarOpt.h`, maybe `src/pass/oir/OIROptimizationPipelinePass.cpp` | `xmake`; focused FileCheck from P1; `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_digit_extraction_cse --jobs 1 --o1` | complete | Implemented load/call-only pre-inline CSE using existing GVN machinery plus DCE before `inline_functions`, with a general alias rule that current-function `alloca` storage cannot alias argument/global/other stack roots. A full pre-inline GVN trial improved `03_sort*` but regressed `knapsack_naive*`, so it was narrowed. No benchmark/function/file special-casing. |
| P3 | Add or extend range-proven power-of-two digit-extraction canonicalization only when existing OIR ops and value ranges make it semantics-preserving | `src/pass/oir/OIRValueRange.cpp`, `test/ir/oir_opt.sy` or focused test | `python3 scripts/run_tests.py --suite filecheck --filter oir_opt --jobs 1`; focused OIR stage | skipped | Existing range rewrite is sufficient for this patch; no new shift/mask IR or value-range canonicalization needed. |
| P4 | Validate performance impact and guard against backend/register-pressure regressions | task file, `build/perf-ci/perf-report.md`, `build/perf-ci/perf-report.json` | `python3 scripts/run_tests.py --suite stage --suite e2e --filter 03_sort --jobs 1 --o1`; full perf gates below | complete | Broad perf passes: 119/119, GCC geomean 0.97515x, Clang 1.01831x, MIR metrics OK, QEMU insn count disabled. |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | 2026-07-08 passed after final narrowed P2 edits, build ok in 5.52s |
| Focused digit FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_digit_extraction_cse --jobs 1` | yes | PASS | Initially failed before P2 on duplicate second `sdiv`/`srem`; passed after final narrowed P2 |
| Existing GVN FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_cfg_gvn --jobs 1` | yes | PASS | 2026-07-08 passed. This command was accidentally launched in parallel with the focused OIR stage; remaining test scripts will be sequential. |
| Existing OIR opt FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_opt --jobs 1` | yes if `OIRValueRange.cpp` or local simplification changes | SKIP | P3 skipped; no `OIRValueRange.cpp` or local simplification edit |
| Focused OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter 03_sort --jobs 1 --o1` | yes | PASS | 2026-07-08 passed 3 `03_sort*` cases after final narrowed P2. An earlier run was accidentally parallel with existing GVN FileCheck; final rerun was sequential. |
| Focused MIR/ASM stage | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter 03_sort --jobs 1 --o1` | yes | PASS | 2026-07-08 passed 6 `03_sort*` MIR/ASM items after final narrowed P2 |
| Focused E2E | `python3 scripts/run_tests.py --suite e2e --filter 03_sort --jobs 1 --o1` | yes | PASS | 2026-07-08 passed 3 `03_sort*` e2e cases after final narrowed P2 |
| Full optimized stage/e2e | `python3 scripts/run_tests.py --suite stage --suite e2e --jobs 1 --o1` | yes | PASS | 2026-07-08 passed 1393, failed 0, skipped 1 (`test/performance/shuffle1.sy` e2e skipped by script) after final narrowed P2 |
| Full optimized all-suite | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | PASS | 2026-07-08 passed 1440, failed 0, skipped 1 (`test/performance/shuffle1.sy` e2e skipped by script) |
| Performance baseline readback | inspect pre-change `build/perf-ci/perf-report.md` and `.json` if available | yes | PASS | Existing pre-change report generated 2026-07-07: 60/60 pass, GCC geomean 0.97196x, Clang 1.01784x, `03_sort1/2/3` compiler 0.0525s/0.0459s/0.0455s, MIR metrics OK, QEMU insn count disabled |
| Full preliminary performance | `PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py` | yes | PASS | 2026-07-08 final narrowed P2: 60/60 pass, GCC geomean 0.97734x, Clang 1.01047x, QEMU insn count disabled, MIR metrics OK. `03_sort1` 0.0525s -> 0.0454s, `03_sort2` 0.0459s -> 0.0487s, `03_sort3` 0.0455s -> 0.0452s vs baseline readback; knapsack returned near baseline after narrowing full GVN. |
| Broad final performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | yes | PASS | 2026-07-08 final report: 119/119 pass, GCC geomean 0.97515x, Clang 1.01831x, faster cases GCC 38 / Clang 42, MIR metrics OK, QEMU insn count disabled |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Extend existing `OIRGVNPass` | Current pass already has expression keys, dominance scoping, replacement application, and memory handling | preferred initial path |
| Add a separate OIR local/PRE pass | May be needed if the miss is availability across simple diamonds rather than dominated expression reuse | allowed only after P2 diagnosis shows GVN is the wrong home |
| Add range-proven digit canonicalization | Could reduce `% 16`/`/ 16` chains beyond CSE when nonnegative ranges are known | secondary; must be proof-based and should not introduce new IR lightly |
| MIR/backend peephole only | Could optimize final lowered division/remainder sequences | rejected for this task; audit classified the primary gap as OIR cleanup after inlining |

## Change Log

- 2026-07-08: created scoped task file from the performance optimization audit first finding.
- 2026-07-08: implementation setup started; created `task/oir-digit-extraction-cse` from
  `45759a2` / current HEAD, preserving existing task-file and task README changes.
- 2026-07-08: P1 added `test/ir/oir_digit_extraction_cse.sy`; focused FileCheck failed on the
  repeated loaded digit case before implementation, confirming the generic miss.
- 2026-07-08: P2 initially tried full GVN/DCE before inlining; preliminary perf improved
  `03_sort*` but regressed `knapsack_naive*` spills/time, so the patch was narrowed to
  load/call-only pre-inline CSE.
- 2026-07-08: Final narrowed P2 keeps focused digit and `03_sort*` gates passing. Full optimized
  stage/e2e passed 1393/0 with one script skip. `test/performance` perf passed with GCC geomean
  0.97734x and Clang 1.01047x.
- 2026-07-08: Broad final perf passed 119/119 with GCC geomean 0.97515x and Clang 1.01831x.
  Full optimized all-suite passed 1440/0 with one script skip. Task marked ready_for_review.

## Open Questions

- Resolved: controller approved base `45759a2` / current HEAD; implementation branch is
  `task/oir-digit-extraction-cse`.
- Resolved: no new OIR shift/mask representation was needed. Range-canonicalization P3 was skipped
  and safe CSE completed first.

## Handoff Note

Current state:

- Implementation complete on `task/oir-digit-extraction-cse`; status is `ready_for_review`.
- Task file and task README changes from task generation are preserved.
- P1 focused test added and passes after P2. The test failed before P2 on duplicate second
  `sdiv`/`srem` when a repeated digit input was loaded twice with a local bucket store in between.
- P2 is implemented as load/call-only pre-inline CSE using existing GVN machinery plus a general
  stack-object alias rule. A full pre-inline GVN trial was rejected because preliminary perf showed
  `knapsack_naive*` spill/time regression.
- P3 was skipped; no range or new OIR op change was needed.
- Correctness: all focused gates, full optimized stage/e2e, and full optimized all-suite passed.
- Performance: broad `test/performance,test/bsb-final` perf passed 119/119. Final report is
  `build/perf-ci/perf-report.md` / `.json`; QEMU instruction count is disabled by environment.
- Source audit mapping: `docs/performance-optimization-opportunity-audit.md` finding 1 and
  prioritized next task 1, "`03_sort*`: repeated digit extraction after inlining".
- The selected implementation direction is generic OIR CSE/value numbering first, with
  range-proven digit canonicalization only if it fits existing OIR legality and representation.

Next action:

- Review subagent should inspect the current diff against this task, verify no benchmark-specific
  logic was introduced, and review `build/perf-ci/perf-report.md` plus `.json`.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-08-oir-digit-extraction-cse.md`
- `docs/performance-optimization-opportunity-audit.md`
- `/home/yoo/.codex/skills/yoolang-optimization/SKILL.md`
- `src/pass/oir/OIRGVNPass.cpp`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`
- `include/oir/OIRScalarOpt.h`
- `src/oir/OIRAnalysis.cpp`
- `test/ir/oir_cfg_gvn.sy`
- `test/ir/oir_digit_extraction_cse.sy`
- `test/performance/03_sort1.sy`
