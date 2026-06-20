// NSE NNF session management: login, heartbeat, sign-off and reconnect.
//
// Modelled as a pure state machine that emits outbound NNF messages into a
// caller-provided buffer and consumes inbound responses, so it is fully
// testable against a mock gateway without real sockets. The full NNF login
// handshake (Gateway Router request/response, Secure Box registration, Sign On)
// is represented here in compressed form; expand to the exact spec sequence at
// integration time.
#ifndef HFT_SESSION_NNF_SESSION_HPP_
#define HFT_SESSION_NNF_SESSION_HPP_

#include <cstddef>
#include <cstdint>

#include "common/time_utils.hpp"
#include "common/types.hpp"
#include "order_manager/nnf_encoder.hpp"

namespace hft {

// NSE NNF error codes (representative; see the NNF spec for the full list).
inline constexpr int16_t kErrOrderNotFound = 16444;    // Stale cancel: log + ignore.
inline constexpr int16_t kErrPriceOutOfRange = 16445;  // Circuit breach: log + alert.

enum class SessionState : uint8_t {
  kDisconnected = 0,
  kSignOnSent,
  kReady,
  kSignOffSent,
  kClosed,
};

enum class ErrorAction : uint8_t {
  kNone = 0,
  kIgnore,  // e.g. order-not-found on a stale cancel.
  kAlert,   // e.g. price-out-of-range -> refresh price band.
};

inline constexpr uint64_t kHeartbeatIntervalNs = 30ULL * kNsPerSecond;
inline constexpr uint64_t kReconnectWindowNs = 5ULL * kNsPerSecond;

class NnfSession {
 public:
  void configure(int32_t trader_id, char alpha0, char alpha1, const char* password) {
    encoder_.set_trader_id(trader_id);
    encoder_.set_alpha_char(alpha0, alpha1);
    trader_id_ = trader_id;
    password_ = password;
  }

  NnfEncoder& encoder() { return encoder_; }
  SessionState state() const { return state_; }
  uint64_t out_seq() const { return out_seq_; }

  // Begin (or restart) login. Encodes a Sign On into buf. Returns its length.
  size_t start_login(uint8_t* buf, Timestamp now) {
    state_ = SessionState::kSignOnSent;
    last_send_ns_ = now;
    return encoder_.encode_sign_on(buf, password_, now);
  }

  // Process the Sign On response. error==0 => session ready.
  void on_sign_on_response(int16_t error, uint64_t assigned_seq) {
    if (error == 0) {
      state_ = SessionState::kReady;
      out_seq_ = assigned_seq;
    } else {
      state_ = SessionState::kDisconnected;
    }
  }

  // Begin logout. Encodes a Sign Off into buf. Returns its length.
  size_t start_logout(uint8_t* buf, Timestamp now) {
    state_ = SessionState::kSignOffSent;
    last_send_ns_ = now;
    return encoder_.encode_sign_off(buf, now);
  }
  void on_sign_off_response() { state_ = SessionState::kClosed; }

  // TCP dropped. Record when so the reconnect window can be enforced.
  void on_disconnect(Timestamp now) {
    state_ = SessionState::kDisconnected;
    disconnect_ns_ = now;
  }

  // True while still inside the 5-second reconnect window.
  bool should_reconnect(Timestamp now) const {
    return state_ == SessionState::kDisconnected && disconnect_ns_ != 0 &&
           (now - disconnect_ns_) <= kReconnectWindowNs;
  }

  // Heartbeat is due when the link is idle for the heartbeat interval.
  bool should_heartbeat(Timestamp now) const {
    return state_ == SessionState::kReady && (now - last_send_ns_) >= kHeartbeatIntervalNs;
  }
  size_t make_heartbeat(uint8_t* buf, Timestamp now) {
    last_send_ns_ = now;
    return encoder_.encode_heartbeat(buf, now);
  }

  // Call after sending any application message so heartbeats only fill idle gaps
  // and so the per-session outgoing sequence advances.
  void note_send(Timestamp now) {
    last_send_ns_ = now;
    ++out_seq_;
  }

  // Classify an inbound NSE error code into an action.
  static ErrorAction classify_error(int16_t code) {
    switch (code) {
      case kErrOrderNotFound:
        return ErrorAction::kIgnore;
      case kErrPriceOutOfRange:
        return ErrorAction::kAlert;
      default:
        return code != 0 ? ErrorAction::kAlert : ErrorAction::kNone;
    }
  }

  bool is_ready() const { return state_ == SessionState::kReady; }

 private:
  NnfEncoder encoder_;
  SessionState state_ = SessionState::kDisconnected;
  int32_t trader_id_ = 0;
  const char* password_ = nullptr;
  uint64_t out_seq_ = 0;
  Timestamp last_send_ns_ = 0;
  Timestamp disconnect_ns_ = 0;
};

}  // namespace hft

#endif  // HFT_SESSION_NNF_SESSION_HPP_
