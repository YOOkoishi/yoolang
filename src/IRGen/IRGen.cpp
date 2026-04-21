#include "../../include/IRGen/IRGen.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace irgen {

namespace {

// -------------
// 常量折叠（仅在 lowering 前期、针对字面量构成的简单表达式）
// 主要用于：数组维度、常量初始化器、全局初始化。
// --------------------

struct ConstValue {
    enum class Kind { None, Int, Float };
    Kind kind = Kind::None;
    std::int64_t i = 0;
    float f = 0.0f;

    static ConstValue make_int(std::int64_t v) {
        ConstValue r;
        r.kind = Kind::Int;
        r.i = v;
        return r;
    }
    static ConstValue make_float(float v) {
        ConstValue r;
        r.kind = Kind::Float;
        r.f = v;
        return r;
    }

    bool is_int() const {
        return kind == Kind::Int;
    }
    bool is_float() const {
        return kind == Kind::Float;
    }
    bool ok() const {
        return kind != Kind::None;
    }

    float as_float() const {
        return is_float() ? f : static_cast<float>(i);
    }
    std::int64_t as_int() const {
        return is_int() ? i : static_cast<std::int64_t>(f);
    }
};

ConstValue eval_const_expr(Expr *expr) {
    if (expr == nullptr) {
        return {};
    }
    if (auto *lit = dynamic_cast<IntLiteral *>(expr)) {
        return ConstValue::make_int(lit->value);
    }
    if (auto *lit = dynamic_cast<FloatLiteral *>(expr)) {
        return ConstValue::make_float(lit->value);
    }
    if (auto *u = dynamic_cast<UnaryExpr *>(expr)) {
        auto v = eval_const_expr(u->operand.get());
        if (!v.ok())
            return {};
        switch (u->op) {
        case UnaryOp::Pos:
            return v;
        case UnaryOp::Neg:
            return v.is_int() ? ConstValue::make_int(-v.i) : ConstValue::make_float(-v.f);
        case UnaryOp::Not:
            return ConstValue::make_int(v.is_int() ? (v.i == 0) : (v.f == 0.0f));
        }
        return {};
    }
    if (auto *b = dynamic_cast<BinaryExpr *>(expr)) {
        auto l = eval_const_expr(b->lhs.get());
        auto r = eval_const_expr(b->rhs.get());
        if (!l.ok() || !r.ok())
            return {};
        const bool use_float = l.is_float() || r.is_float();
        auto li = l.as_int();
        auto ri = r.as_int();
        auto lf = l.as_float();
        auto rf = r.as_float();
        switch (b->op) {
        case BinaryOp::Add:
            return use_float ? ConstValue::make_float(lf + rf) : ConstValue::make_int(li + ri);
        case BinaryOp::Sub:
            return use_float ? ConstValue::make_float(lf - rf) : ConstValue::make_int(li - ri);
        case BinaryOp::Mul:
            return use_float ? ConstValue::make_float(lf * rf) : ConstValue::make_int(li * ri);
        case BinaryOp::Div:
            if (use_float)
                return ConstValue::make_float(lf / rf);
            if (ri == 0)
                return {};
            return ConstValue::make_int(li / ri);
        case BinaryOp::Mod:
            if (use_float || ri == 0)
                return {};
            return ConstValue::make_int(li % ri);
        case BinaryOp::Lt:
            return ConstValue::make_int(use_float ? (lf < rf) : (li < ri));
        case BinaryOp::Le:
            return ConstValue::make_int(use_float ? (lf <= rf) : (li <= ri));
        case BinaryOp::Gt:
            return ConstValue::make_int(use_float ? (lf > rf) : (li > ri));
        case BinaryOp::Ge:
            return ConstValue::make_int(use_float ? (lf >= rf) : (li >= ri));
        case BinaryOp::Eq:
            return ConstValue::make_int(use_float ? (lf == rf) : (li == ri));
        case BinaryOp::Ne:
            return ConstValue::make_int(use_float ? (lf != rf) : (li != ri));
        case BinaryOp::And:
            return ConstValue::make_int((use_float ? lf != 0.0f : li != 0) &&
                                        (use_float ? rf != 0.0f : ri != 0));
        case BinaryOp::Or:
            return ConstValue::make_int((use_float ? lf != 0.0f : li != 0) ||
                                        (use_float ? rf != 0.0f : ri != 0));
        }
    }
    return {};
}

std::size_t eval_dimension(const std::unique_ptr<Expr> &expr) {
    if (expr == nullptr) {
        return 1;
    }

    auto v = eval_const_expr(expr.get());
    if (v.is_int() && v.i > 0) {
        return static_cast<std::size_t>(v.i);
    }
    return 1;
}

ir::Value *try_lower_global_scalar_init(ir::Module &module, const VarDecl &decl,
                                        ir::Type *value_type) {
    if (decl.init == nullptr || decl.init->expr == nullptr) {
        return nullptr;
    }

    auto v = eval_const_expr(decl.init->expr.get());
    if (!v.ok()) {
        return nullptr;
    }

    if (value_type->is_integer()) {
        return module.create_i32(v.as_int());
    }
    if (value_type->is_float()) {
        return module.create_f32(v.as_float());
    }
    return nullptr;
}

ir::Type *build_decl_type(ir::TypeContext &types, ir::Type *base_type,
                          const std::vector<std::unique_ptr<Expr>> &dimensions) {
    ir::Type *current = base_type;
    for (auto it = dimensions.rbegin(); it != dimensions.rend(); ++it) {
        current = types.array_ty(current, eval_dimension(*it));
    }
    return current;
}

// =====================================================================
// FunctionLowering — 负责单个函数体内所有 AST 节点到 IR 的翻译
// =====================================================================

// 前向声明（定义在 FunctionLowering 之后）
ir::Type *map_builtin_type_local(ir::TypeContext &types, BuiltinType type);

class FunctionLowering {
  public:
    FunctionLowering(ir::Module *module, ir::Function *func)
        : module_(module), func_(func), builder_(module), entry_block_(nullptr), unique_id_(0) {
    }

