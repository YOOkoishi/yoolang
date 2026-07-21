# Task: Context-Sensitive Callsite Inlining And Transactional Residual Partial Evaluation

Status: in_progress
Created: 2026-07-17
Last update: 2026-07-21
Owner: Codex task-generation agent
Branch: `task/context-inline-residual-pe`
Base commit: `c815887`

## Goal

Implement an OIR-only interprocedural optimization path that:

1. chooses inline candidates from a lightweight direct-call graph/SCC model and general callsite
   context instead of module definition order;
2. discovers and requeues calls exposed by a successful inline, with a bounded affected cleanup
   window before the next decision; and
3. evaluates constant-argument partial evaluation on a detached scratch residual program, measures
   the real post-substitution/post-cleanup cost, and commits or rejects without speculative mutation
   of the live module.

Add only the minimum OIR function provenance needed by this task: an OIR `FunctionOrigin` and a
stable root function ID used for specialization lineage, clone counting, and growth accounting.
Generated clone names remain printable symbols only and must not drive an optimization decision.

The three Huffman files are discovery evidence and a required performance gate, never a production
matcher. The transform specification comes from generic IR invariants and metamorphic tests.

Success criteria:

- Alpha-renaming, function-definition reordering, and equivalent CFG forms produce equivalent
  structural decisions and budget outcomes.
- Newly exposed nested calls are reconsidered with fresh caller context after bounded cleanup.
- Constant substitution, return-demand slicing, cleanup, residual measurement, cost rejection,
  commit, and every failure path are independently testable.
- Cost rejection, scratch cleanup budget exhaustion, unsupported/error outcomes, and failed commit
  preflight leave live functions, blocks/instructions, use lists, function table, name allocator,
  commit stats, and cumulative growth budget unchanged; only an auditable rejected diagnostic may
  be appended.
- The focused Huffman set improves by at least 10% in geometric-mean Yoolang runtime against an
  isolated same-machine `de93e24` baseline, with no individual case slower by both more than 5% and
  more than 0.003 seconds.
- In the final focused `scripts/compare_perf.py` report, Yoolang `-O1` is faster than Clang `-O3`
  for each of `huffman-01.sy`, `huffman-02.sy`, and `huffman-03.sy`, and the complete focused
  report has `clang_o3_geomean > 1.0`. This is a hard acceptance gate, not diagnostic context.
- The required `pseudo_md5`, complete `test/performance`, and current CI-parity comparisons show no
  unattributed meaningful regression. Full optimized correctness passes.
- No final performance claim uses `PERF_MAX_CASES` or an incomplete/timeout report.
- Contest legality is a hard gate: performance success cannot compensate for a benchmark-identity
  selector, hardcoded result, runtime-input specialization, weakened test, or any other forbidden
  shortcut.

Execution ownership:

- The main/coordinator agent only passes this task record and evidence between agents. Every build,
  correctness test, and performance command is run by an implementation, repair, or review
  subagent. Every performance run must invoke `scripts/compare_perf.py` exactly as recorded below.

## Non-goals

- Do not add `is_entrypoint`, `IntrinsicID`, or a general cross-layer semantic-function metadata
  system in this task.
- Do not change AST/YIR, AST-to-YIR or YIR-to-OIR propagation, dead-argument elimination's `main`
  handling, runtime mod/ref classification, either OIR-to-MIR lowerer, stack lowering, or runtime
  ABI symbol remapping. Migrating those name decisions is a separate future task.
- Do not add YIR, stack-lowerer-specific, runtime ABI, or non-`-O1` validation gates to this task.
- Do not recognize or replace the 32-iteration `_and`, `_or`, or `_xor`-style loops with a direct
  machine bit operation.
- Do not add a Huffman/decoder/helper matcher, benchmark-specific cost rule, Huffman-only SMT/CEGIS,
  or a selector based on filenames, testcase identity, function/variable/parameter names, exact
  parameter count, magic argument tuples such as `(1)`, `(2)`, or `(5)`, test inputs, runtime input
  values, or expected output.
- Do not blindly raise the ordinary inline block/cost threshold.
- Do not expand SMT, add OIR `Or`/rotate support, or change solver budgets. The first residual PE
  implementation is constant-lattice/data-flow based and must report zero solver participation.
- Do not change signed division/remainder semantics or assume signed `/ 2` and `% 2` equal shifts
  and bit extraction on negative inputs.
- Do not weaken the existing large live-pointer/mutable-memory specialization guard or its
  `pseudo_md5` regression coverage.
- Do not broaden backend work beyond the exact `MIRAddressModeCombinePass.cpp` operand-lifetime
  repair recorded below. This task does not change MIR matching semantics, profitability, pass
  order, register allocation, lowering, or assembly emission.
- Do not implement a bounded-loop superoptimizer. That is a Priority-1 future alternative requiring
  a separate task and written technical-committee confirmation.
- Do not modify `docs/README.md`, `docs/egraph-design.md`, or the independent guarded-small-loop
  task file.

## Future Separate Task

Create a separate task before changing any of the following:

- frontend semantic IDs and AST -> YIR -> OIR propagation;
- `is_entrypoint` and the DAE `main` decision;
- `IntrinsicID`, runtime mod/ref summaries, timing builtin ABI mapping;
- VReg/stack OIR-to-MIR lowerers or non-`-O1` ABI validation.

That future work may reuse the concept of semantic metadata, but this task must not pre-emptively
touch those files or broaden its tests to cover them.

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

OIR scope includes the inline/residual implementation plus the ADCE, CFG/phi, analysis,
scalar-purity, global-cleanup, ownership/use-list, and transactional body-swap safety repairs that
the detached residual path exercised. Test-infrastructure scope is limited to keeping
`scripts/oir_infra_tests.py` compatible with the authoritative function-argument construction API
and exercising those OIR ownership/use-list invariants.

MIR scope is one narrow correctness prerequisite: context-generated OIR exposed an existing
use-after-free in `MIRAddressModeCombinePass.cpp`, where the pass rewrote an instruction and then
read an operand reference owned by the replaced instruction. The allowed repair snapshots the
address register by value before replacement and uses that saved value for later use-count logic.
No other MIR/ASM production change is authorized. ASM remains downstream verification only.

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: exactly 2:
  - `/home/yoo/.codex/skills/yoolang-optimization/references/compiler-state.md`, needed for the live
    OIR pipeline, cost-model order, verifier, and downstream metric stages;
  - `/home/yoo/.codex/skills/yoolang-optimization/references/performance-workflow.md`, needed for
    isolated same-machine runs, report preservation/provenance, CI-parity scope, and timeout rules.
- Source/script anchors: at most 8 `keep=yes` files at one time; additional actual-scope files in
  the ledger stay `conditional` and are opened only for their named safety patch/review
- Focused test fixtures: at most 3 at one time; drop a completed fixture before opening another
- Large-file rule: read only named functions/queries unless this ledger is updated first

The previous Huffman task records are not required implementation context: their relevant diagnosis
and rejected alternatives are preserved below. Do not reopen them unless new evidence contradicts
this self-contained record.

Do not read unless a recorded blocker requires it:

- AST, YIR, DAE, either OIR-to-MIR lowerer, MIR/ASM implementation other than the named narrow
  `MIRAddressModeCombinePass.cpp` repair, runtime;
- OIR analysis/cleanup implementation outside the exact affected safety files and queries recorded
  in the ledger;
- SMT/e-graph implementation or polyhedral/loop-superoptimizer code;
- unrelated benchmark sources or full generated artifact directories;
- `docs/README.md` and `docs/egraph-design.md` contents beyond the protection hashes/diff gate.

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required before every resumed turn |
| `/home/yoo/.codex/skills/yoolang-optimization/references/compiler-state.md` | full | OIR pipeline/verifier/cost-model contract | yes | verified for `de93e24` |
| `/home/yoo/.codex/skills/yoolang-optimization/references/performance-workflow.md` | full | reproducible perf/report contract | yes | verified for `de93e24` |
| `src/pass/oir/OIRInlinePass.cpp` | constants, callee inspection, eligibility, clone, inline, specialization, module scan | primary behavior anchor | yes | current 12-block cap, name-derived clone lineage, coarse residual estimate |
| `src/pass/oir/OIROptimizationPipelinePass.cpp` | inline/specialization windows and cleanup helpers | bounded post-inline cleanup integration | yes | do not broaden unrelated pass ordering |
| `include/oir/OIR.h` | `Function` and `Module::create_function` | minimum OIR origin/root ID | yes | no frontend metadata propagation |
| `src/oir/OIR.cpp` | function creation plus use-list, function-set/body-swap, destruction, and verifier safety ranges | metadata plus transactional ownership/rollback substrate | yes | required by P1a and the recorded OIR safety prerequisite; do not read unrelated ranges |
| `include/pass/oir/OIRCostModel.h` | `OIRTransformCostEstimate` | actual residual metrics and risk | yes | keep estimate shape OIR-specific |
| `src/pass/oir/OIRCostModel.cpp` | estimate -> shared candidate adapter | residual/cost diagnostics | yes | no legality bypass |
| `include/pass/CostModel.h` | `PartialEvalCost`, risk/policy fields only | reuse shared cost fields/budgets | yes | do not read solver/e-graph sources |
| `src/pass/CostModel.cpp` | partial-evaluation decision/report ranges only | shared PE rejection and diagnostic behavior used by P6 | conditional | actual affected cost scope; do not read unrelated solver/e-graph ranges |
| `include/oir/OIRScalarOpt.h` | `Stats`, cleanup/growth/pressure fields | shared bounded-call state used by P3/P9 | conditional | promote only while reviewing the corresponding patch |
| `src/oir/OIRAnalysis.cpp` | trapping load/div-rem and function mod/ref ranges | preserve observable/trapping work during residual cleanup | conditional | actual affected OIR safety scope |
| `src/oir/OIRCFGUtils.cpp` | `add_edge` only | strongly exception-safe bidirectional CFG update | conditional | actual affected OIR CFG safety scope |
| `src/pass/oir/OIRADCEPass.cpp` | liveness root/removability helpers | retain operand definitions for non-removable/trapping instructions | conditional | actual affected OIR ADCE safety scope |
| `src/pass/oir/OIRCFGCleanupPass.cpp` | unreachable-block removal only | remove phi incoming references even with stale cached successor data | conditional | actual affected OIR phi/CFG safety scope |
| `src/pass/oir/OIRGlobalOptPass.cpp` | global replacement cleanup only | erase dead replaced instructions without dangling uses | conditional | actual affected OIR global safety scope |
| `src/pass/oir/OIRLocalSimplify.cpp` | short-circuit bool diamond ranges only | relocate the pure arm expression needed by residual cleanup | conditional | actual affected OIR scalar cleanup scope |
| `src/pass/oir/OIRScalarOptUtils.cpp` | `is_pure_instruction` only | loads and potentially trapping signed div/rem fail closed | conditional | actual affected OIR scalar safety scope |
| `scripts/oir_infra_tests.py` | function-argument construction fixtures | keep ownership/use-list infrastructure tests on the live API | conditional | actual affected test-infrastructure scope |
| `src/pass/mir/MIRAddressModeCombinePass.cpp` | address combine rewrite/use-count range only | five-case ASAN UAF repair required by context-generated downstream shape | yes | sole MIR production exception; no matching/profitability change |
| `scripts/compare_perf.py` | only Clang flags, report overwrite, selection env if HEAD changes | live perf behavior | conditional | promote by dropping `src/oir/OIR.cpp` after P1 |
| `.github/workflows/test.yml` | current perf dirs/exclusions only | CI-parity selection | conditional | read immediately before CI-parity runs |
| `test/performance/huffman-01.sy` | discovery source only | artifact attribution | no | never copy names/constants/shape into selectors |
| `build/perf-ci/perf-report.{json,md}` | current three rows and cost summaries | orientation evidence | no | P0 must create isolated reports |

