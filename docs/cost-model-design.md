# yoolang Cost Model 设计说明

本文定义 yoolang 中长期使用的 cost model。它不是某个优化 pass 的局部阈值表，而是连接手写优化、SMT 证明、partial evaluation 和 e-graph 重写的统一决策层。

## 1. 设计目标

cost model 的职责是回答：

- 一个已经证明语义正确的候选变换是否值得启用。
- 多个等价候选实现中应该选择哪一个。
- 某个候选为什么被接受或拒绝。
- 静态估算与实际 MIR metrics、最终汇编、性能报告之间是否一致。

cost model 不负责回答：

- 一个变换是否合法。
- 一个针对特定文件名、函数名、变量名、输入规模或测试数据的变换能否通过比赛。
- 一个未证明语义保持的重写能否因为跑得快而被接受。

因此 yoolang 的优化决策必须分成两层：

```text
semantic legality / equivalence proof
  -> cost model profitability / risk decision
  -> transform commit
  -> verifier + metrics + perf calibration
```

只要第一层失败，后面的收益估算没有意义。SMT、PE、e-graph 可以帮助证明或产生候选，但不能绕过这个边界。

## 2. yoolang 约束与机会

yoolang 当前有四个关键层次：

```text
AST
  -> YIR       结构化控制流，保留 for/while/if/region 意图
  -> OIR       SSA、BasicBlock、Phi、平台无关优化
  -> MIR       RV64GC/LP64D 目标相关机器 IR，PreRA/PostRA/Final stages
  -> ASM       RISC-V 汇编
```

这些层次决定了 cost model 的信息来源：

- YIR 适合识别结构化循环、嵌套 region、数组访问形态、polyhedral/PE 候选。
- OIR 适合做 SSA 级候选评估，例如 inline、SCCP、GVN、LICM、loop transform、if-conversion、memory idiom。
- MIR 适合评估真实后端成本，例如指令数、load/store、branch、call、virtual register、spill、stack slot、callee-saved register。
- ASM/perf 报告只用于校准，不应成为针对个案写规则的来源。

yoolang 的 `-O1` 是比赛优化流水线。cost model 的默认策略应该服务 `-O1`，而不是新增 `-O2/-O3`。

## 3. 总体架构

cost model 由五个层次组成：

```text
CandidateProvider
  -> ProofGate
  -> StaticEstimator
  -> DecisionEngine
  -> CalibrationRecorder
```

### 3.1 CandidateProvider

产生候选变换，但不直接修改 IR。来源包括：

- 手写 OIR/MIR pass。
- SMT 驱动的代数或条件重写。
- partial evaluation 产生的 residual program。
- e-graph equality saturation 提取出来的候选表达式或子图。
- polyhedral/YIR loop pipeline 产生的 loop schedule、interchange、tiling、split 候选。

### 3.2 ProofGate

判断候选是否语义保持。可接受的证明来源包括：

- 结构性证明：例如支配关系、无副作用、无 speculative load、SSA use-def 完整。
- 数据流证明：例如 value range、alias/memory clobber、loop dependence。
- SMT 证明：例如 bitvector 等价、guard implication、overflow 条件。
- e-graph 等价证明：来自已注册的等价 rewrite rule。
- PE 正确性证明：specialization 输入只来自编译期常量或已证明不变值。

ProofGate 失败时，候选必须被拒绝，并记录 `RejectReason::Illegal` 或更细粒度原因。

### 3.3 StaticEstimator

估算候选应用前后的成本。估算必须结构化输出，而不是一个裸分数。

### 3.4 DecisionEngine

根据策略、成本、风险、置信度和预算决定：

- accept
- reject
- defer
- ask-for-later-stage
- choose one among alternatives

### 3.5 CalibrationRecorder

把静态估算与真实后端指标关联起来：

- `--emit-mir-metrics`
- `--emit-mir-stage=<stage>`
- `scripts/compare_perf.py`
- `build/perf-ci/perf-report.json`
- 最终 assembly 指令形态

校准只能调整通用权重和阈值，不能引入测试身份判断。

## 4. 核心数据定义

建议公共接口放在：

```text
include/pass/CostModel.h
src/pass/CostModel.cpp
include/pass/oir/OIRCostModel.h
src/pass/oir/OIRCostModel.cpp
include/pass/mir/MIRCostModel.h
src/pass/mir/MIRCostModel.cpp
```

### 4.1 IR stage

