# Task: Range Gated Arith Idioms

Status: proposed
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: task/range-gated-arith-idioms
Worktree: ../yoolang-range-gated-arith-idioms
Base commit: d206fd7

## Goal

Optimize arithmetic idioms such as `% power-of-two`, `/ power-of-two`, and rotate-like expressions only when range information proves the replacement is semantically equivalent.

## Non-goals

- Do not rely on C/C++ signed overflow undefined behavior.
- Do not optimize artificial bit idioms by recognizing function names or benchmark structure.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [ ] YIR
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
- Related docs: max 1, `docs/mir-design.md`
- Source/script anchors: max 6
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- `src/pass/yir/`
- unrelated OIR global or inlining passes
- `runtime/`

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/mir-design.md` | integer arithmetic sections | signed `i32` machine semantics | yes | read relevant ranges |
| `src/pass/oir/OIRValueRange.cpp` | full or relevant solver ranges | range proof source | yes | primary anchor |
| `src/pass/oir/OIRAlgebraicSimplifyPass.cpp` | arithmetic simplification | OIR idiom placement | yes | primary anchor |
| `src/pass/oir/OIRLocalSimplify.cpp` | local folding | existing simplification helpers | yes | primary anchor |
| `src/pass/mir/MIRImmediateCombinePass.cpp` | full | MIR constant idiom combine | yes | primary anchor |
| `src/pass/mir/MIRBitIdiomCombinePass.cpp` | full | bitwise cleanup interactions | yes | primary anchor |
| `src/pass/mir/MIRRemZeroBranchCombinePass.cpp` | full | existing signed rem zero idiom | yes | primary anchor |

## Worktree

Decision: used

Reason:

```text
This task changes arithmetic semantics across OIR and MIR. Use an isolated worktree before code edits.
The worktree is planned here but not created by this task-record split.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git worktree add ../yoolang-range-gated-arith-idioms -b task/range-gated-arith-idioms
```

## Invariants And Risks

Correctness invariants:

- `%` and `/` replacements must match signed SysY `i32` behavior for all values admitted by the range proof.
- Rotate-like replacements must prove no signed overflow or explicitly model the required wrap behavior if the IR semantics allow it.
- MIR-only combines may apply only to already-equivalent low-level instruction sequences.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- Signed division rounding, signed remainder sign, overflow, range propagation precision, OIR/MIR semantic mismatch.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Expose or reuse OIR range proof for non-negative and bounded values | `OIRValueRange.cpp`, OIR simplify helpers | OIR stage focused tests | pending | No transform without proof |
| P2 | Add OIR/MIR power-of-two div/rem idioms with signed safeguards | `OIRAlgebraicSimplifyPass.cpp`, `MIRImmediateCombinePass.cpp`, `MIRRemZeroBranchCombinePass.cpp` | FileCheck and e2e tests | pending | Cover negative values |
| P3 | Add rotate-like arithmetic transforms only where semantics are proven | OIR/MIR arithmetic passes | focused perf and correctness tests | pending | Keep broad patterns, no name matching |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_o3 --jobs 1` | yes | NOT_RUN | add signed edge cases |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_o3 --jobs 1 --o1` | yes | NOT_RUN | |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter mir_backend_combine --jobs 1 --o1` | yes | NOT_RUN | |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter mir_backend_combine --jobs 1 --o1` | yes | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter crypto --jobs 1 --o1` | yes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=9 python3 scripts/compare_perf.py` | yes | NOT_RUN | include crypto/huffman/sort |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Apply unsigned-style bit tricks broadly | Fast and simple | rejected: signed semantics risk |
| Restrict to MIR patterns only | Lower proof burden | partially chosen for low-level equivalent sequences, but OIR range is needed for broader cases |

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
- `docs/tasks/2026-06-07-range-gated-arith-idioms.md`
- `docs/mir-design.md`
- `src/pass/oir/OIRValueRange.cpp`
- `src/pass/oir/OIRAlgebraicSimplifyPass.cpp`
- `src/pass/oir/OIRLocalSimplify.cpp`
- `src/pass/mir/MIRImmediateCombinePass.cpp`
- `src/pass/mir/MIRBitIdiomCombinePass.cpp`
- `src/pass/mir/MIRRemZeroBranchCombinePass.cpp`
