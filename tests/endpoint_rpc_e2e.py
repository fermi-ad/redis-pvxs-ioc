#!/usr/bin/env python3
"""Exercise namespace rejection against actual reflected services and PVA."""
import argparse
import copy
import json
import os
from pathlib import Path
import selectors
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
    for name in ("ioc", "fixture", "pvxget", "pvxcall"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="redis-pvxs-rpc-") as directory:
        directory = Path(directory)
        fixture = subprocess.Popen([args.fixture], stdout=subprocess.PIPE, text=True)
        ioc = None
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(fixture.stdout, selectors.EVENT_READ)
                if not selector.select(10):
                    raise RuntimeError("reflection fixture did not start")
                grpc_port = int(fixture.stdout.readline().strip())
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0))
                port = listener.getsockname()[1]
            environment = dict(os.environ, EPICS_PVA_AUTO_ADDR_LIST="NO",
                               EPICS_PVA_ADDR_LIST=f"127.0.0.1:{port}")
            redis_port = int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"])

            def publish(value):
                fields = [b"XADD", b"{endpoints}:keep", b"*", b"_", struct.pack("=d", value)]
                packet = b"*5\r\n" + b"".join(b"$" + str(len(field)).encode() + b"\r\n" + field + b"\r\n" for field in fields)
                with socket.create_connection(("127.0.0.1", redis_port), timeout=2) as client:
                    client.sendall(packet)
                    with client.makefile("rb") as reply:
                        header = reply.readline()
                        assert header.startswith(b"$"), header
                        assert reply.read(int(header[1:]) + 2).endswith(b"\r\n")

            publish(4.)
            config = dict(
                server=dict(instance="endpoints", namespace="TEST", interfaces=["127.0.0.1"],
                            tcp_port=port, udp_port=port, auto_beacon=False),
                redis=dict(host="127.0.0.1", port=redis_port, base_key="endpoints"),
                discovery=dict(enabled=False),
                pvs=[dict(name="keep", type="float64", shape="scalar", read=dict(key="keep"), initial=4)],
                rpc_services=[dict(endpoint=f"127.0.0.1:{grpc_port}", service="redis_pvxs_test.First", suffix="_RPC")])
            path = directory / "config.json"
            path.write_text(json.dumps(config))
            with (directory / "ioc.log").open("w") as log:
                ioc = subprocess.Popen([args.ioc, "--config", str(path)], stdout=log, stderr=subprocess.STDOUT)

                def get(name):
                    return subprocess.check_output([args.pvxget, "-w", "1", name], env=environment,
                                                   text=True, stderr=subprocess.STDOUT, timeout=3)

                def wait_for(name, expected):
                    deadline = time.monotonic() + 15
                    while time.monotonic() < deadline:
                        if ioc.poll() is not None:
                            raise RuntimeError((directory / "ioc.log").read_text())
                        try:
                            if expected in get(name):
                                return
                        except subprocess.CalledProcessError:
                            pass
                        time.sleep(.05)
                    raise AssertionError(f"{name} did not contain {expected}")

                wait_for("SYS:endpoints:config:generation", "value int64_t = 1")
                changed = copy.deepcopy(config)
                changed["pvs"][0]["aliases"] = ["TEST:VALUE_RPC"]
                path.write_text(json.dumps(changed))
                ioc.send_signal(signal.SIGHUP)
                wait_for("SYS:endpoints:config:lastError", "duplicate served PV name")
                assert "value int64_t = 1" in get("SYS:endpoints:config:generation")
                assert "value double = 4" in get("TEST:keep")
                publish(5.)
                wait_for("TEST:keep", "value double = 5")

                changed = copy.deepcopy(config)
                changed["rpc_services"].append(dict(config["rpc_services"][0], service="redis_pvxs_test.Second"))
                path.write_text(json.dumps(changed))
                ioc.send_signal(signal.SIGHUP)
                wait_for("SYS:endpoints:config:lastError", "owned by redis_pvxs_test.First/Value")
                assert "value int64_t = 1" in get("SYS:endpoints:config:generation")
                publish(6.)
                wait_for("TEST:keep", "value double = 6")
                reply = subprocess.check_output([args.pvxcall, "-w", "3", "TEST:VALUE_RPC", "number=5"],
                                                env=environment, text=True, stderr=subprocess.STDOUT, timeout=5)
                assert "int32_t number = 5" in reply, reply
                stop(ioc)
                ioc = None

                changed = copy.deepcopy(config)
                changed["server"]["namespace"] = ""
                changed["rpc_services"][0]["suffix"] = ":endpoints:config:reload"
                path.write_text(json.dumps(changed))
                rejected = subprocess.run([args.ioc, "--config", str(path)], text=True,
                                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
                assert rejected.returncode != 0
                assert "reserved metadata PV 'SYS:endpoints:config:reload'" in rejected.stdout, rejected.stdout
            print("reflected RPC collisions preserve the live generation and callable endpoints")
        except Exception:
            logfile = directory / "ioc.log"
            if logfile.exists():
                print(logfile.read_text(), file=sys.stderr)
            raise
        finally:
            stop(ioc)
            stop(fixture)
            if fixture.stdout:
                fixture.stdout.close()


if __name__ == "__main__":
    main()
