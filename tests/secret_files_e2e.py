#!/usr/bin/env python3
"""Real Redis authentication, file rotation and failed-input reload preservation."""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ioc", required=True)
    parser.add_argument("--pvxget", required=True)
    args = parser.parse_args()
    redis_port = int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"])
    # This is the test helper's private Redis, never a configured deployment.
    with socket.create_connection(("127.0.0.1", redis_port), timeout=2) as control:
        reply = control.makefile("rb")

        def command(*items):
            items = [str(item).encode() for item in items]
            control.sendall(b"*%d\r\n" % len(items) + b"".join(b"$%d\r\n" % len(item) + item + b"\r\n" for item in items))
            assert reply.readline() == b"+OK\r\n", "fixture ACL command failed"

        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            pva_port = listener.getsockname()[1]
        command("ACL", "SETUSER", "fixture-user", "on", ">fixture-secret-one", "~*", "+@all")
        command("ACL", "SETUSER", "default", "off")
        env = dict(os.environ, EPICS_PVA_AUTO_ADDR_LIST="NO", EPICS_PVA_ADDR_LIST=f"127.0.0.1:{pva_port}")
        with tempfile.TemporaryDirectory(prefix="secret-files-") as directory:
            folder = Path(directory)
            (folder / "user.secret").write_text("fixture-user\n")
            password = folder / "password.secret"
            password.write_bytes(b"fixture-secret-one\r\n")
            config = dict(server=dict(instance="secrets", namespace="TEST", interfaces=["127.0.0.1"],
                                      tcp_port=pva_port, udp_port=pva_port, auto_beacon=False),
                          discovery=dict(enabled=False),
                          redis=dict(host="127.0.0.1", port=redis_port, base_key="secrets",
                                     user_file="user.secret", password_file="password.secret"),
                          pvs=[dict(name="value", type="float64", shape="scalar", read=dict(key="value"), initial=5)])
            path = folder / "config.json"
            path.write_text(json.dumps(config))
            with (folder / "ioc.log").open("w") as log:
                process = subprocess.Popen([args.ioc, "--config", str(path)], stdout=log, stderr=subprocess.STDOUT)
                try:
                    def get(name):
                        return subprocess.check_output([args.pvxget, "-w", "1", name], env=env,
                                                       stderr=subprocess.STDOUT, text=True, timeout=3)

                    def eventually(predicate, message):
                        until = time.monotonic() + 7
                        while time.monotonic() < until:
                            assert process.poll() is None, (folder / "ioc.log").read_text()
                            try:
                                if predicate():
                                    return
                            except subprocess.CalledProcessError:
                                pass
                            time.sleep(.02)
                        raise AssertionError(message)

                    eventually(lambda: '"1/1 connected"' in get("SYS:secrets:backend:health"), "file authentication")
                    assert "value int64_t = 1" in get("SYS:secrets:config:generation")
                    password.write_text("")
                    process.send_signal(signal.SIGHUP)
                    eventually(lambda: "secret file must not be empty" in get("SYS:secrets:config:lastError"), "failed-input reload")
                    assert "value int64_t = 1" in get("SYS:secrets:config:generation")
                    assert '"1/1 connected"' in get("SYS:secrets:backend:health")
                    assert "value double = 5" in get("TEST:value")
                    command("ACL", "SETUSER", "fixture-user", "resetpass", ">fixture-secret-two")
                    replacement = folder / "new.secret"
                    replacement.write_text("fixture-secret-two\n")
                    replacement.replace(password)
                    process.send_signal(signal.SIGHUP)
                    eventually(lambda: "value int64_t = 2" in get("SYS:secrets:config:generation"), "rotation generation")
                    eventually(lambda: '"1/1 connected"' in get("SYS:secrets:backend:health"), "rotated authentication")
                    assert "fixture-secret" not in (folder / "ioc.log").read_text()
                    print("Redis credential files, rotation and failed-file reload preservation passed")
                except Exception:
                    print((folder / "ioc.log").read_text(), file=sys.stderr)
                    raise
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
        reply.close()


if __name__ == "__main__":
    main()
