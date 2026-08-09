#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[1]


HARNESS = r"""
#include "oir/OIR.h"
#include "pass/oir/OIRFatMultiversion.h"

#include <iostream>
#include <stdexcept>
#include <string>

#define REQUIRE(condition)                                                        \
  do {                                                                            \
    if (!(condition))                                                             \
      throw std::runtime_error(std::string("requirement failed: ") + #condition); \
  } while (false)

using Code = pass::oir_fat::EligibilityCode;

void require_code(const oir::Module &module, Code code, const char *needle) {
  auto result = pass::oir_fat::analyze_eligibility(module);
  REQUIRE(!result.eligible);
  REQUIRE(result.code == code);
  REQUIRE(result.message.find(needle) != std::string::npos);
  REQUIRE(pass::oir_fat::eligibility_code_name(code) == needle);
}

int main() {
  {
    oir::Module module("eligible-recursion");
    auto *type = module.types().func_ty(module.types().int32_ty(),
                                        {module.types().int32_ty()});
    auto *function = module.create_function("recursive", type);
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *call = builder.create_call(function, module.types().int32_ty(),
                                     {function->args()[0].get()}, "again");
    builder.create_ret(call);
    auto result = pass::oir_fat::analyze_eligibility(module);
    REQUIRE(result.eligible);
    REQUIRE(result.code == Code::Eligible);
    REQUIRE(result.defined_functions.size() == 1);
    REQUIRE(result.defined_functions[0].name == "recursive");
  }
  {
    oir::Module module("variadic-definition");
    auto *type = module.types().func_ty(module.types().int32_ty(), {}, true);
    auto *function = module.create_function("variadic", type);
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_ret(builder.i32(0));
    require_code(module, Code::VariadicUnsupported, "FAT_VARIADIC_UNSUPPORTED");
  }
  {
    oir::Module module("scalar-variadic-external");
    auto *variadic_type = module.types().func_ty(
        module.types().void_ty(), {module.types().int32_ty()}, true);
    auto *external = module.create_function("putf", variadic_type, true);
    auto *caller_type = module.types().func_ty(module.types().int32_ty(), {});
    auto *caller = module.create_function("caller", caller_type);
    auto *entry = caller->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_call(external, module.types().void_ty(),
                        {builder.i32(1), builder.f32(2.0F), builder.i32(3)});
    builder.create_ret(builder.i32(0));
    auto result = pass::oir_fat::analyze_eligibility(module);
    REQUIRE(result.eligible);
    REQUIRE(result.code == Code::Eligible);
  }
  {
    oir::Module module("vector-variadic-argument");
    auto *variadic_type = module.types().func_ty(
        module.types().void_ty(), {module.types().int32_ty()}, true);
    auto *external = module.create_function("sink", variadic_type, true);
    auto *caller_type = module.types().func_ty(module.types().void_ty(), {});
    auto *caller = module.create_function("caller", caller_type);
    auto *entry = caller->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *vector = module.types().fixed_vector_ty(module.types().int32_ty(), 3);
    builder.create_call(external, module.types().void_ty(),
                        {builder.i32(1), module.create_undef(vector)});
    builder.create_ret();
    require_code(module, Code::VariadicUnsupported, "FAT_VARIADIC_UNSUPPORTED");
  }
  {
    oir::Module module("scalable-public-abi");
    auto *scalable = module.types().scalable_vector_ty(module.types().int32_ty(), 4);
    auto *type = module.types().func_ty(scalable, {scalable});
    module.create_function("scalable_identity", type, true);
    require_code(module, Code::InputVerificationFailed,
                 "FAT_INPUT_VERIFICATION_FAILED");
  }
  {
    oir::Module module("vector-abi");
    auto *vector = module.types().fixed_vector_ty(module.types().int32_ty(), 3);
    auto *type = module.types().func_ty(vector, {vector});
    auto *function = module.create_function("identity", type);
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_ret(function->args()[0].get());
    auto result = pass::oir_fat::analyze_eligibility(module);
    REQUIRE(result.eligible);
    REQUIRE(result.code == Code::Eligible);
    REQUIRE(result.defined_functions.size() == 1);
    REQUIRE(result.defined_functions[0].name == "identity");
  }
  {
    oir::Module module("function-address");
    auto *callback = module.types().func_ty(module.types().int32_ty(), {});
    auto *callback_pointer = module.types().ptr_ty(callback);
    auto *type = module.types().func_ty(module.types().void_ty(), {callback_pointer});
    auto *function = module.create_function("takes_callback", type);
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_ret();
    require_code(module, Code::FunctionAddressUnsupported,
                 "FAT_FUNCTION_ADDRESS_UNSUPPORTED");
  }
  {
    oir::Module module("indirect-call");
    auto *callee_type = module.types().func_ty(module.types().int32_ty(), {});
    auto *callee_pointer = module.types().ptr_ty(callee_type);
    auto *slot = module.create_global("callback_slot", callee_pointer, false);
    auto *caller_type = module.types().func_ty(module.types().int32_ty(), {});
    auto *caller = module.create_function("caller", caller_type);
    auto *entry = caller->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *callee = builder.create_load(slot, callee_pointer, "callee");
    auto *call = builder.create_call(callee, module.types().int32_ty(), {}, "result");
    builder.create_ret(call);
    require_code(module, Code::IndirectCallUnsupported,
                 "FAT_INDIRECT_CALL_UNSUPPORTED");
  }
  {
    oir::Module module("function-address-global");
    auto *callee_type = module.types().func_ty(module.types().int32_ty(), {});
    auto *callee_pointer = module.types().ptr_ty(callee_type);
    module.create_global("callback_slot", callee_pointer, false);
    auto *type = module.types().func_ty(module.types().void_ty(), {});
    auto *function = module.create_function("ordinary", type);
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_ret();
    require_code(module, Code::FunctionAddressUnsupported,
                 "FAT_FUNCTION_ADDRESS_UNSUPPORTED");
  }
  {
    oir::Module module("external-call");
    auto *type = module.types().func_ty(module.types().int32_ty(), {});
    auto *external = module.create_function("external", type, true);
    auto *caller = module.create_function("caller", type);
    auto *entry = caller->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *call = builder.create_call(external, module.types().int32_ty(), {}, "result");
    builder.create_ret(call);
    auto result = pass::oir_fat::analyze_eligibility(module);
    REQUIRE(result.eligible);
    REQUIRE(result.code == Code::Eligible);
    REQUIRE(result.defined_functions.size() == 1);
    REQUIRE(result.defined_functions[0].name == "caller");
  }
  {
    oir::Module module("external-vector-call");
    auto *vector = module.types().fixed_vector_ty(module.types().int32_ty(), 3);
    auto *external_type = module.types().func_ty(vector, {vector});
    auto *external = module.create_function("external_vector", external_type, true);
    auto *caller_type = module.types().func_ty(vector, {vector});
    auto *caller = module.create_function("caller", caller_type);
    auto *entry = caller->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    auto *call = builder.create_call(external, vector, {caller->args()[0].get()}, "result");
    builder.create_ret(call);
    auto result = pass::oir_fat::analyze_eligibility(module);
    REQUIRE(result.eligible);
    REQUIRE(result.code == Code::Eligible);
    REQUIRE(result.defined_functions.size() == 1);
    REQUIRE(result.defined_functions[0].name == "caller");
  }
  {
    oir::Module module("reserved");
    auto *type = module.types().func_ty(module.types().void_ty(), {});
    auto *function = module.create_function("__yoolang_collision", type);
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_ret();
    require_code(module, Code::ReservedSymbol, "FAT_RESERVED_SYMBOL");
  }
  {
    oir::Module module("reserved-global");
    module.create_global("__yoolang_scalar_collision", module.types().int32_ty(), false);
    auto *type = module.types().func_ty(module.types().void_ty(), {});
    auto *function = module.create_function("ordinary", type);
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_ret();
    require_code(module, Code::ReservedSymbol, "FAT_RESERVED_SYMBOL");
  }
  {
    oir::Module module("reserved-external");
    auto *type = module.types().func_ty(module.types().void_ty(), {});
    module.create_function("__yoolang_scalar_collision", type, true);
    auto *function = module.create_function("ordinary", type);
    auto *entry = function->create_block("entry");
    oir::IRBuilder builder(&module);
    builder.set_insert_point(entry);
    builder.create_ret();
    require_code(module, Code::ReservedSymbol, "FAT_RESERVED_SYMBOL");
  }
  {
    oir::Module module("malformed");
    auto *type = module.types().func_ty(module.types().void_ty(), {});
    module.create_function("missing_body", type);
    require_code(module, Code::InputVerificationFailed,
                 "FAT_INPUT_VERIFICATION_FAILED");
  }
  {
    const std::string scalar =
        "\t.attribute arch, \"rv64gc\"\n"
        "\t.data\n"
        "\t.globl shared\n"
        "\t.align 2\n"
        "\t.type shared, @object\n"
        "\t.size shared, 4\n"
        "shared:\n"
        "\t.byte 7\n"
        "\t.zero 3\n"
        "\t.text\n";
    const std::string vector =
        "\t.attribute arch, \"rv64gcv\"\n"
        "\t.data\n"
        "\t.globl shared\n"
        "\t.align 2\n"
        "\t.type shared, @object\n"
        "\t.size shared, 4\n"
        "shared:\n"
        "\t.byte 7\n"
        "\t.zero 3\n"
        "\t.data\n"
        "\t.globl __yoo_vec_const_0\n"
        "\t.align 2\n"
        "\t.type __yoo_vec_const_0, @object\n"
        "\t.size __yoo_vec_const_0, 12\n"
        "__yoo_vec_const_0:\n"
        "\t.byte 1\n"
        "\t.zero 11\n"
        "\t.text\n";
    std::string output;
    std::string error;
    REQUIRE(pass::oir_fat::detail::extract_vector_only_globals(
        scalar, vector, output, error));
    REQUIRE(output.find("__yoo_vec_const_0") != std::string::npos);
    REQUIRE(output.find("\t.globl shared\n") == std::string::npos);
    REQUIRE(output.rfind("\t.data\n", 0) == 0);

    auto mismatched = vector;
    const auto shared_byte = mismatched.find("\t.byte 7\n");
    REQUIRE(shared_byte != std::string::npos);
    mismatched.replace(shared_byte, std::string("\t.byte 7\n").size(), "\t.byte 8\n");
    output.clear();
    error.clear();
    REQUIRE(!pass::oir_fat::detail::extract_vector_only_globals(
        scalar, mismatched, output, error));
    REQUIRE(error == "FAT_ASSEMBLY_GLOBAL_MISMATCH: @shared");
  }
  std::cout << "PASS fat_eligibility_codes\n";
  std::cout << "PASS fat_direct_recursion_eligible\n";
  std::cout << "PASS fat_unsupported_boundaries\n";
  std::cout << "PASS fat_vector_constant_pool_composition\n";
  return 0;
}
"""


