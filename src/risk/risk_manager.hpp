// SEBI-mandated pre-trade risk manager.
//
// Every outbound order passes check() before it may reach NSE. The checks are
// ordered cheapest-and-most-critical first and all run inline with no heap
// traffic. Kill switches are atomic so a monitoring/watchdog thread can trip
// them concurrently with the risk thread.
#ifndef HFT_RISK_RISK_MANAGER_HPP_
#define HFT_RISK_RISK_MANAGER_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/compiler.hpp"
#include "common/time_utils.hpp"
#include "common/types.hpp"
#include "session/symbol_master.hpp"

namespace hft {

enum class RejectReason : uint8_t {
  kApproved = 0,
  kKillSwitch,            // Manual or auto (daily-loss) kill switch is active.
  kOutsideMarketHours,
  kOrderTooLarge,
  kLotSizeMismatch,
  kPriceBandBreach,
  kPriceSanity,
  kPositionLimit,
  kRateLimitExceeded,
};

struct RiskConfig {
  int max_orders_per_sec = 1000;
  Quantity max_order_qty = 10000;        // Max single-order quantity.
  int max_token_position_lots = 100;     // Max net per-token position (lots).
  int64_t max_cm_notional_inr = 1'000'000'000;  // Aggregate CM notional cap.
  int64_t max_daily_loss_inr = 100'000;  // Daily loss kill threshold (INR).
  bool allow_preopen = false;            // Permit orders in the pre-open window.
  int price_sanity_pct = 2;              // Max % deviation from market.
};

class RiskManager {
 public:
  RiskManager(const SymbolMaster* symbols, const RiskConfig& cfg)
      : symbols_(symbols),
        cfg_(cfg),
        position_(kSymbolTableSize, 0),
        ref_bid_(kSymbolTableSize, 0),
        ref_ask_(kSymbolTableSize, 0),
        tokens_(static_cast<double>(cfg.max_orders_per_sec)),
        last_refill_ns_(0),
        kill_switch_(false),
        daily_loss_kill_(false),
        cashflow_paisa_(0),
        mark_pnl_inr_(0),
        sim_now_ns_(0) {}

  void set_now(Timestamp ist_ns) { sim_now_ns_ = ist_ns; }

  // Update top-of-book reference for a token (drives the price-sanity check).
  void update_reference(Token token, Price bid, Price ask) {
    if (token < kSymbolTableSize) {
      ref_bid_[token] = bid;
      ref_ask_[token] = ask;
    }
  }

  // The single pre-trade gate. Returns kApproved or the first failing reason.
  RejectReason check(const Order& o) {
    // 0/8. Kill switches (manual or auto daily-loss): reject everything.
    if (HFT_UNLIKELY(kill_switch_.load(std::memory_order_acquire) ||
                     daily_loss_kill_.load(std::memory_order_acquire))) {
      return RejectReason::kKillSwitch;
    }

    const Timestamp now = sim_now_ns_ != 0 ? sim_now_ns_ : ist_now_ns();

    // 1. Market-hours gate.
    if (HFT_UNLIKELY(!is_within_market_hours(now))) {
      if (!(cfg_.allow_preopen && is_preopen(now))) {
        return RejectReason::kOutsideMarketHours;
      }
    }

    // 6. Order size and FO lot-multiple.
    if (HFT_UNLIKELY(o.qty <= 0 || o.qty > cfg_.max_order_qty)) {
      return RejectReason::kOrderTooLarge;
    }
    const Quantity lot = LotSize(o.token);
    if (o.segment == kSegmentFo && lot > 0 && (o.qty % lot) != 0) {
      return RejectReason::kLotSizeMismatch;
    }

    // 3. NSE circuit price band (around previous close).
    Price band_lo = 0;
    Price band_hi = 0;
    if (PriceBand(o.token, &band_lo, &band_hi)) {
      if (HFT_UNLIKELY(o.price < band_lo || o.price > band_hi)) {
        return RejectReason::kPriceBandBreach;
      }
    }

    // 4. Price sanity vs current market (skipped if no reference yet).
    if (HFT_UNLIKELY(!PriceSane(o))) {
      return RejectReason::kPriceSanity;
    }

    // 5. Position limit (projected net position after this order).
    if (HFT_UNLIKELY(!PositionOk(o, lot))) {
      return RejectReason::kPositionLimit;
    }

    // 2. Order-rate limiter (token bucket). Consumed only by otherwise-valid
    // orders so the budget reflects messages we actually send.
    if (HFT_UNLIKELY(!ConsumeRateToken(now))) {
      return RejectReason::kRateLimitExceeded;
    }
    return RejectReason::kApproved;
  }