```cpp
enum class CostIRStage {
    YIR,
    OIR,
    PreRAMIR,
    PostRAMIR,
    FinalMIR,
    ASM,
};
```

### 4.2 候选类型

```cpp
enum class TransformKind {
    Inline,
    ConstantArgumentSpecialization,
    PartialEvaluation,
    EGraphRewrite,
    AlgebraicSimplify,
    StrengthReduction,
    LoopInvariantCodeMotion,
    LoopUnswitch,
    LoopRotate,
    LoopInterchange,
    LoopTiling,
    LoopUnroll,
    LoopIdiom,
    MemoryForwarding,
    MemZeroLowering,
    IfConversion,
    BranchCombine,
    CompareBranchCombine,
    LocalCSE,
    GlobalCSE,
    AddressModeFold,
    InstructionScheduling,
    RegisterAllocationHint,
    Peephole,
};
```

### 4.3 目标信息

```cpp
struct TargetCostProfile {
    std::string arch = "rv64gc";
    std::string abi = "lp64d";
    int xlen_bits = 64;
    int flen_bits = 64;
    int stack_align = 16;

    int alu_i32 = 1;
    int alu_i64 = 1;
    int mul_i32 = 3;
    int div_i32 = 24;
    int rem_i32 = 24;
    int fp_add = 4;
    int fp_mul = 5;
    int fp_div = 30;
    int load = 4;
    int store = 4;
    int branch = 2;
    int unpredictable_branch = 8;
    int call = 12;
    int spill_load = 6;
    int spill_store = 6;
    int code_byte = 1;
};
```

这些数值是初始权重，不是硬件精确延迟。它们用于排序和拒绝明显不划算的候选，后续通过 perf 和 MIR metrics 校准。

### 4.4 执行频率估计

yoolang 默认没有 PGO，因此热度只能来自结构：

```cpp
enum class FrequencySource {
    Unknown,
    StructuredYIRLoop,
    OIRLoopAnalysis,
    ConstantTripCount,
    ValueRangeTripCount,
    HeuristicLoopDepth,
    UserDisabled,
};

struct FrequencyEstimate {
    int64_t scale = 1;
    int loop_depth = 0;
    bool exact_trip_count = false;
    bool bounded_trip_count = false;
    FrequencySource source = FrequencySource::Unknown;
    double confidence = 0.5;
};
```

规则：

- YIR 中结构化 for/while 信息优先级最高。
- OIR loop analysis 和 value range 次之。
- 只有 loop depth 时，用保守倍数，不可把深层循环自动视为可无限膨胀。
- 未知 trip count 时，不允许依赖巨大动态收益来抵消明显代码膨胀。

### 4.5 成本向量

```cpp
struct CostVector {
    int64_t static_instrs = 0;
    int64_t dynamic_instrs = 0;
    int64_t code_bytes = 0;

    int64_t int_alu = 0;
    int64_t int_mul = 0;
    int64_t int_div_rem = 0;
    int64_t fp_alu = 0;
    int64_t fp_div = 0;

    int64_t loads = 0;
    int64_t stores = 0;
    int64_t memzero_bytes = 0;
    int64_t pointer_arith = 0;

    int64_t branches = 0;
    int64_t unpredictable_branches = 0;
    int64_t jumps = 0;
    int64_t calls = 0;

    int64_t phis = 0;
    int64_t moves = 0;
    int64_t virtual_regs = 0;
    int64_t live_values = 0;
    int64_t max_live_values = 0;
    int64_t estimated_spills = 0;
    int64_t stack_slots = 0;

    int64_t compile_time_units = 0;
    int64_t proof_time_units = 0;
    int64_t egraph_nodes = 0;
    int64_t smt_queries = 0;

    int64_t estimated_cycles = 0;
};
```

解释：

- `static_instrs` 是代码大小和编译后 shape 的近似。
- `dynamic_instrs` 是乘过频率后的执行成本近似。
- `estimated_cycles` 是 weighted sum，不能替代分项指标。
- `live_values/max_live_values/estimated_spills` 是 yoolang 后端必须重视的风险项，因为 PreRA 优化可能被 RA 后的 spill 抵消。
- `compile_time_units/proof_time_units/egraph_nodes/smt_queries` 用于限制 SMT/e-graph/PE 本身的编译期开销。

### 4.6 风险向量

