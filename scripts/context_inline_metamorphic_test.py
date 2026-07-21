#!/usr/bin/env python3
import argparse
import json
import os
import subprocess


def evidence(compiler, source, extra_env):
    env = os.environ.copy()
    env.update(extra_env)
    proc = subprocess.run(
        [compiler, "--emit-cost-model=json",
         "--cost-model-filter=ConstantArgumentSpecialization", "-O1", source],
        check=True, capture_output=True, text=True, env=env)
    decisions = json.loads(proc.stdout)["decisions"]
    structural = [d for d in decisions
                  if d["proof"]["rule_id"].startswith("callsite.")]
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--lhs", required=True)
    parser.add_argument("--rhs", required=True)
    parser.add_argument("--rhs-permute", action="store_true")
    args = parser.parse_args()
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
