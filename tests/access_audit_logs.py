#!/usr/bin/env python3
"""Check authorization/completion correlation against real PVA operation outcomes."""
import re
import subprocess
import sys

result = subprocess.run([sys.argv[1]], text=True, capture_output=True, timeout=45)
assert result.returncode == 0, result.stdout + result.stderr
assert "private-rpc-value-must-not-be-logged" not in result.stderr
events = {}
for line in result.stderr.splitlines():
    if "access audit " not in line:
        continue
    match = re.search(r"id=(\d+) phase=(authorization|completion) operation=(put|rpc).* result=(\w+)", line)
    assert match, line
    identifier, phase, operation, outcome = match.groups()
    if identifier == "0":
        assert phase == "authorization" and outcome == "denied", line
    else:
        events.setdefault(identifier, []).append((phase, operation, outcome))
outcomes = set()
for pair in events.values():
    assert len(pair) == 2 and pair[0][0] == "authorization" and pair[0][2] == "allowed", pair
    assert pair[1][0] == "completion" and pair[0][1] == pair[1][1], pair
    outcomes.add(pair[1][2])
assert outcomes == {"success", "error", "cancelled", "abandoned"}, outcomes
print("authorization, distinct completion outcomes, exactly-once audit and RPC payload omission passed")
