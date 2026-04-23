# YIR Design Draft

## Position

Proposed pipeline:

`AST -> YIR -> SSA_IR -> MIR`

`YIR` is the structured, source-close IR for SysY. It is not SSA CFG IR, and it is not machine IR.

Its job is:

- preserve structured control flow (`if`, `while`, `break`, `continue`)
- preserve source-level memory objects (locals, globals, arrays, init lists)
- make array decay, casts, and short-circuit explicit
- be easy to verify before lowering to `SSA_IR`

Its job is not:

- global SSA optimizations
- register allocation
- ABI details
- instruction selection

## Alignment With SysY2022

This draft is aligned to [SysY2022语言定义-V1.pdf](/home/yoo/Documents/Compliers/yoolang/%E5%8F%82%E8%80%83/%E7%AC%AC%E4%B8%80%E7%89%88pku-minic/SysY2022%E8%AF%AD%E8%A8%80%E5%AE%9A%E4%B9%89-V1.pdf).

The important language constraints that directly shape `YIR` are:

- SysY source syntax contains `if (...) stmt` and `while (...) stmt`, but no `do ... while`, `for`, or ternary operator.
- `Exp` is an `int/float` expression; `Cond` is a condition expression.
- `!` belongs to conditional expressions in the source language and is not a general arithmetic operator.
- source language has implicit `int <-> float` conversion, but no explicit cast syntax.
- function parameter arrays decay to addresses; only the first dimension may be omitted.
- top-level declarations and function definitions cannot redefine the same identifier.
- a legal SysY program must contain exactly one `int main()`.

Because of that, `YIR` should not pretend SysY has extra source constructs. In particular:

- no `do`-style loop syntax in the `YIR` text format
- no source-level explicit cast syntax
- no source-visible boolean type; `i1` is an internal `YIR` type only
- no general pointer arithmetic beyond array/parameter addressing

## What We Borrow From The References

From the PKU-minic IR:

- structured regions are much easier to get right than building CFG too early
- explicit `alloca/load/store/call/return` is a good fit for SysY
- loops should stay loops in the IR instead of being flattened immediately

From ClangIR/CIR:

- strong typing
- region-based `if` / `loop`
- explicit memory model
- explicit casts instead of implicit type magic

From SysY itself:

- the language is small, so the IR should stay small
- no need for structs, unions, exceptions, classes, varargs, or general pointer arithmetic
- arrays, init lists, arithmetic, calls, and loops are the real core

## Design Goals

1. Correctness first.
   `YIR` should make it hard to build malformed control flow and malformed array semantics.

2. Structured control flow.
   `if` and `while` remain first-class operations with regions.

3. Explicit memory.
   Mutable variables and arrays live in memory objects. `load/store` are explicit.

4. Strong but minimal typing.
   Enough typing to verify SysY semantics, but not a full C/C++ type system.

5. Easy lowering to `SSA_IR`.
   Every `YIR` op should have a simple and mostly local lowering rule.

## Non-Goals

- no optimization-specific mega IR
- no generic MLIR-like extensibility system
- no attempt to represent all of C
- no backend concerns in this layer

## Core Model

`YIR` is a typed, value-based, structured IR.

- Expressions produce SSA-like temporary values.
- Source variables are represented by explicit storage objects.
- Control flow uses regions, not raw basic blocks.
- Short-circuit semantics are represented by structured control flow, not by ad hoc tricks.

This gives us a hybrid model:

- scalar computations are value-oriented
- mutable state is memory-oriented
- control flow is structured

That is a good match for SysY.

## Type System

Primitive types:

- `i1`
- `i32`
- `f32`
- `void`

Derived types:

- `ptr<T>`
- `array<N x T>`
- `func<(T1, T2, ...) -> R>`

Notes:

- `i1` is an internal boolean type in `YIR`.
- SysY logical/comparison expressions still semantically behave like integers; explicit conversion ops bridge `i1 <-> i32`.
- Local/global arrays use full `array<...>` types.
- Array parameters use already-decayed pointer forms:
  - `int a[]` -> `ptr<i32>`
  - `int a[][10]` -> `ptr<array<10 x i32>>`

This keeps parameter semantics simple and avoids inventing a second array-parameter type.

## Values And Objects

There are two important categories:

1. Scalar values
   Examples: results of arithmetic, comparisons, casts, and loads.

