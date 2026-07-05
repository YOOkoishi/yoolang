# Task: Cost Model Implementation

Status: scoped
Created: 2026-07-05
Last update: 2026-07-05
Owner: Codex
Branch: not used yet
Base commit: 5e40c44

## Goal

Fully implement the cost model described in `docs/cost-model-design.md`: shared cost data
types, OIR/MIR static estimators, diagnostic output, selected OIR/MIR pass gates, SMT proof
integration, partial-evaluation candidates, e-graph extraction, and perf-report calibration.

The task must be executable by Codex `/goal` as a sequence of bounded checkpoints. Each
checkpoint must leave enough context in this file for the next run to continue without
rescanning the repository.

## Non-goals

- Do not add `-O2` or `-O3`; the cost model serves the existing `-O1` competition pipeline.
- Do not use source filename, testcase name, function name, variable name, literal string, input
  size, expected output, or runtime input data as an optimization trigger.
- Do not let cost model profitability override legality. Unknown, timeout, or failed proof means
  reject or defer.
- Do not gate canonicalization, DCE/ADCE, verifier-only, diagnostics-only, or no-tradeoff CFG
  cleanup passes unless a real profitability tradeoff appears.
- Do not tune weights from a single benchmark identity. Tune only generic risk/benefit categories.

## Affected Pipeline

- [x] Docs only
- [ ] Parser / AST
- [x] YIR
- [x] OIR
- [x] MIR
- [x] ASM
- [ ] Runtime
- [x] Test infrastructure
- [x] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: max 2, starting with `docs/cost-model-design.md`
- Source/script anchors: max 8 promoted anchors per checkpoint
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- `runtime/`
- unrelated frontend/parser/type-system files
- unrelated archived perf history
- broad full-directory scans outside `include/pass`, `src/pass`, `src/main`, `scripts`, and focused
  tests

Checkpoint expansion rule:

- Each checkpoint may add at most 4 new `keep=yes` anchors.
- If more anchors are needed, finish the current checkpoint with `Status: blocked` or split the
  next checkpoint into a smaller patch.
- Every promoted source anchor must state the exact pass, API, or verifier reason that makes it
  necessary.

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol, branch and checkpoint expectations | yes | required on every resume |
| `docs/cost-model-design.md` | headings plus `1-140`, `112-714`, `714-940` | source design for architecture, data types, pass order, diagnostics, perf loop, compliance, milestones | yes | primary requirements document |
| `docs/tasks/TEMPLATE.md` | full | required task record shape | no | used only to create this file |
| `docs/tasks/README.md` | full | active task registration format | yes | update status/branch/date as task advances |
| `src/main/main.cpp` | `49-234`, `261-560` | CLI options, emit-mode restrictions, OIR/MIR pipeline wiring, diagnostics artifact printing | yes | first implementation anchor for `--emit-cost-model` |
| `rg --files include src scripts test \| rg "(main\\.cpp\|OIRInlinePass\|OIR.*Pass\|MIR.*Pass\|compare_perf\|Diagnostics\|Metrics)"` | query | locate candidate pass/script anchors without broad reading | no | promote exact files per checkpoint |
| `/home/yoo/.codex/skills/yoolang-optimization/SKILL.md` | full | contest compliance, validation, perf-report workflow | yes | external instruction, not a repo file |

## Branch

Decision: defer branch until implementation starts

Reason:

```text
This turn creates only the task record. The implementation is broad and should start from a
dedicated branch before any source patch. Current branch is master; current untracked file
`basic.ll` is unrelated and must not be touched.
```

Commands already run:

```bash
git status --short
# ?? basic.ll
git rev-parse --short HEAD
# 5e40c44
git branch --show-current
# master
```

Command to run before the first source patch:

```bash
git checkout -b task/cost-model-implementation
```

## Invariants And Risks

Correctness invariants:

- Every accepted transform must have `legal == true` and a proven or structurally justified
  semantic invariant before profitability is evaluated.
- `TransformCandidate::scope` describes IR scope such as function, loop, block, instruction slice,
  or e-class; it must never encode testcase identity.
- Plain `-S -O1` remains quiet unless a cost-model diagnostic flag is passed.
- `--emit-cost-model` and `--emit-cost-model=json` reproduce decisions without changing generated
  code beyond decisions already enabled by the selected policy.
- Cost model decisions must remain deterministic for the same IR, target profile, and policy.
- SMT timeout/unknown, e-graph budget exhaustion with incomplete proof, or PE proof uncertainty must
  reject or defer the candidate.

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes,
  input values, or expected outputs.
- Do not use unproved undefined-behavior assumptions.
- Use only IR structure, types, SSA use-def, dominance, loop structure, value ranges,
  alias/dependence proof, target profile, and generic pass metrics.

Risk areas:

- OIR inlining/specialization can increase code size and register pressure before MIR cleanup.
- If-conversion and LICM can reduce branches/instructions while increasing live ranges and spills.
- MIR PreRA improvements can be erased by PostRA spills; calibration must compare `pre-ra`,
  `post-ra`, and `final`.
