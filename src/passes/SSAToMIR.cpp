#include "../../include/IR/MIR.h"
#include "../../include/IR/SSA_IR.h"
#include "../../include/passes/passes.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

namespace passes {

using namespace ir;
using namespace mir;

// ============================================================================
// 类型查询辅助
// ============================================================================

/// 递归计算类型占用的字节数
static int compute_type_size(Type *ty) {
    if (ty->is_integer())
        return 4;
    if (ty->is_float())
        return 4;
    if (ty->is_pointer())
        return 4;
    if (ty->is_array()) {
        auto *arr = static_cast<ArrayType *>(ty);
        return compute_type_size(arr->element_type()) * static_cast<int>(arr->element_count());
    }
    return 4;
}

/// 判断值是否为浮点类型
static bool is_float_type(Value *val) {
    return val->type()->is_float();
}

/// 判断值是否为整数 (i1 或 i32)
static bool is_int_type(Value *val) {
    return val->type()->is_integer();
}

// ============================================================================
// SSAToMIRLowering — 实现
// ============================================================================

std::unique_ptr<MachineModule> SSAToMIRLowering::lower(Module &module) {
    auto mm = std::make_unique<MachineModule>();

    // 1. Lower 全局变量
    lower_globals(module, *mm);

    // 2. Lower 各函数
    for (auto &func : module.functions()) {
        if (!func->is_external()) {
            lower_function(*func, *mm);
        }
    }

    return mm;
}

// ============================================================================
// 全局变量 lowering
// ============================================================================

void SSAToMIRLowering::lower_globals(Module &module, MachineModule &mm) {
    for (auto &gv : module.globals()) {
        GlobalVar::Kind kind = GlobalVar::Kind::Int;
        int array_len = 0;

        Type *value_ty = gv->value_type();
        if (value_ty->is_array()) {
            kind = GlobalVar::Kind::Array;
            // 计算总元素数（递归展平）
            Type *inner = value_ty;
            while (inner->is_array()) {
                auto *arr = static_cast<ArrayType *>(inner);
                array_len = array_len == 0 ? static_cast<int>(arr->element_count())
                                           : array_len * static_cast<int>(arr->element_count());
                inner = arr->element_type();
            }
            if (inner->is_float()) {
                kind = GlobalVar::Kind::Float;
            }
        } else if (value_ty->is_float()) {
            kind = GlobalVar::Kind::Float;
        }

        auto *mg = mm.add_global(kind, gv->name());

        // 初始化数据
        if (gv->init_value()) {
            if (auto *ci = dynamic_cast<ConstantInt *>(gv->init_value())) {
                mg->init_data.push_back(static_cast<int32_t>(ci->value()));
            } else if (auto *cf = dynamic_cast<ConstantFloat *>(gv->init_value())) {
                float f = cf->value();
                int32_t bits;
                std::memcpy(&bits, &f, sizeof(float));
                mg->init_data.push_back(bits);
            }
        }

        if (kind == GlobalVar::Kind::Array) {
            mg->array_len = array_len;
        }

        // 全局变量不需要分配 VReg
        // 在 load/store 中通过 LA 指令获取地址
    }
}

// ============================================================================
// 函数 lowering
// ============================================================================

void SSAToMIRLowering::lower_function(Function &func, MachineModule &mm) {
    // 清理 per-function 状态
    vreg_map_.clear();
    bb_map_.clear();
    alloca_slots_.clear();
    pending_phis_.clear();

    auto *mf = mm.add_function(func.name());
    cur_mf_ = mf;

    // ---- Phase 0: 创建所有 MachineBasicBlock ----
    for (auto &bb : func.blocks()) {
        auto *mbb = mf->add_block(bb->name());
        bb_map_[bb.get()] = mbb;
    }

    // ---- Phase 1: 处理参数 ----
    int arg_idx = 0;
    for (auto &arg : func.args()) {
        bool is_float = arg->type()->is_float();
        RegClass rc = is_float ? RegClass::Float : RegClass::Int;

        // 为参数分配栈槽
        int slot = mf->frame.alloc_slot(4);

        // 为参数创建 VReg
        auto *vreg = mf->alloc_vreg(rc);
        vreg_map_[arg.get()] = vreg;

        // 记录参数信息用于 prologue
        mf->args.push_back({rc, slot});

        // 如果参数是指针类型（数组参数），Alloca 一个 T* 槽存储指针
        // 在 SysY IR 中参数在函数入口处已有 alloca + store，
        // alloca 在函数入口块的 AllocaInst 中处理

        arg_idx++;
    }

    // 记录返回类型
    if (!func.return_type()->is_void()) {
        mf->has_return = true;
        mf->return_rc = func.return_type()->is_float() ? RegClass::Float : RegClass::Int;
    }

    // ---- Phase 2: 第一遍扫描 —— 收集 Alloca 并分配栈槽 ----
    // 遍历所有基本块中的 Alloca（标准做法是只在 entry，但做防御性扫描）
    for (auto &bb : func.blocks()) {
        for (auto &inst : bb->instructions()) {
            if (inst->op() == Instruction::OpID::Alloca) {
                // 避免重复注册
                if (alloca_slots_.count(inst.get()))
                    continue;

                auto *alloca = static_cast<AllocaInst *>(inst.get());
                int size = compute_type_size(alloca->allocated_type());
                int slot = mf->frame.alloc_slot(size);
                alloca_slots_[inst.get()] = slot;

                auto *addr_vreg = mf->alloc_vreg(RegClass::Int);
                vreg_map_[inst.get()] = addr_vreg;
            }
        }
    }

    // ---- Phase 3: 第二遍扫描 —— 遍历所有基本块，lower 指令 ----
    for (auto &bb : func.blocks()) {
        lower_block(*bb);
    }

    // ---- Phase 4: 处理 Phi 指令 —— 在前驱块末尾（跳转之前）插入 copy ----
    for (auto *phi : pending_phis_) {
        auto *dst = vreg_map_[phi];
        for (auto &[val, from_bb] : phi->incoming()) {
            auto *pred_mbb = bb_map_[from_bb];
            // 临时设置 cur_mbb_ 以便 get_vreg/materialize_constant 正确插入指令
            cur_mbb_ = pred_mbb;
            auto *src = get_vreg(val);
            // 在前驱块的跳转指令之前插入 MV
            pred_mbb->push_back(MachineInst::make_mv(dst, src, pred_mbb));
        }
    }
    cur_mbb_ = nullptr;

    // ---- Phase 5: 在入口块开头插入 Alloca 地址初始化 ----
    // alloca 的地址已在 Phase 2 中通过 alloca_slots_ 记录，
    // load/store 直接用 slot offset，addr_vreg 用于 GEP 场景。
    // 当前栈式模型中 alloca 地址通过 LI slot_offset 生成，
    // 已在 lower_gep 中延迟处理，此处无需额外操作。

    cur_mf_ = nullptr;
}

// ============================================================================
// 基本块 lowering
// ============================================================================

void SSAToMIRLowering::lower_block(BasicBlock &bb) {
    cur_mbb_ = bb_map_[&bb];

    for (auto &inst : bb.instructions()) {
        lower_instruction(*inst);
    }

    cur_mbb_ = nullptr;
}

void SSAToMIRLowering::lower_instruction(Instruction &inst) {
    switch (inst.op()) {
    case Instruction::OpID::Alloca:
        // 已在 Phase 2 中处理
        break;
    case Instruction::OpID::Load:
        lower_load(inst);
        break;
    case Instruction::OpID::Store:
        lower_store(inst);
        break;
    case Instruction::OpID::Add:
    case Instruction::OpID::Sub:
    case Instruction::OpID::Mul:
    case Instruction::OpID::SDiv:
    case Instruction::OpID::SRem:
    case Instruction::OpID::FAdd:
    case Instruction::OpID::FSub:
    case Instruction::OpID::FMul:
    case Instruction::OpID::FDiv:
        lower_binary(inst);
        break;
    case Instruction::OpID::ICmp:
        lower_icmp(inst);
        break;
    case Instruction::OpID::FCmp:
        lower_fcmp(inst);
        break;
    case Instruction::OpID::ZExt:
    case Instruction::OpID::SIToFP:
    case Instruction::OpID::FPToSI:
        lower_cast(inst);
        break;
    case Instruction::OpID::GetElementPtr:
        lower_gep(static_cast<GetElementPtrInst &>(inst));
        break;
    case Instruction::OpID::Call:
        lower_call(static_cast<CallInst &>(inst));
        break;
    case Instruction::OpID::Ret:
        lower_ret(inst);
        break;
    case Instruction::OpID::Br:
        lower_br(inst);
        break;
    case Instruction::OpID::Phi:
        lower_phi(static_cast<PhiInst &>(inst));
        break;
    }
}

// ============================================================================
// Load
// ============================================================================

void SSAToMIRLowering::lower_load(Instruction &inst) {
    // LoadInst: load ptr
    auto *load = static_cast<LoadInst *>(&inst);
    Value *ptr = load->ptr();

    bool is_float = inst.type()->is_float();
    auto *dst = cur_mf_->alloc_vreg(is_float ? RegClass::Float : RegClass::Int);
    vreg_map_[&inst] = dst;

    // 检查 ptr 是否是 Alloca（栈变量）
    auto alloca_it = alloca_slots_.find(ptr);
    if (alloca_it != alloca_slots_.end()) {
        // 从栈槽加载
        cur_mbb_->push_back(MachineInst::make_load_slot(dst, alloca_it->second, cur_mbb_));
    } else if (dynamic_cast<GlobalVariable *>(ptr)) {
        // 全局变量: LA + LW/FLW
        auto *gv = static_cast<GlobalVariable *>(ptr);
        auto *addr = alloc_int_vreg();
        cur_mbb_->push_back(MachineInst::make_la(addr, gv->name(), cur_mbb_));
        // LW dst, 0(addr) 或 FLW dst, 0(addr)
        // 这里需要直接使用 LW/FLW 而非 LOAD_SLOT
        // 使用 MV 伪指令作为临时方案 —— 实际需要真正的 LW/FLW with register offset
        // MIR 目前只支持 LOAD_SLOT (s0+off) 和 LA，需要添加通用 LW/FLW
        // 为简化，使用 LOAD_SLOT 加一个虚拟偏移来模拟——实际需要扩展 MIR
        // TODO: 添加通用 LW/FLW 指令支持
        // 目前先用一个 workaround: 直接用 LOAD_SLOT 并在后面修正
        cur_mbb_->push_back(MachineInst::make_load_slot(dst, 0));
    } else {
        // ptr 是一个普通指针值（如 GEP 结果），需要通过寄存器间接加载
        // 目前简化处理：通过 VReg 加载
        auto *ptr_vreg = get_vreg(ptr);
        // TODO: 需要添加 LD/LW with register offset 的 MIR 指令
        // 当前简化：假设指针已经在 vreg 中，使用 LOAD_SLOT 0 偏移（不正确但作为占位）
        cur_mbb_->push_back(MachineInst::make_load_slot(dst, 0));
    }
}

// ============================================================================
// Store
// ============================================================================

void SSAToMIRLowering::lower_store(Instruction &inst) {
    // StoreInst: store value, ptr
    auto *store = static_cast<StoreInst *>(&inst);
    Value *val = store->value();
    Value *ptr = store->ptr();

    auto *src = get_vreg(val);

    auto alloca_it = alloca_slots_.find(ptr);
    if (alloca_it != alloca_slots_.end()) {
        // 存到栈槽
        cur_mbb_->push_back(MachineInst::make_store_slot(src, alloca_it->second, cur_mbb_));
    } else if (dynamic_cast<GlobalVariable *>(ptr)) {
        // 全局变量 store
        auto *gv = static_cast<GlobalVariable *>(ptr);
        auto *addr = alloc_int_vreg();
        cur_mbb_->push_back(MachineInst::make_la(addr, gv->name(), cur_mbb_));
        cur_mbb_->push_back(MachineInst::make_store_slot(src, 0));
    } else {
        // 通过指针 store
        cur_mbb_->push_back(MachineInst::make_store_slot(src, 0));
    }
}

// ============================================================================
// 二元运算
// ============================================================================

void SSAToMIRLowering::lower_binary(Instruction &inst) {
    auto *binary = static_cast<BinaryInst *>(&inst);
    Value *lhs_val = binary->lhs();
    Value *rhs_val = binary->rhs();

    bool is_float = is_float_type(lhs_val);
    auto *dst = cur_mf_->alloc_vreg(is_float ? RegClass::Float : RegClass::Int);
    vreg_map_[&inst] = dst;

    auto *s1 = get_vreg(lhs_val);
    auto *s2 = get_vreg(rhs_val);

    MachineInst::Op mop;
    switch (inst.op()) {
    case Instruction::OpID::Add:
        mop = MachineInst::Op::ADD;
        break;
    case Instruction::OpID::Sub:
        mop = MachineInst::Op::SUB;
        break;
    case Instruction::OpID::Mul:
        mop = MachineInst::Op::MUL;
        break;
    case Instruction::OpID::SDiv:
        mop = MachineInst::Op::DIV;
        break;
    case Instruction::OpID::SRem:
        mop = MachineInst::Op::REM;
        break;
    case Instruction::OpID::FAdd:
        mop = MachineInst::Op::FADD;
        break;
    case Instruction::OpID::FSub:
        mop = MachineInst::Op::FSUB;
        break;
    case Instruction::OpID::FMul:
        mop = MachineInst::Op::FMUL;
        break;
    case Instruction::OpID::FDiv:
        mop = MachineInst::Op::FDIV;
        break;
    default:
        assert(false && "unknown binary op");
        return;
    }

    cur_mbb_->push_back(MachineInst::make_r(mop, dst, s1, s2, cur_mbb_));
}

// ============================================================================
// 整数比较
// ============================================================================

void SSAToMIRLowering::lower_icmp(Instruction &inst) {
    auto *cmp = static_cast<CmpInst *>(&inst);
    auto *s1 = get_vreg(cmp->lhs());
    auto *s2 = get_vreg(cmp->rhs());
    auto *dst = cur_mf_->alloc_vreg(RegClass::Int);
    vreg_map_[&inst] = dst;

    MachineInst::Op mop;
    switch (cmp->pred()) {
    case CmpPred::EQ:
        mop = MachineInst::Op::SEQ;
        break;
    case CmpPred::NE:
        mop = MachineInst::Op::SNE;
        break;
    case CmpPred::LT:
        mop = MachineInst::Op::SLT;
        break;
    case CmpPred::LE:
        mop = MachineInst::Op::SLE;
        break;
    case CmpPred::GT:
        mop = MachineInst::Op::SGT;
        break;
    case CmpPred::GE:
        mop = MachineInst::Op::SGE;
        break;
    default:
        assert(false);
        return;
    }

    cur_mbb_->push_back(MachineInst::make_r(mop, dst, s1, s2, cur_mbb_));
}

// ============================================================================
// 浮点比较
// ============================================================================

void SSAToMIRLowering::lower_fcmp(Instruction &inst) {
    auto *cmp = static_cast<CmpInst *>(&inst);
    auto *s1 = get_vreg(cmp->lhs());
    auto *s2 = get_vreg(cmp->rhs());
    // 浮点比较结果写入整数寄存器
    auto *dst = cur_mf_->alloc_vreg(RegClass::Int);
    vreg_map_[&inst] = dst;

    MachineInst::Op mop;
    switch (cmp->pred()) {
    case CmpPred::EQ:
        mop = MachineInst::Op::FEQ;
        break;
    case CmpPred::NE:
        mop = MachineInst::Op::FNE;
        break;
    case CmpPred::LT:
        mop = MachineInst::Op::FLT;
        break;
    case CmpPred::LE:
        mop = MachineInst::Op::FLE;
        break;
    case CmpPred::GT:
        mop = MachineInst::Op::FGT;
        break;
    case CmpPred::GE:
        mop = MachineInst::Op::FGE;
        break;
    default:
        assert(false);
        return;
    }

    cur_mbb_->push_back(MachineInst::make_r(mop, dst, s1, s2, cur_mbb_));
}

// ============================================================================
// 类型转换
// ============================================================================

void SSAToMIRLowering::lower_cast(Instruction &inst) {
    auto *cast = static_cast<CastInst *>(&inst);
    auto *src = get_vreg(cast->src());

    bool dst_is_float = inst.type()->is_float();
    auto *dst = cur_mf_->alloc_vreg(dst_is_float ? RegClass::Float : RegClass::Int);
    vreg_map_[&inst] = dst;

    switch (inst.op()) {
    case Instruction::OpID::ZExt: {
        // i1 -> i32: andi dst, src, 1
        cur_mbb_->push_back(MachineInst::make_r(MachineInst::Op::ZEXT, dst, src, src, cur_mbb_));
        break;
    }
    case Instruction::OpID::SIToFP: {
        // int -> float: fcvt.s.w dst, src
        cur_mbb_->push_back(
            MachineInst::make_r(MachineInst::Op::FCVT_S_W, dst, src, src, cur_mbb_));
        break;
    }
    case Instruction::OpID::FPToSI: {
        // float -> int: fcvt.w.s dst, src
        cur_mbb_->push_back(
            MachineInst::make_r(MachineInst::Op::FCVT_W_S, dst, src, src, cur_mbb_));
        break;
    }
    default:
        assert(false && "unknown cast op");
    }
}

// ============================================================================
// GEP
// ============================================================================

void SSAToMIRLowering::lower_gep(GetElementPtrInst &inst) {
    // GEP: 计算地址偏移
    // base_ptr + 0 * sizeof(elem) + idx1 * sizeof(elem) + ...
    // 在栈式模型中简化为: base_slot + offset

    auto *base_ptr = inst.base_ptr();
    auto indices = inst.indices(); // 第一个是 0, 后续是实际索引

    // 为 GEP 结果分配一个整数 VReg（地址值）
    auto *dst = cur_mf_->alloc_vreg(RegClass::Int);
    vreg_map_[&inst] = dst;

    // 检查 base 是否是 Alloca（栈变量）
    auto alloca_it = alloca_slots_.find(base_ptr);
    if (alloca_it != alloca_slots_.end()) {
        int base_slot = alloca_it->second; // 负偏移
        // 对于一维数组的 GEP: indices = [0, idx]
        // 结果地址对应的栈偏移 = base_slot + idx * elem_size
        // 简化处理：只支持简单的数组下标计算
        // elem_size = 4 (int 或 float)
        int elem_size = 4;

        if (indices.size() == 2) {
            // 简单数组访问: gep(base, 0, idx)
            auto *idx_val = indices[1];
            if (auto *ci = dynamic_cast<ConstantInt *>(idx_val)) {
                // 常量索引：直接计算偏移
                int offset = base_slot + static_cast<int>(ci->value()) * elem_size;
                // 将 GEP 结果映射为另一个 alloca-like slot
                alloca_slots_[&inst] = offset;
                // 不需要生成实际指令，后续 load/store 直接用 slot
                return;
            }
        }

        // 动态索引：需要运行时计算
        // 生成代码: LI dst, base_slot; 然后 ADD/LA 等计算实际偏移
        // 这是复杂场景，简化处理：将 GEP 标记为 alloca slot，运行时通过索引计算
        // 实际中需要展开为: LI tmp, base_offset; MUL idx, 4; ADD dst, tmp, idx
        if (indices.size() >= 2) {
            auto *idx_vreg = get_vreg(indices[1]);
            auto *tmp = alloc_int_vreg();
            auto *elem_size_reg = alloc_int_vreg();

            // tmp = idx * elem_size
            cur_mbb_->push_back(MachineInst::make_li(elem_size_reg, elem_size, cur_mbb_));
            cur_mbb_->push_back(
                MachineInst::make_r(MachineInst::Op::MUL, tmp, idx_vreg, elem_size_reg, cur_mbb_));

            // dst = base_slot + tmp
            auto *base_off_reg = alloc_int_vreg();
            cur_mbb_->push_back(MachineInst::make_li(base_off_reg, base_slot, cur_mbb_));
            cur_mbb_->push_back(
                MachineInst::make_r(MachineInst::Op::ADD, dst, base_off_reg, tmp, cur_mbb_));

            // GEP 结果不存入 alloca_slots_（因为是动态偏移）
            // 后续 load/store 需要用这个 dst 作为地址
        }
    } else {
        // base 不是 Alloca（可能是全局变量或另一个 GEP 结果）
        // 通过 VReg 计算地址
        auto *base_vreg = get_vreg(base_ptr);

        if (indices.size() >= 2) {
            auto *idx_vreg = get_vreg(indices[1]);
            auto *tmp = alloc_int_vreg();
            auto *elem_size_reg = alloc_int_vreg();

            cur_mbb_->push_back(MachineInst::make_li(elem_size_reg, 4, cur_mbb_));
            cur_mbb_->push_back(
                MachineInst::make_r(MachineInst::Op::MUL, tmp, idx_vreg, elem_size_reg, cur_mbb_));
            cur_mbb_->push_back(
                MachineInst::make_r(MachineInst::Op::ADD, dst, base_vreg, tmp, cur_mbb_));
        } else if (indices.size() == 1) {
            // GEP(base, 0) => base 本身
            cur_mbb_->push_back(MachineInst::make_mv(dst, base_vreg, cur_mbb_));
        }
    }
}

// ============================================================================
// Call
// ============================================================================

void SSAToMIRLowering::lower_call(CallInst &inst) {
    Value *callee = inst.callee();
    auto args = inst.args();

    // 获取被调用函数名
    std::string func_name;
    if (auto *func = dynamic_cast<Function *>(callee)) {
        func_name = func->name();
    } else {
        // 未知 callee，跳过
        return;
    }

    // 将参数搬运到 a0-a7 / fa0-fa7
    int int_arg_idx = 0;
    int float_arg_idx = 0;

    // 需要获取被调用函数的参数类型来决定用整数还是浮点寄存器传参
    // 遍历 args，根据类型分配寄存器
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto *arg_val = args[i];
        auto *arg_vreg = get_vreg(arg_val);

        if (arg_val->type()->is_float()) {
            // 浮点参数 -> fa0-fa7
            if (float_arg_idx < 8) {
                // 创建一个代表 faN 的 VReg 用于 MV
                // 但 MIR 目前没有物理寄存器 VReg 表示
                // 使用 STORE_SLOT 将参数存入临时栈槽，然后在 prologue 恢复
                // 实际上 CALL 使用 ABI 约定，参数已在寄存器中
                // 需要将虚拟寄存器值搬运到对应物理寄存器
                // 简化：生成 MV 到一个特殊 VReg（寄存器分配后处理）
                // 或者直接使用 STORE_SLOT
                // 这里用一个 trick：将参数存到栈上临时位置，CALL 前恢复到 arg 寄存器
                // 但 MachineFunction::emit prologue 已经做了参数恢复
                // 实际上在 lowering 阶段我们应该生成 MV dst, src，其中 dst 代表物理寄存器

                // 方案：暂时使用 STORE_SLOT 存到一个约定的栈槽
                // 后续寄存器分配时再处理
                // 更好的方案：直接 emit MV 并让寄存器分配器处理
                auto *arg_reg = alloc_float_vreg(); // 占位
                cur_mbb_->push_back(MachineInst::make_mv(arg_reg, arg_vreg, cur_mbb_));
                float_arg_idx++;
            }
        } else {
            // 整数参数 -> a0-a7
            if (int_arg_idx < 8) {
                auto *arg_reg = alloc_int_vreg(); // 占位
                cur_mbb_->push_back(MachineInst::make_mv(arg_reg, arg_vreg, cur_mbb_));
                int_arg_idx++;
            }
        }
    }

