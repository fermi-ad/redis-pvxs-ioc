#!/usr/bin/env python3
"""Bounded traffic/cursor capture inside the workflow-owned tools container."""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import struct
import threading
import time


class Redis:
    def __init__(self, host, timeout=2, port=6379):
        self.socket = socket.create_connection((host, port), timeout=timeout)
        self.stream = self.socket.makefile("rb")

    def close(self):
        self.stream.close()
        self.socket.close()

    def command(self, *args):
        encoded = [a if isinstance(a, bytes) else str(a).encode() for a in args]
        self.socket.sendall(b"*" + str(len(args)).encode() + b"\r\n" + b"".join(
            b"$" + str(len(a)).encode() + b"\r\n" + a + b"\r\n" for a in encoded))
        return self.response()

    def response(self):
        line = self.stream.readline(4096)
        if not line.endswith(b"\r\n"):
            raise ValueError("truncated or excessive Redis response")
        kind, value = line[:1], line[1:-2]
        if kind == b"-":
            raise ValueError("Redis command rejected: " + value.decode(errors="replace"))
        if kind == b"+":
            return value
        if kind == b":":
            return int(value)
        if kind == b"$":
            length = int(value)
            if length == -1:
                return None
            if not 0 <= length <= 4 * 1024**2:
                raise ValueError("excessive Redis bulk response")
            data = self.stream.read(length + 2)
            if len(data) != length + 2 or not data.endswith(b"\r\n"):
                raise ValueError("truncated Redis bulk response")
            return data[:-2]
        if kind == b"*":
            length = int(value)
            if length == -1:
                return None
            if not 0 <= length <= 4096:
                raise ValueError("excessive Redis array response")
            return [self.response() for _ in range(length)]
        raise ValueError("unexpected Redis response")


def cursor(nanos):
    return f"{nanos // 1000000}-{nanos % 1000000}"


def verify_content(host, base, observation, port=6379):
    redis = Redis(host, port=port)
    try:
        cursors = {}
        for kind, key, count in (("scalar", "scalar", 1), ("alias", "scalar", 1), ("array", "array", 4096)):
            stream_id = cursor(observation[kind + "_time_ns"])
            entries = redis.command("XRANGE", "{" + base + "}:" + key, stream_id, stream_id, "COUNT", 1)
            if len(entries) != 1 or entries[0][0].decode() != stream_id:
                raise ValueError("PVA source cursor missing from private Redis history")
            fields = dict(zip(entries[0][1][::2], entries[0][1][1::2]))
            expected = struct.pack("<" + str(count) + "I", *range(observation[kind], observation[kind] + count))
            if fields.get(b"_") != expected:
                raise ValueError("PVA content/alias does not match exact Redis entry")
            cursors[kind] = stream_id
        observation.update(source_match=True, source_cursors=cursors)
        return observation
    finally:
        redis.close()


def produce(host, base, output, duration, port=6379):
    if output.exists():
        raise ValueError("producer progress path exists")
    output.parent.mkdir(parents=True, exist_ok=True)
    stop = threading.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: stop.set())
    start = time.monotonic()
    accepted, failed, value, last_flush, redis = 0, 0, 0, 0, None
    schedule_start_ns, last_attempt_ns, max_late_ms = time.monotonic_ns(), 0, 0
    journal = output.with_name("producer-events.jsonl").open("x")
    journal.write(json.dumps(dict(kind="start", schema=1, schedule_start_mono_ns=schedule_start_ns,
                                 rate_hz=100, socket_timeout_ms=1000)) + "\n")
    journal.flush()
    records, last_attempt_end = 0, None
    def event(value):
        nonlocal records
        records += 1
        if records > 8192 or journal.tell() > 8 * 1024**2:
            raise ValueError("producer failure/stall evidence exceeds bound")
        journal.write(json.dumps(value) + "\n")
        journal.flush()
    try:
        while not stop.is_set() and time.monotonic() - start < duration:
            due = start + value / 100
            stop.wait(max(0, due - time.monotonic()))
            if stop.is_set():
                break
            value += 1
            last_attempt_ns = time.monotonic_ns()
            if last_attempt_end and last_attempt_ns - last_attempt_end > 2 * 10**9:
                event(dict(kind="stall", start_mono_ns=last_attempt_end, end_mono_ns=last_attempt_ns, attempt=value))
            max_late_ms = max(max_late_ms, (last_attempt_ns - (schedule_start_ns + (value - 1) * 10000000)) / 1e6)
            try:
                if redis is None:
                    redis = Redis(host, timeout=1, port=port)
                stamp = cursor(time.time_ns())
                for key, count in (("scalar", 1), ("array", 4096)):
                    payload = struct.pack("<" + str(count) + "I", *range(value, value + count))
                    response = redis.command("XADD", "{" + base + "}:" + key, "MAXLEN", 1024, stamp, "_", payload)
                    if response.decode() != stamp:
                        raise ValueError("Redis changed explicit acquisition cursor")
                accepted += 1
            except (OSError, ValueError) as error:
                failed += 1
                event(dict(kind="failure", start_mono_ns=last_attempt_ns, end_mono_ns=time.monotonic_ns(),
                           attempt=value, accepted=accepted, failed=failed, error=type(error).__name__))
                if redis:
                    redis.close()
                    redis = None
                # Each attempt gets a new counter/cursor. An ambiguous write is
                # never retried, including after the controlled outage.
            last_attempt_end = time.monotonic_ns()
            if last_attempt_end - last_attempt_ns > 4 * 10**9:
                event(dict(kind="unbounded-request", start_mono_ns=last_attempt_ns, end_mono_ns=last_attempt_end, attempt=value))
            if time.monotonic() - last_flush >= 0.5 or stop.is_set():
                temporary = output.with_suffix(".tmp")
                temporary.write_text(json.dumps(dict(pid=os.getpid(), pid_alive=True, mono_ns=time.monotonic_ns(),
                    accepted=accepted, failed=failed, value=value, rate_hz=100, array_elements=4096,
                    schedule_start_mono_ns=schedule_start_ns, last_attempt_mono_ns=last_attempt_ns,
                    producer_max_late_ms=max_late_ms)) + "\n")
                temporary.replace(output)
                last_flush = time.monotonic()
    finally:
        journal.close()
        if redis:
            redis.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["producer", "content", "delay"])
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, default=6379)
    parser.add_argument("--base", default="soak")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--duration", type=int, default=90000)
    parser.add_argument("--observation")
    args = parser.parse_args()
    if not 1 <= args.duration <= 93600:
        parser.error("duration outside bounded qualification window")
    if not 1 <= args.port <= 65535:
        parser.error("Redis port outside valid bounds")
    if args.mode == "producer":
        produce(args.host, args.base, args.output, args.duration, args.port)
    elif args.mode == "content":
        print(json.dumps(verify_content(args.host, args.base, json.loads(args.observation), args.port)))
    else:
        redis = Redis(args.host, port=args.port)
        try:
            redis.command("CLIENT", "PAUSE", 250, "ALL")
            start = time.monotonic()
            redis.command("PING")
            print(json.dumps(dict(pause_ms=250, measured_redis_delay_ms=(time.monotonic() - start) * 1000)))
        finally:
            redis.close()


if __name__ == "__main__":
    main()