```cpp
struct RiskVector {
    int64_t code_growth = 0;
    int64_t live_range_growth = 0;
    int64_t register_pressure_growth = 0;
    int64_t memory_pressure_growth = 0;
    int64_t branch_predictability_loss = 0;
    int64_t locality_loss = 0;
    int64_t compile_time_growth = 0;
    int64_t proof_timeout_risk = 0;
    int64_t cleanup_dependency = 0;
};
```

`cleanup_dependency` 表示候选收益依赖后续 SCCP/DCE/GVN/ADCE 清理。inline、PE、e-graph 提取都可能产生这类风险。

### 4.7 证明对象

```cpp
enum class ProofKind {
    None,
    Structural,
    DataFlow,
    Dependence,
    SMT,
    EGraphEquality,
    PartialEvaluation,
    Composite,
};

enum class ProofStatus {
    Proven,
    Refuted,
    Timeout,
    Unknown,
};

struct EquivalenceProof {
    ProofKind kind = ProofKind::None;
    ProofStatus status = ProofStatus::Unknown;
    std::string summary;
    std::string rule_id;
    std::string solver_id;
    int64_t time_us = 0;
    int64_t obligations = 0;
};
```

浮点变换默认不能用普通实数代数证明。涉及 `float` 的重写必须满足 yoolang 明确选择的浮点语义；没有精确证明时只能保守拒绝。

### 4.8 候选定义

```cpp
struct TransformCandidate {
    TransformKind kind;
    CostIRStage stage;
    std::string pass_name;
    std::string candidate_id;
    std::string scope;

    FrequencyEstimate frequency;
    EquivalenceProof proof;

    CostVector before;
    CostVector after;
    CostVector setup;
    RiskVector risk;

    std::vector<std::string> required_cleanup_passes;
    std::vector<std::string> reason_hints;
};
```

`scope` 应描述 IR 范围，例如 function、loop、basic block、instruction slice、e-class，而不是测试文件或 benchmark 名称。

### 4.9 决策结果

```cpp
enum class DecisionAction {
    Accept,
    Reject,
    Defer,
    PreferAlternative,
    RequireLaterStageCheck,
};

enum class RejectReason {
    None,
    Illegal,
    ProofTimeout,
    ProofUnknown,
    NegativeGain,
    LowConfidence,
    CodeGrowthTooHigh,
    RegisterPressureTooHigh,
    MemoryPressureTooHigh,
    CompileTimeTooHigh,
    CleanupTooSpeculative,
    TargetUnsupported,
};

struct TransformDecision {
    DecisionAction action = DecisionAction::Reject;
    RejectReason reject_reason = RejectReason::None;

    bool legal = false;
    bool profitable = false;
    double confidence = 0.0;

    int64_t estimated_gain = 0;
    int64_t setup_cost = 0;
    int64_t risk_penalty = 0;
    int64_t final_score = 0;

    CostVector before;
    CostVector after;
    CostVector delta;
    RiskVector risk;
    EquivalenceProof proof;

    std::string transform;
    std::string reason;
};
```

决策公式建议先保持可解释：

```text
estimated_gain = weighted(before) - weighted(after)
setup_cost     = weighted(setup)
risk_penalty   = weighted(risk)
final_score    = estimated_gain - setup_cost - risk_penalty
```

接受条件：

```text
proof.status == Proven
final_score >= policy.min_final_score
code_growth <= policy.max_code_growth
register_pressure_growth <= policy.max_register_pressure_growth
compile_time_growth <= policy.max_compile_time_growth
confidence >= policy.min_confidence
```

## 5. OIR 成本语义

OIR 是平台无关 SSA 层，但 yoolang 目标固定为 RV64GC/LP64D，因此 OIR 估算可以带目标意识。

### 5.1 OIR 指令成本

建议初始映射：

| OIR 指令 | 成本含义 |
| --- | --- |
| `Add/Sub/And/Xor/ZExt` | 低成本 ALU |
| `Mul` | 中成本 ALU |
| `SDiv/SRem` | 高成本 ALU |
| `FAdd/FSub/FMul` | 中成本 FP |
| `FDiv` | 高成本 FP |
| `Load/Store` | 内存成本，受 alias/locality 影响 |
| `GetElementPtr` | 地址形成成本，可能降低到 `AddI/SllI/Add` |
| `Call` | 高固定成本，加 ABI clobber 和 inline 机会 |
| `Br` | 控制流成本，条件分支有 predictability 风险 |
| `Phi` | SSA 成本，后端可能表现为 move/copy 或 edge block |
| `MemZero` | 大块清零可 lower 为 memset，小块可能 inline |

