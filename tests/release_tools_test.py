import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("release_image", ROOT / "scripts/release-image.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
SHA = "a" * 40
DIGEST = "sha256:" + "b" * 64
IMAGE = release.IMAGE_REPOSITORY + "@" + DIGEST


class ReleaseTests(unittest.TestCase):
    def test_python_and_cmake_accept_the_same_semver(self):
        versions = {
            "0.8.2": True, "0.9.0-rc.1": True, "1.0.0-alpha.0+build.01": True,
            "1.0.0-0a": True, "1.0.0+001": True,
            "01.0.0": False, "1.00.0": False, "1.0.0-01": False,
            "1.0.0-rc..1": False, "1.0.0+": False, "1.0.0-": False,
            "1.0.0-a_1": False, "1.0": False, "v1.0.0": False,
        }
        for version, valid in versions.items():
            with self.subTest(version=version):
                try:
                    release.version_parts(version)
                    accepted = True
                except ValueError:
                    accepted = False
                self.assertEqual(accepted, valid)
                result = subprocess.run(["cmake", "-DREDIS_PVXS_IOC_VERSION=" + version,
                                         "-P", str(ROOT / "cmake/ProjectVersion.cmake")],
                                        capture_output=True)
                self.assertEqual(result.returncode == 0, valid, result.stderr.decode())

    def evidence(self):
        record = dict(version="0.9.0", revision=SHA, image=IMAGE, run_id=123,
                      run_attempt=2, platform="linux/amd64", checks=list(release.CANDIDATE_CHECKS),
                      evidence={name: "d" * 64 for name in release.CANDIDATE_EVIDENCE})
        workflow = dict(conclusion="success", head_sha=SHA, event="workflow_dispatch",
                        path=".github/workflows/candidate-image.yml", id=123, run_attempt=2)
        return record, workflow

    def test_evidence_rejects_wrong_source_attempt_workflow_or_failed_checks(self):
        record, workflow = self.evidence()
        release.verify_record(record, workflow, "0.9.0", SHA, "candidate")
        for key, value in {"revision": "c" * 40, "version": "0.9.0-rc.1",
                           "run_attempt": 1, "platform": "linux/arm64", "checks": ["image"],
                           "evidence": {}, "image": "other@" + DIGEST}.items():
            with self.subTest(record=key), self.assertRaises(ValueError):
                release.verify_record(dict(record, **{key: value}), workflow, "0.9.0", SHA, "candidate")
        for key, value in {"head_sha": "c" * 40, "conclusion": "failure", "event": "pull_request",
                           "path": ".github/workflows/unrelated.yml"}.items():
            with self.subTest(workflow=key), self.assertRaises(ValueError):
                release.verify_record(record, dict(workflow, **{key: value}), "0.9.0", SHA, "candidate")

    def test_qualification_requires_soak_rollback_and_integration_evidence(self):
        record, workflow = self.evidence()
        workflow["path"] = ".github/workflows/qualify-image.yml"
        record.update(checks=list(release.QUALIFICATION_CHECKS), soak_seconds=86400, rollback_version="0.8.2")
        release.verify_record(record, workflow, "0.9.0", SHA, "qualification")
        for updates in ({"soak_seconds": 86399}, {"rollback_version": "0.8.1"}, {"checks": []}):
            with self.subTest(updates=updates), self.assertRaises(ValueError):
                release.verify_record(dict(record, **updates), workflow, "0.9.0", SHA, "qualification")

    def test_invalid_image_identity_fails_before_runtime_execution(self):
        labels = {"org.opencontainers.image." + k: v for k, v in
                  dict(version="0.8.2", revision=SHA, source=release.SOURCE, licenses="BSD-3-Clause").items()}
        info = dict(Os="linux", Architecture="amd64", Config=dict(Labels=labels))
        for mutation in ("architecture", "revision", "version"):
            broken = copy.deepcopy(info)
            if mutation == "architecture":
                broken["Architecture"] = "arm64"
            else:
                broken["Config"]["Labels"]["org.opencontainers.image." + mutation] = "wrong"
            with self.subTest(mutation=mutation), patch.object(release, "run", return_value=json.dumps([broken])) as run:
                with self.assertRaises(ValueError):
                    release.validate_image(IMAGE, "0.8.2", SHA)
                self.assertEqual(run.call_count, 1)

    def test_attestations_require_sbom_full_provenance_and_matching_revision(self):
        values = {
            "SBOM": {"linux/amd64": {"SPDX": {"spdxVersion": "SPDX-2.3", "packages": [{"name": "test"}]}}},
            "Provenance": {"linux/amd64": {"SLSA": {"buildConfig": {"llbDefinition": [1]},
                "materials": [{"uri": "test"}], "invocation": {"parameters": {
                    "args": {"build-arg:REDIS_PVXS_IOC_REVISION": SHA}}}}}},
            "Manifest": {"digest": DIGEST},
        }
        def command(*args):
            return json.dumps(values[args[-1].removeprefix("{{json .").removesuffix("}}")])
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "source-inputs.json").write_text(json.dumps({
                "repository": release.SOURCE, "revision": SHA, "submodules": [{"path": "test", "revision": SHA}]}))
            with patch.object(release, "run", side_effect=command):
                hashes = release.capture_attestations(IMAGE, SHA, directory)
                self.assertEqual(set(hashes), release.CANDIDATE_EVIDENCE)
                with self.assertRaisesRegex(ValueError, "revision"):
                    release.capture_attestations(IMAGE, "c" * 40, directory)
                values["Provenance"]["linux/amd64"]["SLSA"].pop("buildConfig")
                with self.assertRaisesRegex(ValueError, "full build provenance"):
                    release.capture_attestations(IMAGE, SHA, directory)
                values["SBOM"] = None
                with self.assertRaisesRegex(ValueError, "missing image attestation"):
                    release.capture_attestations(IMAGE, SHA, directory)

    def test_source_inventory_rejects_uninitialized_or_changed_submodules(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "source-inputs.json"
            for prefix in ("-", "+", "U"):
                with self.subTest(prefix=prefix), patch.object(release, "run", return_value=""), \
                        patch.object(release.subprocess, "check_output", return_value=prefix + SHA + " third_party/test\n"):
                    with self.assertRaisesRegex(ValueError, "pinned revisions"):
                        release.source_inputs(output)
            with patch.object(release, "run", return_value=" M Dockerfile"):
                with self.assertRaisesRegex(ValueError, "clean checkout"):
                    release.source_inputs(output)

    def test_prerelease_promotion_preserves_digest_and_never_touches_latest(self):
        manifests = {}
        calls = []
        def command(*args):
            calls.append(args)
            self.assertEqual(args[:5], ("docker", "buildx", "imagetools", "create", "--prefer-index=false"))
            self.assertEqual(args[-1], IMAGE)
            manifests[args[-2]] = DIGEST
            return ""
        with patch.object(release, "registry_digest", side_effect=manifests.get), patch.object(release, "run", side_effect=command):
            tag, latest = release.promote_digest(IMAGE, "0.9.0-rc.1+build.2")
        self.assertFalse(latest)
        self.assertTrue(tag.endswith(":v0.9.0-rc.1_build.2"))
        self.assertEqual(len(calls), 1)

    def test_release_tag_cannot_be_overwritten(self):
        with patch.object(release, "registry_digest", return_value="sha256:" + "c" * 64), patch.object(release, "run") as run:
            with self.assertRaises(ValueError):
                release.promote_digest(IMAGE, "0.8.2")
            run.assert_not_called()

    def test_old_release_cannot_move_latest_backward(self):
        target = release.IMAGE_REPOSITORY + ":v0.8.2"
        manifests = {release.IMAGE_REPOSITORY + ":latest": DIGEST}
        calls = []
        def command(*args):
            calls.append(args)
            if args[:3] == ("docker", "image", "inspect"):
                return json.dumps([{"Config": {"Labels": {"org.opencontainers.image.version": "0.9.0"}}}])
            if "create" in args:
                manifests[args[-2]] = DIGEST
            return ""
        with patch.object(release, "registry_digest", side_effect=manifests.get), patch.object(release, "run", side_effect=command):
            self.assertEqual(release.promote_digest(IMAGE, "0.8.2"), (target, False))
        self.assertEqual([c[-2] for c in calls if "create" in c], [target])

    def test_stable_release_rejects_missing_qualification_before_promotion(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "VERSION").write_text("0.9.0\n")
            (root / "CHANGELOG.md").write_text("## v0.9.0\n\nChanges.\n")
            with patch.object(release, "ROOT", root), patch.object(release, "run", return_value=SHA), \
                    patch.object(release, "load_record", return_value=self.evidence()[0]), \
                    patch.object(release, "promote_digest") as promote:
                with self.assertRaisesRegex(ValueError, "qualification"):
                    release.release("v0.9.0", "123", "")
                promote.assert_not_called()

    def test_existing_draft_is_published_with_final_stable_classification(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "VERSION").write_text("0.8.2\n")
            (root / "CHANGELOG.md").write_text("## v0.8.2\n\nCorrectness fixes.\n")
            record = dict(self.evidence()[0], version="0.8.2")
            def candidate_record(*args):
                candidate = args[-1] / "candidate"
                candidate.mkdir()
                (candidate / "candidate.json").write_text(json.dumps(record))
                (candidate / "smoke.log").write_text("passed\n")
                return record
            with patch.object(release, "ROOT", root), patch.object(release, "run", return_value=SHA), \
                    patch.object(release, "load_record", side_effect=candidate_record), \
                    patch.object(release, "validate_image"), \
                    patch.object(release, "existing_release", return_value={"isDraft": True}), \
                    patch.object(release, "verify_release_assets") as verify, \
                    patch.object(release, "promote_digest", return_value=(release.IMAGE_REPOSITORY + ":v0.8.2", True)), \
                    patch.object(release.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as calls:
                release.release("v0.8.2", "123", "")
            edits = [call.args[0] for call in calls.call_args_list
                     if call.args[0][:3] == ["gh", "release", "edit"] and "--draft=false" in call.args[0]]
            self.assertEqual(len(edits), 1)
            self.assertIn("--draft=false", edits[0])
            self.assertIn("--prerelease=false", edits[0])
            self.assertIn("--latest=true", edits[0])
            uploads = [call.args[0] for call in calls.call_args_list if call.args[0][:3] == ["gh", "release", "upload"]]
            self.assertTrue(any(any(arg.endswith("release-evidence.tar.gz") for arg in call) for call in uploads))
            verify.assert_called_once()

    def test_assets_are_uploaded_and_verified_before_new_draft_or_retry_is_published(self):
        for existing in (None, {"isDraft": True}, {"isDraft": False}):
            with self.subTest(existing=existing), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                notes = root / "notes.md"
                assets = [root / name for name in ("candidate.json", "qualification.json", "release-evidence.tar.gz")]
                events = []
                def command(args, **kwargs):
                    events.append(args)
                    return subprocess.CompletedProcess(args, 0)
                def verified(tag, uploaded):
                    self.assertEqual(uploaded, assets)
                    events.append(["verified"])
                with patch.object(release, "existing_release", return_value=existing), \
                        patch.object(release.subprocess, "run", side_effect=command), \
                        patch.object(release, "verify_release_assets", side_effect=verified):
                    release.publish_release("v0.9.0", notes, assets, False, True)
                upload = next(i for i, event in enumerate(events) if event[:3] == ["gh", "release", "upload"])
                verification = events.index(["verified"])
                publication = next(i for i, event in enumerate(events) if "--draft=false" in event)
                self.assertLess(upload, verification)
                self.assertLess(verification, publication)
                self.assertIn("--latest=true", events[publication])
                if existing is None:
                    self.assertIn("--draft", events[0])
                elif not existing["isDraft"]:
                    self.assertFalse(any("--draft=true" in event for event in events))

    def test_upload_or_verification_failure_leaves_a_new_release_unpublished(self):
        for failure in ("upload", "verify"):
            with self.subTest(failure=failure):
                events = []
                def command(args, **kwargs):
                    events.append(args)
                    if failure == "upload" and args[:3] == ["gh", "release", "upload"]:
                        raise subprocess.CalledProcessError(1, args)
                    return subprocess.CompletedProcess(args, 0)
                def verified(*args):
                    if failure == "verify":
                        raise ValueError("uploaded release evidence differs")
                with patch.object(release, "existing_release", return_value=None), \
                        patch.object(release.subprocess, "run", side_effect=command), \
                        patch.object(release, "verify_release_assets", side_effect=verified):
                    with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                        release.publish_release("v0.9.0", Path("notes"), [Path("candidate.json")], False, True)
                self.assertFalse(any("--draft=false" in event or "--latest=true" in event for event in events))

    def test_downloaded_asset_bytes_must_match_the_validated_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            asset = Path(temporary) / "candidate.json"
            asset.write_bytes(b"validated evidence")
            for altered in (False, True):
                def download(*args):
                    directory = Path(args[args.index("--dir") + 1])
                    (directory / asset.name).write_bytes(b"different bytes" if altered else asset.read_bytes())
                    return ""
                with self.subTest(altered=altered), patch.object(release, "run", side_effect=download):
                    if altered:
                        with self.assertRaisesRegex(ValueError, "evidence differs"):
                            release.verify_release_assets("v0.9.0", [asset])
                    else:
                        release.verify_release_assets("v0.9.0", [asset])

    def test_published_assets_are_verified_without_clobbering_or_withdrawing_them(self):
        asset = Path("candidate.json")
        info = {"isDraft": False, "assets": [{"name": asset.name}]}
        with patch.object(release, "existing_release", return_value=info), \
                patch.object(release, "verify_release_assets", side_effect=ValueError("mismatched bytes")), \
                patch.object(release.subprocess, "run") as commands:
            with self.assertRaisesRegex(ValueError, "mismatched bytes"):
                release.publish_release("v0.9.0", Path("notes"), [asset], False, True)
            commands.assert_not_called()
        with patch.object(release, "existing_release", return_value=info), \
                patch.object(release, "verify_release_assets"), \
                patch.object(release.subprocess, "run") as commands:
            release.publish_release("v0.9.0", Path("notes"), [asset], False, True)
            self.assertFalse(any("upload" in c.args[0] or "--draft=true" in c.args[0] for c in commands.call_args_list))

    def test_evidence_archive_is_reproducible_across_extraction_timestamps(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "candidate").mkdir()
            asset = root / "candidate/candidate.json"
            asset.write_text("same validated bytes")
            before = release.evidence_archive(root, "").read_bytes()
            os.utime(asset, (123456789, 123456789))
            os.utime(root / "candidate", (987654321, 987654321))
            self.assertEqual(release.evidence_archive(root, "").read_bytes(), before)

    def test_release_lookup_failure_is_not_treated_as_an_absent_release(self):
        failure = subprocess.CompletedProcess([], 1, "", "authentication failed (HTTP 401)")
        with patch.object(release.subprocess, "run", return_value=failure):
            with self.assertRaisesRegex(RuntimeError, "cannot inspect"):
                release.existing_release("v0.9.0")
        missing = subprocess.CompletedProcess([], 1, "", "release not found")
        with patch.object(release.subprocess, "run", return_value=missing):
            self.assertIsNone(release.existing_release("v0.9.0"))


class SmokeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.log = self.directory / "commands.jsonl"
        docker = self.directory / "docker"
        docker.write_text('''#!/usr/bin/env python3
import json, os, sys
with open(os.environ["TEST_DOCKER_LOG"], "a") as log:
    log.write(json.dumps(sys.argv[1:]) + "\\n")
if sys.argv[1:3] == ["image", "inspect"]:
    sys.exit(int(os.environ.get("TEST_INSPECT_RESULT", "0")))
if sys.argv[1] == "compose" and "up" in sys.argv:
    sys.exit(42)
''')
        docker.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.directory) + os.pathsep + os.environ["PATH"],
                        TEST_DOCKER_LOG=str(self.log))

    def commands(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def pull(self, image, policy="auto", cached=True):
        self.env["TEST_INSPECT_RESULT"] = "0" if cached else "1"
        return subprocess.run(["bash", "-c", 'source "$1"; ensure_test_image "$2" "$3"',
                               "test", str(ROOT / "scripts/image-pull.sh"), image, policy],
                              env=self.env, capture_output=True)

    def test_cached_remote_tag_is_refreshed(self):
        self.assertEqual(self.pull("registry/project:candidate").returncode, 0)
        self.assertEqual(self.commands(), [["pull", "registry/project:candidate"]])

    def test_cached_digest_is_reused(self):
        self.assertEqual(self.pull(IMAGE).returncode, 0)
        self.assertEqual(self.commands(), [["image", "inspect", IMAGE]])

    def test_missing_digest_is_pulled(self):
        self.assertEqual(self.pull(IMAGE, cached=False).returncode, 0)
        self.assertEqual(self.commands(), [["image", "inspect", IMAGE], ["pull", IMAGE]])

    def test_local_override_cannot_silently_pull(self):
        self.assertNotEqual(self.pull("local:build", "never", cached=False).returncode, 0)
        self.assertEqual(self.commands(), [["image", "inspect", "local:build"]])

    def test_invalid_policy_fails_without_docker(self):
        self.assertNotEqual(self.pull("local:build", "invalid").returncode, 0)
        self.assertEqual(self.commands(), [])

    def test_failed_smoke_startup_cleans_only_its_own_unique_project(self):
        self.env.update(COMPOSE_PROJECT_NAME="production", COMPOSE_FILE="production.yml",
                        REDIS_CONTAINER_NAME="production-redis", REDIS_PVXS_IOC_CONTAINER_NAME="production-ioc",
                        REDIS_PVXS_IOC_IMAGE="local:build", REDIS_PVXS_IOC_PULL_POLICY="never")
        projects = []
        for _ in range(2):
            self.log.write_text("")
            result = subprocess.run(["bash", str(ROOT / "scripts/smoke-test.sh")], env=self.env, capture_output=True)
            self.assertEqual(result.returncode, 42, result.stderr.decode())
            commands = [c for c in self.commands() if c[0] == "compose"]
            project = commands[0][2]
            projects.append(project)
            self.assertTrue(project.startswith("smoke-run-"))
            self.assertTrue(any("down" in c for c in commands))
            for command in commands:
                self.assertEqual(command[1:5], ["--project-name", project, "--file", str(ROOT / "tests/smoke.compose.yml")])
                self.assertNotIn("production", command)
        self.assertNotEqual(*projects)


if __name__ == "__main__":
    unittest.main()
