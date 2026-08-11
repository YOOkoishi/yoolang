## 3. 整体 Roadmap

```text
Phase 0: IR Canonicalization
  - loop-normalize
  - affine-expr-canonicalize
  - symbol-detect
  - loop-invariant-hoist-lite
  - region-structurize

Phase 1: SCoP Detection
  - scop-detect
  - scop-canonicalize
  - stmt-outline
  - scop-annotate

Phase 2: Polyhedral Model Construction
  - poly-domain-extract
  - poly-access-extract
  - poly-schedule-extract
  - poly-model-build

Phase 3: Analysis
  - poly-dependence-analyze
  - poly-dependence-simplify
  - poly-legality-check
  - poly-parallel-detect
  - poly-locality-estimate

Phase 4: Transformations
  - poly-loop-interchange
  - poly-strip-mine
  - poly-loop-tile
  - poly-loop-fusion
  - poly-scalar-replacement
  - poly-parallel-mark
  - poly-unroll-jam
  - poly-skew
  - poly-distribute

Phase 5: Code Regeneration
  - poly-schedule-apply
  - poly-codegen
  - poly-cleanup

Phase 6: Lowering
  - lower-structured-to-ssa
  - ssa-canonicalize
  - loop-unroll
  - vectorization-prep
  - lower-to-riscv
```

---

# Phase 0：IR Canonicalization

## 目标

把 L1 IR 整理成适合多面体分析的结构：

```text
- loop 结构统一
- induction variable 明确
- bounds 明确
- step 明确
- affine expression 规范化
- region 中 statement 边界清晰
```

---

## Pass 0.1：`loop-normalize`

### 功能

把各种 loop 统一成标准形式：

```text
for i = lb; i < ub; i += step
```

可以进一步限制成：

```text
for i = 0; i < N; i += 1
```

或者保留一般 affine bound：

```text
for i = affine_lb(symbols); i < affine_ub(symbols); i += const_step
```

### 输入

L1 loop IR。

### 输出

规范化后的 L1 loop IR。

### 需要处理

```text
- 统一比较方向
- 统一 step 方向
- 确保 step 为正
- 把 <= 转成 <
- 把 > / >= 转成正向 loop
- 可选：把非 unit step loop 归一化
```

### 示例

原始形式：

```c
for (i = 1; i <= N; i++)
```

归一化：

```c
for (i = 1; i < N + 1; i++)
```

---

## Pass 0.2：`affine-expr-canonicalize`

### 功能

把 affine expression 整理成标准形式。

### 示例

```text
i + (j + 3) + 2  ->  i + j + 5
2 * (i + 1) + j  ->  2i + j + 2
i + i + 3        ->  2i + 3
```

### 输出形式建议

可以用如下结构表示：

```text
AffineExpr {
  terms: [(coefficient, variable)]
  constant: int
  div_mod_terms: optional
}
```

### 说明

这个 pass 是后续分析的基础，因为：

```text
domain extraction
access extraction
dependence analysis
legality check
```

都需要比较和简化 affine expression。

---

## Pass 0.3：`symbol-detect`

### 功能

区分 polyhedral model 中的：

```text
dimensions: loop induction variables
symbols: SCoP 内不变化的参数
```

### 示例

```c
for (i = 0; i < N; i++)
  A[i] = B[i] + 1;
```

其中：

```text
dimension: i
symbol: N
```

### 输出

给 SCoP 或 loop region 添加 symbol 信息：

```text
ScopSymbols = [N, M, K]
```

---

## Pass 0.4：`loop-invariant-hoist-lite`

### 功能

识别并提升 loop-invariant expression，帮助其成为 symbol。

### 示例

```c
t = N + 1
for (i = 0; i < t; i++)
  S(i)
```

或在 IR 中将 `N + 1` 标记为 symbol expression。

### 注意

第一版可以只做分析，不一定真的移动代码。

---

## Pass 0.5：`region-structurize`（可选）

### 功能

将可以结构化的 region 转成清晰的 loop / if 嵌套。

### 什么时候需要

如果你们的 L1 本身已经是结构化 IR，可以暂时不做。

如果前端产生了较复杂的 region，这个 pass 会比较有用。

---

# Phase 1：SCoP Detection

## 目标

识别 Static Control Part，也就是可以进入多面体 pipeline 的 region。

一个 SCoP 应该满足：