## Starting State And Ownership

### Historical pre-commit repository state (superseded)

The 2026-07-17 independent scope review originally observed these five dirty paths before they were
committed. This block is historical provenance only; implementation agents must not use it as the
current pre-check contract.

```text
HEAD: de93e24
branch: master
git status --short:
 M docs/README.md
 M docs/tasks/README.md
?? docs/egraph-design.md
?? docs/tasks/2026-07-17-context-inline-residual-pe.md
?? docs/tasks/2026-07-17-oir-guarded-small-loop-unroll.md
```

| Path | Ownership | Allowed action in this task |
| --- | --- | --- |
| `docs/README.md` | user | none; preserve byte-for-byte and preserve tracked diff |
| `docs/tasks/README.md` | shared task index | preserve both 2026-07-17 rows; change only this task's row if its status/branch changes |
| `docs/egraph-design.md` | user, untracked | none; preserve byte-for-byte |
| `docs/tasks/2026-07-17-context-inline-residual-pe.md` | this task | update after each meaningful result |
| `docs/tasks/2026-07-17-oir-guarded-small-loop-unroll.md` | independent task-owned, byte-protected | none; preserve file byte-for-byte and preserve its Active Tasks row |

### Historical protected-path orientation (superseded)

The hashes and commands in this subsection record the pre-`c815887` state only. Do not execute them
as P0 gates. The live tracked-base contract below supersedes every `initial-*` manifest instruction
in this historical subsection.

The following hashes were recorded during task generation as orientation evidence, not as the
future implementation gate or source of truth:

```text
sha256 docs/README.md:
934789820bad682e2f8f3ba42fc357bac3328b7c78662ad42ba85d5b03831143

sha256 docs/egraph-design.md:
d3c17fe32bb09f314732e865b6a9590f72a30a38eba5df7b9a34114771321a90

sha256 docs/tasks/2026-07-17-oir-guarded-small-loop-unroll.md:
3bc7b05757a9a88f19f8b0b57b2d8e5e74d683d0796cdf5d029bfca8e68f69cf

git diff --numstat -- docs/README.md docs/egraph-design.md:
1  0  docs/README.md

sha256 of `git diff --binary -- docs/README.md`:
3c6c6359a2d3038879a40526b91df3cd677f53e9a90dfbb64d6c0c1e6d85c000
```

`docs/README.md` is tracked and its current diff is exactly one insertion. `docs/egraph-design.md`
and the independent guarded-small-loop task are untracked, so each is protected by its whole-file
SHA-256. Do not stage protected files merely to make their diffs easier to compare.

P0's first action, before any branch checkout, must generate this task-specific initial evidence
from the bytes then present. Future verification reads these files; it must not compare against the
orientation hashes embedded in this document.

```bash
mkdir -p build/perf-ci/context-inline-residual-pe/protection
sha256sum docs/README.md docs/egraph-design.md \
  docs/tasks/2026-07-17-oir-guarded-small-loop-unroll.md \
  > build/perf-ci/context-inline-residual-pe/protection/initial-sha256.txt
git diff --binary -- docs/README.md | sha256sum \
  > build/perf-ci/context-inline-residual-pe/protection/initial-docs-readme-diff.sha256
git diff --numstat -- docs/README.md docs/egraph-design.md \
  > build/perf-ci/context-inline-residual-pe/protection/initial-docs-readme-numstat.txt
git status --short \
  > build/perf-ci/context-inline-residual-pe/protection/initial-status.txt
awk 'index($0, "docs/tasks/2026-07-17-context-inline-residual-pe.md") == 0' \
  docs/tasks/README.md \
  > build/perf-ci/context-inline-residual-pe/protection/initial-task-index-excluding-this-row.txt
```

The immutable byte-protected set is exactly `docs/README.md`, `docs/egraph-design.md`, and
`docs/tasks/2026-07-17-oir-guarded-small-loop-unroll.md`; all three receive initial and final
closure through `initial-sha256.txt`. The shared `docs/tasks/README.md` is intentionally mutable
only at this task's row, so the filtered snapshot closes every other byte, including the independent
task's row. This task file is task-owned and intentionally mutable. The tracked user diff, its
numstat, and the five-path pre-checkout status receive their own initial evidence as shown above.

Immediately after creating the files, run the manifest-driven pre-check commands in the Verification
Matrix. If current ownership/status does not match the five-path table, reconcile it in this task
before checkout and obtain user direction when ownership is ambiguous; never restore or overwrite a
protected path. Immediately after checkout and before `ready_for_review`, verification must use
`sha256sum -c` and `cmp` against the P0 evidence, never the hardcoded orientation values.

The preceding paragraph is also historical and must not be executed for the current task base.

### Live implementation-start contract

The 2026-07-17 audit began on clean `huff@c815887`; `master` and `origin/master` also pointed at
`c815887`. Commit `c815887` contains only the five documentation paths listed above relative to
`de93e24`; it is the implementation/branch base. This audit modifies this task record, and the
independent audit modifies the guarded-small-loop task record. Those two task-owned documentation
diffs are authorized starting state, not production changes. The isolated same-machine
compiler-performance baseline remains `de93e24`, whose production compiler and scripts are
identical to `c815887`.

Before implementation, a subagent must verify `HEAD == c815887`, classify every dirty path, and
confirm that only the two audited task records are dirty. If another task agent has changed the
shared checkout, coordinate an isolated task worktree or wait; do not switch the shared branch out
from under another agent. The protected set is:

- `docs/README.md`;
- `docs/egraph-design.md`;
- `docs/tasks/2026-07-17-oir-guarded-small-loop-unroll.md`;
- every byte of `docs/tasks/README.md` except this task's own row.

P0 snapshots the independent audit's actual bytes and the shared index after both audit agents have
finished. This task's own record is intentionally mutable:

```bash
mkdir -p build/perf-ci/context-inline-residual-pe/protection
git rev-parse HEAD
git status --short
git diff --exit-code c815887 -- docs/README.md docs/egraph-design.md
sha256sum docs/tasks/2026-07-17-oir-guarded-small-loop-unroll.md \
  > build/perf-ci/context-inline-residual-pe/protection/implementation-start-other-task.sha256
awk 'index($0, "docs/tasks/2026-07-17-context-inline-residual-pe.md") == 0' \
  docs/tasks/README.md \
  > build/perf-ci/context-inline-residual-pe/protection/implementation-start-task-index-excluding-this-row.txt
sha256sum -c \
  build/perf-ci/context-inline-residual-pe/protection/implementation-start-other-task.sha256
awk 'index($0, "docs/tasks/2026-07-17-context-inline-residual-pe.md") == 0' \
  docs/tasks/README.md \
  | cmp - build/perf-ci/context-inline-residual-pe/protection/implementation-start-task-index-excluding-this-row.txt
```

The same base-doc diff, other-task SHA-256, and filtered-index `cmp` are required immediately after
branch/worktree creation and before review. Never restore, reset, clean, overwrite, stage, or commit
a protected path as a shortcut.

## Preserved Performance Diagnosis

The existing `build/perf-ci/perf-report.json` was generated on 2026-07-16 with the compiler in this
`de93e24` checkout, `test_dirs=["test/performance"]`, 60 cases, and zero failures. It is orientation
evidence only; P0 must reproduce an isolated same-machine baseline.

| Case | GCC `-O3` | Clang 22 `-O3` | Yoolang `-O1` | Slower than GCC | Slower than Clang | Final MIR | Spills |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `test/performance/huffman-01.sy` | 0.0524s | 0.0341s | 0.0638s | 21.8% | 87.1% | 458 | 0 |
| `test/performance/huffman-02.sy` | 0.0520s | 0.0347s | 0.0660s | 26.9% | 90.2% | 458 | 0 |
| `test/performance/huffman-03.sy` | 0.0519s | 0.0340s | 0.0656s | 26.4% | 92.9% | 458 | 0 |

The three cases are about 22-27% slower than GCC and 87-93% slower than Clang. Every row reports 12
constant-argument specialization candidates rejected as `CodeGrowthTooHigh`. Current PE accounting
uses the original/stale callee plus a coarse constant-count discount instead of substituting into a
scratch body, cleaning it, and charging the real residual.

Current inliner failure chain:

- ordinary inlining has `kMaxCalleeBlocks = 12` plus resource/cost checks;
- `inline_functions` scans module functions for a few broad rounds without caller-context or CGSCC
  prioritization;
- a callee can absorb nested helpers and expand before a hot outer caller is considered, then fail
  the 12-block eligibility check at that outer callsite;
- newly exposed calls are rediscovered only by broad rescans, without explicit inherited loop depth,
  constant operands, dead-return demand, or estimated dynamic call facts;
- cleanup is batch-oriented rather than coupled to each newly exposed caller context.

Clang 22.1.8 does not replace the entire 32-trip signed-arithmetic helper loop with one bit
operation. Its faster shape comes from decoder -> entry-function context, nested inlining, constant
propagation, dead-return slicing/DCE, global scalar promotion, LICM, and GVN. Final Yoolang spill
count is already zero, so this is not primarily a register-allocation fix.

## P0 Auditable Clang 22 Reproduction

P0 must save a task-specific, reproducible Clang comparison under:

```text
build/perf-ci/context-inline-residual-pe/diagnosis/clang22/
  tool-version.txt
  compile-command.txt
  source-sha256.txt
  huffman-01.baseline.cc
  huffman-01.clang.ll
  huffman-01.clang.s
  huffman-01.opt.yaml
  huffman-01.remarks.txt
  huffman-01.remarks.o
  inspection.md
```

Required procedure:

1. Run the focused `de93e24` performance baseline first so
   `build/perf-ci/test/performance/huffman-01/huffman-01.baseline.cc` and the runtime wrapper are
   generated by the live comparison script.
2. Record `clang++ --version`, resolved `RISCV_CLANGXX`, sysroot, target, runtime wrapper, and the
   exact flags. Copy the generated `baseline.cc`; record SHA-256 for all three Huffman `.sy` and
   generated baseline sources. If their hashes differ, emit the full artifact set for each file.
3. Reproduce the live flags:

```bash
clang++ -O3 -mcmodel=medany -std=gnu++17 --target=riscv64-linux-gnu \
  --sysroot=/usr/riscv64-linux-gnu \
  -include <de93e24-worktree>/build/perf-ci/runtime/sylib_wrapper.hpp \
  -x c++ huffman-01.baseline.cc -S -emit-llvm -o huffman-01.clang.ll

clang++ -O3 -mcmodel=medany -std=gnu++17 --target=riscv64-linux-gnu \
  --sysroot=/usr/riscv64-linux-gnu \
  -include <de93e24-worktree>/build/perf-ci/runtime/sylib_wrapper.hpp \
  -x c++ huffman-01.baseline.cc -S -o huffman-01.clang.s

clang++ -O3 -mcmodel=medany -std=gnu++17 --target=riscv64-linux-gnu \
  --sysroot=/usr/riscv64-linux-gnu \
  -include <de93e24-worktree>/build/perf-ci/runtime/sylib_wrapper.hpp \
  -x c++ huffman-01.baseline.cc -c -o huffman-01.remarks.o \
  -fsave-optimization-record -foptimization-record-file=huffman-01.opt.yaml \
  -Rpass=inline -Rpass-missed=inline -Rpass-analysis=inline
```

Use `clang++ ... -###` with the same arguments to save `compile-command.txt`; capture the final
command's diagnostics as `huffman-01.remarks.txt`.

`inspection.md` must cite artifact locations and demonstrate all of the following:

- standalone helper definitions remain present;
- the signed-arithmetic helper retains a loop backedge and a 32-trip induction/count condition;
- `and`/`or`/`xor` instructions inside the loop implement per-iteration predicates and do not mean
  the whole loop became one machine bit operation;
