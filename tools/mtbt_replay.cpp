// mtbt_replay: replay recorded NSE MTBT packets through the feed handler for
// offline testing. The capture format is a sequence of [uint32 length]
// [length bytes packet] records (little-endian length).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "feed_handler/feed_handler.hpp"

namespace {

bool ReadU32(std::FILE* f, uint32_t* out) {
  uint8_t b[4];
  if (std::fread(b, 1, 4, f) != 4) {
    return false;
  }
  *out = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <capture.mtbt>\n", argv[0]);
    std::fprintf(stderr,
                 "  capture format: repeated [u32 le length][packet bytes]\n");
    return 2;
  }
  std::FILE* f = std::fopen(argv[1], "rb");
  if (f == nullptr) {
    std::fprintf(stderr, "error: cannot open %s\n", argv[1]);
    return 1;
  }

  hft::FeedHandler feed;
  std::vector<uint8_t> buf;
  uint64_t packets = 0;
  uint64_t events = 0;
  uint32_t len = 0;
  while (ReadU32(f, &len)) {
    if (len == 0 || len > (1U << 20)) {
      break;
    }
    buf.resize(len);
    if (std::fread(buf.data(), 1, len, f) != len) {
      break;
    }
    ++packets;
    events += feed.on_packet(hft::FeedSource::kPrimary, buf.data(), len,
                             [&](const hft::MarketEvent&) {});
  }
  std::fclose(f);

  std::printf("mtbt_replay: packets=%llu events=%llu malformed=%llu\n",
              static_cast<unsigned long long>(packets),
              static_cast<unsigned long long>(events),
              static_cast<unsigned long long>(feed.malformed()));
  return 0;
}
