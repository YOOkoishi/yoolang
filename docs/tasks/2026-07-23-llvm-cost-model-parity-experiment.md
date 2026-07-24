# Task: LLVM-Level Cost Model Parameter Experiment

Status: ready_for_review
Created: 2026-07-23
Last update: 2026-07-23
Owner: Codex
Branch: `experiment/llvm-cost-model-parity`
Base commit: `c815887`

## Goal

Calibrate the default balanced cost-model policy toward the comparable Clang/LLVM RISC-V `-O3`
heuristics, then run a reproducible same-machine experiment against both the unchanged Yoolang
baseline and the real pre-cost-model compiler at `6bc9662`.

## Non-goals

- Do not copy LLVM thresholds whose units or transform semantics do not map to Yoolang.
- Do not add benchmark-, function-, filename-, or input-specific behavior.
- Do not change legality/proof fail-closed behavior.
- Do not claim complete LLVM parity from a focused experimental scope.

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [ ] YIR
- [x] OIR
- [x] MIR
- [ ] ASM
- [ ] Runtime
- [x] Test infrastructure
- [x] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: `docs/cost-model-calibration.md`, `docs/llvm/riscv_o3_pipeline_report.md`
- Source/script anchors: `include/pass/CostModel.h`, `src/pass/CostModel.cpp`,
  `src/pass/oir/OIRInlinePass.cpp`, `scripts/compare_perf.py`
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- Parser, AST, runtime, and unrelated YIR transform sources
- Historical task records outside cost-model and current LLVM comparison anchors

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |
| optimization skill compiler/performance references | full | live pipeline and measurement contract | yes | verified snapshot differs from HEAD; live sources rechecked |
| `include/pass/CostModel.h` | cost profile and policy structs | parameter source of truth | yes | balanced defaults are declared in the struct |
| `src/pass/CostModel.cpp` | policy construction and decision path | confirm which parameters gate transforms | yes | legality remains before profitability |
| `src/pass/oir/OIRInlinePass.cpp` | constants, unit model, gate sites | map LLVM inline units safely | yes | nested calls now charge `6 + argument_count`; direct calls use callsite credit |
| `test/ir/cost_model_inline_llvm_o3.sy` | full | boundary regression test | yes | covers zero- and one-argument nested-call charge/callsite-credit boundaries; aggressive accepts both sides |
| `scripts/compare_perf.py` | compiler flags and report behavior | establish experiment procedure | yes | target flags are now explicitly fixed to RV64GC/LP64D/medany |
| `docs/cost-model-calibration.md` | full | prior broad calibration | yes | existing broad evidence reaches Clang geomean parity |
| `docs/llvm/riscv_o3_pipeline_report.md` | O3 IR pipeline and source provenance | local LLVM comparison anchor | yes | LLVM 23 source audit, RV64 non-LTO O3 |
| LLVM upstream `InlineCost.h/.cpp` | current constants and decision formula | primary external parameter reference | yes | O3 threshold 250, instruction cost 5, call penalty 25, single-BB bonus 50% |
| historical cost-model tasks and git history | activation boundary and legacy limits | define the requested pre-model baseline | yes | `6bc9662` is the final pre-implementation compiler |
| focused and CI-parity reports under `build/llvm-cost-model-experiment/` | same-machine evidence | compare original, current, and pre-model compilers | yes | all focused runs and all three 115-case runs passed |

## Branch

Decision: used

Reason:

```text
The user explicitly requested a new branch for an experimental parameter change.
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git switch -c experiment/llvm-cost-model-parity
```

## Invariants And Risks

Correctness invariants:

- Every accepted transform must still pass its existing semantic legality/equivalence proof.
- `Refuted`, `Timeout`, and `Unknown` proof outcomes remain rejected.
- Cost and eligibility changes may alter optimization choices, but emitted program semantics must
  remain unchanged.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- Higher code-growth allowances can increase instruction-cache footprint and compile time.