- the decoder call is absent from the hot entry loop and nested read/state work is exposed there;
- optimization remarks/LLVM IR/assembly support the normal inline/constant/DCE/global/LICM/GVN
  explanation, with inference explicitly labeled.

These artifacts are diagnosis only. No helper name, loop count, remark text, source hash, or Clang
shape may enter a production selector or cost bonus.

## Branch

Decision: use `task/context-inline-residual-pe`, based on `c815887`.

Because the two requested implementations use separate subagents but share the repository, the
coordinator must not allow concurrent branch switching in one checkout. Either run the two task
agents sequentially in the shared checkout or give this agent an isolated worktree rooted at
`c815887` and seed it with this audited task record. The implementation agent, not the coordinator,
performs the branch/worktree commands.

Safe sequence:

```bash
git status --short
test "$(git rev-parse HEAD)" = "$(git rev-parse c815887)"
git branch --show-current
git diff --exit-code c815887 -- docs/README.md docs/egraph-design.md
sha256sum -c build/perf-ci/context-inline-residual-pe/protection/implementation-start-other-task.sha256
awk 'index($0, "docs/tasks/2026-07-17-context-inline-residual-pe.md") == 0' \
  docs/tasks/README.md \
  | cmp - build/perf-ci/context-inline-residual-pe/protection/implementation-start-task-index-excluding-this-row.txt
test "$(rg -cF 'docs/tasks/2026-07-17-context-inline-residual-pe.md' docs/tasks/README.md)" -eq 1
git checkout -b task/context-inline-residual-pe
git status --short
git branch --show-current
git diff --exit-code c815887 -- docs/README.md docs/egraph-design.md
sha256sum -c build/perf-ci/context-inline-residual-pe/protection/implementation-start-other-task.sha256
awk 'index($0, "docs/tasks/2026-07-17-context-inline-residual-pe.md") == 0' \
  docs/tasks/README.md \
  | cmp - build/perf-ci/context-inline-residual-pe/protection/implementation-start-task-index-excluding-this-row.txt
test "$(rg -cF 'docs/tasks/2026-07-17-context-inline-residual-pe.md' docs/tasks/README.md)" -eq 1
```

After checkout, the branch must be `task/context-inline-residual-pe`, HEAD must still be `c815887`,
only the two authorized audited task-record diffs may precede implementation, both Active Tasks rows
must remain, and the tracked-base/SHA/index checks must pass. If the branch already exists, verify
that its merge base with `c815887` is `c815887` and inspect its current diff before entering it; do
not recreate, reset, or overwrite it.

Never stash, reset, clean, restore, stage, commit, or overwrite protected paths as a branching
shortcut. Production commits must exclude them unless their owner separately authorizes and scopes
that action. This task record and its own Active Tasks row remain task-owned and may be updated.

## Invariants And Risks

Correctness invariants:

- Inlining and residualization preserve return values, memory effects, traps, ordering, and
  termination for every valid SysY input.
- The worklist orders compiler decisions only; it never reorders runtime execution.
- Every popped callsite is revalidated against current caller, callee, operands, CFG, loop facts,
  return demand, SCC relation, mod/ref, and cumulative budgets.
- Recursive and mutually recursive SCCs remain governed by existing bounded recursive legality and
  growth rules; context bonuses never bypass them.
- `FunctionOrigin` has only the minimal OIR meaning needed here (ordinary/original versus residual
  specialization). Every OIR function has a stable function ID; an original's root ID is itself and
  a residual clone inherits the original root ID. Names do not establish origin or lineage.
- Existing external/entry/runtime semantics are unchanged; this task does not reinterpret those
  functions or migrate their name-based logic.
- A dead return permits removal only of pure value-computation slices. Calls, stores, traps, and
  memory operations with uncertain effects remain unless independently proven removable.
- The constant lattice distinguishes at least `Unknown`, exact typed `Constant`, and `Overdefined`.
  Folding uses existing OIR signed/float semantics and fails closed on unsupported operations.
- Scratch cloning owns all speculative blocks, values, function-table entries, name allocation,
  stats, and growth accounting separately from the live module.
- OIR operand/use-list, function-set/body-swap, CFG predecessor/successor, and phi incoming updates
  are strongly exception safe: an allocation or failed transactional preflight cannot leave a
  dangling/cross-owner operand or one-sided edge.
- A retained load or a potentially trapping signed division/remainder remains an ADCE liveness
  root with all operand definitions live; residual cleanup may erase it only after the existing
  analysis proves it removable and nontrapping.
- Profitability sees actual verified residual metrics after substitution, return-demand slicing,
  and bounded cleanup; it does not infer eliminated instructions from constant-argument count.
- Direct callsite residual commit is preferred. A persistent clone is allowed only when measured
  reuse covers its cumulative code cost; it carries `FunctionOrigin::ResidualSpecialization` and
  the stable root ID.
- Immediate cleanup is bounded. It runs only when an inline changed a caller, records every function
  actually changed by the cleanup window, verifies the module, invalidates stale analyses, and
  requeues only affected/new callsites.
- If live immediate cleanup reaches its cap, stop the window, verify/rebuild facts, and stop making
  profitability decisions that assumed additional cleanup. Scratch PE cleanup cap exhaustion is a
  rejection and leaves the live module unchanged.
- Global/per-root code growth, persistent clone fanout, live pointers, alias uncertainty, loads,
  stores, maximum live values, memory pressure, and spill proxies are charged cumulatively.
- MIR address-mode combine never reads an operand reference after replacing its owning instruction;
  the saved register value is observationally identical and does not broaden the combine matcher.

Transactional rollback invariant:

- Before scratch evaluation, snapshot live function/block/instruction counts and identities,
  function-table mappings, operand/use counts, module verification result, next clone/name state,
  `Stats` commit counters, and cumulative growth/pressure budgets.
- On cost rejection, cleanup cap exhaustion, unsupported/error, verification failure, or commit
  preflight failure, all snapshot fields must compare equal afterward. The only allowed change is a
  rejected cost diagnostic with a precise reason.
- A following accepted candidate must receive the same clone ID/name and remaining budget as in a
  control module where the failed candidate never existed.

Tie and global-budget invariant:

- Candidate score and hard legality never use function names, module insertion order, allocation
  addresses, or mutable numeric `FunctionID` ordering.
- Sort distinct candidates by descending score then a canonical structural key built from
  normalized SCC topology, canonical callsite facts, normalized callee/caller shape, and
  intra-function instruction position. Revalidate immediately before every commit.
- Candidates with the same structural key form a tie batch. Preflight the combined residual growth:
  if the full still-valid batch fits, accept all; if it does not fit, reject the whole batch as
  `CumulativeBudgetExhausted`. Never let module order choose an arbitrary subset.
- For distinct structural keys, commit one revalidated candidate at a time; later candidates that
  no longer fit are rejected with the recorded remaining budget. Alpha rename, definition reorder,
  and equivalent-CFG permutations must preserve accepted structural fingerprints and totals.

Signed arithmetic invariant:

- Signed division truncates toward zero and remainder follows that quotient. In the motivating
  loop, `_and(-1, 1)` evaluates to `0` because `-1 % 2 == -1`, while machine `-1 & 1` is `1`.
  `_or(-1, 0)` yields `0` while machine result is `-1`; `_xor(-1, 1)` yields `1` while machine result
  is `-2`. These mandatory negative tests rule out a whole-loop bitop rewrite.

Contest/compliance constraints:

- No filename, benchmark, test, function/variable/parameter name, exact arity, magic argument tuple,
  known input, expected output, or source hash may affect production behavior.
- General context facts may include loop depth, exact typed constants, return demand, SCC relation,
  side effects/mod-ref, measured residual cost, and a saturating dynamic-call estimate derived only
  from IR call/loop structure.
- SMT or PE cannot legalize an illegally selected candidate. The first version uses no SMT;
  `Refuted`, `Timeout`, and `Unknown` remain rejection.
- GCC/Clang/Huffman artifacts are evidence only and are never read by the compiler.

Risk areas:

- dangling callsite handles or stale SCC/loop facts after inline/cleanup;
- compile-time explosion from cleanup/requeue cycles;
- detached scratch references to live functions/globals and phi/use-list correctness;
- a scratch or failed commit leaking a function-table/name allocator entry;
- code-size/locality wins hiding later MIR live-range/spill regressions;
- global scalar promotion across an uncertain call/alias;
- report overwrite, mismatched binaries/runtime, timeout, or stale artifacts corrupting comparison.

## Patch Queue

Every row is one executable behavior point. Each source patch is limited to at most about three
production files and 300 net production lines; test files may accompany it. If the row cannot meet
that bound, split it in this table before editing. Update Status/Notes and the Change Log after each
row.

