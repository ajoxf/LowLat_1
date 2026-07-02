// NSE transaction-cost and rebate model for backtesting.
//
// HONEST FRAMING: unlike many crypto venues, NSE has NO blanket maker rebate.
// You pay charges on BOTH legs of a round trip, and STT is a significant,
// sell-side-only drag. The only genuine "rebate" is via NSE's Liquidity
// Enhancement Scheme (LES) / designated-market-maker incentives on specific
// (usually illiquid) contracts -- modelled here as maker_rebate_bp100.
//
// All rates are in HUNDREDTHS OF A BASIS POINT (bp100): 100 = 1 bp = 0.01%.
// Cost is computed in integer paise (no floating point). The DEFAULTS are
// rough NSE FO approximations and CHANGE frequently -- set them from your
// member's actual contract-note / rate card before trusting any number.
#ifndef HFT_SIM_FEE_MODEL_HPP_
#define HFT_SIM_FEE_MODEL_HPP_

#include <cstdint>

#include "common/types.hpp"

namespace hft {

struct FeeModel {
  // Applied to traded notional on BOTH sides:
  int64_t txn_bp100 = 19;    // exchange transaction charge (~0.0019% FO futures)
  int64_t sebi_bp100 = 1;    // SEBI turnover fee (~0.0001%)
  // Asymmetric statutory charges:
  int64_t stt_sell_bp100 = 200;   // Securities Transaction Tax, SELL side (~0.02% fut)
  int64_t stamp_buy_bp100 = 20;   // stamp duty, BUY side (~0.002%)
  int gst_pct = 18;               // GST on (txn + sebi), integer percent
  // LES / designated-market-maker rebate credited on BOTH sides (0 = none):
  int64_t maker_rebate_bp100 = 0;

  // Net cost in paise for one fill: positive = cost, negative = net credit.
  int64_t cost_paise(Side side, Price price_paise, Quantity qty) const {
    const int64_t notional = static_cast<int64_t>(price_paise) * qty;  // paise
    // rate/1e4 (bps) then /100 (bp100) => /1e6.
    const int64_t txn = notional * txn_bp100 / 1'000'000;
    const int64_t sebi = notional * sebi_bp100 / 1'000'000;
    const int64_t gst = (txn + sebi) * gst_pct / 100;
    const int64_t stt = (side == kSell) ? notional * stt_sell_bp100 / 1'000'000 : 0;
    const int64_t stamp = (side == kBuy) ? notional * stamp_buy_bp100 / 1'000'000 : 0;
    const int64_t rebate = notional * maker_rebate_bp100 / 1'000'000;
    return txn + sebi + gst + stt + stamp - rebate;
  }

  // Options are charged on PREMIUM value with different rates; provide a preset.
  static FeeModel nse_fo_options() {
    FeeModel f;
    f.txn_bp100 = 500;       // ~0.05% of premium
    f.stt_sell_bp100 = 1000; // ~0.10% of premium, sell side (2024 rate)
    f.stamp_buy_bp100 = 30;  // ~0.003%
    return f;
  }
};

}  // namespace hft

#endif  // HFT_SIM_FEE_MODEL_HPP_
