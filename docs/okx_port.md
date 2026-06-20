# Porting the NSE HFT system to OKX

This document covers (1) what changes when moving from NSE to OKX, (2) a detailed
build prompt for the OKX version, and (3) the lease/OPEX infrastructure cost.

> Reality check: "HFT" on a crypto venue like OKX is a **millisecond** game, not
> a nanosecond one. The matching engine is reached over WebSocket/FIX on AWS, not
> a sub-microsecond physical cross-connect. The book/strategy/risk/OMS design
> ports directly; the feed/gateway/encoder layers are rewritten, and the edge
> shifts from wire latency to co-location-in-AWS, fast serialization, rate-limit
> discipline, and connection management.

## 1. What changes (NSE -> OKX)

| Area | NSE (this repo) | OKX |
|---|---|---|
| Market data | MTBT binary UDP **multicast**, dual source | **WebSocket** JSON channels (`books`, `bbo-tbt`, `books-l2-tbt`, `trades`); no multicast |
| Order entry | NNF **binary TCP** (proprietary) | **WebSocket private** JSON ops, **REST**, or **FIX 4.4** (institutional) |
| Auth/session | NNF Gateway Router + Secure Box + Sign On | API key + secret + **passphrase**, HMAC-SHA256 signed; WS `login` op |
| Instruments | numeric **Token**, CSV symbol master | **instId** strings (`BTC-USDT-SWAP`); `GET /api/v5/public/instruments` |
| Numerics | integer **paisa** (×100) fixed point | decimal strings; scale to integers via per-instrument `tickSz`/`lotSz` |
| Sizes | shares / lots | `lotSz`, `minSz`, contract `ctVal`/`ctMult` (perps/futures) |
| Hours | 09:15-15:30 IST, pre-open, circuit | **24/7**, no sessions; perps have **funding** every 8h |
| Book integrity | sequence numbers + gaps | per-message **CRC32 checksum** + seqId; resubscribe on mismatch |
| Regulation | **SEBI** algo rules, circuit bands | none equivalent; exchange **rate limits**, price limits, position tiers, STP |
| Latency target | p99 < 5 us (colo, kernel bypass) | p99 ~ hundreds of us to low ms (AWS same-AZ, WS/FIX) |
| Access cost | crore-scale membership + colo | **funded account**; VIP tiers earned by volume; AWS OPEX |
| Market-data fee | per-message-rate category | **free** via API |

### What is REUSED unchanged (from this repo)
`common/` (ring buffer, memory pool, RDTSC/latency, logger), `order_book/`
(price-level arrays, book manager), `strategy/` (market making + inventory/flow
skew), `risk/` (re-cast checks), `order_manager/` state machine, `pipeline.hpp`,
the **backtest harness**, the **fill simulator**, and the entire **dashboard +
metrics** plane.

### What is REPLACED
- `feed_handler/mtbt_parser` -> OKX WebSocket JSON book/trade parser (+ CRC32).
- `gateway/` -> WebSocket client (TLS) + optional FIX client (no multicast).
- `session/nnf_session` -> OKX WS `login` (HMAC) / FIX `Logon`.
- `order_manager/nnf_encoder` -> OKX JSON order builder / FIX message builder.
- `session/symbol_master` -> OKX instruments loader (`/public/instruments`).
- `common/types` Price -> per-instrument scaled fixed point (parse decimal
  strings into integers using `tickSz`; never use float).

---

## 2. Detailed build prompt (use with Claude Code)