2. Address values
   Examples: results of `alloca`, global addresses, and element addresses.

Addresses always have type `ptr<T>`.

Examples:

- local scalar variable: `%x.addr : ptr<i32>`
- local array: `%a.addr : ptr<array<10 x i32>>`
- element address: `%elt.addr : ptr<i32>`

## Memory Model

Memory is explicit.

Key rule:

- source variables are not directly SSA values
- source variables are storage locations
- reading a variable requires `load`
- writing a variable requires `store`

This is deliberate. It keeps `YIR` simple and avoids re-solving SSA in the front end.

## Operation Set

The `YIR` op set should stay small.

### Module / Symbol Ops

- `yir.global`
- `yir.func`
- `yir.extern_func`

### Memory Ops

- `yir.alloca`
- `yir.addr_of`
- `yir.load`
- `yir.store`
- `yir.elem_addr`
- `yir.decay`

### Constant Ops

- `yir.const.i32`
- `yir.const.f32`
- `yir.const.bool`
- `yir.zero`

### Arithmetic Ops

Integer:

- `yir.addi`
- `yir.subi`
- `yir.muli`
- `yir.divsi`
- `yir.remsi`

Float:

- `yir.addf`
- `yir.subf`
- `yir.mulf`
- `yir.divf`

### Comparison Ops

Integer comparisons:

- `yir.icmp eq/ne/lt/le/gt/ge`

Float comparisons:

- `yir.fcmp eq/ne/lt/le/gt/ge`

Result type is always `i1`.

### Conversion Ops

- `yir.zext_i1_to_i32`
- `yir.trunc_i32_to_i1`
- `yir.sitofp`
- `yir.fptosi`
- `yir.to_bool`

`yir.to_bool` is important for SysY truthiness:

- `i1 -> i1` no-op
- `i32 -> i1` compare with zero
- `f32 -> i1` compare with `0.0`

These are IR-internal conversions inserted by lowering. They are not source-language syntax.

### Call Ops

- `yir.call`

### Structured Control Ops

- `yir.if`
- `yir.while`
- `yir.break`
- `yir.continue`
- `yir.return`
- `yir.cond`

## Why `if` And `while` Are Region Ops

This is the main design choice.

`YIR` should not expose raw blocks and branches. That belongs to `SSA_IR`.

So:

- `yir.if` owns `then` and optional `else` regions
- `yir.while` owns `cond` and `body` regions
- `break` / `continue` are only valid inside `yir.while`

This makes verification much easier:

- no dangling branches
- no malformed phi-like edges
- no misplaced terminators

## `yir.if`

`yir.if` corresponds directly to SysY `if` syntax and owns a `then` region plus an optional `else` region.

Text form example:

```mlir
yir.if %cond {
  ...
} else {
  ...
}
```

`YIR` may internally allow a result-producing structured branch for short-circuit lowering, but that is an internal IR design choice rather than a source-language construct. The final text syntax for that internal form is intentionally left unspecified here.

### Short-Circuit Lowering

Since SysY defines `&&` and `||` with C-like short-circuit semantics, `YIR` must preserve that behavior before lowering to CFG form.

Conceptually:

```mlir
`a && b`
  evaluate `a`
  if `a` is false, result is `0`
  otherwise evaluate `b`, convert to truth value, and produce `0/1`

`a || b`
  evaluate `a`
  if `a` is true, result is `1`
  otherwise evaluate `b`, convert to truth value, and produce `0/1`
```

Example for `a && b`:

```mlir
%lhs = ...
%lhs.b = yir.to_bool %lhs : i32 -> i1
%r = yir.if %lhs.b {
  %rhs = ...
  %rhs.b = yir.to_bool %rhs : i32 -> i1
  %rhs.i = yir.zext_i1_to_i32 %rhs.b : i1 -> i32
  ; internal result path
} else {
  %zero = yir.const.i32 0
  ; internal result path
}
```

Similarly, `a || b` yields `1` in the true branch and evaluates `b` only in the false branch.

This avoids the broken CFG/phi situation we ran into before.

## `yir.while`

`yir.while` corresponds directly to SysY `while (Cond) Stmt`.

The concrete text format should not introduce source-like extra keywords such as `do`.

Recommended abstract shape:

- one condition region
- one body region

Recommended text spelling:

```mlir
yir.while cond {
  ...
  yir.cond %cond : i1
} body {
  ...
}
```

