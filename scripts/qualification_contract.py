"""The release qualification proof contract. No caller-supplied check assertions.

The producer of this bundle is a reviewed workflow on main. Hashes alone are
not authentication: consumers also recheck the originating GitHub runs, jobs,
attempts, source ancestry, and the published rollback release through the API.
"""
import datetime as dt
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import stat
import tarfile
import zipfile

REPOSITORY = "fermi-ad/redis-pvxs-ioc"
SOURCE = "https://github.com/" + REPOSITORY
IMAGE_REPOSITORY = "adregistry.fnal.gov/instrumentation/redis-pvxs-ioc"
CHECKS = {"native-macos", "full-feature", "minimal", "sanitizers", "redis-pva",
          "discovery-catalog", "rpc-http", "imaging-600-frames", "capacity", "rollback"}
CANDIDATE_CHECKS = {"image", "smoke", "access", "sbom", "provenance", "source-inputs"}
ATTESTATIONS = {"source-inputs.json", "sbom.json", "provenance.json", "registry-manifest.json"}
MAX_FILES = 1024
MAX_FILE_BYTES = 32 * 1024 * 1024
MAX_BUNDLE_BYTES = 128 * 1024 * 1024
SHA = re.compile(r"[0-9a-f]{40}")
HASH = re.compile(r"[0-9a-f]{64}")
CONTAINER_ID = re.compile(r"[0-9a-f]{64}")
NATIVE_TESTS = {"config_tests", "access_runtime_tests", "source_health_e2e", "ndarray_transfer_e2e", "discovery_e2e"}
FULL_TESTS = {"endpoint_rpc_e2e", "rpc_hardening_e2e", "rpc_recovery_e2e", "channelfinder_http_tests"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def integer(value, name, minimum=0, maximum=2**63 - 1):
    require(type(value) is int and minimum <= value <= maximum, "invalid " + name)
    return value


def number(value, name, minimum=0, maximum=1e12):
    require(type(value) in (int, float) and math.isfinite(value)
            and minimum <= value <= maximum, "invalid " + name)
    return value


def timestamp(value):
    require(isinstance(value, str) and len(value) <= 40, "invalid UTC timestamp")
    result = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    require(result.tzinfo is not None and result.utcoffset() == dt.timedelta(0), "timestamp must be UTC")
    return result.timestamp()


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result


def parse_json(data):
    require(len(data) <= MAX_FILE_BYTES, "JSON evidence exceeds file bound")
    return json.loads(data, object_pairs_hook=unique_pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError("non-finite JSON: " + value)))


def relative_path(name):
    require(isinstance(name, str) and len(name) <= 240 and "\\" not in name
            and re.fullmatch(r"[A-Za-z0-9_./-]+", name), "unsafe artifact path")
    path = PurePosixPath(name)
    require(name and not path.is_absolute() and all(p not in ("", ".", "..") for p in name.split("/"))
            and ":" not in name, "unsafe artifact path: " + name)
    return path


def evidence_path(directory, name):
    path = Path(directory).joinpath(*relative_path(name).parts)
    for parent in (path, *path.parents):
        require(not parent.is_symlink(), "symlink in evidence path")
        if parent == Path(directory):
            break
    require(path.is_file() and path.stat().st_size <= MAX_FILE_BYTES, "missing or oversized evidence: " + name)
    return path


def digest(path):
    require(path.is_file() and not path.is_symlink() and path.stat().st_size <= MAX_FILE_BYTES,
            "invalid evidence file")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(directory, name):
    return parse_json(evidence_path(directory, name).read_bytes())


def inventory(directory, exclude=frozenset()):
    result = {}
    total = 0
    for path in sorted(Path(directory).rglob("*")):
        require(not path.is_symlink(), "symlink in evidence bundle")
        if path.is_dir():
            continue
        name = path.relative_to(directory).as_posix()
        relative_path(name)
        require(path.is_file(), "non-regular evidence file")
        total += path.stat().st_size
        require(total <= MAX_BUNDLE_BYTES and len(result) < MAX_FILES, "evidence bundle exceeds bound")
        if name not in exclude:
            result[name] = digest(path)
    return result


