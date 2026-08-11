# Performance 派生的 RVV 正确性测例

本目录保存基于 `test/performance` 场景和当前 O2/O3 向量化特性整理的
独立正确性测例。记录基于 `master` 提交 `4403552`，验证环境为
`806-os`、RVV 1.0 和 QEMU 8.2.2。

## O2/O3 RVV 测例与验证

这不是只看编译器日志的“看起来通过”。正常集合的证据链是：

`SysY 源码 → O2/O3 向量计划 → RVV 汇编 → GNU 静态链接 ELF → objdump 解码 → QEMU 多 VLEN 执行 → 独立期望值校验`

其中几个主要算术测例使用预先写死的期望数组，避免用与被测循环完全相同的
表达式作为 oracle。统一脚本同时检查向量化计划、解码后的 RVV 指令和程序
退出码；任一环节不符合预期都会非零退出。

在 Linux/RISC-V 工具链环境执行以下命令，会在指定目录下新建一个不会覆盖
旧结果的 `run-*` 证据目录：

```bash
python3 scripts/rvv_performance_generated_cases_infra_tests.py \
  --artifacts-dir build/rvv-evidence

python3 scripts/rvv_performance_known_failures.py \
  --artifacts-dir build/rvv-known-failures
```

第一个目录保留每个用例 O2/O3 的 `.s`、实际执行的 `.elf`、仅 `main`
的反汇编、向量计划 JSON、SHA-256 和汇总
`report.md/report.json`。第二个目录以同样方式保留四个已知失败及其正确的
O0/O1/标量控制组；脚本打印 `XFAIL` 是“问题被成功复现”，不是把错误冒充
成通过。

通过 rsync 在远端镜像验证时，应显式传入本地基线提交，避免远端未同步的
`.git` 元数据污染报告；每个源码和门禁脚本的 SHA-256 仍会单独记录：

```bash
YOOLANG_SOURCE_REVISION="$(git rev-parse HEAD)" \
  python3 scripts/rvv_performance_generated_cases_infra_tests.py \
  --artifacts-dir build/rvv-evidence
```

## 目录与当前结论

- `passing/`：23 个当前可以通过的 `.sy`。统一门禁会检查向量化计划、
  链接后 ELF 的反汇编，以及 QEMU 执行结果。
- `known_failures/`：4 个当前无法通过的 `.sy`。它们保留真实语义，
  不会为了通过测试而弱化，也不会混入正常通过集合。
- `../../scripts/rvv_performance_generated_cases_infra_tests.py`：正常集合的
  汇编和多 VLEN QEMU 门禁。
- `../../scripts/rvv_performance_known_failures.py`：四个问题的统一复现器，
  同时运行能通过的控制配置，防止把测例本身的问题误判为 RVV 问题。

正常集合已完成 O2/O3 × VLEN 128/256/512/1024，共 184 次 QEMU 执行。
默认 `rv64gc` 的 O2/O3 也分别执行了 23 个测例。上述结果均为通过。

## 这批证据能证明什么

- 当前后端已经能执行这 20 个正向模式涉及的连续/部分跨步访存、整数与浮点
  向量算术、归约、diamond if-conversion、O3 SLP 和 interleave；在这些模式
  上不改后端也能完成源码到 QEMU 的闭环。
- 3 个拒绝控制说明“没有生成 RVV”也受检查：当前不支持的非规范循环、嵌套
  clamp 和过短循环会保持标量且结果正确，不会被 PASS 文案掩盖。
- 它不能证明“决赛出现任何向量化程序都没问题”。当前至少有两类真实边界：
  显式 masked/indexed memory 在优化 OIR 崩溃，`i*2`/`i+i` stride-2 循环会被
  错判为 unit stride 并算错。后者已经定位在 SCEV/LoopAccessAnalysis，前者
  位于优化 OIR 的 operand/lifetime 维护；现有证据都不支持归咎于 RVV 汇编
  后端。

