# Task: Transpose2 and T03-61 Performance Gap Attribution

Status: ready_for_review
Created: 2026-06-12
Last update: 2026-06-12
Owner: Codex
Branch: master
Base commit: 24b9739

## Goal

Attribute the current yoolang `-O1` gap against GCC `-O3` for `test/performance/transpose2.sy` and `test/bsb-final/2025-T03-61.sy` by comparing perf data, OIR/MIR dumps, and compiler/GCC assembly, then identify general optimization passes worth adding.

## Non-goals

- Do not implement compiler optimizations in this task.
- Do not special-case these filenames, functions, input sizes, or benchmark identities.
- Do not change performance scripts except if a tooling blocker makes evidence collection impossible.

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
- Related docs: max 1, only if existing pipeline docs are needed
- Source/script anchors: max 8
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- unrelated frontend/parser/runtime code
- all benchmark directories beyond `test/performance/transpose2.sy` and `test/bsb-final/2025-T03-61.sy`

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/tasks/TEMPLATE.md` | full | task shape | no | used once to create this record |
| `docs/tasks/README.md` | full | active task registry | yes | keep in sync |
| `test/performance/transpose2.sy` | full | source shape for reported performance case | yes | identical to `2025-T03-61.sy` |
| `test/bsb-final/2025-T03-61.sy` | full | source shape for reported performance case | yes | identical to `transpose2.sy` |
| `build/perf-ci/perf-report.{md,json}` | focused rows and MIR metrics | current focused perf and stage attribution | yes | regenerated on 2026-06-12 |
| `build/perf-ci/test/performance/transpose2/transpose2.{compiler,gcc}.s` | full | assembly comparison | yes | GCC narrows inner loop bound |
| `src/main/main.cpp` | `205-340` | optimized pipeline order and diagnostic stages | yes | `-O1` pipeline |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | full | existing OIR pass order | yes | loop passes exist but miss this pattern |
| `src/pass/oir/OIRLoopTransforms.cpp` | `1450-1585` | existing rotate/unswitch/memzero loop transform scope | yes | unswitch handles invariant conditions, not IV-vs-IV bounds |
| `src/pass/oir/OIRLoopStrengthReductionPass.cpp` | `1-220` | existing induction/pointer recurrence helpers | yes | useful implementation anchor for a future pass |

## Branch

Decision: not used

Reason:

```text
This is a docs-only performance investigation. No compiler code is edited in this task.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
```

Observed:

```text
git status --short: clean
git rev-parse --short HEAD: 24b9739
git branch --show-current: master
```

## Invariants And Risks

Correctness invariants:

- Any proposed pass must be semantics-preserving for the general IR/MIR pattern, not for a named testcase.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- Loop canonicalization and interchange legality.
- Alias/modref precision for array updates.
- Register pressure and spill behavior after loop transforms.
- Integer-address strength reduction and late addressing-mode folding.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Record evidence-backed pass attribution | `docs/tasks/2026-06-12-transpose2-t03-61-perf-gap.md`, `docs/tasks/README.md` | focused perf and IR/ASM inspection | done | no compiler code changes |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Existing report | inspect `build/perf-ci/perf-report.{md,json}` | yes | PASS | old 2026-06-09 artifact was stale vs user numbers |
| Build | `xmake` | yes | PASS | rebuilt release compiler at `24b9739` |
| Focused performance | `PERF_TEST_DIRS=test/performance/transpose2.sy,test/bsb-final/2025-T03-61.sy python3 scripts/compare_perf.py` | yes | PASS | current focused rerun |
| Dynamic instruction count | `ENABLE_QEMU_INSN_COUNT=1 PERF_TEST_DIRS=test/performance/transpose2.sy,test/bsb-final/2025-T03-61.sy python3 scripts/compare_perf.py` | optional | FAILED | QEMU plugin API mismatch; do not use as evidence |
| OIR dump | `build/linux/x86_64/release/compiler test/performance/transpose2.sy --emit-oir -O1 -o /tmp/transpose2.oir` | yes | PASS | OIR keeps `j < rowsize` plus `i < j` continue branch |
| MIR dumps | `--emit-mir-stage={lowered,pre-ra,final} -O1` for `transpose2.sy` | yes | PASS | pre/post RA unchanged; no spills |
| Assembly comparison | compare compiler and GCC `.s` artifacts | yes | PASS | GCC narrows inner loop to `j <= min(i, rowsize - 1)` |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Implement immediately | Could directly try passes | rejected for this task until evidence identifies general patterns |
| Only read source benchmarks | Faster but weak attribution | rejected; must compare IR/MIR/ASM |

## Change Log

- 2026-06-12: created task file and started focused attribution.
- 2026-06-12: completed focused perf, OIR/MIR/ASM comparison, and pass-gap attribution.

## Open Questions

- None.

## Findings

The two reported files are identical, and their `.in` files are identical. Treat them as one optimization pattern.

Focused current performance after rebuilding:

| Case | GCC `-O3` | Clang++ `-O3` | yoolang `-O1` | yoolang/GCC |
| --- | ---: | ---: | ---: | ---: |
| `test/performance/transpose2.sy` | `0.0692s` | `0.1762s` | `0.2078s` | `0.33x` |
| `test/bsb-final/2025-T03-61.sy` | `0.0741s` | `0.1821s` | `0.1918s` | `0.39x` |

MIR stage metrics for the two-case focused run:

| Stage | Instrs | Moves | Branches | Jumps | Loads | Stores | Spills | Stack slots |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| lowered | 374 | 148 | 24 | 72 | 8 | 8 | 0 | 0 |
| post-combine | 348 | 148 | 24 | 72 | 8 | 8 | 0 | 0 |
| pre-ra | 284 | 122 | 24 | 34 | 8 | 8 | 0 | 0 |
| post-ra | 284 | 122 | 24 | 34 | 8 | 8 | 0 | 0 |
| final | 252 | 90 | 24 | 34 | 8 | 8 | 0 | 0 |

This is not primarily an RA/spill problem: `pre-ra == post-ra`, `spills=0`, `stack_slots=0`.

The hot source pattern is:

```c
while (j < rowsize) {
    if (i < j) {
        j = j + 1;
        continue;
    }
    matrix[j * colsize + i] = matrix[i * rowsize + j];
    j = j + 1;
}
```

The optimized OIR still has the same structure:

```text
%j.loop.1 = phi ...
%v5 = icmp lt i32 %j.loop.1, %rowsize.arg
br i1 %v5, %while.body.5, %while.end.6
%v6 = icmp lt i32 %i.loop, %j.loop.1
br i1 %v6, %if.then.7, %if.end.9
```

yoolang assembly therefore keeps two inner-loop branches:

```text
bge a0, s3, inner_exit
blt a4, a0, continue_path
```

GCC instead proves the continue predicate is monotonic in `j` and changes the inner loop bound to the active range:

```text
a1 = min(i, rowsize - 1)
for (j = 0; j <= a1; ++j) copy
```

In the input used by both cases, `n=907200`, `len=100`. Estimating only the transposed inner loops:

| Shape | Iterations |
| --- | ---: |
| Current yoolang `j < rowsize` scan | 84,485,736 |
| GCC-style narrowed active range | 27,466,351 |
| Ratio | 3.08x |

This matches the observed yoolang/GCC gap direction much better than any backend spill explanation.

`--polyhedral` does not solve this case. Forced polyhedral only recognizes the one-dimensional init/load/sum SCoPs and leaves the inlined transpose inner `while`/`continue` shape unchanged.

## Recommended Passes

1. Add an OIR loop-bound tightening pass for monotonic continue/guard branches.

   Match a counted loop with an increasing IV, a side-effect-free skipped branch, and a condition such as `outer_iv < inner_iv` or `inner_iv > invariant`. If the skipped path only reaches the common latch and the latch recurrences are not needed as live-out values, replace `inner_iv < ub` plus the guard with `inner_iv < ub && inner_iv <= invariant`, or materialize `effective_ub = min(ub, invariant + 1)`. This is the main missing pass for these two cases.

2. Add OIR min/max/select canonicalization for loop bounds.

   The loop-bound pass needs a clean way to express `min(rowsize, i + 1)` or `min(i, rowsize - 1)`. If OIR has no select form, build a small preheader diamond that computes the effective bound and let CFG cleanup simplify it.

3. Extend YIR/polyhedral canonicalization to recover triangular loop domains from `while` plus `continue`.

   This is a higher-level alternative or follow-up: convert the nested transpose loop to a triangular domain before lowering to OIR so the polyhedral model can see `0 <= j <= i` directly. Current forced polyhedral evidence shows this is not happening today.

4. Lower-priority MIR/ASM cleanup: remove compare-plus-unconditional-jump patterns after CFG shaping.

   yoolang often emits `branch to skipped block; j body`. GCC's narrowed loop has a simpler loop body. This cleanup can reduce static branches, but it will not recover the 3x iteration-count loss without the OIR loop-bound transform.

Do not start with register allocation work for this issue; the measured stages show no spill or stack-slot growth.

## Handoff Note

Current state:

- Evidence collection complete. The actionable gap is a general OIR triangular/monotonic-continue loop-bound tightening pass, not a testcase-specific workaround or RA fix.

Next action:

- Implement the pass in a new task, using this file as the bounded context source.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-12-transpose2-t03-61-perf-gap.md`
- `docs/tasks/README.md`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`
- `src/pass/oir/OIRLoopTransforms.cpp`
- `src/pass/oir/OIRLoopStrengthReductionPass.cpp`
- `include/oir/OIRScalarOpt.h`
