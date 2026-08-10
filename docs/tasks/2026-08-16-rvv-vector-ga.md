# RVV 1.0、自动向量化与一等 vector/mask 类型 GA

状态：实现收尾；**未达到 Release/GA**（仍有本地功能缺口与真实硬件性能门）

## 目标

在保持 `rv64gc/lp64d` 标量行为兼容的前提下，完成用户任务书规定的 SysY 固定长度
`vector<int|float,N>`、`mask<N>`、YIR/OIR fixed/scalable vector、Loop/SLP 向量化、
RVV 1.0 VLA 后端、标准 aggregate ABI、可选 vector ABI、验证、差分测试、性能门禁与文档。

本任务不通过删除测试、放宽断言、无条件 XFAIL、跳过 verifier 或硬编码输入制造通过；
不提交、不推送。

## 初始基线（2026-08-16）

- 工作树：`rvv` 分支，开始时干净；HEAD `e0757f3`。
- 前端：`BuiltinType` 仅有 `Void/Int/Float`，尚无结构化源类型或表达式 resolved type。
- YIR/OIR/MIR：均无 vector/mask 类型或 RVV 指令；MIR 仅有 `GPR/FPR32`。
- 目标：固定 `rv64gc/lp64d`；CLI 仅正式接受 `-O1`。
- 工具：存在 `riscv64-linux-gnu-gcc`、`riscv64-linux-gnu-objdump`、`qemu-riscv64`、
  `clang`、`llvm-mc`；未发现 Spike。真实 RVV 硬件尚未确认。
- `xmake -v`：通过。
- `scripts/run_tests.py --suite stage`：1192 passed，0 failed。

## 分阶段实施

1. 统一 `TargetMachine/TargetProfile`、`DataLayout/TypeSize`、结构化常量、builtin registry、
   RISC-V CC 与机器指令描述；消除 O0/优化 lowerer 的类型/ABI 分叉。
2. 建立 `TypeSyntax`、interned `SemanticType`、`SemanticModel`、常量求值和 source-range 诊断；
   解析并验证 fixed vector/mask。
3. 扩展 YIR/OIR 类型、操作、常量、printer/parser/verifier，并实现无 V 目标的完整标量化。
4. 实现 Loop Vectorizer、SLP Vectorizer、remarks、alias versioning、VLA strip-mining 与 RVV 成本模型。
5. 扩展 MIR、RVV legalizer/ISel、vector RA、scalable spill、VL/VTYPE dataflow、pseudo expansion、
   final verifier 和汇编器属性。
6. 实现 standard aggregate ABI、显式 psABI vector CC、FFI/分发；完成负向、随机差分、
   多 VLEN、互操作、真实性能与文档验证。

## 验证日志

精确命令与结果持续追加；未列出的验证视为未运行。

```text
xmake -v
  PASS: build ok, 9.152s

scripts/run_tests.py --suite stage
  PASS: 1192 passed, 0 failed, 0 skipped, 0 xfailed, 0 xpassed
```

## 实施日志（尚未达到 GA）

### 已落盘的基础设施

- 新增统一 `TargetMachine/TargetProfile`、目标特性与 RVV VLEN 配置解析；默认仍为
  `rv64gc/lp64d`。`psabi-vector` 已分阶段接入 entry/call/return lowerer、MIR/RA、scalable
  callee-save frame 和 AsmPrinter。它要求显式 numeric ABI_VLEN 与 target minimum VLEN
  一致，按 v0、v8-v23、LMUL1/2/4/8、间接 fallback、返回/sret 分类，并对定义和引用
  生成 `.variant_cc`/`STO_RISCV_VARIANT_CC`。公开 CLI 仍因 source/OIR tuple NFIELDS 缺口与
  GCC 15 无法表达 `riscv_vls_cc` 而 fail-closed；scalable signature 和 vararg 也精确拒绝。
- `-mcpu/-mtune` 已从任意字符串透传改为严格、带完整参数表的
  `generic-rv64`/`generic-rvv` registry；unknown name 稳定拒绝，`generic-rvv` 在无显式
  `-march` 时推导 `rv64gcv`，显式 `-march/-mtune` 保持覆盖语义。独立
  `RVVTargetCostModel` 已接 Loop adapter，区分 unit/strided/indexed/segment、mask、reduction、
  `vsetvl`、LMUL/mask ratio、寄存器压力、预测 spill、alias setup、code growth 与 short-trip
  break-even；统一 cost report 和 vector-plan JSON 记录实际 tune/成本证据。O3 现可为
  已证明的 simple canonical/rotated constant-stride plan 选择真实 factor-2 两 chunk VLA
  recipe：每组独立 setvl/EVL/mask/SSA/pointer base，回边使用 `vl0+vl1`；模型完整计两份
  body/vset/code、压力和有界 memory-pipeline overlap credit。reduction、diamond、runtime
  versioning 和复杂 live-out 以 `INTERLEAVE_FACTOR_2_RECIPE_UNAVAILABLE` 降级 factor 1，
  O2 始终 factor 1，且不存在 plan2/transform1。
