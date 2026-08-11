# OIR 自动向量化

本文记录当前仓库中已经实现并由测试约束的 OIR Loop Vectorizer 和 SLP
Vectorizer。它描述的是当前行为，不是未来路线图，也不承诺尚未实现的 ABI 或任意
后端覆盖。当前状态不是“完整 GA 向量化支持”。

源语言中显式 fixed vector/mask 的类型与 intrinsic 另见
[vector-mask-language.md](vector-mask-language.md)。本文关注从标量 OIR 自动生成 vector/VP
OIR 的优化。

## Pipeline 与开关

OIR pipeline 先运行标量优化固定点，再运行自动向量化：

1. `OIROptimizationPipelinePass` 规范化循环、CFG、内存和标量表达式；
2. 若启用，`OIRLoopVectorizerPass` 构造 scalable VLA 循环；
3. 若启用，`OIRSLPVectorizerPass` 在基本块内构造 fixed-width pack；
4. 只要任一自动向量器进入 pipeline，最后运行经过审核的
   `OIRVectorCleanupPass`，删除无副作用的死 vector/地址计算，并在输入和输出处验证
   OIR。

默认优化级别如下：

| 优化级别 | Loop Vectorizer | SLP Vectorizer |
| --- | --- | --- |
| `-O0` | 关闭，显式启用也不会越过 O0 边界 | 关闭，显式启用也不会越过 O0 边界 |
| `-O1` | 默认关闭，可用 `-fvectorize` 启用 | 默认关闭，可用 `-fslp-vectorize` 启用 |
| `-O2` | 默认启用 | 默认关闭，可显式启用 |
| `-O3` | 默认启用 | 默认启用 |

`-fno-vectorize` 和 `-fno-slp-vectorize` 分别关闭两个向量器。`-fvectorize` 与
`-fslp-vectorize` 只是启用开关；它们不会绕过合法性或收益判断，也不等同于 pass
内部 programmatic `force` 选项。

RVV 目标还有一条受限的 Polyhedral→SLP 衔接：多面体分析证明 output-reduction 的输出点
彼此独立后，可生成 factor-2/4 lane pack，并通过显式 pass artifact 请求 OIR SLP 接手。
因此在 `-O1 -fvectorize` 或 `-O2` 下，即使通用 SLP 默认关闭，这类 lane pack 仍会运行一次
SLP 合法性、收益和 verifier；没有该 artifact 时不会额外运行。显式
`-fno-slp-vectorize` 会关闭这条衔接。标量目标继续使用原有 register-blocked lowering。

自动向量化还要求 target profile 提供 V 或兼容的 Zve 能力以及相应的 32-bit
整数/浮点 vector element family。例如测试使用 `-march=rv64gcv`。CLI 在 non-V
target（例如 `-march=rv64gc`）上不会调度自动向量器，因此 `--emit-vector-plan`
通常为空；这与 programmatic API 对已调用向量器报告 `REJECT_TARGET_FEATURE` 是两个不同
层次的行为。显式源语言 vector 的 portable scalarization 不属于本文的自动向量化契约。

### ABI、对象与后端边界

自动向量器生成的 scalable vector 是循环内部 SSA 值；OIR verifier 不允许 scalable
vector 出现在普通 global/alloca storage 或函数 signature 中。fixed vector/mask 另有
target-independent portable object layout，可以作为对象和 aggregate 的组成部分；standard
LP64D calling-convention classifier 也把它们按 aggregate 分类，而不是擅自放进 vector
argument register。

