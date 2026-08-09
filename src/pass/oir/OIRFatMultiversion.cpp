#include "pass/oir/OIRFatMultiversion.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pass::oir_fat {
namespace {

EligibilityResult fail(EligibilityCode code, std::string message) {
    EligibilityResult result;
    result.code = code;
    result.message = std::move(message);
    return result;
}

bool has_reserved_name(const std::string &name) {
    return name.rfind("__yoolang_", 0) == 0;
}

bool is_function_pointer(const oir::Type *type) {
    while (const auto *pointer = dynamic_cast<const oir::PointerType *>(type)) {
        type = pointer->element_type();
        if (type->is_function()) {
            return true;
        }
    }
    return false;
}

// A fat dispatcher does not reinterpret its arguments: it preserves every
// LP64D argument register, leaves the caller's stack argument area in place,
// and tail-calls a variant with the original FunctionType.  Consequently the
// ordinary fixed-vector/mask aggregate ABI is just as safe here as a scalar
// ABI value.  Scalable vectors are deliberately excluded (and are also
// rejected by OIR verification), while arbitrary arrays remain storage types
// rather than first-class function values.
bool standard_fat_abi_type(const oir::Type *type, bool allow_void) {
    if (type == nullptr) {
        return false;
    }
    if (type->is_void()) {
        return allow_void;
    }
    if (type->is_float() || type->is_pointer()) {
        return true;
    }
    if (const auto *vector = dynamic_cast<const oir::VectorType *>(type)) {
        return vector->element_count().is_fixed();
    }
    const auto *integer = dynamic_cast<const oir::IntegerType *>(type);
    return integer != nullptr && (integer->bit_width() == 1U || integer->bit_width() == 32U);
}

struct AssemblyGlobal final {
    std::string name;
    std::string section;
    std::string body;
};

bool assembly_globals(std::string_view assembly, std::vector<AssemblyGlobal> &globals,
                      std::string &error) {
    constexpr std::string_view text_marker = "\t.text\n";
    const auto text = assembly.find(text_marker);
    if (text == std::string_view::npos) {
        error = "FAT_ASSEMBLY_COMPOSITION_FAILED: branch has no .text section";
        return false;
    }

    std::string current_section;
    std::size_t cursor = 0;
    while (cursor < text) {
        const auto line_end = assembly.find('\n', cursor);
        const auto next_line = line_end == std::string_view::npos ? text : line_end + 1;
        const auto line = assembly.substr(cursor, next_line - cursor);
        if (line == "\t.data\n" || line == "\t.bss\n") {
            current_section = std::string(line);
            cursor = next_line;
            continue;
        }
        constexpr std::string_view global_marker = "\t.globl ";
        if (line.rfind(global_marker, 0) != 0) {
            cursor = next_line;
            continue;
        }
        if (current_section.empty()) {
            error = "FAT_ASSEMBLY_COMPOSITION_FAILED: global object has no section";
            return false;
        }

        const auto name_begin = cursor + global_marker.size();
        const auto name_end = assembly.find('\n', name_begin);
        if (name_end == std::string_view::npos || name_end >= text) {
            error = "FAT_ASSEMBLY_COMPOSITION_FAILED: malformed global directive";
            return false;
        }

        auto block_end = name_end + 1;
        while (block_end < text) {
            const auto candidate_end = assembly.find('\n', block_end);
            const auto candidate_next =
                candidate_end == std::string_view::npos ? text : candidate_end + 1;
            const auto candidate = assembly.substr(block_end, candidate_next - block_end);
            if (candidate == "\t.data\n" || candidate == "\t.bss\n" ||
                candidate.rfind(global_marker, 0) == 0) {
                break;
            }
            block_end = candidate_next;
        }
        globals.push_back({std::string(assembly.substr(name_begin, name_end - name_begin)),
                           current_section,
                           std::string(assembly.substr(cursor, block_end - cursor))});
        cursor = block_end;
    }
    return true;
}

} // namespace

