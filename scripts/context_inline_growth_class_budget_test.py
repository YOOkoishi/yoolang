#!/usr/bin/env python3
import argparse
import json
import os
import re
import subprocess


def run_compiler(compiler, source, budget, option):
    env = os.environ.copy()
    env["YOOLANG_TEST_OIR_CALL_GROWTH_BUDGET"] = str(budget)
    env.pop("YOOLANG_TEST_OIR_RESIDUAL_FAILURE", None)
    return subprocess.run(
        [compiler, option, "-O1", source],
        check=True, capture_output=True, text=True, env=env).stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--source", required=True)
    args = parser.parse_args()

    exact_budget = 2
    exact_output = run_compiler(
        args.compiler, args.source, exact_budget, "--emit-cost-model=json")
    decisions = json.loads(exact_output)["decisions"]
    inline_decisions = [decision for decision in decisions
                        if decision["pass"] == "OIRInlinePass"]
    accepted = [decision for decision in inline_decisions
                if decision["action"] == "Accept"]
    ordinary = [decision for decision in accepted
                if decision["transform"] == "Inline" and
                decision["scope"] == "call"]
    residual = [decision for decision in accepted
                if decision["transform"] ==
                "ConstantArgumentSpecialization" and
                decision["scope"] == "direct-residual"]
    if len(accepted) != 2 or len(ordinary) != 1 or len(residual) != 1:
        raise SystemExit(
            "exact class budgets must accept only one ordinary wrapper inline "
            "and one direct residual: "
            f"{[(decision['transform'], decision['scope'], decision['action']) for decision in inline_decisions]}")

    ordinary_index = inline_decisions.index(ordinary[0])
    residual_index = inline_decisions.index(residual[0])
    first_window_rejections = [
        index for index, decision in enumerate(inline_decisions)
        if index < ordinary_index and
        decision["transform"] == "ConstantArgumentSpecialization" and
        decision["scope"] == "call" and
        decision["action"] == "Reject" and
        decision["reject_reason"] == "ProofUnknown"]
    if not first_window_rejections or ordinary_index >= residual_index:
        raise SystemExit(
            "expected first-window specialization rejection, then ordinary "
            "commit, then second-window direct residual acceptance")
    if residual[0]["risk"]["code_growth"] != exact_budget:
        raise SystemExit(
            "second-window residual did not consume the exact specialization "
            f"quota: {residual[0]}")

    class_text = (
        "ordinary nonrecursive inline class module/root growth quota exhausted")
    total_text = "total call module/root growth hard cap exhausted"
    combined_rejections = [decision for decision in inline_decisions
                           if decision["action"] == "Reject" and
                           decision["reject_reason"] ==
                           "CumulativeBudgetExhausted" and
                           class_text in decision["proof"]["summary"] and
                           total_text in decision["proof"]["summary"]]
    if not combined_rejections:
        raise SystemExit(
            "an oversized ordinary candidate did not report both class and "
            "total growth exhaustion")

    exact_oir = run_compiler(
        args.compiler, args.source, exact_budget, "--emit-oir")
    main_match = re.search(
        r"define i32 @main\(\) \{(?P<body>.*?)^\}", exact_oir,
        flags=re.MULTILINE | re.DOTALL)
    if main_match is None:
        raise SystemExit("exact-budget OIR has no main definition")
    exact_main = main_match.group("body")
    if ("call i32 @forward" in exact_main or
            "call i32 @choose" in exact_main or
            not re.search(r"add i32 .*?, 23", exact_main)):
        raise SystemExit(
            "exact budgets did not commit the wrapper inline and the "
            "second-window residual in main")

    one_short = exact_budget - 1
    short_output = run_compiler(
        args.compiler, args.source, one_short, "--emit-cost-model=json")
    short_decisions = [decision for decision in
                       json.loads(short_output)["decisions"]
                       if decision["pass"] == "OIRInlinePass"]
    if any(decision["action"] == "Accept" for decision in short_decisions):
        raise SystemExit(
            "one-short ordinary quota unexpectedly exposed or accepted the "
            "second-window residual")
    class_only_rejections = [
        decision for decision in short_decisions
        if decision["reject_reason"] == "CumulativeBudgetExhausted" and
        class_text in decision["proof"]["summary"] and
        total_text not in decision["proof"]["summary"]]
    if not class_only_rejections:
        raise SystemExit(
            "one-short wrapper did not prove a class-quota rejection while "
            "the three-class total cap still had room")
    short_oir = run_compiler(
        args.compiler, args.source, one_short, "--emit-oir")
    short_main_match = re.search(
        r"define i32 @main\(\) \{(?P<body>.*?)^\}", short_oir,
        flags=re.MULTILINE | re.DOTALL)
    if (short_main_match is None or
            "call i32 @forward" not in short_main_match.group("body")):
        raise SystemExit(
            "one-short ordinary rejection did not preserve the wrapper call")

    print(json.dumps({
        "accepted_scopes": [ordinary[0]["scope"], residual[0]["scope"]],
        "class_only_rejections": len(class_only_rejections),
        "combined_class_total_rejections": len(combined_rejections),
        "exact_budget": exact_budget,
        "first_window_rejections": len(first_window_rejections),
        "one_short_budget": one_short,
        "second_window_growth": residual[0]["risk"]["code_growth"],
    }, sort_keys=True))


if __name__ == "__main__":
    main()
