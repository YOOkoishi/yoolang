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
#include "ast/ast_printer.h"
#include "front/Diagnostic.h"
#include "front/SourceLocation.h"
#include "front/parser.h"
#include "sema/ConstantEvaluator.h"
#include "sema/SemanticModel.h"
#include "sema/SemanticType.h"

#include <cstdio>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

struct TestFailure final : public std::exception {
    explicit TestFailure(std::string message) : message(std::move(message)) {}

    const char *what() const noexcept override {
        return message.c_str();
    }

    std::string message;
};

#define REQUIRE(condition)                                                                      \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            throw TestFailure(std::string(__FILE__) + ":" + std::to_string(__LINE__) +        \
                              ": requirement failed: " #condition);                           \
        }                                                                                       \
    } while (false)

template <typename Action>
void require_invalid_argument(Action action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    REQUIRE(rejected);
}

const sema::CheckedInteger &require_integer(const sema::CheckedIntegerResult &result) {
    REQUIRE(result.ok());
    REQUIRE(result.error == sema::ConstantEvalError::None);
    REQUIRE(result.value.has_value());
    return *result.value;
}

void require_error(const sema::CheckedIntegerResult &result,
                   sema::ConstantEvalError expected) {
    REQUIRE(!result.ok());
    REQUIRE(!result.value.has_value());
    REQUIRE(result.error == expected);
    REQUIRE(!result.message.empty());
}

void require_extent_error(const sema::CheckedExtentResult &result,
                          sema::ConstantEvalError expected) {
    REQUIRE(!result.ok());
    REQUIRE(!result.value.has_value());
    REQUIRE(result.error == expected);
    REQUIRE(!result.message.empty());
}

struct ParsedSource {
    int result = 1;
    std::unique_ptr<CompUnit> ast;
    front::DiagnosticEngine diagnostics;
};

FILE *open_source(const std::string &source) {
    FILE *file = std::tmpfile();
    if (file == nullptr) {
        throw TestFailure("failed to create temporary source file");
    }
    if (std::fwrite(source.data(), 1, source.size(), file) != source.size()) {
        std::fclose(file);
        throw TestFailure("failed to write temporary source file");
    }
    std::rewind(file);
    return file;
}

ParsedSource parse_source(const std::string &source, front::SourceFileId file_id = 41) {
    ParsedSource parsed;
    FILE *file = open_source(source);
    lexer_input = file;
    parsed.result = parse(parsed.ast, parsed.diagnostics, file_id);
    lexer_input = nullptr;
    std::fclose(file);
    return parsed;
}

void test_source_locations_and_diagnostics() {
    const front::SourceLocation begin(7, 20, 3, 5);
    const front::SourceLocation end(7, 24, 3, 9);
    const front::SourceRange range(begin, end);

    REQUIRE(begin.valid());
    REQUIRE(range.valid());
    REQUIRE(front::SourceRange::point(begin).valid());
    REQUIRE(!front::SourceLocation().valid());
    REQUIRE(!front::SourceRange(begin, front::SourceLocation(8, 24, 3, 9)).valid());
    REQUIRE(!front::SourceRange(end, begin).valid());

    front::DiagnosticEngine diagnostics;
    diagnostics.note(front::DiagnosticCode::Unknown, front::SourceRange::point(begin),
                     "context");
    diagnostics.warning(front::DiagnosticCode::SemaTypeMismatch, range, "conversion");
    diagnostics.error(front::DiagnosticCode::ConstOverflow, range, "constant overflow");

    REQUIRE(diagnostics.diagnostics().size() == 3);
    REQUIRE(diagnostics.error_count() == 1);
    REQUIRE(diagnostics.has_error());
    REQUIRE(diagnostics.diagnostics()[2].range == range);
    REQUIRE(diagnostics.diagnostics()[2].message == "constant overflow");
    REQUIRE(front::diagnostic_severity_name(front::DiagnosticSeverity::Warning) == "warning");
    REQUIRE(front::diagnostic_code_name(front::DiagnosticCode::ConstOverflow) == "CE0003");
    REQUIRE(front::diagnostic_code_name(front::DiagnosticCode::SemaTypeMismatch) == "SE0002");

    diagnostics.clear();
    REQUIRE(diagnostics.diagnostics().empty());
    REQUIRE(diagnostics.error_count() == 0);
    REQUIRE(!diagnostics.has_error());
}

