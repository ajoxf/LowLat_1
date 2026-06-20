// Milestone 6/7 tests: NNF session login / heartbeat / reconnect against a
// mock gateway.
#include "session/nnf_session.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "common/time_utils.hpp"
#include "order_manager/nnf_encoder.hpp"

namespace hft {
namespace {

Timestamp At(int h, int m, int s) { return ist_time_of_day_ns(ist_now_ns(), h, m, s); }

// A trivial mock NSE gateway: records the transaction code of each message it
// receives and replies to a Sign On with a success response.
struct MockGateway {
  std::vector<uint16_t> received;
  void receive(const uint8_t* buf, size_t len) {
    ASSERT_GE(len, kNnfHeaderSize);
    received.push_back(load_tc(buf));
  }
  static uint16_t load_tc(const uint8_t* buf) {
    return static_cast<uint16_t>(buf[0] | (static_cast<uint16_t>(buf[1]) << 8));
  }
};

// 8. Full login sequence: Sign On -> Ready.
TEST(NnfSession, LoginSequence) {
  NnfSession s;
  s.configure(/*trader_id=*/1234, 'F', 'O', "secret");
  MockGateway gw;
  uint8_t buf[kNnfMaxMsg];

  size_t len = s.start_login(buf, At(9, 0, 0));
  ASSERT_GT(len, 0u);
  gw.receive(buf, len);
  EXPECT_EQ(s.state(), SessionState::kSignOnSent);
  EXPECT_EQ(gw.received.back(), kTcSignOn);

  // Gateway replies success with an assigned sequence number.
  s.on_sign_on_response(/*error=*/0, /*assigned_seq=*/100);
  EXPECT_TRUE(s.is_ready());
  EXPECT_EQ(s.out_seq(), 100u);
}

// 9. Box disconnect mid-session: re-login within 5 seconds.
TEST(NnfSession, ReconnectWithinWindow) {
  NnfSession s;
  s.configure(1234, 'F', 'O', "secret");
  uint8_t buf[kNnfMaxMsg];
  s.start_login(buf, At(9, 0, 0));
  s.on_sign_on_response(0, 1);
  ASSERT_TRUE(s.is_ready());

  const Timestamp dc = At(10, 0, 0);
  s.on_disconnect(dc);
  EXPECT_EQ(s.state(), SessionState::kDisconnected);
  // 3 seconds later: still inside the window -> re-login allowed.
  EXPECT_TRUE(s.should_reconnect(dc + 3ULL * kNsPerSecond));
  // 6 seconds later: window missed.
  EXPECT_FALSE(s.should_reconnect(dc + 6ULL * kNsPerSecond));

  s.start_login(buf, dc + 3ULL * kNsPerSecond);
  s.on_sign_on_response(0, 2);
  EXPECT_TRUE(s.is_ready());
}

// Heartbeat is due only after the idle interval.
TEST(NnfSession, HeartbeatTiming) {
  NnfSession s;
  s.configure(1234, 'F', 'O', "secret");
  uint8_t buf[kNnfMaxMsg];
  const Timestamp t0 = At(9, 30, 0);
  s.start_login(buf, t0);
  s.on_sign_on_response(0, 1);
  EXPECT_FALSE(s.should_heartbeat(t0 + 10ULL * kNsPerSecond));
  EXPECT_TRUE(s.should_heartbeat(t0 + 31ULL * kNsPerSecond));
  size_t len = s.make_heartbeat(buf, t0 + 31ULL * kNsPerSecond);
  EXPECT_EQ(len, kNnfHeaderSize);
  // After a heartbeat the idle timer resets.
  EXPECT_FALSE(s.should_heartbeat(t0 + 41ULL * kNsPerSecond));
}

// NSE error-code classification.
TEST(NnfSession, ErrorClassification) {
  EXPECT_EQ(NnfSession::classify_error(kErrOrderNotFound), ErrorAction::kIgnore);
  EXPECT_EQ(NnfSession::classify_error(kErrPriceOutOfRange), ErrorAction::kAlert);
  EXPECT_EQ(NnfSession::classify_error(0), ErrorAction::kNone);
}

// Sign-off completes the session.
TEST(NnfSession, Logout) {
  NnfSession s;
  s.configure(1234, 'F', 'O', "secret");
  uint8_t buf[kNnfMaxMsg];
  s.start_login(buf, At(9, 0, 0));
  s.on_sign_on_response(0, 1);
  size_t len = s.start_logout(buf, At(15, 35, 0));
  EXPECT_EQ(len, kNnfHeaderSize);
  EXPECT_EQ(s.state(), SessionState::kSignOffSent);
  s.on_sign_off_response();
  EXPECT_EQ(s.state(), SessionState::kClosed);
}

}  // namespace
}  // namespace hft
