#!/usr/bin/env python3
"""Bounded loopback qualification of native discovery -> manifest -> IOC watcher.

Owns only subprocesses, temporary files and loopback sockets. This is an
integration regression, not device, deployment, long soak or throughput evidence.
"""
import argparse
import contextlib
import json
import os
from pathlib import Path
import selectors
import signal
import socket
import subprocess
import tempfile
import time


PREFIX = "QUAL:"
ALIASES = [
    ("BASE", "ACQ2206-097:0:MODEL", "carrier:MODEL"),
    ("BASE", "ACQ2206-097:0:SERIAL", "carrier:SERIAL"),
    ("BASE", "ACQ2206-097:1:NCHAN", "module1:NCHAN"),
    ("BASE", "ACQ2206-097:2:NCHAN", "module2:NCHAN"),
    ("BASE", "ACQ2206-097:0:WR:TIME_VALID", "timing:WR:TIME_VALID"),
    ("APP", "acq2206_597:SOE_HLD:TAB:Tbl", "application:SOE_HLD:TAB:Tbl"),
    ("APP", "acq2206_597:FMT:TAB:Tbl", "application:FMT:TAB:Tbl"),
]


def require(condition, text):
    if not condition:
        raise AssertionError(text)


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def eventually(predicate, description, timeout=10):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            value = predicate()
            if value:
                return value
        except (AssertionError, OSError, subprocess.SubprocessError, ValueError) as error:
            last = error
        time.sleep(.05)
    raise AssertionError(f"{description} timed out; last error: {last}")


class NativeServer:
    def __init__(self, fixture, role, port, directory, extra):
        self.log_path = directory / f"native-{role}.log"
        self.log = self.log_path.open("w+")
        self.process = subprocess.Popen(
            [fixture, "--serve", "--role", role, "--udp-port", str(port), "--extra", str(extra)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log, text=True,
            env=dict(os.environ, EPICS_PVA_AUTO_ADDR_LIST="NO", EPICS_PVA_NAME_SERVERS=""))
        self.info = self.read()
        require(self.info["role"] == role, "native role startup mismatch")

    def read(self):
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            require(selector.select(8), "native fixture command timed out: " + self.log_path.read_text())
        line = self.process.stdout.readline()
        require(line, "native fixture exited: " + self.log_path.read_text())
        return json.loads(line)

    def command(self, command):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        return self.read()

    def stats(self):
        return self.command("stats")["pvs"]

    def close(self):
        if self.process.poll() is None:
            try:
                self.process.stdin.write("quit\n")
                self.process.stdin.flush()
                self.process.wait(timeout=4)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait(timeout=4)
        self.process.stdin.close()
        self.process.stdout.close()
        self.log.close()
        require(self.process.returncode == 0, "native fixture did not shut down cleanly: " + self.log_path.read_text())


@contextlib.contextmanager
def subprocess_log(command, log_path):
    with log_path.open("w+") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log)
        try:
            yield process
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


def total(stats, key):
    return sum(entry[key] for entry in stats.values())


def delta(before, after, key):
    return total(after, key) - total(before, key)