std::string_view eligibility_code_name(EligibilityCode code) {
    switch (code) {
    case EligibilityCode::Eligible:
        return "FAT_ELIGIBLE";
    case EligibilityCode::InputVerificationFailed:
        return "FAT_INPUT_VERIFICATION_FAILED";
    case EligibilityCode::VariadicUnsupported:
        return "FAT_VARIADIC_UNSUPPORTED";
    case EligibilityCode::NonScalarABIUnsupported:
        return "FAT_NONSCALAR_ABI_UNSUPPORTED";
    case EligibilityCode::FunctionAddressUnsupported:
        return "FAT_FUNCTION_ADDRESS_UNSUPPORTED";
    case EligibilityCode::IndirectCallUnsupported:
        return "FAT_INDIRECT_CALL_UNSUPPORTED";
    case EligibilityCode::ReservedSymbol:
        return "FAT_RESERVED_SYMBOL";
    case EligibilityCode::DuplicateSymbol:
        return "FAT_DUPLICATE_SYMBOL";
    case EligibilityCode::NoDefinitions:
        return "FAT_NO_DEFINITIONS";
    }
    return "FAT_UNKNOWN";
}

EligibilityResult analyze_eligibility(const oir::Module &module) {
    std::string verify_error;
    if (!module.verify(&verify_error)) {
        return fail(EligibilityCode::InputVerificationFailed,
                    "FAT_INPUT_VERIFICATION_FAILED: " + verify_error);
    }

    EligibilityResult result;
    std::unordered_set<std::string> names;
    std::string function_pointer_global;
    for (const auto &global : module.globals()) {
        if (has_reserved_name(global->name())) {
            return fail(EligibilityCode::ReservedSymbol, "FAT_RESERVED_SYMBOL: @" + global->name());
        }
        if (!names.insert(global->name()).second) {
            return fail(EligibilityCode::DuplicateSymbol,
                        "FAT_DUPLICATE_SYMBOL: @" + global->name());
        }
        if (is_function_pointer(global->value_type())) {
            function_pointer_global = global->name();
        }
    }
    for (const auto &function_owner : module.functions()) {
        const auto *function = function_owner.get();
        const auto *signature = function->function_type();
        // A source definition cannot implement an open-ended parameter list,
        // but a direct declaration such as putf uses the same scalar LP64D
        // vararg ABI in both variants and therefore needs no dispatcher ABI
        // adaptation.
        if (signature->is_variadic() && !function->is_external()) {
            return fail(EligibilityCode::VariadicUnsupported,
                        "FAT_VARIADIC_UNSUPPORTED: variadic definition @" + function->name());
        }
        if (has_reserved_name(function->name())) {
            return fail(EligibilityCode::ReservedSymbol,
                        "FAT_RESERVED_SYMBOL: @" + function->name());
        }
        if (!names.insert(function->name()).second) {
            return fail(EligibilityCode::DuplicateSymbol,
                        "FAT_DUPLICATE_SYMBOL: @" + function->name());
        }
        for (const auto &use : function->uses()) {
            const auto *call = dynamic_cast<const oir::CallInst *>(use.user);
            if (call == nullptr || use.operand_index != 0 || call->callee() != function) {
                return fail(EligibilityCode::FunctionAddressUnsupported,
                            "FAT_FUNCTION_ADDRESS_UNSUPPORTED: @" + function->name());
            }
        }
        if (is_function_pointer(signature->return_type())) {
            return fail(EligibilityCode::FunctionAddressUnsupported,
                        "FAT_FUNCTION_ADDRESS_UNSUPPORTED: return type of @" + function->name());
        }
        if (!standard_fat_abi_type(signature->return_type(), true)) {
            return fail(EligibilityCode::NonScalarABIUnsupported,
                        "FAT_NONSCALAR_ABI_UNSUPPORTED: return type of @" + function->name() +
                            " is " + signature->return_type()->print());
        }
        for (std::size_t index = 0; index < signature->param_types().size(); ++index) {
            auto *type = signature->param_types()[index];
            if (is_function_pointer(type)) {
                return fail(EligibilityCode::FunctionAddressUnsupported,
                            "FAT_FUNCTION_ADDRESS_UNSUPPORTED: parameter " + std::to_string(index) +
                                " of @" + function->name());
            }
            if (!standard_fat_abi_type(type, false)) {
                return fail(EligibilityCode::NonScalarABIUnsupported,
                            "FAT_NONSCALAR_ABI_UNSUPPORTED: parameter " + std::to_string(index) +
                                " of @" + function->name() + " is " + type->print());
            }
        }
        if (function->is_external()) {
            continue;
        }
        result.defined_functions.push_back({function->name()});
    }

    if (result.defined_functions.empty()) {
        return fail(EligibilityCode::NoDefinitions,
                    "FAT_NO_DEFINITIONS: module has no function to multiversion");
    }

    for (const auto &function_owner : module.functions()) {
        if (function_owner->is_external()) {
            continue;
        }
        for (const auto &block : function_owner->blocks()) {
            for (const auto &instruction : block->instructions()) {
                const auto *call = dynamic_cast<const oir::CallInst *>(instruction.get());
                if (call == nullptr) {
                    continue;
                }
                const auto *callee = dynamic_cast<const oir::Function *>(call->callee());
                if (callee == nullptr) {
                    return fail(EligibilityCode::IndirectCallUnsupported,
                                "FAT_INDIRECT_CALL_UNSUPPORTED: call in @" +
                                    function_owner->name());
                }
                if (callee->function_type()->is_variadic()) {
                    const auto fixed_arity = callee->function_type()->param_types().size();
                    const auto args = call->args();
                    for (std::size_t index = fixed_arity; index < args.size(); ++index) {
                        if (dynamic_cast<const oir::VectorType *>(args[index]->type()) != nullptr) {
                            return fail(
                                EligibilityCode::VariadicUnsupported,
                                "FAT_VARIADIC_UNSUPPORTED: vector or mask variadic argument " +
                                    std::to_string(index) + " in call to @" + callee->name());
                        }
                    }
                }
                // Direct declarations retain their source symbol in both
                // snapshots.  Both branches use the same ordinary standard
                // aggregate ABI, so no adapter or variant symbol is needed.
            }
        }
    }

    if (!function_pointer_global.empty()) {
        return fail(EligibilityCode::FunctionAddressUnsupported,
                    "FAT_FUNCTION_ADDRESS_UNSUPPORTED: global @" + function_pointer_global);
    }

    result.eligible = true;
    result.code = EligibilityCode::Eligible;
    result.message = "FAT_ELIGIBLE: functions=" + std::to_string(result.defined_functions.size());
    return result;
}