- O0、O1、O2、O3 和大函数均进入同一 vreg/RA lowering 路径；已移除活跃流水线中的
  stack-slot fallback。共享 `RISCVCallingConvention` 已用于标量函数入口、调用、返回及
  结构化变参分类；普通调用不再按 `putf` 名字特判。固定参数采用 LP64D named 规则，
  只有变参 tail 的浮点值转入整数参数规则；vector/mask/aggregate 变参稳定拒绝。standard
  fixed vector/mask aggregate 的多位置、by-reference 与 hidden sret 已由同一分类结果端到端
  lowering；scalar-only entry 保留原 fast path。
- 新增 OIR `ElementCount`、fixed/scalable `VectorType`、结构化 interning、统一
  `DataLayout/TypeSize`、typed aggregate/vector/mask constants 以及 fail-closed verifier。
- 新增前端 `TypeSyntax`、source range/diagnostic、interned `SemanticType`、
  `SemanticModel`、checked constant evaluator、builtin registry，并完成 vector/mask 词法、
  解析、AST 打印及 bitwise precedence。AST→YIR 现强制消费权威 SemanticModel，覆盖
  fixed vector/mask 的 global/local/array/参数/返回、literal/zero/splat/cast、逐 lane
  算术/比较/bitwise、lane、shuffle、memory intrinsic 和 reduction；整数 reduction 为
  unordered，默认浮点 reduction 为 ordered。
- YIR 已有 fixed vector/mask 类型、typed constants、逐 lane算术/比较、splat、
  stepvector、extract/insert/shuffle/select/cast、masked memory、gather/scatter、reduction
  的结构化 operation、printer 与 verifier。
- OIR 已有 vectorization remark、LoopAccess/legality/cost、`setvl` 与 transforming Loop
  Vectorizer。已生成真实 `remaining != 0` / `setvl` / EVL / IV 更新 CFG，支持
  Add/Mul/And/Or/Xor i32 reduction、同 op 的线性 unrolled reduction chain、rotated
  单块循环、pointer induction 和单层 diamond
  if-conversion。diamond 使用 `then = active & cmp`、`else = active ^ then`，分支内存按
  mask 执行，merge PHI 转为 vector select。变换以可回滚事务执行，只有
  post-transform verifier 通过才记录 `VECTORIZED`。已支持 array affine 与 pointer-phi
  的 signed stride `+1/+2/+4/-1/-2/-4`；正向 `+1` 使用连续 VP memory，正向
  `+2/+4` 使用 `stepvector * stride` 的 gather/scatter。负步长以
  `low_base = lane0 + (VL - 1) * stride` 和
  `index = ((VL - 1) - stepvector) * abs(stride)` 生成非负、无重复 e32 索引，保持
  canonical reverse loop 的标量 lane 顺序，并对最大架构 VLEN 下的元素及字节偏移做
  i32 溢出证明；不可证明则 fail closed。同 lane read-modify-write 可证明安全；其余
  unknown alias 默认保守拒绝；对所有 pair 都能构造完整 affine i32 byte range 的
  zero-based forward two-block loop，以及带零趟 guard 和规范 induction exit phi 的
  rotated single-block loop，现会调用纯 `__yoolang_ranges_disjoint` 做
  overflow-safe runtime versioning。所有 pair 取 AND，overlap/exact/溢出走完整 scalar
  clone，disjoint 走 scalable VP fast path；事务与最终 verifier 成功后才发布 remark。
- Transforming SLP 已接入 O3：支持 fixed i32/f32、N=3/7 非 2 幂 pack、连续
  load/store、同 opcode 算术、i32/f32 compare、i1 mask And/Or/Xor 和结构化 splat；
  按序 extract pack 会直接复用同一 vector producer，形成 load→arithmetic→compare→mask
  多层树而不制造 extract→insert 往返。别名、顺序、call、trap、依赖链和目标特性
  fail closed。新增审计过的 vector cleanup，只清除无副作用的死 vector/SetVL/GEP，
  不触碰 memory、reduction、div、call 或 control op。
- OIR 已有完整文本 parser：支持 scalar/fixed/scalable/VP 全部当前 operation、typed
  constants、前向 SSA、phi 回边与 variadic function type；parse 后强制 verifier，并提供
  带行列范围的 fail-closed 诊断和 printer→parser→printer round-trip 测试。
