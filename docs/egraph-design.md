# yoolang OIR E-graph 设计

## 1. 结论

yoolang 的第一版 e-graph 应实现为一个有硬预算、可证明、cost-model 驱动的
OIR 局部 equality-saturation pass。它处理单个基本块内的纯整数 SSA expression
slice，不处理 CFG、内存状态、调用和浮点。

推荐的数据流是：

```text
OIR expression slice
  -> typed ENode/EClass import
  -> registered rules + guarded proof
  -> bounded equality saturation
  -> target-aware extraction
  -> exact OIR candidate costing
  -> shared cost-model decision
  -> transactional materialization
  -> GVN/DCE cleanup + OIR verifier
```

设计的核心约束是：

- e-graph 只建立等价关系，不直接决定某种形态一定更快。
- rule 的合法性先于 cost model；`Refuted`、`Timeout`、`Unknown` 都不能建立 equality。
- cost model 负责在等价候选中提取和批准最终 OIR，不使用最少节点数作为唯一目标。
- 达到 saturation 预算只表示停止搜索，不自动表示已有 equality 不可信。只有缺少完整
  proof path 的候选才必须拒绝。
- 普通 `-O1` 的 IR 结果不受 `--cost-model-filter` 影响；filter 只能筛选诊断输出。

## 2. 为什么选 OIR

当前 OIR 是类型化 SSA/CFG IR，已有 use-def、支配、value range、GVN、DCE、SMT
和共享 cost-model 基础设施，适合表达局部等价重写。YIR 应继续负责结构化循环、数组
view 和 polyhedral 变换，MIR 应继续负责 target-specific combine、调度和寄存器压力问题。

第一版不把整个函数或循环放入 e-graph，原因是：

- OIR 的 `Phi` 和控制流需要 path/region 语义，不是普通表达式 DAG。
- `Load`、`Store`、`Call`、`MemZero` 需要 memory token、alias 和 clobber 证明。
- 浮点代数需要明确的 NaN、舍入和 fast-math 语义；当前不能套用整数等式。
- 局部纯表达式已覆盖代数化简、strength reduction、compare 归约和部分 GEP index
  canonicalization，并能用较小改动接入现有流水线。

## 3. 当前基础与需要替换的 hook

仓库已有：

- `TransformKind::EGraphRewrite`、`ProofKind::EGraphEquality`。
- `CandidateProviderRequest/Result`、`AlternativeCost`、`EGraphExtractCost`。
- `CostModelPolicy::max_egraph_nodes` 和三档 policy。
- OIR cost-model adapter、QF_BV SMT solver、value range、GVN、DCE 和 OIR verifier。
- `OIRLocalSimplify` 中的 `x * 2 -> x + x` 注册式示范路径。

示范路径不是 equality saturation。正式实现应替换它，而不是在该 helper 上继续堆规则。
当前示范路径还有几个正式实现必须消除的行为：

- 用 `complexity * 1000` 模拟 e-node 数，没有真实 EClass/ENode。
- 只有单条直接 rewrite，没有 rebuild、saturation 或 alternative extraction。
- 对成功候选使用 `BypassProfitability`，没有让 target cost 比较 `mul` 和 `add`。
- diagnostic filter 不匹配时直接返回 `false`，会改变优化结果。
- proof 只记录一个 rule id，不能解释多步等价路径。

## 4. V1 范围

### 4.1 接受的 OIR 节点

V1 仅导入无副作用、无控制流、同一基本块内的节点：

- i1/i32 常量、Argument 和 slice 外部 SSA 值作为 leaf。
- `Add`、`Sub`、`Mul`、`And`、`Xor`。
- `ICmp`。
- `ZExt`，仅支持当前 OIR 已有的 i1 到 i32 形式。

V1 暂不导入：

- `SDiv`、`SRem`：需要除零和 `INT_MIN / -1` 等语义边界证明。
- `Phi`、`Br`、`Ret`：属于 CFG/region 语义。
- `Load`、`Store`、`Call`、`MemZero`、`Alloca`：属于 memory/effect 语义。
- `FAdd`、`FSub`、`FMul`、`FDiv`、`FCmp`、浮点 cast。
- `GetElementPtr`：留到 V2，在 layout、pointer provenance 和 index width 契约明确后接入。

### 4.2 slice 边界

