# yoolang RISC-V MIR 设计草案

本文设计一版面向 RISC-V、贴近底层汇编的 MIR（Machine IR），用于承接当前
`AST -> YIR -> SSA OIR` 管线，并作为寄存器分配、栈帧生成和汇编输出的核心数据
结构。

## 1. 设计目标

MIR 不再表达源语言结构，也不再保留 OIR 的平台无关 SSA 抽象。它要表达：

- 真实 RISC-V 指令形态：三地址整数指令、load/store、比较即分支、无条件跳转、
  `jal/jalr` 调用返回、独立浮点寄存器文件。
- 真实 ABI 约束：参数/返回寄存器、调用者/被调用者保存寄存器、16 字节栈对齐、
  `ra/sp/gp/tp` 等特殊寄存器。
- 真实机器资源：虚拟寄存器、物理寄存器、寄存器类、栈对象、frame index、隐式
  clobber、内存读写副作用。
- 汇编前仍需要延迟决策的事项：大立即数、全局地址、超出 12 位的栈偏移、长分支、
  函数调用伪指令等。

设计上不把 MIR 做成“另一版 OIR”。OIR 负责 SSA、类型和平台无关优化；MIR 负责
指令选择之后的一切后端问题。

## 2. 目标平台选择

当前仓库的 `runtime/libsysy_riscv.a` 经 `readelf` 检查为 ELF64 RISC-V，属性包含
`rv64i_m_a_f_d_c_zicsr_zifencei`，ELF flags 为 double-float ABI，栈对齐属性为
16 bytes。因此 yoolang 第一版后端应以：

```text
arch = rv64gc
abi  = lp64d
xlen = 64
flen = 64
stack_align = 16
```

为默认目标。

这有几个直接影响：

- `int`/`float` 仍是 4 字节，指针是 8 字节。
- OIR 的 `i32` 算术应优先选择 RV64 的 word 指令，如 `addw/subw/mulw/divw/remw`，
  使结果按 32 位语义产生并符号扩展。
- 指针、数组地址、栈地址使用 64 位 GPR。
- 普通浮点参数/返回值走 `fa0-fa7`；但可变参数按整数调用约定处理。
- 栈指针在函数入口和整个函数执行期间保持 16 字节对齐。

参考依据：`参考/RISC-V-Reader-Chinese-v1.pdf` 第 2、3、5 章，以及 RISC-V psABI
2026-04-25 draft。psABI 说明 LP64D 是 RV64G 的推荐默认 ABI，并规定了整数/浮点
寄存器约定、栈对齐、硬浮点参数和可变参数规则。

## 3. 层级定位

建议新增后端管线：

```text
AST
  -> YIR            高层、结构化、贴近源语言
  -> OIR            SSA、平台无关、优化友好
  -> PreRA MIR      目标相关，虚拟寄存器，frame index，少量 pseudo
  -> PostRA MIR     物理寄存器，仍可含 frame index / li / la / call pseudo
  -> Final MIR      frame index 已消除，立即数合法化，分支已放松
  -> ASM            RISC-V 汇编文本
```

`PreRA MIR` 和 `PostRA MIR` 使用同一套对象模型，只是验证规则不同。这样后续可以
在任意阶段打印 `--emit-mir` 做调试。

## 4. MIR 核心对象模型

### 4.1 Module / Function / BasicBlock

```cpp
namespace mir {

struct TargetInfo {
    TargetArch arch;      // RV64GC
    TargetABI abi;        // LP64D
    unsigned xlen_bits;   // 64
    unsigned flen_bits;   // 64
    unsigned stack_align; // 16
};

class Module {
    TargetInfo target;
    std::vector<Global> globals;
    std::vector<std::unique_ptr<MachineFunction>> functions;
};

class MachineFunction {
    std::string name;
    FunctionType lowered_type;
    MachineFrameInfo frame;
    MachineRegisterInfo regs;
    std::list<std::unique_ptr<MachineBasicBlock>> blocks;
    bool is_external;
    bool is_vararg;
};

class MachineBasicBlock {
    std::string name;
    std::vector<MachineBasicBlock*> preds;
    std::vector<MachineBasicBlock*> succs;
    std::list<MachineInstr> instrs;
    std::vector<Register> liveins;
};

}
```

