#!/usr/bin/env python3
"""Required reflection, strict arguments/defaults, limits and failed reload preservation."""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import selectors
import signal
import socket
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


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    for name in ("ioc", "fixture", "pvxget", "pvxcall", "arguments-fixture", "runtime-fixture"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    fixture = subprocess.Popen([args.fixture], stdout=subprocess.PIPE, text=True)
    ioc = None
    with tempfile.TemporaryDirectory(prefix="rpc-hardening-") as directory:
        folder = Path(directory)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(fixture.stdout, selectors.EVENT_READ)
                assert selector.select(10), "reflection fixture startup"
                grpc_port = int(fixture.stdout.readline())
            port = free_port()
            env = dict(os.environ, EPICS_PVA_AUTO_ADDR_LIST="NO", EPICS_PVA_ADDR_LIST=f"127.0.0.1:{port}")
            base = dict(server=dict(instance="rpc-hardening", namespace="RPC", interfaces=["127.0.0.1"],
                                    tcp_port=port, udp_port=port, auto_beacon=False),
                        redis=dict(host="127.0.0.1", port=int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"]), base_key="rpc-test"),
                        discovery=dict(enabled=False),
                        rpc_services=[dict(endpoint=f"127.0.0.1:{grpc_port}", service="redis_pvxs_test.Strict", suffix="_RPC",
                                           defaults=dict(number="7", label="shared", digitizer="default-device"),
                                           method_defaults=dict(Other=dict(label="method-default")),
                                           discovery_timeout_ms=300, timeout_ms=1000)])
            path = folder / "config.json"

            def save(config):
                path.write_text(json.dumps(config))

            def get(name):
                return subprocess.check_output([args.pvxget, "-w", "1", name], text=True,
                                               stderr=subprocess.STDOUT, env=env, timeout=3)

            def eventually(predicate, message):
                deadline = time.monotonic() + 6
                while time.monotonic() < deadline:
                    assert ioc.poll() is None, (folder / "ioc.log").read_text()
                    try:
                        if predicate():
                            return
                    except subprocess.CalledProcessError:
                        pass
                    time.sleep(.02)
                raise AssertionError(message)

            def call(*fields, method="ECHO", error=None):
                result = subprocess.run([args.pvxcall, "-w", "2", f"RPC:{method}_RPC", *fields],
                                        env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=4)
                assert (result.returncode == 0) == (error is None), result.stdout
                if error:
                    assert error in result.stdout, result.stdout
                return result.stdout

            def field(output, name, value):
                assert re.search(r"\b" + re.escape(name) + r"\s*=\s*" + re.escape(str(value)), output), output

            def rejected(config, message):
                save(config)
                result = subprocess.run([args.ioc, "--config", str(path)], text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=6)
                assert result.returncode != 0 and message in result.stdout, result.stdout

            save(base)
            with (folder / "ioc.log").open("w") as log:
                ioc = subprocess.Popen([args.ioc, "--config", str(path)], stdout=log, stderr=subprocess.STDOUT)
                eventually(lambda: "value int64_t = 1" in get("SYS:rpc-hardening:config:generation"), "startup")
                first = call()
                field(first, "number", 7)
                field(first, "calls", 1)
                assert 'digitizer = "default-device"' in first, first
                assert 'label = "method-default"' in call(method="OTHER")
                valid = call("number=-2147483648", "unsigned_number=4294967295", "signed_big=-9223372036854775808",
                             "unsigned_big=18446744073709551615", "ratio=1.5", "fraction=2.25", "flag=yes",
                             "mode=MODE_ONE", "digitizer=caller-device")
                for name, value in dict(number=-2147483648, unsigned_number=4294967295,
                                        signed_big=-9223372036854775808, unsigned_big=18446744073709551615,
                                        ratio=1.5, fraction=2.25, mode=1, calls=2).items():
                    field(valid, name, value)
                assert 'digitizer = "caller-device"' in valid, valid
                for fields, error in [
                    (["unknown=1"], "unknown RPC field"),
                    (["index=1"], "ambiguous RPC field"),
                    (["number=2147483648"], "out-of-range integer"),
                    (["number=12junk"], "invalid or out-of-range integer"),
                    (["unsigned_number=-1"], "negative unsigned integer"),
                    (["unsigned_big=18446744073709551616"], "out-of-range integer"),
                    (["flag=maybe"], "invalid boolean"),
                    (["ratio=inf"], "invalid or out-of-range real"),
                    (["fraction=nan"], "invalid or out-of-range real"),
                    (["mode=NO_SUCH_MODE"], "unknown enum value"),
                    (["mode=999"], "unknown enum value"),
                    (["source=flat"], "singular scalar"),
                ]:
                    call(*fields, error=error)
                field(call(), "calls", 3)  # invalid arguments never reached the backend

                replacement = copy.deepcopy(base)
                replacement["rpc_services"][0]["endpoint"] = f"127.0.0.1:{free_port()}"
                save(replacement)
                ioc.send_signal(signal.SIGHUP)
                eventually(lambda: "required rpc_service" in get("SYS:rpc-hardening:config:lastError"), "required reload rejection")
                assert "value int64_t = 1" in get("SYS:rpc-hardening:config:generation")
                field(call(), "calls", 4)
                subprocess.run([args.arguments_fixture, str(port)], check=True, timeout=10)
                stop(ioc)
                ioc = None

            subprocess.run([args.runtime_fixture, str(grpc_port)], check=True, timeout=30)

            for replacement, error in [
                (dict(defaults=dict(typo="1")), "unknown shared RPC default"),
                (dict(method_defaults=dict(Missing=dict(number="1"))), "unknown RPC method"),
                (dict(method_defaults=dict(Echo=dict(flag="maybe"))), "invalid boolean"),
                (dict(service="redis_pvxs_test.Unsupported", optional=True, defaults={}, method_defaults={}), "recursive or too deeply nested"),
                (dict(endpoint=f"127.0.0.1:{free_port()}"), "required rpc_service"),
            ]:
                candidate = copy.deepcopy(base)
                candidate["rpc_services"][0].update(replacement)
                rejected(candidate, error)

            limited = copy.deepcopy(base)
            limited["limits"] = dict(max_payload_bytes=4096)
            save(limited)
            with (folder / "ioc.log").open("w") as log:
                ioc = subprocess.Popen([args.ioc, "--config", str(path)], stdout=log, stderr=subprocess.STDOUT)
                eventually(lambda: "value int64_t = 1" in get("SYS:rpc-hardening:config:generation"), "limited startup")
                call("text=large", error="decoded RPC reply exceeds payload limit")
                stop(ioc)
                ioc = None

            optional = copy.deepcopy(base)
            optional["rpc_services"][0].update(endpoint=f"127.0.0.1:{free_port()}", optional=True)
            save(optional)
            with (folder / "ioc.log").open("w") as log:
                ioc = subprocess.Popen([args.ioc, "--config", str(path)], stdout=log, stderr=subprocess.STDOUT)
                eventually(lambda: "value int64_t = 1" in get("SYS:rpc-hardening:config:generation"), "explicit optional startup")
                stop(ioc)
                ioc = None
            print("required/optional reflection, strict args/defaults, reply limits and reload preservation passed")
        except Exception:
            if (folder / "ioc.log").exists():
                print((folder / "ioc.log").read_text(), file=sys.stderr)
            raise
        finally:
            stop(ioc)
            stop(fixture)
            fixture.stdout.close()


if __name__ == "__main__":
    main()
