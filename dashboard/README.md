# Algo dashboard

A self-contained, dependency-free (no npm, no CDN) visual dashboard for the NSE
HFT system. It renders the JSON-lines metrics stream emitted by the backtester
(and, in production, by the monitoring thread) as live SVG charts.

## What it shows

**Summary cards:** cumulative PnL, max drawdown, fills, orders sent, fill rate,
risk-reject rate, net position, latency p99.

**Charts:**
- **Cumulative PnL (₹)** — the equity curve; the headline "are we making or
  losing money" line.
- **Net position** — inventory over time; a position that trends away from zero
  means the maker is accumulating directional risk (a warning sign).
- **Orders sent vs fills vs rejected** — how much we quote, how much actually
  trades, and how much risk blocks.
- **Tick-to-trade latency (p50/p99/p999)** — are we staying fast under load.

## Use it (on your research VM)

```bash
# 1. Produce metrics
./build/tools/backtest                    # writes backtest_metrics.jsonl

# 2a. Serve over HTTP (enables Fetch + Live auto-refresh)
./scripts/serve_dashboard.sh 8000
#    SSH-forward the port:  ssh -L 8000:localhost:8000 user@vm
#    Browse:  http://localhost:8000/dashboard/
```

Or open `dashboard/index.html` directly (`file://`) and click **Load file…** to
pick `backtest_metrics.jsonl` — no server needed.

## Live mode

The metrics format is identical for backtest and live trading. In production the
monitoring thread (Core 7, off the hot path) appends a snapshot per second to a
`.jsonl` file; tick the **Live (2s)** box and the dashboard polls and redraws.
This keeps the visualisation entirely on the research/ops plane — it only ever
*reads* metrics the trading box emits, never the order path.

## Metrics schema (one JSON object per line)

| field | meaning |
|---|---|
| `t` | x-axis (event index in backtest; epoch ms when live) |
| `events` | cumulative market events processed |
| `orders_sent` / `orders_rejected` | cumulative orders sent / risk-rejected |
| `fills` | cumulative fills |
| `position` | current net position (units) |
| `pnl_inr` | cumulative mark-to-market PnL (rupees) |
| `lat_p50` / `lat_p99` / `lat_p999` | tick-to-trade latency (ns) |
