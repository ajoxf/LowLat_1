// Implementation of the iRage ConnectLib market-making strategy.
//
// Threading (per the API doc): onMarketData runs on the SEND thread;
// onAck/onExec/onCxl/onRej on the RECV thread; update on the UPDT thread;
// getUIData on the GUID thread; onTimerUpdate on its own thread. All order
// sending happens ONLY from onMarketData (SEND thread) -- the timer callback is
// restricted to metrics so no two threads race on impl->sendOrder. Cross-thread
// counters are std::atomic.
#include "mystrat.h"

#include <cstdio>
#include <fstream>

#include "strategy/quoting_math.hpp"

namespace my_strat_ns {

using alpha::enOrdSide;
using alpha::EXEC_REPORT;
using alpha::MTICK;
using alpha::Quote;
using alpha::SymbolIdType;

std::unique_ptr<alpha::AlphaStrategy> strat_1::clone() {
  return std::unique_ptr<alpha::AlphaStrategy>(new strat_1());
}

void strat_1::initialize(std::map<int, std::string>& configMap) {
  // configMap[16] holds the symbol count; symbols start at key 751.
  sym_count = std::stoi(configMap[16]);
  if (sym_count > kMaxSymbols) {
    sym_count = kMaxSymbols;
  }
  const int start = 751;
  for (int i = 0; i < sym_count; ++i) {
    const SymbolIdType curr_id =
        static_cast<SymbolIdType>(std::stol(configMap[start + i]));
    auto found_info = impl->getSymInfo(curr_id);
    if (!found_info.first) {
      throw std::runtime_error("Symbol not found in syminfo");
    }
    std::map<std::string, std::string>& syminfo = found_info.second;
    syms[i].id = curr_id;
    syms[i].tick_size = std::stoi(syminfo["tick_size"]);
    syms[i].lot_size = std::stoi(syminfo["lot_size"]);
    if (syms[i].tick_size <= 0) {
      syms[i].tick_size = 1;
    }
    if (syms[i].lot_size <= 0) {
      syms[i].lot_size = 1;
    }
  }

  impl->init(configMap);  // Platform init -- must be called.

  // Optional client-order details (key 901 selects a client config).
  // Populate and call impl->updateClientDetails(acno, settlor, pan) here when
  // trading client (vs PRO) orders.

  // Optional dashboard metrics stream (path from lightening.conf).
  auto metrics_cfg = impl->readGConfig("MetricsJsonlPath");
  if (metrics_cfg.first) {
    metrics_path = metrics_cfg.second;
  }
}

strat_1::SymState* strat_1::FindSym(SymbolIdType id) {
  for (int i = 0; i < sym_count; ++i) {
    if (syms[i].id == id) {
      return &syms[i];
    }
  }
  return nullptr;
}

int strat_1::IstSecondsOfDay() const {
  // impl->getSeconds() is UTC epoch seconds; IST = UTC + 5:30 (19800 s).
  return static_cast<int>((static_cast<long long>(impl->getSeconds()) + 19800) % 86400);
}

bool strat_1::sanityCheck(MTICK& tick, int /*index*/) {
  // Null/fake tick after a packet drop: top sizes are zero (per API doc).
  return tick.bid_size[0] > 0 && tick.ask_size[0] > 0;
}

bool strat_1::QuoteSide(SymState& s, enOrdSide side, int price, int qty) {
  Quote q;
  q.symId = s.id;
  q.price = price;
  q.qty = qty;
  q.side = side;
  q.index = 0;
  // Churn control: do not RPL for sub-tick moves (E_SAME_ORD also guards the
  // identical-quote case). NSE charges per message and enforces OPS.
  q.minPriceChange = s.tick_size;
  if (impl->sendOrder(q, /*ioc=*/false)) {
    orders_sent.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  // E_NO_ACK (awaiting previous response) and E_SAME_ORD (no change) are
  // normal flow-control outcomes -- retry naturally on the next callback.
  const int err = impl->getErrorCode();
  if (err != alpha::E_NO_ACK && err != alpha::E_SAME_ORD) {
    impl->logMsg("sendOrder failed: " + impl->getError());
  }
  return false;
}

void strat_1::CancelSide(SymState& s, enOrdSide side) {
  // Cancel = price/qty zero, but only if an order is actually standing
  // (avoids E_NO_ORDER churn).
  if (!impl->getStandingOrder(s.id, side, 0, nullptr)) {
    return;
  }
  Quote q;
  q.symId = s.id;
  q.price = 0;
  q.qty = 0;
  q.side = side;
  q.index = 0;
  impl->sendOrder(q, false);
}

void strat_1::CancelAll() {
  for (int i = 0; i < sym_count; ++i) {
    CancelSide(syms[i], alpha::SIDE_BUY);
    CancelSide(syms[i], alpha::SIDE_SELL);
  }
}

int strat_1::onMarketData() {
  if (!impl->running()) {
    return 1;  // STOP mode: platform cancels standing orders itself.
  }
  if (killed.load(std::memory_order_acquire)) {
    CancelAll();
    return 1;
  }
  // Pre-close sweep: from 15:25 IST cancel everything, quote nothing. The
  // platform's MarketCloseTime additionally hard-gates sends.
  if (IstSecondsOfDay() >= preclose_ist_sod) {
    CancelAll();
    return 0;
  }

  // OPS headroom guard: above the threshold stop RE-QUOTING (cancels and the
  // in-flight state machine still proceed) so we never run into the limit.
  const int total_ops = impl->getTotalOps();
  const bool ops_ok =
      total_ops <= 0 ||
      impl->getOpsConsumed() < static_cast<int>(total_ops * kOpsHeadroom);

  for (int i = 0; i < sym_count; ++i) {
    SymState& s = syms[i];
    MTICK* t = impl->getMtick(s.id);  // Pointer must not be stored.

    // Null/fake tick (packet drop) or one-sided book: pull quotes, wait for
    // recovery.
    if (t->bid_size[0] <= 0 || t->ask_size[0] <= 0) {
      CancelSide(s, alpha::SIDE_BUY);
      CancelSide(s, alpha::SIDE_SELL);
      continue;
    }

    const int best_bid = t->bid[0];
    const int best_ask = t->ask[0];
    s.last_mid = (best_bid + best_ask) / 2;

    // Spread guard.
    if (best_ask - best_bid > max_spread_ticks * s.tick_size) {
      CancelSide(s, alpha::SIDE_BUY);
      CancelSide(s, alpha::SIDE_SELL);
      continue;
    }

    // Inventory (authoritative, from the platform) and its cap.
    const int position = impl->getInventory(s.id);
    const long long max_pos_units =
        static_cast<long long>(impl->getMaxInventory(s.id)) * s.lot_size;

    // Shared quoting math: one tick inside the touch, shifted by
    // (flow-skew - inventory-skew) ticks.
    const hft::quoting::DesiredQuotes dq = hft::quoting::desired_quotes(
        best_bid, best_ask, s.tick_size, t->bid_size[0], t->ask_size[0], position,
        max_pos_units, inv_skew_ticks, flow_skew_ticks);
    int bid_px = static_cast<int>(dq.bid_price);
    int ask_px = static_cast<int>(dq.ask_price);

    // DPR clamp: the platform RMS rejects out-of-DPR prices; snap into range
    // (respecting the tick grid) and drop a side that cannot be inside.
    const int dpr_lo = impl->getDprLow(s.id);
    const int dpr_hi = impl->getDprHigh(s.id);
    bool bid_ok = true;
    bool ask_ok = true;
    if (dpr_hi > 0 && dpr_hi > dpr_lo) {
      if (bid_px < dpr_lo) {
        bid_px = ((dpr_lo + s.tick_size - 1) / s.tick_size) * s.tick_size;
      }
      if (bid_px > dpr_hi) {
        bid_ok = false;
      }
      if (ask_px > dpr_hi) {
        ask_px = (dpr_hi / s.tick_size) * s.tick_size;
      }
      if (ask_px < dpr_lo) {
        ask_ok = false;
      }
    }

    // Position gates: at max long only quote the offload (sell) side, and
    // vice-versa.
    if (max_pos_units > 0) {
      if (position >= max_pos_units) {
        bid_ok = false;
      }
      if (position <= -max_pos_units) {
        ask_ok = false;
      }
    }

    // Reject backoff: after kMaxConsecutiveRejects on a side, stop quoting it
    // (the platform auto-stops the portfolio at 3 anyway; this keeps us from
    // driving it there).
    if (s.rejects_buy >= kMaxConsecutiveRejects) {
      bid_ok = false;
    }
    if (s.rejects_sell >= kMaxConsecutiveRejects) {
      ask_ok = false;
    }

    const int qty = quote_lots * s.lot_size;

    if (bid_ok && ops_ok) {
      if (QuoteSide(s, alpha::SIDE_BUY, bid_px, qty)) {
        s.last_bid_quote = bid_px;
      }
    } else if (!bid_ok) {
      CancelSide(s, alpha::SIDE_BUY);
      s.last_bid_quote = 0;
    }

    if (ask_ok && ops_ok) {
      if (QuoteSide(s, alpha::SIDE_SELL, ask_px, qty)) {
        s.last_ask_quote = ask_px;
      }
    } else if (!ask_ok) {
      CancelSide(s, alpha::SIDE_SELL);
      s.last_ask_quote = 0;
    }
  }
  return 0;
}

void strat_1::onAck(EXEC_REPORT& e) {
  // A clean ack resets the reject backoff for that side.
  SymState* s = FindSym(e.securityId);
  if (s == nullptr) {
    return;
  }
  if (e.orderSide == alpha::SIDE_BUY) {
    s->rejects_buy = 0;
  } else {
    s->rejects_sell = 0;
  }
}

void strat_1::onExec(EXEC_REPORT& e) {
  fills.fetch_add(1, std::memory_order_relaxed);
  // Passive limit orders trade at our quote price; buys spend cash, sells
  // receive it. Position itself is tracked by the platform (getInventory).
  const long long notional = static_cast<long long>(e.fillQty) * e.price;
  if (e.orderSide == alpha::SIDE_BUY) {
    cash_paise.fetch_sub(notional, std::memory_order_relaxed);
  } else {
    cash_paise.fetch_add(notional, std::memory_order_relaxed);
  }
  SymState* s = FindSym(e.securityId);
  if (s != nullptr) {
    if (e.orderSide == alpha::SIDE_BUY) {
      s->rejects_buy = 0;
    } else {
      s->rejects_sell = 0;
    }
  }
}

void strat_1::onCxl(EXEC_REPORT&) {}

void strat_1::onRej(EXEC_REPORT& e) {
  SymState* s = FindSym(e.securityId);
  if (s == nullptr) {
    return;
  }
  if (e.orderSide == alpha::SIDE_BUY) {
    ++s->rejects_buy;
  } else {
    ++s->rejects_sell;
  }
  impl->logMsg("REJ sym=" + std::to_string(e.securityId) +
               " side=" + std::to_string(e.orderSide) +
               " code=" + std::to_string(e.rejCode));
}

void strat_1::onRplRej(EXEC_REPORT& e) { onRej(e); }
void strat_1::onLighteningRej(EXEC_REPORT& e) { onRej(e); }

void strat_1::update(std::map<int, std::string>& updateMap, bool /*isPortfolioUpdate*/,
                     SymbolIdType /*symid*/) {
  for (std::map<int, std::string>::iterator it = updateMap.begin(); it != updateMap.end();
       ++it) {
    const ui_flids key = static_cast<ui_flids>(it->first);
    const std::string& val = it->second;
    switch (key) {
      case ui_flids::kMaxSpreadTicks:
        max_spread_ticks = std::stoi(val);
        break;
      case ui_flids::kInvSkewTicks:
        inv_skew_ticks = std::stoi(val);
        break;
      case ui_flids::kFlowSkewTicks:
        flow_skew_ticks = std::stoi(val);
        break;
      case ui_flids::kQuoteLots:
        quote_lots = std::stoi(val);
        break;
      case ui_flids::kKillSwitch:
        killed.store(std::stoi(val) != 0, std::memory_order_release);
        break;
      default:
        break;
    }
  }
}

void strat_1::getUIData(SymbolIdType symbol_id, std::ostringstream& oss) {
  SymState* s = FindSym(symbol_id);
  if (s == nullptr) {
    return;
  }
  const int position = impl->getInventory(symbol_id);
  const long long pnl =
      cash_paise.load(std::memory_order_relaxed) +
      static_cast<long long>(position) * s->last_mid;
  oss << static_cast<int>(ui_flids::kPosition) << "=" << position << "^";
  oss << static_cast<int>(ui_flids::kBidQuote) << "=" << s->last_bid_quote << "^";
  oss << static_cast<int>(ui_flids::kAskQuote) << "=" << s->last_ask_quote << "^";
  oss << static_cast<int>(ui_flids::kFills) << "="
      << fills.load(std::memory_order_relaxed) << "^";
  oss << static_cast<int>(ui_flids::kPnlPaise) << "=" << pnl << "^";
}

void strat_1::onTimerUpdate() {
  // Metrics only -- order traffic stays on the SEND thread (onMarketData).
  if (metrics_path.empty()) {
    return;
  }
  long long position = 0;
  long long mark_value = 0;
  for (int i = 0; i < sym_count; ++i) {
    const int inv = impl->getInventory(syms[i].id);
    position += inv;
    mark_value += static_cast<long long>(inv) * syms[i].last_mid;
  }
  const long long pnl_inr =
      (cash_paise.load(std::memory_order_relaxed) + mark_value) / 100;
  std::ofstream out(metrics_path.c_str(), std::ios::app);
  if (!out.is_open()) {
    return;
  }
  out << "{\"t\":" << impl->getSeconds()
      << ",\"events\":0"
      << ",\"orders_sent\":" << orders_sent.load(std::memory_order_relaxed)
      << ",\"orders_rejected\":0"
      << ",\"fills\":" << fills.load(std::memory_order_relaxed)
      << ",\"position\":" << position << ",\"pnl_inr\":" << pnl_inr
      << ",\"lat_p50\":0,\"lat_p99\":0,\"lat_p999\":0}\n";
}

}  // namespace my_strat_ns
