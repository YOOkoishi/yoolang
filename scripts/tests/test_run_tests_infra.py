from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
RUN_TESTS_PATH = ROOT / "scripts/run_tests.py"
SPEC = importlib.util.spec_from_file_location("yoolang_run_tests", RUN_TESTS_PATH)
assert SPEC is not None and SPEC.loader is not None
run_tests = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = run_tests
SPEC.loader.exec_module(run_tests)


class InfraDiscoveryTests(unittest.TestCase):
    def test_every_suffix_script_is_auto_discovered(self) -> None:
        expected = set((ROOT / "scripts").glob("*_infra_tests.py"))
        discovered = set(run_tests.discover_infra_scripts())
        self.assertTrue(expected)
        self.assertTrue(expected.issubset(discovered))

    def test_legacy_scripts_remain_discovered(self) -> None:
        discovered = {path.name for path in run_tests.discover_infra_scripts()}
        expected = {
            name
            for name in run_tests.LEGACY_INFRA_SCRIPTS
            if (ROOT / "scripts" / name).exists()
        }
        self.assertTrue(expected.issubset(discovered))

    def test_filter_applies_to_infra_paths(self) -> None:
        selected = run_tests.discover_infra_scripts("vector_numeric_semantics")
        self.assertEqual([path.name for path in selected], ["vector_numeric_semantics_infra_tests.py"])

    def test_profiles_are_an_exact_partition(self) -> None:
        discovered = run_tests.discover_infra_scripts()
        host, host_excluded = run_tests.select_infra_scripts(discovered, "host")
        toolchain, toolchain_excluded = run_tests.select_infra_scripts(discovered, "toolchain")
        all_scripts, all_excluded = run_tests.select_infra_scripts(discovered, "all")
        self.assertFalse(all_excluded)
        self.assertEqual(set(all_scripts), set(discovered))
        self.assertFalse(set(host) & set(toolchain))
        self.assertEqual(set(host) | set(toolchain), set(discovered))
        self.assertEqual(set(host_excluded), set(toolchain))
        self.assertEqual(set(toolchain_excluded), set(host))
        self.assertEqual(
            {path.name for path in toolchain},
            set(run_tests.TOOLCHAIN_INFRA_SCRIPTS),
        )

    def test_toolchain_requirement_environment_matches_profiles(self) -> None:
        self.assertFalse(run_tests.infra_profile_requires_toolchain("host"))
        self.assertTrue(run_tests.infra_profile_requires_toolchain("toolchain"))
        self.assertTrue(run_tests.infra_profile_requires_toolchain("all"))


class InfraInvocationTests(unittest.TestCase):
    def test_compiler_argument_mapping_uses_exact_runner_binary(self) -> None:
        compiler = Path("/tmp/yoolang-ci-custom-compiler")
        for name in run_tests.COMPILER_ARG_INFRA_SCRIPTS:
            command = run_tests.infra_command(ROOT / "scripts" / name, compiler)
            self.assertEqual(command[-2:], ["--compiler", str(compiler)])
        command = run_tests.infra_command(
            ROOT / "scripts/frontend_infra_tests.py", compiler
        )
        self.assertNotIn("--compiler", command)

    def test_every_child_receives_compiler_environment(self) -> None:
        with tempfile.TemporaryDirectory(prefix="run-tests-infra-env-") as temp:
            probe = Path(temp) / "probe_infra_tests.py"
            probe.write_text(
                "import os\nprint(os.environ.get('YOOLANG_COMPILER', 'missing'))\n",
                encoding="utf-8",
            )
            compiler = Path("/tmp/exact-yoolang-compiler")
            result = run_tests.run_infra(probe, compiler, 5.0)
        self.assertEqual(result.status, "PASS")
        self.assertIn(str(compiler), result.detail)

    def test_exit_77_is_an_explicit_skip(self) -> None:
        with tempfile.TemporaryDirectory(prefix="run-tests-infra-skip-") as temp:
            probe = Path(temp) / "probe_infra_tests.py"
            probe.write_text(
                "import sys\nprint('SKIP explicit capability probe')\nsys.exit(77)\n",
                encoding="utf-8",
            )
            result = run_tests.run_infra(probe, Path("/tmp/compiler"), 5.0)
        self.assertEqual(result.status, "SKIP")
        self.assertIn("explicit capability", result.detail)

    def test_nested_capability_skip_is_detectable(self) -> None:
        self.assertTrue(
            run_tests.contains_skip_line(
                "PASS host-only contract\nSKIP optional qemu capability\n"
            )
        )
        self.assertFalse(run_tests.contains_skip_line("PASS all required contracts\n"))

    def test_nested_capability_skip_is_not_reported_as_pass(self) -> None:
        with tempfile.TemporaryDirectory(prefix="run-tests-infra-nested-skip-") as temp:
            probe = Path(temp) / "probe_infra_tests.py"
            probe.write_text(
                "print('PASS host-only contract')\n"
                "print('SKIP optional qemu capability')\n",
                encoding="utf-8",
            )
            result = run_tests.run_infra(probe, Path("/tmp/compiler"), 5.0)
        self.assertEqual(result.status, "SKIP")
        self.assertIn("optional qemu capability", result.detail)


