"""Bounded producer/storage regressions, not release qualification evidence.

The loopback RESP fixture inspects real socket traffic. Separate temporary
directories model the producer and host storage boundaries; they do not claim
to reproduce a Linux tmpfs mount or the historical host's disk workload.
"""
import importlib.util
import json
from pathlib import Path
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


traffic = load("producer_traffic", "qualification-traffic.py")
qualification = load("producer_qualification", "qualify-image.py")


class RedisFixture(socketserver.TCPServer):
    """Only the XADD protocol used by the producer; no external Redis needed."""

    allow_reuse_address = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), RedisHandler)
        self.entries, self.errors = [], []
        self.worker = threading.Thread(target=self.serve_forever,
                                       kwargs={"poll_interval": 0.01}, daemon=True)

    def __enter__(self):
        self.worker.start()
        return self

    def __exit__(self, *_):
        self.shutdown()
        self.server_close()
        self.worker.join(timeout=1)


class RedisHandler(socketserver.StreamRequestHandler):
    def handle(self):
        try:
            while True:
                line = self.rfile.readline()
                if not line:
                    return
                if not line.startswith(b"*") or not line.endswith(b"\r\n"):
                    raise ValueError("invalid RESP command header")
                arguments = []
                for _ in range(int(line[1:])):
                    header = self.rfile.readline()
                    if not header.startswith(b"$"):
                        raise ValueError("invalid RESP argument header")
                    length = int(header[1:])
                    if not 0 <= length <= 20000:
                        raise ValueError("fixture argument outside bound")
                    data = self.rfile.read(length + 2)
                    if len(data) != length + 2 or not data.endswith(b"\r\n"):
                        raise ValueError("truncated fixture argument")
                    arguments.append(data[:-2])
                if (len(arguments) != 7 or arguments[0] != b"XADD"
                        or arguments[2:4] != [b"MAXLEN", b"1024"]
                        or arguments[5] != b"_"):
                    raise ValueError("unexpected fixture command")
                self.server.entries.append((time.monotonic_ns(), arguments[1],
                                            arguments[4], arguments[6]))
                stamp = arguments[4]
                self.wfile.write(b"$" + str(len(stamp)).encode() + b"\r\n"
                                 + stamp + b"\r\n")
                self.wfile.flush()
        except Exception as error:
            self.server.errors.append(error)


