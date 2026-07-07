# SMT 求解器 Code Review 说明

审阅日期：2026-07-07

## 结论概览

当前实现已经不是原来的静态/伪 SMT 适配器，而是一个进程内的 Boolean + QF_BV 求解器。它可以把受支持的布尔与定宽位向量表达式 bit-blast 成 CNF，再用内部 SAT 求解器判断 `Sat` / `Unsat` / `Timeout` / `Unknown`，并通过 `pass::smt::prove_obligation` 接入 cost-model 证明体系。

对编译器优化而言，最重要的安全边界是：只有 `Unsat` 反例查询会映射成 `ProofStatus::Proven` 并允许 legality 相关改写；`Sat` 会映射成 `Refuted`，`Timeout` 和 `Unknown` 都不会证明转换合法。这个方向是正确的。

但当前实现仍有两个需要优先修复或明确接受的风险：

1. `SolverOptions` 的资源限制只在 SAT 搜索阶段生效，bit-blast 编码阶段不受 `timeout_us`、`max_sat_variables`、`max_clauses` 约束。
2. OIR 普通优化路径在没有 cost-model report 时也会实际调用 SMT，且不会经过 `CostModelPolicy::allow_smt` 的统一门控。

## 目前能完成的功能

### 表达式与类型系统

入口：`include/smt/Expr.h`、`src/smt/Expr.cpp`

已支持：

- `Bool` 与 `(_ BitVec width)` 两类 sort。
- 位向量宽度限制为 `1..64`。
- 布尔常量、布尔变量。
- 位向量常量、位向量变量。
- 构造表达式时做 sort 校验；非法组合会抛 `std::invalid_argument`。
- 表达式为不可变 DAG，节点通过 `shared_ptr<const Node>` 共享。
- 提供 `smt::to_string` 输出人类可读公式。

注意：`to_string` 不是完整 SMT-LIB 序列化器，当前项目也没有 SMT-LIB parser。

### Boolean 求解能力

入口：`smt::Solver::check`

已支持：

- `not`
- `and`
- `or`
- `xor`
- implication
- Boolean equality / disequality
- 多个 Boolean assertions 按合取处理
- `Sat` 时提取 Boolean model

### QF_BV 位向量能力

入口：`BitBlaster`，实现位于 `src/smt/Solver.cpp`

已支持：

- `bvnot`
- `bvand`
- `bvor`
- `bvxor`
- `bvadd`
- `bvsub`
- `bvneg`
- 位向量 equality / disequality
- unsigned compare：`bvult`、`bvule`
- signed compare：`bvslt`、`bvsle`
- 常量位移：左移、逻辑右移、算术右移
- `concat`
- `extract`
- `Sat` 时提取 1..64 位的 bit-vector model
- 对 `add/sub/neg` 线性等式做一个小型正规化，能提前证明一部分代数恒等式，例如 `(x - y) + y == x`

### SAT 核心能力

入口：`SatSolver`，实现位于 `src/smt/Solver.cpp`

已支持：

- CNF clause 存储
- deterministic variable allocation
- watched-literal propagation
- unit propagation
- chronological backtracking
- 简单 DPLL 搜索
- 决策数、冲突数、传播数、SAT 变量数、clause 数诊断
- `max_decisions`、`max_conflicts`、`max_propagations`、`max_sat_variables`、`max_clauses`、`timeout_us` 搜索预算

未实现：

- clause learning
- restart
- pure literal elimination
- incremental solving
- unsat core

### Cost-model 证明适配

入口：`include/pass/SMTProof.h`、`src/pass/SMTProof.cpp`

已支持：

- `SMTObligation` 携带 typed `smt::Expr` assertions。
- 对 typed assertions 调用 `smt::Solver`。
- 状态映射：
  - solver `Unsat` -> proof `Proven`
  - solver `Sat` -> proof `Refuted`
  - solver `Timeout` -> proof `Timeout`
  - solver `Unknown` -> proof `Unknown`
- `Sat` 时在 summary 中写入 counterexample model。
- proof cache 会纳入 stage、公式文本、assumptions、guarantees、timeout、estimated cost、solver options、typed assertion 结构。
- cache key 已避免单纯依赖 `smt::to_string` 的文本碰撞。

### 当前 OIR 集成能力

入口：`src/pass/oir/OIRLocalSimplify.cpp`

当前只把 SMT 用在一个局部代数规则上：

```text
(x - y) + z  =>  x
```

具体行为：

- 如果 `y` 与 `z` 是同一个 SSA value 或相同整数常量，走结构化 fast path。
- 如果不是结构化相同，会尝试把相关 i32 表达式翻译为 QF_BV。
- 目前 OIR translator 支持 i32 的：
  - `Add`
  - `Sub`
  - `And`
  - `Xor`
  - 常量
  - 非 binary i32 value 作为符号变量
- translator 不支持的表达式会导致 proof `Unknown`，不会提交 rewrite。
- SMT 证明方式是构造反例公式 `distinct(transformed, original)`；只有该公式 `Unsat` 才证明 rewrite 合法。

## Code Review 发现

### 1. 资源限制没有覆盖 bit-blast 阶段

严重级别：中高

位置：

- `src/smt/Solver.cpp` 中 `Solver::check` 先构造 `SatSolver` 和 `BitBlaster`，然后对所有 assertion 执行 `bitblaster.bool_expr(assertion)` / `assert_lit`。
- 之后才调用 `sat.solve({}, options)`。
- `max_sat_variables`、`max_clauses`、`timeout_us` 的检查在 `SatSolver::within_limits()`，也就是搜索阶段。

