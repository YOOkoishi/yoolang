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
#include "ast/ast.h"
#include "front/parser.h"
#include "pass/PassManager.h"
#include "pass/ast/ASTSemanticAnalysisPass.h"
#include "sema/SemanticModel.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Failure final : std::exception {
    explicit Failure(std::string text) : text(std::move(text)) {}
    const char *what() const noexcept override { return text.c_str(); }
    std::string text;
};

#define REQUIRE(condition)                                                                    \
    do {                                                                                      \
        if (!(condition))                                                                     \
            throw Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +          \
                          ": requirement failed: " #condition);                             \
    } while (false)

FILE *open_source(const std::string &source) {
    FILE *file = std::tmpfile();
    if (!file) throw Failure("tmpfile failed");
    if (std::fwrite(source.data(), 1, source.size(), file) != source.size()) {
        std::fclose(file);
        throw Failure("source write failed");
    }
    std::rewind(file);
    return file;
}

struct Analysis final {
    pass::PassResult result;
    std::unique_ptr<pass::PassContext> context;
    std::shared_ptr<sema::SemanticModel> model;
};

Analysis analyze_ast(std::unique_ptr<CompUnit> ast) {
    auto context = std::make_unique<pass::PassContext>();
    context->set_ast(std::move(ast));
    pass::ASTSemanticAnalysisPass semantic;
    auto result = semantic.run(*context);
    auto *artifact = context->get_artifact<std::shared_ptr<sema::SemanticModel>>(
        pass::ASTSemanticAnalysisPass::kArtifactKey);
    REQUIRE(artifact != nullptr);
    REQUIRE(*artifact != nullptr);
    return Analysis{std::move(result), std::move(context), *artifact};
}

Analysis analyze(const std::string &source) {
    FILE *file = open_source(source);
    std::unique_ptr<CompUnit> ast;
    front::DiagnosticEngine parse_diagnostics;
    lexer_input = file;
    const int parse_result = parse(ast, parse_diagnostics, 71);
    lexer_input = nullptr;
    std::fclose(file);
    if (parse_result != 0 || !ast)
        throw Failure("test source did not parse: " + source);
    return analyze_ast(std::move(ast));
}

void require_invalid(const std::string &source, const std::string &code) {
    auto analysis = analyze(source);
    REQUIRE(!analysis.result.success);
    REQUIRE(analysis.result.message.rfind(code + ":", 0) == 0);
    REQUIRE(analysis.model->diagnostics().has_error());
}

