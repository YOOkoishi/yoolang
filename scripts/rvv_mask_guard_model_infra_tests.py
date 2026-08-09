#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import unittest

import rvv_mask_guard_tests as guard


class GuardCoverageModelTests(unittest.TestCase):
    def test_same_binary_vlen_matrix_is_fixed_and_complete(self) -> None:
        self.assertEqual(guard.VLENS, (128, 256, 512, 1024))
        self.assertEqual({case.name for case in guard.CASES}, {"allfalse", "sparse"})
        self.assertEqual(
            guard.harness_arguments("sparse", 0x1234, 4),
            ["sparse", "4660", "4"],
        )
        with self.assertRaisesRegex(ValueError, "unknown MASK2"):
            guard.harness_arguments("not-a-case")

    def test_allfalse_places_every_inactive_lane_in_guard_memory(self) -> None:
        case = next(case for case in guard.CASES if case.name == "allfalse")
        self.assertFalse(any(case.mask))
        self.assertEqual(case.rw_lanes, ())
        self.assertEqual(case.guard_lanes, tuple(range(7)))
        self.assertEqual(case.expected_return, 143)

    def test_sparse_has_only_rw_active_lanes_and_guarded_inactive_lanes(self) -> None:
        case = next(case for case in guard.CASES if case.name == "sparse")
        active_lanes = {lane for lane, active in enumerate(case.mask) if active}
        self.assertEqual(active_lanes, {0, 2})
        self.assertTrue(active_lanes.issubset(set(case.rw_lanes)))
        self.assertTrue(set(case.guard_lanes))
        self.assertTrue(
            all(not case.mask[lane] for lane in case.guard_lanes),
            "every lane whose unit-stride address crosses into PROT_NONE must be inactive",
        )
        self.assertEqual(case.expected_return, 1569)

    def test_fixture_uses_only_scalar_public_abi_and_fixed_local_masked_memory(self) -> None:
        source = guard.SOURCE.read_text(encoding="utf-8")
        self.assertIn("int masked_guard_allfalse(int inaccessible[])", source)
        self.assertIn("int masked_guard_sparse(int boundary[], int observed[])", source)
        self.assertIn("mask<7>{}", source)
        self.assertIn("mask<7>{1, 0, 1, 0, 0, 0, 0}", source)
        self.assertEqual(source.count("masked_load("), 2)
        self.assertEqual(source.count("masked_store("), 2)
        self.assertNotIn("gather(", source)
        self.assertNotIn("scatter(", source)

    def test_harness_has_two_prot_none_guards_and_rw_canary_oracle(self) -> None:
        harness = guard.HARNESS.read_text(encoding="utf-8")
        self.assertIn("page_size * 3U, PROT_NONE", harness)
        self.assertIn("mprotect(rw_page, page_size, PROT_READ | PROT_WRITE)", harness)
        self.assertIn("page_size - 3U * sizeof(int32_t)", harness)
        self.assertIn("!byte_is_part_of_active_lane", harness)
        self.assertIn("case_seed % aligned_slots", harness)
        self.assertIn("seed_delta", harness)
        self.assertIn("strtoull(argv[2]", harness)
        self.assertIn('strcmp(argv[1], "allfalse")', harness)
        self.assertIn('strcmp(argv[1], "sparse")', harness)


class AssemblyParserTests(unittest.TestCase):
    ASSEMBLY = """
masked_guard_allfalse:
  vsetivli t0, 7, e32, m2, ta, ma
  vle32.v v2, (a0), v0.t
  vse32.v v4, (a0), v0.t
  .size masked_guard_allfalse, .-masked_guard_allfalse
masked_guard_sparse:
  vsetivli t0, 7, e32, m2, ta, ma
  vle32.v v6, (a0), v0.t
  vse32.v v8, (a0), v0.t
  .size masked_guard_sparse, .-masked_guard_sparse
"""

    OBJDUMP = """
0000000000000000 <masked_guard_allfalse>:
   0: cd13f2d7  vsetivli t0,7,e32,m2,ta,ma
   4: 00056107  vle32.v v2,(a0),v0.t
   8: 00056227  vse32.v v4,(a0),v0.t
000000000000000c <masked_guard_sparse>:
   c: cd13f2d7  vsetivli t0,7,e32,m2,ta,ma
  10: 00056307  vle32.v v6,(a0),v0.t
  14: 00056427  vse32.v v8,(a0),v0.t
"""

    def test_assembly_and_objdump_models_accept_required_masked_ops(self) -> None:
        guard.validate_codegen(self.ASSEMBLY, objdump=False)
        guard.validate_codegen(self.OBJDUMP, objdump=True)

    def test_unmasked_memory_is_a_hard_failure(self) -> None:
        unsafe = self.ASSEMBLY.replace(", v0.t", "", 1)
        with self.assertRaisesRegex(guard.GateFailure, "unsafe unmasked"):
            guard.validate_codegen(unsafe, objdump=False)

    def test_missing_masked_backend_surface_is_explicitly_blocked(self) -> None:
        absent = self.ASSEMBLY.replace("  vle32.v v2, (a0), v0.t\n", "")
        with self.assertRaisesRegex(guard.ImplementationBlocked, "exactly one"):
            guard.validate_codegen(absent, objdump=False)


class FixtureLocationTests(unittest.TestCase):
    def test_fixture_paths_are_repository_local_files(self) -> None:
        root = Path(__file__).resolve().parents[1]
        self.assertTrue(guard.SOURCE.is_file())
        self.assertTrue(guard.HARNESS.is_file())
        self.assertTrue(guard.SOURCE.is_relative_to(root))
        self.assertTrue(guard.HARNESS.is_relative_to(root))


if __name__ == "__main__":
    unittest.main(verbosity=2)
