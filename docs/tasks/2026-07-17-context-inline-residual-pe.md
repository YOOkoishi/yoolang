# Task: Context-Sensitive Callsite Inlining And Transactional Residual Partial Evaluation

Status: scoped
Created: 2026-07-17
Last update: 2026-07-17
Owner: Codex task-generation agent
Branch: `task/context-inline-residual-pe` (planned)
Base commit: `de93e24`

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
- The required `pseudo_md5`, complete `test/performance`, and current CI-parity comparisons show no
  unattributed meaningful regression. Full optimized correctness passes.
- No final performance claim uses `PERF_MAX_CASES` or an incomplete/timeout report.

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
- [ ] MIR
- [ ] ASM
- [ ] Runtime
- [ ] Test infrastructure
- [x] Performance

MIR and ASM optimized-stage checks remain downstream verification for an OIR transform; no MIR/ASM
production source is in scope.

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: exactly 2:
  - `/home/yoo/.codex/skills/yoolang-optimization/references/compiler-state.md`, needed for the live
    OIR pipeline, cost-model order, verifier, and downstream metric stages;
  - `/home/yoo/.codex/skills/yoolang-optimization/references/performance-workflow.md`, needed for
    isolated same-machine runs, report preservation/provenance, CI-parity scope, and timeout rules.
- Source/script anchors: at most 8 `keep=yes` files at one time
- Focused test fixtures: at most 3 at one time; drop a completed fixture before opening another
- Large-file rule: read only named functions/queries unless this ledger is updated first

The previous Huffman task records are not required implementation context: their relevant diagnosis
and rejected alternatives are preserved below. Do not reopen them unless new evidence contradicts
this self-contained record.

Do not read unless a recorded blocker requires it:

- AST, YIR, DAE, `OIRAnalysis.cpp`, either OIR-to-MIR lowerer, MIR/ASM implementation, runtime;
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
| `src/oir/OIR.cpp` | `Function` constructor and function creation only | metadata implementation/name-allocation snapshot | yes | drop after P1b if another anchor is needed |
| `include/pass/oir/OIRCostModel.h` | `OIRTransformCostEstimate` | actual residual metrics and risk | yes | keep estimate shape OIR-specific |
| `src/pass/oir/OIRCostModel.cpp` | estimate -> shared candidate adapter | residual/cost diagnostics | yes | no legality bypass |
| `include/pass/CostModel.h` | `PartialEvalCost`, risk/policy fields only | reuse shared cost fields/budgets | yes | do not read solver/e-graph sources |
| `scripts/compare_perf.py` | only Clang flags, report overwrite, selection env if HEAD changes | live perf behavior | conditional | promote by dropping `src/oir/OIR.cpp` after P1 |
| `.github/workflows/test.yml` | current perf dirs/exclusions only | CI-parity selection | conditional | read immediately before CI-parity runs |
| `test/performance/huffman-01.sy` | discovery source only | artifact attribution | no | never copy names/constants/shape into selectors |
| `build/perf-ci/perf-report.{json,md}` | current three rows and cost summaries | orientation evidence | no | P0 must create isolated reports |

## Starting State And Ownership

### Current repository state

The 2026-07-17 independent scope review observed exactly these five dirty paths:

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

### Protected-path orientation and P0 manifest contract

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

Decision: create `task/context-inline-residual-pe` after P0 creates and verifies the initial
protection evidence above. There is no need to wait for another owner once the five-path ownership
table and manifest checks pass; a normal branch checkout carries the dirty files without rewriting
them.

Safe sequence:

```bash
git status --short
git rev-parse --short HEAD
git branch --show-current
sha256sum -c build/perf-ci/context-inline-residual-pe/protection/initial-sha256.txt
git status --short | cmp - build/perf-ci/context-inline-residual-pe/protection/initial-status.txt
git diff --numstat -- docs/README.md docs/egraph-design.md | cmp - build/perf-ci/context-inline-residual-pe/protection/initial-docs-readme-numstat.txt
git diff --binary -- docs/README.md | sha256sum | cmp - build/perf-ci/context-inline-residual-pe/protection/initial-docs-readme-diff.sha256
awk 'index($0, "docs/tasks/2026-07-17-context-inline-residual-pe.md") == 0' docs/tasks/README.md | cmp - build/perf-ci/context-inline-residual-pe/protection/initial-task-index-excluding-this-row.txt
test "$(rg -cF 'docs/tasks/2026-07-17-context-inline-residual-pe.md' docs/tasks/README.md)" -eq 1
git checkout -b task/context-inline-residual-pe
git status --short
git branch --show-current
sha256sum -c build/perf-ci/context-inline-residual-pe/protection/initial-sha256.txt
git status --short | cmp - build/perf-ci/context-inline-residual-pe/protection/initial-status.txt
git diff --numstat -- docs/README.md docs/egraph-design.md | cmp - build/perf-ci/context-inline-residual-pe/protection/initial-docs-readme-numstat.txt
git diff --binary -- docs/README.md | sha256sum | cmp - build/perf-ci/context-inline-residual-pe/protection/initial-docs-readme-diff.sha256
awk 'index($0, "docs/tasks/2026-07-17-context-inline-residual-pe.md") == 0' docs/tasks/README.md | cmp - build/perf-ci/context-inline-residual-pe/protection/initial-task-index-excluding-this-row.txt
test "$(rg -cF 'docs/tasks/2026-07-17-context-inline-residual-pe.md' docs/tasks/README.md)" -eq 1
```

After checkout, the branch must be `task/context-inline-residual-pe`, HEAD must still be `de93e24`,
the same five dirty paths must remain before implementation, both Active Tasks rows must remain, and
all manifest-driven byte/diff/index checks must pass. If the branch already exists, verify its
base/diff and use `git checkout task/context-inline-residual-pe` only when the same protection
checks pass.

Never stash, reset, clean, restore, stage, commit, or overwrite the user/independent-task paths as a
branching shortcut. Production commits must exclude them unless their owner separately authorizes
and scopes that action.

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
| P0 | Before checkout generate the initial protection manifest/snapshots; after checkout freeze all required `de93e24` baseline reports/provenance and create the auditable Clang 22 artifact set | none | Protection, baseline build/perf, and Clang rows in Verification Matrix | pending | Protection evidence must exist before checkout; all P0 work finishes before source edits; no `PERF_MAX_CASES`. |
| P1a | Add minimal OIR `FunctionOrigin`, stable function ID, and stable root ID with default-original creation/copy APIs | `include/oir/OIR.h`, `src/oir/OIR.cpp` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_function_origin --jobs 1` | pending | No AST/YIR/entrypoint/intrinsic metadata. |
| P1b | Replace `__yo_constprop.*` prefix decisions/counting with origin/root lineage while retaining generated names only for display/collision avoidance | `src/pass/oir/OIRInlinePass.cpp` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_function_origin --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_function_origin --jobs 1 --o1` | pending | Same spelling/different origin must not change behavior. |
| P2 | Add lightweight direct-call graph/SCC context and deterministic worklist with tie-batch/global-budget policy | `src/pass/oir/OIRInlinePass.cpp`, optional `include/pass/oir/OIRInlineWorklist.h`, optional `src/pass/oir/OIRInlineWorklist.cpp` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_worklist --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_context_inline_worklist --jobs 1 --o1` | pending | General loop depth/constants/dead return/dynamic-call facts only. |
| P3 | Add bounded affected cleanup after an accepted inline and requeue every function/callsite actually changed | `src/pass/oir/OIRInlinePass.cpp`, `src/pass/oir/OIROptimizationPipelinePass.cpp`, optional one cleanup helper | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_cleanup_budget --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_context_inline_cleanup_budget --jobs 1 --o1` | pending | Bound windows/rounds; SCCP, branches, CFG, DCE/ADCE, VRP, GVN, safe global promotion. |
| P4 | Build a detached scratch clone and substitute exact constant-lattice arguments without live-module ownership | `src/pass/oir/OIRInlinePass.cpp`, optional `include/pass/oir/OIRResidualPE.h`, optional `src/pass/oir/OIRResidualPE.cpp` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_clone --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_context_inline_residual_pe_clone --jobs 1 --o1` | pending | Unsupported instructions fail closed; zero solver queries. |
| P5 | Add explicit scalar `ReturnDemandMask` and side-effect-aware dead-return slicing on scratch IR | residual helper/header, `src/pass/oir/OIRInlinePass.cpp` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_return_demand --jobs 1`; `python3 scripts/run_tests.py --suite e2e --filter context_inline_residual_pe_return_demand --jobs 1 --o1` | pending | Dead versus used return; global/alias/side-effect negatives. |
| P6 | Measure verified post-cleanup residual metrics and integrate them into the OIR cost request/rejection diagnostic | `src/pass/oir/OIRInlinePass.cpp`, `include/pass/oir/OIRCostModel.h`, `src/pass/oir/OIRCostModel.cpp` | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter cost_model_pe --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_cost --jobs 1` | pending | Record before/residual/eliminated/cleanup/pressure; cost reject before commit. |
| P7 | Implement atomic direct residual commit and measured persistent-clone reuse semantics | `src/pass/oir/OIRInlinePass.cpp`, residual helper/header | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_commit --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --filter oir_context_inline_residual_pe_commit --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e --filter context_inline_residual_pe_commit --jobs 1 --o1` | pending | Direct callsite first; persistent clone uses origin/root, not name. |
| P8 | Prove rollback for cost reject, scratch cleanup budget exhaustion, unsupported/error, verifier failure, and commit-preflight failure | `src/pass/oir/OIRInlinePass.cpp`, residual helper/header | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_rollback --jobs 1`; `python3 scripts/run_tests.py --suite e2e --filter context_inline_residual_pe_rollback --jobs 1 --o1` | pending | Check module/use/name/stats/growth snapshots and control candidate numbering. |
| P9 | Add cumulative module/root growth and live-pointer/memory/register/spill-pressure limits to inline and residual commit | `src/pass/oir/OIRInlinePass.cpp`, `include/pass/oir/OIRCostModel.h`, at most one shared cost header | `xmake`; `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_pressure --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --suite e2e --filter crypto- --jobs 1 --o1` | pending | Preserve large mutable-memory guard; tie batches preflight combined growth. |
| P10 | Add the complete generic metamorphic/signed/alias/SCC test battery | no production files | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline --jobs 1`; `python3 scripts/run_tests.py --suite stage --stage oir --stage mir --stage asm --filter context_inline --jobs 1 --o1`; `python3 scripts/run_tests.py --suite e2e --filter context_inline_residual_pe --jobs 1 --o1` | pending | No motivating identifiers in transform-shape tests. |
| P11 | Run full correctness and every required sequential baseline/current performance row, preserve reports, generate four explicit deltas, inspect results, and update task | none | complete Verification Matrix | pending | Required timeout/failure keeps task out of `ready_for_review`. |

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

