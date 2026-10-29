#!/usr/bin/env python3
"""Exercise catalog safety and publication using the network-free inventory seam."""
import copy
import fcntl
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = str(Path(sys.argv.pop(1)).resolve())
IP = "192.0.2.10"
BASE = ["ACQ2206-097:0:MODEL", "ACQ2206-097:0:SERIAL", "ACQ2206-097:1:NCHAN", "shared"]
APP = ["acq2206_597:FMT:TAB:Tbl", "acq2206_597:SOE_HLD:TAB:Tbl", "shared"]


def inventory(app_port=44000):
    return {"schema_version": 1, "servers": [
        {"server": f"{IP}:5075", "channels": BASE},
        {"server": f"{IP}:{app_port}", "channels": APP},
    ]}


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xrm-proxy-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "inventory.json"
        self.output = self.root / "gateway.yaml"
        self.write(inventory())

    def write(self, value):
        self.source.write_text(json.dumps(value))

    def command(self, *extra):
        return [BINARY, "--ip", IP, "--inventory", str(self.source),
                "--manifest", str(self.output), *extra]

    def run_once(self, *extra, success=True):
        result = subprocess.run(self.command("--once", *extra), text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return result

    def rejected_unchanged(self, candidate, *extra):
        self.write(inventory())
        self.run_once()
        before = self.output.read_bytes()
        before_stat = self.output.stat()
        self.write(candidate)
        self.run_once(*extra, success=False)
        self.assertEqual(self.output.read_bytes(), before)
        self.assertEqual(self.output.stat().st_ino, before_stat.st_ino)

    def test_roles_keep_overlapping_native_names_distinct(self):
        self.run_once()
        text = self.output.read_text()
        self.assertIn("BASE:shared", text)
        self.assertIn("APP:shared", text)
        self.assertEqual(text.count("upstream:"), 7)
        self.assertIn("schema_version: 1", text)
        self.assertNotIn("access:", text)

    def test_invalid_optional_demux_config_does_not_stop_mirror(self):
        result = self.run_once("--demux-config", str(self.root / "missing-demux.yaml"))
        self.assertIn("demux startup failed; PVA inventory continues", result.stderr)
        self.assertTrue(self.output.exists())
        self.assertEqual(self.output.read_text().count("upstream:"), 7)
        invalid = self.root / "invalid-demux.yaml"
        invalid.write_text("schema_version: 1\nunknown_option: true\n")
        result = self.run_once("--demux-config", str(invalid))
        self.assertIn("demux startup failed; PVA inventory continues", result.stderr)

    def demux_config(self):
        return {
            "schema_version": 1,
            "redis": {"host": "127.0.0.1", "port": 1},
            "sources": [{
                "stream": "fixture:raw", "checkpoint": "fixture:checkpoint",
                "format": "int16-le", "layout": "sample-major", "samples_per_channel": 1,
                "start_id": "0-0", "retention": 10, "timestamp": {"mode": "absent"},
                "channels": [{"channel": 1, "raw": "fixture:ch1:raw", "volts": "fixture:ch1:volts",
                              "eslo": 1, "eoff": 0}],
            }],
        }

    def test_redis_connection_failure_is_independent_of_mirror(self):
        config = self.root / "demux.json"
        config.write_text(json.dumps(self.demux_config()))
        with tempfile.TemporaryFile(mode="w+") as log:
            proc = subprocess.Popen(self.command("--demux-config", str(config), "--refresh-ms", "100"),
                                    stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    log.seek(0)
                    text = log.read()
                    if re.search(r"failures=[1-9]", text) and self.output.exists():
                        break
                    self.assertIsNone(proc.poll(), text)
                    time.sleep(0.03)
                else:
                    self.fail("Redis failure was not reported independently of inventory")
                self.assertNotIn("demux startup failed", text)
                self.write(inventory(44555))
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    if f"{IP}:44555" in self.output.read_text():
                        break
                    time.sleep(0.03)
                else:
                    self.fail("mirror did not refresh during Redis failure")
            finally:
                proc.terminate()
                proc.wait(timeout=3)
            self.assertEqual(proc.returncode, 0)

    def test_check_demux_config_has_no_inventory_side_effects(self):
        config = self.root / "demux.json"
        config.write_text(json.dumps(self.demux_config()))
        result = subprocess.run([BINARY, "--check-demux-config", str(config)],
                                text=True, capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("configuration is valid", result.stdout)
        self.assertFalse(self.output.exists())
        self.assertFalse(Path(str(self.output) + ".lock").exists())
        config.write_text("schema_version: 1\nunknown_option: true\n")
        result = subprocess.run([BINARY, "--check-demux-config", str(config)],
                                text=True, capture_output=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)

    def test_aliases_preserved_and_prefix_applies_only_to_canonical(self):
        compat = self.root / "compat.json"
        compat.write_text(json.dumps({"schema_version": 1, "aliases": [
            {"role": "BASE", "upstream": BASE[0], "aliases": ["SL:INST:ICM:XRM:carrier:MODEL"]}
        ]}))
        self.run_once("--prefix", "PROJECT:XRM", "--compat-aliases", str(compat))
        text = self.output.read_text()
        self.assertIn("PROJECT:XRM:BASE:ACQ2206-097:0:MODEL", text)
        self.assertIn("SL:INST:ICM:XRM:carrier:MODEL", text)
        self.assertNotIn("PROJECT:XRM:SL:INST", text)

    def test_lowercase_native_prefix_is_preserved_with_exact_friendly_alias(self):
        candidate = copy.deepcopy(inventory())
        candidate["servers"][0]["channels"] = [channel.replace("ACQ2206-097", "acq2206-097")
                                                 for channel in BASE]
        self.write(candidate)
        compat = self.root / "compat.json"
        compat.write_text(json.dumps({"schema_version": 1, "aliases": [
            {"role": "BASE", "upstream": "acq2206-097:0:MODEL",
             "aliases": ["SL:INST:ICM:XRM:carrier:MODEL"]}
        ]}))
        self.run_once("--compat-aliases", str(compat))
        text = self.output.read_text()
        self.assertIn("BASE:acq2206-097:0:MODEL", text)
        self.assertIn("SL:INST:ICM:XRM:carrier:MODEL", text)
        self.assertNotIn("ACQ2206-097", text)
        compat.write_text(compat.read_text().replace("acq2206-097", "ACQ2206-097"))
        before = self.output.read_bytes()
        self.run_once("--compat-aliases", str(compat), success=False)
        self.assertEqual(self.output.read_bytes(), before)

    def test_reordering_is_noop_and_port_change_keeps_names(self):
        self.run_once()
        first, inode = self.output.read_bytes(), self.output.stat().st_ino
        reordered = copy.deepcopy(inventory())
        reordered["servers"].reverse()
        for server in reordered["servers"]:
            server["channels"].reverse()
        self.write(reordered)
        result = self.run_once()
        self.assertIn("unchanged", result.stdout)
        self.assertEqual(self.output.read_bytes(), first)
        self.assertEqual(self.output.stat().st_ino, inode)
        self.write(inventory(44555))
        self.run_once()
        updated = self.output.read_text()
        self.assertNotIn(f"{IP}:44000", updated)
        self.assertIn(f"{IP}:44555", updated)
        self.assertEqual([line for line in first.decode().splitlines() if "name:" in line],
                         [line for line in updated.splitlines() if "name:" in line])

    def test_partial_inventory_retains_last_good_file(self):
        candidate = inventory()
        candidate["servers"].pop()
        self.rejected_unchanged(candidate)

    def test_duplicate_role_and_mixed_role_rejected(self):
        candidate = copy.deepcopy(inventory())
        candidate["servers"][1]["channels"] = BASE
        self.rejected_unchanged(candidate)
        candidate["servers"][1]["channels"] = BASE + APP
        self.rejected_unchanged(candidate)

    def test_wrong_ip_and_acquisition_endpoint_rejected(self):
        for target in ("192.0.2.11:44000", f"{IP}:4210"):
            candidate = copy.deepcopy(inventory())
            candidate["servers"][1]["server"] = target
            self.rejected_unchanged(candidate)

    def test_missing_and_colliding_compatibility_aliases_rejected(self):
        self.run_once()
        before = self.output.read_bytes()
        for upstream, alias in (("missing", "friendly"), (BASE[0], "APP:shared"),
                                (BASE[0], "shared")):
            compat = self.root / "compat.json"
            compat.write_text(json.dumps({"schema_version": 1, "aliases": [
                {"role": "BASE", "upstream": upstream, "aliases": [alias]}
            ]}))
            self.run_once("--compat-aliases", str(compat), success=False)
            self.assertEqual(self.output.read_bytes(), before)

    def test_unknown_policy_fields_rejected(self):
        candidate = inventory()
        candidate["access"] = {"enabled": False}
        self.rejected_unchanged(candidate)
        compat = self.root / "compat.json"
        compat.write_text(json.dumps({"schema_version": 1, "aliases": [], "access": {}}))
        self.run_once("--compat-aliases", str(compat), success=False)

    def test_duplicate_yaml_keys_and_multiple_documents_rejected(self):
        self.run_once()
        before = self.output.read_bytes()
        valid = json.dumps(inventory())
        self.source.write_text("schema_version: 1\nschema_version: 1\nservers: []\n")
        self.run_once(success=False)
        self.source.write_text(valid + "\n---\n" + valid)
        self.run_once(success=False)
        self.assertEqual(self.output.read_bytes(), before)

    def test_manifest_symlink_and_second_writer_rejected(self):
        target = self.root / "target.yaml"
        target.write_text("retain")
        self.output.symlink_to(target)
        self.run_once(success=False)
        self.assertEqual(target.read_text(), "retain")
        self.output.unlink()
        with open(str(self.output) + ".lock", "a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.run_once(success=False)
        self.assertFalse(self.output.exists())

    def test_full_size_inventory_and_stable_revision(self):
        candidate = copy.deepcopy(inventory())
        candidate["servers"][0]["channels"] = BASE[:2] + [f"DEV:base:{i}" for i in range(2554)]
        candidate["servers"][1]["channels"] = APP[:2] + [f"DEV:app:{i}" for i in range(141)]
        self.write(candidate)
        self.run_once()
        self.assertEqual(self.output.read_text().count("upstream:"), 2699)
        self.run_once()

    def test_continuous_retry_recovers_without_replacing_good_file_early(self):
        self.run_once()
        before = self.output.read_bytes()
        self.write({"schema_version": 1, "servers": []})
        with tempfile.TemporaryFile(mode="w+") as log:
            proc = subprocess.Popen(self.command("--refresh-ms", "100", "--retry-ms", "100"),
                                    stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    log.seek(0)
                    if "inventory rejected" in log.read():
                        break
                    time.sleep(0.03)
                else:
                    self.fail("continuous worker did not report rejection")
                self.assertEqual(self.output.read_bytes(), before)
                self.write(inventory(44555))
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    if f"{IP}:44555" in self.output.read_text():
                        break
                    time.sleep(0.03)
                else:
                    self.fail("continuous worker did not recover")
            finally:
                proc.terminate()
                proc.wait(timeout=3)
            self.assertEqual(proc.returncode, 0)
        self.assertEqual(list(self.root.glob(".xrm-manifest.*")), [])


if __name__ == "__main__":
    unittest.main()