- 新增 table-driven `MachineInstrDesc` 并让 scheduler、诊断和若干 MIR 优化消费 descriptor；
  MIR 已建模 logical fixed/scalable vector、SEW、LMUL、mask ratio、VR/VMASK/VRNoV0/VSTATE、
  结构化 RVV pseudo、implicit VL/VTYPE/FRM、scalable slot 及 PreRA/PostRA/Final verifier。
  专用 vector RA 已实现 LMUL group/alignment/overlap、v0/VRNoV0、fractional LMUL、
  call clobber、destructive-tie 合并、whole-register scalable spill 和结构化 relegalization
  请求；当请求发生时当前仍 fail closed，尚没有降低 LMUL/拆分并重试的消费者。
  fixed logical lane count 与 LMUL container 独立，支持 N=3/7/31。已有 OIR→RVV 主链
  `setvl`、mask splat、VP unit load/store 与 int/float VP binary，PostRA pseudo expansion
  转成非 pseudo Final opcode，MIRToAsm 前强制 Final verifier。固定 N=1/3/7/31 的 literal、
  splat、step、整数/浮点算术、比较、mask logic、select、数值 cast、extract/insert/shuffle、
  phi 与 fixed EVL/TU/MU 已贯穿真实 RVV assembly。普通 mask value 分配到 v1-v31，只有
  masked/merge use 通过显式 mask copy 使用 v0。VL cache 现同时跟踪 AVL identity 与 policy；
  fixed passthrough copy 先以 VL=N 完整保留对象再恢复 EVL，scalable copy 先以 VLMAX 完整复制
  再恢复原 AVL/policy。
- scalable frame 已按运行时 `vlenb` 分配并保持 16-byte SP 对齐；m1/m2/m4/m8 whole-register
  spill/reload 使用真实 `vsNr.v`/`vlNre32.v`，入口 stack argument 地址计入动态 frame。
  N=31/M8 压力、跨普通/嵌套调用与第九个栈参数已在 O0-O3、VLEN 128/256/512/1024 执行。
  默认 public ABI 的 fixed vector/mask 已按普通 LP64D aggregate 完整接入：<=16B 在
  GPR/stack 分片，12B 在七个先行 GPR 后按 `a7 + stack0` 拆分且后续 scalar 位于 stack8；
  `vector<float,3>` 严格走整数 aggregate 位置，`mask<31>` packed align1 且出站清高位；
  >16B 使用 caller-owned by-reference temporary，返回使用 staged hidden sret pointer。
  原 7 个互操作例加 by-ref/sret、反向 split、递归/嵌套共 11 例，均通过 GCC/Clang、
  O0-O3、VLEN=128/256/512/1024，并由 GNU as/readelf/objdump 审计；门禁已晋升
  auto-discovered toolchain infra。
  fractional LMUL 与 ordinary mask 也按物理 group 宽度保存（MF2/mask 均为 1×vlenb），
  并已通过 call-live 与 32-value 压力。ordered indexed memory 已接入：可证明非负且
  byte offset 适配 u32 的索引使用 `vloxei32/vsoxei32`，其余 signed index 以 `vfirst`
  按 active lane 顺序 scalar fallback；整数 Add/Min/Max/And/Or/Xor、ordered f32 Add、
  mask any/all/parity 有原生 reduction，整数/浮点 Mul 与严格 f32 Min/Max 保序 fallback。
  oversized fixed value 已有独立结构化 chunk planner：在 minimum VLEN=128、e32 下
  N=33/63/65 分别规划为 32+1、32+31、32+32+1，逐片选择最小合法 MF2/M1/M2/M4/M8，
  data byte offset 与 mask packed-bit offset 连续无 padding。OIR→MIR lowerer 已消费该计划：
  每个 OIR SSA 映射为普通 piece vreg bundle，RA 不认识 tuple；literal/splat/iota、逐片算术/
  compare/mask/select、dynamic extract/insert、shuffle、load/store、fixed-full-EVL masked memory、
  phi、global、standard aggregate direct/byref/sret/call/return 均保持逻辑 lane 与 packed offset。
  N=33/63/65 已在 O0-O3 与同一 binary 的 VLEN=128/256/512/1024 执行，mask 最终高位为零。
  scalable type 也不再硬编码 M1，而从 minimum lanes 推导 MF2/M1/M2/M4/M8；成本模型移除
  会要求非法 vbool128 的 fractional `mf4`，plan 与最终 `vsetvli` 现保持一致。
- 无 V 目标现只在进入 target-specific MIR 时运行事务化 portable scalarizer；
  `--emit-oir` 继续保留 target-independent typed vector。source-local fixed SSA 覆盖
  N=1/3/7/31、int/float cast、mask logic/compare/select、literal/splat、dynamic lane、shuffle
  与 fixed-full-EVL VP/masked memory；生成的 rv64gc executable 已验证全局无 RVV opcode并在
  `qemu-riscv64 -cpu rv64,v=false` 执行。fixed-only `abi.fixed.extract/pack/load_lane/
  store_lane` boundary dialect 现保持 vector/mask global、array object、公开函数参数/返回与
  `ptr<vector>` 的 ABI/layout identity，只把函数体转换为 scalar lane；VReg lowerer 继续消费
  同一 standard LP64D assignment，覆盖 direct/split、byref+sret、递归/嵌套调用以及 packed
  mask bit RMW/末字节清高位。GCC/Clang 双向 O0-O3 与 `rv64,v=false` 已通过，最终 object/
  executable 全局无 V/Zve opcode 与属性；裸 vector OIR 绕过 scalarizer 仍精确 fail closed。
