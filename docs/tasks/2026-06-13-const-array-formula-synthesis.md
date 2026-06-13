# Task: Const Array Formula Synthesis

Status: proposed
Created: 2026-06-13
Last update: 2026-06-13
Owner: Codex
Branch: not used yet
Base commit: df20afe

## Goal

Add a simple constant-array synthesis pass that replaces loads from recognized one-dimensional `i32` constant arrays with equivalent arithmetic expressions.

The first version should avoid SMT and only recognize cheap, deterministic patterns such as constants, arithmetic progressions, small periods, simple linear forms, low-degree integer polynomials, bit-mask forms, and small piecewise constants.

## Non-goals

- Do not import or depend on an SMT solver in the first version.
- Do not synthesize expressions for mutable arrays or arrays whose initializer is not fully known.
- Do not transform floating-point or non-`i32` arrays initially.
- Do not special-case table contents by benchmark identity.

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

- SMT-related external code from other repositories
- MIR backend internals
- unrelated polyhedral transforms

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/README.md` | full | active task registry | yes | keep in sync |
| `docs/yir-design.md` | globals and array sections | understand where const initializer is available | yes | read first |
| `include/yir/YIR.h` | global array and array-load nodes | decide if YIR is the right layer | yes | focused ranges |
| `src/pass/yir/YIRToOIRPass.cpp` | const array lowering ranges | ensure replacement lowers cleanly | yes | validation |
| `src/pass/oir/OIRGlobalOptPass.cpp` | existing global constant propagation | avoid duplicate logic | yes | may be best home if YIR lacks information |
| `include/oir/OIRScalarOpt.h` | pass declaration if implemented in OIR | pipeline hook | yes | conditional |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | OIR placement if needed | pipeline hook | yes | conditional |

## Branch

Decision: not used for task creation

Reason:

```text
This file only scopes future work. Create task/const-array-formula-synthesis before implementation.
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

- Only replace loads when the array is immutable and the index expression is the same value used by the synthesized expression.
- Synthesized arithmetic must match `i32` semantics, including signedness and overflow behavior used by the IR.
- If the load has an out-of-bounds behavior assumption, the replacement must not make invalid accesses appear valid.
- The replacement must not duplicate side-effecting index computations.
- Cost model must reject expressions that are more expensive than the load for common targets unless performance evidence says otherwise.

Contest / compliance constraints:

- Pattern detection must be based only on array contents and IR semantics, not benchmark identity.

Risk areas:

- Incorrect polynomial or periodic matching due to overflow.
- Replacing a cache-friendly load with expensive arithmetic.
- Losing global-constant propagation opportunities by choosing the wrong IR layer.
- Transforming arrays used by address-taking or external calls.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Decide YIR vs OIR home and add pass shell | YIR or OIR pass files + pipeline | `xmake` | pending | prefer YIR unless initializer/load info is better in OIR |
| P2 | Recognize constant, all-equal, and arithmetic progression tables | pass + tests | FileCheck/stage/e2e | pending | one-dimensional `i32` only |
| P3 | Add small periodic and mask-like forms with cost checks | pass + tests | stage/e2e | pending | avoid overfitting |
| P4 | Add optional small polynomial/piecewise recognition under conservative bounds | pass + tests | full optimized + perf | pending | only if profitable |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter const_array_synth --jobs 1` | yes | NOT_RUN | add new cases |
| YIR stage | `python3 scripts/run_tests.py --suite stage --stage yir --filter const_array_synth --jobs 1 --o1` | if YIR pass | NOT_RUN | |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter const_array_synth --jobs 1 --o1` | yes | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter const_array_synth --jobs 1 --o1` | yes | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before claiming speedup | NOT_RUN | watch arithmetic cost |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Port SMT-based synthesis | More powerful | rejected for first task; too much toolchain complexity |
| Only use global constant propagation | Already exists | rejected; does not infer formulas from table contents |
| Restrict to OIR only | SSA replacement is simple | open; choose after inspecting initializer availability |

## Change Log

- 2026-06-13: created proposed task from the 2025 T0 vs yoolang gap comparison.

## Open Questions

- Is the best first home YIR, where array structure is clear, or OIR, where expression builders and cleanup are stronger?
- What cost threshold should reject periodic/polynomial replacements?

## Handoff Note

Current state:

- Task is scoped but not implemented.
- Priority: P1.

Next action:

- Create `task/const-array-formula-synthesis`, inspect how const global array initializers and loads appear in YIR and OIR, then choose the pass layer.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-13-const-array-formula-synthesis.md`
- `docs/yir-design.md`
- `src/pass/oir/OIRGlobalOptPass.cpp`
- `src/pass/yir/YIRToOIRPass.cpp`