Rules:

- the condition region must end with `yir.cond`
- the body region may end normally, or via `yir.break`, or via `yir.continue`
- `yir.break` exits the nearest enclosing `yir.while`
- `yir.continue` jumps to the next condition evaluation of the nearest enclosing `yir.while`

This keeps loop structure explicit for later loop-oriented analysis and optimization.

## Arrays

Arrays are first-class aggregate types in `YIR`.

Examples:

- `array<10 x i32>`
- `array<4 x array<2 x i32>>`

Local array:

```mlir
%a.addr = yir.alloca array<10 x i32>
```

Array element address:

```mlir
%e.addr = yir.elem_addr %a.addr[%i] : ptr<array<10 x i32>> -> ptr<i32>
```

Multidimensional:

```mlir
%e.addr = yir.elem_addr %m.addr[%i, %j] : ptr<array<4 x array<2 x i32>>> -> ptr<i32>
```

### Array Decay

SysY function arguments need C-like array decay.

Example:

```c
int f(int a[]) { ... }
int main() {
  int x[10];
  return f(x);
}
```

In `YIR`:

```mlir
%x.addr = yir.alloca array<10 x i32>
%x.ptr = yir.decay %x.addr : ptr<array<10 x i32>> -> ptr<i32>
%r = yir.call @f(%x.ptr) : (ptr<i32>) -> i32
```

This should be explicit. It is one of the places where previous lowering went wrong.

Important SysY-specific restriction:

- in normal `LVal` use, indexing must reach an element
- bare array values are only allowed in contexts where SysY semantics imply decay to an address, chiefly function arguments

## Initializers

`YIR` should keep aggregate initializers explicit instead of flattening them too early.

Recommended representation:

- globals keep a constant initializer tree
- locals lower init lists into explicit stores during YIR construction

This split is pragmatic:

- global initializers must be link-time constants anyway
- local initializers are easier to reason about as executable code

This also matches the SysY2022 rules for `ConstInitVal` / `InitVal`:

- empty braces mean zero-initialization
- nested braces may exactly mirror dimensions
- row-major flattened initialization is also allowed

### Global Initializer Form

```mlir
yir.global @a : array<4 x array<2 x i32>> =
  yir.init [
    1,
    2,
    [3],
    4,
    5,
    [6]
  ]
```

### Local Initializer Lowering

For locals:

```c
int a[3] = {1, 2};
```

YIR after construction may become:

```mlir
%a.addr = yir.alloca array<3 x i32>
%p0 = yir.elem_addr %a.addr[0] : ptr<array<3 x i32>> -> ptr<i32>
yir.store (yir.const.i32 1), %p0 : i32, ptr<i32>
%p1 = yir.elem_addr %a.addr[1] : ptr<array<3 x i32>> -> ptr<i32>
yir.store (yir.const.i32 2), %p1 : i32, ptr<i32>
```

Any unspecified elements are zero-initialized by explicit stores or a zero-init helper.

## Globals

Globals should stay simple:

- scalar global with constant initializer
- scalar global with zero initializer
- aggregate global with constant initializer tree
- external declarations for runtime functions

Examples:

```mlir
yir.global @count : i32 = 0
yir.extern_func @getint() -> i32
yir.extern_func @putint(i32) -> void
```

## Suggested Text Format

The text format should be readable, SysY-oriented, and should avoid suggesting source constructs that SysY does not have.

Example:

```mlir
module {
  yir.extern_func @getint() -> i32
  yir.extern_func @putint(i32) -> void

  yir.func @main() -> i32 {
    %i.addr = yir.alloca i32
    %sum.addr = yir.alloca i32

    %c0 = yir.const.i32 0
    yir.store %c0, %i.addr : i32, ptr<i32>
    yir.store %c0, %sum.addr : i32, ptr<i32>

    %n = yir.call @getint() : () -> i32

    yir.while cond {
      %i = yir.load %i.addr : ptr<i32> -> i32
      %cond = yir.icmp lt %i, %n : i32
      yir.cond %cond : i1
    } body {
      %sum = yir.load %sum.addr : ptr<i32> -> i32
      %i2 = yir.load %i.addr : ptr<i32> -> i32
      %sum2 = yir.addi %sum, %i2 : i32
      yir.store %sum2, %sum.addr : i32, ptr<i32>

      %one = yir.const.i32 1
      %i3 = yir.addi %i2, %one : i32
      yir.store %i3, %i.addr : i32, ptr<i32>
    }

    %ret = yir.load %sum.addr : ptr<i32> -> i32
    yir.call @putint(%ret) : (i32) -> void
    %zero = yir.const.i32 0
    yir.return %zero : i32
  }
}
```

