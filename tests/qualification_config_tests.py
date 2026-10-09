#!/usr/bin/env python3
"""Validate every qualification-generated configuration with the real parser."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))


def load_collector():
    spec = importlib.util.spec_from_file_location("qualify_image", ROOT / "scripts/qualify-image.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate(ioc, name, config):
    with tempfile.TemporaryDirectory(prefix="qualification-config-") as directory:
        config = copy.deepcopy(config)
        access = Path(directory) / "access.acf"
        access.write_text("ASG(QREAD) { RULE(0, READ) }\nASG(QADMIN) { RULE(0, WRITE) }\n")
        config["access"]["file"] = str(access)
        path = Path(directory) / (name + ".json")
        path.write_text(json.dumps(config))
        result = subprocess.run([ioc, "--check-config", str(path)], text=True, capture_output=True, timeout=10)
        if result.returncode:
            raise AssertionError(name + " configuration rejected:\n" + result.stdout + result.stderr)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ioc", required=True)
    args = parser.parse_args()
    collector = load_collector()

    initial = collector.fixture_config()
    candidate = copy.deepcopy(initial)
    candidate["redis_backends"]["soak"]["reader_probe_ms"] = 1000
    for pv in candidate["pvs"]:
        pv["source_health"] = dict(required=True, stale_after_ms=2000)

    imaging, _ = collector.routed_config(candidate, "imaging", 0)
    capacity, _ = collector.routed_config(candidate, "scalar", 1)
    array_capacity, _ = collector.routed_config(candidate, "array", 2, elements=4096)
    configurations = [
        ("initial", initial),
        ("candidate", candidate),
        ("imaging", imaging),
        ("imaging-removed", collector.unrouted_config(imaging, "imaging")),
        ("capacity-scalar", capacity),
        ("capacity-array", array_capacity),
        ("capacity-removed", collector.unrouted_config(capacity, "scalar")),
    ]
    for name, config in configurations:
        validate(args.ioc, name, config)


if __name__ == "__main__":
    main()