void test_valid_authoritative_model() {
    auto analysis = analyze(R"(
const int N = 3;
vector<int,N> table[7];
vector<float,N> calculate(vector<float,N> x, vector<int,N> y, int scalar) {
  mask<N> compared = x > scalar;
  mask<N> inverted = ~compared;
  mask<N> combined = compared & inverted;
  vector<float,N> sum = x + scalar;
  vector<int,N> lanes = {1, 2, 3};
  int lane = compared[0];
  sum = select(compared, sum, x);
  if (any(combined)) return sum;
  return x;
}
int consume(int row[]) { return row[0]; }
int matrix_value(int data[][3]) { return data[0][0]; }
int main() {
  int values[2] = {1, 2};
  int matrix[2][3] = {};
  getarray(matrix);
  return consume(values) + matrix_value(matrix);
}
)");
    REQUIRE(analysis.result.success);
    REQUIRE(!analysis.model->diagnostics().has_error());

    auto &unit = *analysis.context->ast();
    REQUIRE(unit.global_decls.size() == 2);
    auto &table = *unit.global_decls[1]->decls[0];
    auto *table_type = analysis.model->declaration_type(table);
    REQUIRE(table_type != nullptr && table_type->is_array());
    REQUIRE(table_type->array_bound() == 7);
    REQUIRE(table_type->element_type()->is_fixed_vector());
    REQUIRE(table_type->element_type()->lane_count() == 3);
    REQUIRE(analysis.model->checked_extent(*table.dimensions[0]) != nullptr);
    REQUIRE(*analysis.model->checked_extent(*table.dimensions[0]) == 7);

    auto &function = *unit.functions[0];
    auto *function_type = analysis.model->function_type(function);
    REQUIRE(function_type != nullptr && function_type->is_function());
    REQUIRE(function_type->return_type()->is_fixed_vector());
    REQUIRE(function_type->parameter_types().size() == 3);
    REQUIRE(analysis.model->parameter_type(function.params[0]) ==
            function_type->parameter_types()[0]);

    auto &compared_decl = *dynamic_cast<DeclStmt *>(function.body->stmts[0].get())->decls[0];
    auto &comparison = *dynamic_cast<BinaryExpr *>(compared_decl.init->expr.get());
    REQUIRE(analysis.model->expr_type(comparison)->is_mask());
    REQUIRE(analysis.model->expr_type(comparison)->lane_count() == 3);
    const auto *comparison_splat = analysis.model->conversions(*comparison.rhs);
    REQUIRE(comparison_splat != nullptr && comparison_splat->size() >= 2);
    REQUIRE((*comparison_splat)[comparison_splat->size() - 2].kind ==
            sema::ConversionKind::IntToFloat);
    REQUIRE(comparison_splat->back().kind == sema::ConversionKind::ScalarSplat);

    auto &sum_decl = *dynamic_cast<DeclStmt *>(function.body->stmts[3].get())->decls[0];
    auto &sum = *dynamic_cast<BinaryExpr *>(sum_decl.init->expr.get());
    REQUIRE(analysis.model->expr_type(sum)->is_fixed_vector());
    REQUIRE(analysis.model->expr_type(sum)->element_type()->is_float());

    auto &assignment = *dynamic_cast<AssignStmt *>(function.body->stmts[6].get());
    auto &select = *dynamic_cast<CallExpr *>(assignment.value.get());
    REQUIRE(analysis.model->builtin_binding(select) != nullptr);
    REQUIRE(analysis.model->builtin_binding(select)->result_type->is_fixed_vector());
    REQUIRE(analysis.model->builtin_binding(select)->integer_arguments ==
            std::vector<std::uint64_t>({3}));

    auto &condition = *dynamic_cast<IfStmt *>(function.body->stmts[7].get())->cond;
    auto &any = *dynamic_cast<CallExpr *>(&condition);
    REQUIRE(analysis.model->builtin_binding(any) != nullptr);
    REQUIRE(analysis.model->expr_type(any)->is_integer());

    auto &lane_decl = *dynamic_cast<DeclStmt *>(function.body->stmts[5].get())->decls[0];
    auto &lane = *dynamic_cast<LValExpr *>(lane_decl.init->expr.get());
    REQUIRE(analysis.model->expr_type(lane)->is_integer());
    const auto *lane_conversions = analysis.model->conversions(lane);
    REQUIRE(lane_conversions != nullptr);
    bool found_mask_lane = false;
    for (const auto &conversion : *lane_conversions)
        found_mask_lane = found_mask_lane ||
                          conversion.kind == sema::ConversionKind::MaskLaneToInt;
    REQUIRE(found_mask_lane);
}