- builtin `select` 现对 numeric vector 与 mask value 共享一个结构化 type pattern：
  `(mask<N>, vector<T,N>, vector<T,N>) -> vector<T,N>` 与
  `(mask<N>, mask<N>, mask<N>) -> mask<N>` 均由同一 registry/semantic binding 实例化，
  不允许 vector/mask 混合。mask-result 路径在 rv64gc 上完全 scalarize、在 rv64gcv 上生成
  真实 `vmerge.vvm`，并已在 VLEN=128/256/512/1024 执行。
- 修复 i1 对象布局债：OIR→MIR 对象为 size/align 1，普通 i1 memory 使用 `lbu/sb`；
  RA spill 仍使用独立 4-byte legal slot，并由 stack-slot size 明确区分。
- MIR global initializer 现只保存精确 little-endian typed bytes；OIR Constant tree 负责按
  DataLayout 编码 i1/i32/f32、array、fixed vector 和 packed mask，AsmPrinter 已删除文本扫描、
  `stof/stoll` 及按 IR 字符串猜 float 的路径。YIR/OIR legacy textual initializer API 与
  parser 路径已删除；只接受 typed constant tree。
- CI 已加入 infra、FileCheck 与 O0 functional 回归门；原 O1 functional 门保留。
- RVV 随机差分默认 smoke 现直接执行 34 个 run + 5 个 runtime-versioned success plan +
  7 个稳定 reject，地址偏移强制包含
  真正的 1/2/3 byte 非对齐，并在 scalar 与 VLEN=128/256/512/1024 间比较；另有显式
  extended/nightly deterministic tier。runtime failure 会最小化 length/alignment/seed，
  保存原始与 minimized replay artifact，required case 不使用 XFAIL/skip。
- Linux runtime 已新增弱符号 `__yoolang_rvv_available`：硬件能力按 HWCAP 与 hwprobe
  一致性 fail closed，线程 vector control 每次用 prctl 查询；硬件事实可缓存，线程状态不缓存。
  detector 本身只按 rv64gc 编译、不执行 RVV，也不使用 SIGILL probe 或擅自开启线程 vector state。
- runtime archive 已加入独立 `alias_runtime.c`。其 `uintptr_t` half-open range helper 不解引用
  pointer，并对 `UINTPTR_MAX` 附近的乘/加/减溢出 fail closed；builtin registry 与 OIR ModRef
  将编译器内部 call 标为 `MemoryEffect::None` 且无可观察副作用。
- `-mrvv-deployment=fat` 已接入首个可执行 compiler slice：从同一 verified OIR 事务化克隆
  rv64gc portable-scalar 与 rv64gcv RVV 两个独立 pipeline，所有 source definition 整体改名，
  因而 direct call/recursion 固定落到同一 variant；公开 scalar-ABI dispatcher 在 detector
  缺失/返回 0 时 tail-call scalar，只在非零时选 RVV。variant 为 hidden，用户原符号保持
  default visibility；ELF arch attribute 仍为 rv64gc，RVV text 仅置于局部
  `.option push/.option arch,+v/.option pop`。fat optimizer 禁用签名破坏型 DAE；standard
  fixed vector/mask aggregate public ABI 已端到端开放，dispatcher 完整保存 a0-a7、fa0-fa7、
  stack argument 与 hidden sret 语义。direct extern/runtime declaration 不改名，两 variant
  按同一 LP64D standard ABI 调用同一 UND symbol；declaration-only scalar variadic direct call
  也保持 standard ABI，variadic source definition、vector/mask variadic tail、indirect/
  address-taken、scalable、psabi-vector 与 numeric fixed-VLEN 仍稳定 fail closed。
  组合器同时保留只由 RVV lowerer 生成的 typed vector/mask constant-pool objects，公共 global
  必须在两分支的 section、size 与 initializer bytes 完全一致，否则以
  `FAT_ASSEMBLY_GLOBAL_MISMATCH` 事务化拒绝；不会再因只截取 RVV `.text` 而留下未定义常量。
- 标量 inliner 现在会在任何 clone/mutation 前识别 vector/mask signature、typed
  vector constant 或 vector/VP body，用稳定原因拒绝而不会半克隆 vector callee。

### 已执行的分层验证

以下结果是对应切片完成时的记录；共享工作树后续继续变化，最终仍需重新运行全量矩阵。

