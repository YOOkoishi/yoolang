# SSA IR 设计与 AST->SSA IR 说明

## 1. 文档目标

本文档说明当前 `yoolang` 的 SSA IR 设计、设计原因，以及 AST 降级到 SSA IR 的实现方式。

当前阶段目标是：先保证 IR 结构正确、可验证、可打印、可扩展，不做高级优化（如支配树优化、mem2reg、CFG 优化）。

## 2. 代码位置

- SSA IR 定义：`include/IR/SSA_IR.h`
- SSA IR 实现：`src/IR/SSA_IR.cpp`
- AST -> SSA IR 入口：`include/IRGen/IRGen.h`
- AST -> SSA IR 实现：`src/IRGen/IRGen.cpp`
- 兼容旧命名入口：`include/CodeGen/CodeGen.h`

## 3. SSA IR 为什么这样设计

### 3.1 强类型优先（Type / TypeContext）

当前 Type 系统拆为：

- `VoidType`, `LabelType`, `IntegerType`, `FloatType`, `PointerType`, `ArrayType`, `FunctionType`
- `TypeContext` 统一管理基础类型与复合类型创建

这样设计的原因：

1. 类型错误尽早暴露。IR 阶段能识别 load/store、返回值、比较指令的类型是否匹配。
2. AST 到 IR 时映射直接。`BuiltinType` 可以稳定映射到 IR 类型。
3. 后续扩展方便。比如新增 `i64`、向量类型或更复杂数组布局时，不需要推翻框架。

### 3.2 Value-User-Instruction 三层模型

当前遵循通用 IR 模型：

- `Value`：所有可被引用的值
- `User`：引用其他 `Value` 的节点
- `Instruction`：具体指令（算术、内存、控制流、调用、phi 等）

这样设计的原因：

1. 操作数关系明确。每条指令的输入输出都以 `Value*` 连接，而不是字符串拼接。
2. 更接近主流编译器中间表示，后续对接优化/后端更自然。
3. 可打印可验证。每种指令都能 `print()`，并被统一验证器检查。

### 3.3 BasicBlock 显式记录 CFG 信息

`BasicBlock` 同时维护：

- 指令序列（`instructions_`）
- 前驱/后继列表（`predecessors_` / `successors_`）
- terminator 约束检查（`has_terminator()`）

这样设计的原因：

1. CFG 是控制流语义核心。没有前驱/后继，phi 检查和分支验证会很弱。
2. 先把 CFG 结构搭好，后续再加优化 pass 成本更低。

### 3.4 Module/Function 统一拥有对象生命周期

`Module` 持有函数、全局变量、常量对象，`Function` 持有参数和基本块，`BasicBlock` 持有指令。

这样设计的原因：

1. 明确所有权，避免悬空指针。
2. 构造和销毁顺序可控，调试时更稳定。

### 3.5 IRBuilder 与 Verifier 分离

- `IRBuilder`：负责构造指令、维护插入点、补充 CFG 边
- `Verifier`：负责规则检查（终结指令、返回类型、load/store 指针类型、phi incoming 类型等）

这样设计的原因：

1. 构造逻辑与正确性检查解耦，便于逐步开发。
2. AST lowering 可先“能生成”，再靠 Verifier 快速定位错误。

## 4. AST -> SSA IR 如何实现（当前版本）

当前主入口类为：`irgen::ASTToIRLowering`。

### 4.1 总流程

`lower(CompUnit&, module_name)` 依次执行：

1. `declare_runtime_builtins`
2. `lower_global_decls`
3. `lower_function_signatures`
4. `lower_function_bodies`

### 4.2 运行时内建函数声明

`declare_runtime_builtins` 会注册 SysY 常见运行时函数签名，例如：

- `getint/getfloat/getarray/getfarray`
- `putint/putfloat/putarray/putfarray/putch`
- `starttime/stoptime`

作用：

1. 保证调用点在 IR 里有可引用的函数实体。
2. 后续解析真实 AST 时，调用 lowering 不会缺失符号。

### 4.3 全局声明降级

`lower_global_decls` 处理全局变量：

1. 先把 AST `BuiltinType` 映射为 IR 基础类型
2. 用 `build_decl_type` 从维度信息构造多维 `ArrayType`
3. 尝试 `try_lower_global_scalar_init` 处理标量字面量初始化
4. 通过 `Module::create_global` 注册全局符号

当前策略偏保守：只直接吸收可静态识别的标量字面量初始化，其他初始化后续扩展。

### 4.4 函数签名降级

`lower_function_signatures` 先创建函数壳：

1. 生成返回类型
2. 生成参数类型列表
3. 对数组参数先按指针形态处理（当前实现）
4. 创建函数并同步参数名

这样做的原因：

1. 先建符号表，再 lower 函数体时可处理互相调用。
2. 降低“先见后用”问题。

### 4.5 函数体降级（当前阶段）

`lower_function_bodies` 目前是最小闭环实现：

1. 给每个函数确保有 `entry` 基本块
2. 设置 IRBuilder 插入点
3. 根据返回类型插入默认 `ret`（void/float/int）

这是“先保证结构正确”的阶段性实现，目的是：

1. 让 Verifier 和 IR 文本输出完整跑通
2. 给后续语句/表达式 lowering 提供稳定底座

## 5. 当前实现的边界

已完成：

- 类型系统、值系统、指令系统、CFG 基础、Builder、Verifier
- 全局声明和函数签名骨架
- 可生成并打印结构合法的 SSA IR

未完成（下一步）：

- 语句级 lowering：`if/while/break/continue/return` 完整语义
- 表达式级 lowering：短路逻辑、数组下标寻址、函数调用参数细化
- 更完整的初始化列表展开
- 高级优化 pass

## 6. 与旧命名的兼容策略

考虑历史代码中可能仍引用 `CodeGen`/`codegen::ASTToSSA`，当前保留兼容：

- `include/CodeGen/CodeGen.h` 作为兼容头
- 通过类型别名转发到 `irgen::ASTToIRLowering`

这样可以先推进命名清晰化，同时降低重构冲击。

## 7. 推荐后续开发顺序

1. 先完成表达式 lowering（算术、比较、调用、短路）
2. 再完成语句 lowering（分支、循环、跳转、块作用域）
3. 增强数组/GEP 与初始化展开
4. 用 Verifier + IR dump + 样例对照做回归

按这个顺序可以在不引入高级优化的前提下，较快获得“功能完整”的 SSA IR。
