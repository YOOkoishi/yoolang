# Task: Yoolang Compiler Design Documentation

Status: ready_for_review
Created: 2026-08-05
Last update: 2026-08-05
Owner: Codex
Branch: `master` working tree
Base commit: `e69beaf`

## Goal

面向竞赛评委，在 `docs/yoolang-design.md` 中简洁说明 yoolang 的系统架构、模块划分和优化策略；逐层覆盖 YIR、OIR、MIR/ASM Pass 及其作用，并把 `docs/poly.md` 与 `docs/ci(1).md` 的核心设计纳入正文。

## Non-goals

- 不改变编译器行为、pass 顺序、构建配置或测试策略。
- 不修改队友提供的 `docs/poly.md`、`docs/ci(1).md` 原文。
- 不处理工作区中既有的 `docs/AI_USAGE.md` 修改和 `docs/smt-solver-review-zh.md` 删除。

## Affected Pipeline

- [x] Docs only
- [ ] Parser / AST
- [ ] YIR
- [ ] OIR
- [ ] MIR
- [ ] ASM
- [ ] Runtime
- [ ] Test infrastructure
- [ ] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: `docs/poly.md`, `docs/ci(1).md`, `docs/yir-design.md`, `docs/mir-design.md`, `docs/polyhedral.md`
- Source/script anchors: driver/pipeline入口、各层 pass 注册/聚合入口、构建清单和 CI/测试入口；为满足“逐个 pass”要求，按注册点定位后读取每个已接入 pass 的声明或实现，非关键实现记为 `keep=no`
- Large-file rule: 先用 `rg` 定位类、工厂函数、pipeline 调用和注释，仅在作用无法从局部确定时扩大阅读范围

Do not read unless explicitly needed:

- `build/`、性能运行产物、第三方依赖和测试输入正文
- 与编译器设计无关的历史任务记录

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/yoolang-design.md` | full | target document | yes | initially empty |
| `docs/poly.md` | full | teammate polyhedral module text | yes | preserve and integrate |
| `docs/ci(1).md` | full | teammate CI module text | yes | preserve and integrate |
| `/home/yoo/Documents/blog/src/content/blog/IR设计与编译优化.md` | IR design section | judge-facing IR demo reference | no | retained the shared-example structure, not the long Collatz listing |
| `test/ir/analysis_loop.sy` | full and live emits | compact shared YIR/OIR/MIR example | yes | current printer output verified for all three layers |
| `docs/image/*.png` | image metadata and visual inspection | architecture and CI evidence for the judge-facing document | yes | three user-provided images renamed to stable descriptive paths |
| `src/main/PipelineBuilder.cpp` | full / pipeline calls | active end-to-end pipeline | yes | live source of truth |
| `src/pass/yir/` | registrations and pass bodies | complete YIR pass inventory | no | 14 public classes plus active/internal and unreachable strategies |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | full / pass calls | active OIR pass order | yes | live source of truth |
| `src/pass/oir/` | registrations and pass bodies | complete OIR pass inventory | no | 21 public classes and all aggregate helper entrypoints |
| `src/pass/mir/` | registrations and pass bodies | complete MIR/RA/ASM pass inventory | no | 19 public classes plus LICM/pointer-loop helpers |
| `xmake.lua`, `.github/workflows/`, `scripts/` | targeted queries | modules, build and CI design | no | live CI differs from teammate roadmap on fuzz/hotspot coverage |

## Branch

Decision: not used

Reason:

```text
文档-only 任务，且工作区已有用户未提交内容；留在当前 master 工作树，避免切分支影响现有文件。
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
```

## Invariants And Risks

Correctness invariants:

- pass 名称、启用条件、顺序和作用必须能追溯到当前源码；提案或历史任务不得冒充已启用功能。
- 多面体与 CI 模块保留原始设计要点，但不出现来源核对、内部任务或交接语气。

Contest / compliance constraints:

- 文档只描述通用、语义保持的优化，不把测例身份或已知输出视为合法优化条件。

Risk areas:

- 聚合 pipeline 中的重复 pass、条件 pass、内部子阶段容易漏记或误称为独立 pass。
- 旧设计文档可能与当前实现不同，必须以 live source 为准。
- O0 与 O1、PreRA 与 PostRA、IR lowering 与优化 pass 必须明确区分。

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | 建立源码事实清单和逐层 pass inventory | task record | registration/source cross-check | complete | 58 个公开 Pass 类；另核对所有聚合 helper |
| P2 | 编写总设计并整合 poly/CI 模块 | `docs/yoolang-design.md` | Markdown/link/source-name checks | complete | 初版完整技术审计稿 |
| P3 | 更新文档索引与任务 handoff | `docs/README.md`, task docs | `git diff --check` | complete | 总设计已加入 docs 索引，任务记录已收尾 |
| P4 | 重写为面向评委的精简设计 | `docs/yoolang-design.md`, `docs/README.md` | Pass/措辞/Markdown 检查 | complete | 860 行压缩为 222 行，完整 Pass 表移至附录 |
| P5 | 为三层 IR 增加同一求和循环的结构节选 | `docs/yoolang-design.md` | live emit、focused FileCheck、Markdown 检查 | complete | 仅在 4.1–4.3 展示 Region、SSA/CFG 和 RISC-V MIR 的关键差异 |
| P6 | 重命名并插入架构与 CI 图片 | `docs/yoolang-design.md`, `docs/image/*.png` | image metadata、link target、Markdown 检查 | complete | 架构图替换重复 Mermaid；CI 通知与历史索引图放入 3.2 |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | no | SKIP | docs-only，不改变编译器或构建输入 |
| Source pass coverage | `rg` 提取 `include/pass` 类并逐名查文档；核对 OIR aggregate API | yes | PASS | 58/58 public Pass classes；34/34 OIR transform APIs（`local_simplify` 含两种 mode） |
| Poly/CI integration | review module summaries against source documents | yes | PASS | 保留建模、依赖、变换、构建、性能比较与报告等核心设计 |
| IR demo syntax | live `--emit-yir/--emit-oir/--emit-mir` on `test/ir/analysis_loop.sy`; focused FileCheck | yes | PASS | 三段节选与当前 printer 一致；FileCheck 1 passed |
| Image assets | visual inspection、`file docs/image/*.png`、Markdown target scan | yes | PASS | 3 张 PNG 内容、命名、插入位置和相对路径均已核对 |
| Judge-facing wording | 扫描内部审计、交接和 AI 相关措辞 | yes | PASS | 总设计未检出内部工作措辞 |
| Markdown links/structure | local target scan；代码围栏计数 | yes | PASS | 图片目标均存在；10 个围栏组成 5 个完整代码块 |
| Diff whitespace | `git diff --check`; untracked files with `git diff --no-index --check` | yes | PASS | 无 trailing whitespace 或 whitespace error |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| 逐字复制 poly/CI 并附实现核对 | 信息完整，但评委版重复且带有内部审计口吻 | rejected；将准确的核心设计直接改写进模块正文 |
| 只依据旧设计文档整理 | 更快，但无法保证当前 pass 完整性 | rejected；源码注册和调用点优先 |

## Change Log

- 2026-08-05: 创建任务记录，确认文档-only 范围、当前工作区保护策略和源码核对方法。
- 2026-08-05: 完成三层 IR、MIR/ASM、cost/SMT 与 CI 源码清单，写入总设计并进入 verifying。
- 2026-08-05: 通过 Pass/内部 API 覆盖、队友正文、链接、围栏和 whitespace 检查；状态改为 ready_for_review。
- 2026-08-05: 根据评委阅读场景重写总设计，删除来源核对、内部状态和维护提示，压缩正文并进入 verifying。
- 2026-08-05: 完成评委版终审；58/58 个公开 Pass、措辞、链接、围栏和 whitespace 检查全部通过。
- 2026-08-05: 参照博客的同程序逐层展示方式，为 YIR/OIR/MIR 补充 `sum_to` 核心结构节选，并以当前编译器输出和 focused FileCheck 核验语法。
- 2026-08-05: 将三张新增图片重命名为稳定英文路径；架构图替换重复 Mermaid，CI 通知与历史报告图插入持续集成模块。

## Open Questions

- 无。工作区实际文件名为 `docs/ci(1).md`；保留该文件并在总设计/索引中使用编码链接，避免未经授权移动队友文件。

## Handoff Note

Current state:

- 评委版总设计已完成并通过全部公开 Pass 覆盖、评委口吻和 Markdown 检查。
- 第 4 节已用一个紧凑求和循环展示 YIR Region、OIR SSA/CFG 和 RISC-V MIR 的降层差异。
- 系统架构与持续集成模块已加入 3 张评委可直接查看的架构/CI 图片。
- 已保留工作区原有 `docs/AI_USAGE.md` 修改和 `docs/smt-solver-review-zh.md` 删除。

Next action:

- 交由评审；如需进一步压缩，可仅调整正文，不删除附录中的 Pass 覆盖。

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-08-05-yoolang-compiler-design.md`
- `docs/yoolang-design.md`
- `src/main/PipelineBuilder.cpp`
