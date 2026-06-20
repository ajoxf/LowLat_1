#!/usr/bin/env bash
# Serve the dashboard + metrics over HTTP so the browser can fetch (and live-poll)
# the metrics file. Run from the repo root after a backtest has produced
# backtest_metrics.jsonl.
#
#   ./scripts/serve_dashboard.sh [port]
#
# Then open http://<vm-ip>:<port>/dashboard/  (or forward the port over SSH:
#   ssh -L 8000:localhost:8000 user@vm   then browse http://localhost:8000/dashboard/ )
set -euo pipefail
PORT="${1:-8000}"
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ ! -f backtest_metrics.jsonl ]; then
  echo "note: backtest_metrics.jsonl not found -- run ./build/tools/backtest first"
fi

echo "Serving repo root on :$PORT"
echo "Open  http://localhost:$PORT/dashboard/  (the dashboard fetches ../backtest_metrics.jsonl)"
exec python3 -m http.server "$PORT"