- SMT, PE, and e-graph can increase compile time; all must have budgets and timeout accounting.
- JSON diagnostics can become unstable if candidate IDs depend on pointer values or iteration over
  unordered containers.

## Goal Execution Contract

For `/goal`, execute checkpoints in order. Do not skip a checkpoint unless this file is updated
with an explicit reason and the skipped work is genuinely unnecessary.

Each checkpoint must end by updating:

- `Status`
- `Context Ledger`
- `Patch Queue`
- `Stage Checkpoints`
- `Verification Matrix`
- `Change Log`
- `Handoff Note`

Checkpoint completion criteria:

- Source compiles if source changed.
- Focused verifier/test gate for the touched layer has run or is explicitly marked not applicable.
- Any generated cost-model JSON is parsed by a test or a simple JSON consumer.
- Any transform-gating change includes an accept and reject path test when feasible.
- Performance-affecting checkpoints include a focused `compare_perf.py` run; final checkpoint
  includes the full performance set unless the environment blocks it.

## Stage Plan

| Checkpoint | Deliverable | Main anchors to promote | Required result |
| --- | --- | --- | --- |
| C0 | Branch setup and source survey | `src/main/main.cpp`, pass manager/context APIs | task branch exists, first 8 anchors selected |
| C1 | Shared cost model core | `include/pass/CostModel.h`, `src/pass/CostModel.cpp` | data types, policies, decision engine, unit/FileCheck-style coverage |
| C2 | Read-only collectors and CLI diagnostics | `src/main/main.cpp`, OIR/MIR collector files, diagnostics artifact path | `--emit-cost-model=json` works and does not change codegen |
| C3 | OIR pass integration batch 1 | `OIRInlinePass`, constant argument specialization, OIR pipeline | accept/reject trace for inline/specialization with policy gates |
| C4 | OIR pass integration batch 2 | OIR if-conversion/loop transform anchors | cost gates for high-growth loop/if transforms, no gate on no-tradeoff cleanup |
| C5 | MIR pass integration | MIR LICM/CSE, small-if, scheduler, diagnostics | PreRA decisions calibrated against PostRA/final metrics |
| C6 | SMT proof-gate integration | SMT obligation/proof API and one guarded i32 rewrite provider | timeout/cache/proven/refuted states appear in decision trace |
| C7 | Partial evaluation integration | specialization/clone machinery, SCCP/GVN/DCE cleanup window | clone/code-growth/specialization budgets gate PE candidates |
| C8 | E-graph extraction integration | OIR expression slice/e-graph provider | extraction uses cost vector and budgets, not minimum node count |
| C9 | Perf calibration and finalization | `scripts/compare_perf.py`, `docs/cost-model-calibration.md` | perf report includes decision summary and calibration notes |

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| C0 | Create implementation branch and promote exact anchors for C1/C2 | task file only | `git status --short` | pending | Do before source edits |
| C1.1 | Add shared public cost model definitions | `include/pass/CostModel.h`, `src/pass/CostModel.cpp`, build config if needed | `xmake` | pending | No pass behavior change |
| C1.2 | Add policy defaults and decision scoring | same core files | focused cost model test or JSON smoke | pending | Balanced is default for `-O1` |
| C2.1 | Add pass-context artifact and read-only OIR/MIR collectors | cost model core plus collector files | OIR/MIR stage focused tests | pending | Must not affect generated ASM |
| C2.2 | Add CLI flags `--emit-cost-model`, `--emit-cost-model=json`, `--cost-model-filter`, `--cost-model-policy` | `src/main/main.cpp` plus diagnostic printer | CLI smoke, JSON parse | pending | Keep mutually exclusive emit rules explicit |
| C3.1 | Gate OIR inline and constant-argument specialization through cost decisions | `src/pass/oir/OIRInlinePass.cpp`, OIR cost model | focused FileCheck/e2e | pending | Include accept/reject traces |
| C4.1 | Gate OIR if-conversion and loop transforms with code-growth/live-range risks | OIR transform anchors | OIR stage and e2e focused tests | pending | Promote exact files after survey |
| C5.1 | Gate MIR LICM/CSE/small-if/scheduler with PreRA risk and PostRA metrics | MIR pass anchors, MIR cost model | MIR/ASM stage focused tests | pending | Attribute PreRA to PostRA/final deltas |
| C6.1 | Add SMT obligation/proof result API and guarded i32 provider | SMT/proof anchors | FileCheck/e2e with timeout and unknown cases | pending | SMT is ProofGate only |
| C7.1 | Add PE candidate and budget integration | PE/specialization anchors | OIR stage/e2e, JSON trace | pending | Cleanup dependency must affect risk |
| C8.1 | Add e-graph expression-slice provider and cost extraction | e-graph anchors | FileCheck/e2e, budget tests | pending | Reject incomplete/untrusted proof |
| C9.1 | Extend perf report with cost-model summaries | `scripts/compare_perf.py` | focused perf smoke | pending | accepted/rejected counts by kind/reason |
| C9.2 | Full validation, calibration doc, final handoff | `docs/cost-model-calibration.md`, task file | full optimized tests and perf set | pending | Move status to `ready_for_review` only after gates |

