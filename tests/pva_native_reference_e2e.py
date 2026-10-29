#!/usr/bin/env python3
"""Qualify patched PVXS against an independently built stock pvAccessCPP peer."""
import argparse
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import time


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def free_port():
    with socket.socket() as tcp:
        tcp.bind(("127.0.0.1", 0))
        port = tcp.getsockname()[1]
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", port))
        return port


class Peer:
    def __init__(self, command):
        self.command = list(map(str, command))
        self.lines = []
        self.pending = ""
        self.stderr = ""
        self.process = subprocess.Popen(
            self.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, env=dict(os.environ, LC_ALL="C", LANG="C"),
        )

    def read_json(self, key=None):
        deadline = time.monotonic() + 8
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            while time.monotonic() < deadline:
                while "\n" in self.pending:
                    line, self.pending = self.pending.split("\n", 1)
                    self.lines.append(line)
                    try:
                        result = json.loads(line)
                    except ValueError:
                        continue
                    if key is None or key in result:
                        return result
                check(selector.select(max(0, deadline - time.monotonic())), "peer startup/read timed out")
                chunk = os.read(self.process.stdout.fileno(), 4096)
                check(chunk, f"peer closed stdout: {self.command}")
                self.pending += chunk.decode("utf-8", errors="replace")
        raise RuntimeError(f"peer did not return {key}: {self.command}")

    def stats(self):
        self.process.stdin.write("stats\n")
        self.process.stdin.flush()
        return self.read_json()

    def stop(self):
        try:
            output, self.stderr = self.process.communicate("quit\n", timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            output, self.stderr = self.process.communicate(timeout=5)
        self.lines += output.splitlines()
        check(self.process.returncode == 0, f"peer did not exit cleanly ({self.process.returncode}): {self.stderr}")
        check("assert(" not in self.stderr and "Assertion" not in self.stderr,
              f"peer lifecycle assertion: {self.stderr}")


def run_client(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=60)
    check(result.returncode == 0, f"client failed ({result.returncode}): {result.stdout}\n{result.stderr}")
    return {"command": list(map(str, command)), "stdout": result.stdout, "stderr": result.stderr}


def qualify(native, fixture):
    result = {}
    peer = Peer([fixture, "--serve", "--role", "BASE", "--udp-port", str(free_port())])
    try:
        announce = peer.read_json("tcp")
        result["native_client_to_patched_server"] = run_client([
            native, "--client", f"127.0.0.1:{announce['tcp']}", "CONTROL", "UNSUPPORTED", "--patched",
        ])
        stats = peer.stats()
        control = stats["pvs"]["CONTROL"]
        check(control["processes"] == 1 and control["putgets"] == 65,
              f"PROCESS/PUT_GET changed semantics: {control}")
        check(control["arrayWrites"] == 2 and control["arrayReads"] == 5,
              f"ARRAY operation counts changed: {control}")
        check(stats["pvs"]["UNSUPPORTED"]["mutations"] == 0,
              "unsupported operations changed provider state")
        result["native_client_to_patched_server"]["provider_stats"] = stats
    finally:
        peer.stop()
    peer = Peer([native, "--server", str(free_port())])
    try:
        announce = peer.read_json("tcp")
        result["patched_client_to_native_server"] = run_client([
            fixture, "--client", "native-reference", "--endpoint", f"127.0.0.1:{announce['tcp']}", "--name", "CONTROL",
        ])
        stats = peer.stats()
        check(stats["argument"] == 9 and stats["result"] == 90,
              f"native PUT_GET state was not atomic: {stats}")
        result["patched_client_to_native_server"]["provider_stats"] = stats
    finally:
        peer.stop()
    result["clean_shutdown"] = True
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    result = qualify(args.native.resolve(), args.fixture.resolve())
    if args.report:
        args.report.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"independent_native_interop": True, "directions": 2, "clean_shutdown": True}))


if __name__ == "__main__":
    main()