standard ABI 的 fixed vector/mask 跨函数边界按普通 LP64D aggregate 传递；其布局、间接
参数、sret 和跨语言验证见 [standard-vector-aggregate-abi.md](standard-vector-aggregate-abi.md)。
显式 `-mvector-abi=psabi-vector` 的 classifier、lowerer、MIR/RA 和 ELF
`.variant_cc` 路径已分阶段接入，但公开 CLI 仍 fail-closed。原始开放条件要求
fixed vector tuple 端到端 lowering 与 GCC/Clang 双向互操；当前 source/OIR
函数类型不能携带 tuple NFIELDS，且 GCC 15 会忽略 `riscv_vls_cc`属性。因此即使
ABI_VLEN profile 合法，也会以 `PSABI_VECTOR_ABI_UNAVAILABLE` 精确拒绝，不得静默切换
到 standard ABI，也不得称为完整/GA。

在最终发布 gate 之前，target 仍会先验证 compile-time V/Zve、显式 numeric
`-mrvv-vector-bits=ABI_VLEN`，且 ABI_VLEN 必须等于 `-march` 保证的 minimum VLEN；例如
`-march=rv64gcv_zvl128b -mrvv-vector-bits=128 -mvector-abi=psabi-vector`。没有 V/Zve、
使用 scalable/未指定/不匹配的 ABI_VLEN、公开 scalable signature、vararg 或非法 tuple
都会精确拒绝。签名还按 element family 检查能力：仅有 `Zve32x` 时不能公开
`vector<float,N>`。这些 ABI 规则独立于自动向量化收益判断；自动向量器产生的 scalable
SSA 仍只能留在函数内部。

## Loop Vectorizer：VLA 执行模型

Loop Vectorizer 只在完整 planner 合法性检查和收益选择完成后修改 IR。它把已证明的
规范循环 strip-mine 为 target-independent scalable VP 循环，核心状态是
`remaining`、`setvl` 的实际返回值和标量 induction：

```text
remaining.init = iteration_count(max(trip_count, 0), abs(stride))
loop.header:
  remaining = phi [remaining.init, preheader], [remaining.next, body]
  if remaining != 0 -> body else exit
loop.body:
  actual_vl = setvl <vscale x N x T>, remaining
  ... VP operations ..., evl actual_vl
  iv.next        = iv + actual_vl * stride
  pointer.next   = pointer + actual_vl * pointer_stride
  remaining.next = remaining - actual_vl
```

`setvl` 的 `i32` 结果是实际 VL，不是假定的 VLEN/VLMAX 常量。所有生成的 VP
load/store、算术、比较、gather/scatter 和 reduction 都显式使用该值作为 EVL；IV、
pointer induction 和 `remaining` 也消费同一实际值。因而代码不会假定某一硬件 VLEN，
并正确覆盖零次、完整 VL 和尾部不足一个 VL 的迭代数。

对步长绝对值大于一的循环，`remaining.init` 是
`ceil(max(trip_count, 0) / abs(stride))`。这表示待执行的标量迭代数，而不是最后一个数组
下标。

### 连续、strided/indexed、segment 与反向 recipe

当前支持可证明的编译期元素步长 `+1`、`+2`、`+4`、`-1`、`-2`、`-4`；read-only
invariant address 还支持步长 `0`，包括：

- canonical integer induction；
- pointer phi 加常量 GEP 的 pointer induction；
- pointer induction 上只含 loop-invariant index 的常量偏移 GEP；这类地址继承 base pointer
  的已证明 stride，不把动态/非仿射 index 猜成线性访问；
- final GEP index 中由 `Add`、`Sub`、`Mul` 组成的可证明仿射表达式，例如
  `i++` 配合 `a[i * 2]`。

OIR Loop recipe 对正向 `+1` 生成连续 `VPLoad`/`VPStore`；步长 `0` 的 load 使用全零
offset gather，步长 `0` 的 store 因 duplicate destination 无法保持标量顺序而拒绝。
`+2`、`+4` 和所有负步长先以 `VPGather`/`VPScatter` 表达，保留完整 lane 顺序。
RVV lowering 会严格证明其 index 是 arithmetic progression；证明成功时选择带 signed byte
stride 的 `vlse32.v`/`vsse32.v`，否则保留 ordered indexed memory，或对一般 signed index
使用逐 active lane 的保序 fallback。相邻、同 type/index/mask/EVL、TA/MA 且 field 1 恰为
field 0 加一的两字段 stride-2 访问还可合并为 `vlseg2e32.v`/`vsseg2e32.v`；PostRA/Final
verifier 会检查 tuple 连续、LMUL 对齐及 `NFIELDS * LMUL <= 8`。特别地，负步长不能把 lane
顺序误当成正向连续内存。