void test_lexer_tokens_ranges_and_bitwise_punctuation() {
    const std::string source =
        "vector<int,3> value; mask<7> m; ~value ^ 1 extern int f(...);";
    FILE *file = open_source(source);
    front::DiagnosticEngine diagnostics;
    lexer_input = file;
    resetLexer(&diagnostics, 19);

    REQUIRE(nextToken() == TOK_VECTOR);
    REQUIRE(token_value.spelling == "vector");
    REQUIRE(token_value.range.valid());
    REQUIRE(token_value.range.begin.file_id == 19);
    REQUIRE(token_value.range.begin.line == 1);
    REQUIRE(token_value.range.begin.column == 1);
    REQUIRE(nextToken() == '<');
    REQUIRE(nextToken() == TOK_INT);
    REQUIRE(nextToken() == ',');
    REQUIRE(nextToken() == TOK_INT_CONST);
    REQUIRE(token_value.int_val == 3);
    REQUIRE(nextToken() == '>');
    REQUIRE(nextToken() == TOK_IDENT);
    REQUIRE(token_value.str_val == "value");
    REQUIRE(nextToken() == ';');
    REQUIRE(nextToken() == TOK_MASK);
    while (nextToken() != '~') {
        REQUIRE(token_value.range.valid());
    }
    REQUIRE(token_value.spelling == "~");
    REQUIRE(nextToken() == TOK_IDENT);
    REQUIRE(nextToken() == '^');
    REQUIRE(token_value.spelling == "^");
    REQUIRE(nextToken() == TOK_INT_CONST);
    REQUIRE(nextToken() == TOK_EXTERN);
    REQUIRE(token_value.spelling == "extern");
    REQUIRE(nextToken() == TOK_INT);
    REQUIRE(nextToken() == TOK_IDENT);
    REQUIRE(token_value.str_val == "f");
    REQUIRE(nextToken() == '(');
    REQUIRE(nextToken() == TOK_ELLIPSIS);
    REQUIRE(token_value.spelling == "...");
    REQUIRE(nextToken() == ')');
    REQUIRE(nextToken() == ';');
    REQUIRE(nextToken() == TOK_EOF);
    REQUIRE(token_value.range.valid());
    REQUIRE(!diagnostics.has_error());

    lexer_input = nullptr;
    std::fclose(file);
}

