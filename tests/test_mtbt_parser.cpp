// Milestone 3 tests: MTBT parser, dual-source feed handler, symbol master.
#include "feed_handler/mtbt_parser.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include "feed_handler/feed_handler.hpp"
#include "session/symbol_master.hpp"

namespace hft {
namespace {

// --- Little-endian encoders for building synthetic MTBT packets. ------------
template <typename T>
void put_le(std::vector<uint8_t>& b, T value) {
  auto u = static_cast<uint64_t>(static_cast<std::make_unsigned_t<T>>(value));
  for (size_t i = 0; i < sizeof(T); ++i) {
    b.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xFF));
  }
}

void EncodeHeader(std::vector<uint8_t>& b, uint16_t stream, uint64_t seq, uint8_t count,
                  uint64_t ts) {
  put_le<uint16_t>(b, stream);
  put_le<uint64_t>(b, seq);
  put_le<uint8_t>(b, count);
  put_le<uint64_t>(b, ts);
}

void EncodeAdd(std::vector<uint8_t>& b, int64_t order_no, uint8_t side, int32_t qty,
               uint32_t token, int64_t price) {
  b.push_back('A');
  put_le<int64_t>(b, order_no);
  put_le<uint8_t>(b, side);
  put_le<int32_t>(b, qty);
  put_le<uint32_t>(b, token);
  put_le<int64_t>(b, price);
}

void EncodeModify(std::vector<uint8_t>& b, int64_t order_no, int32_t qty, uint32_t token,
                  int64_t price) {
  b.push_back('M');
  put_le<int64_t>(b, order_no);
  put_le<int32_t>(b, qty);
  put_le<uint32_t>(b, token);
  put_le<int64_t>(b, price);
}

void EncodeCancel(std::vector<uint8_t>& b, int64_t order_no, uint32_t token) {
  b.push_back('X');
  put_le<int64_t>(b, order_no);
  put_le<uint32_t>(b, token);
}

void EncodeTrade(std::vector<uint8_t>& b, int64_t buy_no, int64_t sell_no, uint32_t token,
                 int64_t price, int32_t qty, int64_t trade_no) {
  b.push_back('T');
  put_le<int64_t>(b, buy_no);
  put_le<int64_t>(b, sell_no);
  put_le<uint32_t>(b, token);
  put_le<int64_t>(b, price);
  put_le<int32_t>(b, qty);
  put_le<int64_t>(b, trade_no);
}

void EncodeHeartbeat(std::vector<uint8_t>& b) { b.push_back('H'); }

// 1. Order Add round-trips, little-endian.
TEST(MtbtParser, ParseOrderAdd) {
  std::vector<uint8_t> b;
  EncodeAdd(b, 123456789, /*buy*/ 1, 250, 26009, 200000);
  MtbtParser p;
  MarketEvent ev;
  size_t n = p.parse_message(b.data(), b.size(), 7, 42, 999, &ev);
  EXPECT_EQ(n, kSizeOrderAdd);
  EXPECT_EQ(ev.order_no, 123456789);
  EXPECT_EQ(ev.side, kBuy);
  EXPECT_EQ(ev.qty, 250);
  EXPECT_EQ(ev.token, 26009u);
  EXPECT_EQ(ev.price, 200000);
  EXPECT_EQ(ev.seq_no, 42u);
  EXPECT_EQ(ev.stream_id, 7u);
}

// 2. Order Modify.
TEST(MtbtParser, ParseOrderModify) {
  std::vector<uint8_t> b;
  EncodeModify(b, 555, 99, 100, 150050);
  MtbtParser p;
  MarketEvent ev;
  ASSERT_EQ(p.parse_message(b.data(), b.size(), 1, 1, 0, &ev), kSizeOrderModify);
  EXPECT_EQ(ev.order_no, 555);
  EXPECT_EQ(ev.qty, 99);
  EXPECT_EQ(ev.price, 150050);
}

// 3. Order Cancel.
TEST(MtbtParser, ParseOrderCancel) {
  std::vector<uint8_t> b;
  EncodeCancel(b, 777, 100);
  MtbtParser p;
  MarketEvent ev;
  ASSERT_EQ(p.parse_message(b.data(), b.size(), 1, 1, 0, &ev), kSizeOrderCancel);
  EXPECT_EQ(ev.order_no, 777);
  EXPECT_EQ(ev.token, 100u);
}

