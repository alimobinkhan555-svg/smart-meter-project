#!/usr/bin/env bash
#
# scripts/run_demo.sh - build if needed and run the non-interactive demo.
#
#   ./scripts/run_demo.sh [seconds]
#
# Requires no root and no kernel module: the application falls back to its
# in-process pulse simulator.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECONDS_TO_RUN="${1:-30}"
LOG_DIR="$ROOT/logs"
EXPORT_DIR="$ROOT/exports"

cd "$ROOT"

if [[ ! -x build/app ]]; then
    echo "build/app missing - building..."
    make app
fi

mkdir -p "$LOG_DIR" "$EXPORT_DIR"

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"

echo "=== smart meter demo (${SECONDS_TO_RUN}s) ==="
./build/app \
    --demo \
    --run-seconds "$SECONDS_TO_RUN" \
    --log "$LOG_DIR/demo_${STAMP}.csv" \
    --export "$EXPORT_DIR/demo_${STAMP}"

echo
echo "artefacts:"
ls -l "$LOG_DIR/demo_${STAMP}.csv" "$EXPORT_DIR/demo_${STAMP}"_samples.csv \
      "$EXPORT_DIR/demo_${STAMP}"_summary.json "$EXPORT_DIR/demo_${STAMP}.gp" 2>/dev/null || true

if command -v gnuplot >/dev/null 2>&1; then
    gnuplot "$EXPORT_DIR/demo_${STAMP}.gp" && echo "plot: $EXPORT_DIR/demo_${STAMP}_samples.csv.png"
else
    echo "gnuplot not installed - skipping the plot (exported script is ready)"
fi