反向 header-tested 规范形为：

```text
iv.start = trip_count - 1
while (iv >= 0) {
  ...
  iv -= abs(stride)
}
```

scalable lane IV 为 `splat(iv) + stepvector * signed_stride`；因此 lane 0 仍对应当前
标量迭代，后续 lane 按原标量执行顺序访问递减地址。也支持由
`icmp ge %trip_count - 1, 0` 零趟 guard 保护、latch 使用 `icmp ge %iv.next, 0` 的
rotated reverse 形态；guard、共同 exit 或 signed stride 证明不完整时稳定报告
`REJECT_NON_CANONICAL_LOOP` 或 `REJECT_STRIDE`。

### VP mask 与 diamond if-conversion

无条件 VLA body 使用同 shape 的 scalable `i1` active mask。EVL 之外的 lane 不执行
操作。单层、无副作用、无 early-exit 的规范 diamond 可以 if-convert；支持普通
header-tested 形态，也支持带零趟 preheader guard、在独立 latch 测试回边的 rotated
形态：

```text
condition = vp.cmp(..., mask=active, evl=actual_vl)
then_mask = active & condition
else_mask = active ^ then_mask
```

then/else lane 运算和 store 分别使用对应 mask；merge phi 变为同 shape 的 vector
select。then-only store 只在 `then_mask` lane 上访问内存。OIR VP memory 的规范合同是：
`mask=false` 或 `lane>=EVL` 的 lane 不得产生内存访问、fault 或地址可观察行为；load
在 undisturbed policy 下从 passthrough 取得 inactive lane。当前向量器生成的 policy
为显式 `Agnostic`，并为有结果的 VP 操作提供结构化 passthrough。

guarded rotated diamond 必须精确满足：guard true-edge 进入 header，latch true-edge 回到
同一 header，两个 false-edge 指向同一 exit，lane diamond 的零个或一个 then/else arm
直接汇合到该 latch，且没有额外 loop block。变换保留 latch 作为 loop-control block，
`remaining`、integer IV 和 pointer IV 的回边 predecessor 仍是 latch；仅删除 lane arm，
把 header 改为无条件流向 latch。共同 exit、零趟 guard 或 predecessor 证明任一不成立都
fail closed，不发布 `VECTORIZED`。

普通 two-block header-tested loop 的 preheader 也可以是外层条件分支：必须恰有一个 successor
进入当前 loop，另一个留在 loop 外。向量器保留该原始 guard，只在进入 loop 的边上计算
strip-mine 状态；因此外层条件为 false 或 trip count 为零时不会执行 vector body，reduction
live-out 仍取原 scalar seed。无法证明唯一入边时继续报告 `REJECT_EARLY_EXIT`。

### Loop reduction

当前支持关联的 scalar `i32` loop-carried reduction：

- `Add`、`Mul`；
- bitwise `And`、`Or`、`Xor`。

生成的 `VPReductionInst` 使用原 scalar accumulator 作为 passthrough，并把结果继续接回
loop-carried phi。严格浮点模式不重排 float reduction，稳定报告
`REJECT_FP_ORDER`；彼此独立的逐 lane `FAdd`、`FSub`、`FMul`、`FDiv` 不因 strict FP
本身被拒绝。

