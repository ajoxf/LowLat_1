// Market-making strategy for NSE liquid instruments (Nifty 50 equities in CM,
// Nifty / BankNifty near-month futures in FO).
//
// Quote one tick inside the touch on each side, respecting lot sizes, position
// limits, circuit price bands, spread width, quote lifetime (TTL), and NSE
// trading-hours rules. The hot path allocates nothing: pending orders/cancels
// land in fixed-size buffers and per-token quote state lives in a flat array.
#ifndef HFT_STRATEGY_MARKET_MAKING_HPP_
#define HFT_STRATEGY_MARKET_MAKING_HPP_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "common/compiler.hpp"
#include "common/time_utils.hpp"
#include "common/types.hpp"
#include "order_book/book_manager.hpp"
#include "order_book/order_book.hpp"
#include "session/symbol_master.hpp"
#include "strategy/strategy_base.hpp"

namespace hft {

struct MarketMakingConfig {
  Segment segment = kSegmentCm;
  int max_position_lots = 10;     // Max net position, in lots (long or short).
  int max_spread_ticks = 3;       // Do not quote into spreads wider than this.
  uint64_t quote_ttl_ns = 500'000;  // Cancel/refresh quotes older than this.
  Quantity quote_lots = 1;        // Lots to quote per side.
};

class MarketMaking : public StrategyBase {
 public:
  MarketMaking(const SymbolMaster* symbols, const MarketMakingConfig& cfg)
      : symbols_(symbols),
        cfg_(cfg),
        quotes_(kMaxTokens),
        position_(kMaxTokens, 0),
        next_clordid_(1),
        sim_now_ns_(0) {}

  // Tests inject a deterministic IST clock; 0 means "use the real clock".
  void set_now(Timestamp ist_ns) { sim_now_ns_ = ist_ns; }

  Quantity position(Token token) const {
    return token < kMaxTokens ? position_[token] : 0;
  }

  void on_book_update(Token token, const OrderBook& book) override {
    pending_count_ = 0;
    cancel_count_ = 0;
    if (token >= kMaxTokens) {
      return;
    }
    const Timestamp now = sim_now_ns_ != 0 ? sim_now_ns_ : ist_now_ns();

    // Pre-close sweep: from 15:25 IST cancel everything, place nothing.
    if (now >= preclose_cancel_ns(now)) {
      CancelSide(token, /*bid=*/true);
      CancelSide(token, /*bid=*/false);
      return;
    }
    // Only quote during the normal-market window (pre-open collects, no live
    // orders; outside hours nothing is sent).
    if (!is_within_market_hours(now)) {
      return;
    }

    const PriceLevel* bid = book.best_bid();
    const PriceLevel* ask = book.best_ask();
    if (bid == nullptr || ask == nullptr) {
      return;  // Flat book: do not quote.
    }

    const Price tick = TickSize(token);
    if (tick <= 0) {
      return;
    }
    const Price spread = ask->price - bid->price;
    if (spread > static_cast<Price>(cfg_.max_spread_ticks) * tick) {
      return;  // Spread too wide.
    }

    const Quantity lot = LotSize(token);
    const Quantity qty = cfg_.quote_lots * lot;
    const int net_lots = lot > 0 ? position_[token] / lot : position_[token];

    // Circuit price band around the previous close.
    Price band_lo = 0;
    Price band_hi = 0;
    const bool band = PriceBand(token, &band_lo, &band_hi);

    const Price desired_bid = bid->price - tick;
    const Price desired_ask = ask->price + tick;

    // Bid side: only if we are not already at max long and price is in band.
    const bool bid_ok = net_lots < cfg_.max_position_lots &&
                        (!band || (desired_bid >= band_lo && desired_bid <= band_hi));
    QuoteOneSide(token, /*is_bid=*/true, desired_bid, qty, bid_ok, now, tick);

    // Ask side: only if we are not already at max short and price is in band.
    const bool ask_ok = net_lots > -cfg_.max_position_lots &&
                        (!band || (desired_ask >= band_lo && desired_ask <= band_hi));
    QuoteOneSide(token, /*is_bid=*/false, desired_ask, qty, ask_ok, now, tick);
  }

  void on_fill(const FillReport& fill) override {
    if (fill.token >= kMaxTokens) {
      return;
    }
    position_[fill.token] += fill.side == kBuy ? fill.fill_qty : -fill.fill_qty;
    QuoteState& q = quotes_[fill.token];
    if (fill.client_order_id == q.bid_clordid && fill.remaining_qty == 0) {
      q.bid_active = false;
    }
    if (fill.client_order_id == q.ask_clordid && fill.remaining_qty == 0) {
      q.ask_active = false;
    }
  }

