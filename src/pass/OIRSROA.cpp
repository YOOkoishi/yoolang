#include "../../include/oir/OIRScalarOpt.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

constexpr std::size_t kMaxSROAElements = 128;

struct Slice {
    std::string key;
    std::vector<std::int64_t> path;
    oir::Type *type = nullptr;
    oir::AllocaInst *alloca = nullptr;
};

struct MemoryAccess {
    oir::Instruction *inst = nullptr;
    std::string key;
    bool is_load = false;
};

struct Candidate {
    oir::AllocaInst *alloca = nullptr;
    std::unordered_map<std::string, Slice> slices;
    std::vector<MemoryAccess> accesses;
    std::vector<oir::StoreInst *> aggregate_zero_stores;
    std::vector<oir::GetElementPtrInst *> geps;
    std::unordered_set<oir::GetElementPtrInst *> seen_geps;
};

bool is_sroa_scalar_type(oir::Type *type) {
    if (type == nullptr) {
        return false;
    }
    if (type->is_float()) {
        return true;
    }
    auto *integer = dynamic_cast<oir::IntegerType *>(type);
    return integer != nullptr && (integer->bit_width() == 1 || integer->bit_width() == 32);
}

std::optional<std::size_t> supported_scalar_count(oir::Type *type) {
    if (is_sroa_scalar_type(type)) {
        return 1;
    }
    auto *array = dynamic_cast<oir::ArrayType *>(type);
    if (array == nullptr || array->element_count() == 0) {
        return std::nullopt;
    }
    auto element_count = supported_scalar_count(array->element_type());
    if (!element_count.has_value()) {
        return std::nullopt;
    }
    if (*element_count > kMaxSROAElements / array->element_count()) {
        return std::nullopt;
    }
    return *element_count * array->element_count();
}

bool is_supported_aggregate(oir::Type *type) {
    if (dynamic_cast<oir::ArrayType *>(type) == nullptr) {
        return false;
    }
    auto count = supported_scalar_count(type);
    return count.has_value() && *count > 0 && *count <= kMaxSROAElements;
}

std::string key_for_path(const std::vector<std::int64_t> &path) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i != 0) {
            oss << '.';
        }
        oss << path[i];
    }
    return oss.str();
}

std::string slice_name(const oir::AllocaInst &alloca, const std::vector<std::int64_t> &path) {
    std::string name = alloca.name().empty() ? "sroa" : alloca.name();
    name += ".sroa";
    for (auto index : path) {
        name += "." + std::to_string(index);
    }
    return name;
}

Slice &ensure_slice(Candidate &candidate, const std::vector<std::int64_t> &path,
                    oir::Type *type) {
    auto key = key_for_path(path);
    auto [it, inserted] = candidate.slices.emplace(key, Slice{key, path, type, nullptr});
    if (!inserted && it->second.type != type) {
        it->second.type = nullptr;
    }
    return it->second;
}

struct DecodedGEP {
    std::vector<std::int64_t> path;
    oir::Type *pointee = nullptr;
};

std::optional<DecodedGEP> decode_constant_gep(oir::GetElementPtrInst &gep,
                                              oir::Type *base_pointee,
                                              const std::vector<std::int64_t> &base_path) {
    auto *cursor = base_pointee;
    auto path = base_path;
    auto indices = gep.indices();

    for (std::size_t i = 0; i < indices.size(); ++i) {
        auto index = int_constant(indices[i]);
        if (!index.has_value()) {
            return std::nullopt;
        }

        if (i == 0) {
            if (*index != 0) {
                return std::nullopt;
            }
            continue;
        }

        auto *array = dynamic_cast<oir::ArrayType *>(cursor);
        if (array == nullptr || *index < 0 ||
            static_cast<std::uint64_t>(*index) >= array->element_count()) {
            return std::nullopt;
        }
        path.push_back(*index);
        cursor = array->element_type();
    }

    auto *result_ptr = dynamic_cast<oir::PointerType *>(gep.type());
    if (result_ptr == nullptr || result_ptr->element_type() != cursor) {
        return std::nullopt;
    }
    return DecodedGEP{std::move(path), cursor};
}

