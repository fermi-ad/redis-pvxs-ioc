#!/usr/bin/env python3
"""Optional reflection recovery publishes one validated generation with the active ACF."""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import sys
import tempfile
import time

from rpc_hardening_e2e import free_port, stop


def main():
    parser = argparse.ArgumentParser()
    for name in ("ioc", "fixture", "pvxget", "pvxcall"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    ioc = backend = log = None
    with tempfile.TemporaryDirectory(prefix="rpc-recovery-") as directory:
        folder = Path(directory)
        path = folder / "config.json"
        acf = folder / "access.acf"
        port, rpc_port = free_port(), free_port()
        env = dict(os.environ, EPICS_PVA_AUTO_ADDR_LIST="NO", EPICS_PVA_ADDR_LIST=f"127.0.0.1:{port}")
        base = dict(server=dict(instance="rpc-recovery", namespace="RPC", interfaces=["127.0.0.1"],
                                tcp_port=port, udp_port=port, auto_beacon=False),
                    redis=dict(host="127.0.0.1", port=int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"]), base_key="recovery"),
                    discovery=dict(enabled=True, bind_address="127.0.0.1", udp_port=0),
                    pvs=[dict(name="keep", type="int32", shape="scalar", read=dict(key="keep"), initial=17)],
                    rpc_services=[dict(endpoint=f"127.0.0.1:{rpc_port}", service="redis_pvxs_test.Strict",
                                       suffix="_RPC", optional=True, discovery_timeout_ms=300,
                                       retry_interval_ms=200, timeout_ms=1000)])

        def get(name):
            return subprocess.check_output([args.pvxget, "-w", "1", name], text=True, env=env,
                                           stderr=subprocess.STDOUT, timeout=3)

        def eventually(predicate, message):
            until = time.monotonic() + 7
            while time.monotonic() < until:
                assert ioc.poll() is None, (folder / "ioc.log").read_text()
                try:
                    if predicate():
                        return
                except subprocess.CalledProcessError:
                    pass
                time.sleep(.02)
            raise AssertionError(message)

        def generation(expected):
            return f"value int64_t = {expected}" in get("SYS:rpc-recovery:config:generation")

        def start_ioc(config):
            nonlocal ioc, log
            stop(ioc)
            if log:
                log.close()
            path.write_text(json.dumps(config))
            log = (folder / "ioc.log").open("w")
            ioc = subprocess.Popen([args.ioc, "--config", str(path)], stdout=log, stderr=subprocess.STDOUT)
            eventually(lambda: generation(1), "IOC startup")

        def start_backend(backend_port):
            nonlocal backend
            stop(backend)
            backend = subprocess.Popen([args.fixture, str(backend_port)], stdout=subprocess.PIPE, text=True)
            with selectors.DefaultSelector() as selector:
                selector.register(backend.stdout, selectors.EVENT_READ)
                assert selector.select(5), "late backend startup"
                assert int(backend.stdout.readline()) == backend_port

        def call(name, *fields, succeeds=True):
            result = subprocess.run([args.pvxcall, "-w", ".5", name, *fields], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=2)
            assert (result.returncode == 0) == succeeds, result.stdout
            return result.stdout

        try:
            permitted = "ASG(DEFAULT) { RULE(0, WRITE) }\n"
            acf.write_text(permitted)
            secured = copy.deepcopy(base)
            secured["access"] = dict(enabled=True, file=str(acf), watch=dict(enabled=False))
            start_ioc(secured)
            assert "value int32_t = 17" in get("RPC:keep")
            fingerprint = re.search(r'value string = "([^\"]+)"', get("SYS:rpc-recovery:access:policyFingerprint")).group(1)
            acf.write_text("ASG(DEFAULT) { RULE(0, INVALID) }\n")
            start_backend(rpc_port)
            eventually(lambda: generation(2), "optional service recovery")
            assert 'text = "recovered"' in call("RPC:ECHO_RPC", "text=recovered")
            assert "value int32_t = 17" in get("RPC:keep")
            assert fingerprint in get("SYS:rpc-recovery:access:policyFingerprint")
            assert "value int64_t = 1" in get("SYS:rpc-recovery:access:generation")
            eventually(lambda: "desiredGeneration uint64_t = 2" in get("SYS:rpc-recovery:discovery:status"), "catalog publication")
            assert '"rpc-recovery"' in get("SYS:rpc-recovery:config:reloadStatus")
            stop(backend)
            # An unrelated reload retains the discovered endpoints during outage.
            acf.write_text(permitted)
            secured["pvs"][0]["metadata"] = dict(description="updated during backend outage")
            path.write_text(json.dumps(secured))
            ioc.send_signal(signal.SIGHUP)
            eventually(lambda: generation(3), "unchanged reflected service retained")
            assert "gRPC" in call("RPC:ECHO_RPC", succeeds=False)
            stop(ioc)

            # Recovery must reject the whole reflected service on any collision.
            rpc_port = free_port()
            collision = copy.deepcopy(base)
            collision["rpc_services"][0]["endpoint"] = f"127.0.0.1:{rpc_port}"
            collision["pvs"][0]["name"] = "ECHO_RPC"
            start_ioc(collision)
            start_backend(rpc_port)
            eventually(lambda: "duplicate served PV name" in get("SYS:rpc-recovery:rpc:status"), "collision status")
            assert generation(1)
            assert "value int32_t = 17" in get("RPC:ECHO_RPC")
            call("RPC:OTHER_RPC", succeeds=False)
            assert '"invalid"' in get("SYS:rpc-recovery:rpc:status")
            stop(ioc)
            stop(backend)

            # A completed background attempt from a retired generation cannot publish.
            rpc_port = free_port()
            retiring = copy.deepcopy(base)
            retiring["rpc_services"][0].update(endpoint=f"127.0.0.1:{rpc_port}", discovery_timeout_ms=1500)
            start_ioc(retiring)
            eventually(lambda: '"discovering"' in get("SYS:rpc-recovery:rpc:status"), "background attempt")
            retiring.pop("rpc_services")
            path.write_text(json.dumps(retiring))
            ioc.send_signal(signal.SIGHUP)
            eventually(lambda: generation(2), "service retirement")
            start_backend(rpc_port)
            call("RPC:ECHO_RPC", succeeds=False)
            eventually(lambda: '"disabled"' in get("SYS:rpc-recovery:rpc:status"), "retired service diagnostics")
            assert generation(2) and "value int32_t = 17" in get("RPC:keep")
            print("optional retry, atomic recovery/collision, active ACF, catalog and retired-generation fencing passed")
        except Exception:
            if (folder / "ioc.log").exists():
                print((folder / "ioc.log").read_text(), file=sys.stderr)
            raise
        finally:
            stop(ioc)
            stop(backend)
            if backend and backend.stdout:
                backend.stdout.close()
            if log:
                log.close()


if __name__ == "__main__":
    main()