class Qualification:
    def __init__(self, args, directory, redis_port):
        self.args, self.directory, self.redis_port = args, directory, redis_port
        self.discovery_port = free_port(socket.SOCK_DGRAM)
        self.manifest = directory / "gateway.yaml"
        self.compat = directory / "compat.json"
        self.cache = directory / "last-good.yaml"
        self.compat.write_text(json.dumps({"schema_version": 1, "aliases": [
            {"role": role, "upstream": upstream, "aliases": ["SL:INST:ICM:XRM:" + alias]}
            for role, upstream, alias in ALIASES]}))
        self.base = self.app = None

    def close(self):
        if self.base:
            self.base.close()
        if self.app:
            self.app.close()

    def sidecar(self, success=True):
        result = subprocess.run([
            self.args.sidecar, "--ip", "127.0.0.1", "--discovery-port", str(self.discovery_port),
            "--manifest", str(self.manifest), "--prefix", PREFIX, "--compat-aliases", str(self.compat),
            "--once", "--discovery-timeout-ms", "500", "--request-timeout-ms", "1500"],
            capture_output=True, text=True, timeout=8,
            env=dict(os.environ, EPICS_PVA_AUTO_ADDR_LIST="YES", EPICS_PVA_ADDR_LIST="192.0.2.99:5076",
                     EPICS_PVA_NAME_SERVERS="192.0.2.99:5075"))
        require((result.returncode == 0) == success,
                f"sidecar returned {result.returncode}: {result.stdout}\n{result.stderr}")
        return result

    def probe(self, mode, *extra, timeout=25):
        result = subprocess.run([self.args.fixture, "--client", mode, "--endpoint", self.endpoint,
                                 "--prefix", PREFIX, *extra], capture_output=True, text=True, timeout=timeout)
        require(result.returncode == 0, f"{mode}: {result.stdout}\n{result.stderr}")
        return json.loads(result.stdout)

    def status(self):
        return self.probe("status", timeout=8)

    def all_stats(self):
        return {**{"BASE:" + name: value for name, value in self.base.stats().items()},
                **{"APP:" + name: value for name, value in self.app.stats().items()}}

    def status_active(self, count, minimum=1):
        value = self.status()
        require(value["pvCount"] == count and value["aliasCount"] == 7,
                "status count mismatch: " + repr(value))
        require(value["generation"] >= minimum and value["activeRevision"], "manifest is not active")
        expected = {(PREFIX + role + ":" + name, name, "127.0.0.1:" + str(native.info["tcp"]))
                    for role, native in (("BASE", self.base), ("APP", self.app))
                    for name in native.stats()}
        actual = {(source["name"], source["upstream"], source["server"]) for source in value["sources"]}
        require(actual == expected, "status lost, duplicated or mislabeled native sources")
        return value

    def configuration(self, policy, port):
        config = dict(
            server=dict(instance="xrm-integration", namespace="", interfaces=["127.0.0.1"],
                        tcp_port=port, udp_port=port, auto_beacon=False),
            redis=dict(base_key="xrm-integration", host="127.0.0.1", port=self.redis_port),
            discovery=dict(enabled=False),
            pva_gateway=dict(file=str(self.manifest), cache_file=str(self.cache),
                             watch=dict(enabled=True, interval_ms=100, settle_ms=0)))
        if policy != "off":
            config["access"] = dict(enabled=True)
            if policy in ("write", "exception"):
                acf = self.directory / f"{policy}.acf"
                acf.write_text("ASG(DEFAULT) { RULE(0, " + ("WRITE" if policy == "write" else "READ") +
                               ") }\nASG(WRITEABLE) { RULE(0, WRITE) }\n")
                config["access"]["file"] = str(acf)
                if policy == "exception":
                    config["pva_gateway"]["access_overrides"] = {
                        PREFIX + "BASE:CONTROL": dict(asg="WRITEABLE", asl=0)}
        return config

    def run(self):
        self.base = NativeServer(self.args.fixture, "BASE", self.discovery_port, self.directory, self.args.extra)
        self.app = NativeServer(self.args.fixture, "APP", self.discovery_port, self.directory, self.args.extra)
        count = self.base.info["count"] + self.app.info["count"]
        self.sidecar()
        before = self.all_stats()
        require(total(before, "gets") == total(before, "mutations") == total(before, "subscriptions") == 0,
                "discovery executed a native read, mutation or monitor")
        initial = self.manifest.read_bytes()
        inode = self.manifest.stat().st_ino
        self.sidecar()
        require(self.manifest.read_bytes() == initial and self.manifest.stat().st_ino == inode,
                "unchanged native catalog replaced the manifest")

        for policy in ("off", "default", "write", "exception"):
            self.endpoint = "127.0.0.1:" + str(free_port())
            path = self.directory / f"ioc-{policy}.json"
            path.write_text(json.dumps(self.configuration(policy, int(self.endpoint.rsplit(":", 1)[1]))))
            with subprocess_log([self.args.ioc, "--config", str(path)], self.directory / f"ioc-{policy}.log") as ioc:
                active = eventually(lambda: self.status_active(count), f"IOC {policy} startup")
                require(ioc.poll() is None, f"IOC {policy} exited")
                baseline = self.all_stats()
                require(total(baseline, "active") == 0, "idle IOC opened upstream monitors")
                if policy == "off":
                    require(total(baseline, "gets") == total(baseline, "mutations") == total(baseline, "subscriptions") == 0,
                            "full catalog IOC warmup executed a native GET, mutation or monitor")
                    print(f"full catalog idle: {count} native PVs, {len(active['sources'])} source labels,0 GETs,0 mutations,0 monitors",
                          flush=True)
                self.probe("observe", "--aliases")
                observed = self.all_stats()
                require(delta(baseline, observed, "gets") == 22,
                        f"GET forwarding used {delta(baseline, observed, 'gets')} upstream executions; expected22")
                require(delta(baseline, observed, "mutations") == delta(baseline, observed, "subscriptions") == 0,
                        "read/type/alias probing created a mutation or monitor")
                require(observed["APP:NESTED"]["gets"] == baseline["APP:NESTED"]["gets"],
                        "GET_FIELD performed an upstream GET")
                self.probe("monitor")
                eventually(lambda: total(self.all_stats(), "active") == 0, "upstream monitor cleanup")
                writable = policy != "default"
                before_ops = self.all_stats()
                self.probe("operations", "--writable", "yes" if writable else "no",
                           *(["--exception"] if policy == "exception" else []), timeout=40)
                after_ops = self.all_stats()
                require(delta(before_ops, after_ops, "gets") == 0, "mutation forwarding performed an extra GET")
                require(delta(before_ops, after_ops, "mutations") == (6 if writable else 0),
                        "mutation count changed or denied operation reached source")
                if policy == "off":
                    self.exercise_load(ioc)
                    self.exercise_changes(count, active)
                elif policy in ("write", "exception"):
                    self.exercise_statuses()
                print(f"native sidecar/IOC {policy}: {count} exact source labels,7 aliases, type/value/request/monitor and operations passed",
                      flush=True)
            require(ioc.returncode == 0, f"IOC {policy} did not shut down cleanly")

    def exercise_load(self, ioc):
        require(self.args.extra >= 160, "global monitor qualification requires at least160 extra PVs per role")
        before = self.all_stats()
        idle_rss = subprocess.check_output(["ps", "-o", "rss=", "-p", str(ioc.pid)], text=True).strip()
        log_path = self.directory / "load.log"
        with log_path.open("w+") as log:
            client = subprocess.Popen([self.args.fixture, "--client", "load", "--endpoint", self.endpoint,
                                       "--prefix", PREFIX, "--monitors", "320"],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True)
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(client.stdout, selectors.EVENT_READ)
                    require(selector.select(18), "global monitor admission timed out")
                line = client.stdout.readline()
                if not line:
                    native, telemetry = self.all_stats(), self.status()
                    summary = {field: sum(source[field] for source in telemetry["sources"])
                               for field in ("monitors", "channels", "inFlight", "rejected")}
                    raise AssertionError("global monitor client exited before admission report: " + log_path.read_text() +
                                         " native active=" + str(total(native, "active")) +
                                         " native subscriptions=" + str(delta(before, native, "subscriptions")) +
                                         " telemetry=" + repr(summary))
                result = json.loads(line)
                require(result["accepted"] == 256 and result["rejected"] == 64,
                        "global monitor capacity multiplied across native sources: " + repr(result))
                native = self.all_stats()
                require(total(native, "active") == 256 and delta(before, native, "subscriptions") == 256,
                        "native subscriptions exceed shared global monitor capacity")
                require(delta(before, native, "gets") == delta(before, native, "mutations") == 0,
                        "monitor load performed an upstream GET or mutation")
                status = eventually(lambda: (value if sum(source["monitors"] for source in value["sources"]) == 256 else None)
                                    if (value := self.status()) else None, "shared monitor status publication")
                load_rss = subprocess.check_output(["ps", "-o", "rss=", "-p", str(ioc.pid)], text=True).strip()
                require(sum(source["rejected"] for source in status["sources"]) >= 64,
                        "shared monitor rejection telemetry missing")
                print("global monitor qualification: " + json.dumps(dict(
                    sources=len(status["sources"]), attempted=320, accepted=256, rejected=64,
                    native_subscriptions=256, initial_events=result["events"], waveform_elements=result["waveElements"],
                    idle_rss_kib=int(idle_rss), load_rss_kib=int(load_rss))), flush=True)
            finally:
                if client.poll() is None:
                    try:
                        client.stdin.write("release\n")
                        client.stdin.flush()
                        client.wait(timeout=8)
                    except (BrokenPipeError, subprocess.TimeoutExpired):
                        client.kill()
                        client.wait(timeout=4)
                client.stdin.close()
                client.stdout.close()
            require(client.returncode == 0, "global monitor client failed: " + log_path.read_text())
        eventually(lambda: total(self.all_stats(), "active") == 0, "global monitor lease cleanup")

    def exercise_statuses(self):
        for severity in ("warning", "fatal"):
            self.base.command(severity + " CONTROL")
            self.probe("native-status", "--severity", severity, timeout=35)
        self.base.command("normal CONTROL")

    def exercise_changes(self, count, active):
        self.exercise_statuses()
        for family in ("put", "rpc", "process", "putget", "arrayput", "arrayset"):
            self.base.command("drop LOST")
            before = self.base.stats()["LOST"]
            self.probe("lost", "--family", family, timeout=15)
            after = self.base.stats()["LOST"]
            require(after["mutations"] == before["mutations"] + 1,
                    f"lost {family} reply replayed mutation: {before}->{after}")
            require(after["gets"] == before["gets"], f"lost {family} forwarding performed an extra GET")
        retained = self.manifest.read_bytes()
        self.app.command("stop")
        self.sidecar(success=False)
        require(self.manifest.read_bytes() == retained, "partial native discovery replaced good manifest")
        self.app.command("start")
        eventually(lambda: self.probe("get", "--name", PREFIX + "APP:IP") == "APP", "source reconnect")
        self.base.command("string CONTROL")
        eventually(lambda: self.probe("get", "--name", PREFIX + "BASE:CONTROL") == "fixture-99", "native type renegotiation")
        previous = self.app.info["tcp"]
        self.app.info = self.app.command("rotate")
        require(previous != self.app.info["tcp"], "native fixture failed to change TCP endpoint")
        self.sidecar()
        updated = eventually(lambda: self.status_active(count, active["generation"] + 1), "manifest watcher TCP endpoint update")
        require(updated["activeRevision"] != active["activeRevision"], "endpoint change kept catalog revision")
        self.probe("observe", "--aliases")
        generation = updated["generation"]
        bad = self.manifest.with_suffix(".candidate")
        bad.write_text("schema_version: 1\npvs: [invalid]\n")
        bad.replace(self.manifest)
        eventually(lambda: self.status()["lastError"], "watcher invalid manifest rejection")
        require(self.status()["generation"] == generation, "invalid manifest replaced active generation")
        self.sidecar()
        eventually(lambda: self.status_active(count, generation), "watcher recovery")
        for _ in range(3):
            self.probe("monitor")
        eventually(lambda: total(self.all_stats(), "active") == 0, "bounded channel/monitor churn cleanup")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--ioc", required=True)
    parser.add_argument("--sidecar", required=True)
    parser.add_argument("--extra", type=int, default=1340, help="additional native PVs per role; default total2699")
    args = parser.parse_args()
    require(0 <= args.extra <= 2048, "extra fixture PV count must be in0..2048")
    redis_port = int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"])
    with tempfile.TemporaryDirectory(prefix="xrm-native-integration-") as temporary:
        directory = Path(temporary)
        qualification = Qualification(args, directory, redis_port)
        try:
            qualification.run()
        except BaseException:
            for path in sorted(directory.glob("*.log")):
                print(f"{path.name}:\n{path.read_text()[-16000:]}", flush=True)
            raise
        finally:
            qualification.close()
    print("isolated native qualification passed; no hardware or deployed services were used", flush=True)


if __name__ == "__main__":
    main()
