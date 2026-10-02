#!/usr/bin/env python3
"""Short independent PVA helper test; never release/soak evidence."""
import argparse
import json
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--probe", required=True)
    args = parser.parse_args()
    for corrupt in (False, True):
        process = subprocess.Popen([args.fixture] + (["--corrupt"] if corrupt else []), stdout=subprocess.PIPE, text=True)
        try:
            import selectors
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                if not selector.select(5):
                    raise ValueError("independent PVA fixture startup timed out")
                line = process.stdout.readline().strip()
            if not line.startswith("PVA_ADDRESS="):
                raise ValueError("PVA fixture did not return its owned loopback address: " + line)
            address = line.split("=", 1)[1]
            result = subprocess.run([args.probe, address], capture_output=True, text=True, timeout=8)
            if corrupt:
                if result.returncode == 0 or "array corruption" not in result.stderr:
                    raise ValueError("probe failed to reject the corrupted final array element")
            else:
                if result.returncode != 0:
                    raise ValueError(result.stderr)
                value = json.loads(result.stdout)
                if not (value["scalar"] == value["alias"] == value["array"] == 1000 and value["array_exact"]
                        and value["array_elements"] == 4096 and value["ready"] and value["backend_connected"]
                        and value["scalar_time_ns"] == 1760000000000000123 and value["generation"] == 1):
                    raise ValueError("PVA observation changed content/source timestamp/identity")
                legacy = subprocess.run([args.probe, address, "--legacy"], capture_output=True, text=True, timeout=8)
                if legacy.returncode or json.loads(legacy.stdout)["array"] != 1000:
                    raise ValueError("legacy rollback observation failed")
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=3)
            process.stdout.close()
    print("private PVA probe: exact array/alias/timestamp, legacy observation and corruption rejection passed")


if __name__ == "__main__":
    main()
