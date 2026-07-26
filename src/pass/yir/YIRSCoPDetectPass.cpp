#include "pass/yir/YIRSCoPDetectPass.h"
#include "pass/yir/YIRPolyhedralCanonicalizePass.h"
#include "pass/ast/ASTToYIRPass.h"
#include "yir/YIR.h"

#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>
#include <cstdint>

namespace pass {

namespace {

class SCoPDetector {
public:
    explicit SCoPDetector(const YIRPolyhedralCanonicalInfo& canonical_info)
        : next_scop_id_(0), next_stmt_id_(0) {
        for (const auto& loop_info : canonical_info.loops) {
            if (loop_info.loop == nullptr) {
                continue;
            }
            poly_loops_.insert(loop_info.loop);
            loop_symbols_.emplace(loop_info.loop, loop_info.symbols);
        }
    }

    YIRSCoPInfo detect(const yir::Module& module) {
        YIRSCoPInfo info;
        std::vector<const yir::ForOp*> loop_stack;
        for (const auto& func : module.functions()) {
            scan_region_for_scops(func->body(), loop_stack, info);
        }
        return info;
    }

private:
    static constexpr std::size_t kMaxScopStatements = 2048;

    bool is_poly_loop(const yir::ForOp* for_op) const {
        return for_op != nullptr && poly_loops_.find(for_op) != poly_loops_.end();
    }

    // A SCoP is only useful if every control predicate and subscript can be
    // represented in the affine model. Values defined outside the candidate
    // region are treated as parameters; values defined inside must be built
    // from constants, loop IVs, and affine integer arithmetic.
    bool is_affine_value(const yir::Value* value,
                         const std::unordered_set<const yir::Region*>& regions,
                         const std::unordered_set<const yir::Value*>& active_ivs,
                         std::unordered_set<const yir::Value*>& visiting) const {
        if (value == nullptr || value->type() == nullptr ||
            value->type()->kind() != yir::Type::Kind::I32) {
            return false;
        }
        if (!visiting.insert(value).second) {
            return false;
        }
        const auto finish = [&](bool result) {
            visiting.erase(value);
            return result;
        };
        auto* def = value->defining_op();
        if (def == nullptr || regions.find(def->parent()) == regions.end()) {
            return finish(true);
        }
        if (dynamic_cast<const yir::ConstI32Op*>(def) != nullptr) {
            return finish(true);
        }
        if (auto* add = dynamic_cast<const yir::AddIOp*>(def)) {
            return finish(is_affine_value(add->lhs(), regions, active_ivs, visiting) &&
                          is_affine_value(add->rhs(), regions, active_ivs, visiting));
        }
        if (auto* sub = dynamic_cast<const yir::SubIOp*>(def)) {
            return finish(is_affine_value(sub->lhs(), regions, active_ivs, visiting) &&
                          is_affine_value(sub->rhs(), regions, active_ivs, visiting));
        }
        if (auto* mul = dynamic_cast<const yir::MulIOp*>(def)) {
            const auto* lhs = mul->lhs();
            const auto* rhs = mul->rhs();
            const auto is_const = [](const yir::Value* v) {
                return v != nullptr && dynamic_cast<const yir::ConstI32Op*>(v->defining_op()) != nullptr;
            };
            if ((is_const(lhs) && is_affine_value(rhs, regions, active_ivs, visiting)) ||
                (is_const(rhs) && is_affine_value(lhs, regions, active_ivs, visiting))) {
                return finish(true);
            }
            // Parametric strides such as `i * rowsize` are valid affine
            // subscripts in Polly's model. Keep the product limited to one
            // active induction variable and one loop-invariant operand.
            const bool lhs_iv = active_ivs.count(lhs) != 0;
            const bool rhs_iv = active_ivs.count(rhs) != 0;
            if (lhs_iv == rhs_iv) return finish(false);
            const auto *invariant = lhs_iv ? rhs : lhs;
            const auto *invariant_def = invariant->defining_op();
            return finish(invariant_def == nullptr || regions.find(invariant_def->parent()) == regions.end());
        }
        return finish(false);
    }

