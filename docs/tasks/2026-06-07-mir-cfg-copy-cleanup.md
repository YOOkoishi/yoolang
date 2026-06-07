# Task: MIR CFG Copy Cleanup

Status: ready_for_review
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

Decision: not used for this execution

Reason:

```text
The task originally planned an isolated worktree, but the user invoked Codex from the active
workspace. Edits were kept to MIR copy/jump cleanup, one focused FileCheck test, and this task
record. Pre-existing unrelated workspace changes were left untouched.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
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
| P1 | Add safe cross-block copy propagation for single-predecessor/single-successor regions | `MIRCopyCoalescingPass.cpp` | MIR FileCheck/stage smoke | done | Replaces only when every non-implicit use is on a linear single-pred/single-succ path; blocks at calls and source/destination redefs |
| P2 | Merge/lay out avoidable PHI edge copy blocks after lowering | `MIRJumpCleanupPass.cpp` | MIR/ASM FileCheck | done | Moves single-predecessor pure copy/constant edge blocks after predecessor or before target; payload limited to pure value materialization/copy opcodes |
| P3 | Add fallthrough-aware block simplification and branch inversion | `MIRJumpCleanupPass.cpp`, existing `MIRBlockSimplifyPass.cpp` | ASM stage and e2e focused tests | done | Jump cleanup now feeds existing branch inversion/fallthrough removal; no `MIRBlockSimplifyPass.cpp` code change needed |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | Ran `xmake -q` |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir --jobs 1` | yes | PASS | 3 passed; added `phi_shuffle` MIR/ASM edge fallthrough checks |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter control_flow --jobs 1 --o1` | if lowering assumptions change | NOT_RUN | |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter 01_mm --jobs 1 --o1` | yes | PASS | 3 passed; `--filter mir` matched 0 stage tests, so used `01_mm` smoke |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter 01_mm --jobs 1 --o1` | yes | PASS | 3 passed |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter 01_mm --jobs 1 --o1` | yes | PASS | 3 passed |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | Left for broader review because focused correctness and perf gates passed |
| Performance | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=6 python3 scripts/compare_perf.py` | yes | PASS | 6 cases, failed=0, geomean speedup GCC 0.72x / Clang++ 0.73x |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Only remove jump-only blocks | Small change | rejected: most observed overhead is copies plus jumps |
| Rewrite OIR CFG first | Higher-level cleanup | rejected for this task; start at MIR lowering and peephole behavior |

## Change Log

- 2026-06-07: created task file.
- 2026-06-07: implemented MIR linear cross-block copy propagation and copy-edge block layout cleanup; added MIR/ASM FileCheck coverage; focused build, FileCheck, stage, e2e, and small perf gates pass.

## Open Questions

- None.

## Handoff Note

Current state:

- Ready for review. Implemented:
  - PreRA copy coalescing now removes a copy when all uses are reachable along a linear single-predecessor/single-successor chain without crossing calls or source/destination redefs.
  - Jump cleanup now reorders single-predecessor pure copy/constant edge blocks to expose predecessor or target fallthrough, preserving CFG by only moving blocks when surrounding layout fallthrough is explicit-safe.
  - `test/ir/mir_backend_combine.sy` checks that `phi_shuffle` avoids redundant jumps on selected PHI edge paths.

Next action:

- Review the diff and optionally run full optimized tests/perf before merge.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-07-mir-cfg-copy-cleanup.md`
- `src/pass/mir/MIRCopyCoalescingPass.cpp`
- `src/pass/mir/MIRJumpCleanupPass.cpp`
- `include/pass/mir/MIRPeepholeCommon.h`
- `test/ir/mir_backend_combine.sy`
