#!/usr/bin/env python3

from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

import rvv_random_differential as diff


class StableCaseGenerationTests(unittest.TestCase):
    def test_seed_derivation_is_stable_and_domain_separated(self) -> None:
        first = diff.derive_seed(0x1234, "case-a")
        self.assertEqual(first, diff.derive_seed(0x1234, "case-a"))
        self.assertNotEqual(first, diff.derive_seed(0x1234, "case-b"))
        self.assertNotEqual(first, diff.derive_seed(0x1235, "case-a"))

    def test_case_ids_and_serialization_are_stable(self) -> None:
        cases_a = diff.generate_smoke_cases(0xCAFE)
        cases_b = diff.generate_smoke_cases(0xCAFE)
        self.assertEqual(cases_a, cases_b)
        self.assertEqual(
            [case.case_id for case in cases_a],
            [case.case_id for case in cases_b],
        )
        for case in cases_a:
            self.assertEqual(diff.DifferentialCase.from_dict(case.to_dict()), case)

    def test_extended_and_nightly_are_deterministic_strict_supersets(self) -> None:
        smoke = diff.generate_tier_cases(0xCAFE, "smoke")
        extended = diff.generate_tier_cases(0xCAFE, "extended")
        nightly = diff.generate_tier_cases(0xCAFE, "nightly")
        self.assertEqual(extended, diff.generate_tier_cases(0xCAFE, "extended"))
        smoke_ids = {case.case_id for case in smoke}
        extended_ids = {case.case_id for case in extended}
        nightly_ids = {case.case_id for case in nightly}
        self.assertLess(smoke_ids, extended_ids)
        self.assertLess(extended_ids, nightly_ids)
        self.assertGreater(len(nightly), len(extended))
        self.assertTrue(
            all(
                case.expectation != diff.EXPECT_BLOCKED
                and (
                    case.expectation != diff.EXPECT_RUN
                    or diff.active_case_is_supported(case)
                )
                for case in nightly
            )
        )

    def test_different_master_seed_changes_case_identity_not_mandatory_shape(self) -> None:
        first = diff.generate_smoke_cases(1)
        second = diff.generate_smoke_cases(2)
        self.assertNotEqual(
            {case.case_id for case in first}, {case.case_id for case in second}
        )
        first_run = [case for case in first if case.expectation == diff.EXPECT_RUN]
        second_run = [case for case in second if case.expectation == diff.EXPECT_RUN]
        self.assertTrue(
            set(diff.mandatory_boundary_lengths()).issubset(
                {case.length for case in first_run}
            )
        )
        self.assertTrue(
            set(diff.mandatory_boundary_lengths()).issubset(
                {case.length for case in second_run}
            )
        )

    def test_case_id_filter_accepts_exact_or_unique_prefix(self) -> None:
        cases = diff.generate_smoke_cases(0xBEEF)
        selected = cases[0]
        self.assertEqual(diff.select_case_id(cases, selected.case_id), selected)
        prefix_length = 1
        while prefix_length < len(selected.case_id):
            prefix = selected.case_id[:prefix_length]
            if sum(case.case_id.startswith(prefix) for case in cases) == 1:
                break
            prefix_length += 1
        self.assertEqual(diff.select_case_id(cases, prefix), selected)
        with self.assertRaisesRegex(ValueError, "unknown case"):
            diff.select_case_id(cases, "not-a-case")


class MandatoryCoverageContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cases = diff.generate_smoke_cases(diff.DEFAULT_SEED)

    def test_active_lengths_cover_every_e32_m1_vlen_boundary(self) -> None:
        lengths = {
            case.length
            for case in self.cases
            if case.expectation == diff.EXPECT_RUN
        }
        self.assertTrue(set(diff.mandatory_boundary_lengths()).issubset(lengths))
        self.assertTrue(any(length > 65 for length in lengths))

    def test_alignment_alias_stride_mask_and_float_dimensions_are_forced(self) -> None:
        self.assertEqual(
            {
                case.alignment_offset
                for case in self.cases
                if case.expectation == diff.EXPECT_RUN
            },
            set(diff.ALIGNMENT_OFFSETS),
        )
        self.assertEqual({case.alias for case in self.cases}, set(diff.ALIAS_PATTERNS))
        self.assertEqual({case.stride for case in self.cases}, set(diff.CONSTANT_STRIDES))
        self.assertEqual({case.mask for case in self.cases}, set(diff.MASK_PATTERNS))
        self.assertEqual({case.dtype for case in self.cases}, {"i32", "f32"})
        self.assertTrue({1, 2, 3}.issubset(set(diff.ALIGNMENT_OFFSETS)))
        active_offsets = {
            case.alignment_offset
            for case in self.cases
            if case.expectation == diff.EXPECT_RUN
            and case.kernel != "masked_i32"
        }
        self.assertTrue({1, 2, 3}.issubset(active_offsets))
        for driver in (
            diff.ACTIVE_DRIVER,
            diff.FLOAT_DRIVER,
            diff.INDEXED_DRIVER,
            diff.REDUCTION_DRIVER,
        ):
            self.assertNotIn("offset & 3U", driver)
            self.assertNotIn("int32_t *values =", driver)
            self.assertNotIn("float *values =", driver)
            self.assertIn("storage + offset", driver)
        self.assertIn("random_add_bias(void *values", diff.ACTIVE_DRIVER)
        self.assertIn("random_float_bias(void *values", diff.FLOAT_DRIVER)

    def test_only_the_current_real_backend_subset_is_marked_run(self) -> None:
        active = [case for case in self.cases if case.expectation == diff.EXPECT_RUN]
        self.assertTrue(active)
        self.assertTrue(all(diff.active_case_is_supported(case) for case in active))
        masked = [case for case in active if case.kernel == "masked_i32"]
        self.assertEqual(len(masked), 2)
        self.assertEqual({case.mask for case in masked}, {"allfalse", "sparse"})
        self.assertTrue(all(case.reason is None for case in masked))
        floating = [case for case in active if case.kernel == "unit_f32"]
        self.assertEqual(len(floating), 1)
        self.assertEqual(floating[0].dtype, "f32")
        self.assertIsNone(floating[0].reason)
        indexed = [case for case in active if case.kernel == "indexed_i32"]
        self.assertEqual(len(indexed), 5)
        self.assertEqual({case.stride for case in indexed}, {2, 4, -1, -2, -4})
        self.assertTrue(all(case.reason is None for case in indexed))
        reductions = [case for case in active if case.kernel == "integer_reduction"]
        self.assertEqual(len(reductions), 1)
        self.assertEqual(reductions[0].mask, "alltrue")
        self.assertIsNone(reductions[0].reason)
        self.assertTrue(
            all(
                case.reason
                for case in self.cases
                if case.expectation == diff.EXPECT_BLOCKED
            )
        )
        self.assertTrue(
            all(
                case.expected_code and case.kernel in diff.CONTRACT_SOURCES
                for case in self.cases
                if case.expectation in {diff.EXPECT_PLAN, diff.EXPECT_REJECT}
            )
        )

    def test_scalable_float_run_keeps_ieee_boundary_oracle(self) -> None:
        floating = [
            case
            for case in self.cases
            if case.expectation == diff.EXPECT_RUN and case.kernel == "unit_f32"
        ]
        self.assertEqual(len(floating), 1)
        self.assertEqual(floating[0].length, 65)
        for bits in (
            "0x00000000",
            "0x80000000",
            "0x7f800000",
            "0xff800000",
            "0x7fc12345",
            "0x00000001",
            "0x007fffff",
        ):
            self.assertIn(bits, diff.FLOAT_DRIVER)
        self.assertIn("scalar_expected(before)", diff.FLOAT_DRIVER)

    def test_constant_stride_runs_cover_positive_and_rotated_reverse_sources(self) -> None:
        indexed = [
            case
            for case in self.cases
            if case.expectation == diff.EXPECT_RUN and case.kernel == "indexed_i32"
        ]
        self.assertEqual({case.stride for case in indexed}, {2, 4, -1, -2, -4})
        for symbol in (
            "random_stride_p2",
            "random_stride_p4",
            "random_stride_n1",
            "random_stride_n2",
            "random_stride_n4",
        ):
            self.assertIn(symbol, diff.INDEXED_SOURCE)
        self.assertIn("is_touched(lane, n, stride)", diff.INDEXED_DRIVER)

    def test_integer_reduction_run_covers_native_and_ordered_fallback_ops(self) -> None:
        reductions = [
            case
            for case in self.cases
            if case.expectation == diff.EXPECT_RUN
            and case.kernel == "integer_reduction"
        ]
        self.assertEqual(len(reductions), 1)
        for suffix in ("add", "mul", "and", "or", "xor"):
            self.assertIn(f"random_reduce_{suffix}", diff.REDUCTION_SOURCE)
        self.assertIn("expected_mul *= bits", diff.REDUCTION_DRIVER)
        self.assertIn("actual_xor != expected_xor", diff.REDUCTION_DRIVER)

    def test_required_backend_manifest_has_no_implementation_blockers(self) -> None:
        blocked = [
            case for case in self.cases if case.expectation == diff.EXPECT_BLOCKED
        ]
        self.assertEqual(blocked, [])

    def test_plan_and_reject_contracts_cover_alias_stride_and_fp_order(self) -> None:
        plans = {
            (case.kernel, case.expected_code)
            for case in self.cases
            if case.expectation == diff.EXPECT_PLAN
        }
        rejects = {
            (case.kernel, case.expected_code)
            for case in self.cases
            if case.expectation == diff.EXPECT_REJECT
        }
        self.assertIn(("alias_i32", "VECTORIZED"), plans)
        self.assertTrue(
            {
                ("dynamic_stride_i32", "REJECT_NON_CANONICAL_LOOP"),
                ("strict_float_reduction", "REJECT_FP_ORDER"),
            }.issubset(rejects)
        )


