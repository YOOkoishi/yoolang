#include "../../include/oir/OIRScalarOpt.h"

#include <cerrno>
#include <cstdlib>
#include <string>

namespace pass::oir_opt {
namespace {

std::string trim(std::string value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\n' ||
            value[begin] == '\r')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\n' ||
            value[end - 1] == '\r')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool parse_i32_literal(const std::string &literal, std::int64_t &out) {
    errno = 0;
    char *end = nullptr;
    const long value = std::strtol(literal.c_str(), &end, 0);
    if (errno != 0 || end == literal.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<std::int32_t>(value);
    return true;
}

bool parse_f32_literal(const std::string &literal, float &out) {
    errno = 0;
    char *end = nullptr;
    const float value = std::strtof(literal.c_str(), &end);
    if (errno != 0 || end == literal.c_str() || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

oir::Value *constant_value_for_global(oir::Module &module, oir::GlobalVariable &global) {
    if (!global.is_const() || !is_scalar_type(global.value_type())) {
        return nullptr;
    }

    if (auto *init = global.init_value()) {
        if (init->type() == global.value_type()) {
            return init;
        }
    }

    const std::string literal = trim(global.initializer_literal());
    if (literal.empty() || literal == "zero") {
        return make_zero_constant(module, global.value_type());
    }

    if (global.value_type()->is_integer()) {
        std::int64_t value = 0;
        if (!parse_i32_literal(literal, value)) {
            return nullptr;
        }
        return make_int_constant(module, global.value_type(), value);
    }

    if (global.value_type()->is_float()) {
        float value = 0.0F;
        if (!parse_f32_literal(literal, value)) {
            return nullptr;
        }
        return module.create_f32(value);
    }

    return nullptr;
}

} // namespace

bool propagate_global_constants(oir::Module &module, Stats &stats) {
    ReplacementMap replacements;

    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }

        for (auto &block : function->blocks()) {
            for (auto &inst : block->instructions()) {
                auto *load = dynamic_cast<oir::LoadInst *>(inst.get());
                if (load == nullptr) {
                    continue;
                }

                auto *global = dynamic_cast<oir::GlobalVariable *>(load->ptr());
                if (global == nullptr || global->value_type() != load->type()) {
                    continue;
                }

                auto *constant = constant_value_for_global(module, *global);
                if (constant != nullptr && constant->type() == load->type()) {
                    replacements[load] = constant;
                }
            }
        }
    }

    if (replacements.empty()) {
        return false;
    }

    const unsigned replaced = apply_replacements(module, replacements);
    if (replaced == 0) {
        return false;
    }
    stats.globals += static_cast<unsigned>(replacements.size());
    return true;
}

} // namespace pass::oir_opt