void test_invalid_semantics() {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"vector<int,0> x; int main(){return 0;}", "SE0006"},
        {"vector<int,-1> x; int main(){return 0;}", "SE0006"},
        {"int n=4; vector<int,n> x; int main(){return 0;}", "SE0005"},
        {"vector<int,2147483647+1> x; int main(){return 0;}", "CE0003"},
        {"vector<int,4/0> x; int main(){return 0;}", "CE0004"},
        {"int x[0]; int main(){return 0;}", "SE0006"},
        {"vector<int,3> a; vector<int,4> b; int main(){a+b; return 0;}", "SE0007"},
        {"mask<3> a; int main(){a+1; return 0;}", "SE0007"},
        {"mask<3> a; int main(){if(a) return 1; return 0;}", "SE0011"},
        {"float x; int main(){return x%2;}", "SE0007"},
        {"int x; int main(){return x[0];}", "SE0013"},
        {"vector<int,3> a; mask<4> m; int main(){select(m,a,a); return 0;}", "SE0009"},
        {"vector<int,3> a; mask<3> m; int main(){select(m,a,m); return 0;}", "SE0008"},
        {"vector<int,2> x={{1},{2}}; int main(){return 0;}", "SE0010"},
        {"vector<int,3> x; int main(){getarray(x); return 0;}", "SE0008"},
        {"int main(){vector<int,3> x=vector<int,3>{1,2};return 0;}", "SE0015"},
        {"int main(){mask<3> x=mask<3>{0,2,1};return 0;}", "SE0015"},
        {"int main(){vector<int,3> x={};return x[3];}", "SE0014"},
        {"int main(){vector<int,3> x={};return extract_lane(x,-1);}", "SE0014"},
        {"int main(){vector<int,3> x={};vector<int,3> i={0,1,2};"
         "shuffle(x,x,i);return 0;}", "SE0017"},
        {"int main(){vector<int,3> x={};"
         "shuffle(x,x,vector<int,3>{0,1,6});return 0;}", "SE0017"},
        {"int main(){vector<int,3> x={};putf(x);return 0;}", "SE0016"},
    };
    for (const auto &[source, code] : cases) require_invalid(source, code);
}

void test_typed_expression_constants_and_shuffle() {
    auto analysis = analyze(R"(
vector<int,3> make(vector<float,3> input) {
  vector<int,3> literal = vector<int,3>{-1, 0, 5};
  vector<int,3> converted = vector<int,3>(input);
  return shuffle(converted, literal, vector<int,3>{-1, 0, 5});
}
)");
    REQUIRE(analysis.result.success);
    auto &body = *analysis.context->ast()->functions[0]->body;
    auto &literal_decl = *dynamic_cast<DeclStmt *>(body.stmts[0].get())->decls[0];
    auto &literal = *dynamic_cast<TypedVectorLiteralExpr *>(literal_decl.init->expr.get());
    const auto *literal_constant = analysis.model->constant(literal);
    REQUIRE(literal_constant != nullptr && *literal_constant != nullptr);
    REQUIRE((*literal_constant)->kind() == sema::SemanticConstant::Kind::Aggregate);
    REQUIRE((*literal_constant)->elements().size() == 3);
    REQUIRE((*literal_constant)->elements()[0]->integer_value() == -1);

    auto &converted_decl = *dynamic_cast<DeclStmt *>(body.stmts[1].get())->decls[0];
    auto &converted = *dynamic_cast<VectorCastExpr *>(converted_decl.init->expr.get());
    const auto *conversions = analysis.model->conversions(*converted.operand);
    REQUIRE(conversions != nullptr && !conversions->empty());
    REQUIRE(conversions->back().kind == sema::ConversionKind::VectorElementCast);

    auto &return_stmt = *dynamic_cast<ReturnStmt *>(body.stmts[2].get());
    auto &shuffle = *dynamic_cast<CallExpr *>(return_stmt.expr.get());
    REQUIRE(analysis.model->builtin_binding(shuffle) != nullptr);
    const auto *indices = analysis.model->constant(*shuffle.args[2]);
    REQUIRE(indices != nullptr && (*indices)->elements().size() == 3);
}

