#!/usr/bin/env python3
"""Own a second private Redis process for the PVA partial-outage regression."""
import argparse
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--redis-server", required=True)
    parser.add_argument("--fixture", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="redis-pvxs-health-") as directory:
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        path = Path(directory) / "redis.log"
        with path.open("w") as log:
            redis = subprocess.Popen([args.redis_server, "--bind", "127.0.0.1", "--port", str(port),
                                      "--save", "", "--appendonly", "no", "--dir", directory],
                                     stdout=log, stderr=subprocess.STDOUT)
            try:
                for _ in range(100):
                    assert redis.poll() is None, path.read_text()
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=.1) as client:
                            client.sendall(b"*1\r\n$4\r\nPING\r\n")
                            if client.recv(64).startswith(b"+PONG"):
                                break
                    except OSError:
                        pass
                    time.sleep(.05)
                else:
                    raise AssertionError("second Redis did not start")
                result = subprocess.run([args.fixture], timeout=55,
                                        env=dict(os.environ, REDIS_PVXS_SECOND_TEST_PORT=str(port),
                                                 REDIS_PVXS_SECOND_TEST_PID=str(redis.pid)))
                if result.returncode:
                    print(path.read_text(), flush=True)
                    raise SystemExit(result.returncode)
            finally:
                if redis.poll() is None:
                    redis.send_signal(signal.SIGCONT)
                    redis.terminate()
                    try:
                        redis.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        redis.kill(); redis.wait(timeout=5)


if __name__ == "__main__":
    main()
