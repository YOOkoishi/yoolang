from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPTS_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS_DIR))

import compare_perf_baseline as baseline_compare  # noqa: E402
import check_perf_regression  # noqa: E402
import generate_perf_report  # noqa: E402


def perf_row(
    elapsed: float,
    *,
    asm_hash: str | None = None,
    executable_hash: str | None = None,
    instructions: int | None = None,
    samples: list[float] | None = None,
) -> dict[str, object]:
    row: dict[str, object] = {
        "case": "test/performance/example.sy",
        "compiler": f"{elapsed:.4f}s",
        "gcc": f"{elapsed:.4f}s",
        "clang": f"{elapsed:.4f}s",
        "status": "OK",
        "instruction_counts": {"compiler": instructions},
        "timing_samples_sec": {"compiler": samples or []},
    }
    if asm_hash is not None:
        row["assembly_artifacts"] = {
            "compiler": {
                "sha256": asm_hash,
                "size_bytes": 42,
                "executable_sha256": executable_hash,
            }
        }
    return row


class ChangeClassificationTests(unittest.TestCase):
    def test_identical_assembly_overrides_wall_time_noise(self) -> None:
        current = perf_row(0.20, asm_hash="same", instructions=200)
        baseline = perf_row(0.10, asm_hash="same", instructions=100)
        status, _ = baseline_compare.classify_change(current, baseline, 0.20, 0.10)
        self.assertEqual(status, "NO_CODE_CHANGE")

    def test_identical_dynamic_count_neutralizes_missing_hash(self) -> None:
        current = perf_row(0.20, instructions=100)
        baseline = perf_row(0.10, instructions=100)
        status, _ = baseline_compare.classify_change(current, baseline, 0.20, 0.10)
        self.assertEqual(status, "NO_DYNAMIC_CHANGE")

    def test_changed_executable_does_not_use_matching_assembly_fallback(self) -> None:
        current = perf_row(0.20, asm_hash="same", executable_hash="current")
        baseline = perf_row(0.10, asm_hash="same", executable_hash="baseline")
        status, _ = baseline_compare.classify_change(current, baseline, 0.20, 0.10)
        self.assertEqual(status, "REGRESSION")

    def test_overlapping_sample_ranges_are_inconclusive(self) -> None:
        current = perf_row(0.12, asm_hash="current", samples=[0.08, 0.12, 0.14])
        baseline = perf_row(0.10, asm_hash="baseline", samples=[0.09, 0.10, 0.13])
        status, _ = baseline_compare.classify_change(current, baseline, 0.12, 0.10)
        self.assertEqual(status, "INCONCLUSIVE")

    def test_separated_sample_ranges_report_regression(self) -> None:
        current = perf_row(0.20, asm_hash="current", samples=[0.19, 0.20, 0.21])
        baseline = perf_row(0.10, asm_hash="baseline", samples=[0.09, 0.10, 0.11])
        status, _ = baseline_compare.classify_change(current, baseline, 0.20, 0.10)
        self.assertEqual(status, "REGRESSION")

    def test_three_percent_delta_is_measurable(self) -> None:
        current = perf_row(1.03, asm_hash="current")
        baseline = perf_row(1.00, asm_hash="baseline")
        status, _ = baseline_compare.classify_change(current, baseline, 1.03, 1.00)
        self.assertEqual(status, "REGRESSION")

    def test_delta_below_three_percent_is_inconclusive(self) -> None:
        current = perf_row(1.029, asm_hash="current")
        baseline = perf_row(1.00, asm_hash="baseline")
        status, _ = baseline_compare.classify_change(current, baseline, 1.029, 1.00)
        self.assertEqual(status, "INCONCLUSIVE")


