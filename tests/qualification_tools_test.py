"""Deterministic proof fixtures, never release evidence or a live soak.

Every API response below is an explicit in-process test double. The production
writer and promotion always authenticate with GitHub; they have no offline or
assert-checks option.
"""
import base64
import copy
import datetime as dt
import importlib.util
import io
import json
from pathlib import Path
import stat
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import qualification_contract as contract


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


collector = module("qualify_image", "qualify-image.py")
traffic = module("qualification_traffic", "qualification-traffic.py")
SHA, OLD_SHA = "a" * 40, "c" * 40
IMAGE, OLD_IMAGE = (contract.IMAGE_REPOSITORY + "@sha256:" + char * 64 for char in ("b", "d"))
START_NS = 10000000000000
START_UTC = dt.datetime(2026, 9, 1, tzinfo=dt.timezone.utc)


def utc(seconds=0):
    return (START_UTC + dt.timedelta(seconds=seconds)).isoformat()


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n")


def probe(value=1000, seconds=0, ready=True):
    nanos = int((START_UTC + dt.timedelta(seconds=seconds)).timestamp() * 1000000000)
    return dict(scalar=value, alias=value, array=value, array_elements=4096, array_exact=True,
        scalar_time_ns=nanos, alias_time_ns=nanos, array_time_ns=nanos, scalar_alarm=0, array_alarm=0,
        generation=1, backend_connected=ready, ready=ready, source_match=ready,
        source_cursors={kind: f"{nanos // 1000000}-{nanos % 1000000}" for kind in ("scalar", "alias", "array")},
        read_failures=0, read_rejections=0, stream_resets=0, retention_gaps=0)


def progress(seconds, failed=0):
    mono = START_NS + int(seconds * 10**9)
    count = int((seconds + 10) * 100)
    return dict(pid_alive=True, accepted=count - failed, failed=failed, value=count, rate_hz=100, array_elements=4096,
                mono_ns=mono, schedule_start_mono_ns=START_NS - 10 * 10**9,
                last_attempt_mono_ns=mono - 10000000, producer_max_late_ms=1)