影响：

- 大公式会先完成 bit-blast，期间可以创建大量 SAT 变量和 clause。
- 如果公式在编码阶段就已经超过预算，当前不会提前返回 `Timeout`。
- 对来自编译器 pass 的表达式，目前有深度/复杂度估计保护；但公共 solver API 本身没有编码阶段预算保护。

建议：

- 把 `SolverOptions` 传入 `BitBlaster` 或 `SatSolver::new_var/add_clause`。
- 在 `new_var()` 和 `add_clause()` 时即时检查 `max_sat_variables` / `max_clauses`。
- 在 bit-blast 递归入口检查 monotonic `timeout_us`。
- 让编码阶段预算耗尽也返回 `CheckStatus::Timeout`，并填充 diagnostics。

### 2. 普通 OIR 优化路径会绕过 `allow_smt` 门控

严重级别：中

位置：

- `src/pass/oir/OIRLocalSimplify.cpp` 中，非结构化 add/sub cancellation 会调用 `prove_obligation`。
- 当 `stats.cost_model_report == nullptr` 时，函数直接返回 `proof.status == Proven`。
- `CostModelPolicy::allow_smt` 目前只在 `cost_model_allows_transform` 中检查；没有 report 时不会经过该统一路径。

影响：

- 即使未来 policy 想禁用 SMT，普通编译路径仍可能调用 solver。
- 这会改变默认编译时开销和优化行为。
- 目前每个 obligation 有 `max_smt_time_us` 和复杂度估计，但是否允许 SMT 的策略位没有被统一尊重。

建议：

- 在进入 SMT 前显式检查 `policy.allow_smt`。
- 或把无 report 的路径也统一走一个轻量 decision helper。
- 如果设计目标是普通 `-O1` 默认允许 SMT，应在文档中明确，并保留一个可关闭开关。

### 3. `describe_model` 输出顺序不稳定

严重级别：低

位置：

- `src/pass/SMTProof.cpp` 的 `describe_model` 遍历 `unordered_map`。

影响：

- `Refuted` summary 中变量顺序可能不稳定。
- 如果后续 FileCheck、JSON snapshot 或性能报告依赖完整 summary，可能产生噪音。

建议：

- 输出前把 model 项收集到 vector 并按变量名排序。

### 4. 还有未使用的旧 helper

严重级别：低

位置：

- `src/pass/oir/OIRLocalSimplify.cpp` 中 `smt_expr_has_unsupported_op` 当前已不再参与 SMT 翻译判断。

影响：

- 不影响行为，但会误导后续维护者，以为 unsupported 判断仍由这个函数负责。

建议：

- 删除该 helper，或改成 translator 前置过滤并保持单一来源。

### 5. 测试覆盖集中但还不够系统

严重级别：中低

已覆盖：

- Boolean SAT/UNSAT
- add/sub 等价证明
- refuted model
- signed/unsigned compare 区分
- 模运算 model
- `concat` / `extract` / `lshr`
- 资源限制 timeout
- proof adapter/cache
- cost-model SMT 状态输出

建议补充：

- `bv_shl`、`bv_ashr`、shift amount >= width。
- `bv_ule`、`bv_sle` 的边界值。
- Boolean equality / disequality。
- 64-bit 宽度边界。
- unsupported assertion 返回 `Unknown`。
- 编码阶段预算耗尽。
- 随机小宽度 QF_BV 与暴力枚举对拍。
- 若允许，可增加与 Z3/CVC5 的离线差分测试，但不要作为编译器运行时依赖。

## 验证结果

本次 review 中实际运行：

```bash
xmake
xmake run smt_solver_tests
python3 scripts/run_tests.py --suite filecheck --filter cost_model_smt --jobs 1
python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1
```

结果：

- `xmake`：PASS
- `smt_solver_tests`：PASS
- `cost_model_smt` FileCheck：1 passed
- `cost_model` FileCheck：4 passed

未在本次 review 中重新运行完整 e2e、完整 FileCheck、完整性能测试。任务文档 `docs/tasks/2026-07-07-smt-solver.md` 记录过更完整的验证矩阵，但本 review 只以上述命令作为重新验证证据。

## 当前不支持的能力

当前求解器不支持：

- 量词
- array theory
- memory model
- floating point
- unbounded integer arithmetic
- bit-vector multiplication
- division / remainder
- variable shift
- SMT-LIB parsing
- incremental solving
- unsat core
- 外部 solver fallback

这些不支持项目前都应该 fail closed，也就是返回 `Unknown` 或由上层 translator 拒绝生成 typed assertion。

## 建议优先级

建议先修：

1. 给 bit-blast 阶段接入预算检查。
2. 明确并实现普通优化路径的 SMT 开关策略。
3. 对 model summary 排序，减少报告非确定性。
4. 删除或合并未使用的 unsupported helper。
5. 增加小宽度随机对拍测试，提升 solver 核心可信度。

总体判断：当前实现已经能承担小范围 compiler legality proof，尤其是 OIR i32 add/sub cancellation 这类局部 QF_BV 证明。但它还不适合作为无边界的通用 SMT 前端使用；在扩大到更多 OIR/MIR 规则前，应先把编码阶段预算和测试对拍补上。