class ProducerStorageTests(unittest.TestCase):
    def produce(self, fixture, output, duration):
        # Keep test signal handlers from changing the test runner's handlers.
        with patch.object(traffic.signal, "signal"):
            traffic.produce("127.0.0.1", "unit", output, duration,
                            fixture.server_address[1])

    def ledger(self, output):
        return [json.loads(line) for line in
                output.with_name("producer-events.jsonl").read_text().splitlines()]

    def check_socket_entries(self, fixture):
        self.assertEqual(fixture.errors, [])
        self.assertGreater(len(fixture.entries), 0)
        self.assertEqual(len(fixture.entries) % 2, 0)
        previous = None
        for scalar, array in zip(fixture.entries[::2], fixture.entries[1::2]):
            self.assertEqual(scalar[1], b"{unit}:scalar")
            self.assertEqual(array[1], b"{unit}:array")
            self.assertEqual(scalar[2], array[2])
            cursor = tuple(map(int, scalar[2].split(b"-")))
            if previous is not None:
                self.assertGreater(cursor, previous)
            previous = cursor
            value = struct.unpack("<I", scalar[3])[0]
            self.assertEqual(struct.unpack("<4096I", array[3]),
                             tuple(range(value, value + 4096)))

    def test_synchronous_host_write_reproduces_recorded_fatal_stall(self):
        with tempfile.TemporaryDirectory() as temporary, RedisFixture() as fixture:
            output = Path(temporary) / "host" / "producer.json"
            original = Path.write_text
            delayed = []

            def slow_write(path, *args, **kwargs):
                if path == output.with_suffix(".tmp") and not delayed:
                    delayed.append(time.monotonic_ns())
                    time.sleep(2.2)
                return original(path, *args, **kwargs)

            with patch.object(Path, "write_text", slow_write):
                with self.assertRaisesRegex(ValueError, "scheduling stall"):
                    self.produce(fixture, output, 4)
            self.check_socket_entries(fixture)
            ledger = self.ledger(output)
            self.assertEqual([entry["kind"] for entry in ledger], ["start", "stall"])
            self.assertGreater(ledger[1]["end_mono_ns"] - ledger[1]["start_mono_ns"],
                               2 * 10**9)
            self.assertEqual(len(fixture.entries), 2)  # Fails before the next write.

    def test_separate_host_write_delay_keeps_actual_source_traffic_moving(self):
        with tempfile.TemporaryDirectory() as temporary, RedisFixture() as fixture:
            folder = Path(temporary)
            output, host = folder / "producer-runtime" / "producer.json", folder / "host-evidence.json"
            original = Path.write_text
            entered, finished = threading.Event(), threading.Event()
            blocked, errors = [], []

            def slow_write(path, *args, **kwargs):
                if path == host:
                    blocked.append(time.monotonic_ns())
                    entered.set()
                    time.sleep(3.8)
                    blocked.append(time.monotonic_ns())
                return original(path, *args, **kwargs)

            def persist():
                try:
                    host.write_text("captured host evidence\n")
                except Exception as error:
                    errors.append(error)
                finally:
                    finished.set()

            with patch.object(Path, "write_text", slow_write):
                writer = threading.Thread(target=persist, daemon=True)
                writer.start()
                self.assertTrue(entered.wait(timeout=1))
                try:
                    self.produce(fixture, output, 4.2)
                finally:
                    writer.join(timeout=5)
            self.assertTrue(finished.is_set())
            self.assertEqual(errors, [])
            self.check_socket_entries(fixture)
            scalars = fixture.entries[::2]
            during = [entry for entry in scalars if blocked[0] <= entry[0] <= blocked[1]]
            self.assertGreaterEqual(len(during), 300)
            self.assertLess(max(b[0] - a[0] for a, b in zip(scalars, scalars[1:])),
                            2 * 10**9)
            self.assertEqual([entry["kind"] for entry in self.ledger(output)], ["start"])
            progress = json.loads(output.read_text())
            self.assertEqual(progress["failed"], 0)
            self.assertGreaterEqual(progress["accepted"], 300)
            self.assertEqual(progress["accepted"], progress["value"])

    def test_real_source_scheduling_delay_remains_recorded_and_fatal(self):
        real_event = threading.Event

        class SlowSourceEvent:
            def __init__(self):
                self.event, self.waits = real_event(), 0

            def is_set(self):
                return self.event.is_set()

            def set(self):
                self.event.set()

            def wait(self, timeout=None):
                self.waits += 1
                if self.waits == 2:
                    time.sleep(2.2)
                return self.event.wait(timeout)

        with tempfile.TemporaryDirectory() as temporary, RedisFixture() as fixture:
            output = Path(temporary) / "producer-runtime" / "producer.json"
            with patch.object(traffic.threading, "Event", SlowSourceEvent):
                with self.assertRaisesRegex(ValueError, "source freshness bound"):
                    self.produce(fixture, output, 4)
            ledger = self.ledger(output)
            self.assertEqual(ledger[-1]["kind"], "stall")
            self.assertGreater(ledger[-1]["end_mono_ns"] - ledger[-1]["start_mono_ns"],
                               2 * 10**9)
            self.check_socket_entries(fixture)
            self.assertEqual(len(fixture.entries), 2)


