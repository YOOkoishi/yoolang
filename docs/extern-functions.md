# External function declarations

yoolang supports declaration-only foreign functions with explicit `extern`
syntax. An external declaration contributes a typed callable symbol but never
emits a local function body:

```c
extern int read_scaled(int values[], float scale);
extern void notify(void);
extern int log_values(int level, ...);
extern vector<int,3> transform(vector<int,3> value, mask<3> active);
extern mask<31> clean_mask(mask<31> value);
```

`()` and `(void)` both spell a zero-parameter prototype. Parameter names are
optional in an external declaration, including `extern float convert(int,
float);`; function definitions still require named parameters. Array
parameters use the ordinary source rule and decay to pointers in the canonical
function type. `void` is otherwise not a parameter or object type.

The ellipsis is explicit and must be last. Variadic definitions are not
accepted: the current source spelling is declaration-only, such as `extern int
log(int level, ...);` or `extern int log_anything(...);`. Fixed arguments retain
their declared types. Extra variadic arguments may use the scalar types
supported by OIR, but a fixed vector, mask, or an aggregate containing either
is rejected. There are no implicit function declarations: every call must bind
to a preceding-or-later compatible declaration, definition, or registered
builtin in the same compilation unit.

Compatible external declarations may repeat, and one compatible definition
may coexist with them in either source order. Compatibility is tested on the
resolved semantic function type, after array-parameter decay and including the
return type, every fixed parameter, and the variadic bit. A mismatched
redeclaration reports `SE0018`; a second definition reports `SE0004`.
Declarations also cannot replace a global object or builtin registry entry.
Calls keep the existing contextual conversion rules and diagnose wrong arity,
incompatible fixed-vector/mask shape, or an unknown callee.

The declaration is preserved at each IR boundary:

- AST printing uses `FuncDecl ... extern=true` and records no body;
- YIR printing uses `yir.declare`, with typed parameter values and no region;
- YIR-to-OIR lowering creates an external OIR `declare` and does not synthesize
  an entry block;
- OIR printing, parsing, and verification round-trip that declaration.

An external fixed-vector or mask signature uses the same selected public ABI as
an ordinary source definition. On the default RV64GCV mode this is the standard
LP64D aggregate convention described in
[standard-vector-aggregate-abi.md](standard-vector-aggregate-abi.md), not the
vector-register calling convention. The language still has no scalable-vector
type syntax.

Focused coverage lives in `test/ir/frontend_extern_*.sy`,
`scripts/frontend_semantic_infra_tests.py`, `scripts/yir_infra_tests.py`, and
`scripts/oir_parser_infra_tests.py`. The bidirectional GCC/Clang/QEMU ABI gate
uses real undefined external symbols for every yoolang-caller → C-callee case;
it does not weaken a locally generated stub.
