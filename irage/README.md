# iRage ConnectLib strategy adapter

Production path for this system: trade at NSE colocation **through iRage's
ConnectLib platform** (API v3.4). Their `lightening` platform owns the exchange
session (NNF login/encryption), the MTBT feed and 5-level book (`MTICK`),
exchange RMS (DPR/TER/outstanding-value) and OPS rate limiting. **Our code is
the `AlphaStrategy` plug-in** — the market-making logic only.

```
                         iRage lightening platform (closed source)
                 ┌────────────────────────────────────────────────────┐
 NSE MTBT ──────►│ feed / book / RMS / OPS / session / UI (blotter)   │◄──── NSE NNF
                 │                                                    │
                 │   SEND thread          RECV thread                 │
                 │   sanityCheck()        onAck/onExec/onCxl/onRej()  │
                 │   onMarketData()  ◄──  EXEC_REPORT                 │
                 └───────┬────────────────────▲───────────────────────┘
                         │ impl->getMtick()   │ impl->sendOrder(Quote)
                         ▼                    │
                 ┌────────────────────────────┴───────────────────────┐
                 │  irage/mystrat.cpp  (THIS repo)                    │
                 │  quoting via src/strategy/quoting_math.hpp         │
                 │  (inventory skew + order-flow skew, integer ticks) │
                 └────────────────────────────────────────────────────┘
```

## What the adapter does

`mystrat.cpp` implements the callbacks per the API v3.4 document:

| Concern | Implementation |
|---|---|
| Quoting | One tick inside the touch, shifted by inventory-skew + flow-skew (shared `quoting_math.hpp`); `sendOrder()` auto NEW/RPL/CXL; `minPriceChange = 1 tick` for churn control |
| Position | Authoritative from `impl->getInventory()`; at `getMaxInventory()` cap only the offload side is quoted |
| Packet drops | Null/fake ticks (`bid_size[0]==0`) fail `sanityCheck()` and pull standing quotes until recovery |
| DPR | Quotes snapped into `[getDprLow, getDprHigh]`; a side that cannot be inside is cancelled (avoids `E_RMS_DPR`) |
| OPS | Above 80% of `getTotalOps()` the strategy stops re-quoting (never trips the exchange limit) |
| Rejects | 3 consecutive rejects on a side disables that side (the platform auto-stops the portfolio at 3 — we stop short of it) |
| Flow control | `E_NO_ACK` / `E_SAME_ORD` from `sendOrder()` are treated as normal outcomes and retried on the next callback |
| Close | From 15:25 IST all orders are cancelled and quoting stops (platform `MarketCloseTime` additionally hard-gates) |
| Kill switch | Blotter flid 9804 → cancel all, stop quoting |
| Live tuning | Blotter spin buttons: spread guard, inv/flow skew ticks, quote lots |
| Monitoring | `getUIData()` publishes position/quotes/fills/PnL to the blotter; `onTimerUpdate()` optionally appends dashboard-compatible JSONL (`MetricsJsonlPath` in lightening.conf) |

Threading discipline: **all order traffic stays on the SEND thread**
(`onMarketData`); the RECV callbacks only update atomics/counters; the timer
thread only writes metrics.

## Local build & tests (this repo, no iRage lib needed)

`irage/mock/ConnectHeader.h` is a faithful mock of the documented v3.4 surface
(including `sendOrder`'s NEW/RPL/CXL semantics, `E_NO_ACK`/`E_SAME_ORD` flow
control and null ticks). The adapter compiles against it as **C++17** — the
same standard the iRage toolchain uses (`-std=c++1z`):

```bash
cmake --build build --target test_irage_strat && ./build/irage/test_irage_strat
```

## Deploying on the iRage box

1. Copy `irage/` plus `src/strategy/quoting_math.hpp` (keeping the
   `strategy/` include structure) to the box.
2. Edit `irage/Makefile`: set `LIB_VER` to the ConnectLib version installed
   (`ls /usr/local/lib/`), check the gcc path. Run `make`.
   The real `ConnectHeader.h` is picked up from `/usr/local/include/...` —
   any signature drift vs the mock surfaces as compile errors in
   `mystrat.cpp` only.
3. Merge `irage/ui/portfolio.conf`, `view.conf`, `spin.conf` into `prod/ui/`
   (symbol names must exist in `syminfo.csv`; regenerate with
   portfolio-info-creator).
4. Set risk in `rms.csv` (max order size / net lots / order value) and
   `setMaxInventory` defaults; optionally add `MetricsJsonlPath` to
   `lightening.conf` for the dashboard stream.
5. Launch via `my_launch_lightening.sh`, START the portfolio from the blotter.

## Go-live checklist (in order)

- [ ] Binary builds on the box against the real ConnectLib (step 2 above)
- [ ] Exchange-simulation / mock session run arranged with iRage
- [ ] `rms.csv` limits set LOW (1-lot orders, small net lots) for first days
- [ ] SEBI algo registration for this strategy confirmed via the member
- [ ] Blotter kill switch (flid 9804) tested live
- [ ] Pre-close sweep verified in simulation (15:25 IST cancels)
- [ ] Dropcopy/EOD reconciliation of fills vs `*.fix` files in place
- [ ] Session-disconnect drill: binary relaunch procedure rehearsed
      (per the doc, a disconnect stops all strategies and requires relaunch)

## Known deltas to verify on the real box

- The mock's `getSymInfo` returns `std::pair<bool, map>`; the doc shows
  structured bindings over the same shape.
- `getMaxInventory()` is treated as **lots** here (multiplied by lot size);
  the doc's setter takes lots — confirm the getter's unit with iRage.
- EXEC_REPORT's `price` is used as the fill price for PnL (passive quotes fill
  at our limit). If partial-fill prices differ, switch PnL to the `*.fix`
  file's tag 31 (fill price) at reconciliation.
