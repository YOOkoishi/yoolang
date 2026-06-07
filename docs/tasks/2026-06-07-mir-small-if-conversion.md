# Task: MIR Small If Conversion

Status: proposed
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: task/mir-small-if-conversion
Worktree: ../yoolang-mir-small-if-conversion
Base commit: d206fd7

## Goal

Convert small side-effect-free branch diamonds into branchless MIR sequences when doing so reduces hot-loop branch overhead without excessive register pressure.

## Non-goals

- Do not if-convert loops with memory side effects on both arms unless proven equivalent.
- Do not introduce speculative loads or stores.

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
- OIR loop transforms
- `runtime/`

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/mir-design.md` | branch/control-flow sections | MIR control-flow constraints | yes | read relevant ranges |
| `src/pass/mir/MIRBlockSimplifyPass.cpp` | full | CFG diamond simplification target | yes | primary anchor |
| `src/pass/mir/MIRBitIdiomCombinePass.cpp` | full | mask/bit sequence cleanup | yes | primary anchor |
| `src/pass/mir/MIRCompareBranchCombinePass.cpp` | full | branch predicate producer interactions | yes | primary anchor |
| `src/pass/mir/MIRPeepholeCommon.cpp` | full | pure-def and branch helpers | yes | primary anchor |
| `include/mir/MIR.h` | opcode definitions | available branchless instructions | yes | primary anchor |

## Worktree

Decision: used

Reason:

```text
This task changes control flow into arithmetic and can affect register pressure. Use an isolated worktree before code edits.
The worktree is planned here but not created by this task-record split.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git worktree add ../yoolang-mir-small-if-conversion -b task/mir-small-if-conversion
```

## Invariants And Risks

Correctness invariants:

- Converted arms must be side-effect-free, bounded in size, and must not contain calls, stores, or terminators except the diamond branches.
- The converted expression must produce the same value for both branch outcomes.
- Do not speculate instructions that may trap or have undefined signed behavior.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- Diamond detection, side-effect classification, arithmetic equivalence, register pressure, branch prediction tradeoffs.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Identify tiny side-effect-free diamonds in MIR | `MIRBlockSimplifyPass.cpp`, `MIRPeepholeCommon.cpp` | MIR stage focused tests | pending | No transformation yet if needed |
| P2 | Generate branchless mask/select-like sequences | `MIRBlockSimplifyPass.cpp`, `MIRBitIdiomCombinePass.cpp` | FileCheck and e2e tests | pending | Keep opcode set minimal |
| P3 | Add register-pressure and block-size thresholds | `MIRBlockSimplifyPass.cpp` | perf focused tests | pending | Disable if it bloats hot code |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir_backend_combine --jobs 1` | yes | NOT_RUN | add if-conversion checks |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter mir_backend_combine --jobs 1 --o1` | yes | NOT_RUN | |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter mir_backend_combine --jobs 1 --o1` | yes | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter huffman --jobs 1 --o1` | yes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=9 python3 scripts/compare_perf.py` | yes | NOT_RUN | include huffman/crypto |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Do if-conversion in OIR | SSA makes value flow easier | rejected for first pass: target branchless sequence is MIR-specific |
| Convert all small branches | Simple heuristic | rejected: can increase register pressure and regress predictable branches |

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
- `docs/tasks/2026-06-07-mir-small-if-conversion.md`
- `docs/mir-design.md`
- `src/pass/mir/MIRBlockSimplifyPass.cpp`
- `src/pass/mir/MIRBitIdiomCombinePass.cpp`
- `src/pass/mir/MIRCompareBranchCombinePass.cpp`
- `src/pass/mir/MIRPeepholeCommon.cpp`
- `include/mir/MIR.h`
