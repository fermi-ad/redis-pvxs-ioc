#!/usr/bin/env python3
"""Unrelated backend/alarm edits must preserve cached values and pending puts."""
import argparse
import copy
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser()
    for name in ("ioc", "pvxget", "pvxput"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    redis_port = int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"])

    def command(*fields):
        fields = [item if isinstance(item, bytes) else str(item).encode() for item in fields]
        packet = b"*" + str(len(fields)).encode() + b"\r\n"
        packet += b"".join(b"$" + str(len(item)).encode() + b"\r\n" + item + b"\r\n" for item in fields)
        with socket.create_connection(("127.0.0.1", redis_port), timeout=2) as client:
            client.sendall(packet)
            with client.makefile("rb") as reply:
                header = reply.readline()
                assert header[:1] != b"-", header
                if header.startswith(b"$"):
                    return reply.read(int(header[1:]) + 2)[:-2]
                if header.startswith(b":"):
                    return int(header[1:])
                raise AssertionError(header)

    def publish(base, key, value):
        command("XADD", "{" + base + "}:" + key, "*", "_", struct.pack("=d", value))

    with tempfile.TemporaryDirectory(prefix="redis-pvxs-reload-") as directory:
        directory = Path(directory)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        environment = dict(os.environ, EPICS_PVA_AUTO_ADDR_LIST="NO", EPICS_PVA_ADDR_LIST=f"127.0.0.1:{port}")
        config = dict(
            server=dict(instance="reload", namespace="TEST", interfaces=["127.0.0.1"],
                        tcp_port=port, udp_port=port, auto_beacon=False),
            redis_backends={name: dict(host="127.0.0.1", port=redis_port, base_key="reload-" + name)
                            for name in ("stable", "other")},
            alarms=dict(backend="stable", stream="reload-alarms"), discovery=dict(enabled=False),
            pvs=[dict(name="keep", aliases=["TEST:keepAlias"], type="float64", shape="scalar",
                      read=dict(backend="stable", key="read"), write=dict(backend="stable", key="command"),
                      confirm=dict(backend="stable", key="ack", timeout_ms=10000), initial=-999),
                 dict(name="other", type="float64", shape="scalar", read=dict(backend="other", key="read"))])
        path = directory / "config.json"
        path.write_text(json.dumps(config))
        publish("reload-stable", "read", 4.)
        publish("reload-other", "read", 23.)
        publish("reload-other-next", "read", 42.)
        ioc = pending = None
        with (directory / "ioc.log").open("w") as log:
            try:
                ioc = subprocess.Popen([args.ioc, "--config", str(path)], stdout=log, stderr=subprocess.STDOUT)

                def get(name):
                    return subprocess.check_output([args.pvxget, "-w", "1", name], env=environment,
                                                   text=True, stderr=subprocess.STDOUT, timeout=3)

                def wait_for(predicate, description, timeout=6):
                    deadline = time.monotonic() + timeout
                    while time.monotonic() < deadline:
                        if ioc.poll() is not None:
                            raise RuntimeError((directory / "ioc.log").read_text())
                        try:
                            if predicate():
                                return
                        except subprocess.CalledProcessError:
                            pass
                        time.sleep(.02)
                    raise AssertionError(description)

                wait_for(lambda: "value int64_t = 1" in get("SYS:reload:config:generation"), "initial generation")
                assert "value double = 4" in get("TEST:keep")
                assert "value double = 23" in get("TEST:other")
                # Rebuilding the unchanged stable runtime would now lose its last-good value.
                command("DEL", "{reload-stable}:read")
                for generation, value in ((2, 100.), (3, 200.)):
                    before = command("XLEN", "{reload-stable}:command")
                    pending = subprocess.Popen([args.pvxput, "-w", "12", "TEST:keepAlias", str(value)],
                                               env=environment, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                    wait_for(lambda: command("XLEN", "{reload-stable}:command") > before, "write dispatched")
                    changed = copy.deepcopy(config)
                    if generation == 2:
                        changed["redis_backends"]["other"]["base_key"] = "reload-other-next"
                    else:
                        changed["alarms"]["stream"] = "reload-alarms-next"
                    path.write_text(json.dumps(changed))
                    ioc.send_signal(signal.SIGHUP)
                    # Current synchronous confirmation callbacks block PVA GETs.
                    # Hold this command across the reload request, then release
                    # confirmation before querying the committed generation.
                    # Concurrent responsiveness belongs to the executor gate.
                    time.sleep(.75)
                    assert pending.poll() is None, "reload canceled the unchanged runtime's pending put"
                    publish("reload-stable", "ack", value)
                    output = pending.communicate(timeout=4)[0]
                    assert pending.returncode == 0, output
                    pending = None
                    wait_for(lambda: f"value int64_t = {generation}" in get("SYS:reload:config:generation"), "reload committed")
                    assert "value double = 4" in get("TEST:keep")
                    assert "value double = 4" in get("TEST:keepAlias")
                    assert "value double = 42" in get("TEST:other")
                    config = changed
                publish("reload-stable", "read", 9.)
                wait_for(lambda: "value double = 9" in get("TEST:keep"), "reader still receives updates")
                print("backend and alarm reloads preserve unchanged cached values, alias puts and readers")
            except Exception:
                print((directory / "ioc.log").read_text(), file=sys.stderr)
                raise
            finally:
                stop(pending)
                stop(ioc)


if __name__ == "__main__":
    main()
