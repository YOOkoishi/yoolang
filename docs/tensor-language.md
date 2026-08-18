# Tensor 语言扩展与 sret 实现说明

本文说明 `tensor` 的源语言语法、语义类型、符号表处理、AST 到 YIR 的展开方式，
以及 tensor 作为函数返回值时采用的 sret（structure return）指针约定。

## 1. 源语言语法

tensor 的元素只能是 `int` 或 `float`，形状写在变量名后：

```c
tensor int a[2][2] = {{1, 2}, {3, 4}};
tensor float b[2][3];
```

函数返回类型必须在函数名前写出完整形状：

```c
tensor int[2][2] make_tensor(int bias) {
  tensor int value[2][2] = {{1, 2}, {3, 4}};
  return value + bias;
}
```

这里采用 `tensor int[2][2] make_tensor()`，而不是把维度写到函数名后。
原因是这些维度描述的是“返回类型本身”，放在类型旁边最直观，也不会和函数参数列表混淆。

目前支持：

- `+`、`-`、`*`、`/`：形状和元素类型相同的两个 tensor 按元素计算。
- tensor 与 `int`/`float` 标量做上述四种运算：标量先转换成 tensor 的元素类型，
  再用于每一个元素。例如 `a + 2` 和 `2 + a` 都合法。
- 一元 `+` 和 `-`。其中 `-a` 对每一个元素取负。
- `@`：仅用于二维 tensor 矩阵乘法。`[M][K] @ [K][N]` 的结果为 `[M][N]`。
- 下标、切片、整体初始化和整体赋值。例如 `a[0]` 的类型是少一维的 tensor。

`@` 与 `*`、`/` 使用相同的语法优先级，按从左到右结合。

## 2. 为什么语义层不能直接把 tensor 当作 array

YIR 最终确实把 tensor 展开为 array，但语义分析和符号表中仍需要独立的
`SemanticType::Kind::Tensor`。这是本次实现最重要的设计点。

普通 SysY array 在表达式中通常会退化为指针，而 tensor 是值类型：

- `a + b` 需要知道两边都是 tensor，才能检查形状并展开逐元素计算。
- `a + 1` 需要知道 `a` 的元素类型和完整形状，才能做标量提升。
- `a @ b` 需要读取两边二维形状并检查内维相等。
- tensor 函数调用的表达式类型必须保留完整形状，调用结果才能继续参与 `@` 或赋值。
- `a[0]` 应得到尾部形状对应的 tensor，而不是立即变成普通指针。

如果在符号表中一开始就记录成 array，上述信息会和普通数组的 decay 规则混在一起，
每个表达式位置都需要反向猜测它原来是否为 tensor，代码反而更复杂且容易出错。

因此，语义符号表保存的是源语言真实类型。例如：

```text
a            : tensor<int, [2, 2]>
make_tensor  : (int) -> tensor<int, [2, 2]>
```

`SemanticTypeContext::tensor_type(element, shape)` 负责驻留类型。相同的元素类型和形状
会得到同一个规范类型对象，所以两个 tensor 是否完全同型可以直接比较类型指针。

## 3. AST 和语义检查的修改

### 3.1 AST 与 Parser

`TypeSyntax` 增加 `Tensor`，它只保存 `tensor int` 或 `tensor float` 的元素类型。
变量形状仍复用原有声明符的 `dimensions`。

函数返回值没有变量声明符，因此 `FuncDef` 新增 `return_dimensions`，专门保存：

```c
tensor int[2][3] f()
          ^^^^^^
```

中的两个维度表达式。Parser 只在基础类型为 tensor 时读取这里的方括号；普通函数和普通
数组声明仍走原来的路径。

### 3.2 语义类型和符号表

变量声明时，语义分析先解析元素类型，再检查所有维度是否为正的编译期整数，最后形成
规范的 tensor 类型并写入 `SemanticModel` 和当前作用域的 `SemanticSymbol`。

函数在分析任何函数体以前会先统一声明。对于 tensor 返回函数，返回形状也在这个阶段
解析并写入函数类型。因此，即使被调函数的定义写在调用点后面，调用表达式仍能得到完整的
tensor 返回类型。

需要特别区分两个符号表：

- 语义分析符号表保存源语言签名 `(参数...) -> tensor<...>`，用于类型检查、重声明匹配、
  调用结果推导和形状检查。
- ASTToYIR 自己的 lowering 符号表保存已经降级后的 YIR 签名，并额外记录
  `has_sret` 和 `sret_storage_type`，用于生成隐藏参数和调用者返回槽。

不要在语义符号表阶段把返回类型改成 `void`。否则 `make_tensor()` 会被推导为 `void`，
后续初始化、`@` 和返回值检查都会失败。sret 是调用约定的实现细节，只应在 lowering 层出现。

### 3.3 运算检查