## Verifier Rules

`YIR` needs a real verifier. At minimum:

### Structural

- module symbols are unique
- there is exactly one `int main()` with no parameters
- every function body is well-formed
- every region terminates correctly
- `break` / `continue` appear only inside `yir.while`
- `yir.cond` appears only as the terminator of a while-condition region

### Type

- `load/store` operand types match pointee type
- `elem_addr` indices are integers
- `elem_addr` index count matches aggregate nesting
- `%` is only used on integer operands
- `call` arguments match parameter types exactly after inserted implicit-conversion ops
- `if` condition is `i1`
- `while` condition result is `i1`
- `return` value matches function return type

### SysY-Specific

- no `void` variables
- global initializers must be constant
- local `const` objects must have initializers
- function parameter arrays use decayed pointer types
- `Exp`-context values are `i32` or `f32`
- `Cond`-context values are lowered through `yir.to_bool`
- no source-level explicit cast nodes survive into `YIR`; inserted conversions are compiler-generated
- array parameter declarations may omit only the first dimension
- for source-level `LVal`, full indexing is required except in decay contexts such as array actual arguments

## Lowering To `SSA_IR`

Lowering should be deterministic and mostly syntax-directed.

### Memory / Scalars

- `alloca` -> entry-block alloca
- `load/store` -> direct SSA_IR load/store
- arithmetic, cmp, cast -> direct SSA_IR instructions

### `if`

- create `then`, `else`, `merge` blocks
- statement-form `if` lowers without phi
- any internal result-producing branch form used for short-circuit lowering lowers with one phi per result

### `while`

- create `cond`, `body`, `end` blocks
- `yir.cond` lowers to conditional branch
- `break` lowers to branch to `end`
- `continue` lowers to branch to `cond`

### `decay`

- lower to GEP zero-zero when needed
- for pointer-to-array parameters, decay rules remain explicit and local

### Benefit

Because short-circuit and loops are already structured in `YIR`, `SSA_IR` lowering becomes mechanical instead of heuristic.

## Why This Design Is Better Than Direct AST -> SSA_IR

Because the bug-prone parts become explicit before CFG lowering:

- short-circuit is encoded through structured `if`
- `break/continue` live in real loop regions
- array decay is explicit
- init lists are explicit
- inserted implicit conversions are explicit

That is exactly the set of places where direct AST -> SSA IR usually gets messy.

## Why This Design Is Better Than Putting Loop Optimization In MIR

Loop optimizations want:

- source-like loop structure
- explicit memory objects
- no backend artifacts

`YIR` has all three.

`MIR` should only care about:

- RV64 legality
- calling convention
- stack slots
- instruction selection

So loop optimizations belong in `YIR` or later in `SSA_IR`, not in `MIR`.

## Recommended Optimization Split

Do in `YIR`:

- constant folding
- dead branch elimination when condition is constant
- simple init-list canonicalization
- loop normalization for `while`
- explicit array decay / cast canonicalization

Do in `SSA_IR`:

- mem2reg
- DCE
- GVN/CSE
- LICM
- strength reduction
- induction-variable analysis

This split is much cleaner than trying to make one IR serve every stage.

## Suggested MVP

If we want to build `YIR` incrementally, the first version should support only:

- globals
- scalar locals
- local arrays
- `alloca/load/store`
- `elem_addr`
- arithmetic / cmp / inserted conversions
- `call`
- statement `if`
- `while`
- `break/continue`
- `return`
- explicit `decay`

That is already enough to cover the current SysY tests and unblock correct lowering.

## Final Recommendation

I recommend `YIR` as:

- one structured high-level IR layer
- strongly typed
- memory explicit
- region based
- still intentionally small

I do not recommend:

- multiple high-level IR layers before `SSA_IR`
- putting backend details into `YIR`
- flattening into CFG before `YIR` verification

If this direction looks right, the next concrete step should be defining:

1. the C++ class hierarchy for `YIR`
2. a verifier checklist
3. the exact textual printer format
4. the lowering contract from `AST` to `YIR`
