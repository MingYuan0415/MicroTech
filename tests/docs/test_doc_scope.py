"""Documentation checks must ignore local dependencies, not owned new docs."""

from pathlib import Path
import subprocess
import tempfile
import unittest

import test_doc_links as links


class DocumentScopeTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()
        self.git("init", "-q")

    def git(self, *args, cwd=None):
        return subprocess.check_output(
            ["git", "-C", str(cwd or self.root), *args], text=True)

    def write(self, name, text="[target](missing.md)\n"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def test_tracks_new_and_existing_but_not_ignored_or_deleted(self):
        self.write(".gitignore", "node_modules/\n")
        self.write("doc/tracked.md")
        deleted = self.write("doc/deleted.md")
        self.git("add", ".")
        deleted.unlink()
        self.write("doc/new file.md")
        self.write("node_modules/package/README.md")
        self.write("managed_components/vendor/README.md")
        self.assertEqual({str(p) for p, _ in links.documents(self.root)},
                         {"doc/tracked.md", "doc/new file.md"})

    def test_initialized_submodule_docs_are_included(self):
        repo = self.root / "layers/apps"
        repo.mkdir(parents=True)
        self.git("init", "-q", cwd=repo)
        self.write("layers/apps/README.md")
        self.git("add", ".", cwd=repo)
        self.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                 "commit", "-qm", "fixture", cwd=repo)
        revision = self.git("rev-parse", "HEAD", cwd=repo).strip()
        self.git("update-index", "--add", "--cacheinfo",
                 f"160000,{revision},layers/apps")
        self.write("layers/apps/new.md")
        self.assertEqual({str(p) for p, _ in links.documents(self.root)},
                         {"layers/apps/README.md", "layers/apps/new.md"})

    def test_real_owned_broken_link_still_fails(self):
        self.write("README.md")
        check = links.DocLinkTest("test_relative_markdown_links_resolve")
        check.root = self.root
        with self.assertRaisesRegex(AssertionError, "missing.md"):
            check.test_relative_markdown_links_resolve()