```text
python3 scripts/frontend_infra_tests.py
  PASS: 12/12（FE1a 完成时）
python3 scripts/builtin_infra_tests.py
  PASS: 3/3
python3 scripts/yir_infra_tests.py
  PASS: 3/3
python3 scripts/yir_vector_tests.py
  PASS: 4 groups
python3 scripts/yir_to_oir_vector_tests.py
  PASS: 2/2（typed constants 与 fixed vector/VP lowering）
python3 scripts/oir_infra_tests.py
  PASS: 31/31
python3 scripts/oir_parser_infra_tests.py
  PASS: 5/5
python3 scripts/vectorization_remark_tests.py
  PASS
python3 scripts/vectorization_analysis_tests.py
  PASS: 5/5
python3 scripts/calling_convention_tests.py
  PASS: 9/9（varargs、4/8/12/16B aggregate pieces、float-vector GPR、mask31、split、indirect、sret）
python3 scripts/psabi_vector_calling_convention_infra_tests.py
  PASS: 10/10（ABI_VLEN、v0/v8-v23、LMUL/tuple、fallback、sret、保存集与负例）
python3 scripts/psabi_vector_abi_e2e_infra_tests.py
  PASS（O0-O3、int/float/mask、N33 indirect/sret、跨 call/phi/压力、GNU ELF、
        Clang 双向互调、同一 binary 的 VLEN=128/256/512/1024）
python3 scripts/i1_layout_tests.py
  PASS: 2/2（相邻 i1 对象/数组布局、lbu/sb 与 4-byte spill 分离）
python3 scripts/target_infra_tests.py
  PASS: target harness + CLI（严格 CPU/tune registry、普通 -O3 -march=rv64gcv 的真实
        factor-2 plan/OIR、O2 factor 1、结构化 --emit-vector-plan JSON）
python3 scripts/rvv_cost_model_infra_tests.py
  PASS: 8 groups（CPU/tune 参数表与 shared report 映射；unit/strided/indexed/segment；
        mask/reduction；LMUL/pressure/predicted spill；code-size/short-trip break-even；force
        legality boundary；factor-1/2 选择、压力回退与 capability gate；JSON/CLI schema）
python3 scripts/typed_global_infra_tests.py
  PASS: 3/3（scalar/array/N=3,N=7/mask<10> exact bytes、legacy gate、ASM）
python3 scripts/mir_infra_tests.py
  PASS: 29/29（vector model、分阶段 verifier、LMUL/mask RA、indexed/reduction descriptor、Final gate）
python3 scripts/loop_vectorizer_infra_tests.py
  PASS: 29/29（VLA CFG、真实 factor-2 canonical/rotated constant-stride 两组 setvl/EVL、
        O2/reduction/diamond/versioning 降级与 rollback、普通及 guarded rotated diamond/mask/PHI、零趟共同 exit、
        0/1/VLMAX±1 inactive-lane 模型、非负 reverse indexed 配方与溢出证明、
        canonical/rotated reduction 与 reverse loop、guarded two-block preheader、线性 unrolled
        integer reduction chain、rotated runtime alias fast/slow/exit-liveout、rollback）
python3 scripts/rvv_interleave2_infra_tests.py
  PASS（普通 O3 -march plan2、O2 plan1、两组独立 OIR；GNU as/objdump；QEMU
        VLEN=128/256/512/1024 的 0/1/VLMAX±1/2VLMAX±1/随机大长度、offset 0/1 与 tail guard）
python3 scripts/rvv_perf_corpus_vectorization_infra_tests.py
  PASS: transpose2 的 main:init 从 0-vector 变为 verified VECTORIZED；原 4 个 unknown-alias
        loop 仍稳定 REJECT_ALIAS；OIR 双 predicated VP store；GNU objdump 解码真实
        vsetvli/vid/vse32/vmseq/vmand；rv64gc scalar 与 RVV VLEN=128/256/512/1024 输出一致
python3 scripts/rvv_runtime_alias_versioning_infra_tests.py
  PASS: disjoint/overlap/exact/zero/negative/stride0/reverse 与 UINTPTR_MAX 附近溢出整数模型；
        0/1/VLMAX±1 fast/slow routing；PROT_NONE guard pages 无解引用；UBSan clean
python3 scripts/rvv_alias_versioning_perf_corpus_infra_tests.py
  PASS: 01_mm3 的 mm:while.cond.16 从 0-vector 变为 two-pair verified VECTORIZED；OIR 保留
        scalar slow clone，ASM/objdump 有 helper call 与真实 RVV；corpus scalar 对 VLEN
        128/256/512/1024 一致；实际 mm disjoint/exact/partial-overlap driver 四种 VLEN 全通过
python3 scripts/rvv_remaining_perf_corpus_infra_tests.py
  PASS: many_mat_cal-3/matmul2/fft2 的 verified loop plan 分别为 1/1/6；OIR 锁定 guarded
        reduction、stride-4 四路 gather/reduction chain、rotated alias fast/slow/三路 exit phi；
        GNU objdump 解码真实 RVV，rv64gc scalar 与 VLEN=128/256/512/1024 corpus 输出一致；
        实际 fft2 memmove1 的 disjoint/exact/双向 overlap/zero/VLMAX±1/PROT_NONE guard 全通过
python3 scripts/slp_vectorizer_infra_tests.py
  PASS: 19/19（i32/f32/N=3/7、producer reuse、compare/mask tree、legality/cost/rollback）
python3 scripts/oir_inline_infra_tests.py
  PASS: 4/4（vector-aware pre-clone gate）
python3 scripts/portable_vector_scalarizer_infra_tests.py
  PASS: 7/7（lane scalarization、CFG/masked-memory、phi、rollback、稳定拒绝）
python3 scripts/portable_vector_e2e_infra_tests.py
  PASS: 7 groups（typed OIR；scalar MIR/ASM；GNU as；全 executable objdump；
        rv64,v=false 15 cases；三类 aggregate ABI 负例）
python3 scripts/vector_numeric_semantics_infra_tests.py
  PASS: 4 groups（N=1/3/7/31；i32 wrap/div/rem 边界；f32 NaN/Inf/±0/subnormal；
        ordered reduction；typed OIR、portable MIR、rv64gc GNU as 与 rv64,v=false bit-exact）
python3 scripts/rvv_perf_infra_tests.py
  PASS: 34/34（v3 manifest、每个 expected kernel 的 fail-closed verified-loop minimum、
  perf/objdump/vector-plan/vlenb/post-RA MIR parser、精确 whole-register spill 字节、native
  环境拒绝与五项 Release 阈值；SLP success 不能冒充 loop minimum）
QEMU 11.0.2 vlenb probe（仅语义，不计时）
  PASS: VLEN=128/256/512/1024 分别读取 vlenb=16/32/64/128
transpose2 真实分析链（不计时）
  PASS: vector-plan 29 条（1 VECTORIZED/28 稳定拒绝）；post-RA/ELF 均为 0 whole spill，
  ELF 解码 25 条 RVV、9 条 vsetvl
python3 scripts/vectorization_docs_infra_tests.py
  PASS: 4 groups（Loop/SLP OIR、plan、remarks、target/ABI gates）
python3 scripts/run_tests.py --suite filecheck --filter frontend_vector_exprs --jobs 1
  PASS: frontend→YIR 与 frontend→OIR 两条 vector/mask RUN
python3 scripts/run_tests.py --suite filecheck --filter frontend_bitwise_scalar --jobs 1
  PASS: YIR/OIR/MIR scalar bitwise lowering
qemu-riscv64 -cpu rv64,v=false /tmp/yoolang_frontend_bitwise_scalar
  PASS: exit 0
python3 scripts/run_tests.py --suite stage --stage oir --jobs 4 --o1
  PASS: 298/298（OIR verifier/constant 切片完成时）
python3 scripts/run_tests.py --suite stage --suite e2e --test-root test/functional \
  --opt-level {0,1,2,3} --jobs 4
  PASS: O0/O1/O2/O3 各 530/530，0 failed、0 skipped
python3 scripts/run_tests.py --suite infra --jobs 1 --infra-timeout 120
  历史基础切片 PASS: 22 scripts，0 failed；该结果早于当前完整 host/toolchain 集合，不能作为
  最终共享树全量证据。随机差分 required 集合随后已达到
  34 run / 5 verified plan / 7 stable reject / 0 blocked。
python3 scripts/rvv_differential_tests.py
  PASS: scalar oracle 21 cases；RVV VLEN=128/256/512/1024 各 21 cases；
        SLP scalar oracle 12 cases；SLP 四个 VLEN 各 12 cases
python3 scripts/rvv_guard_page_tests.py
  PASS: scalar control 与 RVV VLEN=128/256/512/1024，mask/tail 未触及 PROT_NONE 页
python3 scripts/rvv_mask_guard_tests.py --timeout 30
  PASS: all-false load/store 直接指向 PROT_NONE；sparse mask 仅 lane 0/2 active，其余 lane
        跨入 guard page 仍不访问；同一 binary 在 VLEN=128/256/512/1024 共 8 个执行 case
python3 scripts/rvv_backend_infra_tests.py
  PASS: 8/8（fixed/scalable EVL、TA/MA↔TU/MU、MF2/mask spill、Final 零 pseudo、四 VLEN QEMU）
python3 scripts/rvv_indexed_reduction_backend_infra_tests.py
  PASS: 8/8（ordered ei32、signed fallback、duplicate scatter、PROT_NONE/EVL0、整数/strict-f32/
        mask reduction、Final/GNU as/objdump 与 VLEN=128/256/512/1024）
python3 scripts/rvv_fixed_vector_legalization_infra_tests.py
  PASS: 7 groups（N=1/3/7/31/33/63/65、VLEN128/256、float/mask packed layout、large/overflow）
python3 scripts/rvv_fixed_chunk_backend_infra_tests.py
  PASS: 13/13（N=33/63/65，i32/f32/mask、VP/load/store/shuffle/phi/call 压力；
        O0/O1/O2/O3 × 同一 ELF VLEN=128/256/512/1024；GNU as/objdump、Final 零 pseudo）
python3 scripts/rvv_scalable_lmul_consistency_infra_tests.py
  PASS: MF2/M1/M2/M4/M8 minimum-lane contract 到 Final ASM/objdump；fractional mf4/mf8 与
        超过 M8 capacity 的 scalable shape 精确 fail closed
python3 scripts/rvv_lmul_vlen_boundary_infra_tests.py
  PASS: MF2/M1/M2/M4/M8 × VLEN=128/256/512/1024 ×
        {0,1,VLMAX-1,VLMAX,VLMAX+1,2*VLMAX-1,2*VLMAX,2*VLMAX+1,随机大长度}
        × 2 种 byte alignment，共 360 次 QEMU；scalar oracle、canary、PROT_NONE guard、
        GNU as/link/objdump 与 Final 零 pseudo 全通过
python3 scripts/rvv_fixed_value_backend_infra_tests.py
  PASS: N=1/3/7/31，O0/O1/O2/O3 × VLEN=128/256/512/1024；N31/M8 whole spill 与 call-live
python3 scripts/rvv_runtime_dispatch_infra_tests.py
  PASS: 12/12（HWCAP/hwprobe/prctl、线程状态、强符号 override、rv64gc 无 RVV）
python3 scripts/rvv_fat_eligibility_infra_tests.py
  PASS: variadic、function-address/indirect、unsupported ABI、reserved symbol、malformed input
        的稳定原因；direct recursion、fixed aggregate 与 direct external 支持集；以及 RVV-only
        constant pool 保留/公共 global mismatch fail-closed
python3 scripts/rvv_fat_multiversion_infra_tests.py
  PASS: GNU as/readelf/objdump；combined object/executable Tag_RISCV_arch=rv64gc；scalar 与
        dispatcher object range 无 RVV；detector 缺失/0/真实 prctl-EINVAL 走 scalar；detector=1
        在 VLEN=128/256/512/1024 走 RVV；source-local fixed N=3/7/31 同矩阵；10 个 GPR/FPR/
        stack 参数跨 dispatcher 保真；recursive call rewrite 与 hidden/default visibility
python3 scripts/rvv_fat_standard_abi_infra_tests.py
  PASS: 14 cases × O0/O1/O2/O3 × GCC/Clang；public N=1/3/7/31/33、large byref/sret、
        mask31/33 tail、float union、a7+stack、global/array、extern/runtime、recursive/nested；
        detector missing/0/1，v=false 与 VLEN=128/256/512/1024；ELF attr/visibility/range/UND/
        reloc/objdump 均通过，scalar/dispatcher 零 RVV、RVV variant 有 decoded RVV
python3 scripts/rvv_random_differential_model_infra_tests.py
  PASS: 22/22（stable seed/case-id/manifest/replay/artifact/repro、mask/f32/indexed/reduction oracle 与 mandatory-dimension 合同）
python3 scripts/rvv_random_differential_infra_tests.py
  PASS（34 个实际 RUN〔含 all-false/sparse PROT_NONE mask、IEEE f32 bit-pattern oracle、
       +2/+4/-1/-2/-4 ordered indexed 与 Add/Mul/And/Or/Xor source reduction〕的 rv64gc
       scalar 与 VLEN=128/256/512/1024 全一致；另 5 个 alias runtime-versioned success plan、
       7 个 dynamic-stride/strict-FP 稳定 reject；0 blocked）
xmake build compiler
  PASS: Final RVV 主链、LV2、SLP 与 typed-global 集成后链接成功
riscv64-linux-gnu-gcc -static -march=rv64gcv -mabi=lp64d <LV assembly + runtime>
  PASS: GNU as/binutils 2.44
qemu-riscv64 -cpu rv64,v=true,vlen=128|256|512|1024,elen=64 <same LV binary>
  PASS: 四个 VLEN 输出一致
```

