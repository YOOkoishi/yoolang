#!/usr/bin/env python3
import argparse
import json
import os
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--pipeline-source", required=True)
    args = parser.parse_args()

    proc = subprocess.run(
        [args.compiler, "--emit-cost-model=json",
         "--cost-model-filter=ConstantArgumentSpecialization", "-O1",
         args.source],
        check=True, capture_output=True, text=True)
    decisions = json.loads(proc.stdout)["decisions"]
    accepted = [decision for decision in decisions
                if decision["action"] == "Accept"]
    members = [decision for decision in accepted
               if decision["scope"] ==
               "measured-persistent-reuse-member"]
    aggregates = [decision for decision in accepted
                  if decision["scope"] == "measured-persistent-reuse"]
    direct = [decision for decision in accepted
              if decision["scope"] == "direct-residual"]
    all_direct = [decision for decision in decisions
                  if decision["scope"] == "direct-residual"]
    direct_ties = [decision for decision in decisions
                   if decision["scope"] == "direct-residual-tie-batch"]

    if len(members) != 2:
        raise SystemExit(
            "expected only the two same-callee/same-binding calls to be "
            f"persistent members, found {len(members)}")
    fingerprints = [member["proof"]["rule_id"] for member in members]
    if len(set(fingerprints)) != 2 or any(
            not fingerprint.startswith("callsite.")
            for fingerprint in fingerprints):
        raise SystemExit(
            "different canonical call positions did not retain distinct "
            f"decision fingerprints: {fingerprints}")
    if len(aggregates) != 1:
        raise SystemExit(
            "expected exactly one persistent aggregate/clone for the two "
            f"positions, found {len(aggregates)}")
    aggregate = aggregates[0]
    if (not aggregate["proof"]["rule_id"].startswith("reuse.") or
            "one persistent clone" not in aggregate["proof"]["summary"] or
            aggregate["risk"]["code_growth"] != 6 or
            any(member["risk"]["code_growth"] != 6
                for member in members)):
        raise SystemExit(
            "persistent aggregate did not record one position-independent "
            f"clone growth: {aggregate}")
    if direct_ties:
        raise SystemExit(
            "different positions were incorrectly exposed as an atomic direct "
            f"tie: {[decision['proof']['rule_id'] for decision in direct_ties]}")
    # The four negative source sites are each deliberately eligible and reduced.
    # Seeing them as independent direct candidates, while the total persistent
    # member count remains exactly two, proves constant/mask/callee/dead-return
    # mismatches cannot enter the positive group.
    if len(all_direct) < 4 or len({decision["proof"]["rule_id"]
                                  for decision in all_direct}) < 4:
        raise SystemExit(
            "expected four independently handled negative candidates "
            f"(constant/mask/callee/dead-return), found {len(all_direct)}")

    fallback_env = os.environ.copy()
    fallback_env["YOOLANG_TEST_OIR_RESIDUAL_FAILURE"] = "tie-budget"
    fallback_env["YOOLANG_TEST_OIR_CALL_GROWTH_BUDGET"] = "100"
    fallback_proc = subprocess.run(
        [args.compiler, "--emit-cost-model=json",
         "--cost-model-filter=ConstantArgumentSpecialization", "-O1",
         args.source],
        check=True, capture_output=True, text=True, env=fallback_env)
    fallback_decisions = json.loads(fallback_proc.stdout)["decisions"]
    fallback_direct = [decision for decision in fallback_decisions
                       if decision["scope"] == "direct-residual" and
                       decision["action"] == "Accept"]
    fallback_group_scopes = [decision["scope"]
                             for decision in fallback_decisions
                             if decision["scope"] in (
                                 "measured-persistent-reuse",
                                 "measured-persistent-reuse-member",
                                 "direct-residual-tie-batch")]
    if fallback_group_scopes or len(fallback_direct) < 5:
        raise SystemExit(
            "a rejected persistent budget must fall back per decision position, "
            "not as a cross-position direct tie; "
            f"group_scopes={fallback_group_scopes}, "
            f"accepted_direct={len(fallback_direct)}")
    fallback_growth_sixes = 0
    for decision in fallback_direct:
        # This fixture's reduced residuals all have one return, so G is exactly
        # static instructions + branches + phis.  This catches both the old
        # byte-delta /16 risk and the former block-count commit unit.
        expected_growth = (decision["after"]["static_instrs"] +
                           decision["after"]["branches"] +
                           decision["after"]["phis"])
        if decision["risk"]["code_growth"] != expected_growth:
            raise SystemExit(
                "direct fallback risk did not use residual instruction growth "
                f"G: {decision}")
        if expected_growth == 6:
            fallback_growth_sixes += 1
    if fallback_growth_sixes < 2:
        raise SystemExit(
            "expected at least two independently charged G=6 direct fallbacks, "
            f"found {fallback_growth_sixes}")

    work_env = os.environ.copy()
    work_env["YOOLANG_TEST_OIR_RESIDUAL_FAILURE"] = "work-budget"
    work_proc = subprocess.run(
        [args.compiler, "--emit-cost-model=json",
         "--cost-model-filter=ConstantArgumentSpecialization", "-O1",
         args.source],
        check=True, capture_output=True, text=True, env=work_env)
    work_decisions = json.loads(work_proc.stdout)["decisions"]
    work_rejections = [decision for decision in work_decisions
                       if decision["scope"] ==
                       "specialization-work-budget" and
                       decision["action"] == "Reject" and
                       decision["reject_reason"] ==
                       "CompileTimeTooHigh"]
    if len(work_rejections) != 1 or len(work_decisions) != 1:
        raise SystemExit(
            "forced work exhaustion must emit exactly one diagnostic and the "
            "second specialization window must not retry: "
            f"{[(decision['scope'], decision['action'], decision['reject_reason']) for decision in work_decisions]}")

    def decisions_with_growth_budget(budget):
        env = os.environ.copy()
        env["YOOLANG_TEST_OIR_CALL_GROWTH_BUDGET"] = str(budget)
        result = subprocess.run(
            [args.compiler, "--emit-cost-model=json",
             "--cost-model-filter=ConstantArgumentSpecialization", "-O1",
             args.source],
            check=True, capture_output=True, text=True, env=env)
        return json.loads(result.stdout)["decisions"]

    exact_fit = decisions_with_growth_budget(6)
    one_short = decisions_with_growth_budget(5)
    if not any(decision["scope"] == "direct-residual" and
               decision["action"] == "Accept" and
               decision["risk"]["code_growth"] == 6
               for decision in exact_fit):
        raise SystemExit("growth budget 6 did not accept an exact-fit G=6 residual")
    if (any(decision["scope"] == "direct-residual" and
            decision["action"] == "Accept" and
            decision["risk"]["code_growth"] == 6
            for decision in one_short) or
            not any(decision["scope"] == "direct-residual" and
                    decision["reject_reason"] ==
                    "CumulativeBudgetExhausted" and
                    decision["risk"]["code_growth"] == 6
                    for decision in one_short)):
        raise SystemExit(
            "growth budget 5 did not reject the G=6 residual by exactly one unit")

    with open(args.pipeline_source, encoding="utf-8") as source_file:
        pipeline_source = source_file.read()
    if not re.search(
            r"constexpr\s+unsigned\s+kMaxRounds\s*=\s*12\s*;",
            pipeline_source):
        raise SystemExit("specialization pipeline fixed-point cap is not 12")

    print(json.dumps({
        "aggregate_clone_growth": aggregate["risk"]["code_growth"],
        "direct_candidates": len(direct),
        "exact_fit_growth": 6,
        "fallback_direct_candidates": len(fallback_direct),
        "fallback_growth_sixes": fallback_growth_sixes,
        "member_fingerprints": sorted(fingerprints),
        "negative_groups_excluded": 4,
        "pipeline_round_cap": 12,
        "work_budget_rejections": len(work_rejections),
    }, sort_keys=True))


if __name__ == "__main__":
    main()
