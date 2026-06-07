# Task: MIR CFG Copy Cleanup

Status: proposed
Created: 2026-06-07
Last update: 2026-06-07
Owner: Codex
Branch: task/mir-cfg-copy-cleanup
Worktree: ../yoolang-mir-cfg-copy-cleanup
Base commit: d206fd7

## Goal

Reduce redundant MIR edge blocks, cross-block copies, and unconditional jumps so hot loops have fewer `mv` and `j` instructions.

## Non-goals

- Do not change OIR-level CFG transforms beyond what is necessary to lower PHI copies correctly.
- Do not introduce target-specific testcase matching.

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
- unrelated OIR scalar passes
- `runtime/`

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/mir-design.md` | CFG/copy sections | MIR block and PHI-copy model | yes | read relevant ranges |
| `src/pass/oir/OIRToMIRVRegLowerer.cpp` | PHI edge block/copy lowering | source of `edge.*` blocks and parallel copies | yes | large file, read PHI ranges |
| `src/pass/mir/MIRCopyCoalescingPass.cpp` | full | existing copy propagation | yes | primary anchor |
| `src/pass/mir/MIRJumpCleanupPass.cpp` | full | existing jump-only cleanup | yes | primary anchor |
| `src/pass/mir/MIRBlockSimplifyPass.cpp` | full | fallthrough and local block cleanup | yes | primary anchor |
| `include/pass/mir/MIRPeepholeCommon.h` | full | helper API and stats | yes | pair with cpp if needed |
| `src/mir/MIRVerifier.cpp` | CFG/register checks | preserve verifier invariants | yes | read relevant ranges |

## Worktree

Decision: used

Reason:

```text
This task changes control-flow and copy propagation behavior. Use an isolated worktree before code edits.
The worktree is planned here but not created by this task-record split.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git worktree add ../yoolang-mir-cfg-copy-cleanup -b task/mir-cfg-copy-cleanup
```

## Invariants And Risks

Correctness invariants:

- PHI parallel copies must preserve source values on every predecessor edge.
- Branch targets and fallthrough layout changes must preserve exact CFG semantics.
- Copies involving physical ABI registers must not be propagated past clobbers or calls.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- Critical edges, self edges, PHI cycles, physical register clobbers, PostRA register equality.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add safe cross-block copy propagation for single-predecessor/single-successor regions | `MIRCopyCoalescingPass.cpp`, `MIRPeepholeCommon.*` | MIR stage focused tests | pending | Do not cross calls or physical defs |
| P2 | Merge avoidable PHI edge copy blocks after lowering | `OIRToMIRVRegLowerer.cpp`, `MIRJumpCleanupPass.cpp` | MIR/ASM stage focused tests | pending | Preserve critical-edge safety |
| P3 | Add fallthrough-aware block simplification and branch inversion | `MIRBlockSimplifyPass.cpp`, `MIRJumpCleanupPass.cpp` | ASM stage and e2e focused tests | pending | Target `mv/j` reduction, not layout churn |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir --jobs 1` | yes | NOT_RUN | add/adjust copy CFG checks |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter control_flow --jobs 1 --o1` | if lowering assumptions change | NOT_RUN | |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter mir --jobs 1 --o1` | yes | NOT_RUN | |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter mir --jobs 1 --o1` | yes | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter 01_mm --jobs 1 --o1` | yes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=6 python3 scripts/compare_perf.py` | yes | NOT_RUN | compare `mv/j` on slow examples |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Only remove jump-only blocks | Small change | rejected: most observed overhead is copies plus jumps |
| Rewrite OIR CFG first | Higher-level cleanup | rejected for this task; start at MIR lowering and peephole behavior |

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
- `docs/tasks/2026-06-07-mir-cfg-copy-cleanup.md`
- `docs/mir-design.md`
- `src/pass/oir/OIRToMIRVRegLowerer.cpp`
- `src/pass/mir/MIRCopyCoalescingPass.cpp`
- `src/pass/mir/MIRJumpCleanupPass.cpp`
- `src/pass/mir/MIRBlockSimplifyPass.cpp`
- `include/pass/mir/MIRPeepholeCommon.h`
- `src/mir/MIRVerifier.cpp`
