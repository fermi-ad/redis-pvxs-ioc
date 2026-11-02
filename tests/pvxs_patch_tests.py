#!/usr/bin/env python3
"""Exercise the pinned-source patch gate without modifying dependency inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None


class PatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workspace = tempfile.TemporaryDirectory(prefix="pvxs-patch-tests-")
        cls.root = Path(cls.workspace.name).resolve()
        cls.script = PROJECT / "scripts/apply-pvxs-patches.py"
        cls.manifest = json.loads((PROJECT / "patches/pvxs/manifest.json").read_text())
        cls.template = cls.root / "pristine"
        cls.template.mkdir()
        for name in cls.manifest["files"]:
            source = SOURCE / name
            if source.exists():
                target = cls.template / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        cls.call(cls.template, success=True)
        env = dict(os.environ)
        for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR"):
            env.pop(key, None)
        env["GIT_CEILING_DIRECTORIES"] = str(cls.template)
        subprocess.run(["git", "apply", "--reverse",
                        str(PROJECT / "patches/pvxs" / cls.manifest["patch"])],
                       cwd=cls.template, env=env, check=True, capture_output=True)
        for name, record in cls.manifest["files"].items():
            assert digest(cls.template / name) == record["before_sha256"], name

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    @classmethod
    def call(cls, source, *args, success=True, script=None, env=None):
        result = subprocess.run([PYTHON, str(script or cls.script), str(source), *args],
                                text=True, capture_output=True, env=env)
        if (result.returncode == 0) != success:
            raise AssertionError(result.stdout + result.stderr)
        return result

    def setUp(self):
        self.work = tempfile.TemporaryDirectory(dir=self.root)
        self.source = Path(self.work.name) / "source"
        shutil.copytree(self.template, self.source)

    def tearDown(self):
        self.work.cleanup()

    def hashes(self):
        return {name: digest(self.source / name) for name in self.manifest["files"]}

    def test_pristine_check_apply_and_idempotency(self):
        before = self.hashes()
        self.call(self.source, "--check", success=False)
        self.assertEqual(self.hashes(), before)
        self.call(self.source)
        self.call(self.source, "--check")
        for name, record in self.manifest["files"].items():
            self.assertEqual(digest(self.source / name), record["after_sha256"])
        mtimes = {name: (self.source / name).stat().st_mtime_ns for name in before}
        self.call(self.source)
        self.assertEqual(mtimes, {name: (self.source / name).stat().st_mtime_ns for name in before})

    def test_mixed_docker_source_overlay(self):
        self.call(self.source)
        # COPY of the pinned source restores existing files but leaves new patch
        # files and dependency build products present in the destination.
        for name, record in self.manifest["files"].items():
            if record["before_sha256"] is not None:
                shutil.copy2(self.template / name, self.source / name)
        artifact = self.source / "lib/test-built-artifact"
        artifact.parent.mkdir()
        artifact.write_bytes(b"preserve")
        self.call(self.source)
        self.call(self.source, "--check")
        self.assertEqual(artifact.read_bytes(), b"preserve")

    def test_unknown_edits_reject_before_any_changes(self):
        name = next(name for name, record in self.manifest["files"].items()
                    if record["before_sha256"] is not None)
        with (self.source / name).open("ab") as stream:
            stream.write(b"\nlocal edit\n")
        before = self.hashes()
        result = self.call(self.source, success=False)
        self.assertIn("differs from pinned/patched input", result.stderr)
        self.assertEqual(before, self.hashes())

    def test_symlink_rejected(self):
        name = next(name for name, record in self.manifest["files"].items()
                    if record["before_sha256"] is not None)
        target = self.source / name
        original = target.read_bytes()
        target.unlink()
        target.symlink_to(self.template / name)
        self.call(self.source, success=False)
        self.assertEqual((self.template / name).read_bytes(), original)

    def test_inherited_git_state_ignored(self):
        env = dict(os.environ, GIT_DIR="/not-a-repository", GIT_WORK_TREE="/not-a-tree",
                   GIT_INDEX_FILE="/not-an-index", GIT_COMMON_DIR="/not-common")
        self.call(self.source, env=env)
        self.call(self.source, "--check")

    def test_patch_checksum_rejected_before_changes(self):
        project = Path(self.work.name) / "project"
        (project / "scripts").mkdir(parents=True)
        shutil.copy2(self.script, project / "scripts" / self.script.name)
        shutil.copytree(PROJECT / "patches", project / "patches")
        patch = project / "patches/pvxs" / self.manifest["patch"]
        with patch.open("ab") as stream:
            stream.write(b"corrupt")
        before = self.hashes()
        self.call(self.source, script=project / "scripts" / self.script.name, success=False)
        self.assertEqual(before, self.hashes())


if __name__ == "__main__":
    import sys
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    args, rest = parser.parse_known_args()
    SOURCE = args.source.resolve()
    PROJECT = Path(__file__).resolve().parents[1]
    PYTHON = sys.executable
    unittest.main(argv=[sys.argv[0], *rest])
