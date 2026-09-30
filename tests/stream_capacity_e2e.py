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


def check_fixture_report(evidence, samples, clients):
    expected = samples * clients
    if evidence.get("samples") != samples or evidence.get("clients") != clients:
        raise ValueError("capacity fixture report identifies a different workload")
    if evidence.get("received") != expected or evidence.get("missed_updates") != 0:
        raise ValueError(f"capacity fixture requires all {expected} updates without loss: {evidence}")
    if evidence.get("duplicate_updates") != 0 or evidence.get("final_values_converged") is not True:
        raise ValueError("capacity fixture must converge without duplicate updates")
    if not evidence["latency_p99_ms"] >= evidence["latency_p50_ms"] >= 0:
        raise ValueError("capacity fixture latency quantiles are invalid")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ioc", required=True)
    parser.add_argument("--client", required=True)
    args = parser.parse_args()
    redis_port = int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"])
    for case, elements, clients in (("scalar", 1, 1), ("array", 32, 1), ("fanout", 32, 16)):
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
                try:
                    command = [args.client, "--redis-port", str(redis_port), "--base-key", f"capacity-{case}",
                               "--pv", "CAP:value", "--pva-address", f"127.0.0.1:{port}",
                               "--samples", "64", "--rate", "100", "--elements", str(elements),
                               "--clients", str(clients), "--output", str(report)]
                    subprocess.run(command, check=True, timeout=10)
                    evidence = json.loads(report.read_text())
                    if ioc.poll() is not None:
                        raise RuntimeError("IOC exited during the capacity fixture")
                    check_fixture_report(evidence, 64, clients)
                    # A repeated workload cannot silently overwrite its report.
                    if subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      timeout=5).returncode == 0:
                        raise RuntimeError("capacity client overwrote an existing report")
                    print(case, json.dumps(evidence))
                except Exception:
                    print((folder / "ioc.log").read_text(), file=sys.stderr)
                    raise
                finally:
                    ioc.send_signal(signal.SIGTERM)
                    try:
                        ioc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        ioc.kill()
                        ioc.wait(timeout=5)


if __name__ == "__main__":
    main()
