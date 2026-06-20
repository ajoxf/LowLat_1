#include "session/symbol_master.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace hft {
namespace {

InstrumentType ParseInstrumentType(const std::string& s) {
  if (s == "EQ" || s == "equity") return InstrumentType::kEquity;
  if (s == "FUTIDX") return InstrumentType::kFutIdx;
  if (s == "FUTSTK") return InstrumentType::kFutStk;
  if (s == "OPTIDX") return InstrumentType::kOptIdx;
  if (s == "OPTSTK") return InstrumentType::kOptStk;
  return InstrumentType::kUnknown;
}

OptionType ParseOptionType(const std::string& s) {
  if (s == "CE") return OptionType::kCall;
  if (s == "PE") return OptionType::kPut;
  return OptionType::kNone;
}

void CopyFixed(char* dst, size_t cap, const std::string& src) {
  const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

}  // namespace

long SymbolMaster::load_csv(const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    return -1;
  }
  // Reset table.
  for (auto& row : table_) {
    row.active = false;
  }
  count_ = 0;

  std::string line;
  bool header = true;
  long rows = 0;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    if (header) {
      header = false;  // Skip the column header row.
      continue;
    }
    std::stringstream ss(line);
    std::string field;
    std::vector<std::string> cols;
    while (std::getline(ss, field, ',')) {
      cols.push_back(field);
    }
    if (cols.size() < 12) {
      continue;  // Malformed row; skip.
    }
    Instrument inst;
    inst.token = static_cast<Token>(std::strtoul(cols[0].c_str(), nullptr, 10));
    CopyFixed(inst.symbol, sizeof(inst.symbol), cols[1]);
    CopyFixed(inst.series, sizeof(inst.series), cols[2]);
    inst.instrument_type = ParseInstrumentType(cols[3]);
    inst.segment = static_cast<Segment>(std::strtoul(cols[4].c_str(), nullptr, 10));
    inst.expiry_yyyymmdd = static_cast<uint32_t>(std::strtoul(cols[5].c_str(), nullptr, 10));
    inst.strike_price = static_cast<Price>(std::strtoll(cols[6].c_str(), nullptr, 10));
    inst.option_type = ParseOptionType(cols[7]);
    inst.lot_size = static_cast<Quantity>(std::strtol(cols[8].c_str(), nullptr, 10));
    inst.tick_size = static_cast<Price>(std::strtoll(cols[9].c_str(), nullptr, 10));
    inst.prev_close = static_cast<Price>(std::strtoll(cols[10].c_str(), nullptr, 10));
    inst.circuit_pct_x100 =
        static_cast<uint16_t>(std::strtoul(cols[11].c_str(), nullptr, 10));
    if (inst.lot_size <= 0) {
      inst.lot_size = 1;
    }
    put(inst);
    ++rows;
  }
  return rows;
}

Token SymbolMaster::get_token(const std::string& symbol, const std::string& series) const {
  for (const auto& row : table_) {
    if (row.active && symbol == row.symbol && series == row.series) {
      return row.token;
    }
  }
  return 0;
}

}  // namespace hft
