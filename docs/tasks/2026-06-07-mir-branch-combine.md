# Task: MIR Branch Combine

Status: ready_for_review
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: tasksys
Worktree: .
Base commit: 047a018

## Goal

Combine boolean-producing compare idioms into direct RISC-V compare branches, especially `xori/seqz/bnez`, `snez`, and constant comparisons.

## Non-goals

- Do not change OIR compare semantics.
- Do not add branch transforms that rely on undefined signed overflow behavior.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [ ] YIR
- [ ] OIR
- [x] MIR
- [x] ASM
- [ ] Runtime
- [ ] Test infrastructure
- [x] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 1, `docs/mir-design.md`
- Source/script anchors: max 5
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- `src/pass/yir/`
- OIR passes unrelated to compare lowering
- `runtime/`

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/mir-design.md` | compare/branch lowering | legal branch forms | yes | read relevant ranges |
| `src/pass/mir/MIRCompareBranchCombinePass.cpp` | full | existing compare branch combine | yes | primary anchor |
| `src/pass/mir/MIRRemZeroBranchCombinePass.cpp` | full | existing branch idiom matcher | yes | reference matcher style |
| `src/pass/mir/MIRBitIdiomCombinePass.cpp` | full | boolean/bit simplification interactions | yes | primary anchor |
| `include/pass/mir/MIRCombineCommon.h` | full | shared helpers and stats | yes | pair with cpp if needed |
| `src/mir/AsmPrinter.cpp` | branch emission ranges | ensure direct branch output | yes | read branch cases only |
| `src/pass/oir/OIRToMIRVRegLowerer.cpp` | compare/branch/value_reg ranges | confirm constant materialization and boolean lowering shapes | no | constants lower through `LI`; zero constants use `zero` |
| `scripts/run_tests.py` | discovery/stage ranges | explain focused stage filter behavior | no | `stage` suite excludes `test/ir`, which is FileCheck-only |

## Worktree

Decision: current checkout used

Reason:

```text
The current checkout was clean and already on the task-system branch where the user asked to
complete the task. No separate worktree was created.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
git worktree list
```

## Invariants And Risks

Correctness invariants:

- A branch combine may remove compare producers only when the produced vreg has no remaining uses.
- Constant materialization must preserve signed `i32` comparison semantics.
- Branch inversion must keep true/false target behavior identical.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- Signed comparison, dead producer erasure order, single-use assumptions, fallthrough target inversion.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Support direct branches against small constants by reusing materialized constant regs | `MIRCompareBranchCombinePass.cpp`, `test/ir/mir_backend_combine.sy` | focused FileCheck | done | `cmp_ge_const` keeps `LI 1234` and branches directly, with no `SLT/XORI` |
| P2 | Match `XorI 1`, `SeqZ`, `Snez`, and their branch-zero/nonzero uses | `MIRCompareBranchCombinePass.cpp`, `test/ir/mir_backend_combine.sy` | focused FileCheck, full tests | done | Matcher now returns dead producer indices; boolean temps are erased in the combine |
| P3 | Add branch inversion cleanup after combine | `MIRCompareBranchCombinePass.cpp`, `test/ir/mir_backend_combine.sy` | ASM/e2e focused tests | done | Existing block simplify performs final fallthrough inversion; no `MIRBitIdiomCombinePass` change was needed |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | debug build passed before switching release; release build also passed after `xmake f -m release` |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir_backend_combine --jobs 1` | yes | PASS | covers MIR and ASM RUN lines |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter mir_backend_combine --jobs 1 --o1` | yes | SKIP | command ran but matched 0 tests because `stage` excludes `test/ir` FileCheck cases |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter mir_backend_combine --jobs 1 --o1` | yes | SKIP | command ran but matched 0 tests for the same reason |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter 01_mm --jobs 1 --o1` | yes | PASS | 3 passed, 0 failed |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | PASS | 1410 passed, 0 failed, 1 skipped |
| Performance | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=6 python3 scripts/compare_perf.py` | yes | PASS | 6 cases OK; geomean speedup GCC 0.66x / Clang++ 0.72x in `build/perf-ci/perf-report.md` |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Do all compare cleanup in OIR | SSA has more context | rejected for this task: target compare-branch forms are MIR-specific |
| Emit extra pseudo compare opcodes | Cleaner matching | rejected initially: existing opcode set is sufficient |

## Change Log

- 2026-06-07: created task file.
- 2026-06-07: implemented MIR compare-branch combine extensions, added FileCheck coverage, and completed verification gates.

## Open Questions

- None.

## Handoff Note

Current state:

- Implementation is ready for review.
- `MIRCompareBranchCombinePass` now rewrites `SLT`, `XOR+SEQZ/SNEZ`,
  generic `SEQZ/SNEZ`, safe boolean `XORI 1`, and `SLT+XORI 1` branch users
  to direct MIR branches.
- Matched dead boolean producers are erased explicitly; constant `LI` producers
  used by the replacement branch are preserved.
- Focused FileCheck, focused e2e, full optimized tests, and sampled perf compare passed.

Next action:

- Review the uncommitted diff and merge when accepted.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-07-mir-branch-combine.md`
- `src/pass/mir/MIRCompareBranchCombinePass.cpp`
- `test/ir/mir_backend_combine.sy`