void test_type_syntax_parser_and_ast_ranges() {
    const std::string source = R"(
vector<int,1> g1;
vector<float,3> g3[2];
mask<7> g7;
vector<int, (8 > 3) + 2> comparison_lane;

vector<int,31> pass(vector<int,31> x, mask<31> active) {
  vector<float,1 + 2> local = {1.0, 2.0, 3.0};
  return x;
}

void sink(mask<7> value) {
  return;
}
)";
    auto parsed = parse_source(source, 42);
    REQUIRE(parsed.result == 0);
    REQUIRE(parsed.ast != nullptr);
    REQUIRE(!parsed.diagnostics.has_error());
    REQUIRE(parsed.ast->source_range.valid());
    REQUIRE(parsed.ast->source_range.begin.file_id == 42);
    REQUIRE(parsed.ast->global_decls.size() == 4);
    REQUIRE(parsed.ast->functions.size() == 2);

    const auto &g1 = *parsed.ast->global_decls[0];
    REQUIRE(g1.source_range.valid());
    REQUIRE(g1.type_syntax->kind() == TypeSyntax::Kind::Vector);
    REQUIRE(g1.type_syntax->vector_element_type() == BuiltinType::Int);
    REQUIRE(g1.base_type == BuiltinType::Int);
    REQUIRE(g1.type_syntax == g1.decls[0]->type_syntax);
    REQUIRE(g1.type_syntax.use_count() >= 2);
    auto *g1_lanes = dynamic_cast<const IntLiteral *>(
        &g1.type_syntax->lane_expression()->expression());
    REQUIRE(g1_lanes != nullptr);
    REQUIRE(g1_lanes->value == 1);
    REQUIRE(g1_lanes->source_range.valid());

    const auto &g3 = *parsed.ast->global_decls[1];
    REQUIRE(g3.type_syntax->kind() == TypeSyntax::Kind::Vector);
    REQUIRE(g3.type_syntax->vector_element_type() == BuiltinType::Float);
    REQUIRE(g3.decls[0]->dimensions.size() == 1);
    REQUIRE(g3.decls[0]->dimensions[0]->source_range.valid());

    const auto &g7 = *parsed.ast->global_decls[2];
    REQUIRE(g7.type_syntax->kind() == TypeSyntax::Kind::Mask);
    REQUIRE(g7.base_type == BuiltinType::Int);
    REQUIRE(g7.type_syntax->lane_expression()->source_range.valid());

    const auto &comparison_lane = *parsed.ast->global_decls[3];
    auto *lane_add = dynamic_cast<const BinaryExpr *>(
        &comparison_lane.type_syntax->lane_expression()->expression());
    REQUIRE(lane_add != nullptr);
    REQUIRE(lane_add->op == BinaryOp::Add);
    auto *lane_compare = dynamic_cast<const BinaryExpr *>(lane_add->lhs.get());
    REQUIRE(lane_compare != nullptr);
    REQUIRE(lane_compare->op == BinaryOp::Gt);

    const auto &pass = *parsed.ast->functions[0];
    REQUIRE(pass.return_type_syntax->kind() == TypeSyntax::Kind::Vector);
    REQUIRE(pass.return_type == BuiltinType::Int);
    REQUIRE(pass.params.size() == 2);
    REQUIRE(pass.params[0].type_syntax->kind() == TypeSyntax::Kind::Vector);
    REQUIRE(pass.params[1].type_syntax->kind() == TypeSyntax::Kind::Mask);
    REQUIRE(pass.params[0].source_range.valid());
    REQUIRE(pass.body->source_range.valid());
    auto *local = dynamic_cast<DeclStmt *>(pass.body->stmts[0].get());
    REQUIRE(local != nullptr);
    REQUIRE(local->type_syntax->kind() == TypeSyntax::Kind::Vector);
    REQUIRE(local->decls[0]->init != nullptr);
    REQUIRE(local->decls[0]->init->elems.size() == 3);

    const auto &sink = *parsed.ast->functions[1];
    REQUIRE(sink.return_type_syntax->kind() == TypeSyntax::Kind::Builtin);
    REQUIRE(sink.return_type_syntax->builtin_type() == BuiltinType::Void);
    REQUIRE(sink.params[0].type_syntax->kind() == TypeSyntax::Kind::Mask);

    const std::string printed = print_ast_to_string(*parsed.ast);
    REQUIRE(printed.find("DeclStmt type=vector<int, 1>") != std::string::npos);
    REQUIRE(printed.find("DeclStmt type=vector<float, 3>") != std::string::npos);
    REQUIRE(printed.find("DeclStmt type=mask<7>") != std::string::npos);
    REQUIRE(printed.find("vector<int, ((8 > 3) + 2)>") != std::string::npos);
    REQUIRE(printed.find("FuncDef name=pass return=vector<int, 31>") != std::string::npos);
    REQUIRE(printed.find("Param[1] name=active type=mask<31>") != std::string::npos);
    REQUIRE(printed.find("FuncDef name=sink return=void") != std::string::npos);

    static_assert(std::is_same_v<TypeSyntaxRef::element_type, const TypeSyntax>);
}

