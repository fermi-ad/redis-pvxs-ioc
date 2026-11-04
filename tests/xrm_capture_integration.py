#!/usr/bin/env python3
"""Black-box loopback qualification of XRM capture and its Redis handoff.

Run through scripts/run-with-redis.py, passing --sidecar /path/to/xrm-proxy,
--ioc /path/to/redis-pvxs-ioc and --pva-probe /path/to/xrm_capture_pva_probe.
The synthetic profile is the qualified 32-byte ES header followed by 16383
sample-major frames of sixteen little-endian int16 counts. No hardware,
non-loopback network, external Python packages, or operator configuration is used.
"""
import argparse
import contextlib
import copy
import json
import os
from pathlib import Path
import queue
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid


FRAME_BYTES = 524288
SAMPLES = 16383
CHANNELS = 16
MAGIC = 0xAA55F154
ARGS = None


def eventually(predicate, description, timeout=10):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except (OSError, AssertionError, ValueError) as error:
            last = error
        time.sleep(.025)
    raise AssertionError(f"{description} timed out; last error: {last}")


def unused_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def resp_command(*parts):
    values = [part if isinstance(part, bytes) else str(part).encode() for part in parts]
    return b"*%d\r\n" % len(values) + b"".join(
        b"$%d\r\n" % len(value) + value + b"\r\n" for value in values)


class RedisError(Exception):
    pass


def read_resp(stream, wire=None):
    """Parse RESP2, optionally retaining the exact bytes for the fault proxy."""
    line = stream.readline()
    if not line:
        raise EOFError("Redis connection closed")
    if not line.endswith(b"\r\n"):
        raise ValueError("truncated RESP line")
    if wire is not None:
        wire.append(line)
    kind, body = line[:1], line[1:-2]
    if kind in (b"+", b"-"):
        return RedisError(body.decode(errors="replace")) if kind == b"-" else body
    if kind == b":":
        return int(body)
    if kind == b"$":
        size = int(body)
        if size == -1:
            return None
        if size < 0 or size > 64 * 1024 * 1024:
            raise ValueError("unbounded RESP bulk string")
        data = stream.read(size + 2)
        if len(data) != size + 2 or not data.endswith(b"\r\n"):
            raise EOFError("truncated RESP bulk string")
        if wire is not None:
            wire.append(data)
        return data[:-2]
    if kind == b"*":
        size = int(body)
        if size == -1:
            return None
        if size < 0 or size > 100000:
            raise ValueError("unbounded RESP array")
        return [read_resp(stream, wire) for _ in range(size)]
    raise ValueError(f"unsupported RESP type {kind!r}")


class Redis:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.socket.settimeout(3)
        self.stream = self.socket.makefile("rb")

    def close(self):
        self.stream.close()
        self.socket.close()

    def command(self, *parts):
        self.socket.sendall(resp_command(*parts))
        result = read_resp(self.stream)
        if isinstance(result, RedisError):
            raise result
        return result

    def rows(self, key):
        return [(row[0], dict(zip(row[1][::2], row[1][1::2])))
                for row in self.command("XRANGE", key, "-", "+")]


class LoopbackDevice:
    """Keep accepts observable; tests control bytes and EOF on each connection."""
    def __init__(self):
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(8)
        self.listener.settimeout(.1)
        self.port = self.listener.getsockname()[1]
        self.connections = []
        self.accepted = queue.Queue()
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self._accept, daemon=True)
        self.thread.start()

    def _accept(self):
        while not self.stopped.is_set():
            try:
                client, _ = self.listener.accept()
                client.settimeout(5)
                self.connections.append(client)
                self.accepted.put(client)
            except socket.timeout:
                continue
            except OSError:
                return

    def accept(self, timeout=8):
        try:
            return self.accepted.get(timeout=timeout)
        except queue.Empty:
            raise AssertionError("capture did not connect to loopback device") from None

    def close(self):
        self.stopped.set()
        self.listener.close()
        for connection in self.connections:
            with contextlib.suppress(OSError):
                connection.shutdown(socket.SHUT_RDWR)
            connection.close()
        self.thread.join(timeout=2)


