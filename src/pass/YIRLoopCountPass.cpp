#include "../../include/pass/YIRLoopCountPass.h"

#include "../../include/pass/ASTToYIRPass.h"
#include "../../include/yir/YIR.h"
#include <iostream>

namespace pass {
namespace {

class LoopCounter {
  public:
    std::size_t count_loops(const yir::Module &module) const {
        std::size_t total = 0;
        for (const auto &function : module.functions()) {
            total += scan_region(function->body());
        }
        return total;
    }

  private:
    std::size_t scan_region(const yir::Region &region) const {
        std::size_t count = 0;
        for (const auto &op : region.operations()) {
            count += scan_op(*op);
        }
        return count;
    }

    std::size_t scan_op(const yir::Operation &op) const {
        std::size_t count = 0;
        if (const auto *if_op = dynamic_cast<const yir::IfOp *>(&op)) {
            count += scan_region(if_op->then_region());
            if (if_op->has_else())
                count += scan_region(if_op->else_region());
        } else if (const auto *while_op = dynamic_cast<const yir::WhileOp *>(&op)) {
            count += 1;
            count += scan_region(while_op->cond_region());
            count += scan_region(while_op->body_region());
        } else if (const auto *for_op = dynamic_cast<const yir::ForOp *>(&op)) {
            count += 1;
            count += scan_region(for_op->body_region());
        }
        return count;
    }
};

} // namespace

std::string_view YIRLoopCountPass::name() const {
    return "YIRLoopCountPass";
}

PassKind YIRLoopCountPass::kind() const {
    return PassKind::Analysis;
}

PassResult YIRLoopCountPass::run(PassContext &context) {
    auto *artifact = context.get_artifact<std::unique_ptr<yir::Module>>(ASTToYIRPass::kArtifactKey);

    if (artifact == nullptr || *artifact == nullptr) {
        return PassResult::fail("YIRLoopCountPass requires YIR module in pass context");
    }

    LoopCounter counter;
    std::size_t loop_count = counter.count_loops(**artifact);

    out_ << "--- [YIR] Loop Statistics, total number of loops: " << loop_count << "\n";

    return PassResult::ok(false);
}

} // namespace pass
