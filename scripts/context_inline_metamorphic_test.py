#!/usr/bin/env python3
import argparse
import json
import os
import subprocess


def structural_decisions(compiler, source, extra_env):
    env = os.environ.copy()
    env.update(extra_env)
    proc = subprocess.run(
        [compiler, "--emit-cost-model=json",
         "--cost-model-filter=ConstantArgumentSpecialization", "-O1", source],
        check=True, capture_output=True, text=True, env=env)
    decisions = json.loads(proc.stdout)["decisions"]
    return [d for d in decisions
            if d["proof"]["rule_id"].startswith("callsite.")]


def evidence(compiler, source, extra_env):
    structural = structural_decisions(compiler, source, extra_env)
    accepted = sorted(d["proof"]["rule_id"] for d in structural
                      if d["action"] == "Accept")
    rejected = sorted((d["proof"]["rule_id"], d["reject_reason"])
                      for d in structural if d["reject_reason"] != "None")
    fields = ("code_growth", "live_range_growth",
              "register_pressure_growth", "memory_pressure_growth")
    budgets = tuple(sum(d["risk"][field] for d in structural
                        if d["action"] == "Accept") for field in fields)
    if not accepted and not rejected:
        raise SystemExit(f"no structural callsite evidence for {source}")
    return accepted, rejected, budgets


def distinct_occurrence_evidence(compiler, source, expected_count):
    # This mode isolates the positional-key invariant from P12d's deliberately
    # tight cumulative-growth policy.  Otherwise ordinary inline growth between
    # the two specialization windows can leave a transformed, later callsite to
    # be reported as an unrelated budget rejection.
    structural = structural_decisions(
        compiler, source,
        {"YOOLANG_TEST_OIR_CALL_GROWTH_BUDGET": "1000"})
    fingerprints = [d["proof"]["rule_id"] for d in structural]
    if len(structural) != expected_count:
        raise SystemExit(
            f"expected {expected_count} structural callsites for {source}, "
            f"found {len(structural)}: {fingerprints}")
    if len(set(fingerprints)) != expected_count:
        raise SystemExit(
            "different same-block program positions collapsed to one fingerprint\n"
            f"structural fingerprints={fingerprints}")
    if any(d["scope"] != "direct-residual" for d in structural):
        raise SystemExit(
            "different same-block program positions were incorrectly handled as a tie\n"
            f"scopes={[d['scope'] for d in structural]}")
    print(json.dumps({"distinct_occurrence_fingerprints": sorted(fingerprints)},
                     sort_keys=True))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--lhs")
    parser.add_argument("--rhs")
    parser.add_argument("--rhs-permute", action="store_true")
    parser.add_argument("--distinct-occurrence-source")
    parser.add_argument("--expect-distinct-count", type=int)
    args = parser.parse_args()
    if args.distinct_occurrence_source is not None:
        if (args.lhs is not None or args.rhs is not None or args.rhs_permute or
                args.expect_distinct_count is None):
            parser.error("distinct-occurrence mode requires only its source and count")
        distinct_occurrence_evidence(
            args.compiler, args.distinct_occurrence_source,
            args.expect_distinct_count)
        return
    if (args.lhs is None or args.rhs is None or
            args.expect_distinct_count is not None):
        parser.error("metamorphic mode requires --lhs and --rhs")
    modes = ({}, {"YOOLANG_TEST_OIR_RESIDUAL_FAILURE": "tie-budget"})
    for mode in modes:
        lhs = evidence(args.compiler, args.lhs, mode)
        rhs_env = dict(mode)
        if args.rhs_permute:
            rhs_env["YOOLANG_TEST_OIR_CANONICAL_CONTAINER_PERMUTE"] = "1"
        rhs = evidence(args.compiler, args.rhs, rhs_env)
        if lhs != rhs:
            raise SystemExit(
                "metamorphic evidence differs\n"
                f"lhs accepted/rejected/budgets={lhs}\n"
                f"rhs accepted/rejected/budgets={rhs}")
        print(json.dumps({"accepted_structural_fingerprints": lhs[0],
                          "reject_reasons": lhs[1],
                          "consumed_budget_totals": lhs[2]}, sort_keys=True))


if __name__ == "__main__":
    main()
