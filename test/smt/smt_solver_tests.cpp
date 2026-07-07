#include "pass/SMTProof.h"
#include "smt/Solver.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "smt_solver_tests: " << message << '\n';
        std::exit(1);
    }
}

void test_boolean_sat_unsat() {
    smt::ExprBuilder b;
    smt::Solver solver;
    auto a = b.bool_var("a");
    auto contradiction = b.bool_and(a, b.bool_not(a));
    auto unsat = solver.check(contradiction);
    require(unsat.status == smt::CheckStatus::Unsat, "a && !a must be unsat");

    auto disjunction = b.bool_or(a, b.bool_var("b"));
    auto sat = solver.check(disjunction);
    require(sat.status == smt::CheckStatus::Sat, "a || b must be sat");
    bool value = false;
    require(sat.model.bool_value("a", value) || sat.model.bool_value("b", value),
            "SAT model should expose a Boolean assignment");
}

void test_bv_add_sub_equivalence() {
    smt::ExprBuilder b;
    smt::Solver solver;
    auto x = b.bv_var(32, "x");
    auto y = b.bv_var(32, "y");
    auto counterexample = b.distinct(b.bv_add(b.bv_sub(x, y), y), x);
    auto result = solver.check(counterexample);
    require(result.status == smt::CheckStatus::Unsat,
            "(x - y) + y == x must be proven by UNSAT counterexample, got " +
                smt::to_string(result.status) + " (" + result.reason + ")");
}

void test_linear_normalizer_rejects_textual_atom_collision() {
    smt::ExprBuilder b;
    smt::Solver solver;
    auto left_name = b.bv_var(8, "a b");
    auto left_c = b.bv_var(8, "c");
    auto right_a = b.bv_var(8, "a");
    auto right_name = b.bv_var(8, "b c");

    auto left = b.bv_and(left_name, left_c);
    auto right = b.bv_and(right_a, right_name);
    require(smt::to_string(left) == smt::to_string(right),
            "regression setup should exercise a textual render collision");

    auto result = solver.check(b.distinct(left, right));
    require(result.status == smt::CheckStatus::Sat,
            "textually colliding nonlinear BV atoms must not prove equal");
}

void test_bv_refutation_model() {
    smt::ExprBuilder b;
    smt::Solver solver;
    auto x = b.bv_var(8, "x");
    auto y = b.bv_var(8, "y");
    auto z = b.bv_var(8, "z");
    auto counterexample = b.distinct(b.bv_add(b.bv_sub(x, y), z), x);
    auto result = solver.check(counterexample);
    require(result.status == smt::CheckStatus::Sat,
            "(x - y) + z == x must be refutable when y and z may differ");
    smt::BitVectorValue model_y;
    smt::BitVectorValue model_z;
    require(result.model.bit_vector_value("y", model_y), "SAT model should include y");
    require(result.model.bit_vector_value("z", model_z), "SAT model should include z");
    require(model_y.width == 8 && model_z.width == 8, "model widths must be preserved");
}

void test_signed_unsigned_distinction() {
    smt::ExprBuilder b;
    smt::Solver solver;
    auto minus_one = b.bv_const(8, 0xff);
    auto zero = b.bv_const(8, 0);
    auto signed_true = solver.check(b.bv_slt(minus_one, zero));
    require(signed_true.status == smt::CheckStatus::Sat, "signed -1 < 0 should be sat");

    auto unsigned_false = solver.check(b.bv_ult(minus_one, zero));
    require(unsigned_false.status == smt::CheckStatus::Unsat, "unsigned 255 < 0 should be unsat");
}

void test_model_for_modular_arithmetic() {
    smt::ExprBuilder b;
    smt::Solver solver;
    auto x = b.bv_var(8, "x");
    auto wraps_to_zero = b.equal(b.bv_add(x, b.bv_const(8, 1)), b.bv_const(8, 0));
    auto result = solver.check(wraps_to_zero);
    require(result.status == smt::CheckStatus::Sat, "x + 1 == 0 over bv8 should be sat");
    smt::BitVectorValue model_x;
    require(result.model.bit_vector_value("x", model_x), "model should include x");
    require(model_x.width == 8 && model_x.value == 0xff, "model should assign x = 255");
}

