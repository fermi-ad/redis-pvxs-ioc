#!/usr/bin/env python3
"""Render raw access benchmark JSON files into a compact Markdown report."""

import json
import pathlib
import statistics
import sys


def main() -> int:
    report_dir = pathlib.Path(sys.argv[1])
    environment = (report_dir / "environment.txt").read_text(encoding="utf-8")
    samples = {}
    for mode in ("baseline", "allow", "mixed", "reload"):
        paths = sorted(report_dir.glob(f"{mode}-*.json"))
        if not paths:
            paths = [report_dir / f"{mode}.json"]
        samples[mode] = [json.loads(path.read_text(encoding="utf-8")) for path in paths]

    columns = (
        "get_rate",
        "put_rate",
        "get_p50_us",
        "get_p95_us",
        "get_p99_us",
        "put_p50_us",
        "put_p95_us",
        "put_p99_us",
        "monitor_rate",
        "cpu_seconds",
        "max_rss_kb",
        "reconnect_disruptions",
    )
    lines = [
        "# Access-control benchmark",
        "",
        "This report records measurements only; it does not apply a pass/fail threshold.",
        "",
        "## Environment",
        "",
        "```text",
        environment.rstrip(),
        "```",
        "",
        "## Results",
        "",
        "| sample | " + " | ".join(columns) + " |",
        "| --- | " + " | ".join("---:" for _ in columns) + " |",
    ]
    for mode, results in samples.items():
        for index, result in enumerate(results, 1):
            metrics = result["metrics"]
            values = []
            for column in columns:
                value = metrics[column]
                values.append(f"{value:.3f}" if isinstance(value, float) else str(value))
            lines.append(f"| {mode} #{index} | " + " | ".join(values) + " |")

    medians = {
        mode: {
            column: statistics.median(result["metrics"][column] for result in results)
            for column in columns
        }
        for mode, results in samples.items()
    }
    if any(len(results) > 1 for results in samples.values()):
        lines.extend(
            [
                "",
                "## Median results",
                "",
                "| mode | " + " | ".join(columns) + " |",
                "| --- | " + " | ".join("---:" for _ in columns) + " |",
            ]
        )
        for mode, metrics in medians.items():
            values = [f"{metrics[column]:.3f}" for column in columns]
            lines.append(f"| {mode} | " + " | ".join(values) + " |")

    baseline = medians["baseline"]
    allow = medians["allow"]
    lines.extend(
        [
            "",
            "## Allow-all comparison",
            "",
            "Positive deltas mean the allow-all median was higher; no pass/fail threshold is applied.",
            "",
            "| metric | baseline | allow | delta |",
            "| --- | ---: | ---: | ---: |",
        ]
    )
    for column in (
        "get_rate",
        "put_rate",
        "get_p50_us",
        "get_p95_us",
        "get_p99_us",
        "put_p50_us",
        "put_p95_us",
        "put_p99_us",
        "cpu_seconds",
        "max_rss_kb",
    ):
        base_value = baseline[column]
        allow_value = allow[column]
        delta = ((allow_value / base_value) - 1.0) * 100.0 if base_value else 0.0
        lines.append(
            f"| {column} | {base_value:.3f} | {allow_value:.3f} | {delta:+.2f}% |"
        )
    lines.extend(
        [
            "",
            "Raw JSON for each mode is stored beside this report.",
            "The baseline uses PVXS's direct static source; the other modes use the ACF wrapper.",
            "",
        ]
    )
    (report_dir / "report.md").write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