```text
- loop bounds 是 affine
- if conditions 是 affine
- memory access 是 affine
- control flow 是 structured
- 不包含未知副作用
- alias 情况可控
```

---

## Pass 1.1：`scop-detect`

### 功能

扫描 L1 region，找出可以多面体优化的区域。

### 输出方式

方式 A：添加属性

```text
region @R { poly.scop = true }
```

方式 B：显式包装

```text
poly.scop {
  ...
}
```

### 检查内容

```text
- loop bound affine
- loop step constant
- if condition affine
- memory index affine
- no unknown side effect
- no break / continue
- no irreducible CFG
- alias safe
```

### 重要性

这是整个 polyhedral pipeline 的入口。
宁可保守，也不要错误接受不合法 region。

---

## Pass 1.2：`scop-canonicalize`

### 功能

对被识别的 SCoP 做局部清理。

### 典型操作

```text
- 合并冗余 affine apply
- 简化 if 条件
- 消除 trivially true / false condition
- 拍平多余 region 包装
- 统一 statement 形式
```

---

## Pass 1.3：`stmt-outline`

### 功能

给 SCoP 中每个 statement 显式编号。

### 示例

源程序：

```c
for (i = 0; i < N; i++) {
  A[i] = B[i] + 1;
  C[i] = A[i] * 2;
}
```

编号后：

```text
S0: A[i] = B[i] + 1
S1: C[i] = A[i] * 2
```

### 为什么需要

多面体模型的基本对象是 statement instance：

```text
S0[i]
S1[i]
```

因此 statement identity 必须稳定。

---

## Pass 1.4：`scop-annotate`

### 功能

给 IR 添加 SCoP 元信息。

### 可能的信息

```text
- scop id
- scop parameters
- statement id
- loop depth
- memory object id
```

---

# Phase 2：Polyhedral Model Construction

## 目标

从 IR 中提取多面体模型。

核心对象包括：

```text
- iteration domain
- access relation
- initial schedule
- statement list
- parameter list
```

---

## 建议内部数据结构

```text
PolyScop {
  params: [N, M, K]

  statements: [
    PolyStmt {
      id: S0
      dims: [i, j]
      domain: PresburgerSet
      reads: [AccessRelation]
      writes: [AccessRelation]
      schedule: ScheduleRelation
    }
  ]
}
```

---

## Pass 2.1：`poly-domain-extract`

### 功能

为每个 statement 构造 iteration domain。

### 示例

源程序：

```c
for (i = 0; i < N; i++)
  for (j = 0; j < M; j++)
    S(i, j);
```

Domain：

```text
Domain(S) = { S[i, j] : 0 <= i < N and 0 <= j < M }
```

### 输入

```text
SCoP + statement id + surrounding loop nest
```

### 输出

```text
Statement -> PresburgerSet
```

### 难点

```text
- 多层 loop bound
- affine if condition
- symbol 参数
- 非 unit step loop
```

第一版可以只支持 unit step。

---

## Pass 2.2：`poly-access-extract`

### 功能

提取每个 statement 的内存访问关系。

### 示例

```c
A[i][j] = B[i][j] + C[i][j];
```

Access relation：

```text
Write_A : S[i, j] -> A[i, j]
Read_B  : S[i, j] -> B[i, j]
Read_C  : S[i, j] -> C[i, j]
```

### 输出

```text
Access {
  kind: read | write
  memory: A
  relation: PresburgerRelation
}
```

### 难点

```text
- 区分 read / write
- 识别同一个 memory object
- 下标表达式规范化
- 处理多维 memref layout
```

---

## Pass 2.3：`poly-schedule-extract`

### 功能

从原始 loop nest 中提取初始执行顺序。

### 示例 1：单 statement

```c
for i
  for j
    S(i, j)
```

Schedule：

```text
Theta(S[i, j]) = [i, j]
```

### 示例 2：多个 statement

```c
for i
  for j {
    S0(i, j)
    S1(i, j)
  }
```

Schedule：

```text
Theta(S0[i, j]) = [i, j, 0]
Theta(S1[i, j]) = [i, j, 1]
```

### 说明

statement 顺序通常需要用额外 schedule dimension 表示。

---

## Pass 2.4：`poly-model-build`

### 功能

把 domain、access、schedule 合并为完整的 PolyScop。

### 输出

可以是：

```text
- 内部分析结果
- IR attribute
- dumpable poly model
```

### 建议

第一阶段至少要能 dump：