### 5.2 OIR 结构成本

函数级：

- block 数量。
- terminator 数量。
- phi 数量。
- call site 数量。
- alloca/load/store 剩余数量。
- 参数数量和 dead argument 机会。

循环级：

- loop depth。
- trip count 是否已知。
- induction variable 数量。
- invariant 指令数量。
- memory access stride。
- exiting block 数量。
- latch/control overhead。

内存级：

- 访问宽度。
- 是否连续。
- 是否重复 load。
- 是否可证明无 clobber。
- 是否可化为 `MemZero` 或未来 `MemSet/MemCopy`。

### 5.3 OIR 风险

OIR 估算必须提前考虑后端风险：

- inline/PE 可能增加 live values 和 block size。
- LICM 可能把 loop 内临时值提升到 loop 外，延长 live range。
- if-conversion 可能减少 branch 但增加临时值。
- loop unswitch 可能大幅增加 code size。
- e-graph 提取可能选择局部指令少但寄存器压力高的表达式。

## 6. MIR 成本语义

MIR 是 yoolang cost model 的校准锚点。

已有 MIR stages：

```text
lowered
post-combine
pre-ra
post-ra
final
```

MIR cost collector 应扩展现有 metrics：

- functions
- basic_blocks
- instructions
- moves
- jumps
- branches
- loads
- stores
- load_slots
- store_slots
- spills
- stack_slots
- calls
- virtual_regs
- max_live_in
- max_live_out
- callee_saved_regs
- outgoing_arg_bytes
- frame_size

### 6.1 PreRA 判断

PreRA 决策重点：

- virtual register 数量。
- 单 basic block 内 def/use 密度。
- loop 内 max live values。
- 是否包含 call。
- 是否引入长 live range。
- 是否会减少 load/store/branch。

### 6.2 PostRA 判断

PostRA 决策重点：

- spill slot 数。
- `LoadSlot/StoreSlot` 数。
- stack frame 大小。
- callee-saved register 保存恢复成本。
- final branch/jump 数。
- final load/store 数。

如果一个 OIR/MIR 变换在 PreRA 看起来减少了指令，但 PostRA 增加 spill，校准时应把该类变换的 register pressure penalty 提高。

## 7. SMT 对接

SMT 的定位是 ProofGate，不是收益模型。

### 7.1 SMT 适用范围

适合：

- i32 bitvector 代数等价。
- 比较/分支 guard implication。
- range-gated rewrite 的前置条件。
- 无符号/有符号边界条件。
- 简单 memory index disjointness 辅助证明。

谨慎或默认拒绝：

- 未建模的内存副作用。
- 函数调用。
- 浮点重写。
- 依赖 C/SysY 未定义行为的等价。
- 需要特定输入值才能成立的变换。

### 7.2 SMT 查询对象

```cpp
struct SMTObligation {
    std::string id;
    CostIRStage stage;
    std::string formula;
    int64_t timeout_us;
    std::vector<std::string> assumptions;
    std::vector<std::string> guarantees;
};
```

每个 SMT 查询必须有 timeout。结果为 timeout/unknown 时候选不能视为 proven。

### 7.3 SMT 与 cost model 的交互

```text
CandidateProvider creates rewrite
  -> SMT proves equivalence or guard
  -> CostModel scores proven candidate
  -> DecisionEngine accepts/rejects
```

SMT 成本计入 `proof_time_units` 和 `smt_queries`。如果某类候选证明太贵，即使运行时收益不错，也可能因编译期开销被拒绝或降级为离线规则。

## 8. Partial Evaluation 对接

PE 的定位是 CandidateProvider + ProofGate。它生成 specialized/residual 版本，并证明 specialization 输入是编译期已知或循环不变。

### 8.1 PE 候选

典型候选：

- 常量参数函数 specialization。
- `if` 条件已知后的 dead branch residualization。
- 固定数组维度或 stride 的地址计算残留化。
- loop bound 已知时的局部展开或边界收紧。
- 库风格小函数的跨调用常量传播。

### 8.2 PE 成本字段

PE 需要额外记录：

```cpp
struct PartialEvalCost {
    int64_t cloned_functions = 0;
    int64_t cloned_blocks = 0;
    int64_t residual_instrs = 0;
    int64_t eliminated_instrs = 0;
    int64_t eliminated_branches = 0;
    int64_t eliminated_calls = 0;
    int64_t new_constants = 0;
    int64_t required_cleanup_rounds = 0;
};
```

