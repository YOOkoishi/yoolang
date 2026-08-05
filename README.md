# yoolang

![yoolang](/yoolang.jpg)

## 申明

本 gitlab 仓库是 github 仓库[yoolang](https://github.com/YOOkoishi/yoolang)的一个快照,如果组委会或其余参赛队需要参考git记录，请查看github仓库。

本项目开发中使用了 OpenAI Codex。AI 辅助范围包括部分编译器源代码、脚本、测试和
文档；工具、生成范围及人工复核状态详见 [AI 使用声明](docs/AI_USAGE.md)。

yoolang 总体的设计文档可以参考 [yoolang-design](docs/yoolang-design.md)

## 使用方法

使用 xmake 作为构建工具.

输入

```bash
xmake
```

即可构建

```bash
compiler xxx.sy -S -o xxx.s #功能测试流程

compiler xxx.sy -S -o xxx.s -O1 #启用优化

compiler --help #查看额外参数
```

## 测试

本仓库提供统一测试入口：

```bash
# 构建并运行全部测试套件
scripts/run_tests.py --build --suite all

# 构建并运行全部测试套件(启用优化)
scripts/run_tests.py --build --suite all --o1

# 只检查三层 IR / ASM 是否可生成
scripts/run_tests.py --suite stage

# 只运行 FileCheck 风格的 IR 结构测试
scripts/run_tests.py --suite filecheck

# 只运行 RISC-V 汇编、链接 runtime、qemu 执行、对比 .out 的端到端测试
scripts/run_tests.py --suite e2e
```

端到端测试需要 `riscv64-linux-gnu-gcc` 和 `qemu-riscv64`。当前已知语义失败列在 `test/xfail.txt`；如果某个已知失败变成通过，测试会报告 `XPASS`，应从 xfail 列表中移除。