class ProofFixture:
    def __init__(self, directory):
        self.directory, self.remote = directory, {}
        self.identity = dict(version="0.9.0", revision=SHA, image=IMAGE, platform="linux/amd64", run_id=100,
                             run_attempt=2, scope="qualification-100-2-aaaaaaaaaaaa")
        write(directory / "runtime-ownership.json", dict(identity=self.identity, closed=True, containers={}, images=[], network=None, builder=None,
              planned=dict(containers={}, network=None, builder=None, images=[])))
        policy_bytes = (ROOT / "docs/qualification-policy.json").read_bytes()
        (directory / "policy.json").write_bytes(policy_bytes)
        instrument = "tooling/qualification-timings.patch"
        (directory / "tooling").mkdir()
        (directory / instrument).write_bytes((ROOT / "scripts/qualification-timings.patch").read_bytes())
        self.remote[f"repos/{contract.REPOSITORY}/contents/VERSION?ref={SHA}"] = self.content(b"0.9.0\n")
        self.remote[f"repos/{contract.REPOSITORY}/contents/docs/qualification-policy.json?ref={SHA}"] = self.content(policy_bytes)
        self.remote[f"repos/{contract.REPOSITORY}/contents/scripts/qualification-timings.patch?ref={SHA}"] = self.content((directory / instrument).read_bytes())
        native_contract = json.loads((ROOT / "tests/qualification-native-contract.json").read_text())
        self.remote[f"repos/{contract.REPOSITORY}/contents/CMakeLists.txt?ref={SHA}"] = self.content(native_contract["cmake_excerpt"].encode())
        self.remote[f"repos/{contract.REPOSITORY}/contents/.github/workflows/native.yml?ref={SHA}"] = self.content(native_contract["native_workflow"].encode())
        self.remote[f"repos/{contract.REPOSITORY}/compare/{SHA}...main"] = dict(status="identical", merge_base_commit=dict(sha=SHA))
        self.own_run = self.run(100, "qualify-image.yml", attempt=2)
        candidate_run = self.run(11, "candidate-image.yml")
        self.candidate("candidate", candidate_run, "0.9.0", SHA, IMAGE)
        write(directory / "ci/candidate-run.json", candidate_run)
        ci = self.ci()
        old_run = self.run(14, "candidate-image.yml", revision=OLD_SHA)
        self.candidate("rollback", old_run, "0.8.2", OLD_SHA, OLD_IMAGE)
        write(directory / "rollback/run.json", old_run)
        released = dict(id=5, tag_name="v0.8.2", draft=False, prerelease=False, published_at=utc(-3600),
                        assets=[dict(id=41, name="candidate.json"), dict(id=42, name="release-evidence.tar.gz")])
        write(directory / "rollback/release.json", released)
        self.remote[f"repos/{contract.REPOSITORY}/releases/tags/v0.8.2"] = released
        tag = dict(type="commit", sha=OLD_SHA)
        write(directory / "rollback/tag.json", tag)
        self.remote[f"repos/{contract.REPOSITORY}/git/ref/tags/v0.8.2"] = dict(object=tag)
        config = collector.fixture_config()
        write(directory / "rollback/saved-config.json", config)
        write(directory / "rollback/restored-config.json", config)
        for filename in ("saved.acf", "restored.acf"):
            (directory / "rollback" / filename).write_text("ASG(QREAD) { RULE(0, READ) }\n")
        config["redis"]["reader_probe_ms"] = 1000
        write(directory / "soak/config.json", config)
        self.proof = dict(identity=self.identity, policy="policy.json", instrumentation=instrument,
            candidate=dict(record="candidate/candidate.json", run="ci/candidate-run.json"), ci=ci, capacity=[],
            soak=dict(manifest="soak/collector.json", config="soak/config.json", observations="soak/observations.jsonl",
                      producer_events="runtime/producer-events.jsonl"),
            rollback=dict(report="rollback/report.json", candidate="rollback/candidate.json", run="rollback/run.json",
                release="rollback/release.json", tag="rollback/tag.json", saved_config="rollback/saved-config.json",
                restored_config="rollback/restored-config.json", saved_acf="rollback/saved.acf", restored_acf="rollback/restored.acf"))
        events = [self.event("reload", 120), self.event("backend-delay", 240), self.event("outage", 360)]
        (directory / "runtime").mkdir()
        (directory / "runtime/producer-events.jsonl").write_text(json.dumps(dict(kind="start", schema=1,
            schedule_start_mono_ns=START_NS - 10 * 10**9, rate_hz=100, socket_timeout_ms=1000)) + "\n")
        workloads = []
        for hour in range(24):
            for index, axis in enumerate(("imaging", "fan-out")):
                entry = self.workload(axis, f"hour-{hour}-{axis}", hour * 3600 + 10 + index * 100)
                workloads.append(entry)
                if hour == 0:
                    self.proof["capacity"].append(entry)
                    if axis == "imaging":
                        self.proof["frames"] = entry
        for axis, seconds in (("scalar", 600), ("array", 700), ("reload", 115), ("backend-delay", 235)):
            entry = self.workload(axis, axis, seconds)
            if axis in {"reload", "backend-delay"}:
                entry["event"] = axis
            self.proof["capacity"].append(entry)
        manifest = dict(self.identity, completed=True, start_mono_ns=START_NS, end_mono_ns=START_NS + 86400 * 10**9,
            started_at=utc(), ended_at=utc(86400), soak_seconds=86400.0, ioc_container="1" * 64,
            redis_container="2" * 64, tools_container="3" * 64, network_internal=True, host_ports=[],
            ioc_memory_bytes=512 * 1024**2, ioc_nano_cpus=10**9, config_sha256=contract.digest(directory / "soak/config.json"),
            events=events, workloads=workloads)
        journal = directory / "soak/observations.jsonl"
        with journal.open("w") as stream:
            for index in range(2881):
                sample = self.sample(index)
                stream.write(json.dumps(sample) + "\n")
        manifest["observations_sha256"] = contract.digest(journal)
        write(directory / "soak/collector.json", manifest)
        self.rollback()
        self.refresh()

    def content(self, data):
        return dict(encoding="base64", content=base64.b64encode(data).decode())

    def run(self, run_id, workflow, revision=SHA, attempt=1):
        info = dict(id=run_id, run_attempt=attempt, head_sha=revision, head_branch="main", event="workflow_dispatch",
            path=".github/workflows/" + workflow, status="completed", conclusion="success", repository=dict(full_name=contract.REPOSITORY),
            head_repository=dict(full_name=contract.REPOSITORY), run_started_at=utc(-600), updated_at=utc(87000))
        self.remote[f"repos/{contract.REPOSITORY}/actions/runs/{run_id}"] = info
        return info

    def candidate(self, prefix, info, version, revision, image):
        folder = self.directory / prefix
        folder.mkdir(exist_ok=True)
        values = {
            "source-inputs.json": dict(repository=contract.SOURCE, revision=revision,
                                       submodules=[dict(path="third_party/pvxs", revision="e" * 40)]),
            "registry-manifest.json": dict(digest=image.split("@")[1]),
            "sbom.json": dict(SPDX=dict(spdxVersion="SPDX-2.3", packages=[dict(name="fixture")])),
            "provenance.json": dict(SLSA=dict(buildConfig=dict(test=True), materials=[dict(uri="fixture")],
                invocation=dict(parameters=dict(args={"build-arg:REDIS_PVXS_IOC_REVISION": revision})))),
        }
        for name, value in values.items():
            write(folder / name, value)
        record = dict(version=version, revision=revision, image=image, platform="linux/amd64",
            run_id=info["id"], run_attempt=info["run_attempt"], checks=sorted(contract.CANDIDATE_CHECKS),
            evidence={name: contract.digest(folder / name) for name in contract.ATTESTATIONS})
        write(folder / "candidate.json", record)

    def ci(self):
        result = {}
        for kind, run_id, workflow in (("native", 12, "native.yml"), ("image", 13, "ci-image.yml")):
            info = self.run(run_id, workflow)
            write(self.directory / ("ci/" + kind + "-run.json"), info)
            jobs, logs = [], {}
            cases = [("validate", ["adlinux3"])] if kind == "image" else [
                ("native (macos-14, none)", ["macos-14"]), ("native (ubuntu-24.04, none)", ["ubuntu-24.04"]),
                ("native (ubuntu-24.04, address,undefined)", ["ubuntu-24.04"])]
            steps = ["Build validation image", "Validate image metadata", "Validate runtime image", "Validate isolated Redis/PVA and access behavior"] \
                if kind == "image" else ["Configure and build service", "Test native runtime and access control", "Test with real RecCeiver and its ChannelFinder processor"]
            for index, (name, labels) in enumerate(cases):
                job = dict(id=run_id * 10 + index, run_id=run_id, head_sha=SHA, name=name, labels=labels,
                    status="completed", conclusion="success", steps=[dict(name=s, status="completed", conclusion="success") for s in steps])
                jobs.append(job)
                if kind == "native":
                    path = f"ci/job-{job['id']}.log"
                    flags = "INTEGRATIONS: ON\nSANITIZERS: address,undefined\n" if "address," in name else "INTEGRATIONS: OFF\n"
                    tests = ["config_tests", "access_runtime_tests", "source_health_e2e", "ndarray_transfer_e2e", "discovery_e2e"]
                    if "address," in name:
                        tests += ["endpoint_rpc_e2e", "rpc_hardening_e2e", "rpc_recovery_e2e", "channelfinder_http_tests"]
                    (self.directory / path).write_text(flags + "\n".join(f"Test #{i}: {n} .... Passed" for i, n in enumerate(tests)) +
                        "\nreal RecCeiver with in-memory ChannelFinder client: registration, aliases, metadata, removal and receiver restart passed\n")
                    logs[name] = path
            data = dict(total_count=len(jobs), jobs=jobs)
            write(self.directory / ("ci/" + kind + "-jobs.json"), data)
            self.remote[f"repos/{contract.REPOSITORY}/actions/runs/{run_id}/attempts/1/jobs?per_page=100"] = data
            result[kind] = dict(run="ci/" + kind + "-run.json", jobs="ci/" + kind + "-jobs.json", logs=logs)
        return result

    def event(self, kind, seconds):
        event = dict(id=kind, kind=kind, container=("1" if kind == "reload" else "2") * 64,
            start_mono_ns=START_NS + seconds * 10**9, end_mono_ns=START_NS + (seconds + 5) * 10**9,
            before=probe(int((seconds + 10) * 100), seconds), during=probe(int((seconds + 11) * 100), seconds + 1, ready=kind != "outage"),
            after=probe(int((seconds + 15) * 100), seconds + 5))
        if kind == "reload":
            event.update(action="SIGHUP", config_before_sha256="f" * 64, config_after_sha256="e" * 64)
            event["after"]["generation"] = event["during"]["generation"] = 2
        elif kind == "backend-delay":
            event.update(action="redis-client-pause", pause_ms=250, measured_redis_delay_ms=250.5)
        else:
            event["action"] = "docker-pause"
        if kind != "reload":
            event.update(operation_start_mono_ns=event["start_mono_ns"],
                operation_end_mono_ns=event["start_mono_ns"] + (250000000 if kind == "backend-delay" else 4 * 10**9),
                producer_before=progress(seconds), producer_during=progress(seconds + 1), producer_after=progress(seconds + 5))
        return event

    def workload(self, axis, name, seconds):
        report_name, execute_name = "workloads/" + name + ".json", "workloads/" + name + "-execution.json"
        executed = dict(self.identity, start_mono_ns=START_NS + seconds * 10**9, end_mono_ns=START_NS + (seconds + 60) * 10**9,
            started_at=utc(seconds), ended_at=utc(seconds + 60), elapsed_seconds=60, timeout_seconds=90, exit_code=0,
            ioc_container="1" * 64, argv=["ndarray_transfer", "--frames", "600", "--width", "1920", "--height", "1080", "--fps", "10"])
        if axis == "imaging":
            report = dict(passed=True, frames=600, width=1920, height=1080, fps_target=10, format="Mono8",
                pixels_checked=600 * 1920 * 1080, unexplained_gaps=0, duplicates=0, elapsed_seconds=60,
                latency_p50_ms=1, latency_p95_ms=2, latency_p99_ms=3, producer_max_late_ms=1,
                monitor_queue_entries=16, redis_history_entries=16, timing_instrumentation="steady-send-v1",
                producer_steady_send_ns=[executed["start_mono_ns"] + i * 100_000_000 for i in range(600)])
            executed["instrumentation_sha256"] = contract.digest(self.directory / "tooling/qualification-timings.patch")
        else:
            clients, elements = (16 if axis == "fan-out" else 1), (4096 if axis == "array" else 1)
            report = dict(samples=10000, clients=clients, elements=elements, payload_bytes=elements * 4, rate_target_hz=1000,
                produced_hz=1000, elapsed_seconds=10, received=10000 * clients, missed_updates=0, duplicate_updates=0,
                final_values_converged=True, latency_p50_ms=1, latency_p95_ms=2, latency_p99_ms=3, producer_max_late_ms=1,
                monitor_queue_limit=16, monitor_queue_peak=2, redis_history_entries=16)
            executed["argv"] = ["stream_capacity", "--samples", "10000", "--clients", str(clients), "--elements", str(elements), "--rate", "1000"]
        write(self.directory / report_name, report)
        executed["report_sha256"] = contract.digest(self.directory / report_name)
        write(self.directory / execute_name, executed)
        entry = dict(axis=axis, report=report_name, execution=execute_name, start_mono_ns=executed["start_mono_ns"])
        if axis != "imaging":
            entry["classification"] = "lossless"
        return entry

    def sample(self, index):
        seconds = index * 30
        mono = START_NS + seconds * 10**9
        count = (seconds + 10) * 100
        pva = probe(count, seconds)
        event = "outage" if seconds == 360 else None
        if event:
            pva["ready"] = False
            pva["source_match"] = False
        return dict(mono_ns=mono, utc=utc(seconds), scope=self.identity["scope"], ioc_container="1" * 64, event=event,
            rss_bytes=20 * 1024**2, cpu_ticks=index * 20, pva=pva,
            producer=progress(seconds),
            docker=dict(running=True, oom_killed=False, restart_count=0, memory_limit_bytes=512 * 1024**2, nano_cpus=10**9,
                        host_ports=[], readonly_rootfs=True, cap_drop=["ALL"], owner=self.identity["scope"], image=IMAGE, image_id="sha256:" + "f" * 64))

    def rollback(self):
        config_hash = contract.digest(self.directory / "rollback/saved-config.json")
        acf_hash = contract.digest(self.directory / "rollback/saved.acf")
        observations = []
        for index, phase in enumerate(("baseline-before", "candidate", "baseline-restored")):
            image, version, revision = (IMAGE, "0.9.0", SHA) if index == 1 else (OLD_IMAGE, "0.8.2", OLD_SHA)
            container = str(index + 4) * 64
            observations.append(dict(phase=phase, image=image, version=version, revision=revision, platform="linux/amd64",
                container=container, binary_identity=f"redis-pvxs-ioc {version} ({revision})", utc=utc((-100, -50, 86410)[index]),
                pva=probe(1000 + index * 1000), acf_sha256=acf_hash,
                config_sha256=contract.digest(self.directory / "soak/config.json") if index == 1 else config_hash,
                inspect=dict(Id=container, Config=dict(Image=image), State=dict(Running=True, OOMKilled=False))))
        for version, image in (("0.8.2", OLD_IMAGE), ("0.9.0", IMAGE)):
            write(self.directory / ("rollback/check-" + version + ".json"), dict(Config=dict(Image=image),
                Args=["--check-config", "/fixture/config.json"], State=dict(Status="exited", ExitCode=0)))
        write(self.directory / "rollback/report.json", dict(self.identity, candidate_image=IMAGE, baseline_image=OLD_IMAGE,
            baseline_version="0.8.2", saved_before_candidate=True, compatibility_checks=["0.8.2", "0.9.0"],
            baseline_candidate_sha256=contract.digest(self.directory / "rollback/candidate.json"), config_sha256=config_hash,
            acf_sha256=acf_hash, observations=observations))

    def refresh(self):
        write(self.directory / "proof.json", self.proof)
        manifest = contract.read(self.directory, "soak/collector.json")
        manifest["observations_sha256"] = contract.digest(self.directory / "soak/observations.jsonl")
        write(self.directory / "soak/collector.json", manifest)
        for entry in [*self.proof["capacity"], *manifest["workloads"]]:
            executed = contract.read(self.directory, entry["execution"])
            executed["report_sha256"] = contract.digest(self.directory / entry["report"])
            write(self.directory / entry["execution"], executed)
        self.record = dict(self.identity, schema=1, candidate_run=11, candidate_attempt=1, checks=sorted(contract.CHECKS),
                           soak_seconds=86400.0, rollback_version="0.8.2", evidence=contract.inventory(self.directory))

    def mutate(self, name, function):
        value = contract.read(self.directory, name)
        function(value)
        write(self.directory / name, value)

    def journal(self, function):
        path = self.directory / "soak/observations.jsonl"
        values = [contract.parse_json(line) for line in path.read_bytes().splitlines()]
        function(values)
        path.write_text("".join(json.dumps(value) + "\n" for value in values))

    def api(self, endpoint):
        return copy.deepcopy(self.remote[endpoint])

    def verify(self):
        return contract.verify_bundle(self.directory, self.record, self.own_run, self.api)


class QualificationContractTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="qualification-unit-")
        self.addCleanup(self.temporary.cleanup)
        self.fixture = ProofFixture(Path(self.temporary.name))

    def test_complete_actual_proof_fixture_derives_every_required_gate(self):
        record = self.fixture.verify()
        self.assertEqual(record["checks"], sorted(contract.CHECKS))
        self.assertFalse((self.fixture.directory / "qualification.json").exists())

    def reject(self, message=None):
        self.fixture.refresh()
        with self.assertRaisesRegex(ValueError, message or "."):
            self.fixture.verify()

    def test_typed_summary_assertions_alone_are_rejected(self):
        record = dict(self.fixture.record, evidence={}, soak_seconds=86400, checks=sorted(contract.CHECKS))
        with self.assertRaisesRegex(ValueError, "evidence"):
            contract.verify_bundle(self.fixture.directory, record, self.fixture.own_run, self.fixture.api)

    def test_failed_late_workflow_cannot_authorize_promotion(self):
        self.fixture.own_run["conclusion"] = "failure"
        self.reject("must succeed")

    def test_same_source_pr_or_fork_evidence_is_rejected(self):
        self.fixture.remote[f"repos/{contract.REPOSITORY}/actions/runs/12"]["event"] = "pull_request"
        self.reject("trusted-main")

    def test_rerun_attempt_invalidates_retained_ci(self):
        self.fixture.remote[f"repos/{contract.REPOSITORY}/actions/runs/12"]["run_attempt"] = 2
        self.reject("stale")

    def test_unmerged_source_is_rejected(self):
        self.fixture.remote[f"repos/{contract.REPOSITORY}/compare/{SHA}...main"]["status"] = "diverged"
        self.reject("not merged")

    def test_version_mismatch_and_later_policy_fail_closed(self):
        self.fixture.remote[f"repos/{contract.REPOSITORY}/contents/VERSION?ref={SHA}"] = self.fixture.content(b"0.9.0-rc.1")
        self.reject("VERSION")
        self.fixture.record["version"] = "1.0.0"
        with self.assertRaisesRegex(ValueError, "only exact"):
            self.fixture.verify()

    def test_green_matrix_missing_rpc_http_test_is_rejected(self):
        filename = self.fixture.proof["ci"]["native"]["logs"]["native (ubuntu-24.04, address,undefined)"]
        path = self.fixture.directory / filename
        path.write_text(path.read_text().replace("endpoint_rpc_e2e", "omitted_test"))
        self.reject("test evidence")

    def test_actual_ctest_registration_name_is_required_not_cpp_executable(self):
        source = self.fixture.remote[f"repos/{contract.REPOSITORY}/contents/CMakeLists.txt?ref={SHA}"]
        text = base64.b64decode(source["content"]).decode().replace("NAME source_health_e2e ", "NAME source_health_pva_tests ")
        self.fixture.remote[f"repos/{contract.REPOSITORY}/contents/CMakeLists.txt?ref={SHA}"] = self.fixture.content(text.encode())
        self.reject("actual candidate CMake")

    def test_full_feature_ci_requires_actual_channelfinder_http_test(self):
        filename = self.fixture.proof["ci"]["native"]["logs"]["native (ubuntu-24.04, address,undefined)"]
        path = self.fixture.directory / filename
        path.write_text(path.read_text().replace("channelfinder_http_tests", "omitted_test"))
        self.reject("test evidence")

    def test_checked_native_contract_uses_authoritative_cmake_names(self):
        data = json.loads((ROOT / "tests/qualification-native-contract.json").read_text())
        import re
        names = set(re.findall(r"add_test\(NAME\s+([A-Za-z0-9_]+)", data["cmake_excerpt"]))
        self.assertEqual(names, contract.NATIVE_TESTS | contract.FULL_TESTS)
        self.assertEqual(data["revision"], "ee59df8e1451edf7b352c818ef08640a58a3e992")
        self.assertNotIn("source_health_pva_tests", names)

    def test_minimal_cannot_substitute_for_full_feature_sanitizer(self):
        filename = self.fixture.proof["ci"]["native"]["logs"]["native (ubuntu-24.04, address,undefined)"]
        path = self.fixture.directory / filename
        path.write_text(path.read_text().replace("INTEGRATIONS: ON", "INTEGRATIONS: OFF"))
        self.reject("full integrations")

    def test_wrong_platform_or_native_matrix_job_is_rejected(self):
        data = self.fixture.remote[f"repos/{contract.REPOSITORY}/actions/runs/12/attempts/1/jobs?per_page=100"]
        data["jobs"][0]["labels"] = ["ubuntu-24.04"]
        write(self.fixture.directory / "ci/native-jobs.json", data)
        self.reject("matrix job")

    def test_another_candidate_digest_is_rejected(self):
        self.fixture.proof["identity"]["image"] = OLD_IMAGE
        self.fixture.mutate("runtime-ownership.json", lambda r: r.update(identity=self.fixture.identity))
        self.reject("different candidate")

    def test_partial_or_lost_imaging_is_rejected(self):
        self.fixture.mutate(self.fixture.proof["frames"]["report"], lambda r: r.update(frames=599, pixels_checked=599 * 1920 * 1080))
        self.reject("600-frame")

    def test_target_10fps_label_without_actual_timing_is_rejected(self):
        self.fixture.mutate(self.fixture.proof["frames"]["report"], lambda r: r.pop("producer_steady_send_ns"))
        self.reject("steady-clock")

    def test_real_slow_producer_cannot_claim_10fps(self):
        start = contract.read(self.fixture.directory, self.fixture.proof["frames"]["execution"])["start_mono_ns"]
        # Extend the executed span too: failure must come from actual cadence,
        # not only a report/execution timing inconsistency.
        self.fixture.mutate(self.fixture.proof["frames"]["report"], lambda r: r.update(
            producer_steady_send_ns=[start + i * 105_000_000 for i in range(600)]))
        execute = self.fixture.proof["frames"]["execution"]
        self.fixture.mutate(execute, lambda r: r.update(end_mono_ns=r["start_mono_ns"] + 65 * 10**9,
                        elapsed_seconds=65, ended_at=utc(75)))
        self.reject("producer cadence")

    def test_no_unapproved_100ms_max_lateness_gate(self):
        # Average measured cadence satisfies the draft source policy. The
        # roadmap's existing procedure did not impose a maximum-lateness gate.
        self.fixture.mutate(self.fixture.proof["frames"]["report"], lambda r: r.update(producer_max_late_ms=150))
        self.fixture.refresh()
        self.fixture.verify()

    def test_capacity_requires_reload_and_backend_delay_axes(self):
        self.fixture.proof["capacity"] = [e for e in self.fixture.proof["capacity"] if e["axis"] != "backend-delay"]
        self.reject("capacity sweep|six axes")

    def test_claimed_capacity_action_outside_workload_is_rejected(self):
        self.fixture.mutate("soak/collector.json", lambda r: r["events"].__setitem__(1, self.fixture.event("backend-delay", 900)))
        self.reject("during actual workload")

    def test_lossy_exploration_never_passes_as_lossless_capacity(self):
        entry = next(e for e in self.fixture.proof["capacity"] if e["axis"] == "scalar")
        self.fixture.mutate(entry["report"], lambda r: r.update(received=9999, missed_updates=1))
        self.reject("lossy accepted")
        entry["classification"] = "exploration"
        self.fixture.refresh()
        self.fixture.verify()

    def test_idle_or_partial_hourly_work_is_not_representative_soak(self):
        self.fixture.mutate("soak/collector.json", lambda r: r["workloads"].pop())
        self.reject("missing imaging/multi-consumer")

    def test_short_or_partial_soak_is_rejected(self):
        self.fixture.mutate("soak/collector.json", lambda r: r.update(end_mono_ns=START_NS + 86399 * 10**9))
        self.reject("monotonic end")

    def test_authenticated_run_must_enclose_actual_collector_duration(self):
        self.fixture.own_run["updated_at"] = utc(600)
        self.reject("authenticated workflow")

    def test_stale_observation_timeline_is_rejected(self):
        self.fixture.journal(lambda rows: rows[100].update(utc=utc(0)))
        self.reject("clock mismatch")

    def test_unobserved_interval_is_rejected(self):
        self.fixture.journal(lambda rows: rows.pop(100))
        self.reject("unobserved soak")

    def test_no_percent_allowance_for_unexplained_write_failure(self):
        def failure(rows):
            rows[100]["producer"]["failed"] += 1
            rows[100]["producer"]["accepted"] -= 1
        self.fixture.journal(failure)
        self.reject("precise request attribution")

    def test_idle_pva_despite_live_producer_is_rejected(self):
        self.fixture.journal(lambda rows: rows[100]["pva"].update(scalar=rows[99]["pva"]["scalar"]))
        self.reject("workload stalled")

    def test_failure_after_fault_in_same_observation_interval_is_rejected(self):
        # Reviewer regression: 250 ms delay at 240 s cannot excuse a new
        # failure at 260 s first observed at 270 s, even with consistent counts.
        def failure(rows):
            for row in rows[9:]:
                row["producer"]["failed"] += 1
                row["producer"]["accepted"] -= 1
        self.fixture.journal(failure)
        path = self.fixture.directory / "runtime/producer-events.jsonl"
        with path.open("a") as stream:
            stream.write(json.dumps(dict(kind="failure", start_mono_ns=START_NS + 260 * 10**9,
                end_mono_ns=START_NS + 261 * 10**9, attempt=27001, accepted=27000, failed=1)) + "\n")
        self.reject("outside actual controlled")

    def test_valid_inflight_fault_request_is_attributed_and_recovery_required(self):
        # The request began before Docker pause and completed with an ambiguous
        # failure during it. It is not retried; later fresh samples recover.
        path = self.fixture.directory / "runtime/producer-events.jsonl"
        with path.open("a") as stream:
            stream.write(json.dumps(dict(kind="failure", start_mono_ns=START_NS + 359900000000,
                end_mono_ns=START_NS + 360900000000, attempt=36991, accepted=36990, failed=1)) + "\n")
        self.fixture.journal(lambda rows: [row["producer"].update(failed=1, accepted=row["producer"]["accepted"] - 1)
                                          for row in rows[13:]])
        def boundaries(manifest):
            for phase in ("during", "after"):
                item = manifest["events"][2]["producer_" + phase]
                item.update(failed=1, accepted=item["accepted"] - 1)
        self.fixture.mutate("soak/collector.json", boundaries)
        self.fixture.refresh()
        self.fixture.verify()

    def test_post_fault_pva_stall_in_same_interval_is_rejected(self):
        self.fixture.journal(lambda rows: rows[9]["pva"].update(scalar=25500, array=25500))
        self.reject("healthy subsegment")

    def test_catchup_cadence_cannot_hide_actual_producer_stall(self):
        path = self.fixture.directory / "runtime/producer-events.jsonl"
        with path.open("a") as stream:
            stream.write(json.dumps(dict(kind="stall", start_mono_ns=START_NS + 260 * 10**9,
                end_mono_ns=START_NS + 263 * 10**9, attempt=27001)) + "\n")
        self.reject("scheduling stall")

    def test_slow_hourly_fanout_cannot_claim_requested_1000hz(self):
        workloads = contract.read(self.fixture.directory, "soak/collector.json")["workloads"]
        for entry in workloads:
            if entry["axis"] == "fan-out":
                self.fixture.mutate(entry["report"], lambda r: r.update(produced_hz=600, elapsed_seconds=10000 / 600))
        self.reject("measured 1000 Hz")

    def test_wrong_running_digest_or_oom_is_rejected(self):
        self.fixture.journal(lambda rows: rows[100]["docker"].update(image=OLD_IMAGE, oom_killed=True))
        self.reject("private/runtime")

    def test_fault_exemption_cannot_cover_uncontrolled_stalls(self):
        self.fixture.journal(lambda rows: rows[100].update(event="outage"))
        self.reject("outside controlled")

    def test_outage_requires_observed_invalidity_and_recovery(self):
        self.fixture.mutate("soak/collector.json", lambda r: r["events"][2]["during"].update(ready=True))
        self.reject("invalidity")

    def test_rollback_restores_exact_config_and_acf_bytes(self):
        (self.fixture.directory / "rollback/restored.acf").write_text("ASG(NEW) { RULE(0, WRITE) }\n")
        self.reject("saved compatible acf")

    def test_version_label_without_actual_pva_rollback_is_rejected(self):
        self.fixture.mutate("rollback/report.json", lambda r: r["observations"][2]["pva"].update(array_exact=False))
        self.reject("PVA content")

    def test_failed_compatibility_check_cannot_be_replaced_by_typed_check_list(self):
        self.fixture.mutate("rollback/check-0.8.2.json", lambda r: r["State"].update(ExitCode=1))
        self.reject("compatibility check")

    def test_unpublished_baseline_is_not_qualified_rollback(self):
        endpoint = f"repos/{contract.REPOSITORY}/releases/tags/v0.8.2"
        self.fixture.remote[endpoint]["draft"] = True
        write(self.fixture.directory / "rollback/release.json", self.fixture.remote[endpoint])
        self.reject("published qualified")

    def test_missing_or_extra_changed_bytes_cannot_pass_original_record(self):
        (self.fixture.directory / "unknown.json").write_text("{}\n")
        with self.assertRaisesRegex(ValueError, "evidence"):
            self.fixture.verify()