所以可以对队友作出的严谨结论是：**现有后端对已覆盖的 RVV 指令链路可用，
但当前编译器不能宣称完整支持所有向量化场景。** 如果决赛前不改后端，仍可
通过在 OIR 分析/变换处保守拒绝危险 stride-2 形式、让未支持形态走标量路径
来守住正确性；若要支持显式 masked/indexed 或正确向量化该 stride-2 形式，
仍需修复前端/OIR 分析，而不是靠增加测试绕过。

统一运行命令：

```bash
python3 scripts/run_tests.py \
  --suite infra \
  --infra-profile toolchain \
  --filter rvv_performance_generated_cases \
  --jobs 1 \
  --fail-on-skip
```

单独运行源码集合：

```bash
python3 scripts/run_tests.py \
  --suite e2e \
  --test-root test/rvv_performance_generated/passing \
  --march rv64gcv \
  --qemu-cpu rv64,v=true,vlen=128,elen=64 \
  --o2 --jobs 1 --require-e2e-tools --fail-on-skip
```

将 `--o2` 改为 `--o3` 可验证 O3。多 VLEN 和汇编指令断言由统一 infra
脚本负责。

## 当前无法 pass 的 4 个 `.sy`

### `explicit_masked_memory_optimized_oir_crash.sy`

- 当前结果：`--emit-yir` 正常；`--emit-oir -O1/-O2/-O3` 均以 139
  退出，即 SIGSEGV。O0 能编译、链接，并在四种 VLEN 下返回 0，因此
  语法、类型和基本运行语义成立；问题只在启用优化后出现。
- 原因分析：debug 栈停在
  `FunctionModRefAnalysis::scan_function(VPLoad)` → `add_pointer_effect` →
  `OIRAliasAnalysis::memory_location` 的 `dynamic_cast`，调用者是第二轮
  `promote_global_loads`。这说明某个更早的标量 OIR transform 留下了失效的
  vector-memory pointer operand，ModRef 再次扫描时触发 use-after-free/悬空
  引用。当前已定位到优化 OIR 的 operand/lifetime 维护，尚未证明究竟是哪
  一个更早 pass 删除了该值，不能直接归咎于后端。
- 测例自身边界：它足以证明“优化不能崩溃”，但当前只检查 `output[0]`，
  尚不足以在修复后证明所有 active/inactive lane 的完整 mask 语义。
- To-do：在每个内部 OIR transform 后运行 verifier 以找到第一个破坏 use
  链的 pass；检查 GEP 删除和 replacement 是否覆盖 `VPLoad/VPStore`；修复
  后补查全部 active lane、inactive lane 不写和 passthrough，再增加
  OIR/MIR/ASM/QEMU 门禁并移入 `passing/`。

### `explicit_indexed_mask_optimized_oir_crash.sy`

- 当前结果：显式 `gather/scatter` 能生成 YIR，但 O1/O2/O3 的优化 OIR
  同样 SIGSEGV，退出码 139；O0 在四种 VLEN 下均返回 0。
- 原因分析：debug 栈与上一个复现相同，但入口是
  `FunctionModRefAnalysis::scan_function(VPGather)`。因此两者可以归为
  “标量 OIR 优化没有完整维护 VP memory pointer operand 生命周期”这一类，
  但 gather/scatter 还多出 indices、mask、EVL 和 passthrough，仍需分别
  验证，不能因栈相同就假设所有修复点完全一致。
- 测例自身边界：当前只检查一个 active index，适合作为崩溃复现；修复后
  必须检查所有 active index 的值、inactive index 未写和非索引位置未写。
- To-do：增加仅 gather 和仅 scatter 的最小复现；在 OIR replacement/DCE
  中检查所有 VP operands；修复后要求 `vloxei32.v/vsoxei32.v`，并完成
  active/inactive、越界屏蔽和多 VLEN 验证。

### `stride2_load_contiguous_store_wrong_result.sy`

- 当前结果：O2/O3 向量计划报告 `VECTORIZED`，但输入 17 时在
  VLEN 128/256/512/1024 都返回 11；O0 RVV 和 `rv64gc` 标量结果正确。
