# Performance measurement

Python 3 is required to render the Markdown report from the raw benchmark data.
Set `RENDER_REPORT=0` to collect only the environment and raw JSON when the
benchmark runtime does not contain Python.

Access control is absent from the disabled request path: disabled deployments
continue registering PVs in PVXS's built-in static source. When enabled, each
channel caches aggregate read, write, and trap-write rights. An allowed request
does one atomic rights load. Redis update processing and `SharedPV::post()` do
not perform authorization or take access-policy locks.

Build and run the repeatable local benchmark with:

```sh
cmake --build build --target redis-pvxs-access-benchmark
ITERATIONS=10000 sh scripts/benchmark-access.sh
```

The default three repetitions alternate baseline/allow ordering, warm up both
paths equally, preserve every raw sample, and compare medians. Override the
sample count with `REPETITIONS` when needed.

The harness measures these modes:

- `baseline`: access disabled and direct PVXS static-source registration;
- `allow`: enabled with an allow-all policy;
- `mixed`: alternating allowed and denied reads;
- `reload`: allowed traffic plus deny/restore policy activation and reconnect.

It records GET/PUT rate and p50/p95/p99 latency, monitor delivery rate, CPU time,
maximum RSS, denied-operation counts, and reconnect disruption. The generated
directory contains environment metadata, raw JSON per mode, and `report.md`.
The report deliberately has no regression threshold; compare results on the
same host and build configuration.
