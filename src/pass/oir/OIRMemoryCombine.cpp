#include "oir/OIRScalarOpt.h"

#include "oir/OIRAnalysis.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

namespace pass::oir_opt {
namespace {

struct ByteSlice {
    const oir::Value *base = nullptr;
    std::int64_t begin = 0;
    std::int64_t end = 0;
};

bool touches_or_overlaps(const ByteSlice &lhs, const ByteSlice &rhs) {
    return lhs.base == rhs.base && lhs.begin <= rhs.end && rhs.begin <= lhs.end;
}

bool contains(const ByteSlice &outer, const ByteSlice &inner) {
    return outer.base == inner.base && outer.begin <= inner.begin && outer.end >= inner.end;
}

std::optional<ByteSlice> memset_slice(const oir::MemZeroInst &memset,
                                      const oir::OIRAliasAnalysis &aa) {
    auto count = int_constant(memset.byte_count());
    auto location = aa.memory_location(memset.ptr());
    if (!count || *count <= 0 || !location.base || !location.offset || *location.offset < 0 ||
        *count > std::numeric_limits<std::int32_t>::max() - *location.offset) {
        return std::nullopt;
    }
    return ByteSlice{location.base, *location.offset, *location.offset + *count};
}

std::optional<ByteSlice> store_slice(const oir::StoreInst &store, const oir::OIRAliasAnalysis &aa) {
    auto location = aa.memory_location(store.ptr());
    if (!location.base || !location.offset || !location.size || *location.offset < 0 ||
        // The current MemZero backend's inline/fallback lowering writes whole words.
        // Keep this combine word-granular even if a future frontend exposes i1 stores.
        *location.size != 4 || *location.offset % 4 != 0 ||
        *location.size > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
        *location.offset >
            std::numeric_limits<std::int32_t>::max() - static_cast<std::int64_t>(*location.size)) {
        return std::nullopt;
    }
    return ByteSlice{location.base, *location.offset,
                     *location.offset + static_cast<std::int64_t>(*location.size)};
}

std::optional<std::uint8_t> repeated_store_byte(const oir::StoreInst &store,
                                                const ByteSlice &slice) {
    const auto bytes = static_cast<unsigned>(slice.end - slice.begin);
    if (dynamic_cast<oir::ConstantZero *>(store.value()) != nullptr) {
        return std::uint8_t{0};
    }
    auto constant = int_constant(store.value());
    if (!constant || bytes == 0 || bytes > 4) {
        return std::nullopt;
    }
    const auto bits = static_cast<std::uint32_t>(*constant);
    const auto byte = static_cast<std::uint8_t>(bits & 0xffU);
    for (unsigned i = 1; i < bytes; ++i) {
        if (static_cast<std::uint8_t>((bits >> (i * 8U)) & 0xffU) != byte) {
            return std::nullopt;
        }
    }
    return byte;
}

bool erase_instructions(oir::Function &function,
                        const std::unordered_set<oir::Instruction *> &dead) {
    if (dead.empty()) {
        return false;
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
    return true;
}

bool combine_memsets(oir::Module &module, oir::Function &function, const oir::OIRAliasAnalysis &aa,
                     Stats &stats) {
    struct Candidate {
        oir::MemZeroInst *inst = nullptr;
        ByteSlice slice;
        std::int64_t byte = 0;
    };

    std::unordered_set<oir::Instruction *> dead;
    for (auto &block : function.blocks()) {
        std::optional<Candidate> previous;
        for (auto &inst_ptr : block->instructions()) {
            auto *inst = inst_ptr.get();
            if (auto *memset = dynamic_cast<oir::MemZeroInst *>(inst)) {
                auto slice = memset_slice(*memset, aa);
                auto byte = int_constant(memset->byte_value());
                if (!slice || !byte) {
                    previous.reset();
                    continue;
                }
                Candidate current{memset, *slice, *byte};
                if (previous && dead.find(previous->inst) == dead.end() &&
                    touches_or_overlaps(previous->slice, current.slice)) {
                    if (contains(current.slice, previous->slice)) {
                        dead.insert(previous->inst);
                    } else if (previous->byte == current.byte) {
                        const auto begin = std::min(previous->slice.begin, current.slice.begin);
                        const auto end = std::max(previous->slice.end, current.slice.end);
                        auto *start_ptr = previous->slice.begin <= current.slice.begin
                                              ? previous->inst->ptr()
                                              : current.inst->ptr();
                        current.inst->set_operand(0, start_ptr);
                        current.inst->set_operand(2, module.create_i32(end - begin));
                        current.slice.begin = begin;
                        current.slice.end = end;
                        dead.insert(previous->inst);
                    }
                }
                previous = current;
                continue;
            }

            if (dynamic_cast<oir::LoadInst *>(inst) != nullptr ||
                dynamic_cast<oir::StoreInst *>(inst) != nullptr ||
                dynamic_cast<oir::CallInst *>(inst) != nullptr) {
                previous.reset();
            }
        }
    }
    if (!erase_instructions(function, dead)) {
        return false;
    }
    stats.memcpyopt += static_cast<unsigned>(dead.size());
    return true;
}

bool combine_store_runs(oir::Module &module, oir::Function &function,
                        const oir::OIRAliasAnalysis &aa, Stats &stats) {
    struct StorePart {
        oir::StoreInst *inst = nullptr;
        ByteSlice slice;
    };
    struct StoreRun {
        std::vector<StorePart> parts;
        const oir::Value *base = nullptr;
        std::int64_t begin = 0;
        std::int64_t end = 0;
        std::uint8_t byte = 0;
    };

    std::unordered_set<oir::Instruction *> dead;
    unsigned combined_runs = 0;
    for (auto &block : function.blocks()) {
        StoreRun run;
        auto flush = [&]() {
            if (run.parts.size() < 2) {
                run = {};
                return;
            }
            auto *last = run.parts.back().inst;
            auto *start_ptr = run.parts.front().inst->ptr();
            for (const auto &part : run.parts) {
                if (part.slice.begin == run.begin) {
                    start_ptr = part.inst->ptr();
                    break;
                }
            }
            auto before = std::find_if(block->instructions().begin(), block->instructions().end(),
                                       [&](const auto &inst) { return inst.get() == last; });
            if (before == block->instructions().end()) {
                run = {};
                return;
            }
            auto memset = std::make_unique<oir::MemZeroInst>(
                module.types().void_ty(), start_ptr, module.create_i32(run.byte),
                module.create_i32(run.end - run.begin), block.get());
            block->instructions().insert(before, std::move(memset));
            for (const auto &part : run.parts) {
                dead.insert(part.inst);
            }
            ++combined_runs;
            run = {};
        };

        for (auto &inst_ptr : block->instructions()) {
            auto *inst = inst_ptr.get();
            auto *store = dynamic_cast<oir::StoreInst *>(inst);
            if (store != nullptr) {
                auto slice = store_slice(*store, aa);
                auto byte = slice ? repeated_store_byte(*store, *slice) : std::nullopt;
                if (!slice || !byte) {
                    flush();
                    continue;
                }
                if (run.parts.empty()) {
                    run.parts.push_back({store, *slice});
                    run.base = slice->base;
                    run.begin = slice->begin;
                    run.end = slice->end;
                    run.byte = *byte;
                    continue;
                }
                ByteSlice covered{run.base, run.begin, run.end};
                if (run.byte != *byte || !touches_or_overlaps(covered, *slice)) {
                    flush();
                    run.parts.push_back({store, *slice});
                    run.base = slice->base;
                    run.begin = slice->begin;
                    run.end = slice->end;
                    run.byte = *byte;
                    continue;
                }
                run.parts.push_back({store, *slice});
                run.begin = std::min(run.begin, slice->begin);
                run.end = std::max(run.end, slice->end);
                continue;
            }

            if (dynamic_cast<oir::LoadInst *>(inst) != nullptr ||
                dynamic_cast<oir::CallInst *>(inst) != nullptr ||
                dynamic_cast<oir::MemZeroInst *>(inst) != nullptr) {
                flush();
            }
        }
        flush();
    }
    if (!erase_instructions(function, dead)) {
        return false;
    }
    stats.memcpyopt += static_cast<unsigned>(dead.size() + combined_runs);
    return true;
}

} // namespace

bool combine_memory_intrinsics(oir::Module &module, Stats &stats) {
    oir::OIRAliasAnalysis aa;
    bool changed = false;
    for (auto &function : module.functions()) {
        if (function->is_external()) {
            continue;
        }
        changed |= combine_memsets(module, *function, aa, stats);
        changed |= combine_store_runs(module, *function, aa, stats);
    }
    return changed;
}

} // namespace pass::oir_opt
