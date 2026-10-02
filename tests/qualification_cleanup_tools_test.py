"""Offline daemon-side success/lost-return tests. Never call real Docker."""
import importlib.util
import json
import os
from pathlib import Path
from types import SimpleNamespace
import tempfile
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("qualify_cleanup", ROOT / "scripts/qualify-image.py")
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)
IDENTITY = dict(scope="qualification-100-2-aaaaaaaaaaaa", revision="a" * 40, version="0.9.0", image="fixture",
                run_id=100, run_attempt=2, platform="linux/amd64")


class FakeDaemon:
    def __init__(self, folder):
        self.folder = folder
        self.containers, self.networks, self.images, self.volumes = {}, {}, {}, {}
        self.events, self.lost, self.exception = [], None, KeyboardInterrupt("lost CLI return")

    def fail_after(self, kind):
        if self.lost == kind:
            self.lost = None
            raise self.exception

    def container(self, name, owner=True, builder=False):
        identifier = "c" * 62 + format(len(self.containers) + 1, "02x")
        labels = {collector.OWNER: IDENTITY["scope"] if owner else "production"}
        config = dict(Labels=labels, Env=[], Image="fixture")
        if builder:
            config = dict(Labels={}, Env=[collector.BUILDER_OWNER + "=" + IDENTITY["scope"]], Image=collector.BUILDKIT_IMAGE)
        self.containers[identifier] = dict(Id=identifier, Name="/" + name, Config=config,
            State=dict(Running=True, Paused=False), HostConfig={}, Image="sha256:" + "d" * 64)
        return identifier

    def command(self, *args, **kwargs):
        self.events.append(args)
        if args[:2] == ("docker", "ps"):
            name = args[args.index("--filter") + 1].split("=", 1)[1]
            return "\n".join(identifier for identifier, value in self.containers.items() if name in value["Name"])
        if args[:3] == ("docker", "network", "ls"):
            name = args[args.index("--filter") + 1].split("=", 1)[1]
            return "\n".join(identifier for identifier, value in self.networks.items() if name in value["Name"])
        if args[:3] == ("docker", "image", "ls"):
            name = args[args.index("--filter") + 1].split("=", 1)[1]
            return "\n".join(value["Id"] for tag, value in self.images.items() if tag == name)
        if args[:3] == ("docker", "volume", "ls"):
            name = args[args.index("--filter") + 1].split("=", 1)[1]
            return "\n".join(tag for tag in self.volumes if tag == name)
        if args[:2] == ("docker", "inspect"):
            return json.dumps([self.containers[args[-1]]])
        if args[:3] == ("docker", "network", "inspect"):
            value = dict(self.networks[args[-1]])
            value["Containers"] = {key: {} for key, container in self.containers.items()
                                   if container["HostConfig"].get("NetworkMode") == args[-1]}
            return json.dumps([value])
        if args[:3] == ("docker", "image", "inspect"):
            return json.dumps([next(value for value in self.images.values() if value["Id"] == args[-1])])
        if args[:3] == ("docker", "volume", "inspect"):
            return json.dumps([self.volumes[args[-1]]])
        if args[:3] == ("docker", "network", "create"):
            identifier = "a" * 64
            self.networks[identifier] = dict(Id=identifier, Name=args[-1], Internal=True, Labels={collector.OWNER: IDENTITY["scope"]})
            self.fail_after("network")
            return identifier
        if args[:2] == ("docker", "create"):
            name = args[args.index("--name") + 1]
            identifier = self.container(name)
            self.containers[identifier]["HostConfig"]["NetworkMode"] = args[args.index("--network") + 1]
            self.fail_after("container")
            return identifier
        if args[:2] == ("docker", "start"):
            return args[-1]
        if args[:2] == ("docker", "rm"):
            del self.containers[args[-1]]
            return args[-1]
        if args[:3] == ("docker", "network", "rm"):
            del self.networks[args[-1]]
            return args[-1]
        if args[:3] == ("docker", "image", "rm"):
            del self.images[args[-1]]
            return "removed"
        if args[:3] == ("docker", "buildx", "create"):
            name = args[args.index("--name") + 1]
            opts = dict(part.split("=", 1) for part in args[args.index("--driver-opt") + 1].split(","))
            path = self.folder / "buildx/instances" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(dict(Name=name, Driver="docker-container", Nodes=[dict(Name=name + "0", DriverOpts=opts)])))
            self.fail_after("builder")
            return name
        if args[:3] == ("docker", "buildx", "build"):
            name = args[args.index("--builder") + 1]
            self.container("buildx_buildkit_" + name + "0", builder=True)
            volume = "buildx_buildkit_" + name + "0_state"
            self.volumes[volume] = dict(Name=volume)
            tag = args[args.index("--tag") + 1]
            self.images[tag] = dict(Id="sha256:" + "d" * 64, RepoTags=[tag], Config=dict(Labels={collector.OWNER: IDENTITY["scope"]}))
            self.fail_after("build")
            return ""
        if args[:3] == ("docker", "buildx", "rm"):
            name = args[-1]
            (self.folder / "buildx/instances" / name).unlink()
            node = "buildx_buildkit_" + name + "0"
            for identifier in list(self.containers):
                if self.containers[identifier]["Name"] == "/" + node:
                    del self.containers[identifier]
            self.volumes.pop(node + "_state", None)
            return "removed"
        raise AssertionError("unexpected fake daemon command: " + str(args))


class PlannedCleanupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="qualification-cleanup-unit-")
        self.addCleanup(self.temporary.cleanup)
        self.folder = Path(self.temporary.name)
        (self.folder / "buildx").mkdir()
        self.daemon = FakeDaemon(self.folder)
        self.env = patch.dict(os.environ, dict(GITHUB_RUN_ID="100", GITHUB_RUN_ATTEMPT="2", BUILDX_CONFIG=str(self.folder / "buildx")))
        self.env.start(); self.addCleanup(self.env.stop)
        self.mock = patch.object(collector, "command", side_effect=self.daemon.command)
        self.mock.start(); self.addCleanup(self.mock.stop)
        self.scope = collector.Scope(IDENTITY, self.folder)

    def clean(self):
        collector.cleanup(self.folder)
        self.assertEqual(self.daemon.containers, {})
        self.assertEqual(self.daemon.networks, {})
        self.assertEqual(self.daemon.images, {})
        self.assertEqual(self.daemon.volumes, {})

    def test_daemon_container_creation_with_lost_signal_return_is_recovered(self):
        self.scope.start()
        self.daemon.lost = "container"
        with self.assertRaises(KeyboardInterrupt):
            self.scope.create("ioc", "fixture", [])
        plan = json.loads((self.folder / "runtime-ownership.json").read_text())
        self.assertEqual(plan["containers"], {})
        self.assertEqual(plan["planned"]["containers"], {"ioc": IDENTITY["scope"] + "-ioc"})
        self.clean()
        self.assertTrue(any(args[:2] == ("docker", "rm") for args in self.daemon.events))

    def test_daemon_network_creation_with_lost_timeout_return_is_recovered(self):
        import subprocess
        self.daemon.lost, self.daemon.exception = "network", subprocess.TimeoutExpired(["docker"], 30)
        with self.assertRaises(subprocess.TimeoutExpired):
            self.scope.start()
        self.clean()

    def test_builder_metadata_creation_with_lost_return_is_recovered_before_bootstrap(self):
        self.daemon.lost = "builder"
        with self.assertRaises(KeyboardInterrupt):
            self.scope.create_builder()
        self.clean()
        self.assertFalse(list((self.folder / "buildx/instances").iterdir()))

    def test_lost_build_return_recovers_labeled_output_and_bootstrapped_builder_state(self):
        import subprocess
        builder = self.scope.create_builder()
        tag = "redis-pvxs-qualification-tools:" + IDENTITY["scope"]
        self.scope.plan_image(tag)
        self.daemon.lost, self.daemon.exception = "build", subprocess.TimeoutExpired(["docker"], 30)
        with self.assertRaises(subprocess.TimeoutExpired):
            collector.command("docker", "buildx", "build", "--builder", builder, "--tag", tag)
        self.clean()

    def test_existing_container_collision_is_never_created_over_or_removed(self):
        self.scope.start()
        identifier = self.daemon.container(IDENTITY["scope"] + "-ioc", owner=False)
        with self.assertRaisesRegex(ValueError, "existing container"):
            self.scope.create("ioc", "fixture", [])
        self.scope.close()
        self.assertIn(identifier, self.daemon.containers)
        self.assertFalse(any(args[:2] == ("docker", "rm") for args in self.daemon.events))

    def test_recovered_owner_collision_is_not_removed(self):
        self.scope.start()
        self.daemon.lost = "container"
        with self.assertRaises(KeyboardInterrupt):
            self.scope.create("ioc", "fixture", [])
        info = next(iter(self.daemon.containers.values()))
        info["Config"]["Labels"][collector.OWNER] = "production"
        with self.assertRaisesRegex(ValueError, "owner collision"):
            collector.cleanup(self.folder)
        self.assertFalse(any(args[:2] == ("docker", "rm") for args in self.daemon.events))

    def test_builder_daemon_marker_collision_is_not_removed(self):
        builder = self.scope.create_builder()
        tag = "redis-pvxs-qualification-tools:" + IDENTITY["scope"]
        self.scope.plan_image(tag)
        collector.command("docker", "buildx", "build", "--builder", builder, "--tag", tag)
        next(iter(self.daemon.containers.values()))["Config"]["Env"] = []
        with self.assertRaisesRegex(ValueError, "owner marker collision"):
            collector.cleanup(self.folder)
        self.assertFalse(any(args[:3] == ("docker", "buildx", "rm") for args in self.daemon.events))

    def test_interrupted_post_create_atomic_update_preserves_readable_prior_plan(self):
        original = Path.replace
        count = 0
        def interrupted(path, target):
            nonlocal count
            count += 1
            if count == 2: # pre-create planned write succeeds; post-create replacement is lost
                raise KeyboardInterrupt("interrupted atomic journal update")
            return original(path, target)
        with patch.object(Path, "replace", interrupted), self.assertRaises(KeyboardInterrupt):
            self.scope.start()
        plan = json.loads((self.folder / "runtime-ownership.json").read_text())
        self.assertIsNone(plan["network"])
        self.assertEqual(plan["planned"]["network"], IDENTITY["scope"])
        self.clean()

    def test_closed_cleanup_is_byte_identical_and_performs_no_daemon_lookup(self):
        self.scope.close()
        path = self.folder / "runtime-ownership.json"
        before, operations = path.read_bytes(), len(self.daemon.events)
        collector.cleanup(self.folder)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(len(self.daemon.events), operations)

    def test_stop_failure_still_closes_scope_and_preserves_python39_primary(self):
        class LegacyError(Exception):
            add_note = None # model the host Python 3.9 exception interface
        actions = []
        class StubScope:
            def __init__(self, *args): self.images = []
            def create_builder(self): return "builder"
            def plan_image(self, name): pass
            def persist(self): pass
            def start(self): pass
            def create(self, *args, **kwargs): pass
            def close(self): actions.append("scope closed")
        class StubCollector:
            def __init__(self, *args): pass
            def eventually(self, *args): pass
            def collect(self): raise LegacyError("original collection failure")
            def stop(self): actions.append("stop failed"); raise ValueError("stop failure")
        prepare = (dict(IDENTITY), {}, {}, {})
        with patch.object(collector, "prepare", return_value=prepare), patch.object(collector, "Scope", StubScope), \
             patch.object(collector, "Collector", StubCollector), patch.object(collector, "command", return_value=json.dumps([dict(Os="linux", Architecture="amd64")])):
            with self.assertRaisesRegex(LegacyError, "original collection failure"):
                collector.run(SimpleNamespace(output=self.folder))
        self.assertEqual(actions, ["stop failed", "scope closed"])
        evidence = json.loads((self.folder / "cleanup-errors.json").read_text())
        self.assertEqual(evidence["primary"], "original collection failure")
        self.assertIn("stop failure", evidence["cleanup"][0])
        self.assertFalse((self.folder / "qualification.json").exists())


if __name__ == "__main__":
    unittest.main()