同一迭代中由上述同一种关联整数运算形成的线性 unrolled chain 也可规约，例如四个顺序
`sum = sum + load`。planner 要求 accumulator 在每一级只进入一个 operand，中间结果只有
下一级一个 use，且每个 lane value 都有完整 widening recipe。变换按原 scalar 顺序生成
多个 `VPReductionInst`，前一级结果作为后一级 passthrough；中间值 escape、分叉或 op
不一致均稳定 `REJECT_REDUCTION`。这项能力不扩展到浮点重结合。

## SLP Vectorizer：fixed-width pack

SLP 在单个基本块内寻找相邻、同类型的 scalar pack。当前 element family 是 `i32` 和
`f32`，默认最小/preferred/最大 pack lane 数为 2/4/8。实现覆盖：

- 可证明 same-base、连续 byte range 的 plain scalar load/store pack；
- `i32` 的 `Add`、`Sub`、`Mul`、`And`、`Or`、`Xor`；
- `f32` 的 `FAdd`、`FSub`、`FMul`；
- 同 predicate 的 `i32`/`f32` compare pack，结果为 fixed mask；
- scalar `i1` 的 `And`、`Or`、`Xor` mask pack；
- 已有 vector producer 的按序 extract pack 直接复用、相同 scalar operand 的 splat，
  以及必要时的 insert/extract pack/unpack。load→arithmetic→compare→mask 的多层树会
  直接连接 vector producer，不制造 extract→insert 往返；lane 重排或 shape 不同则不误复用。

生成的 VP 指令使用 fixed vector type、全真 fixed mask 和等于 pack lane 数的常量
EVL。load、binary 和 store 分别独立计入 pack 统计；成功计划中的 lane 数是该基本块
成功 pack 的最大宽度。

SLP 在 mutation journal 中完成插入、use replacement 和 scalar instruction detach。
所有候选修改完成后运行 fail-closed OIR verifier（以及可选的集成 verifier）；验证或
mutation 失败会回滚，不发布 `SLP_VECTORIZED`。只有验证通过并提交 journal 后才发布
成功记录。Loop Vectorizer 同样在 module transaction 中修改、post-verify 后提交，失败
恢复变换前文本。

## 合法性与保守拒绝

两个向量器都遵循“无法证明即拒绝”，`force` 也不会改变这一点。

Loop Vectorizer 当前要求规范 single-latch、单一 exiting region 的循环。它会稳定拒绝：

- 无法匹配的 induction、signed stride、rotated guard、recurrence 或 live-out；
- early exit、未支持的 nested/复杂 CFG；
- 没有 vector mapping 的 call；
- divisor 未证明非零的整数 `SDiv`/`SRem`；
- 非支持的 reduction 或 strict FP reduction 重排；
- 不可证明的 memory stride、loop-carried dependence 或 unknown alias。

SLP 会保守拒绝 call、可能 trapping 的 div/rem、intra-pack SSA dependence、无法证明
连续的地址、穿过 memory barrier 的重排、unsupported type/op 和非 plain scalar
load/store memory effect。

### Alias 与 overlap

Loop Vectorizer 对一类完整可证明的 unknown pointer alias 实现 overflow-safe runtime
versioning。当前 fast-path 范围必须是 zero-based、forward unit-induction 的规范 two-block
loop，或由零趟 preheader guard 保护、latch 条件回边的 single-block rotated loop；每个
unknown pair 的两侧都必须能表示为 i32 元素的 invariant/pointer-phi/simple
`base[iv]` affine stream；所有 pair 的检查结果取 AND。每个 stream 传递 base、trip count、
signed element stride 和 element bytes 给纯 runtime helper。helper 仅以 RV64 `uintptr_t`
整数构造 half-open byte range，不解引用地址；乘法、正向 end 加法、反向 low 减法任一
溢出，或 count/stride/element-size 描述非法，都返回 false。

