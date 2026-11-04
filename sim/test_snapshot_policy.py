"""Policy checks for the simulator snapshot review workflow."""

from __future__ import annotations

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class SnapshotPolicyTest(unittest.TestCase):
    def test_no_tracked_ui_golden_assets(self):
        self.assertFalse(
            (ROOT / "sim" / "ci" / "golden").exists(),
            "simulator UI goldens are review artifacts, not version baselines",
        )

    def test_review_spec_has_no_golden_flags(self):
        spec = json.loads(
            (ROOT / "sim" / "ci" / "review" / "spec.json").read_text(
                encoding="utf-8"
            )
        )

        def has_no_golden(node):
            if isinstance(node, dict):
                if "no_golden" in node:
                    return True
                return any(has_no_golden(value) for value in node.values())
            if isinstance(node, list):
                return any(has_no_golden(value) for value in node)
            return False

        self.assertFalse(has_no_golden(spec))


if __name__ == "__main__":
    unittest.main()
