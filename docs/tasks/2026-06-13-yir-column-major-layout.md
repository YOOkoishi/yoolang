# Task: YIR Column-major Layout Conversion

Status: proposed
Created: 2026-06-13
Last update: 2026-06-13
Owner: Codex
Branch: not used yet
Base commit: df20afe

## Goal

Add a conservative YIR pass that converts selected private two-dimensional arrays from row-major physical layout to column-major physical layout when the observed loop-nest access pattern is predominantly column-contiguous.

The task is separate from loop interchange: it changes array layout plus index order while preserving the source program's logical indexing.

## Non-goals

- Do not transform function parameter arrays, globals with external linkage, arrays passed to IO/runtime helpers, or arrays whose layout is ABI-visible.
- Do not implement general data-layout optimization for all ranks in the first version.
- Do not change loop order as part of this task except for local cleanup that naturally follows.
- Do not special-case benchmark identities or input sizes.

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

- MIR RA and ASM passes
- unrelated scalar OIR passes
- full polyhedral transform internals beyond access-summary interaction points

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/README.md` | full | active task registry | yes | keep in sync |
| `docs/yir-design.md` | array/type sections | understand ranked array representation | yes | required |
| `src/main/main.cpp` | `add_ast_pipeline` | choose pass position before loop/polyhedral transforms | yes | YIR pipeline hook |
| `include/yir/YIR.h` | array type, initializer, access nodes | confirm rewrite APIs | yes | focused ranges |
| `include/yir/YIRLoopAnalysis.h` | loop/access summary APIs | reuse existing loop facts where possible | yes | avoid duplicate analysis |
| `src/yir/YIRLoopAnalysis.cpp` | focused access collection ranges | understand affine access summaries | yes | likely dependency |
| `src/pass/yir/YIRLoopOptimizationPass.cpp` | loop nest handling ranges | reference existing loop traversal patterns | yes | do not broaden initially |
| `src/pass/yir/YIRToOIRLowerer.cpp` | array layout lowering ranges | ensure new layout lowers as intended | yes | validation anchor |

## Branch

Decision: not used for task creation

Reason:

```text
This file only scopes future work. Create task/yir-column-major-layout before implementation.
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
```

## Invariants And Risks

Correctness invariants:

- Only private arrays whose every access can be rewritten may be transformed.
- All initializers, stores, loads, address computations, and later reads must agree on the new physical layout.
- ABI-visible uses, calls, and IO helpers must block the transform.
- The logical value of `A[i][j]` in source terms must remain unchanged.
- The profitability heuristic must be conservative and easy to disable if it regresses hot cases.

Contest / compliance constraints:

- Implement a general layout optimization; do not key on testcase or benchmark identity.

Risk areas:

- Misclassifying escaped arrays.
- Rewriting initializer data in the wrong order.
- Fighting later loop interchange, tiling, or polyhedral transforms.
- Increasing register pressure or address arithmetic in cases with mixed row and column access.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add layout-candidate analysis for private 2D arrays | new YIR pass + tests | YIR stage | pending | no mutation first |
| P2 | Add profitability heuristic from loop-nest access summary | same pass file | focused stage tests | pending | require clear column-major win |
| P3 | Rewrite array type/initializer/access index order | same pass file, lowerer if needed | YIR/OIR/e2e | pending | local arrays only |
| P4 | Add negative tests for ABI-visible arrays and mixed access patterns | tests | filecheck/e2e | pending | prevent unsafe conversions |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter column_major --jobs 1` | yes | NOT_RUN | add new case |
| YIR stage | `python3 scripts/run_tests.py --suite stage --stage yir --filter column_major --jobs 1 --o1` | yes | NOT_RUN | |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter column_major --jobs 1 --o1` | yes | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter column_major --jobs 1 --o1` | yes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before claiming speedup | NOT_RUN | inspect regressions from mixed access |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Prefer loop interchange only | Already exists and can improve locality | rejected as complete answer; some dependencies block interchange |
| Transform OIR GEP order | Easier to encode as address arithmetic | rejected initially; ABI and initializer intent clearer in YIR |
| Support all ranks | More general | deferred; begin with 2D private arrays |

## Change Log

- 2026-06-13: created proposed task from the 2025 T0 vs yoolang gap comparison.

## Open Questions

- Should column-major conversion run before or after `YIRViewPass` if both are enabled?
- What exact cost threshold avoids hurting mixed row/column access programs?

## Handoff Note

Current state:

- Task is scoped but not implemented.
- Priority: P0.

Next action:

- Create `task/yir-column-major-layout`, dump YIR for small 2D kernels, and verify whether `YIRLoopAnalysis` already exposes enough access-stride information.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-13-yir-column-major-layout.md`
- `docs/yir-design.md`
- `include/yir/YIRLoopAnalysis.h`
- `src/yir/YIRLoopAnalysis.cpp`