`MachineBasicBlock` 里的控制流必须是低层控制流：块末尾只有 branch / jump / ret，
不再有 `if/while/for` 等结构化节点。

### 4.2 Register

MIR 寄存器分为虚拟寄存器和物理寄存器：

```cpp
struct Register {
    enum class Kind { Virtual, Physical };
    Kind kind;
    uint32_t id;
};
```

寄存器类：

- `GPR`：RV64 整数寄存器，承载 `i1/i32/i64/ptr`。值类型记录在 vreg 上。
- `FPR32`：承载 `float`。
- `FPR64`：为 LP64D ABI 和未来 `double` 预留。

物理寄存器分组：

- 固定不可分配：`zero`, `sp`, `gp`, `tp`。
- 特殊：`ra` 可被 call 隐式定义，非叶函数需要保存。
- GPR caller-saved：`t0-t6`, `a0-a7`。
- GPR callee-saved：`s0-s11`，其中 `s0/fp` 在启用 frame pointer 时保留。
- FPR caller-saved：`ft0-ft11`, `fa0-fa7`。
- FPR callee-saved：`fs0-fs11`，在 LP64D 下需按 ABI 保存恢复。

第一版实现可以保守预留 `t0/t1` 作为 frame-index 消除和大立即数展开的 scratch；
后续引入 register scavenger 后再释放它们参与分配。

### 4.3 MachineInstr

```cpp
class MachineInstr {
    Opcode opcode;
    std::vector<MachineOperand> operands;
    std::vector<MachineMemOperand> mem_operands;
    InstrFlags flags; // terminator/call/return/mayLoad/mayStore/sideEffect
};
```

指令必须能表达隐式寄存器：

- `CALL @foo`：隐式 use 实参寄存器，隐式 def `ra`、返回寄存器和所有 caller-saved。
- `RET`：隐式 use `ra`，以及按返回类型 use `a0` 或 `fa0`。
- `DIVW/REMW/FDIV.S`：标记可能有较高 latency，但语义上无额外状态。

### 4.4 MachineOperand

```cpp
class MachineOperand {
    enum class Kind {
        Reg,
        Imm,
        Block,
        GlobalAddress,
        ExternalSymbol,
        FrameIndex,
        ConstantPoolIndex
    };
};
```

寄存器 operand 带 def/use/kill/dead/implicit 标志。内存操作不直接使用字符串地址，
而通过 `MachineMemOperand` 记录访问宽度、对齐和别名信息：

```cpp
struct MachineMemOperand {
    enum class Kind { Load, Store };
    Kind kind;
    ValueType value_type; // I32, F32, Ptr 等
    unsigned size;
    unsigned align;
    std::optional<int> frame_index;
    std::optional<std::string> global;
};
```

这能避免旧版 `To_RiscV.cpp` 中靠名字前缀判断“是不是指针”的问题。

## 5. 目标指令与 pseudo

### 5.1 真实 RISC-V opcode

第一版需要覆盖当前 OIR 指令：

- 整数/逻辑：`ADD`, `SUB`, `ADDW`, `ADDIW`, `SUBW`, `MULW`, `DIVW`, `REMW`,
  `AND`, `OR`, `XOR`, `SLLW`, `SRAW`, `SRLW`, `SLT`, `SLTU`。
- 访存：`LB/LH/LW/LBU/LHU/LWU/LD`, `SB/SH/SW/SD`。
- 地址与立即数：`LUI`, `AUIPC`, `ADDI`, `SLLI`。
- 控制流：`BEQ`, `BNE`, `BLT`, `BGE`, `BLTU`, `BGEU`, `JAL`, `JALR`。
- 浮点：`FLW`, `FSW`, `FADD.S`, `FSUB.S`, `FMUL.S`, `FDIV.S`, `FEQ.S`, `FLT.S`,
  `FLE.S`, `FCVT.S.W`, `FCVT.W.S`, `FMV.W.X`, `FMV.X.W`, `FSGNJ.S`。

当前语言只需要 `float`，但目标 ABI 是 LP64D，所以 MIR 架构上保留 `FPR64` 和
`FCVT.D.*`/`FMV.X.D` 的扩展点，主要服务未来 `putf` 可变参数和 `double`。

