#!/usr/bin/env python3
"""Run deterministic transfer acceptance against an external IOC and private Redis."""
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
    parser.add_argument("--frames", type=int, default=16)
    parser.add_argument("--width", type=int, default=32)
    parser.add_argument("--height", type=int, default=16)
    parser.add_argument("--fps", type=int, default=40)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.report and args.report.exists():
        parser.error("report already exists; choose a new evidence path")
    if min(args.frames, args.width, args.height, args.fps) < 1:
        parser.error("frame count, dimensions and rate must be positive")
    redis_port = int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"])
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as listener:
        listener.bind(("127.0.0.1", 0))
        pva_port = listener.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="ndarray-transfer-") as directory:
        folder = Path(directory)
        config = dict(server=dict(instance="imaging-transfer", namespace="IMAGE", interfaces=["127.0.0.1"],
                                  tcp_port=0, udp_port=pva_port, auto_beacon=False),
                      discovery=dict(enabled=False),
                      redis=dict(host="127.0.0.1", port=redis_port, base_key="imaging-transfer"),
                      pvs=[dict(name="frame", kind="ntndarray", aliases=["IMAGE:alias"],
                                read=dict(key="image"), max_frame_bytes=args.width * args.height)])
        path, report = folder / "config.json", folder / "transfer.json"
        path.write_text(json.dumps(config))
        with (folder / "ioc.log").open("w") as log:
            ioc = subprocess.Popen([args.ioc, "--config", str(path)], stdout=log, stderr=subprocess.STDOUT)
            try:
                command = [args.client, "--redis-port", str(redis_port), "--base-key", "imaging-transfer",
                           "--pv", "IMAGE:alias", "--pva-address", f"127.0.0.1:{pva_port}",
                           "--frames", str(args.frames), "--width", str(args.width), "--height", str(args.height),
                           "--fps", str(args.fps), "--output", str(report)]
                subprocess.run(command, check=True, timeout=args.frames / args.fps + 15)
                evidence = json.loads(report.read_text())
                assert ioc.poll() is None
                evidence["ioc_peak_rss_bytes"] = None
                status = Path(f"/proc/{ioc.pid}/status")
                if status.exists():
                    for line in status.read_text().splitlines():
                        if line.startswith("VmHWM:"):
                            evidence["ioc_peak_rss_bytes"] = int(line.split()[1]) * 1024
                if args.report:
                    args.report.write_text(json.dumps(evidence, indent=2) + "\n")
                print(json.dumps(evidence))
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
