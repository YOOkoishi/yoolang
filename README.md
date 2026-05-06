# yoolang

![yoolang](/yoolang.jpg)

## 使用方法

使用 xmake 作为构建工具.

输入

```bash
xmake
```

即可构建

```bash
compiler xxx.sy -S -o xxx.s #正常流程
compiler xxx.sy -S -o xxx.s -O1 #启用优化
```

## 测试

本仓库提供统一测试入口：

```bash
# 构建并运行全部测试套件
scripts/run_tests.py --build --suite all

# 只检查三层 IR / ASM 是否可生成
scripts/run_tests.py --suite stage

# 只运行 FileCheck 风格的 IR 结构测试
scripts/run_tests.py --suite filecheck

# 只运行 RISC-V 汇编、链接 runtime、qemu 执行、对比 .out 的端到端测试
scripts/run_tests.py --suite e2e
```

端到端测试需要 `riscv64-linux-gnu-gcc` 和 `qemu-riscv64`。当前已知语义失败列在 `test/xfail.txt`；如果某个已知失败变成通过，测试会报告 `XPASS`，应从 xfail 列表中移除。