Final pseudo expansion 引起的旧输出契约回归已收口：plain `--emit-mir`
从 diagnostics artifact 输出 post-RA 可读 pseudo MIR，但 pipeline 仍继续执行
pseudo expansion 和 Final verifier；ASM FileCheck 改为等强的精确真实指令序列，
没有改成宽泛 wildcard。普通 mask 现在先占用 v1-v31，并在执行点以精确
`vmand.mm v0,vs,vs` 复制到 v0；对应 ASM 检查继续锁定紧随的 `v0.t` use。主代理复跑
`python3 scripts/run_tests.py --suite filecheck --jobs 4`：49/49 PASS。

正式性能报告不再把 `spill_like_count` 当作 VR spill 证据：原生门先读取实际 `vlenb`，
从最终 ELF 的 `vs{1,2,4,8}r.v`/`vl{1,2,4,8}re*.v` 精确换算静态 store、reload
和总传输字节；另以 post-RA MIR 统计唯一 spill/callee-save slot 字节并与 ELF site 数
交叉校验；缺 vlenb 或字段不一致即 fail closed。每个 workload 还额外运行不计时的
`--emit-vector-plan` 分析编译，保存带 SHA-256 的原始 JSON，并在 JSON/Markdown 报告中
逐条记录 vectorizer、function/region、`VECTORIZED` 或稳定拒绝码及说明。

