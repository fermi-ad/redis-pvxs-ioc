"""Local fake XRM + private Redis tests; does not connect to an instrument."""
import os
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest


TOOL = sys.argv.pop(1)
REDIS_PORT = int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"])


def command(sock, *args):
    parts = [x if isinstance(x, bytes) else str(x).encode() for x in args]
    sock.sendall(b"*%d\r\n" % len(parts) + b"".join(b"$%d\r\n" % len(x) + x + b"\r\n" for x in parts))


def response(stream):
    line = stream.readline()
    if not line.endswith(b"\r\n"):
        raise ValueError("truncated RESP response")
    kind, value = line[:1], line[1:-2]
    if kind in (b"+", b"-"):
        return kind + value
    if kind == b":":
        return int(value)
    if kind == b"$":
        length = int(value)
        if length < 0:
            return None
        data = stream.read(length + 2)
        if len(data) != length + 2 or data[-2:] != b"\r\n":
            raise ValueError("truncated RESP bulk")
        return data[:-2]
    if kind == b"*":
        length = int(value)
        return None if length < 0 else [response(stream) for _ in range(length)]
    raise ValueError("unexpected RESP response")


def entries(key):
    with socket.create_connection(("127.0.0.1", REDIS_PORT), timeout=2) as sock, sock.makefile("rb") as stream:
        command(sock, "XRANGE", key, "-", "+")
        return [(ident, dict(zip(fields[::2], fields[1::2]))) for ident, fields in response(stream)]


def tool_args(xrm_port, redis_port=REDIS_PORT):
    return [TOOL, "--xrm-host", "127.0.0.1", "--xrm-port", str(xrm_port),
            "--redis-host", "127.0.0.1", "--redis-port", str(redis_port),
            "--base-key", "xrm-test", "--channels", "1,5", "--sample-bytes", "4",
            "--sample-rate-hz", "1000", "--samples-per-batch", "2", "--maxlen", "2",
            "--duration-ms", "1500"]


class Listener:
    def __init__(self, handler):
        self.socket = socket.socket()
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.listen(4)
        self.socket.settimeout(5)
        self.port = self.socket.getsockname()[1]
        self.errors = []
        def run():
            try:
                handler(self.socket)
            except Exception as error:
                self.errors.append(error)
            finally:
                self.socket.close()
        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()

    def finish(self):
        self.thread.join(timeout=6)
        if self.thread.is_alive():
            raise AssertionError("fixture did not stop")
        if self.errors:
            raise self.errors[0]


