#include "pass/yir/YIRPolyhedralPipelinePass.h"

#include "pass/yir/YIRPolyhedralCanonicalizePass.h"
#include "pass/yir/YIRPolyhedralDependenceAnalysisPass.h"
#include "pass/yir/YIRPolyhedralModelBuildPass.h"
#include "pass/yir/YIRPolyhedralTransformPass.h"
#include "pass/yir/YIRSCoPDetectPass.h"

#include <sstream>
#include <utility>

namespace pass {

std::string_view YIRPolyhedralPipelinePass::name() const {
    return "YIRPolyhedralPipelinePass";
}

PassKind YIRPolyhedralPipelinePass::kind() const {
    return PassKind::Transform;
}

PassResult YIRPolyhedralPipelinePass::run(PassContext &context) {
    PassManager pm;
    pm.add_pass<YIRPolyhedralCanonicalizePass>();
    pm.add_pass<YIRSCoPDetectPass>();
    pm.add_pass<YIRPolyhedralModelBuildPass>();
    pm.add_pass<YIRPolyhedralDependenceAnalysisPass>();
    pm.add_pass<YIRPolyhedralTransformPass>();

    auto result = pm.run(context);
    if (!result.success) {
        if (result.executions.empty()) {
            return PassResult::fail("polyhedral pipeline failed before running any pass");
        }

        const auto &execution = result.executions.back();
        std::string message = execution.name;
        if (!execution.result.message.empty()) {
            message += ": ";
            message += execution.result.message;
        }
        return PassResult::fail(std::move(message));
    }

    std::ostringstream message;
    message << "ran " << result.executions.size() << " polyhedral passes";
    return PassResult::ok(result.changed, message.str());
}

} // namespace pass
