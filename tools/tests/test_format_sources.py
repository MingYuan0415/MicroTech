"""Explicit selection and mutation boundaries for the formatting entrypoint."""

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    "format_sources", Path(__file__).resolve().parents[1] / "format_sources.py")
formatter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(formatter)


class FormatSourcesTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)

    def source(self, name, text="int main(void){return 0;}\n"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def require_astyle(self):
        if not shutil.which("astyle"):
            self.skipTest("AStyle is not installed")
        version = subprocess.run(
            ["astyle", "--options=none", "--project=none", "--version"],
            capture_output=True, text=True, check=True)
        if "Version 3.6.19" not in version.stdout + version.stderr:
            self.skipTest("requires the project AStyle version")

    def test_only_explicit_files_including_new_and_space_names(self):
        selected = self.source("main/new file.c")
        other = self.source("main/unrelated.c")
        self.assertEqual(formatter.select_sources(
            self.root, ["main/new file.c", str(selected)]), [selected])
        self.assertEqual(other.read_text(), "int main(void){return 0;}\n")

    def test_rejects_third_party_generated_ignored_and_outside(self):
        (self.root / ".gitignore").write_text("local/\n")
        for name in ("managed_components/x.c", "layers/bsp/XPowersLib/x.h",
                     "build/sim/gen.h", "build-other/gen.h",
                     "main/mmap_generate_x.h", "local/generated.c"):
            self.source(name)
            with self.subTest(name=name), self.assertRaises(ValueError):
                formatter.select_sources(self.root, [name])
        for name in ("../external.c", "main", "missing.c"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                formatter.select_sources(self.root, [name])

    def test_nested_repo_file_is_selected(self):
        repo = self.root / "layers/apps"
        repo.mkdir(parents=True)
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        source = self.source("layers/apps/page.c")
        self.assertEqual(formatter.select_sources(
            self.root, ["layers/apps/page.c"]), [source])

    def test_symlink_is_not_a_mutation_alias(self):
        target = self.source("main/other.c")
        (self.root / "alias.c").symlink_to(target)
        with self.assertRaises(ValueError):
            formatter.select_sources(self.root, ["alias.c"])

    def test_empty_selection_never_invokes_astyle(self):
        with patch.object(formatter.subprocess, "run") as run:
            self.assertEqual(formatter.main([]), 0)
        run.assert_not_called()

    def test_real_formatter_check_and_idempotence(self):
        self.require_astyle()
        source = self.source("main/new file.c")
        other = self.source("main/unrelated.c")
        before = source.read_bytes()
        with patch.object(formatter, "ROOT", self.root):
            self.assertNotEqual(formatter.main(["--check", str(source)]), 0)
            self.assertEqual(source.read_bytes(), before)
            self.assertEqual(formatter.main([str(source)]), 0)
            formatted = source.read_bytes()
            self.assertNotEqual(formatted, before)
            self.assertEqual(formatter.main([str(source)]), 0)
            self.assertEqual(formatter.main(["--check", str(source)]), 0)
        self.assertEqual(source.read_bytes(), formatted)
        self.assertEqual(other.read_bytes(), before)
        self.assertEqual(list(self.root.rglob("*.orig")), [])

    def test_user_and_project_options_cannot_change_the_baseline(self):
        self.require_astyle()
        home = self.root / "home"
        home.mkdir()
        config = home / ".astylerc"
        config.write_text("--keep-one-line-blocks\n")
        source = self.source("main/input.c", "int main(void) {return 0;}\n")
        original = source.read_bytes()
        expected = b"int main(void)\n{\n    return 0;\n}\n"
        environments = (
            {"HOME": str(home)},
            {"ARTISTIC_STYLE_OPTIONS": str(config)},
            {"ARTISTIC_STYLE_PROJECT_OPTIONS": str(config)},
        )
        for environment in environments:
            with self.subTest(environment=environment), \
                    patch.dict(os.environ), patch.object(formatter, "ROOT", self.root):
                for key in ("ARTISTIC_STYLE_OPTIONS", "ARTISTIC_STYLE_PROJECT_OPTIONS"):
                    os.environ.pop(key, None)
                os.environ.update(environment)
                source.write_bytes(original)
                self.assertNotEqual(formatter.main(["--check", str(source)]), 0)
                self.assertEqual(source.read_bytes(), original)
                self.assertEqual(formatter.main([str(source)]), 0)
                self.assertEqual(source.read_bytes(), expected)
                self.assertEqual(formatter.main(["--check", str(source)]), 0)
