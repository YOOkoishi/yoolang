# yoolang docs

本目录保存面向开发者的设计文档和任务流程文档。

- [yoolang-design.md](yoolang-design.md): 编译器总设计，包含系统架构、模块划分、各层 IR、Pass、优化策略、后端与持续集成。
- [poly.md](poly.md): YIR 多面体优化设计摘要。
- [ci(1).md](ci%281%29.md): 持续集成与性能分析设计摘要。
- [task-system.md](task-system.md): Markdown 任务系统，规定后续任务如何限缩上下文、拆小 patch、使用 worktree、运行 IR verifier 和自动测试。
- [tasks/TEMPLATE.md](tasks/TEMPLATE.md): 每个任务复制使用的 Markdown 任务记录模板。
- [yir-design.md](yir-design.md): YIR 结构化 IR 设计说明。
- [mir-design.md](mir-design.md): RISC-V MIR 设计说明。
- [polyhedral.md](polyhedral.md): polyhedral pipeline 设计说明。
- [cost-model-design.md](cost-model-design.md): 面向 OIR/MIR、SMT、partial evaluation 和 e-graph 的 cost model 设计说明。
- [egraph-design.md](egraph-design.md): OIR 局部 equality saturation、proof、cost-model extraction 和分阶段落地设计。
