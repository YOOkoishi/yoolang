# Clang/LLVM RISC-V `-O3` Pipeline Report

Scope: this report describes the in-tree pipeline used by Clang for a normal
non-LTO compile such as:

```sh
clang -target riscv64-unknown-elf -O3 -c input.c
```

Assumptions:

- LLVM checkout: `bf005a1227a4822c7c2535dd5f5f3626fbe441b2`, `23.0.0git`.
- Baseline mode: no LTO, no PGO/SamplePGO/MemProf, no sanitizers, no pass
  plugins, no debugify, no `-fglobal-isel`, no machine outliner/basic-block
  sections/MFS/IPRA unless stated.
- Default RISC-V code generation uses SelectionDAG, then the legacy codegen
  pass manager.
- Local `build/bin/llc` and `install/bin/llc` were built with
  `LLVM_HAS_RISCV_TARGET 0`, so the RISC-V backend sequence below is derived
  from source, not from executing this local binary. The generic new-PM `-O3`
  pipeline was cross-checked with `opt -O3 -print-pipeline-passes`.

Key source entry points:

- `clang/lib/CodeGen/BackendUtil.cpp:636`: Clang maps frontend optimization
  level `3` to `OptimizationLevel::O3`.
- `clang/lib/CodeGen/BackendUtil.cpp:1125`: for non-LTO, Clang runs
  `PassBuilder::buildPerModuleDefaultPipeline(Level)`.
- `clang/lib/CodeGen/BackendUtil.cpp:1233`: codegen still uses the legacy pass
  manager.
- `llvm/lib/Target/RISCV/RISCVTargetMachine.cpp:481`: RISC-V overrides the
  legacy codegen `TargetPassConfig` hooks.
- `llvm/lib/Target/RISCV/RISCVTargetMachine.cpp:674`: RISC-V registers a new
  pass-manager callback that adds predicated loop idiom vectorization.

## 1. Top-level Clang flow

For `-O3`, Clang builds and runs two major pipelines:

1. New pass manager IR optimization pipeline:
   `PassBuilder::buildPerModuleDefaultPipeline(OptimizationLevel::O3)`.
2. Legacy codegen pipeline:
   `TargetMachine::addPassesToEmitFile`, using `RISCVPassConfig`.

The IR optimization pipeline is mostly target-independent, but RISC-V registers
one target callback into the late loop optimizer extension point:

```cpp
LPM.addPass(LoopIdiomVectorizePass(LoopIdiomVectorizeStyle::Predicated));
```

That pass is inserted for every non-`O0` optimization level, so it runs at
`-O3`.

## 2. New PM IR `-O3` pipeline

This is the normal non-LTO `buildPerModuleDefaultPipeline(O3)` structure.
The pass order below omits optional PGO/LTO/sanitizer/debug/plugin insertions.

### 2.1 Pipeline prologue

1. `MemProfRemoveInfo`
2. `Annotation2MetadataPass`
3. `ForceFunctionAttrsPass`
4. pipeline-start callbacks, if any
5. `buildModuleSimplificationPipeline(O3)`
6. `buildModuleOptimizationPipeline(O3)`
7. `AnnotationRemarksPass`

### 2.2 Module simplification pipeline

Module-level passes:

1. `InferFunctionAttrsPass`
2. `CoroEarlyPass`
3. early function cleanup:
   `EntryExitInstrumenterPass`, `LowerExpectIntrinsicPass`,
   `SimplifyCFGPass`, `SROAPass(modify-cfg)`, `EarlyCSEPass`,
   `CallSiteSplittingPass` (`O3` only)
4. `OpenMPOptPass`
5. `IPSCCPPass`
6. `CalledValuePropagationPass`
7. `GlobalOptPass`
8. global cleanup function pipeline:
   `PromotePass`, `InstCombinePass`, `SimplifyCFGPass`
9. `AlwaysInlinerPass`
10. inliner pipeline
11. `DeadArgumentEliminationPass`
12. `CoroCleanupPass`
13. `GlobalOptPass`
14. `GlobalDCEPass`

The default inliner pipeline is `ModuleInlinerWrapperPass`/CGSCC:

1. require `GlobalsAA`
2. invalidate function `AAManager`
3. require `ProfileSummaryAnalysis`
4. `InlinerPass` inside `devirt<4>`
5. `PostOrderFunctionAttrsPass(skip-non-recursive)`
6. `ArgumentPromotionPass` (`O3` only)
7. `OpenMPOptCGSCCPass`
8. nested function simplification pipeline
9. `PostOrderFunctionAttrsPass`
10. require `ShouldNotRunFunctionPassesAnalysis`
11. `CoroSplitPass`
12. `CoroAnnotationElidePass`

Nested function simplification pipeline:

1. `SROAPass(modify-cfg)`
2. `EarlyCSEPass(memssa)`
3. `SpeculativeExecutionPass(only-if-divergent-target)`
4. `JumpThreadingPass`
5. `CorrelatedValuePropagationPass`
6. `SimplifyCFGPass`
7. `InstCombinePass`
8. `AggressiveInstCombinePass`
9. `LibCallsShrinkWrapPass`
10. `TailCallElimPass`
11. `SimplifyCFGPass`
12. `ReassociatePass`
13. `ConstraintEliminationPass`
14. loop pipeline 1, using MemorySSA:
    `LoopInstSimplifyPass`, `LoopSimplifyCFGPass`, `LICMPass(no speculation)`,
    `LoopRotatePass`, `LICMPass(allow speculation)`,
    `SimpleLoopUnswitchPass(nontrivial at O3)`
15. `SimplifyCFGPass`
16. `InstCombinePass`
17. loop pipeline 2:
    `LoopIdiomRecognizePass`, `IndVarSimplifyPass`,
    `ExtraSimpleLoopUnswitchPasses`,
    `LoopIdiomVectorizePass(Predicated)` from the RISC-V callback,
    `LoopDeletionPass`, `LoopFullUnrollPass`
18. `SROAPass(modify-cfg)`
19. `VectorCombinePass(early folds only)`
20. `MergedLoadStoreMotionPass`
21. `GVNPass`
22. `SCCPPass`
23. `BDCEPass`
24. `InstCombinePass`
25. `JumpThreadingPass`
26. `CorrelatedValuePropagationPass`
27. `ADCEPass`
28. `MemCpyOptPass`
29. `DSEPass`
30. `MoveAutoInitPass`
31. `LICMPass(allow speculation)` under MemorySSA
32. `CoroElidePass`
33. `SimplifyCFGPass`
34. `InstCombinePass`

### 2.3 Module optimization pipeline

Module-level setup:

1. `EliminateAvailableExternallyPass`
2. `ReversePostOrderFunctionAttrsPass`
3. `RecomputeGlobalsAAPass`

Main late function optimization pipeline:

1. `DropUnnecessaryAssumesPass`
2. `Float2IntPass`
3. `LowerConstantIntrinsicsPass`
4. `ControlHeightReductionPass` (`chr`, enabled at `O3`)
5. loop pipeline:
   `LoopRotatePass`, `LoopDeletionPass`
6. `LoopDistributePass`
7. `InjectTLIMappings`
8. vector pipeline:
   `LoopVectorizePass`, `DropUnnecessaryAssumesPass(drop dereferenceable)`,
   `InferAlignmentPass`, `LoopLoadEliminationPass`, `InstCombinePass`,
   `SimplifyCFGPass`, `SLPVectorizerPass`, `VectorCombinePass`,
   `InstCombinePass`, `LoopUnrollPass(O3)`,
   `WarnMissedTransformationsPass`, `SROAPass(preserve-cfg)`,
   `InferAlignmentPass`, `InstCombinePass`, `LICMPass(allow speculation)`,
   `AlignmentFromAssumptionsPass`
9. `LoopSinkPass`
10. `InstSimplifyPass`
11. `DivRemPairsPass`
12. `ExpandMemCmpPass`
13. `TailCallElimPass`
14. `SimplifyCFGPass`

Late module cleanup:

1. `AllocTokenPass`
2. optimizer-last callbacks, if any
3. `GlobalDCEPass`
4. `ConstantMergePass`
5. `CGProfilePass`
6. `RelLookupTableConverterPass`
7. `AnnotationRemarksPass`
8. `VerifierPass`, if Clang verification is enabled

## 3. Legacy RISC-V codegen pipeline

The codegen pipeline is assembled by `TargetPassConfig::addISelPasses()` and
`TargetPassConfig::addMachinePasses()`, with RISC-V overrides from
`RISCVPassConfig`.

### 3.1 IR-to-IR preparation before instruction selection

Normal `-O3` SelectionDAG path:

1. `TargetTransformInfoWrapperPass`
2. `TargetLibraryInfoWrapperPass`
3. `RuntimeLibraryInfoWrapper`
4. `TargetPassConfig`
5. `MachineModuleInfoWrapperPass`
6. `ObjCARCContractPass`
7. `PreISelIntrinsicLoweringPass`
8. `ExpandIRInstsPass`
9. `AtomicExpandLegacyPass`
10. `RISCVZacasABIFixPass`
11. `LoopDataPrefetchPass` (`riscv-enable-loop-data-prefetch`, default true)
12. `RISCVGatherScatterLoweringPass`
13. `InterleavedAccessPass`
14. `RISCVCodeGenPrepareLegacyPass`
15. generic codegen IR passes:
    `VerifierPass`, `TypeBasedAAWrapperPass`, `ScopedNoAliasAAWrapperPass`,
    `BasicAAWrapperPass`, `CanonicalizeFreezeInLoopsPass`,
    `LoopStrengthReducePass`, `LoopTermFoldPass` (RISC-V enables this),
    `GCLowering`, `ShadowStackGCLowering`,
    `UnreachableBlockEliminationPass`, `ConstantHoistingPass`,
    `ReplaceWithVeclibLegacyPass`, `PartiallyInlineLibCallsPass`,
    `PostInlineEntryExitInstrumenterPass`,
    `ScalarizeMaskedMemIntrinLegacyPass`, `ExpandReductionsPass`,
    `SelectOptimizePass`
16. second RISC-V `SelectOptimizePass` at `CodeGenOptLevel::Aggressive`
    (`-O3`, gated by `riscv-select-opt`, default true)
17. `TypePromotionLegacyPass`
18. `CodeGenPrepareLegacyPass`
19. exception preparation:
    usually `DwarfEHPreparePass` for the default ELF/DWARF EH model; other
    targets/options may use `SjLjEHPreparePass`, `WinEHPass`,
    `WasmEHPass`, or `LowerInvokePass`
20. RISC-V `addPreISel`:
    `RISCVPromoteConstantPass`, `BarrierNoopPass`, `GlobalMergePass`
21. `InlineAsmPreparePass`
22. `SafeStackPass`
23. `StackProtectorPass`
24. final IR `VerifierPass`
25. `RISCVDAGToDAGISelLegacyPass`
26. `FinalizeISel`

GlobalISel alternative, not baseline: if `-fglobal-isel`/target option enables
it, RISC-V inserts `IRTranslator`, `RISCVPreLegalizerCombiner`, `Legalizer`,
`RISCVPostLegalizerCombiner`, `RegBankSelect`, and `InstructionSelect`, with
SelectionDAG fallback depending on abort/fallback policy.

### 3.2 Machine SSA optimization

RISC-V runs these before the generic Machine SSA pipeline:

1. `RISCVVLOptimizerPass`
2. `RISCVVectorPeepholePass`
3. `RISCVFoldMemOffsetPass`

Then generic Machine SSA optimization:

1. `EarlyTailDuplicate`
2. `OptimizePHIs`
3. `StackColoring`
4. `LocalStackSlotAllocation`
5. `DeadMachineInstructionElim`
6. `MachineCombiner` from `RISCVPassConfig::addILPOpts`
   (`riscv-enable-machine-combiner`, default true)
7. `EarlyMachineLICM`
8. `MachineCSE`
9. `MachineSinking`
10. `PeepholeOptimizer`
11. `DeadMachineInstructionElim`

After the generic block, RISC-V64 adds:

1. `RISCVOptWInstrsPass`

### 3.3 Pre-register-allocation RISC-V passes

1. `RISCVPreRAExpandPseudoPass`
2. `RISCVMergeBaseOffsetOptPass`
3. `RISCVPreAllocZilsdOptPass`
4. `RISCVInsertReadWriteCSRPass`
5. `RISCVInsertWriteVXRMPass`
6. `RISCVLandingPadSetupPass`
7. `MachinePipeliner` only if `riscv-enable-pipeliner=true`
   (default false)
8. `RISCVVMV0EliminationPass`

### 3.4 Optimized register allocation

Generic optimized RA setup:

1. `DetectDeadLanes`
2. `InitUndef`
3. `ProcessImplicitDefs`
4. `UnreachableMachineBlockElim`
5. `LiveVariables`
6. `MachineLoopInfo`
7. `PHIElimination`
8. optional `LiveIntervals` if `EarlyLiveIntervals`
9. `TwoAddressInstructionPass`
10. `RegisterCoalescer`
11. `RenameIndependentSubregs`
12. `MachineScheduler`