def extract_archive(archive, destination, kind="zip"):
    """Preflight every member before writing; never use extractall()."""
    destination = Path(destination)
    require(not destination.exists(), "artifact destination already exists")
    require(Path(archive).stat().st_size <= MAX_BUNDLE_BYTES, "compressed artifact exceeds bound")
    opener = zipfile.ZipFile if kind == "zip" else tarfile.open
    with opener(archive) as bundle:
        members = bundle.infolist() if kind == "zip" else bundle.getmembers()
        require(len(members) <= MAX_FILES, "too many artifact entries")
        seen, files, total = set(), [], 0
        for member in members:
            name = member.filename if kind == "zip" else member.name
            directory = member.is_dir() if kind == "zip" else member.isdir()
            if directory:
                name = name.rstrip("/")
            relative_path(name)
            require(name not in seen, "duplicate artifact entry")
            seen.add(name)
            if kind == "zip":
                mode = member.external_attr >> 16
                require(not stat.S_ISLNK(mode) and (not stat.S_IFMT(mode) or
                        stat.S_ISREG(mode) or stat.S_ISDIR(mode)), "non-regular ZIP entry")
                size = member.file_size
            else:
                require(directory or member.isfile(), "non-regular tar entry")
                size = member.size
            total += size
            require(size <= MAX_FILE_BYTES and total <= MAX_BUNDLE_BYTES, "expanded artifact exceeds bound")
            if not directory:
                files.append((name, member, size))
        file_names = {name for name, _, _ in files}
        require(not any(str(parent) in file_names for name in seen
                        for parent in PurePosixPath(name).parents), "file/directory artifact collision")
        destination.mkdir(parents=True)
        for name, member, size in files:
            path = destination.joinpath(*relative_path(name).parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            stream = bundle.open(member) if kind == "zip" else bundle.extractfile(member)
            with stream, path.open("xb") as output:
                data = stream.read(size + 1)
                require(len(data) == size, "artifact size mismatch")
                output.write(data)


def immutable_image(value):
    require(isinstance(value, str) and re.fullmatch(re.escape(IMAGE_REPOSITORY) +
            r"@sha256:[0-9a-f]{64}", value), "expected project immutable image")
    return value


def validate_run(info, revision, workflow, dispatch_only=False, active=False):
    require(info.get("head_sha") == revision and info.get("head_branch") == "main"
            and info.get("path") == ".github/workflows/" + workflow
            and info.get("event") in ({"workflow_dispatch"} if dispatch_only else {"push", "workflow_dispatch"}),
            "run is not matching trusted-main evidence")
    require(info.get("repository", {}).get("full_name") == REPOSITORY
            and info.get("head_repository", {}).get("full_name") == REPOSITORY,
            "run repository identity mismatch")
    integer(info.get("id"), "run ID", 1)
    integer(info.get("run_attempt"), "run attempt", 1)
    require((info.get("status") == "completed" and info.get("conclusion") == "success") or
            (active and info.get("status") == "in_progress" and info.get("conclusion") is None),
            "evidence workflow must succeed")
    timestamp(info["run_started_at"])
    timestamp(info["updated_at"])
    return info


def authenticated_run(retained, api, revision, workflow, dispatch_only=False, active=False):
    live = api(f"repos/{REPOSITORY}/actions/runs/{integer(retained.get('id'), 'run ID', 1)}")
    validate_run(live, revision, workflow, dispatch_only, active)
    for field in ("id", "run_attempt", "head_sha", "head_branch", "event", "path", "run_started_at"):
        require(retained.get(field) == live.get(field), "stale or mismatched run metadata: " + field)
    validate_run(retained, revision, workflow, dispatch_only, active)
    return live


def validate_candidate(record, directory, prefix, run_info, version, revision):
    require(record.get("version") == version and record.get("revision") == revision
            and record.get("run_id") == run_info["id"] and record.get("run_attempt") == run_info["run_attempt"]
            and record.get("platform") == "linux/amd64"
            and CANDIDATE_CHECKS.issubset(record.get("checks", [])), "candidate identity or checks mismatch")
    immutable_image(record.get("image"))
    for name in ATTESTATIONS:
        require(record.get("evidence", {}).get(name) == digest(evidence_path(directory, prefix + name)),
                "candidate attestation checksum mismatch")
    source = read(directory, prefix + "source-inputs.json")
    require(source.get("revision") == revision and source.get("repository") == SOURCE
            and isinstance(source.get("submodules"), list) and source["submodules"], "candidate source inventory mismatch")
    paths = set()
    for module in source["submodules"]:
        relative_path(module.get("path"))
        require(module["path"] not in paths and SHA.fullmatch(module.get("revision", "")), "invalid pinned source inventory")
        paths.add(module["path"])
    require(read(directory, prefix + "registry-manifest.json").get("digest") == record["image"].split("@")[1],
            "candidate manifest digest mismatch")
    sbom = read(directory, prefix + "sbom.json")
    spdx = sbom.get("linux/amd64", sbom).get("SPDX", {})
    provenance = read(directory, prefix + "provenance.json")
    slsa = provenance.get("linux/amd64", provenance).get("SLSA", {})
    require(spdx.get("spdxVersion") and spdx.get("packages") and slsa.get("buildConfig") and slsa.get("materials")
            and slsa.get("invocation", {}).get("parameters", {}).get("args", {}).get("build-arg:REDIS_PVXS_IOC_REVISION") == revision,
            "missing full candidate attestations")


def successful_steps(job, names):
    require(job.get("conclusion") == "success" and job.get("status") == "completed", "required CI job failed")
    steps = {step["name"]: step for step in job.get("steps", [])}
    require(all(name in steps and steps[name].get("status") == "completed"
                and steps[name].get("conclusion") == "success" for name in names), "required CI steps did not succeed")


def validate_ci(directory, proof, revision, api):
    import base64
    cmake = api(f"repos/{REPOSITORY}/contents/CMakeLists.txt?ref={revision}")
    require(cmake.get("encoding") == "base64", "missing same-source CMake CI contract")
    registered = set(re.findall(r"add_test\(NAME\s+([A-Za-z0-9_]+)", base64.b64decode(cmake["content"]).decode()))
    require((NATIVE_TESTS | FULL_TESTS).issubset(registered), "required CTest names are absent from actual candidate CMake source")
    native = api(f"repos/{REPOSITORY}/contents/.github/workflows/native.yml?ref={revision}")
    require(native.get("encoding") == "base64", "missing same-source native workflow contract")
    workflow_source = base64.b64decode(native["content"]).decode()
    require(all(text in workflow_source for text in ("ctest --test-dir build --output-on-failure", "address,undefined",
        "REDIS_PVXS_IOC_ENABLE_GRPC", "REDIS_PVXS_IOC_ENABLE_CHANNELFINDER", "Test with real RecCeiver and its ChannelFinder processor")),
        "actual native workflow does not implement required CI contract")
    for kind, workflow in (("native", "native.yml"), ("image", "ci-image.yml")):
        retained = read(directory, proof[kind]["run"])
        run_info = authenticated_run(retained, api, revision, workflow)
        jobs = read(directory, proof[kind]["jobs"])
        live = api(f"repos/{REPOSITORY}/actions/runs/{run_info['id']}/attempts/{run_info['run_attempt']}/jobs?per_page=100")
        require(jobs == live and jobs.get("total_count") == len(jobs.get("jobs", [])), "CI jobs stale, partial, or mismatched")
        for job in jobs["jobs"]:
            require(job.get("run_id") == run_info["id"] and job.get("head_sha") == revision, "CI job source mismatch")
        if kind == "image":
            selected = [j for j in jobs["jobs"] if j.get("name") == "validate" and "adlinux3" in j.get("labels", [])]
            require(len(selected) == 1, "missing release-equivalent image CI")
            successful_steps(selected[0], ["Build validation image", "Validate image metadata", "Validate runtime image",
                                           "Validate isolated Redis/PVA and access behavior"])
        else:
            # GitHub's job labels are runs-on selectors, not necessarily the
            # automatic OS/architecture labels of self-hosted runners.
            for job_name, labels in (("native (macos-14, none)", {"macos-14"}),
                                     ("native (ubuntu-24.04, none)", {"ubuntu-24.04"}),
                                     ("native (ubuntu-24.04, address,undefined)", {"ubuntu-24.04"})):
                selected = [j for j in jobs["jobs"] if j.get("name") == job_name and labels.issubset(j.get("labels", []))]
                require(len(selected) == 1, "missing native/minimal/sanitizer CI matrix job: " + job_name)
                successful_steps(selected[0], ["Configure and build service", "Test native runtime and access control",
                                               "Test with real RecCeiver and its ChannelFinder processor"])
                log = evidence_path(directory, proof[kind].get("logs", {}).get(job_name, "")).read_text()
                tests = NATIVE_TESTS
                if "address,undefined" in job_name:
                    tests = tests | FULL_TESTS
                    require(re.search(r"INTEGRATIONS:\s+ON", log) and re.search(r"SANITIZERS:\s+address,undefined", log),
                            "sanitizer CI did not configure full integrations")
                else:
                    require(re.search(r"INTEGRATIONS:\s+OFF", log), "minimal/native CI did not disable optional integrations")
                require(all(re.search(r"Test\s+#\d+:\s+" + re.escape(name) + r"\s+\.+\s+Passed", log) for name in tests),
                        "required runtime/discovery/RPC-HTTP test evidence is absent")
                require("real RecCeiver with in-memory ChannelFinder client:" in log,
                        "real RecCeiver/catalog acceptance evidence missing")


def validate_policy(policy):
    require(type(policy.get("schema")) is int and policy.get("schema") == 1 and policy.get("version") == "0.9.0"
            and policy.get("profile") == "scalar-array-imaging-fanout-v1", "unsupported qualification policy")
    # The exact source policy is reviewed with the protected-main changes.
    # Workflow dispatch cannot change its acceptance limits or assert approval.
    require(isinstance(policy.get("pacing_rationale"), str) and len(policy["pacing_rationale"]) >= 20,
            "10 fps pacing policy requires an explicit rationale")
    require(policy.get("frame_min_produced_hz") == 9.99 and policy.get("frame_max_produced_hz") == 10.01,
            "unrecognized average 10 fps cadence policy")
    require(policy.get("fanout_min_produced_hz") == 999 and policy.get("fanout_max_produced_hz") == 1002,
            "unrecognized measured representative fan-out cadence policy")
    number(policy.get("soak_max_rss_growth_bytes"), "approved RSS growth limit", 0, 512 * 1024**2)
    require(policy.get("soak_seconds") == 86400 and policy.get("scalar_rate_hz") == 100
            and policy.get("array_elements") == 4096 and policy.get("fanout_clients") == 16
            and policy.get("periodic_workload_seconds") == 3600 and policy.get("observation_seconds") == 30,
            "unrecognized representative soak policy")


def execution(directory, entry, identity):
    result = read(directory, entry["execution"])
    for key in ("revision", "image", "run_id", "run_attempt", "scope"):
        require(result.get(key) == identity[key], "workload execution identity mismatch")
    require(result.get("exit_code") == 0 and result.get("platform") == "linux/amd64"
            and CONTAINER_ID.fullmatch(result.get("ioc_container", "")), "workload did not succeed on the candidate")
    seconds = number(result.get("elapsed_seconds"), "workload duration", 0.001, 900)
    start = integer(result.get("start_mono_ns"), "workload monotonic start", 1)
    end = integer(result.get("end_mono_ns"), "workload monotonic end", start + 1)
    require(abs((end - start) / 1e9 - seconds) <= 0.001, "workload monotonic duration mismatch")
    require(abs(timestamp(result["ended_at"]) - timestamp(result["started_at"]) - seconds) <= 2,
            "workload clock/duration mismatch")
    require(result.get("report_sha256") == digest(evidence_path(directory, entry["report"])), "workload report checksum mismatch")
    require(isinstance(result.get("argv"), list) and result["argv"] and
            all(isinstance(arg, str) and len(arg) < 1024 for arg in result["argv"]), "missing actual workload argv")
    return read(directory, entry["report"]), result


def validate_frames(report, executed, policy):
    require(report.get("passed") is True and (report.get("frames"), report.get("width"), report.get("height"),
            report.get("fps_target"), report.get("format")) == (600, 1920, 1080, 10, "Mono8"), "not the full 600-frame 1080p Mono8 workload")
    require(report.get("pixels_checked") == 600 * 1920 * 1080 and report.get("unexplained_gaps") == 0
            and report.get("duplicates") == 0, "partial/corrupt/lost/duplicate imaging frames")
    require(59.9 <= number(report.get("elapsed_seconds"), "frame duration") <= 90
            and executed["elapsed_seconds"] <= 90 and executed.get("timeout_seconds") == 90,
            "imaging run does not satisfy the accepted 90-second bound")
    number(report.get("producer_max_late_ms"), "frame pacing lateness")
    times = report.get("producer_steady_send_ns", [])
    require(len(times) == 600 and all(type(value) is int and value > 0 for value in times)
            and report.get("timing_instrumentation") == "steady-send-v1"
            and HASH.fullmatch(executed.get("instrumentation_sha256", "")), "missing actual producer steady-clock send timings")
    require(all(b > a for a, b in zip(times, times[1:])), "producer steady send clock regressed")
    require(executed["start_mono_ns"] <= times[0] < times[-1] <= executed["end_mono_ns"],
            "producer send timings are outside actual workload execution")
    produced_hz = 599 * 1e9 / (times[-1] - times[0])
    require(policy["frame_min_produced_hz"] <= produced_hz <= policy["frame_max_produced_hz"],
            "actual producer cadence does not satisfy reviewed 10 fps policy")
    for flag, expected in (("--frames", "600"), ("--width", "1920"), ("--height", "1080"), ("--fps", "10")):
        argv = executed["argv"]
        require(flag in argv and argv.index(flag) + 1 < len(argv) and argv[argv.index(flag) + 1] == expected,
                "frame command differs from accepted procedure")
    require(report.get("monitor_queue_entries") == 16 and report.get("redis_history_entries") == 16,
            "imaging history/queue bound differs")
    latency(report)


def latency(report):
    values = [number(report.get("latency_p" + str(n) + "_ms"), "latency") for n in (50, 95, 99)]
    require(values == sorted(values), "invalid latency quantiles")


def validate_capacity(directory, entries, identity, policy, events):
    require(isinstance(entries, list) and 6 <= len(entries) <= 64, "capacity sweep evidence missing")
    axes = set()
    for entry in entries:
        axis = entry.get("axis")
        require(axis in {"scalar", "array", "imaging", "fan-out", "reload", "backend-delay"}, "unknown capacity axis")
        axes.add(axis)
        report, executed = execution(directory, entry, identity)
        if axis == "imaging":
            validate_frames(report, executed, policy)
            continue
        samples = integer(report.get("samples"), "capacity samples", 1, 100000)
        clients = integer(report.get("clients"), "capacity clients", 1, 64)
        elements = integer(report.get("elements"), "capacity elements", 1, 8388608)
        require(report.get("payload_bytes") == elements * 4 and report.get("final_values_converged") is True,
                "capacity payload/content or final convergence failure")
        missed = integer(report.get("missed_updates"), "capacity missed updates", 0, samples * clients)
        duplicates = integer(report.get("duplicate_updates"), "capacity duplicates", 0, samples * clients)
        received = integer(report.get("received"), "capacity received updates", 1, 1000000)
        require(received + missed == samples * clients, "inconsistent capacity accounting")
        number(report.get("produced_hz"), "actual capacity producer rate", 0.001, 20000)
        number(report.get("producer_max_late_ms"), "capacity producer lateness")
        latency(report)
        require(report.get("monitor_queue_limit") == 16 and report.get("monitor_queue_peak", 17) <= 16
                and report.get("redis_history_entries") == 16, "capacity queue/history bound differs")
        require(entry.get("classification") in {"lossless", "exploration"}, "capacity point requires explicit classification")
        if entry["classification"] == "lossless":
            require(missed == 0 and duplicates == 0 and received == samples * clients, "lossy accepted capacity point")
        if axis == "scalar":
            require(elements == 1 and clients == 1, "scalar capacity axis mismatch")
        if axis == "array":
            require(elements == 4096 and clients == 1, "array capacity axis mismatch")
        if axis == "fan-out":
            require(clients == 16, "fan-out capacity axis mismatch")
        if axis in {"reload", "backend-delay"}:
            event = events.get(entry.get("event"))
            require(event and event["kind"] == axis and event["start_mono_ns"] >= executed["start_mono_ns"]
                    and event["end_mono_ns"] <= executed["end_mono_ns"], "capacity action not observed during actual workload")
    require(axes == {"scalar", "array", "imaging", "fan-out", "reload", "backend-delay"}, "capacity sweep requires all six axes")


def validate_probe(probe, legacy=False, ready=True):
    require(type(probe.get("alias")) is int and probe["alias"] > 0 and type(probe.get("scalar")) is int
            and probe["scalar"] > 0 and type(probe.get("array")) is int and probe["array"] > 0
            and probe.get("array_elements") == 4096 and probe.get("array_exact") is True,
            "PVA content/alias mismatch")
    integer(probe.get("generation"), "config generation", 1)
    for name in ("scalar_time_ns", "alias_time_ns", "array_time_ns"):
        integer(probe.get(name), "source timestamp", 1)
    if ready:
        require(probe.get("source_match") is True and set(probe.get("source_cursors", {})) == {"scalar", "alias", "array"},
                "PVA samples lack exact Redis source/cursor verification")
        for kind in ("scalar", "alias", "array"):
            nanos = probe[kind + "_time_ns"]
            require(probe["source_cursors"][kind] == f"{nanos // 1000000}-{nanos % 1000000}", "source cursor/timestamp identity mismatch")
        require(probe.get("scalar_alarm") == 0 and probe.get("array_alarm") == 0, "PVA sample invalid")
        require(probe.get("backend_connected") is True, "Redis backend unavailable")
        if not legacy:
            require(probe.get("ready") is True, "candidate required sources are not ready")


def validate_soak(directory, item, identity, policy, run_info):
    manifest = read(directory, item["manifest"])
    for key in ("revision", "image", "run_id", "run_attempt", "scope"):
        require(manifest.get(key) == identity[key], "soak identity mismatch")
    require(manifest.get("platform") == "linux/amd64" and manifest.get("completed") is True,
            "incomplete/non-amd64 soak")
    start = integer(manifest.get("start_mono_ns"), "soak monotonic start", 1)
    end = integer(manifest.get("end_mono_ns"), "soak monotonic end", start + 86400 * 10**9)
    seconds = (end - start) / 1e9
    require(manifest.get("soak_seconds") == seconds and seconds <= 90000, "soak duration mismatch")
    started, ended = timestamp(manifest["started_at"]), timestamp(manifest["ended_at"])
    # GitHub's in-progress updated_at need not advance while a long step runs.
    # Creation uses the actual collector clock; promotion requires the eventual
    # successful run's authenticated completion time to enclose the whole soak.
    authenticated_end = timestamp(run_info["updated_at"])
    if run_info["status"] == "in_progress":
        authenticated_end = max(authenticated_end, dt.datetime.now(dt.timezone.utc).timestamp())
    require(abs(ended - started - seconds) <= 5 and started >= timestamp(run_info["run_started_at"])
            and ended <= authenticated_end,
            "soak does not fit authenticated workflow elapsed time")
    config = read(directory, item["config"])
    require(manifest.get("config_sha256") == digest(evidence_path(directory, item["config"]))
            and config.get("redis", {}).get("reader_probe_ms", 1000) == 1000,
            "soak configuration/polling policy mismatch")
    for name in ("ioc_container", "redis_container", "tools_container"):
        require(CONTAINER_ID.fullmatch(manifest.get(name, "")), "missing owned container ID")
    require(len({manifest[n] for n in ("ioc_container", "redis_container", "tools_container")}) == 3,
            "private runtime identities collide")
    require(manifest.get("network_internal") is True and manifest.get("host_ports") == []
            and manifest.get("ioc_memory_bytes") == 512 * 1024**2 and manifest.get("ioc_nano_cpus") == 10**9,
            "soak private/resource boundary mismatch")
    events = {}
    for event in manifest.get("events", []):
        require(event.get("id") not in events and event.get("kind") in {"reload", "backend-delay", "outage"},
                "invalid controlled event")
        a = integer(event.get("start_mono_ns"), "event start", start)
        b = integer(event.get("end_mono_ns"), "event end", a + 1, end)
        require(b - a <= 120 * 10**9 and event.get("container") in
                {manifest["ioc_container"], manifest["redis_container"]}, "unbounded/unowned controlled fault")
        before, during, after = event["before"], event["during"], event["after"]
        validate_probe(before)
        validate_probe(during, ready=event["kind"] == "reload")
        validate_probe(after)
        require(after["scalar"] > before["scalar"] and after["array"] > before["array"], "controlled action did not recover new PVA content")
        if event["kind"] == "reload":
            require(event.get("action") == "SIGHUP" and after["generation"] == before["generation"] + 1
                    and event.get("config_before_sha256") != event.get("config_after_sha256"), "reload did not publish the actual new generation")
        elif event["kind"] == "outage":
            require(event.get("action") in {"redis-client-pause", "docker-pause"} and during.get("ready") is False
                    and during["scalar"] >= before["scalar"] and during["array"] >= before["array"],
                    "fault lacks observed invalidity/retained content/recovery")
        else:
            require(event.get("action") == "redis-client-pause" and event.get("pause_ms") == 250
                    and number(event.get("measured_redis_delay_ms"), "actual backend delay") >= 200,
                    "backend delay lacks actual measured command latency")
        if event["kind"] != "reload":
            op_start = integer(event.get("operation_start_mono_ns"), "fault operation start", a, b)
            integer(event.get("operation_end_mono_ns"), "fault operation end", op_start + 1, b)
            for phase in ("before", "during", "after"):
                require(isinstance(event.get("producer_" + phase), dict), "fault lacks producer boundary evidence")
        events[event["id"]] = event
    require({e["kind"] for e in events.values()} == {"reload", "backend-delay", "outage"},
            "soak lacks actual reload/delay/outage injection")
    failures = []
    producer_events = evidence_path(directory, item["producer_events"])
    ledger = [parse_json(line) for line in producer_events.read_bytes().splitlines()]
    require(ledger and ledger[0].get("kind") == "start" and type(ledger[0].get("schema")) is int and ledger[0].get("schema") == 1
            and ledger[0].get("socket_timeout_ms") == 1000 and ledger[0].get("rate_hz") == 100,
            "producer request ledger missing")
    require(len(ledger) <= 8193, "producer request ledger exceeds bound")
    for entry in ledger[1:]:
        require(entry.get("kind") == "failure", "producer has an unexplained scheduling stall/unbounded request")
        a = integer(entry.get("start_mono_ns"), "failed request start", ledger[0]["schedule_start_mono_ns"])
        b = integer(entry.get("end_mono_ns"), "failed request end", a, a + 4 * 10**9)
        require(any(e["kind"] in {"outage", "backend-delay"} and a <= e["operation_end_mono_ns"]
                    and b >= e["operation_start_mono_ns"] for e in events.values()),
                "producer failed/ambiguous request outside actual controlled fault operation")
        require(entry.get("failed") == len(failures) + 1 and entry.get("attempt") == entry.get("accepted", -1) + entry["failed"],
                "producer failure ledger accounting mismatch")
        failures.append(entry)

    def producer_boundary(progress):
        observed = integer(progress.get("mono_ns"), "producer boundary observation", ledger[0]["schedule_start_mono_ns"])
        require(progress.get("schedule_start_mono_ns") == ledger[0]["schedule_start_mono_ns"]
                and progress.get("failed") == sum(f["end_mono_ns"] <= observed for f in failures),
                "producer failure counters lack precise request attribution")

    for event in events.values():
        if event["kind"] != "reload":
            for phase in ("before", "during", "after"):
                producer_boundary(event["producer_" + phase])
    journal = evidence_path(directory, item["observations"])
    require(digest(journal) == manifest.get("observations_sha256"), "observation journal checksum mismatch")
    previous, observations, first_rss, last_rss = None, 0, None, None
    for line in journal.read_bytes().splitlines():
        sample = parse_json(line)
        mono = integer(sample.get("mono_ns"), "observation monotonic time", start, end)
        require(sample.get("ioc_container") == manifest["ioc_container"] and sample.get("scope") == identity["scope"],
                "observation targets another runtime")
        timestamp(sample["utc"])
        require(abs(timestamp(sample["utc"]) - started - (mono - start) / 1e9) <= 5, "observation UTC/monotonic clock mismatch")
        fault = sample.get("event")
        if fault is not None:
            require(fault in events and events[fault]["start_mono_ns"] <= mono <= events[fault]["end_mono_ns"],
                    "observation fault exemption outside controlled window")
        state = sample["docker"]
        require(state.get("running") is True and state.get("oom_killed") is False and state.get("restart_count") == 0
                and state.get("memory_limit_bytes") == 512 * 1024**2 and state.get("nano_cpus") == 10**9
                and state.get("host_ports") == [] and state.get("readonly_rootfs") is True
                and state.get("cap_drop") == ["ALL"] and state.get("owner") == identity["scope"]
                and state.get("image") == identity["image"] and re.fullmatch(r"sha256:[0-9a-f]{64}", state.get("image_id", "")),
                "Docker observation violates private/runtime boundary")
        rss = integer(sample.get("rss_bytes"), "observed IOC RSS", 1, 512 * 1024**2)
        integer(sample.get("cpu_ticks"), "observed IOC CPU ticks", 0)
        producer = sample["producer"]
        producer_boundary(producer)
        integer(producer.get("accepted"), "accepted producer samples", 1)
        integer(producer.get("failed"), "failed producer writes", 0)
        require(producer.get("pid_alive") is True and producer.get("rate_hz") == 100
                and producer.get("array_elements") == 4096, "representative producer stopped/changed")
        attempts = integer(producer.get("value"), "producer attempts", 1)
        require(attempts == producer["accepted"] + producer["failed"], "producer success/failure accounting mismatch")
        due_start = integer(producer.get("schedule_start_mono_ns"), "producer schedule start", 1, mono)
        attempted_at = integer(producer.get("last_attempt_mono_ns"), "producer last attempt", due_start, mono)
        require(mono - integer(producer.get("mono_ns"), "producer observation time", due_start, mono) <= 3 * 10**9,
                "stale producer progress observation")
        number(producer.get("producer_max_late_ms"), "producer scheduling lateness")
        if fault is None and attempts > 1000:
            measured_rate = (attempts - 1) * 1e9 / max(1, attempted_at - due_start)
            require(99.9 <= measured_rate <= 100.1, "representative producer did not maintain measured nominal 100 Hz cadence")
        validate_probe(sample["pva"], ready=fault is None)
        if previous:
            gap = (mono - previous["mono_ns"]) / 1e9
            require(0 < gap <= 45, "unobserved soak interval")
            require(producer["accepted"] >= previous["producer"]["accepted"] and
                    producer["failed"] >= previous["producer"]["failed"] and
                    sample["cpu_ticks"] >= previous["cpu_ticks"], "soak counter regressed")
            # Validate healthy subsegments. A fault does not excuse the whole
            # 30-second observation interval. Actual request intervals above
            # attribute every failed write, including one already in flight.
            faults = sorted((e for e in events.values() if e["kind"] in {"outage", "backend-delay"}
                             and e["start_mono_ns"] <= mono and e["end_mono_ns"] >= previous["mono_ns"]),
                            key=lambda e: e["start_mono_ns"])
            left, left_time = previous["pva"], previous["mono_ns"]
            segments = []
            for event in faults:
                if event["start_mono_ns"] > left_time:
                    segments.append((left, event["before"], event["start_mono_ns"] - left_time))
                left, left_time = event["after"], event["end_mono_ns"]
            if mono > left_time:
                segments.append((left, sample["pva"], mono - left_time))
            for before, after, interval in segments:
                if interval >= 2 * 10**9:
                    require(after["scalar"] > before["scalar"] and after["array"] > before["array"],
                            "PVA workload stalled in a healthy subsegment outside controlled fault")
                for field in ("read_failures", "read_rejections", "stream_resets", "retention_gaps"):
                    require(after.get(field) == before.get(field), "unexplained source-reader failure/discontinuity outside controlled fault")
            require(producer["accepted"] > previous["producer"]["accepted"] or fault is not None,
                    "producer workload stalled in a healthy interval")
        else:
            require(mono - start <= 45 * 10**9 and producer["failed"] == 0, "soak starts with missing/failing evidence")
            first_rss = rss
        previous, last_rss = sample, rss
        observations += 1
    require(observations >= 2881 and previous and end - previous["mono_ns"] <= 45 * 10**9,
            "partial 24-hour observation journal")
    require(last_rss - first_rss <= policy["soak_max_rss_growth_bytes"], "soak RSS growth fails reviewed policy")
    # Each hourly interval must contain measured imaging and 16-client work.
    workloads = manifest.get("workloads", [])
    for hour in range(24):
        selected = [w for w in workloads if hour * 3600 <= (w.get("start_mono_ns", 0) - start) / 1e9 < (hour + 1) * 3600]
        require({w.get("axis") for w in selected} >= {"imaging", "fan-out"}, "representative soak missing imaging/multi-consumer activity")
        for entry in selected:
            report, executed = execution(directory, entry, identity)
            require(entry["start_mono_ns"] == executed["start_mono_ns"] and executed["end_mono_ns"] <= end,
                    "periodic workload outside observed soak")
            if entry["axis"] == "imaging":
                validate_frames(report, executed, policy)
            elif entry["axis"] == "fan-out":
                require(report.get("clients") == 16 and report.get("samples") == 10000
                        and report.get("rate_target_hz") == 1000 and report.get("received") == 160000
                        and report.get("missed_updates") == 0 and report.get("duplicate_updates") == 0
                        and report.get("final_values_converged") is True, "soak multi-consumer workload incomplete/lossy")
                require(policy["fanout_min_produced_hz"] <= number(report.get("produced_hz"), "measured fan-out rate")
                        <= policy["fanout_max_produced_hz"], "representative fan-out did not maintain measured 1000 Hz cadence")
                latency(report)
    return seconds, events


def validate_rollback(directory, item, identity, api, qualification_run):
    report = read(directory, item["report"])
    baseline = read(directory, item["candidate"])
    revision = baseline.get("revision", "")
    require(SHA.fullmatch(revision), "invalid rollback source revision")
    retained = read(directory, item["run"])
    run_info = authenticated_run(retained, api, revision, "candidate-image.yml", dispatch_only=True)
    validate_candidate(baseline, directory, item["candidate"].removesuffix("candidate.json"), run_info, "0.8.2", revision)
    release = read(directory, item["release"])
    live = api(f"repos/{REPOSITORY}/releases/tags/v0.8.2")
    require(release == live and live.get("draft") is False and live.get("prerelease") is False
            and live.get("tag_name") == "v0.8.2" and live.get("published_at"), "rollback image is not the published qualified 0.8.2")
    assets = {a["name"]: a for a in live.get("assets", [])}
    require("candidate.json" in assets and "release-evidence.tar.gz" in assets, "rollback release lacks candidate qualification assets")
    tag = read(directory, item["tag"])
    live_tag = api(f"repos/{REPOSITORY}/git/ref/tags/v0.8.2")["object"]
    if live_tag.get("type") == "tag":
        live_tag = api(f"repos/{REPOSITORY}/git/tags/{live_tag['sha']}")["object"]
    require(tag == live_tag and tag.get("type") == "commit" and tag.get("sha") == revision, "rollback release tag/source mismatch")
    require(report.get("baseline_candidate_sha256") == digest(evidence_path(directory, item["candidate"]))
            and report.get("candidate_image") == identity["image"] and report.get("baseline_image") == baseline["image"]
            and report.get("baseline_version") == "0.8.2", "rollback image identity mismatch")
    for key in ("run_id", "run_attempt", "scope"):
        require(report.get(key) == identity[key], "rollback execution identity mismatch")
    for kind in ("config", "acf"):
        saved = digest(evidence_path(directory, item["saved_" + kind]))
        restored = digest(evidence_path(directory, item["restored_" + kind]))
        require(saved == restored == report.get(kind + "_sha256"), "rollback did not restore saved compatible " + kind + " bytes")
    require(report.get("saved_before_candidate") is True and report.get("compatibility_checks") == ["0.8.2", identity["version"]],
            "saved configuration lacks both actual compatibility checks")
    for expected in (baseline, identity):
        check = read(directory, "rollback/check-" + expected["version"] + ".json")
        require(check.get("Config", {}).get("Image") == expected["image"]
                and check.get("State", {}).get("ExitCode") == 0 and check.get("State", {}).get("Status") == "exited"
                and check.get("Args") == ["--check-config", "/fixture/config.json"], "actual saved-config compatibility check missing or failed")
    sequence = report.get("observations", [])
    require(len(sequence) == 3 and [s.get("phase") for s in sequence] == ["baseline-before", "candidate", "baseline-restored"],
            "rollback sequence incomplete")
    ids = set()
    previous_time = 0
    for index, observation in enumerate(sequence):
        expected = baseline if index != 1 else identity
        require(observation.get("image") == expected["image"] and observation.get("version") == expected["version"]
                and observation.get("revision") == expected["revision"] and observation.get("platform") == "linux/amd64"
                and CONTAINER_ID.fullmatch(observation.get("container", "")), "rollback observed a different runtime")
        require(observation["container"] not in ids and observation.get("binary_identity") ==
                f"redis-pvxs-ioc {expected['version']} ({expected['revision']})", "rollback binary/container identity mismatch")
        ids.add(observation["container"])
        config_hash = report["config_sha256"] if index != 1 else digest(evidence_path(directory, "soak/config.json"))
        require(observation.get("config_sha256") == config_hash and observation.get("acf_sha256") == report["acf_sha256"],
                "running IOC did not load the retained fixture configuration/ACF bytes")
        inspected = observation.get("inspect", {})
        require(inspected.get("Id") == observation["container"] and inspected.get("Config", {}).get("Image") == expected["image"]
                and inspected.get("State", {}).get("Running") is True and inspected.get("State", {}).get("OOMKilled") is False,
                "rollback lacks actual matching running-container evidence")
        require(timestamp(observation["utc"]) > previous_time, "rollback observation order invalid")
        previous_time = timestamp(observation["utc"])
        authenticated_end = timestamp(qualification_run["updated_at"])
        if qualification_run["status"] == "in_progress":
            authenticated_end = max(authenticated_end, dt.datetime.now(dt.timezone.utc).timestamp())
        require(timestamp(qualification_run["run_started_at"]) <= previous_time <= authenticated_end,
                "rollback observation is outside authenticated qualification run")
        validate_probe(observation["pva"], legacy=True)
    require(sequence[2]["pva"]["scalar"] > sequence[1]["pva"]["scalar"] and
            sequence[2]["pva"]["array"] > sequence[1]["pva"]["array"], "rollback lacks fresh restored PVA content")


def verify_bundle(directory, record, run_info, api, active=False):
    require(type(record.get("schema")) is int and record.get("schema") == 1, "missing qualification proof schema")
    version, revision = record.get("version", ""), record.get("revision", "")
    require(version == "0.9.0" and SHA.fullmatch(revision),
            "qualification policy supports only exact final merged VERSION 0.9.0")
    validate_run(run_info, revision, "qualify-image.yml", dispatch_only=True, active=active)
    require(record.get("run_id") == run_info["id"] and record.get("run_attempt") == run_info["run_attempt"]
            and record.get("platform") == "linux/amd64", "qualification run/platform mismatch")
    immutable_image(record.get("image"))
    require(record.get("evidence") == inventory(directory, {"qualification.json"}), "qualification evidence missing, extra, or changed")
    proof = read(directory, "proof.json")
    identity = proof["identity"]
    for key in ("version", "revision", "image", "run_id", "run_attempt", "platform"):
        require(identity.get(key) == record[key], "qualification proof identity mismatch")
    require(re.fullmatch(r"qualification-[0-9]+-[0-9]+-[0-9a-f]{12}", identity.get("scope", "")), "invalid private scope")
    ownership = read(directory, "runtime-ownership.json")
    require(ownership.get("identity") == identity and ownership.get("closed") is True and ownership.get("containers") == {}
            and ownership.get("network") is None and ownership.get("builder") is None and ownership.get("images") == [],
            "qualification private resources were not cleaned up")
    require(ownership.get("planned") == dict(containers={}, network=None, builder=None, images=[]),
            "qualification has unresolved planned resources")
    comparison = api(f"repos/{REPOSITORY}/compare/{revision}...main")
    require(comparison.get("status") in {"ahead", "identical"} and comparison.get("merge_base_commit", {}).get("sha") == revision,
            "candidate source is not merged into main")
    source_version = api(f"repos/{REPOSITORY}/contents/VERSION?ref={revision}")
    import base64
    require(source_version.get("encoding") == "base64" and base64.b64decode(source_version["content"]).decode().strip() == version,
            "candidate VERSION differs from merged source")
    policy = read(directory, proof["policy"])
    validate_policy(policy)
    policy_source = api(f"repos/{REPOSITORY}/contents/docs/qualification-policy.json?ref={revision}")
    require(policy_source.get("encoding") == "base64" and
            hashlib.sha256(base64.b64decode(policy_source["content"])).hexdigest() == digest(evidence_path(directory, proof["policy"])),
            "qualification policy is not the reviewed source policy")
    instrument = read(directory, "proof.json").get("instrumentation")
    instrumentation_hash = digest(evidence_path(directory, instrument))
    instrumentation_source = api(f"repos/{REPOSITORY}/contents/scripts/qualification-timings.patch?ref={revision}")
    require(instrumentation_source.get("encoding") == "base64" and
            hashlib.sha256(base64.b64decode(instrumentation_source["content"])).hexdigest() == instrumentation_hash,
            "helper timing instrumentation differs from reviewed source")
    candidate_run = authenticated_run(read(directory, proof["candidate"]["run"]), api, revision, "candidate-image.yml", dispatch_only=True)
    candidate = read(directory, proof["candidate"]["record"])
    validate_candidate(candidate, directory, "candidate/", candidate_run, version, revision)
    require(candidate["image"] == record["image"] and record.get("candidate_run") == candidate_run["id"]
            and record.get("candidate_attempt") == candidate_run["run_attempt"], "qualification tested a different candidate run/digest")
    validate_ci(directory, proof["ci"], revision, api)
    seconds, events = validate_soak(directory, proof["soak"], identity, policy, run_info)
    frame_report, frame_run = execution(directory, proof["frames"], identity)
    validate_frames(frame_report, frame_run, policy)
    require(frame_run["instrumentation_sha256"] == instrumentation_hash, "frame report uses different timing instrumentation")
    for entry in [*proof["capacity"], *read(directory, proof["soak"]["manifest"])["workloads"]]:
        if entry.get("axis") == "imaging":
            require(read(directory, entry["execution"])["instrumentation_sha256"] == instrumentation_hash,
                    "periodic imaging uses different timing instrumentation")
    validate_capacity(directory, proof["capacity"], identity, policy, events)
    validate_rollback(directory, proof["rollback"], identity, api, run_info)
    require(record.get("checks") == sorted(CHECKS) and record.get("soak_seconds") == seconds
            and record.get("rollback_version") == "0.8.2", "qualification summary differs from derived proof")
    return record