class RedisProxy:
    """Fault only capture's Redis traffic, without pausing the private server.

    Parsing is binary-safe and replies pass through unchanged. A dropped reply
    is triggered only after Redis has committed a publication script containing
    both the raw-stream key and capture lease key. Renewal scripts do not match.
    """
    def __init__(self, upstream, raw, lease):
        self.upstream, self.raw, self.lease = upstream, raw.encode(), lease.encode()
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(16)
        self.listener.settimeout(.1)
        self.port = self.listener.getsockname()[1]
        self.lock = threading.Lock()
        self.offline = False
        self.drop_next = False
        self.dropped_reply = threading.Event()
        self.stopped = threading.Event()
        self.sockets = set()
        self.workers = []
        self.thread = threading.Thread(target=self._accept, daemon=True)
        self.thread.start()

    def _accept(self):
        while not self.stopped.is_set():
            try:
                client, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with self.lock:
                offline = self.offline
                self.sockets.add(client)
            if offline:
                client.close()
                with self.lock:
                    self.sockets.discard(client)
                continue
            worker = threading.Thread(target=self._relay, args=(client,), daemon=True)
            self.workers.append(worker)
            worker.start()

    def _relay(self, client):
        upstream = None
        try:
            client.settimeout(3)
            upstream = socket.create_connection(("127.0.0.1", self.upstream), timeout=3)
            upstream.settimeout(3)
            with self.lock:
                self.sockets.add(upstream)
            with client.makefile("rb") as source, upstream.makefile("rb") as target:
                while not self.stopped.is_set():
                    request_wire = []
                    request = read_resp(source, request_wire)
                    with self.lock:
                        if self.offline:
                            return
                    upstream.sendall(b"".join(request_wire))
                    reply_wire = []
                    reply = read_resp(target, reply_wire)
                    publish = (isinstance(request, list) and request and
                               request[0].upper() in (b"EVAL", b"EVALSHA") and
                               self.raw in request[2:] and self.lease in request[2:])
                    with self.lock:
                        drop = publish and self.drop_next and not isinstance(reply, RedisError)
                        if drop:
                            self.drop_next = False
                    if drop:
                        self.dropped_reply.set()
                        return
                    client.sendall(b"".join(reply_wire))
        except (OSError, EOFError, ValueError):
            pass
        finally:
            for connection in (client, upstream):
                if connection is not None:
                    with contextlib.suppress(OSError):
                        connection.shutdown(socket.SHUT_RDWR)
                    connection.close()
                    with self.lock:
                        self.sockets.discard(connection)

    def set_offline(self, value):
        with self.lock:
            self.offline = value
            sockets = list(self.sockets) if value else []
        for connection in sockets:
            with contextlib.suppress(OSError):
                connection.shutdown(socket.SHUT_RDWR)

    def lose_next_publish_reply(self):
        with self.lock:
            self.drop_next = True
        self.dropped_reply.clear()

    def close(self):
        self.stopped.set()
        self.set_offline(True)
        self.listener.close()
        self.thread.join(timeout=2)
        for worker in self.workers:
            worker.join(timeout=2)


def count_value(sample, channel, tag):
    # Covers signed extremes and sample/channel distinctions without ES prefixes.
    return ((sample * 13 + channel * 997 + tag * 31) % 65536) - 32768


def pulse(tag):
    payload = bytearray(struct.pack("<8I", *([MAGIC] * 4), tag, tag + 12345, tag, tag + 12345))
    for sample in range(SAMPLES):
        payload.extend(struct.pack("<16h", *(count_value(sample, channel, tag)
                                             for channel in range(1, CHANNELS + 1))))
    assert len(payload) == FRAME_BYTES
    return bytes(payload)