RISC-V then overrides `addRegAssignAndRewriteOptimized()` and performs a
separate RVV allocation/rewrite before the normal allocator:

1. RVV register allocator: default `GreedyRegisterAllocator` restricted to
   RVV register classes (`-riscv-rvv-regalloc=` can select another)
2. `VirtRegRewriter(false)` for that RVV allocation stage
3. `RISCVInsertVSETVLIPass`
4. `RISCVDeadRegisterDefinitionsPass`
   (`riscv-enable-dead-defs`, default true)
5. normal target register allocator:
   default `GreedyRegisterAllocator` unless `-regalloc=` overrides it
6. generic `VirtRegRewriter`
7. `RegAllocScoringPass`

After successful optimized RA:

1. `StackSlotColoring`
2. `MachineCopyPropagation`
3. `MachineLICM`

### 3.5 Post-register-allocation to emit

1. `RISCVRedundantCopyEliminationPass`
   (`riscv-enable-copyelim`, default true)
2. `RemoveRedundantDebugValues`
3. `FixupStatepointCallerSaved`
4. `PostRAMachineSinking`
5. `ShrinkWrap`
6. `PrologEpilogInserter`
7. machine late optimization:
   `MachineLateInstrsCleanup`, `BranchFolder`, `TailDuplicate`,
   `MachineCopyPropagation`
8. `ExpandPostRAPseudos`
9. RISC-V pre-sched2:
   `RISCVPostRAExpandPseudoPass`, `KCFIPass`, `RISCVLoadStoreOptPass`
10. post-RA scheduler:
    RISC-V substitutes `PostRAScheduler` with `PostMachineScheduler` at
    optimized levels
11. `GCMachineCodeAnalysis`
12. `MachineBlockPlacement`
13. `FEntryInserter`
14. `XRayInstrumentation`
15. `PatchableFunction`
16. RISC-V pre-emit:
    `MachineCopyPropagation(true)`, `RISCVLateBranchOptPass`,
    `RISCVIndirectBranchTrackingPass`, `BranchRelaxationPass`,
    `RISCVMakeCompressibleOptPass`
17. `FuncletLayout`
18. `RemoveLoadsIntoFakeUses`
19. `StackMapLiveness`
20. `LiveDebugValues`
21. `MachineSanitizerBinaryMetadata`
22. optional profile/layout passes depending on MFS, BB sections, static data
    partitioning, and related flags
23. `CFIFixup` by default: `RISCVTargetMachine` sets CFI fixup enabled unless
    `riscv-enable-cfi-instr-inserter=true`
24. `StackFrameLayoutAnalysis`
25. RISC-V final pre-emit:
    `RISCVMoveMergePass`, `RISCVPushPopOptimizationPass`,
    `RISCVExpandPseudoPass`, `RISCVExpandAtomicPseudoPass`,
    `UnpackMachineBundlesLegacy` for KCFI bundles, optional
    `CFIInstrInserter`
26. `RISCVAsmPrinter`
27. `FreeMachineFunctionPass`

## 4. RISC-V-specific pass inventory

IR/new-PM:

- `loop-idiom-vectorize`: inserted as
  `LoopIdiomVectorizePass(LoopIdiomVectorizeStyle::Predicated)` for non-`O0`.
- `riscv-codegenprepare`: registered in the target pass registry for new-PM
  textual use; Clang codegen also runs the legacy `RISCVCodeGenPrepare`.

Legacy IR/pre-isel:

- `RISCVZacasABIFixPass` (`riscv-zacas-abi-fix`): inserted unconditionally in
  codegen, but changes code only with `+zacas` and SC failure ordering.
- `LoopDataPrefetchPass`: default enabled at optimized levels.
- `RISCVGatherScatterLoweringPass` (`riscv-gather-scatter-lowering`).
- `InterleavedAccessPass`.
- `RISCVCodeGenPrepareLegacyPass` (`riscv-codegenprepare`).
- `RISCVPromoteConstantPass` (`riscv-promote-const`): inserted at optimized
  levels; the transform is mainly useful for RV64 with legal `f64/i64`.
- `GlobalMergePass`: inserted by default at optimized levels because
  `riscv-enable-global-merge` defaults to unset and RISC-V treats unset as on.

Machine:

- `RISCVVLOptimizerPass` (`riscv-vl-optimizer`)
- `RISCVVectorPeepholePass` (`riscv-vector-peephole`)
- `RISCVFoldMemOffsetPass` (`riscv-fold-mem-offset`)
- `RISCVOptWInstrsPass` (`riscv-opt-w-instrs`, RV64 only)
- `RISCVPreRAExpandPseudoPass` (`riscv-prera-expand-pseudo`)
- `RISCVMergeBaseOffsetOptPass` (`riscv-merge-base-offset`)
- `RISCVPreAllocZilsdOptPass` (`riscv-prera-zilsd-opt`): inserted at optimized
  levels, active only for RV32 with `+zilsd`.
- `RISCVInsertReadWriteCSRPass` (`riscv-insert-read-write-csr`)
- `RISCVInsertWriteVXRMPass` (`riscv-insert-write-vxrm`)
- `RISCVLandingPadSetupPass` (`riscv-lpad-setup`)
- `RISCVVMV0EliminationPass` (`riscv-vmv0-elimination`)
- RVV-specific register allocation before normal RA
- `RISCVInsertVSETVLIPass` (`riscv-insert-vsetvli`)
- `RISCVDeadRegisterDefinitionsPass` (`riscv-dead-defs`)
- `RISCVRedundantCopyEliminationPass` (`riscv-copyelim`)
- `RISCVPostRAExpandPseudoPass` (`riscv-post-ra-expand-pseudo`)
- `RISCVLoadStoreOptPass` (`riscv-load-store-opt`)
- `RISCVLateBranchOptPass` (`riscv-late-branch-opt`)
- `RISCVIndirectBranchTrackingPass` (`riscv-indirect-branch-tracking`)
- `RISCVMakeCompressibleOptPass` (`riscv-make-compressible`)
- `RISCVMoveMergePass` (`riscv-move-merge`)
- `RISCVPushPopOptimizationPass` (`riscv-push-pop-opt`)
- `RISCVExpandPseudoPass` (`riscv-expand-pseudo`)
- `RISCVExpandAtomicPseudoPass` (`riscv-expand-atomic-pseudo`)
- `RISCVAsmPrinter` (`riscv-asm-printer`)

## 5. Important conditionals and variants

- `riscv64` vs `riscv32`: `RISCVOptWInstrsPass` is added only for RISC-V64.
- RVV: many RVV passes are inserted regardless, but they only transform when
  vector instructions or scalable-vector constructs are present. Scheduler
  mutation `RISCVVectorMaskDAGMutation` is added only when the subtarget has V.
- `+zacas`: `RISCVZacasABIFixPass` is inserted always, but meaningful only
  when Zacas is enabled.
- `+zilsd`: `RISCVPreAllocZilsdOptPass` is inserted at optimized levels, but
  its implementation runs only on RV32 with Zilsd.
- Machine pipeliner is not on in the default release pipeline:
  `riscv-enable-pipeliner` defaults to false.
- CFI instruction inserter is not on by default:
  `riscv-enable-cfi-instr-inserter` defaults to false. Therefore RISC-V
  enables the generic `CFIFixup` path by default.
- GlobalISel is not the default baseline path. With `-fglobal-isel`, the RISC-V
  GlobalISel legalizer/combiner/select passes replace or precede the normal
  SelectionDAG path depending on fallback policy.
- LTO, PGO, SamplePGO, CS-PGO, MemProf, sanitizers, ObjC ARC, pass plugins,
  debugify, machine outliner, machine function splitter, basic block sections,
  and static data partitioning all add or alter passes and are outside the
  baseline `-O3 -c` pipeline described here.

## 6. Verification notes

The local build cannot execute RISC-V codegen because both local `llc` binaries
report only x86/x86-64 registered targets, and
`install/include/llvm/Config/Targets.h` has `LLVM_HAS_RISCV_TARGET 0`.

The generic new-PM `-O3` pass string was printed from the monorepo root with:

```sh
printf '%s\n' \
  'target triple = "riscv64-unknown-elf"' \
  '' \
  'define i32 @f(i32 %x) {' \
  'entry:' \
  '  %y = add i32 %x, 1' \
  '  ret i32 %y' \
  '}' |
  build/bin/opt -O3 -print-pipeline-passes -disable-output -
```

Because this build has no RISC-V target machine, that printed string does not
include the RISC-V late-loop callback. The callback is therefore added from
`RISCVTargetMachine.cpp:678-681` in the IR pipeline section above.
