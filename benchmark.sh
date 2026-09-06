#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY="$DIR/dwmterm"

if [[ ! -x "$BINARY" ]]; then
    echo "==> Building dwmterm..."
    make -C "$DIR"
fi

echo "=========================================================="
echo "    DWM Terminal Performance & Stress Benchmark Harness   "
echo "=========================================================="

# 1. Binary Footprint
echo ""
echo "[1] Binary Metadata & Static Size:"
ls -lh "$BINARY"
file "$BINARY"

# 2. Memory Footprint (RSS)
echo ""
echo "[2] Memory Footprint Measurement:"
"$BINARY" &
TERM_PID=$!
sleep 0.5

if ps -p "$TERM_PID" > /dev/null; then
    RSS_KB=$(ps -o rss= -p "$TERM_PID" | tr -d ' ')
    VSZ_KB=$(ps -o vsz= -p "$TERM_PID" | tr -d ' ')
    echo "  PID: $TERM_PID"
    echo "  Resident Set Size (RSS): $((RSS_KB / 1024)) MB ($RSS_KB KB)"
    echo "  Virtual Memory (VSZ):   $((VSZ_KB / 1024)) MB ($VSZ_KB KB)"
    kill "$TERM_PID" 2>/dev/null || true
    wait "$TERM_PID" 2>/dev/null || true
else
    echo "  Warning: Could not measure active RSS."
fi

# 3. Burst Ingestion Throughput Test
echo ""
echo "[3] Ingestion Throughput Test (100,000 Lines with ANSI colors):"
echo "  Benchmarking dwmterm live ingestion throughput (100,000 lines via -e)..."
python3 -c '
import subprocess, time

t0 = time.perf_counter()
subprocess.run(["'$BINARY'", "-e", "python3", "-c", """
import sys
colors = ["\\033[31m", "\\033[32m", "\\033[33m", "\\033[34m", "\\033[35m", "\\033[36m"]
reset = "\\033[0m"
out = sys.stdout
for i in range(100_000):
    c = colors[i % 6]
    out.write(f"[{i:06d}] {c}INFO{reset}: system event payload at /var/log/syslog.entry.{i}\\n")
"""], check=True)
t1 = time.perf_counter()
dt = t1 - t0
lines_sec = 100_000 / dt
print(f"  Processed 100,000 lines in {dt:.3f}s")
print(f"  Live DWM Terminal Throughput: {lines_sec:,.0f} lines/sec")
'
echo ""
echo "=========================================================="
echo "Benchmark completed successfully."
echo "=========================================================="
