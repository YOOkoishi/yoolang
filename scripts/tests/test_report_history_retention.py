from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location(
    "update_report_history", ROOT / "scripts/update_report_history.py"
)
assert SPEC is not None and SPEC.loader is not None
UPDATE_HISTORY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(UPDATE_HISTORY)


class ReportHistoryRetentionTests(unittest.TestCase):
    def test_prunes_runs_and_branches_outside_retained_history(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            public = Path(directory)
            paths = [
                public / "perf-report/runs/main-keep",
                public / "perf-report/runs/main-drop",
                public / "instruction-report/runs/main-keep",
                public / "instruction-report/runs/main-drop",
                public / "branches/topic/perf-report/runs/topic-keep",
                public / "branches/topic/perf-report/runs/topic-drop",
                public / "branches/topic/instruction-report/runs/topic-keep",
                public / "branches/stale/perf-report/runs/stale-drop",
            ]
            for path in paths:
                path.mkdir(parents=True)
                (path / "index.html").write_text("report")

            UPDATE_HISTORY.prune_report_directories(
                public,
                [
                    {"run_id": "main-keep", "branch": "main", "is_main_branch": True},
                    {"run_id": "topic-keep", "branch": "topic", "is_main_branch": False},
                ],
            )

            self.assertTrue((public / "perf-report/runs/main-keep").is_dir())
            self.assertFalse((public / "perf-report/runs/main-drop").exists())
            self.assertTrue(
                (public / "branches/topic/perf-report/runs/topic-keep").is_dir()
            )
            self.assertFalse(
                (public / "branches/topic/perf-report/runs/topic-drop").exists()
            )
            self.assertFalse((public / "branches/stale").exists())


if __name__ == "__main__":
    unittest.main()