    void run(FuncDef &ast_func) {
        entry_block_ = func_->create_block("entry");
        builder_.set_insert_point(entry_block_);

        // 参数 alloca + store
        const auto n = std::min(func_->args().size(), ast_func.params.size());
        for (std::size_t i = 0; i < n; ++i) {
            auto &param_ast = ast_func.params[i];
            auto *arg = func_->args()[i].get();
            ir::Type *arg_ty = arg->type();

            // 如果参数是指针类型（数组参数），alloca 一个 T* 槽；否则 alloca 标量
            ir::Type *alloca_ty = arg_ty;
            auto *alloca = emit_alloca(alloca_ty, param_ast.name);
            builder_.create_store(arg, alloca);
            push_scope();
            symbol_table_[param_ast.name] = alloca;
        }

        if (n == 0) {
            push_scope(); // 确保至少一层作用域
        }

        // Lower 函数体
        bool terminated = false;
        if (ast_func.body) {
            terminated = lower_block_body(*ast_func.body);
        }

        // 如果没有终结指令，插入默认 return
        if (!terminated) {
            emit_default_ret();
        }
    }

  private:
    ir::Module *module_;
    ir::Function *func_;
    ir::IRBuilder builder_;
    ir::BasicBlock *entry_block_; // entry 块指针，alloca 始终插入此处
    int unique_id_;               // 唯一 ID 计数器
    std::vector<std::unordered_map<std::string, ir::Value *>> scopes_;
    std::unordered_map<std::string, ir::Value *> symbol_table_;

    // break/continue 的跳转目标栈
    std::vector<ir::BasicBlock *> break_stack_;
    std::vector<ir::BasicBlock *> continue_stack_;

    // ---- 唯一名称生成 ----

    std::string unique_name(const std::string &base) {
        return base + "." + std::to_string(unique_id_++);
    }

    // ---- 在 entry 块插入 alloca ----

