# Task: OIR If-conversion and Select

Status: ready_for_review
Created: 2026-06-13
Last update: 2026-06-15
Owner: Codex
Branch: task/oir-if-conversion-select
Base commit: f615d9e

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
| `test/ir/oir_if_conversion.sy` | full | focused FileCheck coverage for this task | yes | new |

## Branch

Decision: task branch

Reason:

```text
Implementation touches OIR optimization logic and tests; use a task branch.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
```

Observed:

```text
status before implementation: clean
base commit: f615d9e
created branch: task/oir-if-conversion-select
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
| P1 | Audit current conditional-add conversion and add tests for existing behavior | existing OIR pass/tests | FileCheck | done | Existing conditional-add and short-circuit bool coverage remains in `oir_huffman_gap`; new focused file adds value-select baseline. |
| P2 | Add phase-1 diamond PHI conversion using existing arithmetic/logic instructions | `src/pass/oir/OIRLocalSimplify.cpp`, `test/ir/oir_if_conversion.sy` | OIR/MIR/e2e | done | Converts only low-cost empty-arm i32 PHI selects: `cond ? 1 : 0` to `zext`, and `cond ? -1 : 0` to an all-ones mask. General value selects stay branched. |
| P3 | Add profitability and negative cases for side effects, calls, stores, and complex PHIs | pass + tests | full optimized | done | Matcher still recognizes empty-arm value selects, but conversion requires a cheap lowering; call/store arms and generic `x/y` or `7/13` selects stay branched in FileCheck. |
| P4 | Design and optionally implement `SelectInst` plus MIR lowering | OIR/MIR files + tests | full stage/e2e/perf | deferred | Online perf/instruction reports showed broad arithmetic expansion is not robust; without a target cmov, a first-class `SelectInst` would still need profitability-driven lowering. |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter if_conversion --jobs 1` | yes | PASS | 1 passed |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter if_conversion --jobs 1 --o1` | yes | PASS | 0 matched by stage runner; direct `--emit-oir -O1 test/ir/oir_if_conversion.sy` passed verifier |
| MIR/ASM stage | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter if_conversion --jobs 1 --o1` | yes | PASS | direct `--emit-mir-stage=pre-ra -O1` and `-S -O1` for `test/ir/oir_if_conversion.sy` passed |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter if_conversion --jobs 1 --o1` | yes | PASS | 0 matched by e2e runner; covered by full optimized e2e |
| OIR FileCheck sweep | `python3 scripts/run_tests.py --suite filecheck --filter oir_ --jobs 1` | no | PASS | 10 passed |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before finalization | PASS | 1433 passed, 0 failed, 1 skipped |
| Performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before claiming speedup | PASS | 119 cases, 0 failed; GCC geomean 0.94x, Clang++ geomean 0.99x; MIR final totals: 41351 instrs, 9034 moves, 3014 branches, 3963 jumps, 2375 loads, 1562 stores, 256 spills, 310 stack slots; instruction count disabled |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Add `SelectInst` immediately | Cleaner representation | deferred; higher integration risk and no current target-independent win without a cheap target select |
| Keep only conditional-add conversion | Already present and safe | rejected as final scope; too narrow |
| Convert all empty-arm i32 PHI selects with `false + ((true - false) & mask)` | Removes branches uniformly | rejected after report analysis; it expands predictable cheap diamonds into multiple ALU ops |
| Do MIR-only if-conversion | Avoid OIR changes | rejected initially; OIR has CFG/PHI structure |

## Change Log

- 2026-06-13: created proposed task from the 2025 T0 vs yoolang gap comparison.
- 2026-06-15: implemented conservative empty-arm i32 value-select if-conversion and focused FileCheck coverage; deferred first-class `SelectInst`.
- 2026-06-15: completed full optimized and performance verification; marked ready for review.
- 2026-06-15: after reviewing branch perf and instruction reports, restricted value-select if-conversion to low-cost `zext`/mask forms and left generic i32 selects as CFG diamonds.

## Open Questions

- Phase-1 result is intentionally narrow: empty-arm i32 PHI selects are only converted when they lower to one or two simple integer operations.
- `SelectInst` remains deferred. A first-class instruction would be cleaner for representation, but RISC-V lowering still needs a profitability choice between branch/PHI and mask arithmetic.

## Handoff Note

Current state:

- Implementation is complete and verified on branch `task/oir-if-conversion-select`.
- `src/pass/oir/OIRLocalSimplify.cpp` now recognizes empty-arm i32 value-select diamonds after the existing short-circuit bool and conditional-add special cases, but only converts `0/1` and `0/-1` selections to cheap branchless forms.
- `test/ir/oir_if_conversion.sy` covers profitable bool/mask selects and negative cases for generic value selects, calls, and stores.
- No first-class `SelectInst` was added; the decision is deferred until lowering can choose branch versus arithmetic profitably.

Next action:

- Review and merge if acceptable.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-13-oir-if-conversion-select.md`
- `include/oir/OIRScalarOpt.h`
- `src/pass/oir/OIRLocalSimplify.cpp`
- `test/ir/oir_if_conversion.sy`