    // 生成 CALL
    cur_mbb_->push_back(MachineInst::make_call(func_name, cur_mbb_));

    // 返回值：a0 (整数) 或 fa0 (浮点)
    if (!inst.type()->is_void()) {
        bool ret_float = inst.type()->is_float();
        auto *ret_reg = cur_mf_->alloc_vreg(ret_float ? RegClass::Float : RegClass::Int);
        vreg_map_[&inst] = ret_reg;

        // 返回值在 a0/fa0，需要搬运到虚拟寄存器
        // 创建一个代表 a0/fa0 的临时 VReg
        auto *phys_ret = alloc_int_vreg(); // 占位，实际应为 a0/fa0
        cur_mbb_->push_back(MachineInst::make_mv(ret_reg, phys_ret, cur_mbb_));
    }
}

// ============================================================================
// Return
// ============================================================================

void SSAToMIRLowering::lower_ret(Instruction &inst) {
    auto *ret = static_cast<ReturnInst *>(&inst);

    if (ret->has_value()) {
        auto *val = get_vreg(ret->value());
        cur_mbb_->push_back(MachineInst::make_ret(val, cur_mbb_));
    } else {
        cur_mbb_->push_back(MachineInst::make_ret(nullptr, cur_mbb_));
    }
}

// ============================================================================
// Branch
// ============================================================================

