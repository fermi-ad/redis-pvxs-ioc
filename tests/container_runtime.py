#!/usr/bin/env python3
"""Validate one explicitly named image using only disposable, isolated containers."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid


def docker(*arguments, success=True, timeout=15):
    result = subprocess.run(["docker", *map(str, arguments)], text=True, capture_output=True, timeout=timeout)
    assert (result.returncode == 0) == success, result.stdout + result.stderr
    return result.stdout + result.stderr


def main():
    image = sys.argv[1]
    assert docker("image", "inspect", "--format", "{{.Config.User}}", image).strip() == "10001:10001"
    sandbox = ["--read-only", "--network", "none", "--cap-drop=ALL", "--security-opt=no-new-privileges"]
    assert docker("run", "--rm", *sandbox, "--entrypoint", "/usr/bin/id", image, "-u").strip() == "10001"
    assert "redis-pvxs-ioc" in docker("run", "--rm", *sandbox, image, "--version")
    with tempfile.TemporaryDirectory(prefix="ioc-container-") as directory:
        folder = Path(directory)
        folder.chmod(0o755)
        config = dict(server=dict(instance="container", namespace="TEST", interfaces=["127.0.0.1"],
                                  tcp_port=5075, udp_port=5076, auto_beacon=False),
                      discovery=dict(enabled=False),
                      redis=dict(host="127.0.0.1", port=1, base_key="isolated",
                                 password_file="password.secret"),
                      pvs=[dict(name="value", type="int32", shape="scalar", read=dict(key="value"), initial=42)])
        path = folder / "config.json"
        path.write_text(json.dumps(config))
        path.chmod(0o644)
        password = folder / "password.secret"
        password.write_text("isolated-fixture-secret\n")
        password.chmod(0o400)
        mount = ["--mount", f"type=bind,source={folder},target=/config,readonly"]
        if os.getuid() != 10001:
            output = docker("run", "--rm", *sandbox, *mount, image, "--check-config", "/config/config.json", success=False)
            assert "cannot open secret file" in output and "isolated-fixture-secret" not in output
        override = f"{os.getuid()}:{os.getgid()}"
        docker("run", "--rm", *sandbox, *mount, "--user", override, image, "--check-config", "/config/config.json")
        assert docker("run", "--rm", *sandbox, "--user", "12345:12345", "--entrypoint", "/usr/bin/id", image, "-u").strip() == "12345"
        password.chmod(0o444)  # Synthetic test credential, readable by the image's default identity.
        docker("run", "--rm", *sandbox, *mount, image, "--check-config", "/config/config.json")
        name = "redis-pvxs-runtime-test-" + uuid.uuid4().hex[:16]
        created = False
        try:
            docker("run", "--detach", "--name", name, *sandbox, *mount,
                   "--memory", "256m", "--pids-limit", "128", image, "--config", "/config/config.json")
            created = True
            until = time.monotonic() + 15
            while time.monotonic() < until:
                result = subprocess.run(["docker", "exec", "-e", "EPICS_PVA_AUTO_ADDR_LIST=NO",
                                         "-e", "EPICS_PVA_ADDR_LIST=127.0.0.1:5076", name,
                                         "pvxget", "-w", "1", "TEST:value"], text=True,
                                        capture_output=True, timeout=3)
                if result.returncode == 0:
                    assert "value int32_t = 42" in result.stdout, result.stdout
                    break
                time.sleep(.05)
            else:
                raise AssertionError(docker("logs", name))
            assert docker("exec", name, "id", "-u").strip() == "10001"
        finally:
            if created:
                docker("rm", "--force", name)
    print("non-root identity, UID override, secret permissions and isolated read-only PVA runtime passed")


if __name__ == "__main__":
    main()