```
# Low-Latency Crypto Market-Making System for OKX — Build Prompt

Build a production-grade, low-latency market-making / execution system in C++20
for the OKX exchange (spot + perpetual swaps + futures + options). Reuse the
architecture of the NSE HFT system (lock-free ring buffers, flat-array order
book, inventory/flow-skew market maker, pre-trade risk, OMS state machine,
backtest harness, metrics dashboard); replace the NSE-specific feed, gateway,
session, encoder and symbol master with OKX equivalents.

Target tick-to-trade: p99 < 500 microseconds from an EC2 instance co-located in
AWS Tokyo (ap-northeast-1), where OKX's matching engine is hosted. NO floating
point anywhere: decimal prices/sizes are parsed into scaled integers using each
instrument's tickSz/lotSz.

## Connectivity (OKX v5 API)
- REST base: https://www.okx.com  (e.g. GET /api/v5/public/instruments,
  /api/v5/account/*, POST /api/v5/trade/order, /trade/cancel-order).
- WebSocket: public  wss://ws.okx.com:8443/ws/v5/public
             private wss://ws.okx.com:8443/ws/v5/private
             business wss://ws.okx.com:8443/ws/v5/business
- FIX API (institutional, optional, lowest-latency order entry + drop copy):
  request onboarding; implement Logon/Heartbeat/NewOrderSingle/OrderCancelRequest/
  OrderCancelReplaceRequest/ExecutionReport.
- Auth: prehash = timestamp + method + requestPath + body;
  sign = Base64(HMAC_SHA256(prehash, apiSecret)); send OK-ACCESS-KEY/SIGN/
  TIMESTAMP/PASSPHRASE headers (REST) or the WS `login` op (apiKey, passphrase,
  timestamp, sign). Support IP-allowlisted, trade-permission API keys.

## Milestone 1 — Foundation (REUSE)
Reuse common/ verbatim: lock-free SPSC ring buffer, memory pool, RDTSC clock and
LatencyTracker, async logger, CPU pinning. Drop the IST/market-hours helpers
(OKX is 24/7); replace with a funding-time clock for perps (next funding ts).

## Milestone 2 — Instrument reference + scaled-decimal numerics
- Load GET /api/v5/public/instruments per instType (SPOT/SWAP/FUTURES/OPTION).
- Per instrument store: instId, baseCcy/quoteCcy/settleCcy, tickSz, lotSz, minSz,
  ctVal, ctMult, state, expTime, plus a price scale (decimals from tickSz).
- ScaledPrice/ScaledQty: parse decimal strings like "63875.5" into int64 ticks
  (value / tickSz) and integer lots. Provide format-back for the wire. Unit-test
  round-trips for representative tickSz (1e-1 .. 1e-8).
- Index instruments by a compact internal id (hash instId -> id) for O(1) routing.

## Milestone 3 — Market-data feed (WebSocket)
- WS client (TLS, e.g. Boost.Beast or a minimal libwebsockets wrapper) on a
  pinned thread; auto-reconnect with backoff; subscribe arg lists.
- Channels: `books` (400-level, snapshot+updates), `bbo-tbt` (tick BBO),
  `books-l2-tbt` (VIP4+), `trades`. Parse JSON with a fast parser (simdjson) but
  store only scaled integers in the order book.
- Maintain the local book from snapshot + incremental updates; validate the
  per-message **CRC32 checksum** (OKX formula over top 25 levels); on mismatch,
  drop the book and resubscribe (this replaces NSE gap/retransmit).
- Feed decoded MarketEvents into the ring buffer to the book/strategy thread.

## Milestone 4 — Order book + strategy (REUSE)
- Reuse the flat-array OrderBook/BookManager (key levels by scaled price).
- Reuse MarketMaking incl. inventory-skew and order-flow-imbalance quoting.
- Add perp specifics: funding-rate-aware fair value (skew quotes by expected
  funding), and quote in contracts (ctVal) for SWAP/FUTURES.

## Milestone 5 — Risk manager (re-cast for OKX)
Pre-trade checks (all integer, < few us):
  1. Kill switch (manual + auto daily-loss), as in NSE build.
  2. **Rate-limit governor** (CRITICAL): token buckets per OKX endpoint/rule
     (order placement, amend, cancel, per-instrument and per-account). Never
     exceed OKX limits — breaches throttle/ban the key. Track WS + REST + FIX.
  3. Price-limit check: reject orders too far from mark/last (OKX rejects "px
     out of range"); use OKX price-limit endpoint or a % band around mark.
  4. Order size: >= minSz, multiple of lotSz, <= max; contracts for derivs.
  5. Position / leverage limits per instrument tier; liquidation-distance guard.
  6. Self-trade prevention (set STP mode; also pre-check our own resting orders).
  7. Notional + max-open-orders caps; funding-aware PnL kill.

## Milestone 6 — Order gateway + OMS
- Order entry over WS private (`order`, `batch-orders`, `amend-order`,
  `cancel-order`) and/or FIX. JSON builder / FIX builder, no sprintf on hot path
  where avoidable; pre-size buffers.
- Assign clOrdId; map clOrdId<->ordId on the `order` ack / ExecutionReport.
- OMS state machine: PENDING -> LIVE -> PARTIALLY_FILLED -> FILLED / CANCELED /
  REJECTED (carry OKX sCode/sMsg). Reconcile via the `orders` private channel
  and/or FIX drop copy.
- WS keepalive ('ping'/'pong'), re-login and re-subscribe on reconnect; do NOT
  blindly resend in-flight orders (reconcile first).

## Milestone 7 — Deployment (AWS Tokyo)
- Run on an EC2 instance in ap-northeast-1, ideally a metal/network-optimized
  type (c7i.metal / c6in.*) in a cluster placement group with enhanced
  networking (ENA). Pin threads, disable C-states, busy-poll the sockets.
- Separate research/ops plane (cheaper EC2 + S3) for recorded WS captures,
  backtests, monitoring — never on the order path.

## Milestone 8 — Backtest + dashboard (REUSE)
- Reuse the fill simulator, the metrics JSONL emitter and the HTML dashboard
  unchanged. Replace the synthetic MTBT generator with a recorded-OKX-WS replayer
  (store raw WS frames; replay through the same pipeline). Add a queue-aware fill
  model and funding accrual for perps.

## Quality bar (same as NSE build)
C++20, -O3, no exceptions/RTTI/float/heap on the hot path; GoogleTest +
GoogleBenchmark; ASan/TSan/UBSan; clang-tidy; CRC32 book validation tested
against OKX's published examples; rate-limit governor unit-tested against OKX's
documented limits.
```

