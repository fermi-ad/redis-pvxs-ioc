#!/usr/bin/env python3
"""Exercise the real RecCeiver protocol and CF processor with an in-memory catalog.

The upstream receiver is supplied by --source; its protocol and CF translation
are used unchanged. This test never contacts a production ChannelFinder.
"""
import argparse
import copy
import fnmatch
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import base64
import urllib.request

from discovery_e2e import free_port


def serve(args):
    from configparser import ConfigParser
    sys.path.insert(0, str(Path(args.source).resolve()))
    from twisted.internet import reactor, task
    from recceiver import cfstore
    from recceiver.processors import ConfigAdapter
    from recceiver.recast import CastFactory

    class CatalogClient:
        def __init__(self, **_):
            self.channels = {}
            self.properties = {}
            self.lock = threading.RLock()

        def save(self):
            target = Path(args.catalog)
            temporary = target.with_suffix(".tmp")
            temporary.write_text(json.dumps(self.channels, sort_keys=True))
            temporary.replace(target)

        def getAllProperties(self):
            return list(self.properties.values())

        def set(self, *, channels=None, property=None):
            with self.lock:
                if property is not None:
                    self.properties[property["name"]] = copy.deepcopy(property)
                for channel in channels or []:
                    self.channels[channel["name"]] = copy.deepcopy(channel)
                self.save()

        def findByArgs(self, pairs):
            with self.lock:
                found = []
                for channel in self.channels.values():
                    properties = {item["name"]: item["value"] for item in channel["properties"]}
                    match = True
                    for key, value in pairs:
                        if key == "~name":
                            match &= any(fnmatch.fnmatchcase(channel["name"], part) for part in value.split("|"))
                        elif not key.startswith("~"):
                            match &= properties.get(key) == value
                    if match:
                        found.append(copy.deepcopy(channel))
                return found

        def update(self, *, property, channelNames):
            with self.lock:
                for name in channelNames:
                    properties = self.channels[name]["properties"]
                    properties[:] = [item for item in properties if item["name"] != property["name"]]
                    properties.append(copy.deepcopy(property))
                self.save()

    if not args.cf_url:
        cfstore.ChannelFinderClient = CatalogClient
    settings = ConfigParser()
    settings.read_dict({"cf": dict(baseUrl=args.cf_url or "http://unused.invalid", username="discovery-test",
                                   cfUsername="admin", cfPassword="password",
                                   cleanOnStart="False", cleanOnStop="False", alias="True",
                                   recordType="True", recordDesc="True", iocConnectionInfo="True",
                                   environment_vars="PVXS_PROTOCOL:protocol", infotags="units")})
    processor = cfstore.CFProcessor("acceptance", ConfigAdapter(settings, "cf"))
    processor.startService()
    factory = CastFactory()
    factory.protocol.timeout = 0.5
    factory.commit = processor.commit
    listener = reactor.listenTCP(0, factory, interface="127.0.0.1")
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    advertisement = struct.pack("!HBBIHHi", 0x5243, 0, 0, 0xffffffff, listener.getHost().port, 0, 42)
    announce = task.LoopingCall(lambda: udp.sendto(advertisement, ("127.0.0.1", args.port)))
    announce.start(0.1)
    reactor.run()
    udp.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, help="Path containing the upstream recceiver Python package")
    parser.add_argument("--ioc")
    parser.add_argument("--pvxget")
    parser.add_argument("--receiver", action="store_true")
    parser.add_argument("--port", type=int)
    parser.add_argument("--catalog")
    parser.add_argument("--cf-url", default="", help="Isolated demo-auth ChannelFinder URL; admin/password fixture only")
    args = parser.parse_args()
    if args.receiver:
        serve(args)
        return
    assert args.ioc
    port = free_port(socket.SOCK_DGRAM)
    config = dict(server=dict(instance="recceiver-acceptance", namespace="RC", interfaces=["127.0.0.1"],
                              tcp_port=0, udp_port=0, auto_beacon=False),
                  discovery=dict(bind_address="127.0.0.1", udp_port=port, timeout_ms=2000, max_holdoff_ms=0),
                  redis=dict(base_key="recceiver-acceptance", host="127.0.0.1", port=int(os.environ["REDIS_PVXS_TEST_REDIS_PORT"])),
                  pvs=[dict(name="value", aliases=["RC:old-alias"], type="float64", shape="scalar",
                            read=dict(key="value"), metadata=dict(description="Real receiver", units="A"))])
    with tempfile.TemporaryDirectory(prefix="redis-pvxs-recceiver-") as directory:
        directory = Path(directory)
        path, catalog_path = directory / "config.json", directory / "catalog.json"
        path.write_text(json.dumps(config))
        log = (directory / "test.log").open("w+")
        processes = []

        def start_receiver():
            receiver = subprocess.Popen([sys.executable, __file__, "--receiver", "--source", args.source,
                                         "--port", str(port), "--catalog", str(catalog_path),
                                         "--cf-url", args.cf_url], stdout=log, stderr=log)
            processes.append(receiver)
            return receiver

        def properties(channel):
            return {item["name"]: item["value"] for item in channel["properties"]}

        def await_catalog(predicate):
            deadline = time.monotonic() + 12
            while time.monotonic() < deadline:
                if args.cf_url:
                    request = urllib.request.Request(args.cf_url.rstrip("/") + "/resources/channels?~name=*")
                    request.add_header("Authorization", "Basic " + base64.b64encode(b"admin:password").decode())
                    request.add_header("Accept", "application/json")
                    with urllib.request.urlopen(request, timeout=3) as response:
                        body = response.read(2 * 1024 * 1024 + 1)
                        assert len(body) <= 2 * 1024 * 1024
                        catalog = {item["name"]: item for item in json.loads(body)}
                    if predicate(catalog):
                        return catalog
                elif catalog_path.exists():
                    catalog = json.loads(catalog_path.read_text())
                    if predicate(catalog):
                        return catalog
                time.sleep(0.05)
            raise AssertionError("real RecCeiver catalog did not reach expected state")

        def read_from_catalog(name, catalog):
            if not args.pvxget:
                return
            location = properties(catalog[name])
            endpoint = f"{location['iocIP']}:{location['pvaPort']}"
            env = dict(os.environ, EPICS_PVA_AUTO_ADDR_LIST="NO", EPICS_PVA_ADDR_LIST="",
                       EPICS_PVA_NAME_SERVERS=endpoint)
            result = subprocess.check_output([args.pvxget, "-w", "3", name], env=env,
                                             text=True, stderr=subprocess.STDOUT, timeout=5)
            assert name in result

        try:
            receiver = start_receiver()
            ioc = subprocess.Popen([args.ioc, "--config", str(path)], stdout=log, stderr=log)
            processes.append(ioc)
            catalog = await_catalog(lambda data: "RC:old-alias" in data and
                                    properties(data["RC:old-alias"]).get("pvStatus") == "Active")
            value = properties(catalog["RC:value"])
            assert value["iocName"] == "recceiver-acceptance" and value["protocol"] == "pva"
            assert 0 < int(value["pvaPort"]) <= 65535 and "caPort" not in value
            assert value["recordType"] == "epics:nt/NTScalar:1.0"
            assert value["units"] == "A"
            assert properties(catalog["RC:old-alias"])["alias"] == "RC:value"
            assert "SYS:recceiver-acceptance:discovery:status" in catalog
            read_from_catalog("RC:old-alias", catalog)
            config["pvs"][0]["aliases"] = ["RC:new-alias"]
            config["pvs"][0]["metadata"]["units"] = "mA"
            temporary = path.with_suffix(".tmp")
            temporary.write_text(json.dumps(config))
            temporary.replace(path)
            ioc.send_signal(signal.SIGHUP)
            catalog = await_catalog(lambda data: "RC:new-alias" in data and
                                    properties(data["RC:new-alias"]).get("pvStatus") == "Active" and
                                    properties(data["RC:old-alias"]).get("pvStatus") == "Inactive")
            assert properties(catalog["RC:value"])["units"] == "mA"
            read_from_catalog("RC:new-alias", catalog)
            # A fresh receiver with an empty catalog is repopulated automatically.
            receiver.terminate()
            receiver.wait(timeout=5)
            catalog_path.unlink(missing_ok=True)
            receiver = start_receiver()
            catalog = await_catalog(lambda data: "RC:new-alias" in data and
                                    properties(data["RC:new-alias"]).get("pvStatus") == "Active")
            assert "RC:old-alias" not in catalog or properties(catalog["RC:old-alias"])["pvStatus"] == "Inactive"
            read_from_catalog("RC:new-alias", catalog)
            backend = "HTTP ChannelFinder" if args.cf_url else "in-memory ChannelFinder client"
            print(f"real RecCeiver with {backend}: registration, aliases, metadata, removal and receiver restart passed")
        except Exception:
            log.flush()
            print((directory / "test.log").read_text())
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            log.close()


if __name__ == "__main__":
    main()
