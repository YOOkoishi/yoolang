# Task: Range Gated Arith Idioms

Status: ready_for_review
Created: 2026-06-07
Last update: 2026-06-08
Owner: Codex
Branch: task/range-gated-arith-idioms
Worktree: current workspace
Base commit: 5801a2a

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

Decision: not used

Reason:

```text
This task changes arithmetic semantics across OIR and MIR, so an isolated worktree was preferred.
The worktree creation required writing Git metadata outside the sandbox and was rejected.
The user explicitly requested creating a branch and doing the implementation there, so the current
workspace is on branch task/range-gated-arith-idioms.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git worktree add /tmp/yoolang-range-gated-arith-idioms -b task/range-gated-arith-idioms # rejected
git switch -c task/range-gated-arith-idioms
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
| P1 | Expose or reuse OIR range proof for non-negative and bounded values | `OIRValueRange.cpp`, OIR simplify helpers | OIR stage focused tests | done | Added path-sensitive branch-constraint overlay without copying whole-function range maps. |
| P2 | Add OIR/MIR power-of-two div/rem idioms with signed safeguards | `OIRValueRange.cpp`, OIR/MIR lowering helpers | FileCheck and e2e tests | done | OIR rewrites non-negative `% +/-2^k` to `and mask`; existing MIR non-negative `/ 2^k` lowering remains guarded by range proof. |
| P3 | Add rotate-like arithmetic transforms only where semantics are proven | OIR/MIR arithmetic passes | focused perf and correctness tests | done | No rotate-like transform added: current OIR lacks a proven general rotate expression shape and no safe range proof was available. |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_opt --jobs 1` | yes | PASS | Added signed guarded `% 16` and `% -8` checks. |
| Full FileCheck | `python3 scripts/run_tests.py --suite filecheck --jobs 1` | yes | PASS | 17 passed. |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter 20_rem --jobs 1 --o1` | yes | PASS | Signed remainder verifier smoke. |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter 20_rem --jobs 1 --o1` | yes | PASS | Signed remainder verifier smoke. |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter 20_rem --jobs 1 --o1` | yes | PASS | Signed remainder verifier smoke. |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter crypto --jobs 1 --o1` | yes | PASS | 3 passed. |
| Compile-time regression check | `python3 scripts/run_tests.py --suite stage --suite e2e --filter 29_long_line --jobs 1 --o1` | yes | PASS | 5 passed after range overlay fix. |
| Full optimized stage/e2e | `python3 scripts/run_tests.py --suite stage --suite e2e --jobs 1 --o1` | before broad finalization | PASS | 1388 passed, 0 failed, 1 skipped; log at `/tmp/yoolang-stage-e2e-range-gated.log`. |
| Performance | `PERF_TEST_DIRS=test/performance PERF_MAX_CASES=9 python3 scripts/compare_perf.py` | yes | PASS | 9 cases, GCC geomean 0.775x, Clang++ geomean 0.751x, MIR stage metrics OK, QEMU instruction count disabled. |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Apply unsigned-style bit tricks broadly | Fast and simple | rejected: signed semantics risk |
| Restrict to MIR patterns only | Lower proof burden | partially chosen for low-level equivalent sequences, but OIR range is needed for broader cases |

## Change Log

- 2026-06-07: created task file.
- 2026-06-08: switched current workspace to `task/range-gated-arith-idioms` after worktree creation was rejected.
- 2026-06-08: implemented range-gated non-negative `% +/-2^k -> and mask`, completed OIR `And` lowering/support, fixed range propagation compile-time regression, and verified.

## Open Questions

- None.

## Handoff Note

Current state:

- Implementation is ready for review on branch `task/range-gated-arith-idioms`.
- OIR `And` is now a verified integer binary instruction, participates in constant/SCCP/local simplification, GVN/LICM/loop cloning, and lowers through both VReg and stack lowering.
- `OIRValueRange` now carries path-sensitive branch constraints with a lightweight overlay and rewrites `srem` by positive or negative powers of two to `and mask` only when the left-hand range is proven non-negative.
- No rotate-like transform was added because no general, proven rotate expression exists in the current OIR surface.

Next action:

- Review the diff. Full stage/e2e and focused performance smoke are passing.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-07-range-gated-arith-idioms.md`
- `src/pass/oir/OIRValueRange.cpp`
- `src/pass/oir/OIRLocalSimplify.cpp`
- `src/pass/oir/OIRToMIRVRegLowerer.cpp`
- `src/pass/oir/OIRToMIRStackLowerer.cpp`
- `test/ir/oir_opt.sy`
