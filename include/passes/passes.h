#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ir {
class Module;
class Function;
class BasicBlock;
class Instruction;
class Value;
class ConstantInt;
class ConstantFloat;
class AllocaInst;
class GetElementPtrInst;
class PhiInst;
class CallInst;
class GlobalVariable;
} // namespace ir

namespace mir {
class MachineModule;
class MachineFunction;
class MachineBasicBlock;
class VReg;
} // namespace mir

namespace passes {

/// SSA IR → RISC-V Machine IR 的指令选择 + 栈帧构建
class SSAToMIRLowering {
  public:
    /// 对整个模块做 lowering，返回 MachineModule
    std::unique_ptr<mir::MachineModule> lower(ir::Module &module);

  private:
    // ---- 模块级 ----
    void lower_globals(ir::Module &module, mir::MachineModule &mm);
    void lower_function(ir::Function &func, mir::MachineModule &mm);

    // ---- 基本块级 ----
    void lower_block(ir::BasicBlock &bb);
    void lower_instruction(ir::Instruction &inst);

    // ---- 各类指令 lowering ----
    void lower_alloca(ir::AllocaInst &inst);
    void lower_load(ir::Instruction &inst);
    void lower_store(ir::Instruction &inst);
    void lower_binary(ir::Instruction &inst);
    void lower_icmp(ir::Instruction &inst);
    void lower_fcmp(ir::Instruction &inst);
    void lower_cast(ir::Instruction &inst);
    void lower_gep(ir::GetElementPtrInst &inst);
    void lower_call(ir::CallInst &inst);
    void lower_ret(ir::Instruction &inst);
    void lower_br(ir::Instruction &inst);
    void lower_phi(ir::PhiInst &inst);

    // ---- 辅助 ----
    mir::VReg *get_vreg(ir::Value *val);
    mir::VReg *materialize_constant(ir::Value *val);
    mir::VReg *alloc_int_vreg();
    mir::VReg *alloc_float_vreg();

    // 指令插入点
    mir::MachineFunction *cur_mf_ = nullptr;
    mir::MachineBasicBlock *cur_mbb_ = nullptr;

    // SSA Value → VReg 映射
    std::unordered_map<ir::Value *, mir::VReg *> vreg_map_;

    // SSA BasicBlock → MachineBasicBlock 映射
    std::unordered_map<ir::BasicBlock *, mir::MachineBasicBlock *> bb_map_;

    // Alloca → 栈偏移映射
    std::unordered_map<ir::Value *, int> alloca_slots_;

    // Phi 指令收集（需要延迟处理）
    std::vector<ir::PhiInst *> pending_phis_;
};

} // namespace passes