bool is_aggregate_zero_store(oir::StoreInst &store, const Candidate &candidate,
                             const std::vector<std::int64_t> &path) {
    return path.empty() && store.ptr() == candidate.alloca &&
           store.value()->type() == candidate.alloca->allocated_type() &&
           dynamic_cast<oir::ConstantZero *>(store.value()) != nullptr;
}

bool collect_pointer_uses(Candidate &candidate, oir::Value *ptr, oir::Type *pointee,
                          const std::vector<std::int64_t> &path,
                          std::unordered_set<oir::Value *> &visited) {
    if (ptr == nullptr || pointee == nullptr) {
        return false;
    }
    if (!visited.insert(ptr).second) {
        return true;
    }

    auto uses = ptr->uses();
    for (const auto &use : uses) {
        auto *inst = dynamic_cast<oir::Instruction *>(use.user);
        if (inst == nullptr) {
            return false;
        }

        if (auto *gep = dynamic_cast<oir::GetElementPtrInst *>(inst)) {
            if (gep->base_ptr() != ptr) {
                return false;
            }
            auto decoded = decode_constant_gep(*gep, pointee, path);
            if (!decoded.has_value()) {
                return false;
            }
            if (candidate.seen_geps.insert(gep).second) {
                candidate.geps.push_back(gep);
            }
            if (!collect_pointer_uses(candidate, gep, decoded->pointee, decoded->path, visited)) {
                return false;
            }
            continue;
        }

        if (auto *load = dynamic_cast<oir::LoadInst *>(inst)) {
            if (load->ptr() != ptr || path.empty() || load->type() != pointee ||
                !is_sroa_scalar_type(pointee)) {
                return false;
            }
            auto &slice = ensure_slice(candidate, path, pointee);
            if (slice.type == nullptr) {
                return false;
            }
            candidate.accesses.push_back({load, slice.key, true});
            continue;
        }

        if (auto *store = dynamic_cast<oir::StoreInst *>(inst)) {
            if (store->ptr() != ptr) {
                return false;
            }
            if (is_aggregate_zero_store(*store, candidate, path)) {
                candidate.aggregate_zero_stores.push_back(store);
                continue;
            }
            if (path.empty() || store->value()->type() != pointee ||
                !is_sroa_scalar_type(pointee)) {
                return false;
            }
            auto &slice = ensure_slice(candidate, path, pointee);
            if (slice.type == nullptr) {
                return false;
            }
            candidate.accesses.push_back({store, slice.key, false});
            continue;
        }

        return false;
    }

    return true;
}

