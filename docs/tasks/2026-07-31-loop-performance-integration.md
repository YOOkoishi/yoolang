# Task: Loop Performance Audit And Optimization Integration

Status: verifying
Created: 2026-07-31
Last update: 2026-07-31
Owner: Codex
Branch: `master` working tree
Base commit: `786085c`

## Goal

Audit every physical case in `test/performance` and `test/bsb-final`, determine whether loop
interchange or adjacent general optimizations still have evidence-backed headroom, integrate only
the safe and profitable candidates, and publish reproducible correctness/performance results.

## Non-goals

- Do not add benchmark-, filename-, function-, input-, or expected-output-specific behavior.
- Do not enable forced polyhedral transforms by default when Auto profitability rejects them.
- Do not land speculative transforms whose measured benefit is noise or whose legality proof is
  incomplete.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [x] YIR
- [x] OIR
- [x] MIR
- [x] ASM
- [ ] Runtime
- [x] Test infrastructure
- [x] Performance

## Context Budget

This integration task combines already-scoped branch work, so each imported semantic patch keeps
its original task/report as the detailed design record. The integration review is bounded to the
changed pass entry points, their focused tests, the two benchmark directories, and the reporting
scripts.

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/performance-optimization-opportunity-audit.md` | findings | prior corpus gap ranking | yes | candidate source |
| `scripts/compare_perf.py` | case discovery, execution, JSON schema | final 119-case evidence | yes | three-run medians |
| `src/pass/yir/YIRPolyhedralTransformPass.cpp` | output reduction and lane sharing | imported profitable loop transform | yes | Auto only |
| `src/pass/oir/OIRLoopStrengthReductionPass.cpp` | shared GEP recurrences | required register-pressure companion | yes | imported with poly patch |
| `src/pass/oir/OIRInlinePass.cpp` | structural callsite advisor | Huffman gap | yes | bounded profitability gate |
| `src/pass/yir/YIRPolyhedralDependenceAnalysisPass.cpp` | cross-base alias handling | transform legality | yes | unknown pointer bases may alias |
| `src/yir/YIRLoopAnalysis.cpp` | legacy loop reorder legality | transform legality | yes | same provenance rule as formal path |
| `test/ir/yir_loop_transforms.sy` | positive and negative reorder shapes | forced-poly regression | yes | includes alias and recurrence guards |
| `test/easy/poly_aliasing_params.sy` | runtime alias counterexample | optimized end-to-end semantics | yes | expected value 456 |

## Branch

Decision: keep the current working tree.

Reason:

```text
The user requested one integrated result in the shared workspace. Candidate development and A/B
evaluation use isolated /tmp clones; only accepted patches are copied into the shared tree.
```

## Invariants And Risks

Correctness invariants:

- Loop interchange, tiling, vector-style expansion, and unroll-and-jam must preserve every
  loop-carried memory dependence.
- Distinct formal pointer values are not no-alias evidence. Only distinct globals or local unique
  allocations may be treated as independent without a callsite/runtime proof.
- Sequential expansion must reject scalar or memory recurrences that require original iteration
  order.
- Structural inlining must retain signature/CFG legality and remain behind the cost model.
- Output-reduction blocking remains Auto-selected; forced polyhedral behavior is not promoted to
  the default pipeline.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes,
  or expected outputs.

Risk areas:

- Polyhedral dependence precision and legacy reorder analysis can disagree unless both use the
  same conservative base-provenance rule.
- Huffman inlining improves runtime but grows final MIR/assembly and compile time.
- Many-matrix reduction blocking improves runtime while increasing code size and some spills.
- QEMU wall-clock measurements are noisy; duplicate physical paths are not independent workloads.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Reject unsound sequential expansion and reordered pointwise accesses | YIR loop transform/analysis and tests | recurrence and alias probes | complete | fixes wrong-code results |
| P2 | Add bounded structural OIR callsite inlining | OIR inline/cost/pipeline and tests | Huffman 11-run interleaved A/B; stage/e2e | complete | about 1.22x Huffman speedup in isolated evaluation |
| P3 | Add output-reduction unroll-and-jam, lane sharing, reanalysis, and shared-GEP LSR | YIR poly pipeline/transform, OIR LSR and tests | matmul/many_mat A/B; 60-case correctness | complete | default Auto, not Force |
| P4 | Make formal and legacy dependence analysis conservative for may-alias parameters | formal dep analysis, legacy analysis, tests | forced-poly alias value 456 and IR checks | complete | derived Decay/ElemAddr roots canonicalized |
| P5 | Evaluate additional historical/static candidates | isolated clones only | focused deterministic and wall-clock A/B | in progress | reject neutral or regressive candidates |
| P6 | Make optional A/B compiler failures and output mismatches fail the performance run | `scripts/compare_perf.py` | negative harness plus final 119-case run | complete | optional compiler is no longer informational-only |
| P7 | Freeze final compiler and run 119 physical cases plus deduplicated analysis | reports/notebook | full performance, stage, e2e | pending | 60 unique workloads, 22 source bodies |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | combined P1-P4 build succeeds |
| Forced alias E2E | manual `-O1 --polyhedral` compile/link/run | yes | PASS | prints 456; old transform printed 24 |
| Alias FileCheck equivalent | `--emit-poly` and `--emit-yir` pattern checks | yes | PASS | unknown formal-base dependence; i-before-j; no jam |
| Focused OIR/poly shapes | direct emit/cost-model checks | yes | PASS | FileCheck executable unavailable locally |
| Targeted combined A/B | 18 hotspot cases, baseline versus P1-P4 | yes | PASS | all outputs correct; Huffman/matmul/many_mat improve |
| GCC/Clang interchange audit | default versus interchange-disabled assembly, 60 cases | yes | PASS | byte-identical for GCC 15.1 and Clang 22.1.8 |
| Full optimized stage/e2e | final frozen candidate | yes | NOT_RUN | pending final candidate selection |
| Final 119-case performance | baseline versus final frozen candidate | yes | NOT_RUN | pending final candidate selection |
| Portable report and notebook | execute, render, and inspect | yes | NOT_RUN | pending final benchmark |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Formal loop interchange alone | original question and existing pass | rejected as the main opportunity: no corpus decision/assembly evidence in Yoolang, GCC, or Clang |
| H8 loop-carried memory value forwarding | removes one dynamic load per iteration | rejected: 15-run medians are within 0.1%-0.4% noise while final MIR grows 194 to 197 |
| Blind exact/full loop unrolling | may expose conv2d/crypto simplification | rejected: prior prototype caused wrong-code/crashes or severe code/spill growth without stable gain |
| Forced conv2d polyhedral transform | could improve locality | rejected: final MIR 446 to 889 and spills 0 to 25 without runtime gain |
| Dynamic MemZero and pointer-loop gates from `ad678f6` unchanged | historical branch fixes small cases | under evaluation; unchanged defaults regress `01_mm*` deterministically |

## Change Log

- 2026-07-31: inventoried 119 physical cases, 60 unique workload triplets, and 22 unique source
  bodies; confirmed all 59 `bsb-final` cases mirror `performance` and only `shuffle1` is unmatched.
- 2026-07-31: integrated the structural inline and output-reduction/shared-LSR patches after
  isolated full-corpus correctness and targeted A/B evaluation.
- 2026-07-31: found and fixed formal plus legacy may-alias reorder wrong code; added structural and
  end-to-end regressions.
- 2026-07-31: completed GCC/Clang interchange-disabled audit and rejected neutral H8 forwarding.

## Open Questions

- Whether a revised, proof-aware pointer-loop unroll gate can reject the small `n=1` fixture while
  retaining the currently profitable large-kernel transform.
- Whether any individually isolated OIR scalar-precision pass from `776a374` has a real corpus hit
  and non-noise benefit under the current pipeline.

## Handoff Note

Current state:

- P1-P4 are integrated and build successfully.
- The candidate set is not frozen until the remaining two isolated audits finish.
- Final full stage/e2e, 119-case A/B, deduplicated calculations, notebook, and portable report are
  still required.

Next action:

- Finish P5, freeze the candidate binary/hash, then run the final verification matrix.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-31-loop-performance-integration.md`
- the keep=yes context files listed above
