#include "oir/OIRAnalysis.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <sstream>

namespace oir {
namespace {

template <typename T> bool contains_value(const std::vector<T> &values, T value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::optional<std::int64_t> constant_int_value(const Value *value) {
    if (auto *constant = dynamic_cast<const ConstantInt *>(value)) {
        return constant->value();
    }
    if (dynamic_cast<const ConstantZero *>(value) != nullptr && value->type()->is_integer()) {
        return 0;
    }
    return std::nullopt;
}

bool loop_contains(const Loop &loop, const BasicBlock *block) {
    return contains_value(loop.blocks, block);
}

bool is_scev_speculatable(const Instruction &inst) {
    switch (inst.op()) {
    case Instruction::OpID::Add:
    case Instruction::OpID::Sub:
    case Instruction::OpID::Mul:
    case Instruction::OpID::And:
    case Instruction::OpID::Xor:
    case Instruction::OpID::SDiv:
    case Instruction::OpID::SRem:
    case Instruction::OpID::FAdd:
    case Instruction::OpID::FSub:
    case Instruction::OpID::FMul:
    case Instruction::OpID::FDiv:
    case Instruction::OpID::ICmp:
    case Instruction::OpID::FCmp:
    case Instruction::OpID::GetElementPtr:
    case Instruction::OpID::ZExt:
    case Instruction::OpID::SIToFP:
    case Instruction::OpID::FPToSI:
        return true;
    case Instruction::OpID::Ret:
    case Instruction::OpID::Br:
    case Instruction::OpID::Alloca:
    case Instruction::OpID::Load:
    case Instruction::OpID::Store:
    case Instruction::OpID::Call:
    case Instruction::OpID::MemZero:
    case Instruction::OpID::Phi:
        return false;
    }
    return false;
}

CmpPred inverse_predicate(CmpPred pred) {
    switch (pred) {
    case CmpPred::EQ:
        return CmpPred::EQ;
    case CmpPred::NE:
        return CmpPred::NE;
    case CmpPred::LT:
        return CmpPred::GT;
    case CmpPred::LE:
        return CmpPred::GE;
    case CmpPred::GT:
        return CmpPred::LT;
    case CmpPred::GE:
        return CmpPred::LE;
    }
    return pred;
}

std::optional<std::int64_t> trip_count_for(std::int64_t start, std::int64_t bound,
                                           std::int64_t step, CmpPred pred) {
    if (step == 0) {
        return std::nullopt;
    }

    switch (pred) {
    case CmpPred::LT: {
        if (step <= 0) {
            return std::nullopt;
        }
        const std::int64_t distance = bound - start;
        if (distance <= 0) {
            return 0;
        }
        return (distance + step - 1) / step;
    }
    case CmpPred::LE: {
        if (step <= 0) {
            return std::nullopt;
        }
        const std::int64_t distance = bound - start;
        if (distance < 0) {
            return 0;
        }
        return distance / step + 1;
    }
    case CmpPred::GT: {
        if (step >= 0) {
            return std::nullopt;
        }
        const std::int64_t step_abs = -step;
        const std::int64_t distance = start - bound;
        if (distance <= 0) {
            return 0;
        }
        return (distance + step_abs - 1) / step_abs;
    }
    case CmpPred::GE: {
        if (step >= 0) {
            return std::nullopt;
        }
        const std::int64_t step_abs = -step;
        const std::int64_t distance = start - bound;
        if (distance < 0) {
            return 0;
        }
        return distance / step_abs + 1;
    }
    case CmpPred::EQ:
    case CmpPred::NE:
        return std::nullopt;
    }
    return std::nullopt;
}

const Value *gep_chain_root(const Value *value) {
    if (auto *gep = dynamic_cast<const GetElementPtrInst *>(value)) {
        return gep_chain_root(gep->base_ptr());
    }
    return value;
}

bool is_argument_or_distinct_object(const Value *value) {
    return dynamic_cast<const Argument *>(value) != nullptr ||
           dynamic_cast<const AllocaInst *>(value) != nullptr ||
           dynamic_cast<const GlobalVariable *>(value) != nullptr;
}

} // namespace

UseAnalysis::UseAnalysis(Function &function) {
    scan(function);
}

UseAnalysis::UseAnalysis(Module &module) {
    scan(module);
}

void UseAnalysis::scan(Function &function) {
    clear();
    function_ = &function;
    for (auto &block : function.blocks()) {
        for (auto &instruction : block->instructions()) {
            scan_instruction(instruction.get());
        }
    }
}

void UseAnalysis::scan(Module &module) {
    clear();
    module_ = &module;
    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        for (auto &block : function->blocks()) {
            for (auto &instruction : block->instructions()) {
                scan_instruction(instruction.get());
            }
        }
    }
}

void UseAnalysis::clear() {
    function_ = nullptr;
    module_ = nullptr;
    users_.clear();
}

std::size_t UseAnalysis::use_count(const Value *value) const {
    return users(value).size();
}

const std::vector<Instruction *> &UseAnalysis::users(const Value *value) const {
    static const std::vector<Instruction *> empty;
    auto found = users_.find(value);
    return found == users_.end() ? empty : found->second;
}

bool UseAnalysis::has_uses(const Value *value) const {
    return use_count(value) != 0;
}

void UseAnalysis::replace_all_uses_with(Value *old_value, Value *new_value) {
    if (old_value == nullptr || old_value == new_value) {
        return;
    }

    old_value->replace_all_uses_with(new_value);

    if (module_ != nullptr) {
        scan(*module_);
    } else if (function_ != nullptr) {
        scan(*function_);
    }
}

void UseAnalysis::scan_instruction(Instruction *instruction) {
    for (auto *operand : instruction->operands()) {
        if (operand == nullptr) {
            continue;
        }
        users_[operand].push_back(instruction);
    }
}

DominatorTree::DominatorTree(const Function &function) : function_(&function) {
    for (const auto &block : function.blocks()) {
        blocks_.push_back(block.get());
    }
    compute_reachable();
    compute_dominators();
    compute_immediate_dominators();
    compute_children();
    compute_dom_dfs_numbers();
}

bool DominatorTree::dominates(const BasicBlock *a, const BasicBlock *b) const {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    if (a == b) {
        return true;
    }
    // Unreachable blocks are modeled conservatively: they only dominate themselves.
    if (!is_reachable(a) || !is_reachable(b)) {
        return false;
    }

    auto pre_a = dom_pre_.find(a);
    auto pre_b = dom_pre_.find(b);
    auto post_a = dom_post_.find(a);
    auto post_b = dom_post_.find(b);
    if (pre_a == dom_pre_.end() || pre_b == dom_pre_.end() || post_a == dom_post_.end() ||
        post_b == dom_post_.end()) {
        return false;
    }
    return pre_a->second <= pre_b->second && post_b->second <= post_a->second;
}

const BasicBlock *DominatorTree::immediate_dominator(const BasicBlock *block) const {
    auto found = idom_.find(block);
    return found == idom_.end() ? nullptr : found->second;
}

const std::vector<const BasicBlock *> &DominatorTree::children(const BasicBlock *block) const {
    static const std::vector<const BasicBlock *> empty;
    auto found = children_.find(block);
    return found == children_.end() ? empty : found->second;
}

bool DominatorTree::is_reachable(const BasicBlock *block) const {
    return reachable_.find(block) != reachable_.end();
}

void DominatorTree::compute_reachable() {
    auto *entry = function_->entry_block();
    if (entry == nullptr) {
        return;
    }

    std::deque<const BasicBlock *> worklist;
    reachable_.insert(entry);
    worklist.push_back(entry);
    while (!worklist.empty()) {
        auto *block = worklist.front();
        worklist.pop_front();
        reachable_blocks_.push_back(block);
        for (auto *succ : block->successors()) {
            if (reachable_.insert(succ).second) {
                worklist.push_back(succ);
            }
        }
    }
}

void DominatorTree::compute_dominators() {
    dominators_.clear();
}

void DominatorTree::compute_immediate_dominators() {
    auto *entry = function_->entry_block();
    idom_.clear();
    for (auto *block : blocks_) {
        idom_[block] = nullptr;
    }
    if (entry == nullptr || !is_reachable(entry)) {
        return;
    }

    std::vector<const BasicBlock *> postorder;
    std::unordered_set<const BasicBlock *> seen;
    std::function<void(const BasicBlock *)> dfs = [&](const BasicBlock *block) {
        if (!seen.insert(block).second) {
            return;
        }
        for (auto *succ : block->successors()) {
            if (is_reachable(succ)) {
                dfs(succ);
            }
        }
        postorder.push_back(block);
    };
    dfs(entry);

    std::vector<const BasicBlock *> rpo(postorder.rbegin(), postorder.rend());
    std::unordered_map<const BasicBlock *, std::size_t> rpo_index;
    for (std::size_t i = 0; i < rpo.size(); ++i) {
        rpo_index[rpo[i]] = i;
    }

    std::unordered_map<const BasicBlock *, const BasicBlock *> work_idom;
    work_idom[entry] = entry;

    auto intersect = [&](const BasicBlock *lhs, const BasicBlock *rhs) {
        auto *finger_lhs = lhs;
        auto *finger_rhs = rhs;
        while (finger_lhs != finger_rhs) {
            while (rpo_index.at(finger_lhs) > rpo_index.at(finger_rhs)) {
                finger_lhs = work_idom.at(finger_lhs);
            }
            while (rpo_index.at(finger_rhs) > rpo_index.at(finger_lhs)) {
                finger_rhs = work_idom.at(finger_rhs);
            }
        }
        return finger_lhs;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (auto *block : rpo) {
            if (block == entry) {
                continue;
            }

            const BasicBlock *new_idom = nullptr;
            for (auto *pred : block->predecessors()) {
                if (!is_reachable(pred) || work_idom.find(pred) == work_idom.end()) {
                    continue;
                }
                new_idom = new_idom == nullptr ? pred : intersect(pred, new_idom);
            }
            if (new_idom != nullptr && work_idom[block] != new_idom) {
                work_idom[block] = new_idom;
                changed = true;
            }
        }
    }

    for (auto *block : blocks_) {
        if (!is_reachable(block) || block == entry) {
            idom_[block] = nullptr;
            continue;
        }
        auto found = work_idom.find(block);
        idom_[block] = found == work_idom.end() ? nullptr : found->second;
    }
}

void DominatorTree::compute_children() {
    for (auto *block : blocks_) {
        children_[block];
    }
    for (const auto &item : idom_) {
        if (item.second != nullptr) {
            children_[item.second].push_back(item.first);
        }
    }
}

void DominatorTree::compute_dom_dfs_numbers() {
    dom_pre_.clear();
    dom_post_.clear();
    auto *entry = function_->entry_block();
    if (entry == nullptr || !is_reachable(entry)) {
        return;
    }

    std::size_t next = 0;
    std::function<void(const BasicBlock *)> dfs = [&](const BasicBlock *block) {
        dom_pre_[block] = next++;
        for (auto *child : children(block)) {
            dfs(child);
        }
        dom_post_[block] = next++;
    };
    dfs(entry);
}

LoopInfo::LoopInfo(const Function &function, const DominatorTree &dom_tree)
    : function_(&function), dom_tree_(&dom_tree) {
    for (const auto &block : function.blocks()) {
        if (!dom_tree.is_reachable(block.get())) {
            continue;
        }
        for (auto *succ : block->successors()) {
            if (dom_tree.is_reachable(succ) && dom_tree.dominates(succ, block.get())) {
                add_back_edge(block.get(), succ);
            }
        }
    }
    rebuild_block_map();
}

const std::vector<Loop> &LoopInfo::loops() const {
    return loops_;
}

const Loop *LoopInfo::loop_for(const BasicBlock *block) const {
    auto found = loop_for_block_.find(block);
    return found == loop_for_block_.end() ? nullptr : &loops_[found->second];
}

bool LoopInfo::is_loop_header(const BasicBlock *block) const {
    for (const auto &loop : loops_) {
        if (loop.header == block) {
            return true;
        }
    }
    return false;
}

void LoopInfo::add_back_edge(const BasicBlock *latch, const BasicBlock *header) {
    auto loop_blocks = collect_natural_loop(latch, header);

    auto found = std::find_if(loops_.begin(), loops_.end(),
                              [header](const Loop &loop) { return loop.header == header; });
    if (found == loops_.end()) {
        Loop loop;
        loop.header = header;
        loop.latches.push_back(latch);
        loop.blocks = std::move(loop_blocks);
        loops_.push_back(std::move(loop));
        return;
    }

    if (!contains_value(found->latches, latch)) {
        found->latches.push_back(latch);
    }
    for (auto *block : loop_blocks) {
        if (!contains_value(found->blocks, block)) {
            found->blocks.push_back(block);
        }
    }
}

std::vector<const BasicBlock *> LoopInfo::collect_natural_loop(const BasicBlock *latch,
                                                               const BasicBlock *header) const {
    std::vector<const BasicBlock *> blocks;
    std::unordered_set<const BasicBlock *> seen;
    std::deque<const BasicBlock *> worklist;

    seen.insert(header);
    seen.insert(latch);
    blocks.push_back(header);
    if (latch != header) {
        blocks.push_back(latch);
        worklist.push_back(latch);
    }

    while (!worklist.empty()) {
        auto *block = worklist.front();
        worklist.pop_front();
        for (auto *pred : block->predecessors()) {
            if (!dom_tree_->is_reachable(pred)) {
                continue;
            }
            if (seen.insert(pred).second) {
                blocks.push_back(pred);
                if (pred != header) {
                    worklist.push_back(pred);
                }
            }
        }
    }
    return blocks;
}

void LoopInfo::rebuild_block_map() {
    loop_for_block_.clear();
    for (std::size_t i = 0; i < loops_.size(); ++i) {
        for (auto *block : loops_[i].blocks) {
            auto found = loop_for_block_.find(block);
            if (found == loop_for_block_.end() ||
                loops_[i].blocks.size() < loops_[found->second].blocks.size()) {
                loop_for_block_[block] = i;
            }
        }
    }
}

SCEVExpr::SCEVExpr() = default;

SCEVExpr::SCEVExpr(SCEVKind kind) : kind_(kind) {
}

SCEVExpr SCEVExpr::unknown() {
    return SCEVExpr(SCEVKind::Unknown);
}

SCEVExpr SCEVExpr::constant(std::int64_t value) {
    SCEVExpr expr(SCEVKind::Constant);
    expr.constant_ = value;
    return expr;
}

SCEVExpr SCEVExpr::symbol(const Value *value) {
    if (value == nullptr) {
        return unknown();
    }
    SCEVExpr expr(SCEVKind::Symbol);
    expr.symbol_ = value;
    return expr;
}

SCEVExpr SCEVExpr::add(SCEVExpr lhs, SCEVExpr rhs) {
    if (lhs.is_unknown() || rhs.is_unknown()) {
        return unknown();
    }
    auto lhs_const = lhs.constant_value();
    auto rhs_const = rhs.constant_value();
    if (lhs_const.has_value() && rhs_const.has_value()) {
        return constant(*lhs_const + *rhs_const);
    }
    if (lhs_const.has_value() && *lhs_const == 0) {
        return rhs;
    }
    if (rhs_const.has_value() && *rhs_const == 0) {
        return lhs;
    }
    if (lhs.kind() == SCEVKind::AddRec && lhs.lhs() != nullptr && lhs.rhs() != nullptr) {
        return add_rec(add(*lhs.lhs(), std::move(rhs)), *lhs.rhs(), lhs.loop());
    }
    if (rhs.kind() == SCEVKind::AddRec && rhs.lhs() != nullptr && rhs.rhs() != nullptr) {
        return add_rec(add(std::move(lhs), *rhs.lhs()), *rhs.rhs(), rhs.loop());
    }
    SCEVExpr expr(SCEVKind::Add);
    expr.lhs_ = std::make_shared<SCEVExpr>(std::move(lhs));
    expr.rhs_ = std::make_shared<SCEVExpr>(std::move(rhs));
    return expr;
}

SCEVExpr SCEVExpr::mul(SCEVExpr lhs, SCEVExpr rhs) {
    if (lhs.is_unknown() || rhs.is_unknown()) {
        return unknown();
    }
    auto lhs_const = lhs.constant_value();
    auto rhs_const = rhs.constant_value();
    if (lhs_const.has_value() && rhs_const.has_value()) {
        return constant(*lhs_const * *rhs_const);
    }
    if ((lhs_const.has_value() && *lhs_const == 0) ||
        (rhs_const.has_value() && *rhs_const == 0)) {
        return constant(0);
    }
    if (lhs_const.has_value() && *lhs_const == 1) {
        return rhs;
    }
    if (rhs_const.has_value() && *rhs_const == 1) {
        return lhs;
    }
    if (lhs.kind() == SCEVKind::AddRec && rhs_const.has_value() && lhs.lhs() != nullptr &&
        lhs.rhs() != nullptr) {
        return add_rec(mul(*lhs.lhs(), SCEVExpr::constant(*rhs_const)),
                       mul(*lhs.rhs(), SCEVExpr::constant(*rhs_const)), lhs.loop());
    }
    if (rhs.kind() == SCEVKind::AddRec && lhs_const.has_value() && rhs.lhs() != nullptr &&
        rhs.rhs() != nullptr) {
        return add_rec(mul(SCEVExpr::constant(*lhs_const), *rhs.lhs()),
                       mul(SCEVExpr::constant(*lhs_const), *rhs.rhs()), rhs.loop());
    }
    SCEVExpr expr(SCEVKind::Mul);
    expr.lhs_ = std::make_shared<SCEVExpr>(std::move(lhs));
    expr.rhs_ = std::make_shared<SCEVExpr>(std::move(rhs));
    return expr;
}

SCEVExpr SCEVExpr::add_rec(SCEVExpr start, SCEVExpr step, const Loop *loop) {
    if (start.is_unknown() || step.is_unknown() || loop == nullptr) {
        return unknown();
    }
    SCEVExpr expr(SCEVKind::AddRec);
    expr.lhs_ = std::make_shared<SCEVExpr>(std::move(start));
    expr.rhs_ = std::make_shared<SCEVExpr>(std::move(step));
    expr.loop_ = loop;
    return expr;
}

SCEVKind SCEVExpr::kind() const {
    return kind_;
}

bool SCEVExpr::is_unknown() const {
    return kind_ == SCEVKind::Unknown;
}

std::optional<std::int64_t> SCEVExpr::constant_value() const {
    if (kind_ != SCEVKind::Constant) {
        return std::nullopt;
    }
    return constant_;
}

const Value *SCEVExpr::symbol_value() const {
    return symbol_;
}

const Loop *SCEVExpr::loop() const {
    return loop_;
}

const SCEVExpr *SCEVExpr::lhs() const {
    return lhs_.get();
}

const SCEVExpr *SCEVExpr::rhs() const {
    return rhs_.get();
}

std::string SCEVExpr::str() const {
    switch (kind_) {
    case SCEVKind::Unknown:
        return "<unknown>";
    case SCEVKind::Constant:
        return std::to_string(constant_);
    case SCEVKind::Symbol:
        return symbol_ == nullptr || symbol_->name().empty() ? "<symbol>" : "%" + symbol_->name();
    case SCEVKind::Add:
        return "(" + (lhs_ == nullptr ? "<null>" : lhs_->str()) + " + " +
               (rhs_ == nullptr ? "<null>" : rhs_->str()) + ")";
    case SCEVKind::Mul:
        return "(" + (lhs_ == nullptr ? "<null>" : lhs_->str()) + " * " +
               (rhs_ == nullptr ? "<null>" : rhs_->str()) + ")";
    case SCEVKind::AddRec: {
        std::ostringstream oss;
        oss << "{" << (lhs_ == nullptr ? "<null>" : lhs_->str()) << ",+,"
            << (rhs_ == nullptr ? "<null>" : rhs_->str()) << "}";
        if (loop_ != nullptr && loop_->header != nullptr) {
            oss << "<%" << loop_->header->name() << ">";
        }
        return oss.str();
    }
    }
    return "<unknown>";
}

ScalarEvolution::ScalarEvolution(const Function &function, const LoopInfo &loop_info)
    : function_(&function), loop_info_(&loop_info) {
}

SCEVExpr ScalarEvolution::expression_for(const Value *value, const Loop *loop) const {
    std::unordered_set<const Value *> active;
    return expression_for_impl(value, loop, active);
}

SCEVExpr ScalarEvolution::expression_for_impl(
    const Value *value, const Loop *loop, std::unordered_set<const Value *> &active) const {
    if (value == nullptr) {
        return SCEVExpr::unknown();
    }
    if (auto constant = constant_int_value(value)) {
        return SCEVExpr::constant(*constant);
    }

    auto *inst = dynamic_cast<const Instruction *>(value);
    if (inst == nullptr) {
        return SCEVExpr::symbol(value);
    }
    if (!active.insert(value).second) {
        return SCEVExpr::unknown();
    }

    auto finish = [&](SCEVExpr expr) {
        active.erase(value);
        return expr;
    };

    if (loop != nullptr) {
        if (auto *phi = dynamic_cast<const PhiInst *>(inst)) {
            if (phi->parent() == loop->header) {
                auto add_rec = try_add_rec(*phi, *loop, active);
                if (!add_rec.is_unknown()) {
                    return finish(std::move(add_rec));
                }
            }
        }
    }

    if (auto *binary = dynamic_cast<const BinaryInst *>(inst)) {
        auto lhs = expression_for_impl(binary->lhs(), loop, active);
        auto rhs = expression_for_impl(binary->rhs(), loop, active);
        switch (binary->op()) {
        case Instruction::OpID::Add:
            return finish(SCEVExpr::add(std::move(lhs), std::move(rhs)));
        case Instruction::OpID::Sub:
            return finish(SCEVExpr::add(std::move(lhs),
                                        SCEVExpr::mul(SCEVExpr::constant(-1), std::move(rhs))));
        case Instruction::OpID::Mul:
            return finish(SCEVExpr::mul(std::move(lhs), std::move(rhs)));
        default:
            break;
        }
    }

    if (auto *cast = dynamic_cast<const CastInst *>(inst)) {
        switch (cast->op()) {
        case Instruction::OpID::ZExt:
        case Instruction::OpID::SIToFP:
        case Instruction::OpID::FPToSI:
            return finish(expression_for_impl(cast->src(), loop, active));
        default:
            break;
        }
    }

    if (loop != nullptr && loop_contains(*loop, inst->parent())) {
        return finish(SCEVExpr::unknown());
    }
    return finish(SCEVExpr::symbol(value));
}

SCEVExpr ScalarEvolution::try_add_rec(
    const PhiInst &phi, const Loop &loop, std::unordered_set<const Value *> &active) const {
    if (phi.incoming().size() != 2) {
        return SCEVExpr::unknown();
    }

    const Value *start = nullptr;
    const Value *latch_value = nullptr;
    for (const auto &incoming : phi.incoming()) {
        if (loop_contains(loop, incoming.second)) {
            if (latch_value != nullptr) {
                return SCEVExpr::unknown();
            }
            latch_value = incoming.first;
        } else {
            if (start != nullptr) {
                return SCEVExpr::unknown();
            }
            start = incoming.first;
        }
    }
    if (start == nullptr || latch_value == nullptr) {
        return SCEVExpr::unknown();
    }

    auto *step_inst = dynamic_cast<const BinaryInst *>(latch_value);
    if (step_inst == nullptr) {
        return SCEVExpr::unknown();
    }

    const Value *step_value = nullptr;
    bool negate_step = false;
    if (step_inst->op() == Instruction::OpID::Add) {
        if (step_inst->lhs() == &phi) {
            step_value = step_inst->rhs();
        } else if (step_inst->rhs() == &phi) {
            step_value = step_inst->lhs();
        }
    } else if (step_inst->op() == Instruction::OpID::Sub && step_inst->lhs() == &phi) {
        step_value = step_inst->rhs();
        negate_step = true;
    }
    if (step_value == nullptr || !is_loop_invariant(step_value, loop)) {
        return SCEVExpr::unknown();
    }

    auto start_expr = expression_for_impl(start, nullptr, active);
    auto step_expr = expression_for_impl(step_value, nullptr, active);
    if (negate_step) {
        step_expr = SCEVExpr::mul(SCEVExpr::constant(-1), std::move(step_expr));
    }
    return SCEVExpr::add_rec(std::move(start_expr), std::move(step_expr), &loop);
}

std::optional<std::int64_t> ScalarEvolution::constant_expr_value(const SCEVExpr &expr) const {
    if (auto constant = expr.constant_value()) {
        return constant;
    }
    if (expr.kind() == SCEVKind::Add && expr.lhs() != nullptr && expr.rhs() != nullptr) {
        auto lhs = constant_expr_value(*expr.lhs());
        auto rhs = constant_expr_value(*expr.rhs());
        if (lhs.has_value() && rhs.has_value()) {
            return static_cast<std::int32_t>(*lhs + *rhs);
        }
    }
    if (expr.kind() == SCEVKind::Mul && expr.lhs() != nullptr && expr.rhs() != nullptr) {
        auto lhs = constant_expr_value(*expr.lhs());
        auto rhs = constant_expr_value(*expr.rhs());
        if (lhs.has_value() && rhs.has_value()) {
            return static_cast<std::int32_t>(*lhs * *rhs);
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> ScalarEvolution::constant_trip_count(const Loop &loop) const {
    auto *branch = dynamic_cast<const BranchInst *>(loop.header->terminator());
    if (branch == nullptr || !branch->is_conditional()) {
        return std::nullopt;
    }
    auto *cmp = dynamic_cast<const CmpInst *>(branch->cond());
    if (cmp == nullptr || cmp->op() != Instruction::OpID::ICmp) {
        return std::nullopt;
    }

    auto lhs = expression_for(cmp->lhs(), &loop);
    auto rhs = expression_for(cmp->rhs(), &loop);

    struct LinearAddRec {
        std::int64_t start = 0;
        std::int64_t step = 0;
    };

    auto linear_add_rec = [&](const SCEVExpr &expr,
                              const auto &self) -> std::optional<LinearAddRec> {
        if (expr.kind() == SCEVKind::AddRec && expr.lhs() != nullptr &&
            expr.rhs() != nullptr) {
            auto start = constant_expr_value(*expr.lhs());
            auto step = constant_expr_value(*expr.rhs());
            if (start.has_value() && step.has_value()) {
                return LinearAddRec{*start, *step};
            }
            return std::nullopt;
        }

        if (expr.kind() != SCEVKind::Add || expr.lhs() == nullptr || expr.rhs() == nullptr) {
            return std::nullopt;
        }
        if (auto lhs_rec = self(*expr.lhs(), self)) {
            if (auto rhs_constant = constant_expr_value(*expr.rhs())) {
                lhs_rec->start = static_cast<std::int32_t>(lhs_rec->start + *rhs_constant);
                return lhs_rec;
            }
        }
        if (auto rhs_rec = self(*expr.rhs(), self)) {
            if (auto lhs_constant = constant_expr_value(*expr.lhs())) {
                rhs_rec->start = static_cast<std::int32_t>(rhs_rec->start + *lhs_constant);
                return rhs_rec;
            }
        }
        return std::nullopt;
    };

    auto count_from = [&](const SCEVExpr &add_rec, const SCEVExpr &bound,
                          CmpPred pred) -> std::optional<std::int64_t> {
        auto rec = linear_add_rec(add_rec, linear_add_rec);
        auto limit = constant_expr_value(bound);
        if (!rec.has_value() || !limit.has_value()) {
            return std::nullopt;
        }
        return trip_count_for(rec->start, *limit, rec->step, pred);
    };

    if (auto count = count_from(lhs, rhs, cmp->pred())) {
        return count;
    }
    return count_from(rhs, lhs, inverse_predicate(cmp->pred()));
}

bool ScalarEvolution::is_loop_invariant(const Value *value, const Loop &loop) const {
    if (value == nullptr) {
        return false;
    }
    if (constant_int_value(value).has_value()) {
        return true;
    }
    auto *inst = dynamic_cast<const Instruction *>(value);
    if (inst == nullptr) {
        return true;
    }
    if (!loop_contains(loop, inst->parent())) {
        return true;
    }
    if (!is_scev_speculatable(*inst)) {
        return false;
    }
    for (auto *operand : inst->operands()) {
        if (!is_loop_invariant(operand, loop)) {
            return false;
        }
    }
    return true;
}

AliasResult OIRAliasAnalysis::alias(const Value *a, const Value *b) const {
    if (a == b) {
        return AliasResult::MustAlias;
    }
    if (a == nullptr || b == nullptr) {
        return AliasResult::MayAlias;
    }

    auto loc_a = memory_location(a);
    auto loc_b = memory_location(b);
    auto *path_root_a = gep_chain_root(a);
    auto *path_root_b = gep_chain_root(b);
    auto *root_a = loc_a.base != nullptr ? loc_a.base : underlying_object(path_root_a);
    auto *root_b = loc_b.base != nullptr ? loc_b.base : underlying_object(path_root_b);

    const bool a_is_stack_object = dynamic_cast<const AllocaInst *>(root_a) != nullptr;
    const bool b_is_stack_object = dynamic_cast<const AllocaInst *>(root_b) != nullptr;
    const auto *known_root_a = root_a != nullptr ? root_a : path_root_a;
    const auto *known_root_b = root_b != nullptr ? root_b : path_root_b;
    if (root_a != root_b &&
        ((a_is_stack_object && is_argument_or_distinct_object(known_root_b)) ||
         (b_is_stack_object && is_argument_or_distinct_object(known_root_a)))) {
        return AliasResult::NoAlias;
    }

    if (path_root_a != nullptr && path_root_a == path_root_b) {
        // Canonically different GEP paths can still denote the same byte
        // range (for example, nested array indexing versus a flattened
        // element GEP).  Only complete byte intervals can prove their
        // relationship.  When a dynamic index makes either offset unknown,
        // fail closed: GEP segment boundaries, base types, and strides cannot
        // be recovered from a concatenated index sequence.
        if (loc_a.offset && loc_b.offset && loc_a.size && loc_b.size) {
            const auto a_begin = *loc_a.offset;
            const auto b_begin = *loc_b.offset;
            const auto a_end = a_begin + static_cast<std::int64_t>(*loc_a.size);
            const auto b_end = b_begin + static_cast<std::int64_t>(*loc_b.size);
            if (a_end <= b_begin || b_end <= a_begin) {
                return AliasResult::NoAlias;
            }
            if (a_begin == b_begin && *loc_a.size == *loc_b.size) {
                return AliasResult::MustAlias;
            }
            return AliasResult::MayAlias;
        }
        return AliasResult::MayAlias;
    }

    if (root_a == root_b && is_distinct_object(root_a)) {
        if (loc_a.offset && loc_b.offset && loc_a.size && loc_b.size) {
            const auto a_begin = *loc_a.offset;
            const auto b_begin = *loc_b.offset;
            const auto a_end = a_begin + static_cast<std::int64_t>(*loc_a.size);
            const auto b_end = b_begin + static_cast<std::int64_t>(*loc_b.size);
            if (a_end <= b_begin || b_end <= a_begin) {
                return AliasResult::NoAlias;
            }
            if (a_begin == b_begin && *loc_a.size == *loc_b.size) {
                return AliasResult::MustAlias;
            }
            return AliasResult::MayAlias;
        }
        return AliasResult::MayAlias;
    }
    if (root_a != nullptr && root_b != nullptr && root_a != root_b && is_distinct_object(root_a) &&
        is_distinct_object(root_b)) {
        return AliasResult::NoAlias;
    }

    return AliasResult::MayAlias;
}

std::uint64_t OIRAliasAnalysis::type_size(const Type *type) const {
    if (type == nullptr || type->is_void() || type->is_label() || type->is_function()) {
        return 0;
    }
    if (auto *integer = dynamic_cast<const IntegerType *>(type)) {
        return (integer->bit_width() + 7) / 8;
    }
    if (type->is_float()) {
        return 4;
    }
    if (type->is_pointer()) {
        return 8;
    }
    if (auto *array = dynamic_cast<const ArrayType *>(type)) {
        return type_size(array->element_type()) * array->element_count();
    }
    return 0;
}

std::optional<std::int64_t>
OIRAliasAnalysis::constant_gep_offset(const GetElementPtrInst &gep) const {
    auto *ptr_type = dynamic_cast<const PointerType *>(gep.base_ptr()->type());
    if (ptr_type == nullptr) {
        return std::nullopt;
    }

    std::int64_t offset = 0;
    const Type *cursor = ptr_type->element_type();
    auto indices = gep.indices();
    for (std::size_t i = 0; i < indices.size(); ++i) {
        auto index = constant_int_value(indices[i]);
        if (!index) {
            return std::nullopt;
        }

        std::uint64_t stride = 0;
        if (i == 0) {
            stride = type_size(cursor);
        } else if (auto *array = dynamic_cast<const ArrayType *>(cursor)) {
            stride = type_size(array->element_type());
            cursor = array->element_type();
        } else {
            stride = type_size(cursor);
        }
        if (stride == 0) {
            return std::nullopt;
        }
        offset += *index * static_cast<std::int64_t>(stride);
    }
    return offset;
}

MemoryLocation OIRAliasAnalysis::memory_location(const Value *value) const {
    if (value == nullptr) {
        return {};
    }
    if (auto *gep = dynamic_cast<const GetElementPtrInst *>(value)) {
        auto base = memory_location(gep->base_ptr());
        auto gep_offset = constant_gep_offset(*gep);
        if (base.offset && gep_offset) {
            base.offset = *base.offset + *gep_offset;
        } else {
            base.offset = std::nullopt;
        }
        if (auto *ptr = dynamic_cast<const PointerType *>(gep->type())) {
            base.size = type_size(ptr->element_type());
        }
        return base;
    }

    MemoryLocation loc;
    loc.base = underlying_object(value);
    loc.offset = 0;
    if (auto *ptr = dynamic_cast<const PointerType *>(value->type())) {
        loc.size = type_size(ptr->element_type());
    }
    return loc;
}

bool OIRAliasAnalysis::points_to_constant_global(const Value *value) const {
    auto *base = dynamic_cast<const GlobalVariable *>(memory_location(value).base);
    return base != nullptr && base->is_const();
}

bool OIRAliasAnalysis::call_may_clobber(const CallInst &call, const Value *ptr) const {
    (void)call;
    return !points_to_constant_global(ptr);
}

bool OIRAliasAnalysis::may_read_memory(const Instruction &inst) const {
    switch (inst.op()) {
    case Instruction::OpID::Load:
    case Instruction::OpID::Call:
        return true;
    default:
        return false;
    }
}

bool OIRAliasAnalysis::may_write_memory(const Instruction &inst) const {
    switch (inst.op()) {
    case Instruction::OpID::Store:
    case Instruction::OpID::MemZero:
    case Instruction::OpID::Call:
        return true;
    default:
        return false;
    }
}

bool OIRAliasAnalysis::has_side_effect(const Instruction &inst) const {
    switch (inst.op()) {
    case Instruction::OpID::Store:
    case Instruction::OpID::MemZero:
    case Instruction::OpID::Call:
        return true;
    case Instruction::OpID::Ret:
    case Instruction::OpID::Br:
        // Treat terminators as side-effecting from a DCE perspective: removing them changes CFG.
        return true;
    default:
        return false;
    }
}

const Value *OIRAliasAnalysis::underlying_object(const Value *value) const {
    auto *gep = dynamic_cast<const GetElementPtrInst *>(value);
    if (gep != nullptr) {
        return underlying_object(gep->base_ptr());
    }
    if (is_distinct_object(value)) {
        return value;
    }
    return nullptr;
}

bool OIRAliasAnalysis::is_distinct_object(const Value *value) const {
    return dynamic_cast<const AllocaInst *>(value) != nullptr ||
           dynamic_cast<const GlobalVariable *>(value) != nullptr;
}

namespace {

template <typename T> bool set_equal(const std::unordered_set<T> &lhs,
                                     const std::unordered_set<T> &rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (const auto &value : lhs) {
        if (rhs.find(value) == rhs.end()) {
            return false;
        }
    }
    return true;
}

void merge_summary(FunctionMemorySummary &dst, const FunctionMemorySummary &src) {
    dst.read_globals.insert(src.read_globals.begin(), src.read_globals.end());
    dst.written_globals.insert(src.written_globals.begin(), src.written_globals.end());
    dst.read_param_indices.insert(src.read_param_indices.begin(), src.read_param_indices.end());
    dst.written_param_indices.insert(src.written_param_indices.begin(),
                                     src.written_param_indices.end());
    dst.reads_unknown = dst.reads_unknown || src.reads_unknown;
    dst.writes_unknown = dst.writes_unknown || src.writes_unknown;
    dst.reads_all = dst.reads_all || src.reads_all;
    dst.writes_all = dst.writes_all || src.writes_all;
    dst.has_side_effect = dst.has_side_effect || src.has_side_effect;
}

bool is_pointer_value(const Value *value) {
    return value != nullptr && value->type() != nullptr && value->type()->is_pointer();
}

const Argument *argument_base(const Value *value) {
    if (auto *arg = dynamic_cast<const Argument *>(value)) {
        return arg;
    }
    if (auto *gep = dynamic_cast<const GetElementPtrInst *>(value)) {
        return argument_base(gep->base_ptr());
    }
    return nullptr;
}

void add_pointer_effect(FunctionMemorySummary &summary, const Value *ptr,
                        const OIRAliasAnalysis &alias_analysis, bool is_write) {
    auto loc = alias_analysis.memory_location(ptr);
    if (auto *global = dynamic_cast<const GlobalVariable *>(loc.base)) {
        if (is_write) {
            summary.written_globals.insert(global);
        } else {
            summary.read_globals.insert(global);
        }
        return;
    }

    if (auto *arg = argument_base(ptr)) {
        if (is_write) {
            summary.written_param_indices.insert(arg->index());
        } else {
            summary.read_param_indices.insert(arg->index());
        }
        return;
    }

    if (dynamic_cast<const AllocaInst *>(loc.base) != nullptr) {
        return;
    }

    if (is_write) {
        summary.writes_unknown = true;
    } else {
        summary.reads_unknown = true;
    }
}

FunctionMemorySummary project_call_summary(const CallInst &call,
                                           const FunctionMemorySummary &callee_summary,
                                           const OIRAliasAnalysis &alias_analysis) {
    FunctionMemorySummary out;
    out.read_globals = callee_summary.read_globals;
    out.written_globals = callee_summary.written_globals;
    out.reads_unknown = callee_summary.reads_unknown;
    out.writes_unknown = callee_summary.writes_unknown;
    out.reads_all = callee_summary.reads_all;
    out.writes_all = callee_summary.writes_all;
    out.has_side_effect = callee_summary.has_side_effect;

    auto args = call.args();
    for (auto index : callee_summary.read_param_indices) {
        if (index < args.size() && is_pointer_value(args[index])) {
            add_pointer_effect(out, args[index], alias_analysis, false);
        } else {
            out.reads_unknown = true;
        }
    }
    for (auto index : callee_summary.written_param_indices) {
        if (index < args.size() && is_pointer_value(args[index])) {
            add_pointer_effect(out, args[index], alias_analysis, true);
        } else {
            out.writes_unknown = true;
        }
    }
    return out;
}

} // namespace

bool FunctionMemorySummary::operator==(const FunctionMemorySummary &other) const {
    return reads_unknown == other.reads_unknown && writes_unknown == other.writes_unknown &&
           reads_all == other.reads_all && writes_all == other.writes_all &&
           has_side_effect == other.has_side_effect &&
           set_equal(read_globals, other.read_globals) &&
           set_equal(written_globals, other.written_globals) &&
           set_equal(read_param_indices, other.read_param_indices) &&
           set_equal(written_param_indices, other.written_param_indices);
}

bool FunctionMemorySummary::operator!=(const FunctionMemorySummary &other) const {
    return !(*this == other);
}

bool FunctionMemorySummary::may_read_memory() const {
    return reads_all || reads_unknown || !read_globals.empty() || !read_param_indices.empty();
}

bool FunctionMemorySummary::may_write_memory() const {
    return writes_all || writes_unknown || !written_globals.empty() ||
           !written_param_indices.empty();
}

FunctionModRefAnalysis::FunctionModRefAnalysis(const Module &module) : module_(&module) {
    std::unordered_map<const Function *, std::unordered_set<const Function *>> callers;
    std::deque<const Function *> worklist;
    std::unordered_set<const Function *> queued;

    // Internal summaries start at the lattice bottom.  Revisit only reverse callers when a
    // direct callee grows; indirect and unresolved calls remain conservative in call_summary().
    for (const auto &function : module.functions()) {
        if (function->is_external()) {
            summaries_[function.get()] = external_summary(*function);
        } else {
            summaries_[function.get()] = {};
            worklist.push_back(function.get());
            queued.insert(function.get());

            for (const auto &block : function->blocks()) {
                for (const auto &inst : block->instructions()) {
                    auto *call = dynamic_cast<const CallInst *>(inst.get());
                    if (call == nullptr) {
                        continue;
                    }
                    auto *callee = dynamic_cast<const Function *>(call->callee());
                    if (callee != nullptr) {
                        callers[callee].insert(function.get());
                    }
                }
            }
        }
    }

    while (!worklist.empty()) {
        const auto *function = worklist.front();
        worklist.pop_front();
        queued.erase(function);

        FunctionMemorySummary next = scan_function(*function);
        auto &current = summaries_[function];
        if (current == next) {
            continue;
        }
        current = std::move(next);

        auto found = callers.find(function);
        if (found == callers.end()) {
            continue;
        }
        for (const auto *caller : found->second) {
            if (queued.insert(caller).second) {
                worklist.push_back(caller);
            }
        }
    }
}

const FunctionMemorySummary &FunctionModRefAnalysis::summary(const Function *function) const {
    static const FunctionMemorySummary unknown = [] {
        FunctionMemorySummary out;
        out.reads_all = true;
        out.writes_all = true;
        out.has_side_effect = true;
        return out;
    }();

    auto found = summaries_.find(function);
    return found == summaries_.end() ? unknown : found->second;
}

FunctionMemorySummary FunctionModRefAnalysis::unknown_external_summary() const {
    FunctionMemorySummary out;
    out.reads_all = true;
    out.writes_all = true;
    out.has_side_effect = true;
    return out;
}

FunctionMemorySummary FunctionModRefAnalysis::external_summary(const Function &function) const {
    FunctionMemorySummary out;
    const auto &name = function.name();

    if (name == "getint" || name == "getch" || name == "getfloat") {
        out.has_side_effect = true;
        return out;
    }
    if (name == "putint" || name == "putch" || name == "putfloat" ||
        name == "starttime" || name == "stoptime" || name == "_sysy_starttime" ||
        name == "_sysy_stoptime") {
        out.has_side_effect = true;
        return out;
    }
    if (name == "getarray" || name == "getfarray") {
        out.written_param_indices.insert(0);
        out.has_side_effect = true;
        return out;
    }
    if (name == "putarray" || name == "putfarray") {
        out.read_param_indices.insert(1);
        out.has_side_effect = true;
        return out;
    }
    if (name == "putf") {
        out.reads_unknown = true;
        out.has_side_effect = true;
        return out;
    }

    return unknown_external_summary();
}

FunctionMemorySummary FunctionModRefAnalysis::scan_function(const Function &function) const {
    FunctionMemorySummary out;
    OIRAliasAnalysis alias_analysis;

    for (const auto &block : function.blocks()) {
        for (const auto &inst : block->instructions()) {
            if (auto *load = dynamic_cast<const LoadInst *>(inst.get())) {
                add_pointer_effect(out, load->ptr(), alias_analysis, false);
                continue;
            }

            if (auto *store = dynamic_cast<const StoreInst *>(inst.get())) {
                add_pointer_effect(out, store->ptr(), alias_analysis, true);
                continue;
            }

            if (auto *memzero = dynamic_cast<const MemZeroInst *>(inst.get())) {
                add_pointer_effect(out, memzero->ptr(), alias_analysis, true);
                continue;
            }

            if (auto *call = dynamic_cast<const CallInst *>(inst.get())) {
                auto callee_summary =
                    project_call_summary(*call, call_summary(*call), alias_analysis);
                merge_summary(out, callee_summary);
            }
        }
    }

    return out;
}

FunctionMemorySummary FunctionModRefAnalysis::call_summary(const CallInst &call) const {
    auto *callee = dynamic_cast<const Function *>(call.callee());
    if (callee == nullptr) {
        return unknown_external_summary();
    }
    return summary(callee);
}

namespace {

bool call_param_may_alias(const CallInst &call, const std::unordered_set<std::size_t> &indices,
                          const Value *ptr, const OIRAliasAnalysis &alias_analysis) {
    auto args = call.args();
    for (auto index : indices) {
        if (index >= args.size()) {
            return true;
        }
        auto *arg = args[index];
        if (!is_pointer_value(arg)) {
            // The callee summary says this formal parameter carries a memory
            // effect.  A mismatched actual cannot prove separation.
            return true;
        }

        // A parameter effect describes memory reachable through the pointer,
        // not just the single element denoted by the actual argument.  Strip
        // GEPs before querying AA so that, for example, a callee read through
        // `arg[index]` may overlap a store to a different constant element of
        // the same caller object.  Distinct stack/global roots still prove
        // separation; unknown roots conservatively remain MayAlias.
        auto *arg_root = gep_chain_root(arg);
        auto *ptr_root = gep_chain_root(ptr);
        if (alias_analysis.alias(arg_root, ptr_root) != AliasResult::NoAlias) {
            return true;
        }
    }
    return false;
}

} // namespace

bool FunctionModRefAnalysis::call_may_clobber(
    const CallInst &call, const Value *ptr, const OIRAliasAnalysis &alias_analysis) const {
    if (ptr == nullptr || alias_analysis.points_to_constant_global(ptr)) {
        return false;
    }

    auto info = call_summary(call);
    if (info.writes_all) {
        return true;
    }
    if (info.writes_unknown) {
        return true;
    }
    if (call_param_may_alias(call, info.written_param_indices, ptr, alias_analysis)) {
        return true;
    }

    auto loc = alias_analysis.memory_location(ptr);
    if (auto *global = dynamic_cast<const GlobalVariable *>(loc.base)) {
        if (info.written_globals.find(global) != info.written_globals.end()) {
            return true;
        }
        return false;
    }

    if (loc.base == nullptr) {
        return !info.written_globals.empty();
    }
    return false;
}

bool FunctionModRefAnalysis::call_may_read(
    const CallInst &call, const Value *ptr, const OIRAliasAnalysis &alias_analysis) const {
    if (ptr == nullptr) {
        return true;
    }

    auto info = call_summary(call);
    if (info.reads_all) {
        return true;
    }
    if (info.reads_unknown) {
        return true;
    }
    if (call_param_may_alias(call, info.read_param_indices, ptr, alias_analysis)) {
        return true;
    }

    auto loc = alias_analysis.memory_location(ptr);
    if (auto *global = dynamic_cast<const GlobalVariable *>(loc.base)) {
        if (info.read_globals.find(global) != info.read_globals.end()) {
            return true;
        }
        return false;
    }

    if (loc.base == nullptr) {
        return !info.read_globals.empty();
    }
    return false;
}

bool FunctionModRefAnalysis::call_has_side_effect(const CallInst &call) const {
    const auto info = call_summary(call);
    return info.has_side_effect || info.may_write_memory();
}

bool FunctionModRefAnalysis::call_may_read_memory(const CallInst &call) const {
    return call_summary(call).may_read_memory();
}

bool FunctionModRefAnalysis::call_may_write_memory(const CallInst &call) const {
    return call_summary(call).may_write_memory();
}

MemoryAccess::MemoryAccess(unsigned id, MemoryAccessKind kind, BasicBlock *block,
                           Instruction *instruction)
    : id_(id), kind_(kind), block_(block), instruction_(instruction) {
}

MemoryAccessKind MemoryAccess::kind() const {
    return kind_;
}

unsigned MemoryAccess::id() const {
    return id_;
}

Instruction *MemoryAccess::instruction() const {
    return instruction_;
}

BasicBlock *MemoryAccess::block() const {
    return block_;
}

MemoryAccess *MemoryAccess::defining_access() const {
    return defining_access_;
}

const std::vector<std::pair<BasicBlock *, MemoryAccess *>> &MemoryAccess::incoming() const {
    return incoming_;
}

bool MemoryAccess::is_live_on_entry() const {
    return kind_ == MemoryAccessKind::LiveOnEntry;
}

bool MemoryAccess::is_use() const {
    return kind_ == MemoryAccessKind::Use;
}

bool MemoryAccess::is_def() const {
    return kind_ == MemoryAccessKind::Def;
}

bool MemoryAccess::is_phi() const {
    return kind_ == MemoryAccessKind::Phi;
}

void MemoryAccess::set_defining_access(MemoryAccess *access) {
    defining_access_ = access;
}

void MemoryAccess::clear_incoming() {
    incoming_.clear();
}

void MemoryAccess::add_incoming(BasicBlock *block, MemoryAccess *access) {
    incoming_.push_back({block, access});
}

MemorySSA::MemorySSA(Function &function, const OIRAliasAnalysis &alias_analysis,
                     const FunctionModRefAnalysis &modref)
    : function_(&function), alias_analysis_(&alias_analysis), modref_(&modref) {
    build();
}

MemoryAccess *MemorySSA::live_on_entry() const {
    return live_on_entry_;
}

MemoryAccess *MemorySSA::access_for(const Instruction *instruction) const {
    auto found = access_for_inst_.find(instruction);
    return found == access_for_inst_.end() ? nullptr : found->second;
}

MemoryAccess *MemorySSA::memory_phi(const BasicBlock *block) const {
    auto found = phi_for_block_.find(block);
    return found == phi_for_block_.end() ? nullptr : found->second;
}

const std::vector<std::unique_ptr<MemoryAccess>> &MemorySSA::accesses() const {
    return accesses_;
}

void MemorySSA::build() {
    accesses_.clear();
    access_for_inst_.clear();
    phi_for_block_.clear();
    block_end_access_.clear();
    live_on_entry_ = create_access(MemoryAccessKind::LiveOnEntry, nullptr);

    auto rpo = reachable_reverse_postorder();
    std::unordered_set<BasicBlock *> reachable(rpo.begin(), rpo.end());

    for (auto *block : rpo) {
        if (block == function_->entry_block()) {
            continue;
        }
        unsigned reachable_preds = 0;
        for (auto *pred : block->predecessors()) {
            if (reachable.find(pred) != reachable.end()) {
                ++reachable_preds;
            }
        }
        if (reachable_preds > 1) {
            phi_for_block_[block] = create_access(MemoryAccessKind::Phi, block);
        }
    }

    for (auto &block : function_->blocks()) {
        if (reachable.find(block.get()) == reachable.end() && block->predecessors().size() > 1) {
            phi_for_block_[block.get()] = create_access(MemoryAccessKind::Phi, block.get());
        }
    }

    for (auto *block : rpo) {
        scan_block(block, entry_access_for(block, reachable));
    }

    for (auto &block : function_->blocks()) {
        if (reachable.find(block.get()) != reachable.end()) {
            continue;
        }
        auto *start = memory_phi(block.get());
        scan_block(block.get(), start != nullptr ? start : live_on_entry_);
    }

    populate_phi_incomings(reachable);
}

std::vector<BasicBlock *> MemorySSA::reachable_reverse_postorder() const {
    std::vector<BasicBlock *> postorder;
    std::unordered_set<BasicBlock *> seen;
    auto *entry = function_->entry_block();
    if (entry == nullptr) {
        return {};
    }

    std::function<void(BasicBlock *)> dfs = [&](BasicBlock *block) {
        if (!seen.insert(block).second) {
            return;
        }
        for (auto *succ : block->successors()) {
            dfs(succ);
        }
        postorder.push_back(block);
    };
    dfs(entry);

    return std::vector<BasicBlock *>(postorder.rbegin(), postorder.rend());
}

MemoryAccess *MemorySSA::create_access(MemoryAccessKind kind, BasicBlock *block,
                                       Instruction *instruction) {
    const auto id = static_cast<unsigned>(accesses_.size());
    auto access = std::unique_ptr<MemoryAccess>(new MemoryAccess(id, kind, block, instruction));
    auto *raw = access.get();
    accesses_.push_back(std::move(access));
    return raw;
}

MemoryAccess *
MemorySSA::entry_access_for(BasicBlock *block,
                            const std::unordered_set<BasicBlock *> &reachable) const {
    if (block == function_->entry_block()) {
        return live_on_entry_;
    }
    if (auto *phi = memory_phi(block)) {
        return phi;
    }

    MemoryAccess *single_pred_access = nullptr;
    unsigned reachable_preds = 0;
    for (auto *pred : block->predecessors()) {
        if (reachable.find(pred) == reachable.end()) {
            continue;
        }
        ++reachable_preds;
        auto found = block_end_access_.find(pred);
        single_pred_access = found == block_end_access_.end() ? live_on_entry_ : found->second;
    }

    return reachable_preds == 1 ? single_pred_access : live_on_entry_;
}

void MemorySSA::scan_block(BasicBlock *block, MemoryAccess *start_access) {
    auto *current = start_access != nullptr ? start_access : live_on_entry_;
    for (auto &inst_ptr : block->instructions()) {
        auto *inst = inst_ptr.get();
        if (auto *load = dynamic_cast<LoadInst *>(inst)) {
            auto *access = create_access(MemoryAccessKind::Use, block, load);
            access->set_defining_access(current);
            access_for_inst_[load] = access;
            continue;
        }

        if (auto *store = dynamic_cast<StoreInst *>(inst)) {
            auto *access = create_access(MemoryAccessKind::Def, block, store);
            access->set_defining_access(current);
            access_for_inst_[store] = access;
            current = access;
            continue;
        }

        if (auto *memzero = dynamic_cast<MemZeroInst *>(inst)) {
            auto *access = create_access(MemoryAccessKind::Def, block, memzero);
            access->set_defining_access(current);
            access_for_inst_[memzero] = access;
            current = access;
            continue;
        }

        if (auto *call = dynamic_cast<CallInst *>(inst)) {
            if (modref_->call_may_write_memory(*call)) {
                auto *access = create_access(MemoryAccessKind::Def, block, call);
                access->set_defining_access(current);
                access_for_inst_[call] = access;
                current = access;
            } else if (modref_->call_may_read_memory(*call)) {
                auto *access = create_access(MemoryAccessKind::Use, block, call);
                access->set_defining_access(current);
                access_for_inst_[call] = access;
            }
        }
    }
    block_end_access_[block] = current;
}

void MemorySSA::populate_phi_incomings(const std::unordered_set<BasicBlock *> &reachable) {
    for (auto &[block, phi] : phi_for_block_) {
        phi->clear_incoming();
        const bool block_reachable =
            reachable.find(const_cast<BasicBlock *>(block)) != reachable.end();
        for (auto *pred : block->predecessors()) {
            if (block_reachable && reachable.find(pred) == reachable.end()) {
                continue;
            }
            auto found = block_end_access_.find(pred);
            phi->add_incoming(pred,
                              found == block_end_access_.end() ? live_on_entry_ : found->second);
        }
        if (phi->incoming().empty()) {
            phi->add_incoming(nullptr, live_on_entry_);
        }
    }
}

MemoryAccess *MemorySSA::clobbering_access(const LoadInst &load) const {
    auto *access = access_for(&load);
    if (access == nullptr) {
        return live_on_entry_;
    }
    std::unordered_set<const MemoryAccess *> active;
    std::unordered_map<const MemoryAccess *, MemoryAccess *> memo;
    return find_pointer_clobber(access->defining_access(), load.ptr(), active, memo);
}

MemoryAccess *MemorySSA::clobbering_access(const StoreInst &store) const {
    auto *access = access_for(&store);
    if (access == nullptr) {
        return live_on_entry_;
    }
    std::unordered_set<const MemoryAccess *> active;
    std::unordered_map<const MemoryAccess *, MemoryAccess *> memo;
    return find_pointer_clobber(access->defining_access(), store.ptr(), active, memo);
}

MemoryAccess *MemorySSA::clobbering_access(const CallInst &call) const {
    auto *access = access_for(&call);
    if (access == nullptr || !modref_->call_may_read_memory(call)) {
        return live_on_entry_;
    }
    std::unordered_set<const MemoryAccess *> active;
    std::unordered_map<const MemoryAccess *, MemoryAccess *> memo;
    return find_call_read_clobber(access->defining_access(), call, active, memo);
}

MemoryAccess *
MemorySSA::find_pointer_clobber(MemoryAccess *access, const Value *ptr,
                                std::unordered_set<const MemoryAccess *> &active,
                                std::unordered_map<const MemoryAccess *, MemoryAccess *> &memo)
    const {
    if (access == nullptr) {
        return live_on_entry_;
    }
    if (access->is_live_on_entry()) {
        return access;
    }
    auto memoized = memo.find(access);
    if (memoized != memo.end()) {
        return memoized->second;
    }
    if (!active.insert(access).second) {
        return access;
    }

    auto finish = [&](MemoryAccess *result) {
        memo[access] = result;
        active.erase(access);
        return result;
    };

    if (access->is_def()) {
        if (access_clobbers_pointer(*access, ptr)) {
            return finish(access);
        }
        return finish(find_pointer_clobber(access->defining_access(), ptr, active, memo));
    }

    if (access->is_use()) {
        return finish(find_pointer_clobber(access->defining_access(), ptr, active, memo));
    }

    if (access->is_phi()) {
        MemoryAccess *common = nullptr;
        bool saw_incoming = false;
        for (const auto &incoming : access->incoming()) {
            auto *candidate = find_pointer_clobber(incoming.second, ptr, active, memo);
            if (!saw_incoming) {
                common = candidate;
                saw_incoming = true;
                continue;
            }
            if (common != candidate) {
                return finish(access);
            }
        }
        return finish(saw_incoming ? common : live_on_entry_);
    }

    return finish(live_on_entry_);
}

MemoryAccess *
MemorySSA::find_call_read_clobber(MemoryAccess *access, const CallInst &call,
                                  std::unordered_set<const MemoryAccess *> &active,
                                  std::unordered_map<const MemoryAccess *, MemoryAccess *> &memo)
    const {
    if (access == nullptr) {
        return live_on_entry_;
    }
    if (access->is_live_on_entry()) {
        return access;
    }
    auto memoized = memo.find(access);
    if (memoized != memo.end()) {
        return memoized->second;
    }
    if (!active.insert(access).second) {
        return access;
    }

    auto finish = [&](MemoryAccess *result) {
        memo[access] = result;
        active.erase(access);
        return result;
    };

    if (access->is_def()) {
        if (access_clobbers_call_read(*access, call)) {
            return finish(access);
        }
        return finish(find_call_read_clobber(access->defining_access(), call, active, memo));
    }

    if (access->is_use()) {
        return finish(find_call_read_clobber(access->defining_access(), call, active, memo));
    }

    if (access->is_phi()) {
        MemoryAccess *common = nullptr;
        bool saw_incoming = false;
        for (const auto &incoming : access->incoming()) {
            auto *candidate = find_call_read_clobber(incoming.second, call, active, memo);
            if (!saw_incoming) {
                common = candidate;
                saw_incoming = true;
                continue;
            }
            if (common != candidate) {
                return finish(access);
            }
        }
        return finish(saw_incoming ? common : live_on_entry_);
    }

    return finish(live_on_entry_);
}

bool MemorySSA::access_clobbers_pointer(const MemoryAccess &access, const Value *ptr) const {
    auto *inst = access.instruction();
    if (inst == nullptr || !access.is_def()) {
        return false;
    }

    if (auto *store = dynamic_cast<StoreInst *>(inst)) {
        return alias_analysis_->alias(store->ptr(), ptr) != AliasResult::NoAlias;
    }
    if (auto *memzero = dynamic_cast<MemZeroInst *>(inst)) {
        return alias_analysis_->alias(memzero->ptr(), ptr) != AliasResult::NoAlias;
    }
    if (auto *call = dynamic_cast<CallInst *>(inst)) {
        return modref_->call_may_clobber(*call, ptr, *alias_analysis_);
    }
    return false;
}

bool MemorySSA::access_clobbers_call_read(const MemoryAccess &access,
                                          const CallInst &call) const {
    auto *inst = access.instruction();
    if (inst == nullptr || !access.is_def()) {
        return false;
    }

    if (auto *store = dynamic_cast<StoreInst *>(inst)) {
        return modref_->call_may_read(call, store->ptr(), *alias_analysis_);
    }
    if (auto *memzero = dynamic_cast<MemZeroInst *>(inst)) {
        return modref_->call_may_read(call, memzero->ptr(), *alias_analysis_);
    }
    if (auto *writer = dynamic_cast<CallInst *>(inst)) {
        return modref_->call_may_write_memory(*writer);
    }
    return false;
}

} // namespace oir