### 5.2 pseudo opcode

MIR 允许少量 pseudo，直到合适阶段展开：

- `COPY dst, src`：寄存器复制，寄存器分配前后都可存在，最终展开为 `mv`、
  `fsgnj.s`、`fmv.x.w` 等。
- `LI dst, imm`：大立即数物化，最终由 asm printer 或 pseudo expansion 展开。
- `LA dst, symbol`：全局地址物化，默认交给汇编器生成 PC-relative 序列并允许链接放松。
- `CALL symbol`：最终可打印为 `call symbol`，或展开为 `auipc+jalr`。
- `TAIL symbol`：尾调用优化用。
- `RET`：最终打印 `ret`，语义等价 `jalr zero, 0(ra)`。
- `J block`：最终打印 `j label`，语义等价 `jal zero, label`。
- `ADJCALLSTACKDOWN/UP`：若采用动态 call stack 调整时使用；第一版建议固定 outgoing
  arg area，可不生成。
- `MachinePHI`：OIR phi 进入 MIR 后的短暂形式，必须在寄存器分配前消除。

默认目标代码模型是静态 RV64 medany non-PIC。ASM printer 输出 `.option nopic`，
并保留 `la` 给 GNU as 展开成 PC-relative 重定位序列，从而避免普通全局地址物化使用
GOT/PIC load。未来如果需要 PIC 或其他 code model，应先增加显式目标选项再改变默认行为。

原则：final MIR 里的真实 opcode 必须满足 RISC-V 立即数和操作数约束；汇编器广泛
支持的 `li/la/call/j/ret/mv` 可以在 asm printer 层作为文本伪指令输出。

## 6. 类型与数据布局

目标布局：

| OIR 类型 | MIR value type | 大小 | 对齐 | 说明 |
| --- | --- | ---: | ---: | --- |
| `i1` | `I1` in GPR | 1/寄存器中 0 或 1 | 1 | 通常不落内存；spill 可用 GPR slot |
| `i32` | `I32` in GPR | 4 | 4 | 使用 `*W` 指令维持 32 位语义 |
| `float` | `F32` in FPR32 | 4 | 4 | 使用 `.S` 浮点指令 |
| `ptr` | `PTR` in GPR | 8 | 8 | LP64D 指针宽度 |
| `array` | 栈/全局对象 | 元素递归布局 | 元素对齐 | MIR 不把 array 当寄存器值 |

`i32` 的寄存器规范化策略：

- `lw`、`addw`、`subw`、`mulw`、`divw`、`remw` 都产生符号扩展到 XLEN 的结果。
- 比较可直接用 `slt`/`blt` 等 64 位比较，因为值保持了正确符号扩展。
- `i1` 始终规范化为 `0/1`，`zext i1 -> i32` 可视为 copy。

## 7. OIR 到 MIR 的降级规则

### 7.1 函数入口

每个 OIR 参数先生成 ABI live-in：

- `i32/ptr` 参数：从 `a0-a7` 或 incoming stack slot 进入 GPR vreg。
- `float` 参数：普通命名参数从 `fa0-fa7` 进入 FPR vreg；当浮点参数寄存器耗尽时转入
  stack slot。
- 可变参数：可变部分按整数调用约定，不使用 `fa*`。

入口块开头插入 `COPY %v, aN/faN`，或从 fixed frame object 加载栈上传入参数。

### 7.2 Alloca / 全局 / GEP

- `alloca [N x T]` 不生成指令，创建 frame object，返回 `FrameIndex` 地址。
- 全局变量地址使用 `LA %addr, @global`。
- `gep` 不保留为高层指令，降为地址计算：
  - 常量下标折叠进 frame/global offset。
  - 动态下标按元素 stride 生成 `slli` 或 `li+mul`，再 `add` 到 base。
  - 最终 load/store 只使用 RISC-V 支持的 `base + signed 12-bit offset`，超限由后续 pass
    合法化。

### 7.3 Load / Store

按访问类型选择指令：

- `i32`：`LW` / `SW`
- `float`：`FLW` / `FSW`
- `ptr`：`LD` / `SD`

内存 operand 记录 size/align，便于 verifier 检查 `LW` 不能访问 8 字节 ptr slot。

