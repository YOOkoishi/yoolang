# Task: OIR If-conversion and Select

Status: proposed
Created: 2026-06-13
Last update: 2026-06-13
Owner: Codex
Branch: not used yet
Base commit: df20afe

## Goal

Broaden yoolang's conditional-add if-conversion into a more general OIR if-conversion path, then evaluate whether to add a first-class `SelectInst` with MIR lowering.

The first phase should not require a new IR instruction: handle narrow `i1`/`i32` diamond PHI patterns by lowering to existing arithmetic/logic forms such as `cond*zext`, `add`, `sub`, and `and`.

## Non-goals

- Do not add a broad predication framework in the first phase.
- Do not if-convert branches with side effects, calls, stores, or complex memory dependence.
- Do not add `SelectInst` until phase 1 shows enough benefit and the lowering design is clear.
- Do not special-case particular benchmark branches.

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
- register allocator internals unless select lowering affects it
- runtime/parser

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/README.md` | full | active task registry | yes | keep in sync |
| `include/oir/OIRScalarOpt.h` | existing `if_convert_conditional_adds` declaration | extension point | yes | keep |
| `src/pass/oir/OIRLocalSimplify.cpp` | arithmetic simplification and canonical forms | cleanup after conversion | yes | focused ranges |
| `src/pass/oir/OIRCFGCleanupPass.cpp` | diamond cleanup and PHI handling | reuse CFG helpers | no | reference |
| `src/pass/oir/OIRLoopTransforms.cpp` | PHI/block manipulation examples | reference safe rewrites | no | reference |
| `include/oir/OIR.h` | PHI, branch, compare, arithmetic APIs | matcher and possible `SelectInst` design | yes | keep |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | current if-conversion placement | pipeline hook | yes | keep |
| `src/pass/oir/OIRToMIRPass.cpp` | phase 2 select lowering if added | conditional | yes | phase 2 only |

## Branch

Decision: not used for task creation

Reason:

```text
This file only scopes future work. Create task/oir-if-conversion-select before implementation.
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

- Only if-convert diamonds where both arms are side-effect-free and the selected values are safe to compute speculatively.
- Preserve PHI semantics and dominance after removing or bypassing blocks.
- Do not increase arithmetic cost beyond the expected branch-pressure reduction for hot patterns.
- If `SelectInst` is added later, update verifier, printer, parser/dumper expectations, all lowering paths, and simplifiers consistently.

Contest / compliance constraints:

- Generic pattern-based if-conversion only; no benchmark-specific branch matching.

Risk areas:

- Wrong signedness when replacing boolean conditions with integer masks.
- Extra instructions increasing register pressure or slowing predictable branches.
- Incomplete `SelectInst` integration if phase 2 is attempted too early.
- Interaction with existing `if_convert_conditional_adds`.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Audit current conditional-add conversion and add tests for existing behavior | existing OIR pass/tests | FileCheck | pending | establish baseline |
| P2 | Add phase-1 diamond PHI conversion using existing arithmetic/logic instructions | OIR pass + tests | OIR/MIR/e2e | pending | i1/i32 only |
| P3 | Add profitability and negative cases for side effects, calls, stores, and complex PHIs | pass + tests | full optimized | pending | prevent unsafe conversion |
| P4 | Design and optionally implement `SelectInst` plus MIR lowering | OIR/MIR files + tests | full stage/e2e/perf | pending | only after P1-P3 evidence |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter if_conversion --jobs 1` | yes | NOT_RUN | add cases |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter if_conversion --jobs 1 --o1` | yes | NOT_RUN | |
| MIR/ASM stage | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter if_conversion --jobs 1 --o1` | yes | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter if_conversion --jobs 1 --o1` | yes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before claiming speedup | NOT_RUN | branch count and spills matter |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Add `SelectInst` immediately | Cleaner representation | deferred; higher integration risk |
| Keep only conditional-add conversion | Already present and safe | rejected as final scope; too narrow |
| Do MIR-only if-conversion | Avoid OIR changes | rejected initially; OIR has CFG/PHI structure |

## Change Log

- 2026-06-13: created proposed task from the 2025 T0 vs yoolang gap comparison.

## Open Questions

- Which phase-1 patterns are profitable on current RISC-V output without adding a select instruction?
- Should `SelectInst` be represented in OIR only, or also as a dedicated MIR pseudo before final lowering?

## Handoff Note

Current state:

- Task is scoped but not implemented.
- Priority: P2.

Next action:

- Create `task/oir-if-conversion-select`, inspect the existing conditional-add conversion, then add a baseline FileCheck before broadening it.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-13-oir-if-conversion-select.md`
- `include/oir/OIRScalarOpt.h`
- `include/oir/OIR.h`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`