void test_bitwise_expression_precedence() {
    auto parsed = parse_source(R"(
int bits(int a, int b, int c) {
  return ~a | b ^ c & 1;
}
)");
    REQUIRE(parsed.result == 0);
    REQUIRE(parsed.ast != nullptr);
    auto *return_statement =
        dynamic_cast<ReturnStmt *>(parsed.ast->functions[0]->body->stmts[0].get());
    REQUIRE(return_statement != nullptr);
    auto *bit_or = dynamic_cast<BinaryExpr *>(return_statement->expr.get());
    REQUIRE(bit_or != nullptr);
    REQUIRE(bit_or->op == BinaryOp::BitOr);
    auto *bit_not = dynamic_cast<UnaryExpr *>(bit_or->lhs.get());
    REQUIRE(bit_not != nullptr);
    REQUIRE(bit_not->op == UnaryOp::BitNot);
    auto *bit_xor = dynamic_cast<BinaryExpr *>(bit_or->rhs.get());
    REQUIRE(bit_xor != nullptr);
    REQUIRE(bit_xor->op == BinaryOp::BitXor);
    auto *bit_and = dynamic_cast<BinaryExpr *>(bit_xor->rhs.get());
    REQUIRE(bit_and != nullptr);
    REQUIRE(bit_and->op == BinaryOp::BitAnd);

    const std::string printed = print_ast_to_string(*parsed.ast);
    REQUIRE(printed.find("BinaryExpr op=BitOr") != std::string::npos);
    REQUIRE(printed.find("BinaryExpr op=BitXor") != std::string::npos);
    REQUIRE(printed.find("BinaryExpr op=BitAnd") != std::string::npos);
    REQUIRE(printed.find("UnaryExpr op=BitNot") != std::string::npos);
}

void test_typed_vector_expression_syntax() {
    auto parsed = parse_source(R"(
vector<int,3> build() {
  vector<int,3> a = vector<int,3>{};
  vector<int,3> b = vector<int,3>(1);
  mask<3> m = mask<3>{0, 1, 0};
  vector<int,3>{1, 2, 3};
  return b;
}
)", 93);
    REQUIRE(parsed.result == 0);
    REQUIRE(parsed.ast != nullptr);
    auto &body = *parsed.ast->functions[0]->body;
    REQUIRE(body.stmts.size() == 5);

    auto &a = *dynamic_cast<DeclStmt *>(body.stmts[0].get())->decls[0];
    auto *zero = dynamic_cast<TypedVectorLiteralExpr *>(a.init->expr.get());
    REQUIRE(zero != nullptr);
    REQUIRE(zero->type_syntax->kind() == TypeSyntax::Kind::Vector);
    REQUIRE(zero->lanes.empty());
    REQUIRE(zero->source_range.valid());
    REQUIRE(zero->source_range.begin.file_id == 93);

    auto &b = *dynamic_cast<DeclStmt *>(body.stmts[1].get())->decls[0];
    auto *constructor = dynamic_cast<VectorCastExpr *>(b.init->expr.get());
    REQUIRE(constructor != nullptr);
    REQUIRE(constructor->target_type_syntax->kind() == TypeSyntax::Kind::Vector);
    REQUIRE(dynamic_cast<IntLiteral *>(constructor->operand.get()) != nullptr);
    REQUIRE(constructor->source_range.valid());

    auto &m = *dynamic_cast<DeclStmt *>(body.stmts[2].get())->decls[0];
    auto *mask = dynamic_cast<TypedVectorLiteralExpr *>(m.init->expr.get());
    REQUIRE(mask != nullptr);
    REQUIRE(mask->type_syntax->kind() == TypeSyntax::Kind::Mask);
    REQUIRE(mask->lanes.size() == 3);

    auto *expression_statement = dynamic_cast<ExprStmt *>(body.stmts[3].get());
    REQUIRE(expression_statement != nullptr);
    REQUIRE(dynamic_cast<TypedVectorLiteralExpr *>(expression_statement->expr.get()) != nullptr);

    const std::string printed = print_ast_to_string(*parsed.ast);
    REQUIRE(printed.find("TypedVectorLiteralExpr type=vector<int, 3>") != std::string::npos);
    REQUIRE(printed.find("VectorCastExpr type=vector<int, 3>") != std::string::npos);
    REQUIRE(printed.find("TypedVectorLiteralExpr type=mask<3>") != std::string::npos);
    REQUIRE(printed.find("<zero>") != std::string::npos);
}