  // Fill bookkeeping: update position, cashflow and PnL, trip the daily-loss
  // kill switch if breached.
  void on_fill(const FillReport& fill) {
    if (fill.token >= kSymbolTableSize) {
      return;
    }
    const int64_t signed_qty = fill.side == kBuy ? fill.fill_qty : -fill.fill_qty;
    position_[fill.token] += static_cast<Quantity>(signed_qty);
    // Buys spend cash, sells receive cash (in paisa).
    cashflow_paisa_ += -signed_qty * fill.fill_price;
    RecomputePnl(fill.token, fill.fill_price);
  }

  // Manual kill switch control.
  void set_kill_switch(bool on) { kill_switch_.store(on, std::memory_order_release); }
  bool kill_switch_active() const {
    return kill_switch_.load(std::memory_order_acquire) ||
           daily_loss_kill_.load(std::memory_order_acquire);
  }

  Quantity position(Token token) const {
    return token < kSymbolTableSize ? position_[token] : 0;
  }
  int64_t daily_pnl_inr() const { return mark_pnl_inr_; }

 private:
  Quantity LotSize(Token token) const {
    return symbols_ != nullptr ? symbols_->get_lot_size(token) : 0;
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

  bool PriceSane(const Order& o) const {
    const Price ref = o.side == kBuy ? ref_ask_[o.token] : ref_bid_[o.token];
    if (ref <= 0) {
      return true;  // No reference yet: do not gate.
    }
    const Price diff = o.price > ref ? o.price - ref : ref - o.price;
    // diff/ref <= pct/100  <=>  diff*100 <= ref*pct.
    return diff * 100 <= ref * static_cast<Price>(cfg_.price_sanity_pct);
  }

  bool PositionOk(const Order& o, Quantity lot) const {
    const int64_t signed_qty = o.side == kBuy ? o.qty : -o.qty;
    const int64_t projected = static_cast<int64_t>(position_[o.token]) + signed_qty;
    const int64_t l = lot > 0 ? lot : 1;
    int64_t lots = projected / l;
    if (lots < 0) {
      lots = -lots;
    }
    if (lots > cfg_.max_token_position_lots) {
      return false;
    }
    // CM aggregate notional guard on this order.
    if (o.segment == kSegmentCm) {
      const int64_t notional_inr = static_cast<int64_t>(o.qty) * o.price / 100;
      if (notional_inr > cfg_.max_cm_notional_inr) {
        return false;
      }
    }
    return true;
  }

  bool ConsumeRateToken(Timestamp now) {
    const double rate_per_ns =
        static_cast<double>(cfg_.max_orders_per_sec) / static_cast<double>(kNsPerSecond);
    if (last_refill_ns_ == 0) {
      last_refill_ns_ = now;
    }
    if (now > last_refill_ns_) {
      tokens_ += static_cast<double>(now - last_refill_ns_) * rate_per_ns;
      const double cap = static_cast<double>(cfg_.max_orders_per_sec);
      if (tokens_ > cap) {
        tokens_ = cap;
      }
      last_refill_ns_ = now;
    }
    if (tokens_ >= 1.0) {
      tokens_ -= 1.0;
      return true;
    }
    return false;
  }

  void RecomputePnl(Token token, Price mark_paisa) {
    const int64_t pos_value_paisa = static_cast<int64_t>(position_[token]) * mark_paisa;
    const int64_t pnl_paisa = cashflow_paisa_ + pos_value_paisa;
    mark_pnl_inr_ = pnl_paisa / 100;
    if (mark_pnl_inr_ < -cfg_.max_daily_loss_inr) {
      daily_loss_kill_.store(true, std::memory_order_release);
    }
  }

  const SymbolMaster* symbols_;
  RiskConfig cfg_;
  std::vector<Quantity> position_;  // Per-token net position.
  std::vector<Price> ref_bid_;
  std::vector<Price> ref_ask_;

  double tokens_;
  uint64_t last_refill_ns_;

  std::atomic<bool> kill_switch_;
  std::atomic<bool> daily_loss_kill_;

  int64_t cashflow_paisa_;
  int64_t mark_pnl_inr_;
  Timestamp sim_now_ns_;
};

}  // namespace hft

#endif  // HFT_RISK_RISK_MANAGER_HPP_
