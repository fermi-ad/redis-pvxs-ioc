#!/usr/bin/env python3
"""Isolated RecCaster wire/PVA discovery acceptance, including hot reload."""
import argparse
import copy
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import time

MAGIC = 0x5243
HEADER = struct.Struct("!HHI")


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def receive(sock, count):
    data = b""
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part:
            raise RuntimeError("unexpected RecCaster disconnect")
        data += part
    return data


def message(sock):
    magic, code, size = HEADER.unpack(receive(sock, 8))
    assert magic == MAGIC and size <= 1024 * 1024
    return code, receive(sock, size)


def send(sock, code, payload):
    sock.sendall(HEADER.pack(MAGIC, code, len(payload)) + payload)


class Receiver:
    def __init__(self, port):
        self.target = ("127.0.0.1", port)
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.listener.settimeout(0.1)
        self.packet = struct.pack("!HBBIHHi", MAGIC, 0, 0, 0xffffffff,
                                  self.listener.getsockname()[1], 0, 123456)
        self.connections = []

    def close(self):
        for conn in self.connections:
            conn.close()
        self.listener.close()
        self.udp.close()

    def connect(self):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            self.udp.sendto(self.packet, self.target)
            try:
                conn, _ = self.listener.accept()
                conn.settimeout(5)
                self.connections.append(conn)
                return conn
            except socket.timeout:
                pass
        raise AssertionError("RecCaster did not discover the receiver")

    def catalog(self, conn):
        code, greeting = message(conn)
        assert code == 1 and greeting == struct.pack("!BBxxI", 0, 0, 123456)
        send(conn, 0x8001, b"\0")
        records, identity = {}, {}
        while True:
            code, payload = message(conn)
            if code == 5:
                assert payload == b"\0" * 4
                break
            if code == 3:
                rid, kind, tlen, nlen = struct.unpack("!IBBH", payload[:8])
                typename = payload[8:8 + tlen].decode()
                name = payload[8 + tlen:].decode()
                assert len(payload) == 8 + tlen + nlen
                if kind == 0:
                    records[rid] = dict(name=name, type=typename, aliases=[], properties={})
                else:
                    assert kind == 1 and not typename
                    records[rid]["aliases"].append(name)
            else:
                assert code == 6
                rid, klen, vlen = struct.unpack("!IBxH", payload[:8])
                assert len(payload) == 8 + klen + vlen
                key, value = payload[8:8 + klen].decode(), payload[8 + klen:].decode()
                (records[rid]["properties"] if rid else identity)[key] = value
        self.ping(conn)
        return {record["name"]: record for record in records.values()}, identity

    @staticmethod
    def ping(conn):
        nonce = b"test"
        send(conn, 0x8002, nonce)
        assert message(conn) == (2, nonce)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ioc", required=True)
    parser.add_argument("--pvxget", required=True)
    args = parser.parse_args()
    redis_port = int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"])
    udp_port = free_port(socket.SOCK_DGRAM)
    pva_port = free_port(socket.SOCK_DGRAM)
    config = dict(server=dict(instance="discovery-test", namespace="DISC", interfaces=["127.0.0.1"],
                              tcp_port=0, udp_port=pva_port, auto_beacon=False),
                  discovery=dict(bind_address="127.0.0.1", udp_port=udp_port, timeout_ms=1500, max_holdoff_ms=0,
                                 max_records=32),
                  redis=dict(base_key="discovery-test", host="127.0.0.1", port=redis_port),
                  pvs=[dict(name="value", aliases=["ALIAS:old"], type="float64", shape="scalar",
                            read=dict(key="value"), initial=7, metadata=dict(description="Value")),
                       dict(name="removed", type="float64", shape="scalar", read=dict(key="removed"))])
    receiver = Receiver(udp_port)
    with tempfile.TemporaryDirectory(prefix="redis-pvxs-discovery-") as directory:
        path = Path(directory) / "config.json"
        path.write_text(json.dumps(config))
        log_path = Path(directory) / "ioc.log"
        with log_path.open("w+") as log:
            process = subprocess.Popen([args.ioc, "--config", str(path)], stdout=log, stderr=log)
            env = dict(os.environ, EPICS_PVA_AUTO_ADDR_LIST="NO", EPICS_PVA_ADDR_LIST=f"127.0.0.1:{pva_port}",
                       EPICS_PVA_NAME_SERVERS="")

            def get(name):
                return subprocess.check_output([args.pvxget, "-w", "3", "-F", "tree", name], env=env,
                                               text=True, stderr=subprocess.STDOUT, timeout=5)

            def reload(value):
                temporary = path.with_suffix(".tmp")
                temporary.write_text(json.dumps(value))
                temporary.replace(path)
                process.send_signal(signal.SIGHUP)

            try:
                first = receiver.connect()
                records, identity = receiver.catalog(first)
                assert records["DISC:value"]["aliases"] == ["ALIAS:old"]
                assert records["DISC:value"]["type"] == "epics:nt/NTScalar:1.0"
                assert identity["IOCNAME"] == "discovery-test"
                assert 0 < int(identity["PVAS_SERVER_PORT"]) <= 65535
                assert identity["PVXS_PROTOCOL"] == "pva" and "RSRV_SERVER_PORT" not in identity
                assert "SYS:discovery-test:discovery:status" in records
                assert "ALIAS:old" in get("ALIAS:old")
                time.sleep(1.1)
                assert '"synchronized"' in get("SYS:discovery-test:discovery:status")

                # Rejected staging leaves the existing RecCeiver session intact.
                invalid = copy.deepcopy(config)
                invalid["pvs"][0]["aliases"] = ["DISC:removed"]
                reload(invalid)
                time.sleep(0.6)
                receiver.ping(first)
                assert "reload failed" in get("SYS:discovery-test:config:lastStatus")
                assert "ALIAS:old" in get("ALIAS:old")

                # Valid definitions fail the catalog count gate after runtime
                # staging; neither registrations nor the active session change.
                over_limit = copy.deepcopy(config)
                for index in range(20):
                    over_limit["pvs"].append(dict(name=f"extra:{index}", type="float64", shape="scalar",
                                                  read=dict(key=f"extra:{index}")))
                reload(over_limit)
                deadline = time.monotonic() + 5
                while "reload rejected" not in get("SYS:discovery-test:config:lastStatus"):
                    assert time.monotonic() < deadline
                    time.sleep(0.1)
                receiver.ping(first)
                assert "ALIAS:old" in get("ALIAS:old")

                changed = copy.deepcopy(config)
                changed["pvs"] = [changed["pvs"][0]]
                changed["pvs"][0]["aliases"] = ["ALIAS:new"]
                changed["pvs"][0]["metadata"]["description"] = "Changed"
                reload(changed)
                second = receiver.connect()
                records, identity = receiver.catalog(second)
                assert identity["CONFIG_GENERATION"] == "2"
                assert records["DISC:value"]["aliases"] == ["ALIAS:new"]
                assert records["DISC:value"]["properties"]["DESC"] == "Changed"
                assert "DISC:removed" not in records
                assert "ALIAS:new" in get("ALIAS:new")

                # A receiver restart gets a complete current-generation upload.
                second.close()
                third = receiver.connect()
                replay, identity = receiver.catalog(third)
                assert replay == records and identity["CONFIG_GENERATION"] == "2"

                # Malformed control data closes this session, then discovery recovers.
                third.sendall(HEADER.pack(MAGIC, 0x8002, 0xffffffff))
                fourth = receiver.connect()
                replay, _ = receiver.catalog(fourth)
                assert replay == records
                fourth.close()
                stalled = receiver.connect()
                assert message(stalled)[0] == 1
                # Withhold the greeting. The deadline releases this connection,
                # while ordinary PVA operations keep working.
                assert "ALIAS:new" in get("ALIAS:new")
                time.sleep(1.7)
                stalled.close()
                recovered = receiver.connect()
                replay, _ = receiver.catalog(recovered)
                assert replay == records
                # Stop interrupts the long heartbeat wait.
                start = time.monotonic()
                process.terminate()
                assert process.wait(timeout=3) == 0
                assert time.monotonic() - start < 3
                print("discovery wire compatibility, aliases, PVA lookup, reload rejection, replacement, reconnect and bounded shutdown passed")
            except Exception:
                log.flush()
                print(log_path.read_text())
                raise
            finally:
                receiver.close()
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == "__main__":
    main()