    bool is_affine_condition(const yir::Value* value,
                             const std::unordered_set<const yir::Region*>& regions,
                             const std::unordered_set<const yir::Value*>& active_ivs,
                             std::unordered_set<const yir::Value*>& visiting) const {
        if (value == nullptr || value->type() == nullptr ||
            value->type()->kind() != yir::Type::Kind::I1) {
            return false;
        }
        auto* def = value->defining_op();
        if (def == nullptr || regions.find(def->parent()) == regions.end() ||
            dynamic_cast<const yir::ConstBoolOp*>(def) != nullptr) {
            return true;
        }
        if (auto* cmp = dynamic_cast<const yir::ICmpOp*>(def)) {
            return is_affine_value(cmp->lhs(), regions, active_ivs, visiting) &&
                   is_affine_value(cmp->rhs(), regions, active_ivs, visiting);
        }
        if (auto* not_op = dynamic_cast<const yir::NotOp*>(def)) {
            return is_affine_condition(not_op->operands().front(), regions, active_ivs, visiting);
        }
        if (auto* to_bool = dynamic_cast<const yir::ToBoolOp*>(def)) {
            return is_affine_value(to_bool->operands().front(), regions, active_ivs, visiting);
        }
        return false;
    }

    void collect_regions(const yir::Region& region,
                         std::unordered_set<const yir::Region*>& regions) const {
        if (!regions.insert(&region).second) {
            return;
        }
        for (const auto& op : region.operations()) {
            if (auto* for_op = dynamic_cast<const yir::ForOp*>(op.get())) {
                collect_regions(for_op->body_region(), regions);
            } else if (auto* if_op = dynamic_cast<const yir::IfOp*>(op.get())) {
                collect_regions(if_op->then_region(), regions);
                if (if_op->has_else()) collect_regions(if_op->else_region(), regions);
            }
        }
    }

    bool validate_loop(const yir::ForOp& for_op,
                       const std::unordered_set<const yir::Value*>& enclosing_ivs = {}) const {
        std::unordered_set<const yir::Region*> regions;
        collect_regions(for_op.body_region(), regions);
        std::unordered_set<const yir::Value*> visiting;
        if (!is_affine_value(for_op.lower_bound(), regions, enclosing_ivs, visiting) ||
            !is_affine_value(for_op.upper_bound(), regions, enclosing_ivs, visiting)) {
            return false;
        }
        auto* step_def = for_op.step() == nullptr ? nullptr : for_op.step()->defining_op();
        auto* step_const = dynamic_cast<const yir::ConstI32Op*>(step_def);
        if (step_const == nullptr || step_const->value() <= 0) {
            return false;
        }
        auto active_ivs = enclosing_ivs;
        active_ivs.insert(for_op.induction_var());
        if (!validate_region(for_op.body_region(), regions, active_ivs, visiting)) {
            return false;
        }
        return true;
    }

