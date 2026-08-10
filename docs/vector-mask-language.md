# yoolang fixed vector 与 mask 源语言语义

本文定义当前编译器已经实现并由测试覆盖的源语言契约。前端/YIR/OIR 语义与具体
平台 ABI 分层；标准 aggregate ABI 和 opt-in vector ABI 分别由独立文档约束，runtime
ISA dispatch 也不由本页的类型语义隐含保证。

## 类型与长度

源语言提供两类定长值类型：

```c
vector<int, N>
vector<float, N>
mask<N>
```

`vector` 的元素只能是 `int` 或 `float`。`mask<N>` 是独立类型，不是
`vector<int,N>`，也不属于整数或浮点类型。`N` 是经过 checked constant evaluator
求值的严格正整数常量表达式；溢出、除零、非常量和非正结果都是编译错误。`N` 无须是
二的幂，因此 1、3、7、31 等长度均合法。源语言没有 scalable vector 类型语法。

vector 和 mask 都是 fixed-value 类型，可以作为全局或局部对象、数组元素、普通函数的
固定参数和返回值，也可以赋值、加载和存储。vector 本身绝不 decay 为指针；只有数组
参数和数组表达式沿用数组到指针的转换。例如 `vector<float,3> values[]` 是“vector
数组参数”，而单个 `vector<float,3>` 不能传给要求 `float *` 的参数。
它们也可出现在显式 `extern` 函数原型的固定参数与返回值中；详见
[`extern-functions.md`](extern-functions.md)。

## 构造、初始化与转换

typed literal 的语法为：

```c
vector<int,3>{1, 2, 3}
vector<float,3>{1, 2.0, 3}
mask<3>{1, 0, 1}
vector<int,3>{}       // 全零
mask<3>{}             // 全 false
```

非空 literal 必须恰有 `N` 个 lane。numeric vector 的每个 lane 按目标元素类型做普通
`int`/`float` context conversion；mask lane 必须是编译期整数常量 0 或 1。声明中的
`{...}` initializer 使用同一规则，数组可用嵌套花括号初始化；空 `{}` 表示整个目标
聚合为零。

显式构造 `vector<T,N>(expression)` 有两种含义：

- numeric scalar 先转换到 `T`，再 splat 到所有 lane；
- 同为 `N` lane 的 numeric vector 保持不变，或逐 lane 做 `int`/`float` cast。

不同 lane 数不能转换。mask 没有对应的 numeric cast/splat 构造。

在 numeric vector 的二元运算中，另一侧可以是兼容的 numeric scalar；标量先按元素
类型转换再隐式 splat。这里的隐式转换只接受已经是元素类型的标量，或从 `int` 到
`vector<float,N>` 的 `int`→`float` 转换；隐式 `float`→`vector<int,N>` 被拒绝，需写
显式构造。赋值、初始化、实参和返回值等 context conversion 使用同一隐式 splat
规则。两个 vector 操作数必须具有完全相同的元素类型和 lane 数。
`+`、`-`、`*`、`/` 支持 numeric vector，`%` 只支持整数 vector；一元 `+`/`-`
保持 vector 类型。比较运算产生同长度的 `mask<N>`。整数 vector 支持 `&`、`|`、
`^`、`~`。

mask 只支持同长度 mask 间的 `&`、`|`、`^` 以及一元 `~`，不支持算术。
`select(condition, when_true, when_false)` 的两个 value 参数也可以都是同长度 mask，
并返回同长度 mask；numeric vector 与 mask 不能混合作为两个 value 参数。mask 也不能
直接作为 `if`、`while`、`&&`、`||` 或 `!` 的条件；应先使用 `any`、`all` 或
`none` 得到源语言 `int`。

## Lane 访问与 shuffle

`v[i]` 取得 numeric vector 的元素，`m[i]` 得到源语言 `int` 0 或 1；两者均可用在
允许的赋值位置。编译器对能在编译期求值的 index 检查 `[0,N)`，动态 index 合法并在
运行时取 lane。动态 index 仍必须由程序保证位于 `[0,N)`；越界是未定义行为，编译器
不插入 bounds check，也不保证 extract/insert 的越界结果。`extract_lane` 和
`insert_lane` 采用同一条 constant-index 与动态越界规则。

`shuffle(a, b, indices)` 要求 `a`、`b` 为完全相同的 `vector<T,N>`，第三个参数必须
是同长度的编译期 `vector<int,N>` typed literal（`{}` 表示全 0），而不能是运行时
vector 值。每个 index 必须位于 `[-1,2N)`：`-1` 表示该结果 lane 未定义，
`[0,N)` 选择 `a`，`[N,2N)` 选择 `b`。这些 index 作为结构化常量进入 YIR/OIR，
不是运行时 SSA operand。

## Builtin registry 中的 vector/mask intrinsic

下表使用注册表中的精确源语言拼写。记 `V = vector<T,N>`、`VI = vector<int,N>`、
`M = mask<N>`、`T = int | float`；一次调用中的所有 vector/mask 必须具有相同 `N`。

