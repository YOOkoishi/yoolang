# Task: OIR Memzero MIR Lowering

Status: proposed
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: task/oir-memzero-mir-lowering
Worktree: ../yoolang-oir-memzero-mir-lowering
Base commit: d206fd7

## Goal

Recognize counted store-zero loops in OIR and lower them to MIR `MemZero`, with final assembly choosing an inline zero loop or `memset` when appropriate.

## Non-goals

- Do not recognize specific benchmark names or array names.
- Do not transform loops unless the store range, trip count, and side-effect constraints are proven safe.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [ ] YIR
- [x] OIR
- [x] MIR
- [x] ASM
- [x] Runtime
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
- unrelated MIR peephole passes
- parser/frontend files

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/mir-design.md` | `MemZero` and lowering sections | MIR opcode expectations | yes | read relevant ranges |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | full | OIR pass placement | yes | primary anchor |
| `src/pass/oir/OIRLoopTransforms.cpp` | loop transform ranges | counted loop utilities | yes | large file, read loop ranges |
| `src/pass/oir/OIRToMIRVRegLowerer.cpp` | store/MemZero lowering | current aggregate zero lowering | yes | read store ranges |
| `include/mir/MIR.h` | opcode/operand definitions | extend `MemZero` operands if needed | yes | primary anchor |
| `src/mir/AsmPrinter.cpp` | `MemZero` emission | inline loop vs `memset` | yes | primary anchor |
| `src/mir/MIRVerifier.cpp` | opcode validation | verify dynamic bytes form | yes | read relevant ranges |

## Worktree

Decision: used

Reason:

```text
This is a cross-layer OIR/MIR/ASM optimization. Use an isolated worktree before code edits.
The worktree is planned here but not created by this task-record split.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git worktree add ../yoolang-oir-memzero-mir-lowering -b task/oir-memzero-mir-lowering
```

## Invariants And Risks

Correctness invariants:

- The transformed loop must write exactly the same bytes and no additional observable memory.
- The loop body must have no side effects other than the proven zero stores.
- Dynamic `MemZero` lowering must preserve ABI argument registers and call clobbers.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- Alias safety, trip count proof, byte size computation, ABI call lowering, runtime availability of `memset`.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Detect store-zero counted loops in OIR using generic loop and alias checks | `OIRLoopTransforms.cpp`, `OIROptimizationPipelinePass.cpp` | OIR stage focused tests | pending | Emit a canonical OIR form or marker suitable for lowering |
| P2 | Extend MIR `MemZero` to support dynamic byte counts | `include/mir/MIR.h`, `OIRToMIRVRegLowerer.cpp`, `MIRVerifier.cpp` | MIR stage focused tests | pending | Keep existing static aggregate zero behavior |
| P3 | Choose inline zero loop or `call memset` in ASM | `AsmPrinter.cpp` | ASM/e2e/perf focused tests | pending | Preserve caller-saved clobber model |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_loop --jobs 1` | yes | NOT_RUN | add memzero shape checks |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_loop --jobs 1 --o1` | yes | NOT_RUN | |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter mir_loop --jobs 1 --o1` | yes | NOT_RUN | |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter mir_loop --jobs 1 --o1` | yes | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter 01_mm --jobs 1 --o1` | yes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=9 python3 scripts/compare_perf.py` | yes | NOT_RUN | include matrix/conv cases |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Recognize zero store loops only in MIR | Avoid OIR changes | rejected: MIR lacks clean trip-count and alias context |
| Always call `memset` | Simple and fast for large blocks | rejected: small blocks may regress and call ABI must be modeled |

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
- `docs/tasks/2026-06-07-oir-memzero-mir-lowering.md`
- `docs/mir-design.md`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`
- `src/pass/oir/OIRLoopTransforms.cpp`
- `src/pass/oir/OIRToMIRVRegLowerer.cpp`
- `include/mir/MIR.h`
- `src/mir/AsmPrinter.cpp`
- `src/mir/MIRVerifier.cpp`