    bool validate_region(const yir::Region& region,
                         const std::unordered_set<const yir::Region*>& regions,
                         const std::unordered_set<const yir::Value*>& active_ivs,
                         std::unordered_set<const yir::Value*>& visiting) const {
        for (const auto& op : region.operations()) {
            if (dynamic_cast<const yir::CallOp*>(op.get()) != nullptr ||
                dynamic_cast<const yir::LoadOp*>(op.get()) != nullptr ||
                dynamic_cast<const yir::StoreOp*>(op.get()) != nullptr ||
                dynamic_cast<const yir::ElemAddrOp*>(op.get()) != nullptr ||
                dynamic_cast<const yir::DecayOp*>(op.get()) != nullptr ||
                dynamic_cast<const yir::AllocaOp*>(op.get()) != nullptr ||
                dynamic_cast<const yir::ArrayInitOp*>(op.get()) != nullptr ||
                dynamic_cast<const yir::WhileOp*>(op.get()) != nullptr ||
                dynamic_cast<const yir::BreakOp*>(op.get()) != nullptr ||
                dynamic_cast<const yir::ContinueOp*>(op.get()) != nullptr ||
                dynamic_cast<const yir::ReturnOp*>(op.get()) != nullptr ||
                dynamic_cast<const yir::CondOp*>(op.get()) != nullptr) {
                return false;
            }
            if (auto* if_op = dynamic_cast<const yir::IfOp*>(op.get())) {
                if (!is_affine_condition(if_op->condition(), regions, active_ivs, visiting) ||
                    !validate_region(if_op->then_region(), regions, active_ivs, visiting) ||
                    (if_op->has_else() &&
                     !validate_region(if_op->else_region(), regions, active_ivs, visiting))) {
                    return false;
                }
                continue;
            }
            if (auto* nested = dynamic_cast<const yir::ForOp*>(op.get())) {
                if (!is_poly_loop(nested) || !validate_loop(*nested, active_ivs)) return false;
                continue;
            }
            if (auto* load = dynamic_cast<const yir::ArrayLoadOp*>(op.get())) {
                for (auto* index : load->indices()) {
                    if (!is_affine_value(index, regions, active_ivs, visiting)) {
                        return false;
                    }
                }
                continue;
            }
            if (auto* store = dynamic_cast<const yir::ArrayStoreOp*>(op.get())) {
                for (auto* index : store->indices()) {
                    if (!is_affine_value(index, regions, active_ivs, visiting)) {
                        return false;
                    }
                }
                continue;
            }
            // Remaining scalar operations are harmless to discovery; their
            // dependences are still preserved by the structured loop IR.
        }
        return true;
    }

    static bool is_poly_statement(const yir::Operation& op) {
        return dynamic_cast<const yir::ArrayLoadOp*>(&op) != nullptr ||
               dynamic_cast<const yir::ArrayStoreOp*>(&op) != nullptr;
    }

    void scan_region_for_scops(const yir::Region& region,
                               std::vector<const yir::ForOp*>& loop_stack,
                               YIRSCoPInfo& info) {
        const auto& ops = region.operations();
        for (std::size_t i = 0; i < ops.size();) {
            auto* for_op = dynamic_cast<const yir::ForOp*>(ops[i].get());
            if (is_poly_loop(for_op)) {
                YIRSCoP scop;
                scop.id = next_scop_id_;
                scop.region = &region;

                bool complete = true;
                std::vector<YIRSCoPStatement::PathCondition> condition_stack;
                while (i < ops.size()) {
                    for_op = dynamic_cast<const yir::ForOp*>(ops[i].get());
                    if (!is_poly_loop(for_op)) {
                        break;
                    }
                    if (!validate_loop(*for_op)) {
                        complete = false;
                        scan_region_for_scops(for_op->body_region(), loop_stack, info);
                        // Do not retry the same rejected loop forever.
                        ++i;
                        break;
                    }
                    complete = collect_poly_loop(*for_op, loop_stack, condition_stack, scop) && complete;
                    ++i;
                    if (!complete) {
                        break;
                    }
                }

                if (complete && !scop.statements.empty()) {
                    ++next_scop_id_;
                    info.scops.push_back(std::move(scop));
                }
                continue;
            }

            scan_nested_regions_outside_scop(*ops[i], loop_stack, info);
            ++i;
        }
    }

    void scan_nested_regions_outside_scop(const yir::Operation& op,
                                          std::vector<const yir::ForOp*>& loop_stack,
                                          YIRSCoPInfo& info) {
        if (auto* for_op = dynamic_cast<const yir::ForOp*>(&op)) {
            scan_region_for_scops(for_op->body_region(), loop_stack, info);
            return;
        }
        if (auto* while_op = dynamic_cast<const yir::WhileOp*>(&op)) {
            scan_region_for_scops(while_op->cond_region(), loop_stack, info);
            scan_region_for_scops(while_op->body_region(), loop_stack, info);
            return;
        }
        if (auto* if_op = dynamic_cast<const yir::IfOp*>(&op)) {
            scan_region_for_scops(if_op->then_region(), loop_stack, info);
            if (if_op->has_else()) {
                scan_region_for_scops(if_op->else_region(), loop_stack, info);
            }
        }
    }