class RequiredE2EToolTests(unittest.TestCase):
    def test_missing_tool_is_skip_by_default_and_failure_when_required(self) -> None:
        source = ROOT / "test/functional/00_main.sy"
        common = dict(
            source=source,
            binary=Path("/tmp/compiler"),
            work_dir=Path("/tmp/run-tests-unit"),
            runtime=Path("/tmp/runtime.a"),
            gcc=None,
            qemu=None,
            compile_timeout=1.0,
            link_timeout=1.0,
            run_timeout=1.0,
            max_input_bytes=0,
            opt_level=0,
            target_flags=["-march=rv64gc", "-mabi=lp64d"],
            qemu_cpu="rv64,v=false",
        )
        optional = run_tests.run_e2e(**common, require_tools=False)
        required = run_tests.run_e2e(**common, require_tools=True)
        self.assertEqual(optional.status, "SKIP")
        self.assertEqual(required.status, "FAIL")
        self.assertIn("riscv64-linux-gnu-gcc", required.detail)

    def test_required_gate_can_fail_on_explicit_skip(self) -> None:
        results = [run_tests.TestResult("infra", "capability", "SKIP")]
        with contextlib.redirect_stdout(io.StringIO()):
            optional = run_tests.summarize(results)
            required = run_tests.summarize(results, fail_on_skip=True)
        self.assertEqual(optional, 0)
        self.assertEqual(required, 1)


class WorkflowGateTests(unittest.TestCase):
    def test_workflow_runs_host_and_required_toolchain_profiles(self) -> None:
        workflow = (ROOT / ".github/workflows/test.yml").read_text(encoding="utf-8")
        for spelling in (
            "--infra-profile host",
            "--infra-profile toolchain",
            "--infra-timeout 120",
            "--infra-timeout 180",
            "--require-e2e-tools",
            "--fail-on-skip",
            "--max-input-bytes 0",
            "--opt-level 2",
            "--opt-level 3",
            "--march rv64gc",
            "--mabi lp64d",
            "command -v FileCheck",
            "command -v riscv64-linux-gnu-as",
            "command -v riscv64-linux-gnu-objdump",
            "command -v riscv64-linux-gnu-readelf",
            "command -v qemu-riscv64",
            'cron: "0 3 * * 0"',
            "--tier nightly",
            'inputs.rvv_diff_tier != \'smoke\'',
            '--tier "${{ inputs.rvv_diff_tier }}"',
        ):
            self.assertIn(spelling, workflow)

        functional_step = workflow.split("- name: Run functional tests", 1)[1].split(
            "- name:", 1
        )[0]
        self.assertIn("id: functional_tests", functional_step)
        self.assertNotIn("continue-on-error", functional_step)
        self.assertIn("FUNCTIONAL_OUTCOME", workflow)
        self.assertIn("Fail job if functional tests failed", workflow)


if __name__ == "__main__":
    unittest.main()