// 4. Trade.
TEST(MtbtParser, ParseTrade) {
  std::vector<uint8_t> b;
  EncodeTrade(b, 111, 222, 100, 175025, 75, 90001);
  MtbtParser p;
  MarketEvent ev;
  ASSERT_EQ(p.parse_message(b.data(), b.size(), 1, 1, 0, &ev), kSizeTrade);
  EXPECT_EQ(ev.order_no, 111);
  EXPECT_EQ(ev.order_no2, 222);
  EXPECT_EQ(ev.price, 175025);
  EXPECT_EQ(ev.qty, 75);
  EXPECT_EQ(ev.trade_no, 90001);
}

// 5. A batched packet of mixed types parses in order.
TEST(MtbtParser, ParseBatch) {
  std::vector<uint8_t> b;
  EncodeHeader(b, 3, 1000, 5, 123456);
  EncodeAdd(b, 1, 1, 10, 100, 5000);
  EncodeModify(b, 1, 20, 100, 5000);
  EncodeTrade(b, 1, 2, 100, 5000, 5, 1);
  EncodeCancel(b, 1, 100);
  EncodeHeartbeat(b);
  MtbtParser p;
  std::vector<MarketEvent> evs;
  size_t emitted = p.parse_packet(b.data(), b.size(), [&](const MarketEvent& e) {
    evs.push_back(e);
  });
  // Heartbeat is not emitted as a book event.
  EXPECT_EQ(emitted, 4u);
  ASSERT_EQ(evs.size(), 4u);
  EXPECT_EQ(evs[0].type, static_cast<uint8_t>(MtbtMsgType::kOrderAdd));
  EXPECT_EQ(evs[1].type, static_cast<uint8_t>(MtbtMsgType::kOrderModify));
  EXPECT_EQ(evs[2].type, static_cast<uint8_t>(MtbtMsgType::kTrade));
  EXPECT_EQ(evs[3].type, static_cast<uint8_t>(MtbtMsgType::kOrderCancel));
  EXPECT_EQ(p.parsed(), 5u);  // Heartbeat counts as parsed.
}

// 6. Heartbeat parses without a crash.
TEST(MtbtParser, ParseHeartbeat) {
  std::vector<uint8_t> b;
  EncodeHeartbeat(b);
  MtbtParser p;
  MarketEvent ev;
  EXPECT_EQ(p.parse_message(b.data(), b.size(), 1, 1, 555, &ev), kSizeHeartbeat);
  EXPECT_EQ(ev.type, static_cast<uint8_t>(MtbtMsgType::kHeartbeat));
}

// 7. Unknown type stops the batch cleanly (size is indeterminate) without a
//    crash; messages before it are delivered.
TEST(MtbtParser, UnknownTypeStopsCleanly) {
  std::vector<uint8_t> b;
  EncodeHeader(b, 1, 1, 2, 0);
  EncodeAdd(b, 1, 1, 10, 100, 5000);
  b.push_back('Z');  // Unknown type.
  MtbtParser p;
  std::vector<MarketEvent> evs;
  p.parse_packet(b.data(), b.size(), [&](const MarketEvent& e) { evs.push_back(e); });
  EXPECT_EQ(evs.size(), 1u);
  EXPECT_EQ(p.malformed(), 1u);
}

// 8. Truncated message: no crash, reported as malformed.
TEST(MtbtParser, TruncatedMessage) {
  std::vector<uint8_t> b;
  b.push_back('A');
  b.push_back(0x01);  // Far too short for an Add.
  MtbtParser p;
  MarketEvent ev;
  EXPECT_EQ(p.parse_message(b.data(), b.size(), 1, 1, 0, &ev), 0u);
}

// 9. Explicit little-endian check: 0x40 0x0D 0x03 0x00 == 200000.
TEST(MtbtParser, LittleEndianDecode) {
  const uint8_t bytes[4] = {0x40, 0x0D, 0x03, 0x00};
  EXPECT_EQ(load_le<uint32_t>(bytes), 200000u);
}

