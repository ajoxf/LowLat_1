// Synthetic NSE MTBT generator for offline testing and backtesting.
//
// Produces a random-walk order book for a single token: a Snap Quote each step
// (re-seeding top of book) plus occasional trades that walk through nearby
// price levels. It can yield MarketEvents directly (for the backtester) or be
// encoded to the length-prefixed capture format consumed by mtbt_replay.
#ifndef HFT_SIM_MTBT_GENERATOR_HPP_
#define HFT_SIM_MTBT_GENERATOR_HPP_

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "common/types.hpp"

namespace hft {

class MtbtGenerator {
 public:
  MtbtGenerator(Token token, Price start_mid, Price tick, uint64_t seed = 1)
      : token_(token), mid_(start_mid), tick_(tick), rng_(seed) {}

  // Append `count` events (alternating snap quotes and occasional trades) to
  // `out`. The mid follows a tick-sized random walk; trades print at or through
  // the touch so resting quotes one tick inside get filled when price moves.
  void generate(std::vector<MarketEvent>& out, int count) {
    std::uniform_int_distribution<int> coin(0, 1);
    std::uniform_int_distribution<int> trade_roll(0, 3);
    std::uniform_int_distribution<int> depth(1, 3);
    std::uniform_int_distribution<int> qty(1, 50);
    for (int i = 0; i < count; ++i) {
      // Random-walk the mid by one tick.
      mid_ += (coin(rng_) == 0 ? tick_ : -tick_);
      if (mid_ < tick_ * 10) {
        mid_ = tick_ * 10;  // Floor so prices stay positive.
      }
      // Snap Quote: one-tick spread around the mid.
      MarketEvent q{};
      q.type = static_cast<uint8_t>(MtbtMsgType::kSnapQuote);
      q.token = token_;
      q.seq_no = seq_++;
      q.bid_price = mid_ - tick_;
      q.bid_qty = qty(rng_);
      q.ask_price = mid_ + tick_;
      q.ask_qty = qty(rng_);
      out.push_back(q);

      // Occasionally a trade prints a few ticks away from the mid.
      if (trade_roll(rng_) == 0) {
        const int dir = coin(rng_) == 0 ? 1 : -1;
        MarketEvent t{};
        t.type = static_cast<uint8_t>(MtbtMsgType::kTrade);
        t.token = token_;
        t.seq_no = seq_++;
        t.price = mid_ + dir * tick_ * depth(rng_);
        t.qty = qty(rng_);
        t.order_no = next_order_++;
        t.order_no2 = next_order_++;
        t.trade_no = next_trade_++;
        out.push_back(t);
      }
    }
  }

  // Encode a single event into the capture format: [u32-le length][packet].
  // Each packet carries one MTBT message (header msg_count = 1).
  static void encode_capture(const MarketEvent& ev, std::vector<uint8_t>& out) {
    std::vector<uint8_t> pkt;
    PutLe<uint16_t>(pkt, static_cast<uint16_t>(ev.stream_id == 0 ? 1 : ev.stream_id));
    PutLe<uint64_t>(pkt, ev.seq_no);
    PutLe<uint8_t>(pkt, 1);            // msg_count.
    PutLe<uint64_t>(pkt, ev.ts);       // timestamp.
    if (static_cast<MtbtMsgType>(ev.type) == MtbtMsgType::kSnapQuote) {
      pkt.push_back('Q');
      PutLe<uint32_t>(pkt, ev.token);
      PutLe<int64_t>(pkt, ev.bid_price);
      PutLe<int32_t>(pkt, ev.bid_qty);
      PutLe<int64_t>(pkt, ev.ask_price);
      PutLe<int32_t>(pkt, ev.ask_qty);
    } else if (static_cast<MtbtMsgType>(ev.type) == MtbtMsgType::kTrade) {
      pkt.push_back('T');
      PutLe<int64_t>(pkt, ev.order_no);
      PutLe<int64_t>(pkt, ev.order_no2);
      PutLe<uint32_t>(pkt, ev.token);
      PutLe<int64_t>(pkt, ev.price);
      PutLe<int32_t>(pkt, ev.qty);
      PutLe<int64_t>(pkt, ev.trade_no);
    } else if (static_cast<MtbtMsgType>(ev.type) == MtbtMsgType::kOrderAdd) {
      pkt.push_back('A');
      PutLe<int64_t>(pkt, ev.order_no);
      PutLe<uint8_t>(pkt, ev.side == kBuy ? 1 : 2);
      PutLe<int32_t>(pkt, ev.qty);
      PutLe<uint32_t>(pkt, ev.token);
      PutLe<int64_t>(pkt, ev.price);
    } else if (static_cast<MtbtMsgType>(ev.type) == MtbtMsgType::kOrderCancel) {
      pkt.push_back('X');
      PutLe<int64_t>(pkt, ev.order_no);
      PutLe<uint32_t>(pkt, ev.token);
    } else {
      return;  // Unsupported message type.
    }
    PutLe<uint32_t>(out, static_cast<uint32_t>(pkt.size()));
    out.insert(out.end(), pkt.begin(), pkt.end());
  }