- Higher pressure allowances can increase spills after register allocation.
- LLVM abstract costs are not raw instruction counts; unit normalization is mandatory.
- Timing-only differences can be QEMU noise, so same-Yoolang deltas and MIR metrics are primary.

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P0 | Preserve same-HEAD baseline and probe existing aggressive policy | generated reports, this task | focused 10-case perf | done | aggressive grew final MIR 4732 -> 6311 and did not improve geomean |
| P1 | Implement the evidence-supported LLVM O3 no-profile inline envelope and decouple Yoolang clone eligibility budgets | `include/pass/CostModel.h`, `src/pass/CostModel.cpp`, `src/pass/oir/OIRInlinePass.cpp`, focused test | build and cost-model FileCheck | done | strict `50/75`, callsite credit `6+nargs`, nested-call argument cost, legacy clone prefilter caps; common risk gates still apply |
| P2 | Run correctness and focused original/current/pre-model performance comparison | reports, this task | focused tests and perf delta | done | 10/10 pass; current final MIR -10.3% vs original and Yoolang runtime +1.36% vs pre-model after the final formula fix |
| P3 | Run broader optimized correctness/performance if focused result is safe | reports, this task | all suite and CI-parity perf | done | 1,456 pass / 0 fail / 1 skip; all three 115-case performance runs pass |
| P4 | Lock external compiler target flags to Yoolang's RV64GC/LP64D target | `scripts/compare_perf.py` | focused three-way rerun | done | GCC, Clang, assembly, link, and runtime all use the same target flags |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake -P /home/yoo/Documents/Compliers/yoolang` | yes | PASS | release build; Xmake projectdir reset to the live worktree and ccache disabled |
| Cost-model FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1` | yes | PASS | 5/5 including zero- and one-argument LLVM O3 boundaries |
| OIR/MIR/ASM smoke | `python3 scripts/run_tests.py --suite stage --stage oir --stage mir --stage asm --filter test/easy/basic.sy --jobs 1 --o1` | yes | PASS | 3/3 |
| Focused E2E | `test/easy/basic.sy`, `--jobs 1 --o1` | yes | PASS | 1/1 |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before review if patch retained | PASS | 1,456 passed, 0 failed, 1 skipped: `shuffle1.in` is 15,767,020 bytes, over the runner's default 10 MiB input cap; the same case passes in the performance harness |
| Original Balanced performance | 10 explicit `.sy` files, target-locked | yes | PASS | 10/10; Yoolang runtime total 1.4396s; final MIR 4,732; spills 4 |
| Aggressive probe | same 10 explicit `.sy` files through an argument wrapper | yes | PASS/REJECTED | 10/10 correct, but final MIR +33%; Huffman timing -21%; not retained |
| Current performance | same 10 explicit `.sy` files, target-locked | yes | PASS | Yoolang runtime total 1.4819s; final MIR 4,244; spills 4 |
| Current vs original delta | `scripts/compare_perf_baseline.py` | yes | PASS | runtime 2.94% slower, 3 wins / 7 losses; final MIR -488 (-10.3%); no significant case regression by report thresholds |
| Historical pre-model performance | same 10 files with `6bc9662` | yes | PASS | Yoolang runtime total 1.5023s; final MIR 4,297; historical cost diagnostics unavailable as expected |
| Current vs pre-model delta | `scripts/compare_perf_baseline.py` | yes | PASS | current is 1.36% faster overall, 5 wins / 5 losses |
| CI-case-scope original Balanced | 115 cases selected like live CI, target-locked | yes | PASS | 115/115; Yoolang runtime total 13.1313s; final MIR 46,498; spills 102 |
| CI-case-scope current | identical 115 cases, target-locked | yes | PASS | 115/115; Yoolang runtime total 13.0416s; final MIR 43,543; spills 102 |
| CI-case-scope current vs original | `scripts/compare_perf_baseline.py` | yes | PASS | aggregate runtime +0.68%, 55 wins / 55 losses / 5 ties; final MIR -6.36%; no spill growth; GCC/Clang geomean speedup 1.019x/1.056x |
| CI-case-scope historical pre-model | identical 115 cases with `6bc9662` | yes | PASS | 115/115; Yoolang runtime total 13.2833s; final MIR 39,823; spills 184 |
| CI-case-scope current vs pre-model | `scripts/compare_perf_baseline.py` | yes | PASS | aggregate runtime +1.82%, 59 wins / 54 losses / 2 ties; spills -44.57%; final MIR +9.34%; historical comparison includes unrelated compiler evolution |
| Static checks | `git diff --check`; `python3 -m py_compile scripts/compare_perf.py` | yes | PASS | no whitespace or Python syntax errors |

## Experiment Environment And Artifacts

- Case selection: `test/performance,test/bsb-final`, excluding
  `test/performance/h-10-02.sy`, `test/performance/h-10-03.sy`,
  `test/bsb-final/2025-CPS-39.sy`, and `test/bsb-final/2025-Z8N-28.sy`.
- Compiler/target: Yoolang `-O1`; external compile, assembly, runtime, and link locked to
  `-march=rv64gc -mabi=lp64d -mcmodel=medany`.
- Host/tools: Linux 7.1.4 x86_64, GCC RISC-V 15.1.0, Clang 22.1.8, QEMU RISC-V 11.0.2.
- Reports:
  `build/llvm-cost-model-experiment/ci-parity-balanced-original/perf-report.{md,json}`,
  `ci-parity-llvm-o3-callsite/perf-report.{md,json}`,
  `ci-parity-pre-cost-6bc9662/perf-report.{md,json}`,
  `ci-parity-llvm-o3-vs-balanced.{md,json}`, and
  `ci-parity-llvm-o3-vs-pre-cost.{md,json}`.
- These files are ignored build artifacts rather than version-controlled evidence. This is local
  parity with CI's case-selection scope; the dedicated CI QEMU instruction-count plugin is not
  installed locally, so dynamic instruction counts are not comparable.

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| Copy LLVM numeric constants directly | superficially exact | rejected: LLVM abstract units and Yoolang fields differ |
| Switch the default to the existing aggressive policy | quickest broad relaxation | probe first; retain only if same-Yoolang evidence is safe |
| Use `225 / 5 = 45` as the O3 cap | initial interpretation | rejected: 225 is the default/O2 threshold; LLVM O3 uses 250 |
| Map LLVM `InlineSizeAllowance=100` to shared growth allowance | initial interpretation | rejected: the LLVM field belongs to an optional hot-profile cost-benefit path |
| Keep one shared inline cap with fixed multipliers | minimal surface change | rejected: it collapses legacy `260/180/90` clone budgets when ordinary inline changes |
| Implement callsite-aware `50/75`, `6+nargs`, and strict `<` | closest comparable no-profile O3 behavior | chosen |
| Tune individual benchmarks | might improve focused timing | rejected: non-general and contest-illegal |

## Change Log

- 2026-07-23: created task file; branch `experiment/llvm-cost-model-parity` from `c815887`.
- 2026-07-23: recorded current 10-case same-HEAD report as the initial baseline and began an
  existing-policy aggressive probe before source changes.
- 2026-07-23: aggressive probe passed correctness but increased final MIR from 4,732 to 6,311
  instructions and reduced focused GCC/Clang geomeans; rejected blanket aggressive defaults.
- 2026-07-23: rejected the initial `225 / 5` and inline-size-allowance mapping after checking
  current LLVM 24.0.0git source. O3 uses 250, single-block callees receive a 50% threshold bonus,
  and callsites receive instruction/call/argument credits.
- 2026-07-23: found the first `llvm-normalized` report invalid: Xmake's cached project directory
  pointed at a deleted read-only worktree, so it had rebuilt stale source. Reset the project to the
  live repository, disabled ccache, rebuilt, and discarded the mislabeled report as evidence.
- 2026-07-23: implemented strict normalized thresholds `50/75`, direct-call credit `6+nargs`,
  nested-call argument charging, an inline-specific growth allowance, and independent
  specialized/recursive/specialization budgets derived from the pre-model pass.
- 2026-07-23: fixed the performance harness target to RV64GC/LP64D/medany and completed target-locked
  10-case runs for original Balanced, the experiment, and historical `6bc9662`.
- 2026-07-23: completed the optimized all-suite gate with 1,456 passes, no failures, and one skip.
- 2026-07-23: completed three target-locked runs over the live 115-case CI scope. The experiment
  is 0.68% faster than the unchanged same-HEAD Balanced compiler, with 6.36% fewer final MIR
  instructions and unchanged spills. It is 1.82% faster than historical pre-cost `6bc9662`, with
  44.57% fewer spills; the historical comparison remains secondary because it spans other compiler
  changes.
- 2026-07-23: final independent review found that nested calls needed base instruction cost plus
  call penalty, i.e. normalized `6+nargs` rather than `5+nargs`. Corrected the formula, split pure
  inline-cost threshold checking from ordinary return-count eligibility to avoid specialization
  coverage loss, added zero/one-argument boundary tests, and regenerated current focused and
  115-case evidence.
- 2026-07-23: recorded the six-case mirrored Huffman regression and the aggregate/individual-case
  boundary, completed static checks and a second independent review with no blockers, and moved the
  uncommitted experimental diff to `ready_for_review`.

## Open Questions

- Three Huffman variants plus their three BSB mirrors reproduce a 20.2%-25.0% timing loss versus
  unchanged Balanced and a 14.6%-16.3% loss versus pre-cost. Aggregate runtime still improves,
  helped materially by six CRC-like wins. A later tuning patch can investigate this family without
  benchmark-specific dispatch.
- QEMU dynamic instruction counting is unavailable in this workspace; timing, target-locked
  external comparisons, and MIR metrics are the available evidence.

## Handoff Note

Current state:

- The experiment branch contains the callsite-aware LLVM O3 mapping, decoupled legacy clone
  budgets, and an RV64GC/LP64D-locked performance harness.
- Build, focused stage/e2e, all five cost-model FileChecks, and the optimized all-suite gate pass.
- On the live 115-case CI selection scope, aggregate current runtime is 0.68% faster than unchanged
  Balanced and 1.82% faster than the real pre-cost-model compiler. It also reaches 1.019x GCC and
  1.056x Clang O3 geomean speedup in this target-locked QEMU experiment.
- Same-HEAD final MIR falls 6.36% with unchanged spills. Compared with the historical compiler,
  spills fall 44.57%, while final MIR is 9.34% larger due to intervening pipeline differences.
- The requested pre-model level is reached for aggregate runtime and register-allocation pressure,
  not for every case or historical MIR size. The patch is ready for review but remains uncommitted.
  QEMU dynamic instruction counts remain unavailable, and the reproducible Huffman-family timing
  loss is recorded rather than hidden.

Next action:

- Review the experimental mapping and evidence, then decide whether to commit or continue with a
  general Huffman-regression investigation.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-23-llvm-cost-model-parity-experiment.md`
- `include/pass/CostModel.h`
- `src/pass/CostModel.cpp`
- `src/pass/oir/OIRInlinePass.cpp`
- `test/ir/cost_model_inline_llvm_o3.sy`
- `scripts/compare_perf.py`
