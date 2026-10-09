#!/usr/bin/env python3
"""Short private Redis traffic-helper checks; not qualification evidence."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import socket
import signal
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("traffic", ROOT / "scripts/qualification-traffic.py")
traffic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(traffic)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--redis-server", required=True)
    args = parser.parse_args()
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="qualification-traffic-unit-") as temporary:
        folder = Path(temporary)
        with (folder / "redis.log").open("w") as log:
            server = subprocess.Popen([args.redis_server, "--port", str(port), "--bind", "127.0.0.1", "--save", "",
                                       "--appendonly", "no", "--dir", str(folder)], stdout=log, stderr=subprocess.STDOUT)
            client = None
            try:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if server.poll() is not None:
                        raise ValueError("owned Redis process exited during startup")
                    try:
                        client = traffic.Redis("127.0.0.1", port=port)
                        if client.command("PING") == b"PONG":
                            break
                    except OSError:
                        time.sleep(0.05)
                if client is None:
                    raise ValueError("owned Redis startup failed")
                info = client.command("INFO", "server").decode()
                if f"process_id:{server.pid}\r\n" not in info or server.poll() is not None:
                    raise ValueError("loopback endpoint does not belong to the Redis PID this fixture created")
                output = folder / "producer.json"
                subprocess.run([sys.executable, str(ROOT / "scripts/qualification-traffic.py"), "producer", "--host", "127.0.0.1",
                    "--port", str(port), "--base", "unit-soak", "--output", str(output), "--duration", "1"], check=True, timeout=5)
                progress = json.loads(output.read_text())
                if progress["failed"] or progress["accepted"] < 30:
                    raise ValueError("private producer did not accept all attempted samples")
                observation = dict(array_elements=4096)
                for kind, key, count in (("scalar", "scalar", 1), ("alias", "scalar", 1), ("array", "array", 4096)):
                    entries = client.command("XREVRANGE", "{unit-soak}:" + key, "+", "-", "COUNT", 1)
                    stream_id = entries[0][0].decode()
                    fields = dict(zip(entries[0][1][::2], entries[0][1][1::2]))
                    data = struct.unpack("<" + str(count) + "I", fields[b"_"])
                    if list(data) != list(range(data[0], data[0] + count)):
                        raise ValueError("producer payload corruption")
                    observation[kind] = data[0]
                    milliseconds, remainder = map(int, stream_id.split("-"))
                    observation[kind + "_time_ns"] = milliseconds * 1000000 + remainder
                if not traffic.verify_content("127.0.0.1", "unit-soak", observation, port)["source_match"]:
                    raise ValueError("exact source cursor/content check failed")
                observation["alias"] -= 1
                try:
                    traffic.verify_content("127.0.0.1", "unit-soak", observation, port)
                except ValueError:
                    pass
                else:
                    raise ValueError("wrong alias content was accepted")
                ledger = [json.loads(line) for line in (folder / "producer-events.jsonl").read_text().splitlines()]
                if len(ledger) != 1 or ledger[0]["kind"] != "start":
                    raise ValueError("healthy private producer reported a failure/stall")
                fault_folder = folder / "fault"
                fault_folder.mkdir()
                fault_progress = fault_folder / "producer.json"
                producer = subprocess.Popen([sys.executable, str(ROOT / "scripts/qualification-traffic.py"), "producer",
                    "--host", "127.0.0.1", "--port", str(port), "--base", "unit-fault",
                    "--output", str(fault_progress), "--duration", "4"])
                try:
                    deadline = time.monotonic() + 2
                    while not fault_progress.exists() and time.monotonic() < deadline:
                        time.sleep(0.02)
                    if not fault_progress.exists():
                        raise ValueError("fault fixture producer startup failed")
                    pause_start = time.monotonic_ns()
                    os.kill(server.pid, signal.SIGSTOP)
                    try:
                        time.sleep(1.6)
                    finally:
                        os.kill(server.pid, signal.SIGCONT)
                        pause_end = time.monotonic_ns()
                    producer.wait(timeout=6)
                    if producer.returncode:
                        raise ValueError("fault fixture producer exited")
                    observed = json.loads(fault_progress.read_text())
                    failures = [json.loads(line) for line in (fault_folder / "producer-events.jsonl").read_text().splitlines()[1:]]
                    if not failures or observed["failed"] != len(failures):
                        raise ValueError("actual timed fault lacks precise failure ledger/accounting")
                    for index, failure in enumerate(failures):
                        if not (failure["kind"] == "failure" and failure["failed"] == index + 1
                                and failure["start_mono_ns"] <= pause_end and failure["end_mono_ns"] >= pause_start
                                and failure["end_mono_ns"] - failure["start_mono_ns"] <= 4 * 10**9):
                            raise ValueError("actual failed request was not bounded/attributable to the owned pause")
                    if observed["accepted"] <= progress["accepted"] or observed["value"] != observed["accepted"] + observed["failed"]:
                        raise ValueError("fault producer did not recover fresh successful writes without replay")
                finally:
                    if producer.poll() is None:
                        producer.terminate()
                        producer.wait(timeout=3)
            finally:
                if client:
                    client.close()
                server.terminate()
                try:
                    server.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    server.kill(); server.wait(timeout=3)
    print("private Redis helper: exact UInt32 payloads/cursors, healthy no-failure path, alias mismatch rejection and timed in-flight fault ledger/recovery passed")


if __name__ == "__main__":
    main()