true guard 进入原 loop 的 scalable VP fast path；false guard 进入变换前 scalar loop 的完整
clone，二者汇合于原 exit，exit phi 为 clone 增加对应 incoming。zero count 可安全选择 fast
path 且不访问内存；overlap、exact alias、算术不可证明和不支持的 live-out 一律走 scalar
或在计划期保持 `REJECT_ALIAS`。`force` 不会绕过 range 可构造性。版本化、向量化和 helper
声明在同一 module text transaction 中完成，最终 verifier/集成 gate 通过后才提交并发布
`VECTORIZED`；成功 plan 的 `runtime_alias_check` 为 `true`，说明中记录 pair 数。

rotated 版本化只接受一个规范 induction exit phi：零趟 incoming 必须等于 induction start，
fast incoming 是按 actual VL 更新后的 induction，slow incoming 来自 scalar clone。三条路径
在共同 exit 合并；额外 live-out 或不匹配 incoming 会在修改前拒绝。slow clone 带独立
`lv.slow.*` 名称且不会被同次向量化递归处理。

已证明来自不同 global 的对象仍可静态判定 no-alias；完全相同的 affine lane address 可按原
lane 内指令顺序向量化；同一已知对象的部分 overlap 仍视为潜在 loop-carried dependence。

SLP 同样只接受 alias analysis 已证明的连续 pack，不进行运行时版本化。

### Volatile/atomic 模型边界

当前 scalar OIR `LoadInst`/`StoreInst` 没有 volatile/atomic qualifier，仓库也没有可供
自动向量器匹配的 scalar atomic memory op。因此当前实现不能表达并证明
volatile/atomic 向量化语义，也不宣称支持它们。SLP 只接受精确的 plain
`LoadInst`/`StoreInst`，其他 memory-effect instruction 以
`SLP_REJECT_MEMORY_SEMANTICS`（共享 code 为 `REJECT_VOLATILE_OR_ATOMIC`）拒绝；Loop
planner 对没有 recipe 的 memory operation 同样不会发布成功。

## Cost model 与 `force`

Loop Vectorizer 的收益判断由独立 `RVVTargetCostModel` 完成；输入是经过合法性分析后的纯
POD 摘要，不持有 OIR 节点，也不能把“便宜”当作“合法”。模型枚举
`mf2/m1/m2/m4/m8`；O3 在完整 OIR plan 明确授权后再形成 LMUL × `{1,2}` 候选矩阵，
同时检查 minimum VLEN、SEW/LMUL mask ratio 与 target LMUL/interleave 上限。
每个候选分别记录：

- 实际 VLA iteration 数和每次 `vsetvl` 成本；
- unit-stride、strided、indexed 和 segment memory 的独立 load/store/field 成本；
- indexed offset 构造及 distinct live index vector；
- mask、tail predicate 和 reduction 成本；
- 按 LMUL group 估算的 live vector register 数、超出可用预算后的预测 spill register 数，
  以及每次 vector iteration 的 spill load/store 代价；
- runtime alias pair 的一次性 fast/slow setup；
- vector region、alias guard 和保留 scalar slow clone 的静态 code bytes/code-growth 代价；
- factor 2 的两份 vector body、两次 `vsetvl`、额外寄存器压力和静态 code bytes；所有操作
  先完整计费，再按同一 target memory cost 表给两个独立 chunk 一项有界 pipeline overlap
  credit；
- 从 scalar-per-lane 与 vector-per-chunk 成本求出的首个 break-even trip count。

Loop planner 把正向 `+1` 计为 unit-stride；其余 affine access 在 OIR 中保持显式 index，
成本适配器按可证明的 AP stride 计为 strided，并对满足上述严格配对条件的两字段候选计入
segment 成本；无法证明的访问继续按 indexed 计费，绝不只为降低成本而改写 memory kind。
常量 trip count（包括零）优先于 heuristic expected trip count。短于 break-even 的循环以
`REJECT_COST` 拒绝；不是把固定阈值冒充 target 收益。

### `-mcpu` / `-mtune` 参数表