```text
Scop:
  Params: [N, M]

  Stmt S0:
    Domain:
      { S0[i,j] : 0 <= i < N and 0 <= j < M }
    Reads:
      S0[i,j] -> B[i,j]
      S0[i,j] -> C[i,j]
    Writes:
      S0[i,j] -> A[i,j]
    Schedule:
      S0[i,j] -> [i,j,0]
```

---

# Phase 3：Dependence Analysis

## 目标

判断不同 statement instance 之间是否存在内存依赖。

需要支持：

```text
RAW: read after write
WAR: write after read
WAW: write after write
```

---

## Pass 3.1：`poly-dependence-analyze`

### 功能

构造 dependence relation。

### 基本条件

两个 statement instance 之间有依赖，通常需要满足：

```text
1. 它们访问同一个 memory object
2. 至少一个访问是 write
3. 访问的是同一个 memory location
4. source 在原 schedule 中早于 target
```

### 形式化描述

对于：

```text
Write_A : S0[i] -> A[f(i)]
Read_A  : S1[j] -> A[g(j)]
```

依赖关系需要满足：

```text
f(i) = g(j)
Theta(S0[i]) <lex Theta(S1[j])
i in Domain(S0)
j in Domain(S1)
```

### 输出

```text
Dependence {
  kind: RAW | WAR | WAW
  source: S0
  target: S1
  relation: PresburgerRelation
}
```

---

## Pass 3.2：`poly-dependence-simplify`

### 功能

简化 dependence relation。

### 目的

减少后续 legality check 的复杂度。

### 典型操作

```text
- 删除 empty dependence
- 合并同类 dependence
- 计算 dependence distance / direction
- 简化约束
```

---

## Pass 3.3：`poly-legality-check`

### 功能

检查候选 transformation 是否保持所有 dependence 顺序。

### 输入

```text
- dependence relations
- original schedule
- candidate schedule
```

### 判定

对于每条依赖：

```text
source -> target
```

必须满足：

```text
new_schedule(source) <lex new_schedule(target)
```

或者至少不反转依赖顺序。

### 输出

```text
legal / illegal
```

如果 illegal，最好能输出违反的 dependence。

---

## Pass 3.4：`poly-parallel-detect`

### 功能

判断某个 loop dimension 是否没有 loop-carried dependence。

### 输出

```text
loop i : sequential
loop j : parallel
```

### 用途

后续可以：

```text
- 添加 parallel marker
- 指导 loop unroll
- 指导 vectorization
```

---

## Pass 3.5：`poly-locality-estimate`（可选）

### 功能

估计不同 loop order 的 locality。

### 简化策略

第一版可以用非常简单的 heuristic：

```text
- 最内层 loop 对连续维度访问更优
- 对 row-major 数组，A[i][j] 通常希望 j 在最内层
- 对 B[k][j] 这类访问，需要结合整个 kernel 判断
```

### 用途

给 `poly-loop-interchange` 或 `poly-loop-tile` 提供决策。

---

# Phase 4：Transformations

## 目标

基于 dependence 和 legality check 做实际优化。

---

## Pass 4.1：`poly-loop-interchange`

### 功能

交换 loop 顺序。

### 示例

```c
for (i = 0; i < N; i++)
  for (j = 0; j < M; j++)
    S(i, j);
```

变为：

```c
for (j = 0; j < M; j++)
  for (i = 0; i < N; i++)
    S(i, j);
```

### 需要

```text
- dependence analysis
- legality check
- locality heuristic
- code regeneration
```

### 难度

低到中。

### 比赛价值

中高。
特别适合改善 row-major / column-major 访问顺序。

---

## Pass 4.2：`poly-strip-mine`

### 功能

对 loop 做 strip mining。

### 示例

```c
for (i = 0; i < N; i++)
  S(i);
```

变为：

```c
for (ii = 0; ii < N; ii += T)
  for (i = ii; i < min(ii + T, N); i++)
    S(i);
```

### 说明

这是 tiling 的基础。

---

## Pass 4.3：`poly-loop-tile`

### 功能

对 2D / 3D loop nest 做 tiling。

### 示例：2D tiling

```c
for (i = 0; i < N; i++)
  for (j = 0; j < M; j++)
    S(i, j);
```

变成：

```c
for (ii = 0; ii < N; ii += Ti)
  for (jj = 0; jj < M; jj += Tj)
    for (i = ii; i < min(ii + Ti, N); i++)
      for (j = jj; j < min(jj + Tj, M); j++)
        S(i, j);
```