### 当前明确未完成项

- RVV Legalizer 还没有把 scalar/immediate operand 选择成真实 VX/VI/VF family；当前生产路径
  主要使用 splat + VV。相关 opcode/verifier 骨架存在，但 lowerer/pseudo expansion 未完整接通。
- vector RA 的高压力 relegalization 目前只产生结构化 request 后 fail closed；尚无降低 LMUL、
  拆分或重试。专用 vector allocator 只有 destructive tie 合并，没有一般 group-aware copy
  coalescing。
- 基础 2-field segment memory 已接通真实 `vlseg2e32.v`/`vsseg2e32.v`，但仅覆盖严格相邻、
  stride=2、同 type/index/mask/EVL、TA/MA 的模式，并使用受 verifier 约束的 scratch tuple；
  尚不是通用 NFIELDS tuple RA。
- `-mrvv-vector-bits=N` 在 standard 模式会被解析，但 fixed legalizer/cost 仍主要消费
  `minimum_vlen_bits`，尚未形成有可观察代码生成差异的正式 fixed-VLEN profile。
- standalone compile-time `psabi-vector` 底层已有 classifier、fixed value entry/call/return、
  callee-save、cross-call spill、`.variant_cc` 和 Clang 双向 slice；但 source/OIR tuple、逐声明
  external CC 所有权与 GCC 双向门未闭合，因此公开 CLI 按要求保持 fail closed。