    ir::AllocaInst *emit_alloca(ir::Type *ty, const std::string &name_hint) {
        // 始终在 entry 块的插入点创建 alloca（标准 SSA 做法）
        auto *saved_bb = builder_.insert_block();
        builder_.set_insert_point(entry_block_);
        auto *inst = builder_.create_alloca(ty, unique_name(name_hint));
        builder_.set_insert_point(saved_bb);
        return inst;
    }

    // ---- 作用域管理 ----

    void push_scope() {
        scopes_.emplace_back();
    }

    void pop_scope() {
        if (scopes_.empty())
            return;
        auto &top = scopes_.back();
        for (auto &[k, v] : top) {
            symbol_table_.erase(k);
        }
        scopes_.pop_back();
    }

    void define(const std::string &name, ir::Value *ptr) {
        scopes_.back()[name] = ptr;
        symbol_table_[name] = ptr;
    }

    ir::Value *lookup(const std::string &name) const {
        auto it = symbol_table_.find(name);
        return it != symbol_table_.end() ? it->second : nullptr;
    }

    ir::TypeContext &types() {
        return module_->types();
    }

    // ---- 默认 return ----

    void emit_default_ret() {
        auto *ret_ty = func_->return_type();
        if (ret_ty->is_void()) {
            builder_.create_ret(nullptr);
        } else if (ret_ty->is_float()) {
            builder_.create_ret(builder_.f32(0.0f));
        } else {
            builder_.create_ret(builder_.i32(0));
        }
    }

    // ---- 语句 lowering ----
    // 返回 true 表示当前插入点已经有终结指令（不应再追加指令）

    bool lower_stmt(Stmt &stmt) {
        if (auto *s = dynamic_cast<ExprStmt *>(&stmt))
            return lower_expr_stmt(*s);
        if (auto *s = dynamic_cast<AssignStmt *>(&stmt))
            return lower_assign(*s);
        if (auto *s = dynamic_cast<BlockStmt *>(&stmt))
            return lower_block(*s);
        if (auto *s = dynamic_cast<ReturnStmt *>(&stmt))
            return lower_return(*s);
        if (auto *s = dynamic_cast<IfStmt *>(&stmt))
            return lower_if(*s);
        if (auto *s = dynamic_cast<WhileStmt *>(&stmt))
            return lower_while(*s);
        if (auto *s = dynamic_cast<BreakStmt *>(&stmt))
            return lower_break(*s);
        if (auto *s = dynamic_cast<ContinueStmt *>(&stmt))
            return lower_continue(*s);
        if (auto *s = dynamic_cast<DeclStmt *>(&stmt))
            return lower_decl_stmt(*s);
        return false;
    }

    bool lower_expr_stmt(ExprStmt &stmt) {
        if (stmt.expr) {
            lower_expr(*stmt.expr);
        }
        return false;
    }

    bool lower_assign(AssignStmt &stmt) {
        ir::Value *val = lower_expr(*stmt.value);

        auto &target = *stmt.target;
        ir::Value *ptr = lookup(target.name);
        if (ptr == nullptr)
            return false;

        if (target.indices.empty()) {
            // 标量赋值
            val = unify_type_for_store(val, ptr);
            builder_.create_store(val, ptr);
        } else {
            // 数组元素赋值：计算 GEP 然后 store
            // 如果 ptr 是 alloca 且指向指针类型（数组参数的 alloca 槽），先 load 获取真正的数组基址
            if (auto *pt = dynamic_cast<ir::PointerType *>(ptr->type())) {
                if (pt->element_type()->is_pointer()) {
                    ptr = builder_.create_load(ptr, pt->element_type(), target.name + ".ptr");
                }
            }
            auto *gep = compute_array_gep(ptr, target.indices);
            val = unify_type_for_store(val, gep);
            builder_.create_store(val, gep);
        }
        return false;
    }

    bool lower_block(BlockStmt &block) {
        return lower_block_body(block);
    }

    // Lower 块内部的语句序列（不创建新作用域——由调用方决定）
    bool lower_block_body(BlockStmt &block) {
        push_scope();
        bool terminated = false;
        for (auto &s : block.stmts) {
            if (terminated)
                break;
            if (s)
                terminated = lower_stmt(*s);
        }
        pop_scope();
        return terminated;
    }

