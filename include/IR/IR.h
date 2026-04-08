#pragma once

#include <cassert>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

namespace mir {

// ============================================================================
// 前向声明
// ============================================================================
class MachineBasicBlock;
class MachineFunction;
class MachineModule;

// ============================================================================
// 虚拟寄存器
// ============================================================================
enum class RegClass : uint8_t { Int, Float };

class VReg {
  public:
    int id; // >= 0 虚拟寄存器编号
    RegClass rc;

    VReg(int id, RegClass rc) : id(id), rc(rc) {
    }

    bool is_int() const {
        return rc == RegClass::Int;
    }
    bool is_float() const {
        return rc == RegClass::Float;
    }

    std::string name() const {
        return is_int() ? ("x" + std::to_string(id)) : ("f" + std::to_string(id));
    }
};

// RISC-V 整数物理寄存器
enum IntPhyReg : int {
    x_zero = 0,
    x_ra = 1,
    x_sp = 2,
    x_gp = 3,
    x_tp = 4,
    x_t0 = 5,
    x_t1 = 6,
    x_t2 = 7,
    x_s0 = 8,
    x_s1 = 9,
    x_a0 = 10,
    x_a1 = 11,
    x_a2 = 12,
    x_a3 = 13,
    x_a4 = 14,
    x_a5 = 15,
    x_a6 = 16,
    x_a7 = 17,
    x_s2 = 18,
    x_s3 = 19,
    x_s4 = 20,
    x_s5 = 21,
    x_s6 = 22,
    x_s7 = 23,
    x_s8 = 24,
    x_s9 = 25,
    x_s10 = 26,
    x_s11 = 27,
    x_t3 = 28,
    x_t4 = 29,
    x_t5 = 30,
    x_t6 = 31,
};

// RISC-V 浮点物理寄存器
enum FloatPhyReg : int {
    f_t0 = 0,
    f_t1 = 1,
    f_t2 = 2,
    f_t3 = 3,
    f_t4 = 4,
    f_t5 = 5,
    f_t6 = 6,
    f_t7 = 7,
    f_s0 = 8,
    f_s1 = 9,
    f_a0 = 10,
    f_a1 = 11,
    f_a2 = 12,
    f_a3 = 13,
    f_a4 = 14,
    f_a5 = 15,
    f_a6 = 16,
    f_a7 = 17,
    f_s2 = 18,
    f_s3 = 19,
    f_s4 = 20,
    f_s5 = 21,
    f_s6 = 22,
    f_s7 = 23,
    f_s8 = 24,
    f_s9 = 25,
    f_s10 = 26,
    f_s11 = 27,
    f_t8 = 28,
    f_t9 = 29,
    f_t10 = 30,
    f_t11 = 31,
};

// ============================================================================
// 操作数 — 安全的 tagged 类型，禁止 dynamic_cast
// ============================================================================
class Operand {
  public:
    enum class Kind : uint8_t { Reg, Imm, Slot };

    // 工厂方法
    static Operand reg(VReg *r) {
        Operand op;
        op.kind_ = Kind::Reg;
        op.reg_ = r;
        op.val_ = 0;
        return op;
    }
    static Operand imm(int v) {
        Operand op;
        op.kind_ = Kind::Imm;
        op.reg_ = nullptr;
        op.val_ = v;
        return op;
    }
    static Operand slot(int offset) {
        Operand op;
        op.kind_ = Kind::Slot;
        op.reg_ = nullptr;
        op.val_ = offset;
        return op;
    }

    Kind kind() const {
        return kind_;
    }
    bool is_reg() const {
        return kind_ == Kind::Reg;
    }
    bool is_imm() const {
        return kind_ == Kind::Imm;
    }
    bool is_slot() const {
        return kind_ == Kind::Slot;
    }

    VReg *reg() const {
        assert(kind_ == Kind::Reg && "operand is not a register");
        return reg_;
    }
    int imm() const {
        assert(kind_ == Kind::Imm && "operand is not an immediate");
        return val_;
    }
    int slot_offset() const {
        assert(kind_ == Kind::Slot && "operand is not a stack slot");
        return val_;
    }

