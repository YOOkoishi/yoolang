# 持续集成与性能分析设计

yoolang 基于 GitHub Actions 和自托管 Linux runner 实现了持续集成与性能分析 pipeline. 我们将 xmake release 构建、`-O1` 功能测试、RISC-V 交叉编译和 QEMU 端到端运行串联起来。考虑到校园网和自托管环境不够稳定，CI checkout 支持 SSH 443、HTTPS 和源码归档多级回退；每次运行前也会清理旧报告，并检查工具链和运行时库，避免旧产物影响本次结果。

性能评测部分以 GCC、Clang 的 RISC-V 优化结果和 main baseline 为参照。我们统一了测试输入、运行时库和计时口径，并统计逐测例耗时、总时间加速比、几何平均加速比和胜负情况。当前提交会自动和 main baseline 对比，加速比按“参照项耗时 / 当前项耗时”计算，同时记录基线提交、分支和运行信息。流程中也加入了差分 fuzz、失败 diff、超时和长错误截断；如果报告缺失或测例失败，CI 会保留错误信息并标记为失败。

除了运行时间，我们还接入了支持 QEMU TCG plugin 的 `qemu-riscv64`，对 yoolang、GCC 和 Clang 生成的同架构程序统计逐测例动态指令数，并比较当前提交与 main baseline. 在此基础上，系统会根据 QEMU translation block 的执行信息生成函数级火焰图和交互式 guest TB 热点图，再将热点地址对应到函数符号和汇编基本块，方便查看时间主要消耗在哪里，以及 yoolang 和其他编译器的生成代码有何差距。

我们的报告系统会把功能测试、运行时间、动态指令数和热点分析输出为 JSON、Markdown 和静态 HTML，再通过 GitHub Actions Summary、Discord 通知和 GitHub Pages 发布。页面按分支和提交保留历史报告，显示 commit、branch 和提交人信息，并支持测例搜索、指标排序、异常筛选、可选指标和 Nord 深色模式。优化提交完成后，可以直接从性能报告、指令数报告和历史索引查看是否变快，以及具体慢在哪些测例和函数中。
