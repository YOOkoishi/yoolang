#include "../../include/IR/MIR.h"

#include <iostream>

namespace mir {

// ============================================================================
// MachineInst 工厂方法
// ============================================================================

std::unique_ptr<MachineInst> MachineInst::make_r(Op op, VReg *dst, VReg *s1, VReg *s2,
                                                 MachineBasicBlock *parent) {
    auto inst = std::unique_ptr<MachineInst>(new MachineInst(op, parent));
    inst->operands_ = {Operand::reg(dst), Operand::reg(s1), Operand::reg(s2)};
    return inst;
}

std::unique_ptr<MachineInst> MachineInst::make_li(VReg *dst, int imm, MachineBasicBlock *parent) {
    auto inst = std::unique_ptr<MachineInst>(new MachineInst(Op::LI, parent));
    inst->operands_ = {Operand::reg(dst), Operand::imm(imm)};
    return inst;
}

std::unique_ptr<MachineInst> MachineInst::make_mv(VReg *dst, VReg *src, MachineBasicBlock *parent) {
    auto inst = std::unique_ptr<MachineInst>(new MachineInst(Op::MV, parent));
    inst->operands_ = {Operand::reg(dst), Operand::reg(src)};
    return inst;
}

std::unique_ptr<MachineInst> MachineInst::make_load_slot(VReg *dst, int offset,
                                                         MachineBasicBlock *parent) {
    auto inst = std::unique_ptr<MachineInst>(new MachineInst(Op::LOAD_SLOT, parent));
    inst->operands_ = {Operand::reg(dst), Operand::slot(offset)};
    return inst;
}

std::unique_ptr<MachineInst> MachineInst::make_store_slot(VReg *src, int offset,
                                                          MachineBasicBlock *parent) {
    auto inst = std::unique_ptr<MachineInst>(new MachineInst(Op::STORE_SLOT, parent));
    inst->operands_ = {Operand::reg(src), Operand::slot(offset)};
    return inst;
}

std::unique_ptr<MachineInst> MachineInst::make_branch(Op op, VReg *s1, VReg *s2,
                                                      MachineBasicBlock *target,
                                                      MachineBasicBlock *parent) {
    auto inst = std::unique_ptr<MachineInst>(new MachineInst(op, parent));
    inst->operands_ = {Operand::reg(s1), Operand::reg(s2)};
    inst->target_bb_ = target;
    return inst;
}

std::unique_ptr<MachineInst> MachineInst::make_j(MachineBasicBlock *target,
                                                 MachineBasicBlock *parent) {
    auto inst = std::unique_ptr<MachineInst>(new MachineInst(Op::J, parent));
    inst->target_bb_ = target;
    return inst;
}

std::unique_ptr<MachineInst> MachineInst::make_call(const std::string &func,
                                                    MachineBasicBlock *parent) {
    auto inst = std::unique_ptr<MachineInst>(new MachineInst(Op::CALL, parent));
    inst->symbol_ = func;
    return inst;
}

std::unique_ptr<MachineInst> MachineInst::make_ret(VReg *val, MachineBasicBlock *parent) {
    auto inst = std::unique_ptr<MachineInst>(new MachineInst(Op::RET, parent));
    if (val)
        inst->operands_.push_back(Operand::reg(val));
    return inst;
}

std::unique_ptr<MachineInst> MachineInst::make_la(VReg *dst, const std::string &label,
                                                  MachineBasicBlock *parent) {
    auto inst = std::unique_ptr<MachineInst>(new MachineInst(Op::LA, parent));
    inst->operands_ = {Operand::reg(dst)};
    inst->symbol_ = label;
    return inst;
}

// ============================================================================
// MachineInst::emit — 输出 RISC-V 汇编
// ============================================================================

void MachineInst::emit() const {
    auto &os = std::cout;
    auto rn = [](VReg *r) -> std::string { return r->name(); };

    switch (op_) {
    // ---- 整数算术 ----
    case Op::ADD:
        os << "  add " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::SUB:
        os << "  sub " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::MUL:
        os << "  mul " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::DIV:
        os << "  div " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::REM:
        os << "  rem " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;

    // ---- 浮点算术 ----
    case Op::FADD:
        os << "  fadd.s " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::FSUB:
        os << "  fsub.s " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::FMUL:
        os << "  fmul.s " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::FDIV:
        os << "  fdiv.s " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;

    // ---- 整数比较 ----
    case Op::SLT:
        os << "  slt " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::SLE: {
        // sle dst, s1, s2  →  slt dst, s2, s1; xori dst, dst, 1
        os << "  slt " << rn(operands_[0].reg()) << ", " << rn(operands_[2].reg()) << ", "
           << rn(operands_[1].reg()) << "\n";
        os << "  xori " << rn(operands_[0].reg()) << ", " << rn(operands_[0].reg()) << ", 1\n";
        break;
    }
    case Op::SGT:
        os << "  slt " << rn(operands_[0].reg()) << ", " << rn(operands_[2].reg()) << ", "
           << rn(operands_[1].reg()) << "\n";
        break;
    case Op::SGE: {
        os << "  slt " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        os << "  xori " << rn(operands_[0].reg()) << ", " << rn(operands_[0].reg()) << ", 1\n";
        break;
    }
    case Op::SEQ: {
        // seq dst, s1, s2  →  sub dst,s1,s2; sltiu dst,dst,1
        os << "  sub " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        os << "  sltiu " << rn(operands_[0].reg()) << ", " << rn(operands_[0].reg()) << ", 1\n";
        break;
    }
    case Op::SNE: {
        // sne dst, s1, s2  →  sub dst,s1,s2; sltu dst,x0,dst
        os << "  sub " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        os << "  sltu " << rn(operands_[0].reg()) << ", x0, " << rn(operands_[0].reg()) << "\n";
        break;
    }

    // ---- 浮点比较 (→ 整数寄存器) ----
    case Op::FLT:
        os << "  flt.s " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::FLE:
        os << "  fle.s " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::FGT:
        os << "  flt.s " << rn(operands_[0].reg()) << ", " << rn(operands_[2].reg()) << ", "
           << rn(operands_[1].reg()) << "\n";
        break;
    case Op::FGE:
        os << "  fle.s " << rn(operands_[0].reg()) << ", " << rn(operands_[2].reg()) << ", "
           << rn(operands_[1].reg()) << "\n";
        break;
    case Op::FEQ:
        os << "  feq.s " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::FNE: {
        os << "  feq.s " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        os << "  xori " << rn(operands_[0].reg()) << ", " << rn(operands_[0].reg()) << ", 1\n";
        break;
    }

    // ---- 逻辑 ----
    case Op::AND:
        os << "  and " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::OR:
        os << "  or " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::XOR:
        os << "  xor " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::NOT:
        os << "  not " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << "\n";
        break;

    // ---- 移位 ----
    case Op::SLL:
        os << "  sll " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::SRL:
        os << "  srl " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;
    case Op::SRA:
        os << "  sra " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", "
           << rn(operands_[2].reg()) << "\n";
        break;

    // ---- 数据搬运 ----
    case Op::MV:
        os << "  mv " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << "\n";
        break;
    case Op::FMV:
        os << "  fmv.s " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << "\n";
        break;
    case Op::LI:
        os << "  li " << rn(operands_[0].reg()) << ", " << operands_[1].imm() << "\n";
        break;

    // ---- 内存 ----
    case Op::LW:
        os << "  lw " << rn(operands_[0].reg()) << ", " << operands_[1].slot_offset() << "(s0)\n";
        break;
    case Op::SW:
        os << "  sw " << rn(operands_[0].reg()) << ", " << operands_[1].slot_offset() << "(s0)\n";
        break;
    case Op::FLW:
        os << "  flw " << rn(operands_[0].reg()) << ", " << operands_[1].slot_offset() << "(s0)\n";
        break;
    case Op::FSW:
        os << "  fsw " << rn(operands_[0].reg()) << ", " << operands_[1].slot_offset() << "(s0)\n";
        break;
    case Op::LA:
        os << "  la " << rn(operands_[0].reg()) << ", " << symbol_ << "\n";
        break;

    // ---- 栈槽伪指令 ----
    case Op::LOAD_SLOT: {
        auto *dst = operands_[0].reg();
        int off = operands_[1].slot_offset();
        if (dst->is_int())
            os << "  lw " << dst->name() << ", " << off << "(s0)\n";
        else
            os << "  flw " << dst->name() << ", " << off << "(s0)\n";
        break;
    }
    case Op::STORE_SLOT: {
        auto *src = operands_[0].reg();
        int off = operands_[1].slot_offset();
        if (src->is_int())
            os << "  sw " << src->name() << ", " << off << "(s0)\n";
        else
            os << "  fsw " << src->name() << ", " << off << "(s0)\n";
        break;
    }

    // ---- 分支 ----
    case Op::BEQ:
        os << "  beq " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", ."
           << target_bb_->label << "\n";
        break;
    case Op::BNE:
        os << "  bne " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", ."
           << target_bb_->label << "\n";
        break;
    case Op::BLT:
        os << "  blt " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", ."
           << target_bb_->label << "\n";
        break;
    case Op::BGE:
        os << "  bge " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", ."
           << target_bb_->label << "\n";
        break;

    // ---- 跳转 ----
    case Op::J:
        os << "  j ." << target_bb_->label << "\n";
        break;
    case Op::JAL:
        os << "  jal " << rn(operands_[0].reg()) << ", " << symbol_ << "\n";
        break;
    case Op::JALR:
        os << "  jalr " << rn(operands_[0].reg()) << ", " << operands_[1].imm() << "("
           << rn(operands_[2].reg()) << ")\n";
        break;

    // ---- 函数 ----
    case Op::CALL:
        os << "  call " << symbol_ << "\n";
        break;
    case Op::RET:
        if (!operands_.empty())
            os << "  mv a0, " << rn(operands_[0].reg()) << "\n";
        os << "  ret\n";
        break;

    // ---- 类型转换 ----
    case Op::FCVT_S_W:
        os << "  fcvt.s.w " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << "\n";
        break;
    case Op::FCVT_W_S:
        os << "  fcvt.w.s " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << "\n";
        break;

    // ---- 零扩展 ----
    case Op::ZEXT:
        // andi dst, src, 1
        os << "  andi " << rn(operands_[0].reg()) << ", " << rn(operands_[1].reg()) << ", 1\n";
        break;

    case Op::NOP:
        os << "  nop\n";
        break;
    }
}

// ============================================================================
// MachineBasicBlock::emit
// ============================================================================
void MachineBasicBlock::emit() const {
    std::cout << "." << label << ":\n";
    for (const auto &inst : insts_)
        inst->emit();
}

// ============================================================================
// MachineFunction::emit
// ============================================================================
void MachineFunction::emit() const {
    std::cout << "  .globl " << name << "\n";
    std::cout << "  .text\n";
    std::cout << name << ":\n";

    // ---- Prologue ----
    int fs = frame.sp_adjust();
    std::cout << "  addi sp, sp, -" << fs << "\n";
    std::cout << "  sw ra, " << (fs - 4) << "(sp)\n";
    std::cout << "  sw s0, " << (fs - 8) << "(sp)\n";
    std::cout << "  addi s0, sp, " << fs << "\n";

    // 恢复参数: a0-a7 → 栈槽
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i].rc == RegClass::Int && i < 8)
            std::cout << "  sw a" << i << ", " << args[i].slot_offset << "(s0)\n";
        else if (args[i].rc == RegClass::Float && i < 8)
            std::cout << "  fsw fa" << i << ", " << args[i].slot_offset << "(s0)\n";
    }

    // ---- 基本块 ----
    for (auto it = blocks_begin(); it != blocks_end(); ++it)
        (*it)->emit();

    // (Epilogue 由 RET 指令生成，或在此补充 fallthrough epilogue)
    std::cout << "\n";
}

// ============================================================================
// GlobalVar::emit
// ============================================================================
void GlobalVar::emit() const {
    std::cout << "  .data\n";
    std::cout << "  .globl " << name << "\n";
    std::cout << "  .align 2\n";
    std::cout << name << ":\n";
    if (init_data.empty()) {
        if (kind == GlobalVar::Kind::Int)
            std::cout << "  .word 0\n";
        else if (kind == GlobalVar::Kind::Float)
            std::cout << "  .word 0\n";
        else
            std::cout << "  .zero " << array_len * 4 << "\n";
    } else {
        for (int32_t v : init_data)
            std::cout << "  .word " << v << "\n";
    }
}

// ============================================================================
// MachineModule::emit
// ============================================================================
void MachineModule::emit() const {
    std::cout << "  .attribute arch, \"rv64i2p1_m2p0_a2p1_f2p2_d2p2_c2p0\"\n";

    // 库函数声明 (SysY runtime)
    std::cout << "  .text\n";

    // 全局变量
    for (const auto &g : globals)
        g->emit();

    // 函数
    for (const auto &f : functions)
        f->emit();
}

} // namespace mir