| 名称 | 源类型 | 语义 |
| --- | --- | --- |
| `select` | `(M, V, V) -> V` 或 `(M, M, M) -> M` | 按 condition lane 选择第二或第三个值；两个 value 必须同型 |
| `any` | `(M) -> int` | 至少一个 true lane |
| `all` | `(M) -> int` | 所有 lane 均为 true |
| `none` | `(M) -> int` | 没有 true lane |
| `extract_lane` | `(V, int) -> T` | 取得一个 lane；常量 index 静态查界 |
| `insert_lane` | `(V, int, T) -> V` | 返回替换一个 lane 后的新 value |
| `iota` | `(VI) -> VI` | 返回 `0,1,...,N-1`；参数仅作为固定类型见证 |
| `reduce_add` | `(V) -> T` | 按 lane 顺序做加法归约 |
| `reduce_mul` | `(V) -> T` | 按 lane 顺序做乘法归约 |
| `reduce_min` | `(V) -> T` | 按 lane 顺序做最小值归约 |
| `reduce_max` | `(V) -> T` | 按 lane 顺序做最大值归约 |
| `reduce_and` | `(VI) -> int` | 整数按位与归约 |
| `reduce_or` | `(VI) -> int` | 整数按位或归约 |
| `reduce_xor` | `(VI) -> int` | 整数按位异或归约 |
| `masked_load` | `(T *, M, V passthrough) -> V` | active lane 加载，inactive lane 取 passthrough |
| `masked_store` | `(T *, M, V) -> void` | 只存储 active lane |
| `gather` | `(T *, VI indices, M, V passthrough) -> V` | active lane 按 index gather，inactive lane 取 passthrough |
| `scatter` | `(T *, VI indices, M, V) -> void` | active lane 按 index scatter |
| `shuffle` | `(V, V, compile-time VI) -> V` | 静态双输入 lane 重排，详见上一节 |

`masked_load`、`masked_store`、`gather` 和 `scatter` 在 OIR 中使用固定 `EVL=N`。一个
lane 只有在其 index 小于 EVL 且对应 mask bit 为 true 时才访问内存；inactive lane
不能访问、触发异常或以其他方式观察其地址。当前源元素 `int`/`float` 的这些操作使用
4-byte alignment 契约。

所有 `float` vector 的 `reduce_add`、`reduce_mul`、`reduce_min` 和 `reduce_max` 都降为
ordered reduction：求值顺序可观察时保持 lane 顺序，不允许按 unordered reduction
任意重结合。整数归约没有该浮点顺序标记。

## Mask 位布局与对象布局

lane 编号从 0 开始。YIR/OIR 的常量与内存布局把 mask 压缩为 `ceil(N/8)` byte：lane 0
是 byte 0 的最低有效位，lane 7 是其最高有效位，lane 8 是 byte 1 的最低有效位；最后
一个 byte 未使用的高位必须为 0。这个 packed 表示不改变 `m[i]` 返回 0/1 的源语义。

当前 target-independent OIR data layout 规定：

- `vector<int,N>` 和 `vector<float,N>` 的 store size 为 `4*N` byte，ABI alignment 为
  4 byte；
- `mask<N>` 的 store size 为 `ceil(N/8)` byte，ABI alignment 为 1 byte；
- 数组 stride 是元素 store size 向其 ABI alignment 上取整后的 alloc size。

这是跨前端、YIR 与 OIR 的对象布局契约，不是对任一平台 vector 寄存器传参规则的声明。
普通固定参数/返回值会保留为 fixed vector/mask 值直到 OIR。源语言不公开 scalable
vector。用户可用显式 declaration-only `extern` 原型声明 variadic 外部函数，
但不可定义 variadic 函数；对所有 variadic 尾部，vector、mask 以及包含它们的
数组/聚合会在语义阶段被明确拒绝。

无 V 目标的本地 fixed-vector SSA 值会在进入 MIR 前逐 lane scalarize；该路径覆盖
N=1/3/7/31、数值 cast、mask 逻辑、select、动态 lane 和 shuffle，并由
`rv64gc` GNU as、全 executable objdump 与 `qemu-riscv64 -cpu rv64,v=false` 测试约束。
`--emit-oir` 仍保留 target-independent typed vector IR；scalarization 只发生在继续进入
MIR/ASM 的编译中。函数参数/返回默认使用
[standard LP64D aggregate ABI](standard-vector-aggregate-abi.md)。显式
`-mvector-abi=psabi-vector` 的分阶段实现见
[fixed-length vector calling-convention variant](psabi-vector-abi.md)，但公开 CLI 在
fixed tuple lowering 和 GCC/Clang 双向互操完成前仍精确 fail-closed。fat-binary/runtime
dispatch 仍有自己的公开 ABI 边界，不能由源语言 fixed vector 类型自动推导。

## 可编译示例与验证

- [`examples/vector_value_semantics.sy`](../examples/vector_value_semantics.sy) 覆盖
  fixed value、N=1/3/7/31、literal/zero、splat/cast、运算、lane、数组和普通函数。
- [`examples/vector_intrinsics.sy`](../examples/vector_intrinsics.sy) 覆盖注册表中的
  全部 vector/mask intrinsic，以及 ordered FP reduction。

运行自动发现的文档测试：

```bash
python3 scripts/vector_docs_infra_tests.py
# 或作为统一 infra 套件的一部分
python3 scripts/run_tests.py --suite infra

# 无 V 的 source-local vector/mask 端到端标量化
python3 scripts/portable_vector_e2e_infra_tests.py
```

测试会把两个示例分别编译为 YIR 与 OIR，核对关键 typed operations，并检查非正或非法
`N`、literal lane、shape mismatch、mask 算术/条件、vector decay、越界 constant
lane、非法 shuffle 和 vector variadic 尾部等 compile-fail 契约。