PE 的收益不能只看残留程序局部变小，还必须计算：

- clone 导致的全局 code growth。
- 后续 SCCP/GVN/DCE 是否必须成功。
- specialization 数量是否爆炸。
- inline 和 PE 是否相互放大。

### 8.3 PE 接受规则

推荐默认规则：

- 单个函数 clone 数有限。
- module 总 code growth 有上限。
- 只有当 call 消除、branch 消除、load 消除或 loop 简化收益明确时接受。
- 对递归、间接调用、不稳定 alias 场景保守。
- 如果收益依赖后续清理，必须把 `cleanup_dependency` 计入风险。

## 9. E-graph 对接

e-graph 的定位是 CandidateProvider + ProofGate + AlternativeProvider。

e-graph 可以产生大量等价表达式，cost model 负责 extraction，不允许每个 rewrite 自己决定最终形态。

### 9.1 e-graph 适用范围

优先从 OIR expression slice 做起：

- 整数代数。
- compare/branch 归约。
- address arithmetic。
- strength reduction。
- bit idiom。
- GEP index canonicalization。
- 小范围 load-free expression。

谨慎范围：

- memory load/store reorder。
- call 周边表达式。
- 浮点表达式。
- 跨 basic block 的大子图。

### 9.2 e-graph 节点成本

e-graph extraction 需要使用多目标成本：

```cpp
struct EGraphExtractCost {
    CostVector cost;
    RiskVector risk;
    int64_t eclass_count = 0;
    int64_t enode_count = 0;
    int64_t saturation_rounds = 0;
    int64_t extraction_time_us = 0;
};
```

默认排序不是最少节点数，而是：

```text
target_weighted_runtime_cost
  + code_size_penalty
  + register_pressure_penalty
  + proof_or_extraction_cost_penalty
```

### 9.3 e-graph budget

必须有硬预算：

- 最大 e-class 数。
- 最大 e-node 数。
- 最大 saturation round。
- 最大 compile time。
- 最大跨 block 范围。

超预算时应停止 saturation，并在当前 proven graph 中做保守 extraction；如果 graph 不完整或 proof 不可信，则拒绝候选。

### 9.4 e-graph 与 SMT

两者关系：

- e-graph rule 本身可以人工证明并注册。
- 对带 guard 的 rewrite，guard 可交给 SMT 证明。
- e-graph 提供候选集合，SMT 可证明 guard，cost model 做 extraction。

## 10. 策略定义

建议有三套策略，但 `-O1` 默认使用 `Balanced`。

```cpp
enum class CostModelPolicyKind {
    Conservative,
    Balanced,
    Aggressive,
};

struct CostModelPolicy {
    CostModelPolicyKind kind = CostModelPolicyKind::Balanced;

    int64_t min_final_score = 1;
    double min_confidence = 0.55;

    int64_t max_function_code_growth = 200;
    int64_t max_module_code_growth_percent = 15;
    int64_t max_register_pressure_growth = 8;
    int64_t max_live_range_growth = 16;
    int64_t max_compile_time_growth = 10000;

    int64_t max_inline_callee_cost = 80;
    int64_t max_specializations_per_function = 4;
    int64_t max_egraph_nodes = 5000;
    int64_t max_smt_time_us = 5000;

    bool allow_loop_unswitch = true;
    bool allow_partial_eval = true;
    bool allow_egraph = true;
    bool allow_smt = true;
};
```

策略只影响收益和预算，不影响合法性。

## 11. 接入现有 pass 的顺序

### 11.1 第一批

这些收益大、风险明显，最适合优先接入：

- `OIRInlinePass`
- constant argument specialization
- OIR loop unswitch / rotate / guard tightening
- OIR if-conversion
- MIR small if conversion
- MIR LICM/CSE
- MIR list scheduler

### 11.2 第二批

- e-graph expression rewrite。
- SMT guarded algebraic rewrite。
- PE residualization。
- loop unroll/interchange/tiling。
- memory idiom beyond `MemZero`。
- RA hint 和 rematerialization policy。

### 11.3 不应优先接入

- 单纯 canonicalization。
- 必然减少代码且无副作用的 DCE/ADCE。
- verifier/diagnostics pass。
- 明确无 tradeoff 的 CFG cleanup。

这些 pass 可以记录 metrics，但不需要 cost model gate。

## 12. 诊断输出

新增命令行建议：