### 7.4 整数算术

| OIR | MIR |
| --- | --- |
| `add i32` | `ADDIW` 或 `ADDW` |
| `sub i32` | `ADDIW imm=-x` 或 `SUBW` |
| `mul i32` | `MULW` |
| `sdiv i32` | `DIVW` |
| `srem i32` | `REMW` |

立即数选择规则：

- 12 位有符号立即数可选 `ADDIW/ADDI`。
- 超过范围的立即数先 `LI` 到临时 vreg。
- 乘除不做复杂 strength reduction 作为 v1 必需项；后续 peephole 可把常数乘法降为
  shift/add。

### 7.5 比较与分支

RISC-V 没有条件码，MIR 也不引入 flags。

OIR `icmp` 若结果被普通使用：

- `lt`：`SLT`
- `gt`：交换操作数后 `SLT`
- `eq/ne`：`XOR` 后 `SEQZ/SNEZ` pseudo，最终展开为 `sltiu`/`sltu` 等序列。
- `le/ge`：`SLT` 后 `XORI 1`，或在 branch combine 中换成反向分支。

OIR `br i1 %cond`：

- 若 `%cond` 来自可合并的 `icmp`，直接选择 `BEQ/BNE/BLT/BGE`。
- 否则使用 `BNE %cond, zero, true` 加 `J false`。

这贴合 RISC-V “比较即分支”的模型，也避免生成无意义的布尔临时。

### 7.6 浮点

| OIR | MIR |
| --- | --- |
| `fadd/fsub/fmul/fdiv` | `FADD.S/FSUB.S/FMUL.S/FDIV.S` |
| `sitofp` | `FCVT.S.W` |
| `fptosi` | `FCVT.W.S` |
| `fcmp eq/lt/le` | `FEQ.S/FLT.S/FLE.S` |
| `fcmp gt/ge` | 交换操作数后 `FLT.S/FLE.S` |
| `fcmp ne` | `FEQ.S` 后 `XORI 1` |

浮点比较结果落入 GPR `i1`，不是 FPR。

### 7.7 Call / Return

调用 lowering 生成：

1. 为每个实参分配 ABI 位置。
2. 对寄存器实参插入 `COPY aN/faN, %arg`。
3. 对栈实参存到 outgoing arg area。
4. 插入 `CALL @callee`，带隐式 use/def/clobber。
5. 若有返回值，从 `a0` 或 `fa0` copy 到结果 vreg。

返回 lowering：

1. 将返回值 copy 到 `a0` 或 `fa0`。
2. 跳转到统一 epilogue block。

第一版建议所有 return 统一跳到 epilogue，便于只生成一份恢复 `ra/s*` 的代码。

## 8. Phi 消除与 SSA 出口

OIR 是 SSA，MIR 在寄存器分配前需要消除 phi。

流程：

1. `OIRToMIR` 初始生成 `MachinePHI`。
2. `SplitCriticalEdges` 确保 phi copy 插入不会污染其他边。
3. `MachinePhiElim` 在前驱块 terminator 前插入 parallel copy。
4. `TwoAddress/COPY coalescing` 尽量消除多余 copy。

必须把 parallel copy 作为一组处理，避免交换值时覆盖。例如：

```text
%a = phi [%b, pred]
%b = phi [%a, pred]
```

需要临时寄存器或 copy cycle breaker。

## 9. 栈帧设计

`MachineFrameInfo` 管理所有栈对象：

```cpp
struct StackObject {
    enum class Kind {
        Local,
        Spill,
        CalleeSaved,
        IncomingArg,
        OutgoingArg
    };
    int id;
    Kind kind;
    uint32_t size;
    uint32_t align;
    int64_t offset;      // frame layout 后填充
    bool fixed;          // incoming arg 等固定对象
};
```

推荐布局：

```text
高地址
  caller frame
  incoming stack args        // 入口 sp + 0, +8, ...
  -------------------- CFA / entry sp
  callee-saved area          // ra, s*, fs*
  spill slots
  local arrays / allocas
  outgoing arg area
  alignment padding
  -------------------- current sp
低地址
```

策略：