void test_checked_numeric_diagnostics_and_parser_recovery() {
    auto parsed = parse_source(R"(
void not_a_variable;
vector<void,4> bad_vector;
int huge = 999999999999999999999999;
int after_huge = 1;
float malformed = 1e+;
int after_malformed = 2;
)", 77);
    REQUIRE(parsed.result != 0);
    REQUIRE(parsed.ast != nullptr);
    REQUIRE(parsed.diagnostics.error_count() >= 5);

    bool saw_integer_range = false;
    bool saw_invalid_float = false;
    bool saw_parse_error = false;
    for (const auto &diagnostic : parsed.diagnostics.diagnostics()) {
        REQUIRE(diagnostic.range.valid());
        REQUIRE(diagnostic.range.begin.file_id == 77);
        REQUIRE(!diagnostic.message.empty());
        saw_integer_range = saw_integer_range ||
                            diagnostic.code ==
                                front::DiagnosticCode::LexIntegerLiteralOutOfRange;
        saw_invalid_float =
            saw_invalid_float || diagnostic.code == front::DiagnosticCode::LexInvalidFloatLiteral;
        saw_parse_error = saw_parse_error ||
                          diagnostic.code == front::DiagnosticCode::ParseExpectedToken ||
                          diagnostic.code == front::DiagnosticCode::ParseUnexpectedToken;
    }
    REQUIRE(saw_integer_range);
    REQUIRE(saw_invalid_float);
    REQUIRE(saw_parse_error);

    bool saw_after_huge = false;
    bool saw_after_malformed = false;
    for (const auto &declaration : parsed.ast->global_decls) {
        for (const auto &item : declaration->decls) {
            saw_after_huge = saw_after_huge || item->name == "after_huge";
            saw_after_malformed = saw_after_malformed || item->name == "after_malformed";
        }
    }
    REQUIRE(saw_after_huge);
    REQUIRE(saw_after_malformed);

    auto comment = parse_source("int before; /* unterminated", 88);
    REQUIRE(comment.result != 0);
    REQUIRE(comment.diagnostics.has_error());
    REQUIRE(comment.diagnostics.diagnostics()[0].code ==
            front::DiagnosticCode::LexUnterminatedComment);
    REQUIRE(comment.diagnostics.diagnostics()[0].range.valid());

    auto no_vector_literal = parse_source("int f() { return vector(1); }", 89);
    REQUIRE(no_vector_literal.result != 0);
    REQUIRE(no_vector_literal.diagnostics.has_error());
}

void test_scalar_parser_compatibility_fields() {
    auto parsed = parse_source(R"(
const int global = 3;
float scalar(float x, int a[]) {
  int local[2] = {1, 2};
  if (x > 0.0) local[0] = global;
  while (local[0] > 0) local[0] = local[0] - 1;
  return x + local[1];
}
)");
    REQUIRE(parsed.result == 0);
    REQUIRE(parsed.ast != nullptr);
    REQUIRE(parsed.ast->global_decls[0]->base_type == BuiltinType::Int);
    REQUIRE(parsed.ast->global_decls[0]->decls[0]->base_type == BuiltinType::Int);
    REQUIRE(parsed.ast->functions[0]->return_type == BuiltinType::Float);
    REQUIRE(parsed.ast->functions[0]->params[0].type == BuiltinType::Float);
    REQUIRE(parsed.ast->functions[0]->params[1].type == BuiltinType::Int);
    REQUIRE(parsed.ast->functions[0]->params[1].dimensions.size() == 1);
    REQUIRE(parsed.ast->functions[0]->params[1].dimensions[0] == nullptr);
}