以 basic block 为扫描单位，把相连的受支持纯指令组成 expression component：

- component leaf 是常量、参数、全局地址，或 component 外定义且支配使用点的 SSA 值。
- component root 是被不支持指令使用、被 block 外使用、被 terminator 使用，或需要保留
  debug/name 可见性的值。
- 跨 basic block 的定义只作为 leaf，不递归导入。
- component 超过 root、depth、EClass 或 ENode 预算时停止扩展；不截断一个节点的类型
  或 operand 列表。

这样可以避免把整个函数变成一个图，同时保留一个 block 内公共子表达式的共享信息。

## 5. 组件划分

建议新增：

```text
include/oir/EGraph.h
src/oir/EGraph.cpp
include/pass/oir/OIREGraphRewritePass.h
src/pass/oir/OIREGraphRewritePass.cpp
src/pass/oir/OIREGraphRules.cpp
src/pass/oir/OIREGraphCost.cpp
test/ir/oir_egraph.sy
test/ir/cost_model_egraph.sy
```

职责如下：

| 组件 | 职责 |
| --- | --- |
| `oir::egraph::EGraph` | hash-cons、union-find、EClass 合并、rebuild 和 analysis |
| `OIRExpressionSliceBuilder` | 从 OIR 构造 typed expression component 和 roots |
| `RewriteRegistry` | 注册规则、phase、proof 契约和 guard |
| `Saturator` | 按 phase 确定性执行规则并执行硬预算 |
| `Extractor` | 用 target/cost-model 节点成本生成 top-K plan |
| `OIRMaterializer` | 检查支配与类型，事务式生成 OIR 并替换 roots |
| `OIREGraphRewritePass` | 串联 provider、proof、cost decision、统计和 verifier |

E-graph 核心不依赖 PassManager 和具体 cost-model policy；OIR adapter 负责把 OIR、规则、
成本和 pass 生命周期连接起来。这样核心数据结构可以单元测试，后续也能被 MIR 的小型
combine 使用，但 V1 不承诺跨 IR 共用 rule。

## 6. 核心数据结构

### 6.1 类型与节点

ENode 的 identity 必须包含结果类型和所有语义 payload：

```cpp
using EClassId = std::uint32_t;
using ENodeId = std::uint32_t;
using RuleId = std::uint32_t;

enum class EOp {
    Leaf,
    ConstantInt,
    Add,
    Sub,
    Mul,
    And,
    Xor,
    ICmp,
    ZExt,
};

struct EType {
    enum class Kind { I1, I32 } kind;
};

struct ENode {
    EOp op;
    EType type;
    std::vector<EClassId> children;
    std::int64_t int_value = 0;       // ConstantInt
    oir::CmpPred cmp_pred{};          // ICmp
    const oir::Value *leaf = nullptr; // Leaf identity
};
```

`ENodeKey` 用 canonical EClass id 做 hash-cons key。`Add/Mul/And/Xor` 的两个 child
在建 key 时按 id 排序，直接消除交换律产生的镜像节点；结合律不在 key 中隐式处理，
避免 materialization 与 proof trace 失真。

### 6.2 EClass 与 union-find

```cpp
struct EClassAnalysis {
    EType type;
    std::optional<std::uint64_t> constant_bits;
    std::optional<std::uint64_t> known_zero_bits;
    std::optional<std::uint64_t> known_one_bits;
    std::uint32_t min_depth = 0;
};

struct EClass {
    EClassId parent;
    std::uint16_t rank;
    std::vector<ENodeId> nodes;
    EClassAnalysis analysis;
};
```

合并前必须检查 `EType` 相等。`merge(a, b, justification)` 同时记录 proof edge；不能
提供合法 justification 的调用者不能执行 merge。除了用于快速查询的 union-find parent，
还要保留不随路径压缩丢失信息的 explanation forest；否则最终 canonical id 相同并不能
还原两个表达式为何等价。

`rebuild()` 执行：

1. 压缩 union-find parent。
2. 把每个 ENode child 改为 canonical id。
3. 重新 hash-cons congruent ENode。
4. 合并 congruent class。
5. 重新计算 analysis，直到稳定或达到 rebuild budget。

### 6.3 proof edge

