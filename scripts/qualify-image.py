#!/usr/bin/env python3
"""Collect qualification of an immutable candidate in a private owned runtime.

Dispatch only through qualify-image.yml on the exact reviewed main revision.
There are no check/soak assertion inputs or existing-container inputs. This
program writes qualification.json only after revalidating its complete proof.
"""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import secrets
import signal
import shutil
import subprocess
import sys
import threading
import time

import qualification_contract as contract
import qualification_evidence as github

ROOT = Path(__file__).resolve().parents[1]
OWNER = "org.fermi-ad.redis-pvxs-ioc.qualification"
BUILDER_OWNER = "REDIS_PVXS_QUALIFICATION_SCOPE"
REDIS_IMAGE = "redis:7.4.2@sha256:fbdbaea47b9ae4ecc2082ecdb4e1cea81e32176ffb1dcf643d422ad07427e5d9"
BUILDKIT_IMAGE = "moby/buildkit@sha256:28a898719c18a33f4e8000685287fa36fd0dd9560c6440227d3a732d79bb41d8"
WORK = "/opt/redis-pvxs-ioc"


def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="microseconds")


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def command(*argv, timeout=30, log=None):
    if log:
        # Docker build output is bounded too; losing a log invalidates the run.
        with log.open("xb") as stream:
            process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            import selectors
            count, deadline = 0, time.monotonic() + timeout
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout, selectors.EVENT_READ)
                    while True:
                        contract.require(time.monotonic() < deadline, "command timed out")
                        if not selector.select(1):
                            continue
                        chunk = os.read(process.stdout.fileno(), 65536)
                        if not chunk:
                            break
                        count += len(chunk)
                        contract.require(count <= contract.MAX_FILE_BYTES, "command log exceeds evidence bound")
                        stream.write(chunk)
                    contract.require(process.wait(timeout=max(0.1, deadline - time.monotonic())) == 0, "command failed; see " + log.name)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                process.stdout.close()
        return ""
    result = subprocess.run(argv, text=True, capture_output=True, timeout=timeout)
    contract.require(len(result.stdout) + len(result.stderr) <= 8 * 1024**2, "command output exceeds bound")
    contract.require(result.returncode == 0, "command failed: " + " ".join(argv[:3]) + ": " + result.stderr[-2000:])
    return result.stdout.strip()


def baseline_evidence(directory):
    info = github.api(f"repos/{contract.REPOSITORY}/releases/tags/v0.8.2")
    contract.require(info.get("draft") is False and info.get("prerelease") is False
                     and info.get("published_at"), "published qualified v0.8.2 must exist first")
    assets = {a["name"]: a for a in info.get("assets", [])}
    contract.require({"candidate.json", "release-evidence.tar.gz"}.issubset(assets), "v0.8.2 qualification assets missing")
    folder = directory / "rollback"
    folder.mkdir()
    write(folder / "release.json", info)
    for name in ("candidate.json", "release-evidence.tar.gz"):
        asset = assets[name]
        contract.integer(asset.get("size"), "release asset size", 1, contract.MAX_BUNDLE_BYTES)
        github.download(f"repos/{contract.REPOSITORY}/releases/assets/{contract.integer(asset.get('id'), 'release asset ID', 1)}",
                        folder / name, limit=contract.MAX_BUNDLE_BYTES, binary=True)
        if asset.get("digest"):
            import hashlib
            contract.require(asset["digest"] == "sha256:" + hashlib.sha256((folder / name).read_bytes()).hexdigest(),
                             "published rollback asset checksum mismatch")
    archive = folder / "release-evidence.tar.gz"
    extracted = folder / "published-evidence"
    contract.extract_archive(archive, extracted, kind="tar")
    contract.require((folder / "candidate.json").read_bytes() == (extracted / "candidate/candidate.json").read_bytes(),
                     "published candidate differs from retained release archive")
    archive.unlink()
    # The baseline's full candidate inventory and attestations survive Actions
    # artifact expiry because they are retained in its published release.
    candidate = contract.read(folder, "candidate.json")
    for name in contract.ATTESTATIONS:
        shutil.copyfile(extracted / "candidate" / name, folder / name)
    shutil.rmtree(extracted)
    run_info = github.retained_run(candidate["run_id"], candidate["revision"], "candidate-image.yml",
                                  folder / "run.json", dispatch_only=True, published_baseline=True)
    contract.validate_candidate(candidate, directory, "rollback/", run_info, "0.8.2", candidate["revision"])
    # Verify the published tag resolves to the actual qualified baseline source.
    ref = github.api(f"repos/{contract.REPOSITORY}/git/ref/tags/v0.8.2")["object"]
    if ref["type"] == "tag":
        ref = github.api(f"repos/{contract.REPOSITORY}/git/tags/{ref['sha']}")["object"]
    contract.require(ref.get("type") == "commit" and ref.get("sha") == candidate["revision"], "rollback tag/source mismatch")
    write(folder / "tag.json", ref)
    return candidate


