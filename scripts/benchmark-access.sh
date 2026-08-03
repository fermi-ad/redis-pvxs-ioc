#!/bin/sh
set -eu

BUILD_DIR=${BUILD_DIR:-build}
ITERATIONS=${ITERATIONS:-10000}
REPETITIONS=${REPETITIONS:-3}
REPORT_DIR=${REPORT_DIR:-"$BUILD_DIR/access-benchmark-report"}
RENDER_REPORT=${RENDER_REPORT:-1}
BENCHMARK="$BUILD_DIR/redis-pvxs-access-benchmark"

if [ ! -x "$BENCHMARK" ]; then
  echo "missing benchmark executable: $BENCHMARK" >&2
  echo "build redis-pvxs-access-benchmark first" >&2
  exit 2
fi

mkdir -p "$REPORT_DIR"
{
  echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "revision=${REVISION:-$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)}"
  echo "uname=$(uname -a)"
  echo "iterations=$ITERATIONS"
  echo "repetitions=$REPETITIONS"
  echo "compiler=$(c++ --version 2>/dev/null | sed -n '1p' || true)"
  echo "build_type=$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null || true)"
  cpu=""
  memory_bytes=""
  if command -v sysctl >/dev/null 2>&1; then
    cpu=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)
    memory_bytes=$(sysctl -n hw.memsize 2>/dev/null || true)
  elif command -v lscpu >/dev/null 2>&1; then
    cpu=$(lscpu | sed -n 's/^Model name:[[:space:]]*//p' | head -n 1)
  fi
  if [ -z "$cpu" ] && [ -r /proc/cpuinfo ]; then
    cpu=$(sed -n 's/^model name[[:space:]]*:[[:space:]]*//p; s/^Hardware[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -n 1)
  fi
  if [ -z "$cpu" ]; then cpu=$(uname -m); fi
  if [ -z "$memory_bytes" ] && [ -r /proc/meminfo ]; then
    memory_kb=$(sed -n 's/^MemTotal:[[:space:]]*\([0-9][0-9]*\).*/\1/p' /proc/meminfo)
    if [ -n "$memory_kb" ]; then memory_bytes=$((memory_kb * 1024)); fi
  fi
  echo "cpu=$cpu"
  echo "memory_bytes=$memory_bytes"
} > "$REPORT_DIR/environment.txt"

repetition=1
while [ "$repetition" -le "$REPETITIONS" ]; do
  if [ $((repetition % 2)) -eq 1 ]; then
    modes="baseline allow mixed reload"
  else
    modes="allow baseline reload mixed"
  fi
  for mode in $modes; do
    "$BENCHMARK" --mode "$mode" --iterations "$ITERATIONS" \
      > "$REPORT_DIR/$mode-$repetition.json"
  done
  repetition=$((repetition + 1))
done

if [ "$RENDER_REPORT" -eq 1 ]; then
  python3 tools/render_access_benchmark.py "$REPORT_DIR"
  echo "access benchmark report: $REPORT_DIR/report.md"
else
  echo "access benchmark raw results: $REPORT_DIR"
fi
