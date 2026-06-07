# Task: OIR Memzero MIR Lowering

Status: ready_for_review
Created: 2026-06-07
Last update: 2026-06-08
Owner: Codex
Branch: mir++
Worktree: /home/yoo/Documents/Compliers/yoolang
Base commit: 481e0b4

## Goal

Recognize counted store-zero loops in OIR and lower them to MIR `MemZero`, with final assembly choosing an inline zero loop or `memset` when appropriate.

## Non-goals

- Do not recognize specific benchmark names or array names.
- Do not transform loops unless the store range, trip count, and side-effect constraints are proven safe.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [ ] YIR
- [x] OIR
- [x] MIR
- [x] ASM
- [x] Runtime
- [ ] Test infrastructure
- [x] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 1, `docs/mir-design.md`
- Source/script anchors: max 6
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- `src/pass/yir/`
- unrelated MIR peephole passes
- parser/frontend files

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| `docs/mir-design.md` | `MemZero` and lowering sections | MIR opcode expectations | yes | read relevant ranges |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | full | OIR pass placement | yes | primary anchor |
| `src/pass/oir/OIRLoopTransforms.cpp` | loop transform ranges | counted loop utilities | yes | large file, read loop ranges |
| `src/pass/oir/OIRToMIRVRegLowerer.cpp` | store/MemZero lowering | current aggregate zero lowering | yes | read store ranges |
| `include/mir/MIR.h` | opcode/operand definitions | extend `MemZero` operands if needed | yes | primary anchor |
| `src/mir/AsmPrinter.cpp` | `MemZero` emission | inline loop vs `memset` | yes | primary anchor |
| `src/mir/MIRVerifier.cpp` | opcode validation | verify dynamic bytes form | yes | read relevant ranges |

## Worktree

Decision: not used for this implementation

Reason:

```text
The user requested completion in the active managed workspace. The starting
worktree was clean, so the implementation stayed on the current branch.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
```

## Invariants And Risks

Correctness invariants:

- The transformed loop must write exactly the same bytes and no additional observable memory.
- The loop body must have no side effects other than the proven zero stores.
- Dynamic `MemZero` lowering must preserve ABI argument registers and call clobbers.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- Alias safety, trip count proof, byte size computation, ABI call lowering, runtime availability of `memset`.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | Detect store-zero counted loops in OIR using generic loop and alias checks | `OIRLoopTransforms.cpp`, `OIROptimizationPipelinePass.cpp`, `OIR.h`, `OIR.cpp`, OIR analyses/passes | OIR/FileCheck focused tests | done | Adds OIR `memzero ptr, byte_count` and rewrites proven single-block counted zero-store loops |
| P2 | Extend MIR `MemZero` to support dynamic byte counts | `include/mir/MIR.h`, `OIRToMIRVRegLowerer.cpp`, `OIRToMIRStackLowerer.cpp`, `MIRVerifier.cpp` | MIR/FileCheck focused tests | done | Static byte counts remain immediate operands; dynamic counts lower through GPR operands |
| P3 | Choose inline zero loop or `call memset` in ASM | `AsmPrinter.cpp`, `AsmPrinter.h`, `MIRRegAllocPass.cpp` | ASM/e2e/perf focused tests | done | Static counts >= 256 bytes call `memset`; smaller or dynamic counts use an inline byte loop with modeled clobbers |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | PASS | build completed successfully |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_memzero_loop --jobs 1` | yes | PASS | 1 passed; checks static OIR/MIR/memset and dynamic OIR/MIR/inline ASM forms |
| Full FileCheck | `python3 scripts/run_tests.py --suite filecheck --jobs 1` | yes | PASS | 17 passed |
| Direct OIR/MIR/ASM emit | compiler on `test/ir/oir_memzero_loop.sy` with `--emit-oir`, `--emit-mir-stage=lowered`, and `-S`, all `-O1` | yes | PASS | stage suite does not discover `test/ir` FileCheck-only files, so this directly verified emitted forms |
| Focused stage | `python3 scripts/run_tests.py --suite stage --stage oir --stage mir --stage asm --filter 85_long_code --jobs 1 --o1` | yes | PASS | 3 passed; covers large static zero loop in a functional testcase |
| Focused stage | `python3 scripts/run_tests.py --suite stage --stage oir --stage mir --stage asm --filter 01_mm1 --jobs 1 --o1` | yes | PASS | 3 passed; covers dynamic row clear in a performance testcase |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter 85_long_code --jobs 1 --o1` | yes | PASS | 1 passed |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter 01_mm1 --jobs 1 --o1` | yes | PASS | 1 passed |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | focused stage/e2e/filecheck passed; full suite left as broader CI coverage |
| Performance | `PERF_TEST_DIRS=test/performance/01_mm1.sy PERF_MAX_CASES=1 python3 scripts/compare_perf.py` | yes | PASS | 1 case passed; compiler output linked and ran under QEMU |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Recognize zero store loops only in MIR | Avoid OIR changes | rejected: MIR lacks clean trip-count and alias context |
| Always call `memset` | Simple and fast for large blocks | rejected: small blocks may regress and call ABI must be modeled |

## Change Log

- 2026-06-08: implemented OIR `MemZeroInst`, OIR zero-store loop rewriting, MIR dynamic byte-count lowering, ASM inline/memset emission, register-allocation clobber modeling, and focused FileCheck/stage/e2e/perf coverage.
- 2026-06-07: created task file.

## Open Questions

- None.

## Handoff Note

Current state:

- Implementation is complete and ready for review. Static large zero loops lower to OIR/MIR `MemZero` and then `call memset`; dynamic zero loops lower to MIR `MemZero` and an inline byte-zero loop.

Next action:

- Review the patch and optionally run the full optimized suite before merging.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-06-07-oir-memzero-mir-lowering.md`
- `docs/mir-design.md`
- `src/pass/oir/OIROptimizationPipelinePass.cpp`
- `src/pass/oir/OIRLoopTransforms.cpp`
- `src/pass/oir/OIRToMIRVRegLowerer.cpp`
- `include/mir/MIR.h`
- `src/mir/AsmPrinter.cpp`
- `src/mir/MIRVerifier.cpp`