  private:
    Kind kind_;
    VReg *reg_; // Kind::Reg 时有效
    int val_;   // Kind::Imm 或 Kind::Slot 的值
};

// ============================================================================
// 机器指令 — 操作数按位置约定，随 Opcode 不同而含义不同
//
// 约定 (operand 索引):
//   算术 R-type:   [0]=dst  [1]=src1  [2]=src2
//   算术 I-type:   [0]=dst  [1]=imm
//   Load:          [0]=dst  [1]=offset(base_reg)  — base 存在 base_ 字段
//   Store:         [0]=src  [1]=offset(base_reg)
//   Branch:        [0]=src1  [1]=src2  — 目标块在 target_ 字段
//   Cmp-set:       [0]=dst  [1]=src1  [2]=src2
//   MV / FMV:      [0]=dst  [1]=src
//   CALL:          无 operands, 参数通过 a0-a7 / fa0-fa7 约定
//   RET:           [0]=返回值 (可选)
// ============================================================================

class MachineInst {
  public:
    enum class Op : uint8_t {
        // ---- 整数算术 ----
        ADD,
        SUB,
        MUL,
        DIV,
        REM,
        // ---- 浮点算术 ----
        FADD,
        FSUB,
        FMUL,
        FDIV,
        // ---- 整数比较 (结果 0/1) ----
        SLT,
        SLE,
        SGT,
        SGE,
        SEQ,
        SNE,
        // ---- 浮点比较 ----
        FLT,
        FLE,
        FGT,
        FGE,
        FEQ,
        FNE,
        // ---- 逻辑 ----
        AND,
        OR,
        XOR,
        NOT,
        // ---- 移位 ----
        SLL,
        SRL,
        SRA,
        // ---- 数据搬运 ----
        MV,
        FMV,
        LI,
        // ---- 内存 (栈式寻址，base = s0) ----
        LW,
        SW,
        FLW,
        FSW,
        // ---- 地址 ----
        LA,
        // ---- 栈槽读写 (伪指令，展开为 LW/SW with s0+off) ----
        LOAD_SLOT,
        STORE_SLOT,
        // ---- 分支 ----
        BEQ,
        BNE,
        BLT,
        BGE,
        // ---- 跳转 ----
        J,
        JAL,
        JALR,
        // ---- 函数 ----
        CALL,
        RET,
        // ---- 类型转换 ----
        FCVT_S_W, // int  → float
        FCVT_W_S, // float → int
        // ---- 零扩展 ----
        ZEXT,
        // ---- 无操作 ----
        NOP,
    };

    MachineBasicBlock *parent;

    // ---- 构造工厂 ----
    // R-type: dst = src1 op src2
    static std::unique_ptr<MachineInst> make_r(Op op, VReg *dst, VReg *s1, VReg *s2,
                                               MachineBasicBlock *parent = nullptr);
    // I-type: dst = imm
    static std::unique_ptr<MachineInst> make_li(VReg *dst, int imm,
                                                MachineBasicBlock *parent = nullptr);
    // MV
    static std::unique_ptr<MachineInst> make_mv(VReg *dst, VReg *src,
                                                MachineBasicBlock *parent = nullptr);
    // 栈加载: dst = mem[s0 + offset]
    static std::unique_ptr<MachineInst> make_load_slot(VReg *dst, int offset,
                                                       MachineBasicBlock *parent = nullptr);
    // 栈存储: mem[s0 + offset] = src
    static std::unique_ptr<MachineInst> make_store_slot(VReg *src, int offset,
                                                        MachineBasicBlock *parent = nullptr);
    // 分支
    static std::unique_ptr<MachineInst> make_branch(Op op, VReg *s1, VReg *s2,
                                                    MachineBasicBlock *target,
                                                    MachineBasicBlock *parent = nullptr);
    // 无条件跳转
    static std::unique_ptr<MachineInst> make_j(MachineBasicBlock *target,
                                               MachineBasicBlock *parent = nullptr);
    // 函数调用
    static std::unique_ptr<MachineInst> make_call(const std::string &func,
                                                  MachineBasicBlock *parent = nullptr);
    // 返回
    static std::unique_ptr<MachineInst> make_ret(VReg *val = nullptr,
                                                 MachineBasicBlock *parent = nullptr);
    // LA
    static std::unique_ptr<MachineInst> make_la(VReg *dst, const std::string &label,
                                                MachineBasicBlock *parent = nullptr);

    // ---- 访问 ----
    Op opcode() const {
        return op_;
    }

    const std::vector<Operand> &operands() const {
        return operands_;
    }

    // 分支/跳转目标
    MachineBasicBlock *target() const {
        return target_bb_;
    }
    void set_target(MachineBasicBlock *bb) {
        target_bb_ = bb;
    }

    // 符号名 (CALL / LA 目标)
    const std::string &symbol() const {
        return symbol_;
    }

    // 输出 RISC-V 汇编
    void emit() const;

