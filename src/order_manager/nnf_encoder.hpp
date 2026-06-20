// NSE NNF (Non-NEAT Front End) Trimmed Protocol binary encoder.
//
// IMPORTANT: the NNF Trimmed Protocol is proprietary; members obtain
// TP_CM_Trimmed_NNF_PROTOCOL / TP_FO_Trimmed_NNF_PROTOCOL from NSE. The layouts
// below are a faithful, representative model: a fixed 32-byte MESSAGE_HEADER
// followed by a transaction-specific body, all little-endian, no heap traffic,
// no sprintf. Adjust offsets/sizes to the real spec at integration time.
#ifndef HFT_ORDER_MANAGER_NNF_ENCODER_HPP_
#define HFT_ORDER_MANAGER_NNF_ENCODER_HPP_

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common/compiler.hpp"
#include "common/time_utils.hpp"
#include "common/types.hpp"

namespace hft {

// NNF transaction codes (representative values).
inline constexpr uint16_t kTcNewOrder = 2073;
inline constexpr uint16_t kTcModifyOrder = 2074;
inline constexpr uint16_t kTcCancelOrder = 2075;
inline constexpr uint16_t kTcHeartbeat = 23;
inline constexpr uint16_t kTcSignOn = 2300;
inline constexpr uint16_t kTcSignOff = 2320;
inline constexpr uint16_t kTcGatewayRequest = 2400;

// MESSAGE_HEADER field offsets and total size.
inline constexpr size_t kHdrTransCode = 0;   // u16
inline constexpr size_t kHdrLogTime = 2;      // i32
inline constexpr size_t kHdrAlphaChar = 6;    // char[2]
inline constexpr size_t kHdrTraderId = 8;     // i32
inline constexpr size_t kHdrErrorCode = 12;   // i16
inline constexpr size_t kHdrTimestamp = 14;   // char[8] HHMMSSss
inline constexpr size_t kHdrTimestamp2 = 22;  // char[8]
inline constexpr size_t kHdrMsgLength = 30;   // i16
inline constexpr size_t kNnfHeaderSize = 32;

inline constexpr size_t kNnfMaxMsg = 256;

// Little-endian store helpers.
template <typename T>
HFT_ALWAYS_INLINE void store_le(uint8_t* p, T value) {
  auto u = static_cast<uint64_t>(static_cast<std::make_unsigned_t<T>>(value));
  for (size_t i = 0; i < sizeof(T); ++i) {
    p[i] = static_cast<uint8_t>((u >> (8 * i)) & 0xFF);
  }
}

// Map our order type to the NNF single-character code.
HFT_ALWAYS_INLINE char nnf_order_type_char(OrderType t) {
  switch (t) {
    case OrderType::kLimit:
      return 'L';
    case OrderType::kMarket:
      return 'M';
    case OrderType::kStopLoss:
      return 'S';
    case OrderType::kStopLossMarket:
      return 'Q';
  }
  return 'L';
}

class NnfEncoder {
 public:
  void set_trader_id(int32_t id) { trader_id_ = id; }
  void set_alpha_char(char a, char b) {
    alpha_[0] = a;
    alpha_[1] = b;
  }
  void set_branch_id(int16_t b) { branch_id_ = b; }
  void set_participant_id(const char* pid) {
    std::memset(participant_, ' ', sizeof(participant_));
    if (pid != nullptr) {
      const size_t n = std::strlen(pid);
      std::memcpy(participant_, pid, n < sizeof(participant_) ? n : sizeof(participant_));
    }
  }

  // Encode the "HHMMSSss" timestamp (ss = hundredths of a second) into 8 bytes.
  static void encode_timestamp(uint8_t* dst, Timestamp ist_ns) {
    const uint64_t tod = ist_ns % kNsPerDay;
    const uint64_t hh = tod / (3600ULL * kNsPerSecond);
    const uint64_t mm = (tod / (60ULL * kNsPerSecond)) % 60;
    const uint64_t ss = (tod / kNsPerSecond) % 60;
    const uint64_t hundredths = (tod % kNsPerSecond) / 10'000'000ULL;
    Two(dst + 0, hh);
    Two(dst + 2, mm);
    Two(dst + 4, ss);
    Two(dst + 6, hundredths);
  }