class ReplayAndArtifactTests(unittest.TestCase):
    def test_failure_minimizer_reduces_length_alignment_and_seed(self) -> None:
        original = next(
            case
            for case in diff.generate_tier_cases(0xFACE, "extended")
            if case.expectation == diff.EXPECT_RUN
            and case.kernel == "unit_i32"
            and case.length > 100
            and case.alignment_offset >= 3
            and case.case_seed > 0xFFFF
        )

        def reproduces(candidate: diff.DifferentialCase) -> bool:
            return (
                candidate.length >= 7
                and candidate.alignment_offset >= 2
                and candidate.case_seed >= 1
            )

        minimized, trace = diff.minimize_failing_case(original, reproduces)
        self.assertEqual(minimized.length, 7)
        self.assertEqual(minimized.alignment_offset, 2)
        self.assertEqual(minimized.case_seed, 1)
        self.assertTrue(trace)
        self.assertEqual(
            diff.DifferentialCase.from_dict(minimized.to_dict()), minimized
        )

    def test_failure_signature_ignores_case_id_but_not_failure_class(self) -> None:
        first = diff.CaseExecutionError(
            diff.generate_smoke_cases(1)[0],
            "case aaa scalar/RVV mismatch at VLEN=256: values differ",
        )
        second = diff.CaseExecutionError(
            diff.generate_smoke_cases(2)[0],
            "case bbb scalar/RVV mismatch at VLEN=256: other values",
        )
        different_vlen = diff.CaseExecutionError(
            diff.generate_smoke_cases(2)[0],
            "case bbb scalar/RVV mismatch at VLEN=512: other values",
        )
        self.assertEqual(diff.failure_signature(first), diff.failure_signature(second))
        self.assertNotEqual(
            diff.failure_signature(first), diff.failure_signature(different_vlen)
        )

    def test_replay_round_trip_validates_case_id(self) -> None:
        case = diff.generate_smoke_cases(0xDEAD)[0]
        with tempfile.TemporaryDirectory(prefix="rvv-diff-replay-model-") as temp:
            path = Path(temp) / "case.json"
            path.write_text(
                json.dumps(diff.replay_payload(0xDEAD, case)), encoding="utf-8"
            )
            seed, replayed = diff.load_replay(path)
            self.assertEqual(seed, 0xDEAD)
            self.assertEqual(replayed, case)

            corrupted = diff.replay_payload(0xDEAD, case)
            corrupted["case"]["length"] += 1
            path.write_text(json.dumps(corrupted), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "case id mismatch"):
                diff.load_replay(path)

    def test_masked_case_seed_case_id_and_replay_round_trip_remain_exact(self) -> None:
        first_cases = diff.generate_smoke_cases(0x1234)
        second_cases = diff.generate_smoke_cases(0x1235)
        first = next(
            case
            for case in first_cases
            if case.kernel == "masked_i32" and case.mask == "sparse"
        )
        second = next(
            case
            for case in second_cases
            if case.kernel == "masked_i32" and case.mask == "sparse"
        )
        self.assertEqual(first.expectation, diff.EXPECT_RUN)
        self.assertNotEqual(first.case_seed, second.case_seed)
        self.assertNotEqual(first.case_id, second.case_id)
        with tempfile.TemporaryDirectory(prefix="rvv-mask-replay-model-") as temp:
            path = Path(temp) / "case.json"
            path.write_text(
                json.dumps(diff.replay_payload(0x1234, first)), encoding="utf-8"
            )
            seed, replayed = diff.load_replay(path)
            self.assertEqual(seed, 0x1234)
            self.assertEqual(replayed, first)

    def test_masked_failure_artifact_records_the_real_guard_argv(self) -> None:
        case = next(
            case
            for case in diff.generate_smoke_cases(0xCAFE)
            if case.kernel == "masked_i32" and case.mask == "allfalse"
        )
        with tempfile.TemporaryDirectory(prefix="rvv-mask-artifact-model-") as temp:
            root = Path(temp)
            workspace = root / "workspace"
            workspace.mkdir()
            artifact = diff.preserve_failure(
                root / "artifacts",
                0xCAFE,
                case,
                "injected masked failure",
                workspace,
                [],
                Path("/tmp/yoolang-compiler"),
            )
            input_text = (artifact / "input.txt").read_text(encoding="ascii")
            self.assertEqual(
                input_text,
                f"argv: allfalse {case.case_seed} {case.alignment_offset}\n",
            )
            seed, replayed = diff.load_replay(artifact / "case.json")
            self.assertEqual(seed, 0xCAFE)
            self.assertEqual(replayed, case)

    def test_failure_artifact_keeps_manifest_logs_workspace_and_repro(self) -> None:
        case = next(
            case
            for case in diff.generate_smoke_cases(0xFACE)
            if case.expectation == diff.EXPECT_RUN
        )
        with tempfile.TemporaryDirectory(prefix="rvv-diff-artifact-model-") as temp:
            root = Path(temp)
            workspace = root / "temporary-workspace"
            workspace.mkdir()
            (workspace / "kernel.s").write_text("vsetvli a0, a0, e32, m1\n")
            record = diff.CommandRecord(
                ["compiler", "kernel.sy"], 1, "stdout", "stderr", "abc", False
            )
            first = diff.preserve_failure(
                root / "artifacts",
                0xFACE,
                case,
                "injected failure",
                workspace,
                [record],
                Path("/tmp/yoolang-compiler"),
            )
            second = diff.preserve_failure(
                root / "artifacts",
                0xFACE,
                case,
                "second failure",
                workspace,
                [record],
                Path("/tmp/yoolang-compiler"),
            )
            self.assertNotEqual(first, second)
            for spelling in (
                "case.json",
                "failure.txt",
                "commands.json",
                "repro.sh",
                "input.txt",
                "workspace/kernel.s",
            ):
                self.assertTrue((first / spelling).is_file(), spelling)
            self.assertIn("--replay=", (first / "repro.sh").read_text())
            self.assertIn("--compiler=", (first / "repro.sh").read_text())
            self.assertTrue((first / "repro.sh").stat().st_mode & 0o100)
            seed, replayed = diff.load_replay(first / "case.json")
            self.assertEqual(seed, 0xFACE)
            self.assertEqual(replayed, case)

    def test_minimized_artifact_repro_points_at_minimized_case(self) -> None:
        case = next(
            case
            for case in diff.generate_smoke_cases(0x1234)
            if case.expectation == diff.EXPECT_RUN and case.length > 1
        )
        minimized = diff.replace_case(
            case, length=1, alignment_offset=0, case_seed=1
        )
        with tempfile.TemporaryDirectory(prefix="rvv-diff-minimized-artifact-") as temp:
            root = Path(temp)
            workspace = root / "workspace"
            workspace.mkdir()
            artifact = diff.preserve_failure(
                root / "artifacts",
                0x1234,
                case,
                "injected failure",
                workspace,
                [],
                Path("/tmp/yoolang-compiler"),
                minimized,
                [{"field": "length", "value": 1, "reproduced": True}],
            )
            self.assertTrue((artifact / "case.json").is_file())
            self.assertTrue((artifact / "minimized-case.json").is_file())
            self.assertTrue((artifact / "shrink.json").is_file())
            self.assertIn(
                "minimized-case.json", (artifact / "repro.sh").read_text()
            )
            _, replayed = diff.load_replay(artifact / "minimized-case.json")
            self.assertEqual(replayed, minimized)

    def test_manifest_only_needs_no_cross_toolchain(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = diff.main(["--manifest-only", "--seed", "0x1234"])
        self.assertEqual(result, 0)
        payload = json.loads(output.getvalue())
        self.assertEqual(payload["schema"], diff.SCHEMA_VERSION)
        self.assertEqual(payload["master_seed"], "0x0000000000001234")
        self.assertEqual(payload["tier"], "smoke")
        self.assertTrue(payload["cases"])

    def test_manifest_only_reports_requested_tier(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = diff.main(
                ["--manifest-only", "--tier", "extended", "--seed", "0x1234"]
            )
        self.assertEqual(result, 0)
        payload = json.loads(output.getvalue())
        self.assertEqual(payload["tier"], "extended")
        self.assertGreater(
            len(payload["cases"]), len(diff.generate_smoke_cases(0x1234))
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