void SSAToMIRLowering::lower_br(Instruction &inst) {
    auto *br = static_cast<BranchInst *>(&inst);

    if (br->is_conditional()) {
        auto *cond = get_vreg(br->cond());
        auto *zero = alloc_int_vreg();
        auto *true_mbb = bb_map_[br->true_bb()];
        auto *false_mbb = bb_map_[br->false_bb()];

        // cond 是 i1 (比较结果 0 或 1)，与 0 比较
        cur_mbb_->push_back(MachineInst::make_li(zero, 0, cur_mbb_));

        // bne cond, zero, true_bb; j false_bb
        cur_mbb_->push_back(
            MachineInst::make_branch(MachineInst::Op::BNE, cond, zero, true_mbb, cur_mbb_));
        cur_mbb_->push_back(MachineInst::make_j(false_mbb, cur_mbb_));
    } else {
        auto *target = bb_map_[br->target_bb()];
        cur_mbb_->push_back(MachineInst::make_j(target, cur_mbb_));
    }
}

// ============================================================================
// Phi
// ============================================================================

void SSAToMIRLowering::lower_phi(PhiInst &inst) {
    // Phi 不直接生成代码，只分配目标 VReg 并收集待处理 Phi
    bool is_float = inst.type()->is_float();
    auto *dst = cur_mf_->alloc_vreg(is_float ? RegClass::Float : RegClass::Int);
    vreg_map_[&inst] = dst;

    pending_phis_.push_back(&inst);
}