    bool lower_return(ReturnStmt &stmt) {
        if (stmt.expr) {
            ir::Value *val = lower_expr(*stmt.expr);
            val = unify_for_func_return(val);
            builder_.create_ret(val);
        } else {
            builder_.create_ret(nullptr);
        }
        return true;
    }

    bool lower_if(IfStmt &stmt) {
        ir::Value *cond = lower_expr(*stmt.cond);
        cond = to_i1(cond);

        auto *true_bb = func_->create_block("if.then");
        auto *false_bb = func_->create_block("if.else");
        auto *merge_bb = func_->create_block("if.end");

        builder_.create_cond_br(cond, true_bb, false_bb);

        // then
        builder_.set_insert_point(true_bb);
        bool then_term = stmt.then_stmt ? lower_stmt(*stmt.then_stmt) : false;
        if (!then_term)
            builder_.create_br(merge_bb);

        // else
        builder_.set_insert_point(false_bb);
        bool else_term = false;
        if (stmt.else_stmt) {
            else_term = lower_stmt(*stmt.else_stmt);
        }
        if (!else_term)
            builder_.create_br(merge_bb);

        // merge — 如果两个分支都 terminated，merge 可能不可达；仍创建
        builder_.set_insert_point(merge_bb);
        return false;
    }

    bool lower_while(WhileStmt &stmt) {
        auto *cond_bb = func_->create_block("while.cond");
        auto *body_bb = func_->create_block("while.body");
        auto *end_bb = func_->create_block("while.end");

        break_stack_.push_back(end_bb);
        continue_stack_.push_back(cond_bb);

        builder_.create_br(cond_bb);

        // condition
        builder_.set_insert_point(cond_bb);
        ir::Value *cond = lower_expr(*stmt.cond);
        cond = to_i1(cond);
        builder_.create_cond_br(cond, body_bb, end_bb);

        // body
        builder_.set_insert_point(body_bb);
        bool body_term = stmt.body ? lower_stmt(*stmt.body) : false;
        if (!body_term)
            builder_.create_br(cond_bb);

        break_stack_.pop_back();
        continue_stack_.pop_back();

        builder_.set_insert_point(end_bb);
        return false;
    }

    bool lower_break(BreakStmt &) {
        if (!break_stack_.empty()) {
            builder_.create_br(break_stack_.back());
        }
        return true;
    }

    bool lower_continue(ContinueStmt &) {
        if (!continue_stack_.empty()) {
            builder_.create_br(continue_stack_.back());
        }
        return true;
    }

    bool lower_decl_stmt(DeclStmt &stmt) {
        for (auto &decl : stmt.decls) {
            if (decl)
                lower_local_var_decl(*decl);
        }
        return false;
    }

    void lower_local_var_decl(VarDecl &decl) {
        ir::Type *base = map_builtin_type_local(types(), decl.base_type);
        ir::Type *value_type = build_decl_type(types(), base, decl.dimensions);
        auto *alloca = emit_alloca(value_type, decl.name);
        define(decl.name, alloca);

        // 初始化
        if (decl.init) {
            if (decl.init->expr && decl.dimensions.empty()) {
                // 标量初始化
                ir::Value *val = lower_expr(*decl.init->expr);
                val = unify_type_for_store(val, alloca);
                builder_.create_store(val, alloca);
            }
            // 聚合初始化（数组等）暂不处理，运行时会零初始化
        }
    }

    // ---- 表达式 lowering ----

    ir::Value *lower_expr(Expr &expr) {
        if (auto *e = dynamic_cast<IntLiteral *>(&expr))
            return lower_int_literal(*e);
        if (auto *e = dynamic_cast<FloatLiteral *>(&expr))
            return lower_float_literal(*e);
        if (auto *e = dynamic_cast<LValExpr *>(&expr))
            return lower_lval(*e);
        if (auto *e = dynamic_cast<BinaryExpr *>(&expr))
            return lower_binary(*e);
        if (auto *e = dynamic_cast<UnaryExpr *>(&expr))
            return lower_unary(*e);
        if (auto *e = dynamic_cast<CallExpr *>(&expr))
            return lower_call(*e);
        return builder_.i32(0);
    }

