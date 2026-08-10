# Linux RVV runtime availability probe

`runtime/rvv_runtime.c` provides the weak production interface

```c
int __yoolang_rvv_available(void);
```

This detector is consumed by the scalar-ABI multiversion dispatcher emitted by
`-mrvv-deployment=fat`.  The supported compiler slice and its fail-closed
boundaries are documented in [rvv-fat-deployment.md](rvv-fat-deployment.md).

The detector fails closed and performs these checks in order:

1. read `AT_HWCAP` with `getauxval` and record the RISC-V `V` bit;
2. query `RISCV_HWPROBE_KEY_IMA_EXT_0` with `riscv_hwprobe` and require its
   `IMA_V` result to agree with HWCAP;
3. only when `riscv_hwprobe` fails with `ENOSYS`, use HWCAP as the hardware
   fallback; any contradiction, unsupported key, or other error returns zero;
4. use `PR_RISCV_V_GET_CONTROL` and return non-zero only when the calling
   thread's current state, masked by `PR_RISCV_V_VSTATE_CTRL_CUR_MASK`, is
   `PR_RISCV_V_VSTATE_CTRL_ON`.

The hardware result is stored in a C11 atomic process-wide cache.  Vector state
is deliberately queried on every invocation because it is thread-specific and
can change.  The probe never changes vector state, installs a `SIGILL` handler,
reads `/proc`, executes an RVV instruction, or uses an RVV intrinsic.

The CMake runtime build keeps `rvv_runtime.c` separate from `sylib.c` in
`libsysy.a`.  Its implementation is weak, while `rvv_runtime.h` intentionally
uses an ordinary declaration.  A test or embedding application can therefore
link a normal strong `__yoolang_rvv_available` stub; the linker need not extract
the detector archive member.  Conversely, a fat executable that wants the weak
production implementation must link `rvv_runtime.c.o` directly or force that
archive member's extraction; leaving it unextracted is the supported
"detector missing" scalar fallback.

Host-only policy and archive tests are available through:

```bash
python3 scripts/rvv_runtime_dispatch_infra_tests.py
```

The test injects `getauxval`, `riscv_hwprobe`, and `prctl` dependencies and does
not require a RISC-V host.  When the RISC-V cross toolchain is installed it also
builds the production object for `rv64gc` and rejects any decoded RVV opcode.
