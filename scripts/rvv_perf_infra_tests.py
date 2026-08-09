#!/usr/bin/env python3

"""Pure-host tests for the native RVV performance gate.

No compiler, benchmark, perf event, QEMU instance, or hardware counter is run by
this file.  All hardware observations below are synthetic policy fixtures.
"""

from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

import perf_common  # noqa: E402
import rvv_hardware_perf as gate  # noqa: E402


MANIFEST_PATH = ROOT / "test/performance/rvv_hardware_manifest.json"


def eligible_environment(*, vector_execution: bool = True) -> dict[str, object]:
    return {
        "machine": "riscv64",
        "cpu": 3,
        "affinity": [3],
        "isa": ["rv64imafdcv_zicsr"],
        "v_extension": True,
        "emulation_tokens": [],
        "vector_control": {"status": "on", "raw": 2},
        "vector_execution_verified": vector_execution,
        "frequency_initial": {
            "available": True,
            "governor": "userspace",
            "driver": "test",
            "scaling_min_khz": 1200000,
            "scaling_max_khz": 1200000,
            "current_khz": 1200000,
        },
        "frequency_samples_khz": [1200000, 1200000, 1200000],
    }


def failure_codes(findings: list[dict[str, str]]) -> set[str]:
    return {finding["code"] for finding in findings}


def synthetic_results(
    manifest: dict[str, object],
    *,
    speed_overrides: dict[str, float] | None = None,
    compile_overrides: dict[str, float] | None = None,
) -> list[dict[str, object]]:
    speeds = speed_overrides or {}
    compile_ratios = compile_overrides or {}
    rows: list[dict[str, object]] = []
    for kernel in manifest["kernels"]:  # type: ignore[index]
        kernel_id = kernel["id"]
        classification = kernel["classification"]
        tags = list(kernel["tags"])
        if classification == gate.NEGATIVE_CONTROL:
            default_speed = 1.0
        elif "unit-stride" in tags:
            default_speed = 1.60
        else:
            default_speed = 1.20
        speedup = speeds.get(kernel_id, default_speed)
        scalar_cycles = 1_600_000.0
        rvv_cycles = scalar_cycles / speedup
        scalar_compile = 1.0
        rvv_compile = scalar_compile * compile_ratios.get(kernel_id, 1.05)

        def variant(
            cycles: float, compile_time: float, rvv_opcodes: int, text_size: int
        ) -> dict[str, object]:
            perf_sample = {
                "cycles": cycles,
                "instructions": cycles * 0.8,
                "ipc": 0.8,
                "wall_time_sec": cycles / 1_200_000_000.0,
                "controller_wall_time_sec": cycles / 1_200_000_000.0,
            }
            return {
                "compile": {
                    "status": "OK",
                    "median_sec": compile_time,
                    "samples_sec": [compile_time] * 5,
                },
                "binary": {
                    "text_size_bytes": text_size,
                    "rvv_opcode_count": rvv_opcodes,
                    "vsetvl_count": 2 if rvv_opcodes else 0,
                    "spill_like_count": 0,
                    "vlenb_bytes": 32,
                    "vr_spill_store_sites": 0,
                    "vr_spill_reload_sites": 0,
                    "vr_spill_store_vlenb_units": 0,
                    "vr_spill_reload_vlenb_units": 0,
                    "vr_spill_store_bytes": 0,
                    "vr_spill_reload_bytes": 0,
                    "vr_spill_transfer_bytes": 0,
                },
                "vectorization": {
                    "status": "OK",
                    "vectorized_count": 1 if rvv_opcodes else 0,
                    "loop_vectorized_count": 1 if rvv_opcodes else 0,
                    "rejected_count": 0,
                    "plans": (
                        [{"code": "VECTORIZED", "vectorizer": "loop"}]
                        if rvv_opcodes
                        else []
                    ),
                },
                "register_allocation": {
                    "status": "OK",
                    "vlenb_bytes": 32,
                    "spill_slot_count": 0,
                    "callee_saved_slot_count": 0,
                    "spill_slot_bytes": 0,
                    "callee_saved_slot_bytes": 0,
                    "whole_spill_sites": 0,
                    "whole_reload_sites": 0,
                    "functions": {},
                },
                "performance": {
                    "samples": [copy.deepcopy(perf_sample) for _ in range(11)],
                    "median_cycles": cycles,
                    "median_instructions": cycles * 0.8,
                    "median_ipc": 0.8,
                    "median_wall_time_sec": cycles / 1_200_000_000.0,
                    "relative_mad_cycles": 0.01,
                },
            }

        rvv_opcodes = 20 if classification == gate.EXPECTED_VECTORIZABLE else 0
        rows.append(
            {
                "id": kernel_id,
                "source": kernel["source"],
                "classification": classification,
                "minimum_verified_vectorized_loops": kernel.get(
                    "minimum_verified_vectorized_loops"
                ),
                "dedupe_group": kernel["dedupe_group"],
                "tags": tags,
                "hotspot": kernel["hotspot"],
                "status": "OK",
                "scalar": variant(scalar_cycles, scalar_compile, 0, 1000),
                "rvv": variant(rvv_cycles, rvv_compile, rvv_opcodes, 1200),
                "speedup_cycles": speedup,
                "slowdown_fraction_cycles": 1.0 / speedup - 1.0,
            }
        )
    return rows


