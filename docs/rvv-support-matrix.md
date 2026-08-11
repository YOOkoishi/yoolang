# RVV / vector 支持矩阵

本文记录当前实现边界。`完成` 表示存在 verifier、汇编/执行门禁；`部分` 表示只覆盖表中
明确列出的形态；`拒绝` 表示编译器以稳定诊断 fail closed。本文不是 GA 声明：真实 RVV
硬件性能门尚未执行，且下列本地缺口仍未闭合。

## 语言与 IR

| 能力 | 状态 | 当前边界 |
| --- | --- | --- |
| `vector<int,N>` / `vector<float,N>` | 完成 | fixed value；`N > 0`，支持非 2 次幂及大于一次 VLMAX |
| `mask<N>` | 完成 | packed LSB-first，`ceil(N/8)` bytes，写出高位清零 |
| global/local/array/param/return/phi | 完成 | standard aggregate ABI；vector 不 decay |
| literal/zero/splat/cast | 完成 | i32/f32 数值 cast；不支持额外 scalar element family |
| arithmetic/compare/select/lane/shuffle | 完成 | compare 产生同 shape mask；dynamic lane 有边界语义 |
| any/all/none 与 reduction | 完成 | integer family；f32 默认 ordered |
| masked load/store、gather/scatter | 完成 | mask/EVL/passthrough/policy 显式建模 |
| YIR fixed vector/mask | 完成 | typed constants；无 textual initializer fallback |
| OIR fixed/scalable vector/VP | 完成 | parser/printer round-trip；storage/ABI 限制 fail closed |
| source scalable vector | 拒绝 | 只允许 compiler-internal OIR/MIR scalable value |

## 自动向量化

| 能力 | 状态 | 当前边界 |
| --- | --- | --- |
| Loop Vectorizer VLA | 完成 | actual `setvl` EVL、未知 trip、canonical/rotated loop |
| unit/constant/reverse stride | 完成 | `+1/+2/+4/-1/-2/-4` 与可证明 pointer induction |
| integer reduction | 完成 | canonical、rotated 及受限 linear-unrolled chain |
| strict f32 reduction | 部分 | 保序 recipe 可用；不能证明顺序时稳定拒绝 |
| diamond if-conversion | 完成 | then/else mask、masked memory、phi select |
| runtime alias versioning | 部分 | 可构造 overflow-safe affine ranges 的受限 two-block/rotated CFG |
| SLP | 部分 | fixed i32/f32 N=3/7、连续 memory、算术/compare/mask producer tree |
| O3 interleave | 部分 | simple independent plan 的真实 factor 2；复杂 recipe 降级 factor 1 |
| Polyhedral output-reduction | 部分 | 已证明独立的 i32/f32 factor-2/4 output lane pack 交给 OIR SLP；支持直接连续 lane load、common×lane-load，以及两个均连续的 lane-load stream；覆盖无条件循环、共用条件 diamond 和 lane-local diamond chain，lane-local load 共享精确 mask；更新可位于条件 true/false arm，false arm 使用精确反向 mask；重复 common load 以零索引 masked gather 保持 guard 访存语义，并在 RVV 选择为零步长 `vlse32.v`；f32 保持每个输出通道的累加顺序，动态范围保留标量 tail，其他 SCoP 形态仍由普通 Loop/SLP 重新分析 |
| 复杂 CFG/mixed conversion/无法物化 range | 拒绝 | 给出稳定 reason；`force` 不绕 legality |

## RVV 后端

| 能力 | 状态 | 当前边界 |
| --- | --- | --- |
| SEW/LMUL/VL/VTYPE/policy | 完成 | 当前公开 element 为 e32；MF2/M1/M2/M4/M8 |
| fixed oversized chunk | 完成 | N=33/63/65、partial/dynamic EVL、跨片 memory/reduction |
| unit / ordered indexed / signed fallback | 完成 | `vle/vse`、`vloxei/vsoxei`、`vfirst` 保序 fallback |
| strided memory | 完成 | 严格 AP 证明后选择 signed-byte `vlse32/vsse32` |
| segment memory | 部分 | 相邻 stride-2、同 type/index/mask/EVL 的 NFIELDS=2；非通用 tuple RA |
| vector register allocation | 部分 | LMUL group、v0、fractional、call clobber、whole spill 完成；一般 copy coalescing 与 LMUL relegalize retry 未完成 |
| scalable frame/spill | 完成 | `vlenb` 动态 frame、whole-register load/store、16-byte SP |
| VL/VTYPE CFG dataflow | 完成 | call/state clobber、scheduler dependency、Final verifier |
| VV/VX/VI/VF selection | 部分 | 生产路径主要 splat + VV；scalar/immediate forms 尚未完整接通 |
| final assembly | 完成 | pseudo expansion 后强制 Final verifier；GNU as/objdump/QEMU 门禁 |

## ABI、部署与目标

| 能力 | 状态 | 当前边界 |
| --- | --- | --- |
| standard LP64D aggregate ABI | 完成 | direct/split/byref/sret、float-vector GPR、packed mask、GCC/Clang 双向 |
| rv64gc portable fallback | 完成 | ABI boundary 保持；最终 ELF 无 V/Zve，`v=false` 执行 |
| fat rv64gc + RVV | 完成 | direct/recursive/standard extern、scalar variadic declaration call |
| `psabi-vector` | 拒绝（staged） | fixed classifier/lowering/Clang slice 已有；source tuple、per-extern CC、GCC 双向未闭合 |
| `-mrvv-vector-bits=scalable` | 完成 | VLA contract |
| `-mrvv-vector-bits=N` standard profile | 部分 | 解析/校验存在；尚未成为 fixed legalizer/cost 的统一有效 VLEN |
| vector/mask varargs | 拒绝 | 不猜测 ABI |

## 验证与发布状态

- QEMU 语义矩阵覆盖 VLEN 128/256/512/1024；MF2/M1/M2/M4/M8 各覆盖
  `0, 1, VLMAX±1, 2*VLMAX±1` 与确定性大长度，共 360 次边界执行。
- 随机差分当前覆盖 Yoolang scalar、Yoolang RVV 和逐 lane oracle；GCC/Clang 尚未编译
  同一随机 kernel body 作为第四/第五条 oracle。
- 正式性能脚本、manifest、cycles/instructions/IPC/阈值判定已实现；当前机器不是原生 RVV
  固频硬件，正式性能门未运行。因此仓库当前状态是“功能实现收尾，未达到 Release/GA”。