```cpp
struct ProofEdge {
    EClassId lhs;
    EClassId rhs;
    std::string rule_id;
    pass::cost_model::ProofKind kind;
    pass::cost_model::ProofStatus status;
    std::string summary;
    std::int64_t time_us = 0;
    std::int64_t obligations = 0;
};
```

只有 `Proven` edge 可以进入 union-find。Extractor 通过 explanation forest 为选中的 plan
生成从原始 root 到目标 ENode 的 proof path，并去掉重复 congruence/rule edge，再汇总为
一个 `EquivalenceProof`。多种证明来源时使用 `ProofKind::Composite`；rule id 列表应进入
e-graph 详细 trace，而不是拼成不稳定的 candidate id。

## 7. Rewrite rule 契约

```cpp
enum class RulePhase {
    Canonicalize,
    Simplify,
    Explore,
};

struct RewriteRule {
    std::string id;             // 稳定，例如 egraph.bv32.add_zero
    RulePhase phase;
    Pattern lhs;
    Pattern rhs;
    bool may_grow;
    ProofKind proof_kind;
    GuardFn guard;
};
```

rule 的注册要求：

- 明确支持的 type/bit width，不能用无类型字符串 pattern。
- 明确整数是固定宽度 bit-vector 语义；不能依赖 C/C++ host overflow。
- unconditional rule 需要人工审查的语义说明和 focused test。
- conditional rule 的 guard 必须返回完整 `EquivalenceProof`。
- value-range/data-flow guard 必须绑定当前 OIR analysis 的事实版本。
- SMT guard 必须构造 typed counterexample obligation；只有 UNSAT/`Proven` 才应用。
- `Unknown`、`Timeout`、`Refuted` 只跳过该 match，不能污染整个 EClass。
- rule 不能读取 testcase、文件名、函数名、变量名或 benchmark 身份。

### 7.1 初始规则集

第一批规则以低增长和现有 OIR 能表达的结果为主：

```text
x + 0       == x
x - 0       == x
x - x       == 0
x * 0       == 0
x * 1       == x
x & 0       == 0
x & allones == x
x & x       == x
x ^ 0       == x
x ^ x       == 0
(x - y) + y == x
(x + y) - y == x
x * 2       == x + x
icmp eq x x == true
icmp ne x x == false
icmp eq x y == icmp eq y x
icmp ne x y == icmp ne y x
```

常量折叠应由 EClass analysis 生成常量 ENode，不为每个常量组合注册 rule。

`x * 2 == x + x` 是 equality，不代表始终改成 `add`。RV64GC 的当前 target profile
会让 extractor 比较 `mul_i32`、`alu_i32`、代码大小和寄存器风险，再由 shared
decision engine 批准或拒绝。

第二批再加入受控 reassociation、常量聚合和 distributivity。完整双向分配律很容易造成
指数增长，只能放在 `Explore` phase，并同时要求：

- component 很小。
- rule-local growth budget 未耗尽。
- 新节点能暴露常量折叠或消除 expensive op。

## 8. Saturation 算法与预算

采用 worklist equality saturation，而不是每轮全图全规则扫描：

```text
import graph
analyze/rebuild
for phase in Canonicalize, Simplify, Explore:
  seed worklist with changed e-classes
  while worklist not empty and phase budget remains:
    run indexed rules applicable to the e-class op/type
    admit only Proven matches
    add enodes / merge classes
    rebuild changed region
    enqueue affected classes
extract from current proven graph
```

所有遍历使用稳定的 rule 注册顺序、EClass id 和 ENode key 顺序，保证相同输入和 policy
产生确定结果。

预算分为 component、function 和 module 三层。V1 沿用当前 policy 的
`max_egraph_nodes` 总上限，并补充独立字段：

```cpp
struct EGraphBudget {
    std::int64_t max_enodes;
    std::int64_t max_eclasses;
    std::int64_t max_merges;
    std::int64_t max_rule_matches;
    std::int64_t max_rebuilds;
    std::int64_t max_rounds;
    std::int64_t max_time_us;
    std::int64_t max_component_instructions;
    std::int64_t max_roots;
    std::int64_t max_extracted_alternatives;
};
```

建议从以下保守值开始，之后用 compile-time/perf 报告校准：

