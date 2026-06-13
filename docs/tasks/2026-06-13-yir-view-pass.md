# Task: YIR View / Reshape Elimination

Status: ready_for_review
Created: 2026-06-13
Last update: 2026-06-13
Owner: Codex
Branch: task/yir-view-pass
Base commit: df20afe

## Goal

Add a conservative `YIRViewPass` that recognizes temporary arrays which are only structured views of another array, then rewrites later loads from the temporary view into loads from the original source array with remapped indices.

This targets reshape, slice, transpose-like, and full-array reindexing patterns that are still clear in YIR before polyhedral and loop optimization.

## Non-goals

- Do not implement general alias analysis in this task.
- Do not rewrite arrays that escape through calls, returns, global stores, or ABI-visible interfaces.
- Do not support arbitrary partial writes in the first implementation.
- Do not special-case benchmark filenames, function names, variable names, input sizes, or expected outputs.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [x] YIR
- [x] OIR
- [ ] MIR
- [ ] ASM
- [ ] Runtime
- [ ] Test infrastructure
- [x] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: 1, `docs/yir-design.md`
- Source/script anchors: max 8
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- MIR, ASM, runtime, parser internals
- unrelated OIR scalar passes beyond checking the lowering impact
- benchmark source directories before a focused implementation exists

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/README.md` | full | active task registry | yes | keep in sync |
| `docs/yir-design.md` | focused YIR array/memory sections | understand structured array representation | yes | read before implementation |
| `src/main/main.cpp` | `add_ast_pipeline` | choose insertion point after `ASTToYIRPass` and before polyhedral pipeline | yes | current optimized YIR entry |
| `include/yir/YIR.h` | array, load/store, call, loop node definitions | identify exact data model and mutators | yes | read focused ranges |
| `src/pass/ast/ASTToYIRPass.cpp` | array initialization lowering ranges | understand view initialization shape | yes | likely matcher input |
| `src/pass/yir/YIRToOIRPass.cpp` | array load/store lowering ranges | confirm rewritten YIR lowers correctly | yes | validation anchor |
| `include/yir/YIRVerifier.h` | verifier API | choose verification gate | yes | required gate |

## Branch

Decision: created `task/yir-view-pass`

Reason:

```text
Implementation touches a new YIR transform, main pipeline registration, and tests.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
```

Observed:

```text
status before docs creation: clean
base commit: df20afe
branch: master
implementation branch: task/yir-view-pass
```

## Invariants And Risks

Correctness invariants:

- The view target must be written exactly once by a recognized full initialization pattern before rewritten loads.
- The source array must not be written between view initialization and any rewritten load.
- The view array must not escape to calls, returns, globals, pointer-like aliases, or external IO.
- The index remap must preserve signed integer semantics and array bounds implied by the original program.
- Rewriting must not change evaluation order of side-effecting index expressions.

Contest / compliance constraints:

- Only implement a general semantic-preserving optimization.
- Do not special-case filenames, testcase identity, function names, variable names, input sizes, or expected outputs.

Risk areas:

- YIR representation of multi-dimensional arrays and flattened offsets.
- Partial initialization and dead stores left after replacement.
- Index expressions with non-affine or side-effecting components.
- Interaction with later polyhedral transforms if the pass changes loop-visible array accesses.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add pass shell, registration, and no-op pipeline hook after `ASTToYIRPass` | `include/pass/yir/YIRViewPass.h`, `src/pass/yir/YIRViewPass.cpp`, `src/main/main.cpp` | `xmake` | done | enabled under `-O1` after `ASTToYIRPass` |
| P2 | Implement escape/write-use analysis for local arrays | same pass file | focused YIR stage | done | local view arrays only; calls/source writes/view writes/decay/elem_addr invalidate |
| P3 | Match single full-array view initialization and build affine index remap | same pass file | focused FileCheck/YIR stage | done | supports full 0..dim loop nests and affine source indices over view dims |
| P4 | Rewrite loads and rely on existing DCE/DSE cleanup for dead temporary arrays | same pass file, tests | YIR/OIR stage + e2e | done | positive transpose-style and affine-flatten cases plus source-write negative case |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | build ok |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter yir_view --jobs 1` | yes | PASS | 1 passed |
| YIR stage | `python3 scripts/run_tests.py --suite stage --stage yir --stage oir --filter yir_view --jobs 1 --o1` | yes | PASS | `test/easy/yir_view.sy [yir, -O1]` passed |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage yir --stage oir --filter yir_view --jobs 1 --o1` | yes | PASS | `test/easy/yir_view.sy [oir, -O1]` passed |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter yir_view --jobs 1 --o1` | yes | PASS | 1 passed |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before finalization | PASS | 1431 passed, 0 failed, 1 skipped (`test/performance/shuffle1.sy`) |
| Performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before claiming speedup | PASS | 119 cases, failed=0, geomean GCC 0.94x / Clang++ 1.00x, MIR metrics OK; QEMU instruction count disabled |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Implement in OIR | Existing memory SSA and DSE are available | rejected initially; view structure is clearer in YIR |
| Full SMT/equality reasoning for index maps | More complete view recognition | deferred; start with affine remaps |
| Handle const global views first | Simpler writes | deferred to a later patch after local arrays work |

## Change Log

- 2026-06-13: implemented conservative `YIRViewPass`, registered it in the `-O1` YIR pipeline, added focused FileCheck and e2e coverage, and verified full optimized tests plus performance report.
- 2026-06-13: created proposed task from the 2025 T0 vs yoolang gap comparison.

## Open Questions

- Future expansion: support pointer/parameter sources, partial slices with nonzero view-store offsets, and candidate activation inside conditional regions once path dominance is modeled.

## Handoff Note

Current state:

- Ready for review on `task/yir-view-pass`.
- Implemented pass rewrites later `ArrayLoadOp` reads from a local temporary view into source-array reads when the view was initialized by a single full 0..dim nested loop and the source index is affine in the view indices.
- Safety gates reject or invalidate candidates across source/view writes, calls, returns, `DecayOp`, and `ElemAddrOp`; pointer/parameter sources are intentionally not optimized in this first version.
- Verified YIR output for `test/ir/yir_view.sy`: positive case rewrites `view[x][y]` to `src[y][x]`; negative source-write case keeps the `view` load.

Next action:

- Review `src/pass/yir/YIRViewPass.cpp` for matcher conservatism and decide whether future work should add pointer-source alias handling or path-sensitive candidate activation.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-13-yir-view-pass.md`
- `src/pass/yir/YIRViewPass.cpp`
- `src/main/main.cpp`
- `test/ir/yir_view.sy`
