# Task: RISC-V Medany Code Model

Status: ready_for_review
Created: 2026-06-08
Last update: 2026-06-08
Owner: Codex
Branch: master
Worktree: .
Base commit: d72f2f7

## Goal

Make yoolang generated RISC-V assembly satisfy the contest requirement that code supports execution from a larger address space under the GCC `-mcmodel=medany` convention. Generated global and function symbol address materialization must use medany-compatible PC-relative code instead of PIC/GOT address loads unless an explicit future PIC mode is added.

## Non-goals

- Do not add a general target configuration system beyond the medany requirement.
- Do not add PIE/PIC support or dynamic linking support.
- Do not change the SysY ABI, runtime API, register allocation policy, or optimization pipeline semantics.
- Do not optimize unrelated loop, arithmetic, or memory-access patterns while fixing the code model.
- Do not special-case benchmark names, source filenames, symbols, input data, or known contest cases.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [ ] YIR
- [ ] OIR
- [ ] MIR
- [x] ASM
- [x] Runtime
- [x] Test infrastructure
- [ ] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 2 (`docs/mir-design.md`, `docs/tasks/README.md`)
- Source/script anchors: max 6
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- `src/pass/ast/`, `src/pass/yir/`, `src/pass/oir/`
- performance testcase sources outside focused regression examples
- polyhedral docs and tests

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required before resuming |
| `docs/tasks/TEMPLATE.md` | full | task shape | no | used to create this file |
| `docs/tasks/README.md` | `1-40` | active task registration | yes | update status when task advances |
| `src/mir/AsmPrinter.cpp` | `30-36`, `120-130` | initial `.option pic` emission and `LoadGlobalAddr` asm form | yes | implementation now emits `.option nopic` and keeps `la` |
| `scripts/run_tests.py` | `380-425` | e2e assembly/link command | yes | implementation now passes `-mcmodel=medany` on e2e link |
| `scripts/compare_perf.py` | `260-310`, `394-454` | runtime build, GCC/Clang baseline, compiler assembly path | yes | implementation now passes `-mcmodel=medany` consistently |
| `test/ir/riscv_medany_code_model.sy` | full | focused asm and relocation regression | yes | added FileCheck plus `readelf -Wr` gate for no `R_RISCV_GOT_HI20` |
| `docs/mir-design.md` | `211-230` | pseudo opcode and target code-model documentation | yes | documents static RV64 medany non-PIC default |

## Worktree

Decision: not used

Reason:

```text
Implementation stayed in the current workspace because the change remained narrow:
AsmPrinter, test/perf scripts, one focused FileCheck regression, and docs.
No concurrent source work was observed outside the existing task-record edits.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git worktree add ../yoolang-riscv-medany-code-model -b task/riscv-medany-code-model
```

## Invariants And Risks

Correctness invariants:

- The emitted assembly must assemble and link under `riscv64-linux-gnu-gcc -mcmodel=medany`.
- `LoadGlobalAddr` for data symbols and function symbols must remain semantically equivalent: it produces the full 64-bit symbol address in the destination GPR.
- The default generated code must remain valid RV64GC/LP64D static code for the current runtime and test harness.
- The final binary must continue to pass existing stage/e2e tests under `-O0` and `-O1`.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.
- The fix must be a general backend/code-model behavior, not a benchmark-specific assembly rewrite.
- Treat `-mcmodel=medany` as the default contest code model for emitted RISC-V assembly.

Risk areas:

- `.option pic` changes the `la` pseudo-instruction expansion to GOT-based addressing. Removing it or switching to `.option nopic` changes relocation shape and must be checked with `readelf -r`/`objdump -dr`.
- Some tests may FileCheck the assembly prologue and need stable expectations for `.option nopic` or absence of `.option pic`.
- `compare_perf.py` uses GCC/Clang baseline compilation and yoolang assembly compilation; all paths should use the same code model to keep comparisons fair.
- Runtime archive generation in perf tests must not introduce incompatible medlow objects if the medany requirement is interpreted for the whole static binary.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Emit medany-compatible non-PIC assembly by default | `src/mir/AsmPrinter.cpp`, focused ASM FileCheck test | focused FileCheck plus manual relocation check | done | Replaced `.option pic` with `.option nopic`; kept `la` and verified GNU as emits PC-relative relocations. |
| P2 | Make test and perf assembly/link commands use `-mcmodel=medany` consistently | `scripts/run_tests.py`, `scripts/compare_perf.py` | focused e2e, focused perf compare | done | Applied to e2e link, perf runtime build, GCC/Clang baseline compile/link, yoolang asm-to-object, and yoolang link. |
| P3 | Add a relocation-level regression gate for global symbol address materialization | `test/ir/riscv_medany_code_model.sy` | `readelf -Wr` must show no `GOT_HI20` for yoolang global loads | done | FileCheck now checks `.option nopic`, `la`, no `R_RISCV_GOT_HI20`, and expected `R_RISCV_PCREL_HI20`. |
| P4 | Document the default code-model assumption | `docs/mir-design.md` | docs review | done | Recorded static RV64 medany non-PIC as the default unless a future explicit target option overrides it. |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | Rebuilt `src/mir/AsmPrinter.cpp` and linked compiler. |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter medany --jobs 1` | yes | PASS | 1 passed; relocation gate uses `readelf -Wr`. |
| Existing MIR FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter mir_loop_licm --jobs 1` | yes | PASS | 1 passed; covers optimized MIR `LA` context. |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter test/easy/basic.sy --jobs 1 --o1` | yes | PASS | 1 passed. |
| E2E focused | `python3 scripts/run_tests.py --suite e2e --filter test/functional --jobs 1 --o1` | yes | PASS | 100 passed. |
| Relocation check | `riscv64-linux-gnu-gcc -mcmodel=medany -c /tmp/yoolang-medany.s -o /tmp/yoolang-medany.o && riscv64-linux-gnu-readelf -Wr /tmp/yoolang-medany.o` | yes | PASS | `medany_global` has `R_RISCV_PCREL_HI20`/`R_RISCV_PCREL_LO12_I`; no `R_RISCV_GOT_HI20`. |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before finalization | PASS | 1422 passed, 0 failed, 1 skipped. |
| Performance smoke | `PERF_TEST_DIRS=test/performance/many_mat_cal-1.sy,test/performance/many_mat_cal-2.sy,test/performance/many_mat_cal-3.sy python3 scripts/compare_perf.py` | yes | PASS | 3 cases passed; geomean speedup GCC 0.97x / Clang++ 0.79x; MIR metrics OK, final 696 instrs, 0 spills. |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Keep `.option pic` and only pass `-mcmodel=medany` to GCC | Simple script-only change | Rejected: `.option pic` still makes `la` expand through GOT, which is not the desired static medany convention and adds an avoidable load. |
| Emit explicit `%pcrel_hi/%pcrel_lo` pairs for every `LoadGlobalAddr` | Avoids assembler pseudo ambiguity | Consider if `.option nopic` plus `la` is not stable enough; more verbose and easier to get wrong around label pairing. |
| Emit `.option nopic` and keep `la symbol` pseudo | Smallest backend change | Preferred first patch if relocation checks confirm `la` expands to PC-relative medany form. |
| Add a compiler CLI option for code model | More flexible long-term | Deferred: contest requirement needs one default medany behavior now. |

## Change Log

- 2026-06-08: created task file for RISC-V `-mcmodel=medany` compliance and registered current known anchors.
- 2026-06-08: implemented static RV64 medany non-PIC default, updated e2e/perf toolchain flags, added relocation regression, documented MIR code-model assumption, and completed verification.

## Open Questions

- Local scripts now model the intended static medany link with `-mcmodel=medany -static` where they control the toolchain. The organizer's final link command may still be external.
- Perf rebuilds the local runtime object with `-mcmodel=medany`; `run_tests.py` still accepts the configured runtime archive but links generated assembly with medany flags.

## Handoff Note

Current state:

- Ready for review. yoolang now emits `.option nopic`; `LoadGlobalAddr` still prints `la dst, symbol`, and GNU as expands ordinary global addresses to PC-relative relocations under the default static medany model.
- `scripts/run_tests.py` links e2e programs with `-mcmodel=medany -static`.
- `scripts/compare_perf.py` uses `-mcmodel=medany` for rebuilt runtime objects, GCC/Clang baseline compile/link, yoolang assembly-to-object, and yoolang link.
- `many_mat_cal-1.compiler.o` was inspected: global A/B/C address materialization uses `R_RISCV_PCREL_HI20` plus LO12 relocations and no `R_RISCV_GOT_HI20`.

Next action:

- Review and merge, or add an explicit target option only if future PIC/code-model configurability is required.

Read next:

- `docs/tasks/2026-06-08-riscv-medany-code-model.md`
- `test/ir/riscv_medany_code_model.sy`
- `src/mir/AsmPrinter.cpp`
- `scripts/run_tests.py`
- `scripts/compare_perf.py`
- `docs/mir-design.md`