class CommonCompatibilityTests(unittest.TestCase):
    def test_expected_output_exit_code_convention(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rvv-perf-common-") as temporary:
            source = Path(temporary) / "case.sy"
            source.write_text("int main(){return 0;}\n")
            source.with_suffix(".out").write_text("answer\r\n7\r\n")
            self.assertEqual(perf_common.read_sysy_expected_output(source), ("answer", 7))

    def test_positive_geomean_matches_historical_behavior(self) -> None:
        self.assertAlmostEqual(perf_common.positive_geomean([4.0, 1.0, 0.0]), 2.0)
        self.assertIsNone(perf_common.positive_geomean([0.0, -1.0]))


class ManifestTests(unittest.TestCase):
    def test_release_manifest_is_complete_and_resolves(self) -> None:
        manifest = gate.load_manifest(MANIFEST_PATH, ROOT)
        self.assertEqual(manifest["thresholds"], gate.RELEASE_THRESHOLDS)
        classifications = {kernel["classification"] for kernel in manifest["kernels"]}
        self.assertEqual(
            classifications, {gate.EXPECTED_VECTORIZABLE, gate.NEGATIVE_CONTROL}
        )
        self.assertTrue(
            any("unit-stride" in kernel["tags"] for kernel in manifest["kernels"])
        )
        self.assertTrue(
            any("pointer-chasing" in kernel["tags"] for kernel in manifest["kernels"])
        )
        self.assertIn("--emit-vector-plan", manifest["build"]["rvv"]["vector_plan_args"])
        self.assertIn("--emit-mir", manifest["build"]["rvv"]["post_ra_mir_args"])
        self.assertTrue(
            all(
                kernel["minimum_verified_vectorized_loops"] >= 1
                for kernel in manifest["kernels"]
                if kernel["classification"] == gate.EXPECTED_VECTORIZABLE
            )
        )

    def test_weakened_threshold_is_rejected(self) -> None:
        payload = json.loads(MANIFEST_PATH.read_text())
        payload["thresholds"]["unit_stride_geomean_min_speedup"] = 1.49
        with tempfile.TemporaryDirectory(prefix="rvv-perf-manifest-") as temporary:
            candidate = Path(temporary) / "manifest.json"
            candidate.write_text(json.dumps(payload))
            with self.assertRaises(gate.GateError) as raised:
                gate.load_manifest(candidate, ROOT)
        self.assertEqual(raised.exception.code, "MANIFEST_THRESHOLD_WEAKENED")

    def test_v1_manifest_without_required_evidence_contract_is_rejected(self) -> None:
        payload = json.loads(MANIFEST_PATH.read_text())
        payload["schema_version"] = 1
        payload["build"]["rvv"].pop("vector_plan_args")
        with tempfile.TemporaryDirectory(prefix="rvv-perf-manifest-") as temporary:
            candidate = Path(temporary) / "manifest.json"
            candidate.write_text(json.dumps(payload))
            with self.assertRaises(gate.GateError) as raised:
                gate.load_manifest(candidate, ROOT)
        self.assertEqual(raised.exception.code, "MANIFEST_SCHEMA_UNSUPPORTED")

    def test_v2_manifest_without_loop_minimum_contract_is_rejected(self) -> None:
        payload = json.loads(MANIFEST_PATH.read_text())
        payload["schema_version"] = 2
        for kernel in payload["kernels"]:
            kernel.pop("minimum_verified_vectorized_loops", None)
        with tempfile.TemporaryDirectory(prefix="rvv-perf-manifest-") as temporary:
            candidate = Path(temporary) / "manifest.json"
            candidate.write_text(json.dumps(payload))
            with self.assertRaises(gate.GateError) as raised:
                gate.load_manifest(candidate, ROOT)
        self.assertEqual(raised.exception.code, "MANIFEST_SCHEMA_UNSUPPORTED")

    def test_weakened_measurement_protocol_is_rejected(self) -> None:
        payload = json.loads(MANIFEST_PATH.read_text())
        payload["protocol"]["minimum_repetitions"] = 10
        with tempfile.TemporaryDirectory(prefix="rvv-perf-manifest-") as temporary:
            candidate = Path(temporary) / "manifest.json"
            candidate.write_text(json.dumps(payload))
            with self.assertRaises(gate.GateError) as raised:
                gate.load_manifest(candidate, ROOT)
        self.assertEqual(raised.exception.code, "MANIFEST_PROTOCOL_WEAKENED")

    def test_path_escape_is_rejected(self) -> None:
        payload = json.loads(MANIFEST_PATH.read_text())
        payload["kernels"][0]["source"] = "../outside.sy"
        with tempfile.TemporaryDirectory(prefix="rvv-perf-manifest-") as temporary:
            candidate = Path(temporary) / "manifest.json"
            candidate.write_text(json.dumps(payload))
            with self.assertRaises(gate.GateError) as raised:
                gate.load_manifest(candidate, ROOT)
        self.assertEqual(raised.exception.code, "MANIFEST_PATH_ESCAPE")

    def test_expected_loop_minimum_is_required_and_fail_closed(self) -> None:
        for invalid in (None, 0, True):
            payload = json.loads(MANIFEST_PATH.read_text())
            if invalid is None:
                payload["kernels"][0].pop("minimum_verified_vectorized_loops")
            else:
                payload["kernels"][0]["minimum_verified_vectorized_loops"] = invalid
            with tempfile.TemporaryDirectory(prefix="rvv-perf-manifest-") as temporary:
                candidate = Path(temporary) / "manifest.json"
                candidate.write_text(json.dumps(payload))
                with self.assertRaises(gate.GateError) as raised:
                    gate.load_manifest(candidate, ROOT)
            self.assertEqual(raised.exception.code, "MANIFEST_VECTOR_PLAN_MINIMUM")


class PerfAndObjdumpParserTests(unittest.TestCase):
    def test_perf_stat_cycles_instructions_ipc_and_wall(self) -> None:
        parsed = gate.parse_perf_stat(
            "1200000;;cycles;100.00;100.00\n"
            "900000;;instructions;100.00;100.00\n"
            "0.001250;;;seconds time elapsed\n"
        )
        self.assertEqual(parsed["cycles"], 1200000.0)
        self.assertEqual(parsed["instructions"], 900000.0)
        self.assertAlmostEqual(parsed["ipc"], 0.75)
        self.assertAlmostEqual(parsed["wall_time_sec"], 0.00125)

    def test_perf_stat_missing_or_unavailable_counter_fails_closed(self) -> None:
        with self.assertRaises(gate.GateError) as unavailable:
            gate.parse_perf_stat(
                "<not counted>;;cycles;0;0\n"
                "10;;instructions;100;100\n"
                "0.01;;;seconds time elapsed\n"
            )
        self.assertEqual(unavailable.exception.code, "PERF_COUNTER_UNAVAILABLE")
        with self.assertRaises(gate.GateError) as missing_wall:
            gate.parse_perf_stat("100;;cycles\n80;;instructions\n")
        self.assertEqual(missing_wall.exception.code, "PERF_WALL_MISSING")

    def test_objdump_parser_counts_rvv_shape_and_exact_whole_spill_bytes(self) -> None:
        parsed = gate.parse_objdump(
            "  1000: 0d007057  vsetvli a0,a0,e32,m1,ta,ma\n"
            "  1004: 02056087  vle32.v v1,(a0)\n"
            "  1008: 9620f157  vadd.vv v2,v1,v1\n"
            "  100c: 6a20a257  vmslt.vv v4,v2,v1\n"
            "  1010: 02016127  vse32.v v2,(sp)\n"
            "  1014: 22830427  vs8r.v v8,(t6)\n"
            "  1018: 2e83e407  vl8re32.v v8,(t6)\n"
            "  101c: 00008067  ret\n",
            vlenb_bytes=32,
        )
        self.assertEqual(parsed["rvv_opcode_count"], 7)
        self.assertEqual(parsed["vsetvl_count"], 1)
        self.assertEqual(parsed["vector_load_count"], 2)
        self.assertEqual(parsed["vector_store_count"], 2)
        self.assertEqual(parsed["mask_count"], 1)
        self.assertEqual(parsed["spill_like_count"], 1)
        self.assertEqual(parsed["vr_spill_store_sites"], 1)
        self.assertEqual(parsed["vr_spill_reload_sites"], 1)
        self.assertEqual(parsed["vr_spill_store_vlenb_units"], 8)
        self.assertEqual(parsed["vr_spill_reload_vlenb_units"], 8)
        self.assertEqual(parsed["vr_spill_store_bytes"], 256)
        self.assertEqual(parsed["vr_spill_reload_bytes"], 256)
        self.assertEqual(parsed["vr_spill_transfer_bytes"], 512)
        self.assertEqual(parsed["sew_lmul_distribution"], {"e32,m1": 1})

    def test_vlenb_and_vector_plan_parsers_fail_closed(self) -> None:
        self.assertEqual(gate.parse_vlenb_output("32\n"), 32)
        for invalid in ("", "15", "24", "8193", "unknown"):
            with self.assertRaises(gate.GateError):
                gate.parse_vlenb_output(invalid)

        parsed = gate.parse_vectorization_plan(
            json.dumps(
                {
                    "vectorization_plans": [
                        {
                            "vectorizer": "loop",
                            "code": "VECTORIZED",
                            "function": "kernel",
                            "region": "loop.header",
                            "explanation": "verified VLA transform",
                            "plan": {"lmul": "m2"},
                        },
                        {
                            "vectorizer": "slp",
                            "code": "REJECT_ALIAS",
                            "function": "kernel",
                            "region": "entry",
                            "explanation": "unknown alias",
                            "plan": {"lmul": "m1"},
                        },
                    ]
                }
            )
        )
        self.assertEqual(parsed["vectorized_count"], 1)
        self.assertEqual(parsed["loop_vectorized_count"], 1)
        self.assertEqual(parsed["rejected_count"], 1)
        self.assertEqual(
            parsed["code_counts"], {"REJECT_ALIAS": 1, "VECTORIZED": 1}
        )
        with self.assertRaises(gate.GateError) as raised:
            gate.parse_vectorization_plan('{"vectorization_plans":[{}]}')
        self.assertEqual(raised.exception.code, "VECTOR_PLAN_SCHEMA_INVALID")

    def test_post_ra_mir_parser_counts_unique_spill_slots_and_sites(self) -> None:
        parsed = gate.parse_post_ra_mir_spills(
            "func @kernel() -> i32 {\n"
            "  frame align=16 size=0 outgoing=0 scalable=vlenb*9\n"
            "  frame objects:\n"
            "    fi#0 rvv.spill.v4 aggregate size=vlenb*8 align=vlenb*8 "
            "offset=scalable+0 kind=spill\n"
            "    fi#1 psabi.save.v1 aggregate size=vlenb align=vlenb "
            "offset=scalable+vlenb*8 kind=callee-saved\n"
            "entry:\n"
            "  PseudoVSPILL_WHOLE fi#0, v8, (implicit-use vlenb)\n"
            "  PseudoVRELOAD_WHOLE v8, fi#0, (implicit-use vlenb)\n"
            "  PseudoVSPILL_WHOLE fi#1, v1, (implicit-use vlenb)\n"
            "  PseudoVRELOAD_WHOLE v1, fi#1, (implicit-use vlenb)\n"
            "}\n",
            vlenb_bytes=32,
        )
        self.assertEqual(parsed["spill_slot_count"], 1)
        self.assertEqual(parsed["callee_saved_slot_count"], 1)
        self.assertEqual(parsed["spill_slot_bytes"], 256)
        self.assertEqual(parsed["callee_saved_slot_bytes"], 32)
        self.assertEqual(parsed["whole_spill_sites"], 2)
        self.assertEqual(parsed["whole_reload_sites"], 2)
        self.assertEqual(parsed["functions"]["kernel"]["spill_slot_bytes"], 256)
        self.assertEqual(gate.parse_scalable_size_eighths("vlenb*4/8"), 4)

        with self.assertRaises(gate.GateError) as raised:
            gate.parse_post_ra_mir_spills(
                "func @bad() -> i32 {\n"
                "entry:\n"
                "  PseudoVSPILL_WHOLE fi#9, v8, (implicit-use vlenb)\n"
                "}\n",
                vlenb_bytes=32,
            )
        self.assertEqual(raised.exception.code, "MIR_SPILL_SCHEMA_INVALID")


class EnvironmentPolicyTests(unittest.TestCase):
    def test_eligible_synthetic_native_environment_passes(self) -> None:
        self.assertEqual(
            gate.environment_failures(
                eligible_environment(), "require-fixed", require_vector_execution=True
            ),
            [],
        )

    def test_non_riscv64_is_rejected(self) -> None:
        snapshot = eligible_environment()
        snapshot["machine"] = "x86_64"
        codes = failure_codes(
            gate.environment_failures(
                snapshot, "require-fixed", require_vector_execution=True
            )
        )
        self.assertIn("ENV_NOT_RISCV64", codes)

    def test_qemu_is_rejected_even_when_isa_has_v(self) -> None:
        snapshot = eligible_environment()
        snapshot["emulation_tokens"] = ["qemu", "tcg"]
        codes = failure_codes(
            gate.environment_failures(
                snapshot, "require-fixed", require_vector_execution=True
            )
        )
        self.assertIn("ENV_EMULATED", codes)

    def test_missing_v_and_disabled_vector_state_are_rejected(self) -> None:
        no_v = eligible_environment()
        no_v["v_extension"] = False
        self.assertIn(
            "ENV_NO_V_EXTENSION",
            failure_codes(
                gate.environment_failures(
                    no_v, "require-fixed", require_vector_execution=True
                )
            ),
        )
        state_off = eligible_environment()
        state_off["vector_control"] = {"status": "off", "raw": 1}
        self.assertIn(
            "ENV_VECTOR_STATE_OFF",
            failure_codes(
                gate.environment_failures(
                    state_off, "require-fixed", require_vector_execution=True
                )
            ),
        )

    def test_vector_execution_probe_is_required_for_final_result(self) -> None:
        snapshot = eligible_environment(vector_execution=False)
        final_codes = failure_codes(
            gate.environment_failures(
                snapshot, "require-fixed", require_vector_execution=True
            )
        )
        preflight_codes = failure_codes(
            gate.environment_failures(
                snapshot, "require-fixed", require_vector_execution=False
            )
        )
        self.assertIn("ENV_VECTOR_EXECUTION_UNVERIFIED", final_codes)
        self.assertNotIn("ENV_VECTOR_EXECUTION_UNVERIFIED", preflight_codes)

    def test_frequency_must_be_fixed_and_record_mode_is_not_official(self) -> None:
        variable = eligible_environment()
        variable["frequency_initial"]["scaling_max_khz"] = 1800000
        self.assertIn(
            "ENV_CPUFREQ_NOT_FIXED",
            failure_codes(
                gate.environment_failures(
                    variable, "require-fixed", require_vector_execution=True
                )
            ),
        )
        record_findings = gate.environment_failures(
            eligible_environment(), "record", require_vector_execution=True
        )
        self.assertIn("ENV_FREQUENCY_NOT_FIXED", failure_codes(record_findings))
        self.assertEqual(gate.preflight_blocking_failures(record_findings, "record"), [])

    def test_record_mode_does_not_hide_non_frequency_environment_failures(self) -> None:
        snapshot = eligible_environment()
        snapshot["emulation_tokens"] = ["qemu"]
        findings = gate.environment_failures(
            snapshot, "record", require_vector_execution=False
        )
        self.assertEqual(
            failure_codes(gate.preflight_blocking_failures(findings, "record")),
            {"ENV_EMULATED"},
        )


class ThresholdPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = gate.load_manifest(MANIFEST_PATH, ROOT)

    def evaluate(self, rows: list[dict[str, object]]) -> set[str]:
        _, findings = gate.evaluate_release_gate(self.manifest, rows)
        return failure_codes(findings)

    def test_all_release_thresholds_pass_with_complete_evidence(self) -> None:
        aggregates, findings = gate.evaluate_release_gate(
            self.manifest, synthetic_results(self.manifest)
        )
        self.assertEqual(failure_codes(findings), set())
        self.assertTrue(
            all(
                evaluation["passed"]
                for evaluation in aggregates["threshold_evaluations"].values()
            )
        )

    def test_unit_stride_1_5x_threshold(self) -> None:
        overrides = {
            kernel["id"]: 1.49
            for kernel in self.manifest["kernels"]
            if "unit-stride" in kernel["tags"]
        }
        self.assertIn(
            "UNIT_STRIDE_SPEEDUP",
            self.evaluate(synthetic_results(self.manifest, speed_overrides=overrides)),
        )

    def test_deduplicated_vector_corpus_1_15x_threshold(self) -> None:
        overrides = {
            kernel["id"]: (1.50 if "unit-stride" in kernel["tags"] else 0.50)
            for kernel in self.manifest["kernels"]
            if kernel["classification"] == gate.EXPECTED_VECTORIZABLE
        }
        self.assertIn(
            "VECTOR_CORPUS_SPEEDUP",
            self.evaluate(synthetic_results(self.manifest, speed_overrides=overrides)),
        )

    def test_vectorized_hotspot_over_5_percent_slowdown_fails(self) -> None:
        hotspot = next(
            kernel
            for kernel in self.manifest["kernels"]
            if kernel["classification"] == gate.EXPECTED_VECTORIZABLE and kernel["hotspot"]
        )
        codes = self.evaluate(
            synthetic_results(
                self.manifest, speed_overrides={hotspot["id"]: 1.0 / 1.051}
            )
        )
        self.assertIn("HOTSPOT_SLOWDOWN", codes)

    def test_negative_control_2_percent_geomean_limit(self) -> None:
        overrides = {
            kernel["id"]: 1.0 / 1.021
            for kernel in self.manifest["kernels"]
            if kernel["classification"] == gate.NEGATIVE_CONTROL
        }
        self.assertIn(
            "NEGATIVE_CONTROL_REGRESSION",
            self.evaluate(synthetic_results(self.manifest, speed_overrides=overrides)),
        )

    def test_compile_time_10_percent_geomean_limit(self) -> None:
        overrides = {kernel["id"]: 1.101 for kernel in self.manifest["kernels"]}
        self.assertIn(
            "COMPILE_TIME_REGRESSION",
            self.evaluate(synthetic_results(self.manifest, compile_overrides=overrides)),
        )

    def test_exact_negative_and_compile_boundaries_are_inclusive(self) -> None:
        speed_overrides = {
            kernel["id"]: 1.0 / 1.02
            for kernel in self.manifest["kernels"]
            if kernel["classification"] == gate.NEGATIVE_CONTROL
        }
        compile_overrides = {kernel["id"]: 1.10 for kernel in self.manifest["kernels"]}
        codes = self.evaluate(
            synthetic_results(
                self.manifest,
                speed_overrides=speed_overrides,
                compile_overrides=compile_overrides,
            )
        )
        self.assertNotIn("NEGATIVE_CONTROL_REGRESSION", codes)
        self.assertNotIn("COMPILE_TIME_REGRESSION", codes)

    def test_missing_metrics_opcodes_and_kernel_fail_closed(self) -> None:
        rows = synthetic_results(self.manifest)
        rows[0]["rvv"]["compile"]["median_sec"] = None
        rows[1]["rvv"]["binary"]["rvv_opcode_count"] = 0
        rows[2]["rvv"]["binary"]["vr_spill_transfer_bytes"] = None
        rows[3]["rvv"]["vectorization"] = {"status": "UNAVAILABLE"}
        rows[4]["rvv"]["register_allocation"] = {"status": "UNAVAILABLE"}
        rows.pop()
        codes = self.evaluate(rows)
        self.assertIn("COMPILE_TIME_MISSING", codes)
        self.assertIn("EXPECTED_RVV_OPCODE_MISSING", codes)
        self.assertIn("VR_SPILL_BYTES_MISSING", codes)
        self.assertIn("VECTOR_PLAN_EVIDENCE_MISSING", codes)
        self.assertIn("MIR_SPILL_EVIDENCE_MISSING", codes)
        self.assertIn("KERNEL_RESULTS_MISSING", codes)

    def test_unstable_samples_fail_closed(self) -> None:
        rows = synthetic_results(self.manifest)
        rows[0]["rvv"]["performance"]["relative_mad_cycles"] = 0.051
        self.assertIn("KERNEL_SAMPLES_UNSTABLE", self.evaluate(rows))

    def test_expected_kernel_requires_verified_vectorized_plan(self) -> None:
        rows = synthetic_results(self.manifest)
        row = next(
            item
            for item in rows
            if item["classification"] == gate.EXPECTED_VECTORIZABLE
        )
        row["rvv"]["vectorization"] = {
            "status": "OK",
            "vectorized_count": 0,
            "loop_vectorized_count": 0,
            "rejected_count": 1,
            "plans": [{"code": "REJECT_ALIAS", "vectorizer": "loop"}],
        }
        self.assertIn("EXPECTED_VECTORIZED_PLAN_MISSING", self.evaluate(rows))

    def test_slp_success_does_not_satisfy_loop_minimum(self) -> None:
        rows = synthetic_results(self.manifest)
        row = next(
            item
            for item in rows
            if item["classification"] == gate.EXPECTED_VECTORIZABLE
        )
        row["rvv"]["vectorization"] = {
            "status": "OK",
            "vectorized_count": 1,
            "loop_vectorized_count": 0,
            "rejected_count": 0,
            "plans": [{"code": "VECTORIZED", "vectorizer": "slp"}],
        }
        self.assertIn("EXPECTED_VECTORIZED_PLAN_MISSING", self.evaluate(rows))


class ReportTests(unittest.TestCase):
    def test_blocked_report_says_gate_not_run(self) -> None:
        report = {
            "status": "BLOCKED",
            "official": False,
            "gate_executed": False,
            "generated_utc": "2026-08-16T00:00:00+00:00",
            "configuration": {"frequency_policy": "require-fixed"},
            "environment": {"machine": "x86_64"},
            "failures": [
                {"code": "ENV_NOT_RISCV64", "detail": "machine is not riscv64"}
            ],
            "kernels": [],
        }
        markdown = gate.render_markdown(report)
        self.assertIn("formal RVV hardware gate was not run", markdown)
        self.assertIn("Official hardware result: **NO**", markdown)
        self.assertIn("QEMU timing accepted: **NO**", markdown)

    def test_markdown_exposes_exact_spill_bytes_and_loop_reasons(self) -> None:
        report = {
            "status": "PASS",
            "official": True,
            "gate_executed": True,
            "generated_utc": "2026-08-16T00:00:00+00:00",
            "configuration": {"frequency_policy": "require-fixed"},
            "environment": {
                "machine": "riscv64",
                "vlen_bits": 256,
                "vlenb_bytes": 32,
            },
            "failures": [],
            "kernels": synthetic_results(
                gate.load_manifest(MANIFEST_PATH, ROOT)
            )[:1],
        }
        row = report["kernels"][0]
        row["rvv"]["binary"]["vr_spill_store_bytes"] = 256
        row["rvv"]["binary"]["vr_spill_reload_bytes"] = 512
        row["rvv"]["register_allocation"]["spill_slot_bytes"] = 256
        row["rvv"]["vectorization"]["plans"] = [
            {
                "vectorizer": "loop",
                "code": "VECTORIZED",
                "function": "kernel",
                "region": "loop.header",
                "explanation": "verified VLA transform",
            }
        ]
        markdown = gate.render_markdown(report)
        self.assertIn("VR spill bytes", markdown)
        self.assertIn("| 256 | 256 | 512 | 1/1/0 (>=1) |", markdown)
        self.assertIn("`VECTORIZED` (loop, `kernel:loop.header`)", markdown)
        self.assertIn("VLEN: `256` bits (`vlenb=32`)", markdown)


if __name__ == "__main__":
    unittest.main(verbosity=2)