### 示例：matmul tiling

```c
for (i = 0; i < N; i++)
  for (j = 0; j < N; j++)
    for (k = 0; k < N; k++)
      C[i][j] += A[i][k] * B[k][j];
```

变为：

```c
for (ii = 0; ii < N; ii += Ti)
  for (jj = 0; jj < N; jj += Tj)
    for (kk = 0; kk < N; kk += Tk)
      for (i = ii; i < min(ii + Ti, N); i++)
        for (j = jj; j < min(jj + Tj, N); j++)
          for (k = kk; k < min(kk + Tk, N); k++)
            C[i][j] += A[i][k] * B[k][j];
```

### 比赛价值

最高。

对 RISC-V 后端尤其有价值，因为它能提升：

```text
- cache locality
- register reuse
- memory bandwidth utilization
```

---

## Pass 4.4：`poly-loop-fusion`

### 功能

融合相邻 loop nest。

### 示例

```c
for (i = 0; i < N; i++)
  A[i] = B[i] + 1;

for (i = 0; i < N; i++)
  C[i] = A[i] * 2;
```

融合为：

```c
for (i = 0; i < N; i++) {
  A[i] = B[i] + 1;
  C[i] = A[i] * 2;
}
```

### 第一版限制

建议只支持：

```text
- 同一层级的 sibling loops
- loop bounds 完全一致
- step 完全一致
- 无 illegal dependence
```

### 收益

```text
- 提升 temporal locality
- 减少 loop overhead
- 给 scalar replacement 创造机会
```

---

## Pass 4.5：`poly-scalar-replacement`

### 功能

把重复访问的数组元素提升为标量临时值。

### 示例

```c
for (i = 0; i < N; i++) {
  A[i] = A[i] + 1;
  A[i] = A[i] * 2;
}
```

优化为：

```c
for (i = 0; i < N; i++) {
  tmp = A[i];
  tmp = tmp + 1;
  tmp = tmp * 2;
  A[i] = tmp;
}
```

### 收益

```text
- 减少 load/store
- 增强寄存器复用
- 对 RISC-V 后端友好
```

---

## Pass 4.6：`poly-parallel-mark`

### 功能

给没有 loop-carried dependence 的 loop 添加 parallel 标记。

### 示例

```text
affine.for %i = 0 to %N {
  parallel = true
}
```

### 用途

```text
- 后续映射到多线程
- 指导 vectorization
- 指导 unroll
- 作为优化报告输出
```

---

## Pass 4.7：`poly-unroll-jam`

### 功能

对外层循环 unroll，并把内层循环合并。

### 适用场景

```text
- tile 后的内层 loop
- 小固定 trip count loop
- stencil / matmul kernel
```

### 建议

放在 tiling 稳定之后再做。

---

## Pass 4.8：`poly-skew`

### 功能

做 loop skewing，常用于 stencil。

### 示例

```text
t, i  ->  t, i + t
```

### 适用场景

```text
- stencil
- wavefront parallelism
```

### 建议

这是进阶 pass，不建议第一阶段实现。

---

## Pass 4.9：`poly-distribute`

### 功能

把一个 loop 中多个 statement 拆到多个 loop 中。

### 用途

```text
- 暴露 parallelism
- 分离不同 dependence pattern
- 给 fusion / tiling 创造机会
```

### 建议

可作为后期优化。

---

# Phase 5：Code Regeneration

## 目标

把 transformed schedule 或变换结果重新生成回 L1 structured IR。

---

## Pass 5.1：`poly-schedule-apply`

### 功能

把候选 schedule 应用到 loop nest。

### 第一版建议

不要做完全通用的 affine schedule codegen。

可以针对每种 transformation 写模板式 rewrite：

```text
- interchange rewrite
- strip-mine rewrite
- tile rewrite
- fusion rewrite
```

---

## Pass 5.2：`poly-codegen`

### 功能

从 polyhedral model 生成新的结构化 loop IR。

### 两种路线

#### 路线 A：模板式 codegen

每种变换单独生成 IR。

优点：

```text
- 容易实现
- 容易调试
- 比赛更稳
```

缺点：

```text
- 泛化能力弱
```

#### 路线 B：通用 schedule codegen

根据 affine schedule 自动生成 loop nest。

优点：

```text
- 更接近真正 polyhedral compiler
```

