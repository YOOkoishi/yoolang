# Task: OIR Global Code Motion

Status: proposed
Created: 2026-06-13
Last update: 2026-06-13
Owner: Codex
Branch: not used yet
Base commit: df20afe

## Goal

Add an `OIRGCMPass` for pure, speculatable OIR instructions. The pass should schedule computations early enough to dominate all uses, then as late as possible while preferring lower loop depth and avoiding unnecessary live-range growth.

This fills the gap between OIR LICM, which only handles loop invariants, and MIR scheduling, which is too late to reshape high-level SSA placement.

## Non-goals

- Do not move loads, stores, calls, allocas, phis, terminators, memzero/memset, or other side-effecting operations.
- Do not change memory ordering or exception/undefined-behavior assumptions.
- Do not add target-specific scheduling heuristics in OIR.
- Do not special-case benchmarks.

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
- Related docs: 0
- Source/script anchors: max 8
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- YIR polyhedral internals
- MIR scheduler internals beyond comparing expected downstream effects
- runtime and parser directories

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/README.md` | full | active task registry | yes | keep in sync |
| `include/oir/OIR.h` | instruction classes and mutation APIs | identify movable instruction set | yes | focused ranges |
| `include/oir/OIRAnalysis.h` | dominator tree and loop info APIs | required analyses | yes | keep |
| `src/oir/OIRAnalysis.cpp` | dominator and loop depth implementation ranges | confirm semantics | yes | focused ranges |
| `include/oir/OIRScalarOpt.h` | pass declaration and stats | pipeline exposure | yes | keep |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | optimization order | hook after GVN/DCE and before late cleanup | yes | keep |
| `src/pass/oir/OIRGVNPass.cpp` | pure-expression handling | reuse legality helpers if available | no | reference only |
| `src/pass/oir/OIRLICMPass.cpp` | existing motion legality | reference side-effect checks | yes | likely useful |

## Branch

Decision: not used for task creation

Reason:

```text
This file only scopes future work. Create task/oir-global-code-motion before implementation.
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

- Every moved instruction must dominate all of its uses after placement.
- Instructions with memory effects, control effects, or non-speculatable behavior must never move.
- Operands must dominate the moved instruction at its new location.
- PHI semantics must be respected: do not place ordinary instructions on invalid predecessor edges.
- Avoid increasing loop execution count for an instruction unless the early-placement phase requires it and the late phase repairs it.

Contest / compliance constraints:

- Implement general pure-code motion only; no testcase-specific scheduling.

Risk areas:

- Incorrect dominator updates after moving instructions.
- Live-range growth causing register pressure regressions.
- Moving integer operations that may rely on undefined overflow assumptions.
- Interaction with GVN, SCCP, ADCE, and cleanup ordering.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add pure/speculatable predicate and no-op GCM pass shell | `include/oir/OIRScalarOpt.h`, new pass file, stats | `xmake` | pending | reuse existing helpers if present |
| P2 | Implement early scheduling based on operand dominance | pass + tests | OIR stage | pending | verify use dominance |
| P3 | Implement late scheduling with LCA of uses and loop-depth preference | pass + tests | FileCheck/OIR/e2e | pending | Click-style core |
| P4 | Hook into OIR pipeline after GVN/DCE windows and before late cleanup | pipeline file | stage/e2e/perf | pending | may need one or two placements |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_gcm --jobs 1` | yes | NOT_RUN | add new cases |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_gcm --jobs 1 --o1` | yes | NOT_RUN | |
| MIR/ASM stage | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter oir_gcm --jobs 1 --o1` | if downstream shape checked | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter oir_gcm --jobs 1 --o1` | yes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before claiming speedup | NOT_RUN | watch spill/load changes |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Strengthen LICM only | Existing pass already moves loop invariants | rejected; GCM covers non-loop and branch placement |
| Rely on MIR scheduler | Already present | rejected; too late to alter SSA computation placement |
| Add GCM at YIR | Earlier and structured | deferred; OIR SSA and dominators are better first target |

## Change Log

- 2026-06-13: created proposed task from the 2025 T0 vs yoolang gap comparison.

## Open Questions

- Does OIR already have a canonical helper for "pure and speculatable" that should be shared?
- Should the first pipeline placement be inside `run_aggressive_iteration` or only in the late cleanup window?

## Handoff Note

Current state:

- Task is scoped but not implemented.
- Priority: P1.

Next action:

- Create `task/oir-global-code-motion`, inspect OIR dominator/loop APIs, and add FileCheck cases for branch-to-join and loop-depth placement.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-13-oir-global-code-motion.md`
- `include/oir/OIRAnalysis.h`
- `src/pass/oir/OIRLICMPass.cpp`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`

