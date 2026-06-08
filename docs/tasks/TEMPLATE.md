# Task: <title>

Status: proposed
Created: YYYY-MM-DD
Last update: YYYY-MM-DD
Owner: <name-or-agent>
Branch: <branch-or-not-used>
Base commit: <short-sha>

## Goal

<One or two sentences describing the required outcome.>

## Non-goals

- <What this task must not change.>

## Affected Pipeline

- [ ] Docs only
- [ ] Parser / AST
- [ ] YIR
- [ ] OIR
- [ ] MIR
- [ ] ASM
- [ ] Runtime
- [ ] Test infrastructure
- [ ] Performance

## Context Budget

Initial read budget:

- Task file: yes
- `docs/task-system.md`: yes
- Related docs: <max N and names>
- Source/script anchors: <max N>
- Large-file rule: read only named ranges unless promoted below

Do not read unless explicitly needed:

- <directories or files outside scope>

## Context Ledger

| File | Lines/Query | Why read it | Keep? | Notes |
| --- | --- | --- | --- | --- |
| `docs/task-system.md` | full | task protocol | yes | required |

## Branch

Decision: <used / not used>

Reason:

```text
<why this task needs or does not need a new branch>
```

Commands:

```bash
git status --short
git rev-parse --short HEAD
git checkout -b task/<slug>
```

## Invariants And Risks

Correctness invariants:

- <Semantic rule that must remain true.>

Contest / compliance constraints:

- Do not special-case filenames, function names, variable names, testcase identity, input sizes, or expected outputs.

Risk areas:

- <IR layer, pass, ABI, aliasing, control flow, register allocation, etc.>

## Patch Queue

| Patch | Intent | Files | Verifier/Test | Status | Notes |
| --- | --- | --- | --- | --- | --- |
| P1 | <one behavior point> | <files> | <command> | pending | |

## Verification Matrix

| Gate | Command | Required? | Result | Notes |
| --- | --- | --- | --- | --- |
| Build | `xmake` | yes | NOT_RUN | |
| Focused FileCheck | `python3 scripts/run_tests.py --suite filecheck --filter <case> --jobs 1` | if IR shape changes | NOT_RUN | |
| YIR stage | `python3 scripts/run_tests.py --suite stage --stage yir --filter <case> --jobs 1 --o1` | if YIR affected | NOT_RUN | |
| OIR stage | `python3 scripts/run_tests.py --suite stage --stage oir --filter <case> --jobs 1 --o1` | if OIR affected | NOT_RUN | |
| MIR stage | `python3 scripts/run_tests.py --suite stage --stage mir --filter <case> --jobs 1 --o1` | if MIR affected | NOT_RUN | |
| ASM stage | `python3 scripts/run_tests.py --suite stage --stage asm --filter <case> --jobs 1 --o1` | if backend affected | NOT_RUN | |
| E2E | `python3 scripts/run_tests.py --suite e2e --filter <case> --jobs 1 --o1` | if behavior affected | NOT_RUN | |
| Full optimized | `python3 scripts/run_tests.py --build --suite all --jobs 1 --o1` | before broad finalization | NOT_RUN | |
| Performance | `PERF_TEST_DIRS=<dir> python3 scripts/compare_perf.py` | if performance affected | NOT_RUN | |

## Alternatives

| Option | Why considered | Decision |
| --- | --- | --- |
| <option> | <reason> | <chosen/rejected> |

## Change Log

- YYYY-MM-DD: created task file.

## Open Questions

- <Question, or empty.>

## Handoff Note

Current state:

- <What is true now.>

Next action:

- <The next command or patch.>

Read next:

- `docs/task-system.md`
- `<this task file>`
- <only keep=yes context files>