- frame size 最终向上取整到 16。
- 非叶函数保存 `ra`；使用了 `s*`/`fs*` 的函数保存对应 callee-saved。
- 第一版固定预留最大 outgoing arg area，调用期间不反复调整 `sp`。
- 无动态 alloca 时默认不使用 frame pointer；若开启 frame pointer，则 `s0` 指向 CFA。
- `IncomingArg` 可通过 `sp + frame_size + offset` 或 `s0 + offset` 访问。

典型 prologue：

```asm
  addi sp, sp, -frame_size
  sd   ra, frame_size-8(sp)
  sd   s1, frame_size-16(sp)
```

frame size 或保存偏移超出 12 位时：

```asm
  li   t0, -frame_size
  add  sp, sp, t0
  li   t0, large_offset
  add  t0, sp, t0
  sd   ra, 0(t0)
```

epilogue 反向恢复并 `ret`。

## 10. 寄存器分配

第一版建议实现线性扫描或简化图染色，接口上预留更强分配器。

输入：

- 已消除 phi 的 PreRA MIR。
- 每条指令的 def/use/implicit-use/implicit-def。
- call clobber 信息。
- loop depth 或 block frequency 可后续加入。

分配策略：

- 短活跃区间优先用 caller-saved：`t*`, `a*`, `ft*`, `fa*`。
- 跨 call 活跃的 vreg 优先尝试 callee-saved：`s*`, `fs*`。
- `GPR` 和 `FPR` 分开分配。
- `i32` spill 可用 4 字节 `sw/lw`；`ptr`/GPR64 spill 用 8 字节 `sd/ld`；`FPR32` spill 用
  `fsw/flw`。
- 分配出 callee-saved 物理寄存器时记录到 `MachineFrameInfo`，由 prologue/epilogue
  插入保存恢复。

后续优化：

- copy coalescing。
- rematerialization：`LI small`, `LA global` 等可重算值减少 spill。
- register scavenger：释放预留 scratch。

## 11. 合法化与汇编输出

### 11.1 ImmediateLegalizer

检查所有真实 opcode 的立即数约束：

- I/S 型访存/算术 offset：signed 12-bit。
- branch offset：交由 branch relaxation，或初期让汇编器处理 label。
- shift amount：立即数字段宽度合法。

不合法时物化临时地址或立即数。

### 11.2 FrameIndexElim

把 `FrameIndex + offset` 改写成：

- `offset(sp)` 或 `offset(s0)`，若 offset 在 12 位范围。
- `li scratch, offset; add scratch, sp/s0, scratch; 0(scratch)`，若超限。

这个 pass 在寄存器分配后运行，需要 scratch 策略。

### 11.3 BranchRelaxation

第一版打印汇编时可让 assembler/linker 处理 label 距离；但 MIR verifier 仍应知道真实
branch 范围。后续若输出机器码，需把超范围 conditional branch 展开为：

```asm
  b<inverse> rs1, rs2, .Lskip
  jal zero, far_target
.Lskip:
```

### 11.4 AsmPrinter

输出格式：

- `.text`, `.globl`, `.type`, function label。
- `.section .rodata/.data/.bss`，全局初始化使用 `.word/.float/.zero`。
- 指令使用 ABI 寄存器名：`a0`, `sp`, `fa0`。
- 默认打印可被 GNU as 接受的伪指令：`li`, `la`, `call`, `j`, `ret`, `mv`。
- 汇编参数应匹配 runtime：`-march=rv64gc -mabi=lp64d`。

### 11.2 RVV VL/VTYPE 状态数据流

RVV 指令对 `vl` 和 `vtype` 有隐式依赖，不能仅在指令选择时记住“最后一条
`vsetvli`”。MIR 把 `VL`/`VTYPE`/`VXRM`/`VXSAT`/`VSTART` 建模为真实的隐式机器
状态：RVV consumer 显式 use 相关状态，`vset{i}vli` 显式 def `VL/VTYPE`，任何可能
调用的指令都隐式 def 五个状态并因此杀死已知配置。

`MIRVectorStatePass` 在向量寄存器分配之后、标量寄存器分配之前运行：

1. 为每个 AVL 请求分配稳定的 `vl-id`。这个身份基于立即数、VLMAX 或标量
   vreg，不依赖后续标量 RA 选择的物理寄存器名。