class ProducerCaptureTests(unittest.TestCase):
    def collector(self, folder, running):
        events = []
        snapshot = json.dumps(dict(pid=7, mono_ns=12345, accepted=2, failed=0, value=2))
        ledger = json.dumps(dict(kind="start", schema=1)) + "\n" + json.dumps(dict(kind="stall"))

        class Process:
            returncode = None if running else 1

            def poll(self):
                return self.returncode

            def wait(self, timeout):
                events.append("wait")
                self.returncode = 0
                return 0

            def kill(self):
                events.append("fallback-kill")
                self.returncode = -9

        class Scope:
            def __init__(self):
                self.folder = folder
                self.containers = {"tools": "owned-tools"}

            def exec(self, role, *argv, **kwargs):
                self.assert_role(role)
                events.append(argv)
                if argv[:2] == ("/bin/kill", "-TERM"):
                    return ""
                if argv[0] != "/bin/cat":
                    raise AssertionError("unexpected helper command")
                if argv[1] == qualification.PRODUCER_DIRECTORY + "/producer.json":
                    return snapshot
                if argv[1] == qualification.PRODUCER_DIRECTORY + "/producer-events.jsonl":
                    return ledger
                raise AssertionError("capture read outside producer tmpfs directory")

            def assert_role(self, role):
                if role != "tools" or role not in self.containers:
                    raise AssertionError("capture reads outside owned tools container")

        collector = qualification.Collector(Scope(), {}, {}, {}, {})
        collector.producer = Process()
        return collector, events, snapshot, ledger

    def test_live_capture_preserves_original_timestamp_without_host_write(self):
        with tempfile.TemporaryDirectory() as temporary:
            collector, _, snapshot, _ = self.collector(Path(temporary), True)
            with patch.object(Path, "write_text", side_effect=AssertionError("host persistence on live read")):
                self.assertEqual(collector.producer_progress(), json.loads(snapshot))
            self.assertFalse((collector.runtime / "producer.json").exists())

    def test_stop_retains_complete_evidence_after_exit_and_is_repeatable(self):
        for running in (True, False):
            with self.subTest(running=running), tempfile.TemporaryDirectory() as temporary:
                collector, events, snapshot, ledger = self.collector(Path(temporary), running)
                collector.stop()
                self.assertEqual((collector.runtime / "producer.json").read_text(), snapshot + "\n")
                self.assertEqual((collector.runtime / "producer-events.jsonl").read_text(), ledger + "\n")
                if running:
                    self.assertLess(events.index("wait"), events.index(("/bin/cat", qualification.PRODUCER_DIRECTORY + "/producer-events.jsonl")))
                else:
                    self.assertNotIn("wait", events)
                # run() closes the tools scope before its unconditional finally
                # calls stop() again. The sealed evidence must suffice then.
                collector.scope.containers.clear()
                commands_before_repeat = list(events)
                collector.stop()
                self.assertEqual(events, commands_before_repeat)
                self.assertEqual((collector.runtime / "producer-events.jsonl").read_text(), ledger + "\n")
                self.assertEqual(sum(event == ("/bin/kill", "-TERM", "7") for event in events), int(running))

    def test_worker_stop_failure_still_stops_source_and_retains_evidence(self):
        class StuckWorker:
            def is_alive(self):
                return True

            def join(self, timeout):
                pass  # Model the bounded join expiring, without waiting 30 s.

        with tempfile.TemporaryDirectory() as temporary:
            collector, events, snapshot, ledger = self.collector(Path(temporary), True)
            collector.worker = StuckWorker()
            with self.assertRaisesRegex(ValueError, "worker failed to stop"):
                collector.stop()
            self.assertIn(("/bin/kill", "-TERM", "7"), events)
            self.assertEqual((collector.runtime / "producer.json").read_text(), snapshot + "\n")
            self.assertEqual((collector.runtime / "producer-events.jsonl").read_text(), ledger + "\n")
            self.assertIsNone(collector.producer)

    def test_missing_initial_progress_still_retains_failure_journal_once(self):
        with tempfile.TemporaryDirectory() as temporary:
            collector, events, _, _ = self.collector(Path(temporary), False)
            journal = (json.dumps(dict(kind="start", schema=1)) + "\n"
                       + json.dumps(dict(kind="failure", attempt=1, accepted=0,
                                         failed=1, error="TimeoutError")))
            original_exec = collector.scope.exec

            def missing_progress(role, *argv, **kwargs):
                collector.scope.assert_role(role)
                if argv == ("/bin/cat", qualification.PRODUCER_DIRECTORY + "/producer.json"):
                    events.append(argv)
                    raise ValueError("producer.json absent after initial failure")
                if argv == ("/bin/cat", qualification.PRODUCER_DIRECTORY + "/producer-events.jsonl"):
                    events.append(argv)
                    return journal
                return original_exec(role, *argv, **kwargs)

            with patch.object(collector.scope, "exec", missing_progress):
                with self.assertRaises(ValueError):
                    collector.stop()
                self.assertFalse((collector.runtime / "producer.json").exists())
                self.assertEqual((collector.runtime / "producer-events.jsonl").read_text(), journal + "\n")
                self.assertIsNone(collector.producer)
                collector.scope.containers.clear()
                commands_before_repeat = list(events)
                collector.stop()
                self.assertEqual(events, commands_before_repeat)

    def test_term_timeout_kills_container_source_before_host_client_and_capture(self):
        with tempfile.TemporaryDirectory() as temporary:
            collector, events, snapshot, ledger = self.collector(Path(temporary), True)
            process = collector.producer
            original_exec = collector.scope.exec
            original_timeout = subprocess.TimeoutExpired("docker exec owned-tools", 5)
            waits = []

            def timeout_then_exit(timeout):
                events.append("wait")
                waits.append(timeout)
                if len(waits) == 1:
                    raise original_timeout
                process.returncode = -9
                return -9

            def allow_container_kill(role, *argv, **kwargs):
                collector.scope.assert_role(role)
                if argv == ("/bin/kill", "-KILL", "7"):
                    events.append(argv)
                    return ""
                return original_exec(role, *argv, **kwargs)

            with patch.object(process, "wait", timeout_then_exit), patch.object(collector.scope, "exec", allow_container_kill):
                with self.assertRaises(subprocess.TimeoutExpired) as raised:
                    collector.stop()
            self.assertIs(raised.exception, original_timeout)
            source_kill = events.index(("/bin/kill", "-KILL", "7"))
            host_kill = events.index("fallback-kill")
            ledger_capture = events.index(("/bin/cat", qualification.PRODUCER_DIRECTORY + "/producer-events.jsonl"))
            self.assertLess(source_kill, host_kill)
            self.assertLess(host_kill, ledger_capture)
            self.assertEqual((collector.runtime / "producer.json").read_text(), snapshot + "\n")
            self.assertEqual((collector.runtime / "producer-events.jsonl").read_text(), ledger + "\n")
            self.assertIsNone(collector.producer)


if __name__ == "__main__":
    unittest.main()