| Policy | function ENode | component ENode | rounds | component instructions | top-K |
| --- | ---: | ---: | ---: | ---: | ---: |
| Conservative | 1500 | 128 | 3 | 24 | 1 |
| Balanced | 5000 | 384 | 5 | 64 | 3 |
| Aggressive | 12000 | 1024 | 8 | 128 | 5 |

wall-clock 只作为最后保险丝；可复现测试主要使用 nodes、matches、merges 和 rounds 这些
确定性预算。时间预算耗尽记录 `budget_exhausted=true`，但仍可从已经由 Proven edge
组成的图中提取。不得因为“没有完全饱和”而把已证明 equality 降级成 `Unknown`。

## 9. Analysis

V1 analysis 只做能安全 join 的事实：

- 唯一常量 bit pattern。
- i1/i32 类型。
- known-zero/known-one bits。
- expression depth 下界。

当同一 EClass 中两个常量 bit pattern 不一致时，这是 rule 或实现错误，pass 必须失败并
输出 verifier 风格错误，不能任选一个常量。

OIR value range 不直接存进 EClass 的等价 identity。它作为 guarded rule 的外部证明事实，
因为 path-sensitive range 可能只在特定 block/edge 上成立。proof edge 要记录 range
analysis 的 rule id 和简要事实。

## 10. Extraction 与 cost model 接入

### 10.1 两阶段 extraction

Extractor 先用 target-aware additive node cost 在每个 root EClass 中生成 top-K plan，
再对 plan 生成精确 OIR DAG 成本：

1. DP/relaxation 计算 ENode 的局部成本，循环 EClass 用迭代松弛并设 iteration cap。
2. 为每个 root 保留不同风险形态的 Pareto 小集合，而不是只留一个最小节点树。
3. 合并 multi-root plan，并通过 materialization interner 复用相同 ENode。
4. 统计最终唯一指令、use、live range、root replacement 和 cleanup dependency。
5. 把原 component 与每个 plan 转为 `TransformCandidate`/`AlternativeCost`。

### 10.2 节点成本

节点成本来自现有 `TargetCostProfile`：

- `Add/Sub/And/Xor` -> `int_alu`。
- `Mul` -> `int_mul`。
- `ICmp` -> `int_alu`，若最终连接 branch，再由外层 candidate 记录 branch cost。
- `ZExt` -> `int_alu` 或经 lowering 校准后的零成本。
- 新唯一 ENode 增加 `static_instrs` 和近似 `code_bytes`。
- leaf/constant 不直接增加 OIR 指令，但 materialization 需要的立即数形态可在 V2
  加 target-specific cost。

Extractor 的预排序目标是：

```text
weighted_runtime_cost
  + code_size_cost
  + register/live-range risk
  + cleanup dependency
```

最终是否应用仍调用 `pass::cost_model::decide()`。完整 e-graph 候选不得默认设置
`bypass_profitability`。

### 10.3 CandidateProvider 接口

`OIREGraphCandidateProvider` 应真正使用已有 provider shape：

```cpp
CandidateProviderResult provide(
    const CandidateProviderRequest &request,
    oir::Module &module,
    const OIRAnalyses &analyses);
```

每个 component 产生一个主 `TransformCandidate`，其余 top-K 方案进入
`candidate.alternatives`。建议补充或约定：

- `candidate.before` 是原 component 的精确唯一 OIR DAG 成本。
- `candidate.after` 是 materialization plan 的精确唯一 DAG 成本。
- `candidate.setup.egraph_nodes` 记录真实访问/创建节点数，不再除以 1000 或伪造。
- `candidate.setup.compile_time_units` 由 matches、merges、rebuilds、extraction 工作量换算。
- `candidate.risk.register_pressure_growth` 来自 before/after block-local liveness 近似。
- `candidate.risk.cleanup_dependency` 只在收益依赖后续 DCE/GVN 时增加。
- `candidate.proof` 是从原 root 到选中 root 的完整 proof path 汇总。

当前 `AlternativeCost` 没有 materialization plan handle。实现时 plan 保存在 pass-local arena，
candidate 使用稳定的 `alternative_id` 关联；不要把裸 OIR 指针放进共享 cost-model 报告。

当前 `pass::cost_model::decide()` 还没有读取 `candidate.alternatives`。接入时新增一个很薄的
`decide_alternatives()`：把每个 proven alternative 投影成普通 `TransformCandidate`，复用
现有 `decide()` 计算 action/score，在 accepted 方案中选择 `final_score` 最高者，并返回
`chosen_alternative_id`。若全部被拒绝则保留原 OIR。这样不复制 policy 逻辑，也不会让
Extractor 绕过 shared decision engine。