---

## 3. Infrastructure cost (lease / OPEX only — no CAPEX)

Because OKX runs on AWS Tokyo, "co-location" means renting EC2 in the same region
— there is **no rack to buy, no membership, no exchange feed fee**. Approximate
monthly figures (on-demand; 1-3yr savings plans cut compute 40-60%):

| Item | Monthly (lease/OPEX) | Notes |
|---|---|---|
| Trading EC2 (metal, ap-northeast-1, cluster PG) | ~$1,500-6,000 | e.g. c6in.metal / c7i.metal; smaller c6in.4xlarge ~$650 |
| Research/ops EC2 (backtests, dashboard) | ~$100-600 | start/stop on demand |
| S3 + data transfer (recorded WS, intra-region) | ~$50-400 | market data via API is **free** |
| Managed proximity/colo to OKX (optional 3rd-party) | ~$0-3,000 | only if you buy a managed AWS-Tokyo proximity/cross-connect |
| OKX VIP / FIX connectivity | usually **$0 cash** | gated by 30-day volume / assets, not a rack rental |
| **Typical all-in to RUN** | **~$2,000-10,000 / mo** | serious single-box; modest start ~$700-1,500/mo |

Contrast NSE: crore-scale membership + lakhs/month colo + per-message data fees.
**OKX is dramatically cheaper and has no membership gate** — Rung 0 (an EC2 in
Tokyo + the free WebSocket API) costs tens of dollars to start measuring.

Caveats: prices change; metal/placement-group availability varies by AZ; FIX and
the lowest-latency `books-l2-tbt` channel require higher VIP tiers (earned by
volume). Validate against live AWS pricing and OKX's current VIP/colo docs.