| Patch | Intent | Production files (max 3) | Required gate/command | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P0 | Verify/classify tracked base `c815887`, snapshot the independent audited task and filtered task index, then freeze all required `de93e24` compiler-baseline reports/provenance and create the auditable Clang 22 artifact set | none | Tracked-base protection, baseline build/perf, and Clang rows in Verification Matrix | completed | Protection/branch gates passed. Release baseline and all four required `compare_perf.py` scopes passed and were frozen with provenance; Clang 22 artifact set and inspection are complete. No `PERF_MAX_CASES`. |
| P1a | Add minimal OIR `FunctionOrigin`, stable function ID, and stable root ID with default-original creation/copy APIs | `include/oir/OIR.h`, `src/oir/OIR.cpp` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_function_origin --jobs 1` | completed | Module creation assigns monotonic stable IDs; originals root to self; detached template copies identity; no AST/YIR/entrypoint/intrinsic metadata. Build and focused FileCheck pass. |
| P1b | Replace `__yo_constprop.*` prefix decisions/counting with origin/root lineage while retaining generated names only for display/collision avoidance | `src/pass/oir/OIRInlinePass.cpp` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_function_origin --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_function_origin --jobs 1 --o1` | completed | Eligibility/counting now use `FunctionOrigin::ResidualSpecialization` plus root ID; generated names are display/collision-only. Build, 1 FileCheck, and 1 OIR stage case pass. |
| P2 | Add lightweight direct-call graph/SCC context and deterministic worklist with tie-batch/global-budget policy | `src/pass/oir/OIRInlinePass.cpp`, optional `include/pass/oir/OIRInlineWorklist.h`, optional `src/pass/oir/OIRInlineWorklist.cpp` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_worklist --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_context_inline_worklist --jobs 1 --o1` | in_progress | Canonical CFG/data-flow refinement now supplies a normalized intra-function call position and stable `callsite.<hash>` audit fingerprint without names, IDs, addresses, block labels, module/block insertion, or successor-container order. Same-block structurally equivalent calls retain atomic tie semantics. Focused release/ASAN gates pass; fresh independent review and broad closure remain. |
| P3 | Add bounded affected cleanup after an accepted inline and requeue every function/callsite actually changed | `src/pass/oir/OIRInlinePass.cpp`, `src/pass/oir/OIROptimizationPipelinePass.cpp`, optional one cleanup helper | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_cleanup_budget --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_context_inline_cleanup_budget --jobs 1 --o1` | in_progress | Three-round verified module cleanup, exposed-call retention, and exhausted-cap diagnostic/decision stop are present; the cleanup FileCheck passes within common 10/10. Keep open for independent review and broad affected-function/full-suite closure. |
| P4 | Build a detached scratch clone and substitute exact constant-lattice arguments without live-module ownership | `src/pass/oir/OIRInlinePass.cpp`, optional `include/pass/oir/OIRResidualPE.h`, optional `src/pass/oir/OIRResidualPE.cpp` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_clone --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_context_inline_residual_pe_clone --jobs 1 --o1` | in_progress | Detached module/function/type/global/function-shadow cloning, exact constant substitution, and unsupported/error rollback gates pass within common 10/10. Keep open for broad ownership/sanitizer closure. |
| P4a | Make the detached/live OIR ownership substrate strongly exception-safe, including use lists, function sets/body swap, destruction, verifier ownership checks, and bidirectional CFG edge insertion | `include/oir/OIR.h`, `src/oir/OIR.cpp`, `src/oir/OIRCFGUtils.cpp` | `python3 scripts/run_tests.py --suite infra --jobs 1`; P4/P8 clone and rollback gates | in_progress | Named clone/ownership/rollback fixtures now pass, and post-repair release OIR infra passes 1/1. Keep open for the exact debug+ASAN reproducer and final sanitizer/full-suite closure. |
| P5 | Add explicit scalar `ReturnDemandMask` and side-effect-aware dead-return slicing on scratch IR | residual helper/header, `src/pass/oir/OIRInlinePass.cpp` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_return_demand --jobs 1`; `python3 scripts/run_tests.py --suite e2e --filter context_inline_residual_pe_return_demand --jobs 1 --o1` | in_progress | Dead/scalar forms, demanded return and visible-side-effect slicing pass the focused FileCheck and common residual e2e. Generic fixed-point positives/negatives also pass; keep open for broader alias/side-effect and full-suite closure. |
| P5a | Preserve observable/trapping loads and signed div/rem through mod/ref, scalar purity, and ADCE liveness | `src/oir/OIRAnalysis.cpp`, `src/pass/oir/OIRADCEPass.cpp`, `src/pass/oir/OIRScalarOptUtils.cpp` | fixed-point/return-demand FileChecks and e2e; `python3 scripts/run_tests.py --suite infra --jobs 1` | in_progress | Actual OIR safety scope. Existing fixed-point positives/negatives cover part of it; the full P5 return-demand/side-effect fixture remains open. |
| P5b | Repair generated residual cleanup shapes: remove stale phi incoming operands, erase dead global replacements, and relocate a pure short-circuit arm value without duplication | `src/pass/oir/OIRCFGCleanupPass.cpp`, `src/pass/oir/OIRGlobalOptPass.cpp`, `src/pass/oir/OIRLocalSimplify.cpp` | cleanup/return-demand FileChecks; focused OIR/e2e; `python3 scripts/run_tests.py --suite infra --jobs 1` | in_progress | Actual OIR CFG/phi/global/scalar cleanup scope; dedicated P3/P5 fixtures and final broad gates remain open. |
| P6 | Measure verified post-cleanup residual metrics and integrate them into the OIR cost request/rejection diagnostic | `src/pass/oir/OIRInlinePass.cpp`, `include/pass/oir/OIRCostModel.h`, `src/pass/oir/OIRCostModel.cpp` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter cost_model_pe --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_cost --jobs 1` | completed | Measured detached residual metrics and zero-SMT assertions pass. `cost_model_pe` again proves an accepted specialization followed by a `CodeGrowthTooHigh` rejection with the measured direct-residual summary; both focused FileChecks pass. |
| P7 | Implement atomic direct residual commit and measured persistent-clone reuse semantics | `src/pass/oir/OIRInlinePass.cpp`, residual helper/header | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_commit --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_context_inline_residual_pe_commit --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e --filter context_inline_residual_pe_commit --jobs 1 --o1` | in_progress | The commit fixture now proves two independent `measured-persistent-reuse-member` cost decisions followed by one aggregate reusable-clone decision. Direct and persistent publication remain atomic; keep open for fresh independent review and broad/full-suite closure. |
| P8 | Prove rollback for cost reject, scratch cleanup budget exhaustion, unsupported/error, verifier failure, and commit-preflight failure | `src/pass/oir/OIRInlinePass.cpp`, residual helper/header | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_rollback --jobs 1`; `python3 scripts/run_tests.py --suite e2e --filter context_inline_residual_pe_rollback --jobs 1 --o1` | completed | Dedicated failpoints cover every named failure and subsequent control acceptance. The exact snapshot now copies and compares the complete live `Module` function-table name-to-`Function*` mapping in addition to object/use identity, allocators, stats, growth, and pressure; rollback and common FileChecks pass under release and debug+ASAN. |
| P9 | Add cumulative module/root growth and live-pointer/memory/register/spill-pressure limits to inline and residual commit | `src/pass/oir/OIRInlinePass.cpp`, `include/pass/oir/OIRCostModel.h`, at most one shared cost header | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_pressure --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --suite e2e --filter crypto- --jobs 1 --o1` | in_progress | Combined direct-tie module/root growth preflight is covered by the persistent `tie-budget` rejection fixture, and the generic large mutable-pointer pressure FileCheck passes. Keep open for required crypto MIR/ASM/e2e and performance preservation gates. |
| P10 | Add the complete generic metamorphic/signed/alias/SCC test battery | no production files | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --stage mir --stage asm --filter context_inline --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e --filter context_inline_residual_pe --jobs 1 --o1` | in_progress | Common FileCheck now passes 12/12. New paired definition-reorder and CFG/block/successor-container permutation fixtures compare accepted structural-fingerprint multisets, reject-reason multisets, and four consumed risk/growth budget totals in normal and forced tie-budget modes. Context OIR/MIR/ASM/e2e passes 8/8; full-suite closure remains. |
| P10a | Keep the standalone OIR infrastructure fixtures on the live function-argument API so ownership/use-list checks exercise valid construction | `scripts/oir_infra_tests.py` | `python3 scripts/run_tests.py --binary build/linux/x86_64/release/compiler --suite infra --jobs 1 --infra-timeout 60` | in_progress | Post-blocker repair release rerun passes 1/1 in 3.35s. Keep open until the final full optimized suite reruns this fixture in context. |
| P10b | Repair the downstream address-mode-combine operand-lifetime UAF exposed by context-generated OIR | `src/pass/mir/MIRAddressModeCombinePass.cpp` | exact five-case debug+ASAN reproducer command must be recorded/rerun; focused MIR/ASM/e2e; final broad gates | in_progress | Independent re-review rebuilt the current debug+ASAN compiler and passed the explicit five-file MIR reproducer 5/5 (three Huffman plus both `context_inline_*` functional cases). The exact command is now recorded in the Verification Matrix. Keep open only for the still-required final broad sanitizer/correctness closure. |
| P11 | Run full correctness and every required sequential baseline/current performance row, preserve reports, generate four explicit deltas, inspect results, and update task | none | complete Verification Matrix | pending | Required timeout/failure keeps task out of `ready_for_review`. |

## Pre-Performance Independent Review

Disposition: **PRE-PERF BLOCKED pending canonical-key/metamorphic repair and fresh independent
review. Do not run `compare_perf.py`.**
The fresh 2026-07-21 independent re-review accepts the two immediately preceding repairs but keeps
performance blocked on two older task invariants that are still not implemented/proven:

- The persistent exact-tie path now independently revalidates every member's eligibility, mask,
  typed binding and live callee, builds and verifies a fresh detached residual, measures residual
  metrics and pressure, and obtains a per-member cost decision before the aggregate reusable-clone
  cost decision.  A member rejection truncates tentative member diagnostics and emits one atomic
  batch rejection; publication retargets every call before charging the one-clone growth/root
  budget, and the enclosing staged-module transaction covers an exceptional post-retarget failure.
- The rollback repair explicitly copies and compares every live function-table name-to-pointer
  mapping in `InlineLiveModuleSnapshot`.  Unsupported, cleanup-budget, detached-verifier,
  typed-import, cost, commit-preflight and commit-after-first failpoints all execute that assertion
  and demonstrate a later accepted control candidate.  The staged cleanup-publication failure also
  restores the complete prepared function set/table and retains its existing control IR checks.
- **Blocker:** `structural_callsite_key` still has no canonical intra-function instruction-position
  component.  It contains SCC/caller/callee shapes, loop/dead-return facts and positional typed
  argument bindings, but two otherwise identical calls at different program positions receive the
  same key.  The current same-block fixture relies on this collapse.  This does not satisfy the
  recorded invariant requiring normalized intra-function instruction position in the key.
- **Blocker:** the focused battery contains alpha-renamed structural pairs, SCC and same-block tie
  coverage, but no paired function-definition-reordered input and no paired equivalent-CFG/block
  permutation that compare accepted structural fingerprints, rejection reasons and consumed
  budgets.  Passing the existing fixtures therefore is not sufficient evidence for the two
  mandatory metamorphic invariants.

Independent commands on the current dirty source passed: release build; context-inline FileCheck
10/10; `cost_model_pe` 1/1; OIR infra 1/1; context OIR/MIR/ASM/e2e 8/8; Huffman
OIR/MIR/ASM/e2e 12/12; crypto OIR/MIR/ASM/e2e 12/12; debug+ASAN build; debug+ASAN
context-inline FileCheck 10/10; debug+ASAN `cost_model_pe` 1/1; and the exact five-file debug+ASAN
MIR reproducer 5/5.  Protected bytes and the correctly filtered task index still match the recovery
manifest.  No `compare_perf.py` command ran.

The 2026-07-21 independent re-review found these two blockers in the preceding implementation:

- `specialize_constant_argument_calls_impl` forms `exact_tie_calls`, but evaluates
  `measured_persistent_reuse` before entering the per-member direct-tie path. That reuse path uses
  the first site's scratch residual and estimate for the full exact tie, aggregates pressure, makes
  one `.reuse` cost decision, retargets every call, and returns. It therefore does not freshly
  residualize, measure, revalidate, and cost-check every exact-tie member as required. The focused
  commit fixture explicitly exercises this path and asserts `measured-persistent-reuse` while
  excluding `direct-residual-tie-batch`.
- The rollback assertion captures printed IR, live object/use identity, function/block allocator
  state, commit counters, cumulative growth, root maps, and pressure, but does not explicitly
  capture or compare `Module` function-table mappings even though the rollback invariant names
  those mappings. This is a coverage gap to close with the persistent-reuse repair.

The current repair removes the early persistent-reuse return. It revalidates eligibility, mask and
typed binding for every live exact-tie member; builds and verifies a fresh detached residual for
each; independently measures its residual, pressure and cost; and only then makes the one-clone
reuse savings/budget/cost decision and atomically retargets the whole batch. The commit fixture now
requires two `measured-persistent-reuse-member` accepts before the aggregate reuse accept. The live
rollback snapshot also copies and compares the complete function-table name-to-`Function*` map,
and every named failpoint diagnostic plus later control acceptance passes that assertion.

The repair agent's release, debug+ASAN, infra, context, Huffman and crypto focused gates pass as
recorded below. This is implementation evidence, not an independent approval: performance remains
blocked until a fresh reviewer inspects this repair and explicitly changes the disposition.

The preceding implementation checkpoint had reported these repaired behaviors:

- canonical call/specialization keys encode every formal argument position, selected typed
  constant, and unknown/unselected placeholder without sorting bindings;
- every direct-tie member is revalidated, residualized, measured, pressure-accounted, and submitted
  to the cost model independently before an atomic batch decision;
- same-basic-block members are grouped and reverse-split by their original program position, with
  focused all-accept and persistent combined-budget all-reject coverage;
- `cost_model_pe` again demonstrates `CodeGrowthTooHigh` after a prior accepted measured residual;
  and
- named rollback failpoints cover cost, cleanup, unsupported/error, detached verification, typed
  import, commit preflight, failure after the first live split, and staged-cleanup publication,
  with exact live-module/use/allocator plus commit-stat/growth/pressure snapshot assertions.

Focused repair gates pass as recorded below. This checkpoint does not itself release the
performance gate: a separate reviewer must inspect the repair and explicitly change the
disposition before any `compare_perf.py` invocation. Remaining broad P2/P3/P4/P5/P7/P9/P10/P11
requirements and final sanitizer/full-suite gates retain their recorded status.

## Required Cleanup And PE Behavior

### Bounded affected cleanup

- Seed the affected set with the caller and its SCC after each accepted inline.
- Run at most the documented cleanup-window and cleanup-round caps. Reuse existing OIR cleanup APIs;
  do not reimplement all scalar passes in one oversized patch.
- If a reused module-wide pass changes another function, add that function to the affected set and
  requeue its current direct callsites. Do not claim function-local cleanup when the API is global.
- Required order is constant folding/SCCP, branch simplification, CFG cleanup, DCE/ADCE, VRP, GVN,
  and safe global scalar promotion, with OIR verification and analysis invalidation between rounds
  where current APIs require it.
- When the live cleanup cap is reached, emit `CleanupBudgetExhausted`, verify/rebuild current facts,
  and do not award any further cleanup-dependent context bonus.

### Worklist/tie-budget procedure

```text
build direct call graph and SCCs
  -> calculate general callsite facts and canonical structural key
  -> group identical structural keys into tie batches
  -> order distinct keys by score and canonical key
  -> revalidate candidate/batch against live IR and cumulative budgets
  -> accept all members of a fitting tie batch or reject the full batch
  -> inline, bounded cleanup, rebuild affected facts, enqueue newly exposed calls
  -> repeat until queue/inline/cleanup/growth caps stop the window