2. 对每个函数做 CFG 固定点数据流。格状态分量跟踪硬件 SEW/LMUL、tail/mask
   policy、实际 VL 身份和可重建的 AVL 请求；固定向量的逻辑 lane 数不伪装成
   硬件 VTYPE。
3. 在 diamond 汇合和 loop backedge 做分量 meet。如果前驱仅 policy 不同但
   SEW/LMUL 与 VL 身份一致，插入 `vsetvli zero, zero` 保留 VL 并恢复所需
   policy；如果 VL 身份不同，只在有可证明的 AVL 请求时才重建，否则 fail
   closed。
4. 删除结果无用且对入状态没有任何改变的冗余 state set。
5. 将每条 consumer 需要的 `vl-id` 写回结构化 `MachineVectorInfo`。PostRA 调度和
   pseudo expansion 后的 Final verifier 重新运行同一 CFG 分析，因此任何非法重排、
   漏配置或 call 后误用都会稳定拒绝。

生产 pipeline 的相对顺序是：

```text
PreRA schedule
  -> vector RA
  -> CFG VL/VTYPE state pass
  -> scalar RA
  -> PostRA peephole/schedule
  -> pseudo expansion
  -> Final verifier
```

这个顺序允许状态修复仍使用 AVL 的标量 vreg，同时让后续调度器通过隐式
use/def 看到真实硬件依赖。

## 12. 文本 MIR 格式

建议 `--emit-mir` 输出可读、可 snapshot 的格式：

```text
; target: riscv64, abi=lp64d, features=+m,+a,+f,+d,+c

func @main() -> i32 {
  frame align=16 size=<unknown>

bb.0.entry:
  %0:gpr = LI 1
  %1:gpr = LI 2
  %2:gpr = ADDW %0:gpr, %1:gpr
  COPY a0, %2:gpr
  J %bb.1.epilogue

bb.1.epilogue:
  RET implicit a0
}
```

带 frame object 的示例：

```text
frame:
  fi#0 local [400 x i32] size=1600 align=4
  fi#1 spill gpr size=8 align=8

bb.0.entry:
  %base:gpr = FI_ADDR fi#0
  %idx:gpr = ADDW %n, 1
  %off:gpr = SLLI %idx, 2
  %addr:gpr = ADD %base, %off
  SW %value, 0(%addr) :: store i32 align 4
```

`FI_ADDR` 是打印用 pseudo，真实 final MIR 会改写为 `addi/add+li`。

## 13. Verifier 规则

PreRA verifier：

- 每个 vreg 有且只有一个寄存器类。
- 真实 opcode 的 operand 数量、def/use 类型匹配。
- block terminator 只能出现在块末尾。
- `MachinePHI` 只能在块开头，且 predecessor 数量匹配。
- call 的 ABI 实参 copy 必须在 call 前可见。
- load/store 的 value type 与 mem size 对应。

PostRA verifier：

- 不再允许 `MachinePHI`。
- 所有 vreg 都已分配或显式 spill/reload。
- callee-saved 使用集与 frame 保存恢复一致。
- `sp/gp/tp/zero` 没有非法 def。
- final 阶段不允许 frame index 出现在真实访存指令中。
- final 阶段真实 opcode 的立即数均合法。

## 14. 与现有代码的集成点

建议新增文件：

```text
include/mir/MIR.h
include/mir/MIRPrinter.h
include/mir/MIRVerifier.h
include/mir/RISCV.h

src/mir/MIR.cpp
src/mir/MIRPrinter.cpp
src/mir/MIRVerifier.cpp
src/mir/RISCV.cpp

include/pass/oir/OIRToMIRPass.h
include/pass/mir/MIRRegAllocPass.h
include/pass/mir/MIRFramePass.h
include/pass/mir/MIRToAsmPass.h

src/pass/oir/OIRToMIRPass.cpp
src/pass/mir/MIRRegAllocPass.cpp
src/pass/mir/MIRFramePass.cpp
src/pass/mir/MIRToAsmPass.cpp
```

`PassContext` 增加：

```cpp
bool has_machine_module() const;
mir::Module *machine_module();
void set_machine_module(std::unique_ptr<mir::Module> module);
std::unique_ptr<mir::Module> take_machine_module();
```

命令行：