class BaselineAggregationTests(unittest.TestCase):
    def test_no_code_change_is_neutral_in_aggregate(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            current_path = root / "current.json"
            baseline_path = root / "baseline.json"
            out_json = root / "delta.json"
            current_path.write_text(json.dumps({"rows": [perf_row(0.20, asm_hash="same")]}))
            baseline_path.write_text(json.dumps({"rows": [perf_row(0.10, asm_hash="same")]}))
            argv = [
                "compare_perf_baseline.py",
                "--current",
                str(current_path),
                "--baseline",
                str(baseline_path),
                "--out-md",
                str(root / "delta.md"),
                "--out-json",
                str(out_json),
            ]
            with mock.patch.object(sys, "argv", argv):
                self.assertEqual(baseline_compare.main(), 0)

            result = json.loads(out_json.read_text())
            self.assertEqual(result["status"], "NO_CODE_CHANGE")
            self.assertEqual(result["total_delta_pct"], 0.0)
            self.assertEqual(result["raw_current_compiler_total"], 0.2)
            self.assertEqual(result["rows"][0]["observed_delta_pct"], -100.0)
            self.assertEqual(result["rows"][0]["delta_pct"], 0.0)

    def test_report_payload_uses_evidence_status_for_direction(self) -> None:
        perf = {"status": "PASS", "rows": [perf_row(0.08)]}
        delta = {
            "rows": [
                {
                    "case": "test/performance/example.sy",
                    "baseline": 0.10,
                    "delta_pct": 20.0,
                    "speedup": 1.25,
                    "status": "IMPROVEMENT",
                }
            ]
        }
        payload = generate_perf_report.build_payload(perf, delta, {})
        self.assertEqual(payload["summary"]["baseline_improvements"], 1)
        self.assertEqual(payload["summary"]["baseline_regressions"], 0)


class PerformanceReportHtmlTests(unittest.TestCase):
    def test_baseline_evidence_statuses_are_visible_and_filterable(self) -> None:
        payload = {
            "rows": [
                {
                    "case": "test/performance/example.sy",
                    "baseline_status": "NO_CODE_CHANGE",
                    "baseline_evidence": "executed ELF SHA-256 is identical",
                    "status": "OK",
                }
            ],
            "summary": {},
            "meta": {},
            "baseline": {},
        }
        with tempfile.TemporaryDirectory() as tmp:
            out_html = Path(tmp) / "index.html"
            generate_perf_report.write_html(payload, out_html)
            html = out_html.read_text()

        self.assertIn("基线判定", html)
        self.assertIn("NO_CODE_CHANGE", html)
        self.assertIn('value="no-code-change"', html)
        self.assertIn('value="no-dynamic-change"', html)
        self.assertIn('value="inconclusive"', html)


class PerformanceRegressionGateTests(unittest.TestCase):
    def test_non_blocking_statuses_pass(self) -> None:
        for status in check_perf_regression.NON_BLOCKING_STATUSES:
            with self.subTest(status=status):
                self.assertEqual(
                    check_perf_regression.regression_reasons(
                        {"status": status, "regressions": []}
                    ),
                    [],
                )

    def test_overall_regression_fails(self) -> None:
        reasons = check_perf_regression.regression_reasons(
            {"status": "REGRESSION", "total_delta_pct": -12.5, "regressions": []}
        )
        self.assertEqual(len(reasons), 1)
        self.assertIn("-12.50%", reasons[0])

    def test_important_case_regression_fails_even_when_total_is_ok(self) -> None:
        reasons = check_perf_regression.regression_reasons(
            {
                "status": "OK",
                "regressions": [
                    {
                        "case": "test/performance/example.sy",
                        "observed_delta_pct": -25.0,
                        "observed_delta_sec": 0.08,
                    }
                ],
            }
        )
        self.assertEqual(len(reasons), 1)
        self.assertIn("test/performance/example.sy", reasons[0])

    def test_unrecognized_status_fails_closed(self) -> None:
        reasons = check_perf_regression.regression_reasons(
            {"status": "UNKNOWN", "regressions": []}
        )
        self.assertEqual(len(reasons), 1)
        self.assertIn("unrecognized", reasons[0])


if __name__ == "__main__":
    unittest.main()
