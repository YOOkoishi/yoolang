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


if __name__ == "__main__":
    unittest.main()