    ir::Value *lower_int_literal(IntLiteral &e) {
        return builder_.i32(e.value);
    }

    ir::Value *lower_float_literal(FloatLiteral &e) {
        return builder_.f32(e.value);
    }

    ir::Value *lower_lval(LValExpr &e) {
        ir::Value *ptr = lookup(e.name);
        if (ptr == nullptr) {
            // 可能是全局变量
            auto *gv = module_->get_global(e.name);
            if (gv)
                ptr = gv;
            else
                return builder_.i32(0);
        }

        if (e.indices.empty()) {
            // 标量：load
            return builder_.create_load(ptr, loaded_type(ptr), e.name);
        } else {
            // 数组元素：GEP + load
            // 如果 ptr 是 alloca 且指向指针类型（数组参数的 alloca 槽），先 load 获取真正的数组基址
            if (auto *pt = dynamic_cast<ir::PointerType *>(ptr->type())) {
                if (pt->element_type()->is_pointer()) {
                    ptr = builder_.create_load(ptr, pt->element_type(), e.name + ".ptr");
                }
            }
            auto *gep = compute_array_gep(ptr, e.indices);
            return builder_.create_load(gep, loaded_type(gep), e.name);
        }
    }

    ir::Value *lower_binary(BinaryExpr &e) {
        // 短路逻辑运算
        if (e.op == BinaryOp::And)
            return lower_logical_and(e);
        if (e.op == BinaryOp::Or)
            return lower_logical_or(e);

        ir::Value *lhs = lower_expr(*e.lhs);
        ir::Value *rhs = lower_expr(*e.rhs);

        // 自动类型统一：如果有一方是 float，另一方 sitofp
        if (lhs->type()->is_float() && rhs->type()->is_integer()) {
            rhs = builder_.create_sitofp(rhs, types().float_ty());
        } else if (lhs->type()->is_integer() && rhs->type()->is_float()) {
            lhs = builder_.create_sitofp(lhs, types().float_ty());
        }

        const bool is_float = lhs->type()->is_float();

        // 比较运算
        switch (e.op) {
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge:
        case BinaryOp::Eq:
        case BinaryOp::Ne: {
            auto pred = binary_to_cmp_pred(e.op);
            ir::CmpInst *cmp = is_float ? builder_.create_fcmp(pred, lhs, rhs, "cmp")
                                        : builder_.create_icmp(pred, lhs, rhs, "cmp");
            // zext i1 -> i32 (SysY 语义)
            return builder_.create_zext(cmp, types().int32_ty(), "bool");
        }
        default:
            break;
        }

        // 算术运算
        auto opcode = binary_to_opcode(e.op, is_float);
        return builder_.create_binary(opcode, lhs, rhs, "bin");
    }

    ir::Value *lower_logical_and(BinaryExpr &e) {
        auto *rhs_bb = func_->create_block("and.rhs");
        auto *merge_bb = func_->create_block("and.end");

        ir::Value *lhs = lower_expr(*e.lhs);
        ir::Value *lhs_bool = to_i1(lhs);
        auto *lhs_bb = builder_.insert_block(); // 当前块（lhs 计算完成后的块）
        builder_.create_cond_br(lhs_bool, rhs_bb, merge_bb);

        builder_.set_insert_point(rhs_bb);
        ir::Value *rhs = lower_expr(*e.rhs);
        ir::Value *rhs_bool = to_i1(rhs);
        auto *rhs_done_bb = builder_.insert_block(); // rhs 计算完成后的块
        builder_.create_br(merge_bb);

        builder_.set_insert_point(merge_bb);
        auto *phi = builder_.create_phi(types().int32_ty(), "and");
        phi->add_incoming(builder_.i32(0), lhs_bb);
        phi->add_incoming(builder_.create_zext(rhs_bool, types().int32_ty()), rhs_done_bb);
        return phi;
    }