class StreamTests(unittest.TestCase):
    def test_split_frames_reconnect_and_bounded_redis_history(self):
        payload = b"".join(struct.pack("<II", n * 10 + 1, n * 10 + 5) for n in range(6))
        def source(listener):
            with listener.accept()[0] as first:
                first.sendall(b"abcde")  # incomplete batch must never contaminate the next connection
                time.sleep(.05)
            with listener.accept()[0] as second:
                for offset in range(0, len(payload), 3):
                    second.sendall(payload[offset:offset + 3])
                    time.sleep(.001)
        fixture = Listener(source)
        result = subprocess.run(tool_args(fixture.port), capture_output=True, text=True, timeout=8)
        fixture.finish()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("discarded 5 partial batch bytes", result.stderr)
        self.assertIn("published=3", result.stderr)
        streams = [entries("{xrm-test}:xrm:CH1"), entries("{xrm-test}:xrm:CH5")]
        self.assertEqual([len(stream) for stream in streams], [2, 2])
        for channel, stream in zip((1, 5), streams):
            previous = (0, 0)
            for batch, (cursor, fields) in enumerate(stream, 2):
                parts = tuple(map(int, cursor.split(b"-")))
                self.assertGreater(parts, previous)
                previous = parts
                self.assertEqual(fields[b"schema"], b"fermi-ad/xrm-waveform:1")
                self.assertEqual(fields[b"sample_bytes"], b"4")
                self.assertEqual(fields[b"acq_time_valid"], b"0")
                self.assertNotIn(b"acq_time_ns", fields)
                self.assertEqual(fields[b"source_validity"], b"unknown")
                self.assertEqual(fields[b"connection"], b"2")
                self.assertEqual(fields[b"batch"], str(batch).encode())
                values = (channel + (batch - 1) * 20, channel + (batch - 1) * 20 + 10)
                self.assertEqual(fields[b"_"], struct.pack("<II", *values))
        self.assertEqual([x[1][b"session"] for x in streams[0]], [x[1][b"session"] for x in streams[1]])

    def test_32_channel_synthetic_transport(self):
        finished = threading.Event()
        # 8 MiB across 32 channels, delivered as 32 independently published batches.
        per_sample = b"".join(struct.pack("<I", channel) for channel in range(1, 33))
        batch = per_sample * 2048
        def source(listener):
            with listener.accept()[0] as conn:
                for _ in range(32):
                    conn.sendall(batch)
                finished.wait(12)
        fixture = Listener(source)
        args = tool_args(fixture.port)
        args[args.index("xrm-test")] = "xrm-throughput"
        args[args.index("1,5")] = ",".join(map(str, range(1, 33)))
        args[args.index("2", args.index("--samples-per-batch"))] = "2048"
        args[args.index("2", args.index("--maxlen"))] = "64"
        args[args.index("1500")] = "15000"
        started = time.monotonic()
        process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            with socket.create_connection(("127.0.0.1", REDIS_PORT), timeout=2) as redis, redis.makefile("rb") as stream:
                while time.monotonic() - started < 12:
                    command(redis, "XLEN", "{xrm-throughput}:xrm:CH32")
                    if response(stream) == 32:
                        break
                    if process.poll() is not None:
                        raise AssertionError("stream companion exited before final batch")
                    time.sleep(.02)
                else:
                    raise AssertionError("32-channel fixture did not reach the final batch")
            elapsed = time.monotonic() - started
            for channel in range(1, 33):
                history = entries("{xrm-throughput}:xrm:CH%d" % channel)
                self.assertEqual(len(history), 32)
                self.assertEqual(history[-1][1][b"_"], struct.pack("<I", channel) * 2048)
                self.assertEqual(history[-1][1][b"batch"], b"32")
            print("synthetic TCP-to-Redis 32-channel throughput: %.1f MiB/s" % (8 / elapsed))
        finally:
            finished.set()
            process.terminate()
            _, stderr = process.communicate(timeout=5)
            fixture.finish()
        self.assertEqual(process.returncode, 0, stderr)
        self.assertIn("published=32", stderr)

    def test_ambiguous_redis_transaction_is_not_replayed(self):
        transactions = []
        second_connection = threading.Event()
        def redis(listener):
            for number in (1, 2):
                with listener.accept()[0] as conn, conn.makefile("rb") as stream:
                    commands = [response(stream) for _ in range(4)]
                    self.assertEqual(commands[0], [b"MULTI"])
                    self.assertEqual(commands[-1], [b"EXEC"])
                    transactions.append(commands[1:3])
                    if number == 1:
                        continue  # transaction may have committed; never replay it
                    conn.sendall(b"+OK\r\n+QUEUED\r\n+QUEUED\r\n*2\r\n$3\r\n1-0\r\n$3\r\n1-0\r\n")
                    second_connection.set()
                    time.sleep(.6)
        def source(listener):
            with listener.accept()[0] as first:
                first.sendall(struct.pack("<IIII", 1, 5, 11, 15))
                first.settimeout(3)
                while first.recv(100):
                    pass
            with listener.accept()[0] as second:
                second.sendall(struct.pack("<IIII", 21, 25, 31, 35))
                second_connection.wait(3)
                time.sleep(.6)
        redis_fixture, source_fixture = Listener(redis), Listener(source)
        result = subprocess.run(tool_args(source_fixture.port, redis_fixture.port),
                                capture_output=True, text=True, timeout=8)
        source_fixture.finish(); redis_fixture.finish()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(transactions), 2)
        self.assertIn("failed_or_ambiguous=1", result.stderr)
        fields = []
        for transaction in transactions:
            for xadd in transaction:
                self.assertEqual(xadd[:6], [b"XADD", xadd[1], b"MAXLEN", b"=", b"2", b"*"])
                fields.append(dict(zip(xadd[6::2], xadd[7::2])))
        self.assertEqual([x[b"batch"] for x in fields], [b"1", b"1", b"2", b"2"])
        self.assertEqual([x[b"connection"] for x in fields], [b"1", b"1", b"2", b"2"])
        self.assertNotEqual(fields[0][b"_"], fields[2][b"_"])

    def test_generated_cursor_follows_a_future_stream_id(self):
        # Model the Redis wall clock moving behind an existing stream ID.
        with socket.create_connection(("127.0.0.1", REDIS_PORT), timeout=2) as redis, redis.makefile("rb") as stream:
            command(redis, "XADD", "{xrm-backward}:xrm:CH1", "9999999999999-0", "_", "old")
            self.assertEqual(response(stream), b"9999999999999-0")
        def source(listener):
            with listener.accept()[0] as conn:
                conn.sendall(struct.pack("<IIII", 1, 5, 11, 15))
                time.sleep(.6)
        fixture = Listener(source)
        args = tool_args(fixture.port)
        args[args.index("xrm-test")] = "xrm-backward"
        args[args.index("1500")] = "700"
        result = subprocess.run(args, capture_output=True, text=True, timeout=5)
        fixture.finish()
        self.assertEqual(result.returncode, 0, result.stderr)
        history = entries("{xrm-backward}:xrm:CH1")
        self.assertEqual(len(history), 2)
        self.assertEqual(history[1][0], b"9999999999999-1")
        self.assertEqual(history[1][1][b"acq_time_valid"], b"0")

    def test_invalid_configuration_rejected_before_connect(self):
        args = tool_args(1)
        args[args.index("1,5")] = "5,1"
        result = subprocess.run(args, capture_output=True, text=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("channels must be unique, ascending", result.stderr)

    def test_stalled_redis_response_respects_duration_and_stop(self):
        for stop_by_signal in (False, True):
            with self.subTest(stop_by_signal=stop_by_signal):
                stalled = threading.Event()
                finished = threading.Event()
                def redis(listener):
                    with listener.accept()[0] as conn, conn.makefile("rb") as stream:
                        self.assertEqual(response(stream), [b"MULTI"])
                        self.assertEqual(response(stream)[0], b"XADD")
                        self.assertEqual(response(stream)[0], b"XADD")
                        self.assertEqual(response(stream), [b"EXEC"])
                        stalled.set()
                        # No response. The companion must close the socket at
                        # its deadline or after receiving the stop signal.
                        conn.settimeout(4)
                        self.assertEqual(conn.recv(1), b"")
                def source(listener):
                    with listener.accept()[0] as conn:
                        conn.sendall(struct.pack("<IIII", 1, 5, 11, 15))
                        finished.wait(4)
                redis_fixture, source_fixture = Listener(redis), Listener(source)
                args = tool_args(source_fixture.port, redis_fixture.port)
                args[args.index("1500")] = "15000" if stop_by_signal else "1000"
                started = time.monotonic()
                process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    self.assertTrue(stalled.wait(3), "Redis fixture did not receive transaction")
                    if stop_by_signal:
                        started = time.monotonic()
                        process.send_signal(signal.SIGTERM)
                    _, stderr = process.communicate(timeout=3)
                    elapsed = time.monotonic() - started
                    self.assertEqual(process.returncode, 0, stderr)
                    self.assertLess(elapsed, 1.75 if not stop_by_signal else 1.0, stderr)
                    self.assertIn("failed_or_ambiguous=1", stderr)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=3)
                    finished.set()
                    source_fixture.finish()
                    redis_fixture.finish()


if __name__ == "__main__":
    unittest.main()