void test_semantic_type_interning_and_shapes() {
    sema::SemanticTypeContext types;
    auto *i32 = types.int_type();
    auto *f32 = types.float_type();
    auto *v4i32 = types.fixed_vector_type(i32, 4);
    auto *v4i32_again = types.fixed_vector_type(i32, 4);
    auto *v4f32 = types.fixed_vector_type(f32, 4);
    auto *mask4 = types.mask_type(4);

    REQUIRE(types.int_type() == i32);
    REQUIRE(types.float_type() == f32);
    REQUIRE(v4i32 == v4i32_again);
    REQUIRE(v4i32 != v4f32);
    REQUIRE(mask4 == types.mask_type(4));
    REQUIRE(mask4 != v4i32);
    REQUIRE(v4i32->is_fixed_vector());
    REQUIRE(v4i32->is_vector());
    REQUIRE(!v4i32->is_integer());
    REQUIRE(!v4i32->is_float());
    REQUIRE(!v4i32->is_numeric_scalar());
    REQUIRE(v4i32->element_type() == i32);
    REQUIRE(v4i32->lane_count() == 4);
    REQUIRE(v4i32->str() == "vector<int, 4>");
    REQUIRE(mask4->is_mask());
    REQUIRE(!mask4->is_vector());
    REQUIRE(mask4->element_type() == nullptr);
    REQUIRE(mask4->lane_count() == 4);
    REQUIRE(mask4->str() == "mask<4>");

    auto *array = types.array_type(v4i32, 8);
    auto *pointer = types.pointer_type(array);
    auto *function = types.function_type(types.void_type(), {pointer, mask4});
    auto *variadic = types.function_type(types.void_type(), {pointer, mask4}, true);
    REQUIRE(array == types.array_type(v4i32_again, 8));
    REQUIRE(array->element_type() == v4i32);
    REQUIRE(array->array_bound() == 8);
    REQUIRE(pointer == types.pointer_type(array));
    REQUIRE(pointer->pointee_type() == array);
    REQUIRE(function == types.function_type(types.void_type(), {pointer, mask4}));
    REQUIRE(function != variadic);
    REQUIRE(!function->is_variadic());
    REQUIRE(variadic->is_variadic());
    REQUIRE(variadic->str().find("...") != std::string::npos);
    REQUIRE(function->return_type() == types.void_type());
    REQUIRE(function->parameter_types() == std::vector<sema::SemanticTypeRef>({pointer, mask4}));
    REQUIRE(function->str() == "func<(ptr<array<8 x vector<int, 4>>>, mask<4>) -> void>");
}

void test_semantic_type_rejects_invalid_shapes() {
    sema::SemanticTypeContext types;

    require_invalid_argument([&] { (void)types.fixed_vector_type(types.int_type(), 0); });
    require_invalid_argument([&] { (void)types.mask_type(0); });
    require_invalid_argument(
        [&] { (void)types.fixed_vector_type(types.mask_type(4), 4); });
    require_invalid_argument(
        [&] { (void)types.fixed_vector_type(types.pointer_type(types.int_type()), 4); });
    require_invalid_argument([&] { (void)types.array_type(types.int_type(), 0); });
    require_invalid_argument([&] { (void)types.array_type(types.void_type(), 1); });
    require_invalid_argument([&] { (void)types.pointer_type(nullptr); });
    require_invalid_argument(
        [&] { (void)types.function_type(types.int_type(), {types.void_type()}); });
}

void test_checked_integer_successes() {
    using sema::CheckedInteger;

    auto parsed_i32 = sema::parse_signed_integer("-2147483648", sema::SignedIntegerWidth::Bits32);
    REQUIRE(require_integer(parsed_i32).value() == std::numeric_limits<std::int32_t>::min());
    auto parsed_i64 =
        sema::parse_signed_integer("9223372036854775807", sema::SignedIntegerWidth::Bits64);
    REQUIRE(require_integer(parsed_i64).value() == std::numeric_limits<std::int64_t>::max());

    REQUIRE(require_integer(sema::checked_add(CheckedInteger::i32(12), CheckedInteger::i32(7)))
                .value() == 19);
    REQUIRE(require_integer(sema::checked_sub(CheckedInteger::i64(12), CheckedInteger::i64(20)))
                .value() == -8);
    REQUIRE(require_integer(sema::checked_mul(CheckedInteger::i32(-9), CheckedInteger::i32(7)))
                .value() == -63);
    REQUIRE(require_integer(sema::checked_div(CheckedInteger::i64(-17), CheckedInteger::i64(5)))
                .value() == -3);
    REQUIRE(require_integer(sema::checked_rem(CheckedInteger::i32(-17), CheckedInteger::i32(5)))
                .value() == -2);
    REQUIRE(require_integer(sema::checked_neg(CheckedInteger::i64(41))).value() == -41);
}

