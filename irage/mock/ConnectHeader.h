// MOCK of the iRage ConnectLib API (ConnectHeader.h), v3.4 surface.
//
// Purpose: lets irage/mystrat.cpp compile and be unit-tested in this repo
// WITHOUT the proprietary iRage library. On the iRage production box the REAL
// header is found first (their Makefile adds -I/usr/local/include/vX/nse/) and
// this file is never used.
//
// Fidelity notes: types, names, call signatures and the NEW/RPL/CXL semantics
// of sendOrder() mirror the "iRage Connect Lib API v3.4" document. Anything
// the document leaves unspecified is implemented with the simplest behaviour
// consistent with it, and any real-world signature drift will surface as a
// compile error localised to irage/mystrat.cpp when building on the iRage box.
//
// This mock is C++17 (the iRage toolchain compiles strategies with -std=c++1z).
#ifndef IRAGE_MOCK_CONNECT_HEADER_H_
#define IRAGE_MOCK_CONNECT_HEADER_H_

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace alpha {

using SymbolIdType = int;  // int for NSE (int64_t for BSE builds).

enum enOrdSide { SIDE_BUY = 1, SIDE_SELL = 2 };

enum class enOrdState : int {
  NO_ORDER = 0,
  UNACKED = 1,
  STANDING_ACKED = 2,
  STANDING_UNACKED = 3,
};

// Error codes (subset used by the strategy; values are mock-local).
enum enError {
  E_OK = 0,
  E_NO_ACK,
  E_ORD_STANDING,
  E_SAME_ORD,
  E_SESSION,
  E_OPS,
  E_INV_UPD,
  E_NO_ORDER,
  E_RMS_DPR,
  E_RMS_TER,
  E_RMS_MAX_OUTSTANDING,
  E_SANITY,
};

// Outgoing order quote for one side of one instrument.
struct Quote {
  SymbolIdType symId = 0;
  int price = 0;  // paise
  int qty = 0;
  enOrdSide side = SIDE_BUY;
  uint8_t index = 0;       // multi-level quoting index (default level 0)
  int minPriceChange = 0;  // min move required before an RPL is sent
};

// 5-level book snapshot per instrument, updated on each tick.
struct MTICK {
  SymbolIdType id = 0;
  int bid[5] = {0, 0, 0, 0, 0};
  int ask[5] = {0, 0, 0, 0, 0};
  int bid_size[5] = {0, 0, 0, 0, 0};
  int ask_size[5] = {0, 0, 0, 0, 0};

  int seq_num = 0;
  char last_side = '-';
  char last_type = '-';  // N/M/T/X, '-' = null/fake tick
  int ltp = 0;
  int ltq = 0;
  int64_t hw_tstamp = 0;
  int64_t publish_tstamp = 0;

  int getExchangeSeqNum() const { return seq_num; }
  char getSide() const { return last_side; }
  char getType() const { return last_type; }
  int getLtp() const { return ltp; }
  int getLtq() const { return ltq; }
  int64_t getHwTstamp() const { return hw_tstamp; }
  int64_t getPublishTstamp() const { return publish_tstamp; }
};

// Exchange response (ACK / EXC / CXD / REJ).
struct EXEC_REPORT {
  SymbolIdType securityId = 0;
  uint64_t iClientId = 0;
  int price = 0;  // paise
  int orderQty = 0;
  uint64_t exchId = 0;
  int orderSide = SIDE_BUY;  // 1 buy, 2 sell
  int leaves = 0;
  int fillQty = 0;
  int cumQty = 0;
  int rejCode = 0;
  int64_t tag = 0;
  int64_t tag2 = 0;
};

// ---------------------------------------------------------------------------
// Mock of the platform implementation object exposed to strategies as `impl`.
// Tests configure its state (ticks, inventory, DPR, running) and inspect the
// orders the strategy sent.
// ---------------------------------------------------------------------------
class ConnectImpl {
 public:
  // --- Test-side configuration -------------------------------------------
  struct SentOrder {
    Quote quote;
    bool ioc = false;
    char type = 'N';  // N=new, R=replace, X=cancel
  };

  void test_setMtick(const MTICK& t) { ticks_[t.id] = t; }
  void test_setRunning(bool r) { running_ = r; }
  void test_setDpr(SymbolIdType id, int lo, int hi) { dpr_[id] = std::make_pair(lo, hi); }
  void test_setInventory(SymbolIdType id, int inv) { inventory_[id] = inv; }
  void test_setSymInfo(SymbolIdType id, const std::map<std::string, std::string>& m) {
    syminfo_[id] = m;
  }
  void test_setSeconds(int s) { seconds_ = s; }
  void test_setGConfig(const std::string& k, const std::string& v) { gconfig_[k] = v; }
  void test_ackAll() {  // Simulate exchange ACK for every in-flight request.
    for (auto& kv : orders_) {
      if (kv.second.state == enOrdState::UNACKED ||
          kv.second.state == enOrdState::STANDING_UNACKED) {
        kv.second.state = kv.second.pending_cancel ? enOrdState::NO_ORDER
                                                   : enOrdState::STANDING_ACKED;
        kv.second.pending_cancel = false;
      }
    }
  }
  std::vector<SentOrder>& test_sent() { return sent_; }

  // --- API called by strategies (per the v3.4 document) --------------------
  void init(std::map<int, std::string>& /*configMap*/) { inited_ = true; }
  void updateClientDetails(const std::string&, const std::string&, const std::string&) {}

  bool running() const { return running_; }

  MTICK* getMtick(SymbolIdType id) { return &ticks_[id]; }

  int getDprLow(SymbolIdType id) {
    auto it = dpr_.find(id);
    return it != dpr_.end() ? it->second.first : 0;
  }
  int getDprHigh(SymbolIdType id) {
    auto it = dpr_.find(id);
    return it != dpr_.end() ? it->second.second : 1 << 30;
  }

  std::pair<bool, std::map<std::string, std::string>> getSymInfo(SymbolIdType id) {
    auto it = syminfo_.find(id);
    if (it == syminfo_.end()) {
      return std::make_pair(false, std::map<std::string, std::string>());
    }
    return std::make_pair(true, it->second);
  }

  // sendOrder with the documented auto semantics:
  //   no standing order          -> NEW
  //   standing order             -> RPL
  //   price or qty == 0          -> CXL
  // Returns false with E_NO_ACK while a previous request is unacked, and
  // false with E_SAME_ORD on an identical replace.
  bool sendOrder(const Quote& q, bool ioc = false) {
    OrderSlot& slot = orders_[Key(q.symId, q.side, q.index)];
    const bool cancel = (q.price == 0 || q.qty == 0);
    if (slot.state == enOrdState::UNACKED || slot.state == enOrdState::STANDING_UNACKED) {
      last_error_ = E_NO_ACK;
      return false;
    }
    if (cancel) {
      if (slot.state == enOrdState::NO_ORDER) {
        last_error_ = E_NO_ORDER;
        return false;
      }
      slot.state = enOrdState::STANDING_UNACKED;
      slot.pending_cancel = true;
      SentOrder s;
      s.quote = q;
      s.ioc = ioc;
      s.type = 'X';
      sent_.push_back(s);
      last_error_ = E_OK;
      return true;
    }
    if (slot.state == enOrdState::STANDING_ACKED) {
      if (slot.quote.price == q.price && slot.quote.qty == q.qty) {
        last_error_ = E_SAME_ORD;
        return false;
      }
      if (q.minPriceChange > 0 &&
          std::abs(q.price - slot.quote.price) < q.minPriceChange) {
        last_error_ = E_SAME_ORD;
        return false;
      }
      slot.quote = q;
      slot.state = enOrdState::STANDING_UNACKED;
      SentOrder s;
      s.quote = q;
      s.ioc = ioc;
      s.type = 'R';
      sent_.push_back(s);
      last_error_ = E_OK;
      return true;
    }
    // NO_ORDER -> NEW
    slot.quote = q;
    slot.state = ioc ? enOrdState::NO_ORDER : enOrdState::UNACKED;
    SentOrder s;
    s.quote = q;
    s.ioc = ioc;
    s.type = 'N';
    sent_.push_back(s);
    last_error_ = E_OK;
    return true;
  }

  bool sendNewLimitOrder(const Quote& q, int64_t /*tag*/ = 0) { return sendOrder(q, false); }
  bool sendReplaceOrder(const Quote& q) { return sendOrder(q, false); }
  bool sendCxlOrder(Quote q) {
    q.price = 0;
    q.qty = 0;
    return sendOrder(q, false);
  }

  bool getStandingOrder(SymbolIdType id, enOrdSide side, uint8_t index, Quote* q) {
    auto it = orders_.find(Key(id, side, index));
    if (it == orders_.end() || it->second.state == enOrdState::NO_ORDER) {
      return false;
    }
    if (q != nullptr) {
      *q = it->second.quote;
    }
    return true;
  }

  enOrdState getPendingOrder(SymbolIdType id, enOrdSide side, uint8_t index, Quote* q) {
    auto it = orders_.find(Key(id, side, index));
    if (it == orders_.end()) {
      return enOrdState::NO_ORDER;
    }
    if (q != nullptr && it->second.state != enOrdState::NO_ORDER) {
      *q = it->second.quote;
    }
    return it->second.state;
  }

  std::string getError() const { return ErrorName(last_error_); }
  int getErrorCode() const { return last_error_; }

  int getOpsConsumed() const { return ops_consumed_; }
  int getTotalOps() const { return total_ops_; }
  void test_setOps(int consumed, int total) {
    ops_consumed_ = consumed;
    total_ops_ = total;
  }

  int getSeconds() const { return seconds_; }
  int64_t getNanoSeconds() const { return static_cast<int64_t>(seconds_) * 1000000000LL; }

  int getMaxOrdSize(SymbolIdType id) {
    auto it = max_ord_.find(id);
    return it != max_ord_.end() ? it->second : 1 << 20;
  }
  int getMaxInventory(SymbolIdType id) {
    auto it = max_inv_.find(id);
    return it != max_inv_.end() ? it->second : 1 << 20;
  }
  int getMaxTarget(SymbolIdType id, enOrdSide) {
    auto it = max_tgt_.find(id);
    return it != max_tgt_.end() ? it->second : 1 << 20;
  }
  void setMaxOrdSize(SymbolIdType id, int lots) { max_ord_[id] = lots; }
  void setMaxInventory(SymbolIdType id, int lots) { max_inv_[id] = lots; }
  void setMaxTarget(SymbolIdType id, enOrdSide, int lots) { max_tgt_[id] = lots; }
  int getInventory(SymbolIdType id) {
    auto it = inventory_.find(id);
    return it != inventory_.end() ? it->second : 0;
  }

  std::pair<bool, std::string> readGConfig(const std::string& key) {
    auto it = gconfig_.find(key);
    if (it == gconfig_.end()) {
      return std::make_pair(false, std::string());
    }
    return std::make_pair(true, it->second);
  }

  void logMsg(const std::string& msg) { log_.push_back(msg); }
  std::vector<std::string>& test_log() { return log_; }
  bool test_inited() const { return inited_; }

 private:
  struct OrderSlot {
    Quote quote;
    enOrdState state = enOrdState::NO_ORDER;
    bool pending_cancel = false;
  };

  static int64_t Key(SymbolIdType id, int side, uint8_t index) {
    return (static_cast<int64_t>(id) << 16) | (static_cast<int64_t>(side) << 8) | index;
  }

  static const char* ErrorName(int code) {
    switch (code) {
      case E_OK: return "E_OK";
      case E_NO_ACK: return "E_NO_ACK";
      case E_SAME_ORD: return "E_SAME_ORD";
      case E_NO_ORDER: return "E_NO_ORDER";
      case E_RMS_DPR: return "E_RMS_DPR";
      case E_OPS: return "E_OPS";
      default: return "E_UNKNOWN";
    }
  }

  std::map<SymbolIdType, MTICK> ticks_;
  std::map<int64_t, OrderSlot> orders_;
  std::vector<SentOrder> sent_;
  std::map<SymbolIdType, std::pair<int, int>> dpr_;
  std::map<SymbolIdType, int> inventory_;
  std::map<SymbolIdType, int> max_ord_;
  std::map<SymbolIdType, int> max_inv_;
  std::map<SymbolIdType, int> max_tgt_;
  std::map<SymbolIdType, std::map<std::string, std::string>> syminfo_;
  std::map<std::string, std::string> gconfig_;
  std::vector<std::string> log_;
  bool running_ = false;
  bool inited_ = false;
  int ops_consumed_ = 0;
  int total_ops_ = 1000;
  int seconds_ = 0;
  int last_error_ = E_OK;
};

// ---------------------------------------------------------------------------
// Strategy base class. One instance per portfolio (clone()d by the platform).
// ---------------------------------------------------------------------------
class AlphaStrategy {
 public:
  virtual ~AlphaStrategy() {}

  ConnectImpl* impl = nullptr;  // Set by the platform before initialize().

  virtual std::unique_ptr<AlphaStrategy> clone() = 0;
  virtual void initialize(std::map<int, std::string>& configMap) = 0;
  virtual int onMarketData() = 0;
  virtual void update(std::map<int, std::string>& updateMap, bool isPortfolioUpdate = false,
                      SymbolIdType symid = 0) {
    (void)updateMap;
    (void)isPortfolioUpdate;
    (void)symid;
  }
  virtual void getUIData(SymbolIdType symbol_id, std::ostringstream& oss) {
    (void)symbol_id;
    (void)oss;
  }
  virtual void onAck(EXEC_REPORT&) {}
  virtual void onExec(EXEC_REPORT&) {}
  virtual void onCxl(EXEC_REPORT&) {}
  virtual void onRej(EXEC_REPORT&) {}
  virtual void onRplRej(EXEC_REPORT&) {}
  virtual void onLighteningRej(EXEC_REPORT&) {}
  virtual bool sanityCheck(MTICK&, int /*index*/) { return true; }
  virtual void onTimerUpdate() {}
  virtual void onWarmupTick() {}
};

void DoPlatformInit(int argc, char** argv);
void GetPortfolios(std::vector<std::map<int, std::string>>& configMaps,
                   std::map<int, std::string>& configMap);
void DoPlatformRun(std::vector<std::map<int, std::string>>& configMaps);

}  // namespace alpha

// Platform registry (strategies are registered here from main()).
class ExternalBaseImpl {
 public:
  static std::map<std::string, std::unique_ptr<alpha::AlphaStrategy>> stratMap;
};

void initAndRun(int argc, char** argv);

#endif  // IRAGE_MOCK_CONNECT_HEADER_H_
