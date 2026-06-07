# Task: MIR Branch Combine

Status: proposed
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: task/mir-branch-combine
Worktree: ../yoolang-mir-branch-combine
Base commit: d206fd7

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

## Worktree

Decision: used

Reason:

```text
This task changes MIR branch semantics and dead producer removal. Use an isolated worktree before code edits.
The worktree is planned here but not created by this task-record split.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git worktree add ../yoolang-mir-branch-combine -b task/mir-branch-combine
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
| P1 | Support direct branches against small constants by materializing reusable constants | `MIRCompareBranchCombinePass.cpp`, `MIRCombineCommon.*` | focused FileCheck | pending | Keep materialization outside hot repeated path when possible |
| P2 | Match `XorI 1`, `SeqZ`, `Snez`, and their branch-zero/nonzero uses | `MIRCompareBranchCombinePass.cpp` | MIR stage focused tests | pending | Remove dead boolean temps |
| P3 | Add branch inversion cleanup after combine | `MIRCompareBranchCombinePass.cpp`, `MIRBitIdiomCombinePass.cpp` | ASM/e2e focused tests | pending | Coordinate with block simplify |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir_backend_combine --jobs 1` | yes | NOT_RUN | add compare branch patterns |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter mir_backend_combine --jobs 1 --o1` | yes | NOT_RUN | |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter mir_backend_combine --jobs 1 --o1` | yes | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter 01_mm --jobs 1 --o1` | yes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=6 python3 scripts/compare_perf.py` | yes | NOT_RUN | |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Do all compare cleanup in OIR | SSA has more context | rejected for this task: target compare-branch forms are MIR-specific |
| Emit extra pseudo compare opcodes | Cleaner matching | rejected initially: existing opcode set is sufficient |

## Change Log

- 2026-06-07: created task file.

## Open Questions

- None.

## Handoff Note

Current state:

- Task record created from the MIR optimization split; no code edits made.

Next action:

- Create the recorded worktree, read keep=yes anchors, then implement P1.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-07-mir-branch-combine.md`
- `docs/mir-design.md`
- `src/pass/mir/MIRCompareBranchCombinePass.cpp`
- `src/pass/mir/MIRRemZeroBranchCombinePass.cpp`
- `src/pass/mir/MIRBitIdiomCombinePass.cpp`
- `include/pass/mir/MIRCombineCommon.h`
- `src/mir/AsmPrinter.cpp`