def prepare(args):
    contract.require(os.environ.get("GITHUB_REF") == "refs/heads/main"
                     and os.environ.get("GITHUB_EVENT_NAME") == "workflow_dispatch"
                     and os.environ.get("GITHUB_REPOSITORY") == contract.REPOSITORY,
                     "qualification executes only in the trusted-main workflow")
    revision = command("git", "rev-parse", "HEAD")
    contract.require(revision == os.environ.get("GITHUB_SHA") and contract.SHA.fullmatch(revision), "workflow checkout identity mismatch")
    contract.require((ROOT / "VERSION").read_text().strip() == "0.9.0", "only exact final VERSION=0.9.0 is supported")
    command("git", "merge-base", "--is-ancestor", revision, "origin/main")
    contract.require(not command("git", "status", "--porcelain", "--untracked-files=all"), "qualification source checkout must be clean")
    # These features are deliberately absent on the 0.8.x preparation base.
    # Refuse missing tools/integration coverage instead of writing weaker proof.
    for name in ("tests/ndarray_transfer.cpp", "tools/stream_capacity.cpp", "tests/source_health_pva_tests.cpp",
                 "tests/endpoint_rpc_e2e.py", "tests/discovery_e2e.py", ".github/workflows/native.yml"):
        contract.require((ROOT / name).is_file(), "merged qualification capability missing: " + name)
    contract.require(not args.output.exists() and args.output.is_absolute(), "choose a new absolute private evidence directory")
    args.output.mkdir(mode=0o700, parents=True)
    policy = contract.parse_json((ROOT / "docs/qualification-policy.json").read_bytes())
    contract.validate_policy(policy)
    shutil.copyfile(ROOT / "docs/qualification-policy.json", args.output / "policy.json")
    (args.output / "tooling").mkdir()
    shutil.copyfile(ROOT / "scripts/qualification-timings.patch", args.output / "tooling/qualification-timings.patch")
    run_id, attempt = int(os.environ["GITHUB_RUN_ID"]), int(os.environ["GITHUB_RUN_ATTEMPT"])
    own = github.api(f"repos/{contract.REPOSITORY}/actions/runs/{run_id}")
    contract.validate_run(own, revision, "qualify-image.yml", dispatch_only=True, active=True)
    contract.require(own["run_attempt"] == attempt and own["status"] == "in_progress", "qualification run attempt changed")
    candidate_run = github.retained_run(args.candidate_run, revision, "candidate-image.yml",
                                        args.output / "ci/candidate-run.json", dispatch_only=True)
    github.artifact(candidate_run, "candidate", args.output / "candidate")
    candidate = contract.read(args.output, "candidate/candidate.json")
    contract.validate_candidate(candidate, args.output, "candidate/", candidate_run, "0.9.0", revision)
    ci = {kind: github.retained_ci(kind, getattr(args, kind + "_run"), revision, args.output / "ci")
          for kind in ("native", "image")}
    contract.validate_ci(args.output, ci, revision, github.api)
    baseline = baseline_evidence(args.output)
    scope = f"qualification-{run_id}-{attempt}-{secrets.token_hex(6)}"
    identity = dict(version="0.9.0", revision=revision, image=candidate["image"], platform="linux/amd64",
                    run_id=run_id, run_attempt=attempt, scope=scope)
    rollback = dict(report="rollback/report.json", candidate="rollback/candidate.json", run="rollback/run.json",
        release="rollback/release.json", tag="rollback/tag.json", saved_config="rollback/saved-config.json",
        restored_config="rollback/restored-config.json", saved_acf="rollback/saved.acf", restored_acf="rollback/restored.acf")
    proof = dict(identity=identity, instrumentation="tooling/qualification-timings.patch",
        candidate=dict(record="candidate/candidate.json", run="ci/candidate-run.json"),
        ci=ci, policy="policy.json", capacity=[], rollback=rollback,
        soak=dict(manifest="soak/collector.json", observations="soak/observations.jsonl", config="soak/config.json",
                  producer_events="runtime/producer-events.jsonl"))
    write(args.output / "proof.json", proof)
    return identity, baseline, policy, proof