两个 tensor 做逐元素运算时，元素类型和形状必须完全相同。tensor 与标量运算时，标量通过
原有转换规则转换到元素类型，但不会在 AST 中真的构造一个 tensor；lowering 展开元素循环时
复用同一个标量值。

矩阵乘法要求两边都是二维 tensor、元素类型相同，并满足左侧列数等于右侧行数。
结果形状在语义阶段直接计算为 `[左侧行数][右侧列数]`。

## 4. AST 到 YIR：tensor 展开为 array

YIR 不新增 tensor 类型。`type_for_semantic` 按维度从内到外构造普通数组：

```text
tensor<int, [2, 3]>
    -> array<2 x array<3 x i32>>
```

局部 tensor 和运算临时值使用 `yir.array_var`。逐元素运算、一元负号和矩阵乘法都在
ASTToYIR 阶段按静态形状展开为现有的 `array_load`、标量算术和 `array_store`。
这样 YIR、OIR、MIR 和汇编后端都不需要认识新类型。

整体赋值和 tensor 初始化表达式采用逐元素拷贝。实现刻意使用直接、朴素的静态展开，
不引入新的运行时或复杂抽象；后续已有优化 pass 可以继续消除不必要的临时量。

## 5. tensor 返回值的 sret 约定

array 不能像一个 `i32` 一样直接放进单个返回寄存器，因此 tensor 返回值在 ASTToYIR 时
转换为“调用者分配返回对象，地址作为隐藏第一个参数传入”。

源代码签名：

```c
tensor int[2][2] make(int bias);
```

概念上的 YIR 签名：

```text
func make(ptr<array<2 x array<2 x i32>>> sret, i32 bias) -> void
```

调用点 `tensor int x[2][2] = make(1);` 的降级过程是：

1. 调用者创建一个 `[2][2]` 的 `ArrayVarOp`，它就是返回值的存储空间。
2. 取得整个 array 对象的地址，作为隐藏的第一个实参。
3. 普通源语言实参按原顺序放在隐藏参数之后。
4. 发出返回类型为 `void` 的 `CallOp`。
5. 调用表达式的 lowering 结果设为第 1 步的 `ArrayVarOp`，所以它仍可被初始化、赋值、
   逐元素运算或 `@` 消费。

被调函数的 `return tensor_expr;` 则变为：

1. 只计算一次 `tensor_expr`。
2. 将结果逐元素复制到隐藏 sret 指针指向的对象。
3. 发出无返回值的 `yir.return`。

这个约定也自然支持函数返回值继续参与表达式，例如：

```c
tensor int result[2][2] = make(0) @ make(1);
```

每次调用各自拥有独立的调用者返回槽，不会互相覆盖。

## 6. tensor 作为形式参数的现状

tensor 作为函数返回值现在可以编译；tensor 作为形式参数目前仍明确拒绝。例如：

```c
int sum(tensor int a[2][2]); // 当前不支持
```

原因不是符号表无法保存它，而是参数还需要单独确定值传递 ABI：是把整个 tensor 复制到
形参私有空间，还是像数组一样传地址，以及调用者和被调者由谁负责复制。直接复用普通 array
参数会让 tensor 悄悄发生 decay，破坏其值语义。为避免产生表面能编译、实际语义不确定的代码，
当前语义分析给出错误，并建议临时使用普通数组参数。

若后续要加入 tensor 形参，最直白且与当前 sret 对称的方案是：lowering 后使用隐藏/显式的
`ptr<array<...>>` 参数，并在函数入口复制到局部 tensor，从而保持按值语义。这应作为独立修改完成。

## 7. 当前限制

- 元素类型仅为 `int` 和 `float`。
- 每一维都必须是正的编译期整数，至少有一维。
- `@` 只接受二维 tensor。
- 不支持 tensor 形式参数。
- 不支持比较、取模、逻辑和位运算等未定义的 tensor 运算。
- sret 是 yoolang 当前内部约定；与外部 C/C++ 编译器链接 tensor 返回函数时，双方必须采用
  完全相同的隐藏指针签名，不能假设普通 C ABI 会自动匹配这个源语言扩展。

## 8. 主要修改位置

- `include/ast/ast.h`、`src/ast/ast.cpp`：tensor AST 类型、`@`、函数返回维度。
- `src/front/lexer.cpp`、`src/front/parser.cpp`：关键字、运算符、声明及返回类型语法。
- `include/sema/SemanticType.h`、`src/sema/SemanticType.cpp`：规范 tensor 类型和形状。
- `src/pass/ast/ASTSemanticAnalysisPass.cpp`：符号、初始化、下标、运算和函数签名检查。
- `src/sema/ConstantEvaluator.cpp`：阻止未定义的 tensor 常量折叠路径。
- `src/pass/ast/ASTToYIRPass.cpp`：array 展开、逐元素计算、矩阵乘法、整体拷贝和 sret。
- `test/easy/tensor_sret.sy`：嵌套 sret 调用及返回结果参与矩阵乘法的端到端样例。