### 10.4 filter 语义

provider、saturation、extraction 和 decision 总是按 policy 执行。只有把 decision 追加到
`CostModelReport::decisions` 时才检查 filter。以下两条命令除诊断输出外必须产生相同 OIR：

```text
compiler -O1 --emit-oir input.sy
compiler -O1 --emit-cost-model --cost-model-filter=Inline input.sy
```

## 11. OIR materialization

IR 修改必须是事务式的：先构造 `MaterializationPlan` 并完整检查，cost decision 接受后才
修改 block。

```cpp
struct PlannedInst {
    ENodeId node;
    oir::Instruction::OpID op;
    oir::Type *type;
    std::vector<PlannedValue> operands;
    oir::CmpPred cmp_pred{};
    std::size_t insertion_index;
};

struct MaterializationPlan {
    std::vector<PlannedInst> instructions;
    std::vector<std::pair<oir::Value *, PlannedValue>> root_replacements;
};
```

调度规则：

- 新指令必须位于所有 operand 定义之后、所有被替换 use 之前。
- 复用的新表达式放在能支配所有 replacement use 的最早合法位置。
- 优先复用 block 内已经存在且支配使用点的等价 OIR 指令。
- 找不到单一合法插入点时，允许复制低成本节点并重新计算成本；否则拒绝 plan。
- 生成顺序为拓扑序；发生环、类型不一致或支配失败时不提交。
- 提交后替换 root uses，旧子图由现有 DCE 删除，不在 materializer 中递归删除。

当前 OIR 只有 `append_instruction` 和 `insert_before_terminator` 公共 helper。实现阶段应新增
统一的 `BasicBlock::insert_before(Instruction*, unique_ptr<Instruction>)`，避免多个 pass
继续手写 list iterator 插入和 parent 设置。

## 12. 流水线位置

不建议在当前最多 8 次的 aggressive iteration 中每次完整 saturation。V1 使用两个固定
window：

```text
... inline / specialization
-> SCCP
-> value range
-> GVN
-> EGraphRewrite(Early, full V1 rules)
-> DCE/GVN cleanup
-> existing loop/CFG aggressive iterations
...
-> final SCCP/value range/GVN
-> EGraphRewrite(Late, Canonicalize + Simplify only)
-> final GVN/DCE/ADCE
```

Early window 能利用 inline/SCCP 暴露的常量，并给 LICM、LSR 和 loop transforms 更简单的
表达式。Late window 只做无增长或明确降成本的收尾，不再用 Explore rule，避免在流水线
末尾制造需要多轮 cleanup 的形态。

实现接口可以先作为 `oir_opt::run_egraph_rewrites(module, stats, phase)` 接入现有
`OIROptimizationPipelinePass`；核心引擎与规则仍放在独立文件。后续若拆分 monolithic OIR
pipeline，可直接包装成独立 `OIREGraphRewritePass`。

## 13. 诊断

沿用 `--emit-cost-model[=text|json]`，并为 e-graph candidate 增加以下稳定字段：

```text
provider=egraph.oir.integer.v1
component_id=<function-local numeric id>
roots=2
input_instrs=11
eclasses=18
enodes=43
merges=12
rule_matches=19
rounds=3
budget_exhausted=false
chosen_alternative=alt.1
rules=[egraph.bv32.add_zero,egraph.bv32.add_sub_cancel]
```

普通 `-O1` 保持静默。JSON 中 rule 列表要按稳定顺序输出。函数名可以用于面向开发者的
scope trace，但 rule、budget 和决策不能根据函数名变化。

## 14. 正确性与失败策略

以下条件 fail closed：

- rule guard 为 `Refuted`、`Timeout` 或 `Unknown`。
- EClass 类型不一致。
- 同一 EClass 出现冲突常量。
- proof path 无法从原始 OIR root 到选中 ENode 闭合。
- materialization 违反 dominance、use-before-def 或 OIR 类型约束。
- candidate 在 decision 前后引用的 analysis version 已失效。