class ArtifactSafetyTests(unittest.TestCase):
    def test_unsafe_zip_paths_links_duplicates_and_file_collisions_rejected_before_writing(self):
        for kind in ("absolute", "traversal", "backslash", "symlink", "duplicate", "collision"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                folder = Path(temporary)
                archive, destination = folder / "unsafe.zip", folder / "output"
                with zipfile.ZipFile(archive, "w") as bundle:
                    if kind in {"absolute", "traversal", "backslash"}:
                        bundle.writestr({"absolute": "/outside", "traversal": "../outside", "backslash": "a\\b"}[kind], b"bad")
                    elif kind == "symlink":
                        member = zipfile.ZipInfo("link")
                        member.external_attr = (stat.S_IFLNK | 0o777) << 16
                        bundle.writestr(member, b"outside")
                    elif kind == "duplicate":
                        import warnings
                        with warnings.catch_warnings():
                            warnings.simplefilter("ignore")
                            bundle.writestr("a", b"1")
                            bundle.writestr("a", b"2")
                    else:
                        bundle.writestr("a", b"1")
                        bundle.writestr("a/b", b"2")
                with self.assertRaises(ValueError):
                    contract.extract_archive(archive, destination)
                self.assertFalse(destination.exists())

    def test_tar_links_and_traversal_rejected_and_normal_archive_roundtrips(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            for name, symlink in (("../escape", False), ("link", True), ("candidate/candidate.json", False)):
                archive, destination = folder / "evidence.tar.gz", folder / "output"
                with tarfile.open(archive, "w:gz") as bundle:
                    info = tarfile.TarInfo(name)
                    if symlink:
                        info.type, info.linkname = tarfile.SYMTYPE, "../outside"
                        bundle.addfile(info)
                    else:
                        info.size = 3
                        bundle.addfile(info, io.BytesIO(b"{}\n"))
                if name.startswith("candidate/"):
                    contract.extract_archive(archive, destination, kind="tar")
                    self.assertEqual(contract.read(destination, name), {})
                else:
                    with self.assertRaises(ValueError):
                        contract.extract_archive(archive, destination, kind="tar")
                    self.assertFalse(destination.exists())

    def test_file_and_bundle_expansion_bounds_are_enforced(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            archive = folder / "large.zip"
            with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
                bundle.writestr("large", b"a" * 1024)
            with patch.object(contract, "MAX_FILE_BYTES", 100), self.assertRaisesRegex(ValueError, "expanded"):
                contract.extract_archive(archive, folder / "output")

    def test_json_duplicate_keys_nonfinite_and_bool_metrics_are_rejected(self):
        for data in (b'{"x":1,"x":2}', b'{"x":NaN}', b'{"x":Infinity}'):
            with self.assertRaises(ValueError):
                contract.parse_json(data)
        with self.assertRaises(ValueError):
            contract.integer(True, "counter")


class CollectorBoundaryTests(unittest.TestCase):
    def test_preflight_rejects_untrusted_ref_before_network_or_docker(self):
        with patch.dict("os.environ", {"GITHUB_REF": "refs/heads/dev/x"}), patch.object(collector, "command") as command:
            with self.assertRaisesRegex(ValueError, "trusted-main"):
                collector.prepare(None)
            command.assert_not_called()

    def test_preparation_08x_cannot_start_live_qualification(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "VERSION").write_text("0.8.2\n")
            env = dict(GITHUB_REF="refs/heads/main", GITHUB_EVENT_NAME="workflow_dispatch", GITHUB_REPOSITORY=contract.REPOSITORY, GITHUB_SHA=SHA)
            with patch.dict("os.environ", env), patch.object(collector, "ROOT", root), patch.object(collector, "command", return_value=SHA):
                with self.assertRaisesRegex(ValueError, "final VERSION"):
                    collector.prepare(None)

    def test_scope_refuses_existing_production_ids_and_changed_owner(self):
        with tempfile.TemporaryDirectory() as temporary:
            scope = collector.Scope(dict(scope="qualification-100-2-aaaaaaaaaaaa"), Path(temporary))
        with patch.object(collector, "command") as command:
            with self.assertRaisesRegex(ValueError, "unowned"):
                scope.own("f" * 64)
            command.assert_not_called()
            scope.containers["redis"] = "f" * 64
            command.return_value = json.dumps([dict(Id="f" * 64, Config=dict(Labels={collector.OWNER: "production"}))])
            with self.assertRaisesRegex(ValueError, "ownership"):
                scope.pause(True)
            self.assertEqual(command.call_count, 1)

    def test_redis_source_check_detects_alias_or_array_corruption(self):
        import struct
        class FakeRedis:
            def __init__(self, *args, **kwargs): pass
            def close(self): pass
            def command(self, _, key, first, last, *args):
                count = 4096 if key.endswith("array") else 1
                payload = struct.pack("<" + str(count) + "I", *range(1000, 1000 + count))
                return [[first.encode(), [b"_", payload]]]
        with patch.object(traffic, "Redis", FakeRedis):
            self.assertTrue(traffic.verify_content("private", "soak", probe())["source_match"])
            broken = probe()
            broken["alias"] = 999
            with self.assertRaisesRegex(ValueError, "content/alias"):
                traffic.verify_content("private", "soak", broken)

    def test_workflow_is_manual_read_only_main_and_pinned(self):
        text = (ROOT / ".github/workflows/qualify-image.yml").read_text()
        self.assertIn("workflow_dispatch:", text)
        self.assertNotIn("pull_request:", text)
        self.assertIn("github.ref == 'refs/heads/main'", text)
        self.assertIn("contents: read", text)
        self.assertIn("actions: read", text)
        self.assertIn("release-qualification-${{ github.run_id }}-${{ github.run_attempt }}", text)
        import re
        self.assertTrue(all(re.fullmatch(r"[a-zA-Z0-9_./-]+@[0-9a-f]{40}", action) for action in re.findall(r"uses:\s+(\S+)", text)))
        self.assertNotIn("--push", text)


if __name__ == "__main__":
    unittest.main()
