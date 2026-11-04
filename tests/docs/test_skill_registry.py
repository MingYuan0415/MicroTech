#!/usr/bin/env python3
"""Keep the agent skill catalog aligned with the code contracts it describes."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SKILLS = ROOT / ".agents" / "skills"
BSP = ROOT / "layers" / "bsp"


class SkillRegistryTest(unittest.TestCase):
    def _skill_dirs(self):
        return sorted(path for path in SKILLS.iterdir() if path.is_dir())

    def test_every_skill_has_matching_frontmatter_and_links(self) -> None:
        for skill in self._skill_dirs():
            with self.subTest(skill=skill.name):
                text = (skill / "SKILL.md").read_text(encoding="utf-8")
                self.assertIn(f"name: {skill.name}", text)
                self.assertIn("description:", text)
                linked = set(re.findall(
                    r"references/[A-Za-z0-9_.-]+\.md", text))
                files = {
                    str(path.relative_to(skill))
                    for path in (skill / "references").glob("*.md")
                }
                self.assertEqual(files - linked, set(),
                                 f"{skill.name} has unlinked references")
                for reference in linked:
                    self.assertTrue((skill / reference).is_file())
                agents = skill / "agents" / "openai.yaml"
                if agents.is_file():
                    self.assertIn("display_name", agents.read_text(
                        encoding="utf-8"))

    def test_esp32_skill_covers_every_board_manifest(self) -> None:
        text = (SKILLS / "esp32" / "SKILL.md").read_text(encoding="utf-8")
        self.assertIn("Adding or Reviewing a Board Variant", text)
        self.assertIn("docs/documents.yaml", text)
        self.assertIn("docs-local/", text)
        for board in BSP.glob("waveshare/*/board.cmake"):
            manifest = board.read_text(encoding="utf-8")
            name = re.search(r'set\(MT_BOARD_NAME\s+"([^"]+)"\)',
                             manifest).group(1)
            self.assertIn(name.lower(), text.lower(),
                          f"esp32 skill omits board {name}")

    def test_operational_skills_name_the_new_contracts(self) -> None:
        app = (SKILLS / "app-development" / "SKILL.md").read_text(
            encoding="utf-8")
        self.assertIn("app_catalog.cmake", app)
        self.assertIn("app_manager_builtin_has_app", app)
        design = (SKILLS / "design-contract" / "SKILL.md").read_text(
            encoding="utf-8")
        self.assertIn("MT_PRODUCT_DESIGN_TARGET", design)
        integration = (SKILLS / "lvgl-integration" / "SKILL.md").read_text(
            encoding="utf-8")
        self.assertIn("esp32-p4-wifi6-touch-lcd-3.5", integration)
        codesize = (SKILLS / "codesize" / "SKILL.md").read_text(
            encoding="utf-8")
        self.assertIn("riscv32", codesize)


if __name__ == "__main__":
    unittest.main()