- 原因分析：源码读取 `input[i * 2]`，O1 OIR 中下标被规范化为
  `%v4.mul2 = add %i.loop, %i.loop`。`SCEVExpr::add` 处理 AddRec+AddRec 时
  把右侧 recurrence 塞进 start，却仍只保留左侧 step=1；随后
  `pointer_stride` 只读取顶层 AddRec step，错误报告 stride=1。O2 OIR 因此
  已生成 `vp.load`，最终反汇编自然成为 `vle32.v`，而正确选择应为
  `vp.gather`/`vlse32.v`。错误在 SCEV/LoopAccessAnalysis，早于后端。
- 测例自身检查：输入 17 时最大读取下标 32，小于数组长度 34；所有输出
  lane 都参与校验，无越界和未定义行为。RVV O0/O1 与 `rv64gc` O2/O3
  全部返回 0，进一步排除期望值错误。
- To-do：正确合并同一 loop 的 AddRec start 和 step，或在不能证明时返回
  unknown 而不是 stride=1；为 `i+i`、`2*i`、`i*2` 增加 SCEV 单测；在
  OIR 要求 indexed recipe，在 ASM 要求 `vlse32.v`；修复前拒绝这种形式。

### `stride2_store_wrong_result.sy`

- 当前结果：O2/O3 RVV 结果错误。输入 17 时，VLEN 128 首个错误为
  lane 8，VLEN 256/512/1024 首个错误为 lane 9；O0、O1 RVV 和 O2
  `rv64gc` 控制均正确。
- 原因分析：它同样经过上述错误的 `i+i` SCEV 路径，但 load/store 同时
  误分类后会互相掩盖部分错误，首个可见失败才随 VLEN 出现在 lane 8/9。
  因而它是组合回归，不应单独用来判断是哪一侧错误；上一个“stride-2
  load + contiguous store”才是定位 load 分类错误的主复现。
- 测例自身边界：最大 load/store 下标均为 32，数组长度 34；RVV O0/O1
  与 `rv64gc` O2/O3 返回 0。当前只检查目标偶数位置，尚未检查奇数 guard
  是否被误写，所以它能证明结果错误，但修复后的完整写边界验证还需加强。
- To-do：先修复并验证单独 stride-2 load，再检查 scatter/store 的 byte
  stride、每批基址和 EVL；增加奇数位置 guard；修复前对该形式保守拒绝。

## `passing/` 中每个 `.sy` 的分析与 To-do

