#!/usr/bin/env python3
"""Fake XRM -> xrm-stream -> isolated Redis -> external IOC -> PVA."""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


def redis_command(port, *arguments):
    encoded = [arg if isinstance(arg, bytes) else str(arg).encode() for arg in arguments]
    packet = b"*%d\r\n" % len(encoded) + b"".join(
        b"$%d\r\n" % len(arg) + arg + b"\r\n" for arg in encoded)
    with socket.create_connection(("127.0.0.1", port), timeout=2) as client:
        client.sendall(packet)
        with client.makefile("rb") as reply:
            return redis_response(reply)


def redis_response(reply):
    header = reply.readline(4096)
    if not header.endswith(b"\r\n"):
        raise ValueError("truncated Redis response")
    kind, value = header[:1], header[1:-2]
    if kind == b"-":
        raise ValueError("Redis rejected fixture command: " + value.decode(errors="replace"))
    if kind == b"+":
        return value
    if kind == b":":
        return int(value)
    if kind == b"$":
        length = int(value)
        if length == -1:
            return None
        if not 0 <= length <= 1024 * 1024:
            raise ValueError("unexpected Redis response length")
        data = reply.read(length + 2)
        if len(data) != length + 2 or not data.endswith(b"\r\n"):
            raise ValueError("truncated Redis bulk response")
        return data[:-2]
    if kind == b"*":
        length = int(value)
        if length == -1:
            return None
        if not 0 <= length <= 128:
            raise ValueError("unexpected Redis response array")
        return [redis_response(reply) for _ in range(length)]
    raise ValueError("unexpected Redis response type")


def history(port, key):
    entries = redis_command(port, "XRANGE", key, "-", "+")
    return [(cursor, dict(zip(fields[::2], fields[1::2]))) for cursor, fields in entries]


def pva_port():
    # Reserve both protocols while checking a shared port; release before the
    # IOC binds, matching the existing isolated PVA end-to-end fixtures.
    for _ in range(100):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.bind(("127.0.0.1", 0))
            port = udp.getsockname()[1]
            with socket.socket() as tcp:
                try:
                    tcp.bind(("127.0.0.1", port))
                    return port
                except OSError:
                    pass
    raise RuntimeError("unable to reserve a private PVA port")


