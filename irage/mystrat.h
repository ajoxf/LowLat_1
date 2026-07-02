// NSE market-making strategy for the iRage ConnectLib platform (API v3.4).
//
// This is the production adapter of our market maker: the iRage `lightening`
// platform owns the exchange session, MTBT feed, 5-level book (MTICK), RMS and
// OPS limiting; this class supplies the quoting logic via the AlphaStrategy
// callback interface. The quote math (inventory skew + order-flow skew) is
// shared with the self-hosted stack via src/strategy/quoting_math.hpp.
//
// C++17 (the iRage toolchain compiles strategies with -std=c++1z).
#ifndef IRAGE_MYSTRAT_H_
#define IRAGE_MYSTRAT_H_

#include "ConnectHeader.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace my_strat_ns {

// UI flids (blotter <-> strategy). Inputs are settable from spin.conf buttons;
// outputs are rendered via view.conf [tick] rows. Custom flids must be > 9000.
enum class ui_flids : int {
  // Inputs (spin.conf):
  kMaxSpreadTicks = 9800,  // do not quote into spreads wider than this
  kInvSkewTicks = 9801,    // max inventory-skew shift, in ticks
  kFlowSkewTicks = 9802,   // max order-flow-skew shift, in ticks
  kQuoteLots = 9803,       // lots to quote per side
  kKillSwitch = 9804,      // 1 = cancel everything and stop quoting
  // Outputs (view.conf [tick]):
  kPosition = 9810,
  kBidQuote = 9811,
  kAskQuote = 9812,
  kFills = 9813,
  kPnlPaise = 9814,
};

class strat_1 : public alpha::AlphaStrategy {
 public:
  std::unique_ptr<alpha::AlphaStrategy> clone() override;
  void initialize(std::map<int, std::string>& configMap) override;
  int onMarketData() override;
  void update(std::map<int, std::string>& updateMap, bool isPortfolioUpdate = false,
              alpha::SymbolIdType symid = 0) override;
  void getUIData(alpha::SymbolIdType symbol_id, std::ostringstream& oss) override;
  void onAck(alpha::EXEC_REPORT& e) override;
  void onExec(alpha::EXEC_REPORT& e) override;
  void onCxl(alpha::EXEC_REPORT& e) override;
  void onRej(alpha::EXEC_REPORT& e) override;
  void onRplRej(alpha::EXEC_REPORT& e) override;
  void onLighteningRej(alpha::EXEC_REPORT& e) override;
  bool sanityCheck(alpha::MTICK& tick, int index) override;
  void onTimerUpdate() override;

  // --- Tunables (defaults; overridable live via UI flids) ------------------
  int max_spread_ticks = 3;
  int inv_skew_ticks = 2;
  int flow_skew_ticks = 1;
  int quote_lots = 1;

  // Pre-close cancel sweep boundary, IST seconds-of-day (15:25:00). The
  // platform additionally hard-gates sends at MarketCloseTime.
  int preclose_ist_sod = 15 * 3600 + 25 * 60;

  static constexpr int kMaxSymbols = 16;
  static constexpr int kMaxConsecutiveRejects = 3;
  static constexpr double kOpsHeadroom = 0.8;  // stop re-quoting above 80% OPS

  // Per-symbol state.
  struct SymState {
    alpha::SymbolIdType id = 0;
    int tick_size = 1;
    int lot_size = 1;
    int rejects_buy = 0;   // consecutive rejects; quoting side disabled at cap
    int rejects_sell = 0;
    int last_mid = 0;      // for PnL marking / UI
    int last_bid_quote = 0;
    int last_ask_quote = 0;
  };

  int sym_count = 0;
  SymState syms[kMaxSymbols];

  // Fill accounting (updated on the RECV thread, read on SEND/GUID threads).
  std::atomic<long long> cash_paise{0};
  std::atomic<long long> fills{0};
  std::atomic<long long> orders_sent{0};
  std::atomic<bool> killed{false};

  std::string metrics_path;  // JSONL metrics for the dashboard ("" = disabled)

 private:
  SymState* FindSym(alpha::SymbolIdType id);
  // Quote (or cancel) one side. Returns true if an order message was sent.
  bool QuoteSide(SymState& s, alpha::enOrdSide side, int price, int qty);
  void CancelSide(SymState& s, alpha::enOrdSide side);
  void CancelAll();
  int IstSecondsOfDay() const;
};

}  // namespace my_strat_ns

#endif  // IRAGE_MYSTRAT_H_
