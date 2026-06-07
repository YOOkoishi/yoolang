# Markdown 任务系统

这个系统的目标是把每个开发任务变成一个可恢复的 Markdown 记录。上下文窗口不够时，下一轮只读任务文件和它列出的锚点文件，就能继续推进；不需要重新扫描整个仓库。

适用范围：

- 优化、IR、后端、测试基础设施、文档等所有非一次性任务。
- 需要多轮 Codex 协作的任务。
- 任何可能触碰 YIR、OIR、MIR、ASM、runtime 或测试脚本的任务。

不适用范围：

- 只需要回答一个简单事实的问题。
- 只运行一个用户明确给出的命令且不产生代码变更的任务。

## 文件布局

```text
docs/
  task-system.md          # 本规范
  tasks/
    README.md             # 任务目录说明
    TEMPLATE.md           # 新任务模板
    YYYY-MM-DD-slug.md    # 具体任务记录
```

任务文件命名使用创建日期和短横线 slug，例如 `docs/tasks/2026-06-07-loop-lsr.md`。

任务文件是任务的唯一长期记忆。聊天记录、终端输出和临时分析都不能替代它。每次推进任务后，都要把会影响下一轮的信息写回任务文件。

## 任务状态

任务文件顶部的 `Status` 使用以下状态之一：

- `proposed`: 任务刚创建，目标和边界还没有确认。
- `scoped`: 已经确定目标、非目标、上下文预算和验证门槛。
- `in_progress`: 正在实现一个或多个小 patch。
- `verifying`: 代码改动完成，正在跑 verifier、测试或性能检查。
- `blocked`: 当前无法继续，任务文件必须写明阻塞条件和需要的输入。
- `ready_for_review`: 实现和必要验证完成，等待审阅或合并。
- `done`: 已合并或用户明确接受。

状态变化必须伴随一条 `Change Log` 记录。

## 固定流程

每个非平凡任务按以下顺序推进。

1. 创建任务文件。
   从 `docs/tasks/TEMPLATE.md` 复制，填入标题、日期、目标、非目标、影响范围、预期验证。没有任务文件时，不开始大范围阅读。

2. 设定上下文预算。
   先写清楚本轮最多读取哪些文件、每个文件为什么值得读、哪些目录不读。默认初始预算是：
   - 任务文件本身。
   - 本规范。
   - 最多 2 个相关设计文档。
   - 最多 8 个源码或脚本锚点文件。
   - 每个大文件先读相关区间，不整文件阅读；需要整文件时先在任务文件里说明理由。

3. 用 `rg` 建立候选文件集。
   搜索只用于定位锚点，不用于无限扩张上下文。每次搜索后，把保留的锚点写到 `Context Ledger`，把丢弃原因写到备注。

4. 选择 worktree 策略。
   侵入式任务或需要并行尝试时，使用 Git worktree；文档或极小修复可以留在当前工作区。无论哪种，都要记录 base commit、branch、worktree path 和 `git status --short`。

5. 写 patch 计划。
   每个 patch 只解决一个行为点。先写 `Patch Queue`，再编辑文件。

6. 执行小 patch 循环。
   每个 patch 循环都按 `intent -> edit -> local verifier -> focused tests -> record` 进行。一个 patch 默认不超过 3 个源码文件和约 300 行净改动；超过就拆分。

7. 跑 IR verifier 和自动测试。
   根据影响范围选择最窄的 verifier/test gate。任何跳过的 gate 都要在任务文件里写明原因。

8. 写 handoff。
   每次停下前更新 `Handoff Note`，包含当前结论、已验证内容、下一步、只需继续阅读的文件列表。

## 上下文预算规则

上下文预算是本系统的核心。每个任务都要在模板中维护 `Context Ledger`：

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `src/pass/oir/FooPass.cpp` | `120-260` | 修改目标 pass | yes | 下一轮必读 |
| `src/oir/OIR.cpp` | `rg "verify_module"` | 确认 verifier API | no | 只需知道 API 存在 |

规则：

- 先搜后读：优先 `rg --files` 和 `rg -n`，避免目录级浏览。
- 先读窄区间：用 `sed -n` 或文件链接定位相关函数。
- 读过的文件必须分类为 `keep` 或 `drop`。
- 下一轮只读 `keep=yes` 的文件和 `Handoff Note` 指定的文件。
- 如果一个任务需要新增超过 8 个 `keep=yes` 源码锚点，先拆任务。

推荐的阅读层级：

```text
任务文件
  -> 相关 docs 设计文档
  -> pass/pipeline 入口
  -> 被修改文件
  -> verifier/test 脚本
```

## Git worktree 规则

worktree 用来隔离多轮实验，避免上下文中断时污染主工作区。

创建前：

```bash
git status --short
git rev-parse --short HEAD
```

常规创建：

```bash
git worktree add ../yoolang-<slug> -b task/<slug>
```

使用已有分支：

```bash
git worktree add ../yoolang-<slug> task/<slug>
```

规则：

