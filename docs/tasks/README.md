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

| Task | Status | Worktree | Last update |
| --- | --- | --- | --- |
| [RISC-V Medany Code Model](2026-06-08-riscv-medany-code-model.md) | ready_for_review | `.` | 2026-06-08 |
| [MIR Perf Diagnostics](2026-06-07-mir-perf-diagnostics.md) | ready_for_review | `.` | 2026-06-07 |
| [MIR CFG Copy Cleanup](2026-06-07-mir-cfg-copy-cleanup.md) | proposed | `../yoolang-mir-cfg-copy-cleanup` | 2026-06-07 |
| [MIR Branch Combine](2026-06-07-mir-branch-combine.md) | proposed | `../yoolang-mir-branch-combine` | 2026-06-07 |
| [MIR Global CSE LICM](2026-06-07-mir-global-cse-licm.md) | ready_for_review | `.` | 2026-06-07 |
| [OIR Memzero MIR Lowering](2026-06-07-oir-memzero-mir-lowering.md) | proposed | `../yoolang-oir-memzero-mir-lowering` | 2026-06-07 |
| [MIR RA PostRA Cleanup](2026-06-07-mir-ra-postra-cleanup.md) | proposed | `../yoolang-mir-ra-postra-cleanup` | 2026-06-07 |
| [MIR List Scheduler](2026-06-07-mir-list-scheduler.md) | proposed | `../yoolang-mir-list-scheduler` | 2026-06-07 |
| [MIR Small If Conversion](2026-06-07-mir-small-if-conversion.md) | proposed | `../yoolang-mir-small-if-conversion` | 2026-06-07 |
| [Range Gated Arith Idioms](2026-06-07-range-gated-arith-idioms.md) | proposed | `../yoolang-range-gated-arith-idioms` | 2026-06-07 |
| [MIR Perf Integration](2026-06-07-mir-perf-integration.md) | ready_for_review | `.` | 2026-06-07 |

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