def parse_value(output):
    array = re.search(r"(?m)^[ \t]*value int32_t\[\] = \{(\d+)\}\[([^\]]*)\]$", output)
    severity = re.search(r"(?m)^[ \t]*alarm\.severity int32_t = (\d+)$", output)
    seconds = re.search(r"(?m)^[ \t]*timeStamp\.secondsPastEpoch int64_t = (\d+)$", output)
    nanos = re.search(r"(?m)^[ \t]*timeStamp\.nanoseconds int32_t = (\d+)$", output)
    if not all((array, severity, seconds, nanos)):
        raise ValueError("unexpected PVA result:\n" + output)
    values = [] if not array.group(2) else [int(part.strip()) for part in array.group(2).split(",")]
    if len(values) != int(array.group(1)):
        raise ValueError("PVA waveform length or output truncation mismatch")
    return values, int(severity.group(1)), int(seconds.group(1)), int(nanos.group(1))


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
    parser.add_argument("--ioc", required=True)
    parser.add_argument("--stream", required=True)
    parser.add_argument("--pvxget", required=True)
    args = parser.parse_args()
    redis_port = int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"])
    base = "xrm-pva-e2e"
    keys = ["{" + base + "}:xrm:CH1", "{" + base + "}:xrm:CH5"]
    expected = [[-131072, 131071, 42, -1], [12345, -12345, 0, 65536]]
    payload = b"".join(struct.pack("<ii", first, second) for first, second in zip(*expected))
    for key in keys:
        if redis_command(redis_port, "XLEN", key) != 0:
            raise RuntimeError("fixture Redis key already exists")

    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(.2)
    source_port = listener.getsockname()[1]
    source_done = threading.Event()
    source_errors = []

    def fake_xrm():
        try:
            client = None
            while not source_done.is_set():
                try:
                    client, _ = listener.accept()
                    break
                except socket.timeout:
                    pass
            if client is None:
                return
            with client:
                # Force arbitrary cuts, including inside individual 4-byte
                # samples and between selected channels.
                for offset in range(0, len(payload), 3):
                    client.sendall(payload[offset:offset + 3])
                    time.sleep(.001)
                source_done.wait(25)
        except Exception as error:
            source_errors.append(error)
        finally:
            listener.close()

    source = threading.Thread(target=fake_xrm, daemon=True)
    source.start()
    port = pva_port()
    environment = dict(os.environ, EPICS_PVA_AUTO_ADDR_LIST="NO",
                       EPICS_PVA_ADDR_LIST=f"127.0.0.1:{port}", EPICS_PVA_NAME_SERVERS="")
    config = dict(server=dict(instance="xrm-pva-e2e", namespace="XRMTEST", interfaces=["127.0.0.1"],
                              tcp_port=port, udp_port=port, auto_beacon=False),
                  discovery=dict(enabled=False),
                  redis=dict(host="127.0.0.1", port=redis_port, base_key=base),
                  pvs=[dict(name=f"CH{channel}", type="int32", shape="array", stream_protocol="xrm_v1",
                            read=dict(key=f"xrm:CH{channel}"),
                            source_health=dict(stale_after_ms=10000)) for channel in (1, 5)])

    with tempfile.TemporaryDirectory(prefix="xrm-pva-e2e-") as temporary:
        folder = Path(temporary)
        config_path, ioc_log, stream_log = folder / "config.json", folder / "ioc.log", folder / "stream.log"
        config_path.write_text(json.dumps(config))
        ioc = producer = None
        with ioc_log.open("w") as ioc_output, stream_log.open("w") as stream_output:
            try:
                ioc = subprocess.Popen([args.ioc, "--config", str(config_path)], stdout=ioc_output,
                                       stderr=subprocess.STDOUT)
                producer = subprocess.Popen([args.stream, "--xrm-host", "127.0.0.1", "--xrm-port", str(source_port),
                    "--redis-host", "127.0.0.1", "--redis-port", str(redis_port), "--base-key", base,
                    "--key-prefix", "xrm", "--channels", "1,5", "--sample-bytes", "4",
                    "--sample-rate-hz", "1000", "--samples-per-batch", "4", "--maxlen", "8"],
                    stdout=stream_output, stderr=subprocess.STDOUT)

                def eventually(predicate, explanation):
                    deadline = time.monotonic() + 15
                    last_error = None
                    while time.monotonic() < deadline:
                        if ioc.poll() is not None or producer.poll() is not None:
                            raise RuntimeError("IOC or XRM companion exited during fixture")
                        if source_errors:
                            raise source_errors[0]
                        try:
                            if predicate():
                                return
                        except (OSError, ValueError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
                            last_error = error
                        time.sleep(.05)
                    raise AssertionError(f"{explanation}; last error: {last_error}")

                def get(channel):
                    output = subprocess.check_output([args.pvxget, "-F", "delta", "-#", "0", "-w", "1",
                                                      f"XRMTEST:CH{channel}"], env=environment,
                                                     stderr=subprocess.STDOUT, text=True, timeout=3)
                    return parse_value(output)

                eventually(lambda: all(len(history(redis_port, key)) == 1 for key in keys),
                           "fake XRM batch did not reach both Redis streams")
                cursors = []
                metadata = []
                for channel, key, values in zip((1, 5), keys, expected):
                    cursor, fields = history(redis_port, key)[0]
                    cursors.append(cursor)
                    metadata.append(fields)
                    assert fields[b"_"] == struct.pack("<4i", *values)
                    assert fields[b"schema"] == b"fermi-ad/xrm-waveform:1"
                    assert fields[b"sample_bytes"] == b"4" and fields[b"sample_count"] == b"4"
                    assert fields[b"sample_rate_hz"] == b"1000" and int(fields[b"channel"]) == channel
                    assert fields[b"session"] and int(fields[b"connection"]) > 0 and int(fields[b"batch"]) > 0
                    assert fields[b"source_validity"] == b"unknown" and fields[b"acq_time_valid"] == b"0"
                    assert b"acq_time_ns" not in fields
                    eventually(lambda: get(channel) == (values, 3, 0, 0),
                               f"CH{channel} did not expose the exact waveform with invalid unknown time")

                def inject(values, stamp, batch):
                    cursor = redis_command(redis_port, "XADD", keys[0], "*", "_", struct.pack("<4i", *values),
                        "schema", "fermi-ad/xrm-waveform:1", "sample_bytes", "4", "sample_count", "4",
                        "sample_rate_hz", "1000", "channel", "1", "session", metadata[0][b"session"],
                        "connection", metadata[0][b"connection"], "batch", str(batch),
                        "source_validity", "valid", "acq_time_valid", "1", "acq_time_ns", str(stamp))
                    if not isinstance(cursor, bytes):
                        raise ValueError("unexpected injected Redis cursor")
                    return cursor

                first_values = [-123, 456, 32768, -32769]
                first_cursor = inject(first_values, 1700000000123456789, int(metadata[0][b"batch"]) + 1)
                assert tuple(map(int, first_cursor.split(b"-"))) > tuple(map(int, cursors[0].split(b"-")))
                eventually(lambda: get(1) == (first_values, 0, 1700000000, 123456789),
                           "PVA did not preserve the first verified acquisition timestamp")

                second_values = [17, -18, 19, -20]
                second_cursor = inject(second_values, 1600000000000000042, int(metadata[0][b"batch"]) + 2)
                assert tuple(map(int, second_cursor.split(b"-"))) > tuple(map(int, first_cursor.split(b"-")))
                eventually(lambda: get(1) == (second_values, 0, 1600000000, 42),
                           "PVA did not preserve a backwards acquisition timestamp")
                assert get(5) == (expected[1], 3, 0, 0)
                print("fake XRM -> Redis -> IOC -> PVA: exact 32-bit waveforms, unknown-time alarm, "
                      "verified timestamp and backwards time passed")
            except Exception:
                ioc_output.flush(); stream_output.flush()
                print("IOC log:\n" + ioc_log.read_text(), file=sys.stderr)
                print("XRM companion log:\n" + stream_log.read_text(), file=sys.stderr)
                raise
            finally:
                stop(producer)
                stop(ioc)
                source_done.set()
                source.join(timeout=5)
                if source.is_alive():
                    listener.close()
                    raise RuntimeError("fake XRM server did not stop")
                if source_errors and sys.exc_info()[0] is None:
                    raise source_errors[0]


if __name__ == "__main__":
    main()