- worktree 路径不要放在仓库内部。
- 每个任务文件只记录一个主 worktree；实验性分支写在 `Alternatives`。
- 不在没有记录的 worktree 中修改代码。
- 合并或丢弃 worktree 前，任务文件必须记录最终 branch、commit 或 diff 状态。

文档-only 或单文件小修可以不创建 worktree，但要在 `Worktree` 节写明 `not used` 和原因。

## 小 patch 规则

小 patch 的目的是让每一步都能独立验证和回滚。

一个 patch 应满足：

- 只改变一个语义点或一个文档点。
- 源码文件默认不超过 3 个；测试文件可以跟随一个源码 patch。
- 净改动默认不超过约 300 行。
- 有明确 invariant，例如“这个变换只在无副作用表达式上进行”。
- 有对应的 verifier 或测试命令。

如果发现 patch 需要横跨多个 IR 层，拆成：

1. IR 数据结构或 verifier 扩展。
2. pass 或 lowering 行为变化。
3. FileCheck 或 e2e 覆盖。
4. 性能验证或清理。

每个 patch 完成后，在 `Patch Queue` 里记录结果和下一步。

## IR verifier gate

本仓库已有多个 verifier 入口。任务系统不重复实现 verifier，而是把它们作为 gate 固定下来。

YIR：

- API: `include/yir/YIRVerifier.h` 中的 `yir::verify_high_level_yir`。
- 已接入：AST 到 YIR、polyhedral canonicalize、polyhedral transform 等路径。
- 常用命令：

```bash
python3 scripts/run_tests.py --suite stage --stage yir --filter <case> --jobs 1
python3 scripts/run_tests.py --suite stage --stage yir --filter <case> --jobs 1 --o1
```

OIR：

- API: `include/oir/OIR.h` 中的 `oir::Module::verify` 和 `oir::Verifier::verify_module`。
- `--emit-oir` 和部分 OIR pass 已使用模块验证。
- 常用命令：

```bash
python3 scripts/run_tests.py --suite stage --stage oir --filter <case> --jobs 1
python3 scripts/run_tests.py --suite stage --stage oir --filter <case> --jobs 1 --o1
```

MIR：

- API: `include/mir/MIRVerifier.h` 中的 `mir::verify_module`。
- 支持 `PreRA` 和 `PostRA` 阶段，OIRToMIR、MIR combine、regalloc、peephole 等路径已接入。
- 常用命令：

```bash
python3 scripts/run_tests.py --suite stage --stage mir --filter <case> --jobs 1
python3 scripts/run_tests.py --suite stage --stage mir --filter <case> --jobs 1 --o1
python3 scripts/run_tests.py --suite stage --stage asm --filter <case> --jobs 1 --o1
```

选择 gate 的原则：

- 改 YIR pass，至少跑 YIR stage；如果会 lower 到 OIR，继续跑 OIR stage。
- 改 OIR pass，至少跑 OIR stage；如果会影响后端，继续跑 MIR 和 ASM stage。
- 改 MIR 或寄存器分配，至少跑 MIR、ASM stage 和相关 e2e。
- 改 parser、AST、类型系统或 runtime，跑 stage 和 e2e，不只看 FileCheck。

## 自动测试 gate

默认测试从窄到宽：

```bash
xmake
python3 scripts/run_tests.py --suite filecheck --filter <case-or-pass> --jobs 1
python3 scripts/run_tests.py --suite stage --suite e2e --filter <case-or-dir> --jobs 1 --o1
python3 scripts/run_tests.py --suite filecheck --suite poly --jobs 1
python3 scripts/run_tests.py --suite stage --suite e2e --jobs 1 --o1
python3 scripts/run_tests.py --build --suite all --jobs 1 --o1
```

性能相关任务再加：

```bash
PERF_TEST_DIRS=<focused-case-or-dir> python3 scripts/compare_perf.py
PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py
```

记录规则：

- 每个命令都写入 `Verification Matrix`。
- `PASS`、`FAIL`、`SKIP`、`NOT_RUN` 必须区分。
- `SKIP` 要写原因，例如缺少 `qemu-riscv64` 或 RISC-V gcc。
- 性能任务必须记录 `build/perf-ci/perf-report.md` 或 `perf-report.json` 的结论。

## 任务恢复协议

下一轮 Codex 或开发者恢复任务时，只做以下动作：

1. 读取 `docs/task-system.md`。
2. 读取对应任务文件。
3. 读取 `Handoff Note` 中列出的 `keep=yes` 锚点文件。
4. 运行任务文件中的第一个待办命令或继续下一个 patch。

禁止恢复时重新全仓库浏览，除非任务文件的 `Open Questions` 明确要求重新定位。

## 完成定义

任务可以标记为 `ready_for_review` 的条件：

- 目标和非目标已满足。
- 所有 patch 都有记录。
- 相关 verifier gate 已通过或跳过原因已记录。
- 相关自动测试已通过或跳过原因已记录。
- `Handoff Note` 写明最终状态。

任务可以标记为 `done` 的条件：

- 用户接受、commit 已落地或任务明确不需要合并。
- 任务文件记录最终 commit 或 final diff 状态。