```text
--emit-mir       输出 PreRA 或 PostRA MIR
--emit-asm       输出 RISC-V 汇编
--mir-stage=pre-isel|pre-ra|post-ra|final
--target=riscv64-linux-gnu
--march=rv64gc
--mabi=lp64d
```

第一版可以只支持 `--emit-mir` 和 `--emit-asm`，其余作为内部默认。

## 15. 落地路线

### 阶段 1：骨架和可打印 MIR

- 实现 MIR 数据结构、RISC-V opcode 表、printer、verifier。
- `OIRToMIRPass` 支持函数、基本块、常量、copy、return。
- 加 `--emit-mir` snapshot 测试。

### 阶段 2：无优化可运行后端

- 覆盖 OIR 的整数、浮点、load/store、gep、call、branch。
- 实现 phi 消除。
- 实现简单寄存器分配和 spill。
- 实现 frame layout、prologue/epilogue、asm printer。
- 用 `test/easy` 中 basic/sum/array/manyargs/float/fib 验证。

### 阶段 3：正确性补齐

- 大栈帧、大偏移、大立即数。
- 多返回点统一 epilogue。
- 递归和跨 call 活跃值。
- `float` 参数、返回值、数组参数。
- 可变参数 `putf` 的 ABI 特化。

### 阶段 4：质量优化

- compare+branch combine。
- copy coalescing。
- rematerialization。
- leaf function 优化。
- tail call。
- 常量乘法 strength reduction。
- 简单块布局和 fallthrough 优化。

## 16. 关键设计取舍

1. 以 RV64GC/LP64D 为默认目标，而不是 RV32I。
   仓库 runtime 已是 ELF64 double-float ABI；若 MIR 设计成 RV32，会在链接和调用约定
   上立即错位。

2. MIR 保留 `FrameIndex`，不要过早变成 `sp+offset` 字符串。
   栈大小、callee-saved、spill、outgoing args 都会改变 offset，过早展开会导致后续难以
   修正。

3. MIR 不建条件码。
   RISC-V 的条件分支直接比较两个寄存器，浮点比较也产出整数布尔值；条件码只会让
   后端模型变复杂。

4. `i32` 在 RV64 上使用 word 指令。
   这样能稳定维持 SysY `int` 的 32 位语义，减少到处补 `sext.w`。

5. pseudo 是后端工程需要，不是抽象泄漏。
   `li/la/call/ret` 都是 RISC-V 汇编器常用伪指令；保留到 asm printer 能让第一版更稳，
   之后再按需要展开。

6. 第一版寄存器分配应先追求正确性。
   旧版直接把所有临时落栈虽然简单，但性能和 ABI 都容易失控；新 MIR 应从 vreg 和
   reg class 开始，即使初始分配器朴素，也要留下优化空间。

## 17. 最小验收标准

- `--emit-mir` 能稳定打印所有 `test/easy/*.sy` 的 MIR。
- `--emit-asm` 生成的汇编能用 `rv64gc/lp64d` 工具链汇编并链接 `runtime/libsysy_riscv.a`。
- 至少通过：
  - 整数：`basic.sy`, `sum.sy`, `fib.sy`, `manyargs.sy`
  - 控制流：`break.sy`, `short.sy`, `tco.sy`
  - 数组/指针：`array.sy`, `bigarray.sy`, `pointer.sy`
  - 浮点：`float.sy`, `floatarr.sy`
- MIR verifier 在 preRA/postRA/final 三个阶段都能捕捉常见错误。

## 18. 参考资料

- `参考/RISC-V-Reader-Chinese-v1.pdf`：RISC-V 指令格式、寄存器、load/store、
  比较分支、调用约定、伪指令、浮点指令。
- RISC-V psABI：<https://riscv-non-isa.github.io/riscv-elf-psabi-doc/>。用于确认
  LP64D、寄存器保存责任、硬浮点调用约定、可变参数和栈对齐。
- `参考/第一版pku-minic/To_RiscV.cpp`：作为反例参考，避免直接字符串输出、名字猜测
  指针、所有临时落栈等设计。
- `runtime/libsysy_riscv.a`：用 `readelf -h/-A` 确认当前运行库目标为 ELF64 RISC-V、
  double-float ABI、16 字节栈对齐。
