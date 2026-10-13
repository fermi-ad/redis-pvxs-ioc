#!/usr/bin/env python3
"""Exercise the capacity client with scalar/array and independent PVA client fan-out."""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time


def check_fixture_report(evidence, samples, clients):
    expected = samples * clients
    if evidence.get("samples") != samples or evidence.get("clients") != clients:
        raise ValueError("capacity fixture report identifies a different workload")
    if evidence.get("received") != expected or evidence.get("missed_updates") != 0:
        raise ValueError(f"capacity fixture requires all {expected} updates without loss: {evidence}")
    if evidence.get("duplicate_updates") != 0 or evidence.get("final_values_converged") is not True:
        raise ValueError(f"capacity fixture must converge without duplicate updates: {evidence}")
    if not evidence["latency_p99_ms"] >= evidence["latency_p50_ms"] >= 0:
        raise ValueError("capacity fixture latency quantiles are invalid")


def wait_for_timed_samples(redis_port, base, process):
    """Wait for the initial value plus several timed samples before reloading."""
    key = "{" + base + "}:data"
    command = f"*2\r\n$4\r\nXLEN\r\n${len(key)}\r\n{key}\r\n".encode()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("capacity client exited before reload injection")
        with socket.create_connection(("127.0.0.1", redis_port), timeout=1) as redis:
            redis.sendall(command)
            with redis.makefile("rb") as response:
                line = response.readline(64)
        if not line.startswith(b":") or not line.endswith(b"\r\n"):
            raise RuntimeError("invalid Redis stream-depth response")
        if int(line[1:-2]) >= 8:
            return
        time.sleep(0.01)
    raise RuntimeError("timed samples did not start before reload deadline")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ioc", required=True)
    parser.add_argument("--client", required=True)
    args = parser.parse_args()
    redis_port = int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"])
    for case, elements, clients in (("scalar", 1, 1), ("array", 32, 1), ("fanout", 32, 16),
                                    ("reload", 1, 1)):
        samples, rate = (128, 50) if case == "reload" else (64, 100)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        with tempfile.TemporaryDirectory(prefix="stream-capacity-") as temporary:
            folder = Path(temporary)
            config = dict(server=dict(instance="capacity", namespace="CAP", interfaces=["127.0.0.1"],
                                      tcp_port=0, udp_port=port, auto_beacon=False),
                          discovery=dict(enabled=False),
                          redis=dict(host="127.0.0.1", port=redis_port, base_key=f"capacity-{case}"),
                          pvs=[dict(name="value", type="uint32", shape="scalar" if elements == 1 else "array",
                                    read=dict(key="data"))])
            path, report = folder / "config.json", folder / "report.json"
            path.write_text(json.dumps(config))
            with (folder / "ioc.log").open("w") as log:
                ioc = subprocess.Popen([args.ioc, "--config", str(path)], stdout=log, stderr=subprocess.STDOUT)
                workload = None
                try:
                    command = [args.client, "--redis-port", str(redis_port), "--base-key", f"capacity-{case}",
                               "--pv", "CAP:value", "--pva-address", f"127.0.0.1:{port}",
                               "--samples", str(samples), "--rate", str(rate), "--elements", str(elements),
                               "--clients", str(clients), "--output", str(report)]
                    if case == "reload":
                        workload = subprocess.Popen(command)
                        wait_for_timed_samples(redis_port, f"capacity-{case}", workload)
                        config["pvs"][0]["metadata"] = dict(description="reload during timed samples")
                        path.write_text(json.dumps(config))
                        ioc.send_signal(signal.SIGHUP)
                        if workload.wait(timeout=10) != 0:
                            raise RuntimeError("capacity client failed during reload workload")
                    else:
                        subprocess.run(command, check=True, timeout=10)
                    evidence = json.loads(report.read_text())
                    if ioc.poll() is not None:
                        raise RuntimeError("IOC exited during the capacity fixture")
                    check_fixture_report(evidence, samples, clients)
                    if case == "reload" and evidence.get("metadata_only_updates", 0) < 1:
                        raise RuntimeError("reload workload did not observe a metadata-only publication")
                    # A repeated workload cannot silently overwrite its report.
                    if subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      timeout=5).returncode == 0:
                        raise RuntimeError("capacity client overwrote an existing report")
                    print(case, json.dumps(evidence))
                except Exception:
                    print((folder / "ioc.log").read_text(), file=sys.stderr)
                    raise
                finally:
                    if workload is not None and workload.poll() is None:
                        workload.kill()
                        workload.wait(timeout=5)
                    ioc.send_signal(signal.SIGTERM)
                    try:
                        ioc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        ioc.kill()
                        ioc.wait(timeout=5)


if __name__ == "__main__":
    main()
