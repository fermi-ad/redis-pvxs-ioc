"""Read-only GitHub evidence acquisition with bounded private extraction."""
import json
import os
from pathlib import Path
import selectors
import subprocess
import tempfile
import time

from qualification_contract import (MAX_BUNDLE_BYTES, REPOSITORY, extract_archive,
                                    integer, parse_json, require, validate_run)


def download(endpoint, output, limit=MAX_BUNDLE_BYTES, binary=False):
    require(not output.exists(), "download output already exists")
    output.parent.mkdir(parents=True, exist_ok=True)
    argv = ["gh", "api", endpoint]
    if binary:
        argv += ["--header", "Accept: application/octet-stream"]
    process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    deadline, count = time.monotonic() + 60, 0
    try:
        with selectors.DefaultSelector() as selector, output.open("xb") as stream:
            selector.register(process.stdout, selectors.EVENT_READ)
            while True:
                require(time.monotonic() < deadline, "evidence download timed out")
                if not selector.select(min(1, max(0, deadline - time.monotonic()))):
                    continue
                chunk = os.read(process.stdout.fileno(), 65536)
                if not chunk:
                    break
                count += len(chunk)
                require(count <= limit, "evidence download exceeds bound")
                stream.write(chunk)
        require(process.wait(timeout=max(0.1, deadline - time.monotonic())) == 0, "GitHub evidence download failed")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        process.stdout.close()


def api(endpoint):
    with tempfile.TemporaryDirectory(prefix="qualification-api-") as temporary:
        path = Path(temporary) / "response.json"
        download(endpoint, path, 8 * 1024**2)
        return parse_json(path.read_bytes())


def artifact(run_info, kind, destination):
    run_id, attempt = run_info["id"], run_info["run_attempt"]
    response = api(f"repos/{REPOSITORY}/actions/runs/{run_id}/artifacts?per_page=100")
    require(response.get("total_count") == len(response.get("artifacts", [])), "artifact list incomplete")
    selected = [a for a in response["artifacts"] if a.get("name") == f"release-{kind}-{run_id}-{attempt}"]
    require(len(selected) == 1, "missing or ambiguous named evidence artifact")
    value = selected[0]
    require(value.get("expired") is False and value.get("workflow_run", {}).get("id") == run_id
            and value["workflow_run"].get("head_sha") == run_info["head_sha"], "artifact source mismatch or expired")
    integer(value.get("size_in_bytes"), "artifact size", 1, MAX_BUNDLE_BYTES)
    artifact_id = integer(value.get("id"), "artifact ID", 1)
    with tempfile.TemporaryDirectory(prefix="qualification-artifact-") as temporary:
        archive = Path(temporary) / "artifact.zip"
        download(f"repos/{REPOSITORY}/actions/artifacts/{artifact_id}/zip", archive)
        extract_archive(archive, destination)
    return value


def retained_run(run_id, revision, workflow, destination, dispatch_only=False):
    run_info = api(f"repos/{REPOSITORY}/actions/runs/{integer(run_id, 'run ID', 1)}")
    validate_run(run_info, revision, workflow, dispatch_only=dispatch_only)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(run_info, indent=2) + "\n")
    return run_info


def retained_ci(kind, run_id, revision, directory):
    run_path, job_path = directory / (kind + "-run.json"), directory / (kind + "-jobs.json")
    workflow = "native.yml" if kind == "native" else "ci-image.yml"
    run_info = retained_run(run_id, revision, workflow, run_path)
    jobs = api(f"repos/{REPOSITORY}/actions/runs/{run_id}/attempts/{run_info['run_attempt']}/jobs?per_page=100")
    require(jobs.get("total_count") == len(jobs.get("jobs", [])), "CI job list incomplete")
    job_path.write_text(json.dumps(jobs, indent=2) + "\n")
    logs = {}
    for job in jobs["jobs"]:
        if kind == "native" and job.get("name") in {
                "native (macos-14, none)", "native (ubuntu-24.04, none)", "native (ubuntu-24.04, address,undefined)"}:
            path = directory / ("job-" + str(integer(job["id"], "job ID", 1)) + ".log")
            download(f"repos/{REPOSITORY}/actions/jobs/{job['id']}/logs", path, 32 * 1024**2)
            logs[job["name"]] = "ci/" + path.name
    return dict(run="ci/" + run_path.name, jobs="ci/" + job_path.name, logs=logs)
