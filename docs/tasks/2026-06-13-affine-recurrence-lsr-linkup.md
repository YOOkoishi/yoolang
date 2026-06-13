# Task: Affine Recurrence and LSR Linkup

Status: proposed
Created: 2026-06-13
Last update: 2026-06-13
Owner: Codex
Branch: not used yet
Base commit: df20afe

## Goal

Strengthen the link between YIR/OIR affine recurrence recognition and existing loop-strength reduction so repeated loop-internal affine address or index arithmetic becomes loop-carried pointer/index recurrence where profitable.

The first version should target patterns such as `A[i * c + k]`, `base + i * stride`, and repeated affine adds inside counted loops.

## Non-goals

- Do not rewrite a full SCEV framework from scratch.
- Do not change loop semantics or rely on undefined overflow.
- Do not handle arbitrary non-affine expressions initially.
- Do not special-case individual benchmark loops.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [x] YIR
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
- Related docs: 1, `docs/yir-design.md`
- Source/script anchors: max 8
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- unrelated MIR peepholes
- parser/runtime
- full polyhedral transform internals unless a YIR recurrence API is reused

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/README.md` | full | active task registry | yes | keep in sync |
| `docs/yir-design.md` | loop and array sections | understand YIR recurrence candidates | yes | required if YIR side changes |
| `include/yir/YIRLoopAnalysis.h` | loop/affine analysis APIs | possible place to extend recognition | yes | keep |
| `src/yir/YIRLoopAnalysis.cpp` | affine collection ranges | implementation anchor | yes | focused ranges |
| `src/pass/yir/YIRLoopOptimizationPass.cpp` | existing unroll/interchange/LSR-adjacent transforms | reuse loop traversal and profitability patterns | yes | focused ranges |
| `src/pass/oir/OIRLoopStrengthReductionPass.cpp` | current OIR LSR implementation | primary implementation anchor if OIR-first | yes | keep |
| `src/oir/OIRAnalysis.cpp` | scalar evolution / loop analysis ranges | recurrence legality | yes | keep |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | ensure LSR is followed by GVN/DCE/ADCE | pipeline ordering | yes | keep |

## Branch

Decision: not used for task creation

Reason:

```text
This file only scopes future work. Create task/affine-recurrence-lsr-linkup before implementation.
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

- Only form recurrences when the induction variable, step, trip structure, and expression evolution are proven.
- Preserve signed/unsigned comparison and overflow semantics.
- The new loop-carried value must have the same value as the original expression on every iteration where it is used.
- Do not introduce extra live-outs or alter PHI values observed after the loop.
- Run cleanup after the rewrite so old arithmetic is removed rather than duplicated.

Contest / compliance constraints:

- Do not key on benchmark names, loop source locations, or input sizes.

Risk areas:

- Overflow in `base + i * stride` rewrite.
- Incorrect insertion of preheader initial values and latch updates.
- Increased register pressure from additional loop-carried PHIs.
- Conflicts with existing YIR unroll, OIR LSR, GVN, and ADCE ordering.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Audit existing YIR/OIR recurrence and LSR coverage with focused examples | docs/task update, tests | emit YIR/OIR/MIR | pending | attribution before implementation |
| P2 | Add a narrow OIR LSR enhancement for repeated affine GEP/index arithmetic | `OIRLoopStrengthReductionPass.cpp`, tests | OIR/MIR/e2e | pending | likely first implementation |
| P3 | If needed, expose YIR affine summaries to seed better OIR lowering | YIR analysis/lowering files | YIR/OIR stage | pending | only after P2 evidence |
| P4 | Ensure OIR pipeline runs GVN/DCE/ADCE after recurrence rewrite | pipeline file | full optimized + perf | pending | avoid dead arithmetic |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter lsr --jobs 1` | yes | NOT_RUN | add or extend cases |
| YIR stage | `python3 scripts/run_tests.py --suite stage --stage yir --filter <case> --jobs 1 --o1` | if YIR changes | NOT_RUN | |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter <case> --jobs 1 --o1` | yes | NOT_RUN | |
| MIR/ASM stage | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter <case> --jobs 1 --o1` | yes | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter <case> --jobs 1 --o1` | yes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before claiming speedup | NOT_RUN | inspect spills and loads/stores |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Implement a full SCEV pass | Broad coverage | rejected initially; too large and duplicates existing analysis |
| YIR-first recurrence rewrite | Higher-level array expressions are clearer | open; start with evidence from current OIR LSR gaps |
| MIR peephole address increments | Easier local codegen improvement | rejected as primary fix; too late to remove high-level arithmetic |

## Change Log

- 2026-06-13: created proposed task from the 2025 T0 vs yoolang gap comparison.

## Open Questions

- Which benchmark cases currently show repeated affine arithmetic that survives existing OIR LSR?
- Should the first patch be OIR-only to minimize blast radius?

## Handoff Note

Current state:

- Task is scoped but not implemented.
- Priority: P1.

Next action:

- Create `task/affine-recurrence-lsr-linkup`, build two focused examples, compare emitted OIR/MIR before changing the pass, and decide OIR-first vs YIR-assisted.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-13-affine-recurrence-lsr-linkup.md`
- `src/pass/oir/OIRLoopStrengthReductionPass.cpp`
- `src/oir/OIRAnalysis.cpp`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`

