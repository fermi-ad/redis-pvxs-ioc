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
for trail in events.values():
    assert trail[0][0] == "authorization" and trail[0][2] == "allowed", trail
    if len(trail) == 3:
        # Refused by the dispatch re-check after a rights change: the denial
        # keeps the admitted id and the operation completes as denied.
        assert trail[1] == ("authorization", trail[0][1], "denied"), trail
        assert trail[2] == ("completion", trail[0][1], "denied"), trail
    else:
        assert len(trail) == 2, trail
        assert trail[1][0] == "completion" and trail[1][1] == trail[0][1], trail
        assert trail[1][2] != "denied", trail
    outcomes.add(trail[-1][2])
assert {"success", "error", "cancelled", "abandoned"} <= outcomes <= {"success", "error", "cancelled", "abandoned", "denied"}, outcomes
print("authorization, distinct completion outcomes, exactly-once audit and RPC payload omission passed")
