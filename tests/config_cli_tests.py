#!/usr/bin/env python3
"""Exercise strict, compatible offline checking and semantic configuration diffs."""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable = sys.argv[1]
    base = dict(server=dict(instance="offline", namespace="C"),
                redis=dict(base_key="offline", host="127.0.0.1", port=1),
                pvs=[dict(name="value", type="float64", shape="scalar", read=dict(key="value"), initial=1,
                          aliases=["A:one", "A:two"])])
    with tempfile.TemporaryDirectory(prefix="redis-pvxs-config-") as directory:
        directory = Path(directory)
        old, new = directory / "old.json", directory / "new.json"
        old.write_text(json.dumps(base))

        def invoke(*args, valid=True):
            process = subprocess.run([executable, *map(str, args), "--json"], text=True, capture_output=True, timeout=4)
            assert process.returncode == (0 if valid else 1), process.stdout + process.stderr
            return json.loads(process.stdout)

        def check(value, valid=True):
            new.write_text(value if isinstance(value, str) else json.dumps(value))
            result = invoke("--check-config", new, valid=valid)
            assert result["valid"] == valid
            return result

        def diff(value):
            new.write_text(json.dumps(value))
            return invoke("--diff-config", old, new)

        def unchanged(result):
            assert all(not value for value in result.values()), result

        result = check(base)
        assert result["schema_version"] == 1 and result["legacy_input"] is True and result["pv_count"] == 1
        versioned = dict(base, schema_version=1)
        assert check(versioned)["legacy_input"] is False
        unchanged(diff(versioned))
        reordered = copy.deepcopy(versioned)
        reordered["pvs"][0]["aliases"].reverse()
        unchanged(diff(reordered))
        check(dict(base, schema_version=2), False)
        check(dict(base, unexpected=True), False)
        check('{"server": {"instance":"x", "instance":"y"}}', False)
        assert "recursive" in check('server: &self {instance: *self}', False)["error"]

        for type_name, valid_values, invalid_values in [
            ("int8", [-128, 127], [-129, 128, 300]),
            ("uint8", [0, 255], [-1, 256]),
            ("int16", [-32768, 32767], [-32769, 32768]),
            ("uint16", [0, 65535], [-1, 65536]),
            ("uint64", [0, 18446744073709551615], [-1, 18446744073709551616]),
            ("float32", [1.5], [1e100]),
        ]:
            for initial in valid_values + invalid_values:
                value = copy.deepcopy(base)
                value["pvs"][0].update(type=type_name, initial=initial)
                check(value, initial in valid_values)

        for section, settings in [
            ("redis", dict(workers=0)), ("redis", dict(readers=257)),
            ("redis", dict(port=-1)), ("server", dict(tcp_port=65536)),
            ("redis", dict(unrecognized=1)), ("server", dict(unrecognized=1)),
        ]:
            value = copy.deepcopy(base)
            value[section].update(settings)
            check(value, False)
        for field, value in [
            ("unknown", 1), ("read", dict(key="x", unknown=1)),
            ("metadata", dict(display=dict(low=2, high=1))),
            ("metadata", dict(control=dict(unknown=1))),
            ("alarm", dict(low_alarm=10, high_alarm=1)),
            ("alarm", dict(hysteresis=-1)),
            ("transform", dict(scale=0)), ("transform", dict(scale=1e-320)),
            ("transform", dict(offset=float("inf"))),
            ("initial", float("nan")),
        ]:
            config = copy.deepcopy(base)
            config["pvs"][0][field] = value
            check(config, False)

        # Full unknown/duplicate coverage extends to free-form maps.
        text = json.dumps(base)[:-1] + ',"channelfinder":{"properties":{"tag":"one","tag":"two"}}}'
        error = check(text, False)["error"]
        assert "duplicate" in error or "unique" in error
        changed = copy.deepcopy(base)
        changed["pvs"][0]["metadata"] = dict(description="Changed", control=dict(low=0, high=10, min_step=0.1))
        report = diff(changed)
        assert report["metadata_changes"] == ["C:value"] and not report["replacements"]
        assert not report["restart_required"]
        check(changed)
        # Preserve existing scalar metadata and direct-key backend forms.
        compatible = copy.deepcopy(base)
        compatible["redis"]["base_key"] = ""
        compatible["pvs"][0]["metadata"] = dict(precision=-1)
        check(compatible)

        changed = copy.deepcopy(base)
        changed["pvs"][0]["read"]["key"] = "new-key"
        assert diff(changed)["replacements"] == ["C:value"]
        changed = copy.deepcopy(base)
        changed["pvs"][0]["aliases"] = ["A:new"]
        assert diff(changed)["replacements"] == ["C:value"]
        changed = copy.deepcopy(base)
        changed["pvs"][0]["name"] = "renamed"
        report = diff(changed)
        assert report["removals"] == ["C:value"] and report["additions"] == ["C:renamed"]

        changed = copy.deepcopy(base)
        changed["redis"]["password"] = "secret-that-must-not-appear"
        report = diff(changed)
        assert report["backend_changes"] == ["default"] and report["replacements"] == ["C:value"]
        assert "secret-that-must-not-appear" not in json.dumps(report)
        assert "secret-that-must-not-appear" not in json.dumps(check(changed))
        # File inputs resolve relative to the config, preserve spaces, support
        # projected-secret symlinks and never expose contents through JSON.
        secret = directory / "password.secret"
        secret.write_bytes(b" secret-that-must-not-appear \r\n")
        alias = directory / "password-link"
        alias.symlink_to(secret.name)
        changed = copy.deepcopy(base)
        changed["redis"]["password_file"] = alias.name
        assert "secret-that-must-not-appear" not in json.dumps(check(changed))
        inline = copy.deepcopy(base)
        inline["redis"]["password"] = " secret-that-must-not-appear "
        old.write_text(json.dumps(inline))
        unchanged(diff(changed))
        conflict = copy.deepcopy(changed)
        conflict["redis"]["password"] = "secret-that-must-not-appear"
        assert "mutually exclusive" in check(conflict, False)["error"]
        for content in (b"", b"\n", b"secret-that-must-not-appear\x00", b"one\ntwo", b"x" * 16385):
            secret.write_bytes(content)
            report = check(changed, False)
            assert "secret-that-must-not-appear" not in json.dumps(report)
        secret.write_bytes(b"x" * 16384 + b"\r\n")
        check(changed)
        secret.unlink()
        assert "cannot open secret file" in check(changed, False)["error"]
        secret.mkdir()
        assert "regular file" in check(changed, False)["error"]
        secret.rmdir()
        os.mkfifo(secret)
        assert "regular file" in check(changed, False)["error"]  # must not block on FIFO
        secret.unlink()
        # Restore the unrelated baseline for the remaining difference checks.
        old.write_text(json.dumps(base))
        changed = copy.deepcopy(base)
        changed["pvs"][0]["aliases"] = ["SYS:offline:config:lastDiff"]
        check(changed, False)
        changed = copy.deepcopy(base)
        changed["server"]["tcp_port"] = 5100
        assert diff(changed)["restart_required"] == ["server.tcp_port"]
        changed = copy.deepcopy(base)
        changed["discovery"] = dict(enabled=False)
        assert diff(changed)["restart_required"] == ["discovery"]
        restrictive = Path(__file__).resolve().parents[1] / "demo" / "config.access.restrictive.yaml"
        assert invoke("--check-config", restrictive)["valid"]
    print("compatible schema, strict unsafe-input rejection, secret-safe JSON check and offline config diff passed")


if __name__ == "__main__":
    main()
