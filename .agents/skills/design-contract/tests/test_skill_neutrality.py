#!/usr/bin/env python3
"""Static checks that skill instructions stay style and panel neutral."""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).parents[4]
SKILLS = ROOT / ".agents" / "skills"
CURRENT_PROFILE = json.loads(
    (ROOT / "doc" / "design" / "profiles" / "dark_sparse.json").read_text(
        encoding="utf-8"))
CURRENT_LITERALS = "|".join(
    re.escape(value) for value in CURRENT_PROFILE["roles"].values())
BANNED = re.compile(
    r"(?:\b368\b|\b448\b|AMOLED|PLUME|AZURE|\b#000000\b|\b#14191E\b|"
    r"\b#43C6DB\b|64 px|420 px|" + CURRENT_LITERALS + ")",
    re.IGNORECASE,
)


class SkillNeutralityTest(unittest.TestCase):
    def test_design_skills_have_no_current_style_or_panel_literals(self) -> None:
        for name in ("design-contract", "visual-design", "aesthetic-review",
                     "motion-interaction", "ux-heuristics"):
            path = SKILLS / name / "SKILL.md"
            self.assertTrue(path.is_file(), path)
            text = path.read_text(encoding="utf-8")
            self.assertIn(f"name: {name}", text, f"{name} frontmatter mismatch")
            self.assertIsNone(BANNED.search(text), f"{name} contains a banned literal")

    def test_design_skills_name_profile_and_target(self) -> None:
        for name in ("design-contract", "visual-design", "aesthetic-review",
                     "motion-interaction", "ux-heuristics"):
            text = (SKILLS / name / "SKILL.md").read_text(encoding="utf-8").lower()
            self.assertIn("profile", text, name)
            self.assertIn("target", text, name)

    def test_resource_and_evidence_skills_name_their_contract(self) -> None:
        for name in ("design-contract", "visual-design", "motion-interaction"):
            text = (SKILLS / name / "SKILL.md").read_text(encoding="utf-8").lower()
            self.assertIn("budget", text, name)
        for name in ("aesthetic-review", "ux-heuristics"):
            text = (SKILLS / name / "SKILL.md").read_text(encoding="utf-8").lower()
            self.assertIn("evidence", text, name)

    def test_all_reference_files_exist_and_are_linked(self) -> None:
        for name in ("design-contract", "visual-design", "aesthetic-review",
                     "motion-interaction", "ux-heuristics"):
            base = SKILLS / name
            text = (base / "SKILL.md").read_text(encoding="utf-8")
            linked = set(re.findall(r"references/[A-Za-z0-9_.-]+\.md", text))
            files = {str(path.relative_to(base)) for path in (base / "references").glob("*.md")}
            self.assertEqual(files - linked, set(), f"{name} has unlinked references")
            for reference in linked:
                self.assertTrue((base / reference).is_file(), f"{name}: {reference}")

    def test_profile_relative_soft_playful_matches_decoration_axis(self) -> None:
        profile = json.loads(
            (ROOT / "doc" / "design" / "profiles" / "soft_playful.json").read_text(
                encoding="utf-8"))
        self.assertEqual(profile["style_axes"]["decoration"], "expressive")
        review = (SKILLS / "aesthetic-review" / "references" /
                  "profile-relative-review.md").read_text(encoding="utf-8")
        section = review.split("### `soft_playful`", 1)[1].split(
            "### `neutral`", 1)[0]
        self.assertIn("expressive decoration", section)
        self.assertNotIn("sparse decoration", section)


    def test_all_skill_docs_have_no_current_panel_or_palette_literals(self) -> None:
        for name in ("design-contract", "visual-design", "aesthetic-review",
                     "motion-interaction", "ux-heuristics"):
            base = SKILLS / name
            docs = [base / "SKILL.md"] + sorted((base / "references").glob("*.md"))
            for path in docs:
                self.assertIsNone(
                    BANNED.search(path.read_text(encoding="utf-8")),
                    f"{path} contains a current-UI literal")

if __name__ == "__main__":
    unittest.main()