def float32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def channel_goldens(tag, channel):
    counts = b"".join(struct.pack("<h", count_value(sample, channel, tag)) for sample in range(SAMPLES))
    volts = b"".join(struct.pack("<f", float32(float32(count_value(sample, channel, tag) * .25) - 1))
                     for sample in range(SAMPLES))
    current = b"".join(struct.pack("<f", float32(value[0] * 2.5))
                       for value in struct.iter_unpack("<f", volts))
    return counts, volts, current


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xrm-capture-test-")
        self.directory = Path(self.temporary.name)
        self.addCleanup(self.temporary.cleanup)
        self.redis = Redis(int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"]))
        self.addCleanup(self.redis.close)
        self.device = LoopbackDevice()
        self.addCleanup(self.device.close)
        self.prefix = "QUAL:" + uuid.uuid4().hex
        self.raw = self.prefix + ":raw"
        self.lease = self.raw + ":capture:lease:v1"
        self.capture_path = self.directory / "capture.json"
        self.demux_path = self.directory / "demux.json"
        self.manifest = self.directory / "gateway.yaml"
        self.log_path = self.directory / "sidecar.log"
        self.capture = {
            "schema_version": 1, "source_stream": self.raw,
            "device": {"host": "127.0.0.1", "port": self.device.port},
            "reconnect": {"initial_ms": 1000, "max_ms": 30000},
            "queue_limit_bytes": 32 * 1024 * 1024,
        }
        self.demux = self.configuration(self.redis.socket.getpeername()[1])
        self.write_configs()
        self.process = None

    def tearDown(self):
        result = self._outcome.result
        if any(test is self for test, _ in [*result.failures, *result.errors]):
            print("\nCapture process log for " + self.id() + ":\n" + self.logs(), file=sys.stderr)

    def configuration(self, port, currents=False):
        source = {
            "stream": self.raw, "checkpoint": self.prefix + ":demux-checkpoint",
            "payload_field": "_", "format": "acq4xx-es-int16-le", "layout": "sample-major",
            "samples_per_channel": SAMPLES, "start_id": "0-0", "retention": 8,
            "input_retention": 8, "timestamp": {"mode": "absent"},
            "channels": [{"channel": channel, "raw": self.channel(channel, "counts"),
                          "volts": self.channel(channel, "volts"), "eslo": .25, "eoff": -1}
                         for channel in range(1, CHANNELS + 1)],
        }
        config = {"schema_version": 1,
                  "redis": {"host": "127.0.0.1", "port": port, "connect_timeout_ms": 100,
                            "command_timeout_ms": 200, "poll_interval_ms": 10, "retry_ms": 50},
                  "sources": [source]}
        if currents:
            config["current_conversions"] = [
                {"stream": self.channel(channel, "volts"), "output": self.channel(channel, "current"),
                 "checkpoint": self.channel(channel, "checkpoint"), "samples_per_pulse": SAMPLES,
                 "payload_field": "_", "start_id": "0-0", "retention": 8,
                 "timestamp": {"mode": "absent"},
                 "gain": {"setting": self.prefix + ":gain:S", "applied": self.prefix + ":gain:R",
                          "bootstrap": 2.5}}
                for channel in range(1, CHANNELS + 1)]
        return config

    def channel(self, number, suffix):
        return f"{self.prefix}:CH{number}:{suffix}"

    def write_configs(self):
        self.capture_path.write_text(json.dumps(self.capture))
        self.demux_path.write_text(json.dumps(self.demux))

    def command(self, *extra):
        return [ARGS.sidecar, "--ip", "127.0.0.1", "--manifest", str(self.manifest),
                "--discovery-port", str(unused_udp_port()), "--discovery-timeout-ms", "50",
                "--request-timeout-ms", "50", "--retry-ms", "100",
                "--capture-config", str(self.capture_path), "--demux-config", str(self.demux_path), *extra]

    def start(self):
        self.write_configs()
        log = self.log_path.open("w+")
        self.process = subprocess.Popen(self.command(), stdout=log, stderr=log,
                                        env=dict(os.environ, EPICS_PVA_AUTO_ADDR_LIST="NO",
                                                 EPICS_PVA_NAME_SERVERS=""))
        self.addCleanup(log.close)
        self.addCleanup(self.stop)
        return self.process

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
                self.fail("capture process did not shut down within five seconds")
        if self.process is not None:
            self.assertEqual(self.process.returncode, 0, self.logs())

    def logs(self):
        return self.log_path.read_text() if self.log_path.exists() else ""

    def alive(self):
        self.assertIsNone(self.process.poll(), self.logs())

    def wait_rows(self, count, timeout=10):
        def ready():
            self.alive()
            rows = self.redis.rows(self.raw)
            return rows if len(rows) >= count else None
        return eventually(ready, f"{count} raw capture records; {self.logs()}", timeout)

    def proxy(self):
        proxy = RedisProxy(self.redis.socket.getpeername()[1], self.raw, self.lease)
        self.addCleanup(proxy.close)
        self.demux["redis"]["port"] = proxy.port
        return proxy

    def assert_no_startup_effects(self):
        self.assertEqual(len(self.device.connections), 0, "offline validation opened device socket")
        self.assertIsNone(self.redis.command("GET", self.lease), "offline validation claimed Redis lease")
        self.assertFalse(self.manifest.exists(), "offline validation published a manifest")
        self.assertFalse(Path(str(self.manifest) + ".lock").exists(), "offline validation took output lock")
        self.assertEqual(self.redis.command("EXISTS", self.raw), 0)

    def test_validation_and_forbidden_cli_have_no_side_effects(self):
        command = [ARGS.sidecar, "--check-capture-config", str(self.capture_path),
                   "--demux-config", str(self.demux_path)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_no_startup_effects()
        for change in ({"schema_version": 2}, {"unknown_capture_option": True},
                       {"queue_limit_bytes": FRAME_BYTES - 1},
                       {"device": {"host": "localhost", "port": self.device.port}}):
            with self.subTest(invalid=change):
                candidate = copy.deepcopy(self.capture)
                candidate.update(change)
                self.capture_path.write_text(json.dumps(candidate))
                result = subprocess.run(command, capture_output=True, text=True, timeout=5)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assert_no_startup_effects()
        self.write_configs()
        for extra in (["--once"], ["--inventory", str(self.directory / "unused.json")]):
            with self.subTest(forbidden=extra):
                result = subprocess.run(self.command(*extra), capture_output=True, text=True, timeout=5)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assert_no_startup_effects()
        # A capture file is never sufficient without its validated demux profile.
        result = subprocess.run([ARGS.sidecar, "--check-capture-config", str(self.capture_path)],
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assert_no_startup_effects()

    def test_fragmented_es_capture_future_ids_and_all_derived_channels(self):
        self.demux = self.configuration(self.redis.socket.getpeername()[1], currents=True)
        seed_id = b"4102444800000-100"
        self.redis.command("XADD", self.raw, seed_id, "_", b"not-a-complete-pulse")
        self.start()
        connection = self.device.accept()
        expected = pulse(17)
        wire = b"initial junk, including partial magic \x54\xf1\x55" + expected
        # Split the ES header, counter words, ADC frames, and pulse end.
        boundaries = (1, 7, 36, 42, 57, 71, 123, 1025, 32769, len(wire) - 3, len(wire))
        offset = 0
        for end in boundaries:
            connection.sendall(wire[offset:end])
            offset = end
        raw_rows = self.wait_rows(2)
        source_id, fields = raw_rows[-1]
        self.assertGreater(tuple(map(int, source_id.split(b"-"))), tuple(map(int, seed_id.split(b"-"))))
        self.assertEqual(fields, {b"_": expected}, "capture changed bytes or added raw envelope metadata")
        eventually(lambda: all(self.redis.command("XLEN", self.channel(channel, "current")) == 1
                               for channel in range(1, CHANNELS + 1)),
                   "all derived current channels", timeout=20)
        for channel in range(1, CHANNELS + 1):
            with self.subTest(channel=channel):
                count_rows = self.redis.rows(self.channel(channel, "counts"))
                volt_rows = self.redis.rows(self.channel(channel, "volts"))
                current_rows = self.redis.rows(self.channel(channel, "current"))
                self.assertEqual((len(count_rows), len(volt_rows), len(current_rows)), (1, 1, 1))
                counts, volts, current = count_rows[0][1], volt_rows[0][1], current_rows[0][1]
                expected_counts, expected_volts, expected_current = channel_goldens(17, channel)
                self.assertEqual(counts[b"_"], expected_counts)
                self.assertEqual(volts[b"_"], expected_volts)
                self.assertEqual(current[b"_"], expected_current)
                self.assertEqual(counts[b"source_id"], source_id)
                self.assertEqual(volts[b"source_id"], source_id)
                self.assertEqual(current[b"upstream_source_id"], source_id)
                for record in (counts, volts, current):
                    self.assertEqual(record[b"acquisition_time_valid"], b"0")
                    self.assertNotIn(b"acquisition_time_ns", record)
                    self.assertEqual(record[b"event_counter_0"], b"17")
                    self.assertEqual(record[b"event_counter_1"], b"12362")

    def test_complete_capture_to_pva_preserves_waveforms_units_and_unknown_time(self):
        self.demux = self.configuration(self.redis.socket.getpeername()[1], currents=True)
        # RedisAdapter's wire schema is {base}:sub. Produce the two IOC-facing
        # streams with that schema, while retaining a unique per-test suffix.
        volts_key = "{XRM}:" + self.channel(1, "volts")
        current_key = "{XRM}:" + self.channel(1, "current")
        self.demux["sources"][0]["channels"][0]["volts"] = volts_key
        self.demux["current_conversions"][0]["stream"] = volts_key
        self.demux["current_conversions"][0]["output"] = current_key
        self.start()
        connection = self.device.accept()
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        endpoint = f"127.0.0.1:{port}"
        config = {
            "schema_version": 1,
            "server": {"instance": "capture-fixture", "namespace": "", "interfaces": ["127.0.0.1"],
                       "tcp_port": port, "udp_port": port, "auto_beacon": False},
            "redis": {"host": "127.0.0.1", "port": self.redis.socket.getpeername()[1], "base_key": "XRM"},
            "discovery": {"enabled": False},
            "pvs": [
                {"name": name, "type": "float32", "shape": "array",
                 "read": {"backend": "redis", "key": self.channel(1, suffix)},
                 "source_time": {"mode": "unavailable"},
                 "source_health": {"required": False, "stale_after_ms": 0},
                 "metadata": {"units": units}}
                for name, suffix, units in (("CAPTURE:VOLTS", "volts", "V"),
                                            ("CAPTURE:MAMPS", "current", "mA"))],
        }
        config_path = self.directory / "ioc.json"
        config_path.write_text(json.dumps(config))
        ioc_log_path = self.directory / "ioc.log"
        environment = dict(os.environ, EPICS_PVA_AUTO_ADDR_LIST="NO", EPICS_PVA_ADDR_LIST=endpoint,
                           EPICS_PVA_NAME_SERVERS="")
        with ioc_log_path.open("w+") as log:
            ioc = subprocess.Popen([ARGS.ioc, "--config", str(config_path)], stdout=log, stderr=log,
                                   env=environment)
            try:
                expected = pulse(81)
                connection.sendall(expected[:19])
                connection.sendall(expected[19:])
                self.assertEqual(self.wait_rows(1)[0][1], {b"_": expected})
                eventually(lambda: self.redis.command("XLEN", current_key) == 1,
                           "captured pulse reaches IOC-facing current stream", timeout=20)
                _, volts, current = channel_goldens(81, 1)
                for name, units, golden in (("CAPTURE:VOLTS", "V", volts),
                                             ("CAPTURE:MAMPS", "mA", current)):
                    golden_path = self.directory / (name.replace(":", "-") + ".float32-le")
                    golden_path.write_bytes(golden)
                    last_probe = []

                    def probe_ready():
                        self.alive()
                        self.assertIsNone(ioc.poll(), ioc_log_path.read_text())
                        result = subprocess.run([ARGS.pva_probe, endpoint, name, str(golden_path), units],
                                                capture_output=True, text=True, timeout=8, env=environment)
                        last_probe[:] = [result.stdout, result.stderr]
                        return result.returncode == 0

                    try:
                        eventually(probe_ready, name + " exact float32 PVA waveform/units/unknown time",
                                   timeout=15)
                    except (AssertionError, subprocess.TimeoutExpired) as error:
                        self.fail(f"{error}\nProbe output: {last_probe!r}\nIOC log:\n{ioc_log_path.read_text()}")
            finally:
                if ioc.poll() is None:
                    ioc.terminate()
                    try:
                        ioc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        ioc.kill()
                        ioc.wait(timeout=5)
                        self.fail("IOC did not stop within five seconds: " + ioc_log_path.read_text())
            self.assertEqual(ioc.returncode, 0, ioc_log_path.read_text())

    def test_raw_and_derived_retention_remain_bounded(self):
        self.demux["sources"][0]["retention"] = 2
        self.demux["sources"][0]["input_retention"] = 2
        self.start()
        connection = self.device.accept()
        payloads = [pulse(tag) for tag in range(71, 75)]
        for tag, payload in zip(range(71, 75), payloads):
            connection.sendall(payload)
            # Waiting for demux progress avoids inducing an unrelated input gap.
            eventually(lambda: any(fields.get(b"event_counter_0") == str(tag).encode()
                                   for _, fields in self.redis.rows(self.channel(16, "volts"))),
                       f"retained pulse {tag} reaches derived stream")
            self.assertLessEqual(self.redis.command("XLEN", self.raw), 2)
        self.assertEqual([fields[b"_"] for _, fields in self.redis.rows(self.raw)], payloads[-2:])
        for channel in range(1, CHANNELS + 1):
            for suffix in ("counts", "volts"):
                rows = self.redis.rows(self.channel(channel, suffix))
                self.assertEqual(len(rows), 2)
                self.assertEqual([fields[b"event_counter_0"] for _, fields in rows], [b"73", b"74"])

    def test_connected_idle_has_no_cadence_reconnect(self):
        self.start()
        connection = self.device.accept()
        connection.sendall(pulse(21))
        self.wait_rows(1)
        # Silence for longer than the old two-second acquisition socket timeout.
        deadline = time.monotonic() + 2.3
        while time.monotonic() < deadline:
            self.alive()
            self.assertEqual(len(self.device.connections), 1, self.logs())
            time.sleep(.05)
        connection.sendall(pulse(22))
        rows = self.wait_rows(2)
        self.assertEqual([row[1][b"_"] for row in rows], [pulse(21), pulse(22)])
        self.assertEqual(len(self.device.connections), 1)

    def test_midpulse_eof_discards_partial_and_reconnects_cleanly(self):
        self.start()
        first = self.device.accept()
        first.sendall(pulse(31)[:100003])
        first.shutdown(socket.SHUT_RDWR)
        first.close()
        second = self.device.accept(timeout=6)
        expected = pulse(32)
        second.sendall(expected)
        self.assertEqual(self.wait_rows(1)[0][1], {b"_": expected})
        self.assertEqual(self.redis.command("XLEN", self.raw), 1)
        eventually(lambda: "severity=ERROR event=disconnect" in self.logs(), "disconnect ERROR reporting")
        self.assertIn("event=retry", self.logs())
        eventually(lambda: "event=recovery" in self.logs(), "first-valid-pulse recovery reporting")

    def test_existing_capture_lease_prevents_initial_device_connection(self):
        owner = b"fixture-other-owner"
        self.redis.command("SET", self.lease, owner, "PX", 10000)
        self.start()
        deadline = time.monotonic() + 1.3
        while time.monotonic() < deadline:
            self.alive()
            self.assertEqual(len(self.device.connections), 0, self.logs())
            time.sleep(.05)
        self.assertEqual(self.redis.command("GET", self.lease), owner)
        self.assertEqual(self.redis.command("XLEN", self.raw), 0)
        self.stop()
        self.assertEqual(self.redis.command("GET", self.lease), owner,
                         "worker released a lease owned by another capture")

    def test_replaced_lease_fences_raw_publication(self):
        self.start()
        connection = self.device.accept()
        self.assertIsNotNone(self.redis.command("GET", self.lease))
        owner = b"fixture-replaced-owner"
        self.redis.command("SET", self.lease, owner, "PX", 10000)
        with contextlib.suppress(BrokenPipeError, ConnectionResetError):
            connection.sendall(pulse(41))
        eventually(lambda: any(int(value) >= 1 for value in re.findall(
            r"\breceived=(\d+)", self.logs())), "fenced pulse reaches capture publisher")
        eventually(lambda: re.search(r"leaseState=expired|event=lease[^\n]*state=contended", self.logs()),
                   "lease loss reporting")
        deadline = time.monotonic() + 1.2
        while time.monotonic() < deadline:
            self.alive()
            self.assertEqual(self.redis.command("XLEN", self.raw), 0, "unowned capture published raw data")
            time.sleep(.05)
        self.assertEqual(self.redis.command("GET", self.lease), owner)

    def test_committed_publication_with_lost_reply_is_never_replayed(self):
        proxy = self.proxy()
        self.start()
        connection = self.device.accept()
        proxy.lose_next_publish_reply()
        first, second = pulse(51), pulse(52)
        connection.sendall(first)
        self.assertTrue(proxy.dropped_reply.wait(timeout=8), "proxy did not lose a committed capture reply")
        self.assertEqual(self.wait_rows(1)[0][1], {b"_": first})
        eventually(lambda: "event=uncertain_write" in self.logs(), "ambiguous publication is reported")
        connection.sendall(second)
        rows = self.wait_rows(2)
        self.assertEqual([row[1][b"_"] for row in rows], [first, second])
        deadline = time.monotonic() + 1.1
        while time.monotonic() < deadline:
            self.alive()
            self.assertEqual(self.redis.command("XLEN", self.raw), 2, "ambiguous publication was replayed")
            self.assertEqual(len(self.device.connections), 1, "Redis reply loss reconnected XRM")
            time.sleep(.05)

    def test_redis_outage_bounds_queue_and_preserves_device_connection(self):
        proxy = self.proxy()
        self.capture["queue_limit_bytes"] = 2 * FRAME_BYTES
        self.start()
        connection = self.device.accept()
        baseline = pulse(60)
        connection.sendall(baseline)
        self.wait_rows(1)
        backlog = [pulse(tag) for tag in range(61, 69)]
        proxy.set_offline(True)
        try:
            for item in backlog:
                connection.sendall(item)
            eventually(lambda: "severity=ERROR event=queue_drop" in self.logs(),
                       "bounded capture queue overflow reporting", timeout=3)
            # A successful send only proves kernel delivery. Wait for the
            # receiver's periodic count before restoring Redis, so the bound
            # cannot race unconsumed data still sitting in the TCP receive queue.
            eventually(lambda: any(int(value) >= 9 for value in re.findall(
                r"\breceived=(\d+)", self.logs())), "all outage pulses consumed", timeout=7)
            self.assertEqual(len(self.device.connections), 1, "Redis outage reconnected XRM")
        finally:
            proxy.set_offline(False)
        eventually(lambda: any(fields.get(b"_") == backlog[-1] for _, fields in self.redis.rows(self.raw)),
                   "queue recovery publishes newest pulse", timeout=10)
        sentinel = pulse(69)
        connection.sendall(sentinel)
        rows = eventually(lambda: self.redis.rows(self.raw) if any(
            fields.get(b"_") == sentinel for _, fields in self.redis.rows(self.raw)) else None,
            "same XRM connection publishes after Redis recovery", timeout=10)
        payloads = [fields[b"_"] for _, fields in rows]
        self.assertEqual(payloads[0], baseline)
        self.assertEqual(payloads[-1], sentinel)
        self.assertLessEqual(len(payloads), 5, "queue retained more than two queued and one in-flight pulse")
        self.assertEqual(len(payloads), len(set(payloads)), "queue replayed a pulse")
        tags = [struct.unpack_from("<I", payload, 16)[0] for payload in payloads]
        self.assertEqual(tags, sorted(tags), "recovered queue reordered pulses")
        self.assertEqual(len(self.device.connections), 1)
        eventually(lambda: re.search(r"localDrops=[1-9]", self.logs()), "counted queue drops")

    def test_unbounded_framing_junk_latches_fault_without_reconnect(self):
        self.start()
        connection = self.device.accept()
        connection.sendall(b"\x00" * (2 * FRAME_BYTES + 64))
        eventually(lambda: "severity=ERROR event=framing_fault" in self.logs(), "framing search budget fault")
        deadline = time.monotonic() + 1.2
        while time.monotonic() < deadline:
            self.alive()
            self.assertEqual(len(self.device.connections), 1, "latched framing fault reconnected device")
            self.assertEqual(self.redis.command("XLEN", self.raw), 0)
            time.sleep(.05)
        self.assertIn("state=fault", self.logs())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sidecar", required=True)
    parser.add_argument("--ioc", required=True)
    parser.add_argument("--pva-probe", required=True)
    ARGS, remaining = parser.parse_known_args()
    ARGS.sidecar = str(Path(ARGS.sidecar).resolve())
    ARGS.ioc = str(Path(ARGS.ioc).resolve())
    ARGS.pva_probe = str(Path(ARGS.pva_probe).resolve())
    if "REDIS_PVXS_TEST_REDIS_PORT" not in os.environ:
        parser.error("run through scripts/run-with-redis.py for an isolated Redis")
    unittest.main(argv=[__file__, *remaining])