```

The canonical key must normalize block labels and exclude function names, module order, allocation
addresses, and mutable ID ordering. Metamorphic tests compare accepted structural fingerprints,
reject reasons, and total consumed budget, not generated symbol spelling.

### Transactional residual PE procedure

```text
revalidate direct callsite and legality
  -> snapshot live module/use/name/stats/budgets
  -> create detached scratch module/function and shadow references
  -> seed Constant/Unknown/Overdefined lattice from typed call operands
  -> substitute and apply ReturnDemandMask
  -> run bounded scratch cleanup to fixed point
  -> verify scratch and measure actual residual/risk
  -> cost decision plus cumulative/tie-batch preflight
  -> reject/error: discard scratch and compare snapshot unchanged
  -> accept: preflight an atomic direct commit or persistent residual clone
  -> commit, verify live OIR, update stats/budgets once, rebuild/requeue facts
```

No scratch function may be inserted into the live module merely to run cleanup. If current cleanup
APIs require a `Module`, construct a detached scratch module with shadow declarations/references.
Reject rather than borrowing live ownership or leaving temporary names in the live function table.

## Required Test Battery

Focused tests must cover:

- alpha-renamed copies, function-definition reorderings, and equivalent CFG/canonical block-label
  permutations with identical structural accept/reject/budget results;
- identical tie batches that either all fit or all reject when the global budget cannot fit every
  member, plus distinct-key candidates that revalidate against remaining budget;
- same spelling with different OIR origins/semantics so clone lineage never comes from name;
- constant and nonconstant arguments; typed integer/float constants; `Unknown`/`Overdefined` paths;
- dead and demanded returns, including side effects needed only along the return slice;
- direct recursion, mutual SCCs, external calls, newly exposed recursive calls, and queue caps;
- calls/stores/global clobbers, escaped pointers, may-alias loads/stores, and safe/unsafe global
  promotion;
- cost rejection, scratch cleanup budget exhaustion, unsupported scratch instruction/error,
  scratch verification failure, commit preflight failure, and a subsequent control acceptance;
- unchanged live object/use/function-table/name/stats/growth snapshots on every rejection path;
- negative, zero, positive, `INT_MIN`, and `INT_MAX` signed `/2` and `%2`, including the
  `_and(-1, 1) == 0` counterexample under generic test names;
- exact-trip 31/32/33 and near-matching loops that are not changed into a whole-loop bit operation;
- zero SMT queries in all first-version residual PE cost records;
- a generic large pointer/mutable-memory helper rejected for pressure and the explicit
  `pseudo_md5` performance group's MIR/clone/spill/stack/runtime preservation.

## Verification Matrix

### Correctness, Scope, And Protection

Normative unexecuted gates remain `NOT_RUN`; interim evidence never substitutes for them. The
post-review implementation repair ran only the focused build/correctness gates updated below; it
ran no sanitizer or performance command and does not supersede the older reported evidence.

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Pre-check tracked base/ownership | `git status --short`; verify `git rev-parse HEAD` equals `c815887`; inspect `git branch --show-current` and every dirty path | yes | PASS | `huff@c815887`; exactly the two audited task records were dirty before branch creation. |
| P0 independent-task/index evidence | generate `implementation-start-other-task.sha256` and `implementation-start-task-index-excluding-this-row.txt` from current audited bytes using the exact live-contract commands | yes, after both audits and before checkout/worktree creation | PASS | Generated from the completed audits before checkout; immediate SHA/index checks passed. |
| Pre-check protected docs/task/index | `git diff --exit-code c815887 -- docs/README.md docs/egraph-design.md`; run the independent-task `sha256sum -c` and filtered-index `cmp` | yes | PASS | Base-doc diff empty; independent-task SHA and filtered-index comparison passed. |
| Post-checkout/worktree state | rerun the base-doc diff, other-task SHA-256, and filtered-index `cmp` from the Branch safe sequence | yes | PASS | Created `task/context-inline-residual-pe` at `c815887`; protected checks and single-row count passed. |
| P0 Clang 22 diagnosis | run all commands in `P0 Auditable Clang 22 Reproduction` and save every listed artifact | yes before source edits | PASS | Clang 22.1.8 LL/ASM/remarks/opt-record/driver/source hashes saved. Inspection confirms standalone 32-trip helper backedges and nested decoder/read-state inlining, not a whole-loop bitop. |
| Build | `xmake f -m release && xmake` | yes after each source patch | PASS (canonical repair) | Current repair release and debug+ASAN builds passed; final release configuration was restored after sanitizer checks. |
| Origin FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_function_origin --jobs 1` | yes | PASS | Recovery rerun: 1/1 FileCheck; OIR/MIR/ASM stage plus e2e 4/4. Cost assertion now records the detached demanded-scalar residual summary. |
| Fixed-point suffix FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_fixed_point_suffix --jobs 1` | yes for the current residual cleanup | PASS | 1/1. Exact 31/32/33/47 loops shorten from ordinary IR facts to 7/7/32/1; demanded-live-out, side-effect, and dynamic-divisor 73-trip negatives remain 73. |
| Fixed-point suffix stages/e2e | `python3 scripts/run_tests.py --suite stage --stage oir --stage mir --stage asm --filter context_inline_fixed_point_suffix --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e --filter context_inline_fixed_point_suffix --jobs 1 --o1` | yes for the current residual cleanup | PASS | OIR/MIR/ASM 3/3 and e2e 1/1; covers positive/negative/INT_MIN signed div, negative srem, exact 31/32/33 and non-32 47/73 counts. |
| OIR infrastructure fixture repair | `python3 scripts/run_tests.py --binary build/linux/x86_64/release/compiler --suite infra --jobs 1 --infra-timeout 60 --work-dir /tmp/yoolang-huffman-repair-final-infra` | yes because test infrastructure changed | PASS (current repair) | Final post-repair release rerun passed 1/1 in 3.22s. Rerun again as part of the final full suite. |
| MIR address-combine ASAN reproducer | `xmake f -m debug && xmake`; then `for source in test/performance/huffman-01.sy test/performance/huffman-02.sy test/performance/huffman-03.sy test/functional/context_inline_fixed_point_suffix.sy test/functional/context_inline_residual_pe.sy; do env ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 build/linux/x86_64/debug/compiler --emit-mir -O1 "$source" >/dev/null || exit 1; done` | yes because MIR production changed | PASS (current repair) | Post-repair current-source debug+ASAN rebuild and all five explicit MIR compilations passed, 5/5. This remains focused rather than the final broad sanitizer gate. |
| Debug+ASAN FileCheck/poly checkpoint | `env ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 python3 scripts/run_tests.py --binary build/linux/x86_64/debug/compiler --suite filecheck --filter oir_context_inline --jobs 1 --filecheck-timeout 180 --work-dir /tmp/yoolang-huffman-repair-asan-filecheck`; then the analogous `--filter cost_model_pe --work-dir /tmp/yoolang-huffman-repair-asan-cost` command | yes for current OIR ownership/CFG changes | PASS (canonical repair); broad checkpoint remains interim | Current-source debug+ASAN passed context-inline 12/12, including both metamorphic pairs, and `cost_model_pe` 1/1. The earlier broad result remains interim. |
| Debug+ASAN full stage/e2e checkpoint | `env ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 python3 scripts/run_tests.py --binary build/linux/x86_64/debug/compiler --suite stage --suite e2e --jobs 1 --o1 --compile-timeout 180 --link-timeout 60 --run-timeout 60` | repair checkpoint; final sanitizer rerun required after blockers | INCOMPLETE | 1412 PASS, 6 compile timeouts at 180s, 1 SKIP. The six timeouts belonged to two groups; implementation reported each group later passed 3/3 in release, which does not provide ASAN closure. |
| Release full-suite checkpoint | `python3 scripts/run_tests.py --binary build/linux/x86_64/release/compiler --suite all --jobs 1 --o1 --compile-timeout 180 --link-timeout 60 --run-timeout 60 --filecheck-timeout 60`; then the exact focused infra command above | repair checkpoint only | INTERIM_PASS | Initial full run: 1480 PASS, 1 infra FAIL, 1 SKIP. After the fixture-only repair, focused infra passed 1/1, giving a combined checkpoint of 1481 PASS, 0 FAIL, 1 SKIP. Remaining source blockers mean the normative final full gate below stays `NOT_RUN`. |
| Worklist FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_worklist --jobs 1` | yes | PASS | 1/1 in 2.87s. The broader common-prefix rerun passed 10/10 and adds positional binding plus same-block all-accept/all-budget-reject coverage. |
| Canonical metamorphic evidence | `python3 scripts/run_tests.py --binary build/linux/x86_64/release/compiler --suite filecheck --filter oir_context_inline_metamorphic --jobs 1 --filecheck-timeout 180` | yes | PASS (canonical repair) | 2/2. Definition reorder and CFG/block/successor-container permutation compare accepted structural fingerprints, reject reasons, and consumed budget totals in normal and forced tie-budget modes. |
| Cleanup FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_cleanup_budget --jobs 1` | yes | PASS | Included in the post-repair common-prefix FileCheck 10/10. |
| Scratch clone FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_clone --jobs 1` | yes | PASS | Included in the post-repair common-prefix FileCheck 10/10. |
| Return-demand FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_return_demand --jobs 1` | yes | PASS | Included in the post-repair common-prefix FileCheck 10/10. |
| Residual-cost FileChecks | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_cost --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter cost_model_pe --jobs 1` | yes | PASS | Residual-cost passed within common-prefix 10/10; `cost_model_pe` separately passed 1/1 and contains measured metrics, zero-SMT, and restored `CodeGrowthTooHigh` rejection assertions. |
| Commit FileCheck | `python3 scripts/run_tests.py --binary build/linux/x86_64/release/compiler --suite filecheck --filter oir_context_inline_residual_pe_commit --jobs 1 --filecheck-timeout 180 --work-dir /tmp/yoolang-huffman-repair-commit` | yes | PASS (current repair) | Dedicated 1/1 and final common-prefix 10/10 pass. Cost output contains two independent `measured-persistent-reuse-member` accepts before one aggregate reuse accept. |
| Rollback FileCheck | `python3 scripts/run_tests.py --binary build/linux/x86_64/release/compiler --suite filecheck --filter oir_context_inline_residual_pe_rollback --jobs 1 --filecheck-timeout 180 --work-dir /tmp/yoolang-huffman-repair-rollback` | yes | PASS (current repair) | Dedicated 1/1 and release/debug+ASAN common-prefix 10/10 pass. Every named failpoint explicitly asserts unchanged/restored function-table-inclusive snapshot state and a subsequent control acceptance. |
| Pressure FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_pressure --jobs 1` | yes | PASS | Included in common-prefix 10/10. Direct-tie combined-budget rejection is additionally covered by `oir_context_inline_positional_tie`. |
| Focused OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter context_inline --jobs 1 --o1` | yes | PASS (current focused) | Post-repair functional context-inline scope passed 2/2: fixed-point suffix and residual PE. Broader full-stage closure remains separate. |
| Focused downstream O1 | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter context_inline --jobs 1 --o1` | yes | PASS (current focused) | Post-repair functional context-inline MIR/ASM scope passed 4/4. Broader full-stage closure remains separate. |
| Focused semantic e2e | `python3 scripts/run_tests.py --suite e2e --filter context_inline_residual_pe --jobs 1 --o1` | yes | PASS (current focused) | The common `context_inline` filter passed both residual-PE and fixed-point functional cases, 2/2, including ordered positional constants, dependent same-block calls, signed division/remainder, and visible side effects. |
| Huffman stages/e2e | `python3 scripts/run_tests.py --binary build/linux/x86_64/release/compiler --suite stage --stage oir --stage mir --stage asm --suite e2e --filter huffman-0 --jobs 1 --o1 --compile-timeout 180 --link-timeout 60 --run-timeout 60 --work-dir /tmp/yoolang-huffman-repair-final-huffman` | yes | PASS (current repair) | Post-repair OIR/MIR/ASM 9/9 and e2e 3/3 passed. Performance remains blocked pending independent review. |
| `pseudo_md5` stages/e2e | `python3 scripts/run_tests.py --binary build/linux/x86_64/release/compiler --suite stage --stage oir --stage mir --stage asm --suite e2e --filter crypto- --jobs 1 --o1 --compile-timeout 180 --link-timeout 60 --run-timeout 60 --work-dir /tmp/yoolang-huffman-repair-final-crypto` | yes | PASS (current repair) | Post-repair OIR/MIR/ASM 9/9 and e2e 3/3 passed. MIR clone/spill/stack performance-report preservation remains a later performance gate. |
| Full FileCheck/poly | `python3 scripts/run_tests.py --suite filecheck --suite poly --jobs 1` | yes | NOT_RUN | Required before broad perf. |
| Full downstream correctness | `python3 scripts/run_tests.py --suite stage --suite e2e --jobs 1 --o1` | yes | NOT_RUN | Required O1 downstream coverage. |
| Full optimized correctness | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | yes | NOT_RUN | Report every skip/failure. |
| Final protected-doc/independent-task gate | verify whole-file and base-diff hashes from `protection/resume-2026-07-21-protection.txt` | yes before review | PASS (current repair closure) | Final repair check: base-doc diff hash remains empty (`e3b0c442...`); `docs/README.md`, `docs/egraph-design.md`, and the independently audited loop task match `93478982...`, `d3c17fe3...`, and `9ef4b99e...`; the independent-task binary-diff hash remains `fd2be95f...`. |
| Final shared-index/ownership gate | hash the index after excluding lines containing the actual relative link target `2026-07-17-context-inline-residual-pe.md`; compare with `protection/resume-2026-07-21-protection.txt`; inspect status/diff | yes before review | PASS (current repair closure) | Correctly filtered index hash remains `2416f730...`, this task row count is exactly one, and the index diff remains limited to this task's status/branch row. |

The exact final protected-path closure is:

```bash
git diff --exit-code c815887 -- docs/README.md docs/egraph-design.md
sha256sum docs/README.md docs/egraph-design.md docs/tasks/2026-07-17-oir-guarded-small-loop-unroll.md
awk 'index($0, "2026-07-17-context-inline-residual-pe.md") == 0' docs/tasks/README.md | sha256sum
test "$(rg -cF '2026-07-17-context-inline-residual-pe.md' docs/tasks/README.md)" -eq 1
git status --short
git diff -- docs/tasks/README.md
```

Final status is inspected against the declared production/test scope. The task-owned task record and
its index row may change; the independent task bytes and all non-this-task index bytes remain
machine-compared to their P0 evidence.

No YIR, AST, DAE, runtime, OIR-to-MIR-lowerer-specific, stack fallback, or non-`-O1` ABI gate is
required or authorized.

### Fixed Performance Artifact Layout

All required raw reports, provenance, and deltas must use exactly this task-specific layout in the
current worktree. Create directories before a run; copy both top-level report files immediately
after every run because the next run overwrites them.

Every shortened destination such as `baseline/huffman/` below is relative to
`/home/yoo/Documents/Compliers/yoolang/build/perf-ci/context-inline-residual-pe/`, even when the run
itself executes in the detached baseline worktree.

```text
build/perf-ci/context-inline-residual-pe/
  protection/
    implementation-start-other-task.sha256
    implementation-start-task-index-excluding-this-row.txt
  diagnosis/clang22/...
  baseline/
    provenance.md
    huffman/perf-report.json
    huffman/perf-report.md
    pseudo-md5/perf-report.json
    pseudo-md5/perf-report.md
    test-performance/perf-report.json
    test-performance/perf-report.md
    ci-parity/perf-report.json
    ci-parity/perf-report.md
    full-unexcluded/perf-report.json        # optional diagnostic
    full-unexcluded/perf-report.md          # optional diagnostic
  current/
    provenance.md
    huffman/perf-report.json
    huffman/perf-report.md
    pseudo-md5/perf-report.json
    pseudo-md5/perf-report.md
    test-performance/perf-report.json
    test-performance/perf-report.md
    ci-parity/perf-report.json
    ci-parity/perf-report.md
    full-unexcluded/perf-report.json        # optional diagnostic
    full-unexcluded/perf-report.md          # optional diagnostic
  delta/
    huffman/perf-delta.json
    huffman/perf-delta.md
    huffman/instruction-count-compare.json
    pseudo-md5/perf-delta.json
    pseudo-md5/perf-delta.md
    pseudo-md5/instruction-count-compare.json
    test-performance/perf-delta.json
    test-performance/perf-delta.md
    test-performance/instruction-count-compare.json
    ci-parity/perf-delta.json
    ci-parity/perf-delta.md
    ci-parity/instruction-count-compare.json