All results start `NOT_RUN`. This task-generation revision runs no build/test/performance command.

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Pre-check dirty ownership | `git status --short && git rev-parse --short HEAD && git branch --show-current` | yes | NOT_RUN | Must match five-path table, `de93e24`, `master` before first checkout. |
| P0 initial protection evidence | generate all five files in `protection/` using the exact commands under `Protected-path orientation and P0 manifest contract` | yes, before checkout | NOT_RUN | Generated from implementation-start bytes; prose hashes are orientation only. |
| Pre-check protected bytes/diff/index | `sha256sum -c build/perf-ci/context-inline-residual-pe/protection/initial-sha256.txt`; compare current status, user diff/numstat, and filtered task index with their initial files using the Branch `cmp` commands | yes | NOT_RUN | Closes all three immutable paths, the tracked user diff, all non-this-task index bytes, and the exact five-path starting status. |
| Post-checkout state | rerun every manifest/`cmp` command in the Branch safe sequence after checkout | yes | NOT_RUN | Branch becomes task branch; HEAD, five dirty paths, protected bytes/diff, and every non-this-task index byte remain unchanged. |
| P0 Clang 22 diagnosis | run all commands in `P0 Auditable Clang 22 Reproduction` and save every listed artifact | yes before source edits | NOT_RUN | Confirm version/flags/source provenance, helper backedge/32-trip loop, non-whole-loop bitop, and nested-inline evidence; diagnosis only. |
| Build | `xmake` | yes after each source patch | NOT_RUN | OIR-only implementation. |
| Origin FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_function_origin --jobs 1` | yes | NOT_RUN | Minimal OIR origin/root lineage; no entry/intrinsic checks. |
| Worklist FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_worklist --jobs 1` | yes | NOT_RUN | Context, nested requeue, SCC, tie batch, reorder/rename invariance. |
| Cleanup FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_cleanup_budget --jobs 1` | yes | NOT_RUN | Bounded affected cleanup and live cap behavior. |
| Scratch clone FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_clone --jobs 1` | yes | NOT_RUN | Detached ownership and exact substitution. |
| Return-demand FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_return_demand --jobs 1` | yes | NOT_RUN | Dead/used return and side-effect slicing. |
| Residual-cost FileChecks | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_cost --jobs 1`; `python3 scripts/run_tests.py --suite filecheck --filter cost_model_pe --jobs 1` | yes | NOT_RUN | Actual residual metrics, cost reject, zero SMT. |
| Commit FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_commit --jobs 1` | yes | NOT_RUN | Direct commit and measured persistent reuse. |
| Rollback FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_rollback --jobs 1` | yes | NOT_RUN | Reject/budget/error/verifier/preflight paths; all live snapshots equal. |
| Pressure FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter oir_context_inline_residual_pe_pressure --jobs 1` | yes | NOT_RUN | Module/root growth, pointers/memory/live/spill proxies. |
| Focused OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter context_inline --jobs 1 --o1` | yes | NOT_RUN | OIR verifier for all common-stem fixtures. |
| Focused downstream O1 | `python3 scripts/run_tests.py --suite stage --stage mir --stage asm --filter context_inline --jobs 1 --o1` | yes | NOT_RUN | Downstream O1 verification only; no stack/non-O1 ABI gate. |
| Focused semantic e2e | `python3 scripts/run_tests.py --suite e2e --filter context_inline_residual_pe --jobs 1 --o1` | yes | NOT_RUN | Signed extremes, side effects, rollback/control output. |
| Huffman stages/e2e | `python3 scripts/run_tests.py --suite stage --stage oir --stage mir --stage asm --suite e2e --filter huffman-0 --jobs 1 --o1` | yes | NOT_RUN | Three explicit files; validation only. |
| `pseudo_md5` stages/e2e | `python3 scripts/run_tests.py --suite stage --stage oir --stage mir --stage asm --suite e2e --filter crypto- --jobs 1 --o1` | yes | NOT_RUN | Inspect clone/origin and MIR spill/stack metrics, not only PASS. |
| Full FileCheck/poly | `python3 scripts/run_tests.py --suite filecheck --suite poly --jobs 1` | yes | NOT_RUN | Required before broad perf. |
| Full downstream correctness | `python3 scripts/run_tests.py --suite stage --suite e2e --jobs 1 --o1` | yes | NOT_RUN | Required O1 downstream coverage. |
| Full optimized correctness | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | yes | NOT_RUN | Report every skip/failure. |
| Final immutable-path and user-diff gate | `sha256sum -c build/perf-ci/context-inline-residual-pe/protection/initial-sha256.txt`; current user numstat and binary-diff SHA stream compared with the corresponding initial files using `cmp` | yes before review | NOT_RUN | Manifest check covers both user files and the independent guarded-small-loop task file; comparisons read P0 evidence, not prose hashes. |
| Final shared-index/ownership gate | filter only this task's row from `docs/tasks/README.md` and `cmp` against `initial-task-index-excluding-this-row.txt`; `git status --short`; inspect `git diff -- docs/tasks/README.md` and this task row | yes before review | NOT_RUN | Every other index byte, including the guarded-small-loop row, remains exact; only this task row may reflect its status/branch. |