std::vector<std::string> sorted_slice_keys(const Candidate &candidate) {
    std::vector<std::string> keys;
    keys.reserve(candidate.slices.size());
    for (const auto &[key, slice] : candidate.slices) {
        (void)slice;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

void create_scalar_allocas(oir::Module &module, oir::Function &function, Candidate &candidate) {
    auto *entry = function.entry_block();
    auto &instructions = entry->instructions();
    auto insert_pos = instructions.begin();
    while (insert_pos != instructions.end() &&
           (*insert_pos)->op() == oir::Instruction::OpID::Alloca) {
        ++insert_pos;
    }

    for (const auto &key : sorted_slice_keys(candidate)) {
        auto &slice = candidate.slices.at(key);
        auto alloca = std::make_unique<oir::AllocaInst>(
            module.types().ptr_ty(slice.type), slice.type, entry, slice_name(*candidate.alloca,
                                                                             slice.path));
        auto *raw = alloca.get();
        raw->set_parent(entry);
        insert_pos = instructions.insert(insert_pos, std::move(alloca));
        ++insert_pos;
        slice.alloca = raw;
    }
}

bool insert_scalar_zero_stores(oir::Module &module, Candidate &candidate,
                               std::unordered_set<oir::Instruction *> &dead) {
    bool changed = false;
    auto keys = sorted_slice_keys(candidate);
    for (auto *store : candidate.aggregate_zero_stores) {
        auto *block = store->parent();
        auto &instructions = block->instructions();
        auto insert_pos =
            std::find_if(instructions.begin(), instructions.end(),
                         [store](const auto &inst) { return inst.get() == store; });
        if (insert_pos == instructions.end()) {
            continue;
        }

        for (const auto &key : keys) {
            auto &slice = candidate.slices.at(key);
            auto scalar_store = std::make_unique<oir::StoreInst>(
                module.types().void_ty(), make_zero_constant(module, slice.type), slice.alloca,
                block);
            scalar_store->set_parent(block);
            instructions.insert(insert_pos, std::move(scalar_store));
        }
        dead.insert(store);
        changed = true;
    }
    return changed;
}

void rewrite_scalar_accesses(Candidate &candidate) {
    for (const auto &access : candidate.accesses) {
        auto &slice = candidate.slices.at(access.key);
        if (access.is_load) {
            access.inst->set_operand(0, slice.alloca);
        } else {
            access.inst->set_operand(1, slice.alloca);
        }
    }
}

void erase_dead_instructions(oir::Function &function,
                             const std::unordered_set<oir::Instruction *> &dead) {
    if (dead.empty()) {
        return;
    }

    for (auto *inst : dead) {
        inst->drop_all_operands();
    }
    for (auto &block : function.blocks()) {
        for (auto it = block->instructions().begin(); it != block->instructions().end();) {
            if (dead.find(it->get()) != dead.end()) {
                it = block->instructions().erase(it);
            } else {
                ++it;
            }
        }
    }
}

std::optional<Candidate> analyze_alloca(oir::AllocaInst &alloca) {
    if (!is_supported_aggregate(alloca.allocated_type())) {
        return std::nullopt;
    }

    Candidate candidate;
    candidate.alloca = &alloca;
    std::unordered_set<oir::Value *> visited;
    if (!collect_pointer_uses(candidate, &alloca, alloca.allocated_type(), {}, visited)) {
        return std::nullopt;
    }
    if (candidate.slices.empty()) {
        return std::nullopt;
    }
    return candidate;
}

bool run_on_function(oir::Module &module, oir::Function &function, Stats &stats) {
    if (function.is_external() || function.entry_block() == nullptr) {
        return false;
    }

    bool changed = false;
    std::unordered_set<oir::Instruction *> dead;
    std::vector<oir::AllocaInst *> allocas;
    for (auto &block : function.blocks()) {
        for (auto &inst : block->instructions()) {
            if (auto *alloca = dynamic_cast<oir::AllocaInst *>(inst.get())) {
                allocas.push_back(alloca);
            }
        }
    }

    for (auto *alloca : allocas) {
        if (dead.find(alloca) != dead.end()) {
            continue;
        }
        auto candidate = analyze_alloca(*alloca);
        if (!candidate.has_value()) {
            continue;
        }

        create_scalar_allocas(module, function, *candidate);
        rewrite_scalar_accesses(*candidate);
        insert_scalar_zero_stores(module, *candidate, dead);
        for (auto *gep : candidate->geps) {
            dead.insert(gep);
        }
        dead.insert(alloca);
        stats.sroa += static_cast<unsigned>(candidate->slices.size());
        changed = true;
    }

    erase_dead_instructions(function, dead);
    return changed;
}

} // namespace

bool scalar_replacement_of_aggregates(oir::Module &module, Stats &stats) {
    bool changed = false;
    for (auto &function : module.functions()) {
        changed |= run_on_function(module, *function, stats);
    }
    return changed;
}

} // namespace pass::oir_opt
