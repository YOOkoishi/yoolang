# RVV 原生硬件性能门禁

scripts/rvv_hardware_perf.py 是 Release/GA 性能证据的唯一前端。它比较同一 SysY
kernel 的 rv64gc scalar 与 rv64gcv RVV 构建，使用硬件 perf 计数器采集 cycles、
instructions、IPC 和 wall time，并同时记录编译时间、ELF/.text 大小及反汇编中的
RVV 指令形态。

本页没有 RVV 硬件性能结果。当前开发环境是 x86_64，正式门禁未运行；因此不能据此
宣称达到 GA 性能目标。

## 性能证据边界

正式结果必须同时满足：

- uname/platform machine 为 riscv64；
- procfs/device-tree/DMI 等证据中没有 QEMU、TCG 或 emulator；
- CPU ISA 明确包含 V；
- Linux PR_RISCV_V_GET_CONTROL 可查询且当前 vector state 不是 off；
- 至少一个经 objdump 解码出 RVV opcode 的 RVV benchmark 在该环境成功执行；
- 进程 affinity 精确等于命令指定的单个 CPU；
- require-fixed 模式下该 CPU 的 cpufreq min/max 相等，current frequency 可读，
  且运行中采样相对固定值没有超过 1% 的漂移；
- perf stat 能取得正的 cycles、instructions 和 seconds time elapsed；unsupported、
  not-counted、缺字段或权限失败都拒绝；
- 所有 warmup 和 measurement 的 stdout、exit status 都与仓库 .out 契约一致。

QEMU 只用于语义、多 VLEN、guard-page 和指令形态验证。脚本没有 QEMU 运行选项，
也不会接收 QEMU wall time 作为正式数据。即便 QEMU system 模拟器报告 riscv64 和 V，
emulation evidence 也会使结果成为 BLOCKED。

frequency-policy=record 会完整记录可见频率信息，适合机器 bring-up，但该模式永远
不是 official，退出码非零。Release 门必须使用 frequency-policy=require-fixed。

## Corpus manifest

test/performance/rvv_hardware_manifest.json 是版本化、机器可读的门禁清单。每个 kernel
显式声明 source、input、output、classification、dedupe_group、tags 和 hotspot；每个
expected-vectorizable kernel 还必须声明整数
`minimum_verified_vectorized_loops >= 1`。

两类 classification 是：

- expected-vectorizable：具有真实可向量化工作的矩阵、卷积、3D stencil、TRSM、
  FFT、transpose 等 workload。该类 RVV ELF 必须至少有一个可解码 RVV opcode，并且
  `--emit-vector-plan` 中 loop vectorizer 的已验证 `VECTORIZED` 数不得低于 manifest
  下限；SLP success 不能冒充 loop minimum。
- negative-control：pointer chasing、递归搜索/控制流、强 loop-carried dependence
  和动态规划。它们不要求产生 RVV，但整体回退受到 2% 门限制。

同一 workload 的多个实现或输入通过 dedupe_group 先在组内求 geomean，再在组间求
geomean，避免矩阵家族因样例较多而重复加权。当前仓库 test/performance 没有独立 DCT
源文件，所以 manifest 不虚构 DCT 数据；加入真实 DCT workload 时必须同时提交
.sy/.in/.out 和对应分类。

manifest 中的 Release 阈值不能弱于脚本内建值。删除分类、遗漏结果、空 aggregate、
非正指标或缺失编译时间都 fail closed。

## 固定测量协议

默认协议为 3 次 warmup、15 次 measurement；正式运行至少需要 3/11。scalar 和 RVV
按 AB/BA 交替，降低次序偏差。每个 sample 启动一次 perf stat，主指标是 median cycles；
wall median 和 controller wall 同时保留。每个 variant 的 cycles relative MAD 必须不高于
5%，否则该 kernel 被判为不稳定，不能通过。

编译模式对 scalar/RVV 各做 1 次 compiler warmup 和 5 次计时编译，取 median；link
time 单独记录，不混入 compiler time。所有命令使用 argv 数组，不经 shell；manifest
占位符只允许由前端提供，未知占位符会拒绝。

