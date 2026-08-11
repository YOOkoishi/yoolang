#include "pass/oir/OIRFatMultiversionPass.h"

#include "oir/OIRParser.h"
#include "pass/mir/MIRCombinePipelinePass.h"
#include "pass/mir/MIRListSchedulerPass.h"
#include "pass/mir/MIRPeepholePipelinePass.h"
#include "pass/mir/MIRPseudoExpansionPass.h"
#include "pass/mir/MIRRegAllocPass.h"
#include "pass/mir/MIRToAsmPass.h"
#include "pass/oir/OIRFatMultiversion.h"
#include "pass/oir/OIRCFGCleanupPass.h"
#include "pass/oir/OIRLoopVectorizerPass.h"
#include "pass/oir/OIROptimizationPipelinePass.h"
#include "pass/oir/OIRPortableVectorScalarizerPass.h"
#include "pass/oir/OIRSLPVectorizerPass.h"
#include "pass/oir/OIRToMIRPass.h"
#include "pass/oir/OIRVectorCleanupPass.h"
#include "pass/yir/YIRPolyhedralTransformPass.h"
#include "target/TargetMachine.h"

#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pass {
namespace {

constexpr const char *kScalarPrefix = "__yoolang_scalar_";
constexpr const char *kVectorPrefix = "__yoolang_rvv_";
constexpr const char *kDetector = "__yoolang_rvv_available";

struct BranchCompilation final {
    bool success = false;
    std::string assembly;
    std::string error;
};

std::unique_ptr<oir::Module> clone_module(const oir::Module &module, const char *source_name,
                                          std::string &error) {
    auto parsed = oir::OIRParser::parse(module.print(), source_name);
    if (!parsed.ok() || parsed.module == nullptr) {
        const auto detail = parsed.errors.empty() ? std::string("unknown parser failure")
                                                  : parsed.errors.front().message;
        error = "FAT_SNAPSHOT_FAILED: " + detail;
        return nullptr;
    }
    return std::move(parsed.module);
}

bool rename_definitions(oir::Module &module, const std::string &prefix,
                        const std::vector<oir_fat::DefinedFunction> &defined, std::string &error) {
    for (const auto &entry : defined) {
        auto *function = module.get_function(entry.name);
        if (function == nullptr || function->is_external()) {
            error = "FAT_RENAME_FAILED: missing definition @" + entry.name;
            return false;
        }
        function->set_name(prefix + entry.name);
    }
    std::string verify_error;
    if (!module.verify(&verify_error)) {
        error = "FAT_POST_RENAME_VERIFICATION_FAILED: " + verify_error;
        return false;
    }
    return true;
}

std::string pipeline_failure(const char *branch, const PassManagerResult &result) {
    if (result.executions.empty()) {
        return std::string("FAT_") + branch + "_PIPELINE_FAILED: empty execution report";
    }
    const auto &last = result.executions.back();
    auto message = std::string("FAT_") + branch + "_PIPELINE_FAILED[" + last.name + "]";
    if (!last.result.message.empty()) {
        message += ": " + last.result.message;
    }
    return message;
}

BranchCompilation compile_branch(std::unique_ptr<oir::Module> module,
                                 const target::TargetProfile &profile, bool vector_branch,
                                 const OIRFatMultiversionOptions &options) {
    BranchCompilation out;
    PassContext context;
    context.set_ssa_module(std::move(module));
    try {
        context.set_artifact(target::kTargetMachineArtifactKey, target::TargetMachine(profile));
    } catch (const std::exception &exception) {
        out.error = std::string(vector_branch ? "FAT_VECTOR_TARGET_FAILED: "
                                              : "FAT_SCALAR_TARGET_FAILED: ") +
                    exception.what();
        return out;
    }

    PassManager pipeline;
    OIROptimizationPipelineOptions optimization_options;
    optimization_options.preserve_function_signatures = true;
    if (vector_branch) {
        if (options.optimize_mir) {
            pipeline.add_pass<OIROptimizationPipelinePass>(optimization_options);
        }
        bool produced_vector_ir = false;
        if (options.loop_vectorize) {
            oir_vectorize::LoopVectorizerOptions vectorizer_options;
            vectorizer_options.enabled = true;
            vectorizer_options.explore_interleave = options.explore_interleave;
            pipeline.add_pass<OIRLoopVectorizerPass>(vectorizer_options);
            produced_vector_ir = true;
        }
        if (options.slp_vectorize) {
            oir_vectorize::SLPVectorizerOptions vectorizer_options;
            vectorizer_options.enabled = true;
            pipeline.add_pass<OIRSLPVectorizerPass>(vectorizer_options);
            produced_vector_ir = true;
        }
        if (produced_vector_ir) {
            pipeline.add_pass<OIRCFGCleanupPass>();
            pipeline.add_pass<OIRVectorCleanupPass>();
        }
    } else {
        // Match the ordinary rv64gc pipeline ordering: optimize the typed OIR
        // while public aggregate values still have their canonical identity,
        // then lower fixed-vector computation to portable lanes.  Running the
        // aggressive whole-module optimizer over the much larger scalarized
        // boundary dialect needlessly magnifies compile time and can expose
        // transformations that were never intended to rewrite ABI staging.
        if (options.optimize_mir) {
            pipeline.add_pass<OIROptimizationPipelinePass>(optimization_options);
        }
        pipeline.add_pass<OIRPortableVectorScalarizerPass>();
    }

    pipeline.add_pass<OIRToMIRPass>();
    if (options.optimize_mir) {
        pipeline.add_pass<MIRCombinePipelinePass>();
        pipeline.add_pass<MIRPeepholePipelinePass>(false);
        pipeline.add_pass<MIRListSchedulerPass>(false);
    }
    pipeline.add_pass<MIRRegAllocPass>();
    if (options.optimize_mir) {
        pipeline.add_pass<MIRPeepholePipelinePass>(true);
        pipeline.add_pass<MIRListSchedulerPass>(true);
    }
    pipeline.add_pass<MIRPseudoExpansionPass>();
    pipeline.add_pass<MIRToAsmPass>();

    auto result = pipeline.run(context);
    if (!result.success) {
        out.error = pipeline_failure(vector_branch ? "VECTOR" : "SCALAR", result);
        return out;
    }
    auto *assembly = context.get_artifact<std::string>(MIRToAsmPass::kArtifactKey);
    if (assembly == nullptr) {
        out.error = std::string(vector_branch ? "FAT_VECTOR_PIPELINE_FAILED"
                                              : "FAT_SCALAR_PIPELINE_FAILED") +
                    ": assembly artifact missing";
        return out;
    }
    out.success = true;
    out.assembly = *assembly;
    return out;
}

std::string hide_variants(std::string assembly, const std::string &prefix) {
    const std::string marker = "\t.globl ";
    std::size_t cursor = 0;
    while ((cursor = assembly.find(marker, cursor)) != std::string::npos) {
        const auto name_begin = cursor + marker.size();
        const auto name_end = assembly.find('\n', name_begin);
        if (name_end == std::string::npos) {
            break;
        }
        const auto name = assembly.substr(name_begin, name_end - name_begin);
        if (name.find(prefix) == std::string::npos) {
            cursor = name_end + 1;
            continue;
        }
        const auto hidden = "\t.hidden " + name + "\n";
        assembly.insert(cursor, hidden);
        cursor = name_end + hidden.size() + 1;
    }
    return assembly;
}

bool text_section_body(const std::string &assembly, std::string &body, std::string &error) {
    constexpr const char *marker = "\t.text\n";
    const auto text = assembly.find(marker);
    if (text == std::string::npos) {
        error = "FAT_ASSEMBLY_COMPOSITION_FAILED: vector branch has no .text section";
        return false;
    }
    body = assembly.substr(text + std::string(marker).size());
    return true;
}

void emit_dispatcher(std::ostringstream &out, const std::string &name) {
    const auto scalar = std::string(kScalarPrefix) + name;
    const auto vector = std::string(kVectorPrefix) + name;
    const auto label = ".L__yoolang_dispatch_" + name;

    out << "\t.align 1\n"
        << "\t.globl " << name << "\n"
        << "\t.type " << name << ", @function\n"
        << name << ":\n"
        << "\tlla t0, " << kDetector << "\n"
        << "\tbeqz t0, " << label << "_scalar\n"
        << "\taddi sp, sp, -144\n"
        << "\tsd a0, 0(sp)\n"
        << "\tsd a1, 8(sp)\n"
        << "\tsd a2, 16(sp)\n"
        << "\tsd a3, 24(sp)\n"
        << "\tsd a4, 32(sp)\n"
        << "\tsd a5, 40(sp)\n"
        << "\tsd a6, 48(sp)\n"
        << "\tsd a7, 56(sp)\n"
        << "\tfsd fa0, 64(sp)\n"
        << "\tfsd fa1, 72(sp)\n"
        << "\tfsd fa2, 80(sp)\n"
        << "\tfsd fa3, 88(sp)\n"
        << "\tfsd fa4, 96(sp)\n"
        << "\tfsd fa5, 104(sp)\n"
        << "\tfsd fa6, 112(sp)\n"
        << "\tfsd fa7, 120(sp)\n"
        << "\tsd ra, 128(sp)\n"
        << "\tjalr ra, 0(t0)\n"
        << "\tsnez t0, a0\n"
        << "\tld a0, 0(sp)\n"
        << "\tld a1, 8(sp)\n"
        << "\tld a2, 16(sp)\n"
        << "\tld a3, 24(sp)\n"
        << "\tld a4, 32(sp)\n"
        << "\tld a5, 40(sp)\n"
        << "\tld a6, 48(sp)\n"
        << "\tld a7, 56(sp)\n"
        << "\tfld fa0, 64(sp)\n"
        << "\tfld fa1, 72(sp)\n"
        << "\tfld fa2, 80(sp)\n"
        << "\tfld fa3, 88(sp)\n"
        << "\tfld fa4, 96(sp)\n"
        << "\tfld fa5, 104(sp)\n"
        << "\tfld fa6, 112(sp)\n"
        << "\tfld fa7, 120(sp)\n"
        << "\tld ra, 128(sp)\n"
        << "\taddi sp, sp, 144\n"
        << "\tbnez t0, " << label << "_vector\n"
        << label << "_scalar:\n"
        << "\ttail " << scalar << "\n"
        << label << "_vector:\n"
        << "\ttail " << vector << "\n"
        << "\t.size " << name << ", .-" << name << "\n";
}

bool compose_assembly(const std::string &scalar_assembly, const std::string &vector_assembly,
                      const std::vector<oir_fat::DefinedFunction> &defined, std::string &combined,
                      std::string &error) {
    if (scalar_assembly.find("\t.attribute arch, \"rv64gc\"\n") != 0) {
        error = "FAT_ASSEMBLY_COMPOSITION_FAILED: scalar branch is not rv64gc baseline";
        return false;
    }
    std::string vector_text;
    if (!text_section_body(vector_assembly, vector_text, error)) {
        return false;
    }

    std::string vector_only_globals;
    if (!oir_fat::detail::extract_vector_only_globals(scalar_assembly, vector_assembly,
                                                      vector_only_globals, error)) {
        return false;
    }

    std::ostringstream out;
    out << hide_variants(scalar_assembly, kScalarPrefix);
    out << "\n" << vector_only_globals;
    out << "\n\t.text\n\t.option push\n\t.option arch, +v\n";
    out << hide_variants(std::move(vector_text), kVectorPrefix);
    out << "\t.option pop\n\n\t.text\n\t.weak " << kDetector << "\n";
    for (const auto &function : defined) {
        emit_dispatcher(out, function.name);
    }
    combined = out.str();
    return true;
}

} // namespace

