#!/usr/bin/env python3
"""An unchanged current alarm must recover after its Redis destination is repaired."""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ioc", required=True)
    parser.add_argument("--pvxget", required=True)
    args = parser.parse_args()
    redis_port = int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"])

    def command(*fields):
        fields = [field if isinstance(field, bytes) else str(field).encode() for field in fields]
        packet = b"*" + str(len(fields)).encode() + b"\r\n"
        packet += b"".join(b"$" + str(len(field)).encode() + b"\r\n" + field + b"\r\n" for field in fields)
        with socket.create_connection(("127.0.0.1", redis_port), timeout=2) as connection:
            connection.sendall(packet)
            with connection.makefile("rb") as reply:
                header = reply.readline()
                if header.startswith(b"$"):
                    return reply.read(int(header[1:]) + 2)[:-2]
                if header.startswith(b":"):
                    return int(header[1:])
                if header.startswith(b"+"):
                    return header[1:-2]
                raise AssertionError(header)

    def wait_for(predicate, message):
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            try:
                if predicate():
                    return
            except subprocess.CalledProcessError:
                pass
            time.sleep(.02)
        raise AssertionError(message)

    command("XADD", "{alarm-recovery}:read", "*", "_", struct.pack("=d", 0.))
    command("SET", "alarm-recovery-destination", "wrong-type")
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        pva_port = listener.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="alarm-recovery-") as folder:
        folder = Path(folder)
        config = dict(server=dict(instance="alarms", namespace="TEST", interfaces=["127.0.0.1"],
                                  tcp_port=pva_port, udp_port=pva_port, auto_beacon=False),
                      redis=dict(host="127.0.0.1", port=redis_port, base_key="alarm-recovery"),
                      discovery=dict(enabled=False), alarms=dict(stream="alarm-recovery-destination"),
                      pvs=[dict(name="value", type="float64", shape="scalar", read=dict(key="read"),
                                alarm=dict(high_warning=1, high_alarm=2))])
        path = folder / "config.json"
        path.write_text(json.dumps(config))
        env = dict(os.environ, EPICS_PVA_AUTO_ADDR_LIST="NO", EPICS_PVA_ADDR_LIST=f"127.0.0.1:{pva_port}")
        with (folder / "ioc.log").open("w") as log:
            process = subprocess.Popen([args.ioc, "--config", str(path)], stdout=log, stderr=subprocess.STDOUT)
            try:
                def get():
                    assert process.poll() is None, (folder / "ioc.log").read_text()
                    return subprocess.check_output([args.pvxget, "-w", "1", "TEST:value"],
                                                   env=env, text=True, stderr=subprocess.STDOUT, timeout=3)

                def attempts():
                    match = re.search(rb"cmdstat_xadd:calls=(\d+)", command("INFO", "commandstats"))
                    return int(match[1]) if match else 0

                wait_for(lambda: "value double = 0" in get(), "startup readback")
                before = attempts()
                command("XADD", "{alarm-recovery}:read", "*", "_", struct.pack("=d", 3.))
                wait_for(lambda: "value double = 3" in get(), "major alarm source readback")
                wait_for(lambda: attempts() >= before + 2, "attempted alarm delivery to the wrong-type key")
                command("DEL", "alarm-recovery-destination")
                # No new source sample or alarm transition is generated here.
                wait_for(lambda: command("XLEN", "alarm-recovery-destination") >= 1,
                         "current alarm was not reconciled after destination repair")
                assert "value double = 3" in get()
                print("unchanged current alarm reconciled after Redis rejection")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    main()