void test_mask_select_binding() {
    auto analysis = analyze(R"(
mask<7> choose(mask<7> condition, mask<7> when_true, mask<7> when_false) {
  return select(condition, when_true, when_false);
}
)");
    REQUIRE(analysis.result.success);
    auto &function = *analysis.context->ast()->functions[0];
    auto &return_stmt = *dynamic_cast<ReturnStmt *>(function.body->stmts[0].get());
    auto &select = *dynamic_cast<CallExpr *>(return_stmt.expr.get());
    const auto *binding = analysis.model->builtin_binding(select);
    REQUIRE(binding != nullptr);
    REQUIRE(binding->result_type->is_mask());
    REQUIRE(binding->result_type->lane_count() == 7);
    REQUIRE(binding->argument_types.size() == 3);
    REQUIRE(binding->argument_types[1] == binding->argument_types[2]);
}

void test_external_function_declaration_semantics() {
    auto analysis = analyze(R"(
extern int array_api(int data[], float scale);
extern int log_api(int level, ...);
extern void sink_api(void);
extern vector<int,3> transform_api(vector<int,3> value, mask<3> active);
extern vector<int,3> transform_api(vector<int,3> input, mask<3> predicate);
extern int local_api(int);
int local_api(int value) { return value + 1; }

int use_apis(int values[]) {
  vector<int,3> value = vector<int,3>{1, 2, 3};
  mask<3> active = mask<3>{1, 0, 1};
  vector<int,3> transformed = transform_api(value, active);
  sink_api();
  log_api(7, 2.5);
  return array_api(values, 1.5) + transformed[0] + local_api(3);
}
)");
    REQUIRE(analysis.result.success);
    REQUIRE(!analysis.model->diagnostics().has_error());
    auto &functions = analysis.context->ast()->functions;
    REQUIRE(functions.size() == 8);
    REQUIRE(functions[0]->is_external);
    REQUIRE(!functions[0]->is_variadic);
    auto *array_type = analysis.model->function_type(*functions[0]);
    REQUIRE(array_type != nullptr && array_type->parameter_types().size() == 2);
    REQUIRE(array_type->parameter_types()[0]->is_pointer());
    REQUIRE(array_type->parameter_types()[0]->pointee_type()->is_integer());
    auto *log_type = analysis.model->function_type(*functions[1]);
    REQUIRE(log_type != nullptr && log_type->is_variadic());
    REQUIRE(log_type->parameter_types().size() == 1);
    auto *sink_type = analysis.model->function_type(*functions[2]);
    REQUIRE(sink_type != nullptr && sink_type->return_type()->is_void());
    REQUIRE(sink_type->parameter_types().empty());
    auto *vector_type = analysis.model->function_type(*functions[3]);
    REQUIRE(vector_type != nullptr && vector_type->return_type()->is_fixed_vector());
    REQUIRE(vector_type->parameter_types()[1]->is_mask());
    REQUIRE(analysis.model->function_type(*functions[3]) ==
            analysis.model->function_type(*functions[4]));
    REQUIRE(functions[5]->is_external);
    REQUIRE(!functions[6]->is_external);
    REQUIRE(analysis.model->function_type(*functions[5]) ==
            analysis.model->function_type(*functions[6]));

    require_invalid("extern int clash(int); extern float clash(int);", "SE0018");
    require_invalid("extern int twice(int); int twice(int x){return x;} "
                    "int twice(int x){return x+1;}", "SE0004");
    require_invalid("extern int getint(void); int main(){return 0;}", "SE0004");
    require_invalid("extern int unary(int); int main(){return unary();}", "SE0009");
    require_invalid("int main(){return not_declared(1);}", "SE0003");
    require_invalid("extern int logging(int,...); int main(){vector<int,3> x={};"
                    "return logging(1,x);}", "SE0016");
    REQUIRE(front::diagnostic_code_name(
                front::DiagnosticCode::SemaConflictingDeclaration) == "SE0018");
}