CPU 与 tuning 名称是严格 registry，不再接受任意字符串。当前公开且有完整参数表的名称
只有 `generic-rv64` 和 `generic-rvv`；未知 `-mcpu` 或 `-mtune` 会列出这两个支持项并
fail closed。

- `-mcpu=generic-rvv` 在没有显式 `-march` 时选择 `rv64gcv`，并默认继承
  `-mtune=generic-rvv`；
- 显式 `-march` 始终保留，因而可以只使用 CPU 的 tuning 而选择更窄 ISA；
- 显式 `-mtune` 覆盖 `-mcpu` 的默认 tuning；`-mcpu` 表示执行目标，`-mtune` 只影响成本。

以下整数是稳定的抽象成本单位，不宣称等于某一微架构的精确 cycle：

| tuning | scalar load/store/branch | RVV ALU | unit L/S | strided L/S | indexed L/S | segment base/field | mask/reduction/vsetvl | spill L/S | VR budget | max interleave |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `generic-rv64` | 4/4/2 | 1 | 5/5 | 8/9 | 12/13 | 3/4 | 2/8/3 | 10/10 | 31 | 2 |
| `generic-rvv` | 4/4/2 | 1 | 3/3 | 5/6 | 8/9 | 2/2 | 1/4/2 | 8/8 | 31 | 2 |

同一 `TargetProfile.tuning` 也会通过 `target_profile_for` 写入统一 cost-model report。
`--emit-cost-model=json` 输出 `cpu`、`tune` 和完整 `rvv_costs`，不再只打印 arch/ABI 后
继续使用默认权重。

`--emit-vector-plan` 的每个 Loop plan 还输出 `tuning`、
`predicted_spill_registers`、`interleave_overlap_credit`、`estimated_code_bytes`、
`break_even_trip_count` 和 `interleave_capability_gate`，使选择或拒绝可以稳定复核。

O3 可以选择真实 factor-2 VLA recipe；O2 及以下始终为 factor 1。一个 outer iteration
顺序发射两个独立 chunk：

```text
vl0  = setvl(remaining)
group0(mask0, evl=vl0, base=iv/pointer)
rem1 = remaining - vl0
vl1  = setvl(rem1)
group1(mask1, evl=vl1, base=iv/pointer advanced by vl0)
total = vl0 + vl1
iv/pointer += total
remaining -= total
```

`rem1 == 0` 时第二组 EVL 为零，VP memory 不访问任何 lane。两组不复用 mask、EVL、lane
SSA、indexed offset 或 pointer base。当前 factor 2 仅授权 simple independent
canonical/rotated constant-stride plan；reduction、diamond if-conversion、runtime alias
versioning、post-merge region 和未证明的 live-out 保持 factor 1，并在 O3 plan 中输出稳定的
`INTERLEAVE_FACTOR_2_RECIPE_UNAVAILABLE`。target tuning 本身只允许 factor 1 时输出
`INTERLEAVE_FACTOR_2_TARGET_UNAVAILABLE`。`force` 不能清除这些 legality gate，也绝不会
出现“plan 2、实际发射 1”。

SLP cost model包含 scalar pack 成本、vector load/store/ALU，以及所需的
pack/unpack/splat 成本。这些数字用于本地收益决策和诊断，不是精确 cycle 模型，也不是
ABI 或后端性能保证。

programmatic `LoopVectorizerOptions::force` 和 `SLPVectorizerOptions::force` 只绕过最后的
profitability 比较。它们不会绕过 target feature、类型、CFG、alias/dependence、memory
order、trap、register budget 或 verifier。CLI 目前没有“强制收益”开关；前述
`-fvectorize`/`-fslp-vectorize` 仅启用 pass。

## Remarks 与 JSON 计划

可用以下 CLI 查看结果：