    ir::Value *lower_logical_or(BinaryExpr &e) {
        auto *rhs_bb = func_->create_block("or.rhs");
        auto *merge_bb = func_->create_block("or.end");

        ir::Value *lhs = lower_expr(*e.lhs);
        ir::Value *lhs_bool = to_i1(lhs);
        auto *lhs_bb = builder_.insert_block();
        builder_.create_cond_br(lhs_bool, merge_bb, rhs_bb);

        builder_.set_insert_point(rhs_bb);
        ir::Value *rhs = lower_expr(*e.rhs);
        ir::Value *rhs_bool = to_i1(rhs);
        auto *rhs_done_bb = builder_.insert_block();
        builder_.create_br(merge_bb);

        builder_.set_insert_point(merge_bb);
        auto *phi = builder_.create_phi(types().int32_ty(), "or");
        phi->add_incoming(builder_.i32(1), lhs_bb);
        phi->add_incoming(builder_.create_zext(rhs_bool, types().int32_ty()), rhs_done_bb);
        return phi;
    }

    ir::Value *lower_unary(UnaryExpr &e) {
        ir::Value *operand = lower_expr(*e.operand);
        switch (e.op) {
        case UnaryOp::Pos:
            return operand;
        case UnaryOp::Neg:
            if (operand->type()->is_float()) {
                return builder_.create_binary(ir::Instruction::OpID::FSub, builder_.f32(0.0f),
                                              operand, "neg");
            }
            return builder_.create_binary(ir::Instruction::OpID::Sub, builder_.i32(0), operand,
                                          "neg");
        case UnaryOp::Not: {
            // !x  =>  (x == 0)  =>  icmp eq x, 0, 然后 zext
            ir::CmpInst *cmp;
            if (operand->type()->is_float()) {
                cmp = builder_.create_fcmp(ir::CmpPred::EQ, operand, builder_.f32(0.0f), "not");
            } else {
                cmp = builder_.create_icmp(ir::CmpPred::EQ, operand, builder_.i32(0), "not");
            }
            return builder_.create_zext(cmp, types().int32_ty(), "not");
        }
        }
        return operand;
    }

    ir::Value *lower_call(CallExpr &e) {
        auto *callee = module_->get_function(e.func_name);
        if (callee == nullptr)
            return builder_.i32(0);

        std::vector<ir::Value *> args;
        args.reserve(e.args.size());
        for (auto &a : e.args) {
            args.push_back(lower_expr(*a));
        }

        // 调整参数类型（int<->float 转换）
        auto &func_args = callee->args();
        for (std::size_t i = 0; i < args.size() && i < func_args.size(); ++i) {
            args[i] = unify_for_param(args[i], func_args[i]->type());
        }

        auto name = callee->return_type()->is_void() ? "" : "call";
        return builder_.create_call(callee, callee->return_type(), args, name);
    }

    // ---- 辅助 ----

    // 获取指针指向的类型
    ir::Type *loaded_type(ir::Value *ptr) {
        auto *pt = dynamic_cast<ir::PointerType *>(ptr->type());
        return pt ? pt->element_type() : types().int32_ty();
    }

    // 计算数组下标 GEP，返回指向元素的指针
    ir::Value *compute_array_gep(ir::Value *base_ptr,
                                 const std::vector<std::unique_ptr<Expr>> &indices) {
        std::vector<ir::Value *> gep_indices;
        gep_indices.push_back(builder_.i32(0)); // 第一维偏移为 0

        for (auto &idx_expr : indices) {
            ir::Value *idx = lower_expr(*idx_expr);
            gep_indices.push_back(idx);
        }

        // GEP 结果类型：指针指向基础元素类型
        // base_ptr 是 T*（标量指针）或 T[N]*（数组指针）...
        // 简化：计算 gep 后的指针类型为 base_element_type*
        ir::Type *elem_ptr_type = compute_gep_result_type(base_ptr);
        return builder_.create_gep(base_ptr, elem_ptr_type, gep_indices, "gep");
    }

    ir::Type *compute_gep_result_type(ir::Value *ptr) {
        // 递归剥到最内层元素类型
        ir::Type *ty = loaded_type(ptr);
        while (ty->is_array()) {
            auto *arr = dynamic_cast<ir::ArrayType *>(ty);
            ty = arr->element_type();
        }
        return types().ptr_ty(ty);
    }

