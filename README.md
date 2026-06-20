# NSE Low-Latency HFT System

A production-grade, low-latency High Frequency Trading system in **C++20**,
targeting India's **National Stock Exchange (NSE)**. It consumes the NSE
**MTBT** (Multicast Tick-By-Tick) market-data feed, maintains real-time order
books, runs a market-making strategy, enforces **SEBI-mandated** pre-trade risk
checks, and sends orders via NSE's **NNF (Non-NEAT Front End) Trimmed Protocol**.

Target tick-to-trade latency: **p99 < 5 µs** on NSE co-located hardware at
Exchange Plaza, BKC, Mumbai.

> **Protocol note.** NSE's MTBT and NNF wire formats are proprietary and
> distributed only to members. The encoders/parsers here are faithful,
> *representative* models (little-endian, numeric tokens, paisa fixed-point).
> When integrating against the live exchange, treat the NSE MTBT spec and the
> `TP_CM_Trimmed_NNF_PROTOCOL` / `TP_FO_Trimmed_NNF_PROTOCOL` documents as the
> ground truth and adjust the field offsets/sizes accordingly.

---

## Build

Requirements: CMake ≥ 3.26, GCC 13 or Clang 16+, network access on first
configure (GoogleTest and Google Benchmark are fetched automatically).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Useful CMake options:

| Option | Default | Purpose |
|---|---|---|
| `HFT_BUILD_TESTS` | ON | Build the GoogleTest unit tests |
| `HFT_BUILD_BENCHMARKS` | ON | Build the Google Benchmark suites |
| `HFT_BUILD_TOOLS` | ON | Build the CLI tools |
| `HFT_NATIVE_ARCH` | ON | `-march=native` (turn OFF for portable/CI builds) |
| `HFT_ENABLE_ASAN` / `HFT_ENABLE_TSAN` / `HFT_ENABLE_UBSAN` | OFF | Sanitizer builds |

Production translation units are compiled `-O3 -march=native -funroll-loops
-fno-exceptions -fno-rtti`; test units enable exceptions/RTTI (required by
GoogleTest) but the headers never depend on them.

## Run

```bash
./build/nse_hft [config/nse_cm_symbols.csv]
```

Without live colocation connectivity the binary runs a self-contained synthetic
session that exercises every stage (book → strategy → risk → NNF encode) and
prints a tick-to-trade latency summary. It is the template for the production
wiring: replace the synthetic event loop with the `MarketDataGateway` (MTBT in)
and `OrderGateway` / `NnfSession` (NNF orders out).

### Tools

```bash
./build/tools/symbol_master_loader config/nse_fo_symbols.csv 26009   # inspect an instrument
./build/tools/mtbt_replay capture.mtbt                               # replay recorded MTBT
./build/tools/latency_histogram samples.txt                          # ASCII p50/p99/p999
```

### Benchmarks

```bash
./build/benchmarks/bench_ring_buffer
./build/benchmarks/bench_order_book
./build/benchmarks/bench_pipeline
```

---

## Architecture

```
            MTBT multicast (Source 1 + Source 2)
                       │
        ┌──────────────▼──────────────┐  Core 2
        │  MarketDataGateway           │  dual-source dedup, gap recovery
        │  + FeedHandler + MtbtParser  │
        └──────────────┬──────────────┘
                       │ MarketEvent ring buffer
        ┌──────────────▼──────────────┐  Core 3
        │  BookManager → OrderBook     │  flat sorted price levels, O(1) top
        │  → MarketMaking strategy     │  quote inside the touch, IST-gated
        └──────────────┬──────────────┘
                       │ Order
        ┌──────────────▼──────────────┐  Core 6 (or inline)
        │  RiskManager (SEBI checks)   │  hours, rate, band, sanity, position,
        └──────────────┬──────────────┘  size, daily-loss + manual kill switch
                       │ approved Order
        ┌──────────────▼──────────────┐  Core 4 / 5
        │  OrderManager → NnfEncoder   │  state machine, NNF binary encode
        │  → OrderGateway / NnfSession │  TCP_NODELAY, login/heartbeat/reconnect
        └──────────────┬──────────────┘
                       ▼
              NNF gateway (TCP) → NSE matching engine
```

Key data structures (all pre-allocated, zero heap traffic on the hot path):

- **Lock-free SPSC ring buffer** with cache-line-isolated head/tail.
- **Order book**: flat, sorted `PriceLevel` arrays (no `std::map`); O(1) best
  bid/ask; pre-allocated open-addressing table maps `order_no → side/price/qty`
  for MBO cancels/modifies/trades.
- **Flat, token-indexed arrays** (`token < 65536`) for books, positions,
  reference prices and the symbol master — O(1), no hashing.

## Source layout

| Path | Milestone | Contents |
|---|---|---|
| `src/common/` | 1 | types, RDTSC/IST clock, ring buffer, memory pool, logger, CPU utils |
| `src/order_book/` | 2 | `OrderBook`, `PriceLevel`, `BookManager` |
| `src/feed_handler/` | 3 | `MtbtParser`, dual-source `FeedHandler` |
| `src/session/` | 3,6 | `SymbolMaster`, `NnfSession` |
| `src/strategy/` | 4 | `StrategyBase`, `MarketMaking` |
| `src/risk/` | 5 | `RiskManager` (SEBI checks) |
| `src/order_manager/` | 6 | `NnfEncoder`, `OrderManager` |
| `src/gateway/` | 7 | `UdpSocket`, `MarketDataGateway`, `OrderGateway` |
| `src/pipeline.hpp`, `src/main.cpp` | 8 | tick-to-trade wiring |

## NSE trading hours (IST)

| Session | Window | System behaviour |
|---|---|---|
| Pre-open | 09:00–09:15 | strategy collects, **no live orders** |
| Continuous | 09:15–15:30 | full quoting; risk gates all orders |
| Pre-close sweep | from 15:25 | cancel all open orders |
| Closed | else | all orders rejected by risk |

All times are IST (UTC+5:30); `ist_now_ns()` and the session-boundary helpers in
`common/time_utils.hpp` drive the gating.

## SEBI pre-trade risk checks

Every order passes `RiskManager::check()` before egress: manual + auto
(daily-loss) **kill switches**, **market-hours** gate, **order-size** and FO
**lot-multiple**, NSE **circuit price band**, **price sanity** vs market,
**position limit**, and a token-bucket **order-rate limiter** (NSE disconnects
above 110 % of the subscribed message rate).

## Latency histogram

`nse_hft`, `tests/test_latency` and `bench_pipeline` report `p50 / p99 / p999`
tick-to-trade latency measured with RDTSC across stages T0 (event in) → T5 (NNF
message handed to the gateway). On an untuned developer/CI host expect
single-to-low-double-digit microseconds; the **p99 < 5 µs** target assumes the
colocated BKC build (isolated cores, pinned threads, OpenOnload, C-states off).

## Colocation deployment (BKC)

See `config/system.toml` for every tunable (multicast IPs/ports, NIC interface,
core affinity, message-rate category, circuit percentages, risk limits). The
host tuning expected on the colo box — `isolcpus`/`nohz_full`/`rcu_nocbs`,
`intel_idle.max_cstate=0`, 2 MB huge pages, performance governor, IRQ pinning,
Solarflare OpenOnload — is documented in the project brief and applied outside
the binary.

## Continuous integration

`.github/workflows/ci.yml` builds with GCC 13 and Clang 18, runs the full test
suite, the ASan+UBSan and TSan sanitizer builds, and clang-tidy.
