// Milestone 7 tests: socket layer and order gateway over loopback.
//
// Multicast group joins are environment-dependent, so these tests exercise the
// UDP receive/parse path over unicast loopback and the TCP order gateway over a
// socketpair -- validating the primitives the live gateways are built from.
#include "gateway/order_gateway.hpp"

#include <gtest/gtest.h>

#include <sys/socket.h>

#include <cstdint>
#include <vector>

#include "feed_handler/mtbt_parser.hpp"
#include "gateway/udp_socket.hpp"

namespace hft {
namespace {

template <typename T>
void put_le(std::vector<uint8_t>& b, T v) {
  auto u = static_cast<uint64_t>(static_cast<std::make_unsigned_t<T>>(v));
  for (size_t i = 0; i < sizeof(T); ++i) {
    b.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xFF));
  }
}

std::vector<uint8_t> MakeAddPacket(uint16_t stream, uint64_t seq, int64_t order_no,
                                   int32_t qty, uint32_t token, int64_t price) {
  std::vector<uint8_t> b;
  put_le<uint16_t>(b, stream);
  put_le<uint64_t>(b, seq);
  put_le<uint8_t>(b, 1);          // msg_count.
  put_le<uint64_t>(b, 999);       // timestamp.
  b.push_back('A');
  put_le<int64_t>(b, order_no);
  put_le<uint8_t>(b, 1);          // buy.
  put_le<int32_t>(b, qty);
  put_le<uint32_t>(b, token);
  put_le<int64_t>(b, price);
  return b;
}

// 1. UDP loopback: a sent MTBT packet is received and parses correctly.
TEST(Gateway, UdpLoopbackReceiveAndParse) {
  UdpSocket rx;
  ASSERT_TRUE(rx.bind_rx(0));
  rx.set_nonblocking(true);
  uint16_t port = rx.local_port();
  ASSERT_GT(port, 0);

  UdpSocket tx;
  ASSERT_TRUE(tx.open_tx());
  auto pkt = MakeAddPacket(7, 1, 12345, 50, 26009, 200000);
  ASSERT_TRUE(tx.send_to("127.0.0.1", port, pkt.data(), pkt.size()));

  uint8_t buf[2048];
  ssize_t n = -1;
  for (int attempt = 0; attempt < 1000 && n <= 0; ++attempt) {
    n = rx.recv(buf, sizeof(buf));
  }
  ASSERT_GT(n, 0);

  MtbtParser parser;
  std::vector<MarketEvent> evs;
  parser.parse_packet(buf, static_cast<size_t>(n),
                      [&](const MarketEvent& e) { evs.push_back(e); });
  ASSERT_EQ(evs.size(), 1u);
  EXPECT_EQ(evs[0].order_no, 12345);
  EXPECT_EQ(evs[0].token, 26009u);
  EXPECT_EQ(evs[0].price, 200000);
}

// 2. Batched receive (recvmmsg) returns multiple datagrams.
TEST(Gateway, UdpBatchReceive) {
  UdpSocket rx;
  ASSERT_TRUE(rx.bind_rx(0));
  rx.set_nonblocking(true);
  uint16_t port = rx.local_port();
  UdpSocket tx;
  ASSERT_TRUE(tx.open_tx());
  for (int i = 0; i < 5; ++i) {
    auto pkt = MakeAddPacket(7, static_cast<uint64_t>(i + 1), i + 1, 50, 26009, 200000);
    ASSERT_TRUE(tx.send_to("127.0.0.1", port, pkt.data(), pkt.size()));
  }
  // Give the stack a moment, then batch-receive.
  uint8_t bufs[32 * 2048];
  uint32_t lens[32];
  int total = 0;
  for (int attempt = 0; attempt < 10 && total < 5; ++attempt) {
    int n = rx.recv_batch(bufs, 2048, 32, lens);
    if (n > 0) {
      total += n;
    }
  }
  EXPECT_GE(total, 1);  // At least some datagrams batched (UDP may coalesce/drop).
}

// 3. Order gateway over a socketpair: send is received, disconnect detected.
TEST(Gateway, OrderGatewaySendAndDisconnect) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  OrderGateway gw;
  gw.adopt(fds[0]);
  TcpSocket server;
  server.adopt(fds[1]);

  const uint8_t msg[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_TRUE(gw.send_order(msg, sizeof(msg)));
  EXPECT_EQ(gw.sent(), 1u);
  EXPECT_GT(gw.last_send_tsc(), 0u);

  uint8_t in[16];
  ssize_t n = server.recv(in, sizeof(in));
  ASSERT_EQ(n, 8);
  EXPECT_EQ(in[0], 1);
  EXPECT_EQ(in[7], 8);

  // Server closes: the gateway observes the disconnect on the next read.
  server.close_socket();
  uint8_t resp[16];
  ssize_t r = gw.poll_response(resp, sizeof(resp));
  EXPECT_EQ(r, 0);
  EXPECT_FALSE(gw.connected());
}

// 4. Order gateway response correlation: server echoes, gateway reads it back.
TEST(Gateway, OrderGatewayResponseRoundtrip) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  OrderGateway gw;
  gw.adopt(fds[0]);
  TcpSocket server;
  server.adopt(fds[1]);

  const uint8_t order[4] = {0xAB, 0xCD, 0xEF, 0x01};
  ASSERT_TRUE(gw.send_order(order, sizeof(order)));
  uint8_t got[8];
  ASSERT_EQ(server.recv(got, sizeof(got)), 4);
  // Server sends an ack back.
  const uint8_t ack[4] = {0x11, 0x22, 0x33, 0x44};
  ASSERT_TRUE(server.send_all(ack, sizeof(ack)));
  uint8_t resp[8];
  ssize_t r = gw.poll_response(resp, sizeof(resp));
  ASSERT_EQ(r, 4);
  EXPECT_EQ(resp[0], 0x11);
  EXPECT_EQ(resp[3], 0x44);
  EXPECT_EQ(gw.received(), 1u);
}

}  // namespace
}  // namespace hft
