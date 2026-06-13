# Task: YIR Tidy Memory Forwarding

Status: proposed
Created: 2026-06-13
Last update: 2026-06-13
Owner: Codex
Branch: not used yet
Base commit: df20afe

## Goal

Add a lightweight YIR memory forwarding pass that replaces a load with the value of a dominating store when the pass can prove the same array element is accessed and no unknown write, call, or control-flow ambiguity intervenes.

This intentionally uses structured YIR information before array accesses are lowered into OIR GEP/load/store sequences.

## Non-goals

- Do not replace OIR MemorySSA, DLE, or DSE.
- Do not cross complex branches or loops in the first version.
- Do not forward through calls or unknown writes.
- Do not implement full alias analysis.

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

- MIR/ASM backend files
- broad OIR DSE internals beyond understanding cleanup expectations
- polyhedral transform internals

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/README.md` | full | active task registry | yes | keep in sync |
| `docs/yir-design.md` | memory and statement sections | understand YIR store/load representation | yes | required |
| `src/main/main.cpp` | `add_ast_pipeline` | choose pass position after AST lowering, before loop transforms | yes | pipeline hook |
| `include/yir/YIR.h` | load/store, block/region, call nodes | matcher and safety checks | yes | focused ranges |
| `include/yir/YIRVerifier.h` | verifier API | verification gate | yes | required |
| `src/pass/yir/YIRLoopOptimizationPass.cpp` | traversal helpers | reference pass traversal style | no | read if needed |
| `src/pass/yir/YIRToOIRPass.cpp` | lowered load/store shape | confirm cleanup path | yes | OIR validation |
| `src/pass/oir/OIRDeadLoadEliminationPass.cpp` | cleanup interaction | understand what remains for OIR | no | reference only |

## Branch

Decision: not used for task creation

Reason:

```text
This file only scopes future work. Create task/yir-tidy-memory-forwarding before implementation.
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

- Forward only when the store dominates the load in the same simple region.
- The array object and every index expression must be proven identical or equivalent under a narrow affine/constant matcher.
- No intervening store to the same array, unknown call, escaped write, or control-flow merge may exist.
- Do not duplicate side-effecting expressions while replacing the load.
- Preserve volatile-like runtime IO behavior by treating calls as barriers.

Contest / compliance constraints:

- Do not key forwarding on benchmark identities or expected outputs.

Risk areas:

- Incorrect equality for affine indices.
- Forwarding across branches where the store is not guaranteed.
- Leaving unused stores that later cleanup cannot remove.
- Missed verifier coverage for malformed YIR after statement replacement.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Add pass shell and local region traversal | new YIR pass + pipeline hook | `xmake`, YIR stage | pending | no-op first |
| P2 | Implement exact same-array/same-index store-load forwarding within a straight-line region | pass + tests | FileCheck/YIR/OIR | pending | constant indices first |
| P3 | Extend index equivalence to simple affine expressions already normalized by YIR | pass + tests | focused e2e | pending | keep matcher narrow |
| P4 | Add barrier and negative tests for calls, branches, alias/escape, and intervening writes | tests | filecheck/e2e | pending | required before performance |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter yir_tidy_memory --jobs 1` | yes | NOT_RUN | add new case |
| YIR stage | `python3 scripts/run_tests.py --suite stage --stage yir --filter yir_tidy_memory --jobs 1 --o1` | yes | NOT_RUN | |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter yir_tidy_memory --jobs 1 --o1` | yes | NOT_RUN | ensure cleanup follows |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter yir_tidy_memory --jobs 1 --o1` | yes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before claiming speedup | NOT_RUN | |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Strengthen OIR DLE/DSE instead | Existing memory cleanup already lives there | rejected as only solution; high-level array identity is lost after lowering |
| Use full MemorySSA in YIR | More complete | deferred; too broad for P0 |
| Cross-branch forwarding | More profitable | deferred until simple local form is proven |

## Change Log

- 2026-06-13: created proposed task from the 2025 T0 vs yoolang gap comparison.

## Open Questions

- Which existing YIR simplifier should run immediately after this pass, or should the pass only rely on OIR DCE/DSE?
- Does YIR currently canonicalize array index expressions enough for an affine matcher?

## Handoff Note

Current state:

- Task is scoped but not implemented.
- Priority: P0.

Next action:

- Create `task/yir-tidy-memory-forwarding`, inspect YIR load/store node APIs, then add a no-op pass and a focused exact-index test.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-13-yir-tidy-memory-forwarding.md`
- `docs/yir-design.md`
- `include/yir/YIR.h`
- `src/pass/yir/YIRToOIRPass.cpp`

