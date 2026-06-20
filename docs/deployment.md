# Deployment: the two-plane architecture

Real NSE HFT splits into two physically separate planes with very different
requirements. **Never mix them.**

```
┌──────────────────────────── TRADING PLANE ─────────────────────────────┐
│  NSE BKC colocation hall — bare-metal server (NOT a VM, NOT cloud)      │
│                                                                         │
│   Solarflare/Xilinx NIC + OpenOnload (kernel bypass)                    │
│   isolcpus / nohz_full / C-states off / huge pages / perf governor      │
│                                                                         │
│   ┌─────────────┐   ┌──────────────┐   ┌───────────┐   ┌────────────┐   │
│   │MarketData GW│-->│ OrderBook +  │-->│RiskManager│-->│OrderManager│   │
│   │+FeedHandler │   │ MarketMaking │   │ (SEBI)    │   │+NNF+Gateway│   │
│   └─────▲───────┘   └──────────────┘   └───────────┘   └─────┬──────┘   │
│         │ MTBT multicast (cross-connect #1)                  │ NNF TCP  │
│         │                                          (cross-connect #2)   │
└─────────┼───────────────────────────────────────────────────┼──────────┘
          │                                                     │
       NSE feed                                          NSE NNF gateway
       dissemination                                     → matching engine
          │                                                     │
          └──────────────── async, non-critical ────────────────┘
                                   │
                    (recorded feed, trade logs, dropcopy, metrics
                     shipped OUT over a normal link — never orders IN)
                                   │
┌──────────────────────────── RESEARCH / OPS PLANE ──────────────────────┐
│  Cloud (EC2 / S3 / containers) — latency-insensitive                    │
│                                                                         │
│   • Backtesting at scale: tools/backtest + tools/mtbt_gen,              │
│     parameter sweeps over recorded MTBT in S3                           │
│   • Strategy research, model training, notebooks                        │
│   • Monitoring dashboards + alerting (reads metrics from the colo box)  │
│   • EOD reconciliation, PnL, symbol-master download                     │
│   • CI/CD: build, test, ASan/TSan/UBSan, clang-tidy (this repo's CI)    │
└─────────────────────────────────────────────────────────────────────────┘
```

## What runs where

| Concern | Trading plane (colo bare metal) | Research/ops plane (cloud) |
|---|---|---|
| Live MTBT feed in | ✅ via cross-connect | ❌ impossible (no feed path) |
| Live NNF orders out | ✅ via cross-connect | ❌ never |
| Order book / strategy / risk / encode | ✅ pinned cores | sim only |
| Backtesting & tuning | — | ✅ `backtest`, `mtbt_gen`, sweeps |
| Recorded-feed storage | record locally, ship to S3 | ✅ S3 |
| Monitoring / alerting | emit metrics | ✅ dashboards |
| CI/CD | — | ✅ GitHub Actions |

## The one rule

The link between the planes carries **data outbound** (recorded feeds, logs,
dropcopy fills, metrics) and **config/binaries inbound** (deploy artifacts,
tuned parameters). It **never** carries a live market-data path or a live order
path. The trading plane is self-sufficient during market hours; if the cloud
link dies, trading continues and the kill switch / risk limits still protect you.

## Promotion flow

1. Research plane: tune strategy parameters with `backtest` over recorded MTBT.
2. CI: build + full test + sanitizers + clang-tidy on every change.
3. Deploy the signed binary + `config/system.toml` to the colo box.
4. Colo box: warm caches, NNF login, MTBT subscribe, enable strategy at 09:15.
5. Research plane: ingest the day's recorded feed + dropcopy for next-day tuning.

See the project README for the colo host tuning (GRUB `isolcpus`, BIOS, huge
pages, IRQ pinning, OpenOnload) that the trading plane requires.