    // i32/i1 -> i1 用于条件判断
    ir::Value *to_i1(ir::Value *val) {
        if (val->type()->is_integer() && val->type() == types().int1_ty()) {
            return val;
        }
        if (val->type()->is_float()) {
            return builder_.create_fcmp(ir::CmpPred::NE, val, builder_.f32(0.0f), "tobool");
        }
        return builder_.create_icmp(ir::CmpPred::NE, val, builder_.i32(0), "tobool");
    }

    // 将 val 转为与 ptr 所指类型匹配的类型，用于 store
    ir::Value *unify_type_for_store(ir::Value *val, ir::Value *ptr) {
        ir::Type *target = loaded_type(ptr);
        if (val->type() == target)
            return val;
        if (target->is_float() && val->type()->is_integer()) {
            return builder_.create_sitofp(val, types().float_ty());
        }
        if (target->is_integer() && val->type()->is_float()) {
            return builder_.create_fptosi(val, types().int32_ty());
        }
        return val;
    }

    // 将 val 转为函数返回类型
    ir::Value *unify_for_func_return(ir::Value *val) {
        auto *ret_ty = func_->return_type();
        if (val->type() == ret_ty)
            return val;
        if (ret_ty->is_float() && val->type()->is_integer()) {
            return builder_.create_sitofp(val, types().float_ty());
        }
        if (ret_ty->is_integer() && val->type()->is_float()) {
            return builder_.create_fptosi(val, types().int32_ty());
        }
        return val;
    }

    // 将 val 转为参数类型
    ir::Value *unify_for_param(ir::Value *val, ir::Type *param_ty) {
        if (val->type() == param_ty)
            return val;
        if (param_ty->is_float() && val->type()->is_integer()) {
            return builder_.create_sitofp(val, types().float_ty());
        }
        if (param_ty->is_integer() && val->type()->is_float()) {
            return builder_.create_fptosi(val, types().int32_ty());
        }
        return val;
    }

    static ir::Instruction::OpID binary_to_opcode(BinaryOp op, bool is_float) {
        switch (op) {
        case BinaryOp::Add:
            return is_float ? ir::Instruction::OpID::FAdd : ir::Instruction::OpID::Add;
        case BinaryOp::Sub:
            return is_float ? ir::Instruction::OpID::FSub : ir::Instruction::OpID::Sub;
        case BinaryOp::Mul:
            return is_float ? ir::Instruction::OpID::FMul : ir::Instruction::OpID::Mul;
        case BinaryOp::Div:
            return is_float ? ir::Instruction::OpID::FDiv : ir::Instruction::OpID::SDiv;
        case BinaryOp::Mod:
            return ir::Instruction::OpID::SRem;
        default:
            return ir::Instruction::OpID::Add;
        }
    }

    static ir::CmpPred binary_to_cmp_pred(BinaryOp op) {
        switch (op) {
        case BinaryOp::Lt:
            return ir::CmpPred::LT;
        case BinaryOp::Le:
            return ir::CmpPred::LE;
        case BinaryOp::Gt:
            return ir::CmpPred::GT;
        case BinaryOp::Ge:
            return ir::CmpPred::GE;
        case BinaryOp::Eq:
            return ir::CmpPred::EQ;
        case BinaryOp::Ne:
            return ir::CmpPred::NE;
        default:
            return ir::CmpPred::EQ;
        }
    }
};

ir::Type *map_builtin_type_local(ir::TypeContext &types, BuiltinType type) {
    switch (type) {
    case BuiltinType::Void:
        return types.void_ty();
    case BuiltinType::Int:
        return types.int32_ty();
    case BuiltinType::Float:
        return types.float_ty();
    }
    return types.int32_ty();
}

} // namespace

ASTToIRLowering::ASTToIRLowering(LoweringOptions options) : options_(options) {
}

