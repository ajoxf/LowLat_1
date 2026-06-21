// Load a market-event stream for backtesting: parse a recorded MTBT capture
// file (length-prefixed packets) or, when no path is given, generate a synthetic
// depth-bearing MBO session. Shared by the backtest and sweep tools.
#ifndef HFT_SIM_EVENT_SOURCE_HPP_
#define HFT_SIM_EVENT_SOURCE_HPP_

#include <cstdint>
#include <cstdio>
#include <vector>

#include "common/types.hpp"
#include "feed_handler/mtbt_parser.hpp"
#include "sim/mtbt_generator.hpp"

namespace hft {

inline bool read_u32_le(std::FILE* f, uint32_t* out) {
  uint8_t b[4];
  if (std::fread(b, 1, 4, f) != 4) {
    return false;
  }
  *out = b[0] | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
  return true;
}

inline std::vector<MarketEvent> load_events(const char* path, Token token, Price mid,
                                            Price tick, int synth_count = 200000) {
  std::vector<MarketEvent> events;
  if (path == nullptr) {
    MtbtGenerator gen(token, mid, tick, 12345);
    gen.generate_mbo(events, synth_count);
    return events;
  }
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "error: cannot open %s\n", path);
    return events;
  }
  MtbtParser parser;
  std::vector<uint8_t> buf;
  uint32_t len = 0;
  while (read_u32_le(f, &len)) {
    if (len == 0 || len > (1U << 20)) {
      break;
    }
    buf.resize(len);
    if (std::fread(buf.data(), 1, len, f) != len) {
      break;
    }
    parser.parse_packet(buf.data(), len, [&](const MarketEvent& e) { events.push_back(e); });
  }
  std::fclose(f);
  return events;
}

}  // namespace hft

#endif  // HFT_SIM_EVENT_SOURCE_HPP_