// 10. Duplicate sequence from the second source is processed once.
TEST(FeedHandler, DeduplicatesAcrossSources) {
  std::vector<uint8_t> b;
  EncodeHeader(b, 5, 1, 1, 0);
  EncodeAdd(b, 1, 1, 10, 100, 5000);
  FeedHandler fh;
  int count = 0;
  fh.on_packet(FeedSource::kPrimary, b.data(), b.size(), [&](const MarketEvent&) { ++count; });
  fh.on_packet(FeedSource::kSecondary, b.data(), b.size(),
               [&](const MarketEvent&) { ++count; });
  EXPECT_EQ(count, 1);
  EXPECT_EQ(fh.stream(5).duplicates, 1u);
}

// 11. Sequence gap triggers a retransmit flag.
TEST(FeedHandler, GapTriggersRetransmit) {
  FeedHandler fh;
  auto sink = [](const MarketEvent&) {};
  std::vector<uint8_t> p1;
  EncodeHeader(p1, 5, 499, 1, 0);
  EncodeAdd(p1, 1, 1, 10, 100, 5000);
  fh.on_packet(FeedSource::kPrimary, p1.data(), p1.size(), sink);
  // Next packet is seq 501 (500 missing).
  std::vector<uint8_t> p2;
  EncodeHeader(p2, 5, 501, 1, 0);
  EncodeAdd(p2, 2, 1, 10, 100, 5000);
  fh.on_packet(FeedSource::kPrimary, p2.data(), p2.size(), sink);
  EXPECT_EQ(fh.stream(5).gaps, 1u);
  EXPECT_TRUE(fh.stream(5).needs_retransmit);
  EXPECT_EQ(fh.stream(5).last_gap_lo, 500u);
}

// 12. Integration: replay many messages with zero parse errors.
TEST(FeedHandler, ReplayManyMessages) {
  FeedHandler fh;
  uint64_t emitted = 0;
  uint64_t seq = 1;
  for (int pkt = 0; pkt < 5000; ++pkt) {
    std::vector<uint8_t> b;
    EncodeHeader(b, 9, seq, 10, 0);
    for (int i = 0; i < 10; ++i) {
      EncodeAdd(b, static_cast<int64_t>(seq + i), 1, 10, 100, 5000 + i);
    }
    fh.on_packet(FeedSource::kPrimary, b.data(), b.size(),
                 [&](const MarketEvent&) { ++emitted; });
    seq += 10;
  }
  EXPECT_EQ(emitted, 50000u);
  EXPECT_EQ(fh.malformed(), 0u);
}

// --- Symbol master ----------------------------------------------------------
std::string WriteTempCsv() {
  std::string path = "/tmp/hft_symbols_test.csv";
  std::ofstream out(path);
  out << "token,symbol,series,instrument_type,segment,expiry,strike,opt,lot,tick,prev,circuit\n";
  out << "2885,RELIANCE,EQ,EQ,1,0,0,,1,5,250000000,2000\n";
  out << "26009,BANKNIFTY,FUT,FUTIDX,2,20260625,0,,15,5,5500000000,1000\n";
  out << "26000,NIFTY,FUT,FUTIDX,2,20260625,0,,25,5,2400000000,1000\n";
  out.close();
  return path;
}

// 13. Reverse lookup by symbol/series.
TEST(SymbolMaster, GetToken) {
  SymbolMaster sm;
  ASSERT_GT(sm.load_csv(WriteTempCsv()), 0);
  EXPECT_EQ(sm.get_token("RELIANCE", "EQ"), 2885u);
}

// 14. Lot sizes loaded correctly.
TEST(SymbolMaster, LotSizes) {
  SymbolMaster sm;
  ASSERT_GT(sm.load_csv(WriteTempCsv()), 0);
  EXPECT_EQ(sm.get_lot_size(26009), 15);  // BankNifty.
  EXPECT_EQ(sm.get_lot_size(26000), 25);  // Nifty.
  EXPECT_EQ(sm.get_tick_size(2885), 5);
  EXPECT_TRUE(sm.is_index_derivative(26009));
  EXPECT_FALSE(sm.is_index_derivative(2885));
}

}  // namespace
}  // namespace hft