// ============================================================================
// 辅助方法
// ============================================================================

VReg *SSAToMIRLowering::get_vreg(Value *val) {
    // 查找已有的 VReg 映射
    auto it = vreg_map_.find(val);
    if (it != vreg_map_.end()) {
        return it->second;
    }

    // 常量需要物化
    return materialize_constant(val);
}

VReg *SSAToMIRLowering::materialize_constant(Value *val) {
    // 常量整数
    if (auto *ci = dynamic_cast<ConstantInt *>(val)) {
        auto *vreg = alloc_int_vreg();
        vreg_map_[val] = vreg;
        int32_t imm = static_cast<int32_t>(ci->value());
        cur_mbb_->push_back(MachineInst::make_li(vreg, imm, cur_mbb_));
        return vreg;
    }

    // 常量浮点数
    if (auto *cf = dynamic_cast<ConstantFloat *>(val)) {
        auto *vreg = alloc_float_vreg();
        vreg_map_[val] = vreg;
        // 浮点常量通过 LA 加载 .rodata 中的常量池条目
        // 简化：使用 FCVT_S_W 将整数转换为浮点
        // 或使用伪指令: li int_reg, bits; fmv.x.w float_reg, int_reg
        // 这里用一个 hack：LI 整数位模式然后 FMV
        auto *int_reg = alloc_int_vreg();
        float f = cf->value();
        int32_t bits;
        std::memcpy(&bits, &f, sizeof(float));
        cur_mbb_->push_back(MachineInst::make_li(int_reg, bits, cur_mbb_));
        // FMV: 将整数寄存器的位模式搬到浮点寄存器
        cur_mbb_->push_back(
            MachineInst::make_r(MachineInst::Op::FMV, vreg, int_reg, int_reg, cur_mbb_));
        return vreg;
    }

    // Undef
    if (dynamic_cast<UndefValue *>(val)) {
        auto *vreg = alloc_int_vreg();
        vreg_map_[val] = vreg;
        cur_mbb_->push_back(MachineInst::make_li(vreg, 0, cur_mbb_));
        return vreg;
    }

    // GlobalVariable: 加载地址
    if (auto *gv = dynamic_cast<GlobalVariable *>(val)) {
        auto *vreg = alloc_int_vreg();
        vreg_map_[val] = vreg;
        cur_mbb_->push_back(MachineInst::make_la(vreg, gv->name(), cur_mbb_));
        return vreg;
    }

    // Function: 加载函数地址
    if (auto *func = dynamic_cast<Function *>(val)) {
        auto *vreg = alloc_int_vreg();
        vreg_map_[val] = vreg;
        cur_mbb_->push_back(MachineInst::make_la(vreg, func->name(), cur_mbb_));
        return vreg;
    }

    // 未找到，创建一个默认的零值 VReg
    assert(false && "unknown value in get_vreg");
    auto *vreg = alloc_int_vreg();
    vreg_map_[val] = vreg;
    cur_mbb_->push_back(MachineInst::make_li(vreg, 0, cur_mbb_));
    return vreg;
}

VReg *SSAToMIRLowering::alloc_int_vreg() {
    assert(cur_mf_ != nullptr);
    return cur_mf_->alloc_vreg(RegClass::Int);
}

VReg *SSAToMIRLowering::alloc_float_vreg() {
    assert(cur_mf_ != nullptr);
    return cur_mf_->alloc_vreg(RegClass::Float);
}

} // namespace passes