报告记录：

- 原始 repetitions 以及 median cycles/instructions/IPC/perf wall/controller wall；
- compiler samples、median compile time 和 link time；
- ELF SHA-256、文件大小、.text 大小；
- RVV opcode histogram、vector ALU/load/store、mask、reduction、vsetvl 数；
- vset 指令中的 SEW/LMUL 分布；
- 原生进程实测的 `vlenb`/VLEN；
- whole-register spill/reload 的静态指令点数、`vlenb` 系数以及据实换算的
  `vr_spill_store_bytes`、`vr_spill_reload_bytes` 和两者之和
  `vr_spill_transfer_bytes`；
- 独立 post-RA MIR 分析中的唯一 RA spill slot/callee-save slot 数与实际字节，并按函数
  保留明细；MIR pseudo site 数必须与最终 ELF whole-register site 一一一致；
- compiler `--emit-vector-plan` 的原始 JSON（带 SHA-256）以及逐条
  vectorizer/function/region、`VECTORIZED` 或稳定拒绝码和 explanation；
- 另保留引用 sp/s0/fp 的普通 vector load/store 近似计数 `spill_like_count`，用于人工诊断。

正式的 VR spill 字节证据不使用 `spill_like_count`。脚本先在同一原生进程环境编译并运行
只读 `vlenb` CSR 的探针，再从最终 ELF 反汇编精确识别 `vs1r/vs2r/vs4r/vs8r` 与
`vl1re*/vl2re*/vl4re*/vl8re*`。每个静态点的传输宽度是 group width × 实测 vlenb；
store、reload 和总传输字节分别报告。没有真实 vlenb、字段不一致或缺 vector-plan
证据时正式门 fail closed。另一次不计时的 post-RA MIR 编译按 `kind=spill` 与
`kind=callee-saved` 分开累计唯一 scalable slot；MIR 与最终 ELF site 数不一致同样失败。
这里的 store/reload 字节数是最终二进制中所有静态 whole-register
spill/reload 点各执行一次时的精确传输量，不冒充动态执行次数。

manifest 的 RVV build 明确给出独立 `vector_plan_args` 与 `post_ra_mir_args`，分析编译
不计入 timed compiler samples。每个 kernel 的原始 plan/MIR 保存到 work directory，
JSON 报告保留完整 entries，
Markdown 报告逐条列出成功/拒绝原因；expected-vectorizable kernel 的已验证 loop
`VECTORIZED` plan 少于 manifest 最低数时，即使 SLP、opcode 或其他指标存在也不能通过
正式门。

## Release 阈值

所有速度比使用 scalar median cycles / RVV median cycles：

| 门 | 阈值 |
| --- | ---: |
| unit-stride 去重组 geomean | 至少 1.50x |
| expected-vectorizable 去重真实 corpus geomean | 至少 1.15x |
| 标记为 hotspot 的自动向量化 kernel | RVV 不得稳定慢于 scalar 超过 5% |
| negative-control 去重组 geomean 回退 | 不超过 2% |
| RVV/scalar 编译时间去重组 geomean 回退 | 不超过 10% |

任一 expected-vectorizable RVV ELF 没有 RVV opcode、scalar ELF 出现 RVV opcode、
counter/size/compile evidence 缺失、sample 不稳定或 kernel 未运行，都会在阈值计算之外
额外产生 blocking finding。

## 在原生 RVV 主机运行

所有外部工具必须用绝对路径传入；前端不从 PATH 猜测编译器、linker、perf 或 objdump。
scalar-compiler 与 rvv-compiler 可以指向同一个 yoolang compiler，也可以指向两个待比较
构建。下面的路径只是命令形状，运行者必须替换为该机器上的真实绝对路径：

    python3 scripts/rvv_hardware_perf.py \
      --manifest /home/yoo/Documents/yoolang/test/performance/rvv_hardware_manifest.json \
      --cpu 3 \
      --frequency-policy require-fixed \
      --scalar-compiler /absolute/path/to/compiler \
      --rvv-compiler /absolute/path/to/compiler \
      --linker /absolute/path/to/riscv64-linux-gnu-gcc \
      --runtime-lib /absolute/path/to/libsysy.a \
      --perf-tool /absolute/path/to/perf \
      --objdump /absolute/path/to/riscv64-linux-gnu-objdump \
      --work-dir /absolute/path/to/rvv-perf-work \
      --output-json /absolute/path/to/rvv-perf-report.json \
      --output-markdown /absolute/path/to/rvv-perf-report.md