void test_extract_concat_and_shifts() {
    smt::ExprBuilder b;
    smt::Solver solver;
    auto high = b.bv_const(4, 0xa);
    auto low = b.bv_const(4, 0x5);
    auto byte = b.concat(high, low);
    auto extract_high = b.equal(b.extract(byte, 7, 4), high);
    auto extract_low = b.equal(b.extract(byte, 3, 0), low);
    auto shifted = b.equal(b.bv_lshr(b.bv_const(8, 0x80), 7), b.bv_const(8, 1));
    auto result = solver.check({extract_high, extract_low, shifted});
    require(result.status == smt::CheckStatus::Sat, "concat/extract/lshr constants should solve");
}

void test_resource_timeout() {
    smt::ExprBuilder b;
    smt::Solver solver;
    auto x = b.bv_var(8, "x");
    smt::SolverOptions options;
    options.max_sat_variables = 1;
    auto result = solver.check(b.bv_ult(x, b.bv_const(8, 128)), options);
    require(result.status == smt::CheckStatus::Timeout,
            "SAT variable budget should produce a deterministic timeout");
}

void test_proof_adapter_and_cache() {
    smt::ExprBuilder b;
    auto x = b.bv_var(32, "x");
    auto y = b.bv_var(32, "y");
    pass::smt::SMTObligation obligation;
    obligation.id = "unit.add_sub";
    obligation.stage = pass::cost_model::CostIRStage::OIR;
    obligation.timeout_us = 5000;
    obligation.estimated_cost_us = 1000;
    obligation.assertions.push_back(b.distinct(b.bv_add(b.bv_sub(x, y), y), x));
    obligation.formula = smt::to_string(obligation.assertions.front());

    pass::smt::SMTProofCache cache;
    auto proof = pass::smt::prove_obligation(
        obligation, pass::cost_model::ProofStatus::Unknown, "adapter proof", &cache);
    require(proof.kind == pass::cost_model::ProofKind::SMT, "adapter proof kind should be SMT");
    require(proof.status == pass::cost_model::ProofStatus::Proven,
            "adapter should map UNSAT counterexample to Proven");
    require(proof.solver_id == "yoolang-qfbv-smt", "adapter should report the in-process solver");

    auto cached = pass::smt::prove_obligation(
        obligation, pass::cost_model::ProofStatus::Unknown, "adapter proof", &cache);
    require(cached.status == pass::cost_model::ProofStatus::Proven,
            "cached proof should preserve Proven status");
    require(cached.solver_id == "yoolang-qfbv-smt-cache",
            "cached proof should report cache solver id");

    pass::smt::SMTObligation option_sensitive;
    option_sensitive.id = "unit.option_cache";
    option_sensitive.stage = pass::cost_model::CostIRStage::OIR;
    option_sensitive.timeout_us = 5000;
    option_sensitive.estimated_cost_us = 1000;
    option_sensitive.assertions.push_back(b.bv_ult(b.bv_var(8, "limit_x"), b.bv_const(8, 128)));
    option_sensitive.formula = smt::to_string(option_sensitive.assertions.front());

    pass::smt::SMTProofCache option_cache;
    auto default_options = pass::smt::prove_obligation(
        option_sensitive, pass::cost_model::ProofStatus::Unknown, "default options",
        &option_cache);
    require(default_options.status == pass::cost_model::ProofStatus::Refuted,
            "SAT comparison should be refuted with default solver options");

    auto limited_options_obligation = option_sensitive;
    limited_options_obligation.options.max_sat_variables = 1;
    auto limited_options = pass::smt::prove_obligation(
        limited_options_obligation, pass::cost_model::ProofStatus::Unknown, "limited options",
        &option_cache);
    require(limited_options.status == pass::cost_model::ProofStatus::Timeout,
            "cache key must distinguish solver resource options");
    require(limited_options.solver_id == "yoolang-qfbv-smt",
            "option-sensitive proof should not reuse the cached default-options result");

    auto colliding_left = b.bv_and(b.bv_var(8, "a b"), b.bv_var(8, "c"));
    auto colliding_right = b.bv_and(b.bv_var(8, "a"), b.bv_var(8, "b c"));
    require(smt::to_string(colliding_left) == smt::to_string(colliding_right),
            "regression setup should create a typed assertion render collision");

    pass::smt::SMTProofCache collision_cache;
    pass::smt::SMTObligation collision_proven;
    collision_proven.id = "unit.cache_collision";
    collision_proven.stage = pass::cost_model::CostIRStage::OIR;
    collision_proven.timeout_us = 5000;
    collision_proven.estimated_cost_us = 1000;
    collision_proven.assertions.push_back(b.distinct(colliding_left, colliding_left));
    collision_proven.formula = smt::to_string(collision_proven.assertions.front());
    auto collision_proof = pass::smt::prove_obligation(
        collision_proven, pass::cost_model::ProofStatus::Unknown, "cache collision proven",
        &collision_cache);
    require(collision_proof.status == pass::cost_model::ProofStatus::Proven,
            "x != x counterexample must be proven unsat");

    pass::smt::SMTObligation collision_refuted = collision_proven;
    collision_refuted.assertions = {b.distinct(colliding_left, colliding_right)};
    collision_refuted.formula = smt::to_string(collision_refuted.assertions.front());
    require(collision_refuted.formula == collision_proven.formula,
            "regression setup should keep the old string cache key identical");
    auto collision_refutation = pass::smt::prove_obligation(
        collision_refuted, pass::cost_model::ProofStatus::Unknown, "cache collision refuted",
        &collision_cache);
    require(collision_refutation.status == pass::cost_model::ProofStatus::Refuted,
            "structurally different typed assertions must not reuse stale cached proof");
    require(collision_refutation.solver_id == "yoolang-qfbv-smt",
            "render-colliding typed assertion should be solved, not returned from cache");

    auto z = b.bv_var(32, "z");
    pass::smt::SMTObligation refuted = obligation;
    refuted.id = "unit.refuted";
    refuted.assertions = {b.distinct(b.bv_add(b.bv_sub(x, y), z), x)};
    refuted.formula = smt::to_string(refuted.assertions.front());
    auto refuted_proof = pass::smt::prove_obligation(
        refuted, pass::cost_model::ProofStatus::Unknown, "adapter refutation", nullptr);
    require(refuted_proof.status == pass::cost_model::ProofStatus::Refuted,
            "adapter should map SAT counterexample to Refuted");

    pass::smt::SMTObligation timeout = obligation;
    timeout.id = "unit.timeout";
    timeout.timeout_us = 100;
    timeout.estimated_cost_us = 1000;
    auto timeout_proof = pass::smt::prove_obligation(
        timeout, pass::cost_model::ProofStatus::Unknown, "adapter timeout", nullptr);
    require(timeout_proof.status == pass::cost_model::ProofStatus::Timeout,
            "adapter should enforce deterministic admission-control timeout");

    pass::smt::SMTObligation unknown;
    unknown.id = "unit.unknown";
    auto unknown_proof = pass::smt::prove_obligation(
        unknown, pass::cost_model::ProofStatus::Unknown, "unsupported expression", nullptr);
    require(unknown_proof.status == pass::cost_model::ProofStatus::Unknown,
            "adapter should preserve Unknown for unsupported typed obligations");
}

} // namespace

int main() {
    test_boolean_sat_unsat();
    test_bv_add_sub_equivalence();
    test_linear_normalizer_rejects_textual_atom_collision();
    test_bv_refutation_model();
    test_signed_unsigned_distinction();
    test_model_for_modular_arithmetic();
    test_extract_concat_and_shifts();
    test_resource_timeout();
    test_proof_adapter_and_cache();
    return 0;
}
