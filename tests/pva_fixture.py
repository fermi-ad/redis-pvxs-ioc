"""Bounded PVA startup checks for external IOC measurement fixtures."""
import os
import re
import subprocess
import time


def wait_for_ioc_generation(ioc, pvxget, instance, address):
    """Prove generation 1 is served before constructing a measurement client."""
    started = time.monotonic()
    deadline = started + 5
    name = f"SYS:{instance}:config:generation"
    environment = dict(os.environ, EPICS_PVA_AUTO_ADDR_LIST="NO",
                       EPICS_PVA_ADDR_LIST=address, EPICS_PVA_NAME_SERVERS="")
    last_error = "no PVA response"
    while True:
        if ioc.poll() is not None:
            raise RuntimeError(f"IOC exited with status {ioc.returncode} before {name} became available; "
                               f"last probe: {last_error}")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError(f"IOC startup did not serve {name}=1 within 5 seconds; "
                               f"last probe: {last_error}")
        budget = min(1.0, remaining)
        try:
            result = subprocess.run([pvxget, "-w", str(budget), "-F", "tree", name],
                                    env=environment, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=budget)
        except subprocess.TimeoutExpired as error:
            # subprocess.run kills and reaps the probe before raising.
            output = error.output or b""
            if isinstance(output, bytes):
                output = output.decode(errors="replace")
            last_error = f"PVA probe timed out: {output.strip()[-1000:]}"
            continue
        except OSError as error:
            raise RuntimeError(f"unable to execute PVA startup probe {pvxget}: {error}") from error
        last_error = f"pvxget exit {result.returncode}: {result.stdout.strip()[-1000:]}"
        if ioc.poll() is not None:
            raise RuntimeError(f"IOC exited with status {ioc.returncode} during startup probe for {name}; "
                               f"last probe: {last_error}")
        if (time.monotonic() <= deadline and result.returncode == 0
                and re.search(r"(?m)^\s*int64_t\s+value\s*=\s*1\s*$", result.stdout)):
            print(f"PVA fixture ready: {name}=1 after {time.monotonic() - started:.3f}s", flush=True)
            return