  private:
    MachineInst(Op op, MachineBasicBlock *p) : op_(op), parent(p) {
    }

    Op op_;
    std::vector<Operand> operands_;
    MachineBasicBlock *target_bb_ = nullptr;
    std::string symbol_; // CALL, LA 等需要的符号名
};

class StackFrame {
  public:
    // 分配一个栈槽，返回相对于 s0 的负偏移
    int alloc_slot(int bytes = 4) {
        offset_ -= bytes;
        slots_.push_back(offset_);
        return offset_;
    }

    // 获取总帧大小 (取绝对值 + ra + old_s0)
    int frame_size() const {
        return -offset_ + 8;
    }

    int sp_adjust() const {
        return -offset_ + 8;
    }

  private:
    int offset_ = 0; // 从 0 向下递减
    std::vector<int> slots_;
};

// ============================================================================
// 机器基本块
// ============================================================================
class MachineBasicBlock {
  public:
    std::string label;
    std::vector<MachineBasicBlock *> preds;
    std::vector<MachineBasicBlock *> succs;

    explicit MachineBasicBlock(const std::string &name) : label(name) {
    }

    MachineInst *push_back(std::unique_ptr<MachineInst> inst) {
        auto *ptr = inst.get();
        inst->parent = this;
        insts_.push_back(std::move(inst));
        return ptr;
    }

    // 在末尾插入 (用于插入 prologue/epilogue)
    MachineInst *push_front(std::unique_ptr<MachineInst> inst) {
        auto *ptr = inst.get();
        inst->parent = this;
        insts_.push_front(std::move(inst));
        return ptr;
    }

    auto begin() {
        return insts_.begin();
    }
    auto end() {
        return insts_.end();
    }
    auto begin() const {
        return insts_.begin();
    }
    auto end() const {
        return insts_.end();
    }

    void emit() const;

  private:
    std::list<std::unique_ptr<MachineInst>> insts_;
};

// ============================================================================
// 机器函数
// ============================================================================
class MachineFunction {
  public:
    std::string name;
    StackFrame frame;

    // 参数信息 (用于 prologue 中恢复参数)
    struct ArgInfo {
        RegClass rc;
        int slot_offset; // 在栈上的位置
    };
    std::vector<ArgInfo> args;
    bool has_return = false;
    RegClass return_rc = RegClass::Int;

    MachineFunction(const std::string &n) : name(n) {
    }

    MachineBasicBlock *add_block(const std::string &label) {
        auto bb = std::make_unique<MachineBasicBlock>(label);
        auto *ptr = bb.get();
        blocks_.push_back(std::move(bb));
        return ptr;
    }

    // 虚拟寄存器分配
    VReg *alloc_vreg(RegClass rc) {
        auto vr = std::make_unique<VReg>(next_vreg_id_++, rc);
        auto *ptr = vr.get();
        vregs_.push_back(std::move(vr));
        return ptr;
    }

    void emit() const;

    // 迭代 (供 emit 等遍历)
    auto blocks_begin() {
        return blocks_.begin();
    }
    auto blocks_end() {
        return blocks_.end();
    }
    auto blocks_begin() const {
        return blocks_.begin();
    }
    auto blocks_end() const {
        return blocks_.end();
    }

  private:
    int next_vreg_id_ = 0;
    std::list<std::unique_ptr<MachineBasicBlock>> blocks_;
    std::vector<std::unique_ptr<VReg>> vregs_;
};

// ============================================================================
// 全局变量
// ============================================================================
class GlobalVar {
  public:
    enum class Kind : uint8_t { Int, Float, Array };

    Kind kind;
    std::string name;
    int array_len = 0;              // 仅 Array 有效
    std::vector<int32_t> init_data; // 初始化值，空 = zeroinit

    GlobalVar(Kind k, const std::string &n) : kind(k), name(n) {
    }

    void emit() const;
};

// ============================================================================
// 机器模块 (整个编译单元)
// ============================================================================
class MachineModule {
  public:
    std::vector<std::unique_ptr<GlobalVar>> globals;
    std::vector<std::unique_ptr<MachineFunction>> functions;

    MachineFunction *add_function(const std::string &name) {
        auto f = std::make_unique<MachineFunction>(name);
        auto *ptr = f.get();
        functions.push_back(std::move(f));
        return ptr;
    }

    GlobalVar *add_global(GlobalVar::Kind kind, const std::string &name) {
        auto g = std::make_unique<GlobalVar>(kind, name);
        auto *ptr = g.get();
        globals.push_back(std::move(g));
        return ptr;
    }

    void emit() const;
};

} // namespace mir
