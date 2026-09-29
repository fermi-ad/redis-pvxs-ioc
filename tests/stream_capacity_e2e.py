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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ioc", required=True)
    parser.add_argument("--client", required=True)
    args = parser.parse_args()
    redis_port = int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"])
    for case, elements, clients in (("scalar", 1, 1), ("array", 32, 1), ("fanout", 32, 4)):
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
                    assert ioc.poll() is None
                    assert evidence["final_values_converged"]
                    assert evidence["received"] + evidence["missed_updates"] == 64 * clients
                    assert evidence["duplicate_updates"] == 0
                    assert evidence["latency_p99_ms"] >= evidence["latency_p50_ms"] >= 0
                    # A repeated workload cannot silently overwrite its report.
                    assert subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                          timeout=5).returncode != 0
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