The exact final protected-path closure is:

```bash
sha256sum -c build/perf-ci/context-inline-residual-pe/protection/initial-sha256.txt
git diff --numstat -- docs/README.md docs/egraph-design.md | cmp - build/perf-ci/context-inline-residual-pe/protection/initial-docs-readme-numstat.txt
git diff --binary -- docs/README.md | sha256sum | cmp - build/perf-ci/context-inline-residual-pe/protection/initial-docs-readme-diff.sha256
awk 'index($0, "docs/tasks/2026-07-17-context-inline-residual-pe.md") == 0' docs/tasks/README.md | cmp - build/perf-ci/context-inline-residual-pe/protection/initial-task-index-excluding-this-row.txt
test "$(rg -cF 'docs/tasks/2026-07-17-context-inline-residual-pe.md' docs/tasks/README.md)" -eq 1
git status --short
git diff -- docs/tasks/README.md
```

`initial-status.txt` is an exact pre/post-checkout gate; it is intentionally not compared at final
review because authorized implementation files and this task record will then differ. Final status
is inspected against the declared production/test scope, while immutable protected bytes and all
non-this-task index bytes remain machine-compared to their P0 evidence.

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
    initial-sha256.txt
    initial-docs-readme-diff.sha256
    initial-docs-readme-numstat.txt
    initial-status.txt
    initial-task-index-excluding-this-row.txt
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
| Baseline release/provenance | workdir `/tmp/yoolang-context-inline-residual-pe-de93e24`: `git rev-parse HEAD`; `git status --short`; `xmake f -m release`; `xmake`; hash compiler/runtime/script and write `baseline/provenance.md` | yes, before source edits | NOT_RUN | Commit must be `de93e24`; record actual compiler/runtime paths and hashes. |
| Current release/provenance | workdir current: `git rev-parse HEAD`; `git status --short`; `xmake f -m release`; `xmake`; hash compiler/runtime/script and write `current/provenance.md` | yes, after implementation | NOT_RUN | Compiler/runtime provenance must be explicit and comparable. |
| Baseline Huffman | baseline workdir: `COMPILER_BIN=/tmp/yoolang-context-inline-residual-pe-de93e24/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/tmp/yoolang-context-inline-residual-pe-de93e24/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance/huffman-01.sy,test/performance/huffman-02.sy,test/performance/huffman-03.sy python3 scripts/compare_perf.py`; save JSON/MD to `baseline/huffman/` | yes | NOT_RUN | All 3 rows complete/OK; then produce Clang P0 artifacts. |
| Current Huffman | current workdir: `COMPILER_BIN=/home/yoo/Documents/Compliers/yoolang/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/home/yoo/Documents/Compliers/yoolang/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance/huffman-01.sy,test/performance/huffman-02.sy,test/performance/huffman-03.sy python3 scripts/compare_perf.py`; save JSON/MD to `current/huffman/` | yes | NOT_RUN | All 3 OK; >=10% Yoolang geomean improvement; per-case threshold above. |
| Huffman delta | current workdir: `python3 scripts/compare_perf_baseline.py --current build/perf-ci/context-inline-residual-pe/current/huffman/perf-report.json --baseline build/perf-ci/context-inline-residual-pe/baseline/huffman/perf-report.json --out-md build/perf-ci/context-inline-residual-pe/delta/huffman/perf-delta.md --out-json build/perf-ci/context-inline-residual-pe/delta/huffman/perf-delta.json --out-insn-json build/perf-ci/context-inline-residual-pe/delta/huffman/instruction-count-compare.json --baseline-label de93e24` | yes | NOT_RUN | Inspect all rows; script exit 0 alone is not PASS. |
| Baseline `pseudo_md5` | baseline workdir: `COMPILER_BIN=/tmp/yoolang-context-inline-residual-pe-de93e24/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/tmp/yoolang-context-inline-residual-pe-de93e24/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance/crypto-1.sy,test/performance/crypto-2.sy,test/performance/crypto-3.sy python3 scripts/compare_perf.py`; save JSON/MD to `baseline/pseudo-md5/` | yes | NOT_RUN | Record MIR, clone decisions, spills, stack slots, loads/stores, runtime. |
| Current `pseudo_md5` | current workdir: `COMPILER_BIN=/home/yoo/Documents/Compliers/yoolang/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/home/yoo/Documents/Compliers/yoolang/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance/crypto-1.sy,test/performance/crypto-2.sy,test/performance/crypto-3.sy python3 scripts/compare_perf.py`; save JSON/MD to `current/pseudo-md5/` | yes | NOT_RUN | No new committed residual clone, spill/stack growth, or meaningful runtime regression. |
| `pseudo_md5` delta | current workdir: `python3 scripts/compare_perf_baseline.py --current build/perf-ci/context-inline-residual-pe/current/pseudo-md5/perf-report.json --baseline build/perf-ci/context-inline-residual-pe/baseline/pseudo-md5/perf-report.json --out-md build/perf-ci/context-inline-residual-pe/delta/pseudo-md5/perf-delta.md --out-json build/perf-ci/context-inline-residual-pe/delta/pseudo-md5/perf-delta.json --out-insn-json build/perf-ci/context-inline-residual-pe/delta/pseudo-md5/instruction-count-compare.json --baseline-label de93e24` | yes | NOT_RUN | Inspect codegen/cost rows, not timing alone. |
| Baseline `test/performance` | baseline workdir: `COMPILER_BIN=/tmp/yoolang-context-inline-residual-pe-de93e24/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/tmp/yoolang-context-inline-residual-pe-de93e24/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py`; save JSON/MD to `baseline/test-performance/` | yes | NOT_RUN | Required complete 60-case-or-current-count scope; record count. |
| Current `test/performance` | current workdir: `COMPILER_BIN=/home/yoo/Documents/Compliers/yoolang/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/home/yoo/Documents/Compliers/yoolang/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance python3 scripts/compare_perf.py`; save JSON/MD to `current/test-performance/` | yes | NOT_RUN | Required; inspect all meaningful per-case and MIR/spill changes. |
| `test/performance` delta | current workdir: `python3 scripts/compare_perf_baseline.py --current build/perf-ci/context-inline-residual-pe/current/test-performance/perf-report.json --baseline build/perf-ci/context-inline-residual-pe/baseline/test-performance/perf-report.json --out-md build/perf-ci/context-inline-residual-pe/delta/test-performance/perf-delta.md --out-json build/perf-ci/context-inline-residual-pe/delta/test-performance/perf-delta.json --out-insn-json build/perf-ci/context-inline-residual-pe/delta/test-performance/instruction-count-compare.json --baseline-label de93e24` | yes | NOT_RUN | Any reported/meaningful regression requires attribution and repair. |
| Baseline CI-parity | baseline workdir: `COMPILER_BIN=/tmp/yoolang-context-inline-residual-pe-de93e24/build/linux/x86_64/release/compiler SYSY_RUNTIME_LIB=/tmp/yoolang-context-inline-residual-pe-de93e24/runtime/libsysy_riscv.a PERF_TEST_DIRS=test/performance,test/bsb-final PERF_EXCLUDE_CASES=test/performance/h-10-02.sy,test/performance/h-10-03.sy,test/bsb-final/2025-CPS-39.sy,test/bsb-final/2025-Z8N-28.sy PERF_TIMEOUT_SEC=20 python3 scripts/compare_perf.py`; save JSON/MD to `baseline/ci-parity/` | yes | NOT_RUN | Re-read live CI first and record exclusions/instruction-count configuration. |
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