    bool collect_poly_loop(const yir::ForOp& for_op,
                           std::vector<const yir::ForOp*>& loop_stack,
                           std::vector<YIRSCoPStatement::PathCondition>& condition_stack,
                           YIRSCoP& scop) {
        append_loop_symbols(for_op, scop);
        loop_stack.push_back(&for_op);
        bool complete = collect_statements(for_op.body_region(), loop_stack, condition_stack, scop);
        loop_stack.pop_back();
        return complete;
    }

    bool collect_statements(const yir::Region& region,
                            std::vector<const yir::ForOp*>& loop_stack,
                            std::vector<YIRSCoPStatement::PathCondition>& condition_stack,
                            YIRSCoP& scop) {
        for (const auto& op : region.operations()) {
            if (is_poly_statement(*op)) {
                if (scop.statements.size() >= kMaxScopStatements) {
                    return false;
                }
                YIRSCoPStatement stmt;
                stmt.id = next_stmt_id_++;
                stmt.op = op.get();
                stmt.op_name = op->op_name();
                stmt.enclosing_loops = loop_stack;
                stmt.path_conditions = condition_stack;
                scop.statements.push_back(std::move(stmt));
            }

            if (auto* for_op = dynamic_cast<const yir::ForOp*>(op.get())) {
                if (is_poly_loop(for_op) &&
                    !collect_poly_loop(*for_op, loop_stack, condition_stack, scop)) {
                    return false;
                }
                continue;
            }

            if (auto* if_op = dynamic_cast<const yir::IfOp*>(op.get())) {
                condition_stack.push_back({if_op->condition(), false});
                const bool then_complete = collect_statements(
                    if_op->then_region(), loop_stack, condition_stack, scop);
                condition_stack.pop_back();
                if (!then_complete) {
                    return false;
                }
                if (if_op->has_else()) {
                    condition_stack.push_back({if_op->condition(), true});
                    const bool else_complete = collect_statements(
                        if_op->else_region(), loop_stack, condition_stack, scop);
                    condition_stack.pop_back();
                    if (!else_complete) return false;
                }
            }
        }
        return true;
    }

    void append_loop_symbols(const yir::ForOp& for_op, YIRSCoP& scop) const {
        auto found = loop_symbols_.find(&for_op);
        if (found == loop_symbols_.end()) {
            return;
        }
        for (auto* symbol : found->second) {
            scop.symbols.insert(symbol);
        }
    }

    std::unordered_set<const yir::ForOp*> poly_loops_;
    std::unordered_map<const yir::ForOp*, std::vector<const yir::Value*>> loop_symbols_;
    std::size_t next_scop_id_;
    std::size_t next_stmt_id_;
};

} // namespace

std::string_view YIRSCoPDetectPass::name() const {
    return "YIRSCoPDetectPass";
}

PassKind YIRSCoPDetectPass::kind() const {
    return PassKind::Analysis;
}

PassResult YIRSCoPDetectPass::run(PassContext &context) {
    auto *module_ptr = context.get_artifact<std::unique_ptr<yir::Module>>(ASTToYIRPass::kArtifactKey);
    if (!module_ptr || !*module_ptr) {
        return PassResult::fail("YIRSCoPDetectPass requires YIR module.");
    }

    auto *canonical_info = context.get_artifact<YIRPolyhedralCanonicalInfo>(
        std::string(YIRPolyhedralCanonicalizePass::kArtifactKey));
    if (!canonical_info) {
        return PassResult::fail("YIRSCoPDetectPass requires YIRPolyhedralCanonicalInfo.");
    }

    SCoPDetector detector(*canonical_info);
    YIRSCoPInfo info = detector.detect(**module_ptr);

    std::size_t num_scops = info.scops.size();
    context.set_artifact<YIRSCoPInfo>(std::string(kArtifactKey), std::move(info));

    std::ostringstream oss;
    oss << "Detected " << num_scops << " SCoPs.";
    return PassResult::ok(false, oss.str());
}

} // namespace pass
