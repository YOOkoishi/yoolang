# Task Records

本目录保存 Markdown 任务记录。创建新任务时，从 `TEMPLATE.md` 复制为：

```text
docs/tasks/YYYY-MM-DD-slug.md
```

规则：

- 一个任务一个文件。
- 每次推进任务后更新对应文件。
- 创建或完成任务时更新下面的 `Active Tasks` 表。
- 上下文恢复时，先读 `docs/task-system.md`，再读具体任务文件。
- 已完成任务可以留在本目录；如果数量过多，再移动到 `docs/tasks/archive/`。

## Active Tasks

| Task | Status | Branch | Last update |
| --- | --- | --- | --- |
| [General Semantics-Preserving Performance Optimizations](2026-08-04-general-performance-optimizations.md) | complete | `task/general-performance-optimizations` | 2026-08-04 |
| [Loop Performance Audit And Optimization Integration](2026-07-31-loop-performance-integration.md) | verifying | `master` working tree | 2026-07-31 |
| [Context-Sensitive Callsite Inlining And Transactional Residual Partial Evaluation](2026-07-17-context-inline-residual-pe.md) | scoped | `task/context-inline-residual-pe` (planned) | 2026-07-17 |
| [OIR Guarded Small-Loop Unroll And Address Recurrence Optimization](2026-07-17-oir-guarded-small-loop-unroll.md) | scoped | `task/oir-guarded-small-loop-unroll` (planned) | 2026-07-17 |
| [Sort Hard Gate Cleanup](2026-07-08-sort-hard-gate-cleanup.md) | blocked | `task/oir-digit-extraction-cse` | 2026-07-08 |
| [Radix General Optimizations](2026-07-08-radix-general-opts.md) | blocked | `task/oir-digit-extraction-cse` | 2026-07-08 |
| [OIR Digit Extraction CSE](2026-07-08-oir-digit-extraction-cse.md) | ready_for_review | `task/oir-digit-extraction-cse` | 2026-07-08 |
| [Performance Optimization Opportunity Audit](2026-07-08-performance-optimization-audit.md) | ready_for_review | `not used` | 2026-07-08 |
| [SMT Solver](2026-07-07-smt-solver.md) | ready_for_review | `task/smt-solver` | 2026-07-07 |
| [Cost Model Rejection Recovery](2026-07-07-cost-model-rejection-recovery.md) | ready_for_review | `task/cost-model-rejection-recovery` | 2026-07-07 |
| [Cost Model Pass Activation](2026-07-07-cost-model-pass-activation.md) | ready_for_review | `task/cost-model-gating-scope` | 2026-07-07 |
| [Cost Model Gating Scope](2026-07-07-cost-model-gating-scope.md) | ready_for_review | `task/cost-model-gating-scope` | 2026-07-07 |
| [Cost Model Implementation](2026-07-05-cost-model-implementation.md) | ready_for_review | `task/cost-model-implementation` | 2026-07-06 |
| [YIR View Full-pipeline Optimization](2026-06-14-yir-view-full-pipeline.md) | ready_for_review | `task/yir-view-full-pipeline` | 2026-06-14 |
| [YIR View / Reshape Elimination](2026-06-13-yir-view-pass.md) | ready_for_review | `task/yir-view-pass` | 2026-06-13 |
| [YIR Column-major Layout Conversion](2026-06-13-yir-column-major-layout.md) | proposed | `not used yet` | 2026-06-13 |
| [YIR Tidy Memory Forwarding](2026-06-13-yir-tidy-memory-forwarding.md) | proposed | `not used yet` | 2026-06-13 |
| [OIR Global Code Motion](2026-06-13-oir-global-code-motion.md) | proposed | `not used yet` | 2026-06-13 |
| [Const Array Formula Synthesis](2026-06-13-const-array-formula-synthesis.md) | proposed | `not used yet` | 2026-06-13 |
| [Affine Recurrence and LSR Linkup](2026-06-13-affine-recurrence-lsr-linkup.md) | proposed | `not used yet` | 2026-06-13 |
| [OIR If-conversion and Select](2026-06-13-oir-if-conversion-select.md) | ready_for_review | `task/oir-if-conversion-select` | 2026-06-15 |
| [At-most-once Inline and Recursion Follow-up](2026-06-13-at-most-once-inline-recursion.md) | proposed | `not used yet` | 2026-06-13 |
| [OIR Loop Bound Tightening](2026-06-12-oir-loop-bound-tightening.md) | ready_for_review | `task/oir-loop-bound-tightening` | 2026-06-12 |
| [Transpose2 and T03-61 Performance Gap Attribution](2026-06-12-transpose2-t03-61-perf-gap.md) | ready_for_review | `master` | 2026-06-12 |
| [Huffman Follow-up Performance Regression Fix](2026-06-09-huffman-followup-regression-fix.md) | ready_for_review | `task/huffman-perf-gap` | 2026-06-09 |
| [Huffman Performance Gap Plan](2026-06-09-huffman-perf-gap.md) | ready_for_review | `task/huffman-perf-gap` | 2026-06-09 |
| [Pointer Loop Exit Regression Fix](2026-06-08-pointer-loop-exit-regression.md) | ready_for_review | `task/pointer-loop-exit-regression` | 2026-06-08 |
| [Many Mat Cal Loop Fixes](2026-06-08-many-mat-cal-loop-fixes.md) | ready_for_review | `task/many-mat-cal-loop-fixes` | 2026-06-08 |
| [Many Mat Cal Performance Gap Attribution](2026-06-08-many-mat-cal-perf-gap.md) | ready_for_review | `master` | 2026-06-08 |
| [RISC-V Medany Code Model](2026-06-08-riscv-medany-code-model.md) | ready_for_review | `master` | 2026-06-08 |
| [MIR Perf Diagnostics](2026-06-07-mir-perf-diagnostics.md) | ready_for_review | `mir++` | 2026-06-07 |
| [MIR CFG Copy Cleanup](2026-06-07-mir-cfg-copy-cleanup.md) | proposed | `task/mir-cfg-copy-cleanup` | 2026-06-07 |
| [MIR Branch Combine](2026-06-07-mir-branch-combine.md) | proposed | `tasksys` | 2026-06-07 |
| [MIR Global CSE LICM](2026-06-07-mir-global-cse-licm.md) | ready_for_review | `tasksys` | 2026-06-07 |
| [OIR Memzero MIR Lowering](2026-06-07-oir-memzero-mir-lowering.md) | proposed | `mir++` | 2026-06-07 |
| [MIR RA PostRA Cleanup](2026-06-07-mir-ra-postra-cleanup.md) | proposed | `task/mir-ra-postra-cleanup` | 2026-06-07 |
| [MIR List Scheduler](2026-06-07-mir-list-scheduler.md) | proposed | `mir++` | 2026-06-07 |
| [MIR Small If Conversion](2026-06-07-mir-small-if-conversion.md) | proposed | `task/mir-small-if-conversion` | 2026-06-07 |
| [Range Gated Arith Idioms](2026-06-07-range-gated-arith-idioms.md) | proposed | `task/range-gated-arith-idioms` | 2026-06-07 |
| [MIR Perf Integration](2026-06-07-mir-perf-integration.md) | ready_for_review | `mir++` | 2026-06-07 |

## Shared MIR Perf Gates

Use these commands when a MIR optimization task needs shared performance evidence:

```bash
xmake
python3 scripts/run_tests.py --suite filecheck --filter mir_vregs --jobs 1
python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter test/easy/basic.sy --jobs 1 --o1
PERF_TEST_DIRS=test/performance PERF_MAX_CASES=3 python3 scripts/compare_perf.py
PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py
```

`scripts/compare_perf.py` writes `build/perf-ci/perf-report.md` and
`build/perf-ci/perf-report.json`. The report includes MIR stage totals,
per-row `codegen_metrics.mir_stages`, and a slow compiler cases table.