def main() -> int:
    cxx = shutil.which("g++") or shutil.which("c++")
    if cxx is None:
        raise RuntimeError("C++ compiler not found")
    with tempfile.TemporaryDirectory(prefix="yoolang-fat-eligibility-") as directory:
        work = Path(directory)
        source = work / "fat_eligibility.cpp"
        binary = work / "fat_eligibility"
        source.write_text(textwrap.dedent(HARNESS), encoding="utf-8")
        command = [
            cxx,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'include'}",
            str(source),
            str(ROOT / "src/oir/OIR.cpp"),
            str(ROOT / "src/oir/OIRAnalysis.cpp"),
            str(ROOT / "src/oir/OIRDataLayout.cpp"),
            str(ROOT / "src/builtin/BuiltinRegistry.cpp"),
            str(ROOT / "src/pass/oir/OIRFatMultiversion.cpp"),
            "-o",
            str(binary),
        ]
        compiled = subprocess.run(
            command,
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if compiled.returncode != 0:
            raise RuntimeError(
                f"eligibility harness compile failed\n{compiled.stdout}\n{compiled.stderr}"
            )
        executed = subprocess.run(
            [str(binary)],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if executed.returncode != 0:
            raise RuntimeError(
                f"eligibility harness failed\n{executed.stdout}\n{executed.stderr}"
            )
        print(executed.stdout, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