std::unique_ptr<ir::Module> ASTToIRLowering::lower(CompUnit &unit, const std::string &module_name) {
    auto module = std::make_unique<ir::Module>(module_name);

    if (options_.declare_runtime_builtins) {
        declare_runtime_builtins(*module);
    }

    lower_global_decls(*module, unit);
    lower_function_signatures(*module, unit);
    lower_function_bodies(*module, unit);

    return module;
}

ir::Type *ASTToIRLowering::map_builtin_type(ir::TypeContext &types, BuiltinType type) const {
    switch (type) {
    case BuiltinType::Void:
        return types.void_ty();
    case BuiltinType::Int:
        return types.int32_ty();
    case BuiltinType::Float:
        return types.float_ty();
    }

    return types.int32_ty();
}

std::vector<ir::Type *>
ASTToIRLowering::map_param_types(ir::TypeContext &types,
                                 const std::vector<FuncParam> &params) const {
    std::vector<ir::Type *> param_types;
    param_types.reserve(params.size());

    for (const auto &param : params) {
        ir::Type *base = map_builtin_type(types, param.type);
        if (!param.dimensions.empty()) {
            param_types.push_back(types.ptr_ty(base));
            continue;
        }
        param_types.push_back(base);
    }

    return param_types;
}

void ASTToIRLowering::declare_runtime_builtins(ir::Module &module) {
    auto &types = module.types();

    auto declare_fn = [&](const std::string &name, ir::Type *ret,
                          const std::vector<ir::Type *> &params) {
        auto *fn_type = types.func_ty(ret, params);
        module.create_function(name, fn_type, true);
    };

    auto *i32 = types.int32_ty();
    auto *f32 = types.float_ty();
    auto *void_ty = types.void_ty();
    auto *pi32 = types.ptr_ty(i32);
    auto *pf32 = types.ptr_ty(f32);

    declare_fn("getint", i32, {});
    declare_fn("getch", i32, {});
    declare_fn("getfloat", f32, {});
    declare_fn("getarray", i32, {pi32});
    declare_fn("getfarray", i32, {pf32});
    declare_fn("putint", void_ty, {i32});
    declare_fn("putch", void_ty, {i32});
    declare_fn("putfloat", void_ty, {f32});
    declare_fn("putarray", void_ty, {i32, pi32});
    declare_fn("putfarray", void_ty, {i32, pf32});
    declare_fn("starttime", void_ty, {});
    declare_fn("stoptime", void_ty, {});
}

void ASTToIRLowering::lower_global_decls(ir::Module &module, CompUnit &unit) {
    auto &types = module.types();

    for (auto &decl_stmt : unit.global_decls) {
        if (decl_stmt == nullptr) {
            continue;
        }

        for (auto &decl : decl_stmt->decls) {
            if (decl == nullptr) {
                continue;
            }

            ir::Type *base_type = map_builtin_type(types, decl->base_type);
            ir::Type *value_type = build_decl_type(types, base_type, decl->dimensions);
            ir::Value *init_value = try_lower_global_scalar_init(module, *decl, value_type);
            module.create_global(decl->name, value_type, decl->is_const, init_value);
        }
    }
}

void ASTToIRLowering::lower_function_signatures(ir::Module &module, CompUnit &unit) {
    auto &types = module.types();

    for (auto &func : unit.functions) {
        if (func == nullptr) {
            continue;
        }

        ir::Type *ret_type = map_builtin_type(types, func->return_type);
        auto param_types = map_param_types(types, func->params);
        auto *fn_type = types.func_ty(ret_type, param_types);
        auto *fn = module.create_function(func->name, fn_type, false);
        fn->set_external(false);

        const auto param_count = std::min(fn->args().size(), func->params.size());
        for (std::size_t i = 0; i < param_count; ++i) {
            fn->args()[i]->set_name(func->params[i].name);
        }
    }
}

void ASTToIRLowering::lower_function_bodies(ir::Module &module, CompUnit &unit) {
    for (auto &func : unit.functions) {
        if (func == nullptr) {
            continue;
        }

        auto *ir_func = module.get_function(func->name);
        if (ir_func == nullptr || ir_func->is_external()) {
            continue;
        }

        FunctionLowering lowering(&module, ir_func);
        lowering.run(*func);
    }
}

} // namespace irgen