void test_programmatic_void_rejection() {
    auto unit = std::make_unique<CompUnit>();
    auto declaration = std::make_unique<DeclStmt>(false, BuiltinType::Void);
    declaration->decls.push_back(
        std::make_unique<VarDecl>(false, BuiltinType::Void, "invalid_object"));
    unit->global_decls.push_back(std::move(declaration));

    auto function = std::make_unique<FuncDef>(BuiltinType::Int, "invalid_parameter");
    function->params.emplace_back(TypeSyntax::make_builtin(BuiltinType::Void), "value");
    function->body = std::make_unique<BlockStmt>();
    function->body->stmts.push_back(
        std::make_unique<ReturnStmt>(std::make_unique<IntLiteral>(0)));
    unit->functions.push_back(std::move(function));

    auto analysis = analyze_ast(std::move(unit));
    REQUIRE(!analysis.result.success);
    REQUIRE(analysis.result.message.rfind("SE0001:", 0) == 0);
    REQUIRE(analysis.model->diagnostics().error_count() >= 2);
}

} // namespace

int main() {
    try {
        test_valid_authoritative_model();
        std::cout << "PASS valid_authoritative_model\n";
        test_invalid_semantics();
        std::cout << "PASS invalid_semantics\n";
        test_typed_expression_constants_and_shuffle();
        std::cout << "PASS typed_expression_constants_and_shuffle\n";
        test_mask_select_binding();
        std::cout << "PASS mask_select_binding\n";
        test_external_function_declaration_semantics();
        std::cout << "PASS external_function_declaration_semantics\n";
        test_programmatic_void_rejection();
        std::cout << "PASS programmatic_void_rejection\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
"""


CLI_CASES = [
    ("vector<int,0> x; int main(){return 0;}", "SE0006"),
    ("vector<int,-1> x; int main(){return 0;}", "SE0006"),
    ("int n=4; vector<int,n> x; int main(){return 0;}", "SE0005"),
    ("vector<int,2147483647+1> x; int main(){return 0;}", "CE0003"),
    ("vector<int,4/0> x; int main(){return 0;}", "CE0004"),
    ("void x; int main(){return 0;}", "PA"),
    ("int f(void x){return 0;} int main(){return 0;}", "PA"),
    ("vector<int,3> a; vector<int,4> b; int main(){a+b; return 0;}", "SE0007"),
    ("mask<3> a; int main(){if(a) return 1; return 0;}", "SE0011"),
    ("vector<vector<int,2>,2> x; int main(){return 0;}", "PA"),
    ("int main(){vector<int,3>x=vector<int,3>{1,2};return 0;}", "SE0015"),
    ("int main(){mask<3>x=mask<3>{0,2,1};return 0;}", "SE0015"),
    ("int main(){vector<int,3>x={};return x[3];}", "SE0014"),
    ("int main(){vector<int,3>x={};shuffle(x,x,vector<int,3>{0,1,6});return 0;}", "SE0017"),
    ("int main(){vector<int,3>x={};putf(x);return 0;}", "SE0016"),
    ("extern int clash(int); extern float clash(int);", "SE0018"),
    ("extern int twice(int); int twice(int x){return x;} int twice(int x){return x;}",
     "SE0004"),
    ("extern int unary(int); int main(){return unary();}", "SE0009"),
    ("int invalid(int fixed,...){return fixed;}", "PA0002"),
]


def find_cxx() -> str | None:
    if os.environ.get("CXX"):
        return os.environ["CXX"]
    for candidate in ("c++", "g++", "clang++"):
        if shutil.which(candidate):
            return shutil.which(candidate)
    return None


def find_compiler() -> Path | None:
    configured = os.environ.get("YOOLANG_COMPILER")
    if configured:
        path = Path(configured)
        if not path.is_absolute():
            path = ROOT / path
        return path.resolve() if path.is_file() and os.access(path, os.X_OK) else None
    candidates = [
        ROOT / "build/linux/x86_64/release/compiler",
        ROOT / "build/linux/x86_64/debug/compiler",
    ]
    newest_frontend_source = max(
        (ROOT / "src/pass/ast/ASTSemanticAnalysisPass.cpp").stat().st_mtime,
        (ROOT / "src/pass/ast/ASTToYIRPass.cpp").stat().st_mtime,
        (ROOT / "src/front/parser.cpp").stat().st_mtime,
    )
    return next(
        (
            path
            for path in candidates
            if path.is_file() and path.stat().st_mtime >= newest_frontend_source
        ),
        None,
    )


def run_cli_cases(compiler: Path, tmp_dir: Path) -> int:
    cases = list(CLI_CASES)
    for index, (source, expected) in enumerate(cases):
        path = tmp_dir / f"compile_fail_{index}.sy"
        path.write_text(source)
        proc = subprocess.run(
            [str(compiler), str(path), "--emit-yir"],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        if proc.returncode == 0 or expected not in proc.stdout:
            print(f"FAIL CLI case {index}: expected {expected}\n{proc.stdout}", file=sys.stderr)
            return 1
    print(f"PASS cli_compile_fail ({len(cases)} cases)")
    success_cases = [
        ("int main(){return ~1|2^3&4;}", ["yir.noti", "yir.andi", "yir.xori", "yir.ori"]),
        ("int main(){vector<int,3>x=vector<int,3>{1,2,3};"
         "mask<3>m=x>1;return any(m)+x[1];}",
         ["yir.vector.create", "yir.vector.splat", ": mask<3>", "yir.mask.any",
          "yir.vector.extract"]),
        ("extern int foreign(int value); int main(){return foreign(7);}",
         ["yir.declare @foreign", "yir.call @foreign"]),
    ]
    for index, (source, expected) in enumerate(success_cases):
        path = tmp_dir / f"compile_success_{index}.sy"
        path.write_text(source)
        proc = subprocess.run(
            [str(compiler), str(path), "--emit-yir"], cwd=ROOT,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False,
        )
        if proc.returncode != 0 or any(item not in proc.stdout for item in expected):
            print(f"FAIL CLI success case {index}: expected {expected}\n{proc.stdout}",
                  file=sys.stderr)
            return 1
    print(f"PASS cli_vector_and_bitwise_lowering ({len(success_cases)} cases)")
    return 0


def main() -> int:
    cxx = find_cxx()
    if cxx is None:
        print("error: no C++ compiler found", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="frontend-semantic-infra-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "frontend_semantic_infra.cpp"
        binary = tmp_dir / "frontend_semantic_infra"
        source.write_text(textwrap.dedent(SOURCE))
        compile_cmd = [
            cxx,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(ROOT / "include"),
            str(source),
            str(ROOT / "src/ast/ast.cpp"),
            str(ROOT / "src/front/Diagnostic.cpp"),
            str(ROOT / "src/front/lexer.cpp"),
            str(ROOT / "src/front/parser.cpp"),
            str(ROOT / "src/builtin/BuiltinRegistry.cpp"),
            str(ROOT / "src/sema/SemanticType.cpp"),
            str(ROOT / "src/sema/ConstantEvaluator.cpp"),
            str(ROOT / "src/sema/SemanticModel.cpp"),
            str(ROOT / "src/oir/OIR.cpp"),
            str(ROOT / "src/oir/OIRAnalysis.cpp"),
            str(ROOT / "src/oir/OIRDataLayout.cpp"),
            str(ROOT / "src/mir/MIR.cpp"),
            str(ROOT / "src/mir/MachineInstrDesc.cpp"),
            str(ROOT / "src/pass/PassManager.cpp"),
            str(ROOT / "src/pass/ast/ASTSemanticAnalysisPass.cpp"),
            "-o",
            str(binary),
        ]
        compile_proc = subprocess.run(
            compile_cmd,
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
        if run_proc.returncode != 0:
            return run_proc.returncode

        compiler = find_compiler()
        if compiler is None:
            print(
                "FAIL cli_compile_fail_and_lowering: compiler not built or not executable",
                file=sys.stderr,
            )
            return 1
        return run_cli_cases(compiler, tmp_dir)


if __name__ == "__main__":
    raise SystemExit(main())