| `.sy` | 当前结果与原因分析 | To-do |
|---|---|---|
| `rvv_perf_explicit_mm_row.sy` | 显式 7-lane 矩阵行乘加；O2/O3 生成 `vmul.vv/vadd.vv`，四种 VLEN 正确。 | 增加接近 `i32` 上下界的数据，确认回绕语义。 |
| `rvv_perf_explicit_conv_nonlinear.sy` | 卷积后处理形态的有符号多项式和余数；生成 `vrem.vv`，负数 lane 正确。 | 增加负除数、`INT_MIN` 和不同 passthrough lane。 |
| `rvv_perf_explicit_stencil7.sy` | `iota` 加七邻域平均；生成向量除法，验证固定向量算术链。 | 增加非整除结果和负数，覆盖向零截断。 |
| `rvv_perf_explicit_fft_pointwise.sy` | FFT/NTT 点乘取模形态；生成向量乘法和 `vrem.vv`。 | 增加可能溢出的乘积，确认与标量 `i32` 一致。 |
| `rvv_perf_explicit_transpose4.sy` | 4×4 tile 通过多级 `shuffle` 转置，固定 VLEN 均正确。 | 增加 8×8 分块和跨寄存器 shuffle 压力。 |
| `rvv_perf_explicit_mask_select.sy` | 比较、mask 和 `select` 生成 `vmerge.vvm`。 | 增加全真、全假和尾部 mask 组合。 |
| `rvv_perf_o2_mm_row.sy` | 动态 17 元素矩阵行循环；O2/O3 loop vectorizer 生成连续 load/store 和乘加。 | 增加两个输出行及可能别名的参数版本。 |
| `rvv_perf_o2_conv_nonlinear.sy` | 原地多项式循环；证明同一对象逐元素 load/store 可安全向量化。 | 增加更长表达式和高寄存器压力版本。 |
| `rvv_perf_o2_reduction.sy` | 乘二后整数求和；生成 `vredsum.vs`，奇数尾部正确。 | 增加多个累加器、乘积归约和回绕边界。 |
| `rvv_perf_o2_reverse.sy` | 反向归纳变量；生成 signed strided load/store，四种 VLEN 正确。 | 增加非零终点和反向 stride-2；后者在修复前应保持拒绝。 |
| `rvv_perf_o2_diamond.sy` | 单层 if/else diamond 被 if-convert，生成比较 mask 和向量 store。 | 增加不对称分支成本及仅一侧写内存的情况。 |
| `rvv_perf_o2_bitwise.sy` | CRC/crypto 形态的 `xor/and` 原地循环，生成 `vxor.vv/vand.vv`。 | 增加逻辑/算术移位及超过 31 的动态移位量。 |
| `rvv_perf_o2_inplace_affine.sy` | 长度 29 的同基址原地乘加；别名规则允许逐元素更新，QEMU 正确。 | 增加同一 base 的不同常量偏移，验证依赖分析会拒绝危险形式。 |
| `rvv_perf_o2_four_array.sy` | 四个连续输入流和一个输出流，覆盖多 load、算术链及向量寄存器压力。 | 逐步增加数组数，覆盖压力阈值和无 spill 限制。 |
| `rvv_perf_o2_float_affine.sy` | 精确可表示的浮点乘加，生成 `vfmul.vv/vfadd.vv`，多 VLEN 正确。 | 增加 NaN、Inf、`-0.0` 和非精确舍入；确认严格 FP 规则。 |
| `rvv_perf_o2_stride4_inplace.sy` | stride-4 原地更新正确生成 `vlse32.v/vsse32.v`；未命中元素保持不变。 | 增加 stride 3、负 stride 4、非对齐基址和不同 n。 |
| `rvv_perf_o2_nonzero_tail.sy` | 当前 O2/O3 不生成 RVV 并正确标量执行；计划以 `REJECT_NON_CANONICAL_LOOP` 拒绝非零起点形态。 | 若扩展 loop canonicalization，改为要求向量化；此前保持负向门禁。 |
| `rvv_perf_o2_nested_clamp.sy` | 两层 clamp 当前正确保持标量；LV2 diamond 只支持单层 body edge，计划以 `REJECT_EARLY_EXIT` 拒绝。 | 增加多 diamond if-conversion 后再转为正向 RVV 测例。 |
| `rvv_perf_o2_short_control.sy` | 三次迭代在 O2/O3 最终汇编中无 RVV，作为成本/常量展开的负向控制。 | 增加 4、7、8 次边界，固定成本模型触发阈值。 |
| `rvv_perf_o3_slp.sy` | 四条相邻独立标量语句；O2 无 RVV，O3 SLP 生成固定向量 load/store。 | 增加不同操作混合和部分 lane 不同常量。 |
| `rvv_perf_o3_slp8.sy` | 八条相邻语句；O3 形成 8-lane SLP pack，O2 保持标量。 | 增加跨基本块 SLP 和两组相邻 pack。 |
| `rvv_perf_o3_interleave.sy` | 同一 loop 在 O2 选择 interleave=1、O3 选择 interleave=2，结果一致。 | 增加短 trip count，验证 O3 成本模型会回退到 1。 |
| `rvv_perf_o3_dual_output.sy` | 两个输出流提高 live vector 数；O2/O3 均正确，O3 选择 factor 2。 | 增加第三输出和别名参数，覆盖 interleave 压力门槛。 |

## 修复失败项时的迁移规则

修复某项后，应先确认它在 O2/O3 和所有四种 VLEN 下返回 0，并增加稳定的
OIR/MIR/ASM 断言；随后将 `.sy/.in/.out` 移到 `passing/` 并登记到统一 infra
脚本。禁止只修改期望输出、缩短输入或关闭检查来制造“通过”。