bool detail::extract_vector_only_globals(std::string_view scalar_assembly,
                                         std::string_view vector_assembly, std::string &output,
                                         std::string &error) {
    output.clear();
    std::vector<AssemblyGlobal> scalar_globals;
    std::vector<AssemblyGlobal> vector_globals;
    if (!assembly_globals(scalar_assembly, scalar_globals, error) ||
        !assembly_globals(vector_assembly, vector_globals, error)) {
        return false;
    }

    std::unordered_map<std::string, const AssemblyGlobal *> scalar_by_name;
    for (const auto &global : scalar_globals) {
        if (!scalar_by_name.emplace(global.name, &global).second) {
            error = "FAT_ASSEMBLY_COMPOSITION_FAILED: duplicate scalar global @" + global.name;
            return false;
        }
    }

    std::unordered_set<std::string> vector_names;
    for (const auto &global : vector_globals) {
        if (!vector_names.insert(global.name).second) {
            error = "FAT_ASSEMBLY_COMPOSITION_FAILED: duplicate vector global @" + global.name;
            return false;
        }
        const auto scalar = scalar_by_name.find(global.name);
        if (scalar != scalar_by_name.end()) {
            if (scalar->second->section != global.section || scalar->second->body != global.body) {
                error = "FAT_ASSEMBLY_GLOBAL_MISMATCH: @" + global.name;
                return false;
            }
            continue;
        }
        output += global.section;
        output += global.body;
    }
    return true;
}

} // namespace pass::oir_fat
