// NSE symbol master: maps a numeric Token to instrument reference data
// (lot size, tick size, expiry, circuit band, ...). NSE publishes a fresh CSV
// before market open each day; this loads it once at startup and is reloaded
// each morning at ~08:30 IST.
//
// Storage is a flat array of 65536 entries indexed directly by token (NSE
// tokens are < 65536), so lookups are O(1) with no hashing and no heap traffic
// after the initial load.
#ifndef HFT_SESSION_SYMBOL_MASTER_HPP_
#define HFT_SESSION_SYMBOL_MASTER_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/types.hpp"

namespace hft {

inline constexpr size_t kSymbolTableSize = 65536;

enum class InstrumentType : uint8_t {
  kUnknown = 0,
  kEquity = 1,    // CM cash equity (EQ series).
  kFutIdx = 2,    // Index future (Nifty / BankNifty).
  kFutStk = 3,    // Stock future.
  kOptIdx = 4,    // Index option.
  kOptStk = 5,    // Stock option.
};

enum class OptionType : uint8_t { kNone = 0, kCall = 1, kPut = 2 };

struct Instrument {
  Token token = 0;
  Segment segment = 0;
  InstrumentType instrument_type = InstrumentType::kUnknown;
  OptionType option_type = OptionType::kNone;
  Quantity lot_size = 1;        // FO order qty must be a multiple of this.
  Price tick_size = 0;          // Minimum price increment, in paisa.
  Price strike_price = 0;       // In paisa (options).
  Price prev_close = 0;         // Previous close, in paisa (for price bands).
  uint16_t circuit_pct_x100 = 0;  // Circuit band, percent * 100 (e.g. 2000 = 20%).
  uint32_t expiry_yyyymmdd = 0;   // FO expiry date.
  bool active = false;          // True once populated from the CSV.
  char symbol[16] = {0};
  char series[4] = {0};
};

class SymbolMaster {
 public:
  SymbolMaster() : table_(kSymbolTableSize), count_(0) {}

  // Load (or reload) from a CSV file. Header line is required. Returns the
  // number of rows loaded, or -1 on failure to open. Reloading replaces the
  // table contents. CSV columns:
  //   token,symbol,series,instrument_type,segment,expiry_yyyymmdd,strike_paisa,
  //   option_type,lot_size,tick_size,prev_close_paisa,circuit_pct_x100
  long load_csv(const std::string& path);

  // Insert / replace a single instrument (used by tests and programmatic load).
  void put(const Instrument& inst) {
    if (inst.token >= kSymbolTableSize) {
      return;
    }
    if (!table_[inst.token].active) {
      ++count_;
    }
    table_[inst.token] = inst;
    table_[inst.token].active = true;
  }

  const Instrument* get(Token token) const {
    if (token >= kSymbolTableSize || !table_[token].active) {
      return nullptr;
    }
    return &table_[token];
  }

  // Reverse lookup by (symbol, series). Linear over active rows -- startup only.
  Token get_token(const std::string& symbol, const std::string& series) const;

  Quantity get_lot_size(Token token) const {
    const Instrument* i = get(token);
    return i != nullptr ? i->lot_size : 0;
  }
  Price get_tick_size(Token token) const {
    const Instrument* i = get(token);
    return i != nullptr ? i->tick_size : 0;
  }
  uint32_t get_expiry(Token token) const {
    const Instrument* i = get(token);
    return i != nullptr ? i->expiry_yyyymmdd : 0;
  }
  bool is_index_derivative(Token token) const {
    const Instrument* i = get(token);
    if (i == nullptr) {
      return false;
    }
    return i->instrument_type == InstrumentType::kFutIdx ||
           i->instrument_type == InstrumentType::kOptIdx;
  }

  size_t count() const { return count_; }

 private:
  std::vector<Instrument> table_;  // Indexed by token; pre-allocated.
  size_t count_;
};

}  // namespace hft

#endif  // HFT_SESSION_SYMBOL_MASTER_HPP_
