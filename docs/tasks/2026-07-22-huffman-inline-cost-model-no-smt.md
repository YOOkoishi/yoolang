# Task: Huffman LLVM-Guided Inlining And Cost Model (No SMT)

Status: ready_for_review
Created: 2026-07-22
Last update: 2026-07-22
Owner: Codex
Branch: `task/huffman-inline-cost-model-no-smt`
Base commit: `c815887`

## Goal

Implement a general direct-call OIR inliner with structural legality, a bounded read-only
callsite estimate, shared cost-model profitability, bounded cleanup, and reconsideration of newly
exposed calls. The three Huffman cases are evidence only and must not affect production decisions.

## Non-goals

- No SMT, e-graph, CEGIS, residual-program cloning, or benchmark/name special cases.
- No YIR, MIR, ASM, runtime, or loop-unroll production changes.
- Do not blindly raise global inline size/growth limits.

## Affected Pipeline

- [x] OIR
- [x] Performance

## Context Budget

- Keep anchors: this file, task protocol, compiler/performance references,
  `OIRInlinePass.cpp`, `OIRCostModel.{h,cpp}`, `CostModel.{h,cpp}`, pipeline, focused tests.
- Read only named ranges; do not inspect SMT or unrelated benchmark implementation.

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `src/pass/oir/OIRInlinePass.cpp` | estimator, legality, commit, driver | main behavior | yes | no SMT |
| `src/pass/oir/OIRCostModel.cpp` | decision adapter | ordinary `-O1` policy | yes | report optional |
| `include/pass/oir/OIRCostModel.h` | estimate fields | adapter contract | yes | structural proof |
| `src/pass/CostModel.cpp` | `decide` | policy semantics | yes | no numeric LLVM copy |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | inline windows | cleanup placement | yes | bounded |
| `test/ir/oir_inline_cost.sy` | focused generic cases | correctness/shape | yes | to add |

## Branch

Decision: used. Created a separate `/tmp/yoolang-huffman-inline` worktree from clean `c815887`
because the primary worktree contains unrelated uncommitted loop/SMT work in overlapping files.

## Invariants And Risks

- Preserve argument/return mapping, CFG edges, PHIs, uses, types, and observable memory/call order.
- Reject recursive/unsupported/invalid sites structurally; rejection must not mutate IR or budget.
- The estimator is bounded and read-only; it may use callsite constants but never clone or query SMT.
- The shared cost model decides optional inlining even when diagnostics are not emitted.
- Reconsider candidates by stable IR pointer identity, never printable names.
- Bound accepted sites, growth, cleanup rounds, and candidate scans.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P0 | Capture clean three-case OIR/cost/perf baseline | evidence | focused commands | complete | 0.0638/0.0634/0.0635 s |
| P1 | Read-only callsite estimator and enforced shared decision | inliner/cost adapter | FileCheck/OIR | complete | structural proof only; diagnostics optional |
| P2 | Bounded worklist and cleanup/revisit | inliner/pipeline | stages/e2e | complete | stable pointers; four cleanup rounds maximum |
| P3 | Generic positive/negative coverage | focused tests | FileCheck/e2e | complete | hot constant positive and memory-heavy negative |
| P4 | Final focused perf and broad correctness | evidence/docs | reports | complete | focused calibration plus one final CI-parity run |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | release build |
| Inline FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_inline --jobs 1` | yes | PASS | generic accept/reject coverage |
| Cost decisions | focused `--emit-cost-model=json --cost-model-filter=Inline -O1` | yes | PASS | `Structural/Proven`, empty `solver_id` |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter huffman --jobs 1 --o1` | yes | PASS | 3/3 |
| MIR/ASM stage | focused Huffman | yes | PASS | 6/6 |
| Focused e2e | three Huffman cases | yes | PASS | 3/3 |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | yes | PASS | 1456 passed, 0 failed, 1 skipped |
| Focused performance | explicit three files, baseline/current | yes | PASS | final 0.0515/0.0504/0.0518 s; about 18.4%-20.5% faster |
| Broad performance | CI directories/exclusions | conditional | PASS | 115 cases, 0 failures; Huffman 0.0518/0.0515/0.0519 s |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| SMT or transactional residual PE | precise but expensive and out of scope | rejected |
| Raise global thresholds | simple but uncontrolled growth | rejected |
| Structural gate plus callsite advisor | LLVM-like mechanism and bounded | chosen |

## Change Log

- 2026-07-22: isolated clean task worktree and recorded implementation plan.
- 2026-07-22: added a bounded, read-only constant/CFG cleanup estimate and loop-frequency signal;
  all ordinary `-O1` direct-call decisions now pass through the shared cost model even when cost
  diagnostics are disabled.
- 2026-07-22: replaced fixed inline sweeps with a stable-identity worklist and bounded cleanup
  windows so newly exposed calls can be reconsidered without unbounded code growth.
- 2026-07-22: added generic positive/negative inline coverage and corrected two obsolete FileCheck
  expectations whose old assertions contradicted the intended single-site/constant-call behavior.
- 2026-07-22: full optimized suite passed (1456/0/1); final CI-parity performance run passed all
  115 cases. The three Huffman cases improved from 0.0638/0.0634/0.0635 s to approximately
  0.0515/0.0504/0.0518 s in the focused run.

## Open Questions

- None for this task. Future retuning should continue to use generic OIR/RV64 evidence.

## Handoff Note

Current state: implementation and verification complete; ready for review. Production decisions
contain no benchmark names and issue only structural proofs, with no solver invocation.

Review focus: `OIRInlinePass.cpp` estimator/worklist bounds, the shared hot-growth exception in
`CostModel.cpp`, and cleanup-window ordering in `OIROptimizationPipelinePass.cpp`.

Reports: `build/perf-ci/huffman-c815887-baseline.json`, `build/perf-ci/huffman-delta.json`, and
`build/perf-ci/perf-report.json` (generated artifacts, not committed).