```text
--emit-cost-model
--emit-cost-model=json
--cost-model-trace
--cost-model-filter=<pass-or-transform>
--cost-model-policy=conservative|balanced|aggressive
```

JSON 结构：

```json
{
  "target": "rv64gc/lp64d",
  "policy": "balanced",
  "functions": [
    {
      "name": "main",
      "summary": {
        "oir_before": {},
        "mir_final": {}
      },
      "decisions": [
        {
          "transform": "Inline",
          "action": "Accept",
          "legal": true,
          "profitable": true,
          "estimated_gain": 42,
          "risk_penalty": 8,
          "reason": "call removed and callee exposes constant branch"
        }
      ]
    }
  ]
}
```

human-readable trace 应强调 reason code，例如：

```text
[cost-model] OIRInlinePass accept callsite=%42 gain=58 risk=12 score=46 reason=call_removed+constant_branch_exposed
[cost-model] LoopUnswitch reject loop=%L3 reason=CodeGrowthTooHigh growth=312 limit=200
[cost-model] EGraphRewrite reject slice=%17 reason=RegisterPressureTooHigh live_growth=11 limit=8
```

## 13. 与 compare_perf.py 的闭环

`scripts/compare_perf.py` 应把 cost model summary 写进 `perf-report.json`：

- accepted transforms count by kind。
- rejected transforms count by reason。
- estimated gain total。
- MIR stage metrics delta。
- final codegen metrics。
- optional QEMU instruction count delta。

回归分析流程：

```text
perf regression
  -> check MIR stage delta
  -> map delta to accepted decisions
  -> inspect accepted high-risk decisions
  -> adjust generic weights or pass-specific estimator
```

不要因为某个 case 慢了就给该 case 特判。正确修正是提高某类风险的 penalty 或改进 proof/estimator。

## 14. Contest 合规规则

cost model 相关代码必须遵守：

- 不读取源文件名。
- 不匹配函数名、变量名、字符串字面量作为优化触发条件。
- 不根据输入数据、输入规模、运行时参数选择特殊路径。
- 不硬编码测试答案。
- 不使用未证明的 UB 假设。
- 不通过 cost model 接受语义未知或 SMT timeout 的变换。

允许使用：

- IR 结构。
- 类型。
- SSA use-def。
- 支配关系。
- loop 结构。
- value range。
- alias/dependence 证明。
- target profile。
- 通用 pass metrics。

## 15. 实现里程碑

### P1: 基础定义和只读诊断

- 添加 `CostVector`、`RiskVector`、`TransformCandidate`、`TransformDecision`。
- 添加 RV64GC/LP64D `TargetCostProfile`。
- 添加 OIR/MIR static collector。
- 添加 `--emit-cost-model=json`。
- 不改变生成代码。

### P2: OIR 手写 pass 接入

- 接入 inline。
- 接入 constant argument specialization。
- 接入 if-conversion 和 loop transform。
- 输出 accept/reject trace。

### P3: MIR 手写 pass 接入

- 接入 MIR LICM/CSE。
- 接入 small if conversion。
- 接入 scheduler。
- 用 `post-ra` 和 `final` metrics 校准 register pressure penalty。

### P4: SMT 接入

- 定义 `SMTObligation`。
- 支持 i32 bitvector guarded rewrite。
- 支持 timeout/cache。
- 把 proof result 写入 decision trace。

### P5: PE 接入

- 把 constant specialization 扩展为通用 PE candidate。
- 增加 clone/code growth/specialization budget。
- 与 SCCP/GVN/DCE cleanup window 联动。

### P6: E-graph 接入

- OIR expression slice equality saturation。
- 注册通用 rewrite rule。
- 用 cost model extraction 替代最小节点 extraction。
- 加 egraph budget 和 trace。

### P7: perf 校准

- 扩展 `compare_perf.py`。
- 对照 `perf-report.json`、MIR stage metrics、assembly。
- 形成 `docs/cost-model-calibration.md`。

## 16. 成功标准

一个完整可用的 yoolang cost model 应满足：

- 每个有 tradeoff 的优化都能解释接受或拒绝原因。
- SMT/PE/e-graph 候选与手写 pass 使用同一套决策输出。
- 普通 `-S -O1` 不输出诊断、不改变语义。
- `--emit-cost-model` 能复现优化决策。
- register pressure 和 code growth 不再散落在各 pass 的 magic number 中。
- 性能回归能从 perf report 追到具体 decision kind 和 risk 项。
- 合规性边界清晰：cost model 只管 profitability，不替代 legality。