```

Baseline worktree is fixed as `/tmp/yoolang-context-inline-residual-pe-de93e24`; validate or create
it detached at `de93e24`. Run baseline and current jobs sequentially on the same quiet machine. Do
not build/test in either worktree while comparable wall-time measurement is running.

```bash
git worktree add --detach /tmp/yoolang-context-inline-residual-pe-de93e24 de93e24
git -C /tmp/yoolang-context-inline-residual-pe-de93e24 rev-parse --short HEAD
git -C /tmp/yoolang-context-inline-residual-pe-de93e24 status --short
```

If the path already exists, do not remove or reset it; validate that it belongs to this repository,
is detached at `de93e24`, and has no source changes before using it.

Each `provenance.md` must record: absolute worktree, full commit, dirty state before build, release
build command/result, compiler absolute path and SHA-256, runtime absolute path and SHA-256,
`compare_perf.py` SHA-256, Python/GCC/Clang/QEMU paths and versions, all relevant environment values,
case selection/exclusions/timeouts, `generated_utc`, instruction-count configuration/status, and
the corresponding saved report paths.

### Performance

Every row has its own Result and must be updated independently. For a baseline row, run from the
fixed detached worktree with:

```text
COMPILER_BIN=/tmp/yoolang-context-inline-residual-pe-de93e24/build/linux/x86_64/release/compiler
SYSY_RUNTIME_LIB=/tmp/yoolang-context-inline-residual-pe-de93e24/runtime/libsysy_riscv.a
```

For a current row, run from `/home/yoo/Documents/Compliers/yoolang` with:

```text
COMPILER_BIN=/home/yoo/Documents/Compliers/yoolang/build/linux/x86_64/release/compiler
SYSY_RUNTIME_LIB=/home/yoo/Documents/Compliers/yoolang/runtime/libsysy_riscv.a
```

| Gate | Workdir and exact command/saved destination | Required? | Result | Acceptance/notes |
| --- | --- | --- | --- | --- |
| Baseline release/provenance | workdir `/tmp/yoolang-context-inline-residual-pe-de93e24`: `git rev-parse HEAD`; `git status --short`; `xmake f -m release`; `xmake`; hash compiler/runtime/script and write `baseline/provenance.md` | yes, before source edits | PASS | Clean detached `de93e2444dc5ec15ecc17bc852d5d5104a1de7c8`; release build passed; absolute paths, SHA-256, tool versions, environments, report times/paths recorded. |
| Current release/provenance | workdir current: `git rev-parse HEAD`; `git status --short`; `xmake f -m release`; `xmake`; hash compiler/runtime/script and write `current/provenance.md` | yes, after implementation | NOT_RUN | Compiler/runtime provenance must be explicit and comparable. |
| Baseline Huffman | baseline workdir: `COMPILER_BIN=/tmp/yoolang-context-inline-residual-pe-de93e24/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/tmp/yoolang-context-inline-residual-pe-de93e24/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance/huffman-01.sy,test/performance/huffman-02.sy,test/performance/huffman-03.sy python3 scripts/compare_perf.py`; save JSON/MD to `baseline/huffman/` | yes | PASS | 3/3 OK, 0 failures; Yoolang `0.0646/0.0634/0.0648s`, Clang `0.0337/0.0337/0.0332s`; `clang_o3_geomean=0.5217955463`; generated `2026-07-17 14:27:59 UTC`. |
| Current Huffman | current workdir: `COMPILER_BIN=/home/yoo/Documents/Compliers/yoolang/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/home/yoo/Documents/Compliers/yoolang/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance/huffman-01.sy,test/performance/huffman-02.sy,test/performance/huffman-03.sy python3 scripts/compare_perf.py`; save JSON/MD to `current/huffman/` | yes | NOT_RUN | All 3 OK; each Yoolang median is lower than its Clang `-O3` median; `clang_o3_geomean > 1.0`; >=10% Yoolang baseline geomean improvement; per-case regression threshold above. |
| Huffman delta | current workdir: `python3 scripts/compare_perf_baseline.py --current build/perf-ci/context-inline-residual-pe/current/huffman/perf-report.json --baseline build/perf-ci/context-inline-residual-pe/baseline/huffman/perf-report.json --out-md build/perf-ci/context-inline-residual-pe/delta/huffman/perf-delta.md --out-json build/perf-ci/context-inline-residual-pe/delta/huffman/perf-delta.json --out-insn-json build/perf-ci/context-inline-residual-pe/delta/huffman/instruction-count-compare.json --baseline-label de93e24` | yes | NOT_RUN | Inspect all rows; script exit 0 alone is not PASS. |
| Baseline `pseudo_md5` | baseline workdir: `COMPILER_BIN=/tmp/yoolang-context-inline-residual-pe-de93e24/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/tmp/yoolang-context-inline-residual-pe-de93e24/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance/crypto-1.sy,test/performance/crypto-2.sy,test/performance/crypto-3.sy python3 scripts/compare_perf.py`; save JSON/MD to `baseline/pseudo-md5/` | yes | PASS | 3/3 OK, 0 failures; Yoolang `0.0466/0.0342/0.0245s`; reports include MIR/cost metrics; generated `2026-07-17 14:29:31 UTC`. |
| Current `pseudo_md5` | current workdir: `COMPILER_BIN=/home/yoo/Documents/Compliers/yoolang/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/home/yoo/Documents/Compliers/yoolang/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance/crypto-1.sy,test/performance/crypto-2.sy,test/performance/crypto-3.sy python3 scripts/compare_perf.py`; save JSON/MD to `current/pseudo-md5/` | yes | NOT_RUN | No new committed residual clone, spill/stack growth, or meaningful runtime regression. |
| `pseudo_md5` delta | current workdir: `python3 scripts/compare_perf_baseline.py --current build/perf-ci/context-inline-residual-pe/current/pseudo-md5/perf-report.json --baseline build/perf-ci/context-inline-residual-pe/baseline/pseudo-md5/perf-report.json --out-md build/perf-ci/context-inline-residual-pe/delta/pseudo-md5/perf-delta.md --out-json build/perf-ci/context-inline-residual-pe/delta/pseudo-md5/perf-delta.json --out-insn-json build/perf-ci/context-inline-residual-pe/delta/pseudo-md5/instruction-count-compare.json --baseline-label de93e24` | yes | NOT_RUN | Inspect codegen/cost rows, not timing alone. |
| Baseline `test/performance` | baseline workdir: `COMPILER_BIN=/tmp/yoolang-context-inline-residual-pe-de93e24/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/tmp/yoolang-context-inline-residual-pe-de93e24/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py`; save JSON/MD to `baseline/test-performance/` | yes | PASS | Complete 60/60 OK, 0 failures; generated `2026-07-17 14:31:21 UTC`; no `PERF_MAX_CASES`. |
| Current `test/performance` | current workdir: `COMPILER_BIN=/home/yoo/Documents/Compliers/yoolang/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/home/yoo/Documents/Compliers/yoolang/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py`; save JSON/MD to `current/test-performance/` | yes | NOT_RUN | Required; inspect all meaningful per-case and MIR/spill changes. |
| `test/performance` delta | current workdir: `python3 scripts/compare_perf_baseline.py --current build/perf-ci/context-inline-residual-pe/current/test-performance/perf-report.json --baseline build/perf-ci/context-inline-residual-pe/baseline/test-performance/perf-report.json --out-md build/perf-ci/context-inline-residual-pe/delta/test-performance/perf-delta.md --out-json build/perf-ci/context-inline-residual-pe/delta/test-performance/perf-delta.json --out-insn-json build/perf-ci/context-inline-residual-pe/delta/test-performance/instruction-count-compare.json --baseline-label de93e24` | yes | NOT_RUN | Any reported/meaningful regression requires attribution and repair. |
| Baseline CI-parity | baseline workdir: `COMPILER_BIN=/tmp/yoolang-context-inline-residual-pe-de93e24/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/tmp/yoolang-context-inline-residual-pe-de93e24/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance,test/bsb-final PERF_EXCLUDE_CASES=test/performance/h-10-02.sy,test/performance/h-10-03.sy,test/bsb-final/2025-CPS-39.sy,test/bsb-final/2025-Z8N-28.sy PERF_TIMEOUT_SEC=20 python3 scripts/compare_perf.py`; save JSON/MD to `baseline/ci-parity/` | yes | PASS | Live CI selection/exclusions confirmed; complete selected 115/115 OK, 0 failures; generated `2026-07-17 14:34:50 UTC`; instruction count DISABLED. |
| Current CI-parity | current workdir: `COMPILER_BIN=/home/yoo/Documents/Compliers/yoolang/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/home/yoo/Documents/Compliers/yoolang/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance,test/bsb-final PERF_EXCLUDE_CASES=test/performance/h-10-02.sy,test/performance/h-10-03.sy,test/bsb-final/2025-CPS-39.sy,test/bsb-final/2025-Z8N-28.sy PERF_TIMEOUT_SEC=20 python3 scripts/compare_perf.py`; save JSON/MD to `current/ci-parity/` | yes | NOT_RUN | Required complete selected scope; CI parity is not full corpus. |
| CI-parity delta | current workdir: `python3 scripts/compare_perf_baseline.py --current build/perf-ci/context-inline-residual-pe/current/ci-parity/perf-report.json --baseline build/perf-ci/context-inline-residual-pe/baseline/ci-parity/perf-report.json --out-md build/perf-ci/context-inline-residual-pe/delta/ci-parity/perf-delta.md --out-json build/perf-ci/context-inline-residual-pe/delta/ci-parity/perf-delta.json --out-insn-json build/perf-ci/context-inline-residual-pe/delta/ci-parity/instruction-count-compare.json --baseline-label de93e24` | yes | NOT_RUN | Inspect failures, total/case timing, instruction status, MIR and cost summaries. |
| Baseline full unexcluded | baseline workdir: `COMPILER_BIN=/tmp/yoolang-context-inline-residual-pe-de93e24/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/tmp/yoolang-context-inline-residual-pe-de93e24/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py`; save JSON/MD to `baseline/full-unexcluded/` | optional diagnostic | NOT_RUN | Optional; do not delay required scopes. Failure/timeout is recorded as FAIL/TIMEOUT, never PASS. |
| Current full unexcluded | current workdir: `COMPILER_BIN=/home/yoo/Documents/Compliers/yoolang/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/home/yoo/Documents/Compliers/yoolang/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py`; save JSON/MD to `current/full-unexcluded/` | optional diagnostic | NOT_RUN | Compare only if baseline/current both complete and comparable. |

For every run, verify the saved JSON's `generated_utc`, compiler binary, test dirs, exclusions, row
membership/count, failures, codegen metrics, cost summaries, and instruction-count status. A missing
dependency is `SKIP` with reason; a required timeout/incomplete report is `TIMEOUT`/`FAIL` and blocks
`ready_for_review`. Optional full-unexcluded failure/timeout remains explicitly diagnostic and may
not be cited as a passing gate. `PERF_MAX_CASES` may be used only for a labeled smoke and never in
the saved final evidence.

An earlier unarchived focused result was described as about **1.19x Clang `-O3`**. It predates the
current OIR safety, rollback/worklist blocker repairs, and MIR UAF scope revision. It is historical
orientation only: it does not satisfy the still-`NOT_RUN` Current Huffman or delta rows and must not
be reported as the current accepted performance level.

## Performance Acceptance Details

- Same-machine Yoolang `de93e24` versus current is the attribution baseline. GCC/Clang ratios are
  external context only.
- Focused Huffman: all three correct; every row's Yoolang `-O1` median is strictly lower than the
  corresponding Clang `-O3` median; focused `clang_o3_geomean > 1.0`; >=10% Yoolang geomean
  improvement against the isolated baseline; and no individual case slower than that baseline by
  both >5% and >0.003 seconds. Any Clang tie/loss blocks acceptance even if the baseline delta wins.
- `pseudo_md5`: no unintended residual clone; compare final MIR instructions, branches/jumps,
  loads/stores, spills, stack slots, live/pressure diagnostics, and runtime. The orientation report
  shows 611 final MIR, 4 spills, and 10 stack slots for each crypto case, but P0 is authoritative.
- Required `test/performance` and CI-parity scopes must both complete for baseline and current. Any
  meaningful regression, even below report heuristics, requires attribution/repair or explicit user
  permission; no permission exists here.
- Dynamic instructions support attribution. `DISABLED`/`SKIPPED` is reported; requested `FAILED`
  is investigated. Static leftover files never establish provenance.

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| OIR context worklist + bounded cleanup | Addresses definition-order/context loss with ordinary IR facts | chosen |
| Detached transactional residual PE | Lets cost see real cleanup and makes rejection auditable | chosen |
| Minimal OIR origin/root lineage | Removes specialization name decisions without cross-layer scope | chosen |
| Full `is_entrypoint`/`IntrinsicID`/AST-YIR-OIR/ABI migration | Other name decisions deserve semantic metadata | deferred to mandatory separate task; outside this patch/context/tests |
| Whole-loop bitop recognizer | Could collapse the visible loops | rejected: signed negative counterexamples make it unsound |
| Huffman matcher or magic tuple | Directly targets observed calls | forbidden and non-general |
| Huffman-only SMT/CEGIS | Could synthesize sampled behavior | forbidden; samples/identity do not prove all signed inputs |
| Blind inline-threshold increase | Could admit expanded callees | rejected: code growth/compile-time/crypto regression risk |
| Live-module speculative clone then delete | Reuses existing cleanup APIs | rejected: cannot prove function table/use/name/stats/budget rollback |
| Solver/OR/rotate work first | Earlier diagnosis suspected bitops | rejected: Clang evidence points first to ordinary context/cleanup |
| Backend/RA-only work | Final code is slower | rejected as primary path: motivating spill count is zero and OIR retains calls |
| Separate prerequisite task for the five-line MIR UAF repair | The original scope marked MIR production false | rejected after scope review: the context-generated downstream shape directly exposed the latent operand-lifetime bug, the exact repair is required for this task's correctness gates, and a third user-visible task would add no independently useful behavior; MIR scope is revised narrowly and audibly here |
| General bounded-loop superoptimizer | Could prove more loop semantics | future Priority-1 only; separate task plus written technical-committee approval |

## Change Log

- 2026-07-17: created the task in `scoped` state at `de93e24`, preserving the Huffman/Clang/inliner
  and stale-residual-cost diagnosis plus generic correctness/performance gates.
- 2026-07-17: revised after independent scope review. Restricted metadata to minimal OIR
  origin/root lineage; moved entrypoint/intrinsic/frontend/DAE/mod-ref/lowerer/ABI migration to a
  separate future task; recorded five-path dirty ownership and exact user-file protection hashes;
  split worklist, cleanup, scratch, return demand, residual cost, commit, rollback, and pressure into
  bounded patches; added auditable Clang 22 artifacts and explicit baseline/current/delta rows.
  Status remains `scoped`; no implementation, build, correctness test, or performance run occurred.
- 2026-07-17: closed the independent-task ownership gap by making P0 generate an initial SHA-256
  manifest for both user files and the guarded-small-loop task before checkout, plus initial tracked
  diff/numstat/status and shared-index snapshots. Post-checkout and final gates now read that P0
  evidence with `sha256sum -c`/`cmp`; embedded hashes remain orientation only. No README, compiler,
  or test source changed, and no branch/build/test/performance gate ran.
- 2026-07-17: audited against live `huff/master@c815887` after the five documentation paths were
  committed. Set `c815887` as the implementation branch base while retaining production-equivalent
  `de93e24` as the isolated compiler-performance baseline; superseded the pre-commit dirty-state
  commands; added a dynamic guard for the independently audited loop task and shared index; made
  per-row Huffman wins over Clang `-O3` plus `clang_o3_geomean > 1.0` a hard acceptance gate; and
  recorded that only implementation/repair/review subagents may run builds, tests, or
  `scripts/compare_perf.py`. No production/test source, README, build, or test was touched.
- 2026-07-17: implementation subagent entered `in_progress`, verified `huff@c815887` with exactly
  the two audited task records dirty, captured the independent-task SHA and filtered shared-index
  snapshots, created `task/context-inline-residual-pe`, and passed the immediate protected-state
  closure. P0 baseline/performance and Clang artifacts remain before source edits.
- 2026-07-17: completed P0 before source edits. Built the clean detached `de93e24` release; ran
  `scripts/compare_perf.py` serially for Huffman (3), pseudo-MD5 (3), complete `test/performance`
  (60), and live CI-parity (115), all PASS with zero failures and no `PERF_MAX_CASES`; froze every
  JSON/Markdown report plus provenance. Generated and inspected the required Clang 22.1.8
  LLVM/assembly/optimization-record artifacts, confirming normal nested inlining/cleanup evidence
  and retained 32-trip helper loops rather than a whole-loop machine bitop.
- 2026-07-17: completed P1a/P1b. Added OIR-only `FunctionOrigin`, stable function/root IDs and
  creation/template-copy propagation; specialization eligibility and per-root fanout now use this
  metadata rather than `__yo_constprop.*` prefixes. Generated names remain printing/collision
  artifacts. Release build, focused FileCheck (1/1), and focused OIR stage (1/1) passed.
- 2026-07-21: recovery implementation agent audited the authoritative dirty branch without reset,
  cleanup, checkout, or source mutation. Confirmed P0 archives and P1 lineage, classified the live
  context worklist/detached PE/direct-inline diff, and found the task record behind the code. The
  latest unarchived focused report belonging to the current binary was PASS at Yoolang
  `0.0272/0.0276/0.0283s` versus Clang `0.0329/0.0336/0.0335s`; it is orientation only until a new
  serialized `compare_perf.py` run is archived.
- 2026-07-21: repaired protected-index recovery evidence. The old predicate containing
  `docs/tasks/...` did not match the relative Markdown link and therefore excluded no row. Wrote
  `build/perf-ci/context-inline-residual-pe/protection/resume-2026-07-21-protection.txt` with current
  whole-file/base-diff hashes and the correctly filtered non-this-task index hash. Base docs,
  independently audited loop task, and non-this-task index bytes remained unchanged through the
  focused work.
- 2026-07-21: added generic fixed-point suffix legality coverage. Exact constant countdowns
  31/32/33/47 shorten only after a pure, constant-nonzero-divisor recurrence reaches a proven fixed
  point; a non-32 73-trip demanded-live-out, global-side-effect, and dynamic-divisor group remains
  intact. Build, FileCheck 1/1, OIR/MIR/ASM stages 3/3, and e2e 1/1 passed. Repaired the P1 cost
  summary assertion and functional exit-code expectation; its FileCheck 1/1 and stages/e2e 4/4
  passed. No performance command ran during this recovery checkpoint.
- 2026-07-21: independent task-scope revision chose the explicit narrow-scope option for the
  `MIRAddressModeCombinePass.cpp` UAF instead of creating a third prerequisite task. The generated
  OIR shape directly exposed a pre-existing post-rewrite operand-lifetime bug, and saving the
  register value before replacement is required by this task's downstream correctness gates.
  Marked MIR and test infrastructure affected; recorded the actual OIR ownership/use-list,
  ADCE/CFG/phi/analysis/scalar/global safety files and patch rows; preserved implementation-reported
  ASAN/release evidence without inventing the lost exact five-case/62-case commands. The same review
  left P2-P11 blocked/open for positional canonical keys, per-member tie estimates, same-block tie
  handling, `CodeGrowthTooHigh` coverage, and rollback coverage. The historical unarchived ~1.19x
  Clang ratio remains non-acceptance evidence. This scope agent changed documentation only and ran
  no build, correctness test, sanitizer, or performance command.
- 2026-07-21: implementation repair closed the five pre-performance review blockers in the
  authoritative dirty worktree. Positional typed argument bindings now remain in canonical and
  specialization keys; every direct-tie member receives a fresh detached residual, measured
  estimate, pressure vector, and cost decision; same-basic-block batches reverse-split by original
  instruction position and obey atomic all-fit/all-reject preflight. Added a persistent generic
  `tie-budget` fixture branch for combined-budget rejection, restored the measured
  `CodeGrowthTooHigh` assertion, and exercised every named rollback failpoint with exact internal
  live/use/allocator and stats/growth/pressure snapshots plus later control acceptance. Release
  build passed; common context-inline FileCheck passed 10/10, `cost_model_pe` 1/1, focused OIR 2/2,
  MIR/ASM 4/4, e2e 2/2, and OIR infra 1/1. No `compare_perf.py` command ran; independent re-review,
  broad sanitizer/correctness, crypto, and performance gates remain required.
- 2026-07-21: independent pre-performance re-review blocked performance. The non-persistent
  direct-tie branch does rebuild and cost every member, but the earlier measured-persistent-reuse
  branch accepts an exact tie from only the first site's detached residual/estimate plus aggregate
  pressure and one `.reuse` cost decision. The existing commit fixture confirms that bypass. The
  same review also noted that rollback assertions do not explicitly compare function-table
  mappings. Current-source debug+ASAN passed the explicit five-file MIR reproducer 5/5, common
  context-inline FileCheck 10/10, and `cost_model_pe` 1/1; release OIR infra, Huffman
  OIR/MIR/ASM/e2e, and crypto OIR/MIR/ASM/e2e passed 1/1, 12/12, and 12/12. Protected bytes and the
  filtered task index still matched the recovery manifest. No performance command ran.
- 2026-07-21: repair subagent closed the two latest review findings without resetting or replacing
  the authoritative dirty diff. Measured persistent reuse now independently revalidates every exact
  tie member's eligibility/mask/key, builds a fresh detached residual, measures metrics and
  pressure, and obtains a per-member cost decision before the one-clone aggregate budget/cost and
  atomic retarget. The commit fixture requires two member records before the aggregate record.
  `Module::function_table_mappings()` supplies a const full-table view used by rollback snapshots to
  copy and compare every name-to-`Function*` entry; all named failure diagnostics and following
  controls assert that coverage. Final release build, common FileCheck 10/10, `cost_model_pe` 1/1,
  infra 1/1, context 8/8, Huffman 12/12, and crypto 12/12 passed. Debug+ASAN build, common
  FileCheck 10/10, `cost_model_pe` 1/1, and the five-file MIR reproducer 5/5 passed. No
  `compare_perf.py` command ran; PRE-PERF remains blocked pending fresh independent review.
- 2026-07-21: fresh independent pre-performance re-review accepted the persistent exact-tie
  per-member evaluation/atomic publication repair and the complete function-table rollback
  snapshot repair.  Release focused gates passed 10/10 context FileCheck, 1/1 cost-model, 1/1
  infra, 8/8 context, 12/12 Huffman and 12/12 crypto; debug+ASAN passed 10/10 context FileCheck,
  1/1 cost-model and the five-file MIR reproducer 5/5.  Performance remains blocked because the
  canonical callsite key still omits normalized intra-function instruction position and the test
  battery still lacks paired definition-reorder/equivalent-CFG fingerprint/reason/budget
  metamorphic evidence.  Protected closure passed and no performance command ran.
- 2026-07-21: canonical-position/metamorphic repair added bounded canonical CFG/data-flow colors,
  a normalized intra-function position class, and stable `callsite.<hash>` cost fingerprints. No
  function name, mutable ID, address, raw label, module/block insertion, or successor-container
  order participates; a scan-local cache bounds repeat work. The same-block fixture now uses two
  observable structurally equivalent calls, proving normal all-fit and forced-budget all-reject.
  New definition-reorder and CFG/block/successor permutation pairs directly compare accepted
  fingerprints, reject reasons, and code/live/register/memory consumed-budget totals. Release
  build, context FileCheck 12/12, cost 1/1, infra 1/1, context 8/8, Huffman 12/12 and crypto 12/12
  passed; debug+ASAN build, context FileCheck 12/12 and cost 1/1 passed. No performance command ran;
  PRE-PERF awaits fresh independent review and the previously recorded broad gates.

## Open Questions

- None at scope time. The minimal OIR metadata boundary, rollback contract, tie/global-budget rule,
  required reports, and branch protection flow are fixed. A need for cross-layer metadata or a loop
  superoptimizer opens a separate task rather than expanding this one. The coordinator must schedule
  the two implementation agents sequentially in the shared checkout or provide isolated worktrees;
  concurrent branch switching in one checkout is forbidden.

## Handoff Note

Current state:

- Implementation is active on `task/context-inline-residual-pe@c815887`; the dirty worktree is the
  authoritative uncommitted implementation. Do not reset, restore, clean, switch branches, or
  overwrite any path. P0 baseline/provenance/Clang evidence and P1 origin/root lineage are present.
- The live diff also contains name-independent call-graph/context scoring, bounded verified cleanup,
  detached scratch residualization, dead/scalar return demand, measured PE cost fields, direct
  residual inline, and a generic exact constant fixed-point suffix proof. P6 and P8 are completed;
  remaining P2-P5/P7/P9/P10 rows stay `in_progress` until their broader named invariants and final
  gates close. Code presence alone is not completion evidence.
- Recovery fixed-point coverage passes for exact 31/32/33/47 positive cases and 73-trip demanded
  live-out/side-effect/dynamic-divisor negatives, including negative, INT_MIN, signed division and
  signed remainder semantics. The transform uses no testcase/function/variable name, parameter
  tuple, runtime input, source hash, expected output, or fixed 32 selector.
- Protected closure source of truth is now
  `build/perf-ci/context-inline-residual-pe/protection/resume-2026-07-21-protection.txt`; exclude this
  task row with the actual relative link substring, not the superseded `docs/tasks/...` predicate.
- A current-binary top-level focused report already crossed the Clang hard gate, but it has not yet
  been rerun/archived as final evidence. The older reported ratio is about 1.19x Clang `-O3`, but it
  predates the current safety/MIR revisions and is not current acceptance evidence. Required
  focused, pseudo-MD5, complete performance, CI-parity, deltas, complete correctness, and a passing
  independent review remain open.
- The latest two pre-performance findings are repaired in the dirty worktree. Persistent exact ties
  now receive per-member revalidation, detached residualization, measurement, pressure review and
  cost decisions before the aggregate one-clone decision. Rollback snapshots explicitly compare
  the full function-table name-to-pointer mapping on every named failure path, and each fixture
  still observes a following control acceptance. Canonical intra-function position and paired
  definition-reorder/CFG-permutation evidence are now also implemented and pass focused release
  and debug+ASAN gates. **PRE-PERF remains BLOCKED** pending fresh independent review of this latest
  repair and the recorded broad correctness gates.
- Actual scope now explicitly includes the OIR ownership/use-list, ADCE/CFG/phi/analysis/scalar/
  global safety repairs and `scripts/oir_infra_tests.py`. It also includes exactly one MIR production
  repair in `MIRAddressModeCombinePass.cpp`; no separate prerequisite task was created.
- Implementation/reviewer evidence reports that the MIR fix changed a common 5/5 ASAN failure to
  5/5 pass, FileCheck/poly passed 62/62 under debug+ASAN, the broad sanitizer checkpoint had 1412
  PASS/6 timeouts/1 SKIP, and the release checkpoint combined to 1481 PASS/0 FAIL/1 SKIP after a
  focused infra repair. Independent re-review has now recorded and passed an exact current-source
  five-file MIR command 5/5 and focused debug+ASAN FileCheck 10/10 plus `cost_model_pe` 1/1. The
  earlier broad 62-case command is still not preserved, so that broad report remains interim rather
  than final verification.

Next action:

1. Obtain a fresh independent review of the canonical-position implementation and paired
   definition-reorder/CFG-permutation evidence. Do not invoke `compare_perf.py` before it passes.
2. Close the remaining P2-P10 invariant gaps and named alias/SCC fixtures.
4. Run focused/full correctness in the matrix. Every test remains serialized with `--jobs 1` in the
   implementation/review subagent; do not treat release reruns as closure for ASAN timeouts.
5. Only after independent pre-performance review passes, build current release, then invoke
   `python3 scripts/compare_perf.py` sequentially for focused,
   pseudo-MD5, complete `test/performance`, and current CI-parity. Copy JSON/MD immediately after
   each run, generate/inspect the four deltas, and enforce every hard Clang/baseline/spill/regression
   condition without `PERF_MAX_CASES` final evidence.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-17-context-inline-residual-pe.md`
- `/home/yoo/.codex/skills/yoolang-optimization/references/compiler-state.md`
- `/home/yoo/.codex/skills/yoolang-optimization/references/performance-workflow.md`
- `src/pass/oir/OIRInlinePass.cpp` at eligibility, call scan, clone, specialization, and cost ranges
- `src/pass/oir/OIROptimizationPipelinePass.cpp` at current call cleanup windows
- `include/oir/OIR.h` and only the recorded `src/oir/OIR.cpp` function-creation,
  ownership/use-list, function-set/body-swap, destruction, and verifier ranges
- `src/pass/mir/MIRAddressModeCombinePass.cpp` only at the saved-address rewrite/use-count range