  // Encode a NEW ORDER. Returns the message length, or 0 on overflow.
  size_t encode_new_order(uint8_t* buf, const Order& o, Timestamp now_ist,
                          char open_close = 'O') {
    constexpr size_t kBody = 48;
    constexpr size_t kTotal = kNnfHeaderSize + kBody;
    if (kTotal > kNnfMaxMsg) {
      return 0;
    }
    WriteHeader(buf, kTcNewOrder, now_ist, static_cast<int16_t>(kTotal));
    uint8_t* b = buf + kNnfHeaderSize;
    store_le<int32_t>(b + 0, static_cast<int32_t>(o.token));
    store_le<int16_t>(b + 4, o.side == kBuy ? 1 : 2);
    store_le<int32_t>(b + 6, o.qty);
    store_le<int32_t>(b + 10, o.disclosed_qty);
    store_le<int64_t>(b + 14, o.price);
    b[22] = static_cast<uint8_t>(nnf_order_type_char(o.order_type));
    store_le<int16_t>(b + 23, static_cast<int16_t>(o.validity));
    store_le<int64_t>(b + 25, o.client_order_id);
    std::memcpy(b + 33, participant_, 12);
    store_le<int16_t>(b + 45, branch_id_);
    b[47] = static_cast<uint8_t>(open_close);
    return kTotal;
  }

  // Encode an ORDER MODIFICATION. Returns the message length.
  size_t encode_modify(uint8_t* buf, const Order& o, Timestamp now_ist) {
    constexpr size_t kBody = 36;
    constexpr size_t kTotal = kNnfHeaderSize + kBody;
    WriteHeader(buf, kTcModifyOrder, now_ist, static_cast<int16_t>(kTotal));
    uint8_t* b = buf + kNnfHeaderSize;
    store_le<int64_t>(b + 0, o.exchange_order_no);
    store_le<int32_t>(b + 8, static_cast<int32_t>(o.token));
    store_le<int64_t>(b + 12, o.price);
    store_le<int32_t>(b + 20, o.qty);
    store_le<int32_t>(b + 24, o.disclosed_qty);
    store_le<int64_t>(b + 28, o.client_order_id);
    return kTotal;
  }

  // Encode an ORDER CANCELLATION. Returns the message length.
  size_t encode_cancel(uint8_t* buf, int64_t exchange_order_no, Token token, ClOrdId clordid,
                       Timestamp now_ist) {
    constexpr size_t kBody = 20;
    constexpr size_t kTotal = kNnfHeaderSize + kBody;
    WriteHeader(buf, kTcCancelOrder, now_ist, static_cast<int16_t>(kTotal));
    uint8_t* b = buf + kNnfHeaderSize;
    store_le<int64_t>(b + 0, exchange_order_no);
    store_le<int32_t>(b + 8, static_cast<int32_t>(token));
    store_le<int64_t>(b + 12, clordid);
    return kTotal;
  }

  // Encode a HEARTBEAT (header only).
  size_t encode_heartbeat(uint8_t* buf, Timestamp now_ist) {
    WriteHeader(buf, kTcHeartbeat, now_ist, static_cast<int16_t>(kNnfHeaderSize));
    return kNnfHeaderSize;
  }

  // Encode a SIGN ON request (header + trader id + password). Returns length.
  size_t encode_sign_on(uint8_t* buf, const char* password, Timestamp now_ist) {
    constexpr size_t kBody = 32;
    constexpr size_t kTotal = kNnfHeaderSize + kBody;
    WriteHeader(buf, kTcSignOn, now_ist, static_cast<int16_t>(kTotal));
    uint8_t* b = buf + kNnfHeaderSize;
    std::memset(b, 0, kBody);
    if (password != nullptr) {
      const size_t n = std::strlen(password);
      std::memcpy(b, password, n < kBody ? n : kBody);
    }
    return kTotal;
  }

  size_t encode_sign_off(uint8_t* buf, Timestamp now_ist) {
    WriteHeader(buf, kTcSignOff, now_ist, static_cast<int16_t>(kNnfHeaderSize));
    return kNnfHeaderSize;
  }

 private:
  static void Two(uint8_t* dst, uint64_t v) {
    dst[0] = static_cast<uint8_t>('0' + (v / 10) % 10);
    dst[1] = static_cast<uint8_t>('0' + v % 10);
  }

  void WriteHeader(uint8_t* buf, uint16_t tc, Timestamp now_ist, int16_t len) {
    store_le<uint16_t>(buf + kHdrTransCode, tc);
    store_le<int32_t>(buf + kHdrLogTime, 0);
    buf[kHdrAlphaChar + 0] = static_cast<uint8_t>(alpha_[0]);
    buf[kHdrAlphaChar + 1] = static_cast<uint8_t>(alpha_[1]);
    store_le<int32_t>(buf + kHdrTraderId, trader_id_);
    store_le<int16_t>(buf + kHdrErrorCode, 0);
    encode_timestamp(buf + kHdrTimestamp, now_ist);
    std::memset(buf + kHdrTimestamp2, 0, 8);
    store_le<int16_t>(buf + kHdrMsgLength, len);
  }

  int32_t trader_id_ = 0;
  int16_t branch_id_ = 0;
  char alpha_[2] = {' ', ' '};
  char participant_[12] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
};

}  // namespace hft

#endif  // HFT_ORDER_MANAGER_NNF_ENCODER_HPP_