  // Generate a persistent order-by-order (MBO) stream with real depth: orders
  // rest across several levels each side (creating a queue), trades consume
  // resting orders at various prices, and the mid follows a tick random walk.
  // This is what the queue-aware fill model needs -- unlike generate(), the book
  // is not wiped each step, so levels accumulate displayed size.
  void generate_mbo(std::vector<MarketEvent>& out, int steps) {
    struct LiveOrder {
      int64_t no;
      Price price;
      Side side;
      Quantity qty;
    };
    std::vector<LiveOrder> live;
    std::uniform_int_distribution<int> qd(5, 40);
    std::uniform_int_distribution<int> coin(0, 1);
    std::uniform_int_distribution<int> roll(0, 9);

    auto add = [&](Price price, Side side, Quantity qty) {
      MarketEvent e{};
      e.type = static_cast<uint8_t>(MtbtMsgType::kOrderAdd);
      e.order_no = next_order_++;
      e.side = side;
      e.price = price;
      e.qty = qty;
      e.token = token_;
      e.seq_no = seq_++;
      out.push_back(e);
      live.push_back({e.order_no, price, side, qty});
    };
    auto cancel_idx = [&](size_t i) {
      MarketEvent e{};
      e.type = static_cast<uint8_t>(MtbtMsgType::kOrderCancel);
      e.order_no = live[i].no;
      e.token = token_;
      e.seq_no = seq_++;
      out.push_back(e);
      live[i] = live.back();
      live.pop_back();
    };

    // Seed three levels of depth each side.
    for (int k = 1; k <= 3; ++k) {
      add(mid_ - tick_ * k, kBuy, qd(rng_));
      add(mid_ + tick_ * k, kSell, qd(rng_));
    }

    for (int s = 0; s < steps; ++s) {
      if (roll(rng_) < 4) {
        mid_ += (coin(rng_) == 0 ? tick_ : -tick_);
        if (mid_ < tick_ * 10) mid_ = tick_ * 10;
      }
      // Replenish depth near the touch (keeps our quoting level populated).
      add(mid_ - tick_, kBuy, qd(rng_));
      add(mid_ + tick_, kSell, qd(rng_));
      if (roll(rng_) < 5) {
        add(mid_ - tick_ * 2, kBuy, qd(rng_));
        add(mid_ + tick_ * 2, kSell, qd(rng_));
      }
      if (!live.empty() && roll(rng_) < 4) {
        cancel_idx(static_cast<size_t>(rng_() % live.size()));
      }
      // Trade: consume a random resting order (scatters across price levels).
      if (!live.empty() && roll(rng_) < 6) {
        const size_t i = static_cast<size_t>(rng_() % live.size());
        const Quantity tq = std::min<Quantity>(live[i].qty, static_cast<Quantity>(qd(rng_)));
        MarketEvent t{};
        t.type = static_cast<uint8_t>(MtbtMsgType::kTrade);
        t.token = token_;
        t.price = live[i].price;
        t.qty = tq;
        t.seq_no = seq_++;
        if (live[i].side == kBuy) {
          t.order_no = live[i].no;
          t.order_no2 = 0;
        } else {
          t.order_no = 0;
          t.order_no2 = live[i].no;
        }
        t.trade_no = next_trade_++;
        out.push_back(t);
        live[i].qty -= tq;
        if (live[i].qty <= 0) {
          live[i] = live.back();
          live.pop_back();
        }
      }
      while (live.size() > 150) {
        cancel_idx(0);
      }
    }
  }

  Price mid() const { return mid_; }

 private:
  template <typename T>
  static void PutLe(std::vector<uint8_t>& b, T v) {
    auto u = static_cast<uint64_t>(static_cast<std::make_unsigned_t<T>>(v));
    for (size_t i = 0; i < sizeof(T); ++i) {
      b.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xFF));
    }
  }

  Token token_;
  Price mid_;
  Price tick_;
  std::mt19937_64 rng_;
  uint64_t seq_ = 1;
  int64_t next_order_ = 1;
  int64_t next_trade_ = 1;
};

}  // namespace hft

#endif  // HFT_SIM_MTBT_GENERATOR_HPP_
