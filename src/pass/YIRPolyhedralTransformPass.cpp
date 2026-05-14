#include "../../include/pass/YIRPolyhedralTransformPass.h"
#include "../../include/pass/YIRPolyhedralModelBuildPass.h"
#include "../../include/pass/YIRPolyhedralDependenceAnalysisPass.h"
#include "../../include/pass/YIRPolyhedralCanonicalizePass.h"
#include "../../include/yir/YIR.h"

#include <sstream>

namespace pass {

namespace {

class PolyhedralTransformer {
public:
    explicit PolyhedralTransformer(const PolyModelInfo& model_info, 
                                   const PolyDependenceInfo& dep_info,
                                   const YIRPolyhedralCanonicalInfo& canonical_info)
        : model_info_(model_info), dep_info_(dep_info), canonical_info_(canonical_info),
          num_interchanged_(0), num_tiled_(0) {}

    bool transform() {
        // Because YIR modification changes structure, we should only apply safe, 
        // well-analyzed independent transformations.
        // We will perform a simple 2D Loop Interchange optimization here if we detect 
        // matrix transpose/column-major patterns that have no loop-carried dependence
        // preventing interchange.

        bool changed = false;
        
        for (const auto& scop : model_info_.models) {
            changed |= try_interchange_or_tile(scop);
        }

        return changed;
    }

    std::size_t num_interchanged() const { return num_interchanged_; }
    std::size_t num_tiled() const { return num_tiled_; }

private:
    bool try_interchange_or_tile(const PolyScop& scop) {
        // In a complete implementation, this would look for perfectly nested
        // 2D/3D loops, evaluate the dependence graph (dep_info_), and determine
        // if Loop Interchange (to improve locality of row-major matrices) or 
        // Loop Tiling (to improve cache reuse) is legal.

        // Without changing the AST structure deeply in this skeleton, we just use 
        // the SCoP boundaries and dependencies to theoretically plan the transform.
        return false;
    }

    const PolyModelInfo& model_info_;
    const PolyDependenceInfo& dep_info_;
    const YIRPolyhedralCanonicalInfo& canonical_info_;
    
    std::size_t num_interchanged_;
    std::size_t num_tiled_;
};

} // namespace

std::string_view YIRPolyhedralTransformPass::name() const {
    return "YIRPolyhedralTransformPass";
}

PassKind YIRPolyhedralTransformPass::kind() const {
    return PassKind::Transform;
}

PassResult YIRPolyhedralTransformPass::run(PassContext &context) {
    auto *model_info = context.get_artifact<PolyModelInfo>(std::string(YIRPolyhedralModelBuildPass::kArtifactKey));
    if (!model_info) {
        return PassResult::fail("YIRPolyhedralTransformPass requires PolyModelInfo.");
    }

    auto *dep_info = context.get_artifact<PolyDependenceInfo>(std::string(YIRPolyhedralDependenceAnalysisPass::kArtifactKey));
    if (!dep_info) {
        return PassResult::fail("YIRPolyhedralTransformPass requires PolyDependenceInfo.");
    }
    
    auto *canonical_info = context.get_artifact<YIRPolyhedralCanonicalInfo>(std::string(YIRPolyhedralCanonicalizePass::kArtifactKey));
    if (!canonical_info) {
        return PassResult::fail("YIRPolyhedralTransformPass requires YIRPolyhedralCanonicalInfo.");
    }

    PolyhedralTransformer transformer(*model_info, *dep_info, *canonical_info);
    bool changed = transformer.transform();

    std::ostringstream oss;
    oss << "Transformed " << transformer.num_interchanged() << " loops via interchange, " 
        << transformer.num_tiled() << " loops via tiling.";
    
    return PassResult::ok(changed, oss.str());
}

} // namespace pass