manifest 固定 scalar 参数为 rv64gc/lp64d、关闭 Loop/SLP vectorization；RVV 参数为
rv64gcv/lp64d、scalable RVV、开启 Loop/SLP vectorization。linker 的 march/mabi 与各自
variant 一致。

也可对已经部署到硬件的 ELF 做诊断：

    python3 scripts/rvv_hardware_perf.py \
      --cpu 3 \
      --frequency-policy require-fixed \
      --scalar-binary-dir /absolute/path/to/scalar-binaries \
      --rvv-binary-dir /absolute/path/to/rvv-binaries \
      --perf-tool /absolute/path/to/perf \
      --objdump /absolute/path/to/riscv64-linux-gnu-objdump

每个目录中的文件名由 manifest 的 binary_name 指定。预构建模式无法产生本次运行的
compiler-time samples，因此会明确报告 COMPILE_TIME_MISSING，并且不能成为 official
Release PASS。它的用途是硬件/计数器/ELF bring-up，不是绕过 10% 编译时间门。

## 输出和退出码

corpus manifest `schema_version=3` 要求 RVV `vector_plan_args`、`post_ra_mir_args`，以及
每个 expected-vectorizable kernel 的 `minimum_verified_vectorized_loops`。旧 v1/v2
manifest 会被明确拒绝，避免把缺失的循环决策、loop minimum 或 spill 字节证据误当成
完整结果。

JSON 使用 schema yoolang.rvv-hardware-performance.v3，包含 environment、toolchain
路径与哈希、configuration、thresholds、aggregates、failures 和逐 kernel 原始/汇总指标。
Markdown 是同一 payload 的人工审阅视图。

- 0：全部环境、证据和五个 Release 门通过，official=true；
- 1：已经运行门禁，但 correctness、证据或阈值失败；
- 2：环境/配置阻塞、record-only、probe-only 或其他非正式状态。

BLOCKED、NOT_RUN、NOT_OFFICIAL 和 ERROR 报告都会写出 JSON/Markdown，并在标题处明确
official=false。它们不能被解释为通过。

## 本地程序化验证

以下测试仅解析文本和合成的 policy fixtures，不启动 perf、编译器、benchmark 或 QEMU：

    python3 -m py_compile \
      scripts/perf_common.py \
      scripts/compare_perf.py \
      scripts/rvv_hardware_perf.py \
      scripts/rvv_perf_infra_tests.py
    python3 scripts/rvv_perf_infra_tests.py

它覆盖 manifest fail-closed 校验、perf/objdump parser、非 riscv64/QEMU/无 V/vector
state off/非固定频率拒绝，以及 1.50x、1.15x、5%、2%、10% 五个门。

三项最后解锁的真实语料另由 toolchain/QEMU gate 覆盖（QEMU 结果仅作语义证据）：

    python3 scripts/rvv_remaining_perf_corpus_infra_tests.py

该 gate 要求 `many_mat_cal-3`、`matmul2`、`fft2` 分别至少有 1/1/6 个 verified loop
plan，检查 OIR 与 GNU objdump 的真实 RVV，并比较 rv64gc scalar 和
VLEN=128/256/512/1024 输出；`fft2` 还运行实际 overlap/exact/zero/guard-page driver。

## 当前外部阻塞

本性能基础设施切片唯一尚未执行的外部验证是：在非模拟的 riscv64 RVV 1.0 硬件上，
以可固定频率、可绑核且允许读取 cycles/instructions 的账户运行上述完整 compile 模式。
在获得该硬件环境并保存真实 JSON/Markdown 之前，正式硬件门状态是“未运行”，不能声称
RVV 性能门或整体 GA 已通过。