OIRFatMultiversionPass::OIRFatMultiversionPass(OIRFatMultiversionOptions options)
    : options_(std::move(options)) {
}

std::string_view OIRFatMultiversionPass::name() const {
    return "OIRFatMultiversionPass";
}

PassKind OIRFatMultiversionPass::kind() const {
    return PassKind::Lowering;
}

PassResult OIRFatMultiversionPass::run(PassContext &context) {
    // Publication is atomic: discard any stale output up front, keep both
    // branch artifacts private, and install mir.asm only after both Final
    // verifiers and assembly composition succeed.
    context.erase_artifact(MIRToAsmPass::kArtifactKey);
    const auto *module = context.ssa_module();
    if (module == nullptr) {
        return PassResult::fail("OIRFatMultiversionPass requires OIR module in pass context");
    }
    const auto *target_machine =
        context.get_artifact<target::TargetMachine>(target::kTargetMachineArtifactKey);
    if (target_machine == nullptr) {
        return PassResult::fail(
            "OIRFatMultiversionPass requires target.machine artifact in pass context");
    }
    if (target_machine->profile().deployment != target::DeploymentMode::Multiversion) {
        return PassResult::fail("FAT_TARGET_REQUIRED: target deployment is not fat");
    }

    auto effective_options = options_;
    if (effective_options.slp_polyhedral_rvv_preparation) {
        const auto *summary = context.get_artifact<YIRPolyhedralTransformSummary>(
            std::string(YIRPolyhedralTransformSummary::kArtifactKey));
        effective_options.slp_vectorize =
            effective_options.slp_vectorize ||
            (summary != nullptr && summary->rvv_preparations != 0);
    }

    std::string error;
    auto eligibility = oir_fat::analyze_eligibility(*module);
    if (!eligibility.eligible) {
        return PassResult::fail(std::move(eligibility.message));
    }
    const auto &defined = eligibility.defined_functions;

    target::TargetProfile scalar_profile;
    target::TargetProfile vector_profile;
    if (!target::make_rvv_multiversion_profiles(target_machine->profile(), scalar_profile,
                                                vector_profile, error)) {
        return PassResult::fail(std::move(error));
    }

    auto scalar_module = clone_module(*module, "<fat-scalar-snapshot>", error);
    if (scalar_module == nullptr) {
        return PassResult::fail(std::move(error));
    }
    auto vector_module = clone_module(*module, "<fat-vector-snapshot>", error);
    if (vector_module == nullptr) {
        return PassResult::fail(std::move(error));
    }
    if (!rename_definitions(*scalar_module, kScalarPrefix, defined, error) ||
        !rename_definitions(*vector_module, kVectorPrefix, defined, error)) {
        return PassResult::fail(std::move(error));
    }

    auto scalar = compile_branch(std::move(scalar_module), scalar_profile, false,
                                 effective_options);
    if (!scalar.success) {
        return PassResult::fail(std::move(scalar.error));
    }
    auto vector = compile_branch(std::move(vector_module), vector_profile, true,
                                 effective_options);
    if (!vector.success) {
        return PassResult::fail(std::move(vector.error));
    }

    std::string assembly;
    if (!compose_assembly(scalar.assembly, vector.assembly, defined, assembly, error)) {
        return PassResult::fail(std::move(error));
    }
    context.set_artifact<std::string>(MIRToAsmPass::kArtifactKey, std::move(assembly));
    return PassResult::ok(true, "FAT_MULTIVERSIONED: functions=" + std::to_string(defined.size()));
}

} // namespace pass