void test_checked_integer_failures() {
    using sema::CheckedInteger;
    using sema::ConstantEvalError;

    require_error(sema::parse_signed_integer("", sema::SignedIntegerWidth::Bits32),
                  ConstantEvalError::InvalidLiteral);
    require_error(sema::parse_signed_integer("1x", sema::SignedIntegerWidth::Bits32),
                  ConstantEvalError::InvalidLiteral);
    require_error(sema::parse_signed_integer("2147483648", sema::SignedIntegerWidth::Bits32),
                  ConstantEvalError::Overflow);
    require_error(
        sema::parse_signed_integer("-9223372036854775809", sema::SignedIntegerWidth::Bits64),
        ConstantEvalError::Overflow);
    require_error(
        sema::checked_add(CheckedInteger::i32(std::numeric_limits<std::int32_t>::max()),
                          CheckedInteger::i32(1)),
        ConstantEvalError::Overflow);
    require_error(
        sema::checked_sub(CheckedInteger::i64(std::numeric_limits<std::int64_t>::min()),
                          CheckedInteger::i64(1)),
        ConstantEvalError::Overflow);
    require_error(sema::checked_mul(CheckedInteger::i32(50000), CheckedInteger::i32(50000)),
                  ConstantEvalError::Overflow);
    require_error(sema::checked_div(CheckedInteger::i64(1), CheckedInteger::i64(0)),
                  ConstantEvalError::DivisionByZero);
    require_error(sema::checked_rem(CheckedInteger::i32(1), CheckedInteger::i32(0)),
                  ConstantEvalError::DivisionByZero);
    require_error(
        sema::checked_div(CheckedInteger::i32(std::numeric_limits<std::int32_t>::min()),
                          CheckedInteger::i32(-1)),
        ConstantEvalError::Overflow);
    require_error(
        sema::checked_rem(CheckedInteger::i64(std::numeric_limits<std::int64_t>::min()),
                          CheckedInteger::i64(-1)),
        ConstantEvalError::Overflow);
    require_error(
        sema::checked_neg(CheckedInteger::i64(std::numeric_limits<std::int64_t>::min())),
        ConstantEvalError::Overflow);
    require_error(sema::checked_add(CheckedInteger::i32(1), CheckedInteger::i64(1)),
                  ConstantEvalError::WidthMismatch);
}

void test_checked_positive_extents() {
    using sema::CheckedInteger;
    using sema::ConstantEvalError;
    using sema::ExtentKind;

    auto lane = sema::checked_positive_extent(CheckedInteger::i32(16), ExtentKind::LaneCount);
    REQUIRE(lane.ok());
    REQUIRE(lane.value == 16);
    require_extent_error(
        sema::checked_positive_extent(CheckedInteger::i64(0), ExtentKind::LaneCount),
        ConstantEvalError::NonPositive);
    require_extent_error(
        sema::checked_positive_extent(CheckedInteger::i32(-2), ExtentKind::ArrayBound),
        ConstantEvalError::NonPositive);

    auto max_extent =
        sema::parse_positive_extent("18446744073709551615", ExtentKind::ArrayBound);
    REQUIRE(max_extent.ok());
    REQUIRE(max_extent.value == std::numeric_limits<std::uint64_t>::max());
    require_extent_error(
        sema::parse_positive_extent("18446744073709551616", ExtentKind::ArrayBound),
        ConstantEvalError::OutOfRange);
    require_extent_error(sema::parse_positive_extent("0", ExtentKind::LaneCount),
                         ConstantEvalError::NonPositive);
    require_extent_error(sema::parse_positive_extent("-1", ExtentKind::ArrayBound),
                         ConstantEvalError::NonPositive);
    require_extent_error(sema::parse_positive_extent("4.0", ExtentKind::LaneCount),
                         ConstantEvalError::InvalidLiteral);
}

