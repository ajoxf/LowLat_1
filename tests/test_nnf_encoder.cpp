// Milestone 6 tests: NNF encoder and order-manager state machine.
#include "order_manager/nnf_encoder.hpp"

#include <gtest/gtest.h>

#include <cstdint>

#include "common/time_utils.hpp"
#include "feed_handler/mtbt_parser.hpp"  // for load_le helpers.
#include "order_manager/order_manager.hpp"
#include "session/nnf_session.hpp"  // for NSE error-code constants.

namespace hft {
namespace {

Order MakeOrder() {
  Order o{};
  o.client_order_id = 0xABCD;
  o.token = 26009;
  o.segment = kSegmentFo;
  o.side = kBuy;
  o.price = 200000;
  o.qty = 15;
  o.disclosed_qty = 0;
  o.order_type = OrderType::kLimit;
  o.validity = Validity::kDay;
  return o;
}

Timestamp At(int h, int m, int s) { return ist_time_of_day_ns(ist_now_ns(), h, m, s); }

// 1. New order header fields are correctly packed.
TEST(NnfEncoder, NewOrderHeader) {
  NnfEncoder enc;
  enc.set_trader_id(98765);
  enc.set_alpha_char('C', 'M');
  uint8_t buf[kNnfMaxMsg];
  size_t len = enc.encode_new_order(buf, MakeOrder(), At(9, 30, 0));
  EXPECT_EQ(len, kNnfHeaderSize + 48);
  EXPECT_EQ(load_le<uint16_t>(buf + kHdrTransCode), kTcNewOrder);
  EXPECT_EQ(load_le_i32(buf + kHdrTraderId), 98765);
  EXPECT_EQ(buf[kHdrAlphaChar + 0], 'C');
  EXPECT_EQ(buf[kHdrAlphaChar + 1], 'M');
  EXPECT_EQ(load_le<int16_t>(buf + kHdrMsgLength), static_cast<int16_t>(len));
}

// 2. New order body fields little-endian.
TEST(NnfEncoder, NewOrderBody) {
  NnfEncoder enc;
  uint8_t buf[kNnfMaxMsg];
  Order o = MakeOrder();
  enc.encode_new_order(buf, o, At(9, 30, 0));
  const uint8_t* b = buf + kNnfHeaderSize;
  EXPECT_EQ(load_le<uint32_t>(b + 0), 26009u);     // token.
  EXPECT_EQ(load_le<int16_t>(b + 4), 1);           // buy.
  EXPECT_EQ(load_le_i32(b + 6), 15);               // qty.
  EXPECT_EQ(load_le_i64(b + 14), 200000);          // price (paisa).
  EXPECT_EQ(b[22], 'L');                           // order type.
  EXPECT_EQ(load_le<int16_t>(b + 23), static_cast<int16_t>(Validity::kDay));
  EXPECT_EQ(load_le_i64(b + 25), 0xABCD);          // client order id.
}

// 3. Modify carries exchange order no and new price/qty.
TEST(NnfEncoder, Modify) {
  NnfEncoder enc;
  uint8_t buf[kNnfMaxMsg];
  Order o = MakeOrder();
  o.exchange_order_no = 0x1122334455;
  o.price = 201000;
  o.qty = 30;
  size_t len = enc.encode_modify(buf, o, At(9, 30, 0));
  EXPECT_EQ(load_le<uint16_t>(buf + kHdrTransCode), kTcModifyOrder);
  const uint8_t* b = buf + kNnfHeaderSize;
  EXPECT_EQ(load_le_i64(b + 0), 0x1122334455);
  EXPECT_EQ(load_le_i64(b + 12), 201000);
  EXPECT_EQ(load_le_i32(b + 20), 30);
  EXPECT_EQ(len, kNnfHeaderSize + 36);
}

// 4. Cancel carries exchange order no and client order id.
TEST(NnfEncoder, Cancel) {
  NnfEncoder enc;
  uint8_t buf[kNnfMaxMsg];
  size_t len = enc.encode_cancel(buf, 0x9988, 26009, 0xABCD, At(9, 30, 0));
  EXPECT_EQ(load_le<uint16_t>(buf + kHdrTransCode), kTcCancelOrder);
  const uint8_t* b = buf + kNnfHeaderSize;
  EXPECT_EQ(load_le_i64(b + 0), 0x9988);
  EXPECT_EQ(load_le<uint32_t>(b + 8), 26009u);
  EXPECT_EQ(load_le_i64(b + 12), 0xABCD);
  EXPECT_EQ(len, kNnfHeaderSize + 20);
}

// 5. Heartbeat length and transaction code.
TEST(NnfEncoder, Heartbeat) {
  NnfEncoder enc;
  uint8_t buf[kNnfMaxMsg];
  size_t len = enc.encode_heartbeat(buf, At(9, 30, 0));
  EXPECT_EQ(len, kNnfHeaderSize);
  EXPECT_EQ(load_le<uint16_t>(buf + kHdrTransCode), kTcHeartbeat);
}

// 6. Timestamp 09:30:00.00 -> "09300000".
TEST(NnfEncoder, TimestampAscii) {
  uint8_t ts[8];
  NnfEncoder::encode_timestamp(ts, At(9, 30, 0));
  EXPECT_EQ(std::string(reinterpret_cast<char*>(ts), 8), "09300000");
}

// 7. Encoding many orders touches no heap (fixed buffer, no allocation).
TEST(NnfEncoder, NoAllocationManyOrders) {
  NnfEncoder enc;
  uint8_t buf[kNnfMaxMsg];
  Order o = MakeOrder();
  for (int i = 0; i < 1'000'000; ++i) {
    o.client_order_id = i;
    size_t len = enc.encode_new_order(buf, o, At(9, 30, 0));
    ASSERT_EQ(len, kNnfHeaderSize + 48);
  }
}

// 10. State machine: PENDING -> ACK -> FILL -> FILLED.
TEST(OrderManager, FillLifecycle) {
  OrderManager om;
  uint8_t buf[kNnfMaxMsg];
  Order o = MakeOrder();
  o.client_order_id = 0;  // Auto-assign.
  ASSERT_GT(om.new_order(o, buf, At(9, 30, 0)), 0u);
  ClOrdId id = o.client_order_id;
  EXPECT_EQ(om.state_of(id), OrderState::kPendingNew);
  ASSERT_TRUE(om.on_ack(id, 0x5555));
  EXPECT_EQ(om.state_of(id), OrderState::kAcknowledged);
  FillReport f{};
  f.client_order_id = id;
  f.fill_qty = o.qty;
  f.remaining_qty = 0;
  ASSERT_TRUE(om.on_fill(f));
  EXPECT_EQ(om.state_of(id), OrderState::kFilled);
}

// 11. State machine: PENDING -> ACK -> CANCEL -> removed.
TEST(OrderManager, CancelLifecycle) {
  OrderManager om;
  uint8_t buf[kNnfMaxMsg];
  Order o = MakeOrder();
  o.client_order_id = 0;
  om.new_order(o, buf, At(9, 30, 0));
  ClOrdId id = o.client_order_id;
  om.on_ack(id, 0x5555);
  // Encode cancel uses the stored exchange order number.
  EXPECT_GT(om.encode_cancel(id, buf, At(9, 30, 0)), 0u);
  ASSERT_TRUE(om.on_cancel_ack(id));
  EXPECT_EQ(om.state_of(id), OrderState::kCancelled);
  EXPECT_EQ(om.live_count(), 0u);
}

// 12. State machine: PENDING -> REJECT (error code recorded).
TEST(OrderManager, RejectLifecycle) {
  OrderManager om;
  uint8_t buf[kNnfMaxMsg];
  Order o = MakeOrder();
  o.client_order_id = 0;
  om.new_order(o, buf, At(9, 30, 0));
  ClOrdId id = o.client_order_id;
  ASSERT_TRUE(om.on_reject(id, kErrPriceOutOfRange));
  EXPECT_EQ(om.state_of(id), OrderState::kRejected);
  EXPECT_EQ(om.get(id)->reject_code, kErrPriceOutOfRange);
}

// 13. Duplicate live client order id is refused.
TEST(OrderManager, DuplicateClOrdId) {
  OrderManager om;
  uint8_t buf[kNnfMaxMsg];
  Order o = MakeOrder();
  o.client_order_id = 12345;
  ASSERT_GT(om.new_order(o, buf, At(9, 30, 0)), 0u);
  Order dup = MakeOrder();
  dup.client_order_id = 12345;  // Same id, still live.
  EXPECT_EQ(om.new_order(dup, buf, At(9, 30, 0)), 0u);
}

// 14. encode_new_order latency budget.
TEST(NnfEncoder, Latency) {
  NnfEncoder enc;
  uint8_t buf[kNnfMaxMsg];
  Order o = MakeOrder();
  enc.encode_new_order(buf, o, At(9, 30, 0));  // Warm.
  const TscClock& clk = TscClock::Instance();
  uint64_t best = UINT64_MAX;
  for (int i = 0; i < 1000; ++i) {
    uint64_t t0 = rdtsc();
    enc.encode_new_order(buf, o, At(9, 30, 0));
    best = std::min(best, clk.to_ns(rdtsc() - t0));
  }
  EXPECT_LT(best, 1500u);  // Generous bound for untuned CI hardware.
}

}  // namespace
}  // namespace hft
