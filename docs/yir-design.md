# YIR 设计架构原理：Module, Region 与 Operation

YIR (Yoolang Intermediate Representation) 受到 MLIR 和 ClangIR 的深度启发，它放弃了传统的“按基本块 (Basic Block) 连线”的图灵状控制流（CFG），转而采用 **Structured Control Flow（结构化控制流）**。

在 YIR 中，程序的层级关系像“俄罗斯套娃”一样严密嵌套。理解 YIR 的核心，在于理解 `Module`、`Region` 和 `Operation` 相互包含的关系，其整体树状结构如下：

```text
Module (模块)
  └── Function (函数)
        └── Region (区域/函数体)
              ├── Operation (普通算子，如 Add, Assign)
              └── Operation (高级算子，包含嵌套结构，如 IfOp, WhileOp)
                    ├── Region (如 then_region)
                    │     └── Operation (内部指令...)
                    └── Region (如 else_region)
```

---

## 1. Module（模块）：最外层的容器
`Module` 代表了整个编译单元（通常对应一个 `.sy` 或 `.c` 源代码文件）。
它是所有全局对象和函数的“所有者 (Owner)”。
- 它包含一个全局变量列表 (`globals_`)。
- 它包含一个函数列表 (`functions_`)。
在 YIR 中，除了全局声明，所有具体的执行逻辑都必须生存在 Module 管辖的函数内部。

## 2. Region（区域）：结界的守卫者
`Region` 是 YIR 中极具天才设计的一个概念，它对应高级语言里的**代码块 `{ ... }`（作用域）**。
- **本质**：它是一个黑盒子容器，里面装的是一个顺序执行的算子列表（`List<Operation>`），没有任何跳转连线（飞线）。
- **用途**：
  - 所有的函数体内必须有一个 `Region`（即 `function.body()`）来容纳所有的起始计算。
  - **取代 BasicBlock**：传统的 IR 用扁平的 Basic Block 和 Jump/Branch 连接逻辑，一旦拍平，循环边界就丢失了。而在 YIR 中，我们不跳来跳去，我们只“进入”一个 Region，然后“离开”一个 Region。

## 3. Operation（算子）：微观计算的执行者
`Operation` 顾名思义就是单个运行指令。像 `x = a + b`、`Load`、`Store` 都是 Operation。
YIR 所有操作的基类都是 `Operation`，但它最深刻的地方在于：**Operation 可以反过来包含 Region**。

具体分为两类：
1. **叶子节点（普通算子）**：例如 `AddIOp`、`AssignOp` 等等。它们吃进数据，吐出结果。它们必定归属于（所属 `parent_` 指针）某一个特定的 `Region`。
2. **枝干节点（自带 Region 的高级算子）**：
   例如 `WhileOp`：
   ```cpp
   class WhileOp final : public Operation {
       Region cond_region_; // 包含用来计算 while (条件) 的那堆指令
       Region body_region_; // 包含 { 循环体 }
   };
   ```
   当流水线遍历到 `WhileOp` 时，你可以认为遇到了一层新的套娃。你不需要再通过所谓的数据流回边 (Back-edge) 分析去寻找循环到底囊括了多少代码，只要简单地去遍历 `body_region_` `内的算子即可。

## 小结：结构化的威力

这种 **“Region 包裹 Operation -> 某些 Operation 又包裹 Region”** 的递归互指结构，使得 YIR 成为了中端分析（Middle-end Analysis）的神兵利器。

我们不用像以往写 OIR/MIR 和 LLVM IR 一样绞尽脑汁去推导支配树并恢复代码“原本是什么意图”。在 YIR 层面，“意图”（如 `for`、`while` 的边界、内部计数的变量）是被 **100% 结构化保护留存**下来的。这也正是我们在其中编写 `YIRLoopCountPass` 时体验到的极度平滑和轻薄的原因。