void test_semantic_model_side_tables() {
    auto type_context = std::make_shared<sema::SemanticTypeContext>();
    sema::SemanticModel model(type_context);
    IntLiteral literal(7);
    VarDecl declaration(false, BuiltinType::Int, "value");
    FuncDef function(BuiltinType::Int, "f");
    CallExpr call("vector_builtin");

    auto *i32 = model.types().int_type();
    auto *f32 = model.types().float_type();
    auto *v8i32 = model.types().fixed_vector_type(i32, 8);
    auto *function_type = model.types().function_type(i32, {i32});

    REQUIRE(model.expr_type(literal) == nullptr);
    model.set_expr_type(literal, i32);
    model.set_declaration_type(declaration, i32);
    model.set_function_type(function, function_type);
    model.set_conversion(
        literal, sema::ConversionInfo{sema::ConversionKind::IntToFloat, i32, f32});
    model.set_checked_constant(literal, sema::CheckedInteger::i32(7));
    auto semantic_constant = sema::SemanticConstant::integer(i32, 7);
    model.set_constant(literal, semantic_constant);
    InitVal initializer;
    model.set_initializer_constant(initializer, semantic_constant);
    model.set_builtin_binding(
        call, sema::BuiltinBinding{17, v8i32, {i32}, {v8i32}, {8}});

    REQUIRE(model.type_context() == type_context);
    REQUIRE(model.expr_type(literal) == i32);
    REQUIRE(model.declaration_type(declaration) == i32);
    REQUIRE(model.function_type(function) == function_type);
    REQUIRE(model.conversion(literal) != nullptr);
    REQUIRE(model.conversion(literal)->kind == sema::ConversionKind::IntToFloat);
    REQUIRE(model.conversion(literal)->target_type == f32);
    REQUIRE(model.checked_constant(literal) != nullptr);
    REQUIRE(model.checked_constant(literal)->value() == 7);
    REQUIRE(model.constant(literal) != nullptr);
    REQUIRE((*model.constant(literal))->integer_value() == 7);
    REQUIRE(model.initializer_constant(initializer) != nullptr);
    REQUIRE(model.builtin_binding(call) != nullptr);
    REQUIRE(model.builtin_binding(call)->id == 17);
    REQUIRE(model.builtin_binding(call)->result_type == v8i32);
    REQUIRE(model.builtin_binding(call)->integer_arguments == std::vector<std::uint64_t>({8}));

    require_invalid_argument([&] { model.set_expr_type(literal, nullptr); });
    require_invalid_argument([&] { model.set_function_type(function, i32); });
    require_invalid_argument(
        [&] { model.set_builtin_binding(call, sema::BuiltinBinding{}); });
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"source_locations_and_diagnostics", test_source_locations_and_diagnostics},
        {"lexer_tokens_ranges_and_bitwise_punctuation",
         test_lexer_tokens_ranges_and_bitwise_punctuation},
        {"type_syntax_parser_and_ast_ranges", test_type_syntax_parser_and_ast_ranges},
        {"bitwise_expression_precedence", test_bitwise_expression_precedence},
        {"typed_vector_expression_syntax", test_typed_vector_expression_syntax},
        {"checked_numeric_diagnostics_and_parser_recovery",
         test_checked_numeric_diagnostics_and_parser_recovery},
        {"scalar_parser_compatibility_fields", test_scalar_parser_compatibility_fields},
        {"semantic_type_interning_and_shapes", test_semantic_type_interning_and_shapes},
        {"semantic_type_rejects_invalid_shapes", test_semantic_type_rejects_invalid_shapes},
        {"checked_integer_successes", test_checked_integer_successes},
        {"checked_integer_failures", test_checked_integer_failures},
        {"checked_positive_extents", test_checked_positive_extents},
        {"semantic_model_side_tables", test_semantic_model_side_tables},
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
    env_cxx = os.environ.get("CXX")
    if env_cxx:
        return env_cxx
    for candidate in ("c++", "g++", "clang++"):
        path = shutil.which(candidate)
        if path:
            return path
    return None


def main() -> int:
    cxx = find_cxx()
    if cxx is None:
        print("error: no C++ compiler found in PATH", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="frontend-infra-") as tmp:
        tmp_dir = Path(tmp)
        source = tmp_dir / "frontend_infra_tests.cpp"
        binary = tmp_dir / "frontend_infra_tests"
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
            str(ROOT / "src/ast/ast_printer.cpp"),
            str(ROOT / "src/front/Diagnostic.cpp"),
            str(ROOT / "src/front/lexer.cpp"),
            str(ROOT / "src/front/parser.cpp"),
            str(ROOT / "src/sema/SemanticType.cpp"),
            str(ROOT / "src/sema/ConstantEvaluator.cpp"),
            str(ROOT / "src/sema/SemanticModel.cpp"),
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
            if compile_proc.stdout:
                print(compile_proc.stdout, end="")
            if compile_proc.stderr:
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
        if run_proc.stdout:
            print(run_proc.stdout, end="")
        if run_proc.stderr:
            print(run_proc.stderr, end="", file=sys.stderr)
        return run_proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
