#!/usr/bin/env python3
"""Validate immutable images and promote evidence from trusted candidate runs."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

REPOSITORY = "fermi-ad/redis-pvxs-ioc"
IMAGE_REPOSITORY = "adregistry.fnal.gov/instrumentation/redis-pvxs-ioc"
SOURCE = "https://github.com/" + REPOSITORY
ROOT = Path(__file__).resolve().parents[1]
SEMVER = re.compile(
    r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
)
CANDIDATE_CHECKS = {"image", "smoke", "access", "sbom", "provenance", "source-inputs"}
CANDIDATE_EVIDENCE = {"source-inputs.json", "sbom.json", "provenance.json", "registry-manifest.json"}
QUALIFICATION_CHECKS = {
    "native-macos", "full-feature", "minimal", "sanitizers", "redis-pva",
    "discovery-catalog", "rpc-http", "imaging-600-frames", "capacity", "rollback",
}


def version_parts(version):
    match = SEMVER.fullmatch(version)
    if not match:
        raise ValueError("VERSION must be a complete semantic version")
    prerelease = match[4]
    if prerelease and any(p.isdigit() and len(p) > 1 and p[0] == "0"
                          for p in prerelease.split(".")):
        raise ValueError("numeric prerelease identifiers cannot have leading zeros")
    return tuple(int(match[i]) for i in (1, 2, 3)), prerelease


def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def immutable_image(image):
    if not re.fullmatch(re.escape(IMAGE_REPOSITORY) + r"@sha256:[0-9a-f]{64}", image):
        raise ValueError("expected an immutable digest in the project image repository")
    return image


def validate_image(image, version, revision):
    version_parts(version)
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("expected the complete source commit")
    info = json.loads(run("docker", "image", "inspect", image))[0]
    if (info["Os"], info["Architecture"]) != ("linux", "amd64"):
        raise ValueError("release image must target linux/amd64")
    labels = info["Config"].get("Labels") or {}
    for key, expected in {"version": version, "revision": revision,
                          "source": SOURCE, "licenses": "BSD-3-Clause"}.items():
        if labels.get("org.opencontainers.image." + key) != expected:
            raise ValueError("image label mismatch: " + key)
    if run("docker", "run", "--rm", image, "--version") != f"redis-pvxs-ioc {version} ({revision})":
        raise ValueError("binary identity differs from the candidate source")
    run("docker", "run", "--rm", image, "--check-config", "/etc/redis-pvxs-ioc/config.yaml")
    run("docker", "run", "--rm", "--entrypoint", "/bin/sh", image, "-c",
        "test -r /usr/share/doc/redis-pvxs-ioc/LICENSE && "
        "test -r /usr/share/doc/redis-pvxs-ioc/NOTICE && "
        "test -r /usr/share/doc/redis-pvxs-ioc/THIRD_PARTY_NOTICES.md")


def registry_digest(image):
    result = subprocess.run(
        ["docker", "buildx", "imagetools", "inspect", image, "--format", "{{json .Manifest}}"],
        text=True, capture_output=True)
    if result.returncode:
        # A network/authentication error must not be treated as an absent tag.
        if re.search(r"manifest unknown|manifest_unknown|: not found", result.stderr, re.I):
            return None
        raise RuntimeError("cannot inspect registry manifest: " + result.stderr)
    digest = json.loads(result.stdout)["digest"]
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
        raise ValueError("registry returned an invalid digest")
    return digest


def source_inputs(output):
    # Reject changed/missing gitlinks and dirty source, including nested trees.
    # This supplements the scanner, which cannot identify every static C++ library.
    if run("git", "status", "--porcelain", "--untracked-files=all"):
        raise ValueError("candidate source must be a clean checkout")
    lines = subprocess.check_output(["git", "submodule", "status", "--recursive"], text=True).splitlines()
    if not lines or any(not line.startswith(" ") for line in lines):
        raise ValueError("candidate submodules must be initialized at their pinned revisions")
    modules = [dict(revision=line[1:].split()[0], path=line[1:].split()[1]) for line in lines]
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(dict(repository=SOURCE, revision=run("git", "rev-parse", "HEAD"),
        submodules=modules, dockerfile_sha256=hashlib.sha256((ROOT / "Dockerfile").read_bytes()).hexdigest()), indent=2) + "\n")


def capture_attestations(image, revision, directory):
    immutable_image(image)
    values = {}
    for name, field in (("sbom.json", "SBOM"), ("provenance.json", "Provenance"),
                        ("registry-manifest.json", "Manifest")):
        value = json.loads(run("docker", "buildx", "imagetools", "inspect", image,
                               "--format", "{{json ." + field + "}}"))
        if not isinstance(value, dict):
            raise ValueError("missing image attestation: " + name)
        values[name] = value
        (directory / name).write_text(json.dumps(value, indent=2) + "\n")
    if values["registry-manifest.json"].get("digest") != image.split("@", 1)[1]:
        raise ValueError("attestation index differs from the candidate digest")
    sbom = values["sbom.json"].get("linux/amd64", values["sbom.json"]).get("SPDX", {})
    provenance = values["provenance.json"].get("linux/amd64", values["provenance.json"]).get("SLSA", {})
    if not sbom.get("spdxVersion") or not sbom.get("packages"):
        raise ValueError("candidate is missing a populated SPDX SBOM")
    if not provenance.get("buildConfig") or not provenance.get("materials"):
        raise ValueError("candidate is missing full build provenance")
    arguments = provenance.get("invocation", {}).get("parameters", {}).get("args", {})
    if arguments.get("build-arg:REDIS_PVXS_IOC_REVISION") != revision:
        raise ValueError("provenance source revision differs from the candidate")
    source = json.loads((directory / "source-inputs.json").read_text())
    if source.get("revision") != revision or source.get("repository") != SOURCE or not source.get("submodules"):
        raise ValueError("candidate source inventory is missing or mismatched")
    return {name: hashlib.sha256((directory / name).read_bytes()).hexdigest()
            for name in sorted(CANDIDATE_EVIDENCE)}


def verify_record(record, run_info, version, revision, kind):
    workflow = "candidate-image.yml" if kind == "candidate" else "qualify-image.yml"
    if (run_info["conclusion"] != "success" or run_info["head_sha"] != revision
            or run_info["event"] != "workflow_dispatch"
            or run_info["path"] != ".github/workflows/" + workflow):
        raise ValueError("evidence must come from a successful matching trusted workflow run")
    if (record.get("version") != version or record.get("revision") != revision
            or record.get("run_id") != run_info["id"]
            or record.get("run_attempt") != run_info["run_attempt"]):
        raise ValueError("candidate identity/run attempt does not match the release")
    immutable_image(record["image"])
    if record.get("platform") != "linux/amd64":
        raise ValueError("release evidence must qualify linux/amd64")
    expected = CANDIDATE_CHECKS if kind == "candidate" else QUALIFICATION_CHECKS
    if not expected.issubset(record.get("checks", [])):
        raise ValueError("required validation evidence is missing")
    if kind == "candidate":
        evidence = record.get("evidence", {})
        if not CANDIDATE_EVIDENCE.issubset(evidence) or any(
                not re.fullmatch(r"[0-9a-f]{64}", evidence[name]) for name in CANDIDATE_EVIDENCE):
            raise ValueError("candidate attestation/source evidence is missing")
    if kind == "qualification":
        if record.get("soak_seconds", 0) < 86400 or record.get("rollback_version") != "0.8.2":
            raise ValueError("qualification requires 24-hour amd64 soak and 0.8.2 rollback")
    return record


def load_record(run_id, version, revision, kind, directory):
    if not re.fullmatch(r"[0-9]+", str(run_id)):
        raise ValueError("workflow run ID must be numeric")
    info = json.loads(run("gh", "api", f"repos/{REPOSITORY}/actions/runs/{run_id}"))
    # Validate the workflow before downloading its artifacts.
    name = "candidate-image.yml" if kind == "candidate" else "qualify-image.yml"
    if (info["conclusion"] != "success" or info["head_sha"] != revision
            or info["event"] != "workflow_dispatch" or info["path"] != ".github/workflows/" + name):
        raise ValueError("invalid evidence workflow or source revision")
    destination = directory / kind
    run("gh", "run", "download", str(run_id), "--repo", REPOSITORY, "--name",
        f"release-{kind}-{run_id}-{info['run_attempt']}", "--dir", str(destination))
    record = json.loads((destination / (kind + ".json")).read_text())
    verify_record(record, info, version, revision, kind)
    if kind == "candidate":
        for name in CANDIDATE_EVIDENCE:
            if hashlib.sha256((destination / name).read_bytes()).hexdigest() != record["evidence"][name]:
                raise ValueError("candidate evidence checksum differs: " + name)
    return record


def promote_digest(image, version):
    immutable_image(image)
    core, prerelease = version_parts(version)
    # '+' is legal SemVer metadata but not legal in a Docker tag. '_' cannot
    # occur in SemVer, so this mapping is unambiguous.
    tag = IMAGE_REPOSITORY + ":v" + version.replace("+", "_")
    digest = image.split("@", 1)[1]
    existing = registry_digest(tag)
    if existing is not None and existing != digest:
        raise ValueError("refusing to overwrite an existing release digest")
    latest = IMAGE_REPOSITORY + ":latest"
    update_latest = not prerelease
    if update_latest:
        current = registry_digest(latest)
        if current:
            current_image = IMAGE_REPOSITORY + "@" + current
            run("docker", "pull", current_image)
            info = json.loads(run("docker", "image", "inspect", current_image))[0]
            latest_version = info["Config"]["Labels"]["org.opencontainers.image.version"]
            latest_core, _ = version_parts(latest_version)
            update_latest = core >= latest_core
    for target in ([tag, latest] if update_latest else [tag]):
        run("docker", "buildx", "imagetools", "create", "--prefer-index=false", "--tag", target, image)
        if registry_digest(target) != digest:
            raise RuntimeError("promoted digest differs from validated digest: " + target)
    return tag, update_latest


def changelog_entry(version):
    text = (ROOT / "CHANGELOG.md").read_text()
    match = re.search(r"(?m)^## v" + re.escape(version) + r"(?:[ \t].*)?$", text)
    if not match:
        raise ValueError("release has no matching changelog entry")
    end = re.search(r"(?m)^## ", text[match.end():])
    return text[match.end():match.end() + end.start() if end else len(text)].strip()


def existing_release(tag):
    result = subprocess.run(
        ["gh", "release", "view", tag, "--repo", REPOSITORY, "--json", "isDraft,assets"],
        text=True, capture_output=True)
    if result.returncode:
        if "release not found" in result.stderr.lower():
            return None
        raise RuntimeError("cannot inspect existing release: " + result.stderr)
    info = json.loads(result.stdout)
    if not isinstance(info.get("isDraft"), bool):
        raise ValueError("existing release has no valid draft state")
    return info


def verify_release_assets(tag, assets):
    # Verify the uploaded bytes, rather than relying on upload success or asset
    # names. This also validates assets repaired during a retried publication.
    with tempfile.TemporaryDirectory(prefix="redis-pvxs-release-assets-") as temp:
        destination = Path(temp)
        for asset in assets:
            run("gh", "release", "download", tag, "--repo", REPOSITORY,
                "--pattern", asset.name, "--dir", str(destination))
            downloaded = destination / asset.name
            if hashlib.sha256(downloaded.read_bytes()).digest() != hashlib.sha256(asset.read_bytes()).digest():
                raise ValueError("uploaded release evidence differs: " + asset.name)


def publish_release(tag, notes, assets, prerelease, updated_latest):
    info = existing_release(tag)
    metadata = ["--repo", REPOSITORY, "--title", tag, "--notes-file", str(notes)]
    if info is None:
        subprocess.run(["gh", "release", "create", tag, *metadata,
                        "--verify-tag", "--draft"], check=True)
    elif info["isDraft"]:
        subprocess.run(["gh", "release", "edit", tag, *metadata,
                        "--draft=true", "--latest=false"], check=True)
    # Published assets are immutable: verify existing bytes and add only missing
    # files. --clobber would briefly remove evidence from a visible release.
    upload = assets
    clobber = ["--clobber"]
    if info is not None and not info["isDraft"]:
        names = {asset["name"] for asset in info.get("assets", [])}
        existing = [asset for asset in assets if asset.name in names]
        if existing:
            verify_release_assets(tag, existing)
        upload = [asset for asset in assets if asset.name not in names]
        clobber = []
    if upload:
        subprocess.run(["gh", "release", "upload", tag, *(str(asset) for asset in upload),
                        "--repo", REPOSITORY, *clobber], check=True)
    verify_release_assets(tag, assets)
    subprocess.run(["gh", "release", "edit", tag, *metadata, "--draft=false",
                    "--prerelease=true" if prerelease else "--prerelease=false",
                    "--latest=true" if updated_latest else "--latest=false"], check=True)


def evidence_archive(directory, qualification_run):
    # A retry on the same evidence must reproduce the same bytes. Artifact
    # extraction times and local usernames must not alter the published archive.
    archive = directory / "release-evidence.tar.gz"
    def canonical(info):
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        info.mtime = 0
        return info
    with archive.open("wb") as output, gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w") as bundle:
            bundle.add(directory / "candidate", arcname="candidate", filter=canonical)
            if qualification_run:
                bundle.add(directory / "qualification", arcname="qualification", filter=canonical)
    return archive


def release(tag, candidate_run, qualification_run):
    version = (ROOT / "VERSION").read_text().strip()
    core, prerelease = version_parts(version)
    if tag != "v" + version:
        raise ValueError("release tag does not match VERSION")
    revision = run("git", "rev-parse", "--verify", "refs/tags/" + tag + "^{commit}")
    if run("git", "rev-parse", "HEAD") != revision:
        raise ValueError("checkout is not the release tag commit")
    run("git", "merge-base", "--is-ancestor", revision, "origin/main")
    changes = changelog_entry(version)
    with tempfile.TemporaryDirectory(prefix="redis-pvxs-release-") as temp:
        directory = Path(temp)
        record = load_record(candidate_run, version, revision, "candidate", directory)
        if core >= (0, 9, 0) and not prerelease and not qualification_run:
            raise ValueError("stable 0.9.0 and later require a successful qualification run")
        if qualification_run:
            qualification = load_record(qualification_run, version, revision, "qualification", directory)
            if qualification["image"] != record["image"]:
                raise ValueError("qualification tested a different image")
        image = record["image"]
        run("docker", "pull", image)
        validate_image(image, version, revision)
        environment = dict(os.environ, REDIS_PVXS_IOC_IMAGE=image, REDIS_PVXS_IOC_PULL_POLICY="auto")
        subprocess.run([str(ROOT / "scripts/smoke-test.sh")], check=True, env=environment)
        image_tag, updated_latest = promote_digest(image, version)
        notes = directory / "release-notes.md"
        notes.write_text(
            f"{changes}\n\n## Artifact and validation\n\n"
            f"- Image: `{image_tag}@{image.split('@')[1]}`\n"
            f"- Source revision: `{revision}`\n"
            f"- Candidate evidence: {SOURCE}/actions/runs/{candidate_run}\n"
            + (f"- Qualification evidence: {SOURCE}/actions/runs/{qualification_run}\n" if qualification_run else "")
            + "- Promoted the validated Linux amd64 digest without rebuilding.\n"
            + "- SBOM, full build provenance, source inventory and validation logs are retained in release-evidence.tar.gz.\n"
            + "- Rechecked labels, binary identity, default configuration, notices, and isolated Redis/PVA smoke behavior.\n"
            + ("- `latest` now points to the same digest.\n" if updated_latest else "- `latest` was preserved.\n"))
        assets = [directory / "candidate/candidate.json"]
        if qualification_run:
            assets.append(directory / "qualification/qualification.json")
        # Workflow artifacts expire. Keep the complete downloaded evidence with
        # the release as well as the attestations attached to the image index.
        assets.append(evidence_archive(directory, qualification_run))
        publish_release(tag, notes, assets, prerelease, updated_latest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate")
    validate.add_argument("image")
    validate.add_argument("version")
    validate.add_argument("revision")
    record = commands.add_parser("record")
    record.add_argument("image")
    record.add_argument("output", type=Path)
    inputs = commands.add_parser("source-inputs")
    inputs.add_argument("output", type=Path)
    publish = commands.add_parser("promote")
    publish.add_argument("--tag", required=True)
    publish.add_argument("--candidate-run", required=True)
    publish.add_argument("--qualification-run", default="")
    args = parser.parse_args()
    if args.command == "source-inputs":
        source_inputs(args.output)
    elif args.command == "validate":
        validate_image(args.image, args.version, args.revision)
    elif args.command == "record":
        digest = registry_digest(args.image)
        image = immutable_image(IMAGE_REPOSITORY + "@" + (digest or ""))
        revision = run("git", "rev-parse", "HEAD")
        version = (ROOT / "VERSION").read_text().strip()
        # Recheck the pushed bytes before writing a successful record.
        run("docker", "pull", image)
        validate_image(image, version, revision)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        evidence = capture_attestations(image, revision, args.output.parent)
        args.output.write_text(json.dumps(dict(image=image, version=version, revision=revision,
            platform="linux/amd64", checks=sorted(CANDIDATE_CHECKS),
            evidence=evidence,
            run_id=int(os.environ["GITHUB_RUN_ID"]), run_attempt=int(os.environ["GITHUB_RUN_ATTEMPT"])), indent=2) + "\n")
    else:
        release(args.tag, args.candidate_run, args.qualification_run)


if __name__ == "__main__":
    main()