## Stage Checkpoints

Use this table as the durable checkpoint log. Add one row after each completed checkpoint.

| Checkpoint | Status | Summary | Changed files | Verification | Perf / metrics evidence | Next read anchors |
| --- | --- | --- | --- | --- | --- | --- |
| C0 | pending | Not started | none | NOT_RUN | none | `docs/task-system.md`, this task, `docs/cost-model-design.md`, `src/main/main.cpp` |
| C1 | pending | Not started | none | NOT_RUN | none | TBD after C0 |
| C2 | pending | Not started | none | NOT_RUN | none | TBD after C1 |
| C3 | pending | Not started | none | NOT_RUN | none | TBD after C2 |
| C4 | pending | Not started | none | NOT_RUN | none | TBD after C3 |
| C5 | pending | Not started | none | NOT_RUN | none | TBD after C4 |
| C6 | pending | Not started | none | NOT_RUN | none | TBD after C5 |
| C7 | pending | Not started | none | NOT_RUN | none | TBD after C6 |
| C8 | pending | Not started | none | NOT_RUN | none | TBD after C7 |
| C9 | pending | Not started | none | NOT_RUN | none | TBD after C8 |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | every source checkpoint | NOT_RUN | |
| Cost-model JSON smoke | `<compiler> <case.sy> --emit-cost-model=json -O1 | python3 -m json.tool >/tmp/cost-model.json` | after C2 and later diagnostic changes | NOT_RUN | use actual compiler path from build |
| Cost-model quiet codegen | compare `-S -O1` output before/after enabling diagnostics off | after C2, C3-C8 | NOT_RUN | plain codegen must stay quiet |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter cost_model --jobs 1` | after adding tests | NOT_RUN | create focused tests as implementation lands |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter <case> --jobs 1 --o1` | OIR checkpoints | NOT_RUN | |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter <case> --jobs 1 --o1` | MIR checkpoints | NOT_RUN | |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter <case> --jobs 1 --o1` | MIR/backend or codegen decision checkpoints | NOT_RUN | |
| E2E focused | `python3 scripts/run_tests.py --suite e2e --filter <case> --jobs 1 --o1` | behavior-affecting checkpoints | NOT_RUN | |
| MIR metrics | `<compiler> <case.sy> --emit-mir-metrics -O1` | C5 and C9 | NOT_RUN | compare pre-ra/post-ra/final |
| Focused performance | `PERF_TEST_DIRS=<focused-dir> python3 scripts/compare_perf.py` | C3-C9 if decisions affect generated code | NOT_RUN | no `PERF_MAX_CASES` as final evidence |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before finalization | NOT_RUN | |
| Full performance | `PERF_TEST_DIRS=test/performance,test/bsb-final python3 scripts/compare_perf.py` | before finalization | NOT_RUN | inspect `build/perf-ci/perf-report.md/json` |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| One large implementation patch | Minimizes task-record overhead | rejected: too broad for `/goal` recovery and too hard to verify |
| Diagnostics-only cost model | Low-risk first step | chosen only for C1-C2; final task requires actual pass gating |
| Gate all passes immediately | Appears complete quickly | rejected: no-tradeoff passes should only record metrics, and high-risk gates need staged tests |
| Make SMT/e-graph mandatory before OIR/MIR pass gates | Strong proof story | rejected: handwritten structural proofs can gate early passes; SMT/e-graph are later checkpoints |
| Tune from slow benchmark names | Fast local improvement | rejected: contest-noncompliant and explicitly outside design |

## Checkpoint Handoff Template

Append this shape to `Stage Checkpoints` and update `Handoff Note` at the end of every checkpoint:

```text
Checkpoint:
Implemented:
Legality invariant:
Profitability/risk rule:
Files changed:
Tests run:
Perf evidence:
Decision trace examples:
Regressions or skipped gates:
Next exact anchors:
Next command:
```

## Change Log

- 2026-07-05: created scoped multi-checkpoint task from `docs/cost-model-design.md`.

## Open Questions

- Empty. Later checkpoints should add questions only when implementation discovers a real API or
  legality ambiguity.

## Handoff Note

Current state:

- This task is scoped but implementation has not started.
- `docs/cost-model-design.md` is the source design and remains a `keep=yes` anchor.
- `src/main/main.cpp` is the first implementation anchor for CLI flags, emit-mode behavior,
  pipeline wiring, MIR diagnostics artifacts, and JSON output style.
- Current branch is `master`; base commit is `5e40c44`; unrelated untracked `basic.ll` exists and
  must not be touched.

Next action:

- Create `task/cost-model-implementation`, update this task's `Branch`, then execute C0 by reading
  only the keep=yes anchors and promoting exact C1/C2 source anchors.

Read next:

- `docs/task-system.md`
- `docs/tasks/2026-07-05-cost-model-implementation.md`
- `docs/cost-model-design.md`
- `src/main/main.cpp`