缺点：

```text
- 实现难度高
- 需要处理复杂上下界和 guards
```

### 建议

比赛项目优先选择路线 A。

---

## Pass 5.3：`poly-cleanup`

### 功能

清理 codegen 后的 IR。

### 典型操作

```text
- 删除空 loop
- 删除单次 loop
- 简化 min / max bound
- 合并 affine.apply
- DCE
- CSE
- 删除无用 temporary
```

---

# Phase 6：Lowering 与后端衔接

## 目标

将优化后的 L1 IR 降到 L2，再到 RISC-V。

---

## Pass 6.1：`lower-structured-to-ssa`

### 功能

将结构化 loop / if / memref IR 降到 LLVM-like SSA IR。

---

## Pass 6.2：`ssa-canonicalize`

### 功能

做中层常规优化：

```text
- CSE
- DCE
- constant folding
- copy propagation
- simplify cfg
- mem2reg
```

---

## Pass 6.3：`loop-unroll`

### 功能

对适合的 loop 做展开。

### 注意

如果 L1 已经做过 tiling，L2 的 unroll 可以针对 tile 内层 loop。

---

## Pass 6.4：`vectorization-prep`（RVV 目标可用）

### 功能

对已证明输出点相互独立的整数 reduction，Polyhedral pass 生成 factor-2/4 lane pack，并通过
`YIRPolyhedralTransformSummary::rvv_preparations` 交给 OIR SLP。SLP 再独立执行 target、依赖、
memory order、cost 和 verifier 检查，成功后复用现有 OIR→MIR RVV lowering；失败时仍保留
语义等价且经过收益判断的标量 unroll-and-jam。

### 典型操作

```text
- 保证内层 loop stride-1
- 对齐 memory access
- 提取连续 load/store pattern
- 共享 lane-invariant reduction slice
- 为独立 output lane 构造 SLP pack
```

---

## Pass 6.5：`lower-to-riscv`

### 功能

生成 RISC-V 相关 IR 或汇编。

---

# 4. 推荐 Pass 总表

| 类别 | Pass | 作用 | 优先级 |
|---|---|---|---|
| 预处理 | `loop-normalize` | 统一 loop 形式 | 高 |
| 预处理 | `affine-expr-canonicalize` | 规范 affine 表达式 | 高 |
| 预处理 | `symbol-detect` | 区分 dims / symbols | 高 |
| 预处理 | `loop-invariant-hoist-lite` | 识别 loop invariant | 中 |
| SCoP | `scop-detect` | 检测可多面体优化区域 | 最高 |
| SCoP | `scop-canonicalize` | 清理 SCoP | 高 |
| SCoP | `stmt-outline` | 给 statement 编号 | 高 |
| 建模 | `poly-domain-extract` | 提取 iteration domain | 最高 |
| 建模 | `poly-access-extract` | 提取 access relation | 最高 |
| 建模 | `poly-schedule-extract` | 提取初始 schedule | 高 |
| 建模 | `poly-model-build` | 构造 PolyScop | 高 |
| 分析 | `poly-dependence-analyze` | 依赖分析 | 最高 |
| 分析 | `poly-legality-check` | 检查变换合法性 | 最高 |
| 分析 | `poly-parallel-detect` | 检测并行维度 | 中 |
| 变换 | `poly-loop-interchange` | loop 交换 | 高 |
| 变换 | `poly-strip-mine` | strip mining | 高 |
| 变换 | `poly-loop-tile` | loop tiling | 最高 |
| 变换 | `poly-loop-fusion` | loop fusion | 中高 |
| 变换 | `poly-scalar-replacement` | 标量替换 | 中高 |
| 变换 | `poly-parallel-mark` | 并行标记 | 中 |
| 变换 | `poly-unroll-jam` | unroll and jam | 中 |
| 变换 | `poly-skew` | skewing | 低 |
| 代码生成 | `poly-schedule-apply` | 应用 schedule | 高 |
| 代码生成 | `poly-codegen` | 生成优化后 loop | 最高 |
| 代码生成 | `poly-cleanup` | 清理 IR | 高 |
| Lowering | `lower-structured-to-ssa` | 降到 L2 | 高 |
| Lowering | `ssa-canonicalize` | 常规 SSA 优化 | 高 |
| Lowering | `loop-unroll` | loop 展开 | 中 |
| Lowering | `lower-to-riscv` | 后端 lowering | 高 |
