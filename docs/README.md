# yoolang docs

本目录保存面向开发者的设计文档和任务流程文档。

- [yoolang-design.md](yoolang-design.md): 编译器总设计，包含系统架构、模块划分、各层 IR、Pass、优化策略、后端与持续集成。
- [vector-mask-language.md](vector-mask-language.md): 已实现的源语言 fixed vector/mask 类型、表达式、intrinsic、布局和 ABI 边界。
- [extern-functions.md](extern-functions.md): 源语言 `extern` 函数原型、重复/冲突声明、variadic 限制以及 AST→YIR→OIR 保留契约。
- [vectorization.md](vectorization.md): 当前 OIR Loop/SLP 自动向量化 pipeline、VLA/VP 语义、合法性边界、成本与诊断契约。
- [rvv-support-matrix.md](rvv-support-matrix.md): 当前语言、IR、向量器、RVV 后端、ABI/部署的完成、部分支持和 fail-closed 边界。
- [standard-vector-aggregate-abi.md](standard-vector-aggregate-abi.md): fixed vector/mask 的标准 LP64D aggregate ABI、GCC/Clang 双向 oracle 与当前实现门禁。
- [psabi-vector-abi.md](psabi-vector-abi.md): staged fixed-length vector calling convention、显式 ABI_VLEN、v0/v8-v23、callee-save、ELF variant 标记与当前 tuple/GCC 发布门禁。
- [rvv-runtime-dispatch.md](rvv-runtime-dispatch.md): Linux HWCAP/hwprobe/vector-state 探测契约和 fail-closed runtime 接口。
- [rvv-fat-deployment.md](rvv-fat-deployment.md): rv64gc baseline + RVV 局部 variant 的 fat 部署、dispatcher 与当前支持边界。
- [rvv-performance.md](rvv-performance.md): 原生 RVV 硬件 Release 性能协议、corpus、证据边界、阈值和可复现命令。
- [rvv-random-differential.md](rvv-random-differential.md): scalar/RVV 多 VLEN 随机差分、smoke/extended/nightly tier、真非对齐与失败最小化/replay。
- [poly.md](poly.md): YIR 多面体优化设计摘要。
- [ci(1).md](ci%281%29.md): 持续集成与性能分析设计摘要。
- [task-system.md](task-system.md): Markdown 任务系统，规定后续任务如何限缩上下文、拆小 patch、使用 worktree、运行 IR verifier 和自动测试。
- [tasks/TEMPLATE.md](tasks/TEMPLATE.md): 每个任务复制使用的 Markdown 任务记录模板。
- [yir-design.md](yir-design.md): YIR 结构化 IR 设计说明。
- [mir-design.md](mir-design.md): RISC-V MIR 设计说明。
- [polyhedral.md](polyhedral.md): polyhedral pipeline 设计说明。
- [cost-model-design.md](cost-model-design.md): 面向 OIR/MIR、SMT、partial evaluation 和 e-graph 的 cost model 设计说明。
- [egraph-design.md](egraph-design.md): OIR 局部 equality saturation、proof、cost-model extraction 和分阶段落地设计。
