#include "pass/yir/YIRSCoPDetectPass.h"
#include "pass/yir/YIRPolyhedralCanonicalizePass.h"
#include "pass/ast/ASTToYIRPass.h"
#include "yir/YIR.h"

#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

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
                while (i < ops.size()) {
                    for_op = dynamic_cast<const yir::ForOp*>(ops[i].get());
                    if (!is_poly_loop(for_op)) {
                        break;
                    }
                    complete = collect_poly_loop(*for_op, loop_stack, scop) && complete;
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
                           YIRSCoP& scop) {
        append_loop_symbols(for_op, scop);
        loop_stack.push_back(&for_op);
        bool complete = collect_statements(for_op.body_region(), loop_stack, scop);
        loop_stack.pop_back();
        return complete;
    }

    bool collect_statements(const yir::Region& region,
                            std::vector<const yir::ForOp*>& loop_stack,
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
                scop.statements.push_back(std::move(stmt));
            }

            if (auto* for_op = dynamic_cast<const yir::ForOp*>(op.get())) {
                if (is_poly_loop(for_op) && !collect_poly_loop(*for_op, loop_stack, scop)) {
                    return false;
                }
                continue;
            }

            if (auto* if_op = dynamic_cast<const yir::IfOp*>(op.get())) {
                if (!collect_statements(if_op->then_region(), loop_stack, scop)) {
                    return false;
                }
                if (if_op->has_else() &&
                    !collect_statements(if_op->else_region(), loop_stack, scop)) {
                    return false;
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