  void on_cancel(ClOrdId id, uint8_t /*reason_code*/) override {
    // Clear whichever resting quote this id referenced.
    for (Token t = 0; t < kMaxTokens; ++t) {
      QuoteState& q = quotes_[t];
      if (q.bid_active && q.bid_clordid == id) {
        q.bid_active = false;
        return;
      }
      if (q.ask_active && q.ask_clordid == id) {
        q.ask_active = false;
        return;
      }
    }
  }

  std::span<const Order> get_pending_orders() override {
    return {pending_, pending_count_};
  }
  std::span<const ClOrdId> get_pending_cancels() override {
    return {cancels_, cancel_count_};
  }

 private:
  struct QuoteState {
    bool bid_active = false;
    bool ask_active = false;
    Price bid_price = 0;
    Price ask_price = 0;
    Timestamp bid_ts = 0;
    Timestamp ask_ts = 0;
    ClOrdId bid_clordid = 0;
    ClOrdId ask_clordid = 0;
  };

  static constexpr size_t kMaxPending = 16;

  Price TickSize(Token token) const {
    const Price t = symbols_ != nullptr ? symbols_->get_tick_size(token) : 0;
    return t > 0 ? t : 1;
  }
  Quantity LotSize(Token token) const {
    const Quantity l = symbols_ != nullptr ? symbols_->get_lot_size(token) : 0;
    return l > 0 ? l : 1;
  }
  bool PriceBand(Token token, Price* lo, Price* hi) const {
    if (symbols_ == nullptr) {
      return false;
    }
    const Instrument* inst = symbols_->get(token);
    if (inst == nullptr || inst->prev_close <= 0 || inst->circuit_pct_x100 == 0) {
      return false;
    }
    const Price delta = inst->prev_close * inst->circuit_pct_x100 / 10000;
    *lo = inst->prev_close - delta;
    *hi = inst->prev_close + delta;
    return true;
  }

  void EmitNew(Token token, bool is_bid, Price price, Quantity qty, Timestamp now,
               ClOrdId* out_id) {
    if (pending_count_ >= kMaxPending) {
      return;
    }
    Order o{};
    o.client_order_id = next_clordid_++;
    o.exchange_order_no = 0;
    o.token = token;
    o.segment = cfg_.segment;
    o.price = price;
    o.qty = qty;
    o.disclosed_qty = 0;
    o.side = is_bid ? kBuy : kSell;
    o.order_type = OrderType::kLimit;
    o.validity = Validity::kDay;
    o.created_at = now;
    pending_[pending_count_++] = o;
    *out_id = o.client_order_id;
  }

  void EmitCancel(ClOrdId id) {
    if (cancel_count_ < kMaxPending) {
      cancels_[cancel_count_++] = id;
    }
  }

  void CancelSide(Token token, bool bid) {
    QuoteState& q = quotes_[token];
    if (bid && q.bid_active) {
      EmitCancel(q.bid_clordid);
      q.bid_active = false;
    } else if (!bid && q.ask_active) {
      EmitCancel(q.ask_clordid);
      q.ask_active = false;
    }
  }

  // Place / refresh / hold one side's quote, avoiding churn.
  void QuoteOneSide(Token token, bool is_bid, Price desired, Quantity qty, bool allowed,
                    Timestamp now, Price /*tick*/) {
    QuoteState& q = quotes_[token];
    const bool active = is_bid ? q.bid_active : q.ask_active;
    const Price cur_price = is_bid ? q.bid_price : q.ask_price;
    const Timestamp cur_ts = is_bid ? q.bid_ts : q.ask_ts;

    if (!allowed) {
      // Not permitted to quote this side now: pull any existing quote.
      CancelSide(token, is_bid);
      return;
    }

    const bool stale = active && (now - cur_ts) > cfg_.quote_ttl_ns;
    if (active && cur_price == desired && !stale) {
      return;  // Already quoting at the right price: do nothing (no churn).
    }
    if (active) {
      CancelSide(token, is_bid);  // Cancel the old quote before re-quoting.
    }
    ClOrdId id = 0;
    EmitNew(token, is_bid, desired, qty, now, &id);
    if (is_bid) {
      q.bid_active = true;
      q.bid_price = desired;
      q.bid_ts = now;
      q.bid_clordid = id;
    } else {
      q.ask_active = true;
      q.ask_price = desired;
      q.ask_ts = now;
      q.ask_clordid = id;
    }
  }

  const SymbolMaster* symbols_;
  MarketMakingConfig cfg_;
  std::vector<QuoteState> quotes_;   // Per-token, indexed by token.
  std::vector<Quantity> position_;   // Per-token net position (signed units).
  ClOrdId next_clordid_;
  Timestamp sim_now_ns_;

  Order pending_[kMaxPending];
  size_t pending_count_ = 0;
  ClOrdId cancels_[kMaxPending];
  size_t cancel_count_ = 0;
};

}  // namespace hft

#endif  // HFT_STRATEGY_MARKET_MAKING_HPP_
