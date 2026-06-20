// Synthetic NSE MTBT generator for offline testing and backtesting.
//
// Produces a random-walk order book for a single token: a Snap Quote each step
// (re-seeding top of book) plus occasional trades that walk through nearby
// price levels. It can yield MarketEvents directly (for the backtester) or be
// encoded to the length-prefixed capture format consumed by mtbt_replay.
#ifndef HFT_SIM_MTBT_GENERATOR_HPP_
#define HFT_SIM_MTBT_GENERATOR_HPP_

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
    } else {
      return;  // Only Q/T are emitted by this generator.
    }
    PutLe<uint32_t>(out, static_cast<uint32_t>(pkt.size()));
    out.insert(out.end(), pkt.begin(), pkt.end());
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
