#include "../../include/oir/OIRScalarOpt.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

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

std::vector<std::string> scalar_initializer_tokens(const std::string &literal) {
    std::vector<std::string> tokens;
    std::string current;
    auto flush = [&]() {
        auto value = trim(current);
        current.clear();
        if (!value.empty()) {
            tokens.push_back(value);
        }
    };

    for (char ch : literal) {
        switch (ch) {
        case '{':
        case '}':
        case ',':
        case '\n':
        case '\r':
        case '\t':
        case ' ':
            flush();
            break;
        default:
            current.push_back(ch);
            break;
        }
    }
    flush();
    return tokens;
}

std::uint64_t scalar_element_count(oir::Type *type) {
    if (auto *array = dynamic_cast<oir::ArrayType *>(type)) {
        return array->element_count() * scalar_element_count(array->element_type());
    }
    return 1;
}

oir::Type *scalar_element_type(oir::Type *type) {
    while (auto *array = dynamic_cast<oir::ArrayType *>(type)) {
        type = array->element_type();
    }
    return type;
}

std::optional<std::uint64_t> linear_index_for(oir::Type *type,
                                              const std::vector<std::int64_t> &indices,
                                              std::size_t &pos) {
    auto *array = dynamic_cast<oir::ArrayType *>(type);
    if (array == nullptr) {
        return 0;
    }
    if (pos >= indices.size() || indices[pos] < 0 ||
        static_cast<std::uint64_t>(indices[pos]) >= array->element_count()) {
        return std::nullopt;
    }
    const auto index = static_cast<std::uint64_t>(indices[pos++]);
    auto nested = linear_index_for(array->element_type(), indices, pos);
    if (!nested) {
        return std::nullopt;
    }
    return index * scalar_element_count(array->element_type()) + *nested;
}

bool collect_global_indices(oir::Value *ptr, oir::GlobalVariable *&global,
                            std::vector<std::int64_t> &indices) {
    if (auto *g = dynamic_cast<oir::GlobalVariable *>(ptr)) {
        global = g;
        return true;
    }
    auto *gep = dynamic_cast<oir::GetElementPtrInst *>(ptr);
    if (gep == nullptr || !collect_global_indices(gep->base_ptr(), global, indices)) {
        return false;
    }
    for (auto *index : gep->indices()) {
        auto constant = int_constant(index);
        if (!constant) {
            return false;
        }
        indices.push_back(*constant);
    }
    return true;
}

oir::Value *constant_array_element_for_global(oir::Module &module, oir::GlobalVariable &global,
                                              const std::vector<std::int64_t> &raw_indices,
                                              oir::Type *load_type) {
    if (!global.is_const() || !global.value_type()->is_array()) {
        return nullptr;
    }
    std::vector<std::int64_t> indices = raw_indices;
    if (!indices.empty() && indices.front() == 0) {
        indices.erase(indices.begin());
    }
    std::size_t pos = 0;
    auto linear = linear_index_for(global.value_type(), indices, pos);
    if (!linear || pos != indices.size()) {
        return nullptr;
    }

    auto *element_type = scalar_element_type(global.value_type());
    if (element_type != load_type) {
        return nullptr;
    }
    if (trim(global.initializer_literal()).empty() ||
        trim(global.initializer_literal()) == "zero") {
        return make_zero_constant(module, element_type);
    }

    auto tokens = scalar_initializer_tokens(global.initializer_literal());
    if (*linear >= tokens.size()) {
        return make_zero_constant(module, element_type);
    }
    if (element_type->is_integer()) {
        std::int64_t value = 0;
        if (!parse_i32_literal(tokens[*linear], value)) {
            return nullptr;
        }
        return make_int_constant(module, element_type, value);
    }
    if (element_type->is_float()) {
        float value = 0.0F;
        if (!parse_f32_literal(tokens[*linear], value)) {
            return nullptr;
        }
        return module.create_f32(value);
    }
    return nullptr;
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
                oir::Value *constant = nullptr;
                if (global != nullptr && global->value_type() == load->type()) {
                    constant = constant_value_for_global(module, *global);
                } else {
                    oir::GlobalVariable *indexed_global = nullptr;
                    std::vector<std::int64_t> indices;
                    if (collect_global_indices(load->ptr(), indexed_global, indices) &&
                        indexed_global != nullptr) {
                        constant = constant_array_element_for_global(module, *indexed_global,
                                                                     indices, load->type());
                    }
                }
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
