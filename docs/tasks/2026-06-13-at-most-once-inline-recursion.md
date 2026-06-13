# Task: At-most-once Inline and Recursion Follow-up

Status: proposed
Created: 2026-06-13
Last update: 2026-06-13
Owner: Codex
Branch: not used yet
Base commit: df20afe

## Goal

Investigate whether yoolang still loses meaningful performance on at-most-once calls or recursive patterns after the existing OIR inline, constant-argument specialization, tail-recursion elimination, and recursive inline controls.

Only if benchmark evidence supports it, implement a narrow YIR at-most-once inline pass or strengthen OIR recursive specialization / tail-recursion modulo add/mul.

## Non-goals

- Do not assume inline is a major gap without measuring it.
- Do not duplicate the existing OIR inliner unless YIR structure enables a specific missing case.
- Do not inline based on function names, testcase names, or known benchmark call graphs.
- Do not make broad recursive cloning that risks compile-time explosion.

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
- Related docs: 0
- Source/script anchors: max 8
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- MIR RA/scheduler internals
- polyhedral transform internals
- unrelated benchmark directories before profiling selects cases

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/README.md` | full | active task registry | yes | keep in sync |
| `src/pass/oir/OIRInlinePass.cpp` | inliner, specialization, recursion controls | understand existing capability | yes | primary anchor |
| `src/pass/oir/OIRTailRecursionEliminationPass.cpp` | tail recursion support | identify modulo add/mul gap | yes | focused ranges |
| `include/oir/OIRScalarOpt.h` | pass declarations and stats | hook extension if OIR changes | yes | keep |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | inline and cleanup order | pipeline interaction | yes | keep |
| `src/pass/ast/ASTToYIRPass.cpp` | YIR call shape if YIR inline is considered | conditional | yes | phase 2 only |
| `include/yir/YIR.h` | YIR function/call representation | conditional | yes | phase 2 only |
| `scripts/compare_perf.py` | report fields and focused runs | evidence collection | no | use as command reference |

## Branch

Decision: not used for task creation

Reason:

```text
This file only scopes future work. Create task/at-most-once-inline-recursion before implementation.
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

- Any at-most-once decision must be derived from the call graph and control-flow reachability, not names.
- Inlining must preserve call side effects, parameter evaluation order, return semantics, and recursion limits.
- Recursive transforms must preserve exact arithmetic semantics, including overflow assumptions.
- Compile-time and code-size growth must have explicit caps.

Contest / compliance constraints:

- No testcase-specific call graph or recursion handling.

Risk areas:

- Code-size explosion from aggressive inlining.
- Incorrect at-most-once proof across loops, recursion, indirect-like calls, or conditional calls.
- Tail-recursion modulo add/mul changing overflow behavior.
- Benchmark noise leading to an unnecessary pass.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Attribute current recursive/inline performance gaps with focused benchmarks | task doc + perf reports | compare_perf + emitted OIR | pending | required before code |
| P2 | If evidence supports it, add at-most-once call analysis in YIR or OIR | chosen pass files + tests | stage/e2e | pending | no mutation first |
| P3 | Implement narrow at-most-once inline or strengthen existing OIR inliner | pass + tests | full optimized | pending | capped code growth |
| P4 | Optionally handle tail-recursion modulo add/mul under strict legality checks | OIR tail recursion pass + tests | e2e/perf | pending | only with evidence |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes if code changes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter inline --jobs 1` | if IR shape changes | NOT_RUN | add targeted cases |
| YIR stage | `python3 scripts/run_tests.py --suite stage --stage yir --filter <case> --jobs 1 --o1` | if YIR changes | NOT_RUN | |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter <case> --jobs 1 --o1` | yes if OIR changes | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter <case> --jobs 1 --o1` | yes if code changes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | required before deciding implementation | NOT_RUN | use evidence gate |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Implement YIR early inline immediately | Matches the compared compiler's early pass | deferred; yoolang already has strong OIR inline |
| Only tune OIR inline thresholds | Lower implementation cost | open after profiling |
| Focus on tail recursion modulo add/mul | Contest-style recursive patterns may benefit | evidence required first |

## Change Log

- 2026-06-13: created proposed task from the 2025 T0 vs yoolang gap comparison.

## Open Questions

- Which current cases still have call overhead or recursive structure after OIR inline and tail recursion elimination?
- Is a YIR pass actually needed, or can the existing OIR inliner be adjusted safely?

## Handoff Note

Current state:

- Task is scoped but intentionally evidence-gated.
- Priority: P2.

Next action:

- Create `task/at-most-once-inline-recursion`, run focused attribution on recursive/call-heavy cases, and only then decide whether to implement.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-13-at-most-once-inline-recursion.md`
- `src/pass/oir/OIRInlinePass.cpp`
- `src/pass/oir/OIRTailRecursionEliminationPass.cpp`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`