```sh
# 成功记录写到 stderr
compiler --emit-oir -O2 -march=rv64gcv -Rpass=loop input.sy

# 拒绝记录写到 stderr
compiler --emit-oir -O3 -march=rv64gcv -Rpass-missed=slp input.sy

# 全部记录以 JSON 写到 stdout
compiler --emit-vector-plan -O3 -march=rv64gcv input.sy
```

`-Rpass[=<filter>]` 只打印成功，`-Rpass-missed[=<filter>]` 只打印未成功记录。filter
匹配 function、region、vectorizer kind 或稳定 code 的 substring。

共享稳定 code 包括 `VECTORIZED`、`REJECT_DEPENDENCE`、`REJECT_ALIAS`、
`REJECT_FP_ORDER`、`REJECT_CALL`、`REJECT_COST`、`REJECT_REGISTER_PRESSURE`、
`REJECT_UNSUPPORTED_TYPE`、`REJECT_NON_CANONICAL_LOOP`、`REJECT_EARLY_EXIT`、
`REJECT_POTENTIAL_TRAP`、`REJECT_REDUCTION`、`REJECT_STRIDE`、
`REJECT_VOLATILE_OR_ATOMIC`、`REJECT_TARGET_FEATURE` 和 `DISABLED`。SLP explanation
还携带更细的稳定 `SLP_*` reason，例如 `SLP_REJECT_CALL`、
`SLP_REJECT_MEMORY_ORDER` 和 `SLP_REJECT_VERIFICATION`。

只有 code 为 `VECTORIZED` 的 remark 表示成功。内部 `CANDIDATE` 状态、任何 reject 或
disabled code 都不是成功。成功记录只在 transform 完成且 verifier 通过后发布。

`--emit-vector-plan` 的每项包含 vectorizer、code、function、region、explanation 以及：

- `scalable`、`minimum_lanes`、`lmul`、`interleave`；
- `estimated_scalar_cost`、`estimated_vector_cost`、
  `estimated_vector_registers`；
- `runtime_alias_check`、`uses_mask`。

这些字段描述当前 OIR 计划，不构成跨版本 cost 数值稳定性、函数 ABI 或所有
MIR/assembly lowering 的保证。对自动向量化能力的回归，应首先检查 OIR、verifier、
remark code 和 plan shape；具体 backend/toolchain 覆盖由对应后端测试单独约束。

## 可执行契约测试

`scripts/vectorization_docs_infra_tests.py` 直接用当前 compiler 编译现有
`test/ir/oir_loop_vectorize.sy` 和 `test/ir/oir_slp_vectorize.sy`，检查：

- Loop scalable OIR、`setvl`/EVL、成功 text remark 和 JSON plan；
- SLP fixed OIR、成功 text remark 和 JSON plan；
- `-fno-*`、默认优化级别和 non-V target 不产生自动 vector OIR；
- SLP call legality 的稳定 `REJECT_CALL`/`SLP_REJECT_CALL` miss；
- `-mvector-abi=psabi-vector` 缺少 numeric ABI_VLEN 时先精确拒绝；完整合法的
  profile 仍因 tuple/GCC 双向互操缺口以 `PSABI_VECTOR_ABI_UNAVAILABLE` 关闭；
- ordinary no-alias 成功 plan 不声称 runtime alias versioning；版本化 corpus gate 则要求
  `runtime_alias_check=true`、完整 scalar slow clone 和最终 verified success remark。
- `scripts/rvv_remaining_perf_corpus_infra_tests.py` 锁定 `many_mat_cal-3` 的 guarded reduction、
  `matmul2` 的 stride-4 四路整数 reduction chain，以及 `fft2` rotated versioning；同时检查
  plan/OIR/objdump、rv64gc scalar 与 VLEN=128/256/512/1024，并用实际 `memmove1` 覆盖
  disjoint/exact/双向 overlap、zero、VLMAX 边界和 PROT_NONE guard page。

运行：

```sh
python3 scripts/vectorization_docs_infra_tests.py
```