class Scope:
    """All Docker mutations operate on IDs created by this object and relabeled
    immediately before use. No existing container/project identifiers are inputs.
    """
    def __init__(self, identity, folder, persist=True):
        self.identity, self.folder = identity, folder
        self.name, self.network, self.containers, self.images = identity["scope"], None, {}, []
        self.builder = None
        self.planned = dict(containers={}, network=None, builder=None, images=[])
        if persist:
            self.persist()

    def persist(self, closed=False):
        # The last readable plan survives an interrupted post-create update.
        # Reload configuration intentionally uses separate in-place writes.
        target = self.folder / "runtime-ownership.json"
        temporary = self.folder / "runtime-ownership.json.pending"
        value = dict(identity=self.identity, network=self.network, containers=self.containers,
                     images=self.images, builder=self.builder, planned=self.planned, closed=closed)
        flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC | getattr(os, "O_NOFOLLOW", 0)
        with os.fdopen(os.open(temporary, flags, 0o600), "w") as stream:
            stream.write(json.dumps(value, indent=2, allow_nan=False) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(target)

    def lookup(self, kind, name):
        """Return exact names only; IDs are recovered from daemon inspection.
        A filter's partial-name matches are never treated as owned resources.
        """
        if kind == "container":
            output = command("docker", "ps", "--all", "--no-trunc", "--filter", "name=" + name, "--format", "{{.ID}}")
        elif kind == "network":
            output = command("docker", "network", "ls", "--no-trunc", "--filter", "name=" + name, "--format", "{{.ID}}")
        elif kind == "volume":
            output = command("docker", "volume", "ls", "--filter", "name=" + name, "--format", "{{.Name}}")
        else:
            output = command("docker", "image", "ls", "--no-trunc", "--filter", "reference=" + name, "--format", "{{.ID}}")
        identifiers = set(output.splitlines())
        contract.require(len(identifiers) <= 16, "ambiguous private resource lookup")
        found = []
        for identifier in identifiers:
            if kind in {"container", "network"}:
                contract.require(contract.CONTAINER_ID.fullmatch(identifier), "daemon lookup returned incomplete ID")
            argv = ("docker", "inspect", identifier) if kind == "container" else ("docker", kind, "inspect", identifier)
            info = contract.parse_json(command(*argv))[0]
            exact = info.get("Name", "").lstrip("/") == name if kind != "image" else name in (info.get("RepoTags") or [])
            if exact:
                found.append(info)
        contract.require(len(found) <= 1, "colliding exact private resource names")
        return found[0] if found else None

    def absent(self, kind, name):
        contract.require(self.lookup(kind, name) is None, "refusing to reuse an existing " + kind + " name")

    def builder_file(self, name):
        directory = Path(os.environ.get("BUILDX_CONFIG", ""))
        contract.require(directory.is_absolute() and directory.is_dir() and not directory.is_symlink(), "private Buildx configuration missing")
        path = directory / "instances" / name
        contract.require(not path.is_symlink(), "unsafe Buildx instance path")
        return path

    def create_builder(self):
        name = self.name + "-builder"
        path = self.builder_file(name)
        contract.require(not path.exists(), "refusing to reuse an existing builder instance")
        self.absent("container", "buildx_buildkit_" + name + "0")
        self.absent("volume", "buildx_buildkit_" + name + "0_state")
        self.planned["builder"] = name
        self.persist() # plan precedes every daemon/client-side create mutation
        command("docker", "buildx", "create", "--name", name, "--driver", "docker-container", "--driver-opt",
            "image=" + BUILDKIT_IMAGE + ",memory=4g,cpu-quota=400000,cpu-period=100000,env." + BUILDER_OWNER + "=" + self.name)
        self.builder = name
        self.persist()
        return name

    def plan_image(self, name):
        contract.require(name == "redis-pvxs-qualification-tools:" + self.name, "unowned helper output tag")
        self.absent("image", name)
        self.planned["images"].append(name)
        self.persist()

    def recover(self):
        for role, name in self.planned["containers"].items():
            contract.require(name == self.name + "-" + role and role in {"ioc", "redis", "tools", "check"}, "unowned planned container")
            info = self.lookup("container", name)
            if info:
                contract.require(info.get("Config", {}).get("Labels", {}).get(OWNER) == self.name, "planned container owner collision")
                known = self.containers.get(role)
                contract.require(known is None or known == info["Id"], "planned container identity changed")
                self.containers[role] = info["Id"]
            else:
                self.containers.pop(role, None) # daemon-side successful delete with lost CLI return
        if self.planned["network"]:
            contract.require(self.planned["network"] == self.name, "unowned planned network")
            info = self.lookup("network", self.name)
            if info:
                contract.require(info.get("Labels", {}).get(OWNER) == self.name and info.get("Internal") is True, "planned network owner collision")
                contract.require(self.network is None or self.network == info["Id"], "planned network identity changed")
                self.network = info["Id"]
            else:
                self.network = None
        for name in self.planned["images"]:
            contract.require(name == "redis-pvxs-qualification-tools:" + self.name, "unowned planned image tag")
            info = self.lookup("image", name)
            if info:
                contract.require(info.get("Config", {}).get("Labels", {}).get(OWNER) == self.name, "helper output owner collision")
                if name not in self.images:
                    self.images.append(name)
            elif name in self.images:
                self.images.remove(name)
        if self.planned["builder"]:
            name = self.planned["builder"]
            contract.require(name == self.name + "-builder", "unowned planned builder")
            path = self.builder_file(name)
            node_name = "buildx_buildkit_" + name + "0"
            node = self.lookup("container", node_name)
            volume = self.lookup("volume", node_name + "_state")
            if path.exists():
                contract.require(path.stat().st_size <= contract.MAX_FILE_BYTES, "oversized Buildx instance")
                metadata = contract.parse_json(path.read_bytes())
                nodes = metadata.get("Nodes", [])
                contract.require(metadata.get("Name") == name and metadata.get("Driver") == "docker-container" and len(nodes) == 1
                    and nodes[0].get("Name") == name + "0" and nodes[0].get("DriverOpts", {}).get("env." + BUILDER_OWNER) == self.name
                    and nodes[0]["DriverOpts"].get("image") == BUILDKIT_IMAGE, "private builder owner/driver collision")
                if node:
                    contract.require(BUILDER_OWNER + "=" + self.name in node.get("Config", {}).get("Env", [])
                        and node["Config"].get("Image") == BUILDKIT_IMAGE, "daemon builder owner marker collision")
                self.builder = name
            else:
                contract.require(node is None and volume is None, "builder metadata missing for existing daemon state; refuse unknown cleanup")
                self.builder = None
        self.persist()

    def start(self):
        self.absent("network", self.name)
        self.planned["network"] = self.name
        self.persist()
        self.network = command("docker", "network", "create", "--internal", "--label", OWNER + "=" + self.name, self.name)
        self.persist()

    def own(self, container):
        contract.require(container in self.containers.values() and contract.CONTAINER_ID.fullmatch(container), "unowned container action")
        info = contract.parse_json(command("docker", "inspect", container))[0]
        role = next(role for role, identifier in self.containers.items() if identifier == container)
        contract.require(info["Config"].get("Labels", {}).get(OWNER) == self.name
                         and info["Id"] == container and info.get("Name", "").lstrip("/") == self.name + "-" + role, "container ownership changed")
        return info

    def create(self, role, image, arguments, mounts=(), memory="512m", cpus="1"):
        contract.require(role not in self.containers and self.network is not None, "private role already exists")
        self.absent("container", self.name + "-" + role)
        uid, gid = os.getuid(), os.getgid()
        contract.require(uid != 0, "qualification runner must have a non-root UID")
        argv = ["docker", "create", "--name", self.name + "-" + role, "--label", OWNER + "=" + self.name,
                "--network", self.network, "--network-alias", role, "--read-only", "--cap-drop", "ALL",
                "--security-opt", "no-new-privileges", "--user", f"{uid}:{gid}",
                "--memory", memory, "--cpus", cpus, "--pids-limit", "128", "--tmpfs", "/tmp:rw,noexec,nosuid,size=16m"]
        for source, destination, readonly in mounts:
            contract.require(source.is_absolute() and source.exists() and "," not in str(source), "unsafe private bind mount")
            argv += ["--mount", f"type=bind,src={source},dst={destination}" + (",readonly" if readonly else "")]
        if role == "tools":
            argv += ["--env", "LD_LIBRARY_PATH=" + ":".join(WORK + p for p in (
                "/third_party/epics-base/lib/linux-x86_64", "/third_party/pvxs/lib/linux-x86_64",
                "/third_party/pvxs/bundle/usr/linux-x86_64/lib")), "--entrypoint", "/bin/sleep"]
        self.planned["containers"][role] = self.name + "-" + role
        self.persist()
        container = command(*argv, image, *arguments)
        contract.require(contract.CONTAINER_ID.fullmatch(container), "Docker did not return complete created ID")
        self.containers[role] = container
        self.persist()
        self.own(container)
        command("docker", "start", container)
        return container

    def exec(self, role, *argv, timeout=15):
        container = self.containers[role]
        self.own(container)
        return command("docker", "exec", container, *argv, timeout=timeout)

    def remove(self, role):
        container = self.containers[role]
        self.own(container)
        command("docker", "rm", "--force", container)
        del self.containers[role]
        self.planned["containers"].pop(role, None)
        self.persist()

    def signal(self, signal):
        container = self.containers["ioc"]
        self.own(container)
        command("docker", "kill", "--signal", signal, container)

    def pause(self, paused):
        container = self.containers["redis"]
        self.own(container)
        command("docker", "pause" if paused else "unpause", container)

    def close(self):
        self.recover()
        failures = []
        for role in list(self.containers):
            try:
                info = self.own(self.containers[role])
                if info["State"].get("Paused"):
                    command("docker", "unpause", info["Id"])
                self.remove(role)
            except Exception as error:
                failures.append(str(error))
        if self.network:
            try:
                info = contract.parse_json(command("docker", "network", "inspect", self.network))[0]
                contract.require(info.get("Labels", {}).get(OWNER) == self.name and not info.get("Containers"), "network ownership changed or not empty")
                command("docker", "network", "rm", self.network)
                self.network = None
                self.planned["network"] = None
                self.persist()
            except Exception as error:
                failures.append(str(error))
        for image in list(self.images):
            try:
                command("docker", "image", "rm", image)
                self.images.remove(image)
                self.planned["images"].remove(image)
                self.persist()
            except Exception as error:
                failures.append(str(error))
        if self.builder:
            try:
                command("docker", "buildx", "rm", "--force", self.builder)
                node_name = "buildx_buildkit_" + self.builder + "0"
                contract.require(self.lookup("container", node_name) is None and self.lookup("volume", node_name + "_state") is None
                    and not self.builder_file(self.builder).exists(), "owned builder/state volume cleanup incomplete")
                self.builder = None
                self.planned["builder"] = None
                self.persist()
            except Exception as error:
                failures.append(str(error))
        contract.require(not failures, "private runtime cleanup failed: " + "; ".join(failures))
        self.planned = dict(containers={}, network=None, builder=None, images=[])
        self.persist(closed=True)


def fixture_config():
    return dict(server=dict(instance="qualification", namespace="Q", interfaces=["0.0.0.0"],
                            tcp_port=5075, udp_port=5075, auto_beacon=False),
        discovery=dict(enabled=False),
        redis_backends=dict(soak=dict(host="redis", port=6379, base_key="soak")),
        alarms=dict(backend="soak", stream="alarms"),
        access=dict(enabled=True, file="/fixture/access.acf", watch=dict(enabled=False),
                    defaults=dict(pv=dict(asg="QREAD", asl=0), admin_read=dict(asg="QREAD", asl=0), admin_write=dict(asg="QADMIN", asl=0))),
        pvs=[dict(name="scalar", aliases=["Q:alias"], type="uint32", shape="scalar", read=dict(backend="soak", key="scalar")),
             dict(name="array", type="uint32", shape="array", read=dict(backend="soak", key="array"))])


def routed_config(current, axis, index, elements=1):
    import copy
    config = copy.deepcopy(current)
    base = "image-" + str(index) if axis == "imaging" else "capacity-" + str(index)
    backend = "imaging" if axis == "imaging" else "capacity"
    config["redis_backends"][backend] = dict(host="redis", port=6379, base_key=base, reader_probe_ms=1000)
    config["pvs"] = [pv for pv in config["pvs"] if pv["name"] not in ("frame", "capacity")]
    if axis == "imaging":
        config["pvs"].append(dict(name="frame", aliases=["Q:image"], kind="ntndarray", max_frame_bytes=1920 * 1080,
            read=dict(backend=backend, key="image"), source_health=dict(required=False)))
    else:
        config["pvs"].append(dict(name="capacity", type="uint32", shape="scalar" if elements == 1 else "array",
            read=dict(backend=backend, key="data"), source_health=dict(required=False)))
    return config, base


def unrouted_config(current, axis):
    import copy
    config = copy.deepcopy(current)
    config["pvs"] = [pv for pv in config["pvs"] if pv["name"] not in ("frame", "capacity")]
    config["redis_backends"].pop("imaging" if axis == "imaging" else "capacity")
    return config


class Collector:
    def __init__(self, scope, identity, baseline, policy, proof):
        self.scope, self.identity, self.baseline, self.policy, self.proof = scope, identity, baseline, policy, proof
        self.folder = scope.folder
        self.runtime = self.folder / "runtime"
        self.runtime.mkdir()
        self.events, self.workloads, self.lock, self.failure = [], [], threading.Lock(), None
        self.cancelled, self.worker = threading.Event(), None
        self.current_event, self.producer = None, None
        self.config = fixture_config()
        self.started_at, self.start_ns = None, None

    def traffic(self, *argv, timeout=15):
        return self.scope.exec("tools", "/usr/bin/python3", WORK + "/scripts/qualification-traffic.py", *argv, timeout=timeout)

    def probe(self, legacy=False, source=True):
        argv = [WORK + "/build/qualification_probe", "ioc:5075"] + (["--legacy"] if legacy else [])
        result = contract.parse_json(self.scope.exec("tools", *argv, timeout=25))
        if source:
            result = contract.parse_json(self.traffic("content", "--host", "redis", "--base", "soak", "--observation", json.dumps(result)))
        else:
            result["source_match"] = False
        result["observed_mono_ns"] = time.monotonic_ns()
        contract.validate_probe(result, legacy=legacy, ready=source)
        return result

    def eventually(self, predicate, seconds=20):
        deadline, last = time.monotonic() + seconds, None
        while time.monotonic() < deadline:
            try:
                value = predicate()
                if value:
                    return value
            except (ValueError, subprocess.TimeoutExpired) as error:
                last = str(error)
            time.sleep(0.25)
        raise ValueError("private runtime recovery timed out: " + str(last))

    def image(self, record):
        command("docker", "pull", record["image"], timeout=120)
        info = contract.parse_json(command("docker", "image", "inspect", record["image"]))[0]
        contract.require((info["Os"], info["Architecture"]) == ("linux", "amd64"), "runtime platform mismatch")
        labels = info["Config"].get("Labels") or {}
        for key, value in dict(version=record["version"], revision=record["revision"], source=contract.SOURCE, licenses="BSD-3-Clause").items():
            contract.require(labels.get("org.opencontainers.image." + key) == value, "runtime OCI identity mismatch")
        return info

    def start_ioc(self, record):
        self.image(record)
        return self.scope.create("ioc", record["image"], ["--config", "/fixture/config.json"],
                                 mounts=[(self.runtime, "/fixture", True)])

    def observation(self, record, phase, legacy=True):
        probe = self.eventually(lambda: self.probe(legacy=legacy))
        info = self.scope.own(self.scope.containers["ioc"])
        binary = self.scope.exec("ioc", WORK + "/bin/redis-pvxs-ioc", "--version")
        contract.require(binary == f"redis-pvxs-ioc {record['version']} ({record['revision']})", "running binary identity mismatch")
        return dict(phase=phase, image=record["image"], version=record["version"], revision=record["revision"],
                    platform="linux/amd64", container=info["Id"], binary_identity=binary, utc=utc(), pva=probe,
                    inspect=info,
                    config_sha256=self.scope.exec("ioc", "/usr/bin/sha256sum", "/fixture/config.json").split()[0],
                    acf_sha256=self.scope.exec("ioc", "/usr/bin/sha256sum", "/fixture/access.acf").split()[0])

    def save_compatible(self):
        write(self.runtime / "config.json", self.config)
        (self.runtime / "access.acf").write_text("ASG(QREAD) { RULE(0, READ) }\nASG(QADMIN) { RULE(0, WRITE) }\n")
        shutil.copyfile(self.runtime / "config.json", self.folder / "rollback/saved-config.json")
        shutil.copyfile(self.runtime / "access.acf", self.folder / "rollback/saved.acf")
        checks = []
        for record in (self.baseline, self.identity):
            self.image(record)
            # Offline config validation is still a created, owned, bounded
            # container, with the same saved bytes and its own private network.
            container = self.scope.create("check", record["image"], ["--check-config", "/fixture/config.json"],
                                          mounts=[(self.runtime, "/fixture", True)])
            command("docker", "wait", container, timeout=30)
            info = self.scope.own(container)
            contract.require(info["State"]["ExitCode"] == 0, "saved configuration is incompatible with " + record["version"])
            write(self.folder / ("rollback/check-" + record["version"] + ".json"), info)
            self.scope.remove("check")
            checks.append(record["version"])
        return checks

    def begin_producer(self):
        container = self.scope.containers["tools"]
        self.scope.own(container)
        self.producer = subprocess.Popen(["docker", "exec", container, "/usr/bin/python3",
            WORK + "/scripts/qualification-traffic.py", "producer", "--host", "redis", "--base", "soak",
            "--output", "/evidence/runtime/producer.json", "--duration", "93600"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.eventually(lambda: (self.runtime / "producer.json").is_file())

    def reload(self, new_config):
        before = self.probe()
        event = dict(id="reload-" + str(len(self.events)), kind="reload", action="SIGHUP",
                     container=self.scope.containers["ioc"], start_mono_ns=time.monotonic_ns(), before=before,
                     config_before_sha256=contract.digest(self.runtime / "config.json"))
        # In-place rewrite: a running Linux bind sees the same file inode.
        write(self.runtime / "config.json", new_config)
        event["config_after_sha256"] = contract.digest(self.runtime / "config.json")
        self.scope.signal("HUP")
        after = self.eventually(lambda: (p if (p := self.probe())["generation"] == before["generation"] + 1 else None))
        event.update(during=after, after=after, end_mono_ns=time.monotonic_ns())
        self.events.append(event)
        self.config = new_config
        return event

    def route(self, axis, index, elements=1):
        config, base = routed_config(self.config, axis, index, elements)
        self.reload(config)
        return base

    def action(self, kind):
        before = self.probe()
        event = dict(id=kind + "-" + str(len(self.events)), kind=kind, container=self.scope.containers["redis"],
                     start_mono_ns=time.monotonic_ns(), before=before, producer_before=contract.read(self.runtime, "producer.json"))
        with self.lock:
            self.current_event = event
        try:
            if kind == "outage":
                event["action"] = "docker-pause"
                event["operation_start_mono_ns"] = time.monotonic_ns()
                self.scope.pause(True)
                try:
                    event["during"] = self.eventually(lambda: (p if (p := self.probe(source=False))["ready"] is False else None), seconds=20)
                    event["producer_during"] = contract.read(self.runtime, "producer.json")
                    time.sleep(3)
                finally:
                    self.scope.pause(False)
                    event["operation_end_mono_ns"] = time.monotonic_ns()
            else:
                event["action"] = "redis-client-pause"
                event["operation_start_mono_ns"] = time.monotonic_ns()
                measured = contract.parse_json(self.traffic("delay", "--host", "redis"))
                event["operation_end_mono_ns"] = time.monotonic_ns()
                event.update(measured)
                event["during"] = self.probe()
                event["producer_during"] = contract.read(self.runtime, "producer.json")
            after = self.eventually(lambda: (p if (p := self.probe())["scalar"] > before["scalar"]
                and p["array"] > before["array"] else None))
            event.update(after=after, producer_after=contract.read(self.runtime, "producer.json"))
            with self.lock:
                event["end_mono_ns"] = time.monotonic_ns()
                self.events.append(event)
                self.current_event = None
            return event
        finally:
            with self.lock:
                self.current_event = None

    def workload(self, axis, index, rate=1000, samples=10000, elements=1, clients=1, action=None):
        base = self.route(axis, index, elements)
        folder = self.folder / "workloads"
        folder.mkdir(exist_ok=True)
        relative = "workloads/" + str(index) + "-" + axis
        report = self.folder / (relative + ".json")
        target = "/evidence/" + relative + ".json"
        if axis == "imaging":
            argv = [WORK + "/build/ndarray_transfer", "--redis-host", "redis", "--redis-port", "6379", "--base-key", base,
                    "--pv", "Q:image", "--pva-address", "ioc:5075", "--frames", "600", "--width", "1920",
                    "--height", "1080", "--fps", "10", "--output", target]
            timeout = 90
        else:
            argv = [WORK + "/build/stream_capacity", "--redis-host", "redis", "--redis-port", "6379", "--base-key", base,
                    "--pv", "Q:capacity", "--pva-address", "ioc:5075", "--samples", str(samples), "--rate", str(rate),
                    "--elements", str(elements), "--clients", str(clients), "--output", target]
            timeout = samples / rate + clients * 5 + 20
        executed = dict(self.identity, argv=argv, ioc_container=self.scope.containers["ioc"], timeout_seconds=timeout,
                        start_mono_ns=time.monotonic_ns(), started_at=utc(), platform="linux/amd64")
        self.scope.own(self.scope.containers["tools"])
        with (self.folder / (relative + ".log")).open("xb") as log:
            process = subprocess.Popen(["docker", "exec", self.scope.containers["tools"], *argv], stdout=log, stderr=subprocess.STDOUT)
            event = None
            try:
                if action:
                    contract.require(not self.cancelled.wait(3), "qualification cancelled")
                    contract.require(process.poll() is None, "capacity workload finished before controlled action")
                    if action == "reload":
                        import copy
                        config = copy.deepcopy(self.config)
                        config["pvs"][0]["metadata"] = dict(description="capacity reload " + str(index))
                        event = self.reload(config)
                    else:
                        event = self.action(action)
                deadline = executed["start_mono_ns"] / 1e9 + timeout
                while process.poll() is None:
                    contract.require(time.monotonic() < deadline and not self.cancelled.wait(0.1), "workload timed out or cancelled")
                code = process.returncode
                contract.require(code == 0, "workload failed: " + relative)
            finally:
                if process.poll() is None:
                    # Killing docker exec alone does not stop the in-container
                    # client. A failed workload invalidates this run; cleanup
                    # removes its owned tools container before any record.
                    process.kill()
                    process.wait(timeout=5)
        executed.update(exit_code=code, end_mono_ns=time.monotonic_ns(), ended_at=utc(), report_sha256=contract.digest(report))
        executed["elapsed_seconds"] = (executed["end_mono_ns"] - executed["start_mono_ns"]) / 1e9
        if axis == "imaging":
            executed["instrumentation_sha256"] = contract.digest(ROOT / "scripts/qualification-timings.patch")
        write(self.folder / (relative + "-execution.json"), executed)
        entry = dict(axis=axis, report=relative + ".json", execution=relative + "-execution.json",
                     start_mono_ns=executed["start_mono_ns"])
        if axis != "imaging":
            measured = contract.read(self.folder, entry["report"])
            entry["classification"] = "lossless" if measured["missed_updates"] == measured["duplicate_updates"] == 0 else "exploration"
        if event:
            entry["event"] = event["id"]
        # Remove the route after observation, so completed 2 MiB images are not
        # inspected at 1 Hz for the rest of the day. Active sources keep the
        # default 1000 ms policy; image and fan-out traffic recur every hour.
        self.reload(unrouted_config(self.config, axis))
        self.traffic_delete(base, "image" if axis == "imaging" else "data")
        return entry

    def traffic_delete(self, base, key):
        # Only keys generated for this private fixture, on the Redis ID we own.
        contract.require(base.startswith(("image-", "capacity-")) and base.split("-")[-1].isdigit(), "unowned fixture key deletion")
        self.scope.exec("redis", "redis-cli", "DEL", "{" + base + "}:" + key)

    def sample(self):
        with self.lock:
            return self.locked_sample()

    def locked_sample(self):
        event = self.current_event
        event_id = event["id"] if event else None
        probe = self.probe(source=event_id is None)
        info = self.scope.own(self.scope.containers["ioc"])
        status = self.scope.exec("ioc", "/bin/cat", "/proc/1/status")
        statistics = self.scope.exec("ioc", "/bin/cat", "/proc/1/stat")
        rss = int(next(line.split()[1] for line in status.splitlines() if line.startswith("VmRSS:"))) * 1024
        fields = statistics[statistics.rfind(")") + 2:].split()
        ticks = int(fields[11]) + int(fields[12])
        progress = contract.read(self.runtime, "producer.json")
        contract.require(time.monotonic_ns() - progress["mono_ns"] < 3 * 10**9 and self.producer.poll() is None,
                         "producer progress observation stopped")
        self.scope.exec("tools", "/bin/kill", "-0", str(progress["pid"]))
        host = info["HostConfig"]
        state = dict(running=info["State"]["Running"], oom_killed=info["State"]["OOMKilled"], restart_count=info["RestartCount"],
            memory_limit_bytes=host["Memory"], nano_cpus=host["NanoCpus"], host_ports=list((host.get("PortBindings") or {}).keys()),
            readonly_rootfs=host["ReadonlyRootfs"], cap_drop=host["CapDrop"], owner=info["Config"]["Labels"][OWNER],
            image=info["Config"]["Image"], image_id=info["Image"])
        return dict(mono_ns=time.monotonic_ns(), utc=utc(), scope=self.identity["scope"], ioc_container=info["Id"],
                    event=event_id, pva=probe, producer=progress, docker=state, rss_bytes=rss, cpu_ticks=ticks)

    def workloads_loop(self):
        try:
            index = 0
            for hour in range(24):
                due = self.start_ns + hour * 3600 * 10**9
                while time.monotonic_ns() < due:
                    if self.cancelled.wait(min(1, (due - time.monotonic_ns()) / 1e9)):
                        return
                for axis in ("imaging", "fan-out"):
                    contract.require(not self.cancelled.is_set(), "qualification cancelled")
                    entry = self.workload(axis, index, rate=1000, samples=10000, elements=32 if axis == "fan-out" else 1,
                                          clients=16 if axis == "fan-out" else 1)
                    self.workloads.append(entry)
                    if hour == 0:
                        self.proof["capacity"].append(entry)
                        if axis == "imaging":
                            self.proof["frames"] = entry
                    index += 1
                if hour == 0:
                    for axis, elements, rates in (("scalar", 1, (1000, 10000)), ("array", 4096, (100, 2500)),
                                                  ("fan-out", 32, (100, 5000))):
                        for rate in rates:
                            self.proof["capacity"].append(self.workload(axis, index, rate=rate, elements=elements,
                                clients=16 if axis == "fan-out" else 1))
                            index += 1
                    for axis in ("reload", "backend-delay"):
                        self.proof["capacity"].append(self.workload(axis, index, rate=1000, action=axis))
                        index += 1
                    self.action("outage")
        except Exception as error:
            self.failure = error

    def collect(self):
        compatibility = self.save_compatible()
        self.begin_producer()
        self.start_ioc(self.baseline)
        before = self.observation(self.baseline, "baseline-before")
        self.scope.remove("ioc")
        self.config["redis_backends"]["soak"]["reader_probe_ms"] = 1000
        for pv in self.config["pvs"]:
            pv["source_health"] = dict(required=True, stale_after_ms=2000)
        write(self.runtime / "config.json", self.config)
        (self.folder / "soak").mkdir()
        shutil.copyfile(self.runtime / "config.json", self.folder / "soak/config.json")
        self.start_ioc(self.identity)
        candidate = self.observation(self.identity, "candidate", legacy=False)
        self.started_at, self.start_ns = utc(), time.monotonic_ns()
        self.worker = threading.Thread(target=self.workloads_loop, daemon=True)
        journal = self.folder / "soak/observations.jsonl"
        # The first observation precedes controlled actions or workload threads.
        with journal.open("x") as stream:
            stream.write(json.dumps(self.sample()) + "\n")
            stream.flush()
            self.worker.start()
            next_sample = self.start_ns + 30 * 10**9
            while True:
                contract.require(self.failure is None, "workload collector failed: " + str(self.failure))
                time.sleep(max(0, min(1, (next_sample - time.monotonic_ns()) / 1e9)))
                if time.monotonic_ns() < next_sample:
                    continue
                stream.write(json.dumps(self.sample()) + "\n")
                stream.flush()
                contract.require(journal.stat().st_size <= contract.MAX_FILE_BYTES, "observation journal exceeds bound")
                if time.monotonic_ns() - self.start_ns >= 86400 * 10**9:
                    break
                next_sample += 30 * 10**9
        self.worker.join(timeout=1)
        contract.require(not self.worker.is_alive() and self.failure is None, "incomplete representative workload collector")
        ended_at, end_ns = utc(), time.monotonic_ns()
        network = contract.parse_json(command("docker", "network", "inspect", self.scope.network))[0]
        info = self.scope.own(self.scope.containers["ioc"])
        manifest = dict(self.identity, completed=True, start_mono_ns=self.start_ns, end_mono_ns=end_ns,
            started_at=self.started_at, ended_at=ended_at, soak_seconds=(end_ns - self.start_ns) / 1e9,
            ioc_container=info["Id"], redis_container=self.scope.containers["redis"], tools_container=self.scope.containers["tools"],
            network_internal=network["Internal"], host_ports=list((info["HostConfig"].get("PortBindings") or {}).keys()),
            ioc_memory_bytes=info["HostConfig"]["Memory"], ioc_nano_cpus=info["HostConfig"]["NanoCpus"],
            config_sha256=contract.digest(self.folder / "soak/config.json"), observations_sha256=contract.digest(journal),
            events=self.events, workloads=self.workloads)
        write(self.folder / "soak/collector.json", manifest)
        # Restore the exact saved bytes, including ACF, and recreate only our
        # own IOC with the published qualified baseline digest.
        self.scope.remove("ioc")
        for name, saved in (("config.json", "saved-config.json"), ("access.acf", "saved.acf")):
            shutil.copyfile(self.folder / "rollback" / saved, self.runtime / name)
        shutil.copyfile(self.runtime / "config.json", self.folder / "rollback/restored-config.json")
        shutil.copyfile(self.runtime / "access.acf", self.folder / "rollback/restored.acf")
        self.start_ioc(self.baseline)
        after = self.observation(self.baseline, "baseline-restored")
        report = dict(self.identity, candidate_image=self.identity["image"], baseline_image=self.baseline["image"],
            baseline_version="0.8.2", saved_before_candidate=True, compatibility_checks=compatibility,
            baseline_candidate_sha256=contract.digest(self.folder / "rollback/candidate.json"),
            config_sha256=contract.digest(self.folder / "rollback/saved-config.json"),
            acf_sha256=contract.digest(self.folder / "rollback/saved.acf"), observations=[before, candidate, after])
        write(self.folder / "rollback/report.json", report)
        write(self.folder / "proof.json", self.proof)
        return manifest["soak_seconds"]

    def stop(self):
        self.cancelled.set()
        if self.worker and self.worker.is_alive():
            self.worker.join(timeout=30)
            contract.require(not self.worker.is_alive(), "qualification worker failed to stop")
        if self.producer and self.producer.poll() is None:
            try:
                progress = contract.read(self.runtime, "producer.json")
                self.scope.exec("tools", "/bin/kill", "-TERM", str(progress["pid"]))
                self.producer.wait(timeout=5)
            finally:
                if self.producer.poll() is None:
                    self.producer.kill()
                    self.producer.wait(timeout=5)


def run(args):
    identity, baseline, policy, proof = prepare(args)
    scope = Scope(identity, args.output)
    collector = None
    try:
        command("docker", "pull", REDIS_IMAGE, timeout=120)
        tools = "redis-pvxs-qualification-tools:" + identity["scope"]
        builder = scope.create_builder()
        generated = args.output / "qualification-tools.Dockerfile"
        generated.write_bytes((ROOT / "Dockerfile").read_bytes() + b"\n" + (ROOT / "tests/qualification-tools.Dockerfile").read_bytes())
        scope.plan_image(tools)
        command("docker", "buildx", "build", "--builder", builder, "--load", "--target", "qualification-tools",
                "--platform", "linux/amd64", "--file", str(generated), "--tag", tools,
                "--build-arg", "REDIS_PVXS_IOC_VERSION=0.9.0", "--build-arg", "REDIS_PVXS_IOC_REVISION=" + identity["revision"],
                "--build-arg", "QUALIFICATION_SCOPE=" + identity["scope"],
                str(ROOT), timeout=2700, log=args.output / "tools-build.log")
        scope.images.append(tools)
        scope.persist()
        tools_info = contract.parse_json(command("docker", "image", "inspect", tools))[0]
        contract.require((tools_info["Os"], tools_info["Architecture"]) == ("linux", "amd64"), "qualification tools must be Linux amd64")
        write(args.output / "tools-image.json", tools_info)
        scope.start()
        scope.create("redis", REDIS_IMAGE, ["redis-server", "--save", "", "--appendonly", "no", "--maxmemory", "64mb",
            "--maxmemory-policy", "noeviction", "--protected-mode", "no"], memory="128m", cpus="0.5")
        scope.create("tools", tools, ["93600"], mounts=[(args.output, "/evidence", False)], memory="2g", cpus="2")
        collector = Collector(scope, identity, baseline, policy, proof)
        collector.eventually(lambda: scope.exec("redis", "redis-cli", "PING") == "PONG")
        seconds = collector.collect()
        collector.stop()
        # Cleanup is part of success. The record is created only afterward;
        # eventual workflow success is still required by digest promotion.
        scope.close()
        scope = None
        record = dict(identity, schema=1, candidate_run=contract.read(args.output, "ci/candidate-run.json")["id"],
            candidate_attempt=contract.read(args.output, "ci/candidate-run.json")["run_attempt"], checks=sorted(contract.CHECKS),
            soak_seconds=seconds, rollback_version="0.8.2", evidence=contract.inventory(args.output))
        info = github.api(f"repos/{contract.REPOSITORY}/actions/runs/{identity['run_id']}")
        contract.verify_bundle(args.output, record, info, github.api, active=True)
        write(args.output / "qualification.json", record)
        print(json.dumps(record, indent=2))
    finally:
        primary = sys.exc_info()[1]
        errors = []
        try:
            if collector:
                collector.stop()
        except BaseException as error:
            errors.append("collector stop: " + str(error))
        finally:
            try:
                if scope:
                    scope.close()
            except BaseException as error:
                errors.append("private scope cleanup: " + str(error))
        if errors:
            write(args.output / "cleanup-errors.json", dict(primary=str(primary) if primary else None, cleanup=errors))
            if primary:
                # adlinux3's host Python is 3.9; persist diagnostics without
                # relying on BaseException.add_note (introduced in 3.11).
                note = getattr(primary, "add_note", None)
                if callable(note):
                    note("; ".join(errors))
            else:
                raise ValueError("; ".join(errors))


def cleanup(folder):
    path = folder / "runtime-ownership.json"
    if not path.exists():
        return
    state = contract.read(folder, "runtime-ownership.json")
    identity = state["identity"]
    prefix = f"qualification-{os.environ.get('GITHUB_RUN_ID')}-{os.environ.get('GITHUB_RUN_ATTEMPT')}-"
    contract.require(identity.get("scope", "").startswith(prefix) and
        __import__("re").fullmatch(r"qualification-[0-9]+-[0-9]+-[0-9a-f]{12}", identity["scope"]), "cleanup scope differs from this workflow attempt")
    empty_plan = dict(containers={}, network=None, builder=None, images=[])
    if state.get("closed") is True:
        contract.require(state.get("containers") == {} and state.get("network") is None and state.get("builder") is None
            and state.get("images") == [] and state.get("planned") == empty_plan, "closed cleanup journal contains live/planned resources")
        return # record already binds these exact bytes; do not rewrite or rediscover
    scope = Scope(identity, folder, persist=False)
    scope.network, scope.containers, scope.images, scope.builder = state["network"], state["containers"], state["images"], state["builder"]
    scope.planned = state["planned"]
    contract.require(scope.builder in (None, identity["scope"] + "-builder") and
                     all(image == "redis-pvxs-qualification-tools:" + identity["scope"] for image in scope.images), "unowned builder/image cleanup")
    scope.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cleanup", action="store_true")
    parser.add_argument("--candidate-run", type=int)
    parser.add_argument("--native-run", type=int)
    parser.add_argument("--image-run", type=int)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.cleanup:
        cleanup(args.output)
    else:
        if any(value is None or value < 1 for value in (args.candidate_run, args.native_run, args.image_run)):
            parser.error("three positive workflow run IDs are required")
        def interrupted(*_):
            raise KeyboardInterrupt("qualification interrupted; clean up this run's private resources")
        signal.signal(signal.SIGTERM, interrupted)
        signal.signal(signal.SIGINT, interrupted)
        run(args)


if __name__ == "__main__":
    main()