## Performance Acceptance Details

- Same-machine Yoolang `de93e24` versus current is the attribution baseline. GCC/Clang ratios are
  external context only.
- Focused Huffman: all three correct, >=10% Yoolang geomean improvement, and no individual case
  slower by both >5% and >0.003 seconds.
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

## Open Questions

- None at scope time. The minimal OIR metadata boundary, rollback contract, tie/global-budget rule,
  required reports, and branch protection flow are fixed. A need for cross-layer metadata or a loop
  superoptimizer opens a separate task rather than expanding this one.

## Handoff Note

Current state:

- This remains a `scoped` task record only. No compiler/test source was changed and no validation
  was run by the task-generation/revision agent.
- HEAD/base is `de93e24`; the five dirty paths and protected-path orientation hashes/diff are
  recorded above. Future verification is driven by the initial P0 manifest/snapshots.
- Scope is OIR worklist + bounded cleanup + detached transactional residual PE + minimal OIR clone
  lineage. Entry/intrinsic/frontend/ABI metadata migration is explicitly excluded.
- README already has the correct `scoped` row and still preserves the independent guarded-loop row;
  this revision did not need an index edit.

Next action:

1. Run the five-path dirty ownership check, then execute P0's initial protection-evidence commands
   before checkout.
2. Verify that evidence with the manifest/`cmp` pre-checks, then create or enter
   `task/context-inline-residual-pe` using the safe branch sequence.
3. Re-run every manifest/`cmp` protection check immediately after checkout.
4. Complete the rest of P0: isolated baseline release/provenance, four required baseline scopes,
   and the task-specific Clang 22 artifact/inspection set. Preserve every JSON/Markdown before P1a.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-17-context-inline-residual-pe.md`
- `/home/yoo/.codex/skills/yoolang-optimization/references/compiler-state.md`
- `/home/yoo/.codex/skills/yoolang-optimization/references/performance-workflow.md`
- `src/pass/oir/OIRInlinePass.cpp` at eligibility, call scan, clone, specialization, and cost ranges
- `src/pass/oir/OIROptimizationPipelinePass.cpp` at current call cleanup windows
- `include/oir/OIR.h` and narrow `src/oir/OIR.cpp` function-creation ranges for P1a only