- DataLayout 尚未成为两个 OIR→MIR lowerer 唯一的 size/alignment 来源；inactive stack lowerer
  与 active vreg lowerer 仍各有本地 type-info 计算。
- 随机差分已有 Yoolang scalar/RVV/逐 lane oracle，但尚未让 GCC 与 Clang 编译同一批随机
  kernel 作为额外 oracle。
- Loop Vectorizer 对更复杂 CFG、mixed conversion 与更广 alias-versioning materialization 仍会
  给出稳定拒绝；当前不可完整证明的 alias 不会假定 noalias。
- 正式硬件 performance manifest 的 8 个 expected-vectorizable workload 现已全部产生
  至少一个 verified Loop Vectorizer plan。当前 O3 loop `VECTORIZED` 计数为：01_mm3=2、
  matmul2=1、many_mat_cal-3=1、conv2d-3=6、sl3=1、h-10-03=2、fft2=6、transpose2=2。
  v3 manifest 对每项显式要求 `minimum_verified_vectorized_loops >= 1`；分类、速度阈值和
  unknown-alias fail-closed 规则均未放宽。仍待真实硬件执行正式 cycle 门。
- 集中式 operation/support matrix 与部分历史 YIR/MIR 设计文档仍需同步；真实硬件性能门。

## 当前外部资源状态

- QEMU 11.0.2 已用于 VLEN=128/256/512/1024 同一二进制差分与 guard-page
  语义验证；它不作为真实性能数据。
- 真实 RVV 硬件与固定频率/绑核性能环境未确认；硬件 GA 性能门在获得资源前不得宣称通过。
  `scripts/rvv_hardware_perf.py`、版本化 corpus manifest 和 JSON/Markdown 输出已就绪；当前
  x86 负向探针按预期退出 2，状态 `BLOCKED`、`official=false`、`gate_executed=false`，
  且未启动 compiler、perf benchmark 或 QEMU timing。

## 2026-08-16 最终收尾快照

停止扩展新能力后，对共享最终树执行了以下发布烟测：

```text
xmake build compiler
  PASS
python3 scripts/run_tests.py --suite filecheck --jobs 4
  PASS: 54/54, 0 failed, 0 skipped
python3 scripts/run_tests.py --suite infra --infra-profile host --jobs 4
  PASS: 33/33；另 1 个 toolchain 分组按 profile 正常排除
python3 scripts/run_tests.py --suite infra --infra-profile toolchain --jobs 4
  首轮: 24 PASS / 2 FAIL / 0 SKIP
  两个失败均已定向修复并复跑：
    scripts/rvv_oversized_vector_semantics_infra_tests.py  PASS
    scripts/rvv_random_differential_infra_tests.py         PASS
  修复后未再次运行整个 26-script aggregate；不得把它记录成一次新的全量 26/26。
python3 scripts/rvv_lmul_vlen_boundary_infra_tests.py
  PASS: 360 次 QEMU 边界执行
python3 scripts/yir_vector_tests.py
  PASS: 4/4（含 -0.0f 不得折叠为 aggregate zero）
python3 scripts/vectorization_docs_infra_tests.py --compiler build/linux/x86_64/release/compiler
  PASS: 5/5
git diff --check
  PASS
```

最终结论：当前是“功能实现收尾、主要语义/工具链门通过”，不是 Release/GA。除正式硬件
性能门外，仍有上节列出的 psABI tuple/extern CC、LMUL relegalize retry、VV/VX/VI/VF
选择、通用 tuple RA、numeric fixed-VLEN profile、DataLayout 去重与 GCC/Clang 随机 kernel
oracle 等本地工作。