单个 match 证明失败只跳过该 match；核心结构损坏、proof path 损坏或提交后 verifier 失败
则整个 pass 返回失败。cost model 不能批准没有证明的候选。

预算耗尽的处理单独区分：

- 图中已有 equality 全部由 `Proven` edge 建立：允许从当前图提取，并记录预算耗尽风险。
- 某个候选依赖未完成的 SMT/guard：该 equality 没有进入图，不能提取。
- materializer 无法生成选中方案：尝试下一个 top-K plan；全部失败则保持原 IR。

## 15. 测试方案

### 15.1 核心单元测试

- hash-cons 与 union-find canonical id。
- congruence rebuild。
- type-mismatch merge 被拒绝。
- constant analysis 冲突被检测。
- rule worklist 的确定性。
- node/match/round budget。
- 多步 proof path 和 Composite proof 汇总。
- cyclic EClass extraction 能终止。

应把测试 target 做成真正的可执行文件，避免重复当前 `smt_solver_tests` 静态库没有
`main` 的覆盖问题。

### 15.2 FileCheck 与 stage test

- 每条初始 rule 的正例和类型/guard 反例。
- 多步才能得到最优式的 equality-saturation 用例。
- 同一输入在不同 `--cost-model-filter` 下 OIR 相同。
- Conservative/Balanced/Aggressive 的预算 trace 不同但都语义正确。
- budget exhausted 后仅提取已证明候选。
- cost-model JSON 包含真实 nodes/classes/rules/alternative/proof。
- 浮点、load/call、跨 block、phi 不进入 V1 graph。
- OIR verifier、MIR lowering、ASM 和 e2e 均通过。

### 15.3 性能与编译期开销

至少记录：

- 每函数/component 的 EClass、ENode、merge、match、round 和耗时分布。
- accepted/rejected/budget-exhausted 数量及 reject reason。
- OIR 指令变化。
- `lowered -> pre-ra -> post-ra -> final` 的 virtual register、spill、load/store、stack slot
  变化。
- 同一 yoolang baseline 的运行时间与编译时间变化。

性能校准只能调整通用 rule、target cost、risk weight 和 budget，不能按 testcase 身份分支。

## 16. 分阶段落地

### P0：接口修正

- 修复 cost-model filter 改变 e-graph rewrite 行为的问题。
- 移除 `complexity * 1000` 假节点计数和 fixed-peephole profitability bypass。
- 为 `BasicBlock` 增加统一 insert-before API。

### P1：最小可用核心

- 实现 typed ENode/EClass、hash-cons、union-find、rebuild、预算和单元测试。
- 只导入 i1/i32 leaf、constant、Add/Sub/Mul/And/Xor。
- 注册低增长初始规则。

### P2：OIR provider 与 materializer

- 实现 component builder、multi-root extraction、proof path 和事务式 materialization。
- 接入 `CandidateProviderRequest/Result` 与 shared `decide()`。
- 接入 Early/Late 两个 pipeline window。

### P3：分析和 guarded rule

- 加 `ICmp`、`ZExt`、constant/known-bits analysis。
- 用 value range 和现有 QF_BV SMT 支持 conditional rule。
- 对 proof cache 使用 rule、typed expression、assumption 和 analysis fact 的稳定 key。

### P4：校准与扩展

- 完整 correctness、MIR metrics、e2e 和 performance 校准。
- 根据证据决定是否加入 reassociation、受控 distributivity、`SDiv/SRem` 或 GEP index。
- memory e-graph、CFG/region e-graph、浮点和跨 block saturation 另立设计，不顺带扩大 V1。

## 17. 验收标准

V1 完成需要同时满足：

- 存在真实 EClass/ENode、congruence rebuild 和多轮 saturation，不再是单 rewrite helper。
- 至少一个测试必须通过两步以上 equality 才得到最终最优表达式。
- 每个被提交的 plan 都有从原 OIR root 到目标表达式的完整 Proven proof path。
- extraction 使用 RV64GC target cost、code size 和 register/live-range 风险，而非最少节点。
- cost-model filter 不改变 OIR；普通 `-O1` 无额外输出。
- 所有预算可观测且确定性预算可复现。
- focused FileCheck、OIR/MIR/ASM stage、e2e 和完整优化测试通过。
- 性能报告包含 e-graph 决策、编译期开销和 RA 后风险；没有不可解释的显著回归。
