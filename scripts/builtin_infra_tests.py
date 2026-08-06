#!/usr/bin/env python3

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]


SOURCE = r"""
#include "builtin/BuiltinRegistry.h"

#include <exception>
#include <iostream>
#include <string>
#include <unordered_set>
#include <utility>

namespace {

class Failure final : public std::exception {
  public:
    explicit Failure(std::string message) : message_(std::move(message)) {}
    const char *what() const noexcept override { return message_.c_str(); }

  private:
    std::string message_;
};

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +            \
                          ": requirement failed: " #condition);                               \
        }                                                                                       \
    } while (false)

void test_registry_keys_are_unique_and_bidirectional() {
    const auto &registry = builtin::BuiltinRegistry::instance();
    std::unordered_set<std::string_view> names;
    std::unordered_set<unsigned> ids;
    REQUIRE(!registry.entries().empty());
    for (const auto &entry : registry.entries()) {
        REQUIRE(entry.id != builtin::BuiltinID::Invalid);
        REQUIRE(!entry.source_name.empty());
        REQUIRE(names.insert(entry.source_name).second);
        REQUIRE(ids.insert(static_cast<unsigned>(entry.id)).second);
        REQUIRE(registry.find(entry.source_name) == &entry);
        REQUIRE(registry.find(entry.id) == &entry);
        REQUIRE(builtin::builtin_id_name(entry.id) == entry.source_name);
        REQUIRE(!entry.documentation_anchor.empty());
        REQUIRE(!entry.test_tag.empty());
    }
    REQUIRE(registry.find("does_not_exist") == nullptr);
    REQUIRE(registry.find(builtin::BuiltinID::Invalid) == nullptr);
}

void test_scalar_memory_contracts() {
    const auto &registry = builtin::BuiltinRegistry::instance();
    const auto *getarray = registry.find(builtin::BuiltinID::GetArray);
    REQUIRE(getarray != nullptr);
    REQUIRE(getarray->memory_effect == builtin::MemoryEffect::Write);
    REQUIRE(getarray->written_pointer_parameters.size() == 1);
    REQUIRE(getarray->written_pointer_parameters.front() == 0);

    const auto *putarray = registry.find(builtin::BuiltinID::PutArray);
    REQUIRE(putarray != nullptr);
    REQUIRE(putarray->memory_effect == builtin::MemoryEffect::Read);
    REQUIRE(putarray->read_pointer_parameters.size() == 1);
    REQUIRE(putarray->read_pointer_parameters.front() == 1);

    const auto *putf = registry.find(builtin::BuiltinID::PutFormat);
    REQUIRE(putf != nullptr);
    REQUIRE(putf->variadic);
    REQUIRE(putf->memory_effect == builtin::MemoryEffect::Unknown);

    const auto *ranges = registry.find(builtin::BuiltinID::RuntimeRangesDisjoint);
    REQUIRE(ranges != nullptr);
    REQUIRE(ranges->source_name == "__yoolang_ranges_disjoint");
    REQUIRE(ranges->lowering == builtin::LoweringKind::RuntimeCall);
    REQUIRE(ranges->memory_effect == builtin::MemoryEffect::None);
    REQUIRE(!ranges->has_observable_side_effect);
    REQUIRE(ranges->read_pointer_parameters.empty());
    REQUIRE(ranges->written_pointer_parameters.empty());
    REQUIRE(ranges->parameters.size() == 8);
}

void test_vector_relational_schemes() {
    const auto &registry = builtin::BuiltinRegistry::instance();
    const auto *select = registry.find(builtin::BuiltinID::VectorSelect);
    REQUIRE(select != nullptr);
    REQUIRE(select->lowering == builtin::LoweringKind::YIRIntrinsic);
    REQUIRE(select->parameters.size() == 3);
    REQUIRE(select->parameters[0].kind == builtin::TypePatternKind::Mask);
    REQUIRE(select->parameters[1].kind == builtin::TypePatternKind::VectorOrMask);
    REQUIRE(select->result.kind == builtin::TypePatternKind::SameAsArgument);
    REQUIRE(select->result.argument_index == 1);

    const auto *masked_store = registry.find(builtin::BuiltinID::VectorMaskedStore);
    REQUIRE(masked_store != nullptr);
    REQUIRE(masked_store->memory_effect == builtin::MemoryEffect::Write);
    REQUIRE(masked_store->written_pointer_parameters.size() == 1);
    REQUIRE(masked_store->written_pointer_parameters.front() == 0);
}

} // namespace

int main() {
    const std::pair<const char *, void (*)()> tests[] = {
        {"registry_keys_are_unique_and_bidirectional", test_registry_keys_are_unique_and_bidirectional},
        {"scalar_memory_contracts", test_scalar_memory_contracts},
        {"vector_relational_schemes", test_vector_relational_schemes},
    };
    for (const auto &[name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
"""


def find_cxx() -> str | None:
    configured = os.environ.get("CXX")
    if configured:
        return configured
    for candidate in ("c++", "g++", "clang++"):
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    return None


def main() -> int:
    cxx = find_cxx()
    if cxx is None:
        print("error: no C++ compiler found in PATH", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="builtin-infra-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "builtin_infra_tests.cpp"
        binary = tmp_dir / "builtin_infra_tests"
        source.write_text(textwrap.dedent(SOURCE), encoding="utf-8")
        compile_proc = subprocess.run(
            [
                cxx,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "include"),
                str(source),
                str(ROOT / "src/builtin/BuiltinRegistry.cpp"),
                "-o",
                str(binary),
            ],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if compile_proc.returncode != 0:
            print(compile_proc.stdout, end="")
            print(compile_proc.stderr, end="", file=sys.stderr)
            return compile_proc.returncode
        run_proc = subprocess.run(
            [str(binary)],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        print(run_proc.stdout, end="")
        print(run_proc.stderr, end="", file=sys.stderr)
        return run_proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
